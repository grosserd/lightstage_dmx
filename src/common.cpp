#include <share.h>
#include "common.h"
#include <windows.h>
#include <chrono>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace ls
{
M4 Inverse( const M4& mm )
{
	const float* m = mm.m;
	float inv[ 16 ];
	inv[ 0 ]  = m[ 5 ] * m[ 10 ] * m[ 15 ] - m[ 5 ] * m[ 11 ] * m[ 14 ] - m[ 9 ] * m[ 6 ] * m[ 15 ] + m[ 9 ] * m[ 7 ] * m[ 14 ] + m[ 13 ] * m[ 6 ] * m[ 11 ] - m[ 13 ] * m[ 7 ] * m[ 10 ];
	inv[ 4 ]  = -m[ 4 ] * m[ 10 ] * m[ 15 ] + m[ 4 ] * m[ 11 ] * m[ 14 ] + m[ 8 ] * m[ 6 ] * m[ 15 ] - m[ 8 ] * m[ 7 ] * m[ 14 ] - m[ 12 ] * m[ 6 ] * m[ 11 ] + m[ 12 ] * m[ 7 ] * m[ 10 ];
	inv[ 8 ]  = m[ 4 ] * m[ 9 ] * m[ 15 ] - m[ 4 ] * m[ 11 ] * m[ 13 ] - m[ 8 ] * m[ 5 ] * m[ 15 ] + m[ 8 ] * m[ 7 ] * m[ 13 ] + m[ 12 ] * m[ 5 ] * m[ 11 ] - m[ 12 ] * m[ 7 ] * m[ 9 ];
	inv[ 12 ] = -m[ 4 ] * m[ 9 ] * m[ 14 ] + m[ 4 ] * m[ 10 ] * m[ 13 ] + m[ 8 ] * m[ 5 ] * m[ 14 ] - m[ 8 ] * m[ 6 ] * m[ 13 ] - m[ 12 ] * m[ 5 ] * m[ 10 ] + m[ 12 ] * m[ 6 ] * m[ 9 ];
	inv[ 1 ]  = -m[ 1 ] * m[ 10 ] * m[ 15 ] + m[ 1 ] * m[ 11 ] * m[ 14 ] + m[ 9 ] * m[ 2 ] * m[ 15 ] - m[ 9 ] * m[ 3 ] * m[ 14 ] - m[ 13 ] * m[ 2 ] * m[ 11 ] + m[ 13 ] * m[ 3 ] * m[ 10 ];
	inv[ 5 ]  = m[ 0 ] * m[ 10 ] * m[ 15 ] - m[ 0 ] * m[ 11 ] * m[ 14 ] - m[ 8 ] * m[ 2 ] * m[ 15 ] + m[ 8 ] * m[ 3 ] * m[ 14 ] + m[ 12 ] * m[ 2 ] * m[ 11 ] - m[ 12 ] * m[ 3 ] * m[ 10 ];
	inv[ 9 ]  = -m[ 0 ] * m[ 9 ] * m[ 15 ] + m[ 0 ] * m[ 11 ] * m[ 13 ] + m[ 8 ] * m[ 1 ] * m[ 15 ] - m[ 8 ] * m[ 3 ] * m[ 13 ] - m[ 12 ] * m[ 1 ] * m[ 11 ] + m[ 12 ] * m[ 3 ] * m[ 9 ];
	inv[ 13 ] = m[ 0 ] * m[ 9 ] * m[ 14 ] - m[ 0 ] * m[ 10 ] * m[ 13 ] - m[ 8 ] * m[ 1 ] * m[ 14 ] + m[ 8 ] * m[ 2 ] * m[ 13 ] + m[ 12 ] * m[ 1 ] * m[ 10 ] - m[ 12 ] * m[ 2 ] * m[ 9 ];
	inv[ 2 ]  = m[ 1 ] * m[ 6 ] * m[ 15 ] - m[ 1 ] * m[ 7 ] * m[ 14 ] - m[ 5 ] * m[ 2 ] * m[ 15 ] + m[ 5 ] * m[ 3 ] * m[ 14 ] + m[ 13 ] * m[ 2 ] * m[ 7 ] - m[ 13 ] * m[ 3 ] * m[ 6 ];
	inv[ 6 ]  = -m[ 0 ] * m[ 6 ] * m[ 15 ] + m[ 0 ] * m[ 7 ] * m[ 14 ] + m[ 4 ] * m[ 2 ] * m[ 15 ] - m[ 4 ] * m[ 3 ] * m[ 14 ] - m[ 12 ] * m[ 2 ] * m[ 7 ] + m[ 12 ] * m[ 3 ] * m[ 6 ];
	inv[ 10 ] = m[ 0 ] * m[ 5 ] * m[ 15 ] - m[ 0 ] * m[ 7 ] * m[ 13 ] - m[ 4 ] * m[ 1 ] * m[ 15 ] + m[ 4 ] * m[ 3 ] * m[ 13 ] + m[ 12 ] * m[ 1 ] * m[ 7 ] - m[ 12 ] * m[ 3 ] * m[ 5 ];
	inv[ 14 ] = -m[ 0 ] * m[ 5 ] * m[ 14 ] + m[ 0 ] * m[ 6 ] * m[ 13 ] + m[ 4 ] * m[ 1 ] * m[ 14 ] - m[ 4 ] * m[ 2 ] * m[ 13 ] - m[ 12 ] * m[ 1 ] * m[ 6 ] + m[ 12 ] * m[ 2 ] * m[ 5 ];
	inv[ 3 ]  = -m[ 1 ] * m[ 6 ] * m[ 11 ] + m[ 1 ] * m[ 7 ] * m[ 10 ] + m[ 5 ] * m[ 2 ] * m[ 11 ] - m[ 5 ] * m[ 3 ] * m[ 10 ] - m[ 9 ] * m[ 2 ] * m[ 7 ] + m[ 9 ] * m[ 3 ] * m[ 6 ];
	inv[ 7 ]  = m[ 0 ] * m[ 6 ] * m[ 11 ] - m[ 0 ] * m[ 7 ] * m[ 10 ] - m[ 4 ] * m[ 2 ] * m[ 11 ] + m[ 4 ] * m[ 3 ] * m[ 10 ] + m[ 8 ] * m[ 2 ] * m[ 7 ] - m[ 8 ] * m[ 3 ] * m[ 6 ];
	inv[ 11 ] = -m[ 0 ] * m[ 5 ] * m[ 11 ] + m[ 0 ] * m[ 7 ] * m[ 9 ] + m[ 4 ] * m[ 1 ] * m[ 11 ] - m[ 4 ] * m[ 3 ] * m[ 9 ] - m[ 8 ] * m[ 1 ] * m[ 7 ] + m[ 8 ] * m[ 3 ] * m[ 5 ];
	inv[ 15 ] = m[ 0 ] * m[ 5 ] * m[ 10 ] - m[ 0 ] * m[ 6 ] * m[ 9 ] - m[ 4 ] * m[ 1 ] * m[ 10 ] + m[ 4 ] * m[ 2 ] * m[ 9 ] + m[ 8 ] * m[ 1 ] * m[ 6 ] - m[ 8 ] * m[ 2 ] * m[ 5 ];
	float det = m[ 0 ] * inv[ 0 ] + m[ 1 ] * inv[ 4 ] + m[ 2 ] * inv[ 8 ] + m[ 3 ] * inv[ 12 ];
	M4 r;
	if( fabsf( det ) < 1e-20f )
		return r;
	det = 1.0f / det;
	for( int i = 0; i < 16; ++i )
		r.m[ i ] = inv[ i ] * det;
	return r;
}

bool ParseHexColor( const std::string& s, V3& out )
{
	if( s.size() < 7 || s[ 0 ] != '#' )
		return false;
	unsigned v = 0;
	if( sscanf_s( s.c_str() + 1, "%6x", &v ) != 1 )
		return false;
	out = V3( ( ( v >> 16 ) & 255 ) / 255.0f, ( ( v >> 8 ) & 255 ) / 255.0f, ( v & 255 ) / 255.0f );
	return true;
}

V3 Kelvin( float k )
{
	//Tanner Helland approximation, normalized so the brightest channel is 1
	float t = Clamp( k, 1000.0f, 40000.0f ) / 100.0f;
	float r, g, b;
	if( t <= 66 )
	{
		r = 255;
		g = 99.4708025861f * logf( t ) - 161.1195681661f;
		b = t <= 19 ? 0 : 138.5177312231f * logf( t - 10 ) - 305.0447927307f;
	}
	else
	{
		r = 329.698727446f * powf( t - 60, -0.1332047592f );
		g = 288.1221695283f * powf( t - 60, -0.0755148492f );
		b = 255;
	}
	V3 c( Clamp( r, 0.0f, 255.0f ) / 255.0f, Clamp( g, 0.0f, 255.0f ) / 255.0f, Clamp( b, 0.0f, 255.0f ) / 255.0f );
	float m = std::max( c.x, std::max( c.y, c.z ) );
	return m > 0 ? c / m : V3( 1, 1, 1 );
}

std::wstring Widen( const std::string& s )
{
	if( s.empty() )
		return L"";
	int n = MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0 );
	std::wstring w( n, 0 );
	MultiByteToWideChar( CP_UTF8, 0, s.c_str(), (int)s.size(), &w[ 0 ], n );
	return w;
}
std::string Narrow( const std::wstring& w )
{
	if( w.empty() )
		return "";
	int n = WideCharToMultiByte( CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr );
	std::string s( n, 0 );
	WideCharToMultiByte( CP_UTF8, 0, w.c_str(), (int)w.size(), &s[ 0 ], n, nullptr, nullptr );
	return s;
}

std::string ExeDir()
{
	wchar_t buf[ MAX_PATH ];
	GetModuleFileNameW( nullptr, buf, MAX_PATH );
	std::wstring p( buf );
	size_t sl = p.find_last_of( L"\\/" );
	return Narrow( sl == std::wstring::npos ? L"." : p.substr( 0, sl ) );
}

bool ReadFile( const std::string& path, std::vector< uint8_t >& out )
{
	std::ifstream f( std::filesystem::path( Widen( path ) ), std::ios::binary );
	if( !f )
		return false;
	f.seekg( 0, std::ios::end );
	std::streamoff n = f.tellg();
	f.seekg( 0 );
	out.resize( (size_t)std::max< std::streamoff >( 0, n ) );
	if( n > 0 )
		f.read( (char*)out.data(), n );
	return (bool)f || f.eof();
}
bool ReadText( const std::string& path, std::string& out )
{
	std::vector< uint8_t > b;
	if( !ReadFile( path, b ) )
		return false;
	out.assign( (const char*)b.data(), b.size() );
	if( out.size() >= 3 && (uint8_t)out[ 0 ] == 0xEF && (uint8_t)out[ 1 ] == 0xBB && (uint8_t)out[ 2 ] == 0xBF )
		out.erase( 0, 3 );
	return true;
}
bool WriteText( const std::string& path, const std::string& text )
{
	std::ofstream f( std::filesystem::path( Widen( path ) ), std::ios::binary );
	if( !f )
		return false;
	f.write( text.data(), (std::streamsize)text.size() );
	return (bool)f;
}
std::vector< std::string > ListFiles( const std::string& dir, const std::string& ext, bool recursive )
{
	std::vector< std::string > r;
	std::error_code ec;
	std::filesystem::path p( Widen( dir ) );
	if( !std::filesystem::exists( p, ec ) )
		return r;
	auto take = [ & ]( const std::filesystem::directory_entry& e ) {
		if( !e.is_regular_file( ec ) )
			return;
		std::string fn = Narrow( e.path().wstring() );
		if( ext.empty() || ToLower( fn ).size() >= ext.size() && ToLower( fn ).compare( fn.size() - ext.size(), ext.size(), ext ) == 0 )
			r.push_back( fn );
	};
	if( recursive )
		for( auto& e : std::filesystem::recursive_directory_iterator( p, ec ) )
			take( e );
	else
		for( auto& e : std::filesystem::directory_iterator( p, ec ) )
			take( e );
	std::sort( r.begin(), r.end() );
	return r;
}
std::string FileName( const std::string& path )
{
	size_t sl = path.find_last_of( "\\/" );
	return sl == std::string::npos ? path : path.substr( sl + 1 );
}
std::string FileStem( const std::string& path )
{
	std::string n = FileName( path );
	size_t d      = n.find_last_of( '.' );
	return d == std::string::npos ? n : n.substr( 0, d );
}
std::string ToLower( std::string s )
{
	for( char& c : s )
		c = (char)tolower( (unsigned char)c );
	return s;
}

static std::mutex g_logMutex;
void Log( const char* fmt, ... )
{
	char buf[ 2048 ];
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	std::lock_guard< std::mutex > lk( g_logMutex );
	static FILE* f = nullptr;
	if( !f )
		f = _wfsopen( Widen( ExeDir() + "\\lightstage.log" ).c_str(), L"w", _SH_DENYWR );//others may read while it runs
	if( f )
	{
		fprintf( f, "%s\n", buf );
		fflush( f );
	}
	OutputDebugStringA( buf );
	OutputDebugStringA( "\n" );
}

double Now()
{
	using namespace std::chrono;
	return duration< double >( steady_clock::now().time_since_epoch() ).count();
}
}//namespace ls
