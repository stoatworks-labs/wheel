#pragma once

#include "Clock.h"
#include "Onset.h"
#include "PassBuffer.h"
#include "Segments.h"

#include <FFGLSDK.h>

#include <string>
#include <vector>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

namespace wheel
{

/**
	Wheel -- a single-chip DLP projector, as an FFGL effect.

	**The one idea.** A single-chip DLP does not show a colour picture. It
	shows a red one, then a green one, then a blue one (and on many wheels a
	white or CMY segment), in turn, through a spinning filter wheel, and the
	eye adds them up. It adds them up correctly only if the eye is still
	relative to the screen. When the eye moves -- pursuing a moving object,
	or in a saccade across a bright frame -- each colour field lands on the
	retina in a different place. That is the rainbow effect, and it is the
	whole plugin.

	So the model is a retina, not a filter. `Segments.h` turns the wheel
	into a schedule of sub-fields (a filter, a share of the frame, a start
	and an end time), the eye turns each one into a displacement and a blur
	(`Placement`), and the integrate shader adds them in linear light.
	A still eye reconstructs the input exactly, at any wheel speed, on any
	content; that is the harness's headline claim, `--still`.

	See AGENTS.md for the traps and for what is verified.
*/
class Wheel : public CFFGLPlugin
{
public:
	Wheel();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// `instantiateGL` pushes every declared default back through the
	/// setters and deletes the instance the moment one fails, and
	/// CFFGLPlugin's SetTextParameter is a stub that returns exactly that
	/// failure -- so without this override no real host can load the
	/// plugin, while every offline harness carries on passing.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- test hooks. Read or set by whtest; nothing in the plugin's own
	//--- operation touches them.

	/// The harness DECLARES its clock unit rather than leaving the
	/// calibration to infer one: it renders as fast as the GPU allows.
	void SetClockScaleForTest( double scale )
	{
		clock.SetScaleForTest( scale );
	}

	/// Negative control: give the DMD an inter-frame interpolation it does
	/// not have. `--still` must then fail.
	void SetInterpolationForTest( bool on )
	{
		interpolateForTest = on;
	}

	/// Negative control: stretch every sub-field's centre time away from the
	/// frame's midpoint. `--separation` must then fail.
	void SetTimeScaleForTest( double scale )
	{
		timeScaleForTest = scale;
	}

	/// Negative control: bias the box's end taps so the taps no longer sum to
	/// one. `--energy` must then fail.
	void SetTapBiasForTest( float bias )
	{
		tapBiasForTest = bias;
	}

	/// Negative control: an onset detector that is not primed.
	void SetOnsetPrimingForTest( bool on )
	{
		onset.SetPrimingForTest( on );
	}

	const Onset& OnsetForTest() const
	{
		return onset;
	}

	unsigned long SaccadesForTest() const
	{
		return saccadeCount;
	}

	/// The Track eye's current estimate, px per frame, picture space.
	void TrackVelocityForTest( double& vx, double& vy ) const
	{
		vx = trackVelocity[ 0 ];
		vy = trackVelocity[ 1 ];
	}

	const Schedule& ScheduleForTest() const
	{
		return lastSchedule;
	}

	const std::vector< Placement >& PlacementsForTest() const
	{
		return lastPlacements;
	}

	/// The order the host shows them in.
	enum ParamID : FFUInt32
	{
		//Wheel
		PT_WHEEL_TYPE,
		PT_WHEEL_SPEED,
		PT_WIDTH_R,
		PT_WIDTH_G,
		PT_WIDTH_B,
		PT_WIDTH_W,
		PT_WHITE_GAIN,

		//Eye
		PT_EYE,
		PT_PURSUIT_SPEED,
		PT_PURSUIT_ANGLE,
		PT_SACCADE_SIZE,
		PT_SACCADE_TIME,
		PT_FIRE,
		PT_AUDIO,

		//DMD
		PT_BITS_ON,
		PT_BIT_DEPTH,

		//Three-Chip
		PT_CHIP,
		PT_RED_DX,
		PT_RED_DY,
		PT_BLUE_DX,
		PT_BLUE_DY,
		PT_PANEL_ROTATE,

		//Output
		PT_MIX,
		PT_GAMMA,

		//About. Last in the enum, so no saved composition's parameter ids
		//shift. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// What Eye Mode stores.
	enum EyeMode
	{
		kEyeStill   = 0,///< no fringe: the DMD holds the frame and the eye holds still
		kEyePursuit = 1,///< a set velocity and angle
		kEyeTrack   = 2,///< the clip's own dominant motion, by block matching
		kEyeSaccade = 3,///< still, with a burst on every audio onset and on Fire
		kEyeModeCount
	};

	/// What Chip Mode stores.
	enum ChipMode
	{
		kChipSingle = 0,
		kChipThree  = 1,
		kChipModeCount
	};

private:
	/// The Track eye: the picture's global motion between the previous
	/// frame's grid and this one's, in pixels per frame, picture space.
	void estimateMotion( int width, int height );

	ffglex::FFGLShader copyShader;
	ffglex::FFGLShader gridShader;
	ffglex::FFGLShader integrateShader;
	ffglex::FFGLScreenQuad quad;

	PassBuffer linearCopy;
	PassBuffer previousCopy;//only under the interpolation hook
	PassBuffer grid;

	Clock clock;
	double hostTime = -1.0;
	int clockFrames = 0;

	Onset onset;

	//The saccade in flight.
	bool firePending            = false;
	double saccadeRemaining     = 0.0;
	double saccadeDir[ 2 ]      = { 1.0, 0.0 };
	unsigned long saccadeCount  = 0;

	//The Track eye's state across frames.
	std::vector< float > gridNow;
	std::vector< float > gridPrevious;
	bool gridPreviousValid    = false;
	int gridPictureWidth      = 0;
	int gridPictureHeight     = 0;
	double trackVelocity[ 2 ] = { 0.0, 0.0 };

	Schedule lastSchedule;
	std::vector< Placement > lastPlacements;

	bool interpolateForTest  = false;
	double timeScaleForTest  = 1.0;
	float tapBiasForTest     = 1.0f;

	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};

} // namespace wheel
