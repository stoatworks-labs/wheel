#include "Onset.h"

#include <algorithm>
#include <cmath>

namespace wheel
{
namespace
{
/// The absolute floor on flux. The adaptive floor alone divides noise by
/// noise during silence and finds onsets in it.
constexpr float kFluxFloor = 0.004f;

/// How far above its own recent mean the flux has to jump.
constexpr float kFluxMargin = 2.0f;

/// The mean's memory, in seconds.
constexpr float kFluxTau = 1.0f;

/// No second onset within this. 80 ms is 750 bpm in quavers.
constexpr float kRefractory = 0.08f;

float coefficient( float dt, float tau )
{
	if( dt <= 0.0f )
		return 0.0f;//no time passed: the mean does not move
	return 1.0f - std::exp( -dt / tau );
}
} // namespace

void Onset::Reset()
{
	previous.fill( 0.0f );
	fluxMean   = 0.0f;
	refractory = 0.0f;
	primed     = false;
	fired      = false;
}

void Onset::Update( const float* bins, int count, float dt )
{
	fired = false;
	const int n = std::clamp( count, 0, kAudioBins );

	//sqrt because bin magnitudes bunch hard against zero: a spectrum used
	//raw hears the kick drum and nothing else.
	float raw[ kAudioBins ] = {};
	for( int i = 0; i < n; ++i )
		raw[ i ] = std::sqrt( std::max( 0.0f, bins ? bins[ i ] : 0.0f ) );

	if( !primed && primeOnFirstFrame )
	{
		//The first frame is the reference, not a rise from silence.
		for( int i = 0; i < kAudioBins; ++i )
			previous[ i ] = raw[ i ];
		primed = true;
		return;
	}
	primed = true;

	//Positive differences only: a bin falling silent is not an onset.
	float flux = 0.0f;
	for( int i = 0; i < n; ++i )
	{
		flux += std::max( 0.0f, raw[ i ] - previous[ i ] );
		previous[ i ] = raw[ i ];
	}
	for( int i = n; i < kAudioBins; ++i )
		previous[ i ] = 0.0f;
	flux /= static_cast< float >( kAudioBins );

	refractory = std::max( 0.0f, refractory - dt );

	const float bar = std::max( kFluxFloor, fluxMean * ( 1.0f + kFluxMargin ) );
	if( refractory <= 0.0f && flux > bar )
	{
		fired      = true;
		refractory = kRefractory;
		++onsets;
	}

	//The mean follows the flux, hits included, so a run of hits raises the
	//bar and a quiet passage lowers it.
	fluxMean += ( flux - fluxMean ) * coefficient( dt, kFluxTau );
}

} // namespace wheel
