#include "app.h"
#include "../third_party/stb_image_write.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "json.h"
#include <algorithm>
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <map>
#include <mmsystem.h>
#include <avrt.h>
#include <powrprof.h>

namespace ls
{
static const char* QualityName( int q ) { return q <= 0 ? L( "Entwurf" ) : q == 1 ? L( "Normal" ) : L( "Hoch" ); }
//---------------------------------------------------------------- file dialogs
static std::string FileDialog( void* hwnd, bool save, const wchar_t* filter, const wchar_t* defExt, const std::string& initial = "" )
{
	wchar_t buf[ MAX_PATH * 4 ] = {};
	if( !initial.empty() )
	{
		std::wstring w = Widen( initial );
		wcsncpy( buf, w.c_str(), MAX_PATH * 4 - 1 );
	}
	OPENFILENAMEW of{};
	of.lStructSize = sizeof( of );
	of.hwndOwner   = (HWND)hwnd;
	of.lpstrFilter = filter;
	of.lpstrFile   = buf;
	of.nMaxFile    = MAX_PATH * 4;
	of.lpstrDefExt = defExt;
	of.Flags       = OFN_NOCHANGEDIR | OFN_EXPLORER | ( save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST );
	BOOL ok        = save ? GetSaveFileNameW( &of ) : GetOpenFileNameW( &of );
	return ok ? Narrow( buf ) : std::string();
}
#define kShowFilter LW( L"LightStage Show (*.lightstage)\0*.lightstage\0Alle Dateien\0*.*\0" )
#define kModelFilter LW( L"3D-Modelle (*.glb;*.gltf;*.fbx;*.obj)\0*.glb;*.gltf;*.fbx;*.obj\0Alle Dateien\0*.*\0" )
static const wchar_t* kGdtfFilter  = L"GDTF Fixture (*.gdtf)\0*.gdtf\0";

static ImU32 FixtureColor( int id, float a = 1.0f )
{
	V3 c = Hsv( fmodf( id * 0.1618f, 1.0f ), 0.55f, 0.85f );
	return ImGui::ColorConvertFloat4ToU32( ImVec4( c.x, c.y, c.z, a ) );
}

//================================================================ lifecycle
bool App::Init( void* wnd, std::string* err )
{
	hwnd   = wnd;
	exeDir = ExeDir();
	lib.LoadAll( exeDir + "\\library" );
	if( lib.defs.empty() ) Log( "warning: fixture library is empty (%s\\library)", exeDir.c_str() );
	if( !ren.Init( err ) ) return false;
	midi.Open();
	ports_ = ListComPorts();
	//start with the demo show (2.1: the jazz club; main may replace it by a file or --demo)
	BuildJazzDemo();
	io.Start( &show );
	return true;
}
void App::Shutdown()
{
	tlAudio_.Stop();
	ltcIn_.Stop();
	AutosaveClear();//ended normally (unsaved changes were declined in ConfirmClose)
	double t = Now();
	auto lap = [ & ]( const char* what ) {
		const double n = Now();
		if( n - t > 0.05 ) Log( "shutdown: %s %.0f ms", what, ( n - t ) * 1000 );
		t = n;
	};
	StopEngine();
	lap( "engine" );
	bridge.Stop();
	lap( "bridge" );
	link.reset();
	lap( "link" );
	video.Clear();
	NdiShutdown();
	lap( "video + NDI" );
	io.Stop();
	lap( "dmx" );
	midi.Close();
	lap( "midi" );
	osc.Stop();
	lap( "osc" );
	ren.Shutdown();
	lap( "render" );
}
bool App::ConfirmClose()
{
	if( !show.dirty ) return true;
	int r = MessageBoxW( (HWND)hwnd, LW( L"Die Show hat ungespeicherte Änderungen. Speichern?" ), L"LightStage", MB_YESNOCANCEL | MB_ICONQUESTION );
	if( r == IDCANCEL ) return false;
	if( r == IDYES )
	{
		std::string p = show.path;
		if( p.empty() ) p = FileDialog( hwnd, true, kShowFilter, L"lightstage" );
		if( p.empty() ) return false;
		return SaveShow( p );
	}
	return true;
}
void App::SetStatus( const std::string& s )
{
	status     = s;
	statusTime = Now();
	Log( "%s", s.c_str() );
}
void App::NewShow()
{
	show.Clear();
	show.AddUniverse();
	Truss t;
	t.a = V3( -6, 7, -2 );
	t.b = V3( 6, 7, -2 );
	show.trusses.push_back( t );
	sel.clear();
	show.fxSelId = 0;
	//standard MIDI mapping (MIDI tab > "Als Standard speichern")
	if( GetFileAttributesW( Widen( MidiDefaultFile() ).c_str() ) != INVALID_FILE_ATTRIBUTES ) ImportMidi( MidiDefaultFile(), true );
	show.dirty = false;
	io.Apply();
	UndoReset();
	AutosaveClear();
}
bool App::RegisterEmbeddedFixture( const std::string& id, const Json& ofl )
{
	//id = user/<manufacturer>/<name>
	size_t a = id.find( '/' ), b = id.find( '/', a + 1 );
	if( a == std::string::npos || b == std::string::npos || !ofl.IsObj() ) return false;
	const std::string mf = id.substr( a + 1, b - a - 1 ), key = id.substr( b + 1 );
	const std::string root = exeDir + "\\library";
	std::string mfName     = ofl[ "lightstageEditor" ][ "manufacturer" ].Str( mf );
	RegisterUserManufacturer( root, mf, mfName );
	CreateDirectoryW( Widen( root + "\\user\\" + mf ).c_str(), nullptr );
	std::string path = root + "\\user\\" + mf + "\\" + key + ".json", err;
	if( !WriteText( path, WriteJson( ofl, true ) ) ) return false;
	auto nd = std::make_unique< FixtureDef >();
	if( !ParseOflFixture( ofl, id, mfName, *nd, &err ) ) return false;
	nd->source = path;
	lib.AddOrReplace( std::move( nd ) );
	lib.mfNames[ mf ] = mfName;
	add_.lastSearch   = "\x01";
	return true;
}
bool App::OpenShow( const std::string& path )
{
	std::string err;
	//own fixtures embedded in the show and missing here: into library\user first
	{
		std::string txt;
		Json j;
		int n = 0;
		if( ReadText( path, txt ) && ParseJson( txt, j ) )
			for( auto& kv : j[ "userFixtures" ].o )
				if( !lib.Find( kv.first ) && RegisterEmbeddedFixture( kv.first, kv.second ) ) ++n;
		if( n ) Log( "%d eigene Lampe(n) aus der Show in die Bibliothek übernommen", n );
	}
	if( !show.Load( path, lib, &err ) )
	{
		SetStatus( L( "Laden fehlgeschlagen: " ) + err );
		return false;
	}
	for( auto& m : show.models ) ren.EnsureModel( m, nullptr );
	if( show.hasView )//start view of the show (example shows)
	{
		cam.target = show.viewTarget;
		cam.yaw    = show.viewYaw;
		cam.pitch  = show.viewPitch;
		cam.dist   = show.viewDist;
	}
	sel.clear();
	io.Apply();
	UndoReset();
	AutosaveClear();
	SetStatus( err.empty() ? L( "Show geladen: " ) + FileName( path ) : L( "Geladen mit Warnung: " ) + err );
	return true;
}
bool App::SaveShow( const std::string& path )
{
	ls::rs::ResetOriginals();//double-click goes back to the saved value
	std::string err;
	if( !show.Save( path, &err ) )
	{
		SetStatus( L( "Speichern fehlgeschlagen: " ) + err );
		return false;
	}
	SetStatus( L( "Gespeichert: " ) + FileName( path ) );
	AutosaveClear();
	return true;
}
void App::DropFile( const std::string& path )
{
	std::lock_guard< std::recursive_mutex > engineGuard( engineLock_ );//called from the window procedure too
	undoTouched_ = true;//dropped by the user: an undo step
	std::string l = ToLower( path );
	auto ends     = [ & ]( const char* e ) { size_t n = strlen( e ); return l.size() >= n && l.compare( l.size() - n, n, e ) == 0; };
	if( ends( ".lightstage" ) )
	{
		if( ConfirmClose() ) OpenShow( path );
	}
	else if( ends( ".lightshow" ) )
		LbiOpen( path );
	else if( ends( ".gdtf" ) )
	{
		const FixtureDef* d = lib.LoadGdtf( path );
		if( d )
		{
			add_.open    = true;
			add_.replace = false;
			add_.def  = d;
			SetStatus( L( "GDTF importiert: " ) + d->name );
		}
		else
			SetStatus( L( "GDTF-Fehler: " ) + lib.lastError );
	}
	else if( ends( ".wav" ) || ends( ".mp3" ) || ends( ".aif" ) || ends( ".aiff" ) || ends( ".m4a" ) || ends( ".aac" ) || ends( ".flac" ) || ends( ".wma" ) )
	{
		//audio file: audio track of the Time Line. Dropped onto the time area = starts at the mouse (grid), else at 0 / where it was
		double off = -1;
		POINT pt{};
		bool have = dropPt_;
		if( have ) { pt.x = (LONG)dropX_; pt.y = (LONG)dropY_; }
		else have = hwnd && GetCursorPos( &pt ) && ScreenToClient( (HWND)hwnd, &pt );
		if( have && ImGui::GetFrameCount() - tlGeo_.frame <= 2 && tlGeo_.pps > 0 && pt.x >= tlGeo_.x0 && pt.x < tlGeo_.x1 && pt.y >= tlGeo_.y0 && pt.y < tlGeo_.y1 )
			off = std::max( 0.0, show.tl.Snap( tlGeo_.view0 + ( pt.x - tlGeo_.x0 ) / tlGeo_.pps ) );
		const bool added = AddAudioTrack( path, off );
		tlSelectTab_     = true;
		SetStatus( std::string( added ? L( "Audiospur angelegt: " ) : L( "Audiodatei der Audiospur ersetzt: " ) ) + FileStem( path ) );
	}
	else if( ends( ".mp4" ) || ends( ".mov" ) || ends( ".m4v" ) || ends( ".wmv" ) || ends( ".avi" ) || ends( ".mkv" ) )
	{
		//video file: onto the selected projector (Video tab), else a new projector
		Projector* p = projSel_ >= 0 && projSel_ < (int)show.projectors.size() ? &show.projectors[ projSel_ ] : nullptr;
		if( !p )
		{
			if( show.screens.empty() ) show.AddScreen();
			p        = &show.AddProjector();
			projSel_ = (int)show.projectors.size() - 1;
		}
		p->input   = VideoInput::File;
		p->file    = path;
		show.dirty = true;
		SetStatus( L( "Video auf " ) + p->name + ": " + FileName( path ) );
	}
	else if( ends( ".glb" ) || ends( ".gltf" ) || ends( ".fbx" ) || ends( ".obj" ) )
	{
		ModelObj m;
		m.path = path;
		m.name = FileName( path );
		std::string err;
		if( ren.EnsureModel( m, &err ) )
		{
			show.models.push_back( m );
			show.dirty = true;
			SetStatus( L( "Modell importiert: " ) + m.name );
		}
		else
			SetStatus( L( "Modell-Fehler: " ) + err );
	}
}

FixtureInst* App::AddFixture( const FixtureDef* def, int mode, int uni, int addr, const V3& pos, const V3& rot, const std::string& name )
{
	if( !def ) return nullptr;
	if( show.UniIndex( uni ) < 0 )
		while( show.UniIndex( uni ) < 0 ) show.AddUniverse();
	FixtureInst f;
	f.id       = show.NextFixtureId();
	f.def      = def;
	f.defId    = def->id;
	f.mode     = Clamp( mode, 0, (int)def->modes.size() - 1 );
	f.universe = uni;
	f.address  = addr;
	f.pos      = pos;
	f.rot      = rot;
	f.name     = name.empty() ? def->name + " " + std::to_string( f.id ) : name;
	show.fixtures.push_back( f );
	FixtureInst& r = show.fixtures.back();
	show.Home( r );
	show.dirty = true;
	return &show.fixtures.back();
}

bool App::IsSel( int id ) const { return std::find( sel.begin(), sel.end(), id ) != sel.end(); }
void App::Select( int id, bool add, bool range )
{
	if( range && lastClicked_ >= 0 )
	{
		int a = -1, b = -1;
		for( int i = 0; i < (int)show.fixtures.size(); ++i )
		{
			if( show.fixtures[ i ].id == lastClicked_ ) a = i;
			if( show.fixtures[ i ].id == id ) b = i;
		}
		if( a >= 0 && b >= 0 )
		{
			if( !add ) sel.clear();
			for( int i = std::min( a, b ); i <= std::max( a, b ); ++i )
				if( !IsSel( show.fixtures[ i ].id ) ) sel.push_back( show.fixtures[ i ].id );
			return;
		}
	}
	if( add )
	{
		auto it = std::find( sel.begin(), sel.end(), id );
		if( it != sel.end() ) sel.erase( it );
		else sel.push_back( id );
	}
	else
	{
		sel.clear();
		sel.push_back( id );
	}
	lastClicked_ = id;
	pixelSel_    = -1;
}
void App::DuplicateSelected()
{
	if( sel.empty() ) return;
	show.UpdateMounts();
	std::vector< int > newSel;
	std::vector< FixtureInst > copies;
	for( auto& f : show.fixtures )
		if( IsSel( f.id ) ) copies.push_back( f );
	std::map< int, int > standMap;//old stand -> its copy (lamps sharing a stand share the copy)
	const V3 standOff( 1.2f, 0, 0 );
	for( auto& c : copies )
	{
		int addr = show.FreeAddress( c.universe, c.Footprint() );
		int uni  = c.universe;
		if( !addr ) { uni = show.AddUniverse(); addr = 1; }
		const bool onStand = c.mount == MountType::Stand && c.mountIdx >= 0 && c.mountIdx < (int)show.stands.size();
		const bool onTruss = c.mount == MountType::Truss && c.mountIdx >= 0 && c.mountIdx < (int)show.trusses.size();
		V3 off( 0.6f, 0, 0 );
		if( onStand ) off = standOff;
		if( onTruss )
		{
			const Truss& t = show.trusses[ c.mountIdx ];
			V3 d           = t.b - t.a;
			if( Len( d ) > 1e-4f ) off = d * ( 0.6f / Len( d ) );//along the truss
		}
		FixtureInst* n = AddFixture( c.def, c.mode, uni, addr, c.pos + off, c.rot, "" );
		if( !n ) continue;
		n->beam       = c.beam;
		n->invertPan  = c.invertPan;
		n->invertTilt = c.invertTilt;
		if( onStand )
		{
			auto it = standMap.find( c.mountIdx );
			int si  = it != standMap.end() ? it->second : ( standMap[ c.mountIdx ] = show.CloneStand( c.mountIdx, standOff ) );
			show.Attach( *n, MountType::Stand, si );
		}
		else if( onTruss )
			show.Attach( *n, MountType::Truss, c.mountIdx );
		newSel.push_back( n->id );
	}
	sel = newSel;
	show.UpdateMounts();
	show.dirty = true;
	SetStatus( std::to_string( newSel.size() ) + L( " Lampe(n) dupliziert" ) + ( standMap.empty() ? "" : ", " + std::to_string( standMap.size() ) + L( " Stativ(e) dazu" ) ) );
}

bool App::DelRow( float y0 )
{
	if( !delKey_ || !ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows ) ) return false;
	const ImVec2 m  = ImGui::GetIO().MousePos;
	const float y1  = ImGui::GetItemRectMax().y;
	const float wx0 = ImGui::GetWindowPos().x, wx1 = wx0 + ImGui::GetWindowWidth();
	if( m.y < y0 || m.y >= y1 || m.x < wx0 || m.x > wx1 ) return false;
	delKey_ = false;
	return true;
}

void App::DeleteByContext()
{
	if( delCtx_ == 2 )
	{
		int idx = -1;
		for( int i = 0; i < (int)show.effects.size(); ++i )
			if( show.effects[ i ].id == show.fxSelId ) idx = i;
		if( idx < 0 ) return;
		const int id = show.fxSelId;
		show.effects.erase( show.effects.begin() + idx );
		for( auto& p : show.programs ) p.effects.erase( std::remove( p.effects.begin(), p.effects.end(), id ), p.effects.end() );
		for( auto& sn : show.snapshots )
		{
			sn.effects.erase( std::remove( sn.effects.begin(), sn.effects.end(), id ), sn.effects.end() );
			sn.fxOff.erase( std::remove( sn.fxOff.begin(), sn.fxOff.end(), id ), sn.fxOff.end() );
		}
		show.fxSelId = show.effects.empty() ? 0 : show.effects[ std::min( idx, (int)show.effects.size() - 1 ) ].id;
		prevFxSel_   = show.fxSelId;
		show.dirty   = true;
		SetStatus( L( "Effekt gelöscht" ) );
		return;
	}
	if( delCtx_ == 3 )
	{
		if( projSel_ < 0 || projSel_ >= (int)show.projectors.size() ) return;
		show.projectors.erase( show.projectors.begin() + projSel_ );
		projSel_ = prevProj_ = -1;
		show.dirty = true;
		SetStatus( L( "Projektor gelöscht" ) );
		return;
	}
	if( delCtx_ == 4 )
	{
		if( screenSel_ < 0 || screenSel_ >= (int)show.screens.size() ) return;
		show.screens.erase( show.screens.begin() + screenSel_ );
		screenSel_ = prevScreen_ = -1;
		show.dirty = true;
		SetStatus( L( "Leinwand gelöscht" ) );
		return;
	}
	if( !sel.empty() && !ImGui::GetIO().WantTextInput ) DeleteSelected();
}

void App::DeleteSelected()
{
	if( sel.empty() ) return;
	std::vector< int > standsUsed;
	for( auto& f : show.fixtures )
		if( IsSel( f.id ) && f.mount == MountType::Stand ) standsUsed.push_back( f.mountIdx );
	show.fixtures.erase( std::remove_if( show.fixtures.begin(), show.fixtures.end(), [ & ]( const FixtureInst& f ) { return IsSel( f.id ); } ), show.fixtures.end() );
	for( auto& m : show.midi )
		m.fixtures.erase( std::remove_if( m.fixtures.begin(), m.fixtures.end(), [ & ]( int id ) { return IsSel( id ); } ), m.fixtures.end() );
	for( auto& e : show.effects )
		e.fixtures.erase( std::remove_if( e.fixtures.begin(), e.fixtures.end(), [ & ]( int id ) { return IsSel( id ); } ), e.fixtures.end() );
	//a stand that carried only deleted lamps goes with them (highest index first, indices shift)
	std::sort( standsUsed.begin(), standsUsed.end(), std::greater< int >() );
	standsUsed.erase( std::unique( standsUsed.begin(), standsUsed.end() ), standsUsed.end() );
	int standsGone = 0;
	for( int si : standsUsed )
		if( si >= 0 && si < (int)show.stands.size() && show.MountedCount( MountType::Stand, si ) == 0 ) { show.RemoveStand( si ); ++standsGone; }
	SetStatus( std::to_string( sel.size() ) + L( " Lampe(n) gelöscht" ) + ( standsGone ? ", " + std::to_string( standsGone ) + L( " leere(s) Stativ(e) entfernt" ) : "" ) );
	sel.clear();
	show.dirty = true;
}

//================================================================ frame
void App::Frame( float dt )
{
	fps = fps * 0.95f + ( dt > 0 ? 1.0f / dt : 0 ) * 0.05f;
	QualityTick( dt );
	//the UI owns the show while it builds the frame; the engine thread waits (rendering releases it)
	std::unique_lock< std::recursive_mutex > frameLk( engineLock_ );
	struct ClearPtr
	{
		std::unique_lock< std::recursive_mutex >*& p;
		~ClearPtr() { p = nullptr; }
	} clearPtr{ frameLock_ };
	frameLock_ = &frameLk;
	OscListenerUpdate();
	BridgeListenerUpdate();
	if( exp_.active )
	{
		//video export: the show runs in export time, not in real time
		if( !ExportStep( 0.045 ) ) ExportFinish( exp_.err.empty() );
	}
	else if( lbx_.active )
	{
		//Lightshow export: same principle (frame by frame, independent of real time)
		if( !LbxStep( show, sim, lbx_, 0.045 ) ) LbxEnd( lbx_.err.empty() );
	}
	else
	{
		if( !engineRun_ )
		{
			io.PollInput();
			HandleMidi();
			PollOsc();
			bridge.Tick( show, Now() );
		}
		if( demoFx ) DemoFx( dt );
		if( !engineRun_ ) UpdateEffects( Now() );
		Simulate( show, sim, dt );
	}
	if( show.dmxVersion != lastKickVer_ )
	{
		lastKickVer_ = show.dmxVersion;
		io.Kick();
	}
	UpdateCpu();
	{
		ImGuiIO& gi = ImGui::GetIO();
		if( gi.MouseDelta.x != 0 || gi.MouseDelta.y != 0 || ImGui::IsAnyMouseDown() || gi.MouseWheel != 0 || gi.InputQueueCharacters.Size > 0 || gi.WantTextInput ) lastUiInput_ = Now();
	}
	//rates
	if( Now() - rateT_ > 1.0 )
	{
		uint64_t s = io.artnetSent + io.enttecSent, r = io.artnetRecv;
		sentRate_  = (float)( s - lastSent_ ) / (float)( Now() - rateT_ );
		recvRate_  = (float)( r - lastRecv_ ) / (float)( Now() - rateT_ );
		lastSent_  = s;
		lastRecv_  = r;
		rateT_     = Now();
	}
	//shortcuts
	ImGuiIO& gio = ImGui::GetIO();
	//what was selected last decides what Delete removes when the mouse is not over a list row
	if( sel != prevSel_ ) { if( !sel.empty() ) delCtx_ = 1; prevSel_ = sel; }
	if( show.fxSelId != prevFxSel_ ) { if( show.fxSelId ) delCtx_ = 2; prevFxSel_ = show.fxSelId; }
	if( projSel_ != prevProj_ ) { if( projSel_ >= 0 ) delCtx_ = 3; prevProj_ = projSel_; }
	if( screenSel_ != prevScreen_ ) { if( screenSel_ >= 0 ) delCtx_ = 4; prevScreen_ = screenSel_; }
	delKey_ = false;
	tlHovered_    = tlHoveredNow_;//mouse over the timeline in the last frame: Space = play / pause there
	tlHoveredNow_ = false;
	TimelineTick();
	if( !gio.WantTextInput )
	{
		if( gio.KeyCtrl && ImGui::IsKeyPressed( ImGuiKey_S ) )
		{
			std::string p = show.path;
			if( p.empty() || gio.KeyShift ) p = FileDialog( hwnd, true, kShowFilter, L"lightstage", show.path );
			if( !p.empty() ) SaveShow( p );
		}
		if( gio.KeyCtrl && ImGui::IsKeyPressed( ImGuiKey_O ) && ConfirmClose() )
		{
			std::string p = FileDialog( hwnd, false, kShowFilter, L"lightstage" );
			if( !p.empty() ) OpenShow( p );
		}
		if( gio.KeyCtrl && !gio.KeyShift && ImGui::IsKeyPressed( ImGuiKey_Z ) ) Undo();
		if( gio.KeyCtrl && ( ImGui::IsKeyPressed( ImGuiKey_Y ) || ( gio.KeyShift && ImGui::IsKeyPressed( ImGuiKey_Z ) ) ) ) Redo();
		if( gio.KeyCtrl && ImGui::IsKeyPressed( ImGuiKey_A ) )
		{
			sel.clear();
			for( auto& f : show.fixtures ) sel.push_back( f.id );
		}
		if( ImGui::IsKeyPressed( ImGuiKey_F12 ) ) SetPerfMode( !perfMode_ );
		if( ImGui::IsKeyPressed( ImGuiKey_Space, false ) && !gio.KeyCtrl && !ImGui::IsAnyItemActive() )
		{
			if( tlHovered_ ) TimelineToggle();
			else if( !show.cues.empty() ) show.CueGo();
		}
		if( ImGui::IsKeyPressed( ImGuiKey_Escape ) )
		{
			if( aimPick_ ) aimPick_ = false;
			else if( learnArmed_ ) learnArmed_ = false;
			else sel.clear();
		}
		//Delete (Windows) / Backspace (Mac keyboards): a hovered list row takes it while the UI is drawn, else DeleteByContext()
		if( ( ImGui::IsKeyPressed( ImGuiKey_Delete, false ) || ImGui::IsKeyPressed( ImGuiKey_Backspace, false ) ) && !ImGui::IsAnyItemActive() && !gio.KeyCtrl ) delKey_ = true;
		if( ImGui::IsKeyPressed( ImGuiKey_F ) && !gio.WantCaptureKeyboard ) FrameSelection();
		if( ImGui::IsKeyPressed( ImGuiKey_L, false ) && !gio.WantCaptureKeyboard && !gio.KeyCtrl ) show.workLight = !show.workLight;
	}

	Menu();
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	//outer margin: nothing touches the screen edge (maximized window, monitor bezel)
	const float edge = floorf( 8.0f * ui );
	ImGui::SetNextWindowPos( vp->WorkPos );
	ImGui::SetNextWindowSize( vp->WorkSize );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( edge, edge * 0.75f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding, 0 );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0 );
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0, 0 ) );
	ImGui::Begin( "##host", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav );
	//the host never scrolls (keyboard navigation / rounding could shift the whole layout off screen)
	ImGui::SetScrollX( 0 );
	ImGui::SetScrollY( 0 );
	const float sbH = floorf( ImGui::GetFrameHeight() * 0.95f );//2.1 status bar
	float W = floorf( ImGui::GetContentRegionAvail().x ), H = floorf( ImGui::GetContentRegionAvail().y ) - 1 - sbH - floorf( 3.0f * ui );
	const float sp = floorf( 5.0f * ui );
	if( !uiScaled_ )
	{
		leftW_ *= ui;
		rightW_ *= ui;
		bottomH_ *= ui;
		uiScaled_ = true;
	}
	leftW_   = Clamp( leftW_, 200.0f * ui, W * 0.4f );
	rightW_  = Clamp( rightW_, 260.0f * ui, W * 0.45f );
	bottomH_ = Clamp( bottomH_, 200.0f * ui, H * 0.7f );
	float topH = H - bottomH_ - sp;
	ImGui::PopStyleVar( 1 );
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0, 0 ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 10 * ui, 8 * ui ) );
	ImGui::BeginChild( "left", ImVec2( leftW_, topH ), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding );
	ImGui::PopStyleVar( 2 );
	PanelPatch();
	ImGui::EndChild();
	ImGui::SameLine( 0, 0 );//explicit: the global item spacing is active again here
	Splitter( true, &leftW_, 200 * ui, W * 0.4f, 1, topH );
	ImGui::SameLine( 0, 0 );//explicit: the global item spacing is active again here
	float vw = floorf( W - leftW_ - rightW_ - 2 * sp );
	ImGui::BeginChild( "viewport", ImVec2( vw, topH ), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );
	PanelViewport();
	ImGui::EndChild();
	ImGui::SameLine( 0, 0 );//explicit: the global item spacing is active again here
	Splitter( true, &rightW_, 260 * ui, W * 0.45f, -1, topH );
	ImGui::SameLine( 0, 0 );//explicit: the global item spacing is active again here
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0, 0 ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 10 * ui, 8 * ui ) );
	ImGui::BeginChild( "right", ImVec2( rightW_, topH ), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding );
	ImGui::PopStyleVar( 2 );
	PanelInspector();
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0, 0 ) );//row advance of the top row / splitter / bottom: no gaps
	ImGui::EndChild();
	Splitter( false, &bottomH_, 200 * ui, H * 0.7f, -1, W );
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0, 0 ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 10 * ui, 8 * ui ) );
	ImGui::BeginChild( "bottom", ImVec2( W, bottomH_ ), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding );
	ImGui::PopStyleVar( 2 );
	PanelBottom();
	ImGui::EndChild();
	ImGui::Dummy( ImVec2( 1, floorf( 3.0f * ui ) ) );
	ImGui::BeginChild( "statusbar", ImVec2( W, sbH ), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav );
	StatusBar();
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::End();
	ImGui::PopStyleVar( 3 );
	LibraryPopup();
	PerfWindow();
	FixtureEditor();
	OscWindow();
	TargetsWindow();
	AbletonWindow();
	ExportWindow();
	LbxWindow();
	LbiWindow();
	ResolumeWindow();
	TimelineWindow();
	if( delKey_ ) { delKey_ = false; DeleteByContext(); }
	AutosaveTick();
	UndoTick( false );
}

std::string App::OpenAudioDialog()
{
	return FileDialog( hwnd, false, L"Audio (wav, mp3, aiff, m4a, flac, wma)\0*.wav;*.mp3;*.aif;*.aiff;*.m4a;*.aac;*.flac;*.wma\0*.*\0*.*\0", L"wav" );
}

//================================================================ undo / redo
std::string App::UndoState() { return WriteJson( show.ToJson( true ), false ); }

//---- changes from outside as JSON paths. Elements of lists are named by "id" / "name" where they have one,
//so a value still finds its lamp / snapshot when the list was changed by an undo step in between.
static const char kUndoSep = '\x1f';
static std::string UndoElemKey( const Json& e, size_t i )
{
	if( e.IsObj() )
	{
		if( const Json* id = e.Find( "id" ) )
		{
			if( id->IsNum() ) return "#id=" + std::to_string( id->I() );
			if( id->IsStr() && !id->s.empty() ) return "#id=" + id->s;
		}
		if( const Json* n = e.Find( "name" ) )
			if( n->IsStr() && !n->s.empty() ) return "#name=" + n->s;
	}
	return "#" + std::to_string( i );
}
static void UndoDiff( const Json& a, const Json& b, const std::string& path, std::vector< std::pair< std::string, Json > >& out )
{
	if( a.type != b.type ) { out.push_back( { path, b } ); return; }
	switch( b.type )
	{
	case Json::Object:
		for( auto& kv : b.o )
		{
			const std::string p = path + kUndoSep + kv.first;
			if( const Json* av = a.Find( kv.first ) ) UndoDiff( *av, kv.second, p, out );
			else out.push_back( { p, kv.second } );
		}
		return;
	case Json::Array:
	{
		bool same = a.a.size() == b.a.size();
		for( size_t i = 0; same && i < b.a.size(); ++i ) same = UndoElemKey( a.a[ i ], i ) == UndoElemKey( b.a[ i ], i );
		if( !same ) { out.push_back( { path, b } ); return; }//list changed: as a whole
		for( size_t i = 0; i < b.a.size(); ++i ) UndoDiff( a.a[ i ], b.a[ i ], path + kUndoSep + UndoElemKey( b.a[ i ], i ), out );
		return;
	}
	case Json::Number: if( a.n != b.n ) out.push_back( { path, b } ); return;
	case Json::Bool: if( a.b != b.b ) out.push_back( { path, b } ); return;
	case Json::String: if( a.s != b.s ) out.push_back( { path, b } ); return;
	default: return;
	}
}
static void UndoApply( Json& root, const std::string& path, const Json& v )
{
	Json* cur = &root;
	size_t i  = 0;
	if( path.empty() ) return;
	while( i < path.size() )
	{
		//path starts with the separator: segment = up to the next one
		size_t j = path.find( kUndoSep, i + 1 );
		if( j == std::string::npos ) j = path.size();
		const std::string seg = path.substr( i + 1, j - i - 1 );
		const bool last       = j == path.size();
		Json* next            = nullptr;
		if( cur->IsObj() )
		{
			for( auto& kv : cur->o )
				if( kv.first == seg ) { next = &kv.second; break; }
			if( !next && last ) { cur->Set( seg, v ); return; }
		}
		else if( cur->IsArr() )
		{
			for( size_t k = 0; k < cur->a.size() && !next; ++k )
				if( UndoElemKey( cur->a[ k ], k ) == seg ) next = &cur->a[ k ];
		}
		if( !next ) return;//not in this state (deleted / not created yet)
		if( last ) { *next = v; return; }
		cur = next;
		i   = j;
	}
}

void App::UndoReset()
{
	ls::rs::ResetOriginals();
	undo_.clear();
	redo_.clear();
	undoExt_.clear();
	undoSeq_ = 0;
	undoCur_ = UndoState();
	undoSeen_.clear();
	undoTouched_ = false;
}

void App::UndoTick( bool force )
{
	const double now = Now();
	bool user        = false;
	if( ImGui::GetCurrentContext() )
	{
		user = ImGui::IsAnyItemActive() || ImGui::IsAnyMouseDown();
		for( int k = ImGuiKey_NamedKey_BEGIN; !user && k < ImGuiKey_NamedKey_END; ++k )
			if( k < ImGuiKey_MouseLeft && ImGui::IsKeyDown( (ImGuiKey)k ) ) user = true;
	}
	if( user ) undoTouched_ = true;
	if( undoCur_.empty() )
	{
		undoCur_ = UndoState();
		return;
	}
	//while a slider / drag / text field is in use nothing is compared; afterwards a few times a second
	if( !force && ( user || now - undoCheckT_ < 0.25 ) ) return;
	undoCheckT_   = now;
	std::string s = UndoState();
	if( s == undoCur_ )
	{
		undoSeen_.clear();
		if( !user ) undoTouched_ = false;
		return;
	}
	if( !undoTouched_ )
	{
		//changed from outside only (MIDI, OSC, Ableton, Art-Net): becomes the new starting point, no step;
		//older states get these values too when they come back (undo does not turn a knob back)
		Json a, b;
		if( ParseJson( undoCur_, a ) && ParseJson( s, b ) )
		{
			std::vector< std::pair< std::string, Json > > d;
			UndoDiff( a, b, "", d );
			++undoSeq_;
			for( auto& x : d ) undoExt_[ x.first ] = { undoSeq_, std::move( x.second ) };
			if( undoExt_.size() > 20000 ) undoExt_.clear();//safety limit
		}
		undoCur_ = std::move( s );
		undoSeen_.clear();
		return;
	}
	if( !force )
	{
		//one step per finished change: wait until it stayed the same for a moment (at most 1.5 s)
		if( undoSeen_.empty() ) undoPendT_ = now;
		if( s != undoSeen_ )
		{
			undoSeen_ = s;
			undoChgT_ = now;
		}
		if( now - undoChgT_ < 0.4 && now - undoPendT_ < 1.5 ) return;
	}
	undo_.push_back( { std::move( undoCur_ ), undoSeq_ } );
	undoCur_ = std::move( s );
	undoSeen_.clear();
	redo_.clear();
	undoTouched_ = user;
	//limit: 200 steps and 256 MB
	size_t bytes = 0;
	for( auto& u : undo_ ) bytes += u.state.size();
	while( undo_.size() > 1 && ( undo_.size() > 200 || bytes > ( 256u << 20 ) ) )
	{
		bytes -= undo_.front().state.size();
		undo_.erase( undo_.begin() );
	}
}

void App::Undo()
{
	UndoTick( true );//a change still pending is a step of its own
	if( undo_.empty() )
	{
		SetStatus( L( "Nichts rückgängig zu machen" ) );
		return;
	}
	redo_.push_back( { std::move( undoCur_ ), undoSeq_ } );
	const UndoEntry e = std::move( undo_.back() );
	undo_.pop_back();
	RestoreState( e );
	ls::rs::ResetOriginals();
	undoCur_ = UndoState();
	undoSeen_.clear();
	undoTouched_ = false;
	SetStatus( L( "Rückgängig gemacht" ) );
}

void App::Redo()
{
	UndoTick( true );//a new change (none to redo then)
	if( redo_.empty() )
	{
		SetStatus( L( "Nichts wiederherzustellen" ) );
		return;
	}
	undo_.push_back( { std::move( undoCur_ ), undoSeq_ } );
	const UndoEntry e = std::move( redo_.back() );
	redo_.pop_back();
	RestoreState( e );
	ls::rs::ResetOriginals();
	undoCur_ = UndoState();
	undoSeen_.clear();
	undoTouched_ = false;
	SetStatus( L( "Wiederhergestellt" ) );
}

//loads a stored set-up state; everything live keeps running unchanged
void App::RestoreState( const UndoEntry& e )
{
	Json j;
	if( !ParseJson( e.state, j ) ) return;
	for( auto& x : undoExt_ )
		if( x.second.first > e.seq ) UndoApply( j, x.first, x.second.second );
	std::lock_guard< std::recursive_mutex > engineGuard( engineLock_ );
	//live state
	std::vector< Universe > oldU;
	std::vector< DmxFrame > oldD;
	std::vector< std::array< int16_t, 512 > > oldFxOut, oldSnapOut;
	std::vector< std::array< uint32_t, 512 > > oldGrpSeq;
	bool fxAny = false, snapAny = false;
	{
		std::lock_guard< std::mutex > l( show.dmxLock );
		oldU       = show.universes;
		oldD       = show.dmx;
		oldFxOut   = show.fxOut;
		oldSnapOut = show.snapOut;
		oldGrpSeq  = show.grpSeq;
		fxAny      = show.fxAny;
		snapAny    = show.snapAny;
	}
	const Tempo tempo         = show.tempo;
	const float master        = show.master, fxMaster = show.fxMaster, fxSpeed = show.fxSpeed;
	const std::string showId  = show.showId, path = show.path;
	const uint32_t snapSeq    = show.snapSeq;
	const int fxSelId         = show.fxSelId;
	const int cueCur          = show.cueCur;
	const double cueT         = show.cueT, tcSec = show.tcSec, tcPrev = show.tcPrev;
	const int tcFps           = show.tcFps;
	const TlRuntime tlRt      = show.tl.rt;
	const auto progSeq        = show.progSeq;
	const auto palC = show.progPalColor, palP = show.progPalPos;
	const std::vector< Effect > oldFx       = show.effects;
	const std::vector< Snapshot > oldSn     = show.snapshots;
	const std::vector< GroupFader > oldG    = show.groups;
	const std::vector< AbletonMap > oldA    = show.ableton;
	const std::vector< Projector > oldP     = show.projectors;
	const std::vector< MidiMap > oldM       = show.midi;
	const std::string oldOut                = WriteJson( show.ToJson( true )[ "output" ], false );
	std::string err;
	if( !show.FromJson( j, lib, path, &err ) ) return;
	{
		std::lock_guard< std::mutex > l( show.dmxLock );
		bool same = show.universes.size() == oldU.size();
		for( size_t i = 0; i < show.universes.size(); ++i )
		{
			for( size_t k = 0; k < oldU.size(); ++k )
				if( oldU[ k ].num == show.universes[ i ].num && k < oldD.size() ) show.dmx[ i ] = oldD[ k ];
			if( same && oldU[ i ].num != show.universes[ i ].num ) same = false;
		}
		if( same )
		{
			show.fxOut   = oldFxOut;
			show.snapOut = oldSnapOut;
			show.grpSeq  = oldGrpSeq;
			show.fxAny   = fxAny;
			show.snapAny = snapAny;
		}
	}
	show.tempo    = tempo;
	show.master   = master;
	show.fxMaster = fxMaster;
	show.fxSpeed  = fxSpeed;
	show.showId   = showId;
	show.snapSeq  = snapSeq;
	show.cueCur   = cueCur < (int)show.cues.size() ? cueCur : (int)show.cues.size() - 1;
	show.cueT     = cueT;
	show.tcSec    = tcSec;
	show.tcPrev   = tcPrev;
	show.tcFps    = tcFps;
	show.tl.rt    = tlRt;
	if( show.universes.size() == progSeq.size() ) show.progSeq = progSeq;
	show.progPalColor = palC;
	show.progPalPos   = palP;
	for( auto& e : show.effects )
		for( auto& o : oldFx )
			if( o.id == e.id )
			{
				e.on        = o.on;
				e.cur       = o.cur;
				e.freePhase = o.freePhase;
				e.lastBeat  = o.lastBeat;
				e.syncBeat  = o.syncBeat;
				e.lastTempo = o.lastTempo;
				e.clockInit = o.clockInit;
				e.snapW     = o.snapW;
				e.snapI     = o.snapI;
				if( o.fixtures == e.fixtures )
				{
					e.fxD = o.fxD;
					e.fxI = o.fxI;
				}
				break;
			}
	show.fxSelId = show.FindEffect( fxSelId ) ? fxSelId : 0;
	{
		std::vector< bool > used( oldSn.size(), false );
		for( auto& s : show.snapshots )
			for( size_t k = 0; k < oldSn.size(); ++k )
				if( !used[ k ] && oldSn[ k ].name == s.name )
				{
					used[ k ] = true;
					s.active  = oldSn[ k ].active;
					s.cur     = oldSn[ k ].cur;
					s.seq     = oldSn[ k ].seq;
					s.keep    = oldSn[ k ].keep;
					break;
				}
	}
	{
		std::vector< bool > used( oldG.size(), false );
		for( auto& g : show.groups )
			for( size_t k = 0; k < oldG.size(); ++k )
				if( !used[ k ] && oldG[ k ].name == g.name )
				{
					used[ k ] = true;
					g.value   = oldG[ k ].value;
					g.hue     = oldG[ k ].hue;
					g.sat     = oldG[ k ].sat;
					break;
				}
	}
	for( auto& m : show.ableton )
		for( auto& o : oldA )
			if( o.id == m.id ) { m.state = o.state; break; }
	//projectors: same source -> same video slot (the video keeps playing)
	for( size_t i = 0; i < show.projectors.size() && i < oldP.size(); ++i )
	{
		Projector& p       = show.projectors[ i ];
		const Projector& o = oldP[ i ];
		if( p.input == o.input && p.file == o.file && p.ndi == o.ndi ) p.uid = o.uid;
	}
	if( show.midi.size() == oldM.size() )
		for( size_t i = 0; i < show.midi.size(); ++i )
		{
			show.midi[ i ].last    = oldM[ i ].last;
			show.midi[ i ].toggled = oldM[ i ].toggled;
		}
	for( auto& m : show.models ) ren.EnsureModel( m, nullptr );//cached by path: no reload
	//selections that point to things gone now
	sel.erase( std::remove_if( sel.begin(), sel.end(), [ & ]( int id ) { return !show.FindFixture( id ); } ), sel.end() );
	if( projSel_ >= (int)show.projectors.size() ) projSel_ = (int)show.projectors.size() - 1;
	if( screenSel_ >= (int)show.screens.size() ) screenSel_ = (int)show.screens.size() - 1;
	if( liveSnap_ >= (int)show.snapshots.size() ) liveSnap_ = -1;
	pixelSel_    = -1;
	lastClicked_ = -1;
	if( WriteJson( show.ToJson( true )[ "output" ], false ) != oldOut ) io.Apply();
	show.dirty = true;
	++show.dmxVersion;
}

void App::Splitter( bool vertical, float* size, float minv, float maxv, float sign, float length )
{
	const float sp = floorf( 5.0f * ui );
	ImVec2 sz      = vertical ? ImVec2( sp, length ) : ImVec2( length, sp );
	ImGui::PushID( size );
	ImGui::InvisibleButton( "##split", ImVec2( std::max( 1.0f, sz.x ), std::max( 1.0f, sz.y ) ) );
	bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
	if( hov || act ) ImGui::SetMouseCursor( vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS );
	if( act )
	{
		float d = vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
		*size   = Clamp( *size + d * sign, minv, maxv );
	}
	if( hov || act )
	{
		//fine accent line in the middle of the gap (the gap itself stays empty: panels read as cards)
		const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
		const float t  = std::max( 1.0f, floorf( 2.0f * ui ) );
		const ImU32 col = act ? IM_COL32( 90, 165, 255, 255 ) : IM_COL32( 90, 165, 255, 130 );
		if( vertical ) ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( floorf( ( a.x + b.x - t ) * 0.5f ), a.y + 6 * ui ), ImVec2( floorf( ( a.x + b.x - t ) * 0.5f ) + t, b.y - 6 * ui ), col, t );
		else ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( a.x + 6 * ui, floorf( ( a.y + b.y - t ) * 0.5f ) ), ImVec2( b.x - 6 * ui, floorf( ( a.y + b.y - t ) * 0.5f ) + t ), col, t );
	}
	ImGui::PopID();
}

//================================================================ menu
void App::Menu()
{
	if( !ImGui::BeginMainMenuBar() ) return;
	if( ImGui::BeginMenu( L( "Datei" ) ) )
	{
		if( ImGui::MenuItem( L( "Neue Show" ) ) && ConfirmClose() ) NewShow();
		if( ImGui::MenuItem( L( "Öffnen..." ), "Ctrl+O" ) && ConfirmClose() )
		{
			std::string p = FileDialog( hwnd, false, kShowFilter, L"lightstage" );
			if( !p.empty() ) OpenShow( p );
		}
		if( ImGui::MenuItem( L( "Speichern" ), "Ctrl+S" ) )
		{
			std::string p = show.path.empty() ? FileDialog( hwnd, true, kShowFilter, L"lightstage" ) : show.path;
			if( !p.empty() ) SaveShow( p );
		}
		if( ImGui::MenuItem( L( "Speichern unter..." ), "Ctrl+Shift+S" ) )
		{
			std::string p = FileDialog( hwnd, true, kShowFilter, L"lightstage", show.path );
			if( !p.empty() ) SaveShow( p );
		}
		ImGui::Separator();
		if( ImGui::MenuItem( L( "Demo-Show laden (Jazzclub)" ) ) && ConfirmClose() ) BuildJazzDemo();
		if( ImGui::MenuItem( L( "Demo-Show laden (Konzertbühne)" ) ) && ConfirmClose() ) BuildDemo();
		if( ImGui::MenuItem( L( "3D-Modell importieren..." ) ) )
		{
			std::string p = FileDialog( hwnd, false, kModelFilter, L"glb" );
			if( !p.empty() ) DropFile( p );
		}
		if( ImGui::MenuItem( L( "Eigene Lampe erstellen (Fixture-Editor)..." ) ) ) OpenFixtureEditor( nullptr );
		if( ImGui::MenuItem( L( "GDTF-Lampe importieren..." ) ) )
		{
			std::string p = FileDialog( hwnd, false, kGdtfFilter, L"gdtf" );
			if( !p.empty() ) DropFile( p );
		}
		ImGui::Separator();
		if( ImGui::MenuItem( L( "Ableton-Manifest exportieren..." ) ) ) ExportManifest();
		if( ImGui::MenuItem( L( "Video exportieren..." ) ) ) expWin_ = true;
		if( ImGui::MenuItem( L( "Lightshow exportieren..." ) ) ) lbxWin_ = true;
		if( ImGui::MenuItem( L( "Lightshow importieren..." ) ) )
		{
			std::string p = FileDialog( hwnd, false, L"Lightshow (*.lightshow)\0*.lightshow\0", L"lightshow" );
			if( !p.empty() ) LbiOpen( p );
		}
		if( ImGui::MenuItem( L( "Resolume-Preset exportieren..." ) ) ) resWin_ = true;
		if( ImGui::BeginMenu( "Autosave" ) )
		{
			for( int m : { 0, 1, 2, 5, 10 } )
			{
				char l[ 48 ];
				if( m ) snprintf( l, sizeof( l ), L( "alle %d Minuten" ), m );
				else snprintf( l, sizeof( l ), "%s", L( "aus" ) );
				if( ImGui::MenuItem( l, nullptr, autosaveMin == m ) ) { autosaveMin = m; SaveSettings(); }
			}
			ImGui::TextDisabled( L( "Nach einem Absturz bietet LightStage beim\nnächsten Start die Wiederherstellung an." ) );
			ImGui::EndMenu();
		}
		if( ImGui::MenuItem( L( "Screenshot speichern..." ) ) )
		{
			std::string p = FileDialog( hwnd, true, L"PNG (*.png)\0*.png\0", L"png" );
			std::vector< uint8_t > px;
			int w, h;
			if( !p.empty() && ren.ReadOutput( px, w, h ) )
			{
				for( size_t i = 3; i < px.size(); i += 4 ) px[ i ] = 255;
				stbi_write_png( p.c_str(), w, h, 4, px.data(), w * 4 );
				SetStatus( "Screenshot: " + p );
			}
		}
		ImGui::Separator();
		if( ImGui::MenuItem( L( "Beenden" ) ) && ConfirmClose() ) quit_ = true;
		ImGui::EndMenu();
	}
	if( ImGui::BeginMenu( L( "Bearbeiten" ) ) )
	{
		if( ImGui::MenuItem( L( "Rückgängig" ), "Ctrl+Z", false, CanUndo() || undoTouched_ ) ) Undo();
		if( ImGui::MenuItem( L( "Wiederherstellen" ), "Ctrl+Y", false, CanRedo() ) ) Redo();
		ImGui::EndMenu();
	}
	if( ImGui::BeginMenu( L( "Ansicht" ) ) )
	{
		ImGui::MenuItem( L( "Lampen-Nummern" ), nullptr, &showLabels );
		ImGui::MenuItem( L( "Lichtkegel (Volumetrik)" ), nullptr, &show.stage.beams );
		ImGui::MenuItem( L( "Schatten" ), nullptr, &show.stage.shadows );
		ImGui::MenuItem( L( "Linsen-Glow" ), nullptr, &ren.lensGlow );
		if( ImGui::MenuItem( L( "Projektor-Linien" ), nullptr, &ren.projLines ) ) SaveSettings();
		ImGui::MenuItem( L( "Statistik" ), nullptr, &showStats_ );
		ImGui::MenuItem( L( "Arbeitslicht" ), "L", &show.workLight );
		ImGui::Separator();
		ImGui::MenuItem( L( "OSC-Adressen..." ), nullptr, &oscWin_ );
		ImGui::Separator();
		if( ImGui::MenuItem( L( "Performance-Modus (Live)" ), "F12", perfMode_ ) ) SetPerfMode( !perfMode_ );
		ImGui::MenuItem( L( "Performance-Einstellungen..." ), nullptr, &perfWin_ );
		ImGui::Separator();
		ImGui::MenuItem( L( "Demo-Bewegung (Test ohne Pult)" ), nullptr, &demoFx );
		ImGui::Separator();
		if( ImGui::MenuItem( L( "Kamera: Front" ) ) ) { cam.yaw = 0; cam.pitch = 10; cam.dist = 17; cam.target = V3( 0, 3, 0 ); }
		if( ImGui::MenuItem( L( "Kamera: FOH erhöht" ) ) ) { cam.yaw = 18; cam.pitch = 22; cam.dist = 22; cam.target = V3( 0, 2.5f, 0 ); }
		if( ImGui::MenuItem( L( "Kamera: Seite" ) ) ) { cam.yaw = 90; cam.pitch = 8; cam.dist = 18; cam.target = V3( 0, 3, 0 ); }
		if( ImGui::MenuItem( L( "Kamera: Draufsicht" ) ) ) { cam.yaw = 0; cam.pitch = 89; cam.dist = 22; cam.target = V3( 0, 0, 0 ); }
		if( ImGui::MenuItem( L( "Auswahl zentrieren" ), "F" ) ) FrameSelection();
		ImGui::Separator();
		if( ImGui::BeginMenu( "Sprache / Language" ) )
		{
			if( ImGui::MenuItem( "English", nullptr, gLang == kLangEn ) ) { SetLang( "en" ); languageSaved_ = "en"; SaveSettings(); }
			if( ImGui::MenuItem( "Deutsch", nullptr, gLang == kLangDe ) ) { SetLang( "de" ); languageSaved_ = "de"; SaveSettings(); }
			ImGui::EndMenu();
		}
		ImGui::EndMenu();
	}
	if( ImGui::BeginMenu( L( "Hilfe" ) ) )
	{
		ImGui::TextDisabled( "Grosserd LightStage 2.1 - DMX 3D Visualizer" );
		ImGui::Separator();
		ImGui::TextUnformatted( L( "Viewport: Linke Maus ziehen = drehen, Shift/Mitte = verschieben, Rad = Zoom" ) );
		ImGui::TextUnformatted( L( "Klick = Lampe wählen, Ctrl = mehrere, Doppelklick = zentrieren" ) );
		ImGui::TextUnformatted( L( "Entf = Auswahl löschen, Ctrl+A = alle, Esc = Auswahl aufheben" ) );
		ImGui::TextUnformatted( L( "MIDI Learn: Learn einschalten, Regler anklicken, MIDI-Regler bewegen" ) );
		ImGui::TextUnformatted( L( "Dateien (Show, GDTF, Modelle) per Drag&Drop ins Fenster ziehen" ) );
		ImGui::EndMenu();
	}
	ImGui::EndMainMenuBar();
}

//================================================================ patch panel
void App::PanelPatch()
{
	if( fontBig ) ImGui::PushFont( fontBig );
	ImGui::TextUnformatted( "PATCH" );
	if( fontBig ) ImGui::PopFont();
	ImGui::Dummy( ImVec2( 0, 4 ) );
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6, 5 ) );
	if( ImGui::Button( L( "+ Lampe hinzufügen" ) ) )
	{
		add_.open    = true;
		add_.replace = false;
	}
	ImGui::SameLine();
	ImGui::BeginDisabled( sel.empty() );
	if( ImGui::Button( L( "Duplizieren" ) ) ) DuplicateSelected();
	ImGui::SameLine();
	if( ImGui::Button( L( "Löschen" ) ) ) DeleteSelected();
	ImGui::EndDisabled();
	ImGui::PopStyleVar();
	ImGui::Dummy( ImVec2( 0, 4 ) );

	//fixtures table
	float tableH = ImGui::GetContentRegionAvail().y - 170;
	if( ImGui::BeginTable( "fixtures", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp, ImVec2( 0, std::max( 120.0f, tableH ) ) ) )
	{
		ImGui::TableSetupScrollFreeze( 0, 1 );
		ImGui::TableSetupColumn( "ID", ImGuiTableColumnFlags_WidthFixed, 30 * ui );
		ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthStretch, 3 );
		ImGui::TableSetupColumn( "Mode", ImGuiTableColumnFlags_WidthStretch, 1.3f );
		ImGui::TableSetupColumn( L( "U.Adr" ), ImGuiTableColumnFlags_WidthFixed, 52 * ui );
		ImGui::TableSetupColumn( "Ch", ImGuiTableColumnFlags_WidthFixed, 26 * ui );
		ImGui::TableHeadersRow();
		int mvId = 0, mvAnchor = 0;//drag & drop: lamp dragged, dropped on this lamp
		bool mvAfter = false;
		for( auto& f : show.fixtures )
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID( f.id );
			char id[ 16 ];
			snprintf( id, sizeof( id ), "%d", f.id );
			bool s = IsSel( f.id );
			if( ImGui::Selectable( id, s, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick ) )
			{
				Select( f.id, ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyShift );
				if( ImGui::IsMouseDoubleClicked( 0 ) ) FrameSelection();
			}
			//drag & drop: reorder the list, the DMX addresses follow the new order
			if( ImGui::BeginDragDropSource() )
			{
				const int id = f.id;
				ImGui::SetDragDropPayload( "LS_FIXTURE", &id, sizeof( id ) );
				const int n = IsSel( f.id ) ? (int)sel.size() : 1;
				if( n > 1 ) ImGui::Text( L( "%d Lampen verschieben" ), n );
				else ImGui::Text( "%s", f.name.c_str() );
				ImGui::TextDisabled( "%s", L( "Adressen folgen der neuen Reihenfolge" ) );
				ImGui::EndDragDropSource();
			}
			if( ImGui::BeginDragDropTarget() )
			{
				const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
				const bool after = ImGui::GetIO().MousePos.y > ( mn.y + mx.y ) * 0.5f;
				if( const ImGuiPayload* pl = ImGui::AcceptDragDropPayload( "LS_FIXTURE", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect ) )
				{
					const int dragged = *(const int*)pl->Data;
					const bool same   = dragged == f.id || ( IsSel( dragged ) && IsSel( f.id ) );
					if( !same )
					{
						const float y  = after ? mx.y : mn.y;
						const float x1 = ImGui::GetWindowPos().x + ImGui::GetWindowWidth();
						ImGui::GetForegroundDrawList()->AddLine( ImVec2( mn.x, y ), ImVec2( x1, y ), IM_COL32( 255, 170, 60, 255 ), 2.5f );
						if( pl->IsDelivery() )
						{
							mvId     = dragged;
							mvAnchor = f.id;
							mvAfter  = after;
						}
					}
				}
				ImGui::EndDragDropTarget();
			}
			if( ImGui::BeginPopupContextItem( "fctx" ) )
			{
				if( !IsSel( f.id ) ) { sel.clear(); sel.push_back( f.id ); }
				if( ImGui::MenuItem( sel.size() > 1 ? L( "Lampentyp tauschen (Auswahl)..." ) : L( "Lampentyp tauschen..." ) ) ) OpenSwapDialog( sel );
				if( f.def && ImGui::MenuItem( L( "Lampentyp bearbeiten..." ) ) ) OpenFixtureEditor( f.def );
				if( bridge.Subscribers( "dmx" ) > 0 && ImGui::MenuItem( sel.size() > 1 ? L( "Lampen an Ableton senden (DMX-Device)" ) : L( "Lampe an Ableton senden (DMX-Device)" ) ) )
				{
					for( int id : sel ) bridge.SendAssignFixture( id );
					SetStatus( L( "An Ableton gesendet: " ) + std::to_string( sel.size() ) + L( " Lampe(n)" ) );
				}
				if( ImGui::BeginMenu( L( "Time Line" ) ) )
				{
					TimelineAddMenu( sel, nullptr, 0, 0 );
					ImGui::EndMenu();
				}
				ImGui::Separator();
				if( ImGui::MenuItem( L( "Duplizieren" ) ) ) DuplicateSelected();
				if( ImGui::MenuItem( L( "Löschen" ) ) ) DeleteSelected();
				ImGui::EndPopup();
			}
			ImGui::TableNextColumn();
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( p.x, p.y + 3 ), ImVec2( p.x + 4, p.y + ImGui::GetTextLineHeight() - 2 ), FixtureColor( f.id ) );
			ImGui::SetCursorScreenPos( ImVec2( p.x + 8, p.y ) );
			ImGui::TextUnformatted( f.name.c_str() );
			if( ImGui::IsItemHovered() && f.def ) ImGui::SetTooltip( "%s\n%s - %s\n%s", f.name.c_str(), f.def->manufacturer.c_str(), f.def->name.c_str(), KindName( f.def->kind ) );
			if( !f.def ) { ImGui::SameLine(); ImGui::TextColored( ImVec4( 1, 0.3f, 0.3f, 1 ), L( "(Typ fehlt)" ) ); }
			ImGui::TableNextColumn();
			ImGui::TextDisabled( "%s", f.def && f.mode < (int)f.def->modes.size() ? f.def->modes[ f.mode ].shortName.c_str() : "-" );
			ImGui::TableNextColumn();
			bool clash = show.Overlaps( f.universe, f.address, f.Footprint(), f.id ) || f.address + f.Footprint() - 1 > 512;
			if( clash ) ImGui::TextColored( ImVec4( 1, 0.35f, 0.3f, 1 ), "%d.%03d", f.universe, f.address );
			else ImGui::Text( "%d.%03d", f.universe, f.address );
			ImGui::TableNextColumn();
			ImGui::TextDisabled( "%d", f.Footprint() );
			ImGui::PopID();
		}
		ImGui::EndTable();
		if( mvId ) MoveFixturesUi( mvId, mvAnchor, mvAfter );
	}
	ImGui::Dummy( ImVec2( 0, 6 ) );
	//universes
	ImGui::TextDisabled( "UNIVERSES" );
	ImGui::SameLine( ImGui::GetContentRegionAvail().x - 20 * ui );
	if( ImGui::SmallButton( "+" ) ) show.AddUniverse();
	if( ImGui::BeginTable( "unis", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2( 0, ImGui::GetContentRegionAvail().y ) ) )
	{
		ImGui::TableSetupColumn( "U", ImGuiTableColumnFlags_WidthFixed, 20 * ui );
		ImGui::TableSetupColumn( "Art-Net", ImGuiTableColumnFlags_WidthStretch );
		ImGui::TableSetupColumn( "Out", ImGuiTableColumnFlags_WidthFixed, 30 * ui );
		ImGui::TableSetupColumn( "In", ImGuiTableColumnFlags_WidthFixed, 30 * ui );
		ImGui::TableSetupColumn( L( "Belegt" ), ImGuiTableColumnFlags_WidthFixed, 44 * ui );
		ImGui::TableHeadersRow();
		int del = -1;
		for( size_t i = 0; i < show.universes.size(); ++i )
		{
			auto& u = show.universes[ i ];
			ImGui::PushID( (int)i );
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text( "%d", u.num );
			if( ImGui::BeginPopupContextItem( "uctx" ) )
			{
				if( ImGui::MenuItem( L( "Universe entfernen" ), nullptr, false, show.universes.size() > 1 ) ) del = (int)i;
				ImGui::EndPopup();
			}
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth( -1 );
			int port = u.artnetPort;
			if( ImGui::InputInt( "##port", &port, 0 ) ) { u.artnetPort = Clamp( port, 0, 32767 ); u.artnetInPort = u.artnetPort; show.dirty = true; }
			if( ImGui::IsItemHovered() ) ImGui::SetTooltip( L( "Art-Net Port-Adresse (0 = Net 0, Sub 0, Uni 0)" ) );
			ImGui::TableNextColumn();
			ImGui::Checkbox( "##o", &u.artnetOut );
			ImGui::TableNextColumn();
			ImGui::Checkbox( "##i", &u.artnetIn );
			ImGui::TableNextColumn();
			int used = 0;
			for( auto& f : show.fixtures )
				if( f.universe == u.num ) used += f.Footprint();
			ImGui::TextDisabled( "%d", used );
			ImGui::PopID();
		}
		if( del >= 0 )
		{
			std::lock_guard< std::mutex > l( show.dmxLock );
			show.universes.erase( show.universes.begin() + del );
			show.dmx.erase( show.dmx.begin() + del );
			if( del < (int)show.fxOut.size() ) show.fxOut.erase( show.fxOut.begin() + del );
			if( del < (int)show.snapOut.size() ) show.snapOut.erase( show.snapOut.begin() + del );
			if( del < (int)show.progSeq.size() ) show.progSeq.erase( show.progSeq.begin() + del );
		}
		ImGui::EndTable();
	}
}

//================================================================ viewport
void App::RenderViewport( int w, int h )
{
	for( auto& m : show.models )
		if( m.meshId < 0 ) ren.EnsureModel( m, nullptr );
	video.Sync( show.projectors );
	ren.video        = &video;
	ren.selProjector = videoTab_ ? projSel_ : -1;
	ren.selScreen    = videoTab_ ? screenSel_ : -1;
	ren.workLight    = show.workLight;
	lastViewRender_  = Now();
	//GPU work without the show lock: the engine thread may apply MIDI meanwhile
	if( frameLock_ && frameLock_->owns_lock() )
	{
		frameLock_->unlock();
		ren.Render( show, sim, cam, sel, w, h );
		frameLock_->lock();
	}
	else
		ren.Render( show, sim, cam, sel, w, h );
}

void App::FrameSelection()
{
	if( sel.empty() )
	{
		cam.target = V3( 0, 3, 0 );
		return;
	}
	V3 c( 0, 0, 0 );
	int n = 0;
	for( auto& f : show.fixtures )
		if( IsSel( f.id ) ) { c += f.pos; ++n; }
	if( n ) cam.target = c / (float)n;
}

//================================================================ autosave
bool App::AutosaveWrite( const std::string& file )
{
	Json j = show.ToJson();
	j.Set( "autosaveOf", show.path );
	const std::string tmp = file + ".tmp";
	if( !WriteText( tmp, WriteJson( j, false ) ) ) return false;
	return MoveFileExW( Widen( tmp ).c_str(), Widen( file ).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) != 0;
}
bool App::AutosaveRecover( const std::string& file )
{
	std::string txt, orig;
	Json j;
	if( ReadText( file, txt ) && ParseJson( txt, j ) ) orig = j[ "autosaveOf" ].Str();
	if( !OpenShow( file ) ) return false;
	show.path  = orig;//saving goes to the original file again (empty: "Save as")
	show.dirty = true;
	SetStatus( L( "Wiederhergestellt: " ) + ( orig.empty() ? std::string( L( "neue Show" ) ) : FileName( orig ) ) + L( " - bitte speichern" ) );
	return true;
}
void App::AutosaveClear()
{
	if( !settingsLoaded_ ) return;//self test: never touch the user's files
	DeleteFileW( Widen( AutosaveFile() ).c_str() );
	autosaveT_ = Now();
}
void App::AutosaveTick()
{
	if( !settingsLoaded_ || noAutosave ) return;
	if( !autosaveInit_ )
	{
		//left over from a run that did not end normally: keep it aside and ask
		autosaveInit_ = true;
		autosaveT_    = Now();
		const std::wstring a = Widen( AutosaveFile() ), r = Widen( RecoverFile() );
		if( GetFileAttributesW( a.c_str() ) != INVALID_FILE_ATTRIBUTES ) MoveFileExW( a.c_str(), r.c_str(), MOVEFILE_REPLACE_EXISTING );
		WIN32_FILE_ATTRIBUTE_DATA fa{};
		if( GetFileAttributesExW( r.c_str(), GetFileExInfoStandard, &fa ) )
		{
			FILETIME lt;
			SYSTEMTIME st;
			FileTimeToLocalFileTime( &fa.ftLastWriteTime, &lt );
			FileTimeToSystemTime( &lt, &st );
			std::string txt, orig;
			Json j;
			if( ReadText( RecoverFile(), txt ) && ParseJson( txt, j ) ) orig = j[ "autosaveOf" ].Str();
			char b[ 256 ];
			snprintf( b, sizeof( b ), L( "%s, Stand %02d.%02d.%04d %02d:%02d" ), orig.empty() ? L( "neue Show" ) : FileName( orig ).c_str(), st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute );
			recoverInfo_ = b;
			recoverAsk_  = true;
		}
	}
	if( recoverAsk_ )
	{
		if( !ImGui::IsPopupOpen( "##recover" ) ) ImGui::OpenPopup( "##recover" );
		if( ImGui::BeginPopupModal( "##recover", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar ) )
		{
			ImGui::TextUnformatted( L( "LightStage wurde nicht normal beendet." ) );
			ImGui::Text( L( "Automatisch gesicherte Änderungen wiederherstellen?\n%s" ), recoverInfo_.c_str() );
			ImGui::Spacing();
			if( ImGui::Button( L( "Änderungen wiederherstellen" ), ImVec2( 220 * ui, 0 ) ) )
			{
				recoverAsk_ = false;
				AutosaveRecover( RecoverFile() );
				DeleteFileW( Widen( RecoverFile() ).c_str() );
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if( ImGui::Button( L( "Verwerfen" ), ImVec2( 220 * ui, 0 ) ) )
			{
				recoverAsk_ = false;
				//kept once more as a last copy
				MoveFileExW( Widen( RecoverFile() ).c_str(), Widen( exeDir + "\\autosave_verworfen.lightstage" ).c_str(), MOVEFILE_REPLACE_EXISTING );
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		return;
	}
	if( autosaveMin <= 0 || exp_.active ) return;
	if( !show.dirty )
	{
		autosaveT_ = Now();//the interval counts from the first unsaved change
		return;
	}
	if( Now() - autosaveT_ < autosaveMin * 60.0 ) return;
	autosaveT_ = Now();
	if( !AutosaveWrite( AutosaveFile() ) ) Log( "autosave failed" );
}

//================================================================ video export
static const int kExpRes[ 4 ][ 2 ] = { { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
static const int kExpFps[ 4 ]      = { 25, 30, 50, 60 };
bool App::ExportStart( const std::string& path )
{
	if( exp_.active ) return false;
	exp_.err.clear();
	exp_.path   = path;
	exp_.w      = kExpRes[ Clamp( exp_.res, 0, 3 ) ][ 0 ];
	exp_.h      = kExpRes[ Clamp( exp_.res, 0, 3 ) ][ 1 ];
	exp_.fps    = kExpFps[ Clamp( exp_.fpsIdx, 0, 3 ) ];
	exp_.frames = std::max( 1, (int)lroundf( exp_.seconds * exp_.fps ) );
	exp_.frame  = 0;
	exp_.cam0   = cam;
	exp_.vw     = std::make_unique< VideoWriter >();
	const int kbps = (int)std::min( 80000.0, (double)exp_.w * exp_.h * exp_.fps * 0.1 / 1000.0 );
	if( !exp_.vw->Open( path, exp_.w, exp_.h, exp_.fps, kbps, &exp_.err ) )
	{
		SetStatus( L( "Video-Export fehlgeschlagen: " ) + exp_.err );
		exp_.vw.reset();
		return false;
	}
	exp_.engineWas = engineRun_;
	if( engineRun_ ) StopEngine();
	exp_.projLines = ren.projLines;
	ren.projLines  = false;
	if( exp_.cueStart && !show.cues.empty() )
	{
		show.CueStop();
		show.tcSec  = exp_.tcStart;
		show.tcPrev = -1;
		show.CueGo( 0 );
	}
	if( exp_.timeline && !show.tl.tracks.empty() )
	{
		exp_.tlSync    = show.tl.sync;
		exp_.tlPlaying = show.tl.rt.playing;
		show.tl.sync   = TlSync::Internal;
		show.tl.Locate( show, exp_.tlStart );
		show.tl.Play( show, true );
	}
	exp_.started = Now();
	exp_.active  = true;
	return true;
}
bool App::ExportStep( double budgetSec )
{
	if( !exp_.active || !exp_.vw ) return false;
	const double t0 = Now();
	const float dt  = 1.0f / exp_.fps;
	std::vector< uint8_t > px;
	const std::vector< int > noSel;
	do
	{
		if( exp_.frame >= exp_.frames ) return false;
		//show time: tempo, cue list (simulated timecode), effects, snapshots, motors
		show.tempo.beat += dt * std::max( 1.0f, show.tempo.bpm ) / 60.0;
		if( exp_.cueStart ) show.tcSec = exp_.tcStart + exp_.frame * (double)dt;
		RunEffects( show, dt );
		Simulate( show, sim, dt );
		Camera c        = exp_.cam0;
		const float k   = (float)exp_.frame / exp_.frames;
		if( exp_.camMode == 1 ) c.yaw += 360.0f * k;
		else if( exp_.camMode == 2 ) c.yaw += 45.0f * sinf( 2 * 3.14159265f * k );
		video.Sync( show.projectors );
		ren.video        = &video;
		ren.selProjector = -1;
		ren.selScreen    = -1;
		ren.workLight    = show.workLight;
		ren.Render( show, sim, c, noSel, exp_.w, exp_.h );
		int W = 0, H = 0;
		if( !ren.ReadOutput( px, W, H ) || W < ( exp_.w & ~1 ) || H < ( exp_.h & ~1 ) )
		{
			exp_.err = "render / read back";
			return false;
		}
		if( !exp_.vw->Write( px.data() ) )
		{
			exp_.err = "H.264 encoder";
			return false;
		}
		++exp_.frame;
	} while( Now() - t0 < budgetSec );
	return exp_.frame < exp_.frames;
}
void App::ExportFinish( bool ok )
{
	if( !exp_.active ) return;
	const bool closed = exp_.vw && exp_.vw->Close();
	exp_.vw.reset();
	exp_.active   = false;
	ren.projLines = exp_.projLines;
	if( exp_.cueStart ) show.tcSec = -1;
	if( exp_.timeline && !show.tl.tracks.empty() )
	{
		show.tl.Play( show, exp_.tlPlaying );
		show.tl.sync = exp_.tlSync;
	}
	fxT_ = 0;//effects: no jump after the export time
	if( exp_.engineWas ) StartEngine();
	if( ok && closed && exp_.frame >= exp_.frames )
	{
		char b[ 160 ];
		snprintf( b, sizeof( b ), L( "Video exportiert: %s (%d Bilder, %.0f s Rechenzeit)" ), FileName( exp_.path ).c_str(), exp_.frames, Now() - exp_.started );
		SetStatus( b );
	}
	else
		SetStatus( L( "Video-Export abgebrochen" ) + ( exp_.err.empty() ? std::string() : ": " + exp_.err ) );
}
void App::ExportWindow()
{
	if( !expWin_ && !exp_.active ) return;
	ImGui::SetNextWindowSize( ImVec2( 470 * ui, 0 ), ImGuiCond_Appearing );
	bool open = true;
	if( !ImGui::Begin( L( "Video-Export" ), exp_.active ? nullptr : &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize ) )
	{
		ImGui::End();
		return;
	}
	if( exp_.active )
	{
		const float k       = (float)exp_.frame / std::max( 1, exp_.frames );
		const double el     = Now() - exp_.started;
		const double remain = k > 0.01f ? el / k - el : 0;
		char ov[ 64 ];
		snprintf( ov, sizeof( ov ), "%d / %d", exp_.frame, exp_.frames );
		ImGui::ProgressBar( k, ImVec2( 420 * ui, 0 ), ov );
		ImGui::Text( L( "Läuft %.0f s, noch etwa %.0f s" ), el, remain );
		ImGui::TextDisabled( L( "Die Show wird Bild für Bild berechnet (unabhängig von der echten Zeit)." ) );
		if( ImGui::Button( L( "Abbrechen" ) ) )
		{
			exp_.err = L( "abgebrochen" );
			ExportFinish( false );
		}
	}
	else
	{
		ImGui::SetNextItemWidth( 200 * ui );
		ImGui::Combo( L( "Auflösung" ), &exp_.res, "1280 x 720 (HD)\0" "1920 x 1080 (Full HD)\0" "2560 x 1440\0" "3840 x 2160 (4K)\0" );
		ImGui::SetNextItemWidth( 200 * ui );
		ImGui::Combo( L( "Bildrate" ), &exp_.fpsIdx, "25 fps\0" "30 fps\0" "50 fps\0" "60 fps\0" );
		ImGui::SetNextItemWidth( 200 * ui );
		ls::rs::DragFloat( L( "Dauer" ), &exp_.seconds, 0.5f, 1, 7200, "%.0f s" );
		exp_.seconds = Clamp( exp_.seconds, 1.0f, 7200.0f );
		ImGui::SetNextItemWidth( 200 * ui );
		ImGui::Combo( L( "Kamera" ), &exp_.camMode, L( "Aktuelle Ansicht\0Kamerafahrt 360°\0Kamera pendelt ±45°\0" ) );
		ImGui::BeginDisabled( show.cues.empty() );
		ImGui::Checkbox( L( "Cue-Liste von vorn abspielen" ), &exp_.cueStart );
		ImGui::EndDisabled();
		ImGui::SetItemTooltip( L( "Startet Cue 1. Cues mit \"Folgen\" laufen nach, Timecode-Cues zur simulierten Timecode-Zeit." ) );
		if( !show.tl.tracks.empty() )
		{
			ImGui::Checkbox( L( "Time Line abspielen" ), &exp_.timeline );
			ImGui::SetItemTooltip( "%s", L( "Die Show-Time-Line läuft im Video mit (ab der eingestellten Zeit, interner Takt)." ) );
			if( exp_.timeline )
			{
				ImGui::SameLine();
				float ts = (float)exp_.tlStart;
				ImGui::SetNextItemWidth( 120 * ui );
				if( ls::rs::DragFloat( L( "ab (s)##tlexp" ), &ts, 0.1f, 0, 86400, "%.1f s" ) ) exp_.tlStart = std::max( 0.0f, ts );
			}
		}
		if( exp_.cueStart && !show.cues.empty() )
		{
			ImGui::SameLine();
			float tcs = (float)exp_.tcStart;
			ImGui::SetNextItemWidth( 120 * ui );
			if( ls::rs::DragFloat( L( "Timecode ab (s)" ), &tcs, 0.1f, 0, 86400, "%.1f s" ) ) exp_.tcStart = std::max( 0.0f, tcs );
		}
		ImGui::TextDisabled( L( "H.264 / MP4. Effekte und Snapshots laufen weiter wie gerade eingestellt;\nwährend des Exports ruht die MIDI-/OSC-Steuerung." ) );
		if( ImGui::Button( L( "Exportieren..." ), ImVec2( 160 * ui, 0 ) ) )
		{
			std::string p = FileDialog( hwnd, true, L"MP4 (*.mp4)\0*.mp4\0", L"mp4" );
			if( !p.empty() ) ExportStart( p );
		}
	}
	ImGui::End();
	if( !open ) expWin_ = false;
}

//================================================================ Lightshow export (.lightshow, lbshow.h)
bool App::LbxStart( const std::string& path )
{
	if( lbx_.active || exp_.active ) return false;
	LbxOptions o = lbxOpt_;
	o.from       = std::max( 0.0f, lbxFrom_ );
	o.to         = lbxTo_ > 0 ? lbxTo_ : -1;
	lbxEngineWas_ = engineRun_;
	if( engineRun_ ) StopEngine();
	if( !LbxBegin( show, o, path, lbx_ ) )
	{
		SetStatus( L( "Lightshow-Export fehlgeschlagen: " ) + lbx_.err );
		if( lbxEngineWas_ ) StartEngine();
		return false;
	}
	lbx_.started = Now();
	return true;
}
//PATCH list drag & drop: the selection (or the dragged lamp alone) moves in front of / behind the anchor
bool App::MoveFixturesUi( int dragged, int anchorId, bool after )
{
	std::vector< int > ids;
	if( IsSel( dragged ) )
	{
		for( auto& f : show.fixtures )
			if( IsSel( f.id ) ) ids.push_back( f.id );
	}
	else ids.push_back( dragged );
	undoTouched_ = true;
	std::map< int64_t, int64_t > rm;
	std::string why;
	if( !show.MoveFixtures( ids, anchorId, after, &why, &rm ) )
	{
		SetStatus( L( "Verschieben nicht möglich: kein Platz in " ) + why );
		return false;
	}
	bridge.RemapChannels( rm );
	int lamps = 0;
	for( auto& f : show.fixtures )
		for( auto& kv : rm )
			if( kv.second / 1000 == f.universe && kv.second % 1000 == f.address ) { ++lamps; break; }
	char b[ 160 ];
	snprintf( b, sizeof( b ), L( "Reihenfolge geändert: %d Lampe(n) neu adressiert" ), lamps );
	SetStatus( b );
	return true;
}
void App::LbxEnd( bool ok )
{
	if( !lbx_.active ) return;
	const bool done = LbxFinish( show, lbx_, ok );
	if( lbxUiState == 1 ) lbxUiState = 2;
	fxT_ = 0;//effects: no jump after the export time
	if( lbxEngineWas_ ) StartEngine();
	if( done )
	{
		char b[ 200 ];
		snprintf( b, sizeof( b ), L( "Lightshow exportiert: %s (%d Bilder, %.0f s Rechenzeit)" ), FileName( lbx_.path ).c_str(), lbx_.frames, Now() - lbx_.started );
		SetStatus( b );
	}
	else
		SetStatus( L( "Lightshow-Export abgebrochen" ) + ( lbx_.err.empty() ? std::string() : ": " + lbx_.err ) );
}
void App::LbxWindow()
{
	if( !lbxUiTest.empty() && lbxUiState == 0 && engineRun_ && ++lbxUiWait_ > 60 )
	{
		Log( "lightshow ui test: start (engine running)" );
		lbxUiState = LbxStart( lbxUiTest ) ? 1 : 3;
		Log( "lightshow ui test: LbxStart returned, state %d", lbxUiState );
	}
	if( !lbxWin_ && !lbx_.active ) return;
	ImGui::SetNextWindowSize( ImVec2( 470 * ui, 0 ), ImGuiCond_Appearing );
	bool open = true;
	if( !ImGui::Begin( L( "Lightshow exportieren" ), lbx_.active ? nullptr : &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize ) )
	{
		ImGui::End();
		return;
	}
	if( lbx_.active )
	{
		const float k = (float)lbx_.Work() / std::max( 1, lbx_.WorkTotal() );
		char ov[ 64 ];
		if( lbx_.layer < 0 ) snprintf( ov, sizeof( ov ), "%d / %d", lbx_.frame, lbx_.frames );
		else snprintf( ov, sizeof( ov ), L( "Effekt-Ebene %d / %d" ), lbx_.layer + 1, (int)lbx_.layers.size() );
		ImGui::ProgressBar( k, ImVec2( 420 * ui, 0 ), ov );
		ImGui::TextDisabled( L( "Die Show wird Bild für Bild berechnet (unabhängig von der echten Zeit)." ) );
		if( ImGui::Button( L( "Abbrechen" ) ) )
		{
			lbx_.err = L( "abgebrochen" );
			LbxEnd( false );
		}
	}
	else if( show.tl.tracks.empty() )
		ImGui::TextWrapped( "%s", L( "Die Time Line ist leer. Exportiert wird die Time Line – bitte zuerst Spuren anlegen." ) );
	else
	{
		ImGui::TextWrapped( "%s", L( "Rechnet die Time Line mit allen Snapshots, Effekten und Submastern zu fertigen DMX-Bildern (40 pro Sekunde) – zum Abspielen ohne LightStage, z. B. mit Lightbrick auf dem Brick Pro (USB-DMX / Art-Net)." ) );
		ImGui::SetNextItemWidth( 140 * ui );
		ls::rs::DragFloat( L( "ab (s)##lbx" ), &lbxFrom_, 0.1f, 0, (float)show.tl.length, "%.1f s" );
		ImGui::SameLine();
		ImGui::SetNextItemWidth( 140 * ui );
		ls::rs::DragFloat( L( "bis (s)##lbx" ), &lbxTo_, 0.1f, 0, (float)show.tl.length, lbxTo_ > 0 ? "%.1f s" : L( "Ende" ) );
		lbxFrom_ = Clamp( lbxFrom_, 0.0f, (float)show.tl.length );
		lbxTo_   = Clamp( lbxTo_, 0.0f, (float)show.tl.length );
		ImGui::Checkbox( L( "3D-Ansicht mitnehmen" ), &lbxOpt_.visual );
		ImGui::SetItemTooltip( "%s", L( "Lichtfarbe, Helligkeit und Strahlrichtung jeder Lampe aus der Simulation – für eine 3D-Ansicht im Abspielgerät." ) );
		const double len = ( lbxTo_ > 0 ? lbxTo_ : show.tl.length ) - lbxFrom_;
		ImGui::TextDisabled( L( "%.1f s, %d Universen, %d Lampen. Während des Exports ruht die MIDI-/OSC-Steuerung." ), len, (int)show.universes.size(), (int)show.fixtures.size() );
		if( ImGui::Button( L( "Exportieren..." ), ImVec2( 160 * ui, 0 ) ) )
		{
			std::string p = FileDialog( hwnd, true, L"Lightshow (*.lightshow)\0*.lightshow\0", L"lightshow" );
			if( !p.empty() ) LbxStart( p );
		}
	}
	ImGui::End();
	if( !open ) lbxWin_ = false;
}

//================================================================ position tools (fan / aim)
//================================================================ Lightshow import (.lightshow, lbshow.h)
bool App::LbiOpen( const std::string& path )
{
	std::string err;
	LbxFile f;
	if( !LbxRead( path, f, &err ) )
	{
		SetStatus( L( "Lightshow kann nicht gelesen werden: " ) + err );
		return false;
	}
	lbiFile_ = std::move( f );
	lbiPath_ = path;
	lbiOpt_.at      = 0;
	if( show.fixtures.empty() && show.tl.tracks.empty() ) lbiOpt_.newShow = true;
	lbiWin_  = true;
	return true;
}
bool App::LbiRun()
{
	if( lbiPath_.empty() ) return false;
	LbxImportOptions o = lbiOpt_;
	if( o.newShow )
	{
		if( !ConfirmClose() ) return false;
		//own lamps of the source show that are missing here: into library\user first (like opening a show)
		for( auto& kv : lbiFile_.patch[ "userFixtures" ].o )
			if( !lib.Find( kv.first ) ) RegisterEmbeddedFixture( kv.first, kv.second );
		show.tl.Play( show, false );
	}
	else
		o.at = std::max( 0.0, o.at );
	std::string err;
	lbiRes_ = LbxImportResult();
	if( !LbxImport( show, lib, lbiFile_, lbiPath_, o, &lbiRes_, &err ) )
	{
		SetStatus( L( "Lightshow-Import fehlgeschlagen: " ) + err );
		return false;
	}
	if( o.newShow )
	{
		for( auto& m : show.models ) ren.EnsureModel( m, nullptr );
		sel.clear();
		show.fxSelId = 0;
		io.Apply();
		UndoReset();
		AutosaveClear();
		show.dirty = true;
		show.tl.Locate( show, o.at );
	}
	else
		undoTouched_ = true;
	char b[ 256 ];
	if( o.asTrack )
		snprintf( b, sizeof( b ), L( "Lightshow importiert: %s als Lightshow-Spur (%.1f s)" ), FileName( lbiPath_ ).c_str(), (double)lbiFile_.frames / std::max( 1, lbiFile_.fps ) );
	else
		snprintf( b, sizeof( b ), L( "Lightshow importiert: %s - %d Spuren, %d Time Lanes, %d Keys" ), FileName( lbiPath_ ).c_str(), lbiRes_.tracks, lbiRes_.lanes, lbiRes_.keys );
	SetStatus( b );
	tlSelectTab_ = true;
	lbiWin_  = false;
	lbiFile_ = LbxFile();//frames are no longer needed (a Lightshow track reads the file itself)
	return true;
}
std::string App::OpenLightshowDialog()
{
	return FileDialog( hwnd, false, L"Lightshow (*.lightshow)\0*.lightshow\0", L"lightshow" );
}
//Lightshow track -> DMX time lanes in this show (same place on the timeline); the track is removed
bool App::TlConvertShowTrack( uint32_t uid )
{
	TlTrack* t = show.tl.Find( uid );
	if( !t || t->kind != TlTrackKind::Show ) return false;
	LbxFile f;
	std::string err;
	if( !LbxRead( t->file, f, &err ) )
	{
		SetStatus( L( "Lightshow kann nicht gelesen werden: " ) + err );
		return false;
	}
	LbxImportOptions o;
	o.newShow   = false;
	o.asTrack   = false;
	o.at        = std::max( 0.0, t->offset );
	o.tolerance = lbiOpt_.tolerance;
	o.markers   = false;
	o.tempo     = false;
	LbxImportResult r;
	if( !LbxImport( show, lib, f, t->file, o, &r, &err ) )
	{
		SetStatus( L( "Umwandeln fehlgeschlagen: " ) + err );
		return false;
	}
	for( size_t i = 0; i < show.tl.tracks.size(); ++i )
		if( show.tl.tracks[ i ].uid == uid ) { show.tl.tracks.erase( show.tl.tracks.begin() + i ); break; }
	undoTouched_ = true;
	char b[ 160 ];
	snprintf( b, sizeof( b ), L( "Lightshow-Spur umgewandelt: %d Spuren, %d Time Lanes, %d Keys" ), r.tracks, r.lanes, r.keys );
	SetStatus( b );
	return true;
}
void App::LbiWindow()
{
	if( !lbiWin_ ) return;
	const float ui = ImGui::GetFontSize() / 15.0f;
	bool open      = true;
	ImGui::SetNextWindowPos( ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2( 0.5f, 0.5f ) );
	if( !ImGui::Begin( L( "Lightshow importieren" ), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize ) )
	{
		ImGui::End();
		if( !open ) lbiWin_ = false;
		return;
	}
	const LbxFile& f = lbiFile_;
	const double dur = (double)f.frames / std::max( 1, f.fps );
	ImGui::TextUnformatted( FileName( lbiPath_ ).c_str() );
	{
		char d[ 32 ];
		const int sec = (int)( dur + 0.5 );
		snprintf( d, sizeof( d ), sec >= 3600 ? "%d:%02d:%02d" : "%d:%02d", sec >= 3600 ? sec / 3600 : sec / 60, sec >= 3600 ? ( sec / 60 ) % 60 : sec % 60, sec % 60 );
		ImGui::TextDisabled( L( "\"%s\" - %s, %d fps, %d Universen, %d Lampen, %d Marker/Cues" ), f.info[ "name" ].Str().c_str(), d, f.fps, (int)f.info[ "universes" ].Size(),
		                     (int)f.scene[ "fixtures" ].Size(), (int)f.cues[ "cues" ].Size() );
		if( f.patch.IsNull() ) ImGui::TextColored( ImVec4( 1, 0.75f, 0.35f, 1 ), "%s", L( "Ältere Datei ohne Setup: eine neue Show bekommt Lampen mit Standardprofilen." ) );
	}
	ImGui::Separator();
	ImGui::TextUnformatted( L( "Ziel" ) );
	if( ImGui::RadioButton( L( "Neue Show" ), lbiOpt_.newShow ) ) lbiOpt_.newShow = true;
	if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Bühne, Universen und Lampen aus der Datei. Die aktuelle Show wird geschlossen." ) );
	ImGui::SameLine();
	if( ImGui::RadioButton( L( "In die aktuelle Show" ), !lbiOpt_.newShow ) ) lbiOpt_.newShow = false;
	if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Nur Time Line: Kanäle nach Universum und Adresse. Fehlende Universen werden angelegt." ) );
	ImGui::Spacing();
	ImGui::TextUnformatted( L( "Art" ) );
	if( ImGui::RadioButton( L( "Time Lanes (bearbeitbar)" ), !lbiOpt_.asTrack ) ) lbiOpt_.asTrack = false;
	if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Pro Lampe eine Time-Line-Spur mit DMX-Time-Lanes, Kurven ausgedünnt wie bei einer Aufnahme.\nDanach wie jede Time Line bearbeitbar." ) );
	ImGui::SameLine();
	if( ImGui::RadioButton( L( "Lightshow-Spur (1:1)" ), lbiOpt_.asTrack ) ) lbiOpt_.asTrack = true;
	if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Eine Spur, die die aufgezeichneten DMX-Bilder exakt abspielt (aus der Datei).\nVerschiebbar, nicht über Keys bearbeitbar." ) );
	if( !lbiOpt_.asTrack )
	{
		ImGui::SetNextItemWidth( 200 * ui );
		ImGui::SliderFloat( L( "Toleranz##lbi" ), &lbiOpt_.tolerance, 0.5f, 8.0f, L( "%.1f DMX-Stufen" ), ImGuiSliderFlags_Logarithmic );
		if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Wie weit die Kurven vom Original abweichen dürfen.\nKlein = genauer, mehr Keys. 0,5 = praktisch exakt." ) );
	}
	if( !lbiOpt_.newShow )
	{
		float a = (float)lbiOpt_.at;
		ImGui::SetNextItemWidth( 200 * ui );
		if( ImGui::DragFloat( L( "Start auf der Time Line (s)##lbi" ), &a, 0.05f, 0, 36000, "%.2f s" ) ) lbiOpt_.at = std::max( 0.0f, a );
		ImGui::SameLine();
		if( ImGui::SmallButton( L( "Abspielkopf##lbi" ) ) ) lbiOpt_.at = show.tl.rt.pos;
	}
	ImGui::Checkbox( L( "Marker und Cues als Marker" ), &lbiOpt_.markers );
	ImGui::Checkbox( lbiOpt_.newShow ? L( "Tempo und Loop übernehmen" ) : L( "Tempo übernehmen" ), &lbiOpt_.tempo );
	ImGui::Separator();
	if( ImGui::Button( L( "Importieren" ), ImVec2( 140 * ui, 0 ) ) ) LbiRun();
	ImGui::SameLine();
	if( ImGui::Button( L( "Abbrechen" ), ImVec2( 100 * ui, 0 ) ) ) open = false;
	ImGui::End();
	if( !open )
	{
		lbiWin_  = false;
		lbiFile_ = LbxFile();
	}
}

void App::FanBegin( const std::vector< int >& ids )
{
	fanBase_.clear();
	std::vector< FixtureInst* > fx;
	for( int id : ids )
		if( FixtureInst* f = show.FindFixture( id ) )
			if( show.HasAttr( *f, Attr::Pan ) || show.HasAttr( *f, Attr::Tilt ) ) fx.push_back( f );
	std::stable_sort( fx.begin(), fx.end(), []( FixtureInst* a, FixtureInst* b ) { return fabsf( a->pos.x - b->pos.x ) > 0.01f ? a->pos.x < b->pos.x : a->pos.z < b->pos.z; } );
	for( auto* f : fx ) fanBase_.push_back( { f->id, { show.GetAttrBase( *f, Attr::Pan ), show.GetAttrBase( *f, Attr::Tilt ) } } );
}
int App::FanApply( float panDeg, float tiltDeg )
{
	const int n = (int)fanBase_.size();
	int done    = 0;
	for( int i = 0; i < n; ++i )
	{
		FixtureInst* f = show.FindFixture( fanBase_[ i ].first );
		if( !f || !f->def ) continue;
		const float k  = n > 1 ? (float)i / ( n - 1 ) - 0.5f : 0.0f;//-0.5 .. 0.5
		const float pr = f->def->panRange > 1 ? f->def->panRange : 540.0f, tr = f->def->tiltRange > 1 ? f->def->tiltRange : 270.0f;
		if( show.HasAttr( *f, Attr::Pan ) ) show.SetAttr( *f, Attr::Pan, fanBase_[ i ].second.first + k * panDeg / pr * ( f->invertPan ? -1.0f : 1.0f ) );
		if( show.HasAttr( *f, Attr::Tilt ) ) show.SetAttr( *f, Attr::Tilt, fanBase_[ i ].second.second + k * tiltDeg / tr * ( f->invertTilt ? -1.0f : 1.0f ) );
		show.progPalPos.erase( f->id );
		++done;
	}
	return done;
}
int App::AimSelected( const V3& target )
{
	int n = 0;
	for( int id : sel )
		if( FixtureInst* f = show.FindFixture( id ) )
			if( AimAt( show, *f, target ) )
			{
				show.progPalPos.erase( id );
				++n;
			}
	show.dirty = true;
	return n;
}
bool App::RayStage( float mx, float my, float w, float h, V3& hit )
{
	M4 inv         = Inverse( ren.viewProj );
	const float nx = mx / w * 2 - 1, ny = 1 - my / h * 2;
	auto unproj    = [ & ]( float z ) {
		float v[ 4 ] = { nx, ny, z, 1 }, r[ 4 ];
		for( int i = 0; i < 4; ++i ) r[ i ] = inv( i, 0 ) * v[ 0 ] + inv( i, 1 ) * v[ 1 ] + inv( i, 2 ) * v[ 2 ] + inv( i, 3 ) * v[ 3 ];
		return V3( r[ 0 ] / r[ 3 ], r[ 1 ] / r[ 3 ], r[ 2 ] / r[ 3 ] );
	};
	const V3 o = unproj( -1 ), d = Norm( unproj( 1 ) - o );
	if( fabsf( d.y ) < 1e-5f ) return false;
	float td = 0;
	if( show.RayDeckTop( o, d, td ) ) { hit = o + d * td; return true; }
	const float t = -o.y / d.y;
	if( t <= 0 ) return false;
	hit = o + d * t;
	return true;
}

void App::Pick( float mx, float my, float w, float h, bool add )
{
	M4 inv = Inverse( ren.viewProj );
	float nx = mx / w * 2 - 1, ny = 1 - my / h * 2;
	V3 a = XformP( inv, V3( nx, ny, -1 ) ), b = XformP( inv, V3( nx, ny, 1 ) );
	//XformP ignores w: do it manually
	auto unproj = [ & ]( float z ) {
		float v[ 4 ] = { nx, ny, z, 1 }, r[ 4 ];
		for( int i = 0; i < 4; ++i ) r[ i ] = inv( i, 0 ) * v[ 0 ] + inv( i, 1 ) * v[ 1 ] + inv( i, 2 ) * v[ 2 ] + inv( i, 3 ) * v[ 3 ];
		return V3( r[ 0 ] / r[ 3 ], r[ 1 ] / r[ 3 ], r[ 2 ] / r[ 3 ] );
	};
	a = unproj( -1 );
	b = unproj( 1 );
	V3 o = a, d = Norm( b - a );
	int best = -1;
	float bt = 1e9f;
	for( auto& f : show.fixtures )
	{
		V3 e     = f.def ? BodyExtents( *f.def ) : V3( 0.3f, 0.3f, 0.3f );
		V3 c     = XformP( FixtureBase( f ), V3( 0, e.y * 0.5f, 0 ) );
		float r  = std::max( { e.x, e.y, e.z } ) * 0.6f + 0.05f;
		V3 oc    = o - c;
		float bq = Dot( oc, d ), cq = Dot( oc, oc ) - r * r;
		float disc = bq * bq - cq;
		if( disc < 0 ) continue;
		float t = -bq - sqrtf( disc );
		if( t > 0 && t < bt ) { bt = t; best = f.id; }
	}
	if( best >= 0 ) Select( best, add, false );
	else if( !add ) sel.clear();
}

void App::PanelViewport()
{
	ImVec2 avail = ImGui::GetContentRegionAvail();
	int w = (int)avail.x, h = (int)avail.y;
	viewW_ = w;
	viewH_ = h;
	if( w < 16 || h < 16 ) return;
	if( perfMode_ && perfView_ == 0 )
	{
		video.Clear();//no video decoding while nothing is shown
		PanelLive( (float)w, (float)h );
		return;
	}
	if( ShouldRenderViewport( Now() ) && !exp_.active )
	{
		int rw = w, rh = h;
		if( perfMode_ && perfView_ == 1 )
		{
			rw = std::max( 16, (int)( w * perfScale_ ) );
			rh = std::max( 16, (int)( h * perfScale_ ) );
		}
		RenderViewport( rw, rh );
	}
	ImVec2 p0 = ImGui::GetCursorScreenPos();
	ImGui::Image( (ImTextureID)(intptr_t)ren.Output(), avail, ImVec2( 0, 1 ), ImVec2( 1, 0 ) );
	ImGui::SetCursorScreenPos( p0 );
	ImGui::SetNextItemAllowOverlap();//the toolbar drawn on top of it must stay clickable
	ImGui::InvisibleButton( "##vp", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle );
	viewportHovered_ = ImGui::IsItemHovered();
	ImGuiIO& io_     = ImGui::GetIO();
	bool active      = ImGui::IsItemActive();
	if( ImGui::IsItemActivated() ) dragDist_ = 0;
	if( active )
	{
		ImVec2 d = io_.MouseDelta;
		dragDist_ += fabsf( d.x ) + fabsf( d.y );
		bool pan = ImGui::IsMouseDown( ImGuiMouseButton_Middle ) || ( ImGui::IsMouseDown( ImGuiMouseButton_Left ) && io_.KeyShift );
		if( pan )
		{
			M4 v     = cam.View();
			V3 right( v( 0, 0 ), v( 0, 1 ), v( 0, 2 ) ), up( v( 1, 0 ), v( 1, 1 ), v( 1, 2 ) );
			float k  = cam.dist * 0.0016f;
			cam.target += right * ( -d.x * k ) + up * ( d.y * k );
		}
		else if( dragDist_ > 3 )
		{
			cam.yaw -= d.x * 0.25f;
			cam.pitch = Clamp( cam.pitch + d.y * 0.25f, -10.0f, 89.0f );
		}
	}
	if( ImGui::IsItemDeactivated() && dragDist_ <= 3 && ImGui::IsMouseReleased( ImGuiMouseButton_Left ) )
	{
		ImVec2 m = io_.MousePos;
		V3 hit;
		if( aimPick_ )
		{
			if( RayStage( m.x - p0.x, m.y - p0.y, avail.x, avail.y, hit ) )
			{
				aimPt_      = hit;
				const int n = AimSelected( hit );
				char b[ 128 ];
				snprintf( b, sizeof( b ), L( "%d Lampen zielen auf %.1f / %.1f / %.1f m" ), n, hit.x, hit.y, hit.z );
				SetStatus( b );
			}
			aimPick_ = false;
		}
		else
			Pick( m.x - p0.x, m.y - p0.y, avail.x, avail.y, io_.KeyCtrl );
	}
	if( viewportHovered_ && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) ) FrameSelection();
	if( viewportHovered_ && io_.MouseWheel != 0 ) cam.dist = Clamp( cam.dist * powf( 0.9f, io_.MouseWheel ), 1.0f, 150.0f );

	ImDrawList* dl = ImGui::GetWindowDrawList();
	//labels
	if( showLabels )
	{
		ImGui::PushFont( fontMono );
		for( auto& f : show.fixtures )
		{
			V3 e  = f.def ? BodyExtents( *f.def ) : V3( 0.3f, 0.3f, 0.3f );
			V3 wp = XformP( FixtureBase( f ), V3( 0, -0.12f, 0 ) );
			const M4& m = ren.viewProj;
			float cx = m( 0, 0 ) * wp.x + m( 0, 1 ) * wp.y + m( 0, 2 ) * wp.z + m( 0, 3 );
			float cy = m( 1, 0 ) * wp.x + m( 1, 1 ) * wp.y + m( 1, 2 ) * wp.z + m( 1, 3 );
			float cw = m( 3, 0 ) * wp.x + m( 3, 1 ) * wp.y + m( 3, 2 ) * wp.z + m( 3, 3 );
			if( cw <= 0.1f ) continue;
			float sx = p0.x + ( cx / cw * 0.5f + 0.5f ) * avail.x, sy = p0.y + ( 0.5f - cy / cw * 0.5f ) * avail.y;
			char b[ 16 ];
			snprintf( b, sizeof( b ), "%d", f.id );
			ImVec2 ts = ImGui::CalcTextSize( b );
			bool s    = IsSel( f.id );
			dl->AddRectFilled( ImVec2( sx - ts.x * 0.5f - 3, sy - ts.y * 0.5f - 1 ), ImVec2( sx + ts.x * 0.5f + 3, sy + ts.y * 0.5f + 1 ), s ? IM_COL32( 60, 130, 230, 220 ) : IM_COL32( 10, 12, 16, 170 ), 3 );
			dl->AddText( ImVec2( sx - ts.x * 0.5f, sy - ts.y * 0.5f ), s ? IM_COL32( 255, 255, 255, 255 ) : FixtureColor( f.id ), b );
			(void)e;
		}
		ImGui::PopFont();
	}
	TimelineViewportOverlay( dl, p0, avail );
	//toolbar: wraps into a second row when the viewport is narrow; backdrop drawn behind it afterwards
	const float tbLeft = p0.x + 14, tbRight = p0.x + avail.x - 12;
	float tbMaxX = tbLeft, tbMaxY = p0.y + 11;
	dl->ChannelsSplit( 2 );
	dl->ChannelsSetCurrent( 1 );
	auto tbTrack = [ & ] { tbMaxX = std::max( tbMaxX, ImGui::GetItemRectMax().x ); tbMaxY = std::max( tbMaxY, ImGui::GetItemRectMax().y ); };
	auto Flow    = [ & ]( float w, float gap = -1 ) {
		tbTrack();
		const float x = ImGui::GetItemRectMax().x + ( gap >= 0 ? gap : ImGui::GetStyle().ItemSpacing.x );
		if( x + w > tbRight ) ImGui::SetCursorScreenPos( ImVec2( tbLeft, tbMaxY + 4 * ui ) );//next row (no SameLine: it would keep the row's y)
		else ImGui::SameLine( 0, gap );
	};
	ImGui::SetCursorScreenPos( ImVec2( p0.x + 14, p0.y + 11 ) );
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 8, 4 ) );
	ImGui::PushStyleVar( ImGuiStyleVar_GrabMinSize, 4.0f );
	ImGui::PushStyleColor( ImGuiCol_FrameBg, ImVec4( 1, 1, 1, 0.075f ) );
	ImGui::PushStyleColor( ImGuiCol_FrameBgHovered, ImVec4( 1, 1, 1, 0.12f ) );
	ImGui::PushStyleColor( ImGuiCol_FrameBgActive, ImVec4( 1, 1, 1, 0.16f ) );
	ImGui::PushStyleColor( ImGuiCol_SliderGrab, ImVec4( 0.45f, 0.70f, 1.0f, 0.9f ) );
	ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 1, 1, 1, 0.075f ) );
	//switches as chips (pill buttons): lit = on. Smaller than check boxes, so the stage stays visible
	auto chipW = [ & ]( const char* l ) { return ls::ChipWidth( l ); };
	auto Chip  = [ & ]( const char* l, bool* v, ImVec4 on = ImVec4( 0.35f, 0.65f, 1.0f, 1 ) ) { return ls::Chip( l, v, on ); };
	//fold: only master + LIVE stay (more stage, e.g. while programming the Time Line)
	if( ImGui::ArrowButton( "##vpfold", vpBarFold_ ? ImGuiDir_Right : ImGuiDir_Left ) ) { vpBarFold_ = !vpBarFold_; SaveSettings(); }
	ImGui::SetItemTooltip( L( "Leiste ein-/ausklappen" ) );
	Flow( 150 * ui );
	{
		//master 0..1000 %: up to 100 % like a grand master, above that more light out of every fixture
		float pct  = show.master * 100.0f;
		bool boost = show.master > 1.0001f;
		if( boost ) ImGui::PushStyleColor( ImGuiCol_SliderGrab, ImVec4( 1.0f, 0.55f, 0.15f, 1 ) );
		ImGui::SetNextItemWidth( 150 * ui );
		if( ls::rs::SliderFloat( "##master", &pct, 0, 1000, "Master %.0f %%", ImGuiSliderFlags_Logarithmic ) ) { show.master = Clamp( pct / 100.0f, 0.0f, 10.0f ); show.dirty = true; }
		if( boost ) ImGui::PopStyleColor();
		ImGui::SetItemTooltip( L( "Alle Scheinwerfer: 100 %% = Nennleistung, bis 1000 %%. Rechtsklick: 100 %% / Blackout. Strg+Klick: Wert eingeben" ) );
		if( ImGui::BeginPopupContextItem( "##mastermenu" ) )
		{
			if( ImGui::MenuItem( "100 %" ) ) show.master = 1.0f;
			if( ImGui::MenuItem( "200 %" ) ) show.master = 2.0f;
			if( ImGui::MenuItem( "500 %" ) ) show.master = 5.0f;
			if( ImGui::MenuItem( "0 % (Blackout)" ) ) show.master = 0.0f;
			ImGui::Separator();
			if( ImGui::MenuItem( L( "OSC-Adresse kopieren: /ls/master" ) ) ) ImGui::SetClipboardText( "/ls/master" );
			ImGui::EndPopup();
		}
		LearnHook( [ & ] { MidiMap m; m.target = MidiTarget::Master; return m; }() );
	}
	if( !vpBarFold_ )
	{
	Flow( 130 * ui );
	ImGui::SetNextItemWidth( 130 * ui );
	ls::rs::SliderFloat( "##haze", &show.stage.haze, 0, 1, "Haze %.2f" );
	if( ImGui::IsItemEdited() ) show.dirty = true;
	LearnHook( [ & ] { MidiMap m; m.target = MidiTarget::Haze; return m; }() );
	if( !learn_ ) OscContext( "/ls/haze" );
	Flow( 130 * ui + ImGui::GetFrameHeight() + 2 * ui );
	ImGui::SetNextItemWidth( 130 * ui );
	if( ls::rs::SliderFloat( "##env", &show.stage.envLight, 0, 4, L( "Umgebung %.2f" ), ImGuiSliderFlags_Logarithmic ) ) show.dirty = true;
	ImGui::SetItemTooltip( L( "Umgebungslicht (Raum, Boden, Himmel) - unabhängig von den Scheinwerfern" ) );
	OscContext( "/ls/stage/envlight" );
	Flow( ImGui::GetFrameHeight(), 2 * ui );
	if( ImGui::ColorEdit3( "##envcol", &show.stage.ambientColor.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel ) ) show.dirty = true;
	ImGui::SetItemTooltip( L( "Farbe des Umgebungslichts" ) );
	Flow( 150 * ui );
	ImGui::SetNextItemWidth( 150 * ui );
	ls::rs::SliderFloat( "##exp", &show.stage.exposure, 0.1f, 4, L( "Belichtung %.2f" ) );
	ImGui::SetItemTooltip( L( "Kamera-Belichtung: wirkt auf das ganze Bild" ) );
	OscContext( "/ls/stage/exposure" );
	Flow( 130 * ui );
	{
		//beams: strength 0..200 % instead of only on / off (0 = off)
		float bp = show.stage.beams ? show.stage.beamLevel * 100.0f : 0.0f;
		ImGui::SetNextItemWidth( 130 * ui );
		if( ls::rs::SliderFloat( "##beams", &bp, 0, 200, L( "Kegel %.0f %%" ) ) )
		{
			show.stage.beams = bp > 0.5f;
			if( show.stage.beams ) show.stage.beamLevel = bp / 100.0f;
			show.dirty = true;
		}
		ImGui::SetItemTooltip( "%s", L( "Stärke der Lichtkegel im Dunst: 0 = aus, 100 = normal, bis 200" ) );
		OscContext( "/ls/stage/beamlevel" );
	}
	Flow( chipW( L( "Schatten" ) ) );
	if( Chip( L( "Schatten" ), &show.stage.shadows ) ) show.dirty = true;
	OscContext( "/ls/stage/shadows" );
	//2.1 quality level + auto throttling (display only)
	for( int q = 0; q < 3; ++q )
	{
		if( q == 0 ) Flow( chipW( QualityName( 0 ) ) + chipW( QualityName( 1 ) ) + chipW( QualityName( 2 ) ) + chipW( "Auto" ) + 3 * ImGui::GetStyle().ItemSpacing.x + 4 * ui );//the group stays on one row
		else ImGui::SameLine( 0, 2 * ui );
		bool on = quality_ == q;
		ImGui::PushID( 9100 + q );
		if( Chip( QualityName( q ), &on ) ) { quality_ = q; qualityEff_ = q; SaveSettings(); }
		ImGui::PopID();
		UiMark( std::string( "vp.quality" ) + std::to_string( q ), ImGui::GetItemRectMin(), ImGui::GetItemRectMax() );
		ImGui::SetItemTooltip( "%s", L( "Darstellungsqualität: Entwurf (schnell, ohne Schatten/Glow), Normal (wie bisher),\nHoch (Bodenspiegelungen, Kontaktschatten, weiche Schatten, feinere Lichtkegel).\nNur Anzeige - DMX bleibt unverändert, der Video-Export rechnet immer in Hoch." ) );
	}
	ImGui::SameLine();
	if( Chip( "Auto", &qualityAuto_ ) ) { qualityEff_ = quality_; SaveSettings(); }
	ImGui::SetItemTooltip( "%s", L( "Bei niedriger Bildrate automatisch eine Stufe herunter, danach wieder hoch" ) );
	Flow( chipW( L( "Arbeitslicht" ) ) );
	{
		Chip( L( "Arbeitslicht" ), &show.workLight, ImVec4( 1.0f, 0.78f, 0.25f, 1 ) );
		ImGui::SetItemTooltip( L( "Alles hell sehen, auch ohne Scheinwerfer (Taste L). Nur Anzeige, DMX bleibt unverändert." ) );
		OscContext( "/ls/worklight" );
	}
	Flow( chipW( L( "Nr." ) ) );
	Chip( L( "Nr." ), &showLabels );
	if( !show.projectors.empty() )
	{
		Flow( chipW( L( "Proj.-Linien" ) ) );
		if( Chip( L( "Proj.-Linien" ), &ren.projLines ) ) SaveSettings();
		ImGui::SetItemTooltip( L( "Umrisslinien der Projektor-Lichtkegel ein/aus" ) );
	}
	Flow( chipW( L( "Demo-Bewegung" ) ) );
	Chip( L( "Demo-Bewegung" ), &demoFx );
	}
	Flow( ImGui::CalcTextSize( "LIVE" ).x + ImGui::GetStyle().FramePadding.x * 2 );
	ImGui::PushStyleColor( ImGuiCol_Button, perfMode_ ? ImVec4( 0.8f, 0.15f, 0.12f, 1 ) : ImVec4( 1, 1, 1, 0.075f ) );
	if( ImGui::Button( "LIVE" ) ) SetPerfMode( !perfMode_ );
	ImGui::PopStyleColor();
	ImGui::SetItemTooltip( L( "Performance-Modus (F12): Eingaben sofort zu DMX, Anzeige gedrosselt. Rechtsklick: Einstellungen" ) );
	if( ImGui::IsItemClicked( ImGuiMouseButton_Right ) ) perfWin_ = true;
	tbTrack();
	ImGui::PopStyleColor( 5 );
	ImGui::PopStyleVar( 2 );
	dl->ChannelsSetCurrent( 0 );
	dl->AddRectFilled( ImVec2( p0.x + 6, p0.y + 5 ), ImVec2( tbMaxX + 8, tbMaxY + 6 ), IM_COL32( 10, 12, 17, 190 ), 10 * ui );
	dl->AddRect( ImVec2( p0.x + 6, p0.y + 5 ), ImVec2( tbMaxX + 8, tbMaxY + 6 ), IM_COL32( 255, 255, 255, 26 ), 10 * ui );
	dl->ChannelsMerge();
	if( showStats_ )
	{
		char b[ 200 ];
		snprintf( b, sizeof( b ), L( "%d Lampen  %d Lichter  %d Schatten  %d Kegel  Haze %.2f  %.1f ms" ), (int)show.fixtures.size(), ren.stats.lights, ren.stats.shadows, ren.stats.beams, sim.haze, ren.stats.ms );
		ImGui::PushFont( fontMono );
		dl->AddText( ImVec2( p0.x + 12, p0.y + avail.y - 22 ), IM_COL32( 150, 160, 175, 200 ), b );
		ImGui::PopFont();
	}
	if( perfMode_ )
	{
		DmxIO::Latency lt = io.GetLatency();
		char lat[ 32 ] = "-", b[ 200 ];
		if( lt.n ) snprintf( lat, sizeof( lat ), "%.1f ms", lt.avgMs );
		snprintf( b, sizeof( b ), L( "LIVE   Latenz %s   CPU %.1f %%   Engine %.0f Hz" ), lat, cpu_, engineHz );
		ImGui::PushFont( fontMono );
		ImVec2 ts = ImGui::CalcTextSize( b );
		dl->AddRectFilled( ImVec2( p0.x + avail.x - ts.x - 20, p0.y + 8 ), ImVec2( p0.x + avail.x - 6, p0.y + 14 + ts.y ), IM_COL32( 120, 20, 16, 220 ), 4 );
		dl->AddText( ImVec2( p0.x + avail.x - ts.x - 13, p0.y + 11 ), IM_COL32( 255, 230, 225, 255 ), b );
		ImGui::PopFont();
	}
	if( learn_ )
	{
		const char* t = learnArmed_ ? learnText_.c_str() : L( "MIDI LEARN: Regler oder Kanal anklicken" );
		ImVec2 ts     = ImGui::CalcTextSize( t );
		ImVec2 c( p0.x + avail.x * 0.5f - ts.x * 0.5f, tbMaxY + 14 );
		dl->AddRectFilled( ImVec2( c.x - 10, c.y - 5 ), ImVec2( c.x + ts.x + 10, c.y + ts.y + 5 ), IM_COL32( 200, 150, 20, 230 ), 4 );
		dl->AddText( c, IM_COL32( 20, 15, 5, 255 ), t );
	}
}

//================================================================ MIDI
bool App::LearnHook( const MidiMap& tmpl )
{
	if( !learn_ )
	{
		//right click: put this target on an Ableton knob / button
		if( tmpl.target == MidiTarget::FixtureAttr ) ablKeyNext_ = MidiTargetKey( tmpl );//the slider's OscContext shows the menu
		else if( tmpl.target == MidiTarget::DmxChannel || tmpl.target == MidiTarget::Haze )
			if( ImGui::BeginPopupContextItem( "##abl" ) )
			{
				ImGui::TextDisabled( "%s", tmpl.Describe( show ).c_str() );
				AbletonAssignMenu( MidiTargetKey( tmpl ) );
				if( tmpl.target == MidiTarget::DmxChannel )
				{
					ImGui::Separator();
					TimelineChannelMenu( tmpl.universe, tmpl.channelDmx );
				}
				ImGui::EndPopup();
			}
		return false;
	}
	ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
	bool hov = ImGui::IsItemHovered();
	ImGui::GetWindowDrawList()->AddRect( ImVec2( a.x - 1, a.y - 1 ), ImVec2( b.x + 1, b.y + 1 ), hov ? IM_COL32( 255, 220, 60, 255 ) : IM_COL32( 230, 170, 30, 160 ), 3, 0, hov ? 2.0f : 1.0f );
	if( ImGui::IsItemClicked( ImGuiMouseButton_Left ) || ImGui::IsItemActivated() )
	{
		learnTmpl_  = tmpl;
		learnArmed_ = true;
		learnText_  = L( "Bewege jetzt einen MIDI-Regler für: " ) + tmpl.Describe( show );
		return true;
	}
	return false;
}

void App::OscListenerUpdate()
{
	//(re)start the listener when the setting or the port changes; a busy port is retried every few seconds
	const bool want = show.output.oscIn;
	static double retryAt = 0;
	if( !want && osc.Running() ) osc.Stop();
	if( want && ( !osc.Running() || osc.Port() != show.output.oscPort ) && Now() >= retryAt )
	{
		if( !osc.Start( show.output.oscPort ) ) retryAt = Now() + 3.0;
	}
}
void App::BridgeListenerUpdate()
{
	//the bridge runs with the OSC input (it is the control path); HTTP port busy -> retried every few seconds
	static double retryAt = 0;
	if( !bridgeOn )
	{
		if( bridge.Running() ) bridge.Stop();
		return;
	}
	if( !show.output.oscIn ) show.output.oscIn = true;
	if( bridge.Failed() )//port busy (found in the background thread): retry later
	{
		SetStatus( L( "Bridge: " ) + bridge.error );
		bridge.Stop();
		retryAt = Now() + 3.0;
	}
	if( ( !bridge.Running() || bridge.HttpPort() != bridgeHttpPort ) && Now() >= retryAt )
	{
		std::string err;
		if( !bridge.Start( bridgeHttpPort, &err ) )
		{
			retryAt = Now() + 3.0;
			SetStatus( L( "Bridge: " ) + err );
		}
	}
}
void App::ExportManifest()
{
	std::string p = FileDialog( hwnd, true, L"LightStage-Manifest (*.lsmanifest)\0*.lsmanifest\0JSON (*.json)\0*.json\0", L"lsmanifest" );
	if( p.empty() ) return;
	SetStatus( WriteText( p, bridge.ManifestJson( show ) ) ? L( "Manifest gespeichert: " ) + FileName( p ) : L( "Manifest konnte nicht gespeichert werden" ) );
}
void App::PollOsc()
{
	std::vector< OscMsg > ms;
	osc.Poll( ms );
	if( !ms.empty() && ( lastInputT_ <= 0 ) ) lastInputT_ = Now();
	for( auto& m : ms )
	{
		if( IsBridgeMessage( m.address ) ) bridge.OnOsc( m, show );
		else ApplyOsc( show, m );
	}
}

//================================================================ engine thread
void App::StartEngine()
{
	if( engineRun_ ) return;
	engineEvent_ = CreateEventW( nullptr, FALSE, FALSE, nullptr );
	midi.wake    = engineEvent_;
	osc.wake     = engineEvent_;
	engineRun_   = true;
	engine_      = std::thread( [ this ] { EngineLoop(); } );
}
void App::StopEngine()
{
	if( !engineRun_ ) return;
	engineRun_ = false;
	SetEvent( (HANDLE)engineEvent_ );
	if( engine_.joinable() )
	{
		//called from Frame (export start): the UI holds engineLock_ and the engine thread may be waiting for it -
		//release it while joining, otherwise both wait for each other forever
		const bool rel = frameLock_ && frameLock_->owns_lock();
		if( rel ) frameLock_->unlock();
		engine_.join();
		if( rel ) frameLock_->lock();
	}
	midi.wake = nullptr;
	osc.wake  = nullptr;
	CloseHandle( (HANDLE)engineEvent_ );
	engineEvent_ = nullptr;
}
void App::EngineLoop()
{
	timeBeginPeriod( 1 );
	bool boosted  = false;
	HANDLE mmcss  = nullptr;
	double rateT  = Now();
	int ticks     = 0;
	while( engineRun_ )
	{
		WaitForSingleObject( (HANDLE)engineEvent_, 1 );//wakes on MIDI / OSC input, else 1 kHz
		if( engineBoost_ != boosted )
		{
			boosted = engineBoost_;
			if( boosted )
			{
				DWORD idx = 0;
				SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL );
				mmcss = AvSetMmThreadCharacteristicsW( L"Pro Audio", &idx );
			}
			else
			{
				if( mmcss ) AvRevertMmThreadCharacteristics( mmcss );
				mmcss = nullptr;
				SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_NORMAL );
			}
		}
		{
			std::lock_guard< std::recursive_mutex > l( engineLock_ );
			const uint32_t v0 = show.dmxVersion;
			io.PollInput();
			HandleMidi();
			PollOsc();
			bridge.Tick( show, Now() );
			UpdateEffects( Now() );
			if( show.dmxVersion != v0 )
			{
				lastKickVer_ = show.dmxVersion;
				io.Kick( lastInputT_ );
			}
			lastInputT_ = 0;
		}
		++ticks;
		double now = Now();
		if( now - rateT >= 1.0 )
		{
			engineHz = (float)( ticks / ( now - rateT ) );
			ticks    = 0;
			rateT    = now;
		}
	}
	if( mmcss ) AvRevertMmThreadCharacteristics( mmcss );
	timeEndPeriod( 1 );
}

//================================================================ performance mode
void App::SetPerfView( int v )
{
	perfView_ = Clamp( v, 0, 2 );
	SetPerfMode( perfMode_ );//re-apply the limits
}
void App::SetPerfMode( bool on )
{
	const bool changed = on != perfMode_;
	perfMode_          = on;
	ApplyQuality();
	engineBoost_ = on;
	io.boost     = on;
	SetPriorityClass( GetCurrentProcess(), on ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS );
	//no Windows "efficiency mode" throttling of this process while live
	PROCESS_POWER_THROTTLING_STATE pt{};
	pt.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
	pt.ControlMask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
	pt.StateMask   = 0;
	SetProcessInformation( GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof( pt ) );
	lastViewRender_ = 0;
	if( changed )
	{
		SetStatus( on ? L( "Performance-Modus AN (F12 beendet)" ) : L( "Performance-Modus AUS" ) );
		if( io.boost ) io.ClearLatency();
	}
	SaveSettings();
}
//2.1 quality levels: normal = the look of 2.0 (no limits), high = extras (screen-space reflections, ambient
//occlusion, soft 1024 px shadows, GGX highlights, finer haze cones), draft = no shadows / bloom / glow /
//projector haze and few steps (= the live "reduced view"). The performance mode's reduced view limits on top;
//a video export always renders high.
void App::ApplyQuality()
{
	Renderer::Quality q;
	auto limit = [ & ]( float beam, bool fx ) {
		q.beamQuality = q.beamQuality > 0 ? std::min( q.beamQuality, beam ) : beam;
		if( !fx ) q.shadows = q.bloom = q.glow = q.projHaze = false;
	};
	if( exp_.active ) q.high = true;
	else
	{
		if( perfMode_ && perfView_ == 1 ) limit( 0.35f, false );
		const int lv = std::min( quality_, qualityAuto_ ? qualityEff_ : quality_ );
		if( lv == 2 && !( perfMode_ && perfView_ == 1 ) ) q.high = true;
		if( lv == 0 ) limit( 0.35f, false );
	}
	ren.quality = q;
}
void App::QualityTick( float dt )
{
	quality_ = Clamp( quality_, 0, 2 );
	if( !qualityAuto_ || perfMode_ || exp_.active || uiTest_ || noAutosave ) { qualityEff_ = quality_; qLow_ = qHigh_ = 0; }
	else
	{
		qualityEff_ = std::min( qualityEff_, quality_ );
		qLow_       = fps < 28 ? qLow_ + dt : 0;
		qHigh_      = fps > 50 ? qHigh_ + dt : 0;
		if( qLow_ > 2.0f && qualityEff_ > 0 )
		{
			--qualityEff_;
			qLow_ = qHigh_ = 0;
			SetStatus( std::string( L( "Bildrate niedrig - Qualität automatisch: " ) ) + ( qualityEff_ == 1 ? L( "Normal" ) : L( "Entwurf" ) ) );
		}
		if( qHigh_ > 8.0f && qualityEff_ < quality_ ) { ++qualityEff_; qLow_ = qHigh_ = 0; }
	}
	ApplyQuality();
}

//2.1 status bar: DMX output, Time Line sync, engine, inputs, quality, messages, show
void App::StatusBar()
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float h  = ImGui::GetContentRegionAvail().y;
	const float r  = floorf( 3.5f * ui );
	const ImU32 cGreen = IM_COL32( 110, 220, 140, 255 ), cRed = IM_COL32( 255, 95, 85, 255 ), cOrange = IM_COL32( 255, 170, 70, 255 ), cGrey = IM_COL32( 120, 125, 135, 255 ), cBlue = IM_COL32( 90, 165, 255, 255 );
	bool first = true;
	//one field: dot + text, clickable, tooltip; returns clicked
	auto field = [ & ]( const char* id, ImU32 dot, const std::string& text, const char* tip ) {
		if( !first ) { ImGui::SameLine( 0, 0 ); ImGui::Dummy( ImVec2( 14 * ui, h ) ); ImGui::SameLine( 0, 0 ); }
		first = false;
		const ImVec2 ts = ImGui::CalcTextSize( text.c_str() );
		const float w   = ( dot ? 2 * r + 6 * ui : 0 ) + ts.x + 8 * ui;
		const ImVec2 p  = ImGui::GetCursorScreenPos();
		const bool cl   = ImGui::InvisibleButton( id, ImVec2( w, h ) );
		const bool hov  = ImGui::IsItemHovered();
		if( hov ) dl->AddRectFilled( p, ImVec2( p.x + w, p.y + h ), IM_COL32( 255, 255, 255, 16 ), 3 * ui );
		float x = p.x + 4 * ui;
		if( dot )
		{
			dl->AddCircleFilled( ImVec2( x + r, p.y + h * 0.5f ), r, dot );
			x += 2 * r + 6 * ui;
		}
		dl->AddText( ImVec2( x, p.y + ( h - ts.y ) * 0.5f ), hov ? IM_COL32( 230, 235, 245, 255 ) : IM_COL32( 165, 172, 185, 255 ), text.c_str() );
		if( tip && hov ) ImGui::SetTooltip( "%s", tip );
		UiMark( std::string( "status." ) + ( id + 2 ), p, ImVec2( p.x + w, p.y + h ) );
		return cl;
	};
	char b[ 160 ];
	//DMX output
	{
		const OutputCfg& o = show.output;
		std::string t = "DMX";
		if( o.enttec ) t += "  USB";
		if( o.artnet ) t += "  Art-Net";
		if( o.sacn ) t += "  sACN";
		const bool any = o.enttec || o.artnet || o.sacn;
		if( !any ) t += std::string( "  " ) + L( "aus" );
		else { snprintf( b, sizeof( b ), "  %.0f/s", sentRate_ ); t += b; }
		const bool err = o.enttec && !io.enttecOpen;
		const ImU32 dot = !any ? cGrey : err ? cRed : sentRate_ > 0.5f ? cGreen : cOrange;
		const std::string tip = io.Status() + "\n" + L( "Klick: Ausgang / Interface" );
		if( field( "##dmx", dot, t, tip.c_str() ) ) { startTab = "Ausgang"; startTabFrames_ = 0; }
	}
	//Time Line
	{
		const Timeline& tl = show.tl;
		const TlRuntime& rt = tl.rt;
		const int ms = (int)( rt.pos * 1000.0 );
		snprintf( b, sizeof( b ), "%s %d:%02d.%d", rt.playing ? ">" : "||", ms / 60000, ( ms / 1000 ) % 60, ( ms / 100 ) % 10 );
		std::string t = std::string( "Time Line  " ) + b;
		ImU32 dot = rt.playing ? cGreen : cGrey;
		if( tl.sync == TlSync::Mtc || tl.sync == TlSync::Ltc )
		{
			const bool ltc  = tl.sync == TlSync::Ltc;
			const double xs = ltc ? show.ltcSec : show.tcSec;
			const bool xr   = ltc ? show.ltcRun : show.tcRun;
			t += ltc ? "  LTC" : "  MTC";
			if( rt.extRun && !xr ) { dot = cOrange; snprintf( b, sizeof( b ), L( " Freilauf %.1f s" ), std::max( 0.0, tl.freewheel - rt.extIdle ) ); t += b; }
			else if( xs < 0 ) { dot = cRed; t += std::string( " " ) + L( "kein Signal" ); }
			else { dot = xr ? cGreen : cGrey; t += std::string( " " ) + ( xr ? L( "läuft" ) : L( "steht" ) ); }
		}
		else if( tl.sync == TlSync::Beat ) t += "  Beat";
		if( field( "##tl", dot, t, L( "Time Line: Position und Sync (grün läuft, orange Freilauf, rot kein Signal). Klick: Time Line öffnen" ) ) ) tlSelectTab_ = true;
	}
	//engine
	{
		DmxIO::Latency lt = io.GetLatency();
		if( lt.n ) snprintf( b, sizeof( b ), "Engine %.0f Hz  %.1f ms  CPU %.0f %%", engineHz, lt.avgMs, cpu_ );
		else snprintf( b, sizeof( b ), "Engine %.0f Hz  CPU %.0f %%", engineHz, cpu_ );
		const ImU32 dot = engineHz > 600 ? cGreen : engineHz > 200 ? cOrange : cRed;
		if( field( "##engine", dot, b, L( "Engine-Takt, Latenz Eingang -> DMX, CPU. Klick: Performance-Einstellungen" ) ) ) perfWin_ = true;
	}
	//inputs: MIDI / OSC / Art-Net in light up on traffic
	{
		const double now = Now();
		bool mid = false;
		for( auto& a : midi.lastActivity ) mid = mid || now - a.second < 0.15;
		const uint64_t oc = osc.received.load();
		if( oc != oscSeen_ ) { oscSeen_ = oc; oscT_ = now; }
		const bool oac = now - oscT_ < 0.15;
		auto lamp = [ & ]( const char* id, const char* name, bool on, bool enabled, const char* tip ) {
			field( id, on ? cBlue : enabled ? IM_COL32( 70, 80, 95, 255 ) : IM_COL32( 50, 52, 58, 255 ), name, tip );
		};
		lamp( "##midi", "MIDI", mid, !midi.devices.empty(), L( "MIDI-Eingang (leuchtet bei Empfang)" ) );
		lamp( "##osc", "OSC", oac, show.output.oscIn, L( "OSC-Eingang (leuchtet bei Empfang)" ) );
		lamp( "##artin", "Art-Net In", recvRate_ > 0.5f, show.output.artnetInput, L( "Art-Net-Eingang (leuchtet bei Empfang)" ) );
	}
	//quality
	{
		const int lv = qualityAuto_ ? qualityEff_ : quality_;
		std::string t = std::string( L( "Qualität " ) ) + QualityName( lv );
		if( qualityAuto_ && qualityEff_ < quality_ ) t += std::string( " (" ) + L( "gedrosselt" ) + ")";
		else if( qualityAuto_ ) t += " (Auto)";
		const ImU32 dot = qualityAuto_ && qualityEff_ < quality_ ? cOrange : 0;
		if( field( "##quality", dot, t, L( "Darstellungsqualität des Viewports (nur Anzeige, DMX unverändert). Klick: nächste Stufe" ) ) )
		{
			quality_    = ( quality_ + 1 ) % 3;
			qualityEff_ = quality_;
			SaveSettings();
		}
	}
	//right: message + show
	{
		snprintf( b, sizeof( b ), "%s%s   %.0f fps", show.path.empty() ? L( "Unbenannt" ) : FileStem( show.path ).c_str(), show.dirty ? " *" : "", fps );
		const float tw = ImGui::CalcTextSize( b ).x;
		const float x1 = ImGui::GetWindowPos().x + ImGui::GetWindowWidth();
		const ImVec2 p( ImGui::GetWindowPos().x, ImGui::GetWindowPos().y );//the cursor is below the row after the last field
		float x = x1 - tw - 6 * ui;
		dl->AddText( ImVec2( x, p.y + ( h - ImGui::GetTextLineHeight() ) * 0.5f ), IM_COL32( 140, 147, 160, 255 ), b );
		if( !status.empty() && Now() - statusTime < 6.0 )
		{
			const float sw = ImGui::CalcTextSize( status.c_str() ).x;
			x -= sw + 24 * ui;
			if( x > p.x + 20 * ui ) dl->AddText( ImVec2( x, p.y + ( h - ImGui::GetTextLineHeight() ) * 0.5f ), IM_COL32( 255, 204, 90, 255 ), status.c_str() );
		}
	}
}
bool App::ShouldRenderViewport( double now ) const
{
	if( !perfMode_ || perfView_ == 2 ) return true;
	if( perfView_ == 0 ) return false;
	return lastViewRender_ <= 0 || now - lastViewRender_ >= 1.0 / Clamp( perfFps_, 1.0f, 120.0f ) - 0.002;
}
double App::FrameInterval() const
{
	if( !perfMode_ ) return 0;
	return Now() - lastUiInput_ < 2.0 ? 1.0 / 30.0 : 1.0 / 10.0;
}
void App::UpdateCpu()
{
	static ULONGLONG lastK = 0, lastU = 0;
	static double lastT    = 0;
	double now             = Now();
	if( now - lastT < 1.0 ) return;
	FILETIME c, e, k, u;
	if( !GetProcessTimes( GetCurrentProcess(), &c, &e, &k, &u ) ) return;
	ULONGLONG K = ( (ULONGLONG)k.dwHighDateTime << 32 ) | k.dwLowDateTime, U = ( (ULONGLONG)u.dwHighDateTime << 32 ) | u.dwLowDateTime;
	if( lastT > 0 )
	{
		SYSTEM_INFO si;
		GetSystemInfo( &si );
		cpu_ = (float)( ( ( K - lastK ) + ( U - lastU ) ) * 1e-7 / ( ( now - lastT ) * std::max< DWORD >( 1, si.dwNumberOfProcessors ) ) * 100.0 );
	}
	lastK = K;
	lastU = U;
	lastT = now;
}
void App::LoadSettings()
{
	settingsLoaded_ = true;
	std::string txt;
	Json j;
	if( !ReadText( exeDir + "\\settings.json", txt ) || !ParseJson( txt, j, nullptr ) ) return;
	perfView_  = Clamp( j[ "perfView" ].I( 1 ), 0, 2 );
	perfFps_   = Clamp( j[ "perfFps" ].F( 20 ), 2.0f, 120.0f );
	perfScale_ = Clamp( j[ "perfScale" ].F( 0.5f ), 0.2f, 1.0f );
	if( j[ "perfMode" ].B( false ) ) SetPerfMode( true );
	bridgeOn       = j[ "bridgeOn" ].B( false );
	bridgeHttpPort = Clamp( j[ "bridgeHttpPort" ].I( 8001 ), 1024, 65535 );
	ren.projLines  = j[ "projLines" ].B( true );
	vpBarFold_     = j[ "viewportBarFolded" ].B( false );
	quality_       = Clamp( j[ "renderQuality" ].I( 1 ), 0, 2 );//default Normal (= 2.0 look)
	qualityAuto_   = j[ "qualityAuto" ].B( true );
	qualityEff_    = quality_;
	languageSaved_ = j[ "language" ].Str( "" );
	autosaveMin    = Clamp( j[ "autosaveMinutes" ].I( 2 ), 0, 60 );
}
void App::SaveSettings()
{
	if( !settingsLoaded_ ) return;//self test / before start-up: never touch the user's settings
	Json j = Json::Obj();
	j.Set( "perfMode", perfMode_ );
	j.Set( "perfView", (float)perfView_ );
	j.Set( "perfFps", perfFps_ );
	j.Set( "perfScale", perfScale_ );
	j.Set( "bridgeOn", bridgeOn );
	j.Set( "bridgeHttpPort", bridgeHttpPort );
	j.Set( "projLines", ren.projLines );
	j.Set( "viewportBarFolded", vpBarFold_ );
	j.Set( "renderQuality", quality_ );
	j.Set( "qualityAuto", qualityAuto_ );
	if( !languageSaved_.empty() ) j.Set( "language", languageSaved_ );
	j.Set( "autosaveMinutes", autosaveMin );//only what the user chose (View > Language)
	WriteText( exeDir + "\\settings.json", WriteJson( j, true ) );
}

void App::PerfWindow()
{
	if( !perfWin_ ) return;
	ImGui::SetNextWindowSize( ImVec2( 520 * ui, 0 ), ImGuiCond_Appearing );
	if( !ImGui::Begin( L( "Performance-Modus" ), &perfWin_, ImGuiWindowFlags_NoCollapse ) )
	{
		ImGui::End();
		return;
	}
	bool on = perfMode_;
	if( ImGui::Checkbox( L( "Performance-Modus aktiv (F12)" ), &on ) ) SetPerfMode( on );
	ImGui::TextDisabled( L( "MIDI / OSC / Art-Net werden unabhängig von der Anzeige sofort zu DMX." ) );
	ImGui::SeparatorText( L( "Visualisierung im Performance-Modus" ) );
	int v = perfView_;
	if( ImGui::RadioButton( L( "Aus (Live-Panel)" ), v == 0 ) ) SetPerfView( 0 );
	ImGui::SameLine();
	if( ImGui::RadioButton( L( "Vorschau" ), v == 1 ) ) SetPerfView( 1 );
	ImGui::SameLine();
	if( ImGui::RadioButton( L( "Voll" ), v == 2 ) ) SetPerfView( 2 );
	ImGui::BeginDisabled( perfView_ != 1 );
	ImGui::SetNextItemWidth( 200 * ui );
	if( ls::rs::SliderFloat( L( "Bilder pro Sekunde" ), &perfFps_, 2, 60, "%.0f fps" ) ) SaveSettings();
	ImGui::SetNextItemWidth( 200 * ui );
	if( ls::rs::SliderFloat( L( "Auflösung" ), &perfScale_, 0.2f, 1.0f, "%.2f x" ) ) SaveSettings();
	ImGui::TextDisabled( L( "Vorschau: ohne Schatten, Bloom und Linsen-Glow, einfachere Kegel." ) );
	ImGui::EndDisabled();
	ImGui::SeparatorText( "System" );
	if( powerPlan_.empty() )
	{
		GUID* g = nullptr;
		if( PowerGetActiveScheme( nullptr, &g ) == ERROR_SUCCESS )
		{
			wchar_t nm[ 256 ] = {};
			DWORD sz          = sizeof( nm );
			if( PowerReadFriendlyName( nullptr, g, nullptr, nullptr, (UCHAR*)nm, &sz ) == ERROR_SUCCESS ) powerPlan_ = Narrow( nm );
			LocalFree( g );
		}
		if( powerPlan_.empty() ) powerPlan_ = "?";
	}
	ImGui::Text( L( "Energieplan: %s" ), powerPlan_.c_str() );
	ImGui::TextDisabled( L( "Für Live: Energieplan \"Höchstleistung\", USB-Energiesparen für das USB-DMX-Interface aus\n(Geräte-Manager > USB-Root-Hub > Energieverwaltung), Firewall-Freigabe für LightStage." ) );
	ImGui::TextDisabled( L( "Im Performance-Modus: Prozesspriorität \"höher als normal\", Eingabe- und DMX-Thread\nmit Audio-Priorität (MMCSS), kein Windows-Effizienzmodus." ) );
	ImGui::End();
}

void App::PanelLive( float w, float h )
{
	ImVec2 p0      = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled( p0, ImVec2( p0.x + w, p0.y + h ), IM_COL32( 9, 10, 13, 255 ) );
	ImGui::BeginChild( "live", ImVec2( w, h ), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground );
	ImGui::PushFont( fontBig );
	ImGui::TextColored( ImVec4( 1.0f, 0.3f, 0.25f, 1 ), "LIVE" );
	ImGui::PopFont();
	ImGui::SameLine();
	ImGui::TextDisabled( L( "Performance-Modus - Visualisierung aus  (F12 beendet, Ansicht > Performance-Einstellungen)" ) );
	ImGui::Spacing();
	if( ImGui::BeginTable( "live", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV ) )
	{
		ImGui::TableSetupColumn( "a", ImGuiTableColumnFlags_WidthFixed, 200 * ui );
		ImGui::TableSetupColumn( "b", ImGuiTableColumnFlags_WidthStretch, 1.2f );
		ImGui::TableSetupColumn( "c", ImGuiTableColumnFlags_WidthStretch, 1.0f );
		ImGui::TableNextColumn();
		//master + blackout
		ImGui::TextUnformatted( "Master" );
		float pct = show.master * 100.0f;
		ImGui::PushStyleColor( ImGuiCol_SliderGrab, show.master > 1.0001f ? ImVec4( 1.0f, 0.55f, 0.15f, 1 ) : ImVec4( 0.45f, 0.70f, 1.0f, 1 ) );
		if( ImGui::VSliderFloat( "##lm", ImVec2( 80 * ui, std::max( 120.0f, h - 190 * ui ) ), &pct, 0, 1000, "%.0f %%", ImGuiSliderFlags_Logarithmic ) ) show.master = Clamp( pct / 100.0f, 0.0f, 10.0f );
		ImGui::PopStyleColor();
		LearnHook( [ & ] { MidiMap m; m.target = MidiTarget::Master; return m; }() );
		const bool black = show.master <= 0.0001f;
		ImGui::PushStyleColor( ImGuiCol_Button, black ? ImVec4( 0.75f, 0.12f, 0.1f, 1 ) : ImVec4( 0.18f, 0.2f, 0.25f, 1 ) );
		if( ImGui::Button( black ? L( "BLACKOUT\n(aufheben)" ) : "BLACKOUT", ImVec2( 180 * ui, 56 * ui ) ) )
		{
			if( black ) show.master = blackoutPrev_ > 0 ? blackoutPrev_ : 1.0f;
			else
			{
				blackoutPrev_ = show.master;
				show.master   = 0;
			}
		}
		ImGui::PopStyleColor();
		if( ImGui::Button( "100 %", ImVec2( 88 * ui, 0 ) ) ) show.master = 1.0f;
		ImGui::SameLine();
		if( ImGui::Button( "200 %", ImVec2( 88 * ui, 0 ) ) ) show.master = 2.0f;
		ImGui::TableNextColumn();
		//snapshots
		ImGui::TextUnformatted( "Snapshots" );
		if( show.snapshots.empty() ) ImGui::TextDisabled( L( "Keine Snapshots (Tab Snapshots)." ) );
		float x = 0, avail = ImGui::GetContentRegionAvail().x;
		const ImVec2 bs( 150 * ui, 60 * ui );
		for( int i = 0; i < (int)show.snapshots.size(); ++i )
		{
			if( i && x + bs.x <= avail ) ImGui::SameLine();
			else if( i ) x = 0;
			ImGui::PushID( 7000 + i );
			Snapshot& sn = show.snapshots[ i ];
			ImGui::PushStyleColor( ImGuiCol_Button, sn.active ? ImVec4( 0.2f, 0.45f, 0.85f, 1 ) : ImVec4( 0.16f, 0.18f, 0.23f, 1 ) );
			if( ImGui::Button( sn.name.c_str(), bs ) ) show.ToggleSnapshot( i, ImGui::GetIO().KeyShift );
			ImGui::PopStyleColor();
			{
				const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
				if( sn.cur > 0 ) dl->AddRectFilled( ImVec2( a.x + 3, b.y - 7 * ui ), ImVec2( a.x + 3 + ( b.x - a.x - 6 ) * sn.cur * std::max( 0.04f, sn.master ), b.y - 3 ), IM_COL32( 255, 200, 90, 230 ), 2 );
			}
			ImGui::SetItemTooltip( L( "Klick: an/aus  ·  Shift+Klick: dazuschalten (andere bleiben an)  ·  Rechtsklick: Stärke und Fade" ) );
			if( ImGui::IsItemClicked( ImGuiMouseButton_Right ) ) ImGui::OpenPopup( "lsnap" );
			if( ImGui::BeginPopup( "lsnap" ) )
			{
				float pct = sn.master * 100;
				ImGui::SetNextItemWidth( 220 * ui );
				if( ls::rs::SliderFloat( L( "Stärke" ), &pct, 0, 100, "%.0f %%" ) ) sn.master = Saturate( pct / 100 );
				OscContext( "/ls/snapshot/" + std::to_string( i + 1 ) + "/master" );
				ImGui::SetNextItemWidth( 220 * ui );
				ls::rs::SliderFloat( "Fade", &sn.fade, 0.0f, 30.0f, sn.fade < 0.05f ? L( "sofort" ) : "%.1f s", ImGuiSliderFlags_Logarithmic );
				ImGui::EndPopup();
			}
			{
				MidiMap t;
				t.target   = MidiTarget::Snapshot;
				t.snapshot = i;
				t.type     = MidiType::Note;
				t.toggle   = true;
				LearnHook( t );
			}
			ImGui::PopID();
			x += bs.x + ImGui::GetStyle().ItemSpacing.x;
		}
		if( show.ActiveSnapshots() && ImGui::Button( L( "Alle Snapshots aus" ), ImVec2( 180 * ui, 0 ) ) ) show.ReleaseAllSnapshots();
		//group faders
		if( !show.groups.empty() )
		{
			ImGui::SeparatorText( L( "Gruppen" ) );
			for( int i = 0; i < (int)show.groups.size(); ++i )
			{
				GroupFader& g = show.groups[ i ];
				ImGui::PushID( 7500 + i );
				float vp = g.value * 100;
				ImGui::SetNextItemWidth( std::min( 360 * ui, ImGui::GetContentRegionAvail().x * 0.6f ) );
				if( ls::rs::SliderFloat( "##g", &vp, 0, 100, "%.0f %%" ) ) { g.value = Saturate( vp / 100 ); show.ApplyGroup( i ); }
				OscContext( "/ls/group/" + std::to_string( i + 1 ) + "/value" );
				ImGui::SameLine();
				ImGui::TextUnformatted( g.name.c_str() );
				ImGui::PopID();
			}
		}
		ImGui::TableNextColumn();
		//status
		ImGui::TextUnformatted( L( "Ausgang" ) );
		for( size_t u = 0; u < show.universes.size() && u < show.dmx.size(); ++u )
		{
			long sum = 0;
			{
				std::lock_guard< std::mutex > l( show.dmxLock );
				for( int c = 0; c < 512; ++c ) sum += show.dmx[ u ][ c ];
			}
			char b[ 64 ];
			snprintf( b, sizeof( b ), "Universe %d", show.universes[ u ].num );
			ImGui::ProgressBar( sum / ( 512.0f * 255.0f ), ImVec2( -1, 0 ), b );//share of the full universe
		}
		ImGui::Text( L( "%.0f Pakete/s   %s" ), sentRate_, io.Status().c_str() );
		DmxIO::Latency lt = io.GetLatency();
		ImGui::SeparatorText( L( "Latenz Eingang -> DMX" ) );
		if( lt.n ) ImGui::Text( "min %.1f   Ø %.1f   max %.1f ms   (%d)", lt.minMs, lt.avgMs, lt.maxMs, lt.n );
		else ImGui::TextDisabled( L( "noch keine Eingaben" ) );
		ImGui::Text( "Engine %.0f Hz   CPU %.1f %%   UI %.0f fps", engineHz, cpu_, fps );
		ImGui::SeparatorText( "MIDI" );
		for( auto& d : midi.devices )
		{
			auto it   = midi.lastActivity.find( d );
			bool act  = it != midi.lastActivity.end() && Now() - it->second < 0.15;
			ImVec2 cp = ImGui::GetCursorScreenPos();
			float lh  = ImGui::GetTextLineHeight();
			dl->AddCircleFilled( ImVec2( cp.x + 5 * ui, cp.y + lh * 0.5f ), 4 * ui, act ? IM_COL32( 80, 230, 120, 255 ) : IM_COL32( 60, 64, 72, 255 ) );
			ImGui::Dummy( ImVec2( 12 * ui, lh ) );
			ImGui::SameLine();
			ImGui::TextUnformatted( d.c_str() );
		}
		if( midi.devices.empty() ) ImGui::TextDisabled( L( "keine MIDI-Geräte" ) );
		ImGui::SeparatorText( "OSC" );
		if( osc.Running() )
		{
			ImGui::Text( "UDP %d", osc.Port() );
			int n = 0;
			for( auto it = osc.monitor.rbegin(); it != osc.monitor.rend() && n < 6; ++it, ++n ) ImGui::TextDisabled( "%s", it->c_str() );
		}
		else
			ImGui::TextDisabled( L( "aus (Tab Ausgang / Interface)" ) );
		ImGui::EndTable();
	}
	ImGui::EndChild();
}

void App::HandleMidi()
{
	events_.clear();
	midi.Poll( events_ );
	//MIDI clock / transport -> tempo
	for( auto& c : midi.clock )
		if( show.tempo.source == TempoSource::MidiClock ) TempoMidiClock( show.tempo, c.status, c.spp, c.t );
	midi.clock.clear();
	//MIDI timecode -> cue list: running while quarter frames arrive (they come every 1/4 frame)
	if( midi.mtcAt > 0 && Now() - midi.mtcAt < 0.25 )
	{
		show.tcSec = midi.mtcSec + ( Now() - midi.mtcAt );
		show.tcFps = midi.mtcFps;
		show.tcRun = true;
	}
	else if( midi.mtcAt > 0 && Now() - midi.mtcAt < 3.0 )
	{
		show.tcSec = midi.mtcSec;//stopped: stays at the last position (no cue fires)
		show.tcRun = false;
	}
	else
	{
		show.tcSec = -1;
		show.tcRun = false;
	}
	//LTC from the audio input (timeline sync "LTC")
	{
		double sec = -1, at = 0;
		int fps    = 25;
		if( ltcIn_.Running() && ltcIn_.Get( sec, fps, at ) )
		{
			const double age = Now() - at;
			show.ltcFps      = fps;
			if( age < 0.2 ) { show.ltcSec = sec + age; show.ltcRun = true; }
			else if( age < 3.0 ) { show.ltcSec = sec; show.ltcRun = false; }
			else { show.ltcSec = -1; show.ltcRun = false; }
		}
		else
		{
			show.ltcSec = -1;
			show.ltcRun = false;
		}
	}
	for( auto& e : events_ )
		if( e.t > 0 && ( lastInputT_ <= 0 || e.t < lastInputT_ ) ) lastInputT_ = e.t;
	for( auto& e : events_ )
	{
		if( learn_ && learnArmed_ && e.type != MidiType::CC14 && ( e.type != MidiType::Note || e.noteOn ) )
		{
			MidiMap m = learnTmpl_;
			m.device  = e.device;
			m.channel = e.channel;
			m.type    = e.type;
			m.number  = e.number;
			if( m.target == MidiTarget::Snapshot ) m.toggle = true;//pads: press = on, press again = off
			//replace an identical source+target mapping
			show.midi.erase( std::remove_if( show.midi.begin(), show.midi.end(), [ & ]( const MidiMap& o ) { return o.type == m.type && o.number == m.number && o.channel == m.channel && o.target == m.target && o.attr == m.attr && o.fixtures == m.fixtures && o.universe == m.universe && o.channelDmx == m.channelDmx && o.snapshot == m.snapshot; } ), show.midi.end() );
			show.midi.push_back( m );
			show.dirty  = true;
			learnArmed_ = false;
			char b[ 160 ];
			snprintf( b, sizeof( b ), L( "Gelernt: %s Ch%d %s %d -> %s" ), e.device.c_str(), e.channel + 1, MidiTypeName( e.type ), e.number, m.Describe( show ).c_str() );
			SetStatus( b );
			continue;
		}
		ApplyMidi( show, e );
	}
}

//================================================================ inspector
void App::PanelInspector()
{
	uint64_t selHash = 1469598103934665603ull;
	for( int id : sel ) selHash = ( selHash ^ (uint64_t)id ) * 1099511628211ull;
	ls::rs::Salt rsSalt( selHash );
	const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	if( fontBig ) ImGui::PushFont( fontBig );
	ImGui::TextUnformatted( "INSPECTOR" );
	if( fontBig ) ImGui::PopFont();
	{
		const float bw = ImGui::CalcTextSize( "MIDI Learn" ).x + ImGui::GetStyle().FramePadding.x * 2 + 4 * ui;
		ImGui::SameLine( rightEdge - bw );
		ImGui::PushStyleColor( ImGuiCol_Button, learn_ ? ImVec4( 0.85f, 0.6f, 0.1f, 1 ) : ImGui::GetStyle().Colors[ ImGuiCol_Button ] );
	}
	if( ImGui::Button( learn_ ? L( "Learn AN" ) : "MIDI Learn", ImVec2( ImGui::CalcTextSize( "MIDI Learn" ).x + ImGui::GetStyle().FramePadding.x * 2 + 4 * ui, 0 ) ) )
	{
		learn_      = !learn_;
		learnArmed_ = false;
	}
	ImGui::PopStyleColor();
	ImGui::Dummy( ImVec2( 0, 4 ) );
	std::vector< FixtureInst* > fx;
	for( auto& f : show.fixtures )
		if( IsSel( f.id ) ) fx.push_back( &f );
	if( fx.empty() )
	{
		ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6, 6 ) );
		ImGui::TextWrapped( L( "Keine Lampe ausgewählt." ) );
		ImGui::TextDisabled( L( "Im Viewport oder in der Patch-Liste klicken." ) );
		ImGui::TextDisabled( L( "Ctrl+Klick = mehrere, Ctrl+A = alle." ) );
		ImGui::PopStyleVar();
		return;
	}
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6, 5 ) );
	ImGui::BeginChild( "insp", ImVec2( 0, 0 ), ImGuiChildFlags_None );
	FixtureInst& f0 = *fx[ 0 ];
	if( fx.size() == 1 )
	{
		char nm[ 128 ];
		snprintf( nm, sizeof( nm ), "%s", f0.name.c_str() );
		ImGui::SetNextItemWidth( -1 );
		if( ImGui::InputText( "##name", nm, sizeof( nm ) ) ) { f0.name = nm; show.dirty = true; }
		if( f0.def )
		{
			ImGui::TextDisabled( "%s  |  %s  |  %s", f0.def->manufacturer.c_str(), f0.def->name.c_str(), KindName( f0.def->kind ) );
			ImGui::SameLine();
			if( ImGui::SmallButton( L( "Typ bearbeiten" ) ) ) OpenFixtureEditor( f0.def );
			ImGui::SetItemTooltip( "%s", f0.def->id.rfind( "user/", 0 ) == 0 ? L( "Eigene Lampe im Fixture-Editor bearbeiten" ) : L( "Als eigene Lampe (Kopie) im Fixture-Editor bearbeiten" ) );
			ImGui::SameLine();
			if( ImGui::SmallButton( L( "Typ tauschen..." ) ) ) OpenSwapDialog( sel );
			ImGui::SetItemTooltip( L( "Gegen eine andere Lampe austauschen - Position, Befestigung und Adresse bleiben" ) );
		}
		if( f0.def && ImGui::BeginCombo( "Mode", f0.def->modes[ f0.mode ].name.c_str() ) )
		{
			for( int i = 0; i < (int)f0.def->modes.size(); ++i )
			{
				char b[ 160 ];
				snprintf( b, sizeof( b ), L( "%s (%d Kanäle)" ), f0.def->modes[ i ].name.c_str(), f0.def->modes[ i ].Footprint() );
				if( ImGui::Selectable( b, i == f0.mode ) ) { f0.mode = i; show.Home( f0 ); show.dirty = true; }
			}
			ImGui::EndCombo();
		}
		int ua[ 2 ] = { f0.universe, f0.address };
		if( ImGui::InputInt2( L( "Universe / Adresse" ), ua ) )
		{
			f0.universe = std::max( 1, ua[ 0 ] );
			f0.address  = Clamp( ua[ 1 ], 1, 512 );
			while( show.UniIndex( f0.universe ) < 0 ) show.AddUniverse();
			show.dirty = true;
		}
		if( show.Overlaps( f0.universe, f0.address, f0.Footprint(), f0.id ) ) ImGui::TextColored( ImVec4( 1, 0.4f, 0.3f, 1 ), L( "Adresse überschneidet sich mit einer anderen Lampe" ) );
		if( f0.address + f0.Footprint() - 1 > 512 ) ImGui::TextColored( ImVec4( 1, 0.4f, 0.3f, 1 ), L( "Footprint passt nicht in das Universe" ) );
	}
	else
	{
		ImGui::Text( L( "%d Lampen ausgewählt" ), (int)fx.size() );
		ImGui::SameLine();
		if( ImGui::SmallButton( L( "Typ tauschen..." ) ) ) OpenSwapDialog( sel );
		ImGui::SetItemTooltip( L( "Alle ausgewählten Lampen gegen einen anderen Typ austauschen - Positionen bleiben" ) );
	}

	if( ls::SectionHeader( "Position", fx.size() == 1 ? ImGuiTreeNodeFlags_DefaultOpen : 0 ) )
	{
		V3 p = f0.pos, r = f0.rot;
		const bool posCh = ls::rs::DragFloat3( "Position (m)", &p.x, 0.02f );
		ImGui::SetItemTooltip( "X / Y / Z (m)" );
		if( posCh )
		{
			V3 d = p - f0.pos;
			for( auto* f : fx ) f->pos += d;
			show.dirty = true;
		}
		if( ls::rs::DragFloat3( L( "Rotation (Grad)" ), &r.x, 0.5f ) )
		{
			V3 d = r - f0.rot;
			for( auto* f : fx ) f->rot += d;
			show.dirty = true;
		}
		//mounting: free, on a truss, on a stand; the lamp follows when that moves
		{
			std::vector< std::string > items;
			items.push_back( L( "Frei (fest im Raum)" ) );
			for( int i = 0; i < (int)show.trusses.size(); ++i ) items.push_back( L( "Traverse " ) + std::to_string( i + 1 ) );
			for( int i = 0; i < (int)show.stands.size(); ++i ) items.push_back( L( "Stativ " ) + std::to_string( i + 1 ) );
			const int T = (int)show.trusses.size();
			auto itemOf = [ & ]( const FixtureInst& f ) { return f.mount == MountType::Truss ? 1 + f.mountIdx : f.mount == MountType::Stand ? 1 + T + f.mountIdx : 0; };
			int cur = itemOf( f0 );
			bool mixed = false;
			for( auto* f : fx ) mixed |= itemOf( *f ) != cur;
			ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x - 110 * ui );
			if( ImGui::BeginCombo( L( "Befestigung" ), mixed ? L( "(gemischt)" ) : items[ Clamp( cur, 0, (int)items.size() - 1 ) ].c_str() ) )
			{
				for( int k = 0; k < (int)items.size(); ++k )
					if( ImGui::Selectable( items[ k ].c_str(), !mixed && k == cur ) )
					{
						if( k == 0 )
							for( auto* f : fx ) show.Attach( *f, MountType::Free, -1 );
						else if( k <= T )
							for( auto* f : fx ) show.HangOnTruss( *f, k - 1 );
						else
							show.PlaceOnStand( fx, k - 1 - T );
						show.dirty = true;
					}
				ImGui::EndCombo();
			}
			if( !mixed && f0.mount != MountType::Free )
				ImGui::TextDisabled( f0.mount == MountType::Truss ? L( "Bewegt sich mit Traverse %d mit" ) : L( "Steht auf Stativ %d (Höhe im Tab Bühne)" ), f0.mountIdx + 1 );
		}
		if( ImGui::Button( L( "Hängend" ) ) ) { for( auto* f : fx ) f->rot = V3( 180, 0, 0 ); show.dirty = true; }
		ImGui::SameLine();
		if( ImGui::Button( L( "Stehend" ) ) ) { for( auto* f : fx ) f->rot = V3( 0, 0, 0 ); show.dirty = true; }
		ImGui::SameLine();
		if( ImGui::Button( L( "Zum Publikum" ) ) ) { for( auto* f : fx ) f->rot = V3( 90, 0, 0 ); show.dirty = true; }
		if( fx.size() > 2 )
		{
			if( ImGui::Button( L( "Gleichmäßig verteilen (X)" ) ) )
			{
				std::vector< FixtureInst* > s = fx;
				std::sort( s.begin(), s.end(), []( FixtureInst* a, FixtureInst* b ) { return a->pos.x < b->pos.x; } );
				float x0 = s.front()->pos.x, x1 = s.back()->pos.x;
				for( size_t i = 0; i < s.size(); ++i ) s[ i ]->pos.x = Lerp( x0, x1, (float)i / ( s.size() - 1 ) );
				show.dirty = true;
			}
		}
		bool ip = f0.invertPan, it = f0.invertTilt;
		if( ImGui::Checkbox( L( "Pan invertieren" ), &ip ) ) { for( auto* f : fx ) f->invertPan = ip; show.dirty = true; }
		ImGui::SameLine();
		if( ImGui::Checkbox( L( "Tilt invertieren" ), &it ) ) { for( auto* f : fx ) f->invertTilt = it; show.dirty = true; }
	}
	{
		//beam angle: lamps without a zoom channel get a fixed lens angle (narrow spot ... wide flood)
		std::vector< FixtureInst* > fixed;
		for( auto* f : fx )
			if( f->def && f->def->kind != Kind::Hazer && !show.HasAttr( *f, Attr::Zoom ) ) fixed.push_back( f );
		if( !fixed.empty() && ls::SectionHeader( L( "Abstrahlwinkel" ), ImGuiTreeNodeFlags_DefaultOpen ) )
		{
			FixtureInst& g = *fixed[ 0 ];
			float prof     = g.def->beamMin;
			float b        = g.beam > 0.5f ? g.beam : prof;
			ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x - 110 * ui );
			if( ls::rs::SliderFloat( L( "Winkel" ), &b, 3, 100, "%.0f°", ImGuiSliderFlags_Logarithmic ) )
			{
				for( auto* f : fixed ) f->beam = b;
				show.dirty = true;
			}
			struct P { const char* n; float a; } ps[] = { { "Spot 10°", 10 }, { "Medium 25°", 25 }, { "Wide 40°", 40 }, { "Flood 60°", 60 } };
			for( auto& p : ps )
			{
				if( ImGui::SmallButton( p.n ) ) { for( auto* f : fixed ) f->beam = p.a; show.dirty = true; }
				ImGui::SameLine();
			}
			if( ImGui::SmallButton( L( "Profil" ) ) ) { for( auto* f : fixed ) f->beam = 0; show.dirty = true; }
			ImGui::TextDisabled( g.beam > 0.5f ? L( "Eigener Winkel (Profil: %.0f°)" ) : L( "Winkel aus dem Lampenprofil (%.0f°)" ), prof );
			if( fixed.size() < fx.size() ) ImGui::TextDisabled( L( "Lampen mit Zoom-Kanal steuern den Winkel per Zoom." ) );
		}
	}
	if( ls::SectionHeader( L( "Steuerung" ), ImGuiTreeNodeFlags_DefaultOpen ) ) Programmer( fx );
	if( fx.size() == 1 && ls::SectionHeader( L( "DMX-Kanäle" ), ImGuiTreeNodeFlags_DefaultOpen ) ) ChannelList( f0 );
	ImGui::EndChild();
	ImGui::PopStyleVar();
}

static bool XYPad( const char* id, float* x, float* y, float size )
{
	ImGui::PushID( id );
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton( "##pad", ImVec2( size, size ) );
	bool changed = false;
	if( ImGui::IsItemActive() )
	{
		ImVec2 m = ImGui::GetIO().MousePos;
		*x       = Saturate( ( m.x - p.x ) / size );
		*y       = Saturate( 1.0f - ( m.y - p.y ) / size );
		changed  = true;
	}
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled( p, ImVec2( p.x + size, p.y + size ), IM_COL32( 14, 16, 21, 255 ), 4 );
	dl->AddRect( p, ImVec2( p.x + size, p.y + size ), IM_COL32( 60, 66, 80, 255 ), 4 );
	dl->AddLine( ImVec2( p.x + size * 0.5f, p.y ), ImVec2( p.x + size * 0.5f, p.y + size ), IM_COL32( 40, 44, 54, 255 ) );
	dl->AddLine( ImVec2( p.x, p.y + size * 0.5f ), ImVec2( p.x + size, p.y + size * 0.5f ), IM_COL32( 40, 44, 54, 255 ) );
	ImVec2 c( p.x + *x * size, p.y + ( 1 - *y ) * size );
	dl->AddCircleFilled( c, 6, IM_COL32( 90, 160, 255, 255 ) );
	dl->AddCircle( c, 9, IM_COL32( 90, 160, 255, 120 ) );
	ImGui::PopID();
	return changed;
}

void App::Programmer( std::vector< FixtureInst* >& fx )
{
	FixtureInst& f0 = *fx[ 0 ];
	//pixel selection
	if( fx.size() == 1 && f0.def && f0.def->Heads() > 1 )
	{
		ImGui::TextDisabled( L( "Pixel (Klick = einzeln steuern)" ) );
		int nh   = f0.def->Heads();
		float cs = std::min( 26.0f * ui, ( ImGui::GetContentRegionAvail().x - 8 ) / std::min( nh, 12 ) - 4 );
		for( int h = 0; h < nh; ++h )
		{
			V3 c = h < (int)f0.st.heads.size() ? f0.st.heads[ h ] : V3( 0, 0, 0 );
			c    = V3( powf( Saturate( c.x ), 0.45f ), powf( Saturate( c.y ), 0.45f ), powf( Saturate( c.z ), 0.45f ) );
			ImGui::PushID( h );
			if( h % 12 ) ImGui::SameLine();
			if( ImGui::ColorButton( "##px", ImVec4( c.x, c.y, c.z, 1 ), ImGuiColorEditFlags_NoTooltip, ImVec2( cs, cs ) ) ) pixelSel_ = pixelSel_ == h ? -1 : h;
			if( pixelSel_ == h ) ImGui::GetWindowDrawList()->AddRect( ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32( 255, 255, 255, 255 ), 0, 0, 2 );
			ImGui::PopID();
		}
		if( pixelSel_ >= 0 ) { ImGui::Text( "Pixel %d", pixelSel_ + 1 ); ImGui::SameLine(); if( ImGui::SmallButton( L( "alle" ) ) ) pixelSel_ = -1; }
	}
	int head = fx.size() == 1 ? pixelSel_ : -1;
	auto has = [ & ]( Attr a ) {
		for( auto* f : fx )
			if( show.HasAttr( *f, a ) ) return true;
		return false;
	};
	auto first = [ & ]( Attr a ) -> FixtureInst* {
		for( auto* f : fx )
			if( show.HasAttr( *f, a ) ) return f;
		return nullptr;
	};
	auto setAll = [ & ]( Attr a, float v ) {
		for( auto* f : fx ) show.SetAttr( *f, a, v, head );
	};
	auto ids = [ & ]() {
		std::vector< int > r;
		for( auto* f : fx ) r.push_back( f->id );
		return r;
	};
	//quick buttons
	if( ImGui::Button( L( "Voll" ) ) ) { setAll( Attr::Dimmer, 1 ); for( auto* f : fx ) if( !show.HasAttr( *f, Attr::Dimmer ) ) for( Attr a : { Attr::Red, Attr::Green, Attr::Blue, Attr::White } ) show.SetAttr( *f, a, 1, head ); }
	ImGui::SameLine();
	if( ImGui::Button( L( "Aus" ) ) ) { setAll( Attr::Dimmer, 0 ); for( auto* f : fx ) if( !show.HasAttr( *f, Attr::Dimmer ) ) for( Attr a : { Attr::Red, Attr::Green, Attr::Blue, Attr::White, Attr::Amber, Attr::UV } ) show.SetAttr( *f, a, 0, head ); }
	ImGui::SameLine();
	if( ImGui::Button( L( "Weiß" ) ) )
		for( auto* f : fx )
		{
			for( Attr a : { Attr::Red, Attr::Green, Attr::Blue, Attr::White } ) show.SetAttr( *f, a, 1, head );
			for( Attr a : { Attr::Amber, Attr::UV, Attr::Cyan, Attr::Magenta, Attr::Yellow, Attr::CTO, Attr::ColorWheel, Attr::ColorMacro } ) show.SetAttr( *f, a, 0, head );
		}
	ImGui::SameLine();
	if( ImGui::Button( "Home" ) ) for( auto* f : fx ) show.Home( *f );
	ImGui::SameLine();
	if( ImGui::Button( "Locate" ) )
		for( auto* f : fx )
		{
			show.Home( *f );
			show.SetAttr( *f, Attr::Dimmer, 1 );
			for( Attr a : { Attr::Red, Attr::Green, Attr::Blue, Attr::White } ) show.SetAttr( *f, a, 1 );
			show.SetAttr( *f, Attr::Zoom, 0.5f );
		}
	if( has( Attr::Pan ) )
	{
		ImGui::SameLine();
		if( ImGui::Button( L( "Ziel" ) ) )
			for( auto* f : fx ) AimAt( show, *f, cam.target );
		if( ImGui::IsItemHovered() ) ImGui::SetTooltip( L( "Moving Heads auf den Kamera-Drehpunkt richten" ) );
	}
	//colour (2.1: every colour lamp - RGB/W/A/L, CMY, colour wheel - plus colour temperature)
	FixtureInst* cf = nullptr;
	bool anyExtra = false, anyKelvin = false;
	for( auto* f : fx )
	{
		if( !cf && show.HasColor( *f ) ) cf = f;
		anyExtra  = anyExtra || show.HasExtraEmitters( *f );
		anyKelvin = anyKelvin || show.HasKelvin( *f ) || show.HasColor( *f );
	}
	if( cf )
	{
		const V3 cur = show.GetColor( *cf, head );
		float col[ 3 ] = { cur.x, cur.y, cur.z };
		if( ImGui::ColorEdit3( L( "Farbe" ), col, ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_DisplayHSV ) )
			for( auto* f : fx )
			{
				if( !show.HasColor( *f ) ) continue;
				if( show.colorMix == 0 && !( f->def && f->def->subtractive ) && show.HasAttr( *f, Attr::Red ) )
				{
					//classic: the picker sets R/G/B only (as before 2.1)
					show.SetAttr( *f, Attr::Red, col[ 0 ], head );
					show.SetAttr( *f, Attr::Green, col[ 1 ], head );
					show.SetAttr( *f, Attr::Blue, col[ 2 ], head );
				}
				else
					show.SetColor( *f, V3( col[ 0 ], col[ 1 ], col[ 2 ] ), head );
			}
		std::string slot;
		if( !show.HasAttr( *cf, Attr::Red ) && !show.HasAttr( *cf, Attr::Cyan ) && show.WheelValueFor( *cf, cur, &slot ) >= 0 )
		{
			ImGui::SameLine();
			ImGui::TextDisabled( "-> %s", slot.c_str() );
			if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Farbrad: der Slot, der der gewählten Farbe am nächsten kommt" ) );
		}
		if( anyExtra )
		{
			bool am = show.colorMix == 1;
			if( ImGui::Checkbox( L( "Weiß/Amber/Lime mitmischen" ), &am ) )
			{
				show.colorMix = am ? 1 : 0;
				show.dirty    = true;
				for( auto* f : fx )
					if( show.HasExtraEmitters( *f ) ) show.SetColor( *f, show.GetColor( *f, head ), head );
			}
			if( ImGui::IsItemHovered() )
				ImGui::SetTooltip( "%s", L( "An: Weiß, Amber, Lime und Indigo mischen jede Farbe mit (richtiger Farbton, mehr Licht).\nAus: klassisch nur R/G/B, Weiß = gemeinsamer Anteil (Shows vor 2.1).\nGilt für die ganze Show: Paletten, Gruppenfarben, Effekte, Time Line." ) );
		}
	}
	if( anyKelvin )
	{
		static float kel = 5600;
		if( ImGui::SliderFloat( L( "Farbtemperatur" ), &kel, 2000, 10000, "%.0f K" ) )
			for( auto* f : fx )
				if( show.HasKelvin( *f ) || show.HasColor( *f ) ) show.SetKelvin( *f, kel, head );
		if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Weißes Licht in Kelvin: CTO-Kanal, sonst Warm-/Kaltweiß, sonst als Farbe gemischt" ) );
	}
	//pan/tilt pad
	if( has( Attr::Pan ) && has( Attr::Tilt ) )
	{
		FixtureInst* m = first( Attr::Pan );
		float x = show.GetAttr( *m, Attr::Pan ), y = show.GetAttr( *m, Attr::Tilt );
		if( XYPad( "pt", &x, &y, std::min( 170.0f, ImGui::GetContentRegionAvail().x * 0.5f ) ) )
		{
			setAll( Attr::Pan, x );
			setAll( Attr::Tilt, y );
		}
		ImGui::SameLine();
		ImGui::BeginGroup();
		//2.1: pan / tilt in degrees from the centre, editable (all selected lamps)
		float pd = show.GetAttrDeg( *m, Attr::Pan ), td = show.GetAttrDeg( *m, Attr::Tilt );
		const float pr = show.AttrRangeDeg( *m, Attr::Pan ), tr = show.AttrRangeDeg( *m, Attr::Tilt );
		ImGui::SetNextItemWidth( ImGui::GetFontSize() * 7.3f );
		if( ls::rs::DragFloat( "##pdeg", &pd, 0.5f, -pr * 0.5f, pr * 0.5f, "Pan %.1f°" ) )
			for( auto* f : fx ) show.SetAttrDeg( *f, Attr::Pan, pd );
		ImGui::SetItemTooltip( "%s", L( "Pan in Grad ab Mitte (Doppelklick = eintippen)" ) );
		ImGui::SetNextItemWidth( ImGui::GetFontSize() * 7.3f );
		if( ls::rs::DragFloat( "##tdeg", &td, 0.5f, -tr * 0.5f, tr * 0.5f, "Tilt %.1f°" ) )
			for( auto* f : fx ) show.SetAttrDeg( *f, Attr::Tilt, td );
		ImGui::SetItemTooltip( "%s", L( "Tilt in Grad ab Mitte (Doppelklick = eintippen)" ) );
		ImGui::TextDisabled( L( "ist: %5.1f / %5.1f" ), m->st.pan, m->st.tilt );
		ImGui::EndGroup();
	}
	//fan + aim (1.2)
	if( has( Attr::Pan ) || has( Attr::Tilt ) )
	{
		const float w3 = ( ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x ) / 3;
		if( fx.size() >= 2 )
		{
			ImGui::SetNextItemWidth( w3 );
			if( ls::rs::DragFloat( "##fanp", &fanPan_, 0.5f, -360, 360, L( "Fächer Pan %.0f°" ) ) ) FanApply( fanPan_, fanTilt_ );
			if( ImGui::IsItemActivated() ) { FanBegin( ids() ); fanPan_ = fanTilt_ = 0; }
			ImGui::SetItemTooltip( L( "Ziehen: Pan der Auswahl von links nach rechts auffächern (negativ = gespiegelt).\nStartpunkt = aktuelle Werte beim Anklicken." ) );
			ImGui::SameLine();
			ImGui::SetNextItemWidth( w3 );
			if( ls::rs::DragFloat( "##fant", &fanTilt_, 0.5f, -270, 270, L( "Fächer Tilt %.0f°" ) ) ) FanApply( fanPan_, fanTilt_ );
			if( ImGui::IsItemActivated() ) { FanBegin( ids() ); fanPan_ = fanTilt_ = 0; }
			ImGui::SetItemTooltip( L( "Ziehen: Tilt der Auswahl von links nach rechts auffächern" ) );
			ImGui::SameLine();
		}
		if( aimPick_ ) ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.8f, 0.5f, 0.1f, 1 ) );
		if( ImGui::Button( aimPick_ ? L( "Ziel klicken..." ) : L( "Zielen..." ), ImVec2( fx.size() >= 2 ? w3 : -1, 0 ) ) ) ImGui::OpenPopup( "aim" );
		if( aimPick_ ) ImGui::PopStyleColor();
		ImGui::SetItemTooltip( L( "Moving Heads / Scanner der Auswahl auf einen Punkt richten" ) );
		if( ImGui::BeginPopup( "aim" ) )
		{
			if( ImGui::MenuItem( L( "Punkt im 3D-Fenster anklicken" ) ) ) aimPick_ = true;
			ImGui::Separator();
			ImGui::SetNextItemWidth( 220 * ui );
			ls::rs::DragFloat3( "X / Y / Z (m)", &aimPt_.x, 0.05f, -100, 100, "%.2f" );
			if( ImGui::Button( L( "Auf diesen Punkt zielen" ) ) ) { AimSelected( aimPt_ ); ImGui::CloseCurrentPopup(); }
			ImGui::SameLine();
			if( ImGui::Button( L( "Bühnenmitte" ) ) )
			{
				aimPt_ = V3( 0, show.stage.deck ? show.stage.deckHeight : 0.0f, 0 );
				AimSelected( aimPt_ );
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}
	//attribute faders
	static const Attr order[] = { Attr::Dimmer, Attr::Shutter, Attr::Red, Attr::Green, Attr::Blue, Attr::White, Attr::WarmWhite, Attr::ColdWhite, Attr::Amber, Attr::UV, Attr::Lime, Attr::Indigo, Attr::Cyan, Attr::Magenta, Attr::Yellow, Attr::CTO, Attr::ColorWheel, Attr::ColorMacro, Attr::Pan, Attr::Tilt, Attr::PanTiltSpeed, Attr::Zoom, Attr::Focus, Attr::Iris, Attr::Frost, Attr::Gobo, Attr::GoboRot, Attr::Prism, Attr::PrismRot, Attr::Fog, Attr::Effect };
	float labelW = 96 * ui;
	for( Attr a : order )
	{
		FixtureInst* f = first( a );
		if( !f ) continue;
		ImGui::PushID( (int)a );
		float v = show.GetAttr( *f, a, head );
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted( AttrName( a ) );
		ImGui::SameLine( labelW );
		//capability combo for stepped channels
		const Mode* m = f->M();
		const Channel* ch = nullptr;
		for( auto& c : m->ch )
			if( !c.isFine && c.attr == a ) { ch = &c; break; }
		bool stepped = ch && ch->caps.size() > 1 && ( a == Attr::Shutter || a == Attr::ColorWheel || a == Attr::ColorMacro || a == Attr::Gobo || a == Attr::Prism || a == Attr::GoboRot || a == Attr::PrismRot || a == Attr::Effect || a == Attr::CTO || a == Attr::Iris || a == Attr::Frost );
		float fw = ImGui::GetContentRegionAvail().x - ( stepped ? 106 * ui + ImGui::GetStyle().ItemSpacing.x : 0 );
		ImGui::SetNextItemWidth( fw );
		int dmx = (int)( v * 255 + 0.5f );
		char fmt[ 32 ];
		snprintf( fmt, sizeof( fmt ), "%d%%%%  (%d)", (int)( v * 100 + 0.5f ), dmx );
		if( ls::rs::SliderFloat( "##v", &v, 0, 1, fmt ) ) setAll( a, v );
		{
			MidiMap t;
			t.target   = MidiTarget::FixtureAttr;
			t.fixtures = ids();
			t.attr     = a;
			t.head     = head;
			LearnHook( t );
			if( !learn_ )
			{
				std::string k;
				for( char c : ToLower( AttrName( a ) ) )
					if( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) ) k += c;
				OscContext( "/ls/fixture/" + std::to_string( f->id ) + "/" + k + ( head >= 0 ? "/" + std::to_string( head + 1 ) : std::string() ) );
			}
		}
		if( stepped )
		{
			ImGui::SameLine();
			ImGui::SetNextItemWidth( 106 * ui );
			uint16_t v16  = (uint16_t)( v * 65535 );
			const Cap* cc = ch->Find( v16 );
			if( ImGui::BeginCombo( "##cap", cc ? cc->label.c_str() : "", ImGuiComboFlags_HeightLarge ) )
			{
				for( size_t i = 0; i < ch->caps.size(); ++i )
				{
					const Cap& cp = ch->caps[ i ];
					char lb[ 200 ];
					snprintf( lb, sizeof( lb ), "%3d-%3d  %s##%d", cp.lo >> 8, cp.hi >> 8, cp.label.c_str(), (int)i );
					if( ImGui::Selectable( lb, cc == &cp ) ) setAll( a, ( ( cp.lo + cp.hi ) * 0.5f ) / 65535.0f );
				}
				ImGui::EndCombo();
			}
		}
		ImGui::PopID();
	}
}

void App::ChannelList( FixtureInst& f )
{
	const Mode* m = f.M();
	if( !m ) return;
	if( ImGui::BeginTable( "chans", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV ) )
	{
		ImGui::TableSetupColumn( L( "Adr" ), ImGuiTableColumnFlags_WidthFixed, 34 * ui );
		ImGui::TableSetupColumn( L( "Kanal" ), ImGuiTableColumnFlags_WidthStretch, 1.2f );
		ImGui::TableSetupColumn( L( "Wert" ), ImGuiTableColumnFlags_WidthStretch, 1.4f );
		ImGui::TableSetupColumn( L( "Funktion" ), ImGuiTableColumnFlags_WidthStretch, 1.2f );
		ImGui::TableHeadersRow();
		for( int i = 0; i < (int)m->ch.size(); ++i )
		{
			const Channel& c = m->ch[ i ];
			ImGui::PushID( i );
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled( "%d", f.address + i );
			ImGui::TableNextColumn();
			ImGui::Selectable( c.name.c_str(), false, ImGuiSelectableFlags_None );
			if( ImGui::IsItemHovered() ) ImGui::SetTooltip( L( "%s\nAttribut: %s%s\nRechtsklick = zur Time Line" ), c.name.c_str(), AttrName( c.attr ), c.isFine ? L( " (fein)" ) : "" );
			if( ImGui::BeginPopupContextItem( "##chtl" ) )
			{
				//group track for the selection when this lamp is part of it, else for this lamp
				std::vector< int > ids = IsSel( f.id ) ? sel : std::vector< int >{ f.id };
				ImGui::TextDisabled( "%s: %s", L( "Time Line" ), c.name.c_str() );
				TimelineAddMenu( ids, &c, f.universe, f.address + i );
				ImGui::EndPopup();
			}
			ImGui::TableNextColumn();
			int v = show.Get( f, i );
			ImGui::SetNextItemWidth( -1 );
			if( ls::rs::SliderInt( "##v", &v, 0, 255 ) ) show.Set( f, i, (uint8_t)v );
			{
				MidiMap t;
				t.target     = MidiTarget::DmxChannel;
				t.universe   = f.universe;
				t.channelDmx = f.address + i;
				t.sixteen    = c.fine == i + 1;
				LearnHook( t );
			}
			ImGui::TableNextColumn();
			if( !c.isFine )
			{
				const Cap* cp = c.Find( show.Get16( f, i ) );
				ImGui::TextDisabled( "%s", cp ? cp->label.c_str() : "" );
				if( cp && ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", cp->label.c_str() );
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}
}//namespace ls
