// Grosserd LightStage - DMX 3D light visualizer
// entry point: GLFW window, OpenGL core context (4.3 on Windows, 4.1 on macOS), Dear ImGui, command line modes
#include "app.h"
#include "imgui.h"
#include "platform.h"
#include <GL/glew.h>
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_glfw.h"
#include "../third_party/stb_image_write.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>
#pragma comment( lib, "dbghelp.lib" )
#endif

namespace ls
{
int RunSelfTest( App& app, const std::string& dir );
}
using namespace ls;

static App* g_app = nullptr;
static bool g_running = true;
static bool g_uiTest  = false;//scripted UI test: the real mouse / keyboard / focus changes do not reach the UI

//---- hang diagnostics: heartbeat from the main loop and the window procedure (a 1 s timer keeps it
//beating inside modal dialogs). No beat for 6 s -> memory image "haenger_<time>.dmp" next to the exe.
static std::atomic< uint64_t > g_beat{ 0 };
#ifdef _WIN32
static void WriteHangDump( const char* why )
{
	SYSTEMTIME t;
	GetLocalTime( &t );
	char n[ 64 ];
	snprintf( n, sizeof( n ), "\\haenger_%04d%02d%02d_%02d%02d%02d.dmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond );
	const std::string p = ExeDir() + n;
	HANDLE f            = CreateFileW( Widen( p ).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr );
	if( f == INVALID_HANDLE_VALUE ) return;
	const BOOL ok = MiniDumpWriteDump( GetCurrentProcess(), GetCurrentProcessId(), f, (MINIDUMP_TYPE)( MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithHandleData ), nullptr, nullptr, nullptr );
	CloseHandle( f );
	Log( "Haenger (%s): Abbild %s %s", why, p.c_str(), ok ? "geschrieben" : "fehlgeschlagen" );
}
static void StartHangWatch()
{
	std::thread( [] {
		uint64_t last = g_beat;
		int still     = 0;
		for( ;; )
		{
			Sleep( 1000 );
			const uint64_t b = g_beat;
			if( b != last ) still = 0;
			else if( ++still == 6 ) WriteHangDump( "Programm reagiert seit 6 s nicht" );
			last = b;
		}
	} ).detach();
}

//heartbeat inside modal loops (file dialogs, moving / sizing the window): a thread timer keeps beating there
static void CALLBACK HeartbeatTimer( HWND, UINT, UINT_PTR, DWORD ) { ++g_beat; }
#else
static void WriteHangDump( const char* why ) { Log( "Haenger (%s)", why ); }
static void StartHangWatch() {}
#endif

//error before / instead of the main window
static bool g_noDialogs = false;//self test / scripted / command line modes: errors go to the log, never a modal dialog (nobody could close it)
static void FatalMessage( const std::string& msg )
{
	Log( "%s", msg.c_str() );
	if( g_noDialogs ) fprintf( stderr, "LightStage: %s\n", msg.c_str() );
	else ShowError( msg );
}

//---- GLFW callbacks. The UI test (g_uiTest) does not install the ImGui input callbacks: the real mouse,
//keyboard and focus changes never reach the UI, only the script does.
static void OnClose( GLFWwindow* w )
{
	++g_beat;
	if( g_app && !g_app->ConfirmClose() )
	{
		glfwSetWindowShouldClose( w, GLFW_FALSE );
		return;
	}
	g_running = false;
}
static void OnDrop( GLFWwindow*, int n, const char** paths )
{
	++g_beat;
	for( int i = 0; i < n; ++i )
		if( g_app && paths[ i ] ) g_app->DropFile( paths[ i ] );
}
static void OnGlfwError( int code, const char* text ) { Log( "GLFW %d: %s", code, text ? text : "" ); }

static void Style( float scale )
{
	//UI refresh (11/2026): softer layered surfaces, more rounding, a little more air; accent blue 90/165/255 stays
	ImGuiStyle& s       = ImGui::GetStyle();
	s.WindowRounding    = 8;
	s.ChildRounding     = 8;
	s.FrameRounding     = 5;
	s.PopupRounding     = 8;
	s.GrabRounding      = 4;
	s.TabRounding       = 6;
	s.ScrollbarRounding = 8;
	s.ScrollbarSize     = 11;
	s.GrabMinSize       = 10;
	s.FramePadding      = ImVec2( 8, 4 );
	s.ItemSpacing       = ImVec2( 7, 5 );
	s.ItemInnerSpacing  = ImVec2( 6, 4 );
	s.CellPadding       = ImVec2( 5, 3 );
	s.WindowPadding     = ImVec2( 12, 10 );
	s.WindowBorderSize  = 1;//floating windows + popups get a fine outline (the host window switches it off)
	s.PopupBorderSize   = 1;
	s.ChildBorderSize   = 1;
	s.FrameBorderSize   = 0;
	s.TabBorderSize     = 0;
	s.TabBarBorderSize  = 1;
	s.TabBarOverlineSize = 2;
	s.SeparatorTextBorderSize = 1;
	s.SeparatorTextPadding    = ImVec2( 12, 3 );
	s.WindowTitleAlign  = ImVec2( 0.0f, 0.5f );
	s.WindowMenuButtonPosition = ImGuiDir_None;
	s.DisabledAlpha     = 0.45f;
	ImVec4* c = s.Colors;
	auto rgb  = []( int r, int g, int b, float a = 1 ) { return ImVec4( r / 255.0f, g / 255.0f, b / 255.0f, a ); };
	c[ ImGuiCol_Text ]                 = rgb( 226, 230, 238 );
	c[ ImGuiCol_TextDisabled ]         = rgb( 124, 132, 148 );
	c[ ImGuiCol_WindowBg ]             = rgb( 12, 13, 17 );
	c[ ImGuiCol_ChildBg ]              = rgb( 20, 22, 28 );
	c[ ImGuiCol_PopupBg ]              = rgb( 24, 26, 34, 0.985f );
	c[ ImGuiCol_Border ]               = rgb( 255, 255, 255, 0.075f );
	c[ ImGuiCol_BorderShadow ]         = rgb( 0, 0, 0, 0 );
	c[ ImGuiCol_FrameBg ]              = rgb( 33, 36, 46 );
	c[ ImGuiCol_FrameBgHovered ]       = rgb( 42, 47, 61 );
	c[ ImGuiCol_FrameBgActive ]        = rgb( 46, 56, 78 );
	c[ ImGuiCol_TitleBg ]              = rgb( 18, 20, 26 );
	c[ ImGuiCol_TitleBgActive ]        = rgb( 26, 30, 40 );
	c[ ImGuiCol_TitleBgCollapsed ]     = rgb( 18, 20, 26 );
	c[ ImGuiCol_MenuBarBg ]            = rgb( 12, 13, 17 );
	c[ ImGuiCol_ScrollbarBg ]          = rgb( 0, 0, 0, 0 );
	c[ ImGuiCol_ScrollbarGrab ]        = rgb( 255, 255, 255, 0.13f );
	c[ ImGuiCol_ScrollbarGrabHovered ] = rgb( 255, 255, 255, 0.22f );
	c[ ImGuiCol_ScrollbarGrabActive ]  = rgb( 90, 165, 255, 0.75f );
	c[ ImGuiCol_CheckMark ]            = rgb( 90, 165, 255 );
	c[ ImGuiCol_SliderGrab ]           = rgb( 78, 150, 245 );
	c[ ImGuiCol_SliderGrabActive ]     = rgb( 120, 185, 255 );
	c[ ImGuiCol_Button ]               = rgb( 38, 42, 54 );
	c[ ImGuiCol_ButtonHovered ]        = rgb( 52, 60, 80 );
	c[ ImGuiCol_ButtonActive ]         = rgb( 58, 98, 168 );
	c[ ImGuiCol_Header ]               = rgb( 60, 110, 190, 0.38f );
	c[ ImGuiCol_HeaderHovered ]        = rgb( 80, 130, 210, 0.30f );
	c[ ImGuiCol_HeaderActive ]         = rgb( 70, 125, 215, 0.60f );
	c[ ImGuiCol_Tab ]                  = rgb( 0, 0, 0, 0 );
	c[ ImGuiCol_TabHovered ]           = rgb( 255, 255, 255, 0.07f );
	c[ ImGuiCol_TabSelected ]          = rgb( 90, 165, 255, 0.16f );
	c[ ImGuiCol_TabSelectedOverline ]  = rgb( 90, 165, 255 );
	c[ ImGuiCol_TabDimmed ]            = rgb( 0, 0, 0, 0 );
	c[ ImGuiCol_TabDimmedSelected ]    = rgb( 90, 165, 255, 0.10f );
	c[ ImGuiCol_TabDimmedSelectedOverline ] = rgb( 90, 165, 255, 0.5f );
	c[ ImGuiCol_TableHeaderBg ]        = rgb( 27, 30, 38 );
	c[ ImGuiCol_TableBorderStrong ]    = rgb( 255, 255, 255, 0.09f );
	c[ ImGuiCol_TableBorderLight ]     = rgb( 255, 255, 255, 0.05f );
	c[ ImGuiCol_TableRowBgAlt ]        = rgb( 255, 255, 255, 0.022f );
	c[ ImGuiCol_Separator ]            = rgb( 255, 255, 255, 0.08f );
	c[ ImGuiCol_SeparatorHovered ]     = rgb( 90, 165, 255, 0.6f );
	c[ ImGuiCol_SeparatorActive ]      = rgb( 90, 165, 255 );
	c[ ImGuiCol_ResizeGrip ]           = rgb( 255, 255, 255, 0.08f );
	c[ ImGuiCol_ResizeGripHovered ]    = rgb( 90, 165, 255, 0.6f );
	c[ ImGuiCol_ResizeGripActive ]     = rgb( 90, 165, 255 );
	c[ ImGuiCol_TextSelectedBg ]       = rgb( 90, 165, 255, 0.35f );
	c[ ImGuiCol_DragDropTarget ]       = rgb( 255, 170, 60 );
	c[ ImGuiCol_NavCursor ]            = rgb( 90, 165, 255, 0.8f );
	c[ ImGuiCol_ModalWindowDimBg ]     = rgb( 0, 0, 0, 0.6f );
	s.ScaleAllSizes( scale );
}

static int RunLightStage( const std::vector< std::string >& av )
{
	//command line (UTF-8)
	const int argc  = (int)av.size();
	bool selftest   = false;
	std::string uiTest;//scripted UI test: log file (uitest.cpp)
	std::string shotUi, openPath, startTab, lang, screenMask, resOut, fxTypes, lbxOut, lbiIn, lbiMode, lbxUi;
	bool tlDemo = false, tlWindow = false;
	float bottomH = 0;
	bool demoDecks = false, camFoh = false, fxDrawn = false;
	std::string showcaseDir;
	std::string vidOut;//--video: offline video export of the loaded show, then quit (promo clips, tests)
	float vidSecs = 8, vidFrom = 0;
	int vidCam = 2;
	int startScreen = -1;
	bool demo = false, selectAll = false, oscWin = false, swapDlg = false, ablWin = false, resWin = false, lbxWin = false;
	int frames = 0, shotW = 1720, shotH = 1000;
	float shotScale = 0;
	for( int i = 1; i < argc; ++i )
	{
		std::string a = av[ i ];
		if( a == "--selftest" ) selftest = true;
		else if( a == "--shotui" && i + 1 < argc ) shotUi = av[ ++i ];
		else if( a == "--uitest" && i + 1 < argc ) uiTest = av[ ++i ];
		else if( a == "--frames" && i + 1 < argc ) frames = atoi( av[ ++i ].c_str() );
		else if( a == "--size" && i + 2 < argc ) { shotW = atoi( av[ i + 1 ].c_str() ); shotH = atoi( av[ i + 2 ].c_str() ); i += 2; }
		else if( a == "--scale" && i + 1 < argc ) shotScale = (float)atof( av[ ++i ].c_str() );
		else if( a == "--tab" && i + 1 < argc ) startTab = av[ ++i ];
		else if( a == "--demo" ) demo = true;
		else if( a == "--tldemo" ) tlDemo = true;
		else if( a == "--tlwindow" ) tlWindow = true;
		else if( a == "--bottom" && i + 1 < argc ) bottomH = (float)atof( av[ ++i ].c_str() );
		else if( a == "--fxtypes" && i + 1 < argc ) fxTypes = av[ ++i ];//screenshots: e.g. 10,9,11 (FxType numbers), the last one selected
		else if( a == "--decks" ) demoDecks = true;
		else if( a == "--camfoh" ) camFoh = true;
		else if( a == "--fxdrawn" ) fxDrawn = true;
		else if( a == "--showcase" && i + 1 < argc ) showcaseDir = av[ ++i ];//writes the 5 example shows and quits
		else if( a == "--selectall" ) selectAll = true;
		else if( a == "--oscwindow" ) oscWin = true;
		else if( a == "--reswindow" ) resWin = true;
		else if( a == "--lightshowwindow" ) lbxWin = true;
		else if( a == "--lightshowui" && i + 1 < argc ) lbxUi = av[ ++i ];//test: export like the button (UI frame, engine running), then quit//screenshots: Lightshow export window
		else if( a == "--importlightshow" && i + 1 < argc ) lbiIn = av[ ++i ];//opens the Lightshow import window with this file
		else if( a == "--importmode" && i + 1 < argc ) lbiMode = av[ ++i ];//screenshots / tests: import at once (lanes, track, lanes-current, track-current)
		else if( a == "--resolume" && i + 1 < argc ) resOut = av[ ++i ];//write the Advanced Output preset of the show and quit
		else if( a == "--ablwindow" ) ablWin = true;
		else if( a == "--lightshow" && i + 1 < argc ) lbxOut = av[ ++i ];//export the show timeline as a Lightshow (.lightshow) and quit
		else if( a == "--video" && i + 1 < argc ) vidOut = av[ ++i ];
		else if( a == "--videosecs" && i + 1 < argc ) vidSecs = (float)atof( av[ ++i ].c_str() );
		else if( a == "--videofrom" && i + 1 < argc ) vidFrom = (float)atof( av[ ++i ].c_str() );
		else if( a == "--videocam" && i + 1 < argc ) vidCam = atoi( av[ ++i ].c_str() );
		else if( a == "--swap" ) swapDlg = true;
		else if( a == "--lang" && i + 1 < argc ) lang = av[ ++i ];
		else if( a == "--screen" && i + 1 < argc ) startScreen = atoi( av[ ++i ].c_str() );
		else if( a == "--screenmask" && i + 1 < argc ) screenMask = av[ ++i ];
		else if( a.size() && a[ 0 ] != '-' ) openPath = a;
	}
	const bool scripted = !shotUi.empty() || !uiTest.empty();//fixed window size and frame time, no vsync, no autosave
	g_uiTest            = !uiTest.empty();
	g_noDialogs         = selftest || scripted || !showcaseDir.empty() || !resOut.empty() || !lbxOut.empty() || !vidOut.empty();
	//UI language before anything builds names: settings.json "language", --lang overrides, the self test runs in German
	{
		std::string txt;
		Json j;
		if( ReadText( ExeDir() + LS_SEP "settings.json", txt ) && ParseJson( txt, j, nullptr ) ) SetLang( j[ "language" ].Str( "en" ) );
	}
	if( !lang.empty() ) SetLang( lang );
	if( selftest ) gLang = kLangDe;
#ifdef _WIN32
	SetProcessDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
#endif
	glfwSetErrorCallback( OnGlfwError );
	if( !glfwInit() )
	{
		FatalMessage( L( "Fenstersystem konnte nicht starten (GLFW)." ) );
		return 1;
	}
	//OpenGL core profile: 4.3 on Windows / Linux, 4.1 on macOS (the highest Apple offers; the shaders need nothing newer)
#ifdef __APPLE__
	const int glMinor        = 1;
	const char* glslVersion  = "#version 410";
	glfwWindowHint( GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE );
#else
	//LS_GL41=1: the macOS path (OpenGL 4.1 core, GLSL 410) on Windows / Linux, for tests
	const bool gl41          = getenv( "LS_GL41" ) != nullptr;
	const int glMinor        = gl41 ? 1 : 3;
	const char* glslVersion  = gl41 ? "#version 410" : "#version 430";
	if( gl41 ) glfwWindowHint( GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE );
#endif
	glfwWindowHint( GLFW_CONTEXT_VERSION_MAJOR, 4 );
	glfwWindowHint( GLFW_CONTEXT_VERSION_MINOR, glMinor );
	glfwWindowHint( GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE );
	glfwWindowHint( GLFW_DEPTH_BITS, 24 );
	glfwWindowHint( GLFW_VISIBLE, GLFW_FALSE );//shown after the start-up work below
	int sw = 1920, sh = 1080;
	if( const GLFWvidmode* vm = glfwGetVideoMode( glfwGetPrimaryMonitor() ) ) { sw = vm->width; sh = vm->height; }
	int ww = std::min( 1720, sw - 80 ), wh = std::min( 1020, sh - 80 );
	if( scripted ) { ww = shotW; wh = shotH; }
	GLFWwindow* win = glfwCreateWindow( ww, wh, "Grosserd LightStage", nullptr, nullptr );
	if( !win )
	{
		FatalMessage( glMinor == 3 ? L( "OpenGL 4.3 wird benoetigt (Grafiktreiber aktualisieren)." ) : L( "OpenGL 4.1 wird benoetigt." ) );
		glfwTerminate();
		return 1;
	}
	glfwSetWindowPos( win, ( sw - ww ) / 2, ( sh - wh ) / 2 );
	glfwSetWindowSizeLimits( win, 1024, 640, GLFW_DONT_CARE, GLFW_DONT_CARE );
	glfwSetWindowCloseCallback( win, OnClose );
	glfwSetDropCallback( win, OnDrop );
	glfwMakeContextCurrent( win );
	glewExperimental = GL_TRUE;
	glewInit();
	glGetError();//GLEW on a core profile leaves GL_INVALID_ENUM behind
#ifdef _WIN32
	HWND hwnd = glfwGetWin32Window( win );
	{
		HINSTANCE inst = GetModuleHandleW( nullptr );
		SendMessageW( hwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW( inst, MAKEINTRESOURCEW( 1 ) ) );
		SendMessageW( hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW( inst, MAKEINTRESOURCEW( 1 ), IMAGE_ICON, GetSystemMetrics( SM_CXSMICON ), GetSystemMetrics( SM_CYSMICON ), 0 ) );
	}
#endif
	PlatformInit( win );//file dialogs and message boxes are owned by it
	const char* ver = (const char*)glGetString( GL_VERSION );
	const char* ren = (const char*)glGetString( GL_RENDERER );
	Log( "OpenGL %s on %s", ver ? ver : "?", ren ? ren : "?" );
	glfwSwapInterval( scripted ? 0 : 1 );

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io    = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	float scale = 1.0f;
#ifndef __APPLE__
	//Windows / Linux: the window works in pixels, the UI is scaled by the monitor's content scale (96 dpi = 1).
	//macOS works in points and renders Retina through the framebuffer scale, so 1 is right there.
	{
		float sx = 1, sy = 1;
		glfwGetWindowContentScale( win, &sx, &sy );
		scale = sx;
	}
#endif
	if( scale < 1.0f ) scale = 1.0f;
	if( shotScale > 0 ) scale = shotScale;
	Style( scale );
	ImGui_ImplGlfw_InitForOpenGL( win, !g_uiTest );
	ImGui_ImplOpenGL3_Init( glslVersion );

	App app;
	g_app            = &app;
	app.ui           = scale;
	std::string fdir = ExeDir() + LS_SEP "assets" LS_SEP "fonts" LS_SEP;
	ImFontConfig fc;
	fc.OversampleH = 2;
	//Latin + Latin-1, general punctuation (› – … •), arrows, a few math signs (× ≤ ≥)
	static const ImWchar kRanges[] = { 0x0020, 0x00FF, 0x0100, 0x017F, 0x2000, 0x206F, 0x2190, 0x21FF, 0x2200, 0x22FF, 0 };
	app.fontUi     = io.Fonts->AddFontFromFileTTF( ( fdir + "IBMPlexSans.ttf" ).c_str(), 16.0f * scale, &fc, kRanges );
	if( !app.fontUi ) app.fontUi = io.Fonts->AddFontDefault();
	app.fontMono = io.Fonts->AddFontFromFileTTF( ( fdir + "JetBrainsMono.ttf" ).c_str(), 13.5f * scale, &fc );
	if( !app.fontMono ) app.fontMono = app.fontUi;
	app.fontBig = io.Fonts->AddFontFromFileTTF( ( fdir + "Unbounded.ttf" ).c_str(), 15.0f * scale, &fc );
	if( !app.fontBig ) app.fontBig = app.fontUi;

	std::string err;
	if( !app.Init( &err ) )
	{
		Log( "init failed: %s", err.c_str() );
		FatalMessage( L( "Renderer konnte nicht starten:\n" ) + err );
		return 1;
	}
	if( selftest )
	{
		app.quality_ = app.qualityEff_ = 1;//normal = the 2.0 look, no throttling (image checks)
		app.qualityAuto_ = false;
		int r = RunSelfTest( app, ExeDir() );
		app.Shutdown();
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		return r;
	}
	if( !openPath.empty() )
	{
		const std::string lo = ToLower( openPath );
		if( lo.size() > 10 && lo.compare( lo.size() - 10, 10, ".lightshow" ) == 0 ) lbiIn = openPath;//double-clicked Lightshow: import window
		else app.OpenShow( openPath );
	}
	else if( demo ) app.BuildDemo();
	if( !showcaseDir.empty() )
	{
		MakeDir( showcaseDir );
		int ok = 0;
		for( int k = 0; k < 6; ++k )
		{
			std::string title, e;
			if( !app.BuildShowcase( k, title ) ) continue;
			const std::string file = showcaseDir + LS_SEP + title + ".lightstage";
			if( app.show.Save( file, &e ) ) ++ok;
			Log( "showcase %d: %s %s", k + 1, file.c_str(), e.c_str() );
		}
		//2.0: the demo stage with its show time line (without the generated click track, it lives in the temp folder)
		{
			app.BuildDemo();
			app.BuildTimelineDemo();
			auto& tr = app.show.tl.tracks;
			for( size_t i = tr.size(); i-- > 0; )
				if( tr[ i ].kind == TlTrackKind::Audio ) tr.erase( tr.begin() + i );
			for( auto& t : tr )
				for( auto& l : t.lanes )
				{
					l.rec = false;
					for( auto& k : l.keys ) k.sel = false;
				}
			app.show.tl.Locate( app.show, 0 );
			std::string e;
			const std::string file = showcaseDir + LS_SEP + "06 Show Time Line - Cues, Clips & Fans.lightstage";
			if( app.show.Save( file, &e ) ) ++ok;
			Log( "showcase 6: %s %s", file.c_str(), e.c_str() );
		}
		app.Shutdown();
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		return ok == 7 ? 0 : 1;
	}
	if( !resOut.empty() )
	{
		ResolumeOpts ro;
		auto lay = BuildResolumeLayout( app.show, ro );
		std::string e;
		const bool ok = WriteResolumePreset( resOut, ResolumePresetXml( lay, ro ), e );
		app.Shutdown();
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		return ok ? 0 : 1;
	}
	app.startTab   = startTab;
	app.noAutosave = scripted || !showcaseDir.empty() || !resOut.empty() || !lbxOut.empty() || !vidOut.empty();
	if( demoDecks )
	{
		Deck d;
		d.name = "Drum-Riser"; d.z = -2.2f; d.width = 3; d.depth = 2.4f; d.height = 0.6f; d.onDeck = app.show.stage.deck; d.stairs = 1;
		d.edge = V3( 0.2f, 0.45f, 1.0f ); d.edgeLevel = 2;
		app.show.stage.decks.push_back( d );
		Deck c;
		c.name = "Catwalk"; c.z = app.show.stage.depth * 0.5f + 4; c.width = 1.6f; c.depth = 8; c.height = app.show.stage.deck ? app.show.stage.deckHeight : 1.0f;
		app.show.stage.decks.push_back( c );
		Deck b;
		b.name = "B-Stage"; b.shape = 1; b.z = app.show.stage.depth * 0.5f + 10.5f; b.width = 5; b.depth = 5; b.height = c.height; b.stairs = 3;
		b.edge = V3( 1.0f, 0.4f, 0.15f ); b.edgeLevel = 1.5f;
		app.show.stage.decks.push_back( b );
	}
	if( tlDemo ) app.BuildTimelineDemo();
	bool lbiDone = false;
	if( !lbiIn.empty() && !lbiMode.empty() && app.LbiOpen( lbiIn ) )
	{
		app.lbiOpt_.asTrack = lbiMode.rfind( "track", 0 ) == 0;
		app.lbiOpt_.newShow = lbiMode.find( "current" ) == std::string::npos;
		app.show.dirty      = false;//no save question
		app.LbiRun();
		lbiDone = true;
	}
	if( !lbxOut.empty() )
	{
		std::string e;
		const bool ok = LbxExport( app.show, app.sim, LbxOptions(), lbxOut, &e );
		Log( "lightshow export %s: %s %s", lbxOut.c_str(), ok ? "ok" : "failed", e.c_str() );
		app.Shutdown();
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		return ok ? 0 : 1;
	}
	if( tlWindow ) app.ShowTimelineWindow();
	if( bottomH > 0 ) app.SetBottomHeight( bottomH );
	if( camFoh ) { app.cam.yaw = 30; app.cam.pitch = 24; app.cam.dist = 30; app.cam.target = V3( 0, 1.5f, 5 ); }
	if( !vidOut.empty() )
	{
		app.exp_.res      = 1;//1920x1080
		app.exp_.fpsIdx   = 1;//30 fps
		app.exp_.seconds  = vidSecs;
		app.exp_.camMode  = vidCam;
		app.exp_.timeline = true;
		app.exp_.tlStart  = vidFrom;
		bool ok = app.ExportStart( vidOut );
		while( ok && app.ExportStep( 1.0 ) ) {}
		ok = ok && app.exp_.err.empty();
		app.ExportFinish( ok );
		Log( "video export %s: %s %s", vidOut.c_str(), ok ? "ok" : "failed", app.exp_.err.c_str() );
		app.Shutdown();
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		return ok ? 0 : 1;
	}
	if( !fxTypes.empty() )
	{
		size_t p = 0;
		while( p < fxTypes.size() )
		{
			const int t = atoi( fxTypes.c_str() + p );
			if( t >= 0 && t < (int)FxType::Count ) app.AddEffectOfType( (FxType)t );
			p = fxTypes.find( ',', p );
			if( p == std::string::npos ) break;
			++p;
		}
		if( Effect* e = app.show.FindEffect( app.show.fxSelId ) )
			if( fxDrawn )
			{
				//screenshot: a drawn wave with every point type
				e->wave = FxWave::Custom;
				e->pts  = { { 0, 0.1f, 1 }, { 0.18f, 1, 0 }, { 0.35f, 0.4f, 2 }, { 0.55f, 0.85f, 2 }, { 0.72f, 0.2f, 3 }, { 0.85f, 0.6f, 1 } };
				e->hi   = 1;
				e->envDrawn = true;
				if( e->type == FxType::Envelope ) { e->gate = 0.8f; e->pts = { { 0, 0, 0 }, { 0.08f, 1, 1 }, { 0.3f, 0.55f, 2 }, { 0.6f, 0.7f, 2 }, { 1, 0, 1 } }; }
			}
		if( Effect* e = app.show.FindEffect( app.show.fxSelId ) )
			if( e->type == FxType::Sequencer )
			{
				//a richer pattern for the picture
				e->lanes.push_back( FxNewLane( *e, 1 ) );
				e->lanes.back().tgt.attr = Attr::Red;
				e->lanes.push_back( FxNewLane( *e, 2 ) );
				e->lanes.back().tgt.mode = 2;
				e->lanes.back().tgt.ch   = 400;
				e->lanes.back().tgt.count = 4;
				for( int i = 0; i < e->steps; ++i ) e->lanes.back().v[ i ] = 0.5f + 0.5f * sinf( i * 0.6f );
				e->lanes[ 1 ].glide = true;
			}
	}
	if( startScreen >= 0 && startScreen < 8 )//screenshots: make sure the screen (and a projector on it) exists
	{
		while( (int)app.show.screens.size() <= startScreen ) app.show.AddScreen();
		if( app.show.projectors.empty() ) app.show.AddProjector();
		app.startScreen = startScreen;
		std::string e;
		if( !screenMask.empty() && LoadScreenMask( app.show.screens[ startScreen ], screenMask, e ) )
		{
			app.show.screens[ startScreen ].format = 4;
			app.show.screens[ startScreen ].height = app.show.screens[ startScreen ].width;
		}
	}
	if( selectAll )
		for( auto& f : app.show.fixtures ) app.sel.push_back( f.id );
	if( oscWin ) app.ShowOscWindow();
	if( resWin ) app.ShowResolumeWindow();
	if( lbxWin ) app.ShowLbxWindow();
	if( !lbxUi.empty() ) { app.lbxUiTest = lbxUi; app.noAutosave = true; }
	if( !lbiIn.empty() && !lbiDone ) app.LbiOpen( lbiIn );
	if( ablWin ) app.ShowAbletonWindow();
	if( swapDlg ) app.OpenSwapDialog( app.sel );
	glfwShowWindow( win );
#ifdef _WIN32
	SetTimer( nullptr, 0, 1000, HeartbeatTimer );//heartbeat inside modal loops
#endif
	if( !scripted ) StartHangWatch();

	//normal start: user settings (performance mode) and the input engine thread
	Log( "start: Einstellungen" );
	if( uiTest.empty() ) app.LoadSettings();//the UI test never reads or writes the user's settings
	else app.UiTestStart( uiTest );
	Log( "start: Engine" );
	app.StartEngine();
	Log( "start: Hauptschleife" );
	int swapInterval = scripted ? 0 : 1;
	int uiTestRc     = 0;
	double last = Now();
	int frame   = 0;
	std::string lastTitle;
	while( g_running && !app.WantsQuit() )
	{
		++g_beat;
		glfwPollEvents();
		if( !g_running ) break;
		if( glfwGetWindowAttrib( win, GLFW_ICONIFIED ) )
		{
			glfwWaitEventsTimeout( 0.02 );
			last = Now();
			continue;
		}
		double now = Now();
		const double frameStart = now;
		float dt   = (float)Clamp( now - last, 0.0, 0.1 );
		last       = now;
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		if( g_uiTest ) app.UiTestInput();//after the backend: the script's mouse wins over the real one
		ImGui::NewFrame();
		ImGui::PushFont( app.fontUi );
		app.Frame( scripted ? 1.0f / 60.0f : dt );
		ImGui::PopFont();
		if( g_uiTest && app.UiTestDone() ) { Log( "ui test: end, %d FAIL", app.UiTestFails() ); uiTestRc = app.UiTestFails() ? 1 : 0; break; }
		if( app.lbxUiState >= 2 ) { Log( "lightshow ui test: end (state %d)", app.lbxUiState ); break; }
		ImGui::Render();
		int fbW = 0, fbH = 0;
		glfwGetFramebufferSize( win, &fbW, &fbH );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		glViewport( 0, 0, fbW, fbH );
		glClearColor( 0.06f, 0.06f, 0.07f, 1 );
		glClear( GL_COLOR_BUFFER_BIT );
		ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );
		++frame;
		if( !shotUi.empty() && frame >= std::max( frames, 240 ) )
		{
			std::vector< uint8_t > px( (size_t)fbW * fbH * 4 );
			glReadBuffer( GL_BACK );
			glPixelStorei( GL_PACK_ALIGNMENT, 1 );
			glReadPixels( 0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, px.data() );
			std::vector< uint8_t > flip( px.size() );
			for( int y = 0; y < fbH; ++y ) memcpy( &flip[ (size_t)y * fbW * 4 ], &px[ (size_t)( fbH - 1 - y ) * fbW * 4 ], (size_t)fbW * 4 );
			for( size_t i = 3; i < flip.size(); i += 4 ) flip[ i ] = 255;
			stbi_write_png( shotUi.c_str(), fbW, fbH, 4, flip.data(), fbW * 4 );
			break;
		}
		glfwSwapBuffers( win );
		//performance mode: no vsync, the UI runs at 30 fps while used and 10 fps when idle (wakes up on input)
		{
			const double iv = scripted ? 0.0 : app.FrameInterval();
			const int want  = scripted ? 0 : ( iv > 0 ? 0 : 1 );
			if( want != swapInterval )
			{
				glfwSwapInterval( want );
				swapInterval = want;
			}
			if( iv > 0 && g_running )
			{
				//sleep until the frame is due; any input wakes up at once and is handled in the next frame
				const double left = frameStart + iv - Now();
				if( left > 0.0005 ) glfwWaitEventsTimeout( left );
			}
		}
		std::string title = "Grosserd LightStage - " + ( app.show.path.empty() ? std::string( L( "Unbenannt" ) ) : FileName( app.show.path ) ) + ( app.show.dirty ? " *" : "" );
		if( title != lastTitle )
		{
			glfwSetWindowTitle( win, title.c_str() );
			lastTitle = title;
		}
	}
	//closing: the window goes away at once; if a driver (Enttec, MIDI, NDI, video) blocks the clean-up,
	//a watchdog ends the process after 4 s (the show was saved / confirmed before)
	glfwHideWindow( win );
	std::thread( [] {
		std::this_thread::sleep_for( std::chrono::seconds( 4 ) );
		WriteHangDump( "Beenden haengt seit 4 s" );
		Log( "shutdown: haengt seit 4 s, Prozess wird beendet" );
		std::_Exit( 0 );
	} ).detach();
	app.Shutdown();
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow( win );
	glfwTerminate();
	return uiTestRc;
}

#ifdef _WIN32
int WINAPI wWinMain( HINSTANCE, HINSTANCE, PWSTR, int )
{
	int argc      = 0;
	LPWSTR* argvw = CommandLineToArgvW( GetCommandLineW(), &argc );
	std::vector< std::string > av;
	for( int i = 0; i < argc; ++i ) av.push_back( Narrow( argvw[ i ] ) );
	LocalFree( argvw );
	return RunLightStage( av );
}
#else
int main( int argc, char** argv )
{
	return RunLightStage( std::vector< std::string >( argv, argv + argc ) );
}
#endif
