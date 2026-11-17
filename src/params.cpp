#include "params.h"
#include "effects.h"
#include <map>

namespace ls
{
static std::string Key( const std::string& s )
{
	std::string r;
	for( char c : ToLower( s ) )
		if( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) ) r += c;
	return r;
}
static std::string Num( int i ) { return std::to_string( i ); }

//structure signature: the cache is rebuilt when it changes
static std::string Signature( const Show& s )
{
	std::string r;
	r.reserve( 256 );
	for( auto& f : s.fixtures ) r += Num( f.id ) + ":" + f.defId + "#" + Num( f.mode ) + ",";
	r += "|p" + Num( (int)s.projectors.size() ) + "s" + Num( (int)s.screens.size() ) + "m" + Num( (int)s.models.size() ) + "t" + Num( (int)s.stands.size() ) + "n" + Num( (int)s.snapshots.size() ) + "g" + Num( (int)s.programs.size() ) + "e";
	for( auto& e : s.effects ) r += Num( e.id ) + ":" + Num( (int)e.kind ) + ":" + Num( (int)e.colors.size() ) + ",";
	for( auto& p : s.programs ) r += p.name + ";";
	for( auto& n : s.snapshots ) r += n.name + ";";
	r += "|q" + Num( gLang ) + "c" + Num( (int)s.cues.size() );
	for( auto& g : s.groups ) r += g.name + ":" + Num( g.kind ) + ":" + Num( (int)g.attr ) + ";";
	return r;
}

static void EffectFieldParams( Show* s, std::vector< Param >& out, const std::string& g, const std::string& b, const std::string& n, std::function< Effect*() > fx, FxKind kind, bool any, bool withBase );

static void Build( Show& S, std::vector< Param >& out )
{
	out.clear();
	auto F = [ & ]( const std::string& g, const std::string& a, const std::string& l, float mn, float mx, float* v ) {
		Param p;
		p.group = g;
		p.addr  = a;
		p.label = l;
		p.min   = mn;
		p.max   = mx;
		p.get   = [ v ] { return *v; };
		p.set   = [ v, mn, mx ]( float x ) { *v = Clamp( x, mn, mx ); };
		out.push_back( std::move( p ) );
	};
	auto B = [ & ]( const std::string& g, const std::string& a, const std::string& l, bool* v ) {
		Param p;
		p.group = g;
		p.addr  = a;
		p.label = l;
		p.type  = ParamType::Bool;
		p.get   = [ v ] { return *v ? 1.0f : 0.0f; };
		p.set   = [ v ]( float x ) { *v = x > 0.5f; };
		out.push_back( std::move( p ) );
	};
	auto I = [ & ]( const std::string& g, const std::string& a, const std::string& l, int mn, int mx, std::function< float() > get, std::function< void( float ) > set ) {
		Param p;
		p.group = g;
		p.addr  = a;
		p.label = l;
		p.type  = ParamType::Int;
		p.min   = (float)mn;
		p.max   = (float)mx;
		p.get   = std::move( get );
		p.set   = std::move( set );
		out.push_back( std::move( p ) );
	};
	auto T = [ & ]( const std::string& g, const std::string& a, const std::string& l, std::function< void() > fire ) {
		Param p;
		p.group = g;
		p.addr  = a;
		p.label = l;
		p.type  = ParamType::Trigger;
		p.get   = [] { return 0.0f; };
		p.set   = [ fire ]( float x ) { if( x > 0.5f ) fire(); };
		out.push_back( std::move( p ) );
	};
	auto G = [ & ]( const std::string& g, const std::string& a, const std::string& l, float mn, float mx, std::function< float() > get, std::function< void( float ) > set ) {
		Param p;
		p.group = g;
		p.addr  = a;
		p.label = l;
		p.min   = mn;
		p.max   = mx;
		p.get   = std::move( get );
		p.set   = std::move( set );
		out.push_back( std::move( p ) );
	};
	Show* s = &S;

	//---- global
	const std::string gG = L( "Allgemein" );
	F( gG, "/ls/master", "Master (1 = 100 %)", 0, 10, &S.master );
	F( gG, "/ls/haze", "Haze", 0, 1, &S.stage.haze );
	B( gG, "/ls/worklight", L( "Arbeitslicht" ), &S.workLight );
	//---- snapshots + group faders
	const std::string gN = L( "Snapshots & Gruppen" );
	T( gN, "/ls/snapshot/off", L( "Alle Snapshots aus" ), [ s ] { s->ReleaseAllSnapshots(); } );
	for( int i = 0; i < (int)S.snapshots.size(); ++i )
	{
		const std::string b = "/ls/snapshot/" + Num( i + 1 ), n = "Snapshot " + Num( i + 1 ) + " " + S.snapshots[ i ].name;
		auto sn             = [ s, i ]() -> Snapshot* { return i < (int)s->snapshots.size() ? &s->snapshots[ i ] : nullptr; };
		T( gN, b, n + L( " starten" ), [ s, i ] { s->RecallSnapshot( i ); } );
		Param on;
		on.group = gN;
		on.addr  = b + "/on";
		on.label = n + L( " an/aus" );
		on.type  = ParamType::Bool;
		on.get   = [ sn ] { Snapshot* x = sn(); return x && x->active ? 1.0f : 0.0f; };
		on.set   = [ s, i, sn ]( float x ) { Snapshot* y = sn(); if( !y ) return; if( x > 0.5f && !y->active ) s->RecallSnapshot( i ); else if( x <= 0.5f && y->active ) s->ReleaseSnapshot( i ); };
		out.push_back( std::move( on ) );
		T( gN, b + "/toggle", n + L( " umschalten" ), [ s, i ] { s->ToggleSnapshot( i ); } );
		G( gN, b + "/master", n + L( " Stärke" ), 0, 1, [ sn ] { Snapshot* x = sn(); return x ? x->master : 0.0f; }, [ sn ]( float v ) { if( Snapshot* x = sn() ) x->master = Saturate( v ); } );
		G( gN, b + "/fade", n + L( " Fade-Zeit (s)" ), 0, 30, [ sn ] { Snapshot* x = sn(); return x ? x->fade : 0.0f; }, [ sn ]( float v ) { if( Snapshot* x = sn() ) x->fade = Clamp( v, 0.0f, 600.0f ); } );
	}
	//---- cue list
	{
		const std::string gC = L( "Cue-Liste" );
		T( gC, "/ls/cue/go", L( "Cue GO (nächster)" ), [ s ] { s->CueGo(); } );
		T( gC, "/ls/cue/back", L( "Cue zurück" ), [ s ] { s->CueBack(); } );
		T( gC, "/ls/cue/stop", L( "Cue-Liste stoppen" ), [ s ] { s->CueStop(); } );
		if( !S.cues.empty() ) I( gC, "/ls/cue/goto", L( "Cue Nummer starten" ), 1, (int)S.cues.size(), [ s ] { return (float)( s->cueCur + 1 ); }, [ s ]( float v ) { const int n = (int)lroundf( v ); if( n >= 1 && n != s->cueCur + 1 ) s->CueGo( n - 1 ); } );
		B( gC, "/ls/cue/timecode", L( "Cue-Liste folgt Timecode" ), &S.cueTimecode );
	}
	//---- 2.0 show timeline
	{
		const std::string gT = "Time Line";
		T( gT, "/ls/tl/play", L( "Time Line abspielen" ), [ s ] { s->tl.Play( *s, true ); } );
		T( gT, "/ls/tl/pause", L( "Time Line Pause" ), [ s ] { s->tl.Play( *s, false ); } );
		T( gT, "/ls/tl/toggle", L( "Time Line Play / Pause" ), [ s ] { s->tl.Play( *s, !s->tl.rt.playing ); } );
		T( gT, "/ls/tl/stop", L( "Time Line Stopp (zum Anfang)" ), [ s ] { s->tl.Play( *s, false ); s->tl.Locate( *s, 0 ); } );
		G( gT, "/ls/tl/pos", L( "Time Line Position (s)" ), 0, (float)std::max( 1.0, S.tl.length ), [ s ] { return (float)s->tl.rt.pos; }, [ s ]( float v ) { s->tl.Locate( *s, v ); } );
		F( gT, "/ls/tl/master", L( "Time Line Master" ), 0, 1, &S.tl.master );
		B( gT, "/ls/tl/output", L( "Time Line Ausgabe" ), &S.tl.output );
		B( gT, "/ls/tl/loop", L( "Time Line Loop" ), &S.tl.loop );
		for( int i = 0; i < (int)S.tl.markers.size() && i < 64; ++i )
			T( gT, "/ls/tl/marker/" + Num( i + 1 ), L( "Time Line zu Marker " ) + Num( i + 1 ) + " " + S.tl.markers[ i ].name, [ s, i ] { if( i < (int)s->tl.markers.size() ) s->tl.Locate( *s, s->tl.markers[ i ].t ); } );
	}
	for( int i = 0; i < (int)S.groups.size(); ++i )
	{
		const std::string b = "/ls/group/" + Num( i + 1 ), n = L( "Gruppe " ) + Num( i + 1 ) + " " + S.groups[ i ].name;
		auto gp             = [ s, i ]() -> GroupFader* { return i < (int)s->groups.size() ? &s->groups[ i ] : nullptr; };
		G( gN, b + "/value", n + ( S.groups[ i ].kind == 1 ? L( " Helligkeit" ) : std::string( " " ) + AttrName( S.groups[ i ].attr ) ), 0, 1, [ gp ] { GroupFader* g = gp(); return g ? g->value : 0.0f; }, [ s, i, gp ]( float v ) { if( GroupFader* g = gp() ) { g->value = Saturate( v ); s->ApplyGroup( i ); } } );
		if( S.groups[ i ].kind == 1 )
		{
			G( gN, b + "/hue", n + L( " Farbton" ), 0, 1, [ gp ] { GroupFader* g = gp(); return g ? g->hue : 0.0f; }, [ s, i, gp ]( float v ) { if( GroupFader* g = gp() ) { g->hue = Saturate( v ); s->ApplyGroup( i ); } } );
			G( gN, b + "/sat", n + L( " Sättigung" ), 0, 1, [ gp ] { GroupFader* g = gp(); return g ? g->sat : 0.0f; }, [ s, i, gp ]( float v ) { if( GroupFader* g = gp() ) { g->sat = Saturate( v ); s->ApplyGroup( i ); } } );
		}
	}

	//---- stage / environment
	const std::string gS = L( "Bühne & Umgebung" );
	StageCfg& st = S.stage;
	F( gS, "/ls/stage/hazenoise", L( "Haze-Struktur" ), 0, 1, &st.hazeNoise );
	F( gS, "/ls/stage/ambient", L( "Umgebungslicht" ), 0, 0.5f, &st.ambient );
	F( gS, "/ls/stage/envlight", L( "Umgebung (Faktor)" ), 0, 4, &st.envLight );
	F( gS, "/ls/stage/ambientcolor/r", L( "Umgebungsfarbe Rot" ), 0, 1, &st.ambientColor.x );
	F( gS, "/ls/stage/ambientcolor/g", L( "Umgebungsfarbe Grün" ), 0, 1, &st.ambientColor.y );
	F( gS, "/ls/stage/ambientcolor/b", L( "Umgebungsfarbe Blau" ), 0, 1, &st.ambientColor.z );
	F( gS, "/ls/stage/exposure", L( "Belichtung" ), 0.1f, 4, &st.exposure );
	F( gS, "/ls/stage/bloom", "Bloom", 0, 2, &st.bloom );
	F( gS, "/ls/stage/beamquality", L( "Kegel-Qualität" ), 0.25f, 2, &st.beamQuality );
	B( gS, "/ls/stage/beams", L( "Lichtkegel" ), &st.beams );
	F( gS, "/ls/stage/beamlevel", L( "Kegel-Stärke" ), 0, 2, &st.beamLevel );
	B( gS, "/ls/stage/shadows", L( "Schatten" ), &st.shadows );
	F( gS, "/ls/stage/width", L( "Bühne Breite (m)" ), 2, 60, &st.width );
	F( gS, "/ls/stage/depth", L( "Bühne Tiefe (m)" ), 2, 40, &st.depth );
	F( gS, "/ls/stage/deckheight", L( "Podesthöhe (m)" ), 0, 4, &st.deckHeight );
	B( gS, "/ls/stage/deck", L( "Podest" ), &st.deck );
	B( gS, "/ls/stage/backwall", L( "Rückwand" ), &st.backWall );
	I( gS, "/ls/stage/environment", L( "Ort (0 Halle, 1 Outdoor)" ), 0, 1, [ s ] { return (float)s->stage.environment; }, [ s ]( float x ) { s->stage.environment = Clamp( (int)lroundf( x ), 0, 1 ); } );
	I( gS, "/ls/stage/ground", L( "Untergrund (0 Halle .. 5 Holz)" ), 0, 5, [ s ] { return (float)s->stage.ground; }, [ s ]( float x ) { s->stage.ground = Clamp( (int)lroundf( x ), 0, 5 ); } );
	F( gS, "/ls/stage/stars", L( "Sterne" ), 0, 1, &st.stars );
	F( gS, "/ls/stage/moon", L( "Mond" ), 0, 1, &st.moon );
	F( gS, "/ls/stage/moonaz", L( "Mond Richtung (Grad)" ), -180, 180, &st.moonAz );
	F( gS, "/ls/stage/moonel", L( "Mond Höhe (Grad)" ), 0, 90, &st.moonEl );
	B( gS, "/ls/stage/moonshadow", L( "Mond wirft Schatten" ), &st.moonShadow );
	F( gS, "/ls/stage/distancefog", L( "Ferner Dunst" ), 0, 1, &st.distanceFog );
	for( int i = 0; i < 5; ++i ) T( gS, "/ls/stage/mood/" + Num( i + 1 ), std::string( L( "Stimmung: " ) ) + Ls( kMoodNames[ i ] ), [ s, i ] { ApplyMood( s->stage, i ); } );

	//---- tempo + effects
	const std::string gT = L( "Tempo & Effekte" );
	F( gT, "/ls/tempo/bpm", L( "Tempo (BPM, intern)" ), 20, 300, &S.tempo.bpm );
	T( gT, "/ls/tempo/tap", L( "Tap-Tempo" ), [ s ] { TempoTap( s->tempo, Now() ); } );
	T( gT, "/ls/tempo/resync", L( "Taktanfang (Resync)" ), [ s ] { TempoResync( s->tempo ); } );
	I( gT, "/ls/tempo/source", L( "Tempoquelle (0 Intern, 1 MIDI-Clock, 2 Link)" ), 0, 2, [ s ] { return (float)s->tempo.source; }, [ s ]( float x ) { s->tempo.source = (TempoSource)Clamp( (int)lroundf( x ), 0, 2 ); } );
	T( gT, "/ls/fx/stop", L( "Alle Effekte aus" ), [ s ] { StopAllEffects( *s ); } );
	G( gT, "/ls/fx/master", L( "Effekte gesamt (Anteil)" ), 0, 1, [ s ] { return s->fxMaster; }, [ s ]( float x ) { s->fxMaster = Saturate( x ); } );
	G( gT, "/ls/fx/speed", L( "Tempo × (alle Effekte, Mitte = 1)" ), 0.25f, 4, [ s ] { return s->fxSpeed; }, [ s ]( float x ) { s->fxSpeed = Clamp( x, 0.25f, 4.0f ); } );
	B( gT, "/ls/fx/hold", L( "Effekte einfrieren" ), &S.fxHold );
	T( gT, "/ls/program/stop", L( "Programm beenden" ), [ s ] { StopAllEffects( *s ); } );
	T( gT, "/ls/program/next", L( "nächstes Programm" ), [ s ] { const int n = (int)s->programs.size(); if( n ) { const int a = ActiveProgram( *s ); StartProgram( *s, a < 0 ? 0 : ( a + 1 ) % n, true ); } } );
	T( gT, "/ls/program/prev", L( "vorheriges Programm" ), [ s ] { const int n = (int)s->programs.size(); if( n ) { const int a = ActiveProgram( *s ); StartProgram( *s, a < 0 ? n - 1 : ( a - 1 + n ) % n, true ); } } );
	for( int i = 0; i < (int)S.programs.size(); ++i )
		T( gT, "/ls/program/" + Num( i + 1 ), L( "Programm " ) + Num( i + 1 ) + " " + S.programs[ i ].name, [ s, i ] { StartProgram( *s, i, true ); } );
	for( auto& e : S.effects )
	{
		const int id      = e.id;
		const std::string b = "/ls/fx/" + Num( id ), n = L( "Effekt " ) + Num( id ) + " " + e.name;
		auto fx           = [ s, id ]() -> Effect* { return s->FindEffect( id ); };
		G( gT, b + "/on", n + L( ": an/aus" ), 0, 1, [ fx ] { Effect* e = fx(); return e && e->on ? 1.0f : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->on = x > 0.5f; } );
		out.back().type = ParamType::Bool;
		T( gT, b + "/toggle", n + L( ": umschalten" ), [ fx ] { if( Effect* e = fx() ) e->on = !e->on; } );
		G( gT, b + "/level", n + L( ": Anteil" ), 0, 1, [ fx ] { Effect* e = fx(); return e ? e->level : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->level = Saturate( x ); } );
		G( gT, b + "/beats", n + L( ": Beats je Zyklus" ), 0.0625f, 64, [ fx ] { Effect* e = fx(); return e ? e->beats : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->beats = Clamp( x, 0.0625f, 64.0f ); } );
		G( gT, b + "/hz", n + L( ": Frequenz frei (Hz)" ), 0.01f, 10, [ fx ] { Effect* e = fx(); return e ? e->hz : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->hz = Clamp( x, 0.01f, 10.0f ); } );
		G( gT, b + "/spread", n + L( ": Phasenversatz (Grad)" ), 0, 720, [ fx ] { Effect* e = fx(); return e ? e->spread : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->spread = Clamp( x, 0.0f, 720.0f ); } );
		G( gT, b + "/width", n + L( ": Breite" ), 0.01f, 1, [ fx ] { Effect* e = fx(); return e ? e->width : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->width = Clamp( x, 0.01f, 1.0f ); } );
		if( e.kind == FxKind::Position )
		{
			G( gT, b + "/sizepan", n + L( ": Größe Pan (Grad)" ), 0, 540, [ fx ] { Effect* e = fx(); return e ? e->sizePan : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->sizePan = Clamp( x, 0.0f, 540.0f ); } );
			G( gT, b + "/sizetilt", n + L( ": Größe Tilt (Grad)" ), 0, 270, [ fx ] { Effect* e = fx(); return e ? e->sizeTilt : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->sizeTilt = Clamp( x, 0.0f, 270.0f ); } );
		}
		else if( e.kind != FxKind::Color )
		{
			G( gT, b + "/lo", n + ": Minimum", 0, 1, [ fx ] { Effect* e = fx(); return e ? e->lo : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->lo = Saturate( x ); } );
			G( gT, b + "/hi", n + ": Maximum", 0, 1, [ fx ] { Effect* e = fx(); return e ? e->hi : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->hi = Saturate( x ); } );
		}
		EffectFieldParams( s, out, gT, b, n, fx, e.kind, false, false );
	}
	//the selected effect (Show::fxSelId): same fields, for the hardware controls
	{
		auto selFx = [ s ]() -> Effect* { return s->FindEffect( s->fxSelId ); };
		EffectFieldParams( s, out, L( "Ausgewählter Effekt" ), "/ls/fx/selected", L( "Ausgewählter Effekt" ), selFx, FxKind::Dimmer, true, true );
		//selection: index in the effect list (1-based), next / previous
		auto selIdx = [ s ]() { for( size_t i = 0; i < s->effects.size(); ++i ) if( s->effects[ i ].id == s->fxSelId ) return (int)i; return -1; };
		I( L( "Ausgewählter Effekt" ), "/ls/fx/select", L( "Ausgewählter Effekt: Nummer in der Liste" ), 1, std::max( 1, (int)s->effects.size() ), [ selIdx ] { return (float)( selIdx() + 1 ); },
		   [ s ]( float x ) { const int k = Clamp( (int)lroundf( x ) - 1, 0, (int)s->effects.size() - 1 ); if( k >= 0 ) s->fxSelId = s->effects[ k ].id; } );
		T( L( "Ausgewählter Effekt" ), "/ls/fx/select/next", L( "Ausgewählter Effekt: nächster" ), [ s, selIdx ] { const int n = (int)s->effects.size(); if( n ) s->fxSelId = s->effects[ ( selIdx() + 1 + n ) % n ].id; } );
		T( L( "Ausgewählter Effekt" ), "/ls/fx/select/prev", L( "Ausgewählter Effekt: vorheriger" ), [ s, selIdx ] { const int n = (int)s->effects.size(); if( n ) s->fxSelId = s->effects[ ( selIdx() - 1 + n * 2 ) % n ].id; } );
	}

	//---- lamps
	const std::string gF = L( "Lampen" );
	for( auto& f : S.fixtures )
	{
		const int id        = f.id;
		const std::string b = "/ls/fixture/" + Num( id ), n = Num( id ) + " " + f.name;
		auto fi             = [ s, id ]() -> FixtureInst* { return s->FindFixture( id ); };
		const Mode* m       = f.M();
		std::vector< Attr > seen;
		if( m )
			for( auto& c : m->ch )
			{
				if( c.isFine || c.attr == Attr::None || std::find( seen.begin(), seen.end(), c.attr ) != seen.end() ) continue;
				seen.push_back( c.attr );
				const Attr a = c.attr;
				G( gF, b + "/" + Key( AttrName( a ) ), n + ": " + AttrName( a ), 0, 1, [ s, fi, a ] { FixtureInst* f = fi(); return f ? s->GetAttr( *f, a ) : 0.0f; }, [ s, fi, a ]( float x ) { if( FixtureInst* f = fi() ) s->SetAttr( *f, a, x ); } );
			}
		const char* ax[ 3 ] = { "x", "y", "z" };
		for( int k = 0; k < 3; ++k )
			G( gF, b + "/pos/" + ax[ k ], n + ": Position " + ax[ k ] + " (m)", -100, 100, [ fi, k ] { FixtureInst* f = fi(); return f ? f->pos[ k ] : 0.0f; }, [ s, fi, k ]( float x ) { if( FixtureInst* f = fi() ) { f->pos[ k ] = Clamp( x, -100.0f, 100.0f ); s->dirty = true; } } );
		for( int k = 0; k < 3; ++k )
			G( gF, b + "/rot/" + ax[ k ], n + ": Rotation " + ax[ k ] + L( " (Grad)" ), -360, 360, [ fi, k ] { FixtureInst* f = fi(); return f ? f->rot[ k ] : 0.0f; }, [ s, fi, k ]( float x ) { if( FixtureInst* f = fi() ) { f->rot[ k ] = Clamp( x, -360.0f, 360.0f ); s->dirty = true; } } );
		G( gF, b + "/beam", n + L( ": Abstrahlwinkel (Grad, 0 = Profil)" ), 0, 120, [ fi ] { FixtureInst* f = fi(); return f ? f->beam : 0.0f; }, [ fi ]( float x ) { if( FixtureInst* f = fi() ) f->beam = Clamp( x, 0.0f, 120.0f ); } );
	}

	//---- video
	const std::string gV = L( "Video / Leinwand" );
	for( int i = 0; i < (int)S.projectors.size(); ++i )
	{
		const std::string b = "/ls/projector/" + Num( i + 1 ), n = L( "Projektor " ) + Num( i + 1 );
		auto pj             = [ s, i ]() -> Projector* { return i < (int)s->projectors.size() ? &s->projectors[ i ] : nullptr; };
		G( gV, b + "/on", n + L( ": an/aus" ), 0, 1, [ pj ] { Projector* p = pj(); return p && p->on ? 1.0f : 0.0f; }, [ pj ]( float x ) { if( Projector* p = pj() ) p->on = x > 0.5f; } );
		out.back().type = ParamType::Bool;
		struct PF { const char* k; const char* l; float mn, mx; float Projector::*m; };
		const PF pf[] = { { "pan", L( "Pan (Grad)" ), -180, 180, &Projector::pan }, { "tilt", L( "Tilt (Grad)" ), -90, 90, &Projector::tilt }, { "roll", L( "Roll (Grad)" ), -180, 180, &Projector::roll },
		                         { "zoom", L( "Projektionsverhältnis" ), 0.2f, 8, &Projector::throwRatio }, { "shiftx", "Shift X", -1, 1, &Projector::shiftX }, { "shifty", "Shift Y", -1, 1, &Projector::shiftY },
		                         { "lumens", "Lumen", 0, 60000, &Projector::lumens }, { "blur", L( "Unschärfe" ), 0, 1, &Projector::blur } };
		for( auto& q : pf )
		{
			float Projector::*mp = q.m;
			const float mn = q.mn, mx = q.mx;
			G( gV, b + "/" + q.k, n + ": " + q.l, mn, mx, [ pj, mp ] { Projector* p = pj(); return p ? p->*mp : 0.0f; }, [ pj, mp, mn, mx ]( float x ) { if( Projector* p = pj() ) p->*mp = Clamp( x, mn, mx ); } );
		}
		const char* ax[ 3 ] = { "x", "y", "z" };
		for( int k = 0; k < 3; ++k )
			G( gV, b + "/pos/" + ax[ k ], n + ": Position " + ax[ k ] + " (m)", -100, 100, [ pj, k ] { Projector* p = pj(); return p ? p->pos[ k ] : 0.0f; }, [ pj, k ]( float x ) { if( Projector* p = pj() ) p->pos[ k ] = Clamp( x, -100.0f, 100.0f ); } );
	}
	static const float kAsp[ 5 ] = { 16.0f / 9.0f, 16.0f / 10.0f, 4.0f / 3.0f, 21.0f / 9.0f, 1.0f };
	for( int i = 0; i < (int)S.screens.size(); ++i )
	{
		const std::string b = "/ls/screen/" + Num( i + 1 ), n = L( "Leinwand " ) + Num( i + 1 );
		auto sc             = [ s, i ]() -> Screen* { return i < (int)s->screens.size() ? &s->screens[ i ] : nullptr; };
		G( gV, b + "/width", n + L( ": Breite (m)" ), 0.2f, 40, [ sc ] { Screen* c = sc(); return c ? c->width : 0.0f; }, [ sc ]( float x ) { if( Screen* c = sc() ) { c->width = Clamp( x, 0.2f, 40.0f ); if( c->format >= 0 && c->format < 5 ) c->height = c->width / kAsp[ c->format ]; } } );
		G( gV, b + "/height", n + L( ": Höhe (m)" ), 0.2f, 25, [ sc ] { Screen* c = sc(); return c ? c->height : 0.0f; }, [ sc ]( float x ) { if( Screen* c = sc() ) { c->height = Clamp( x, 0.2f, 25.0f ); if( c->format >= 0 && c->format < 5 ) c->width = c->height * kAsp[ c->format ]; } } );
		G( gV, b + "/gain", n + ": Gain", 0.2f, 2.5f, [ sc ] { Screen* c = sc(); return c ? c->gain : 0.0f; }, [ sc ]( float x ) { if( Screen* c = sc() ) c->gain = Clamp( x, 0.2f, 2.5f ); } );
		G( gV, b + "/yaw", n + L( ": Drehung (Grad)" ), -180, 180, [ sc ] { Screen* c = sc(); return c ? c->yaw : 0.0f; }, [ sc ]( float x ) { if( Screen* c = sc() ) c->yaw = Clamp( x, -180.0f, 180.0f ); } );
		G( gV, b + "/tilt", n + L( ": Neigung (Grad)" ), -60, 60, [ sc ] { Screen* c = sc(); return c ? c->tilt : 0.0f; }, [ sc ]( float x ) { if( Screen* c = sc() ) c->tilt = Clamp( x, -60.0f, 60.0f ); } );
		const char* ax[ 3 ] = { "x", "y", "z" };
		for( int k = 0; k < 3; ++k )
			G( gV, b + "/pos/" + ax[ k ], n + ": Position " + ax[ k ] + " (m)", -100, 100, [ sc, k ] { Screen* c = sc(); return c ? c->pos[ k ] : 0.0f; }, [ sc, k ]( float x ) { if( Screen* c = sc() ) c->pos[ k ] = Clamp( x, -100.0f, 100.0f ); } );
	}

	//---- objects
	const std::string gO = L( "Objekte" );
	for( int i = 0; i < (int)S.models.size(); ++i )
	{
		const std::string b = "/ls/model/" + Num( i + 1 ), n = L( "Modell " ) + Num( i + 1 ) + " " + S.models[ i ].name;
		auto md             = [ s, i ]() -> ModelObj* { return i < (int)s->models.size() ? &s->models[ i ] : nullptr; };
		const char* ax[ 3 ] = { "x", "y", "z" };
		for( int k = 0; k < 3; ++k )
			G( gO, b + "/pos/" + ax[ k ], n + ": Position " + ax[ k ] + " (m)", -200, 200, [ md, k ] { ModelObj* m = md(); return m ? m->pos[ k ] : 0.0f; }, [ md, k ]( float x ) { if( ModelObj* m = md() ) m->pos[ k ] = Clamp( x, -200.0f, 200.0f ); } );
		for( int k = 0; k < 3; ++k )
			G( gO, b + "/rot/" + ax[ k ], n + ": Rotation " + ax[ k ] + L( " (Grad)" ), -360, 360, [ md, k ] { ModelObj* m = md(); return m ? m->rot[ k ] : 0.0f; }, [ md, k ]( float x ) { if( ModelObj* m = md() ) m->rot[ k ] = Clamp( x, -360.0f, 360.0f ); } );
		G( gO, b + "/scale", n + L( ": Skalierung" ), 0.001f, 1000, [ md ] { ModelObj* m = md(); return m ? m->scale : 0.0f; }, [ md ]( float x ) { if( ModelObj* m = md() ) m->scale = Clamp( x, 0.001f, 1000.0f ); } );
	}
	for( int i = 0; i < (int)S.stands.size(); ++i )
		G( gO, "/ls/stand/" + Num( i + 1 ) + "/height", L( "Stativ " ) + Num( i + 1 ) + L( ": Höhe (m)" ), 0.5f, 6, [ s, i ] { return i < (int)s->stands.size() ? s->stands[ i ].height : 0.0f; },
		   [ s, i ]( float x ) { if( i < (int)s->stands.size() ) s->stands[ i ].height = Clamp( x, 0.5f, 6.0f ); } );
}

//---------------------------------------------------------------- effect fields
//every setting of the effect editor as a register entry: per effect (/ls/fx/<id>/...) and for the selected effect (/ls/fx/selected/...)
static const float kBeatSteps[ 9 ] = { 0.25f, 0.5f, 1, 2, 4, 8, 16, 32, 64 };
static int NearestBeatStep( float b )
{
	int best = 0;
	for( int i = 1; i < 9; ++i )
		if( fabsf( kBeatSteps[ i ] - b ) < fabsf( kBeatSteps[ best ] - b ) ) best = i;
	return best;
}
//lamp attributes of the effect's lamps in enum order (index = value of the attribute parameter)
static std::vector< Attr > EffectAttrs( Show& s, const Effect* e )
{
	std::vector< Attr > list;
	if( !e ) return list;
	for( int a = 1; a < (int)Attr::Count; ++a )
	{
		for( int id : e->fixtures )
		{
			const FixtureInst* f = s.FindFixture( id );
			if( f && s.HasAttr( *f, (Attr)a ) )
			{
				list.push_back( (Attr)a );
				break;
			}
		}
	}
	return list;
}
//colour component c of palette colour k (0-based): 0 hue, 1 sat, 2 bright, 3 r, 4 g, 5 b
static float ColorPart( const Effect* e, int k, int c )
{
	if( !e || k < 0 || k >= (int)e->colors.size() ) return 0;
	const V3& v = e->colors[ k ];
	if( c >= 3 ) return c == 3 ? v.x : c == 4 ? v.y : v.z;
	const float mx = std::max( v.x, std::max( v.y, v.z ) ), mn = std::min( v.x, std::min( v.y, v.z ) ), d = mx - mn;
	if( c == 2 ) return mx;
	if( c == 1 ) return mx > 0 ? d / mx : 0;
	if( d <= 1e-6f ) return 0;
	float h;
	if( mx == v.x ) h = fmodf( ( v.y - v.z ) / d, 6.0f );
	else if( mx == v.y ) h = ( v.z - v.x ) / d + 2.0f;
	else h = ( v.x - v.y ) / d + 4.0f;
	h /= 6.0f;
	return h < 0 ? h + 1.0f : h;
}
static void SetColorPart( Effect* e, int k, int c, float x )
{
	if( !e || k < 0 || k >= (int)e->colors.size() ) return;
	V3& v = e->colors[ k ];
	if( c >= 3 )
	{
		( c == 3 ? v.x : c == 4 ? v.y : v.z ) = Saturate( x );
		return;
	}
	const float h = c == 0 ? Saturate( x ) : ColorPart( e, k, 0 ), s = c == 1 ? Saturate( x ) : ColorPart( e, k, 1 ), b = c == 2 ? Saturate( x ) : ColorPart( e, k, 2 );
	v = Hsv( h, s, b );
}
static void EffectFieldParams( Show* s, std::vector< Param >& out, const std::string& g, const std::string& b, const std::string& n, std::function< Effect*() > fx, FxKind kind, bool any, bool withBase )
{
	auto add = [ & ]( const std::string& a, const std::string& l, ParamType t, float mn, float mx, std::function< float() > get, std::function< void( float ) > set ) {
		Param p;
		p.group = g;
		p.addr  = a;
		p.label = n + ": " + l;
		p.type  = t;
		p.min   = mn;
		p.max   = mx;
		p.get   = std::move( get );
		p.set   = std::move( set );
		out.push_back( std::move( p ) );
	};
	auto fits = [ & ]( FxKind k ) { return any || kind == k; };
	Effect* cur = fx();
	if( withBase )
	{
		add( b + "/on", L( "an/aus" ), ParamType::Bool, 0, 1, [ fx ] { Effect* e = fx(); return e && e->on ? 1.0f : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->on = x > 0.5f; } );
		add( b + "/toggle", L( "umschalten" ), ParamType::Trigger, 0, 1, [] { return 0.0f; }, [ fx ]( float x ) { if( x > 0.5f ) if( Effect* e = fx() ) e->on = !e->on; } );
		add( b + "/level", L( "Anteil" ), ParamType::Float, 0, 1, [ fx ] { Effect* e = fx(); return e ? e->level : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->level = Saturate( x ); } );
		add( b + "/beats", L( "Beats je Zyklus" ), ParamType::Float, 0.0625f, 64, [ fx ] { Effect* e = fx(); return e ? e->beats : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->beats = Clamp( x, 0.0625f, 64.0f ); } );
		add( b + "/hz", L( "Frequenz frei (Hz)" ), ParamType::Float, 0.01f, 10, [ fx ] { Effect* e = fx(); return e ? e->hz : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->hz = Clamp( x, 0.01f, 10.0f ); } );
		add( b + "/spread", L( "Phasenversatz (Grad)" ), ParamType::Float, 0, 720, [ fx ] { Effect* e = fx(); return e ? e->spread : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->spread = Clamp( x, 0.0f, 720.0f ); } );
		add( b + "/width", L( "Breite" ), ParamType::Float, 0.01f, 1, [ fx ] { Effect* e = fx(); return e ? e->width : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->width = Clamp( x, 0.01f, 1.0f ); } );
		add( b + "/lo", "Minimum", ParamType::Float, 0, 1, [ fx ] { Effect* e = fx(); return e ? e->lo : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->lo = Saturate( x ); } );
		add( b + "/hi", "Maximum", ParamType::Float, 0, 1, [ fx ] { Effect* e = fx(); return e ? e->hi : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->hi = Saturate( x ); } );
		add( b + "/sizepan", L( "Größe Pan (Grad)" ), ParamType::Float, 0, 540, [ fx ] { Effect* e = fx(); return e ? e->sizePan : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->sizePan = Clamp( x, 0.0f, 540.0f ); } );
		add( b + "/sizetilt", L( "Größe Tilt (Grad)" ), ParamType::Float, 0, 270, [ fx ] { Effect* e = fx(); return e ? e->sizeTilt : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->sizeTilt = Clamp( x, 0.0f, 270.0f ); } );
	}
	add( b + "/flash", L( "Flash (an solange gehalten)" ), ParamType::Bool, 0, 1, [ fx ] { Effect* e = fx(); return e && e->on ? 1.0f : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->on = x > 0.5f; } );
	add( b + "/sync", L( "zum Tempo synchron" ), ParamType::Bool, 0, 1, [ fx ] { Effect* e = fx(); return e && e->sync ? 1.0f : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->sync = x > 0.5f; } );
	add( b + "/beatstep", L( "Zyklus in Stufen (0 = 1/4 .. 8 = 64 Beats)" ), ParamType::Int, 0, 8, [ fx ] { Effect* e = fx(); return e ? (float)NearestBeatStep( e->beats ) : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->beats = kBeatSteps[ Clamp( (int)lroundf( x ), 0, 8 ) ]; } );
	add( b + "/fade", L( "Ein-/Ausblendzeit (s)" ), ParamType::Float, 0, 5, [ fx ] { Effect* e = fx(); return e ? e->fade : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->fade = Clamp( x, 0.0f, 5.0f ); } );
	add( b + "/order", L( "Reihenfolge (0 Auswahl, 1 L-R, 2 V-H, 3 Mitte-Außen, 4 Zufall)" ), ParamType::Int, 0, 4, [ fx ] { Effect* e = fx(); return e ? (float)(int)e->order : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->order = (FxOrder)Clamp( (int)lroundf( x ), 0, 4 ); } );
	add( b + "/groups", L( "Gruppen (0 = aus)" ), ParamType::Int, 0, 8, [ fx ] { Effect* e = fx(); return e ? (float)e->groups : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->groups = Clamp( (int)lroundf( x ), 0, 8 ); } );
	add( b + "/mirror", L( "spiegeln" ), ParamType::Bool, 0, 1, [ fx ] { Effect* e = fx(); return e && e->mirror ? 1.0f : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->mirror = x > 0.5f; } );
	add( b + "/reverse", L( "rückwärts" ), ParamType::Bool, 0, 1, [ fx ] { Effect* e = fx(); return e && e->reverse ? 1.0f : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->reverse = x > 0.5f; } );
	add( b + "/pixels", L( "Pixel einzeln" ), ParamType::Bool, 0, 1, [ fx ] { Effect* e = fx(); return e && e->pixels ? 1.0f : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->pixels = x > 0.5f; } );
	add( b + "/restart", L( "neu starten (Phase auf 0)" ), ParamType::Trigger, 0, 1, [] { return 0.0f; }, [ fx ]( float x ) { if( x > 0.5f ) if( Effect* e = fx() ) { e->syncBeat = 0; e->freePhase = 0; } } );
	add( b + "/trigger", L( "Hüllkurve auslösen" ), ParamType::Trigger, 0, 1, [] { return 0.0f; }, [ fx ]( float x ) { if( x > 0.5f ) if( Effect* e = fx() ) { e->trigBeat = e->syncBeat; e->trigTime = e->clock; } } );
	add( b + "/depth", L( "Tiefe (relativ)" ), ParamType::Float, 0, 1, [ fx ] { Effect* e = fx(); return e ? e->depth : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->depth = Saturate( x ); } );
	if( fits( FxKind::Dimmer ) )
		add( b + "/openshutter", L( "Shutter öffnen" ), ParamType::Bool, 0, 1, [ fx ] { Effect* e = fx(); return e && e->openShutter ? 1.0f : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->openShutter = x > 0.5f; } );
		add( b + "/scale", L( "Skalieren" ), ParamType::Bool, 0, 1, [ fx ] { Effect* e = fx(); return e && e->scale ? 1.0f : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->scale = x > 0.5f; } );
	if( fits( FxKind::Dimmer ) || fits( FxKind::Attribute ) || fits( FxKind::Modulator ) )
		add( b + "/wave", L( "Wellenform (0 Sinus, 1 Dreieck, 2 Säge auf, 3 Säge ab, 4 Rechteck, 5 Puls, 6 Zufall, 7 Zufall weich)" ), ParamType::Int, 0, 7, [ fx ] { Effect* e = fx(); return e ? (float)(int)e->wave : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->wave = (FxWave)Clamp( (int)lroundf( x ), 0, 7 ); } );
	if( fits( FxKind::Position ) )
		add( b + "/shape", L( "Form (0 Kreis, 1 Acht, 2 Pan, 3 Tilt, 4 Dreieck, 5 Viereck, 6 Zufall)" ), ParamType::Int, 0, 6, [ fx ] { Effect* e = fx(); return e ? (float)(int)e->shape : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->shape = (FxShape)Clamp( (int)lroundf( x ), 0, 6 ); } );
	if( fits( FxKind::Color ) )
		add( b + "/colormode", L( "Modus (0 Regenbogen, 1 Verlauf, 2 Stufen)" ), ParamType::Int, 0, 2, [ fx ] { Effect* e = fx(); return e ? (float)(int)e->colorMode : 0.0f; }, [ fx ]( float x ) { if( Effect* e = fx() ) e->colorMode = (FxColorMode)Clamp( (int)lroundf( x ), 0, 2 ); } );
	if( fits( FxKind::Attribute ) )
	{
		const int maxIdx = any ? 12 : std::max( 0, (int)EffectAttrs( *s, cur ).size() - 1 );
		add( b + "/attr", L( "Attribut (Nummer in der Liste der Lampen-Attribute)" ), ParamType::Int, 0, (float)maxIdx,
		     [ s, fx ] { Effect* e = fx(); const auto l = EffectAttrs( *s, e ); for( size_t i = 0; i < l.size(); ++i ) if( e && l[ i ] == e->attr ) return (float)i; return 0.0f; },
		     [ s, fx ]( float x ) { Effect* e = fx(); const auto l = EffectAttrs( *s, e ); const int i = Clamp( (int)lroundf( x ), 0, std::max( 0, (int)l.size() - 1 ) ); if( e && i < (int)l.size() ) e->attr = l[ i ]; } );
	}
	if( fits( FxKind::Color ) || any )
	{
		const int nCol = any ? 8 : ( cur ? (int)cur->colors.size() : 0 );
		for( int k = 0; k < nCol; ++k )
		{
			const std::string cb = b + "/color/" + std::to_string( k + 1 ), cl = L( "Farbe " ) + std::to_string( k + 1 );
			static const char* kPart[ 6 ] = { "hue", "sat", "bright", "r", "g", "b" };
			static const char* kPartLabel[ 6 ] = { "Farbton", "Sättigung", "Helligkeit", "Rot", "Grün", "Blau" };
			for( int c = 0; c < 6; ++c )
				add( cb + "/" + kPart[ c ], cl + " " + Ls( kPartLabel[ c ] ), ParamType::Float, 0, 1,
				     [ fx, k, c ] { return ColorPart( fx(), k, c ); }, [ fx, k, c ]( float x ) { SetColorPart( fx(), k, c, x ); } );
		}
	}
}

struct ParamCache
{
	const Show* show = nullptr;
	std::string sig;
	std::vector< Param > list;
	std::map< std::string, size_t > index;
};
static ParamCache& Cache( Show& s )
{
	static ParamCache c;
	std::string sig = Signature( s );
	if( c.show != &s || sig != c.sig )
	{
		c.show = &s;
		c.sig  = std::move( sig );
		Build( s, c.list );
		c.index.clear();
		for( size_t i = 0; i < c.list.size(); ++i ) c.index[ c.list[ i ].addr ] = i;
	}
	return c;
}
const std::vector< Param >& Params( Show& show ) { return Cache( show ).list; }
const Param* FindParam( Show& show, const std::string& addr )
{
	ParamCache& c = Cache( show );
	auto it       = c.index.find( ToLower( addr ) );
	return it == c.index.end() ? nullptr : &c.list[ it->second ];
}
bool SetParam( Show& show, const std::string& addr, float value )
{
	const Param* p = FindParam( show, addr );
	if( !p ) return false;
	if( p->type == ParamType::Bool ) value = value > 0.5f ? 1.0f : 0.0f;
	p->set( value );
	return true;
}
bool SetParamNorm( Show& show, const std::string& addr, float v01 )
{
	const Param* p = FindParam( show, addr );
	if( !p ) return false;
	if( p->type == ParamType::Bool || p->type == ParamType::Trigger ) p->set( v01 > 0.5f ? 1.0f : 0.0f );
	else if( p->type == ParamType::Int ) p->set( (float)lroundf( Lerp( p->min, p->max, Saturate( v01 ) ) ) );
	else p->set( Lerp( p->min, p->max, Saturate( v01 ) ) );
	return true;
}
std::string ParamRange( const Param& p )
{
	char b[ 64 ];
	switch( p.type )
	{
	case ParamType::Bool: return L( "0 / 1 (aus / an)" );
	case ParamType::Trigger: return L( "Auslöser (1)" );
	case ParamType::Int: snprintf( b, sizeof( b ), "%d .. %d", (int)p.min, (int)p.max ); return b;
	default: break;
	}
	auto f = []( float v ) {
		char t[ 24 ];
		snprintf( t, sizeof( t ), fabsf( v - roundf( v ) ) < 1e-4f ? "%.0f" : ( fabsf( v ) < 0.1f ? "%.4g" : "%.2f" ), v );
		return std::string( t );
	};
	return f( p.min ) + " .. " + f( p.max );
}
}//namespace ls
