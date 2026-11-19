#include "common.h"
#include <chrono>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <share.h>
#include <windows.h>
#include <shlobj.h>
#else
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

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
	if( sscanf( s.c_str() + 1, "%6x", &v ) != 1 )
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

#ifdef _WIN32
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
static std::filesystem::path FsPath( const std::string& p ) { return std::filesystem::path( Widen( p ) ); }
static std::string FsString( const std::filesystem::path& p ) { return Narrow( p.wstring() ); }
#else
//wchar_t is UTF-32 here; invalid UTF-8 bytes become U+FFFD
std::wstring Widen( const std::string& s )
{
	std::wstring w;
	w.reserve( s.size() );
	for( size_t i = 0; i < s.size(); )
	{
		const unsigned char c = (unsigned char)s[ i ];
		//number of continuation bytes, -1 = invalid lead byte
		const int n = c < 0x80 ? 0 : ( c >> 5 ) == 6 ? 1 : ( c >> 4 ) == 14 ? 2 : ( c >> 3 ) == 30 ? 3 : -1;
		uint32_t cp = n <= 0 ? c : c & ( 0x3F >> n );
		bool ok     = n >= 0 && i + n < s.size();
		for( int k = 1; ok && k <= n; ++k )
		{
			const unsigned char d = (unsigned char)s[ i + k ];
			ok                    = ( d & 0xC0 ) == 0x80;
			cp                    = ( cp << 6 ) | ( d & 0x3F );
		}
		w += (wchar_t)( ok ? cp : 0xFFFD );
		i += ok ? n + 1 : 1;
	}
	return w;
}
std::string Narrow( const std::wstring& w )
{
	std::string s;
	s.reserve( w.size() );
	for( wchar_t wc : w )
	{
		const uint32_t c = (uint32_t)wc;
		if( c < 0x80 ) s += (char)c;
		else if( c < 0x800 ) { s += (char)( 0xC0 | ( c >> 6 ) ); s += (char)( 0x80 | ( c & 0x3F ) ); }
		else if( c < 0x10000 ) { s += (char)( 0xE0 | ( c >> 12 ) ); s += (char)( 0x80 | ( ( c >> 6 ) & 0x3F ) ); s += (char)( 0x80 | ( c & 0x3F ) ); }
		else { s += (char)( 0xF0 | ( c >> 18 ) ); s += (char)( 0x80 | ( ( c >> 12 ) & 0x3F ) ); s += (char)( 0x80 | ( ( c >> 6 ) & 0x3F ) ); s += (char)( 0x80 | ( c & 0x3F ) ); }
	}
	return s;
}
static std::filesystem::path FsPath( const std::string& p ) { return std::filesystem::path( p ); }//UTF-8 natively
static std::string FsString( const std::filesystem::path& p ) { return p.string(); }
#endif

std::string ExeDir()
{
#ifdef _WIN32
	wchar_t buf[ MAX_PATH ];
	GetModuleFileNameW( nullptr, buf, MAX_PATH );
	std::wstring p( buf );
	size_t sl = p.find_last_of( L"\\/" );
	return Narrow( sl == std::wstring::npos ? L"." : p.substr( 0, sl ) );
#else
	static const std::string dir = [] {
		std::string exe;
#ifdef __APPLE__
		uint32_t n = 0;
		_NSGetExecutablePath( nullptr, &n );
		std::string b( n + 1, '\0' );
		if( _NSGetExecutablePath( &b[ 0 ], &n ) == 0 ) exe = b.c_str();
#else
		char b[ PATH_MAX ] = {};
		const ssize_t n    = readlink( "/proc/self/exe", b, sizeof( b ) - 1 );
		if( n > 0 ) exe.assign( b, (size_t)n );
#endif
		char real[ PATH_MAX ];
		if( !exe.empty() && realpath( exe.c_str(), real ) ) exe = real;
		std::string d = DirName( exe );
		if( d.empty() ) d = ".";
#ifdef __APPLE__
		//inside an app bundle (Contents/MacOS/LightStage) the data lives in Contents/Resources
		// MACOS-UNTESTED: bundle layout; a plain build keeps assets / library next to the binary
		if( !FileExists( d + "/assets" ) && FileExists( d + "/../Resources/assets" ) ) d = DirName( d ) + "/Resources";
#endif
		return d;
	}();
	return dir;
#endif
}

bool ReadFile( const std::string& path, std::vector< uint8_t >& out )
{
	std::ifstream f( FsPath( path ), std::ios::binary );
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
	std::ofstream f( FsPath( path ), std::ios::binary );
	if( !f )
		return false;
	f.write( text.data(), (std::streamsize)text.size() );
	return (bool)f;
}
std::vector< std::string > ListFiles( const std::string& dir, const std::string& ext, bool recursive )
{
	std::vector< std::string > r;
	std::error_code ec;
	std::filesystem::path p( FsPath( dir ) );
	if( !std::filesystem::exists( p, ec ) )
		return r;
	auto take = [ & ]( const std::filesystem::directory_entry& e ) {
		if( !e.is_regular_file( ec ) )
			return;
		std::string fn = FsString( e.path() );
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
std::string DirName( const std::string& path )
{
	size_t sl = path.find_last_of( "\\/" );
	return sl == std::string::npos ? std::string() : path.substr( 0, sl );
}
bool FileExists( const std::string& path )
{
#ifdef _WIN32
	return GetFileAttributesW( Widen( path ).c_str() ) != INVALID_FILE_ATTRIBUTES;
#else
	struct stat st;
	return !path.empty() && stat( path.c_str(), &st ) == 0;
#endif
}
bool MakeDir( const std::string& dir )
{
#ifdef _WIN32
	return CreateDirectoryW( Widen( dir ).c_str(), nullptr ) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
	return mkdir( dir.c_str(), 0755 ) == 0 || errno == EEXIST;
#endif
}
bool MakeDirs( const std::string& dir )
{
	std::error_code ec;
	std::filesystem::create_directories( FsPath( dir ), ec );
	return std::filesystem::is_directory( FsPath( dir ), ec );
}
bool RemoveFile( const std::string& path )
{
#ifdef _WIN32
	return DeleteFileW( Widen( path ).c_str() ) != 0;
#else
	return unlink( path.c_str() ) == 0;
#endif
}
bool MoveFileReplace( const std::string& from, const std::string& to, bool writeThrough )
{
#ifdef _WIN32
	return MoveFileExW( Widen( from ).c_str(), Widen( to ).c_str(), MOVEFILE_REPLACE_EXISTING | ( writeThrough ? MOVEFILE_WRITE_THROUGH : 0 ) ) != 0;
#else
	(void)writeThrough;//rename() replaces atomically
	return rename( from.c_str(), to.c_str() ) == 0;
#endif
}
bool CopyFileReplace( const std::string& from, const std::string& to )
{
#ifdef _WIN32
	return CopyFileW( Widen( from ).c_str(), Widen( to ).c_str(), FALSE ) != 0;
#else
	std::error_code ec;
	return std::filesystem::copy_file( from, to, std::filesystem::copy_options::overwrite_existing, ec );
#endif
}
bool FileModTime( const std::string& path, std::tm& local )
{
#ifdef _WIN32
	WIN32_FILE_ATTRIBUTE_DATA fa{};
	if( !GetFileAttributesExW( Widen( path ).c_str(), GetFileExInfoStandard, &fa ) ) return false;
	FILETIME lt;
	SYSTEMTIME st;
	FileTimeToLocalFileTime( &fa.ftLastWriteTime, &lt );
	FileTimeToSystemTime( &lt, &st );
	local         = std::tm{};
	local.tm_year = st.wYear - 1900;
	local.tm_mon  = st.wMonth - 1;
	local.tm_mday = st.wDay;
	local.tm_hour = st.wHour;
	local.tm_min  = st.wMinute;
	local.tm_sec  = st.wSecond;
	local.tm_wday = st.wDayOfWeek;
	return true;
#else
	struct stat st;
	if( stat( path.c_str(), &st ) != 0 ) return false;
	local = LocalTime( st.st_mtime );
	return true;
#endif
}
FILE* FOpen( const std::string& path, const char* mode )
{
#ifdef _WIN32
	return _wfopen( Widen( path ).c_str(), Widen( mode ).c_str() );
#else
	return fopen( path.c_str(), mode );
#endif
}
std::tm LocalTime( time_t t )
{
	std::tm r{};
#ifdef _WIN32
	localtime_s( &r, &t );
#else
	localtime_r( &t, &r );
#endif
	return r;
}
std::string TempDir()
{
#ifdef _WIN32
	wchar_t tmp[ MAX_PATH ];
	GetTempPathW( MAX_PATH, tmp );
	return Narrow( tmp );
#else
	const char* t = getenv( "TMPDIR" );
	std::string d = t && *t ? t : "/tmp";
	if( !IsPathSep( d.back() ) ) d += '/';
	return d;
#endif
}
std::string DocumentsDir()
{
#ifdef _WIN32
	wchar_t p[ MAX_PATH ] = {};
	if( FAILED( SHGetFolderPathW( nullptr, CSIDL_PERSONAL, nullptr, 0, p ) ) ) return "";
	return Narrow( p );
#else
	const char* h = getenv( "HOME" );
	return h && *h ? std::string( h ) + "/Documents" : std::string();
#endif
}
void SleepMs( int ms )
{
#ifdef _WIN32
	Sleep( (DWORD)ms );
#else
	std::this_thread::sleep_for( std::chrono::milliseconds( ms ) );
#endif
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
#ifdef _WIN32
	if( !f )
		f = _wfsopen( Widen( ExeDir() + "\\lightstage.log" ).c_str(), L"w", _SH_DENYWR );//others may read while it runs
#else
	if( !f )
		f = fopen( ( ExeDir() + "/lightstage.log" ).c_str(), "w" );// MACOS-UNTESTED: not writable inside a signed app bundle
#endif
	if( f )
	{
		fprintf( f, "%s\n", buf );
		fflush( f );
	}
#ifdef _WIN32
	OutputDebugStringA( buf );
	OutputDebugStringA( "\n" );
#else
	fprintf( stderr, "%s\n", buf );
#endif
}

double Now()
{
	using namespace std::chrono;
	return duration< double >( steady_clock::now().time_since_epoch() ).count();
}
}//namespace ls
