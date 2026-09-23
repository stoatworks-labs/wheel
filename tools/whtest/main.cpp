/**
	whtest -- render Wheel offline, and check where the colour fields land.

	Where a colour field lands on the retina is a fact, not a matter of
	taste. This harness drives the REAL plugin class in a headless CGL context
	on a synthetic 60 fps clock, feeds it frames whose content is known
	exactly, and reads the picture back to see whether the model in
	`Segments.h` and the integrate shader did what they claim.

		whtest --out /tmp/frame.png     a picture, on the moving test card
		whtest --list                   every parameter, its kind and default
		whtest --still                  a still eye returns the input, bitwise
		whtest --separation             pursuit: red and green land v(t_G - t_R) apart
		whtest --energy                 displacement moves light and never makes it
		whtest --white                  a W segment brightens white, not a primary
		whtest --converge               three-chip: a channel offset moves that channel exactly
		whtest --saccade                an onset fires once, primed; the fringe decays over Saccade Time
		whtest --bits                   bit planes: 8 is the input, 4 is the 4-bit input
		whtest --track                  the Track eye finds a one-cell-per-frame scroll
		whtest --resize                 a resize mid-run neither crashes nor lies
		whtest --names                  no parameter name over 16 characters
		whtest --negative               every check above can fail
		whtest --bench                  the render cost, 720p through 4K
		whtest --pipe                   raw frames in, raw frames out

	Every check has one flag, every flag has one claim, and every claim is
	stated in the README's Status table with the number this printed. Every
	picture check runs at two rasters, 320x180 (what CI uses) and 1280x720.

	`--pipe` takes the fleet's frame format, so one filming script drives any
	of the plugins:

		ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
		  | whtest --pipe --size 1920x1080 [--fps 30] [--script cues.txt] \
		  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 30 -i - out.mov

	Its clock is synthetic -- milliseconds, as Resolume sends them, at --fps
	-- so the wheel, a pursuit and a saccade move per frame of the take, not
	per wall-clock second of the render. See `runPipe`.
*/

#include "Controls.h"
#include "Segments.h"
#include "Wheel.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace wheel;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Pictures. Top row first, like a file; the session flips on the way in
// and out of GL.
//---------------------------------------------------------------------------
using Image = std::vector< unsigned char >;

Image solid( int width, int height, unsigned char r, unsigned char g, unsigned char b )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < image.size(); i += 4 )
	{
		image[ i ]     = r;
		image[ i + 1 ] = g;
		image[ i + 2 ] = b;
		image[ i + 3 ] = 255;
	}
	return image;
}

void paint( Image& image, int width, int height, int x0, int y0, int x1, int y1, unsigned char r, unsigned char g, unsigned char b )
{
	for( int y = std::max( 0, y0 ); y < std::min( height, y1 ); ++y )
		for( int x = std::max( 0, x0 ); x < std::min( width, x1 ); ++x )
		{
			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			image[ i ]     = r;
			image[ i + 1 ] = g;
			image[ i + 2 ] = b;
			image[ i + 3 ] = 255;
		}
}

/// The test card. Not meant to look nice: each part exercises one claim.
///
///   - a white bar sweeping right at three pixels a frame: what a pursuit
///     fringes, and what the Track eye follows
///   - a bright disc on a Lissajous path: motion that is not a straight edge
///   - a grey ramp and six colour bars: so a wheel with a white segment has
///     greys to brighten and primaries to leave alone
///   - a white square: the thing a saccade throws a rainbow across
Image buildCard( int width, int height, int frame )
{
	const float w = static_cast< float >( width );
	const float h = static_cast< float >( height );
	const float t = static_cast< float >( frame );

	Image card( static_cast< size_t >( width ) * height * 4 );

	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;

			float r = 0.06f + 0.04f * v;
			float g = 0.06f + 0.04f * v;
			float b = 0.09f + 0.05f * v;

			if( v > 0.875f )
			{
				static const float bars[ 6 ][ 3 ] = {
					{ 1.0f, 0.1f, 0.1f }, { 0.1f, 1.0f, 0.1f }, { 0.1f, 0.1f, 1.0f },
					{ 0.1f, 1.0f, 1.0f }, { 1.0f, 0.1f, 1.0f }, { 1.0f, 1.0f, 0.1f }
				};
				const int bar = std::min( 5, static_cast< int >( u * 6.0f ) );
				r             = bars[ bar ][ 0 ];
				g             = bars[ bar ][ 1 ];
				b             = bars[ bar ][ 2 ];
			}
			else if( v > 0.75f )
			{
				r = g = b = u;
			}

			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			card[ i ]      = static_cast< unsigned char >( std::clamp( r, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 1 ]  = static_cast< unsigned char >( std::clamp( g, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 2 ]  = static_cast< unsigned char >( std::clamp( b, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 3 ]  = 255;
		}

	const int barW = std::max( 4, width / 40 );
	const int barX = ( frame * 3 ) % ( width + barW ) - barW;
	paint( card, width, height, barX, static_cast< int >( h * 0.30f ), barX + barW, static_cast< int >( h * 0.70f ), 255, 255, 255 );

	const int sq = std::max( 8, width / 20 );
	paint( card, width, height, width * 3 / 4 - sq / 2, height / 2 - sq / 2, width * 3 / 4 + sq / 2, height / 2 + sq / 2, 245, 245, 245 );

	const float discX = 0.5f + 0.34f * std::sin( t * 0.110f );
	const float discY = 0.4f + 0.20f * std::sin( t * 0.077f + 1.1f );
	const float discR = 0.05f * w;
	const int cx = static_cast< int >( discX * w ), cy = static_cast< int >( discY * h );
	for( int y = std::max( 0, cy - static_cast< int >( discR ) - 1 ); y < std::min( height, cy + static_cast< int >( discR ) + 2 ); ++y )
		for( int x = std::max( 0, cx - static_cast< int >( discR ) - 1 ); x < std::min( width, cx + static_cast< int >( discR ) + 2 ); ++x )
		{
			const float dx = static_cast< float >( x - cx ), dy = static_cast< float >( y - cy );
			if( dx * dx + dy * dy < discR * discR )
			{
				const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
				card[ i ]      = 250;
				card[ i + 1 ]  = 235;
				card[ i + 2 ]  = 200;
			}
		}

	return card;
}

/// A PCG output mix, for a textured scene the Track eye can lock onto.
uint32_t hashInt( uint32_t seed )
{
	uint32_t state = seed * 747796405u + 2891336453u;
	uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

/// Blocks of random grey, `block` pixels square, scrolled right by `shift`
/// pixels with wrap-around. Shift by a whole number of blocks and the
/// picture is exactly the previous one moved over.
Image noiseBlocks( int width, int height, int block, int shift )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const int sx = ( ( x - shift ) % width + width ) % width;
			const uint32_t h = hashInt( static_cast< uint32_t >( ( y / block ) * 4096 + sx / block ) );
			const unsigned char grey = static_cast< unsigned char >( 30 + ( h % 200 ) );
			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			image[ i ] = image[ i + 1 ] = image[ i + 2 ] = grey;
			image[ i + 3 ] = 255;
		}
	return image;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

Image flipRows( const Image& image, int width, int height )
{
	Image flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_BOOLEAN: return "boolean";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_EVENT: return "event";
	default: return "other";
	}
}

int findParameter( Wheel& plugin, const std::string& name )
{
	for( unsigned int i = 0; i < Wheel::PT_COUNT; ++i )
	{
		const char* const declared = plugin.GetParamName( i );
		if( declared != nullptr && name == declared )
			return static_cast< int >( i );
	}
	return -1;
}

bool applySetting( Wheel& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	const int index         = findParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "' (try --list)";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( value.c_str(), nullptr ) );
	return true;
}

void listParameters( Wheel& plugin )
{
	std::printf( "%3s  %-20s  %-8s %8s  range\n", "id", "name", "kind", "default" );
	for( unsigned int i = 0; i < Wheel::PT_COUNT; ++i )
	{
		const char* const name  = plugin.GetParamName( i );
		const unsigned int type = plugin.GetParamType( i );
		const float value       = plugin.GetFloatParameter( i );

		if( i >= Wheel::PT_ABOUT_FIRST )
		{
			std::printf( "%3u  %-20s  %-8s %8s  -\n", i, name ? name : "?", "about", "-" );
			continue;
		}

		float low = 0.0f, high = 1.0f;
		if( type == FF_TYPE_OPTION )
			high = static_cast< float >( plugin.GetNumParamElements( i ) - 1 );
		else if( type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			low                     = range.min;
			high                    = range.max;
		}

		if( type == FF_TYPE_BUFFER )
			std::printf( "%3u  %-20s  %-8s %8s  -\n", i, name ? name : "?", kindName( type ), "-" );
		else
			std::printf( "%3u  %-20s  %-8s %8.4f  [ %g .. %g ]\n", i, name ? name : "?", kindName( type ), value, low, high );
	}
}

/// A click train through the same call the host uses: every sixth frame a
/// hit in the low bins, a slow sawtooth in the top ones so the detector has
/// steady flux to be decisive against.
void injectTone( Wheel& plugin, int frame )
{
	const bool click = ( frame % 6 ) == 0 && frame > 0;
	const float ramp = static_cast< float >( frame % 20 ) / 20.0f;
	for( int i = 0; i < kAudioBins; ++i )
	{
		float value = 0.0f;
		if( i < 8 )
			value = click ? 0.85f : 0.05f;
		else if( i < 28 )
			value = 0.04f;
		else
			value = 0.03f + 0.30f * ramp;
		plugin.SetParamElementValue( Wheel::PT_AUDIO, static_cast< unsigned int >( i ), value );
	}
}

void injectSpectrum( Wheel& plugin, float low, float rest )
{
	for( int i = 0; i < kAudioBins; ++i )
		plugin.SetParamElementValue( Wheel::PT_AUDIO, static_cast< unsigned int >( i ), i < 8 ? low : rest );
}

//---------------------------------------------------------------------------
// A session: one plugin, one picture size, frames in and pictures out.
//---------------------------------------------------------------------------
class Session
{
public:
	Session( int width, int height )
	{
		resize( width, height );
	}

	~Session()
	{
		release();
		if( initialised )
			plugin.DeInitGL();
	}

	Wheel& Plugin()
	{
		return plugin;
	}

	bool set( const std::string& assignment )
	{
		std::string error;
		if( applySetting( plugin, assignment, error ) )
			return true;
		std::fprintf( stderr, "--set %s: %s\n", assignment.c_str(), error.c_str() );
		return false;
	}

	bool setAll( const std::vector< std::string >& assignments )
	{
		for( const std::string& a : assignments )
			if( !set( a ) )
				return false;
		return true;
	}

	void resize( int newWidth, int newHeight )
	{
		release();
		width         = newWidth;
		height        = newHeight;
		sourceTexture = makeTexture( width, height, nullptr );
		outputTexture = makeTexture( width, height, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );
	}

	bool init()
	{
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		initialised = true;
		return true;
	}

	/// One frame in, one picture out, both top row first.
	bool render( const Image& source, int frame, Image* out )
	{
		if( pipeFps > 0.0 )
		{
			//--pipe: milliseconds, as Resolume sends them, at the take's own
			//frame rate. Computed from the frame index in double every time,
			//never accumulated, so frame 100,000 is as exact as frame 1.
			//Declared rather than measured for the same reason as below.
			plugin.SetClockScaleForTest( 0.001 );
			plugin.SetTime( static_cast< double >( frame ) * 1000.0 / pipeFps );
		}
		else
		{
			//A synthetic clock, declared in seconds: the harness renders as
			//fast as the GPU allows, so the unit could not be measured.
			plugin.SetClockScaleForTest( 1.0 );
			plugin.SetTime( static_cast< double >( frame ) / 60.0 );
		}

		if( tone )
			injectTone( plugin, frame );
		if( fireFrame == frame )
			plugin.SetFloatParameter( Wheel::PT_FIRE, 1.0f );

		const Image flipped = flipRows( source, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );

		FFGLTextureStruct inputStruct = {};
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

		ProcessOpenGLStruct process = {};
		process.numInputTextures    = 1;
		process.inputTextures       = inputs;
		process.HostFBO             = outputFBO;

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			return false;

		if( out != nullptr )
		{
			Image raw( static_cast< size_t >( width ) * height * 4 );
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glPixelStorei( GL_PACK_ALIGNMENT, 1 );
			glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, raw.data() );
			*out = flipRows( raw, width, height );
		}
		return true;
	}

	int width     = 0;
	int height    = 0;
	bool tone      = false;
	int fireFrame  = -1;
	double pipeFps = 0.0;//> 0: the --pipe clock, milliseconds at this rate

private:
	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	Wheel plugin;
	bool initialised     = false;
	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
};

//---------------------------------------------------------------------------
// Bookkeeping.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;

void check( bool ok, const char* what )
{
	++g_checks;
	if( !ok )
		++g_failures;
	std::printf( "   %s %s\n", ok ? "ok  " : "FAIL", what );
}

struct Raster
{
	int width, height;
};
const Raster kRasters[] = { { 320, 180 }, { 1280, 720 } };

std::string gammaLinear()
{
	return "Output Gamma=" + std::to_string( GammaParam( 1.0f ) );
}

std::string pursuit( float px )
{
	return "Pursuit Speed=" + std::to_string( PursuitSpeedParam( px ) );
}

//---------------------------------------------------------------------------
// Reading pictures.
//---------------------------------------------------------------------------
size_t differingBytes( const Image& a, const Image& b, int& worst )
{
	size_t n = 0;
	worst    = 0;
	for( size_t i = 0; i < a.size() && i < b.size(); ++i )
	{
		const int d = std::abs( static_cast< int >( a[ i ] ) - static_cast< int >( b[ i ] ) );
		if( d != 0 )
			++n;
		worst = std::max( worst, d );
	}
	return n;
}

/// One channel's column profile over a band of rows, and its coverage-
/// weighted centroid in columns. `bound` is how far 8-bit rounding of the
/// coverage could have moved that centroid: every non-zero pixel may be
/// off by half a code, so at worst sum( |x - c| * 0.5 ) / mass.
struct Profile
{
	double centroid = 0.0;
	double mass     = 0.0;
	double bound    = 0.0;
};

Profile profileOf( const Image& image, int width, int y0, int y1, int channel )
{
	std::vector< double > column( static_cast< size_t >( width ), 0.0 );
	for( int y = y0; y < y1; ++y )
		for( int x = 0; x < width; ++x )
			column[ x ] += image[ ( static_cast< size_t >( y ) * width + x ) * 4 + channel ];

	Profile p;
	double moment = 0.0;
	for( int x = 0; x < width; ++x )
	{
		p.mass += column[ x ];
		moment += column[ x ] * x;
	}
	if( p.mass <= 0.0 )
		return p;
	p.centroid = moment / p.mass;

	double spread = 0.0;
	for( int x = 0; x < width; ++x )
		if( column[ x ] > 0.0 )
			spread += std::fabs( x - p.centroid ) * 0.5 * ( y1 - y0 );
	p.bound = spread / p.mass;
	return p;
}

/// Two-dimensional centroid of one channel, with the same bound.
struct Centroid2
{
	double x = 0.0, y = 0.0, mass = 0.0, bound = 0.0;
};

Centroid2 centroidOf( const Image& image, int width, int height, int channel )
{
	Centroid2 c;
	double mx = 0.0, my = 0.0;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double v = image[ ( static_cast< size_t >( y ) * width + x ) * 4 + channel ];
			c.mass += v;
			mx += v * x;
			my += v * y;
		}
	if( c.mass <= 0.0 )
		return c;
	c.x = mx / c.mass;
	c.y = my / c.mass;
	double spread = 0.0;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double v = image[ ( static_cast< size_t >( y ) * width + x ) * 4 + channel ];
			if( v > 0.0 )
				spread += std::hypot( x - c.x, y - c.y ) * 0.5;
		}
	c.bound = spread / c.mass;
	return c;
}

double channelSum( const Image& image, int channel )
{
	double s = 0.0;
	for( size_t i = channel; i < image.size(); i += 4 )
		s += image[ i ];
	return s;
}

//---------------------------------------------------------------------------
// --still
//
// A still eye, an RGB wheel: the output IS the input, at every wheel speed,
// on a moving clip, with the gamma round trip and without it, and on a
// custom wheel with unequal widths. Bitwise in 8 bits.
//---------------------------------------------------------------------------
int runStill( bool quiet = false )
{
	std::printf( "== still: a still eye returns the input\n" );

	struct Case
	{
		const char* name;
		std::vector< std::string > settings;
	};
	const Case cases[] = {
		{ "RGB, gamma 2.2", { "Wheel Type=0" } },
		{ "RGB, linear", { "Wheel Type=0", gammaLinear() } },
		{ "Custom widths 0.5/0.2/0.3/0, gamma 2.2", { "Wheel Type=3", "Red Width=0.5", "Green Width=0.2", "Blue Width=0.3", "White Width=0" } },
	};

	for( const Raster& r : kRasters )
		for( const Case& c : cases )
			for( int speed = 0; speed < kWheelSpeedCount; ++speed )
			{
				Session session( r.width, r.height );
				if( !session.setAll( c.settings ) || !session.set( "Eye Mode=0" )
				    || !session.set( "Wheel Speed=" + std::to_string( speed ) ) || !session.init() )
					return 1;

				size_t differing = 0;
				int worst        = 0;
				size_t compared  = 0;
				Image out;
				for( int frame = 0; frame < 6; ++frame )
				{
					const Image card = buildCard( r.width, r.height, frame );
					if( !session.render( card, frame, &out ) )
					{
						std::fprintf( stderr, "still: ProcessOpenGL failed\n" );
						return 1;
					}
					int w = 0;
					differing += differingBytes( card, out, w );
					worst = std::max( worst, w );
					compared += card.size();
				}
				if( !quiet )
					std::printf( "   %dx%d %s at %s: %zu of %zu bytes differ, worst %d\n", r.width, r.height, c.name,
					             RotationsFromOption( static_cast< float >( speed ) ) == 1 ? "1x" : ( std::to_string( RotationsFromOption( static_cast< float >( speed ) ) ) + "x" ).c_str(),
					             differing, compared, worst );
				if( differing != 0 || speed == kWheelSpeedCount - 1 )
					check( differing == 0, ( std::string( c.name ) + " at " + std::to_string( r.width ) + "x" + std::to_string( r.height ) + ": bitwise identity at every wheel speed" ).c_str() );
			}

	return 0;
}

//---------------------------------------------------------------------------
// --separation
//
// Pursuit at v px/frame on an RGB wheel: the red field's centroid leads
// the green one's by v * ( t_G - t_R ), measured from the picture, where
// t_G - t_R = ( 1 / 3 ) / rotations is the wheel's stated geometry. Whole
// pixel: red is green translated, bitwise. Fractional: coverage-weighted
// centroids, to the derived 8-bit bound. And the 2x separation is half the
// 1x.
//---------------------------------------------------------------------------
struct SeparationResult
{
	double redGreen = 0.0, greenBlue = 0.0, bound = 0.0;
	bool redIsGreenShifted = false;
	int shiftChecked       = 0;
};

bool measureSeparation( int width, int height, int speedOption, float v, double timeScale, SeparationResult& out )
{
	Session session( width, height );
	session.Plugin().SetTimeScaleForTest( timeScale );
	if( !session.setAll( { "Wheel Type=0", "Eye Mode=1", "Pursuit Angle=0", gammaLinear(), pursuit( v ),
	                    "Wheel Speed=" + std::to_string( speedOption ) } )
	    || !session.init() )
		return false;

	const int barW = 8;
	const int x0   = width / 2 - barW / 2;
	Image source   = solid( width, height, 0, 0, 0 );
	paint( source, width, height, x0, 0, x0 + barW, height, 255, 255, 255 );

	Image image;
	for( int frame = 0; frame < 3; ++frame )
		if( !session.render( source, frame, &image ) )
			return false;

	const int y0 = height / 4, y1 = height * 3 / 4;
	const Profile r = profileOf( image, width, y0, y1, 0 );
	const Profile g = profileOf( image, width, y0, y1, 1 );
	const Profile b = profileOf( image, width, y0, y1, 2 );
	out.redGreen  = r.centroid - g.centroid;
	out.greenBlue = g.centroid - b.centroid;
	out.bound     = r.bound + g.bound;

	//Whole-pixel: red IS green moved over by d, bitwise, on every pixel
	//whose source lies inside the picture.
	const int rotations = RotationsFromOption( static_cast< float >( speedOption ) );
	const double d      = v / 3.0 / rotations;
	const int shift     = static_cast< int >( std::lround( d ) );
	out.redIsGreenShifted = true;
	out.shiftChecked      = 0;
	if( std::fabs( d - shift ) < 1e-9 )
	{
		for( int y = 0; y < height; ++y )
			for( int x = 8; x < width - 8; ++x )
			{
				const unsigned char red   = image[ ( static_cast< size_t >( y ) * width + x ) * 4 + 0 ];
				const unsigned char green = image[ ( static_cast< size_t >( y ) * width + x - shift ) * 4 + 1 ];
				++out.shiftChecked;
				if( red != green )
					out.redIsGreenShifted = false;
			}
	}
	return true;
}

int runSeparation()
{
	std::printf( "== separation: pursuit lands the fields v( t_G - t_R ) apart\n" );

	for( const Raster& r : kRasters )
	{
		//v = 6 at 1x is a 2 px separation; at 2x, 1 px. v = 4.5 at 1x is 1.5
		//px and at 2x 0.75 px: fractional, so nothing can pass by landing on
		//whole pixels.
		const float speeds[] = { 6.0f, 4.5f };
		for( float v : speeds )
		{
			SeparationResult at1, at2;
			if( !measureSeparation( r.width, r.height, 0, v, 1.0, at1 ) || !measureSeparation( r.width, r.height, 1, v, 1.0, at2 ) )
			{
				std::fprintf( stderr, "separation: render failed\n" );
				return 1;
			}
			const double want1 = v / 3.0, want2 = v / 6.0;
			const bool whole   = std::fabs( want1 - std::lround( want1 ) ) < 1e-9;

			std::printf( "   %dx%d v=%.1f 1x: R-G %.4f (want %.4f), G-B %.4f, 8-bit bound %.4f; 2x: R-G %.4f (want %.4f), G-B %.4f, bound %.4f\n",
			             r.width, r.height, v, at1.redGreen, want1, at1.greenBlue, at1.bound, at2.redGreen, want2, at2.greenBlue, at2.bound );

			const std::string tag = std::to_string( r.width ) + "x" + std::to_string( r.height ) + " v=" + std::to_string( static_cast< int >( v * 10 ) ) + "/10";
			if( whole )
			{
				std::printf( "   whole pixel: red is green shifted by %d over %d pixels: %s\n", static_cast< int >( want1 ), at1.shiftChecked, at1.redIsGreenShifted ? "bitwise" : "NO" );
				check( at1.redIsGreenShifted && at1.shiftChecked > 0, ( tag + " 1x whole-pixel separation is an exact translation" ).c_str() );
			}
			check( std::fabs( at1.redGreen - want1 ) <= at1.bound, ( tag + " 1x R-G centroid separation within the 8-bit bound" ).c_str() );
			check( std::fabs( at1.greenBlue - want1 ) <= at1.bound, ( tag + " 1x G-B centroid separation within the 8-bit bound" ).c_str() );
			check( std::fabs( at2.redGreen - want2 ) <= at2.bound, ( tag + " 2x R-G centroid separation within the 8-bit bound" ).c_str() );
			//The ratio: half, to the two bounds combined.
			const double ratio    = at2.redGreen / at1.redGreen;
			const double ratioTol = ( at2.bound + 0.5 * at1.bound ) / std::fabs( at1.redGreen );
			std::printf( "   ratio 2x/1x = %.4f (want 0.5, tolerance %.4f)\n", ratio, ratioTol );
			check( std::fabs( ratio - 0.5 ) <= ratioTol, ( tag + " 2x separation is half the 1x" ).c_str() );
		}
	}
	return 0;
}

//---------------------------------------------------------------------------
// --energy
//
// The per-channel sum over the frame does not depend on the eye's
// velocity. Displacement moves light and neither makes nor loses it --
// except by clamping at the picture's edge, which is why the patch sits in
// the middle with room around it. Linear gamma, because the claim is
// additive in linear light.
//---------------------------------------------------------------------------
bool measureEnergy( int width, int height, const std::vector< std::string >& settings, float bias, double sums[ 3 ], size_t& support )
{
	Session session( width, height );
	session.Plugin().SetTapBiasForTest( bias );
	if( !session.setAll( settings ) || !session.set( gammaLinear() ) || !session.init() )
		return false;

	//A patch with greys in it: a ramp across, so partial values exist for
	//rounding to act on -- and dim enough that a wheel with secondaries,
	//which add light on top of the primaries, cannot push any channel past
	//1.0. Past 1.0 the 8-bit output clips, and a clipped channel is not
	//linear in anything.
	Image source = solid( width, height, 0, 0, 0 );
	const int pw = width / 4, ph = height / 4;
	for( int y = height / 2 - ph / 2; y < height / 2 + ph / 2; ++y )
		for( int x = width / 2 - pw / 2; x < width / 2 + pw / 2; ++x )
		{
			const unsigned char g = static_cast< unsigned char >( 20 + ( 100 * ( x - ( width / 2 - pw / 2 ) ) ) / pw );
			const size_t i        = ( static_cast< size_t >( y ) * width + x ) * 4;
			source[ i ]           = 100;
			source[ i + 1 ]       = g;
			source[ i + 2 ]       = static_cast< unsigned char >( 140 - g );
		}

	Image out;
	for( int frame = 0; frame < 3; ++frame )
		if( !session.render( source, frame, &out ) )
			return false;

	for( int c = 0; c < 3; ++c )
		sums[ c ] = channelSum( out, c );
	support = 0;
	for( size_t i = 0; i < out.size(); i += 4 )
		if( out[ i ] || out[ i + 1 ] || out[ i + 2 ] )
			++support;
	return true;
}

int runEnergy()
{
	std::printf( "== energy: displacement moves light and never makes or loses it\n" );

	struct Case
	{
		const char* name;
		std::vector< std::string > wheel;///< the wheel; the still reference uses exactly this
		std::vector< std::string > eye;  ///< the motion on top
	};
	const Case cases[] = {
		{ "pursuit 6 px rightwards, RGB 2x", { "Wheel Type=0", "Wheel Speed=1" }, { "Eye Mode=1", "Pursuit Angle=0", pursuit( 6.0f ) } },
		{ "pursuit 13.7 px at 37 deg, RGB 2x", { "Wheel Type=0", "Wheel Speed=1" }, { "Eye Mode=1", "Pursuit Angle=0.1028", pursuit( 13.7f ) } },
		{ "pursuit 9 px at 200 deg, RGBCMY 3x", { "Wheel Type=2", "Wheel Speed=2" }, { "Eye Mode=1", "Pursuit Angle=0.5556", pursuit( 9.0f ) } },
		{ "pursuit 6 px, three chip", { "Chip Mode=1" }, { "Eye Mode=1", "Pursuit Angle=0", pursuit( 6.0f ) } },
	};

	for( const Raster& r : kRasters )
		for( const Case& c : cases )
		{
			std::vector< std::string > stillSettings = c.wheel;
			stillSettings.push_back( "Eye Mode=0" );
			std::vector< std::string > movingSettings = c.wheel;
			movingSettings.insert( movingSettings.end(), c.eye.begin(), c.eye.end() );

			double still[ 3 ], moving[ 3 ];
			size_t support = 0, movingSupport = 0;
			if( !measureEnergy( r.width, r.height, stillSettings, 1.0f, still, support )
			    || !measureEnergy( r.width, r.height, movingSettings, 1.0f, moving, movingSupport ) )
				return 1;

			//Worst case: every lit pixel of the moving picture rounded the
			//same way by half a code, and every lit pixel of the still one
			//the other way.
			const double bound = 0.5 * static_cast< double >( support + movingSupport );
			double worstRel    = 0.0;
			for( int ch = 0; ch < 3; ++ch )
				worstRel = std::max( worstRel, std::fabs( moving[ ch ] - still[ ch ] ) / still[ ch ] );
			std::printf( "   %dx%d %s: sums R %.0f G %.0f B %.0f vs still %.0f %.0f %.0f; worst %.2e relative, bound %.2e\n",
			             r.width, r.height, c.name, moving[ 0 ], moving[ 1 ], moving[ 2 ], still[ 0 ], still[ 1 ], still[ 2 ], worstRel, bound / still[ 1 ] );
			bool ok = true;
			for( int ch = 0; ch < 3; ++ch )
				ok = ok && std::fabs( moving[ ch ] - still[ ch ] ) <= bound;
			check( ok, ( std::string( c.name ) + " at " + std::to_string( r.width ) + "x" + std::to_string( r.height ) + ": per-channel energy unchanged within the 8-bit bound" ).c_str() );
		}
	return 0;
}

//---------------------------------------------------------------------------
// --white
//
// An RGBW wheel with a still eye: a grey patch comes out brighter by White
// Gain times the W segment's width over the red segment's, computed here
// from the stated widths; a saturated primary comes out unchanged. Linear
// gamma so the number is a number.
//---------------------------------------------------------------------------
struct WhiteResult
{
	int grey = 0, red = 0, redG = 0, greenG = 0;
};

bool measureWhite( int width, int height, const std::vector< std::string >& settings, WhiteResult& out )
{
	Session session( width, height );
	if( !session.setAll( settings ) || !session.setAll( { "Eye Mode=0", "Wheel Speed=0", gammaLinear() } ) || !session.init() )
		return false;

	Image source = solid( width, height, 0, 0, 0 );
	paint( source, width, height, width / 8, height / 4, width * 3 / 8, height * 3 / 4, 64, 64, 64 );
	paint( source, width, height, width / 2, height / 4, width * 5 / 8, height * 3 / 4, 255, 0, 0 );
	paint( source, width, height, width * 3 / 4, height / 4, width * 7 / 8, height * 3 / 4, 0, 255, 0 );

	Image image;
	if( !session.render( source, 0, &image ) )
		return false;

	auto at = [ & ]( int x, int y, int c ) { return static_cast< int >( image[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ] ); };
	out.grey   = at( width / 4, height / 2, 0 );
	out.red    = at( width * 9 / 16, height / 2, 0 );
	out.redG   = at( width * 9 / 16, height / 2, 1 );
	out.greenG = at( width * 13 / 16, height / 2, 1 );
	return true;
}

int runWhite()
{
	std::printf( "== white: a W segment brightens white by its share and leaves a primary alone\n" );

	struct Case
	{
		const char* name;
		std::vector< std::string > settings;
		double wOverR;///< the W segment's width over the red one's, from the wheel's own definition
		double gain;
	};
	const Case cases[] = {
		{ "RGBW equal quarters, White Gain 1", { "Wheel Type=1", "White Gain=1" }, 1.0, 1.0 },
		{ "RGBW equal quarters, White Gain 0.5", { "Wheel Type=1", "White Gain=0.5" }, 1.0, 0.5 },
		{ "Custom R.3 G.3 B.3 W.1, White Gain 1", { "Wheel Type=3", "Red Width=0.3", "Green Width=0.3", "Blue Width=0.3", "White Width=0.1", "White Gain=1" }, 0.1 / 0.3, 1.0 },
	};

	for( const Raster& r : kRasters )
		for( const Case& c : cases )
		{
			WhiteResult w;
			if( !measureWhite( r.width, r.height, c.settings, w ) )
				return 1;
			const double want = 64.0 * ( 1.0 + c.gain * c.wOverR );
			std::printf( "   %dx%d %s: grey 64 -> %d (want %.2f); red 255 -> %d, its green %d; green's green %d\n",
			             r.width, r.height, c.name, w.grey, want, w.red, w.redG, w.greenG );
			//One code: the expectation is not always an integer, and the
			//measurement is one.
			check( std::fabs( w.grey - want ) <= 1.0, ( std::string( c.name ) + " at " + std::to_string( r.width ) + ": grey raised by the W share, within one code" ).c_str() );
			check( w.red == 255 && w.redG == 0 && w.greenG == 255, ( std::string( c.name ) + " at " + std::to_string( r.width ) + ": saturated primaries unchanged" ).c_str() );
		}
	return 0;
}

//---------------------------------------------------------------------------
// --converge
//
// Three-chip mode, still eye. A whole-pixel offset of ( dx, dy ) on a
// channel moves that channel by exactly ( dx, dy ): bitwise, over every
// pixel whose source is inside the picture. A fractional offset moves its
// centroid by the offset, to the 8-bit bound.
//---------------------------------------------------------------------------
int runConverge()
{
	std::printf( "== converge: a panel offset moves that channel exactly\n" );

	for( const Raster& r : kRasters )
	{
		//Whole pixels, with the gamma round trip in the path.
		{
			const int rdx = 2, rdy = -3, bdx = -1, bdy = 1;
			Session session( r.width, r.height );
			if( !session.setAll( { "Chip Mode=1", "Eye Mode=0",
			                    "Red dx=" + std::to_string( ChipOffsetParam( rdx ) ), "Red dy=" + std::to_string( ChipOffsetParam( rdy ) ),
			                    "Blue dx=" + std::to_string( ChipOffsetParam( bdx ) ), "Blue dy=" + std::to_string( ChipOffsetParam( bdy ) ) } )
			    || !session.init() )
				return 1;

			const Image card = buildCard( r.width, r.height, 4 );
			Image out;
			if( !session.render( card, 0, &out ) )
				return 1;

			//The output at ( x, y ) shows red from ( x + rdx, y + rdy ) in
			//picture space: the panel is displaced by ( dx, dy ), so its
			//pixel x lands at x - dx and what lands at x came from x + dx.
			//Either sign convention is a convention; this one is the
			//plugin's, and the check is that the magnitude is exact.
			size_t wrong = 0, checked = 0;
			for( int y = 8; y < r.height - 8; ++y )
				for( int x = 8; x < r.width - 8; ++x )
				{
					const size_t o  = ( static_cast< size_t >( y ) * r.width + x ) * 4;
					const size_t sr = ( static_cast< size_t >( y + rdy ) * r.width + x + rdx ) * 4;
					const size_t sb = ( static_cast< size_t >( y + bdy ) * r.width + x + bdx ) * 4;
					++checked;
					if( out[ o ] != card[ sr ] || out[ o + 1 ] != card[ o + 1 ] || out[ o + 2 ] != card[ sb + 2 ] )
						++wrong;
				}
			std::printf( "   %dx%d red (%d,%d) blue (%d,%d): %zu of %zu pixels wrong\n", r.width, r.height, rdx, rdy, bdx, bdy, wrong, checked );
			check( wrong == 0, ( std::to_string( r.width ) + "x" + std::to_string( r.height ) + ": whole-pixel offsets translate each channel bitwise" ).c_str() );
		}

		//Fractional: a bar's centroid per channel, linear gamma.
		{
			const float rdx = 1.5f, bdx = -2.25f;
			Session session( r.width, r.height );
			if( !session.setAll( { "Chip Mode=1", "Eye Mode=0", gammaLinear(),
			                    "Red dx=" + std::to_string( ChipOffsetParam( rdx ) ), "Blue dx=" + std::to_string( ChipOffsetParam( bdx ) ) } )
			    || !session.init() )
				return 1;
			Image source = solid( r.width, r.height, 0, 0, 0 );
			paint( source, r.width, r.height, r.width / 2 - 4, 0, r.width / 2 + 4, r.height, 255, 255, 255 );
			Image out;
			if( !session.render( source, 0, &out ) )
				return 1;
			const Profile pr = profileOf( out, r.width, 0, r.height, 0 );
			const Profile pg = profileOf( out, r.width, 0, r.height, 1 );
			const Profile pb = profileOf( out, r.width, 0, r.height, 2 );
			std::printf( "   %dx%d fractional: red centroid - green = %.4f (want %.2f), blue - green = %.4f (want %.2f), bound %.4f\n",
			             r.width, r.height, pr.centroid - pg.centroid, -rdx, pb.centroid - pg.centroid, -bdx, pr.bound + pg.bound );
			check( std::fabs( ( pr.centroid - pg.centroid ) + rdx ) <= pr.bound + pg.bound, ( std::to_string( r.width ) + ": fractional red offset moves the centroid by the offset" ).c_str() );
			check( std::fabs( ( pb.centroid - pg.centroid ) + bdx ) <= pb.bound + pg.bound, ( std::to_string( r.width ) + ": fractional blue offset moves the centroid by the offset" ).c_str() );
		}
	}
	return 0;
}

//---------------------------------------------------------------------------
// --saccade
//
// Silence for ten frames, then a hit. The detector is primed on frame 0 so
// nothing fires until the hit; exactly one saccade fires, on the frame of
// the hit; and the red-green separation it throws across a white square
// decays by e^-1 over Saccade Time.
//---------------------------------------------------------------------------
struct SaccadeRun
{
	unsigned long onsetsBeforeHit = 0;
	unsigned long saccadesAtHit   = 0;
	unsigned long saccadesAtEnd   = 0;
	std::vector< double > separation;//from the hit frame on
	std::vector< double > bound;
};

bool measureSaccade( int width, int height, double tauFrames, bool primed, SaccadeRun& run )
{
	Session session( width, height );
	session.Plugin().SetOnsetPrimingForTest( primed );
	if( !session.setAll( { "Wheel Type=0", "Wheel Speed=0", "Eye Mode=3", gammaLinear(),
	                    "Saccade Size=" + std::to_string( SaccadeSizeParam( 60.0f ) ),
	                    "Saccade Time=" + std::to_string( SaccadeTimeParam( static_cast< float >( tauFrames / 60.0 ) ) ) } )
	    || !session.init() )
		return false;

	const int sq = 16;
	Image source = solid( width, height, 0, 0, 0 );
	paint( source, width, height, width / 2 - sq / 2, height / 2 - sq / 2, width / 2 + sq / 2, height / 2 + sq / 2, 255, 255, 255 );

	const int hit = 10;
	Image out;
	for( int frame = 0; frame < hit + 12; ++frame )
	{
		if( frame == hit )
			injectSpectrum( session.Plugin(), 0.9f, 0.02f );
		else
			injectSpectrum( session.Plugin(), 0.02f, 0.02f );
		if( !session.render( source, frame, &out ) )
			return false;

		if( frame == hit - 1 )
			run.onsetsBeforeHit = session.Plugin().OnsetForTest().Count();
		if( frame == hit )
			run.saccadesAtHit = session.Plugin().SaccadesForTest();
		if( frame >= hit )
		{
			const Centroid2 cr = centroidOf( out, width, height, 0 );
			const Centroid2 cg = centroidOf( out, width, height, 1 );
			run.separation.push_back( std::hypot( cr.x - cg.x, cr.y - cg.y ) );
			run.bound.push_back( cr.bound + cg.bound );
		}
	}
	run.saccadesAtEnd = session.Plugin().SaccadesForTest();
	return true;
}

int runSaccade()
{
	std::printf( "== saccade: an onset fires once, primed, and the fringe decays over Saccade Time\n" );
	const double tau = 4.0;//frames

	for( const Raster& r : kRasters )
	{
		SaccadeRun run;
		if( !measureSaccade( r.width, r.height, tau, true, run ) )
			return 1;

		std::printf( "   %dx%d: onsets before the hit %lu, saccades on the hit frame %lu, at the end %lu\n", r.width, r.height, run.onsetsBeforeHit, run.saccadesAtHit, run.saccadesAtEnd );
		std::printf( "   R-G separation from the hit:" );
		for( double s : run.separation )
			std::printf( " %.3f", s );
		std::printf( "\n" );

		check( run.onsetsBeforeHit == 0, ( std::to_string( r.width ) + ": nothing fires before the first hit (the detector is primed)" ).c_str() );
		check( run.saccadesAtHit == 1 && run.saccadesAtEnd == 1, ( std::to_string( r.width ) + ": exactly one saccade, on the frame of the hit" ).c_str() );

		//e^-1 after tau frames, at three starting points. The tolerance is
		//the two centroid bounds over the later, smaller separation.
		for( int start = 0; start < 3; ++start )
		{
			const double s0 = run.separation[ start ], s1 = run.separation[ start + static_cast< int >( tau ) ];
			const double ratio = s1 / s0;
			const double tol   = ( run.bound[ start + static_cast< int >( tau ) ] + run.bound[ start ] * std::exp( -1.0 ) ) / s0;
			std::printf( "   frames %d -> %d: ratio %.4f (want %.4f, tolerance %.4f)\n", start, start + static_cast< int >( tau ), ratio, std::exp( -1.0 ), tol );
			check( std::fabs( ratio - std::exp( -1.0 ) ) <= tol, ( std::to_string( r.width ) + ": the fringe decays by e^-1 over Saccade Time" ).c_str() );
		}
	}
	return 0;
}

//---------------------------------------------------------------------------
// --bits
//
// Bit planes, still eye, linear gamma: depth 8 is the input, bitwise, and
// depth 4 is the input quantised to 16 levels, bitwise. With eye motion
// the planes come apart: depth 4 differs from depth 8.
//---------------------------------------------------------------------------
bool renderBits( int width, int height, int depth, bool moving, Image& out )
{
	Session session( width, height );
	if( !session.setAll( { "Wheel Type=0", "Wheel Speed=0", gammaLinear(), "Bit Planes=1", "Bit Depth=" + std::to_string( depth ),
	                    moving ? "Eye Mode=1" : "Eye Mode=0", "Pursuit Angle=0", pursuit( 6.0f ) } )
	    || !session.init() )
		return false;
	return session.render( buildCard( width, height, 3 ), 0, &out );
}

int runBits()
{
	std::printf( "== bits: bit planes reconstruct the grey they were cut from\n" );
	for( const Raster& r : kRasters )
	{
		const Image card = buildCard( r.width, r.height, 3 );
		Image eight, four, movingFour, movingEight;
		if( !renderBits( r.width, r.height, 8, false, eight ) || !renderBits( r.width, r.height, 4, false, four )
		    || !renderBits( r.width, r.height, 4, true, movingFour ) || !renderBits( r.width, r.height, 8, true, movingEight ) )
			return 1;

		int worst = 0;
		const size_t d8 = differingBytes( card, eight, worst );

		Image wanted( card.size() );
		for( size_t i = 0; i < card.size(); ++i )
		{
			if( i % 4 == 3 )
			{
				wanted[ i ] = card[ i ];
				continue;
			}
			const int q = static_cast< int >( std::floor( card[ i ] / 255.0 * 15.0 + 0.5 ) );
			wanted[ i ] = static_cast< unsigned char >( std::lround( q / 15.0 * 255.0 ) );
		}
		const size_t d4 = differingBytes( wanted, four, worst );
		const size_t dm = differingBytes( movingFour, movingEight, worst );

		std::printf( "   %dx%d: depth 8 vs input %zu bytes differ; depth 4 vs 4-bit input %zu; moving, depth 4 vs 8: %zu differ\n", r.width, r.height, d8, d4, dm );
		check( d8 == 0, ( std::to_string( r.width ) + ": eight planes are the input, bitwise" ).c_str() );
		check( d4 == 0, ( std::to_string( r.width ) + ": four planes are the 4-bit input, bitwise" ).c_str() );
		check( dm > 0, ( std::to_string( r.width ) + ": under eye motion the planes come apart" ).c_str() );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --track
//
// Random blocks scrolling right by exactly one grid cell a frame. The
// estimate is a whole number of cells, so the answer is exact; the
// smoothing halves the error every frame, so after twelve it is under a
// thousandth of a cell.
//---------------------------------------------------------------------------
int runTrack()
{
	std::printf( "== track: the Track eye finds a one-cell-per-frame scroll\n" );
	for( const Raster& r : kRasters )
	{
		const int cellW = r.width / 32;
		const int cellH = r.height / 18;
		Session session( r.width, r.height );
		if( !session.setAll( { "Eye Mode=2" } ) || !session.init() )
			return 1;

		double vx = 0.0, vy = 0.0;
		for( int frame = 0; frame < 14; ++frame )
		{
			if( !session.render( noiseBlocks( r.width, r.height, cellW, frame * cellW ), frame, nullptr ) )
				return 1;
			session.Plugin().TrackVelocityForTest( vx, vy );
		}
		std::printf( "   %dx%d, cell %dx%d: estimate (%.4f, %.4f) px/frame, want (%d, 0)\n", r.width, r.height, cellW, cellH, vx, vy, cellW );
		check( std::fabs( vx - cellW ) <= 0.01 * cellW && std::fabs( vy ) <= 0.01 * cellH, ( std::to_string( r.width ) + ": one cell a frame, to a hundredth of a cell" ).c_str() );

		//And a still picture is still: the scroll stops where it was and
		//holds. The smoothing halves the estimate each frame, so ten frames
		//take a cell down to a thousandth of one.
		for( int frame = 14; frame < 24; ++frame )
			if( !session.render( noiseBlocks( r.width, r.height, cellW, 13 * cellW ), frame, nullptr ) )
				return 1;
		session.Plugin().TrackVelocityForTest( vx, vy );
		std::printf( "   after ten still frames: (%.4f, %.4f)\n", vx, vy );
		check( std::fabs( vx ) <= 0.01 * cellW && std::fabs( vy ) <= 0.01 * cellH, ( std::to_string( r.width ) + ": a still picture reads as still" ).c_str() );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --resize
//
// The picture changes size mid-run, on one instance. A still eye must
// still return the input, bitwise, on the first frame at the new size; the
// Track eye must not crash and must start again from zero.
//---------------------------------------------------------------------------
int runResize()
{
	std::printf( "== resize: a resize mid-run neither crashes nor lies\n" );
	{
		Session session( 320, 180 );
		if( !session.setAll( { "Eye Mode=0" } ) || !session.init() )
			return 1;
		for( int frame = 0; frame < 5; ++frame )
			if( !session.render( buildCard( 320, 180, frame ), frame, nullptr ) )
				return 1;
		session.resize( 400, 200 );
		const Image card = buildCard( 400, 200, 5 );
		Image out;
		if( !session.render( card, 5, &out ) )
		{
			check( false, "still eye: the first frame after a resize renders" );
			return 1;
		}
		int worst = 0;
		const size_t d = differingBytes( card, out, worst );
		std::printf( "   still, 320x180 -> 400x200: %zu bytes differ on the first frame\n", d );
		check( d == 0, "still eye: the first frame after a resize is the input, bitwise" );
	}
	{
		Session session( 320, 180 );
		if( !session.setAll( { "Eye Mode=2" } ) || !session.init() )
			return 1;
		for( int frame = 0; frame < 8; ++frame )
			if( !session.render( noiseBlocks( 320, 180, 10, frame * 10 ), frame, nullptr ) )
				return 1;
		double vx = 0.0, vy = 0.0;
		session.Plugin().TrackVelocityForTest( vx, vy );
		std::printf( "   track before the resize: %.3f px/frame\n", vx );
		session.resize( 400, 200 );
		Image out;
		if( !session.render( noiseBlocks( 400, 200, 12, 0 ), 8, &out ) )
		{
			check( false, "track eye: the first frame after a resize renders" );
			return 1;
		}
		session.Plugin().TrackVelocityForTest( vx, vy );
		std::printf( "   track after the resize: %.3f px/frame\n", vx );
		check( vx == 0.0 && vy == 0.0, "track eye: the estimate starts again from zero after a resize" );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --names
//---------------------------------------------------------------------------
int runNames()
{
	std::printf( "== names: nothing the host would truncate\n" );
	Wheel plugin;
	bool ok = true;
	std::vector< std::string > seen;
	for( unsigned int i = 0; i < Wheel::PT_COUNT; ++i )
	{
		const char* name = plugin.GetParamName( i );
		const std::string s = name ? name : "";
		if( s.size() > 16 )
		{
			std::printf( "   '%s' is %zu characters\n", s.c_str(), s.size() );
			ok = false;
		}
		if( std::find( seen.begin(), seen.end(), s ) != seen.end() )
		{
			std::printf( "   '%s' is declared twice\n", s.c_str() );
			ok = false;
		}
		seen.push_back( s );
	}
	check( ok, "every parameter name is unique and at most 16 characters" );
	check( std::strlen( "SW Wheel" ) <= 16, "the plugin name fits the 16-character field" );
	return 0;
}

//---------------------------------------------------------------------------
// --negative
//
// A check that cannot fail is not a check. Each perturbation below is what
// a real defect would produce, and the relevant assertion has to reject it.
//---------------------------------------------------------------------------
int runNegative()
{
	std::printf( "== negative: the checks can fail\n" );
	const int w = 320, h = 180;

	//1. A DMD that interpolates between frames: --still must fail.
	{
		Session session( w, h );
		session.Plugin().SetInterpolationForTest( true );
		if( !session.setAll( { "Wheel Type=0", "Eye Mode=0", "Wheel Speed=0" } ) || !session.init() )
			return 1;
		size_t differing = 0;
		Image out;
		for( int frame = 0; frame < 4; ++frame )
		{
			const Image card = buildCard( w, h, frame );
			if( !session.render( card, frame, &out ) )
				return 1;
			int worst = 0;
			if( frame > 0 )
				differing += differingBytes( card, out, worst );
		}
		std::printf( "   a DMD given inter-frame interpolation: %zu bytes differ from the input\n", differing );
		check( differing > 0, "--still rejects a DMD that interpolates between frames" );
	}

	//2. The wrong segment times: --separation must fail, at 15% and at 5%.
	for( double scale : { 1.15, 1.05 } )
	{
		SeparationResult r;
		if( !measureSeparation( w, h, 0, 6.0f, scale, r ) )
			return 1;
		const double want = 6.0 / 3.0;
		std::printf( "   segment times %.0f%% out: R-G %.4f against %.4f, bound %.4f\n", ( scale - 1.0 ) * 100.0, r.redGreen, want, r.bound );
		check( std::fabs( r.redGreen - want ) > r.bound, ( "--separation rejects segment times " + std::to_string( static_cast< int >( ( scale - 1.0 ) * 100.0 + 0.5 ) ) + "% out" ).c_str() );
		check( !r.redIsGreenShifted, ( "--separation's whole-pixel translation rejects segment times " + std::to_string( static_cast< int >( ( scale - 1.0 ) * 100.0 + 0.5 ) ) + "% out" ).c_str() );
	}

	//3. Taps that do not sum to one: --energy must fail.
	{
		double still[ 3 ], moving[ 3 ];
		size_t s0 = 0, s1 = 0;
		if( !measureEnergy( w, h, { "Wheel Type=0", "Wheel Speed=1", "Eye Mode=0" }, 1.0f, still, s0 )
		    || !measureEnergy( w, h, { "Wheel Type=0", "Wheel Speed=1", "Eye Mode=1", "Pursuit Angle=0", pursuit( 6.0f ) }, 1.05f, moving, s1 ) )
			return 1;
		const double bound = 0.5 * ( s0 + s1 );
		std::printf( "   end taps 5%% heavy: green sum %.0f against %.0f, bound %.0f\n", moving[ 1 ], still[ 1 ], bound );
		check( std::fabs( moving[ 1 ] - still[ 1 ] ) > bound, "--energy rejects box taps that sum to more than one" );
	}

	//4. The wrong W share: --white must fail.
	{
		WhiteResult r;
		if( !measureWhite( w, h, { "Wheel Type=1", "White Gain=1" }, r ) )
			return 1;
		const double wrong = 64.0 * ( 1.0 + 1.15 );
		std::printf( "   a W share 15%% out predicts %.2f; measured %d\n", wrong, r.grey );
		check( std::fabs( r.grey - wrong ) > 1.0, "--white rejects a W share 15% out" );
	}

	//5. A panel offset one pixel out: --converge's bitwise compare must fail.
	{
		Session session( w, h );
		if( !session.setAll( { "Chip Mode=1", "Eye Mode=0", "Red dx=" + std::to_string( ChipOffsetParam( 2 ) ) } ) || !session.init() )
			return 1;
		const Image card = buildCard( w, h, 4 );
		Image out;
		if( !session.render( card, 0, &out ) )
			return 1;
		size_t wrong = 0;
		for( int y = 8; y < h - 8; ++y )
			for( int x = 8; x < w - 8; ++x )
				if( out[ ( static_cast< size_t >( y ) * w + x ) * 4 ] != card[ ( static_cast< size_t >( y ) * w + x + 3 ) * 4 ] )
					++wrong;
		std::printf( "   judged as a 3 px offset, a 2 px one has %zu pixels wrong\n", wrong );
		check( wrong > 0, "--converge rejects an offset one pixel out" );
	}

	//6. An unprimed detector fires on the first frame; judged against a
	//   30% wrong Saccade Time the decay is rejected.
	{
		SaccadeRun run;
		if( !measureSaccade( w, h, 4.0, false, run ) )
			return 1;
		std::printf( "   unprimed: onsets before the hit %lu\n", run.onsetsBeforeHit );
		check( run.onsetsBeforeHit > 0, "--saccade's priming assertion rejects an unprimed detector" );

		SaccadeRun primed;
		if( !measureSaccade( w, h, 4.0, true, primed ) )
			return 1;
		const double ratio = primed.separation[ 4 ] / primed.separation[ 0 ];
		const double wrongWant = std::exp( -1.0 / 1.3 );
		const double tol       = ( primed.bound[ 4 ] + primed.bound[ 0 ] * std::exp( -1.0 ) ) / primed.separation[ 0 ];
		std::printf( "   a Saccade Time 30%% long predicts a ratio of %.4f; measured %.4f, tolerance %.4f\n", wrongWant, ratio, tol );
		check( std::fabs( ratio - wrongWant ) > tol, "--saccade rejects a Saccade Time 30% out" );
	}

	//7. Four bit planes judged as the input: --bits must fail.
	{
		Image four;
		if( !renderBits( w, h, 4, false, four ) )
			return 1;
		int worst = 0;
		const size_t d = differingBytes( buildCard( w, h, 3 ), four, worst );
		std::printf( "   four planes judged as eight: %zu bytes differ\n", d );
		check( d > 0, "--bits rejects four planes judged as the input" );
	}

	//8. A scroll of two cells judged as one: --track must fail.
	{
		Session session( w, h );
		if( !session.setAll( { "Eye Mode=2" } ) || !session.init() )
			return 1;
		double vx = 0.0, vy = 0.0;
		for( int frame = 0; frame < 14; ++frame )
			if( !session.render( noiseBlocks( w, h, 10, frame * 20 ), frame, nullptr ) )
				return 1;
		session.Plugin().TrackVelocityForTest( vx, vy );
		std::printf( "   two cells a frame reads %.3f px/frame, judged against 10\n", vx );
		check( std::fabs( vx - 10.0 ) > 0.1, "--track rejects a scroll of two cells judged as one" );
	}

	return 0;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( const std::vector< std::string >& settings, int width, int height, int frames )
{
	Session session( width, height );
	if( !session.setAll( settings ) || !session.init() )
		return -1.0;

	const Image card = buildCard( width, height, 0 );
	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( card, frame, nullptr );
	glFinish();

	const auto start = std::chrono::steady_clock::now();
	for( int frame = 0; frame < frames; ++frame )
		session.render( card, warmup + frame, nullptr );
	glFinish();
	const auto end = std::chrono::steady_clock::now();
	return std::chrono::duration< double >( end - start ).count() * 1000.0 / frames;
}

int runBench( const std::vector< std::string >& settings, int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "2560x1440 ", 2560, 1440 },
		{ "3840x2160 ", 3840, 2160 },
	};
	struct Load
	{
		const char* name;
		std::vector< std::string > settings;
	};
	const Load loads[] = {
		{ "defaults: RGB 2x, still eye (whatever --set said, on top)", settings },
		{ "pursuit 8 px, RGBCMY 6x (the heaviest wheel)", { "Wheel Type=2", "Wheel Speed=4", "Eye Mode=1" } },
		{ "track eye, RGB 2x (a readback per frame)", { "Eye Mode=2" } },
	};

	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides.\n", frames );
	for( const Load& load : loads )
	{
		std::printf( "\n%s\nresolution     ms/frame   equivalent fps   %% of a 60fps frame\n", load.name );
		for( const Size& size : sizes )
		{
			const double ms = benchAt( load.settings, size.width, size.height, frames );
			if( ms < 0.0 )
				return 1;
			std::printf( "%s    %7.3f       %8.0f            %5.1f%%\n", size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0, ms / 16.667 * 100.0 );
		}
	}
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one `frame Parameter Name value` per line, applied when
// the frame number is reached and linearly interpolated between keys -- the
// fleet's format (plumbicon's pbtest, cadence's cdtest), so one filming
// script drives any of the plugins. The value is what the host sends:
// 0..1 for a slider, the element value for an option (Eye Mode 1 is
// Pursuit), 1..8 for Bit Depth, 0 or 1 for Bit Planes and for Fire.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;//blank or comment

		//The name is everything up to the last token, because parameters
		//have spaces in them ("Pursuit Speed") and the value never does.
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const auto& a, const auto& b ) { return a.first < b.first; } );
	return tracks;
}

/// A continuous control: interpolated between keys, held before the first
/// and after the last. Exactly pbtest's `valueAt`.
float rampAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( track[ i ].first < frame )
			continue;
		const float span = static_cast< float >( track[ i ].first - track[ i - 1 ].first );
		const float t    = span > 0.0f ? ( frame - track[ i - 1 ].first ) / span : 0.0f;
		return track[ i - 1 ].second + ( track[ i ].second - track[ i - 1 ].second ) * t;
	}
	return track.back().second;
}

/// A discrete control -- an option, a switch, an event: the last key at or
/// before this frame, with no values in between, and NaN (leave it alone)
/// before the first. An option ramped from 0 to 3 would otherwise visit
/// every mode on the way; one held at its first key from frame 0 would make
/// `65 Wheel Type 2` an RGBCMY wheel for the whole take; and an event held
/// at its first key would make `30 Fire 1` a press on frame 0.
float stepAt( const Track& track, int frame )
{
	float value = std::nanf( "" );
	for( const auto& key : track )
	{
		if( key.first > frame )
			break;
		value = key.second;
	}
	return value;
}

//---------------------------------------------------------------------------
// --pipe
//
// Raw RGBA in, raw RGBA out, one frame at a time, top row first, through the
// real plugin class -- the same Session every check above uses, so what a
// take shows is what the checks measured.
//
// The clock is SYNTHETIC and driven by the frame index: milliseconds, as
// Resolume sends them, at --fps. Not the wall clock and not the rate the
// pipe delivers, so a stall upstream in ffmpeg cannot shorten a saccade.
//
// A cue reaches the plugin only when its value changes, which is what a
// host does. That matters for exactly one control: Fire saccades on every
// value >= 0.5 it is SENT, so re-sending a held 1 every frame would fire a
// saccade every frame.
//
// The Audio buffer is left at silence (or fed --tone's click train), so in
// Saccade mode the only saccades are the ones Fire cues ask for.
//---------------------------------------------------------------------------
int runPipe( int width, int height, double fps, const std::string& scriptPath,
             const std::vector< std::string >& settings, bool tone, int fireFrame )
{
	Session session( width, height );
	session.tone      = tone;
	session.fireFrame = fireFrame;
	session.pipeFps   = fps;

	for( const std::string& setting : settings )
		if( !session.set( setting ) )
			return 2;

	struct Cue
	{
		unsigned int index;
		bool discrete;
		Track keys;
		float sent;
		bool everSent;
	};
	std::vector< Cue > cues;

	//Names are resolved once, up front, and an unknown one refuses the whole
	//run. A misspelled name that silently did nothing would produce a take
	//that looks deliberate and is wrong: the default on screen, and a
	//caption over it describing a control that never moved.
	if( !scriptPath.empty() )
	{
		std::string error;
		const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}

		Wheel& plugin = session.Plugin();
		for( const auto& entry : tracks )
		{
			const int index = findParameter( plugin, entry.first );
			if( index < 0 )
			{
				std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n",
				              entry.first.c_str() );
				return 2;
			}
			const unsigned int type = plugin.GetParamType( static_cast< unsigned int >( index ) );
			if( index >= static_cast< int >( Wheel::PT_ABOUT_FIRST ) || type == FF_TYPE_BUFFER || type == FF_TYPE_TEXT )
			{
				std::fprintf( stderr, "script names '%s', which a cue cannot drive (%s)\n", entry.first.c_str(),
				              type == FF_TYPE_BUFFER ? "the host's audio buffer -- use --tone" : "the About block" );
				return 2;
			}

			Cue cue {};
			cue.index    = static_cast< unsigned int >( index );
			cue.discrete = type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT;
			cue.keys     = entry.second;
			cues.push_back( cue );
		}
	}

	if( !session.init() )
		return 1;

	//A consumer that goes away (ffmpeg's own -frames, a head) must end the
	//run with a short write, not kill it with a signal.
	std::signal( SIGPIPE, SIG_IGN );

	Image frame( static_cast< size_t >( width ) * height * 4 );
	Image out;

	for( int index = 0;; ++index )
	{
		size_t filled = 0;
		while( filled < frame.size() )
		{
			const ssize_t got = read( STDIN_FILENO, frame.data() + filled, frame.size() - filled );
			if( got <= 0 )
				break;
			filled += static_cast< size_t >( got );
		}

		//End of stream. A PARTIAL frame is dropped rather than padded: half
		//a frame of black at the end of a take is a flash. It is also the
		//commonest mistake here, a --size that does not match the stream,
		//which otherwise shears the picture instead of saying so.
		if( filled < frame.size() )
		{
			if( filled > 0 )
				std::fprintf( stderr,
				              "dropped %zu bytes of a partial frame at frame %d -- does --size %dx%d match the stream?\n",
				              filled, index, width, height );
			break;
		}

		for( Cue& cue : cues )
		{
			const float value = cue.discrete ? stepAt( cue.keys, index ) : rampAt( cue.keys, index );
			if( std::isnan( value ) || ( cue.everSent && value == cue.sent ) )
				continue;
			session.Plugin().SetFloatParameter( cue.index, value );
			cue.sent     = value;
			cue.everSent = true;
		}

		if( !session.render( frame, index, &out ) )
		{
			std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", index );
			return 1;
		}

		size_t written = 0;
		while( written < out.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
			if( put <= 0 )
				break;
			written += static_cast< size_t >( put );
		}

		//The consumer went away. Not an error.
		if( written < out.size() )
			break;
	}

	return 0;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"whtest -- render and check the Wheel DLP effect\n"
		"\n"
		"  --out PATH        render the moving test card through the plugin (default /tmp/wheel.png)\n"
		"  --size WxH        picture size (default 1280x720)\n"
		"  --frames N        frames to render before reading back (default 24)\n"
		"  --tone            push a synthetic click train into the Audio buffer every frame\n"
		"  --fire N          press Fire on frame N\n"
		"  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
		"  --list            print every parameter, its kind and its default, then exit\n"
		"  --still --separation --energy --white --converge --saccade --bits --track --resize --names\n"
		"                    one claim each; see the header\n"
		"  --negative        every check can fail\n"
		"  --all             every check, then the negatives\n"
		"  --bench           time ProcessOpenGL at 720p through 4K\n"
		"  --pipe            raw RGBA frames on stdin, raw RGBA frames on stdout, at --size\n"
		"  --script PATH     parameter cues for --pipe: 'frame Parameter Name value' per line\n"
		"  --fps N           --pipe's synthetic clock, frames per second (default 60)\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/wheel.png";
	std::string scriptPath;
	int width = 1280, height = 720, frames = 24, fireFrame = -1;
	double fps = 60.0;
	bool tone = false, wantList = false, wantBench = false, wantAll = false, wantNegative = false, wantPipe = false;
	std::vector< std::string > checks;
	std::vector< std::string > settings;

	const std::vector< std::string > known = { "still", "separation", "energy", "white", "converge", "saccade", "bits", "track", "resize", "names" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fire" && hasNext )
			fireFrame = std::atoi( argv[ ++i ] );
		else if( argument == "--tone" )
			tone = true;
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--all" )
			wantAll = true;
		else if( argument == "--negative" )
			wantNegative = true;
		else if( argument.rfind( "--", 0 ) == 0 && std::find( known.begin(), known.end(), argument.substr( 2 ) ) != known.end() )
			checks.push_back( argument.substr( 2 ) );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 )
	{
		std::fprintf( stderr, "width, height and frames must all be positive\n" );
		return 2;
	}
	if( !( fps > 0.0 ) )
	{
		std::fprintf( stderr, "--fps must be positive\n" );
		return 2;
	}
	//The plugin clamps a frame to 1/240..1/24 s (Clock.h), as it would in
	//Resolume; outside that the saccade's decay runs at the clamped rate.
	if( wantPipe && ( fps < 24.0 || fps > 240.0 ) )
		std::fprintf( stderr, "warning: --fps %g is outside 24..240; the plugin clamps its frame time there\n", fps );
	if( !scriptPath.empty() && !wantPipe )
	{
		std::fprintf( stderr, "--script is for --pipe\n" );
		return 2;
	}

	//No GL needed, so answered before a context is made -- which means it
	//still works on a machine where creating one fails, and in CI.
	if( wantList )
	{
		Wheel plugin;
		for( const std::string& setting : settings )
		{
			std::string error;
			if( !applySetting( plugin, setting, error ) )
			{
				std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
				return 2;
			}
		}
		listParameters( plugin );
		return 0;
	}

	if( wantAll )
	{
		checks = known;
		wantNegative = true;
	}

	//--names needs no context either.
	if( checks.size() == 1 && checks[ 0 ] == "names" )
	{
		runNames();
		std::printf( "\n%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	auto finish = [ & ]( int code ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return code;
	};

	//Before anything else that could print: stdout is the video in --pipe,
	//and one stray line of text in it is a torn frame for the rest of the take.
	if( wantPipe )
		return finish( runPipe( width, height, fps, scriptPath, settings, tone, fireFrame ) );

	if( !checks.empty() || wantNegative )
	{
		int result = 0;
		for( const std::string& c : checks )
		{
			if( c == "still" ) result |= runStill();
			else if( c == "separation" ) result |= runSeparation();
			else if( c == "energy" ) result |= runEnergy();
			else if( c == "white" ) result |= runWhite();
			else if( c == "converge" ) result |= runConverge();
			else if( c == "saccade" ) result |= runSaccade();
			else if( c == "bits" ) result |= runBits();
			else if( c == "track" ) result |= runTrack();
			else if( c == "resize" ) result |= runResize();
			else if( c == "names" ) result |= runNames();
		}
		if( wantNegative )
			result |= runNegative();

		std::printf( "\n%d checks, %d failed\n", g_checks, g_failures );
		std::printf( "%s\n", ( g_failures == 0 && result == 0 ) ? "all checks passed" : "FAILURES above" );
		return finish( ( g_failures == 0 && result == 0 ) ? 0 : 1 );
	}

	if( wantBench )
		return finish( runBench( settings, frames ) );

	//A still, at the end of a run of frames on the moving card.
	Session session( width, height );
	session.tone      = tone;
	session.fireFrame = fireFrame;
	for( const std::string& setting : settings )
		if( !session.set( setting ) )
			return finish( 2 );
	if( !session.init() )
		return finish( 1 );

	Image out;
	for( int frame = 0; frame < frames; ++frame )
		if( !session.render( buildCard( width, height, frame ), frame, frame == frames - 1 ? &out : nullptr ) )
		{
			std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", frame );
			return finish( 1 );
		}

	if( !writePng( outPath, width, height, out ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
