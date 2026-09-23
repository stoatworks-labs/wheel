#pragma once

#include <vector>

/**
	The wheel as arithmetic. No GL, no pixels.

	A single-chip DLP shows one colour at a time. The lamp shines through a
	spinning wheel of filters onto the DMD, which holds the current frame for
	the whole frame period and, during each segment's pass, shows that frame's
	content through that segment's filter. Nothing here interpolates between
	frames, because a DMD does not.

	**Sub-fields.** With `rotations` turns per frame and a wheel of segments
	with angular widths `w_k` (fractions of a turn, summing to 1), each segment
	appears `rotations` times per frame and each appearance lasts `w_k /
	rotations` of the frame. That list -- filter, share of the frame, start and
	end time -- is a `SubField`, and the whole frame is a `Schedule`. The
	harness's `--separation` expectation, `v * ( t_G - t_R )`, is the centre
	time of the green sub-field minus the red one's, and it is computed THERE
	from the wheel's stated widths, not read out of this schedule.

	**Normalisation.** The primaries are normalised per channel so that the
	red, green and blue segments together reconstruct the input exactly: the
	weight of a primary sub-field is `( w_k / rotations ) / sum over the
	primary segments of w_j * F_j[ c ]`. For one primary per channel that is
	`w_k / w_k` times `1 / rotations`, and in IEEE arithmetic `x / x` is
	exactly one. A white or secondary segment (W, C, M, Y) sits on top of
	that at the same lamp brightness, scaled by White Gain -- so an RGBW wheel
	brightens a white patch and leaves a saturated primary alone, which is the
	claim `--white` measures. It also means a bright white can leave the
	0..1 range; that is what a real white segment does to the colour/white
	brightness ratio, and Output Gamma's re-encode clips it.

	**Secondaries show the shared component.** A white segment shows
	min( r, g, b ) -- the part of the pixel all three primaries agree on --
	and a cyan segment min( g, b ), and so on. That is how BrilliantColor-style
	processing splits a pixel across a six-segment wheel, and it is why a
	saturated primary gets nothing from them.
*/
namespace wheel
{

/// What Wheel Type stores. A progression, so the list is not sorted.
enum WheelType
{
	kWheelRGB    = 0,///< three equal segments, R G B
	kWheelRGBW   = 1,///< four equal segments, R G B W
	kWheelRGBCMY = 2,///< six equal segments in the BrilliantColor order R Y G C B M
	kWheelCustom = 3,///< R G B W with the four width sliders
	kWheelTypeCount
};

/// One filter on the wheel.
struct Segment
{
	float filter[ 3 ]; ///< which channels the filter passes, 0 or 1
	double width;      ///< fraction of a turn
	bool secondary;    ///< W, C, M, Y: shows the shared component, scaled by White Gain
};

/// The wheel for a type. `customWidths` is R, G, B, W and is only read for
/// kWheelCustom; the widths are normalised to sum to 1, a primary is never
/// allowed below kMinPrimaryWidth (a wheel with no red segment cannot show
/// red at all, and would divide by zero normalising it), and a secondary at
/// zero width is dropped.
std::vector< Segment > WheelSegments( int type, const float customWidths[ 4 ] );

constexpr double kMinPrimaryWidth = 0.02;

/// One pass of one segment: the DMD showing this frame through this filter
/// between t0 and t1 (fractions of the frame period).
struct SubField
{
	float filter[ 3 ];   ///< the segment's filter
	double weight[ 3 ];  ///< per-channel gain, normalised as described above; White Gain folded in
	double alphaWeight;  ///< share of the frame's alpha this sub-field carries; primaries only, summing to 1
	bool secondary;
	double t0;
	double t1;
	double centre() const
	{
		return 0.5 * ( t0 + t1 );
	}
};

/// Every sub-field in one frame, in time order.
using Schedule = std::vector< SubField >;

/// The largest schedule the shader accepts: six segments at 6x.
constexpr int kMaxSubFields = 36;

/// Build the frame's schedule. `timeScale` is a test hook -- 1.0 is the
/// wheel; anything else stretches every sub-field's centre away from the
/// frame's midpoint, which is the "wrong segment times" the negative
/// control needs.
Schedule BuildSchedule( const std::vector< Segment >& segments, int rotations, double whiteGain, double timeScale = 1.0 );

/// The three-chip schedule: one sub-field, all three channels, the whole
/// frame. No wheel, so no temporal fringe; the eye still blurs it.
Schedule ThreeChipSchedule();

//---------------------------------------------------------------------------
// The eye.
//---------------------------------------------------------------------------

/**
	Retinal displacement within one frame, in pixels, as a function of time
	`t` in 0..1 frames. Positive x is rightwards and positive y is DOWN, in
	picture space.

	A pursuit is a constant velocity. A saccade is a burst whose velocity
	decays exponentially with time constant `tau` frames, having `remaining`
	pixels still to travel at the start of this frame along `direction` (a
	unit vector) -- so the displacement it adds by time t is
	`remaining * ( 1 - exp( -t / tau ) )`. Pursuit and saccade add.

	Everything is evaluated relative to the frame's midpoint, so a still eye
	on a still picture is displaced by nothing and a pursuit leaves the
	picture centred on where the frame put it rather than sliding it by half
	a frame's travel.
*/
struct Eye
{
	double pursuit[ 2 ]     = { 0.0, 0.0 };///< px per frame
	double saccadeRemaining = 0.0;         ///< px left to travel at t = 0
	double saccadeTau       = 1.0;         ///< frames
	double saccadeDir[ 2 ]  = { 1.0, 0.0 };

	/// Displacement at time t, relative to t = 0.
	void DisplacementAt( double t, double out[ 2 ] ) const;
};

/// What the shader needs for one sub-field: where its centre landed on the
/// retina relative to the frame's midpoint, and the vector its box blur
/// spans (from its start to its end).
struct Placement
{
	double offset[ 2 ];
	double box[ 2 ];
};

Placement PlaceSubField( const Eye& eye, const SubField& field );

} // namespace wheel
