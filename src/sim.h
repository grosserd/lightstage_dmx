// fixture simulation: DMX -> physical state (intensity, colour, pan/tilt with motor lag, beam optics)
#pragma once
#include "show.h"

namespace ls
{
struct SimGlobals
{
	float haze       = 0.35f;//current effective haze density
	float hazeTarget = 0.35f;
	double time      = 0;
};

void SimulateFixture( const Show& show, FixtureInst& f, float dt, double t );
void Simulate( Show& show, SimGlobals& g, float dt );
//fixture local -> world, head (yoke/head) transforms
M4 FixtureBase( const FixtureInst& f );
V3 BodyExtents( const FixtureDef& d );//local box extents (x, beam axis, z)
//beam origin + direction in world space, for head h (pixels have their own origins)
void BeamRay( const FixtureInst& f, int head, V3& origin, V3& dir, V3& up );
}//namespace ls
