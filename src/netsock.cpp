// Network interface list and wake event, per platform (see netsock.h).
#include "netsock.h"
#ifdef _WIN32
#include <windows.h>
#include <iphlpapi.h>
#else
#include <chrono>
#include <condition_variable>
#include <ifaddrs.h>
#include <mutex>
#include <net/if.h>
#include <thread>
#endif

namespace ls
{
namespace net
{
#ifdef _WIN32
std::vector< Interface > ListInterfaces()
{
	std::vector< Interface > r;
	ULONG size = 16 * 1024;
	std::vector< uint8_t > buf;
	ULONG res = ERROR_BUFFER_OVERFLOW;
	for( int tries = 0; tries < 3 && res == ERROR_BUFFER_OVERFLOW; ++tries )
	{
		buf.resize( size );
		res = GetAdaptersAddresses( AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size );
	}
	if( res != NO_ERROR ) return r;
	for( auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next )
	{
		char name[ 256 ] = {};
		WideCharToMultiByte( CP_UTF8, 0, a->FriendlyName, -1, name, sizeof( name ), nullptr, nullptr );
		for( auto* u = a->FirstUnicastAddress; u; u = u->Next )
		{
			if( u->Address.lpSockaddr->sa_family != AF_INET ) continue;
			Interface i;
			i.name     = name;
			i.ipNet    = ( (sockaddr_in*)u->Address.lpSockaddr )->sin_addr.s_addr;
			i.maskNet  = u->OnLinkPrefixLength ? htonl( 0xffffffffu << ( 32 - u->OnLinkPrefixLength ) ) : 0;
			i.up       = a->OperStatus == IfOperStatusUp;
			i.loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
			i.ip        = IpStr( i.ipNet );
			i.netmask   = IpStr( i.maskNet );
			i.broadcast = IpStr( i.ipNet | ~i.maskNet );
			r.push_back( i );
		}
	}
	return r;
}
#else
std::vector< Interface > ListInterfaces()
{
	std::vector< Interface > r;
	ifaddrs* list = nullptr;
	if( getifaddrs( &list ) != 0 ) return r;
	for( ifaddrs* a = list; a; a = a->ifa_next )
	{
		if( !a->ifa_addr || a->ifa_addr->sa_family != AF_INET ) continue;
		Interface i;
		i.name     = a->ifa_name ? a->ifa_name : "";
		i.ipNet    = ( (sockaddr_in*)a->ifa_addr )->sin_addr.s_addr;
		i.maskNet  = a->ifa_netmask ? ( (sockaddr_in*)a->ifa_netmask )->sin_addr.s_addr : 0;
		i.up       = ( a->ifa_flags & IFF_UP ) != 0;
		i.loopback = ( a->ifa_flags & IFF_LOOPBACK ) != 0;
		i.ip        = IpStr( i.ipNet );
		i.netmask   = IpStr( i.maskNet );
		i.broadcast = IpStr( ( ( a->ifa_flags & IFF_BROADCAST ) && a->ifa_broadaddr ) ? ( (sockaddr_in*)a->ifa_broadaddr )->sin_addr.s_addr : ( i.ipNet | ~i.maskNet ) );
		r.push_back( i );
	}
	freeifaddrs( list );
	return r;
}
#endif
}//namespace net

}//namespace ls
