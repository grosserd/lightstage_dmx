// DMX I/O: Art-Net output + input (UDP 6454), Enttec DMX USB Pro (label 6) on a serial port.
// A background thread sends all output universes at a fixed rate.
#pragma once
#include "show.h"
#include <atomic>
#include <thread>

namespace ls
{
struct ComPort
{
	std::string port, name, hwid;
	bool ftdi = false;
};
std::vector< ComPort > ListComPorts();
std::vector< uint8_t > BuildArtDmx( int portAddress, uint8_t seq, const uint8_t* data, int len );
std::vector< uint8_t > BuildEnttecDmx( const uint8_t* data, int len );
bool ParseArtDmx( const uint8_t* p, int n, int& portAddress, const uint8_t*& data, int& len );
//sACN / E1.31 data packet (ANSI E1.31-2018), universe 1..63999
std::vector< uint8_t > BuildE131( int universe, uint8_t seq, int priority, const uint8_t cid[ 16 ], const std::string& source, const uint8_t* data, int len );
bool ParseE131( const uint8_t* p, int n, int& universe, int& priority, const uint8_t*& data, int& len );
std::string E131Multicast( int universe );//239.255.hi.lo

class DmxIO
{
public:
	~DmxIO() { Stop(); }
	void Start( Show* show );
	void Stop();
	void Apply();//re-read show->output (sockets / port reopen)
	//main thread: merge received Art-Net into the show (LTP per changed channel)
	void PollInput();
	//probe FTDI ports for an Enttec USB Pro, returns port name or ""
	std::string DetectEnttec( std::string* info = nullptr );

	std::atomic< uint64_t > artnetSent{ 0 }, artnetRecv{ 0 }, enttecSent{ 0 }, sacnSent{ 0 };
	std::atomic< bool > enttecOpen{ false };
	//send now (DMX changed); inputT = arrival time of the input that caused it (latency statistics)
	void Kick( double inputT = 0 );
	struct Latency { float minMs = 0, avgMs = 0, maxMs = 0; int n = 0; };
	Latency GetLatency();
	void ClearLatency();
	std::atomic< bool > boost{ false };//performance mode: output thread time critical + MMCSS
	std::string Status();
	bool acceptLoopback = false;//self-test: accept packets from our own sender
	std::vector< int > lastInputUniverses;

private:
	void OutLoop();
	void InLoop();
	bool OpenEnttec( const std::string& port );
	void CloseEnttec();
	void OpenSockets();
	void CloseSockets();
	Show* show_ = nullptr;
	std::thread out_, in_;
	std::atomic< bool > run_{ false };
	std::atomic< bool > reconfig_{ false };
	std::mutex cfgLock_, inLock_, statusLock_;
	OutputCfg cfg_;
	uintptr_t sendSock_ = ~(uintptr_t)0, recvSock_ = ~(uintptr_t)0;
	uint16_t sendPort_  = 0;
	void* com_          = nullptr;
	std::string comName_, status_;
	double nextEnttecTry_ = 0;
	uint8_t seq_          = 1;
	uint8_t sacnSeq_      = 0;
	uint8_t cid_[ 16 ]    = {};
	uintptr_t sacnSock_   = ~(uintptr_t)0;
	//input
	struct InFrame { int port; DmxFrame data; int len; bool fresh; };
	std::vector< InFrame > inFrames_;
	std::map< int, DmxFrame > lastIn_;
	void* outEvent_ = nullptr;
	std::atomic< bool > kick_{ false };
	std::mutex latLock_;
	double pendingT_ = 0;
	std::vector< float > lat_;
	double lastSendT_ = 0, lastEnttecT_ = 0;
};
}//namespace ls
