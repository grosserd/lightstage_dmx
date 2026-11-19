#include "video.h"
#include "video_int.h"
#include "show.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

namespace ls
{
void MediaSleepMs( int ms )
{
#ifdef _WIN32
	Sleep( ms );
#else
	usleep( (useconds_t)ms * 1000 );
#endif
}
static V3 AvgColor( const uint8_t* px, int w, int h, bool bgra )
{
	double r = 0, g = 0, b = 0;
	int n    = 0;
	int step = std::max( 1, ( w * h ) / 4096 );
	for( int i = 0; i < w * h; i += step, ++n )
	{
		const uint8_t* p = px + (size_t)i * 4;
		r += p[ bgra ? 2 : 0 ];
		g += p[ 1 ];
		b += p[ bgra ? 0 : 2 ];
	}
	if( !n ) return V3();
	auto lin = []( double c ) { c /= 255.0; return (float)( c * c );};//cheap sRGB -> linear
	return V3( lin( r / n ), lin( g / n ), lin( b / n ) );
}

//================================================================ test card
class TestCard : public VideoSource
{
public:
	explicit TestCard( uint32_t seed ) : seed_( seed )
	{
		const int W = kW, H = kH;
		base_.assign( (size_t)W * H * 4, 255 );
		const V3 bars[ 7 ] = { { 0.75f, 0.75f, 0.75f }, { 0.75f, 0.75f, 0 }, { 0, 0.75f, 0.75f }, { 0, 0.75f, 0 }, { 0.75f, 0, 0.75f }, { 0.75f, 0, 0 }, { 0, 0, 0.75f } };
		for( int y = 0; y < H; ++y )
			for( int x = 0; x < W; ++x )
			{
				V3 c;
				float fy = (float)y / H, fx = (float)x / W;
				if( fy < 0.62f ) c = bars[ std::min( 6, x * 7 / W ) ];
				else if( fy < 0.74f ) c = V3( fx, fx, fx );//grey ramp
				else if( fy < 0.86f )
				{
					//colour sweep
					c = Hsv( fx + seed * 0.13f, 0.9f, 0.9f );
				}
				else c = V3( 0.04f, 0.04f, 0.05f );
				//grid
				if( x % 80 == 0 || y % 80 == 0 ) c = c * 0.55f + V3( 0.35f, 0.35f, 0.35f );
				//centre ring + cross
				float dx = x - W * 0.5f, dy = y - H * 0.5f, r = sqrtf( dx * dx + dy * dy );
				if( fabsf( r - H * 0.36f ) < 2.5f ) c = V3( 1, 1, 1 );
				if( ( fabsf( dx ) < 1.5f || fabsf( dy ) < 1.5f ) && r < H * 0.36f ) c = V3( 1, 1, 1 );
				//frame
				if( x < 6 || y < 6 || x >= W - 6 || y >= H - 6 ) c = V3( 1, 1, 1 );
				uint8_t* p = &base_[ ( (size_t)y * W + x ) * 4 ];
				p[ 0 ] = (uint8_t)( Saturate( c.x ) * 255 );
				p[ 1 ] = (uint8_t)( Saturate( c.y ) * 255 );
				p[ 2 ] = (uint8_t)( Saturate( c.z ) * 255 );
			}
		//corner markers: top-left red, top-right green (shows orientation / mirroring)
		for( int y = 14; y < 64; ++y )
			for( int x = 14; x < 64; ++x )
			{
				uint8_t* a = &base_[ ( (size_t)y * W + x ) * 4 ];
				uint8_t* b = &base_[ ( (size_t)y * W + ( W - 1 - x ) ) * 4 ];
				a[ 0 ] = 255; a[ 1 ] = 30; a[ 2 ] = 30;
				b[ 0 ] = 30; b[ 1 ] = 255; b[ 2 ] = 30;
			}
		avg_ = AvgColor( base_.data(), W, H, false );
	}
	bool Latest( VideoFrame& out ) override
	{
		double t = Now();
		if( out.serial && t - last_ < 1.0 / 30.0 ) return false;
		last_ = t;
		const int W = kW, H = kH;
		out.px = base_;
		//moving block in the bottom band (shows that the picture is live)
		int bx = (int)( fmod( t * 0.25, 1.0 ) * ( W - 120 ) );
		for( int y = (int)( H * 0.88f ); y < H - 12; ++y )
			for( int x = bx; x < bx + 120; ++x )
			{
				uint8_t* p = &out.px[ ( (size_t)y * W + x ) * 4 ];
				p[ 0 ] = p[ 1 ] = p[ 2 ] = 255;
			}
		out.w      = W;
		out.h      = H;
		out.bgra   = false;
		out.avg    = avg_;
		out.serial = ++serial_;
		return true;
	}
	std::string Status() const override { return L( "Testbild 960 x 540" ); }
	bool Ok() const override { return true; }

private:
	static const int kW = 960, kH = 540;
	uint32_t seed_;
	std::vector< uint8_t > base_;
	V3 avg_;
	double last_     = 0;
	uint64_t serial_ = 0;
};
std::unique_ptr< VideoSource > MakeTestCard( uint32_t seed ) { return std::unique_ptr< VideoSource >( new TestCard( seed ) ); }

//================================================================ threaded source base
class ThreadedSource : public VideoSource
{
public:
	bool Latest( VideoFrame& out ) override
	{
		std::lock_guard< std::mutex > l( m_ );
		if( !frame_.w || frame_.serial == out.serial ) return false;
		std::swap( out.px, frame_.px );
		out.w      = frame_.w;
		out.h      = frame_.h;
		out.bgra   = frame_.bgra;
		out.avg    = frame_.avg;
		out.serial = frame_.serial;
		return true;
	}
	std::string Status() const override
	{
		std::lock_guard< std::mutex > l( m_ );
		return status_;
	}
	bool Ok() const override { return !failed_; }

protected:
	void SetStatus( const std::string& s )
	{
		std::lock_guard< std::mutex > l( m_ );
		status_ = s;
	}
	//rows already top-down in 'work'; work receives the previous buffer back
	void Publish( std::vector< uint8_t >& work, int w, int h, bool bgra )
	{
		V3 avg = AvgColor( work.data(), w, h, bgra );
		std::lock_guard< std::mutex > l( m_ );
		std::swap( frame_.px, work );
		frame_.w      = w;
		frame_.h      = h;
		frame_.bgra   = bgra;
		frame_.avg    = avg;
		frame_.serial = ++serial_;
	}
	void StopThread()
	{
		stop_ = true;
		if( th_.joinable() ) th_.join();
	}
	std::thread th_;
	std::atomic< bool > stop_{ false }, failed_{ false };
	mutable std::mutex m_;
	VideoFrame frame_;
	std::string status_;
	uint64_t serial_ = 0;
};


//================================================================ video file (platform decoder, paced by its time stamps)
class FileVideo : public ThreadedSource
{
public:
	FileVideo( const std::string& path, bool loop ) : path_( path ), loop_( loop )
	{
		status_ = L( "öffne " ) + FileName( path );
		th_     = std::thread( [ this ] { Run(); } );
	}
	~FileVideo() override { StopThread(); }

private:
	void Run()
	{
		std::unique_ptr< VideoDecoder > dec = MakeVideoDecoder();
		const int e                         = dec->Open( path_ );
		if( e == 1 )
		{
			failed_ = true;
			SetStatus( L( "Datei nicht lesbar: " ) + FileName( path_ ) );
			return;
		}
		if( e != 0 )
		{
			failed_ = true;
#ifdef __APPLE__
			SetStatus( L( "Kein Video oder Codec nicht unterstützt (H.264/HEVC/ProRes gehen; HAP/DXV/WMV nicht)" ) );
#else
			SetStatus( L( "Kein Video oder Codec nicht unterstützt (H.264/HEVC/MPEG-4/WMV gehen; HAP/DXV/ProRes nicht)" ) );
#endif
			return;
		}
		char buf[ 160 ];
		snprintf( buf, sizeof( buf ), "%s  %d x %d  %.2f fps", FileName( path_ ).c_str(), dec->W(), dec->H(), dec->Fps() );
		SetStatus( buf );
		std::vector< uint8_t > work;
		bool started = false;
		double first = 0, t0 = 0;
		while( !stop_ )
		{
			double ts   = 0;
			const int r = dec->Read( work, ts );
			if( r < 0 )
			{
				failed_ = true;
				SetStatus( L( "Lesefehler in " ) + FileName( path_ ) );
				break;
			}
			if( r == 0 )
			{
				if( !loop_ )
				{
					SetStatus( FileName( path_ ) + L( "  (Ende)" ) );
					while( !stop_ ) MediaSleepMs( 20 );
					break;
				}
				dec->Seek( 0 );
				started = false;
				continue;
			}
			if( r == 2 ) continue;
			if( !started )
			{
				started = true;
				first   = ts;
				t0      = Now();
			}
			double due = t0 + ( ts - first );
			while( !stop_ && Now() < due - 0.001 ) MediaSleepMs( 1 );
			if( Now() - due > 0.3 )
			{
				//decoding fell behind: restart the clock instead of racing
				first = ts;
				t0    = Now();
			}
			if( r == 1 && dec->W() > 0 && dec->H() > 0 && work.size() >= (size_t)dec->W() * dec->H() * 4 ) Publish( work, dec->W(), dec->H(), true );
		}
	}
	std::string path_;
	bool loop_;
};
std::unique_ptr< VideoSource > OpenVideoFile( const std::string& path, bool loop ) { return std::unique_ptr< VideoSource >( new FileVideo( path, loop ) ); }

//================================================================ NDI (runtime loaded, minimal ABI of the NDI SDK)
namespace
{
struct NdiSrc
{
	const char* name;
	const char* url;
};
struct NdiFindCreate
{
	bool showLocal;
	const char* groups;
	const char* extraIps;
};
struct NdiRecvCreate
{
	NdiSrc src;
	int colorFormat;//2 = RGBX_RGBA
	int bandwidth;  //100 = highest
	bool fields;
	const char* recvName;
};
struct NdiVideo
{
	int xres, yres;
	uint32_t fourcc;
	int frN, frD;
	float aspect;
	int format;//1 = progressive
	int64_t timecode;
	uint8_t* data;
	int stride;
	const char* meta;
	int64_t timestamp;
};
struct NdiSendCreate
{
	const char* name;
	const char* groups;
	bool clockVideo, clockAudio;
};
constexpr uint32_t Fcc( char a, char b, char c, char d ) { return (uint32_t)(uint8_t)a | ( (uint32_t)(uint8_t)b << 8 ) | ( (uint32_t)(uint8_t)c << 16 ) | ( (uint32_t)(uint8_t)d << 24 ); }

struct NdiApi
{
	bool ok = false;
	std::string path;
	bool ( *initialize )()                                                    = nullptr;
	void* ( *findCreate )( const NdiFindCreate* )                             = nullptr;
	bool ( *findWait )( void*, uint32_t )                                     = nullptr;
	const NdiSrc* ( *findSources )( void*, uint32_t* )                        = nullptr;
	void* ( *recvCreate )( const NdiRecvCreate* )                             = nullptr;
	void ( *recvDestroy )( void* )                                            = nullptr;
	int ( *recvCapture )( void*, NdiVideo*, void*, void*, uint32_t )          = nullptr;
	void ( *recvFree )( void*, const NdiVideo* )                              = nullptr;
	void* ( *sendCreate )( const NdiSendCreate* )                             = nullptr;
	void ( *sendDestroy )( void* )                                            = nullptr;
	void ( *sendVideo )( void*, const NdiVideo* )                             = nullptr;
	const NdiSrc* ( *sendName )( void* )                                      = nullptr;
	void ( *findDestroy )( void* )                                            = nullptr;
	void ( *destroy )()                                                       = nullptr;
	std::mutex m;
	std::vector< std::string > sources;
	std::atomic< bool > finder{ false };
	std::atomic< bool > stop{ false };
	std::thread finderTh;//joined in NdiShutdown: a thread killed inside NDI's network calls can hang the process end
};
static std::atomic< bool > g_ndiLoaded{ false };

//shared library access (NDI runtime)
#ifdef _WIN32
static bool FileExists( const std::string& p ) { return GetFileAttributesW( Widen( p ).c_str() ) != INVALID_FILE_ATTRIBUTES; }
static void* LibOpen( const std::string& p ) { return (void*)LoadLibraryExW( Widen( p ).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH ); }
static void* LibSym( void* m, const char* name ) { return (void*)GetProcAddress( (HMODULE)m, name ); }
static void LibClose( void* m ) { FreeLibrary( (HMODULE)m ); }
static const char* const kNdiLib = "\\Processing.NDI.Lib.x64.dll";
#else
static bool FileExists( const std::string& p )
{
	struct stat st;
	return stat( p.c_str(), &st ) == 0;
}
static void* LibOpen( const std::string& p ) { return dlopen( p.c_str(), RTLD_NOW | RTLD_LOCAL ); }
static void* LibSym( void* m, const char* name ) { return dlsym( m, name ); }
static void LibClose( void* m ) { dlclose( m ); }
#ifdef __APPLE__
static const char* const kNdiLib = "/libndi.dylib";//NDI Tools / NDI SDK for Apple (same C ABI as on Windows)
#else
static const char* const kNdiLib = "/libndi.so.6";
#endif
#endif

static void LoadNdi( NdiApi& a )
{
	if( getenv( "LS_NO_NDI" ) ) return;
	std::vector< std::string > dirs;
	for( const char* e : { "NDI_RUNTIME_DIR_V6", "NDI_RUNTIME_DIR_V5", "NDI_RUNTIME_DIR_V4" } )
		if( const char* v = getenv( e ) ) dirs.push_back( v );
	dirs.push_back( ExeDir() );
#ifdef _WIN32
	for( const char* d : { "C:\\Program Files\\NDI\\NDI 6 Runtime\\v6", "C:\\Program Files\\NDI\\NDI 6 Tools\\Runtime", "C:\\Program Files\\NDI\\NDI 5 Runtime\\v5", "C:\\Program Files\\NDI\\NDI 5 Tools\\Runtime",
	                       "C:\\Program Files\\Resolume Arena", "C:\\Program Files\\Resolume Avenue", "C:\\Program Files\\Resolume Wire", "C:\\Program Files\\vMix", "C:\\Program Files\\OBS Studio\\obs-plugins\\64bit" } )
		dirs.push_back( d );
	//TouchDesigner installs: C:\Program Files\Derivative\TouchDesigner*\bin
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW( L"C:\\Program Files\\Derivative\\*", &fd );
	if( h != INVALID_HANDLE_VALUE )
	{
		do
			if( ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) && fd.cFileName[ 0 ] != L'.' ) dirs.push_back( "C:\\Program Files\\Derivative\\" + Narrow( fd.cFileName ) + "\\bin" );
		while( FindNextFileW( h, &fd ) );
		FindClose( h );
	}
#elif defined( __APPLE__ )
	//MACOS-UNTESTED: NDI Tools / NDI runtime for macOS install libndi.dylib in /usr/local/lib; SDK folder and Homebrew as fallbacks
	for( const char* d : { "/usr/local/lib", "/Library/NDI SDK for Apple/lib/macOS", "/opt/homebrew/lib" } )
		dirs.push_back( d );
#else
	for( const char* d : { "/usr/local/lib", "/usr/lib", "/usr/lib/x86_64-linux-gnu" } )
		dirs.push_back( d );
#endif
	for( auto& d : dirs )
	{
		std::string p = d + kNdiLib;
		if( !FileExists( p ) ) continue;
		void* m = LibOpen( p );
		if( !m ) continue;
		auto get = [ & ]( auto& fn, const char* name ) { fn = reinterpret_cast< std::remove_reference_t< decltype( fn ) > >( LibSym( m, name ) ); return fn != nullptr; };
		bool all = get( a.initialize, "NDIlib_initialize" ) && get( a.findCreate, "NDIlib_find_create_v2" ) && get( a.findWait, "NDIlib_find_wait_for_sources" ) &&
		           get( a.findSources, "NDIlib_find_get_current_sources" ) && get( a.recvCreate, "NDIlib_recv_create_v3" ) && get( a.recvDestroy, "NDIlib_recv_destroy" ) &&
		           get( a.recvCapture, "NDIlib_recv_capture_v2" ) && get( a.recvFree, "NDIlib_recv_free_video_v2" ) && get( a.sendCreate, "NDIlib_send_create" ) &&
		           get( a.sendDestroy, "NDIlib_send_destroy" ) && get( a.sendVideo, "NDIlib_send_send_video_v2" );
		get( a.sendName, "NDIlib_send_get_source_name" );//optional
		get( a.findDestroy, "NDIlib_find_destroy" );//optional
		get( a.destroy, "NDIlib_destroy" );//optional
		if( all && a.initialize() )
		{
			a.ok   = true;
			a.path = p;
			Log( "NDI runtime: %s", p.c_str() );
			return;
		}
		LibClose( m );
	}
	Log( "NDI runtime not found (install NDI Tools / NDI Runtime)" );
}
static NdiApi& Ndi()
{
	static NdiApi* a = [] {
		auto* x = new NdiApi;//never freed: background threads may still use it while the process ends
		LoadNdi( *x );
		g_ndiLoaded = true;
		return x;
	}();
	return *a;
}
static void StartFinder()
{
	NdiApi& a = Ndi();
	if( !a.ok || a.stop || a.finder.exchange( true ) ) return;
	a.finderTh = std::thread( [] {
		NdiApi& a = Ndi();
		NdiFindCreate fc{ true, nullptr, nullptr };
		void* f = a.findCreate( &fc );
		if( !f ) return;
		while( !a.stop )
		{
			a.findWait( f, 200 );
			if( a.stop ) break;
			uint32_t n        = 0;
			const NdiSrc* src = a.findSources( f, &n );
			std::vector< std::string > v;
			for( uint32_t i = 0; i < n; ++i )
				if( src[ i ].name ) v.push_back( src[ i ].name );
			std::lock_guard< std::mutex > l( a.m );
			a.sources.swap( v );
		}
		if( a.findDestroy ) a.findDestroy( f );
	} );
}
}//namespace

void NdiShutdown()
{
	if( !g_ndiLoaded ) return;//never used: nothing to stop (and do not load it now)
	NdiApi& a = Ndi();
	a.stop    = true;
	if( a.finderTh.joinable() ) a.finderTh.join();
	if( a.ok && a.destroy ) a.destroy();//closes NDI's own sockets and threads (mDNS ...)
	a.ok = false;
}

bool NdiAvailable( std::string* where )
{
	NdiApi& a = Ndi();
	if( where ) *where = a.path;
	return a.ok;
}
std::vector< std::string > NdiSources()
{
	NdiApi& a = Ndi();
	if( !a.ok ) return {};
	StartFinder();
	std::lock_guard< std::mutex > l( a.m );
	return a.sources;
}

class NdiRecv : public ThreadedSource
{
public:
	explicit NdiRecv( const std::string& name ) : name_( name )
	{
		status_ = L( "verbinde mit " ) + name;
		th_     = std::thread( [ this ] { Run(); } );
	}
	~NdiRecv() override { StopThread(); }

private:
	void Run()
	{
		NdiApi& a = Ndi();
		if( !a.ok )
		{
			failed_ = true;
			SetStatus( L( "NDI-Runtime nicht gefunden (NDI Tools installieren)" ) );
			return;
		}
		StartFinder();
		NdiRecvCreate rc{ { name_.c_str(), nullptr }, 2, 100, false, "LightStage" };
		void* r = a.recvCreate( &rc );
		if( !r )
		{
			failed_ = true;
			SetStatus( L( "NDI-Empfänger konnte nicht erstellt werden" ) );
			return;
		}
		SetStatus( L( "warte auf " ) + name_ );
		std::vector< uint8_t > work;
		double last = Now(), fpsT = Now();
		int frames  = 0;
		bool gotAny = false;
		while( !stop_ )
		{
			NdiVideo vf{};
			int t = a.recvCapture( r, &vf, nullptr, nullptr, 100 );
			if( t == 1 )
			{
				bool rgba = vf.fourcc == Fcc( 'R', 'G', 'B', 'A' ) || vf.fourcc == Fcc( 'R', 'G', 'B', 'X' );
				bool bgra = vf.fourcc == Fcc( 'B', 'G', 'R', 'A' ) || vf.fourcc == Fcc( 'B', 'G', 'R', 'X' );
				if( vf.data && vf.xres > 0 && vf.yres > 0 && ( rgba || bgra ) )
				{
					int st = vf.stride > 0 ? vf.stride : vf.xres * 4;
					work.resize( (size_t)vf.xres * vf.yres * 4 );
					for( int y = 0; y < vf.yres; ++y ) memcpy( &work[ (size_t)y * vf.xres * 4 ], vf.data + (size_t)y * st, (size_t)vf.xres * 4 );
					Publish( work, vf.xres, vf.yres, bgra );
					++frames;
					gotAny = true;
					last   = Now();
					if( Now() - fpsT > 1.0 )
					{
						char buf[ 200 ];
						snprintf( buf, sizeof( buf ), "%s  %d x %d  %.0f fps", name_.c_str(), vf.xres, vf.yres, frames / ( Now() - fpsT ) );
						SetStatus( buf );
						frames = 0;
						fpsT   = Now();
					}
				}
				a.recvFree( r, &vf );
			}
			else if( Now() - last > 2.0 )
			{
				SetStatus( ( gotAny ? L( "keine Bilder mehr von " ) : L( "warte auf " ) ) + name_ );
				last = Now();
			}
		}
		a.recvDestroy( r );
	}
	std::string name_;
};
std::unique_ptr< VideoSource > OpenNdi( const std::string& name ) { return std::unique_ptr< VideoSource >( new NdiRecv( name ) ); }

struct NdiTestSender::Impl
{
	std::thread th;
	std::atomic< bool > stop{ false };
	void* snd = nullptr;
};
NdiTestSender::~NdiTestSender() { Stop(); }
bool NdiTestSender::Start( const std::string& name, int w, int h, const V3& color )
{
	Stop();
	NdiApi& a = Ndi();
	if( !a.ok ) return false;
	NdiSendCreate sc{ name.c_str(), nullptr, true, false };
	void* snd = a.sendCreate( &sc );
	if( !snd ) return false;
	impl_      = new Impl;
	impl_->snd = snd;
	fullName   = name;
	if( a.sendName )
		if( const NdiSrc* s = a.sendName( snd ) )
			if( s->name ) fullName = s->name;
	Impl* im = impl_;
	im->th   = std::thread( [ im, w, h, color ] {
        NdiApi& a = Ndi();
        std::vector< uint8_t > buf( (size_t)w * h * 4 );
        int f = 0;
        while( !im->stop )
        {
            for( int y = 0; y < h; ++y )
                for( int x = 0; x < w; ++x )
                {
                    uint8_t* p = &buf[ ( (size_t)y * w + x ) * 4 ];
                    bool bar   = ( ( x + f * 4 ) % w ) < w / 10;
                    p[ 0 ]     = bar ? 255 : (uint8_t)( color.x * 255 );
                    p[ 1 ]     = bar ? 255 : (uint8_t)( color.y * 255 );
                    p[ 2 ]     = bar ? 255 : (uint8_t)( color.z * 255 );
                    p[ 3 ]     = 255;
                }
            NdiVideo vf{};
            vf.xres     = w;
            vf.yres     = h;
            vf.fourcc   = Fcc( 'R', 'G', 'B', 'A' );
            vf.frN      = 30;
            vf.frD      = 1;
            vf.format   = 1;
            vf.timecode = INT64_MAX;
            vf.data     = buf.data();
            vf.stride   = w * 4;
            a.sendVideo( im->snd, &vf );//clocked: blocks to 30 fps
            ++f;
        }
	} );
	return true;
}
void NdiTestSender::Stop()
{
	if( !impl_ ) return;
	impl_->stop = true;
	if( impl_->th.joinable() ) impl_->th.join();
	Ndi().sendDestroy( impl_->snd );
	delete impl_;
	impl_ = nullptr;
}

//================================================================ hub
void VideoHub::Sync( std::vector< Projector >& projectors )
{
	std::set< uint32_t > seen;
	for( auto& p : projectors )
	{
		if( !p.uid || seen.count( p.uid ) ) p.uid = NewProjectorUid();//copies get their own source
		seen.insert( p.uid );
		std::string key;
		if( p.on )
		{
			switch( p.input )
			{
			case VideoInput::TestCard: key = "test"; break;
			case VideoInput::File: key = p.file.empty() ? "" : "file|" + p.file + ( p.loop ? "|loop" : "|once" ); break;
			case VideoInput::Ndi: key = p.ndi.empty() ? "" : "ndi|" + p.ndi; break;
			}
		}
		Slot& s = slots_[ p.uid ];
		if( s.key != key || ( key.empty() && s.src ) )
		{
			s.key = key;
			s.src.reset();
			s.frame = VideoFrame();
			if( !key.empty() )
			{
				if( p.input == VideoInput::TestCard ) s.src = MakeTestCard( p.uid );
				else if( p.input == VideoInput::File ) s.src = OpenVideoFile( p.file, p.loop );
				else s.src = OpenNdi( p.ndi );
			}
		}
		if( s.src ) s.src->Latest( s.frame );
	}
	for( auto it = slots_.begin(); it != slots_.end(); )
		it = seen.count( it->first ) ? std::next( it ) : slots_.erase( it );
}
const VideoFrame* VideoHub::Frame( uint32_t uid ) const
{
	auto it = slots_.find( uid );
	return it != slots_.end() && it->second.frame.w > 0 ? &it->second.frame : nullptr;
}
std::string VideoHub::Status( uint32_t uid ) const
{
	auto it = slots_.find( uid );
	if( it == slots_.end() ) return "";
	if( it->second.src ) return it->second.src->Status();
	return it->second.key.empty() ? L( "keine Quelle gewählt" ) : "";
}
}//namespace ls
