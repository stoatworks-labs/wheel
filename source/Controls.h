#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	Every ranged parameter this plugin declares is a plain FF_TYPE_STANDARD
	float in 0..1 -- `SetParamInfo` clamps a standard default into 0..1
	before `SetParamRange` can be called (SDK b1afaf9) -- except Bit Depth,
	which is a real FF_TYPE_INTEGER with a real range because a bit depth of
	0.625 means nothing. The conversions live here, in one file the plugin
	and the harness both use, and every one that a check needs to hit
	EXACTLY has an inverse, so a check can ask for six pixels a frame rather
	than for "about six".

	Option parameters hold their element VALUE (0, 1, 2 ...), not a fraction,
	and are read with `Option()`. An option's range reads back from the host
	as 0..1 whatever its element count, which is why nothing here scales one.
*/
namespace wheel
{

int Option( float value, int count );

/// Wheel Speed's option value to rotations per frame: 1, 2, 3, 4, 6.
int RotationsFromOption( float optionValue );
constexpr int kWheelSpeedCount = 5;

/// White Gain: 0..1 linear, the multiplier on every secondary segment's
/// light. 1 is the lamp brightness the primaries get.
float WhiteGainFromParam( float value );

/// Pursuit Speed: 0..32 px per frame, linear, so whole pixel speeds land on
/// exactly representable slider positions (6 px is 0.1875).
float PursuitSpeedFromParam( float value );
float PursuitSpeedParam( float pxPerFrame );
constexpr float kMaxPursuitSpeed = 32.0f;

/// Pursuit Angle: 0..1 is 0..360 degrees, 0 rightwards, 90 DOWN the picture.
float PursuitAngleRadians( float value );

/// Saccade Size: 0..200 px of travel, linear.
float SaccadeSizeFromParam( float value );
float SaccadeSizeParam( float px );
constexpr float kMaxSaccadeSize = 200.0f;

/// Saccade Time: 0.02 to 1 s, geometric. The time constant the burst's
/// velocity decays with, and so the time the fringe takes to die away.
float SaccadeTimeFromParam( float value );
float SaccadeTimeParam( float seconds );

/// Red dx, Red dy, Blue dx, Blue dy: -8..+8 px, 0.5 is none. Whole pixels
/// land on exact slider positions: 2 px is 0.625.
float ChipOffsetFromParam( float value );
float ChipOffsetParam( float px );
constexpr float kMaxChipOffset = 8.0f;

/// Panel Rotate: -1..+1 degree, 0.5 is none. Red turns one way and blue the
/// other, in radians.
float PanelRotateRadians( float value );

/// Output Gamma: 1.0 (linear, and the pow is skipped) to 3.0, linear in
/// the slider. 2.2 is 0.6.
float GammaFromParam( float value );
float GammaParam( float gamma );

/// Bit Depth is an integer parameter; this only clamps it.
int BitDepthFromParam( float value );
constexpr int kMinBitDepth = 1;
constexpr int kMaxBitDepth = 8;

} // namespace wheel
