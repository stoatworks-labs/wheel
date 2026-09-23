#include "Shaders.h"

namespace wheel
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: copy, linearised. Texel for texel.
//---------------------------------------------------------------------------
const char* const kCopyShader = R"(#version 410 core

uniform sampler2D InputTexture;
uniform float Gamma; //1.0 is linear and skips the pow

out vec4 fragColor;

void main()
{
	//The picture sits at texels 0..Width-1 of the host's texture whatever
	//its hardware size, and this buffer is the picture's size, so the
	//fragment's own coordinate IS the texel. No sampler, no MaxUV.
	vec4 c   = texelFetch( InputTexture, ivec2( gl_FragCoord.xy ), 0 );
	vec3 lin = Gamma <= 1.0 ? c.rgb : pow( max( c.rgb, vec3( 0.0 ) ), vec3( Gamma ) );
	fragColor = vec4( lin, c.a );
}
)";

//---------------------------------------------------------------------------
// Pass 2: grid. One luminance per cell, off the mip chain.
//---------------------------------------------------------------------------
const char* const kGridShader = R"(#version 410 core

uniform sampler2D LinearTexture;
uniform float Lod;
uniform int GridWidth;
uniform int GridHeight;

out vec4 fragColor;

void main()
{
	vec2 c    = gl_FragCoord.xy / vec2( float( GridWidth ), float( GridHeight ) );
	vec4 v    = textureLod( LinearTexture, c, Lod );
	fragColor = vec4( dot( v.rgb, vec3( 0.2126, 0.7152, 0.0722 ) ), 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 3: integrate. The retina.
//---------------------------------------------------------------------------
const char* const kIntegrateShader = R"(#version 410 core

#define MAX_SUB 36
#define MAX_TAPS 9

uniform sampler2D SourceTexture; //the host's input, for Mix
uniform sampler2D LinearTexture; //our linearised copy, picture-sized
uniform sampler2D PrevTexture;   //negative-control hook only
uniform int Interp;              //hook: 1 blends the previous frame in by sub-field time. A DMD does not.
uniform float TapBias;           //hook: multiplies the box's end taps; 1.0 is the model

uniform int Width;
uniform int Height;

uniform int Count;
uniform vec4 SubW[ MAX_SUB ]; //rgb: filter * weight; a: alpha weight
uniform vec4 SubO[ MAX_SUB ]; //xy: offset of the sub-field's centre, GL pixels; zw: the box it spans
uniform vec4 SubT[ MAX_SUB ]; //x: 1 for a secondary segment; y: centre time (the hook's blend)

uniform float Gamma;    //re-encode; 1.0 skips the pow
uniform int BitDepth;   //0 = no bit planes
uniform int ChipMode;   //0 single chip, 1 three chip
uniform vec2 RedOffset; //GL pixels
uniform vec2 BlueOffset;
uniform float RedRotate; //radians, about the picture's centre
uniform float BlueRotate;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

vec4 texelAt( sampler2D s, ivec2 p )
{
	p = clamp( p, ivec2( 0 ), ivec2( Width - 1, Height - 1 ) );
	return texelFetch( s, p, 0 );
}

//What the DMD shows through this segment, per mirror. A primary shows the
//pixel; a secondary shows the part of the pixel every channel it passes
//agrees on. This happens BEFORE the retina blurs anything: min is concave,
//so a min taken after the blur would make light out of the blur itself
//(Jensen), and `--energy` on a six-segment wheel is what found that.
vec4 shown( vec4 v, vec3 filt, float secondary )
{
	if( secondary < 0.5 )
		return v;
	float s = 1e9;
	if( filt.r > 0.0 ) s = min( s, v.r );
	if( filt.g > 0.0 ) s = min( s, v.g );
	if( filt.b > 0.0 ) s = min( s, v.b );
	return vec4( vec3( s ), v.a );
}

//Bilinear, by hand, from four texelFetches, each already what the DMD
//shows. p is in pixels with texel centres at n + 0.5. At a whole-pixel
//position the weights are exactly 1 and 0 and this returns the texel,
//whatever the sampler would have done.
vec4 bilinear( sampler2D s, vec2 p, vec3 filt, float secondary )
{
	vec2 q  = p - 0.5;
	vec2 f  = floor( q );
	vec2 w  = q - f;
	ivec2 i = ivec2( f );
	vec4 a  = shown( texelAt( s, i ), filt, secondary );
	vec4 b  = shown( texelAt( s, i + ivec2( 1, 0 ) ), filt, secondary );
	vec4 c  = shown( texelAt( s, i + ivec2( 0, 1 ) ), filt, secondary );
	vec4 d  = shown( texelAt( s, i + ivec2( 1, 1 ) ), filt, secondary );
	return mix( mix( a, b, w.x ), mix( c, d, w.x ), w.y );
}

//Where a panel's pixel is, given its misconvergence.
vec2 panel( vec2 p, vec2 off, float rot )
{
	if( rot != 0.0 )
	{
		vec2 c  = 0.5 * vec2( float( Width ), float( Height ) );
		vec2 d  = p - c;
		float cs = cos( rot );
		float sn = sin( rot );
		p = c + vec2( cs * d.x - sn * d.y, sn * d.x + cs * d.y );
	}
	return p + off;
}

//The segment's picture at p: one chip, or three panels each a little off.
vec4 fetchField( vec2 p, float tc, vec3 filt, float secondary )
{
	vec4 v;
	if( ChipMode == 1 )
	{
		vec4 g = bilinear( LinearTexture, p, filt, secondary );
		v.r    = bilinear( LinearTexture, panel( p, RedOffset, RedRotate ), filt, secondary ).r;
		v.g    = g.g;
		v.b    = bilinear( LinearTexture, panel( p, BlueOffset, BlueRotate ), filt, secondary ).b;
		v.a    = g.a;
	}
	else
	{
		v = bilinear( LinearTexture, p, filt, secondary );
	}

	if( Interp == 1 )
		v = mix( bilinear( PrevTexture, p, filt, secondary ), v, tc );

	return v;
}

void main()
{
	//The pixel, snapped: see Shaders.h.
	vec2 size = vec2( float( Width ), float( Height ) );
	vec2 p    = floor( uv * size ) + 0.5;

	vec3 colour = vec3( 0.0 );
	float alpha = 0.0;

	for( int k = 0; k < Count; ++k )
	{
		vec4 w         = SubW[ k ];
		vec3 filt      = vec3( w.r > 0.0 ? 1.0 : 0.0, w.g > 0.0 ? 1.0 : 0.0, w.b > 0.0 ? 1.0 : 0.0 );
		vec2 off       = SubO[ k ].xy;
		vec2 box       = SubO[ k ].zw;
		float second   = SubT[ k ].x;
		float tc       = SubT[ k ].y;

		if( BitDepth > 0 )
		{
			//Within the segment the DMD makes grey out of binary-weighted
			//planes, least significant first, each shown for its weight's
			//share of the segment. Each plane is one point on the path, so
			//with eye motion the planes of one grey land in different places
			//and two greys whose bit patterns differ break apart: dynamic
			//false contouring.
			float levels = float( ( 1 << BitDepth ) - 1 );
			float cum    = 0.0;
			for( int b = 0; b < BitDepth; ++b )
			{
				float fb = float( 1 << b ) / levels;
				float sb = cum + 0.5 * fb;
				cum += fb;
				vec4 s  = fetchField( p + off + box * ( sb - 0.5 ), tc, filt, second );
				vec3 q  = clamp( floor( s.rgb * levels + 0.5 ), 0.0, levels );
				vec3 bit = vec3( float( ( int( q.r ) >> b ) & 1 ), float( ( int( q.g ) >> b ) & 1 ), float( ( int( q.b ) >> b ) & 1 ) );
				colour += w.rgb * bit * fb;
				alpha += w.a * s.a * fb;
			}
			continue;
		}

		float len = length( box );
		if( len <= 0.0 )
		{
			//A still eye, or a sub-field of no duration: one exact sample.
			vec4 s = fetchField( p + off, tc, filt, second );
			colour += w.rgb * s.rgb;
			alpha += w.a * s.a;
			continue;
		}

		//A box along the path, as the trapezoid rule over taps no more than a
		//pixel apart: end taps at half weight. For a whole-pixel box every tap
		//is a whole pixel from the centre and every fetch is exact.
		int taps   = min( int( ceil( len - 1e-4 ) ) + 1, MAX_TAPS );
		float norm = 1.0 / float( taps - 1 );
		for( int i = 0; i < taps; ++i )
		{
			float u  = float( i ) * norm;
			float tw = ( i == 0 || i == taps - 1 ) ? 0.5 * TapBias : 1.0;
			vec4 s   = fetchField( p + off + box * ( u - 0.5 ), tc, filt, second );
			colour += w.rgb * s.rgb * ( tw * norm );
			alpha += w.a * s.a * ( tw * norm );
		}
	}

	vec3 enc    = Gamma <= 1.0 ? colour : pow( max( colour, vec3( 0.0 ) ), vec3( 1.0 / Gamma ) );
	vec4 result = vec4( enc, alpha );

	vec4 source = texelFetch( SourceTexture, clamp( ivec2( p ), ivec2( 0 ), ivec2( Width - 1, Height - 1 ) ), 0 );
	fragColor   = mix( source, result, MixAmount );
}
)";

} // namespace wheel
