// Grosserd LightStage - scripted UI test: mouse and key events are fed into the real user interface
// (LightStage.exe --demo --tldemo --tab Timeline --bottom 520 --size 1920 1080 --uitest <log>).
// The window ignores the real mouse / keyboard meanwhile (main.cpp), so the test does not depend on what the user does.
// Places on screen come from App::UiMark (rectangles the UI reports while the test runs).
#include "app.h"
#include <cstdarg>
#include <cstdio>
#include <functional>

namespace ls
{
struct UiTestState
{
	std::string logPath;
	FILE* out = nullptr;
	std::vector< std::function< bool() > > ops;//each returns true when it is finished
	size_t op    = 0;
	int opFrame  = 0;   //frames spent in the current op
	int frame    = 0;
	ImVec2 mouse{ 40, 40 };
	ImVec2 from, to;
	bool done = false, aborted = false;
	int pass = 0, fail = 0;
	//values kept between steps
	double a = 0, b = 0, c = 0, expect = 0;
	int n = 0, m = 0, wi = -1;
};

namespace
{
UiTestState* S = nullptr;
App* A         = nullptr;

void Check( bool ok, const char* fmt, ... )
{
	char b[ 600 ];
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( b, sizeof( b ), fmt, ap );
	va_end( ap );
	fprintf( S->out, "%s %s\n", ok ? "[ OK ]" : "[FAIL]", b );
	fflush( S->out );
	( ok ? S->pass : S->fail )++;
}
bool Mark( const std::string& name, ImVec4& r )
{
	auto it = A->uiMarks_.find( name );
	if( it == A->uiMarks_.end() )
	{
		Check( false, "Stelle '%s' nicht auf dem Bildschirm gefunden - Ablauf abgebrochen", name.c_str() );
		S->aborted = true;
		return false;
	}
	r = it->second;
	return true;
}
ImVec2 Centre( const ImVec4& r ) { return ImVec2( ( r.x + r.z ) * 0.5f, ( r.y + r.w ) * 0.5f ); }
//---- script building blocks
void Wait( int frames )
{
	S->ops.push_back( [ frames ] { return S->opFrame >= frames; } );
}
void Do( std::function< void() > f )
{
	S->ops.push_back( [ f ] { f(); return true; } );
}
//moves the mouse to a place worked out when the step starts
void MoveTo( std::function< bool( ImVec2& ) > target, int frames = 10 )
{
	S->ops.push_back( [ target, frames ] {
		if( S->opFrame == 0 )
		{
			S->from = S->mouse;
			if( !target( S->to ) ) return true;
		}
		const float u = std::min( 1.0f, ( S->opFrame + 1 ) / (float)frames );
		S->mouse      = ImVec2( S->from.x + ( S->to.x - S->from.x ) * u, S->from.y + ( S->to.y - S->from.y ) * u );
		ImGui::GetIO().AddMousePosEvent( S->mouse.x, S->mouse.y );
		return u >= 1.0f;
	} );
}
void MoveToMark( const std::string& name, int frames = 10 )
{
	MoveTo( [ name ]( ImVec2& p ) {
		ImVec4 r;
		if( !Mark( name, r ) ) return false;
		p = Centre( r );
		return true;
	},
	        frames );
}
void Button( bool down )
{
	S->ops.push_back( [ down ] {
		if( S->opFrame == 0 ) ImGui::GetIO().AddMouseButtonEvent( 0, down );
		return S->opFrame >= 1;//one frame for the button alone
	} );
}
void Ctrl( bool down )
{
	S->ops.push_back( [ down ] {
		if( S->opFrame == 0 ) ImGui::GetIO().AddKeyEvent( ImGuiMod_Ctrl, down );
		return S->opFrame >= 1;
	} );
}
void Key( ImGuiKey k )
{
	S->ops.push_back( [ k ] {
		if( S->opFrame == 0 ) ImGui::GetIO().AddKeyEvent( k, true );
		if( S->opFrame == 2 ) ImGui::GetIO().AddKeyEvent( k, false );
		return S->opFrame >= 3;
	} );
}
void Click( const std::string& mark )
{
	MoveToMark( mark, 6 );
	Wait( 2 );
	Button( true );
	Button( false );
	Wait( 3 );
}
int KeysOf( const TlTrack& t )
{
	int n = 0;
	for( auto& l : t.lanes ) n += (int)l.keys.size();
	return n;
}
}//namespace

void App::UiTestStart( const std::string& logPath )
{
	static UiTestState st;
	S          = &st;
	A          = this;
	ut_        = &st;
	uiTest_    = true;
	noAutosave = true;
	st.logPath = logPath;
	st.out     = fopen( logPath.c_str(), "w" );
	if( !st.out ) { st.done = true; st.fail = 1; return; }
	ImGui::GetIO().AddFocusEvent( true );
	Timeline& tl = show.tl;
	auto Tx      = [ this ]( double t ) { return tlGeo_.x0 + (float)( ( t - tlGeo_.view0 ) * tlGeo_.pps ); };
	auto blockMark = [ & ]( int bi ) { return "tl.block." + std::to_string( S->wi ) + "." + std::to_string( bi ); };

	Wait( 40 );//layout, fonts, the start tab
	//================= sections of a collapsed track
	Do( [ this, &tl ] {
		for( int i = 0; i < (int)tl.tracks.size(); ++i )
			if( !tl.tracks[ i ].open && !tl.tracks[ i ].lanes.empty() && S->wi < 0 ) S->wi = i;
		std::vector< TlBlock > b;
		if( S->wi >= 0 ) TlBlocks( tl, tl.tracks[ S->wi ], b );
		Check( S->wi >= 0 && b.size() >= 2 && uiMarks_.count( "tl.block." + std::to_string( S->wi ) + ".0" ), "Demo: eingeklappte Spur mit %d Abschnitten ist auf dem Bildschirm", (int)b.size() );
		if( S->wi < 0 || b.size() < 2 ) { S->aborted = true; return; }
		S->n = (int)b.size();
		S->a = b.back().t0;
		S->m = KeysOf( tl.tracks[ S->wi ] );
	} );
	//1) drag the last block 5 s to the right
	MoveTo( [ = ]( ImVec2& p ) { ImVec4 r; if( !Mark( blockMark( S->n - 1 ), r ) ) return false; p = Centre( r ); return true; } );
	Button( true );
	MoveTo( [ this, &tl ]( ImVec2& p ) {
		const float dx = (float)( 5.0 * tlGeo_.pps );
		p              = ImVec2( S->mouse.x + dx, S->mouse.y );
		S->expect      = tl.Snap( S->a + dx / tlGeo_.pps );
		return true;
	},
	        14 );
	Wait( 2 );
	Button( false );
	Wait( 3 );
	Do( [ &tl ] {
		std::vector< TlBlock > b;
		TlBlocks( tl, tl.tracks[ S->wi ], b );
		Check( !b.empty() && fabs( b.back().t0 - S->expect ) < 1e-6 && fabs( S->expect - S->a ) > 1 && KeysOf( tl.tracks[ S->wi ] ) == S->m, "Abschnitt mit der Maus verschieben: %.2f s -> %.2f s (erwartet %.2f s), %d Keys wie vorher", S->a, b.empty() ? -1.0 : b.back().t0, S->expect, KeysOf( tl.tracks[ S->wi ] ) );
		S->a = b.size() > 1 ? b[ 1 ].t0 : 0;
		S->b = b.size() > 1 ? b[ 1 ].t1 : 0;
		S->n = b.size() > 1 ? b[ 1 ].keys : 0;
	} );
	//2) Ctrl + drag: copy block 1 to 40 s
	MoveTo( [ = ]( ImVec2& p ) { ImVec4 r; if( !Mark( blockMark( 1 ), r ) ) return false; p = Centre( r ); return true; } );
	Button( true );
	MoveTo( [ this, &tl ]( ImVec2& p ) {
		const double dt = 60.0 - S->a;
		p               = ImVec2( S->mouse.x + (float)( dt * tlGeo_.pps ), S->mouse.y );
		S->expect       = tl.Snap( S->a + ( p.x - S->mouse.x ) / tlGeo_.pps );
		return true;
	},
	        14 );
	Ctrl( true );
	Button( false );
	Ctrl( false );
	Wait( 3 );
	Do( [ &tl ] {
		std::vector< TlBlock > b;
		TlBlocks( tl, tl.tracks[ S->wi ], b );
		bool copy = false, orig = false;
		for( auto& x : b )
		{
			copy = copy || ( fabs( x.t0 - S->expect ) < 1e-6 && x.keys == S->n );
			orig = orig || fabs( x.t0 - S->a ) < 1e-6;
		}
		std::string dump;
		for( auto& x : b )
		{
			char q[ 64 ];
			snprintf( q, sizeof( q ), " [%.2f-%.2f:%d]", x.t0, x.t1, x.keys );
			dump += q;
		}
		Check( copy && orig && KeysOf( tl.tracks[ S->wi ] ) == S->m + S->n, "Abschnitt mit Strg kopieren: Kopie bei %.2f s (%d Keys), Original bei %.2f s bleibt, %d Keys insgesamt (vorher %d);%s", S->expect, S->n, S->a, KeysOf( tl.tracks[ S->wi ] ), S->m, dump.c_str() );
		S->a = b[ 0 ].t0;
		S->b = b[ 0 ].t1;
	} );
	//3) trim: drag the right edge of block 0 three seconds to the left
	MoveTo( [ = ]( ImVec2& p ) { ImVec4 r; if( !Mark( blockMark( 0 ), r ) ) return false; p = ImVec2( r.z - 2, ( r.y + r.w ) * 0.5f ); return true; } );
	Wait( 2 );
	Button( true );
	MoveTo( [ this, &tl ]( ImVec2& p ) {
		const float dx = (float)( -3.0 * tlGeo_.pps );
		p              = ImVec2( S->mouse.x + dx, S->mouse.y );
		S->expect      = tl.Snap( S->b + dx / tlGeo_.pps );
		return true;
	},
	        14 );
	Wait( 2 );
	Button( false );
	Wait( 3 );
	Do( [ &tl ] {
		std::vector< TlBlock > b;
		TlBlocks( tl, tl.tracks[ S->wi ], b );
		Check( fabs( b[ 0 ].t1 - S->expect ) < 1e-6 && S->expect < S->b - 1 && fabs( b[ 0 ].t0 - S->a ) < 1e-6, "Abschnitt beschneiden (rechte Kante ziehen): Ende %.2f s -> %.2f s (erwartet %.2f s)", S->b, b[ 0 ].t1, S->expect );
		S->n = (int)b.size();
		S->b = b[ 0 ].t1;
	} );
	//4) S: split block 0 at the playhead
	Do( [ this, &tl ] { tl.Locate( show, S->a + ( S->b - S->a ) * 0.5 + 0.11 ); } );
	MoveTo( [ = ]( ImVec2& p ) { ImVec4 r; if( !Mark( blockMark( 0 ), r ) ) return false; p = Centre( r ); return true; } );
	Wait( 2 );
	Button( true );
	Button( false );
	Wait( 2 );
	Key( ImGuiKey_S );
	Wait( 3 );
	Do( [ &tl ] {
		std::vector< TlBlock > b;
		TlBlocks( tl, tl.tracks[ S->wi ], b );
		Check( (int)b.size() == S->n + 1 && fabs( b[ 1 ].t0 - tl.rt.pos ) < 1e-6, "Abschnitt teilen (Klick + Taste S): %d -> %d Abschnitte, Schnitt bei %.2f s", S->n, (int)b.size(), tl.rt.pos );
	} );
	//================= drag & drop: snapshot -> Time Line (through the tab)
	Do( [ this ] {
		Check( !show.snapshots.empty(), "Demo hat Snapshots (%d)", (int)show.snapshots.size() );
		if( show.snapshots.empty() ) S->aborted = true;
		if( show.effects.empty() ) AddEffectOfType( FxType::Intensity );
	} );
	Click( "tab.snapshots" );
	Wait( 4 );
	MoveToMark( "snap.grip.0" );
	Wait( 2 );
	Button( true );
	MoveToMark( "tab.timeline", 14 );
	Wait( 8 );//the tab opens while the snapshot is held over it
	MoveTo( [ this, Tx, &tl ]( ImVec2& p ) {
		ImVec4 r;
		if( !Mark( "tl.area", r ) ) return false;
		p         = ImVec2( Tx( 30.1 ), ( r.y + r.w ) * 0.5f );//not half way between two grid lines
		S->expect = std::max( 0.0, tl.Snap( tlGeo_.view0 + ( p.x - tlGeo_.x0 ) / tlGeo_.pps ) );
		return true;
	},
	        14 );
	Wait( 3 );
	Button( false );
	Wait( 4 );
	Do( [ this, &tl ] {
		bool ok = false;
		for( auto& t : tl.tracks )
			if( t.kind == TlTrackKind::Clips )
				for( auto& c : t.clips ) ok = ok || ( c.kind == TlClipKind::Snapshot && c.snap == show.snapshots[ 0 ].uid && fabs( c.t - S->expect ) < 1e-6 );
		std::string dump;
		for( auto& t : tl.tracks )
			for( auto& c : t.clips )
			{
				char q[ 64 ];
				snprintf( q, sizeof( q ), " [%s %u/%d @%.2f]", c.kind == TlClipKind::Snapshot ? "snap" : "fx", c.snap, c.fx, c.t );
				dump += q;
			}
		Check( ok, "Snapshot am Griff auf den Tab Time Line und weiter auf die Zeitfläche ziehen: Block bei %.2f s (Snapshot-Kennung %u);%s", S->expect, show.snapshots[ 0 ].uid, dump.c_str() );
		S->n = (int)tl.tracks.size();
	} );
	//================= drag & drop: effect -> the clip track
	Click( "tab.effects" );
	Wait( 4 );
	MoveToMark( "fx.row.0" );
	Wait( 2 );
	Button( true );
	MoveToMark( "tab.timeline", 14 );
	Wait( 8 );
	MoveTo( [ this, Tx, &tl ]( ImVec2& p ) {
		ImVec4 r;
		if( !Mark( "tl.area", r ) ) return false;
		p         = ImVec2( Tx( 38.1 ), ( r.y + r.w ) * 0.5f );
		S->expect = std::max( 0.0, tl.Snap( tlGeo_.view0 + ( p.x - tlGeo_.x0 ) / tlGeo_.pps ) );
		return true;
	},
	        14 );
	Wait( 3 );
	Button( false );
	Wait( 4 );
	Do( [ this, &tl ] {
		bool ok = false;
		for( auto& t : tl.tracks )
			if( t.kind == TlTrackKind::Clips )
				for( auto& c : t.clips ) ok = ok || ( c.kind == TlClipKind::Effect && c.fx == show.effects[ 0 ].id && fabs( c.t - S->expect ) < 1e-6 );
		Check( ok && (int)tl.tracks.size() == S->n, "Effekt aus der Effektliste auf die Time Line ziehen: Block bei %.2f s in derselben Clip-Spur (%d Spuren)", S->expect, (int)tl.tracks.size() );
	} );
	//================= audio file dropped onto the time area (the place of the drop, not the real mouse)
	Do( [ this, Tx, &tl ] {
		ImVec4 r;
		if( !Mark( "tl.area", r ) ) return;
		const float x = Tx( 8.0 );
		DropFileAt( "C:\\uitest\\drop_test.wav", x, ( r.y + r.w ) * 0.5f );
		const TlTrack* at = nullptr;
		int na            = 0;
		for( auto& t : tl.tracks )
			if( t.kind == TlTrackKind::Audio ) { at = &t; ++na; }
		const double ex = std::max( 0.0, tl.Snap( tlGeo_.view0 + ( x - tlGeo_.x0 ) / tlGeo_.pps ) );
		Check( at && na == 1 && at->name == "drop_test" && fabs( at->offset - ex ) < 1e-6, "Audiodatei über der Zeitfläche ablegen: Audiospur '%s' startet bei %.2f s (erwartet %.2f s)", at ? at->name.c_str() : "-", at ? at->offset : -1.0, ex );
	} );
	//================= 2.1 status bar: DMX field opens Output / Interface, quality chips switch the level
	Click( "status.dmx" );
	Wait( 3 );
	Do( [ this ] { Check( ImGui::GetFrameCount() - lastOutputTabFrame_ <= 2, "Statusleiste: Klick auf DMX öffnet 'Ausgang / Interface'" ); } );
	Do( [ this ] { S->n = quality_; } );
	Click( "vp.quality0" );
	Do( [ this ] { Check( quality_ == 0 && ren.quality.shadows == false && ren.quality.beamQuality > 0, "Qualität Entwurf per Chip: ohne Schatten, Kegel-Schritte begrenzt (%.2f)", ren.quality.beamQuality ); } );
	Click( "vp.quality1" );
	Do( [ this ] { Check( quality_ == 1 && ren.quality.shadows && ren.quality.bloom && ren.quality.beamQuality <= 0 && !ren.quality.high, "Qualität Normal per Chip: keine Begrenzung, keine Extras (wie 2.0)" ); } );
	Click( "vp.quality2" );
	Do( [ this ] { Check( quality_ == 2 && ren.quality.high && ren.quality.shadows, "Qualität Hoch per Chip: Extras an (Spiegelungen, AO, weiche Schatten)" ); } );
	Do( [ this ] { quality_ = qualityEff_ = S->n; SaveSettings(); } );
	Wait( 2 );
}

void App::UiTestInput()
{
	if( !ut_ || ut_->done ) return;
	std::lock_guard< std::recursive_mutex > engineGuard( engineLock_ );
	UiTestState& st = *ut_;
	++st.frame;
	//when the test window has the focus, the Win32 backend feeds the real mouse position every frame: the script's position goes last
	ImGui::GetIO().AddMousePosEvent( st.mouse.x, st.mouse.y );
	//one step per frame; a step is called every frame until it reports "finished"
	if( st.op < st.ops.size() && !st.aborted )
	{
		const bool fin = st.ops[ st.op ]();
		++st.opFrame;
		if( fin )
		{
			++st.op;
			st.opFrame = 0;
		}
	}
	if( st.frame > 6000 && !st.aborted ) { Check( false, "Zeitüberschreitung im Schritt %d", (int)st.op ); st.aborted = true; }
	if( st.op >= st.ops.size() || st.aborted )
	{
		ImGui::GetIO().AddMouseButtonEvent( 0, false );
		ImGui::GetIO().AddKeyEvent( ImGuiMod_Ctrl, false );
		fprintf( st.out, "\n%d OK, %d FAIL\n", st.pass, st.fail );
		fclose( st.out );
		st.done = true;
	}
	uiMarks_.clear();//the next frame reports its places again
}
bool App::UiTestDone() const { return ut_ && ut_->done; }
int App::UiTestFails() const { return ut_ ? ut_->fail : 0; }
}//namespace ls
