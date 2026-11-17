// Grosserd LightStage - DMX 3D light visualizer
// entry point: Win32 window, OpenGL 4.3 core context, Dear ImGui, command line modes
#include "app.h"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_win32.h"
#include "../third_party/stb_image_write.h"
#include <GL/wglew.h>
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>
#include <atomic>
#pragma comment( lib, "dbghelp.lib" )

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
namespace ls
{
int RunSelfTest( App& app, const std::string& dir );
}
using namespace ls;

static App* g_app = nullptr;
static bool g_running = true;

//---- hang diagnostics: heartbeat from the main loop and the window procedure (a 1 s timer keeps it
//beating inside modal dialogs). No beat for 6 s -> memory image "haenger_<time>.dmp" next to the exe.
static std::atomic< uint64_t > g_beat{ 0 };
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

static LRESULT CALLBACK WndProc( HWND h, UINT m, WPARAM w, LPARAM l )
{
	++g_beat;
	if( ImGui_ImplWin32_WndProcHandler( h, m, w, l ) ) return 1;
	switch( m )
	{
	case WM_CLOSE:
		if( g_app && !g_app->ConfirmClose() ) return 0;
		g_running = false;
		return 0;
	case WM_DROPFILES:
	{
		HDROP d = (HDROP)w;
		UINT n  = DragQueryFileW( d, 0xFFFFFFFF, nullptr, 0 );
		for( UINT i = 0; i < n; ++i )
		{
			wchar_t buf[ MAX_PATH * 4 ];
			DragQueryFileW( d, i, buf, MAX_PATH * 4 );
			if( g_app ) g_app->DropFile( Narrow( buf ) );
		}
		DragFinish( d );
		return 0;
	}
	case WM_GETMINMAXINFO:
	{
		auto* mm = (MINMAXINFO*)l;
		mm->ptMinTrackSize.x = 1024;
		mm->ptMinTrackSize.y = 640;
		return 0;
	}
	}
	return DefWindowProcW( h, m, w, l );
}

static void Style( float scale )
{
	ImGuiStyle& s      = ImGui::GetStyle();
	s.WindowRounding   = 4;
	s.ChildRounding    = 4;
	s.FrameRounding    = 3;
	s.PopupRounding    = 4;
	s.GrabRounding     = 3;
	s.TabRounding      = 3;
	s.ScrollbarRounding = 4;
	s.FramePadding     = ImVec2( 7, 4 );
	s.ItemSpacing      = ImVec2( 7, 5 );
	s.WindowBorderSize = 0;
	s.ChildBorderSize  = 1;
	s.FrameBorderSize  = 0;
	ImVec4* c = s.Colors;
	auto rgb  = []( int r, int g, int b, float a = 1 ) { return ImVec4( r / 255.0f, g / 255.0f, b / 255.0f, a ); };
	c[ ImGuiCol_Text ]                 = rgb( 222, 226, 234 );
	c[ ImGuiCol_TextDisabled ]         = rgb( 120, 128, 142 );
	c[ ImGuiCol_WindowBg ]             = rgb( 15, 16, 20 );
	c[ ImGuiCol_ChildBg ]              = rgb( 19, 20, 25 );
	c[ ImGuiCol_PopupBg ]              = rgb( 22, 24, 30, 0.98f );
	c[ ImGuiCol_Border ]               = rgb( 34, 37, 46 );
	c[ ImGuiCol_FrameBg ]              = rgb( 30, 33, 41 );
	c[ ImGuiCol_FrameBgHovered ]       = rgb( 40, 45, 57 );
	c[ ImGuiCol_FrameBgActive ]        = rgb( 46, 54, 72 );
	c[ ImGuiCol_TitleBg ]              = rgb( 15, 16, 20 );
	c[ ImGuiCol_TitleBgActive ]        = rgb( 22, 24, 30 );
	c[ ImGuiCol_MenuBarBg ]            = rgb( 12, 13, 16 );
	c[ ImGuiCol_ScrollbarBg ]          = rgb( 15, 16, 20 );
	c[ ImGuiCol_ScrollbarGrab ]        = rgb( 44, 48, 60 );
	c[ ImGuiCol_CheckMark ]            = rgb( 90, 165, 255 );
	c[ ImGuiCol_SliderGrab ]           = rgb( 70, 140, 235 );
	c[ ImGuiCol_SliderGrabActive ]     = rgb( 110, 175, 255 );
	c[ ImGuiCol_Button ]               = rgb( 36, 40, 51 );
	c[ ImGuiCol_ButtonHovered ]        = rgb( 50, 60, 80 );
	c[ ImGuiCol_ButtonActive ]         = rgb( 60, 100, 170 );
	c[ ImGuiCol_Header ]               = rgb( 40, 70, 120, 0.7f );
	c[ ImGuiCol_HeaderHovered ]        = rgb( 50, 85, 145, 0.8f );
	c[ ImGuiCol_HeaderActive ]         = rgb( 60, 100, 170 );
	c[ ImGuiCol_Tab ]                  = rgb( 24, 26, 33 );
	c[ ImGuiCol_TabHovered ]           = rgb( 50, 70, 110 );
	c[ ImGuiCol_TabSelected ]          = rgb( 38, 56, 92 );
	c[ ImGuiCol_TableHeaderBg ]        = rgb( 26, 28, 35 );
	c[ ImGuiCol_TableRowBgAlt ]        = rgb( 255, 255, 255, 0.025f );
	c[ ImGuiCol_Separator ]            = rgb( 38, 42, 52 );
	c[ ImGuiCol_ModalWindowDimBg ]     = rgb( 0, 0, 0, 0.55f );
	s.ScaleAllSizes( scale );
}

int WINAPI wWinMain( HINSTANCE inst, HINSTANCE, PWSTR, int )
{
	//command line
	int argc        = 0;
	LPWSTR* argvw   = CommandLineToArgvW( GetCommandLineW(), &argc );
	bool selftest   = false;
	std::string shotUi, openPath, startTab, lang, screenMask, resOut, fxTypes, lbxOut, lbiIn, lbiMode;
	bool tlDemo = false, tlWindow = false;
	float bottomH = 0;
	bool demoDecks = false, camFoh = false, fxDrawn = false;
	std::string showcaseDir;
	int startScreen = -1;
	bool demo = false, selectAll = false, oscWin = false, swapDlg = false, ablWin = false, resWin = false, lbxWin = false;
	int frames = 0, shotW = 1720, shotH = 1000;
	float shotScale = 0;
	for( int i = 1; i < argc; ++i )
	{
		std::string a = Narrow( argvw[ i ] );
		if( a == "--selftest" ) selftest = true;
		else if( a == "--shotui" && i + 1 < argc ) shotUi = Narrow( argvw[ ++i ] );
		else if( a == "--frames" && i + 1 < argc ) frames = _wtoi( argvw[ ++i ] );
		else if( a == "--size" && i + 2 < argc ) { shotW = _wtoi( argvw[ i + 1 ] ); shotH = _wtoi( argvw[ i + 2 ] ); i += 2; }
		else if( a == "--scale" && i + 1 < argc ) shotScale = (float)_wtof( argvw[ ++i ] );
		else if( a == "--tab" && i + 1 < argc ) startTab = Narrow( argvw[ ++i ] );
		else if( a == "--demo" ) demo = true;
		else if( a == "--tldemo" ) tlDemo = true;
		else if( a == "--tlwindow" ) tlWindow = true;
		else if( a == "--bottom" && i + 1 < argc ) bottomH = (float)_wtof( argvw[ ++i ] );
		else if( a == "--fxtypes" && i + 1 < argc ) fxTypes = Narrow( argvw[ ++i ] );//screenshots: e.g. 10,9,11 (FxType numbers), the last one selected
		else if( a == "--decks" ) demoDecks = true;
		else if( a == "--camfoh" ) camFoh = true;
		else if( a == "--fxdrawn" ) fxDrawn = true;
		else if( a == "--showcase" && i + 1 < argc ) showcaseDir = Narrow( argvw[ ++i ] );//writes the 5 example shows and quits
		else if( a == "--selectall" ) selectAll = true;
		else if( a == "--oscwindow" ) oscWin = true;
		else if( a == "--reswindow" ) resWin = true;
		else if( a == "--lightshowwindow" ) lbxWin = true;//screenshots: Lightshow export window
		else if( a == "--importlightshow" && i + 1 < argc ) lbiIn = Narrow( argvw[ ++i ] );//opens the Lightshow import window with this file
		else if( a == "--importmode" && i + 1 < argc ) lbiMode = Narrow( argvw[ ++i ] );//screenshots / tests: import at once (lanes, track, lanes-current, track-current)
		else if( a == "--resolume" && i + 1 < argc ) resOut = Narrow( argvw[ ++i ] );//write the Advanced Output preset of the show and quit
		else if( a == "--ablwindow" ) ablWin = true;
		else if( a == "--lightshow" && i + 1 < argc ) lbxOut = Narrow( argvw[ ++i ] );//export the show timeline as a Lightshow (.lightshow) and quit
		else if( a == "--swap" ) swapDlg = true;
		else if( a == "--lang" && i + 1 < argc ) lang = Narrow( argvw[ ++i ] );
		else if( a == "--screen" && i + 1 < argc ) startScreen = _wtoi( argvw[ ++i ] );
		else if( a == "--screenmask" && i + 1 < argc ) screenMask = Narrow( argvw[ ++i ] );
		else if( a.size() && a[ 0 ] != '-' ) openPath = a;
	}
	LocalFree( argvw );
	//UI language before anything builds names: settings.json "language", --lang overrides, the self test runs in German
	{
		std::string txt;
		Json j;
		if( ReadText( ExeDir() + "\\settings.json", txt ) && ParseJson( txt, j, nullptr ) ) SetLang( j[ "language" ].Str( "en" ) );
	}
	if( !lang.empty() ) SetLang( lang );
	if( selftest ) gLang = kLangDe;
	SetProcessDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );

	WNDCLASSEXW wc{};
	wc.cbSize        = sizeof( wc );
	wc.style         = CS_OWNDC;
	wc.lpfnWndProc   = WndProc;
	wc.hInstance     = inst;
	wc.hCursor       = LoadCursor( nullptr, IDC_ARROW );
	wc.hIcon         = LoadIconW( inst, MAKEINTRESOURCEW( 1 ) );
	wc.hIconSm       = (HICON)LoadImageW( inst, MAKEINTRESOURCEW( 1 ), IMAGE_ICON, GetSystemMetrics( SM_CXSMICON ), GetSystemMetrics( SM_CYSMICON ), 0 );
	wc.lpszClassName = L"GrosserdLightStage";
	RegisterClassExW( &wc );
	int sw = GetSystemMetrics( SM_CXSCREEN ), sh = GetSystemMetrics( SM_CYSCREEN );
	int ww = std::min( 1720, sw - 80 ), wh = std::min( 1020, sh - 80 );
	if( !shotUi.empty() ) { ww = shotW; wh = shotH; }
	HWND hwnd = CreateWindowExW( 0, wc.lpszClassName, L"Grosserd LightStage", WS_OVERLAPPEDWINDOW, ( sw - ww ) / 2, ( sh - wh ) / 2, ww, wh, nullptr, nullptr, inst, nullptr );
	DragAcceptFiles( hwnd, TRUE );
	HDC dc = GetDC( hwnd );
	PIXELFORMATDESCRIPTOR pfd{};
	pfd.nSize      = sizeof( pfd );
	pfd.nVersion   = 1;
	pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;
	pfd.cDepthBits = 24;
	int pf         = ChoosePixelFormat( dc, &pfd );
	SetPixelFormat( dc, pf, &pfd );
	HGLRC legacy = wglCreateContext( dc );
	wglMakeCurrent( dc, legacy );
	glewExperimental = GL_TRUE;
	glewInit();
	HGLRC rc = legacy;
	if( WGLEW_ARB_create_context )
	{
		int attrs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, 3, WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
		HGLRC core  = wglCreateContextAttribsARB( dc, nullptr, attrs );
		if( core )
		{
			wglMakeCurrent( dc, core );
			wglDeleteContext( legacy );
			rc = core;
			glewExperimental = GL_TRUE;
			glewInit();
			glGetError();
		}
	}
	const char* ver = (const char*)glGetString( GL_VERSION );
	const char* ren = (const char*)glGetString( GL_RENDERER );
	Log( "OpenGL %s on %s", ver ? ver : "?", ren ? ren : "?" );
	if( !GLEW_VERSION_4_3 )
	{
		MessageBoxA( hwnd, L( "OpenGL 4.3 wird benoetigt (Grafiktreiber aktualisieren)." ), "LightStage", MB_ICONERROR );
		return 1;
	}
	if( WGLEW_EXT_swap_control ) wglSwapIntervalEXT( shotUi.empty() ? 1 : 0 );

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io    = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	float scale = GetDpiForWindow( hwnd ) / 96.0f;
	if( scale < 1.0f ) scale = 1.0f;
	if( shotScale > 0 ) scale = shotScale;
	Style( scale );
	ImGui_ImplWin32_Init( hwnd );
	ImGui_ImplOpenGL3_Init( "#version 430" );

	App app;
	g_app            = &app;
	app.ui           = scale;
	std::string fdir = ExeDir() + "\\assets\\fonts\\";
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
	if( !app.Init( hwnd, &err ) )
	{
		Log( "init failed: %s", err.c_str() );
		MessageBoxA( hwnd, ( L( "Renderer konnte nicht starten:\n" ) + err ).c_str(), "LightStage", MB_ICONERROR );
		return 1;
	}
	if( selftest )
	{
		int r = RunSelfTest( app, ExeDir() );
		app.Shutdown();
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplWin32_Shutdown();
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
		CreateDirectoryW( Widen( showcaseDir ).c_str(), nullptr );
		int ok = 0;
		for( int k = 0; k < 5; ++k )
		{
			std::string title, e;
			if( !app.BuildShowcase( k, title ) ) continue;
			const std::string file = showcaseDir + "\\" + title + ".lightstage";
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
			const std::string file = showcaseDir + "\\06 Show Time Line - Cues, Clips & Fans.lightstage";
			if( app.show.Save( file, &e ) ) ++ok;
			Log( "showcase 6: %s %s", file.c_str(), e.c_str() );
		}
		app.Shutdown();
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		return ok == 6 ? 0 : 1;
	}
	if( !resOut.empty() )
	{
		ResolumeOpts ro;
		auto lay = BuildResolumeLayout( app.show, ro );
		std::string e;
		const bool ok = WriteResolumePreset( resOut, ResolumePresetXml( lay, ro ), e );
		app.Shutdown();
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		return ok ? 0 : 1;
	}
	app.startTab   = startTab;
	app.noAutosave = !shotUi.empty() || !showcaseDir.empty() || !resOut.empty() || !lbxOut.empty();
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
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		return ok ? 0 : 1;
	}
	if( tlWindow ) app.ShowTimelineWindow();
	if( bottomH > 0 ) app.SetBottomHeight( bottomH );
	if( camFoh ) { app.cam.yaw = 30; app.cam.pitch = 24; app.cam.dist = 30; app.cam.target = V3( 0, 1.5f, 5 ); }
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
	if( !lbiIn.empty() && !lbiDone ) app.LbiOpen( lbiIn );
	if( ablWin ) app.ShowAbletonWindow();
	if( swapDlg ) app.OpenSwapDialog( app.sel );
	ShowWindow( hwnd, SW_SHOWDEFAULT );
	UpdateWindow( hwnd );
	SetTimer( hwnd, 77, 1000, nullptr );//heartbeat inside modal loops
	if( shotUi.empty() ) StartHangWatch();

	//normal start: user settings (performance mode) and the input engine thread
	Log( "start: Einstellungen" );
	app.LoadSettings();
	Log( "start: Engine" );
	app.StartEngine();
	Log( "start: Hauptschleife" );
	int swapInterval = shotUi.empty() ? 1 : 0;
	double last = Now();
	int frame   = 0;
	std::string lastTitle;
	while( g_running && !app.WantsQuit() )
	{
		++g_beat;
		MSG msg;
		while( PeekMessageW( &msg, nullptr, 0, 0, PM_REMOVE ) )
		{
			TranslateMessage( &msg );
			DispatchMessageW( &msg );
		}
		if( !g_running ) break;
		if( IsIconic( hwnd ) )
		{
			Sleep( 20 );
			last = Now();
			continue;
		}
		double now = Now();
		const double frameStart = now;
		float dt   = (float)Clamp( now - last, 0.0, 0.1 );
		last       = now;
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		ImGui::PushFont( app.fontUi );
		app.Frame( shotUi.empty() ? dt : 1.0f / 60.0f );
		ImGui::PopFont();
		ImGui::Render();
		RECT r;
		GetClientRect( hwnd, &r );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		glViewport( 0, 0, r.right, r.bottom );
		glClearColor( 0.06f, 0.06f, 0.07f, 1 );
		glClear( GL_COLOR_BUFFER_BIT );
		ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );
		++frame;
		if( !shotUi.empty() && frame >= std::max( frames, 240 ) )
		{
			std::vector< uint8_t > px( (size_t)r.right * r.bottom * 4 );
			glReadBuffer( GL_BACK );
			glPixelStorei( GL_PACK_ALIGNMENT, 1 );
			glReadPixels( 0, 0, r.right, r.bottom, GL_RGBA, GL_UNSIGNED_BYTE, px.data() );
			std::vector< uint8_t > flip( px.size() );
			for( int y = 0; y < r.bottom; ++y ) memcpy( &flip[ (size_t)y * r.right * 4 ], &px[ (size_t)( r.bottom - 1 - y ) * r.right * 4 ], (size_t)r.right * 4 );
			for( size_t i = 3; i < flip.size(); i += 4 ) flip[ i ] = 255;
			stbi_write_png( shotUi.c_str(), r.right, r.bottom, 4, flip.data(), r.right * 4 );
			break;
		}
		SwapBuffers( dc );
		//performance mode: no vsync, the UI runs at 30 fps while used and 10 fps when idle (wakes up on input)
		{
			const double iv = shotUi.empty() ? app.FrameInterval() : 0.0;
			const int want  = shotUi.empty() ? ( iv > 0 ? 0 : 1 ) : 0;
			if( want != swapInterval && WGLEW_EXT_swap_control )
			{
				wglSwapIntervalEXT( want );
				swapInterval = want;
			}
			while( iv > 0 && g_running )
			{
				double left = frameStart + iv - Now();
				if( left <= 0.0005 ) break;
				DWORD r = MsgWaitForMultipleObjects( 0, nullptr, FALSE, (DWORD)std::max( 1.0, left * 1000.0 ), QS_ALLINPUT );
				if( r == WAIT_OBJECT_0 ) break;//input or other messages: handle them now
			}
		}
		std::string title = "Grosserd LightStage - " + ( app.show.path.empty() ? std::string( L( "Unbenannt" ) ) : FileName( app.show.path ) ) + ( app.show.dirty ? " *" : "" );
		if( title != lastTitle )
		{
			SetWindowTextW( hwnd, Widen( title ).c_str() );
			lastTitle = title;
		}
	}
	//closing: the window goes away at once; if a driver (Enttec, MIDI, NDI, video) blocks the clean-up,
	//a watchdog ends the process after 4 s (the show was saved / confirmed before)
	ShowWindow( hwnd, SW_HIDE );
	std::thread( [] {
		Sleep( 4000 );
		WriteHangDump( "Beenden haengt seit 4 s" );
		Log( "shutdown: haengt seit 4 s, Prozess wird beendet" );
		ExitProcess( 0 );
	} ).detach();
	app.Shutdown();
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	wglMakeCurrent( nullptr, nullptr );
	wglDeleteContext( rc );
	ReleaseDC( hwnd, dc );
	DestroyWindow( hwnd );
	return 0;
}
