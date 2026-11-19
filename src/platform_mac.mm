// Grosserd LightStage - platform layer, macOS implementation (Cocoa / Foundation / POSIX).
// Compiled with ARC (CMakeLists.txt). Nothing here has run yet: everything is MACOS-UNTESTED until the first
// build in GitHub Actions; the spots with the most doubt carry their own mark.
#include "platform.h"
#include "common.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <sys/qos.h>
#include <sys/resource.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace ls
{
static GLFWwindow* g_win = nullptr;

void PlatformInit( GLFWwindow* win ) { g_win = win; }
GLFWwindow* PlatformWindow() { return g_win; }
void* NativeWindowHandle() { return g_win ? (__bridge void*)glfwGetCocoaWindow( g_win ) : nullptr; }

static NSString* Ns( const std::string& s )
{
	NSString* r = [NSString stringWithUTF8String:s.c_str()];
	return r ? r : @"";
}
static void RestoreFocus()
{
	if( !g_win ) return;
	NSWindow* w = (NSWindow*)glfwGetCocoaWindow( g_win );
	[w makeKeyAndOrderFront:nil];
}

//---------------------------------------------------------------- dialogs
//extensions of all filters ("*.png;*.jpg" -> png, jpg); a filter "*.*" / "*" only counts when it is the only one
static std::vector< std::string > Extensions( const FileFilters& filters )
{
	std::vector< std::string > r;
	for( const FileFilter& f : filters )
	{
		size_t a = 0;
		while( a <= f.patterns.size() )
		{
			size_t b = f.patterns.find( ';', a );
			if( b == std::string::npos ) b = f.patterns.size();
			std::string p = f.patterns.substr( a, b - a );
			while( !p.empty() && p.front() == ' ' ) p.erase( 0, 1 );
			if( p.size() > 2 && p.compare( 0, 2, "*." ) == 0 && p != "*.*" )
			{
				p = ToLower( p.substr( 2 ) );
				if( std::find( r.begin(), r.end(), p ) == r.end() ) r.push_back( p );
			}
			a = b + 1;
		}
	}
	return r;
}
static void SetTypes( NSSavePanel* panel, const std::vector< std::string >& ext )
{
	if( ext.empty() ) return;//all files
	if( @available( macOS 11.0, * ) )
	{
		//MACOS-UNTESTED: own extensions (lightstage, lsmidi, lightshow ...) become dynamic UTTypes
		NSMutableArray< UTType* >* t = [NSMutableArray array];
		for( const std::string& e : ext )
		{
			UTType* u = [UTType typeWithFilenameExtension:Ns( e )];
			if( u ) [t addObject:u];
		}
		if( t.count ) panel.allowedContentTypes = t;
	}
	else
	{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		NSMutableArray< NSString* >* t = [NSMutableArray array];
		for( const std::string& e : ext ) [t addObject:Ns( e )];
		panel.allowedFileTypes = t;
#pragma clang diagnostic pop
	}
}
static std::string FileDialog( bool save, const FileFilters& filters, const std::string& defExt, const std::string& initial )
{
	std::string result;
	@autoreleasepool
	{
		NSSavePanel* panel = nil;
		if( save )
		{
			NSSavePanel* sp = [NSSavePanel savePanel];
			sp.canCreateDirectories = YES;
			if( !initial.empty() ) sp.nameFieldStringValue = Ns( FileName( initial ) );
			panel = sp;
		}
		else
		{
			NSOpenPanel* op = [NSOpenPanel openPanel];
			op.canChooseFiles          = YES;
			op.canChooseDirectories    = NO;
			op.allowsMultipleSelection = NO;
			panel                      = op;
		}
		const std::string dir = DirName( initial );
		if( !dir.empty() ) panel.directoryURL = [NSURL fileURLWithPath:Ns( dir ) isDirectory:YES];
		SetTypes( panel, Extensions( filters ) );
		if( [panel runModal] == NSModalResponseOK && panel.URL ) result = panel.URL.path.UTF8String;
		RestoreFocus();
	}
	//like the Win32 dialog: the default extension when the name has none
	if( save && !result.empty() && !defExt.empty() && FileName( result ).find( '.' ) == std::string::npos ) result += "." + defExt;
	return result;
}
std::string OpenFileDialog( const FileFilters& filters, const std::string& defExt, const std::string& initial ) { return FileDialog( false, filters, defExt, initial ); }
std::string SaveFileDialog( const FileFilters& filters, const std::string& defExt, const std::string& initial ) { return FileDialog( true, filters, defExt, initial ); }

int AskYesNoCancel( const std::string& text )
{
	int r = kAnswerCancel;
	@autoreleasepool
	{
		NSAlert* a    = [[NSAlert alloc] init];
		a.alertStyle  = NSAlertStyleWarning;
		a.messageText = @"LightStage";
		a.informativeText = Ns( text );
		[a addButtonWithTitle:Ns( L( "Ja" ) )];
		[a addButtonWithTitle:Ns( L( "Nein" ) )];
		NSButton* c       = [a addButtonWithTitle:Ns( L( "Abbrechen" ) )];
		c.keyEquivalent   = @"\033";//Esc
		const NSModalResponse m = [a runModal];
		r = m == NSAlertFirstButtonReturn ? kAnswerYes : m == NSAlertSecondButtonReturn ? kAnswerNo : kAnswerCancel;
		RestoreFocus();
	}
	return r;
}
void ShowError( const std::string& text )
{
	@autoreleasepool
	{
		[NSApplication sharedApplication];//may come before GLFW has set up the application
		NSAlert* a        = [[NSAlert alloc] init];
		a.alertStyle      = NSAlertStyleCritical;
		a.messageText     = @"LightStage";
		a.informativeText = Ns( text );
		[a runModal];
	}
}
void OpenUrl( const std::string& url )
{
	@autoreleasepool
	{
		NSURL* u = [NSURL URLWithString:Ns( url )];
		if( u ) [[NSWorkspace sharedWorkspace] openURL:u];
	}
}

bool CursorInWindow( float& x, float& y )
{
	if( !g_win ) return false;
	double cx = 0, cy = 0;
	glfwGetCursorPos( g_win, &cx, &cy );//window coordinates in points = ImGui coordinates on macOS
	x = (float)cx;
	y = (float)cy;
	return true;
}

//---------------------------------------------------------------- wake event
namespace
{
struct WakeEvent
{
	std::mutex m;
	std::condition_variable cv;
	bool set = false;
};
}//namespace
void* EventCreate() { return new WakeEvent; }
void EventSignal( void* ev )
{
	if( !ev ) return;
	WakeEvent* e = (WakeEvent*)ev;
	{
		std::lock_guard< std::mutex > l( e->m );
		e->set = true;
	}
	e->cv.notify_one();
}
void EventWait( void* ev, int ms )
{
	if( !ev ) { SleepMs( ms ); return; }
	WakeEvent* e = (WakeEvent*)ev;
	std::unique_lock< std::mutex > l( e->m );
	e->cv.wait_for( l, std::chrono::milliseconds( ms ), [ e ] { return e->set; } );
	e->set = false;//auto reset
}
void EventDestroy( void* ev ) { delete (WakeEvent*)ev; }

//---------------------------------------------------------------- priorities, power
void TimerPrecision( bool ) {}//the macOS timers are fine-grained already

void* ThreadRealtime( bool on, void* token )
{
	const thread_port_t th = pthread_mach_thread_np( pthread_self() );
	if( on )
	{
		//MACOS-UNTESTED: time-constraint (real-time) policy for a 1 kHz loop: 1 ms period, up to 0.25 ms work
		mach_timebase_info_data_t tb;
		mach_timebase_info( &tb );
		auto toAbs = [ & ]( double ns ) { return (uint32_t)( ns * tb.denom / tb.numer ); };
		thread_time_constraint_policy_data_t p;
		p.period      = toAbs( 1.0e6 );
		p.computation = toAbs( 0.25e6 );
		p.constraint  = toAbs( 1.0e6 );
		p.preemptible = TRUE;
		if( thread_policy_set( th, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT ) == KERN_SUCCESS ) return (void*)1;
		pthread_set_qos_class_self_np( QOS_CLASS_USER_INTERACTIVE, 0 );
		return (void*)2;
	}
	if( token == (void*)1 )
	{
		thread_standard_policy_data_t sp{};
		thread_policy_set( th, THREAD_STANDARD_POLICY, (thread_policy_t)&sp, THREAD_STANDARD_POLICY_COUNT );
	}
	else if( token == (void*)2 )
		pthread_set_qos_class_self_np( QOS_CLASS_DEFAULT, 0 );
	return nullptr;
}
void ProcessPerformance( bool on )
{
	//no App Nap / timer coalescing, no idle system sleep while live (NSActivityUserInitiated includes
	//NSActivityIdleSystemSleepDisabled). A higher process priority (nice < 0) needs root, so it is left out.
	static id< NSObject > activity = nil;
	@autoreleasepool
	{
		if( on && !activity )
			activity = [[NSProcessInfo processInfo] beginActivityWithOptions:( NSActivityUserInitiated | NSActivityLatencyCritical ) reason:@"LightStage performance mode"];
		else if( !on && activity )
		{
			[[NSProcessInfo processInfo] endActivity:activity];
			activity = nil;
		}
	}
}
std::string PowerPlanName()
{
	if( @available( macOS 12.0, * ) ) return [NSProcessInfo processInfo].lowPowerModeEnabled ? L( "Stromsparmodus an" ) : L( "Normal" );
	return "";
}
double ProcessCpuSeconds()
{
	rusage ru{};
	if( getrusage( RUSAGE_SELF, &ru ) != 0 ) return 0;
	return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + ( ru.ru_utime.tv_usec + ru.ru_stime.tv_usec ) * 1e-6;
}
int CpuCount() { return std::max( 1, (int)std::thread::hardware_concurrency() ); }
}//namespace ls
