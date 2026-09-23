#include "Wheel.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

using namespace ffglex;
using namespace wheel;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Wheel >,// Create method
	"WH01",                // Plugin unique ID of maximum length 4.
	"SW Wheel",            // Plugin name
	2,                     // API major version number
	1,                     // API minor version number
	0,                     // Plugin major version number
	1,                     // Plugin minor version number
	FF_EFFECT,             // Plugin type
	"A single-chip DLP projector, rainbow effect and all.\n\nThe projector shows a red picture, then a green one, then a blue one through a spinning filter wheel, and your eye adds them up. It only adds them up correctly if it is still. Pursue something, or let a saccade fly on an audio onset, and each colour lands on the retina somewhere else: the rainbow fringe. A still eye shows no fringe at all, at any wheel speed, because the DMD holds the frame.\n\nStart with Eye Mode on Pursuit.",// Plugin description
	"Wheel FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kWheelTypeNames[]  = { "RGB", "RGBW", "RGBCMY", "Custom" };
const char* const kWheelSpeedNames[] = { "1x", "2x", "3x", "4x", "6x" };
const char* const kEyeNames[]        = { "Still", "Pursuit", "Track", "Saccade" };
const char* const kChipNames[]       = { "Single Chip", "Three Chip" };

constexpr double kTau = 6.283185307179586;

/// A PCG output mix, exact in 32 bits. A saccade's direction has to be
/// random enough to look like one and repeatable enough for the harness.
uint32_t hashInt( uint32_t seed )
{
	uint32_t state = seed * 747796405u + 2891336453u;
	uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

/// The Track eye's search, in grid cells either side.
constexpr int kSearch = 3;
} // namespace

Wheel::Wheel()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//A saccade decays in seconds and the onset detector needs a frame
	//length, so the effect needs the host's clock. Without this the host
	//never fills the FFT buffer either.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter.
	//
	// An RGB wheel at 2x with a still eye: dropped on a layer, this is the
	// projector working as intended, which shows nothing at all until the
	// eye moves. That is the point, and Eye Mode is the first thing to
	// reach for.
	//---------------------------------------------------------------------
	params[ PT_WHEEL_TYPE ]  = static_cast< float >( kWheelRGB );
	params[ PT_WHEEL_SPEED ] = 1.0f;//2x
	params[ PT_WIDTH_R ]     = 0.25f;
	params[ PT_WIDTH_G ]     = 0.25f;
	params[ PT_WIDTH_B ]     = 0.25f;
	params[ PT_WIDTH_W ]     = 0.25f;
	params[ PT_WHITE_GAIN ]  = 0.5f;

	params[ PT_EYE ]           = static_cast< float >( kEyeStill );
	params[ PT_PURSUIT_SPEED ] = PursuitSpeedParam( 8.0f );
	params[ PT_PURSUIT_ANGLE ] = 0.0f;
	params[ PT_SACCADE_SIZE ]  = SaccadeSizeParam( 40.0f );
	params[ PT_SACCADE_TIME ]  = SaccadeTimeParam( 0.1f );
	params[ PT_FIRE ]          = 0.0f;
	params[ PT_AUDIO ]         = 0.0f;

	params[ PT_BITS_ON ]   = 0.0f;
	params[ PT_BIT_DEPTH ] = 6.0f;

	params[ PT_CHIP ]         = static_cast< float >( kChipSingle );
	params[ PT_RED_DX ]       = 0.5f;
	params[ PT_RED_DY ]       = 0.5f;
	params[ PT_BLUE_DX ]      = 0.5f;
	params[ PT_BLUE_DY ]      = 0.5f;
	params[ PT_PANEL_ROTATE ] = 0.5f;

	params[ PT_MIX ]   = 1.0f;
	params[ PT_GAMMA ] = GammaParam( 2.2f );

	//---------------------------------------------------------------------
	// Declaration. Every ranged parameter is a plain 0..1 float: SetParamInfo
	// clamps an FF_TYPE_STANDARD default into 0..1 before a range can be
	// attached (SDK b1afaf9). Bit Depth is a real integer, which is exempt.
	// The conversions live in Controls.cpp.
	//
	// Option lists are in their natural order and NOT sorted: each is a
	// progression (RGB -> RGBCMY, 1x -> 6x, Still -> Saccade) or a pair.
	//---------------------------------------------------------------------
	auto declareOptions = [ this ]( unsigned int paramID, const char* name, const char* const* names, int count ) {
		SetOptionParamInfo( paramID, name, static_cast< unsigned int >( count ), params[ paramID ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( paramID, static_cast< unsigned int >( i ), names[ i ], static_cast< float >( i ) );
	};

	declareOptions( PT_WHEEL_TYPE, "Wheel Type", kWheelTypeNames, kWheelTypeCount );
	declareOptions( PT_WHEEL_SPEED, "Wheel Speed", kWheelSpeedNames, kWheelSpeedCount );
	SetParamInfof( PT_WIDTH_R, "Red Width", FF_TYPE_STANDARD );
	SetParamInfof( PT_WIDTH_G, "Green Width", FF_TYPE_STANDARD );
	SetParamInfof( PT_WIDTH_B, "Blue Width", FF_TYPE_STANDARD );
	SetParamInfof( PT_WIDTH_W, "White Width", FF_TYPE_STANDARD );
	SetParamInfof( PT_WHITE_GAIN, "White Gain", FF_TYPE_STANDARD );

	declareOptions( PT_EYE, "Eye Mode", kEyeNames, kEyeModeCount );
	SetParamInfof( PT_PURSUIT_SPEED, "Pursuit Speed", FF_TYPE_STANDARD );
	SetParamInfof( PT_PURSUIT_ANGLE, "Pursuit Angle", FF_TYPE_STANDARD );
	SetParamInfof( PT_SACCADE_SIZE, "Saccade Size", FF_TYPE_STANDARD );
	SetParamInfof( PT_SACCADE_TIME, "Saccade Time", FF_TYPE_STANDARD );
	//An event, which the host draws as a button: a saccade by hand.
	SetParamInfo( PT_FIRE, "Fire", FF_TYPE_EVENT, false );
	//The spectrum. Resolume fills a buffer declared FF_USAGE_FFT with 64
	//bins once per frame; nothing else in FFGL carries audio.
	SetBufferParamInfo( PT_AUDIO, "Audio", kAudioBins, FF_USAGE_FFT );
	for( int i = 0; i < kAudioBins; ++i )
		SetParamElementInfo( PT_AUDIO, static_cast< unsigned int >( i ), "", 0.0f );

	SetParamInfof( PT_BITS_ON, "Bit Planes", FF_TYPE_BOOLEAN );
	//Only FF_TYPE_STANDARD gets its default clamped into 0..1, so an integer
	//can be declared with its real default and range.
	SetParamInfo( PT_BIT_DEPTH, "Bit Depth", FF_TYPE_INTEGER, params[ PT_BIT_DEPTH ] );
	SetParamRange( PT_BIT_DEPTH, static_cast< float >( kMinBitDepth ), static_cast< float >( kMaxBitDepth ) );

	declareOptions( PT_CHIP, "Chip Mode", kChipNames, kChipModeCount );
	SetParamInfof( PT_RED_DX, "Red dx", FF_TYPE_STANDARD );
	SetParamInfof( PT_RED_DY, "Red dy", FF_TYPE_STANDARD );
	SetParamInfof( PT_BLUE_DX, "Blue dx", FF_TYPE_STANDARD );
	SetParamInfof( PT_BLUE_DY, "Blue dy", FF_TYPE_STANDARD );
	SetParamInfof( PT_PANEL_ROTATE, "Panel Rotate", FF_TYPE_STANDARD );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );
	SetParamInfof( PT_GAMMA, "Output Gamma", FF_TYPE_STANDARD );

	//SetParamGroup collapses consecutive ids under one header, so each
	//group is a contiguous run of the enum.
	for( FFUInt32 i = PT_WHEEL_TYPE; i <= PT_WHITE_GAIN; ++i )
		SetParamGroup( i, "Wheel" );
	for( FFUInt32 i = PT_EYE; i <= PT_AUDIO; ++i )
		SetParamGroup( i, "Eye" );
	for( FFUInt32 i = PT_BITS_ON; i <= PT_BIT_DEPTH; ++i )
		SetParamGroup( i, "DMD" );
	for( FFUInt32 i = PT_CHIP; i <= PT_PANEL_ROTATE; ++i )
		SetParamGroup( i, "Three-Chip" );
	for( FFUInt32 i = PT_MIX; i <= PT_GAMMA; ++i )
		SetParamGroup( i, "Output" );

	// The About block. Inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Wheel effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Wheel::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &copyShader, kCopyShader, "copy" },
		{ &gridShader, kGridShader, "grid" },
		{ &integrateShader, kIntegrateShader, "integrate" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( kVertexShader, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Wheel: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Wheel: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	gridPreviousValid = false;
	saccadeRemaining  = 0.0;
	onset.Reset();
	//firePending is deliberately NOT cleared: a host pushes parameter values
	//before InitGL, and a Fire that arrived then is still a press. The sweep
	//found this as a dead control when it was cleared.

	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void Wheel::estimateMotion( int width, int height )
{
	//A resize files every cell under a different patch of picture, so the
	//previous grid means nothing. Start again rather than compare.
	if( gridPictureWidth != width || gridPictureHeight != height )
	{
		gridPreviousValid = false;
		gridPictureWidth  = width;
		gridPictureHeight = height;
	}

	if( !gridPreviousValid )
	{
		gridPrevious      = gridNow;
		gridPreviousValid = true;
		trackVelocity[ 0 ] = trackVelocity[ 1 ] = 0.0;
		return;
	}

	//Sum of absolute differences at every whole-cell shift, then a
	//parabola through the minimum and its neighbours on each axis for the
	//fraction. This is a global estimate: the eye follows whatever most of
	//the picture is doing.
	auto sad = [ & ]( int sx, int sy ) {
		double sum = 0.0;
		int n      = 0;
		for( int y = 0; y < kGridHeight; ++y )
		{
			const int py = y - sy;
			if( py < 0 || py >= kGridHeight )
				continue;
			for( int x = 0; x < kGridWidth; ++x )
			{
				const int px = x - sx;
				if( px < 0 || px >= kGridWidth )
					continue;
				sum += std::fabs( static_cast< double >( gridNow[ y * kGridWidth + x ] ) - gridPrevious[ py * kGridWidth + px ] );
				++n;
			}
		}
		return n > 0 ? sum / n : 1e9;
	};

	double best = 1e18;
	int bx = 0, by = 0;
	double table[ 2 * kSearch + 1 ][ 2 * kSearch + 1 ];
	for( int sy = -kSearch; sy <= kSearch; ++sy )
		for( int sx = -kSearch; sx <= kSearch; ++sx )
		{
			const double s = sad( sx, sy );
			table[ sy + kSearch ][ sx + kSearch ] = s;
			//Strictly less, scanning from the most negative shift, so a flat
			//picture -- every shift equally good -- would land on the corner.
			//Prefer no motion on a tie instead.
			if( s < best - 1e-12 || ( std::fabs( s - best ) <= 1e-12 && sx == 0 && sy == 0 ) )
			{
				best = s;
				bx   = sx;
				by   = sy;
			}
		}

	auto refine = [ & ]( double left, double centre, double right ) {
		const double denom = left - 2.0 * centre + right;
		if( denom <= 1e-12 )
			return 0.0;
		return std::clamp( 0.5 * ( left - right ) / denom, -0.5, 0.5 );
	};

	double fx = 0.0, fy = 0.0;
	if( bx > -kSearch && bx < kSearch )
		fx = refine( table[ by + kSearch ][ bx - 1 + kSearch ], best, table[ by + kSearch ][ bx + 1 + kSearch ] );
	if( by > -kSearch && by < kSearch )
		fy = refine( table[ by - 1 + kSearch ][ bx + kSearch ], best, table[ by + 1 + kSearch ][ bx + kSearch ] );

	//A still picture is still. The parabola is only meaningful when the
	//minimum is a minimum.
	const double cellW = static_cast< double >( width ) / kGridWidth;
	const double cellH = static_cast< double >( height ) / kGridHeight;
	double vx          = ( bx + fx ) * cellW;
	double vy          = ( by + fy ) * cellH;
	//The grid is in GL orientation, y up; the eye works in picture space,
	//y down.
	vy = -vy;

	if( best <= 1e-6 )
		vx = vy = 0.0;

	//Half-way each frame: the eye catches up over a couple of frames, and a
	//one-frame glitch in the estimate does not throw it.
	trackVelocity[ 0 ] += ( vx - trackVelocity[ 0 ] ) * 0.5;
	trackVelocity[ 1 ] += ( vy - trackVelocity[ 1 ] ) * 0.5;

	gridPrevious = gridNow;
}

//---------------------------------------------------------------------------
FFResult Wheel::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( picture.Width );
	const int height = static_cast< int >( picture.Height );

	//The host's viewport, read before anything of ours changes it.
	//ScopedFBOBinding restores the framebuffer binding and only that (SDK
	//b1afaf9): the copy and grid passes size the viewport to their buffers,
	//and the integrate pass draws to the host's framebuffer with nothing of
	//its own to size itself from.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// Time. The clock measures the host's unit rather than assuming it:
	// Resolume sends milliseconds, the harness sends seconds. Nothing
	// absolute is used: the wheel's phase is per frame, and the saccade is
	// integrated frame by frame in double.
	//---------------------------------------------------------------------
	clock.Update( hostTime );
	const double dt = clock.FrameSeconds();

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime )
		            + " scale=" + std::to_string( clock.ClockScale() )
		            + " seconds=" + std::to_string( clock.Now() ) );

	//---------------------------------------------------------------------
	// What the controls say.
	//---------------------------------------------------------------------
	const int wheelType = Option( params[ PT_WHEEL_TYPE ], kWheelTypeCount );
	const int rotations = RotationsFromOption( params[ PT_WHEEL_SPEED ] );
	const float widths[ 4 ] = { params[ PT_WIDTH_R ], params[ PT_WIDTH_G ], params[ PT_WIDTH_B ], params[ PT_WIDTH_W ] };
	const float whiteGain = WhiteGainFromParam( params[ PT_WHITE_GAIN ] );

	const int eyeMode        = Option( params[ PT_EYE ], kEyeModeCount );
	const float pursuitSpeed = PursuitSpeedFromParam( params[ PT_PURSUIT_SPEED ] );
	const float pursuitAngle = PursuitAngleRadians( params[ PT_PURSUIT_ANGLE ] );
	const float saccadeSize  = SaccadeSizeFromParam( params[ PT_SACCADE_SIZE ] );
	const float saccadeTime  = SaccadeTimeFromParam( params[ PT_SACCADE_TIME ] );

	const bool bitsOn  = params[ PT_BITS_ON ] >= 0.5f;
	const int bitDepth = bitsOn ? BitDepthFromParam( params[ PT_BIT_DEPTH ] ) : 0;

	const int chipMode  = Option( params[ PT_CHIP ], kChipModeCount );
	const float redDx   = ChipOffsetFromParam( params[ PT_RED_DX ] );
	const float redDy   = ChipOffsetFromParam( params[ PT_RED_DY ] );
	const float blueDx  = ChipOffsetFromParam( params[ PT_BLUE_DX ] );
	const float blueDy  = ChipOffsetFromParam( params[ PT_BLUE_DY ] );
	const float rotate  = PanelRotateRadians( params[ PT_PANEL_ROTATE ] );

	const float gamma = GammaFromParam( params[ PT_GAMMA ] );
	const float mixAmount = params[ PT_MIX ];

	//---------------------------------------------------------------------
	// Audio, every frame whatever the mode, so the detector is primed and
	// settled by the time somebody switches to Saccade. An onset this frame
	// is a saccade this frame, not next.
	//---------------------------------------------------------------------
	if( clock.Jumped() )
		onset.Reset();
	{
		float bins[ kAudioBins ] = {};
		int binCount             = 0;
		if( const ParamInfo* info = FindParamInfo( PT_AUDIO ) )
		{
			binCount = static_cast< int >( std::min< size_t >( info->elements.size(), kAudioBins ) );
			for( int i = 0; i < binCount; ++i )
				bins[ i ] = info->elements[ i ].value;
		}
		onset.Update( bins, binCount, dt );
	}

	if( eyeMode == kEyeSaccade && onset.Fired() )
		firePending = true;

	if( firePending )
	{
		firePending      = false;
		saccadeRemaining = saccadeSize;
		//A fresh direction per saccade, from a hash so it is random enough
		//to look like one and the same on every run.
		const double angle = static_cast< double >( hashInt( static_cast< uint32_t >( saccadeCount ) * 2654435761u ^ 0x9E3779B9u ) )
		                     / 4294967296.0 * kTau;
		saccadeDir[ 0 ] = std::cos( angle );
		saccadeDir[ 1 ] = std::sin( angle );
		++saccadeCount;
	}

	//---------------------------------------------------------------------
	// Buffers. Every Ensure() happens here, before anything binds a texture:
	// every ffglex Scoped* binding CLEARS to 0 on scope exit rather than
	// restoring, and FFGLFBO::Initialise sizes its new colour texture under
	// one of them.
	//---------------------------------------------------------------------
	const bool track = eyeMode == kEyeTrack;

	if( !linearCopy.Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Mipmapped ) )
	{
		diag::error( "could not allocate the linear copy: " + std::to_string( width ) + "x" + std::to_string( height ) );
		return FF_FAIL;
	}
	if( track && !grid.Ensure( kGridWidth, kGridHeight, GL_RGBA32F, PassBuffer::Sampling::Nearest ) )
	{
		diag::error( "could not allocate the motion grid" );
		return FF_FAIL;
	}
	if( interpolateForTest && !previousCopy.Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Nearest ) )
		return FF_FAIL;

	//---------------------------------------------------------------------
	// 1. Copy, linearised.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( linearCopy.GetGLID(), ScopedFBOBinding::RB_REVERT );
		linearCopy.ResizeViewPort();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.Handle );

		copyShader.Set( "InputTexture", 0 );
		copyShader.Set( "Gamma", gamma );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 2. The Track eye: a coarse grid off the mip chain, read back.
	//---------------------------------------------------------------------
	if( track )
	{
		linearCopy.GenerateMipmaps();

		const double cellW = static_cast< double >( width ) / kGridWidth;
		const double cellH = static_cast< double >( height ) / kGridHeight;
		const float lod    = static_cast< float >( std::log2( std::max( 1.0, std::min( cellW, cellH ) ) ) );

		ScopedFBOBinding fbo( grid.GetGLID(), ScopedFBOBinding::RB_REVERT );
		grid.ResizeViewPort();
		ScopedShaderBinding shader( gridShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( linearCopy.TextureID() );

		gridShader.Set( "LinearTexture", 0 );
		gridShader.Set( "Lod", lod );
		gridShader.Set( "GridWidth", kGridWidth );
		gridShader.Set( "GridHeight", kGridHeight );
		quad.Draw();

		//A synchronous read of 576 floats. It stalls until the copy and
		//grid passes have run, which is early in the frame; the cost is on
		//the record in the README's bench table.
		std::vector< float > raw( static_cast< size_t >( kGridWidth ) * kGridHeight * 4 );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, kGridWidth, kGridHeight, GL_RGBA, GL_FLOAT, raw.data() );

		gridNow.resize( static_cast< size_t >( kGridWidth ) * kGridHeight );
		for( size_t i = 0; i < gridNow.size(); ++i )
			gridNow[ i ] = raw[ i * 4 ];

		estimateMotion( width, height );
	}
	else
	{
		gridPreviousValid  = false;
		trackVelocity[ 0 ] = trackVelocity[ 1 ] = 0.0;
	}

	//---------------------------------------------------------------------
	// 3. The schedule and the eye.
	//---------------------------------------------------------------------
	Schedule schedule;
	if( chipMode == kChipThree )
		schedule = ThreeChipSchedule();
	else
		schedule = BuildSchedule( WheelSegments( wheelType, widths ), rotations, whiteGain, timeScaleForTest );

	Eye eye;
	if( eyeMode == kEyePursuit )
	{
		eye.pursuit[ 0 ] = pursuitSpeed * std::cos( pursuitAngle );
		eye.pursuit[ 1 ] = pursuitSpeed * std::sin( pursuitAngle );
	}
	else if( eyeMode == kEyeTrack )
	{
		eye.pursuit[ 0 ] = trackVelocity[ 0 ];
		eye.pursuit[ 1 ] = trackVelocity[ 1 ];
	}
	//In frames: the shader's time axis is one frame, and dt is what a frame
	//is in seconds.
	const double tauFrames = std::max( 1e-3, static_cast< double >( saccadeTime ) / std::max( 1e-4, static_cast< double >( dt ) ) );
	eye.saccadeRemaining = saccadeRemaining;
	eye.saccadeTau       = tauFrames;
	eye.saccadeDir[ 0 ]  = saccadeDir[ 0 ];
	eye.saccadeDir[ 1 ]  = saccadeDir[ 1 ];

	lastSchedule = schedule;
	lastPlacements.clear();
	float subW[ kMaxSubFields * 4 ] = {};
	float subO[ kMaxSubFields * 4 ] = {};
	float subT[ kMaxSubFields * 4 ] = {};
	const int count = static_cast< int >( std::min< size_t >( schedule.size(), kMaxSubFields ) );
	for( int k = 0; k < count; ++k )
	{
		const SubField& f   = schedule[ k ];
		const Placement pl  = PlaceSubField( eye, f );
		lastPlacements.push_back( pl );

		for( int c = 0; c < 3; ++c )
			subW[ k * 4 + c ] = static_cast< float >( f.filter[ c ] * f.weight[ c ] );
		subW[ k * 4 + 3 ] = static_cast< float >( f.alphaWeight );

		//Picture space is y down, GL is y up.
		subO[ k * 4 + 0 ] = static_cast< float >( pl.offset[ 0 ] );
		subO[ k * 4 + 1 ] = static_cast< float >( -pl.offset[ 1 ] );
		subO[ k * 4 + 2 ] = static_cast< float >( pl.box[ 0 ] );
		subO[ k * 4 + 3 ] = static_cast< float >( -pl.box[ 1 ] );

		subT[ k * 4 + 0 ] = f.secondary ? 1.0f : 0.0f;
		subT[ k * 4 + 1 ] = static_cast< float >( f.centre() );
	}

	//The saccade's travel this frame is spent.
	if( saccadeRemaining > 0.0 )
	{
		saccadeRemaining *= std::exp( -1.0 / tauFrames );
		if( saccadeRemaining < 1e-3 )
			saccadeRemaining = 0.0;
	}

	//---------------------------------------------------------------------
	// 4. Integrate, straight to the host's framebuffer.
	//---------------------------------------------------------------------
	{
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( integrateShader.GetGLID() );
		const GLuint program = integrateShader.GetGLID();

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding source( picture.Handle );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding linear( linearCopy.TextureID() );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding previous( interpolateForTest ? previousCopy.TextureID() : linearCopy.TextureID() );

		integrateShader.Set( "SourceTexture", 0 );
		integrateShader.Set( "LinearTexture", 1 );
		integrateShader.Set( "PrevTexture", 2 );
		integrateShader.Set( "Interp", interpolateForTest ? 1 : 0 );
		integrateShader.Set( "TapBias", tapBiasForTest );
		integrateShader.Set( "Width", width );
		integrateShader.Set( "Height", height );
		integrateShader.Set( "Count", count );
		//FFGLShader::Set has no array overload; these go through GL directly.
		glUniform4fv( glGetUniformLocation( program, "SubW" ), count, subW );
		glUniform4fv( glGetUniformLocation( program, "SubO" ), count, subO );
		glUniform4fv( glGetUniformLocation( program, "SubT" ), count, subT );
		integrateShader.Set( "Gamma", gamma );
		integrateShader.Set( "BitDepth", bitDepth );
		integrateShader.Set( "ChipMode", chipMode );
		integrateShader.Set( "RedOffset", redDx, -redDy );
		integrateShader.Set( "BlueOffset", blueDx, -blueDy );
		integrateShader.Set( "RedRotate", rotate );
		integrateShader.Set( "BlueRotate", -rotate );
		integrateShader.Set( "MixAmount", mixAmount );
		quad.Draw();
	}

	//The hook's previous frame, for next time.
	if( interpolateForTest )
	{
		ScopedFBOBinding fbo( previousCopy.GetGLID(), ScopedFBOBinding::RB_REVERT );
		previousCopy.ResizeViewPort();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.Handle );
		copyShader.Set( "InputTexture", 0 );
		copyShader.Set( "Gamma", gamma );
		quad.Draw();
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Wheel::DeInitGL()
{
	copyShader.FreeGLResources();
	gridShader.FreeGLResources();
	integrateShader.FreeGLResources();
	quad.Release();

	linearCopy.Destroy();
	previousCopy.Destroy();
	grid.Destroy();

	gridPreviousValid = false;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Wheel::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	//An event arrives as 1.0 on press and 0.0 on release; a saccade per
	//press, not per edge.
	if( index == PT_FIRE )
	{
		if( value >= 0.5f )
			firePending = true;
		params[ index ] = value;
		return FF_SUCCESS;
	}

	params[ index ] = value;
	return FF_SUCCESS;
}

float Wheel::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Wheel::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Wheel::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Wheel::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}
