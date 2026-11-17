// all targets that MIDI learn can assign in the current setup. One list for the "Lernbare Ziele" window
// and the Ableton manifest (midiTargets), so both never disagree.
#pragma once
#include "params.h"

namespace ls
{
struct MidiTargetInfo
{
	std::string key;      //stable key: attr:<id>:<Attr>:<head>, dmx:<u>:<ch>[:16], snap:<n>, haze, master, fx:<addr>
	std::string category; //section, e.g. "Lampen › LED PAR", "DMX › Universe 1"
	std::string label;
	std::string kind;     //fixtureAttr | dmx | snapshot | haze | master | param
	std::string addr;     //OSC address that sets the value
	std::string type;     //float | int | bool | trigger
	float min = 0, max = 1, value = 0;
	MidiMap tmpl;         //template for MIDI learn (same fields MidiLearn uses)
	//kind specific
	std::vector< int > fixtures;
	std::string attr;
	int head = -1;        //0-based pixel, -1 = all
	int universe = 0, channel = 0;
	bool sixteen = false;
	int snapshot = 0;     //1-based
};

std::vector< MidiTargetInfo > CollectMidiTargets( Show& show );
//MIDI learn template for a target (the map a learned controller is stored as)
MidiMap MidiMapForTarget( const MidiTargetInfo& t );
//stable key of an existing mapping (same schema as MidiTargetInfo::key)
std::string MidiTargetKey( const MidiMap& m );
const char* ParamTypeName( ParamType t );
}//namespace ls
