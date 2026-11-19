// MIDI port backend for macOS (CoreMIDI)
// Input: MIDIInputPortCreate + MIDIReadProc (MIDI 1.0 byte stream, parsed here: running status, real time, SysEx),
// one input port per source. Output: one shared MIDIOutputPortCreate port + MIDISend with a MIDIPacketList.
// The device list is read again on every request (InputNames / OutputNames, i.e. MidiIn::Open / Rescan) - like Windows,
// there is no hot-plug notification.
#ifdef __APPLE__
#include "midi_port.h"
#include "common.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mutex>

namespace ls
{
namespace midiport
{
namespace
{
//one client per process, never disposed (Apple's recommendation)
MIDIClientRef Client()
{
	static MIDIClientRef client = 0;
	static std::once_flag once;
	std::call_once( once, [] {
		// MACOS-UNTESTED: no notify proc; the endpoint list is assumed to be current without a run loop on this thread
		OSStatus st = MIDIClientCreate( CFSTR( "LightStage" ), nullptr, nullptr, &client );
		if( st != noErr ) { Log( "midi: MIDIClientCreate failed (%d)", (int)st ); client = 0; }
	} );
	return client;
}
MIDIPortRef OutPort()
{
	static MIDIPortRef port = 0;
	static std::once_flag once;
	std::call_once( once, [] {
		if( !Client() ) return;
		OSStatus st = MIDIOutputPortCreate( Client(), CFSTR( "LightStage Out" ), &port );
		if( st != noErr ) { Log( "midi: MIDIOutputPortCreate failed (%d)", (int)st ); port = 0; }
	} );
	return port;
}

std::string EndpointName( MIDIEndpointRef ep )
{
	CFStringRef s = nullptr;
	if( MIDIObjectGetStringProperty( ep, kMIDIPropertyDisplayName, &s ) != noErr || !s )
	{
		s = nullptr;
		if( MIDIObjectGetStringProperty( ep, kMIDIPropertyName, &s ) != noErr ) s = nullptr;
	}
	if( !s ) return "?";
	char buf[ 256 ];
	std::string r = CFStringGetCString( s, buf, sizeof( buf ), kCFStringEncodingUTF8 ) ? buf : "?";
	CFRelease( s );
	return r;
}

//number of data bytes after a status byte (0xF0 / 0xF7 handled separately)
int DataBytes( uint8_t st )
{
	if( st < 0xF0 )
	{
		const uint8_t ty = st & 0xF0;
		return ty == 0xC0 || ty == 0xD0 ? 1 : 2;
	}
	return st == 0xF1 || st == 0xF3 ? 1 : ( st == 0xF2 ? 2 : 0 );
}

//MIDI 1.0 byte stream -> complete messages, state kept across packets (messages may be split)
struct Parser
{
	static constexpr size_t kMaxSysEx = 65536;
	uint8_t cur = 0;//current status (channel status stays for running status; system common is cleared when complete)
	uint8_t data[ 2 ] = {};
	int have = 0, need = 0;
	bool inSysEx = false;
	std::vector< uint8_t > sx;

	void Feed( const uint8_t* p, size_t n, Sink* sink, int dev, double t )
	{
		for( size_t i = 0; i < n; ++i )
		{
			const uint8_t b = p[ i ];
			if( b >= 0xF8 ) { sink->OnShort( dev, b, t ); continue; }//real time: may appear anywhere, keeps running status
			if( b == 0xF0 )
			{
				inSysEx = true;
				sx.assign( 1, b );
				cur = 0;
				have = need = 0;
				continue;
			}
			if( b == 0xF7 )
			{
				if( inSysEx )
				{
					sx.push_back( b );
					sink->OnSysEx( dev, sx.data(), sx.size(), t );
				}
				inSysEx = false;
				sx.clear();
				continue;
			}
			if( b & 0x80 )//any other status ends an unterminated SysEx (dropped)
			{
				inSysEx = false;
				sx.clear();
				have = 0;
				need = DataBytes( b );
				cur  = b;
				if( need == 0 ) { sink->OnShort( dev, b, t ); cur = 0; }//F4 / F5 / F6
				continue;
			}
			if( inSysEx )
			{
				if( sx.size() < kMaxSysEx ) sx.push_back( b );
				else { inSysEx = false; sx.clear(); }//too long: drop
				continue;
			}
			if( !cur ) continue;//data without status
			data[ have++ ] = b;
			if( have < need ) continue;
			sink->OnShort( dev, cur | ( data[ 0 ] << 8 ) | ( need > 1 ? data[ 1 ] << 16 : 0 ), t );
			have = 0;
			if( cur >= 0xF0 ) cur = 0;//system common clears running status
		}
	}
};

struct InCtx
{
	MIDIPortRef port    = 0;
	MIDIEndpointRef src = 0;
	Sink* sink          = nullptr;
	int dev             = 0;
	Parser parser;//only touched on the CoreMIDI read thread
};

void ReadProc( const MIDIPacketList* pl, void* /*readProcRefCon*/, void* srcConnRefCon )
{
	auto* c = (InCtx*)srcConnRefCon;
	if( !c || !pl ) return;
	const double t = Now();//arrival time like the winmm callback (packet host time stamps are not used)
	const MIDIPacket* p = &pl->packet[ 0 ];
	for( UInt32 i = 0; i < pl->numPackets; ++i )
	{
		c->parser.Feed( p->data, p->length, c->sink, c->dev, t );
		p = MIDIPacketNext( p );
	}
}

bool Send( MIDIEndpointRef dst, const uint8_t* bytes, size_t len )
{
	MIDIPortRef port = OutPort();
	if( !port || !dst || !len ) return false;
	//packet list in 256-byte chunks; uint32_t storage keeps the 4-byte alignment arm64 packets need
	const size_t chunk = 256, bytesNeeded = 64 + len + ( len / chunk + 1 ) * 32;
	std::vector< uint32_t > buf( bytesNeeded / 4 + 1 );
	auto* pl       = (MIDIPacketList*)buf.data();
	MIDIPacket* pk = MIDIPacketListInit( pl );
	for( size_t off = 0; off < len && pk; off += chunk )
	{
		const size_t n = std::min( chunk, len - off );
		pk             = MIDIPacketListAdd( pl, buf.size() * 4, pk, 0, (ByteCount)n, bytes + off );
	}
	if( !pk ) return false;
	// MACOS-UNTESTED: large SysEx in one MIDISend (several packets); MIDISendSysex would be the async alternative
	return MIDISend( port, dst, pl ) == noErr;
}
}//namespace

std::vector< std::string > InputNames()
{
	std::vector< std::string > r;
	if( !Client() ) return r;
	const ItemCount n = MIDIGetNumberOfSources();
	for( ItemCount i = 0; i < n; ++i ) r.push_back( EndpointName( MIDIGetSource( i ) ) );
	return r;
}
std::vector< std::string > OutputNames()
{
	std::vector< std::string > r;
	if( !Client() ) return r;
	const ItemCount n = MIDIGetNumberOfDestinations();
	for( ItemCount i = 0; i < n; ++i ) r.push_back( EndpointName( MIDIGetDestination( i ) ) );
	return r;
}

void* OpenInput( int port, int dev, Sink* sink )
{
	if( !Client() || port < 0 || port >= (int)MIDIGetNumberOfSources() ) return nullptr;
	auto* c = new InCtx;
	c->src  = MIDIGetSource( (ItemCount)port );
	c->sink = sink;
	c->dev  = dev;
	if( !c->src || MIDIInputPortCreate( Client(), CFSTR( "LightStage In" ), ReadProc, nullptr, &c->port ) != noErr )
	{
		delete c;
		return nullptr;
	}
	if( MIDIPortConnectSource( c->port, c->src, c ) != noErr )
	{
		MIDIPortDispose( c->port );
		delete c;
		return nullptr;
	}
	return c;
}
void CloseInput( void* in )
{
	auto* c = (InCtx*)in;
	if( !c ) return;
	MIDIPortDisconnectSource( c->port, c->src );
	// MACOS-UNTESTED: assumes no ReadProc call is running or follows once MIDIPortDispose has returned
	MIDIPortDispose( c->port );
	delete c;
}

struct OutCtx
{
	MIDIEndpointRef dst = 0;
};
void* OpenOutput( int port )
{
	if( !Client() || port < 0 || port >= (int)MIDIGetNumberOfDestinations() || !OutPort() ) return nullptr;
	MIDIEndpointRef dst = MIDIGetDestination( (ItemCount)port );
	if( !dst ) return nullptr;
	return new OutCtx{ dst };
}
bool SendShort( void* out, uint8_t st, uint8_t d1, uint8_t d2 )
{
	if( !out || !( st & 0x80 ) || st == 0xF0 || st == 0xF7 ) return false;
	const uint8_t b[ 3 ] = { st, (uint8_t)( d1 & 0x7F ), (uint8_t)( d2 & 0x7F ) };
	return Send( ( (OutCtx*)out )->dst, b, 1 + (size_t)DataBytes( st ) );
}
bool SendSysEx( void* out, const uint8_t* data, size_t len )
{
	if( !out || !data || len < 2 || data[ 0 ] != 0xF0 ) return false;
	return Send( ( (OutCtx*)out )->dst, data, len );
}
void CloseOutput( void* out )
{
	delete (OutCtx*)out;
}

void SignalWake( void* )
{
	// MACOS-UNTESTED: no portable event here; set MidiIn::wakeFn to signal the engine (it also polls at 1 kHz)
}
}//namespace midiport
}//namespace ls
#endif//__APPLE__
