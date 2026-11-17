// MIDI input (winmm): all input ports, events incl. derived 14-bit CC pairs (n / n+32)
#pragma once
#include "show.h"
#include <deque>

namespace ls
{
struct MidiEvent
{
	std::string device;
	MidiType type = MidiType::CC;
	int channel   = 0;
	int number    = 0;
	float value   = 0;//0..1
	int raw       = 0;
	bool noteOn   = false;
	double t      = 0;//arrival time (Now())
};

class MidiIn
{
public:
	~MidiIn() { Close(); }
	void Open();//opens every input port
	void Close();
	void Rescan() { Close(); Open(); }
	void Poll( std::vector< MidiEvent >& out );
	std::vector< std::string > devices;//opened
	std::vector< std::string > failed;
	std::deque< std::string > monitor;//recent messages, newest last
	uint64_t received = 0;

	//output helper for self tests (e.g. LoopBe1): returns false if no matching port
	static bool SendShort( const std::string& portSubstr, uint8_t status, uint8_t d1, uint8_t d2 );
	static std::vector< std::string > OutputPorts();

	struct Raw { int dev; uint32_t msg; double t = 0; };
	void* wake = nullptr;//event (HANDLE) signalled when a message arrives (engine thread)
	std::map< std::string, double > lastActivity;//device -> Now() of the last message
	//MIDI clock / transport messages since the last Poll (consumed by the tempo clock)
	struct ClockMsg { std::string device; uint8_t status = 0; int spp = 0; double t = 0; };
	std::vector< ClockMsg > clock;
	std::map< std::string, double > lastClock;//device -> Now() of the last clock message
	//MIDI timecode (quarter frames 0xF1): position in seconds at mtcAt (Now()), frame rate; mtcAt = 0: none yet
	double mtcSec = 0, mtcAt = 0;
	int mtcFps    = 25;
	std::string mtcDevice;
	uint8_t mtcPiece_[ 8 ] = {};
	int mtcGot_  = 0;//bit mask of the pieces received
	int mtcLast_ = -1;
	std::vector< Raw > queue_;
	std::mutex lock_;

private:
	std::vector< void* > handles_;
	std::map< int, std::array< uint8_t, 128 > > ccState_;//key = dev*16+ch
};

//apply one event to all matching mappings; returns number of hits
int ApplyMidi( Show& show, const MidiEvent& e );
bool MidiMatches( const MidiMap& m, const MidiEvent& e );
}//namespace ls
