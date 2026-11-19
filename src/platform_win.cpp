// Grosserd LightStage - platform layer, Windows implementation (Win32)
#include "platform.h"
#include "common.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <avrt.h>
#include <powrprof.h>

namespace ls
{
static GLFWwindow* g_win = nullptr;
static HWND g_hwnd       = nullptr;

void PlatformInit( GLFWwindow* win )
{
	g_win  = win;
	g_hwnd = win ? glfwGetWin32Window( win ) : nullptr;
}
GLFWwindow* PlatformWindow() { return g_win; }
void* NativeWindowHandle() { return g_hwnd; }

//---------------------------------------------------------------- dialogs
static std::string FileDialog( bool save, const FileFilters& filters, const std::string& defExt, const std::string& initial )
{
	//filter as the Win32 \0 list: label\0patterns\0 ... \0
	std::wstring flt;
	for( const FileFilter& f : filters )
	{
		flt += Widen( f.label );
		flt += L'\0';
		flt += Widen( f.patterns );
		flt += L'\0';
	}
	flt += L'\0';
	const std::wstring ext = Widen( defExt );
	wchar_t buf[ MAX_PATH * 4 ] = {};
	if( !initial.empty() )
	{
		std::wstring w = Widen( initial );
		wcsncpy( buf, w.c_str(), MAX_PATH * 4 - 1 );
	}
	OPENFILENAMEW of{};
	of.lStructSize = sizeof( of );
	of.hwndOwner   = g_hwnd;
	of.lpstrFilter = filters.empty() ? nullptr : flt.c_str();
	of.lpstrFile   = buf;
	of.nMaxFile    = MAX_PATH * 4;
	of.lpstrDefExt = ext.empty() ? nullptr : ext.c_str();
	of.Flags       = OFN_NOCHANGEDIR | OFN_EXPLORER | ( save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST );
	BOOL ok        = save ? GetSaveFileNameW( &of ) : GetOpenFileNameW( &of );
	return ok ? Narrow( buf ) : std::string();
}
std::string OpenFileDialog( const FileFilters& filters, const std::string& defExt, const std::string& initial ) { return FileDialog( false, filters, defExt, initial ); }
std::string SaveFileDialog( const FileFilters& filters, const std::string& defExt, const std::string& initial ) { return FileDialog( true, filters, defExt, initial ); }

int AskYesNoCancel( const std::string& text )
{
	const int r = MessageBoxW( g_hwnd, Widen( text ).c_str(), L"LightStage", MB_YESNOCANCEL | MB_ICONQUESTION );
	return r == IDYES ? kAnswerYes : r == IDNO ? kAnswerNo : kAnswerCancel;
}
void ShowError( const std::string& text ) { MessageBoxW( nullptr, Widen( text ).c_str(), L"LightStage", MB_ICONERROR ); }
void OpenUrl( const std::string& url ) { ShellExecuteW( nullptr, L"open", Widen( url ).c_str(), nullptr, nullptr, SW_SHOWNORMAL ); }

bool CursorInWindow( float& x, float& y )
{
	POINT pt{};
	if( !g_hwnd || !GetCursorPos( &pt ) || !ScreenToClient( g_hwnd, &pt ) ) return false;
	x = (float)pt.x;
	y = (float)pt.y;
	return true;
}

//---------------------------------------------------------------- wake event
void* EventCreate() { return CreateEventW( nullptr, FALSE, FALSE, nullptr ); }
void EventSignal( void* ev )
{
	if( ev ) SetEvent( (HANDLE)ev );
}
void EventWait( void* ev, int ms )
{
	if( ev ) WaitForSingleObject( (HANDLE)ev, (DWORD)ms );
	else Sleep( (DWORD)ms );
}
void EventDestroy( void* ev )
{
	if( ev ) CloseHandle( (HANDLE)ev );
}

//---------------------------------------------------------------- priorities, power
void TimerPrecision( bool fine )
{
	if( fine ) timeBeginPeriod( 1 );
	else timeEndPeriod( 1 );
}
void* ThreadRealtime( bool on, void* token )
{
	if( on )
	{
		DWORD idx = 0;
		SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL );
		return AvSetMmThreadCharacteristicsW( L"Pro Audio", &idx );
	}
	if( token ) AvRevertMmThreadCharacteristics( (HANDLE)token );
	SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_NORMAL );
	return nullptr;
}
void ProcessPerformance( bool on )
{
	SetPriorityClass( GetCurrentProcess(), on ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS );
	//no Windows "efficiency mode" throttling of this process while live
	PROCESS_POWER_THROTTLING_STATE pt{};
	pt.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
	pt.ControlMask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
	pt.StateMask   = 0;
	SetProcessInformation( GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof( pt ) );
}
std::string PowerPlanName()
{
	std::string r;
	GUID* g = nullptr;
	if( PowerGetActiveScheme( nullptr, &g ) == ERROR_SUCCESS )
	{
		wchar_t nm[ 256 ] = {};
		DWORD sz          = sizeof( nm );
		if( PowerReadFriendlyName( nullptr, g, nullptr, nullptr, (UCHAR*)nm, &sz ) == ERROR_SUCCESS ) r = Narrow( nm );
		LocalFree( g );
	}
	return r;
}
double ProcessCpuSeconds()
{
	FILETIME c, e, k, u;
	if( !GetProcessTimes( GetCurrentProcess(), &c, &e, &k, &u ) ) return 0;
	const ULONGLONG K = ( (ULONGLONG)k.dwHighDateTime << 32 ) | k.dwLowDateTime, U = ( (ULONGLONG)u.dwHighDateTime << 32 ) | u.dwLowDateTime;
	return ( K + U ) * 1e-7;
}
int CpuCount()
{
	SYSTEM_INFO si;
	GetSystemInfo( &si );
	return std::max< int >( 1, (int)si.dwNumberOfProcessors );
}
}//namespace ls
