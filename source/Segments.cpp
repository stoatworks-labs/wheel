#include "Segments.h"

#include <algorithm>
#include <cmath>

namespace wheel
{
namespace
{
Segment primary( int channel, double width )
{
	Segment s {};
	s.filter[ channel ] = 1.0f;
	s.width             = width;
	s.secondary         = false;
	return s;
}

Segment secondary( bool r, bool g, bool b, double width )
{
	Segment s {};
	s.filter[ 0 ] = r ? 1.0f : 0.0f;
	s.filter[ 1 ] = g ? 1.0f : 0.0f;
	s.filter[ 2 ] = b ? 1.0f : 0.0f;
	s.width       = width;
	s.secondary   = true;
	return s;
}
} // namespace

std::vector< Segment > WheelSegments( int type, const float customWidths[ 4 ] )
{
	std::vector< Segment > out;

	switch( type )
	{
	default:
	case kWheelRGB:
		out = { primary( 0, 1.0 / 3.0 ), primary( 1, 1.0 / 3.0 ), primary( 2, 1.0 / 3.0 ) };
		break;

	case kWheelRGBW:
		out = { primary( 0, 0.25 ), primary( 1, 0.25 ), primary( 2, 0.25 ), secondary( true, true, true, 0.25 ) };
		break;

	case kWheelRGBCMY:
		//The BrilliantColor order alternates primaries and secondaries, so a
		//primary is never next to another primary: R Y G C B M.
		out = {
			primary( 0, 1.0 / 6.0 ),
			secondary( true, true, false, 1.0 / 6.0 ),
			primary( 1, 1.0 / 6.0 ),
			secondary( false, true, true, 1.0 / 6.0 ),
			primary( 2, 1.0 / 6.0 ),
			secondary( true, false, true, 1.0 / 6.0 ),
		};
		break;

	case kWheelCustom:
	{
		double w[ 4 ];
		for( int i = 0; i < 4; ++i )
			w[ i ] = std::max( 0.0, static_cast< double >( customWidths ? customWidths[ i ] : 0.25f ) );
		//A primary can be narrow but never absent.
		for( int i = 0; i < 3; ++i )
			w[ i ] = std::max( w[ i ], kMinPrimaryWidth );

		const double sum = w[ 0 ] + w[ 1 ] + w[ 2 ] + w[ 3 ];
		for( int i = 0; i < 4; ++i )
			w[ i ] /= sum;

		out = { primary( 0, w[ 0 ] ), primary( 1, w[ 1 ] ), primary( 2, w[ 2 ] ) };
		if( w[ 3 ] > 0.0 )
			out.push_back( secondary( true, true, true, w[ 3 ] ) );
		break;
	}
	}

	return out;
}

Schedule BuildSchedule( const std::vector< Segment >& segments, int rotations, double whiteGain, double timeScale )
{
	Schedule schedule;
	rotations = std::clamp( rotations, 1, 6 );

	//The per-channel primary normalisation: what the primaries alone add up
	//to for each channel, over one turn.
	double primarySum[ 3 ] = { 0.0, 0.0, 0.0 };
	for( const Segment& s : segments )
		if( !s.secondary )
			for( int c = 0; c < 3; ++c )
				primarySum[ c ] += s.width * s.filter[ c ];
	for( double& p : primarySum )
		if( p <= 0.0 )
			p = 1.0;//a wheel with no such primary; nothing to normalise

	double primaryWidth = 0.0;
	for( const Segment& s : segments )
		if( !s.secondary )
			primaryWidth += s.width;

	double turnStart = 0.0;
	for( int r = 0; r < rotations; ++r )
	{
		double at = turnStart;
		for( const Segment& s : segments )
		{
			SubField f {};
			for( int c = 0; c < 3; ++c )
				f.filter[ c ] = s.filter[ c ];
			f.secondary = s.secondary;

			const double share = s.width / rotations;//of the frame
			f.t0               = at;
			f.t1               = at + share;
			at                 = f.t1;

			for( int c = 0; c < 3; ++c )
			{
				if( s.filter[ c ] <= 0.0f )
				{
					f.weight[ c ] = 0.0;
					continue;
				}
				//For a primary this is ( w / R ) / w = 1 / R exactly when the
				//channel has one primary segment; `s.width / primarySum` is
				//x / x and IEEE makes that exactly 1. The secondaries take
				//the same lamp brightness as that channel's primaries, times
				//White Gain.
				f.weight[ c ] = ( s.width / primarySum[ c ] ) / rotations;
				if( s.secondary )
					f.weight[ c ] *= whiteGain;
			}

			//Alpha rides on the primaries only, in proportion to their
			//widths, so it sums to one however the wheel is cut and is not
			//pushed past one by a white segment.
			f.alphaWeight = s.secondary ? 0.0 : ( s.width / primaryWidth ) / rotations;

			schedule.push_back( f );
		}
		turnStart += 1.0 / rotations;
	}

	if( timeScale != 1.0 )
	{
		//The negative control: every sub-field's centre pushed away from the
		//frame's midpoint, its duration kept.
		for( SubField& f : schedule )
		{
			const double c = 0.5 + ( f.centre() - 0.5 ) * timeScale;
			const double d = f.t1 - f.t0;
			f.t0           = c - 0.5 * d;
			f.t1           = c + 0.5 * d;
		}
	}

	if( schedule.size() > static_cast< size_t >( kMaxSubFields ) )
		schedule.resize( kMaxSubFields );

	return schedule;
}

Schedule ThreeChipSchedule()
{
	SubField f {};
	f.filter[ 0 ] = f.filter[ 1 ] = f.filter[ 2 ] = 1.0f;
	f.weight[ 0 ] = f.weight[ 1 ] = f.weight[ 2 ] = 1.0;
	f.alphaWeight = 1.0;
	f.secondary   = false;
	f.t0          = 0.0;
	f.t1          = 1.0;
	return { f };
}

//---------------------------------------------------------------------------
void Eye::DisplacementAt( double t, double out[ 2 ] ) const
{
	out[ 0 ] = pursuit[ 0 ] * t;
	out[ 1 ] = pursuit[ 1 ] * t;

	if( saccadeRemaining > 0.0 && saccadeTau > 0.0 )
	{
		const double travelled = saccadeRemaining * ( 1.0 - std::exp( -t / saccadeTau ) );
		out[ 0 ] += saccadeDir[ 0 ] * travelled;
		out[ 1 ] += saccadeDir[ 1 ] * travelled;
	}
}

Placement PlaceSubField( const Eye& eye, const SubField& field )
{
	//Light shown at screen position x while the eye has moved by E lands
	//on the retina at x - E, so the retinal image of the held frame is
	//I( r + E ). The sub-field's centre sits at +E( t_c ) relative to the
	//frame's midpoint, and its box runs from E( t0 ) to E( t1 ).
	//
	//The shader integrates a uniform box between the two ends, so the
	//box's own midpoint is what it is centred on. For a pursuit that IS
	//E( t_c ); for a saccade, whose velocity decays across the sub-field,
	//it is the average of the ends and the box is a linear stand-in for a
	//displacement that was slightly curved. See AGENTS.md.
	double mid[ 2 ], start[ 2 ], end[ 2 ];
	eye.DisplacementAt( 0.5, mid );
	eye.DisplacementAt( field.t0, start );
	eye.DisplacementAt( field.t1, end );

	Placement p;
	for( int i = 0; i < 2; ++i )
	{
		p.offset[ i ] = 0.5 * ( start[ i ] + end[ i ] ) - mid[ i ];
		p.box[ i ]    = end[ i ] - start[ i ];
	}
	return p;
}

} // namespace wheel
