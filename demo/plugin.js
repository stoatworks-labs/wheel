/**
 * Wheel — browser demo.
 *
 * A single-chip DLP projector, rainbow effect and all. The projector shows a
 * red picture, then a green one, then a blue one through a spinning filter
 * wheel, and the eye adds them up — correctly only if it is still. Pursue
 * something, or let a saccade fly, and each colour lands on the retina
 * somewhere else: the rainbow fringe. The model is a retina, not a filter.
 *
 * All four shader constants below — `kVertexShader`, `kCopyShader`,
 * `kGridShader` and `kIntegrateShader` — are `source/Shaders.cpp`, copied
 * across unedited. `demo/tools/check_shaders.py` compares them character for
 * character against the C++ and is called from `tools/verify.sh`.
 *
 * The CPU half is a **port**, and **nothing checks it but a reader**:
 * `source/Controls.cpp`, `source/Segments.cpp` (the wheel, the per-channel
 * normalisation, the sub-field schedule, the eye's displacement and each
 * sub-field's placement), the Track eye's block matcher (`estimateMotion`),
 * the saccade's direction hash and decay out of `Wheel::ProcessOpenGL`, and the
 * clamping half of `source/Clock.cpp`.
 *
 * ------------------------------------------------------- what is missing
 *
 * **The audio side.** The `Audio` buffer and `source/Onset.cpp` are absent:
 * the spectrum reaches the plugin through a Resolume FFT parameter and a
 * browser has no equivalent. Under Eye Mode `Saccade` the eye therefore only
 * moves when you press Fire — which is the same code path an onset takes, since
 * an onset in the plugin does nothing but set `firePending`.
 *
 * **Fire is a toggle, not an event.** FFGL has FF_TYPE_EVENT and the kit's
 * parameter model does not, so Fire is a boolean here that the renderer
 * releases itself once it has taken the press (readout's precedent).
 *
 * **Bit Depth is a slider.** It is FF_TYPE_INTEGER in the plugin, 1 to 8.
 *
 * **The host clock's unit.** `Clock.cpp` votes on seconds versus
 * milliseconds; a browser's clock is unambiguous, so only the clamping (a frame
 * is held to 1/240 … 1/24 s) survives. The frame length matters here: Saccade
 * Time is in seconds and the eye works in frames.
 *
 * **The About block** — four buttons that open a browser.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//
// Pass the RAW `#version 410 core` text to Program: its constructor calls the
// kit's port() on both sources itself.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

const COPY = `#version 410 core

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
`;

const GRID = `#version 410 core

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
`;

const INTEGRATE = `#version 410 core

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
//(Jensen), and \`--energy\` on a six-segment wheel is what found that.
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
`;

//===========================================================================
// Controls.cpp — ported.
//===========================================================================

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
const lround = (v) => (v < 0 ? -Math.floor(-v + 0.5) : Math.floor(v + 0.5));
const geometric = (from, to, t) => from * Math.pow(to / from, clamp01(t));
const geometricInverse = (from, to, value) => Math.log(value / from) / Math.log(to / from);

const WHEEL_TYPES = ['RGB', 'RGBW', 'RGBCMY', 'Custom'];
const WHEEL_SPEEDS = ['1x', '2x', '3x', '4x', '6x'];
const EYE_MODES = ['Still', 'Pursuit', 'Track', 'Saccade'];
const CHIP_MODES = ['Single Chip', 'Three Chip'];

const WHEEL_RGB = 0;
const WHEEL_RGBW = 1;
const WHEEL_RGBCMY = 2;
const WHEEL_CUSTOM = 3;
const EYE_PURSUIT = 1;
const EYE_TRACK = 2;
const CHIP_THREE = 1;

const MAX_PURSUIT_SPEED = 32;
const MAX_SACCADE_SIZE = 200;
const MAX_CHIP_OFFSET = 8;
const MIN_BIT_DEPTH = 1;
const MAX_BIT_DEPTH = 8;

const option = (value, count) => clamp(lround(value), 0, count - 1);

const controls = {
  rotationsFromOption: (v) => [1, 2, 3, 4, 6][option(v, WHEEL_SPEEDS.length)],
  whiteGain: (v) => clamp01(v),
  pursuitSpeed: (v) => clamp01(v) * MAX_PURSUIT_SPEED,
  pursuitSpeedParam: (px) => px / MAX_PURSUIT_SPEED,
  pursuitAngle: (v) => clamp01(v) * 2 * Math.PI,
  saccadeSize: (v) => clamp01(v) * MAX_SACCADE_SIZE,
  saccadeSizeParam: (px) => px / MAX_SACCADE_SIZE,
  saccadeTime: (v) => geometric(0.02, 1.0, v),
  saccadeTimeParam: (s) => geometricInverse(0.02, 1.0, s),
  chipOffset: (v) => (clamp01(v) - 0.5) * 2 * MAX_CHIP_OFFSET,
  chipOffsetParam: (px) => 0.5 + px / (2 * MAX_CHIP_OFFSET),
  panelRotate: (v) => (clamp01(v) - 0.5) * 2 * (Math.PI / 180),
  gamma: (v) => 1 + clamp01(v) * 2,
  gammaParam: (g) => (g - 1) / 2,
};

// Bit Depth is FF_TYPE_INTEGER, 1..8, in the plugin. A 0..1 slider here that
// lands on each integer; BitDepthFromParam's clamp is folded in.
const bitDepthFromSlider = (v) => MIN_BIT_DEPTH + Math.round(clamp01(v) * (MAX_BIT_DEPTH - MIN_BIT_DEPTH));
const bitDepthSlider = (bits) => (bits - MIN_BIT_DEPTH) / (MAX_BIT_DEPTH - MIN_BIT_DEPTH);

//===========================================================================
// Segments.cpp — ported. The wheel as arithmetic.
//===========================================================================

const MIN_PRIMARY_WIDTH = 0.02;
const MAX_SUB_FIELDS = 36;

const primary = (channel, width) => {
  const filter = [0, 0, 0];
  filter[channel] = 1;
  return { filter, width, secondary: false };
};
const secondary = (r, g, b, width) => ({ filter: [r ? 1 : 0, g ? 1 : 0, b ? 1 : 0], width, secondary: true });

function wheelSegments(type, customWidths) {
  switch (type) {
    case WHEEL_RGBW:
      return [primary(0, 0.25), primary(1, 0.25), primary(2, 0.25), secondary(true, true, true, 0.25)];
    case WHEEL_RGBCMY:
      // The BrilliantColor order: R Y G C B M.
      return [
        primary(0, 1 / 6),
        secondary(true, true, false, 1 / 6),
        primary(1, 1 / 6),
        secondary(false, true, true, 1 / 6),
        primary(2, 1 / 6),
        secondary(true, false, true, 1 / 6),
      ];
    case WHEEL_CUSTOM: {
      const w = [0, 1, 2, 3].map((i) => Math.max(0, customWidths ? customWidths[i] : 0.25));
      // A primary can be narrow but never absent.
      for (let i = 0; i < 3; i += 1) w[i] = Math.max(w[i], MIN_PRIMARY_WIDTH);
      const sum = w[0] + w[1] + w[2] + w[3];
      for (let i = 0; i < 4; i += 1) w[i] /= sum;
      const out = [primary(0, w[0]), primary(1, w[1]), primary(2, w[2])];
      if (w[3] > 0) out.push(secondary(true, true, true, w[3]));
      return out;
    }
    case WHEEL_RGB:
    default:
      return [primary(0, 1 / 3), primary(1, 1 / 3), primary(2, 1 / 3)];
  }
}

function buildSchedule(segments, rotations, whiteGain) {
  const schedule = [];
  rotations = clamp(rotations, 1, 6);

  // The per-channel primary normalisation.
  const primarySum = [0, 0, 0];
  for (const s of segments) {
    if (!s.secondary) for (let c = 0; c < 3; c += 1) primarySum[c] += s.width * s.filter[c];
  }
  for (let c = 0; c < 3; c += 1) if (primarySum[c] <= 0) primarySum[c] = 1;

  let primaryWidth = 0;
  for (const s of segments) if (!s.secondary) primaryWidth += s.width;

  let turnStart = 0;
  for (let r = 0; r < rotations; r += 1) {
    let at = turnStart;
    for (const s of segments) {
      const share = s.width / rotations;
      const f = {
        filter: s.filter.slice(),
        secondary: s.secondary,
        weight: [0, 0, 0],
        t0: at,
        t1: at + share,
        alphaWeight: s.secondary ? 0 : (s.width / primaryWidth) / rotations,
      };
      at = f.t1;
      for (let c = 0; c < 3; c += 1) {
        if (s.filter[c] <= 0) continue;
        f.weight[c] = (s.width / primarySum[c]) / rotations;
        if (s.secondary) f.weight[c] *= whiteGain;
      }
      schedule.push(f);
    }
    turnStart += 1 / rotations;
  }
  // The timeScale hook is the harness's negative control and is always 1.0 in
  // the plugin, so it is not carried here.
  return schedule.slice(0, MAX_SUB_FIELDS);
}

function threeChipSchedule() {
  return [{ filter: [1, 1, 1], weight: [1, 1, 1], alphaWeight: 1, secondary: false, t0: 0, t1: 1 }];
}

/** Eye::DisplacementAt — retinal displacement at t (frames), picture space, y down. */
function displacementAt(eye, t) {
  const out = [eye.pursuit[0] * t, eye.pursuit[1] * t];
  if (eye.saccadeRemaining > 0 && eye.saccadeTau > 0) {
    const travelled = eye.saccadeRemaining * (1 - Math.exp(-t / eye.saccadeTau));
    out[0] += eye.saccadeDir[0] * travelled;
    out[1] += eye.saccadeDir[1] * travelled;
  }
  return out;
}

/** PlaceSubField — the sub-field's centre relative to the frame's midpoint, and its box. */
function placeSubField(eye, field) {
  const mid = displacementAt(eye, 0.5);
  const start = displacementAt(eye, field.t0);
  const end = displacementAt(eye, field.t1);
  return {
    offset: [0.5 * (start[0] + end[0]) - mid[0], 0.5 * (start[1] + end[1]) - mid[1]],
    box: [end[0] - start[0], end[1] - start[1]],
  };
}

//===========================================================================
// Wheel::ProcessOpenGL — the rest of the CPU half, ported.
//===========================================================================

const TAU = 6.283185307179586;
const GRID_WIDTH = 32; // Shaders.h kGridWidth
const GRID_HEIGHT = 18;
const SEARCH = 3; // the Track eye's search, in grid cells either side
const MIN_FRAME_SECONDS = 1 / 240; // Clock.h
const MAX_FRAME_SECONDS = 1 / 24;

/** The PCG output mix, in uint32 arithmetic. */
function hashInt(seed) {
  const state = (Math.imul(seed >>> 0, 747796405) + 2891336453) >>> 0;
  const word = Math.imul(((state >>> ((state >>> 28) + 4)) ^ state) >>> 0, 277803737) >>> 0;
  return ((word >>> 22) ^ word) >>> 0;
}

function createRenderer(gl, quad) {
  const copyShader = new Program(gl, VERTEX, COPY, 'copy');
  const gridShader = new Program(gl, VERTEX, GRID, 'grid');
  const integrateShader = new Program(gl, VERTEX, INTEGRATE, 'integrate');

  const linearCopy = new PassBuffer(gl, { mip: true });
  const grid = new PassBuffer(gl, { filter: 'nearest' });
  const gridRaw = new Float32Array(GRID_WIDTH * GRID_HEIGHT * 4);

  // Clock.cpp's clamping half.
  let lastNow = -1;

  // Saccade state.
  let firePending = false;
  let saccadeRemaining = 0;
  const saccadeDir = [1, 0];
  let saccadeCount = 0;

  // Track state.
  let gridNow = new Float32Array(GRID_WIDTH * GRID_HEIGHT);
  let gridPrevious = new Float32Array(GRID_WIDTH * GRID_HEIGHT);
  let gridPreviousValid = false;
  let gridPictureWidth = 0;
  let gridPictureHeight = 0;
  const trackVelocity = [0, 0];

  const subW = new Float32Array(MAX_SUB_FIELDS * 4);
  const subO = new Float32Array(MAX_SUB_FIELDS * 4);
  const subT = new Float32Array(MAX_SUB_FIELDS * 4);

  /** Wheel::estimateMotion, ported. */
  function estimateMotion(width, height) {
    if (gridPictureWidth !== width || gridPictureHeight !== height) {
      gridPreviousValid = false;
      gridPictureWidth = width;
      gridPictureHeight = height;
    }
    if (!gridPreviousValid) {
      gridPrevious.set(gridNow);
      gridPreviousValid = true;
      trackVelocity[0] = trackVelocity[1] = 0;
      return;
    }

    const sad = (sx, sy) => {
      let sum = 0;
      let n = 0;
      for (let y = 0; y < GRID_HEIGHT; y += 1) {
        const py = y - sy;
        if (py < 0 || py >= GRID_HEIGHT) continue;
        for (let x = 0; x < GRID_WIDTH; x += 1) {
          const px = x - sx;
          if (px < 0 || px >= GRID_WIDTH) continue;
          sum += Math.abs(gridNow[y * GRID_WIDTH + x] - gridPrevious[py * GRID_WIDTH + px]);
          n += 1;
        }
      }
      return n > 0 ? sum / n : 1e9;
    };

    let best = 1e18;
    let bx = 0;
    let by = 0;
    const table = [];
    for (let sy = -SEARCH; sy <= SEARCH; sy += 1) {
      const row = [];
      for (let sx = -SEARCH; sx <= SEARCH; sx += 1) {
        const s = sad(sx, sy);
        row.push(s);
        // Prefer no motion on a tie.
        if (s < best - 1e-12 || (Math.abs(s - best) <= 1e-12 && sx === 0 && sy === 0)) {
          best = s;
          bx = sx;
          by = sy;
        }
      }
      table.push(row);
    }

    const refine = (left, centre, right) => {
      const denom = left - 2 * centre + right;
      if (denom <= 1e-12) return 0;
      return clamp(0.5 * (left - right) / denom, -0.5, 0.5);
    };

    let fx = 0;
    let fy = 0;
    if (bx > -SEARCH && bx < SEARCH) fx = refine(table[by + SEARCH][bx - 1 + SEARCH], best, table[by + SEARCH][bx + 1 + SEARCH]);
    if (by > -SEARCH && by < SEARCH) fy = refine(table[by - 1 + SEARCH][bx + SEARCH], best, table[by + 1 + SEARCH][bx + SEARCH]);

    const cellW = width / GRID_WIDTH;
    const cellH = height / GRID_HEIGHT;
    let vx = (bx + fx) * cellW;
    let vy = (by + fy) * cellH;
    vy = -vy; // the grid is y up; the eye works y down
    if (best <= 1e-6) vx = vy = 0;

    trackVelocity[0] += (vx - trackVelocity[0]) * 0.5;
    trackVelocity[1] += (vy - trackVelocity[1]) * 0.5;

    const swap = gridPrevious;
    gridPrevious = gridNow;
    gridNow = swap;
  }

  return {
    render({ input, params, width, height, time }) {
      const p = (id) => params.get(id);

      //---------------------------------------------------------------
      // Time: Clock.cpp's clamp. A frame is 1/240 … 1/24 s whatever the
      // browser's gap was.
      //---------------------------------------------------------------
      let dt = 1 / 60;
      if (lastNow >= 0) dt = clamp(time - lastNow, MIN_FRAME_SECONDS, MAX_FRAME_SECONDS);
      lastNow = time;

      //---------------------------------------------------------------
      // What the controls say.
      //---------------------------------------------------------------
      const wheelType = option(p('wheelType'), WHEEL_TYPES.length);
      const rotations = controls.rotationsFromOption(p('wheelSpeed'));
      const widths = [p('widthR'), p('widthG'), p('widthB'), p('widthW')];
      const whiteGain = controls.whiteGain(p('whiteGain'));

      const eyeMode = option(p('eye'), EYE_MODES.length);
      const pursuitSpeed = controls.pursuitSpeed(p('pursuitSpeed'));
      const pursuitAngle = controls.pursuitAngle(p('pursuitAngle'));
      const saccadeSize = controls.saccadeSize(p('saccadeSize'));
      const saccadeTime = controls.saccadeTime(p('saccadeTime'));

      const bitsOn = p('bitsOn') >= 0.5;
      const bitDepth = bitsOn ? bitDepthFromSlider(p('bitDepth')) : 0;

      const chipMode = option(p('chip'), CHIP_MODES.length);
      const redDx = controls.chipOffset(p('redDx'));
      const redDy = controls.chipOffset(p('redDy'));
      const blueDx = controls.chipOffset(p('blueDx'));
      const blueDy = controls.chipOffset(p('blueDy'));
      const rotate = controls.panelRotate(p('panelRotate'));

      const gamma = controls.gamma(p('gamma'));
      const mixAmount = p('mix');

      //---------------------------------------------------------------
      // Fire. FF_TYPE_EVENT in the plugin; a toggle here, released by the
      // renderer once the press is taken, as a host releases an event.
      // No audio reaches this page, so an onset never arms it.
      //---------------------------------------------------------------
      if (p('fire') > 0.5) {
        firePending = true;
        params.set('fire', 0);
      }
      if (firePending) {
        firePending = false;
        saccadeRemaining = saccadeSize;
        const seed = (Math.imul(saccadeCount >>> 0, 2654435761) ^ 0x9E3779B9) >>> 0;
        const angle = (hashInt(seed) / 4294967296) * TAU;
        saccadeDir[0] = Math.cos(angle);
        saccadeDir[1] = Math.sin(angle);
        saccadeCount += 1;
      }

      //---------------------------------------------------------------
      // Buffers.
      //---------------------------------------------------------------
      const track = eyeMode === EYE_TRACK;
      linearCopy.ensure(width, height, gl.RGBA16F);
      if (track) grid.ensure(GRID_WIDTH, GRID_HEIGHT, gl.RGBA32F);

      gl.disable(gl.BLEND);

      //---------------------------------------------------------------
      // 1. Copy, linearised.
      //---------------------------------------------------------------
      linearCopy.bind();
      copyShader.use();
      bindTexture(gl, 0, input.texture);
      copyShader.setSampler('InputTexture', 0);
      copyShader.set('Gamma', gamma);
      quad.draw();

      //---------------------------------------------------------------
      // 2. The Track eye: a coarse grid off the mip chain, read back.
      //---------------------------------------------------------------
      if (track) {
        linearCopy.generateMipmap();
        const cellW = width / GRID_WIDTH;
        const cellH = height / GRID_HEIGHT;
        const lod = Math.log2(Math.max(1, Math.min(cellW, cellH)));

        grid.bind();
        gridShader.use();
        bindTexture(gl, 0, linearCopy.texture);
        gridShader.setSampler('LinearTexture', 0);
        gridShader.set('Lod', lod);
        gridShader.setInt('GridWidth', GRID_WIDTH);
        gridShader.setInt('GridHeight', GRID_HEIGHT);
        quad.draw();

        // A synchronous read of 576 floats, as the plugin does.
        gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
        gl.readPixels(0, 0, GRID_WIDTH, GRID_HEIGHT, gl.RGBA, gl.FLOAT, gridRaw);
        for (let i = 0; i < gridNow.length; i += 1) gridNow[i] = gridRaw[i * 4];
        estimateMotion(width, height);
      } else {
        gridPreviousValid = false;
        trackVelocity[0] = trackVelocity[1] = 0;
      }

      //---------------------------------------------------------------
      // 3. The schedule and the eye.
      //---------------------------------------------------------------
      const schedule = chipMode === CHIP_THREE
        ? threeChipSchedule()
        : buildSchedule(wheelSegments(wheelType, widths), rotations, whiteGain);

      const eye = { pursuit: [0, 0], saccadeRemaining: 0, saccadeTau: 1, saccadeDir: [1, 0] };
      if (eyeMode === EYE_PURSUIT) {
        eye.pursuit[0] = pursuitSpeed * Math.cos(pursuitAngle);
        eye.pursuit[1] = pursuitSpeed * Math.sin(pursuitAngle);
      } else if (eyeMode === EYE_TRACK) {
        eye.pursuit[0] = trackVelocity[0];
        eye.pursuit[1] = trackVelocity[1];
      }
      // In frames: the shader's time axis is one frame.
      const tauFrames = Math.max(1e-3, saccadeTime / Math.max(1e-4, dt));
      eye.saccadeRemaining = saccadeRemaining;
      eye.saccadeTau = tauFrames;
      eye.saccadeDir = [saccadeDir[0], saccadeDir[1]];

      subW.fill(0);
      subO.fill(0);
      subT.fill(0);
      const count = Math.min(schedule.length, MAX_SUB_FIELDS);
      for (let k = 0; k < count; k += 1) {
        const f = schedule[k];
        const pl = placeSubField(eye, f);
        for (let c = 0; c < 3; c += 1) subW[k * 4 + c] = f.filter[c] * f.weight[c];
        subW[k * 4 + 3] = f.alphaWeight;
        // Picture space is y down, GL is y up.
        subO[k * 4 + 0] = pl.offset[0];
        subO[k * 4 + 1] = -pl.offset[1];
        subO[k * 4 + 2] = pl.box[0];
        subO[k * 4 + 3] = -pl.box[1];
        subT[k * 4 + 0] = f.secondary ? 1 : 0;
        subT[k * 4 + 1] = 0.5 * (f.t0 + f.t1);
      }

      // The saccade's travel this frame is spent.
      if (saccadeRemaining > 0) {
        saccadeRemaining *= Math.exp(-1 / tauFrames);
        if (saccadeRemaining < 1e-3) saccadeRemaining = 0;
      }

      //---------------------------------------------------------------
      // 4. Integrate, straight to the canvas.
      //---------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      integrateShader.use();
      bindTexture(gl, 0, input.texture);
      bindTexture(gl, 1, linearCopy.texture);
      bindTexture(gl, 2, linearCopy.texture);
      integrateShader.setSampler('SourceTexture', 0);
      integrateShader.setSampler('LinearTexture', 1);
      integrateShader.setSampler('PrevTexture', 2);
      // The two test hooks, at the values the plugin always runs with.
      integrateShader.setInt('Interp', 0);
      integrateShader.set('TapBias', 1.0);
      integrateShader.setInt('Width', width);
      integrateShader.setInt('Height', height);
      integrateShader.setInt('Count', count);
      integrateShader.setArray('SubW', subW.subarray(0, count * 4), 4);
      integrateShader.setArray('SubO', subO.subarray(0, count * 4), 4);
      integrateShader.setArray('SubT', subT.subarray(0, count * 4), 4);
      integrateShader.set('Gamma', gamma);
      integrateShader.setInt('BitDepth', bitDepth);
      integrateShader.setInt('ChipMode', chipMode);
      integrateShader.set('RedOffset', redDx, -redDy);
      integrateShader.set('BlueOffset', blueDx, -blueDy);
      integrateShader.set('RedRotate', rotate);
      integrateShader.set('BlueRotate', -rotate);
      integrateShader.set('MixAmount', mixAmount);
      quad.draw();
    },
  };
}

//===========================================================================
// The page.
//===========================================================================

const std = (id, name, def, group, display, hint) => ({ id, name, type: 'standard', default: def, group, display, hint });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });
const px = (v) => `${v >= 0 ? '+' : ''}${v.toFixed(2)} px`;
const pct = (v) => `${Math.round(clamp01(v) * 100)}%`;

mountDemo({
  name: 'Wheel',
  pluginId: 'WH01',
  tagline:
    'A single-chip DLP projector, rainbow effect and all. It shows a red picture, then a green one, then a blue one through a spinning filter wheel, and your eye adds them up — correctly only if it is still. Pursue something, or let a saccade fly, and each colour lands on the retina somewhere else: the rainbow fringe. A still eye shows no fringe at all, at any wheel speed, because the DMD holds the frame. Start with Eye Mode on Pursuit.',
  repo: 'https://github.com/stoatworks-labs/wheel',
  page: 'https://stoatworks-labs.com/software/wheel/',
  video: 'https://www.youtube.com/watch?v=_HyLrvH5D88',

  // The linear copy is RGBA16F and the Track grid RGBA32F, read back.
  needFloat: true,

  // Alpha rides on the primaries and sums to one: the output's alpha is the
  // input's.
  showBackdrop: true,

  params: [
    opt('wheelType', 'Wheel Type', WHEEL_TYPES, 0, 'Wheel',
      'RGB: three equal segments. RGBW: four, with a white one. RGBCMY: six, in the BrilliantColor order R Y G C B M. Custom: R G B W with the four width sliders.'),
    opt('wheelSpeed', 'Wheel Speed', WHEEL_SPEEDS, 1, 'Wheel',
      'Rotations per frame. Each segment appears that many times a frame, for that fraction of its width. Faster wheels fringe less far.'),
    std('widthR', 'Red Width', 0.25, 'Wheel', null, 'Custom only. Normalised with the other three to a whole turn; a primary is never narrower than 2%.'),
    std('widthG', 'Green Width', 0.25, 'Wheel', null, 'Custom only.'),
    std('widthB', 'Blue Width', 0.25, 'Wheel', null, 'Custom only.'),
    std('widthW', 'White Width', 0.25, 'Wheel', null, 'Custom only. At zero the white segment is dropped.'),
    std('whiteGain', 'White Gain', 0.5, 'Wheel', pct,
      'How bright the secondary segments are against the primaries. A white segment shows the part of the pixel all three agree on, so it brightens a white and leaves a saturated primary alone — and can push a white past 1.0, as a real RGBW wheel does.'),

    opt('eye', 'Eye Mode', EYE_MODES, 0, 'Eye',
      'Still: the projector working as intended, and no fringe at all. Pursuit: a set velocity and angle. Track: the clip\'s own dominant motion, by coarse block matching. Saccade: still, with a burst on Fire (and, in the plugin, on every audio onset).'),
    std('pursuitSpeed', 'Pursuit Speed', controls.pursuitSpeedParam(8), 'Eye',
      (v) => `${controls.pursuitSpeed(v).toFixed(1)} px/frame`, '0 to 32 px per frame.'),
    std('pursuitAngle', 'Pursuit Angle', 0, 'Eye',
      (v) => `${Math.round(clamp01(v) * 360)}°`, '0 is rightwards, 90 is down the picture.'),
    std('saccadeSize', 'Saccade Size', controls.saccadeSizeParam(40), 'Eye',
      (v) => `${Math.round(controls.saccadeSize(v))} px`, '0 to 200 px of travel per saccade.'),
    std('saccadeTime', 'Saccade Time', controls.saccadeTimeParam(0.1), 'Eye',
      (v) => `${Math.round(controls.saccadeTime(v) * 1000)} ms`,
      'The time constant the burst\'s velocity decays with, 20 ms to 1 s, geometric — and so how long the fringe takes to die away.'),
    bool('fire', 'Fire', 0, 'Eye',
      'A saccade by hand, in any mode. The plugin declares this as FF_TYPE_EVENT and a host draws it as a button; the kit has no event type, so it is a toggle the renderer releases itself — which is why it blinks.'),

    bool('bitsOn', 'Bit Planes', 0, 'DMD',
      'Grey made of binary-weighted planes within each segment. With eye motion the planes of one grey land in different places and two greys whose bit patterns differ break apart: dynamic false contouring.'),
    std('bitDepth', 'Bit Depth', bitDepthSlider(6), 'DMD',
      (v) => `${bitDepthFromSlider(v)} bits`,
      '1 to 8. An integer the plugin takes typed; a slider here that lands on each one.'),

    opt('chip', 'Chip Mode', CHIP_MODES, 0, 'Three-Chip',
      'Three Chip: no wheel — all three colours for the whole frame, so no temporal fringe. What is left is the static misconvergence of three panels, below.'),
    std('redDx', 'Red dx', 0.5, 'Three-Chip', (v) => px(controls.chipOffset(v)), 'Three Chip only. ±8 px.'),
    std('redDy', 'Red dy', 0.5, 'Three-Chip', (v) => px(controls.chipOffset(v)), 'Three Chip only. ±8 px, positive down.'),
    std('blueDx', 'Blue dx', 0.5, 'Three-Chip', (v) => px(controls.chipOffset(v)), 'Three Chip only. ±8 px.'),
    std('blueDy', 'Blue dy', 0.5, 'Three-Chip', (v) => px(controls.chipOffset(v)), 'Three Chip only. ±8 px, positive down.'),
    std('panelRotate', 'Panel Rotate', 0.5, 'Three-Chip',
      (v) => `${((clamp01(v) - 0.5) * 2).toFixed(2)}°`,
      'Three Chip only. ±1°: red turns one way about the centre and blue the other.'),

    std('mix', 'Mix', 1, 'Output', pct, 'Against the untouched input.'),
    std('gamma', 'Output Gamma', controls.gammaParam(2.2), 'Output',
      (v) => (controls.gamma(v) <= 1 ? 'linear' : controls.gamma(v).toFixed(2)),
      'The picture is linearised on the way in and re-encoded on the way out, so the colours add in light. 1.0 is linear and skips the pow.'),
  ],

  sources: ['grid', 'bars', 'scene', 'spot', 'ramp', 'alpha'],

  // Combinations chosen for this page. The plugin ships no factory presets, so
  // every value is a real control at a real position, but the choice is ours.
  presets: {
    'Pursuit (start here)': { eye: 1 },
    'Pursuit, fast wheel (6x)': { eye: 1, wheelSpeed: 4 },
    'Pursuit, slow wheel (1x)': { eye: 1, wheelSpeed: 0, pursuitSpeed: controls.pursuitSpeedParam(16) },
    'RGBCMY, diagonal pursuit': { eye: 1, wheelType: 2, pursuitAngle: 0.125 },
    'RGBW, bright white': { eye: 1, wheelType: 1, whiteGain: 1 },
    'Custom: fat green, no white': { eye: 1, wheelType: 3, widthR: 0.2, widthG: 0.6, widthB: 0.2, widthW: 0 },
    'Track the clip': { eye: 2 },
    'Saccade (press Fire)': { eye: 3, saccadeSize: controls.saccadeSizeParam(120) },
    'False contouring (4 bit planes)': { eye: 1, bitsOn: 1, bitDepth: bitDepthSlider(4) },
    'Three chip, misconverged': { chip: 1, eye: 1, redDx: controls.chipOffsetParam(2), blueDy: controls.chipOffsetParam(-1), panelRotate: 0.7 },
  },

  differences: [
    'The audio side is not here. The plugin\'s 64-bin Audio buffer and its onset detector are absent rather than present and dead: the spectrum reaches the plugin through a Resolume FFT parameter and a browser has no equivalent. Under Eye Mode Saccade the eye moves only when you press Fire — which is the path an onset takes in the plugin, since an onset does nothing but request the same saccade.',
    'Fire is a toggle here, not an event. FFGL has FF_TYPE_EVENT and the kit\'s parameter model does not, so the renderer releases the toggle itself once it has taken the press. Bit Depth is FF_TYPE_INTEGER in the plugin, typed as a number; here it is a slider that lands on each integer from 1 to 8.',
    'Everything on the CPU side is a hand port, and nothing checks it but a reader: Controls.cpp, Segments.cpp — the wheels, the per-channel normalisation, the sub-field schedule, the eye\'s displacement and each sub-field\'s placement — the Track eye\'s block matcher, and the saccade\'s direction and decay out of Wheel::ProcessOpenGL. demo/tools/check_shaders.py only compares the four shaders, and it fails the repository\'s verify script if a character of them drifts from source/Shaders.cpp.',
    'The clock is the browser\'s, and only the clamping half of Clock.cpp survives: a frame is held to 1/240 … 1/24 s. The other half decides whether the host counts in seconds or milliseconds, which does not arise here. Saccade Time is in seconds and the eye works in frames, so a saccade decays over as many frames as your display draws in that time. Changing a control while paused draws a frame, and a saccade in flight advances by one.',
    'Track follows what the generated clip does, and the clips here are slow. Pursuit is the way to see the fringe on this page; Track is the plugin\'s own motion estimate, working, on footage that barely moves.',
    'JavaScript has no float. The plugin\'s conversions are 32-bit in C++ and 64-bit here; the schedule and the placements are double on both sides and rounded to float the same way before the shader sees them.',
    'The presets are this page\'s own combinations. The plugin ships no factory presets; every value is a real control at a real position, but the choice is ours.',
    'The About block is absent: it is four buttons that open a browser.',
    'The plugin\'s numerical proof — a still eye reconstructing the input exactly at every wheel speed, the colour separation equal to the pursuit speed times the gap between the sub-field centres, a white segment brightening a white and leaving a primary alone, and light conserved through the retina\'s box — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});
