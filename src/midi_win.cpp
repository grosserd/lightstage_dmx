// MIDI port backend for Windows (winmm)
#include "midi_port.h"
#include "common.h"
#include <windows.h>
#include <mmsystem.h>

namespace ls
{
namespace midiport
{
namespace
{
struct InCtx
{
	HMIDIIN h  = nullptr;
	Sink* sink = nullptr;
	int dev    = 0;
};
void CALLBACK InProc( HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR )
{
	if( msg != MIM_DATA ) return;
	auto* c = (InCtx*)inst;
	c->sink->OnShort( c->dev, (uint32_t)p1, Now() );
}
}//namespace

std::vector< std::string > InputNames()
{
	std::vector< std::string > r;
	UINT n = midiInGetNumDevs();
	for( UINT i = 0; i < n; ++i )
	{
		MIDIINCAPSA caps{};
		midiInGetDevCapsA( i, &caps, sizeof( caps ) );
		r.push_back( caps.szPname );
	}
	return r;
}
std::vector< std::string > OutputNames()
{
	std::vector< std::string > r;
	UINT n = midiOutGetNumDevs();
	for( UINT i = 0; i < n; ++i )
	{
		MIDIOUTCAPSA c{};
		midiOutGetDevCapsA( i, &c, sizeof( c ) );
		r.push_back( c.szPname );
	}
	return r;
}

void* OpenInput( int port, int dev, Sink* sink )
{
	auto* c = new InCtx;
	c->sink = sink;
	c->dev  = dev;
	if( midiInOpen( &c->h, (UINT)port, (DWORD_PTR)InProc, (DWORD_PTR)c, CALLBACK_FUNCTION ) != MMSYSERR_NOERROR )
	{
		delete c;
		return nullptr;
	}
	midiInStart( c->h );
	return c;
}
void CloseInput( void* in )
{
	auto* c = (InCtx*)in;
	if( !c ) return;
	midiInStop( c->h );
	midiInReset( c->h );
	midiInClose( c->h );
	delete c;
}

void* OpenOutput( int port )
{
	HMIDIOUT h = nullptr;
	if( midiOutOpen( &h, (UINT)port, 0, 0, CALLBACK_NULL ) != MMSYSERR_NOERROR ) return nullptr;
	return h;
}
bool SendShort( void* out, uint8_t st, uint8_t d1, uint8_t d2 )
{
	if( !out ) return false;
	return midiOutShortMsg( (HMIDIOUT)out, st | ( d1 << 8 ) | ( d2 << 16 ) ) == MMSYSERR_NOERROR;
}
bool SendSysEx( void* out, const uint8_t* data, size_t len )
{
	if( !out || !data || len < 2 || data[ 0 ] != 0xF0 ) return false;
	std::vector< char > buf( (const char*)data, (const char*)data + len );
	MIDIHDR hdr{};
	hdr.lpData          = buf.data();
	hdr.dwBufferLength  = (DWORD)buf.size();
	hdr.dwBytesRecorded = (DWORD)buf.size();
	HMIDIOUT h = (HMIDIOUT)out;
	if( midiOutPrepareHeader( h, &hdr, sizeof( hdr ) ) != MMSYSERR_NOERROR ) return false;
	bool ok = midiOutLongMsg( h, &hdr, sizeof( hdr ) ) == MMSYSERR_NOERROR;
	//the buffer must stay valid until the driver is done with it
	for( int i = 0; ok && !( hdr.dwFlags & MHDR_DONE ) && i < 2000; ++i ) Sleep( 1 );
	if( !( hdr.dwFlags & MHDR_DONE ) ) midiOutReset( h );
	midiOutUnprepareHeader( h, &hdr, sizeof( hdr ) );
	return ok;
}
void CloseOutput( void* out )
{
	if( out ) midiOutClose( (HMIDIOUT)out );
}

void SignalWake( void* wake )
{
	if( wake ) SetEvent( (HANDLE)wake );
}
}//namespace midiport
}//namespace ls
