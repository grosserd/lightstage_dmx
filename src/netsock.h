// Thin portable socket layer: Winsock on Windows, BSD sockets elsewhere (macOS, Linux).
// Sockets are stored by the callers as uintptr_t with ~0 = none; that matches INVALID_SOCKET (Windows)
// and -1 (POSIX) after the cast, so the existing storage stays as it is.
// Also: network interface list and a small auto-reset wake event (used by the I/O threads).
#pragma once
#include "platform.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#include <cstdint>
#include <string>
#include <vector>

namespace ls
{
namespace net
{
#ifdef _WIN32
using Sock    = SOCKET;
using SockLen = int;
const Sock kInvalid = INVALID_SOCKET;
#else
using Sock    = int;
using SockLen = socklen_t;
const Sock kInvalid = -1;
#endif
inline Sock FromU( uintptr_t v ) { return v == ~(uintptr_t)0 ? kInvalid : (Sock)v; }
inline uintptr_t ToU( Sock s ) { return s == kInvalid ? ~(uintptr_t)0 : (uintptr_t)s; }

//Windows: WSAStartup (reference counted, harmless to repeat); elsewhere nothing to do
inline void Startup()
{
#ifdef _WIN32
	WSADATA wd;
	WSAStartup( MAKEWORD( 2, 2 ), &wd );
#endif
}
inline int LastError()
{
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}
//new socket; on macOS stream sockets get SO_NOSIGPIPE (a closed peer must not kill the process)
inline Sock Open( int type, int proto )
{
	Sock s = socket( AF_INET, type, proto );
#ifdef __APPLE__
	if( s != kInvalid )
	{
		int one = 1;
		setsockopt( s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof( one ) );
	}
#endif
	return s;
}
inline Sock Udp() { return Open( SOCK_DGRAM, IPPROTO_UDP ); }
inline Sock Tcp() { return Open( SOCK_STREAM, IPPROTO_TCP ); }
inline void Close( Sock s )
{
	if( s == kInvalid ) return;
#ifdef _WIN32
	closesocket( s );
#else
	close( s );
#endif
}
//wake a thread blocked in accept/recv/select on this socket (POSIX: close() alone does not)
inline void Shutdown( Sock s )
{
	if( s == kInvalid ) return;
#ifdef _WIN32
	shutdown( s, SD_BOTH );
#else
	shutdown( s, SHUT_RDWR );
#endif
}
inline bool SetNonBlocking( Sock s, bool on )
{
#ifdef _WIN32
	u_long v = on ? 1 : 0;
	return ioctlsocket( s, FIONBIO, &v ) == 0;
#else
	int f = fcntl( s, F_GETFL, 0 );
	if( f < 0 ) return false;
	return fcntl( s, F_SETFL, on ? ( f | O_NONBLOCK ) : ( f & ~O_NONBLOCK ) ) == 0;
#endif
}
//boolean SOL_SOCKET option (Windows BOOL, POSIX int)
inline void SetFlag( Sock s, int opt, bool on )
{
#ifdef _WIN32
	BOOL b = on ? TRUE : FALSE;
	setsockopt( s, SOL_SOCKET, opt, (const char*)&b, sizeof( b ) );
#else
	int b = on ? 1 : 0;
	setsockopt( s, SOL_SOCKET, opt, &b, sizeof( b ) );
#endif
}
inline void SetBroadcast( Sock s ) { SetFlag( s, SO_BROADCAST, true ); }
//share a UDP port with other programs (Art-Net 6454). macOS/BSD needs SO_REUSEPORT for that;
//Linux SO_REUSEPORT would load-balance unicast between the sockets, so it is not used there.
inline void SetReuse( Sock s )
{
	SetFlag( s, SO_REUSEADDR, true );
#ifdef __APPLE__
	SetFlag( s, SO_REUSEPORT, true );
#endif
}
inline void SetRecvTimeout( Sock s, int ms )
{
#ifdef _WIN32
	DWORD t = (DWORD)ms;
	setsockopt( s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof( t ) );
#else
	timeval t{ ms / 1000, ( ms % 1000 ) * 1000 };
	setsockopt( s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof( t ) );
#endif
}
inline void SetRecvBuffer( Sock s, int bytes ) { setsockopt( s, SOL_SOCKET, SO_RCVBUF, (const char*)&bytes, sizeof( bytes ) ); }
//multicast send options (Windows DWORD, BSD u_char for TTL / loop)
inline void SetMulticastTtl( Sock s, int ttl )
{
#ifdef _WIN32
	DWORD v = (DWORD)ttl;
#else
	u_char v = (u_char)ttl;
#endif
	setsockopt( s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&v, sizeof( v ) );
}
inline void SetMulticastLoop( Sock s, bool on )
{
#ifdef _WIN32
	DWORD v = on ? 1 : 0;
#else
	u_char v = on ? 1 : 0;
#endif
	setsockopt( s, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&v, sizeof( v ) );
}
inline void SetMulticastIf( Sock s, const in_addr& a ) { setsockopt( s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&a, sizeof( a ) ); }
//join a multicast group (sACN input 239.255.x.x) on the interface with address iface (INADDR_ANY = default)
inline bool JoinMulticast( Sock s, const char* group, uint32_t iface = 0 )
{
	ip_mreq m{};
	if( inet_pton( AF_INET, group, &m.imr_multiaddr ) != 1 ) return false;
	m.imr_interface.s_addr = iface;
	return setsockopt( s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof( m ) ) == 0;
}
inline sockaddr_in Addr( uint32_t ipNet, uint16_t port )
{
	sockaddr_in a{};
	a.sin_family      = AF_INET;
	a.sin_addr.s_addr = ipNet;
	a.sin_port        = htons( port );
	return a;
}
//dotted IPv4 -> sockaddr; false if not an address
inline bool Addr( const char* ip, uint16_t port, sockaddr_in& a )
{
	a            = sockaddr_in{};
	a.sin_family = AF_INET;
	a.sin_port   = htons( port );
	return inet_pton( AF_INET, ip, &a.sin_addr ) == 1;
}
inline bool Bind( Sock s, const sockaddr_in& a ) { return bind( s, (const sockaddr*)&a, sizeof( a ) ) == 0; }
inline bool Connect( Sock s, const sockaddr_in& a ) { return connect( s, (const sockaddr*)&a, sizeof( a ) ) == 0; }
inline bool LocalAddr( Sock s, sockaddr_in& a )
{
	SockLen len = sizeof( a );
	return getsockname( s, (sockaddr*)&a, &len ) == 0;
}
inline int SendTo( Sock s, const void* p, int n, const sockaddr_in& to ) { return (int)sendto( s, (const char*)p, n, 0, (const sockaddr*)&to, sizeof( to ) ); }
inline int RecvFrom( Sock s, void* p, int n, sockaddr_in* from )
{
	sockaddr_in f{};
	SockLen fl = sizeof( f );
	int r      = (int)recvfrom( s, (char*)p, n, 0, (sockaddr*)&f, &fl );
	if( from ) *from = f;
	return r;
}
//stream send / receive; no SIGPIPE on Linux (MSG_NOSIGNAL), macOS uses SO_NOSIGPIPE from Open()
inline int Send( Sock s, const void* p, int n )
{
#if defined( MSG_NOSIGNAL )
	return (int)send( s, (const char*)p, n, MSG_NOSIGNAL );
#else
	return (int)send( s, (const char*)p, n, 0 );
#endif
}
inline int Recv( Sock s, void* p, int n ) { return (int)recv( s, (char*)p, n, 0 ); }
//wait until s is readable: 1 readable, 0 timeout, <0 error
inline int WaitReadable( Sock s, int ms )
{
	if( s == kInvalid ) return -1;
	fd_set rs;
	FD_ZERO( &rs );
	FD_SET( s, &rs );
	timeval tv{ ms / 1000, ( ms % 1000 ) * 1000 };
#ifdef _WIN32
	return select( 0, &rs, nullptr, nullptr, &tv );
#else
	return select( s + 1, &rs, nullptr, nullptr, &tv );
#endif
}
inline std::string IpStr( uint32_t ipNet )
{
	in_addr a;
	a.s_addr = ipNet;
	char b[ 32 ] = {};
	inet_ntop( AF_INET, &a, b, sizeof( b ) );
	return b;
}

//IPv4 network interfaces (Windows: GetAdaptersAddresses, else getifaddrs)
struct Interface
{
	std::string name;     //adapter name as the OS shows it (UTF-8)
	std::string ip, netmask, broadcast;
	uint32_t ipNet = 0, maskNet = 0;//network byte order
	bool up = false, loopback = false;
};
std::vector< Interface > ListInterfaces();
}//namespace net

//wake event: EventCreate / EventSignal / EventWait / EventDestroy in platform.h
}//namespace ls
