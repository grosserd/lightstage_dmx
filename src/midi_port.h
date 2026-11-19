// Portable MIDI port backend: device lists, input/output ports (winmm on Windows: midi_win.cpp, CoreMIDI on macOS: midi_mac.cpp)
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ls
{
namespace midiport
{
//receiver of incoming messages; called on the backend's thread (winmm callback / CoreMIDI read thread)
struct Sink
{
	virtual ~Sink() = default;
	//complete short message packed like winmm MIM_DATA: status | d1 << 8 | d2 << 16 (running status already expanded)
	virtual void OnShort( int dev, uint32_t msg, double t ) = 0;
	//complete SysEx message F0 .. F7 (Windows: not delivered, no long buffers are posted - unchanged behaviour)
	virtual void OnSysEx( int dev, const uint8_t* data, size_t len, double t ) = 0;
};

//port names in system order (the index is the port number for OpenInput / OpenOutput)
std::vector< std::string > InputNames();
std::vector< std::string > OutputNames();

//opens and starts input port `port`; `dev` is passed back to the sink; nullptr on failure (e.g. in use)
void* OpenInput( int port, int dev, Sink* sink );
void CloseInput( void* in );//stops; no sink call after it returns

void* OpenOutput( int port );//nullptr on failure
bool SendShort( void* out, uint8_t status, uint8_t d1, uint8_t d2 );
bool SendSysEx( void* out, const uint8_t* data, size_t len );//data = F0 .. F7
void CloseOutput( void* out );

//default wake-up of the engine thread for MidiIn::wake (Windows: SetEvent on the HANDLE)
void SignalWake( void* wake );
}//namespace midiport
}//namespace ls
