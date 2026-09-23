#pragma once

#include <array>

/**
	A yes-or-no per frame from Resolume's spectrum: did a transient just
	arrive. It is what fires a saccade.

	Resolume fills a buffer parameter declared `FF_USAGE_FFT` with 64 bins
	once per frame, so the finest interval this can resolve is a frame.
	Nobody has measured what those bins are -- magnitude or power, which
	window, whether they are linear from 0 to Nyquist -- so nothing here
	depends on it beyond "a hit makes some of them rise": the detector is
	positive spectral flux summed over every bin, against an adaptive floor.

	**It is primed on its first frame.** The first spectrum it sees is taken
	as the previous one and not compared against silence. Without that,
	every bin reads as having risen from zero on frame one, and the detector
	fires the moment a clip is triggered -- which is a saccade nobody asked
	for on every clip launch -- and the one-pole floor, fed a frame of dt = 0,
	would snap to that spurious flux and be deaf for a second afterwards.
*/
namespace wheel
{

constexpr int kAudioBins = 64;

class Onset
{
public:
	/// `bins` is what the host handed over, `count` may be fewer than
	/// kAudioBins; `dt` is the frame in seconds.
	void Update( const float* bins, int count, float dt );

	/// True on the frame a transient was detected.
	bool Fired() const
	{
		return fired;
	}

	unsigned long Count() const
	{
		return onsets;
	}

	/// Forget everything, including the priming. The next frame primes again.
	void Reset();

	/// The negative control: with priming off the first frame compares
	/// against silence and fires on any signal at all.
	void SetPrimingForTest( bool on )
	{
		primeOnFirstFrame = on;
	}

private:
	std::array< float, kAudioBins > previous {};
	float fluxMean         = 0.0f;
	float refractory       = 0.0f;
	bool primed            = false;
	bool primeOnFirstFrame = true;
	bool fired             = false;
	unsigned long onsets   = 0;
};

} // namespace wheel
