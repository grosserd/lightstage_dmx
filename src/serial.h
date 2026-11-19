// Serial port for the Enttec DMX USB Pro (FTDI virtual COM port): 115200 8N1, no flow control,
// DTR/RTS on, ~250 ms read timeout, ~200 ms write timeout.
// Windows: serial_win.cpp (SetupAPI + CreateFile), macOS / Linux: serial_posix.cpp (IOKit or /dev glob + termios).
// ListComPorts() (declared in dmxio.h) is implemented there as well.
#pragma once
#include "dmxio.h"

namespace ls
{
namespace serial
{
void* Open( const std::string& port );//nullptr on failure; port as listed (COM3, /dev/cu.usbserial-XXXX)
void Close( void* h );
bool Write( void* h, const uint8_t* p, size_t n );//true if all bytes were written
//reads up to n bytes; waits up to ~250 ms for the first byte, returns after a 20 ms gap; -1 on error
int Read( void* h, uint8_t* p, int n );
}//namespace serial
}//namespace ls
