// OSC input (UDP): drives fixtures, raw DMX channels, master, haze and snapshots from any OSC sender
// (TouchDesigner, Max, Resolume, Ableton via Max for Live / Connection Kit, phone apps).
//   /ls/fixture/<id>/<attribute> v        attribute name as shown in the inspector, e.g. dimmer, red, pan, zoom
//   /ls/fixture/<id>/<attribute>/<head> v pixel fixtures: one cell (1-based)
//   /ls/fixture/<id>/color r g b          colour in one message (2.1: any colour lamp - RGB/W/A/L, CMY, wheel)
//   /ls/fixture/<id>/kelvin k             colour temperature (k > 1: Kelvin, else 0..1 = 2000..10000 K)
//   /ls/fixture/<id>/pan_deg d            pan / tilt in degrees from the centre (also tilt_deg)
//   /ls/dmx/<universe>/<channel> v        float 0..1 or int 0..255
//   /ls/master v   /ls/haze v   /ls/snapshot n (1-based)
// v: float or int; floats are 0..1. A bundle (#bundle) may carry many messages.
#pragma once
#include "show.h"
#include <atomic>
#include <deque>
#include <thread>

namespace ls
{
struct OscMsg
{
	std::string address;
	std::vector< float > f;//all numeric arguments as float (ints converted)
	std::vector< bool > isInt;
	std::vector< std::string > strs;//string arguments in order
	uint32_t fromIp   = 0;//sender (network byte order), set by OscIn
	uint16_t fromPort = 0;
};

//parses one UDP datagram (message or bundle); returns false if it is not valid OSC
bool ParseOsc( const uint8_t* p, int n, std::vector< OscMsg >& out );
//builds a message with float arguments (self test, tools)
std::vector< uint8_t > BuildOsc( const std::string& address, const std::vector< float >& args, bool asInt = false );

class OscIn
{
public:
	~OscIn() { Stop(); }
	bool Start( int port );//false: port busy
	void Stop();
	bool Running() const { return run_; }
	int Port() const { return port_; }
	void Poll( std::vector< OscMsg >& out );
	std::atomic< uint64_t > received{ 0 }, bad{ 0 };
	std::deque< std::string > monitor;//recent messages (main thread)
	std::string error;
	void* wake = nullptr;//event (HANDLE) signalled when a message arrives

private:
	void Loop();
	std::thread th_;
	std::atomic< bool > run_{ false };
	uintptr_t sock_ = ~(uintptr_t)0;
	int port_       = 0;
	std::mutex lock_;
	std::vector< OscMsg > queue_;
};

//apply one message to the show; returns false if the address is unknown
bool ApplyOsc( Show& show, const OscMsg& m );
}//namespace ls
