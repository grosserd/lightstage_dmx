#include "dmxio.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <mmsystem.h>
#include <set>
#include <avrt.h>
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

//---------------------------------------------------------------- COM ports
std::vector< ComPort > ListComPorts()
{
	std::vector< ComPort > r;
	HDEVINFO h = SetupDiGetClassDevsA( &GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT );
	if( h == INVALID_HANDLE_VALUE ) return r;
	SP_DEVINFO_DATA di{};
	di.cbSize = sizeof( di );
	for( DWORD i = 0; SetupDiEnumDeviceInfo( h, i, &di ); ++i )
	{
		char name[ 256 ] = {}, hw[ 512 ] = {};
		{ wchar_t wn[ 256 ] = {}; SetupDiGetDeviceRegistryPropertyW( h, &di, SPDRP_FRIENDLYNAME, nullptr, (PBYTE)wn, sizeof( wn ), nullptr ); WideCharToMultiByte( CP_UTF8, 0, wn, -1, name, sizeof( name ), nullptr, nullptr ); }//UTF-8 for the UI (umlauts in device names)
		SetupDiGetDeviceRegistryPropertyA( h, &di, SPDRP_HARDWAREID, nullptr, (PBYTE)hw, sizeof( hw ), nullptr );
		HKEY key = SetupDiOpenDevRegKey( h, &di, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ );
		char port[ 64 ] = {};
		if( key != INVALID_HANDLE_VALUE )
		{
			DWORD sz = sizeof( port ), type = 0;
			RegQueryValueExA( key, "PortName", nullptr, &type, (LPBYTE)port, &sz );
			RegCloseKey( key );
		}
		if( strncmp( port, "COM", 3 ) != 0 ) continue;
		ComPort c;
		c.port = port;
		c.name = name;
		c.hwid = hw;
		std::string up = hw;
		for( auto& ch : up ) ch = (char)toupper( (unsigned char)ch );
		c.ftdi = up.find( "VID_0403" ) != std::string::npos || ToLower( name ).find( "dmx" ) != std::string::npos;
		r.push_back( c );
	}
	SetupDiDestroyDeviceInfoList( h );
	std::sort( r.begin(), r.end(), []( const ComPort& a, const ComPort& b ) { return atoi( a.port.c_str() + 3 ) < atoi( b.port.c_str() + 3 ); } );
	return r;
}

static HANDLE OpenCom( const std::string& port )
{
	std::string p = "\\\\.\\" + port;
	HANDLE h      = CreateFileA( p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr );
	if( h == INVALID_HANDLE_VALUE ) return nullptr;
	DCB dcb{};
	dcb.DCBlength = sizeof( dcb );
	GetCommState( h, &dcb );
	dcb.BaudRate = 115200;
	dcb.ByteSize = 8;
	dcb.Parity   = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	dcb.fDtrControl = DTR_CONTROL_ENABLE;
	dcb.fRtsControl = RTS_CONTROL_ENABLE;
	dcb.fOutxCtsFlow = FALSE;
	dcb.fOutX = dcb.fInX = FALSE;
	dcb.fBinary = TRUE;
	SetCommState( h, &dcb );
	COMMTIMEOUTS to{};
	to.ReadIntervalTimeout        = 20;
	to.ReadTotalTimeoutConstant   = 250;
	to.ReadTotalTimeoutMultiplier = 0;
	to.WriteTotalTimeoutConstant  = 200;
	SetCommTimeouts( h, &to );
	PurgeComm( h, PURGE_RXCLEAR | PURGE_TXCLEAR );
	return h;
}

//request widget parameters (label 3); a USB Pro answers with label 3
static bool ProbeEnttec( HANDLE h, std::string* info )
{
	const uint8_t req[] = { 0x7E, 3, 2, 0, 0, 0, 0xE7 };
	DWORD w             = 0;
	if( !::WriteFile( h, req, sizeof( req ), &w, nullptr ) || w != sizeof( req ) ) return false;
	uint8_t buf[ 64 ];
	DWORD got = 0, total = 0;
	double t0 = Now();
	while( Now() - t0 < 0.4 && total < sizeof( buf ) )
	{
		if( !::ReadFile( h, buf + total, sizeof( buf ) - total, &got, nullptr ) ) break;
		total += got;
		if( total >= 10 ) break;
		if( got == 0 && total > 0 ) break;
	}
	for( DWORD i = 0; i + 4 < total; ++i )
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
		HANDLE h = OpenCom( p.port );
		if( !h ) continue;
		bool ok = ProbeEnttec( h, info );
		CloseHandle( h );
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
	HANDLE h = OpenCom( p );
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
	if( com_ ) CloseHandle( (HANDLE)com_ );
	com_       = nullptr;
	enttecOpen = false;
}

//---------------------------------------------------------------- sockets
void DmxIO::OpenSockets()
{
	CloseSockets();
	SOCKET s = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	if( s != INVALID_SOCKET )
	{
		BOOL b = TRUE;
		setsockopt( s, SOL_SOCKET, SO_BROADCAST, (const char*)&b, sizeof( b ) );
		sockaddr_in a{};
		a.sin_family = AF_INET;
		inet_pton( AF_INET, cfg_.artnetBind.empty() ? "0.0.0.0" : cfg_.artnetBind.c_str(), &a.sin_addr );
		a.sin_port = 0;
		bind( s, (sockaddr*)&a, sizeof( a ) );
		int len = sizeof( a );
		getsockname( s, (sockaddr*)&a, &len );
		sendPort_ = ntohs( a.sin_port );
		sendSock_ = (uintptr_t)s;
	}
	if( cfg_.sacn )
	{
		SOCKET m = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
		if( m != INVALID_SOCKET )
		{
			sockaddr_in a{};
			a.sin_family = AF_INET;
			inet_pton( AF_INET, cfg_.artnetBind.empty() ? "0.0.0.0" : cfg_.artnetBind.c_str(), &a.sin_addr );
			bind( m, (sockaddr*)&a, sizeof( a ) );
			DWORD ttl = 8;
			setsockopt( m, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof( ttl ) );
			if( a.sin_addr.s_addr != INADDR_ANY ) setsockopt( m, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&a.sin_addr, sizeof( a.sin_addr ) );
			DWORD loop = acceptLoopback ? 1 : 0;
			setsockopt( m, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof( loop ) );
			sacnSock_ = (uintptr_t)m;
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
		SOCKET r = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
		if( r != INVALID_SOCKET )
		{
			BOOL b = TRUE;
			setsockopt( r, SOL_SOCKET, SO_REUSEADDR, (const char*)&b, sizeof( b ) );
			setsockopt( r, SOL_SOCKET, SO_BROADCAST, (const char*)&b, sizeof( b ) );
			DWORD tmo = 200;
			setsockopt( r, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof( tmo ) );
			sockaddr_in a{};
			a.sin_family      = AF_INET;
			a.sin_addr.s_addr = INADDR_ANY;
			a.sin_port        = htons( 6454 );
			if( bind( r, (sockaddr*)&a, sizeof( a ) ) == 0 )
				recvSock_ = (uintptr_t)r;
			else
			{
				Log( "artnet: bind 6454 failed (%d)", WSAGetLastError() );
				closesocket( r );
				std::lock_guard< std::mutex > l( statusLock_ );
				status_ = L( "Art-Net Eingang: Port 6454 belegt" );
			}
		}
	}
}
void DmxIO::CloseSockets()
{
	if( sendSock_ != ~(uintptr_t)0 ) closesocket( (SOCKET)sendSock_ );
	if( recvSock_ != ~(uintptr_t)0 ) closesocket( (SOCKET)recvSock_ );
	if( sacnSock_ != ~(uintptr_t)0 ) closesocket( (SOCKET)sacnSock_ );
	sendSock_ = recvSock_ = sacnSock_ = ~(uintptr_t)0;
}

void DmxIO::Start( Show* show )
{
	Stop();
	WSADATA wd;
	WSAStartup( MAKEWORD( 2, 2 ), &wd );
	show_ = show;
	{
		std::lock_guard< std::mutex > l( cfgLock_ );
		cfg_ = show->output;
	}
	OpenSockets();
	if( !outEvent_ ) outEvent_ = CreateEventW( nullptr, FALSE, FALSE, nullptr );
	run_ = true;
	out_ = std::thread( [ this ] { OutLoop(); } );
	in_  = std::thread( [ this ] { InLoop(); } );
}
void DmxIO::Stop()
{
	if( !run_ ) return;
	run_ = false;
	if( outEvent_ ) SetEvent( (HANDLE)outEvent_ );//wake the output thread at once (it may wait for the next refresh)
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
	if( outEvent_ ) SetEvent( (HANDLE)outEvent_ );
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
	if( outEvent_ ) SetEvent( (HANDLE)outEvent_ );//wake the output thread: new rate / targets apply at once
}

std::string DmxIO::Status()
{
	std::lock_guard< std::mutex > l( statusLock_ );
	return status_;
}

void DmxIO::OutLoop()
{
	timeBeginPeriod( 1 );
	double next = Now();
	bool lastEnttecWanted = false;
	std::string lastPort;
	bool boosted  = false;
	HANDLE mmcss  = nullptr;
	while( run_ )
	{
		if( boost != boosted )
		{
			boosted = boost;
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
			to.sin_family = AF_INET;
			to.sin_port   = htons( 6454 );
			if( inet_pton( AF_INET, cfg.artnetTarget.c_str(), &to.sin_addr ) == 1 )
			{
				for( auto& fr : frames )
				{
					if( !fr.first.artnetOut ) continue;
					auto pkt = BuildArtDmx( fr.first.artnetPort, seq_, fr.second.data(), 512 );
					if( sendto( (SOCKET)sendSock_, (const char*)pkt.data(), (int)pkt.size(), 0, (sockaddr*)&to, sizeof( to ) ) > 0 )
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
				to.sin_family = AF_INET;
				to.sin_port   = htons( 5568 );
				const std::string ip = cfg.sacnTarget.empty() ? E131Multicast( fr.first.num ) : cfg.sacnTarget;
				if( inet_pton( AF_INET, ip.c_str(), &to.sin_addr ) != 1 ) continue;
				auto pkt = BuildE131( fr.first.num, sacnSeq_, cfg.sacnPriority, cid_, "Grosserd LightStage", fr.second.data(), 512 );
				if( sendto( (SOCKET)sacnSock_, (const char*)pkt.data(), (int)pkt.size(), 0, (sockaddr*)&to, sizeof( to ) ) > 0 ) ++sacnSent;
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
					DWORD w = 0;
					if( ::WriteFile( (HANDLE)com_, f.data(), (DWORD)f.size(), &w, nullptr ) && w == f.size() )
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
			WaitForSingleObject( (HANDLE)outEvent_, (DWORD)std::max( 1.0, std::ceil( waitS * 1000.0 ) ) );
		}
	}
	if( mmcss ) AvRevertMmThreadCharacteristics( mmcss );
	timeEndPeriod( 1 );
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
		localIps.insert( htonl( INADDR_LOOPBACK ) );
	}
	std::vector< uint8_t > buf( 1500 );
	while( run_ )
	{
		SOCKET r;
		{
			std::lock_guard< std::mutex > l( inLock_ );
			r = recvSock_ == ~(uintptr_t)0 ? INVALID_SOCKET : (SOCKET)recvSock_;
		}
		if( r == INVALID_SOCKET )
		{
			Sleep( 50 );
			continue;
		}
		sockaddr_in from{};
		int fl = sizeof( from );
		int n  = recvfrom( r, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fl );
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
			if( sendSock_ != ~(uintptr_t)0 ) sendto( (SOCKET)sendSock_, (const char*)rep.data(), (int)rep.size(), 0, (sockaddr*)&to, sizeof( to ) );
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
