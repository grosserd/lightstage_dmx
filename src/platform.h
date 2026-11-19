// Grosserd LightStage - platform layer: native dialogs, the main window, URLs, thread / process priorities.
// Windows: platform_win.cpp (Win32), macOS: platform_mac.mm (Cocoa). Strings are UTF-8.
#pragma once
#include <string>
#include <vector>

struct GLFWwindow;

namespace ls
{
//file type filter of a dialog: label "Bilder (*.png;*.jpg)", patterns "*.png;*.jpg" ("*.*" = all files)
struct FileFilter
{
	std::string label, patterns;
};
using FileFilters = std::vector< FileFilter >;
//from a \0 list "Label\0*.a;*.b\0Label 2\0*.*\0" - the form L( "..." ) returns (translated as one entry)
inline FileFilters FilterList( const char* list )
{
	FileFilters r;
	while( list && *list )
	{
		FileFilter f;
		f.label = list;
		list += f.label.size() + 1;
		if( !*list ) break;
		f.patterns = list;
		list += f.patterns.size() + 1;
		r.push_back( f );
	}
	return r;
}

//the main window (owner of dialogs); call once after it is created
void PlatformInit( GLFWwindow* win );
GLFWwindow* PlatformWindow();
void* NativeWindowHandle();//HWND / NSWindow*, nullptr before PlatformInit

//modal file dialogs: the chosen path or "" (cancelled). defExt without dot is appended when the name has none;
//initial = suggested file (path or bare name)
std::string OpenFileDialog( const FileFilters& filters, const std::string& defExt = "", const std::string& initial = "" );
std::string SaveFileDialog( const FileFilters& filters, const std::string& defExt = "", const std::string& initial = "" );

enum DialogAnswer { kAnswerYes = 0, kAnswerNo = 1, kAnswerCancel = 2 };
int AskYesNoCancel( const std::string& text );//question owned by the main window
void ShowError( const std::string& text );//error box (also before the window exists)
void OpenUrl( const std::string& url );//default browser

//mouse position relative to the main window's client area (ImGui coordinates); false without a window
bool CursorInWindow( float& x, float& y );

//auto-reset wake event (engine thread: MIDI / OSC input wakes it). On Windows the handle is a Win32 event
//(SetEvent works on it); everywhere else use EventSignal.
void* EventCreate();
void EventSignal( void* ev );
void EventWait( void* ev, int ms );//returns when signalled or after ms
void EventDestroy( void* ev );

//1 ms timer resolution for the calling process (Windows timeBeginPeriod); no-op where not needed
void TimerPrecision( bool fine );
//real-time priority for the calling thread (input / DMX loops). on: returns a token, pass it back with on = false
void* ThreadRealtime( bool on, void* token );
//performance mode: higher process priority, no power throttling / App Nap
void ProcessPerformance( bool on );
std::string PowerPlanName();//active power plan / energy mode, "" if unknown
double ProcessCpuSeconds();//user + kernel time of this process
int CpuCount();
}//namespace ls
