#include "osc.h"
#include "params.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstring>

namespace ls
{
//---------------------------------------------------------------- parsing
static uint32_t BE32( const uint8_t* p ) { return ( (uint32_t)p[ 0 ] << 24 ) | ( (uint32_t)p[ 1 ] << 16 ) | ( (uint32_t)p[ 2 ] << 8 ) | p[ 3 ]; }
static int Pad4( int n ) { return ( n + 3 ) & ~3; }

//OSC string: zero terminated, padded to 4 bytes; returns bytes consumed or -1
static int ReadStr( const uint8_t* p, int n, std::string& s )
{
	int k = 0;
	while( k < n && p[ k ] ) ++k;
	if( k >= n ) return -1;
	s.assign( (const char*)p, k );
	int used = Pad4( k + 1 );
	return used <= n ? used : -1;
}

static bool ParseMessage( const uint8_t* p, int n, std::vector< OscMsg >& out )
{
	OscMsg m;
	int o = ReadStr( p, n, m.address );
	if( o < 0 || m.address.empty() || m.address[ 0 ] != '/' ) return false;
	if( o == n ) { out.push_back( m ); return true; }//no type tags (old senders): no arguments
	std::string tags;
	int t = ReadStr( p + o, n - o, tags );
	if( t < 0 || tags.empty() || tags[ 0 ] != ',' ) return false;
	o += t;
	for( size_t i = 1; i < tags.size(); ++i )
	{
		char c = tags[ i ];
		if( c == 'f' || c == 'i' )
		{
			if( o + 4 > n ) return false;
			uint32_t v = BE32( p + o );
			o += 4;
			float f;
			if( c == 'f' ) memcpy( &f, &v, 4 );
			else f = (float)(int32_t)v;
			m.f.push_back( f );
			m.isInt.push_back( c == 'i' );
		}
		else if( c == 'd' || c == 'h' )
		{
			if( o + 8 > n ) return false;
			uint64_t v = ( (uint64_t)BE32( p + o ) << 32 ) | BE32( p + o + 4 );
			o += 8;
			double d;
			if( c == 'd' ) memcpy( &d, &v, 8 );
			else d = (double)(int64_t)v;
			m.f.push_back( (float)d );
			m.isInt.push_back( c == 'h' );
		}
		else if( c == 'T' || c == 'F' ) { m.f.push_back( c == 'T' ? 1.0f : 0.0f ); m.isInt.push_back( false ); }
		else if( c == 'N' || c == 'I' ) {}
		else if( c == 's' || c == 'S' )
		{
			std::string s;
			int k = ReadStr( p + o, n - o, s );
			if( k < 0 ) return false;
			m.strs.push_back( s );
			o += k;
		}
		else if( c == 'b' )
		{
			if( o + 4 > n ) return false;
			int len = (int)BE32( p + o );
			o += 4 + Pad4( len );
			if( o > n ) return false;
		}
		else
			return false;//unknown type: we cannot skip it safely
	}
	out.push_back( m );
	return true;
}

static bool ParsePacket( const uint8_t* p, int n, std::vector< OscMsg >& out, int depth )
{
	if( n < 4 || depth > 8 ) return false;
	if( n >= 16 && memcmp( p, "#bundle", 8 ) == 0 )
	{
		int o = 16;//"#bundle\0" + 8 byte time tag (ignored: applied immediately)
		bool any = false;
		while( o + 4 <= n )
		{
			int len = (int)BE32( p + o );
			o += 4;
			if( len <= 0 || o + len > n ) return any;
			any |= ParsePacket( p + o, len, out, depth + 1 );
			o += len;
		}
		return any;
	}
	return ParseMessage( p, n, out );
}

bool ParseOsc( const uint8_t* p, int n, std::vector< OscMsg >& out )
{
	return ParsePacket( p, n, out, 0 );
}

std::vector< uint8_t > BuildOsc( const std::string& address, const std::vector< float >& args, bool asInt )
{
	std::vector< uint8_t > b;
	auto str = [ & ]( const std::string& s ) {
		b.insert( b.end(), s.begin(), s.end() );
		b.push_back( 0 );
		while( b.size() & 3 ) b.push_back( 0 );
	};
	str( address );
	str( "," + std::string( args.size(), asInt ? 'i' : 'f' ) );
	for( float f : args )
	{
		uint32_t v;
		if( asInt ) v = (uint32_t)(int32_t)lroundf( f );
		else memcpy( &v, &f, 4 );
		b.push_back( v >> 24 ), b.push_back( ( v >> 16 ) & 255 ), b.push_back( ( v >> 8 ) & 255 ), b.push_back( v & 255 );
	}
	return b;
}

//---------------------------------------------------------------- receiver
bool OscIn::Start( int port )
{
	Stop();
	error.clear();
	WSADATA wd;
	WSAStartup( MAKEWORD( 2, 2 ), &wd );
	SOCKET s = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	if( s == INVALID_SOCKET ) { error = L( "kein Socket" ); return false; }
	sockaddr_in a{};
	a.sin_family      = AF_INET;
	a.sin_port        = htons( (u_short)port );
	a.sin_addr.s_addr = htonl( INADDR_ANY );
	if( bind( s, (sockaddr*)&a, sizeof( a ) ) != 0 )
	{
		closesocket( s );
		error = "Port " + std::to_string( port ) + L( " ist belegt" );
		return false;
	}
	DWORD tmo = 200;//so the thread notices Stop()
	setsockopt( s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof( tmo ) );
	int rb = 1 << 20;
	setsockopt( s, SOL_SOCKET, SO_RCVBUF, (const char*)&rb, sizeof( rb ) );
	sock_ = (uintptr_t)s;
	port_ = port;
	run_  = true;
	th_   = std::thread( [ this ] { Loop(); } );
	Log( "osc in: listening on UDP %d", port );
	return true;
}

void OscIn::Stop()
{
	if( !run_ && sock_ == ~(uintptr_t)0 ) return;
	run_ = false;
	if( th_.joinable() ) th_.join();
	if( sock_ != ~(uintptr_t)0 ) closesocket( (SOCKET)sock_ );
	sock_ = ~(uintptr_t)0;
	port_ = 0;
}

void OscIn::Loop()
{
	std::vector< uint8_t > buf( 65536 );
	while( run_ )
	{
		sockaddr_in from{};
		int fromLen = sizeof( from );
		int n       = recvfrom( (SOCKET)sock_, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fromLen );
		if( n <= 0 ) continue;
		std::vector< OscMsg > ms;
		if( !ParseOsc( buf.data(), n, ms ) ) { ++bad; continue; }
		for( auto& m : ms )
		{
			m.fromIp   = from.sin_addr.s_addr;
			m.fromPort = ntohs( from.sin_port );
		}
		std::lock_guard< std::mutex > l( lock_ );
		for( auto& m : ms )
			if( queue_.size() < 16384 ) queue_.push_back( std::move( m ) );
		if( wake ) SetEvent( (HANDLE)wake );
	}
}

void OscIn::Poll( std::vector< OscMsg >& out )
{
	std::vector< OscMsg > q;
	{
		std::lock_guard< std::mutex > l( lock_ );
		q.swap( queue_ );
	}
	for( auto& m : q )
	{
		++received;
		std::string line = m.address;
		for( size_t i = 0; i < m.f.size() && i < 4; ++i )
		{
			char b[ 24 ];
			snprintf( b, sizeof( b ), m.isInt[ i ] ? " %.0f" : " %.3f", m.f[ i ] );
			line += b;
		}
		monitor.push_back( line );
		while( monitor.size() > 200 ) monitor.pop_front();
		out.push_back( std::move( m ) );
	}
}

//---------------------------------------------------------------- applying
static std::vector< std::string > Split( const std::string& a )
{
	std::vector< std::string > r;
	size_t i = 0;
	while( i < a.size() )
	{
		size_t j = a.find( '/', i );
		if( j == std::string::npos ) j = a.size();
		if( j > i ) r.push_back( a.substr( i, j - i ) );
		i = j + 1;
	}
	return r;
}
static std::string Key( const std::string& s )
{
	std::string r;
	for( char c : ToLower( s ) )
		if( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) ) r += c;
	return r;
}
static Attr AttrByName( const std::string& s )
{
	const std::string k = Key( s );
	for( int i = 1; i < (int)Attr::Count; ++i )
		if( Key( AttrName( (Attr)i ) ) == k ) return (Attr)i;
	//common short forms
	if( k == "dim" || k == "intensity" ) return Attr::Dimmer;
	if( k == "strobe" ) return Attr::Shutter;
	if( k == "r" ) return Attr::Red;
	if( k == "g" ) return Attr::Green;
	if( k == "b" ) return Attr::Blue;
	if( k == "w" ) return Attr::White;
	return Attr::None;
}
static bool ToInt( const std::string& s, int& v )
{
	if( s.empty() ) return false;
	char* e = nullptr;
	long x  = strtol( s.c_str(), &e, 10 );
	if( *e ) return false;
	v = (int)x;
	return true;
}
//argument k as 0..1 (ints are DMX 0..255)
static float Arg01( const OscMsg& m, size_t k )
{
	if( k >= m.f.size() ) return 1.0f;//no argument (e.g. a button): full
	return m.isInt[ k ] ? Saturate( m.f[ k ] / 255.0f ) : Saturate( m.f[ k ] );
}

bool ApplyOsc( Show& show, const OscMsg& m )
{
	auto p = Split( m.address );
	if( p.size() < 2 || Key( p[ 0 ] ) != "ls" ) return false;
	const std::string what = Key( p[ 1 ] );
	if( what == "master" && p.size() == 2 ) { show.master = Arg01( m, 0 ); return true; }
	if( what == "master" && p.size() == 3 && Key( p[ 2 ] ) == "boost" )
	{
		//0..10 = 0..1000 %
		show.master = m.f.empty() ? 1.0f : Clamp( m.f[ 0 ], 0.0f, 10.0f );
		return true;
	}
	if( what == "haze" && p.size() == 2 ) { show.stage.haze = Arg01( m, 0 ); return true; }
	int snapNum = 0;
	if( what == "snapshot" && ( p.size() == 2 || ( p.size() == 3 && ToInt( p[ 2 ], snapNum ) ) ) )//"/ls/snapshot/3/master" etc.: register
	{
		int n = 0;
		if( p.size() >= 3 ) ToInt( p[ 2 ], n );
		else if( !m.f.empty() ) n = (int)lroundf( m.f[ 0 ] );
		if( n < 1 ) return false;
		if( p.size() >= 3 && !m.f.empty() && m.f[ 0 ] <= 0.5f ) return true;//button release
		show.RecallSnapshot( n - 1 );
		return true;
	}
	if( what == "dmx" && p.size() == 4 )
	{
		int u = 0, c = 0;
		if( !ToInt( p[ 2 ], u ) || !ToInt( p[ 3 ], c ) || c < 1 || c > 512 || m.f.empty() ) return false;
		std::lock_guard< std::mutex > l( show.dmxLock );
		DmxFrame* fr = show.Frame( u );
		if( !fr ) return false;
		( *fr )[ c - 1 ] = (uint8_t)( m.isInt[ 0 ] ? Clamp( (int)lroundf( m.f[ 0 ] ), 0, 255 ) : (int)( Saturate( m.f[ 0 ] ) * 255 + 0.5f ) );
		show.Bump();
		return true;
	}
	if( what == "fixture" && p.size() >= 4 )
	{
		int id = 0;
		if( !ToInt( p[ 2 ], id ) ) return false;
		FixtureInst* f = show.FindFixture( id );
		if( !f ) return false;
		int head = -1;
		if( p.size() >= 5 && ToInt( p[ 4 ], head ) ) head -= 1;
		if( Key( p[ 3 ] ) == "color" || Key( p[ 3 ] ) == "rgb" )
		{
			if( m.f.size() < 3 ) return false;
			show.SetAttr( *f, Attr::Red, Arg01( m, 0 ), head );
			show.SetAttr( *f, Attr::Green, Arg01( m, 1 ), head );
			show.SetAttr( *f, Attr::Blue, Arg01( m, 2 ), head );
			return true;
		}
		Attr a = AttrByName( p[ 3 ] );
		if( a != Attr::None )
		{
			show.SetAttr( *f, a, Arg01( m, 0 ), head );
			return true;
		}
	}
	//everything else: parameter register (stage, effects, tempo, projectors, ...), value in the parameter's unit
	return SetParam( show, m.address, m.f.empty() ? 1.0f : m.f[ 0 ] );
}
}//namespace ls
