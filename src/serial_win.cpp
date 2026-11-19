// Serial port, Windows: SetupAPI enumeration and Win32 COM port (moved unchanged from dmxio.cpp).
#include "serial.h"
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <algorithm>

namespace ls
{
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

namespace serial
{
void* Open( const std::string& port )
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
void Close( void* h )
{
	if( h ) CloseHandle( (HANDLE)h );
}
bool Write( void* h, const uint8_t* p, size_t n )
{
	DWORD w = 0;
	return ::WriteFile( (HANDLE)h, p, (DWORD)n, &w, nullptr ) && w == n;
}
int Read( void* h, uint8_t* p, int n )
{
	DWORD got = 0;
	if( !::ReadFile( (HANDLE)h, p, (DWORD)n, &got, nullptr ) ) return -1;
	return (int)got;
}
}//namespace serial
}//namespace ls
