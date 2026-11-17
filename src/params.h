// parameter register: every remote-controllable value with its OSC address, name and range.
// OSC (/ls/...) and MIDI mappings (target "Parameter") both go through it; the OSC window lists it.
#pragma once
#include "show.h"
#include <functional>

namespace ls
{
enum class ParamType : uint8_t { Float, Int, Bool, Trigger };
struct Param
{
	std::string addr;  //lower case, e.g. /ls/stage/exposure
	std::string label; //German name for lists / MIDI descriptions
	std::string group; //section in the OSC window
	ParamType type = ParamType::Float;
	float min = 0, max = 1;
	std::function< float() > get;
	std::function< void( float ) > set;
};
//all parameters of the show (cached; rebuilt when lamps/objects/effects are added or removed)
const std::vector< Param >& Params( Show& show );
const Param* FindParam( Show& show, const std::string& addr );
//value in the parameter's own unit (OSC); false if unknown
bool SetParam( Show& show, const std::string& addr, float value );
//normalized 0..1 over min..max (MIDI); triggers fire above 0.5
bool SetParamNorm( Show& show, const std::string& addr, float v01 );
std::string ParamRange( const Param& p );//"0 .. 1", "an/aus", "Auslöser" ...
}//namespace ls
