#include "dmxio.h"
#include "netsock.h"
#include "serial.h"
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#else
#include <pthread.h>
#endif
#include <chrono>
#include <set>
#include <cmath>
#include <random>

namespace ls
{
//---------------------------------------------------------------- packets
std::vector< uint8_t > BuildArtDmx( int port, uint8_t seq, const uint8_t* data, int len )
{
	len = Clamp( len, 2, 512 );
	if( len & 1 ) ++len;
	std::vector< uint8_t > p( 18 + len, 0 );
	memcpy( p.data(), "Art-Net", 8 );
	p[ 8 ]  = 0x00;//OpDmx 0x5000 little endian
	p[ 9 ]  = 0x50;
	p[ 10 ] = 0;//ProtVer hi
	p[ 11 ] = 14;
	p[ 12 ] = seq;
	p[ 13 ] = 0;
	p[ 14 ] = (uint8_t)( port & 0xff );       //SubUni
	p[ 15 ] = (uint8_t)( ( port >> 8 ) & 0x7f );//Net
	p[ 16 ] = (uint8_t)( len >> 8 );
	p[ 17 ] = (uint8_t)( len & 0xff );
	memcpy( p.data() + 18, data, std::min( len, 512 ) );
	return p;
}
std::vector< uint8_t > BuildE131( int universe, uint8_t seq, int priority, const uint8_t cid[ 16 ], const std::string& source, const uint8_t* data, int len )
{
	len = Clamp( len, 0, 512 );
	std::vector< uint8_t > p( 126 + len, 0 );
	auto be16 = [ & ]( int at, int v ) { p[ at ] = (uint8_t)( v >> 8 ); p[ at + 1 ] = (uint8_t)v; };
	auto be32 = [ & ]( int at, uint32_t v ) { for( int i = 0; i < 4; ++i ) p[ at + i ] = (uint8_t)( v >> ( 24 - 8 * i ) ); };
	const int n = (int)p.size();
	//root layer
	be16( 0, 0x0010 );
	be16( 2, 0 );
	memcpy( &p[ 4 ], "ASC-E1.17\0\0\0", 12 );
	be16( 16, 0x7000 | ( n - 16 ) );
	be32( 18, 0x00000004 );//VECTOR_ROOT_E131_DATA
	memcpy( &p[ 22 ], cid, 16 );
	//framing layer
	be16( 38, 0x7000 | ( n - 38 ) );
	be32( 40, 0x00000002 );//VECTOR_E131_DATA_PACKET
	memcpy( &p[ 44 ], source.c_str(), std::min< size_t >( source.size(), 63 ) );
	p[ 108 ] = (uint8_t)Clamp( priority, 0, 200 );
	be16( 109, 0 );//sync address
	p[ 111 ] = seq;
	p[ 112 ] = 0;//options
	be16( 113, Clamp( universe, 1, 63999 ) );
	//DMP layer
	be16( 115, 0x7000 | ( n - 115 ) );
	p[ 117 ] = 0x02;//VECTOR_DMP_SET_PROPERTY
	p[ 118 ] = 0xa1;
	be16( 119, 0 );
	be16( 121, 1 );
	be16( 123, len + 1 );
	p[ 125 ] = 0;//start code
	if( len ) memcpy( &p[ 126 ], data, len );
	return p;
}
bool ParseE131( const uint8_t* p, int n, int& universe, int& priority, const uint8_t*& data, int& len )
{
	if( n < 126 || memcmp( p + 4, "ASC-E1.17", 9 ) != 0 ) return false;
	if( p[ 21 ] != 4 || p[ 43 ] != 2 || p[ 117 ] != 2 || p[ 125 ] != 0 ) return false;
	universe = ( p[ 113 ] << 8 ) | p[ 114 ];
	priority = p[ 108 ];
	len      = std::min( ( ( p[ 123 ] << 8 ) | p[ 124 ] ) - 1, n - 126 );
	data     = p + 126;
	return len >= 0;
}
std::string E131Multicast( int universe )
{
	universe = Clamp( universe, 1, 63999 );
	return "239.255." + std::to_string( universe >> 8 ) + "." + std::to_string( universe & 255 );
}
bool ParseArtDmx( const uint8_t* p, int n, int& port, const uint8_t*& data, int& len )
{
	if( n < 20 || memcmp( p, "Art-Net", 8 ) != 0 ) return false;
	if( p[ 8 ] != 0x00 || p[ 9 ] != 0x50 ) return false;
	port = p[ 14 ] | ( ( p[ 15 ] & 0x7f ) << 8 );
	len  = ( p[ 16 ] << 8 ) | p[ 17 ];
	len  = std::min( len, n - 18 );
	len  = std::min( len, 512 );
	data = p + 18;
	return len > 0;
}
std::vector< uint8_t > BuildEnttecDmx( const uint8_t* data, int len )
{
	len = Clamp( len, 24, 512 );
	std::vector< uint8_t > f( len + 6 );
	f[ 0 ] = 0x7E;
	f[ 1 ] = 6;//Output Only Send DMX Packet Request
	f[ 2 ] = (uint8_t)( ( len + 1 ) & 0xff );
	f[ 3 ] = (uint8_t)( ( len + 1 ) >> 8 );
	f[ 4 ] = 0;//start code
	memcpy( f.data() + 5, data, len );
	f[ len + 5 ] = 0xE7;
	return f;
}
static std::vector< uint8_t > ArtPollReply( uint32_t ip, int ports )
{
	std::vector< uint8_t > p( 239, 0 );
	memcpy( p.data(), "Art-Net", 8 );
	p[ 8 ] = 0x00;
	p[ 9 ] = 0x21;//OpPollReply
	memcpy( &p[ 10 ], &ip, 4 );
	p[ 14 ] = 0x36;//port 6454 LE
	p[ 15 ] = 0x19;
	p[ 16 ] = 1;//version
	p[ 23 ] = 0xD0;//status1
	strcpy( (char*)&p[ 26 ], "LightStage" );
	strcpy( (char*)&p[ 44 ], "Grosserd LightStage DMX Visualizer" );
	p[ 173 ] = 0;
	p[ 174 ] = (uint8_t)std::min( ports, 4 );
	for( int i = 0; i < 4; ++i )
	{
		p[ 174 + 1 + i ] = 0xC0;//can output + input, DMX
		p[ 186 + i ]     = (uint8_t)i;//SwIn
		p[ 190 + i ]     = (uint8_t)i;//SwOut
	}
	p[ 200 ] = 0;//style node
	return p;
}

//---------------------------------------------------------------- Enttec (serial port: serial_win.cpp / serial_posix.cpp)
//request widget parameters (label 3); a USB Pro answers with label 3
static bool ProbeEnttec( void* h, std::string* info )
{
	const uint8_t req[] = { 0x7E, 3, 2, 0, 0, 0, 0xE7 };
	if( !serial::Write( h, req, sizeof( req ) ) ) return false;
	uint8_t buf[ 64 ];
	int got = 0, total = 0;
	double t0 = Now();
	while( Now() - t0 < 0.4 && total < (int)sizeof( buf ) )
	{
		got = serial::Read( h, buf + total, (int)sizeof( buf ) - total );
		if( got < 0 ) break;
		total += got;
		if( total >= 10 ) break;
		if( got == 0 && total > 0 ) break;
	}
	for( int i = 0; i + 4 < total; ++i )
		if( buf[ i ] == 0x7E && buf[ i + 1 ] == 3 )
		{
			if( info && i + 9 < total )
			{
				char b[ 96 ];
				snprintf( b, sizeof( b ), "firmware %d.%d, refresh %d Hz", buf[ i + 5 ], buf[ i + 4 ], buf[ i + 8 ] );
				*info = b;
			}
			return true;
		}
	return false;
}

std::string DmxIO::DetectEnttec( std::string* info )
{
	for( auto& p : ListComPorts() )
	{
		if( !p.ftdi ) continue;
		if( p.port == comName_ && com_ ) return p.port;//already ours
		void* h = serial::Open( p.port );
		if( !h ) continue;
		bool ok = ProbeEnttec( h, info );
		serial::Close( h );
		if( ok ) return p.port;
	}
	return "";
}

bool DmxIO::OpenEnttec( const std::string& port )
{
	CloseEnttec();
	std::string p = port;
	if( p.empty() || p == "auto" )
	{
		p = DetectEnttec();
		if( p.empty() )
		{
			std::lock_guard< std::mutex > l( statusLock_ );
			status_ = L( "USB-DMX: kein Interface gefunden" );
			return false;
		}
	}
	void* h = serial::Open( p );
	if( !h )
	{
		std::lock_guard< std::mutex > l( statusLock_ );
		status_ = "USB-DMX: " + p + L( " nicht verfügbar" );
		return false;
	}
	com_     = h;
	comName_ = p;
	enttecOpen = true;
	std::lock_guard< std::mutex > l( statusLock_ );
	status_ = "USB-DMX: " + p + L( " offen" );
	Log( "enttec opened on %s", p.c_str() );
	return true;
}
void DmxIO::CloseEnttec()
{
	if( com_ ) serial::Close( com_ );
	com_       = nullptr;
	enttecOpen = false;
}

//---------------------------------------------------------------- sockets
void DmxIO::OpenSockets()
{
	CloseSockets();
	net::Sock s = net::Udp();
	if( s != net::kInvalid )
	{
		net::SetBroadcast( s );
		sockaddr_in a{};
		net::Addr( cfg_.artnetBind.empty() ? "0.0.0.0" : cfg_.artnetBind.c_str(), 0, a );
		net::Bind( s, a );
		net::LocalAddr( s, a );
		sendPort_ = ntohs( a.sin_port );
		sendSock_ = net::ToU( s );
	}
	if( cfg_.sacn )
	{
		net::Sock m = net::Udp();
		if( m != net::kInvalid )
		{
			sockaddr_in a{};
			net::Addr( cfg_.artnetBind.empty() ? "0.0.0.0" : cfg_.artnetBind.c_str(), 0, a );
			net::Bind( m, a );
			net::SetMulticastTtl( m, 8 );
			if( a.sin_addr.s_addr != INADDR_ANY ) net::SetMulticastIf( m, a.sin_addr );
			net::SetMulticastLoop( m, acceptLoopback );
			sacnSock_ = net::ToU( m );
		}
		if( !cid_[ 0 ] && !cid_[ 15 ] )
		{
			std::random_device rd;
			for( auto& b : cid_ ) b = (uint8_t)rd();
			cid_[ 6 ] = ( cid_[ 6 ] & 0x0f ) | 0x40;//UUID v4
			cid_[ 8 ] = ( cid_[ 8 ] & 0x3f ) | 0x80;
		}
	}
	if( cfg_.artnetInput )
	{
		net::Sock r = net::Udp();
		if( r != net::kInvalid )
		{
			net::SetReuse( r );//SO_REUSEADDR (+ SO_REUSEPORT on macOS): share 6454 with other Art-Net programs
			net::SetBroadcast( r );
			net::SetRecvTimeout( r, 200 );
			if( net::Bind( r, net::Addr( htonl( INADDR_ANY ), 6454 ) ) )
				recvSock_ = net::ToU( r );
			else
			{
				Log( "artnet: bind 6454 failed (%d)", net::LastError() );
				net::Close( r );
				std::lock_guard< std::mutex > l( statusLock_ );
				status_ = L( "Art-Net Eingang: Port 6454 belegt" );
			}
		}
	}
}
void DmxIO::CloseSockets()
{
	net::Close( net::FromU( sendSock_ ) );
	net::Close( net::FromU( recvSock_ ) );
	net::Close( net::FromU( sacnSock_ ) );
	sendSock_ = recvSock_ = sacnSock_ = ~(uintptr_t)0;
}

void DmxIO::Start( Show* show )
{
	Stop();
	net::Startup();
	show_ = show;
	{
		std::lock_guard< std::mutex > l( cfgLock_ );
		cfg_ = show->output;
	}
	OpenSockets();
	if( !outEvent_ ) outEvent_ = EventCreate();
	run_ = true;
	out_ = std::thread( [ this ] { OutLoop(); } );
	in_  = std::thread( [ this ] { InLoop(); } );
}
void DmxIO::Stop()
{
	if( !run_ ) return;
	run_ = false;
	if( outEvent_ ) EventSignal( outEvent_ );//wake the output thread at once (it may wait for the next refresh)
	if( out_.joinable() ) out_.join();
	if( in_.joinable() ) in_.join();
	CloseSockets();
	CloseEnttec();
}
void DmxIO::Kick( double inputT )
{
	if( inputT > 0 )
	{
		std::lock_guard< std::mutex > l( latLock_ );
		if( pendingT_ <= 0 || inputT < pendingT_ ) pendingT_ = inputT;
	}
	kick_ = true;
	if( outEvent_ ) EventSignal( outEvent_ );
}
DmxIO::Latency DmxIO::GetLatency()
{
	std::lock_guard< std::mutex > l( latLock_ );
	Latency r;
	r.n = (int)lat_.size();
	if( !r.n ) return r;
	r.minMs = 1e9f;
	double sum = 0;
	for( float v : lat_ )
	{
		r.minMs = std::min( r.minMs, v );
		r.maxMs = std::max( r.maxMs, v );
		sum += v;
	}
	r.avgMs = (float)( sum / r.n );
	return r;
}
void DmxIO::ClearLatency()
{
	std::lock_guard< std::mutex > l( latLock_ );
	lat_.clear();
	pendingT_ = 0;
}
void DmxIO::Apply()
{
	std::lock_guard< std::mutex > l( cfgLock_ );
	cfg_      = show_->output;
	reconfig_ = true;
	if( outEvent_ ) EventSignal( outEvent_ );//wake the output thread: new rate / targets apply at once
}

std::string DmxIO::Status()
{
	std::lock_guard< std::mutex > l( statusLock_ );
	return status_;
}

void DmxIO::OutLoop()
{
#ifdef _WIN32
	timeBeginPeriod( 1 );
	HANDLE mmcss  = nullptr;
#endif
	double next = Now();
	bool lastEnttecWanted = false;
	std::string lastPort;
	bool boosted  = false;
	while( run_ )
	{
		if( boost != boosted )
		{
			boosted = boost;
#ifdef _WIN32
			if( boosted )
			{
				DWORD idx = 0;
				SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL );
				mmcss = AvSetMmThreadCharacteristicsW( L"Pro Audio", &idx );
			}
			else
			{
				if( mmcss ) AvRevertMmThreadCharacteristics( mmcss );
				mmcss = nullptr;
				SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_NORMAL );
			}
#elif defined( __APPLE__ )
			//MACOS-UNTESTED: performance mode = highest QoS class (no MMCSS on macOS)
			pthread_set_qos_class_self_np( boosted ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_DEFAULT, 0 );
#endif
		}
		OutputCfg cfg;
		{
			std::lock_guard< std::mutex > l( cfgLock_ );
			cfg = cfg_;
		}
		if( reconfig_.exchange( false ) )
		{
			//sockets are shared with the input thread: briefly stop using them
			std::lock_guard< std::mutex > l( inLock_ );
			OpenSockets();
			if( !cfg.enttec || cfg.enttecPort != lastPort ) CloseEnttec();
		}
		//enttec (re)connect
		if( cfg.enttec && !com_ && Now() > nextEnttecTry_ )
		{
			if( !OpenEnttec( cfg.enttecPort ) ) nextEnttecTry_ = Now() + 3.0;
			lastPort = cfg.enttecPort;
		}
		if( !cfg.enttec && lastEnttecWanted )
		{
			CloseEnttec();
			std::lock_guard< std::mutex > l( statusLock_ );
			status_.clear();
		}
		lastEnttecWanted = cfg.enttec;
		//snapshot of the buffers
		std::vector< std::pair< Universe, DmxFrame > > frames;
		double inputT = 0;//input contained in this frame (latency)
		{
			std::lock_guard< std::mutex > l( latLock_ );
			inputT    = pendingT_;
			pendingT_ = 0;
		}
		kick_ = false;
		{
			std::lock_guard< std::mutex > l( show_->dmxLock );
			for( size_t i = 0; i < show_->universes.size(); ++i )
			{
				frames.push_back( { show_->universes[ i ], show_->dmx[ i ] } );
				//effects on top of the base values
				if( show_->fxAny && i < show_->fxOut.size() )
				{
					const auto& o = show_->fxOut[ i ];
					auto& d       = frames.back().second;
					for( int c = 0; c < 512; ++c )
						if( o[ c ] >= 0 ) d[ c ] = (uint8_t)o[ c ];
				}
			}
		}
		if( cfg.artnet && sendSock_ != ~(uintptr_t)0 )
		{
			sockaddr_in to{};
			if( net::Addr( cfg.artnetTarget.c_str(), 6454, to ) )
			{
				for( auto& fr : frames )
				{
					if( !fr.first.artnetOut ) continue;
					auto pkt = BuildArtDmx( fr.first.artnetPort, seq_, fr.second.data(), 512 );
					if( net::SendTo( net::FromU( sendSock_ ), pkt.data(), (int)pkt.size(), to ) > 0 )
						++artnetSent;
				}
				seq_ = (uint8_t)( seq_ % 255 + 1 );
			}
		}
		if( cfg.sacn && sacnSock_ != ~(uintptr_t)0 )
		{
			for( auto& fr : frames )
			{
				if( !fr.first.artnetOut ) continue;//"Out" in the patch list: Art-Net and sACN alike
				sockaddr_in to{};
				const std::string ip = cfg.sacnTarget.empty() ? E131Multicast( fr.first.num ) : cfg.sacnTarget;
				if( !net::Addr( ip.c_str(), 5568, to ) ) continue;
				auto pkt = BuildE131( fr.first.num, sacnSeq_, cfg.sacnPriority, cid_, "Grosserd LightStage", fr.second.data(), 512 );
				if( net::SendTo( net::FromU( sacnSock_ ), pkt.data(), (int)pkt.size(), to ) > 0 ) ++sacnSent;
			}
			++sacnSeq_;
		}
		if( com_ && Now() - lastEnttecT_ >= 0.022 )//a full DMX frame takes ~23 ms on the wire
		{
			lastEnttecT_ = Now();
			for( auto& fr : frames )
				if( fr.first.num == cfg.enttecUniverse )
				{
					auto f = BuildEnttecDmx( fr.second.data(), 512 );
					if( serial::Write( com_, f.data(), f.size() ) )
						++enttecSent;
					else
					{
						Log( "enttec write failed, closing" );
						CloseEnttec();
						nextEnttecTry_ = Now() + 2.0;
						std::lock_guard< std::mutex > l( statusLock_ );
						status_ = L( "USB-DMX: Verbindung verloren" );
					}
					break;
				}
		}
		lastSendT_ = Now();
		if( inputT > 0 )
		{
			std::lock_guard< std::mutex > l( latLock_ );
			lat_.push_back( (float)( ( lastSendT_ - inputT ) * 1000.0 ) );
			if( lat_.size() > 200 ) lat_.erase( lat_.begin() );
		}
		next += 1.0 / Clamp( cfg.rate, 1.0f, 44.0f );
		double now = Now();
		if( next < now ) next = now;
		//wait for the refresh time - or less when the DMX changed (Kick), at most one send per 2 ms
		while( run_ )
		{
			now = Now();
			if( now >= next ) break;
			if( kick_ && now - lastSendT_ >= 0.002 ) break;
			if( reconfig_ ) { next = now; break; }
			double waitS = next - now;
			if( kick_ ) waitS = std::min( waitS, 0.002 - ( now - lastSendT_ ) );
			EventWait( outEvent_, (int)std::max( 1.0, std::ceil( waitS * 1000.0 ) ) );
		}
	}
#ifdef _WIN32
	if( mmcss ) AvRevertMmThreadCharacteristics( mmcss );
	timeEndPeriod( 1 );
#endif
}

void DmxIO::InLoop()
{
	std::set< uint32_t > localIps;
	{
		char host[ 256 ];
		if( gethostname( host, sizeof( host ) ) == 0 )
		{
			addrinfo hints{}, *res = nullptr;
			hints.ai_family = AF_INET;
			if( getaddrinfo( host, nullptr, &hints, &res ) == 0 )
			{
				for( auto* p = res; p; p = p->ai_next )
					localIps.insert( ( (sockaddr_in*)p->ai_addr )->sin_addr.s_addr );
				freeaddrinfo( res );
			}
		}
#ifndef _WIN32
		//the host name often resolves to loopback only on macOS / Linux: add the interface addresses
		for( auto& i : net::ListInterfaces() ) localIps.insert( i.ipNet );
#endif
		localIps.insert( htonl( INADDR_LOOPBACK ) );
	}
	std::vector< uint8_t > buf( 1500 );
	while( run_ )
	{
		net::Sock r;
		{
			std::lock_guard< std::mutex > l( inLock_ );
			r = net::FromU( recvSock_ );
		}
		if( r == net::kInvalid )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
			continue;
		}
		sockaddr_in from{};
		int n = net::RecvFrom( r, buf.data(), (int)buf.size(), &from );
		if( n <= 0 ) continue;
		//our own output (broadcast) comes back in: ignore it
		bool own = localIps.count( from.sin_addr.s_addr ) && ntohs( from.sin_port ) == sendPort_;
		if( own && !acceptLoopback ) continue;
		if( n >= 12 && memcmp( buf.data(), "Art-Net", 8 ) == 0 && buf[ 8 ] == 0x00 && buf[ 9 ] == 0x20 )
		{
			//ArtPoll -> reply so consoles can discover us
			uint32_t ip = localIps.empty() ? 0 : *localIps.begin();
			for( uint32_t a : localIps )
				if( a != htonl( INADDR_LOOPBACK ) ) { ip = a; break; }
			auto rep = ArtPollReply( ip, 4 );
			sockaddr_in to = from;
			to.sin_port    = htons( 6454 );
			if( sendSock_ != ~(uintptr_t)0 ) net::SendTo( net::FromU( sendSock_ ), rep.data(), (int)rep.size(), to );
			continue;
		}
		int port, len;
		const uint8_t* data;
		if( !ParseArtDmx( buf.data(), n, port, data, len ) ) continue;
		++artnetRecv;
		std::lock_guard< std::mutex > l( inLock_ );
		InFrame* f = nullptr;
		for( auto& x : inFrames_ )
			if( x.port == port ) f = &x;
		if( !f )
		{
			inFrames_.push_back( InFrame{ port, {}, 0, false } );
			f = &inFrames_.back();
			f->data.fill( 0 );
		}
		memcpy( f->data.data(), data, len );
		f->len   = len;
		f->fresh = true;
	}
}

void DmxIO::PollInput()
{
	std::vector< InFrame > frames;
	{
		std::lock_guard< std::mutex > l( inLock_ );
		for( auto& f : inFrames_ )
			if( f.fresh )
			{
				frames.push_back( f );
				f.fresh = false;
			}
	}
	if( frames.empty() ) return;
	std::lock_guard< std::mutex > l( show_->dmxLock );
	lastInputUniverses.clear();
	for( auto& f : frames )
	{
		bool first = !lastIn_.count( f.port );
		auto& last = lastIn_[ f.port ];
		for( size_t ui = 0; ui < show_->universes.size(); ++ui )
		{
			auto& u = show_->universes[ ui ];
			if( !u.artnetIn || u.artnetInPort != f.port ) continue;
			lastInputUniverses.push_back( u.num );
			for( int c = 0; c < f.len; ++c )
				if( first || f.data[ c ] != last[ c ] )
					show_->dmx[ ui ][ c ] = f.data[ c ];
			++show_->dmxVersion;
		}
		last = f.data;
	}
}
}//namespace ls
