#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace wheel
{
namespace
{
inline float clamp01( float value )
{
	return std::min( std::max( value, 0.0f ), 1.0f );
}

constexpr float kPi = 3.14159265358979f;

/// Geometric interpolation: equal slider movements are equal ratios.
inline float geometric( float from, float to, float t )
{
	return from * std::pow( to / from, clamp01( t ) );
}

inline float geometricInverse( float from, float to, float value )
{
	return std::log( value / from ) / std::log( to / from );
}
} // namespace

int Option( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

int RotationsFromOption( float optionValue )
{
	static const int table[ kWheelSpeedCount ] = { 1, 2, 3, 4, 6 };
	return table[ Option( optionValue, kWheelSpeedCount ) ];
}

float WhiteGainFromParam( float value )
{
	return clamp01( value );
}

float PursuitSpeedFromParam( float value )
{
	return clamp01( value ) * kMaxPursuitSpeed;
}

float PursuitSpeedParam( float pxPerFrame )
{
	return pxPerFrame / kMaxPursuitSpeed;
}

float PursuitAngleRadians( float value )
{
	return clamp01( value ) * 2.0f * kPi;
}

float SaccadeSizeFromParam( float value )
{
	return clamp01( value ) * kMaxSaccadeSize;
}

float SaccadeSizeParam( float px )
{
	return px / kMaxSaccadeSize;
}

float SaccadeTimeFromParam( float value )
{
	return geometric( 0.02f, 1.0f, value );
}

float SaccadeTimeParam( float seconds )
{
	return geometricInverse( 0.02f, 1.0f, seconds );
}

float ChipOffsetFromParam( float value )
{
	return ( clamp01( value ) - 0.5f ) * 2.0f * kMaxChipOffset;
}

float ChipOffsetParam( float px )
{
	return 0.5f + px / ( 2.0f * kMaxChipOffset );
}

float PanelRotateRadians( float value )
{
	return ( clamp01( value ) - 0.5f ) * 2.0f * ( kPi / 180.0f );
}

float GammaFromParam( float value )
{
	return 1.0f + clamp01( value ) * 2.0f;
}

float GammaParam( float gamma )
{
	return ( gamma - 1.0f ) / 2.0f;
}

int BitDepthFromParam( float value )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), kMinBitDepth, kMaxBitDepth );
}

} // namespace wheel
