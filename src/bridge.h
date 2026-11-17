// Ableton / remote-control bridge (protocol 1, see BRIDGE_PROTOKOLL.md):
// discovery (/ls/hello), manifest over HTTP, normalised control, bulk DMX, feedback to the devices.
#pragma once
#include "json.h"
#include "osc.h"
#include "params.h"
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ls
{
struct BridgeClient
{
	std::string name, id, kinds;//kinds: comma separated subscriptions: params, dmx, state
	uint32_t ip = 0;            //network byte order
	uint16_t port = 0;          //reply port of the device
	std::string localIp;        //our address on the route to the device (sent as lightstageIp)
	double lastSeen = 0;
	//feedback memory
	std::map< std::string, float > sentParam;
	std::map< int, std::array< int, 512 > > sentDmx;//universe number -> last sent output (-1 = never)
	std::map< int, int > sentEffect;
	int sentProgram = -2;
	bool Has( const std::string& k ) const;
	std::string Addr() const;   //"ip:port"
};

class Bridge
{
public:
	~Bridge() { Stop(); }
	//opens the HTTP port in the background thread (a firewall / network filter may hold listen() for a long
	//time on the first start of a new program version; that must not freeze the window).
	//wait = true: return only when the port is open or failed (tests).
	bool Start( int httpPort, std::string* err, bool wait = false );
	void Stop();
	bool Running() const { return run_; }
	bool Ready() const { return ready_; }
	bool Failed() const { return failed_; }
	int HttpPort() const { return httpPort_; }

	//engine thread (or UI when the engine is off), engine lock held
	void OnOsc( const OscMsg& m, Show& show );///ls/hello /ls/bye /ls/norm /ls/dmxblock /ls/fixblock /ls/release
	void Tick( Show& show, double now );      //30 Hz: timeouts, structure revision, feedback
	int SendAssign( const Param& p );         //"An Ableton senden": to devices subscribed to params
	int SendAssignFixture( int fixtureId );   //"Lampe an Ableton senden": to devices subscribed to dmx
	int PushMap( const AbletonMap& m );       //knob/button table -> that device (/ls/map/all), if connected
	int Subscribers( const std::string& kind );//connected devices subscribed to params / dmx / state
	std::string ManifestJson( Show& show );   //current manifest (export)

	std::vector< BridgeClient > Clients();    //copy, for the UI
	uint32_t Rev() const { return rev_; }
	std::atomic< uint64_t > sent{ 0 }, received{ 0 };
	std::string error;

private:
	struct Owned
	{
		std::string owner;
		uint8_t saved;
	};
	BridgeClient* FindLocked( uint32_t ip, uint16_t port );
	void WriteLocked( Show& show, int u, int ch, uint8_t v, const std::string& owner );
	void ReleaseLocked( Show& show, const std::string& owner );
	void RebuildCaches( Show& show );
	void HttpLoop();
	void SendRaw( const BridgeClient& c, const std::vector< uint8_t >& msg );
	void SendBundled( const BridgeClient& c, const std::vector< std::vector< uint8_t > >& msgs );

	std::mutex mx_;//clients, ownership, caches (HTTP thread reads the caches)
	std::vector< BridgeClient > clients_;
	std::map< int64_t, Owned > owned_;//universe * 1000 + channel -> who took it and the value before
	uint32_t rev_ = 0;
	bool cacheDirty_ = false;//manifest content changed without a structure change (device state, who is online)
	struct StateIn
	{
		int seq = -1, count = 0, got = 0;
		std::vector< std::string > parts;
	};
	std::map< std::string, StateIn > stateIn_;//device id -> /ls/map/state pieces being assembled
	size_t sigHash_ = 0;
	bool sigInit_   = false;
	double lastTick_ = 0, lastPage_ = 0, lastState_ = 0;
	std::string manifestCache_, pageCache_, oscTxtCache_;
	std::string showName_ = "Unbenannt";
	//network
	std::atomic< bool > run_{ false }, ready_{ false }, failed_{ false };
	uintptr_t out_    = ~(uintptr_t)0;
	std::atomic< uintptr_t > listen_{ ~(uintptr_t)0 };
	int httpPort_     = 0;
	std::thread http_;
};

//messages the bridge handles itself (everything else goes to ApplyOsc)
bool IsBridgeMessage( const std::string& address );
}//namespace ls
