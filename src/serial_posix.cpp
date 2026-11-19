// Serial port, macOS / Linux: enumeration (IOKit on macOS, /dev glob on Linux) and termios.
#include "serial.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <chrono>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#endif

namespace ls
{
static std::string Base( const std::string& p )
{
	size_t s = p.rfind( '/' );
	return s == std::string::npos ? p : p.substr( s + 1 );
}
static void GlobPorts( const char* pattern, std::vector< ComPort >& r )
{
	glob_t g{};
	if( glob( pattern, 0, nullptr, &g ) == 0 )
		for( size_t i = 0; i < g.gl_pathc; ++i )
		{
			ComPort c;
			c.port = g.gl_pathv[ i ];
			c.name = Base( c.port );
			c.ftdi = c.port.find( "usbserial" ) != std::string::npos || c.port.find( "ttyUSB" ) != std::string::npos;
			r.push_back( c );
		}
	globfree( &g );
}

#ifdef __APPLE__
static std::string CfStr( CFTypeRef v )
{
	std::string r;
	if( v && CFGetTypeID( v ) == CFStringGetTypeID() )
	{
		char b[ 512 ] = {};
		if( CFStringGetCString( (CFStringRef)v, b, sizeof( b ), kCFStringEncodingUTF8 ) ) r = b;
	}
	if( v ) CFRelease( v );
	return r;
}
static int CfInt( CFTypeRef v )
{
	int r = 0;
	if( v && CFGetTypeID( v ) == CFNumberGetTypeID() ) CFNumberGetValue( (CFNumberRef)v, kCFNumberIntType, &r );
	if( v ) CFRelease( v );
	return r;
}
//MACOS-UNTESTED: IOKit serial enumeration (callout device /dev/cu.*, USB product name + vendor id from the parent USB device)
static void IoKitPorts( std::vector< ComPort >& r )
{
	CFMutableDictionaryRef match = IOServiceMatching( kIOSerialBSDServiceValue );
	if( !match ) return;
	CFDictionarySetValue( match, CFSTR( kIOSerialBSDTypeKey ), CFSTR( kIOSerialBSDAllTypes ) );
	io_iterator_t it = 0;
	if( IOServiceGetMatchingServices( MACH_PORT_NULL, match, &it ) != KERN_SUCCESS ) return;//consumes match
	const IOOptionBits up = kIORegistryIterateRecursively | kIORegistryIterateParents;
	for( io_object_t s; ( s = IOIteratorNext( it ) ) != 0; IOObjectRelease( s ) )
	{
		ComPort c;
		c.port = CfStr( IORegistryEntryCreateCFProperty( s, CFSTR( kIOCalloutDeviceKey ), kCFAllocatorDefault, 0 ) );
		if( c.port.empty() ) continue;
		const std::string b = Base( c.port );
		if( b.find( "Bluetooth" ) != std::string::npos || b.find( "debug-console" ) != std::string::npos ) continue;
		const std::string product = CfStr( IORegistryEntrySearchCFProperty( s, kIOServicePlane, CFSTR( "USB Product Name" ), kCFAllocatorDefault, up ) );
		const int vid = CfInt( IORegistryEntrySearchCFProperty( s, kIOServicePlane, CFSTR( "idVendor" ), kCFAllocatorDefault, up ) );
		c.name = product.empty() ? b : product + " (" + b + ")";
		if( vid )
		{
			char h[ 32 ];
			snprintf( h, sizeof( h ), "USB\\VID_%04X", vid );
			c.hwid = h;
		}
		c.ftdi = vid == 0x0403 || c.port.find( "usbserial" ) != std::string::npos || ToLower( c.name ).find( "dmx" ) != std::string::npos;
		r.push_back( c );
	}
	IOObjectRelease( it );
}
#endif

std::vector< ComPort > ListComPorts()
{
	std::vector< ComPort > r;
#ifdef __APPLE__
	IoKitPorts( r );
	if( r.empty() ) GlobPorts( "/dev/cu.usbserial-*", r );
#else
	GlobPorts( "/dev/ttyUSB*", r );//MACOS-UNTESTED (Linux): FTDI ports appear as ttyUSB*
	GlobPorts( "/dev/ttyACM*", r );
#endif
	std::sort( r.begin(), r.end(), []( const ComPort& a, const ComPort& b ) { return a.port < b.port; } );
	return r;
}

namespace serial
{
static int Fd( void* h ) { return *(int*)h; }

//MACOS-UNTESTED: termios setup for the FTDI virtual COM port (same settings as the Windows DCB)
void* Open( const std::string& port )
{
	int fd = open( port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK );//O_NONBLOCK: do not wait for carrier detect
	if( fd < 0 ) return nullptr;
	ioctl( fd, TIOCEXCL );//exclusive like the Windows share mode 0
	int fl = fcntl( fd, F_GETFL, 0 );
	if( fl >= 0 ) fcntl( fd, F_SETFL, fl & ~O_NONBLOCK );
	termios t{};
	if( tcgetattr( fd, &t ) != 0 )
	{
		close( fd );
		return nullptr;
	}
	cfmakeraw( &t );
	t.c_cflag |= CLOCAL | CREAD;
	t.c_cflag &= ~( CSIZE | PARENB | CSTOPB | CRTSCTS );
	t.c_cflag |= CS8;
	t.c_iflag &= ~( IXON | IXOFF | IXANY );
	t.c_cc[ VMIN ]  = 0;//reads are bounded by poll() below
	t.c_cc[ VTIME ] = 0;
	cfsetispeed( &t, B115200 );
	cfsetospeed( &t, B115200 );
	if( tcsetattr( fd, TCSANOW, &t ) != 0 )
	{
		close( fd );
		return nullptr;
	}
	int bits = TIOCM_DTR | TIOCM_RTS;
	ioctl( fd, TIOCMBIS, &bits );
	tcflush( fd, TCIOFLUSH );
	return new int( fd );
}
void Close( void* h )
{
	if( !h ) return;
	close( Fd( h ) );
	delete (int*)h;
}
static double Ms()
{
	using namespace std::chrono;
	return duration< double, std::milli >( steady_clock::now().time_since_epoch() ).count();
}
bool Write( void* h, const uint8_t* p, size_t n )
{
	if( !h ) return false;
	const int fd    = Fd( h );
	const double t0 = Ms();
	size_t done     = 0;
	while( done < n )
	{
		const int left = (int)( 200 - ( Ms() - t0 ) );//write timeout 200 ms like COMMTIMEOUTS
		if( left <= 0 ) return false;
		pollfd pf{ fd, POLLOUT, 0 };
		const int pr = poll( &pf, 1, left );
		if( pr < 0 && errno == EINTR ) continue;
		if( pr <= 0 || ( pf.revents & ( POLLERR | POLLHUP | POLLNVAL ) ) ) return false;
		const ssize_t w = write( fd, p + done, n - done );
		if( w < 0 )
		{
			if( errno == EINTR || errno == EAGAIN ) continue;
			return false;
		}
		done += (size_t)w;
	}
	return true;
}
int Read( void* h, uint8_t* p, int n )
{
	if( !h ) return -1;
	const int fd = Fd( h );
	int total    = 0;
	int wait     = 250;//first byte: total timeout, then the 20 ms interval timeout
	while( total < n )
	{
		pollfd pf{ fd, POLLIN, 0 };
		const int pr = poll( &pf, 1, wait );
		if( pr < 0 && errno == EINTR ) continue;
		if( pr < 0 ) return total ? total : -1;
		if( pr == 0 ) break;
		if( pf.revents & ( POLLERR | POLLHUP | POLLNVAL ) ) return total ? total : -1;
		const ssize_t r = read( fd, p + total, (size_t)( n - total ) );
		if( r < 0 )
		{
			if( errno == EINTR || errno == EAGAIN ) continue;
			return total ? total : -1;
		}
		if( r == 0 ) break;
		total += (int)r;
		wait = 20;
	}
	return total;
}
}//namespace serial
}//namespace ls
