// --selftest: automated checks of library, simulation, DMX I/O, MIDI, show files and rendering
#include "app.h"
#include "audio.h"
#include "json.h"
#include "xmlzip.h"
#include "platform.h"
#include "../third_party/stb_image_write.h"
#include "netsock.h"
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <set>
namespace ls
{
static FILE* g_out = nullptr;
static int g_fail = 0, g_pass = 0;
static void T( bool ok, const char* fmt, ... )
{
	char b[ 1024 ];
	va_list a;
	va_start( a, fmt );
	vsnprintf( b, sizeof( b ), fmt, a );
	va_end( a );
	fprintf( g_out, "[%s] %s\n", ok ? " OK " : "FAIL", b );
	fflush( g_out );
	( ok ? g_pass : g_fail )++;
}
static void Info( const char* fmt, ... )
{
	char b[ 1024 ];
	va_list a;
	va_start( a, fmt );
	vsnprintf( b, sizeof( b ), fmt, a );
	va_end( a );
	fprintf( g_out, "       %s\n", b );
	fflush( g_out );
}
static void Run( App& app, float seconds )
{
	int n = (int)( seconds * 60 );
	for( int i = 0; i < n; ++i ) Simulate( app.show, app.sim, 1.0f / 60.0f );
}
static bool SavePng( App& app, const std::string& path, int w, int h, float* avgLum = nullptr, float* bright = nullptr )
{
	app.RenderViewport( w, h );
	std::vector< uint8_t > px;
	int W, H;
	if( !app.ren.ReadOutput( px, W, H ) ) return false;
	double sum = 0;
	int br     = 0;
	for( size_t i = 0; i < px.size(); i += 4 )
	{
		px[ i + 3 ] = 255;
		float l     = 0.2126f * px[ i ] + 0.7152f * px[ i + 1 ] + 0.0722f * px[ i + 2 ];
		sum += l;
		if( l > 200 ) ++br;
	}
	if( avgLum ) *avgLum = (float)( sum / ( px.size() / 4 ) );
	if( bright ) *bright = (float)br / ( px.size() / 4 );
	return stbi_write_png( path.c_str(), W, H, 4, px.data(), W * 4 ) != 0;
}

int RunSelfTest( App& app, const std::string& dir )
{
	std::string logPath = dir + "\\selftest.log";
	g_out               = fopen( logPath.c_str(), "w" );
	if( !g_out ) return 2;
	fprintf( g_out, "LightStage self test\n====================\n" );
	Show& s = app.show;

	//---- library
	T( app.lib.defs.size() > 600, "Bibliothek: %d Lampen geladen", (int)app.lib.defs.size() );
	int generic = 0;
	for( auto& d : app.lib.defs )
		if( d->id.rfind( "generic/", 0 ) == 0 ) ++generic;
	T( generic >= 12, "generische Lampen: %d", generic );
	const FixtureDef* aura = app.lib.Find( "ofl/martin/mac-aura" );
	T( aura && aura->kind == Kind::MovingHead && aura->modes.size() >= 2, "OFL Martin MAC Aura: Moving Head, %d Modes", aura ? (int)aura->modes.size() : 0 );
	const FixtureDef* pxbar = app.lib.Find( "ofl/showtec/pixel-bar-12-mkii" );
	T( pxbar && pxbar->Heads() == 12, "OFL Pixelbar: %d Pixel", pxbar ? pxbar->Heads() : 0 );
	T( Gobos().layers.size() >= 20, "Gobo-Bank: %d Gobos", (int)Gobos().layers.size() );

	//---- demo show
	app.BuildDemo();
	T( s.fixtures.size() >= 30 && s.fixtures.size() <= 40, "Demo-Show: %d Lampen, %d Universes", (int)s.fixtures.size(), (int)s.universes.size() );
	bool clash = false;
	for( auto& f : s.fixtures )
		if( s.Overlaps( f.universe, f.address, f.Footprint(), f.id ) || f.address + f.Footprint() - 1 > 512 ) clash = true;
	T( !clash, "Demo-Patch ohne Adresskonflikte" );
	Run( app, 4.0f );

	//---- simulation: PAR colour
	{
		FixtureInst* par = nullptr;
		for( auto& f : s.fixtures )
			if( f.name == "PAR 1" ) par = &f;
		if( par )
		{
			for( Attr a : { Attr::Red, Attr::Green, Attr::Blue, Attr::White, Attr::Amber, Attr::UV } ) s.SetAttr( *par, a, 0 );
			s.SetAttr( *par, Attr::Dimmer, 1 );
			s.SetAttr( *par, Attr::Red, 1 );
			Run( app, 0.1f );
			T( par->st.color.x > 0.9f && par->st.color.y < 0.1f && par->st.intensity > 0.3f, "PAR rot: Farbe (%.2f %.2f %.2f) Intensität %.2f", par->st.color.x, par->st.color.y, par->st.color.z, par->st.intensity );
			s.SetAttr( *par, Attr::Dimmer, 0.5f );
			Run( app, 0.1f );
			float full = 0;
			s.SetAttr( *par, Attr::Dimmer, 1 );
			Run( app, 0.1f );
			full = par->st.intensity;
			s.SetAttr( *par, Attr::Dimmer, 0.5f );
			Run( app, 0.1f );
			float half = par->st.intensity / std::max( full, 1e-4f );
			T( half > 0.2f && half < 0.4f, "PAR Dimmer 50%%: relative Lichtleistung %.2f (Dimmerkurve)", half );
			s.SetAttr( *par, Attr::Dimmer, 1 );
			//strobe
			int slot = -1;
			for( int i = 0; i < par->Footprint(); ++i )
				if( par->M()->ch[ i ].attr == Attr::Shutter ) slot = i;
			s.Set( *par, slot, 180 );
			int flips = 0;
			bool last = false;
			for( int i = 0; i < 120; ++i )
			{
				Simulate( s, app.sim, 1.0f / 60 );
				bool on = par->st.intensity > 0.05f;
				if( on != last ) ++flips;
				last = on;
			}
			T( flips >= 10, "Strobe: %d Wechsel in 2 s", flips );
			s.Set( *par, slot, 0 );
		}
		else
			T( false, "PAR 1 nicht gefunden" );
	}
	//---- moving head aim
	{
		FixtureInst* sp = nullptr;
		for( auto& f : s.fixtures )
			if( f.name == "Spot 2" ) sp = &f;
		if( sp )
		{
			V3 tgt( 3.0f, 1.0f, 2.0f );
			App::AimAt( s, *sp, tgt );
			Run( app, 0.15f );
			V3 o, d, u;
			BeamRay( *sp, -1, o, d, u );
			float early = acosf( Clamp( Dot( d, Norm( tgt - o ) ), -1.0f, 1.0f ) ) / kDeg;
			Run( app, 4.0f );
			BeamRay( *sp, -1, o, d, u );
			float err = acosf( Clamp( Dot( d, Norm( tgt - o ) ), -1.0f, 1.0f ) ) / kDeg;
			T( err < 1.5f, "Moving Head zielt auf Punkt: Fehler %.2f Grad (nach 0.15 s noch %.1f Grad -> Motorträgheit)", err, early );
			T( early > err, "Pan/Tilt-Bewegung ist gedämpft (nicht sofort)" );
			//colour wheel red (slot 2)
			int cw = -1;
			for( int i = 0; i < sp->Footprint(); ++i )
				if( sp->M()->ch[ i ].name == "Color Wheel" ) cw = i;
			s.Set( *sp, cw, 1 * 8 + 4 );
			s.SetAttr( *sp, Attr::Dimmer, 1 );
			Run( app, 0.1f );
			T( sp->st.color.x > 3 * sp->st.color.z, "Farbrad Rot: (%.2f %.2f %.2f)", sp->st.color.x, sp->st.color.y, sp->st.color.z );
			int gw = -1;
			for( int i = 0; i < sp->Footprint(); ++i )
				if( sp->M()->ch[ i ].name == "Gobo Wheel" ) gw = i;
			s.Set( *sp, gw, 2 * 8 + 4 );
			Run( app, 0.05f );
			T( sp->st.gobo > 0, "Goborad: Gobo-Layer %d", sp->st.gobo );
		}
	}
	//---- pixel bar
	{
		FixtureInst* b = nullptr;
		for( auto& f : s.fixtures )
			if( f.name == "LED Bar 1" ) b = &f;
		if( b )
		{
			for( int h = 0; h < 8; ++h )
				for( Attr a : { Attr::Red, Attr::Green, Attr::Blue } ) s.SetAttr( *b, a, 0, h );
			s.SetAttr( *b, Attr::Green, 1, 2 );
			Run( app, 0.05f );
			T( b->st.heads.size() == 8 && b->st.heads[ 2 ].y > 0.5f && b->st.heads[ 0 ].y < 0.01f && b->st.heads[ 2 ].x < 0.05f, "Pixelbar: Pixel 3 grün (%.2f), Pixel 1 aus (%.2f)", b->st.heads.size() > 2 ? b->st.heads[ 2 ].y : -1.0f, b->st.heads.size() ? b->st.heads[ 0 ].y : -1.0f );
		}
	}
	//---- OFL fixture through the whole chain
	if( aura )
	{
		FixtureInst* f = app.AddFixture( aura, 1, 3, 1, V3( 0, 5, 0 ), V3( 180, 0, 0 ), "Aura Test" );
		s.Home( *f );
		s.SetAttr( *f, Attr::Dimmer, 1 );
		for( auto& c : f->M()->ch )
			if( c.attr == Attr::Shutter )
				for( auto& cp : c.caps )
					if( cp.k == CapK::ShutterOpen ) { s.SetAttr( *f, Attr::Shutter, ( cp.lo + cp.hi ) * 0.5f / 65535.0f ); break; }
		s.SetAttr( *f, Attr::Blue, 1 );
		Run( app, 0.1f );
		T( f->st.intensity > 0.3f && f->st.color.z > f->st.color.x, "MAC Aura (OFL, %s): blau, Intensität %.2f", aura->modes[ 1 ].name.c_str(), f->st.intensity );
		const FixtureDef* sub = nullptr;
		for( auto& d : app.lib.defs )
			if( d->subtractive && d->kind == Kind::MovingHead ) { sub = d.get(); break; }
		if( sub )
		{
			FixtureInst* g = app.AddFixture( sub, 0, 3, 100, V3( 1, 5, 0 ), V3( 180, 0, 0 ), "CMY Test" );
			s.Home( *g );
			s.SetAttr( *g, Attr::Dimmer, 1 );
			s.SetAttr( *g, Attr::Cyan, 1 );
			Run( app, 0.1f );
			T( g->st.color.x < 0.2f * std::max( g->st.color.y, g->st.color.z ), "CMY-Mischung (%s): Cyan filtert Rot (%.2f %.2f %.2f)", sub->name.c_str(), g->st.color.x, g->st.color.y, g->st.color.z );
			s.fixtures.pop_back();
		}
		s.fixtures.pop_back();
	}

	//---- show file roundtrip
	{
		std::string p = dir + "\\selftest_show.lightstage";
		std::string err;
		bool ok = s.Save( p, &err );
		Show s2;
		bool ok2 = ok && s2.Load( p, app.lib, &err );
		bool same = ok2 && s2.fixtures.size() == s.fixtures.size() && s2.universes.size() == s.universes.size() && s2.snapshots.size() == s.snapshots.size();
		for( size_t i = 0; same && i < s.dmx.size(); ++i ) same = s.dmx[ i ] == s2.dmx[ i ];
		for( size_t i = 0; same && i < s.fixtures.size(); ++i ) same = s.fixtures[ i ].address == s2.fixtures[ i ].address && s.fixtures[ i ].def == s2.fixtures[ i ].def && s.fixtures[ i ].mode == s2.fixtures[ i ].mode;
		T( same, "Show speichern/laden identisch (%s)", err.c_str() );
		DeleteFileA( p.c_str() );
	}

	//---- mounting: lamps follow their truss / stand, free lamps stay, file round trip
	{
		auto byName = [ & ]( const char* n ) -> FixtureInst* { for( auto& f : s.fixtures ) if( f.name == n ) return &f; return nullptr; };
		FixtureInst* sp = byName( "Spot 1" );
		T( sp && sp->mount == MountType::Truss, "Demo: Spot 1 hängt an Traverse %d", sp ? sp->mountIdx + 1 : 0 );
		FixtureInst* sd = byName( "Seite 1" );
		T( sd && sd->mount == MountType::Stand, "Demo: Seitenlicht steht auf Stativ %d", sd ? sd->mountIdx + 1 : 0 );
		FixtureInst* hz = byName( "Hazer" );
		V3 hz0          = hz ? hz->pos : V3();
		if( sp && sp->mount == MountType::Truss )
		{
			s.UpdateMounts();
			V3 p0    = sp->pos;
			Truss& t = s.trusses[ sp->mountIdx ];
			t.a += V3( 0, -1.0f, 0.5f );
			t.b += V3( 0, -1.0f, 0.5f );
			s.UpdateMounts();
			V3 dd = sp->pos - p0;
			T( Len( dd - V3( 0, -1.0f, 0.5f ) ) < 1e-3f, "Traverse 1 m tiefer, 0.5 m nach vorn -> Spot 1 folgt (%.2f %.2f %.2f)", dd.x, dd.y, dd.z );
			//rotate the truss about its centre by 90 degrees: lamp moves on the circle and turns with it
			V3 c0 = ( t.a + t.b ) * 0.5f, ha = t.a - c0;
			V3 sa = t.a, sb = t.b;
			t.a = c0 + V3( -ha.z, ha.y, ha.x );
			t.b = c0 - V3( -ha.z, ha.y, ha.x );
			float yaw0 = sp->rot.y;
			s.UpdateMounts();
			T( fabsf( fabsf( sp->rot.y - yaw0 ) - 90.0f ) < 0.5f, "Traverse um 90 Grad gedreht -> Lampe dreht mit (%.1f Grad)", sp->rot.y - yaw0 );
			t.a = sa - V3( 0, -1.0f, 0.5f );
			t.b = sb - V3( 0, -1.0f, 0.5f );
			s.UpdateMounts();
			T( Len( sp->pos - p0 ) < 1e-3f, "Traverse zurück -> Spot 1 wieder am alten Platz" );
		}
		if( hz ) T( hz->mount == MountType::Free && Len( hz->pos - hz0 ) < 1e-5f, "Freie Lampe (Hazer) bleibt unberührt" );
		FixtureInst* pr = byName( "PAR 1" );
		if( pr )
		{
			Stand st;
			st.pos    = V3( 3, 0, 5 );
			st.height = 2.0f;
			s.stands.push_back( st );
			int si = (int)s.stands.size() - 1;
			V3 prOld = pr->pos, prRot = pr->rot;
			s.PlaceOnStand( { pr }, si );
			s.UpdateMounts();
			T( Len( pr->pos - V3( 3, 2, 5 ) ) < 1e-3f, "PAR 1 auf Stativ gesetzt: (%.2f %.2f %.2f)", pr->pos.x, pr->pos.y, pr->pos.z );
			s.stands[ si ].height = 3.2f;
			s.UpdateMounts();
			T( fabsf( pr->pos.y - 3.2f ) < 1e-3f, "Stativ auf 3.2 m ausgefahren -> Lampe auf %.2f m", pr->pos.y );
			pr->pos.x += 0.3f;//moved by hand on the T-bar: stays attached with the new offset
			s.UpdateMounts();
			s.stands[ si ].pos.x += 1.0f;
			s.UpdateMounts();
			T( fabsf( pr->pos.x - 4.3f ) < 1e-3f && pr->mount == MountType::Stand, "Lampe verschoben, dann Stativ verschoben -> x %.2f (erwartet 4.30)", pr->pos.x );
			pr->beam = 33;
			std::string p = dir + "\\selftest_mount.lightstage", err;
			bool ok = s.Save( p, &err );
			Show s2;
			ok = ok && s2.Load( p, app.lib, &err );
			FixtureInst* q = nullptr;
			for( auto& f : s2.fixtures ) if( f.name == "PAR 1" ) q = &f;
			if( ok && q ) s2.UpdateMounts();
			T( ok && q && q->mount == MountType::Stand && q->mountIdx == si && fabsf( q->beam - 33 ) < 0.01f && s2.stands.size() == s.stands.size() && Len( q->pos - pr->pos ) < 1e-3f,
			   "Befestigung, Stative und Abstrahlwinkel in der Show-Datei gespeichert %s", err.c_str() );
			DeleteFileA( p.c_str() );
			V3 before = pr->pos;
			s.RemoveStand( si );
			s.UpdateMounts();
			T( pr->mount == MountType::Free && Len( pr->pos - before ) < 1e-5f, "Stativ gelöscht -> Lampe bleibt frei an ihrem Platz" );
			//beam angle override for a lamp without zoom channel
			s.SetAttr( *pr, Attr::Dimmer, 1 );
			pr->beam = 12;
			Run( app, 1.0f );
			float z12 = pr->st.zoom;
			pr->beam  = 60;
			Run( app, 1.0f );
			float z60 = pr->st.zoom;
			pr->beam  = 0;
			Run( app, 1.0f );
			T( fabsf( z12 - 12 ) < 0.3f && fabsf( z60 - 60 ) < 0.6f && fabsf( pr->st.zoom - pr->def->beamMin ) < 0.6f, "Abstrahlwinkel PAR: 12 -> %.1f, 60 -> %.1f, Profil -> %.1f Grad", z12, z60, pr->st.zoom );
			pr->pos = prOld;
			pr->rot = prRot;
		}
	}

	//---- stand follows its lamp, duplicate / delete with own stand, new stands
	{
		Show& s          = app.show;
		FixtureInst* sd = nullptr;
		for( auto& f : s.fixtures )
			if( f.name == "Seite 1" ) sd = &f;
		T( sd && sd->mount == MountType::Stand, "Seite 1 steht auf einem Stativ" );
		if( sd && sd->mount == MountType::Stand )
		{
			s.UpdateMounts();
			const int si    = sd->mountIdx;
			const Stand st0 = s.stands[ si ];
			const V3 p0     = sd->pos;
			sd->pos += V3( 0.5f, 0.3f, -0.4f );
			s.UpdateMounts();
			const Stand& st = s.stands[ si ];
			T( fabsf( st.pos.x - st0.pos.x - 0.5f ) < 1e-3f && fabsf( st.pos.z - st0.pos.z + 0.4f ) < 1e-3f && fabsf( st.height - st0.height - 0.3f ) < 1e-3f && sd->mount == MountType::Stand && Len( sd->pos - p0 - V3( 0.5f, 0.3f, -0.4f ) ) < 1e-3f,
			   "Lampe bewegt -> Stativ wandert mit (dx %.2f, dz %.2f, Höhe %+.2f)", st.pos.x - st0.pos.x, st.pos.z - st0.pos.z, st.height - st0.height );
			sd->pos = p0;
			s.UpdateMounts();
			T( Len( s.stands[ si ].pos - st0.pos ) < 1e-3f && fabsf( s.stands[ si ].height - st0.height ) < 1e-3f, "Lampe zurück -> Stativ wieder am alten Platz" );
			const size_t ns = s.stands.size(), nf = s.fixtures.size();
			const int sdId  = sd->id;
			app.sel         = { sdId };
			app.DuplicateSelected();
			FixtureInst* dup = app.sel.size() == 1 ? s.FindFixture( app.sel[ 0 ] ) : nullptr;
			sd               = s.FindFixture( sdId );
			T( dup && sd && s.stands.size() == ns + 1 && s.fixtures.size() == nf + 1 && dup->mount == MountType::Stand && dup->mountIdx == (int)ns && sd->mountIdx == si,
			   "Duplizieren -> Kopie bekommt ein eigenes Stativ (%d -> %d Stative)", (int)ns, (int)s.stands.size() );
			if( dup && sd && s.stands.size() == ns + 1 )
			{
				Stand &a = s.stands[ si ], &b = s.stands[ ns ];
				T( fabsf( b.height - a.height ) < 1e-4f && fabsf( b.yaw - a.yaw ) < 1e-4f && Len( ( dup->pos - sd->pos ) - ( b.pos - a.pos ) ) < 1e-3f && fabsf( dup->rot.y - sd->rot.y ) < 1e-3f && fabsf( dup->beam - sd->beam ) < 1e-4f,
				   "Kopie: gleiche Höhe, Ausrichtung und Abstrahlwinkel" );
				b.pos.x += 1.0f;
				V3 dp = dup->pos, sp = sd->pos;
				s.UpdateMounts();
				T( fabsf( dup->pos.x - dp.x - 1.0f ) < 1e-3f && Len( sd->pos - sp ) < 1e-5f, "Kopie-Stativ verschoben -> nur die Kopie bewegt sich" );
			}
			app.DeleteSelected();
			T( s.stands.size() == ns && s.fixtures.size() == nf && s.FindFixture( sdId ) && s.FindFixture( sdId )->mountIdx == si, "Kopie gelöscht -> ihr leeres Stativ ist mit weg" );
		}
		const size_t n0 = s.stands.size();
		const int a = s.AddStand(), b = s.AddStand();
		bool apart = Len( s.stands[ a ].pos - s.stands[ b ].pos ) > 0.5f;
		for( size_t i = 0; i < n0; ++i ) apart = apart && Len( s.stands[ i ].pos - s.stands[ a ].pos ) > 0.5f && Len( s.stands[ i ].pos - s.stands[ b ].pos ) > 0.5f;
		T( (int)s.stands.size() == (int)n0 + 2 && apart, "Neue Stative (je Lampe) stehen jeweils an einem freien Platz" );
		s.RemoveStand( b );
		s.RemoveStand( a );
	}

	//---- textured models (glTF with PNG, OBJ with map_Kd)
	{
		std::string td = dir + "\\selftest_tex";
		CreateDirectoryA( td.c_str(), nullptr );
		uint8_t img[ 8 * 8 * 4 ];
		for( int y = 0; y < 8; ++y )
			for( int x = 0; x < 8; ++x )
			{
				uint8_t* q = &img[ ( y * 8 + x ) * 4 ];
				bool top   = y < 4;//image top = red, bottom = blue
				q[ 0 ]     = top ? 230 : 20;
				q[ 1 ]     = 20;
				q[ 2 ]     = top ? 20 : 230;
				q[ 3 ]     = 255;
			}
		stbi_write_png( ( td + "\\tex.png" ).c_str(), 8, 8, 4, img, 32 );
		//quad 2 x 2 m facing +Z, bottom edge on y = 0
		float pos[ 12 ] = { -1, 0, 0, 1, 0, 0, 1, 2, 0, -1, 2, 0 }, nrm[ 12 ] = { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1 }, uv[ 8 ] = { 0, 1, 1, 1, 1, 0, 0, 0 };
		uint16_t ix[ 6 ] = { 0, 1, 2, 0, 2, 3 };
		std::vector< uint8_t > bin;
		auto put = [ & ]( const void* p, size_t n ) { bin.insert( bin.end(), (const uint8_t*)p, (const uint8_t*)p + n ); };
		put( pos, 48 );
		put( nrm, 48 );
		put( uv, 32 );
		put( ix, 12 );
		FILE* fb = fopen( ( td + "\\quad.bin" ).c_str(), "wb" );
		if( fb ) { fwrite( bin.data(), 1, bin.size(), fb ); fclose( fb ); }
		WriteText( td + "\\quad.gltf", R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}],
"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicFactor":0}}],
"textures":[{"source":0}],"images":[{"uri":"tex.png"}],
"buffers":[{"uri":"quad.bin","byteLength":140}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":48},{"buffer":0,"byteOffset":96,"byteLength":32},{"buffer":0,"byteOffset":128,"byteLength":12}],
"accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[-1,0,0],"max":[1,2,0]},{"bufferView":1,"componentType":5126,"count":4,"type":"VEC3"},
{"bufferView":2,"componentType":5126,"count":4,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":6,"type":"SCALAR"}]})" );
		WriteText( td + "\\quad.mtl", "newmtl m\nKd 0.8 0.8 0.8\nmap_Kd tex.png\n" );
		WriteText( td + "\\quad.obj", "mtllib quad.mtl\nv -1 0 0\nv 1 0 0\nv 1 2 0\nv -1 2 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\nusemtl m\nf 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n" );
		auto topT = []( const MeshData& m ) {//texture t of the highest vertex (expected 0 = first image row = red)
			float best = -1e9f, t = -1;
			for( uint32_t k = 0; k < m.Count(); ++k )
				if( m.v[ k * MeshData::kStride + 1 ] > best ) { best = m.v[ k * MeshData::kStride + 1 ]; t = m.v[ k * MeshData::kStride + 10 ]; }
			return t;
		};
		MeshData g, o;
		std::string e1, e2;
		bool okG = LoadModel( td + "\\quad.gltf", g, &e1 );
		bool okO = LoadModel( td + "\\quad.obj", o, &e2 );
		T( okG && g.images.size() == 1 && g.Textured() == 1 && g.images[ 0 ].w == 8 && fabsf( topT( g ) ) < 1e-4f, "glTF mit PNG-Textur: %d Bild(er), %d texturierte Teile, oben t=%.2f %s", (int)g.images.size(), g.Textured(), topT( g ), e1.c_str() );
		T( okO && o.images.size() == 1 && o.Textured() == 1 && fabsf( topT( o ) ) < 1e-4f, "OBJ mit map_Kd-Textur: %d Bild(er), oben t=%.2f %s", (int)o.images.size(), topT( o ), e2.c_str() );
		//rendered: the upper half of the quad shows red, the lower half blue
		ModelObj mo;
		mo.path  = td + "\\quad.gltf";
		mo.name  = "quad";
		mo.pos   = V3( 0, 1.2f, 6.0f );
		mo.scale = 1.0f;
		s.models.push_back( mo );
		Camera camOld   = app.cam;
		float ambOld    = s.stage.ambient, masterOld = s.master;
		s.stage.ambient = 1.5f;
		s.master        = 0;//no beams in front of the test picture
		Run( app, 0.5f );
		app.cam.target  = V3( 0, 2.2f, 6.0f );
		app.cam.yaw     = 0;
		app.cam.pitch   = 0;
		app.cam.dist    = 4;
		app.cam.fov     = 50;
		SavePng( app, dir + "\\selftest_textured.png", 640, 360 );
		std::vector< uint8_t > px;
		int W = 0, H = 0;
		app.ren.ReadOutput( px, W, H );
		auto at = [ & ]( float fx, float fy, int c ) { int x = (int)( fx * W ), y = (int)( fy * H ); return (int)px[ ( (size_t)y * W + x ) * 4 + c ]; };
		int rt = at( 0.5f, 0.35f, 0 ), bt = at( 0.5f, 0.35f, 2 ), rb = at( 0.5f, 0.65f, 0 ), bb = at( 0.5f, 0.65f, 2 );
		T( s.models.back().textures == 1 && rt > bt + 40 && bb > rb + 40, "Textur im Bild: oben rot (R %d / B %d), unten blau (R %d / B %d)", rt, bt, rb, bb );
		s.models.pop_back();
		s.stage.ambient = ambOld;
		s.master        = masterOld;
		Run( app, 0.5f );
		app.cam = camOld;
	}

	//---- video projectors + screens
	{
		const size_t np0 = s.projectors.size(), ns0 = s.screens.size();
		s.AddScreen();
		const int si = (int)s.screens.size() - 1;
		s.AddProjector();
		const int pi    = (int)s.projectors.size() - 1;
		ScreenGeo sg    = GetScreenGeo( s.screens[ si ] );
		ProjectorGeo pg = GetProjectorGeo( s.projectors[ pi ] );
		float tdist = s.ThrowDistance( pg ), dc = Len( sg.centre - pg.o );
		T( fabsf( tdist - dc ) < 0.05f, "Neuer Projektor zielt auf die Leinwand (Wurfweite %.2f m, Mitte %.2f m)", tdist, dc );
		//all four picture corners land on the screen, at least one touches its edge
		auto onScreen = [ & ]( const ProjectorGeo& g, float& maxRel ) {
			const float cx[ 4 ] = { g.l, g.r, g.r, g.l }, cy[ 4 ] = { g.b, g.b, g.t, g.t };
			maxRel              = 0;
			for( int k = 0; k < 4; ++k )
			{
				V3 d    = Norm( g.right * cx[ k ] + g.up * cy[ k ] + g.dir );
				float t = Dot( sg.centre - g.o, sg.normal ) / Dot( d, sg.normal );
				V3 q    = g.o + d * t - sg.centre;
				maxRel  = std::max( { maxRel, fabsf( Dot( q, sg.right ) ) / ( s.screens[ si ].width * 0.5f ), fabsf( Dot( q, sg.up ) ) / ( s.screens[ si ].height * 0.5f ) } );
			}
			return maxRel <= 1.002f;
		};
		float rel = 0;
		bool fit  = onScreen( pg, rel );
		T( fit && rel > 0.98f, "Zoom angepasst: Bild füllt die Leinwand (Rand %.3f)", rel );
		Projector fr = s.projectors[ pi ];
		s.AimProjector( fr, s.screens[ si ], true );
		ProjectorGeo fg = GetProjectorGeo( fr );
		float pw        = s.ThrowDistance( fg ) * ( fg.r - fg.l );
		T( fabsf( pw - s.screens[ si ].width ) < 0.03f && fabsf( Dot( fg.dir, sg.normal ) + 1 ) < 1e-3f, "Frontal davor: Bildbreite %.2f m = Leinwand %.2f m", pw, s.screens[ si ].width );
		//render: projector off / on, orientation markers of the test card
		Camera camOld   = app.cam;
		float masterOld = s.master;
		s.master        = 0;//rig dark: only the projector
		Run( app, 0.5f );
		app.cam.target = sg.centre;
		app.cam.yaw    = 0;
		app.cam.pitch  = 4;
		app.cam.dist   = 13;
		app.cam.fov    = 45;
		auto pixelAt = [ & ]( const V3& wp, int& r, int& g, int& b ) {
			std::vector< uint8_t > px;
			int W = 0, H = 0;
			app.ren.ReadOutput( px, W, H );
			V3 n = XformP( app.ren.viewProj, wp );
			float w4 = app.ren.viewProj( 3, 0 ) * wp.x + app.ren.viewProj( 3, 1 ) * wp.y + app.ren.viewProj( 3, 2 ) * wp.z + app.ren.viewProj( 3, 3 );
			int x = (int)( ( n.x / w4 * 0.5f + 0.5f ) * W ), y = (int)( ( 0.5f - n.y / w4 * 0.5f ) * H );
			r = g = b = 0;
			int c = 0;
			for( int dy = -3; dy <= 3; ++dy )
				for( int dx = -3; dx <= 3; ++dx )
				{
					int xx = Clamp( x + dx, 0, W - 1 ), yy = Clamp( y + dy, 0, H - 1 );
					const uint8_t* q = &px[ ( (size_t)yy * W + xx ) * 4 ];
					r += q[ 0 ];
					g += q[ 1 ];
					b += q[ 2 ];
					++c;
				}
			r /= c;
			g /= c;
			b /= c;
		};
		//point on the screen where a given picture position (u right, v up, 0..1) lands
		auto picPoint = [ & ]( float u, float v ) {
			ProjectorGeo g = GetProjectorGeo( s.projectors[ pi ] );
			V3 d    = Norm( g.right * Lerp( g.l, g.r, u ) + g.up * Lerp( g.b, g.t, v ) + g.dir );
			float t = Dot( sg.centre - g.o, sg.normal ) / Dot( d, sg.normal );
			return g.o + d * t;
		};
		s.projectors[ pi ].on = false;
		s.stage.beams         = false;//colour checks without haze in front of the screen
		SavePng( app, dir + "\\selftest_projector_off.png", 960, 540 );
		int r0, g0, b0;
		pixelAt( sg.centre + sg.up * 0.6f, r0, g0, b0 );
		s.projectors[ pi ].on = true;
		app.RenderViewport( 960, 540 );//first frame of the test card
		SavePng( app, dir + "\\selftest_projector_nohaze.png", 1280, 720 );
		int r1, g1, b1, rr, rg, rb, gr, gg, gbb;
		pixelAt( sg.centre + sg.up * 0.6f, r1, g1, b1 );
		pixelAt( picPoint( 0.04f, 0.92f ), rr, rg, rb );//red marker top left
		pixelAt( picPoint( 0.96f, 0.92f ), gr, gg, gbb );//green marker top right
		T( app.ren.ProjectorsLit() >= 1 && r1 + g1 + b1 > r0 + g0 + b0 + 60, "Projektor (Testbild) beleuchtet die Leinwand: %d -> %d", r0 + g0 + b0, r1 + g1 + b1 );
		T( rr > rg + 40 && rr > rb + 40 && gg > gr + 40 && gg > gbb + 40, "Bild richtig herum: oben links rot (%d,%d,%d), oben rechts grün (%d,%d,%d)", rr, rg, rb, gr, gg, gbb );
		//mirrored for rear projection
		s.projectors[ pi ].rear = true;
		app.RenderViewport( 960, 540 );
		pixelAt( picPoint( 0.04f, 0.92f ), rr, rg, rb );
		T( rg > rr + 40, "Rückprojektion spiegelt das Bild (oben links jetzt grün: %d,%d,%d)", rr, rg, rb );
		s.projectors[ pi ].rear = false;
		//---- screen shape from a black/white image: a ring, the hole lets the light through
		{
			Screen& sc       = s.screens[ si ];
			const Screen old = sc;
			auto ringPng     = [ & ]( const std::string& p, int w, int h ) {
				std::vector< uint8_t > im( (size_t)w * h * 3, 0 );
				const int side = std::min( w, h );
				for( int y = 0; y < h; ++y )
					for( int x = 0; x < w; ++x )
					{
						const float dx = ( x + 0.5f - w * 0.5f ) / side, dy = ( y + 0.5f - h * 0.5f ) / side;
						const float r  = sqrtf( dx * dx + dy * dy );
						if( r > 0.25f && r < 0.48f ) memset( &im[ ( (size_t)y * w + x ) * 3 ], 255, 3 );
					}
				return stbi_write_png( p.c_str(), w, h, 3, im.data(), w * 3 ) != 0;
			};
			const std::string ring = dir + "\\selftest_ring.png", wide = dir + "\\selftest_ring_wide.png";
			ringPng( ring, 400, 400 );
			ringPng( wide, 600, 400 );
			std::string err, warn;
			bool ok = LoadScreenMask( sc, ring, err, &warn );
			T( ok && sc.maskN == kScreenMaskN && !sc.maskRects.empty() && warn.empty(), "Leinwandform aus Bild: %d x %d Raster, %d Flächen (%s)", sc.maskN, sc.maskN, (int)sc.maskRects.size(), err.c_str() );
			T( !sc.MaskAt( 0.5f, 0.5f ) && sc.MaskAt( 0.5f + 0.36f, 0.5f ) && sc.MaskAt( 0.5f, 0.5f - 0.36f ) && !sc.MaskAt( 0.03f, 0.03f ), "Form: Mitte frei, Ring weiß, Ecke frei" );
			//the merged rectangles cover exactly the white cells
			{
				double area = 0;
				for( auto& r : sc.maskRects ) area += ( r.x1 - r.x0 ) * ( r.y1 - r.y0 );
				size_t cells = 0;
				for( uint8_t c : sc.mask ) cells += c;
				const double ca = (double)cells / ( (double)sc.maskN * sc.maskN );
				T( fabs( area - ca ) < 1e-6, "Form: Rechtecke = weiße Zellen (%.4f / %.4f)", area, ca );
			}
			const std::string str = ScreenMaskToString( sc );
			Screen cp;
			T( ScreenMaskFromString( cp, sc.maskN, str ) && cp.mask == sc.mask && cp.maskRects.size() == sc.maskRects.size(), "Form als Text (%d Zeichen) und zurück", (int)str.size() );
			//projector ray through the hole: no hit on the screen any more
			{
				ProjectorGeo g = GetProjectorGeo( s.projectors[ pi ] );
				const float tFull = Len( sg.centre - g.o );
				const float tHole = s.ThrowDistance( g );
				T( tHole > tFull + 0.5f, "Projektor trifft durch das Loch dahinter (%.2f m statt %.2f m)", tHole, tFull );
			}
			//render: centre of the hole dark (no back wall), the ring lit like the full screen
			const bool bw    = s.stage.backWall;
			s.stage.backWall = false;
			int fr, fg2, fb, hr, hg, hb, qr, qg, qb, q2r, q2g, q2b;
			const V3 ringPt  = sg.centre + sg.right * ( 0.36f * sc.width );
			Screen shaped    = sc;
			sc               = old;//full screen first
			app.RenderViewport( 960, 540 );
			pixelAt( sg.centre, fr, fg2, fb );
			pixelAt( ringPt, qr, qg, qb );
			sc = shaped;
			app.RenderViewport( 960, 540 );
			pixelAt( sg.centre, hr, hg, hb );
			pixelAt( ringPt, q2r, q2g, q2b );
			SavePng( app, dir + "\\selftest_screen_shape.png", 1280, 720 );
			const int full = fr + fg2 + fb, hole = hr + hg + hb, ringF = qr + qg + qb, ringS = q2r + q2g + q2b;
			T( full > hole + 90 && ringS > hole + 90 && abs( ringS - ringF ) < std::max( 40, ringF / 5 ), "Geformte Leinwand gerendert: Loch %d (voll %d), Ring %d (voll %d)", hole, full, ringS, ringF );
			//saved with the show
			{
				const std::string p = dir + "\\selftest_shape.lightstage";
				std::string e2;
				T( s.Save( p, &e2 ), "Show mit geformter Leinwand gespeichert" );
				Show back;
				T( back.Load( p, app.lib, &e2 ) && si < (int)back.screens.size() && back.screens[ si ].mask == sc.mask && !back.screens[ si ].maskRects.empty(), "Form nach dem Laden gleich (%s)", e2.c_str() );
			}
			//not square: fitted into the square, warning
			ok = LoadScreenMask( sc, wide, err, &warn );
			T( ok && !warn.empty() && sc.maskBox[ 0 ] > 0.1f && fabsf( ( sc.maskBox[ 2 ] - sc.maskBox[ 0 ] ) - ( sc.maskBox[ 3 ] - sc.maskBox[ 1 ] ) ) < 0.02f, "Bild nicht 1:1: mittig eingepasst, Ring bleibt rund (x %.2f..%.2f, y %.2f..%.2f) + Hinweis", sc.maskBox[ 0 ], sc.maskBox[ 2 ], sc.maskBox[ 1 ], sc.maskBox[ 3 ] );
			ClearScreenMask( sc );
			T( !sc.HasMask() && sc.MaskAt( 0.5f, 0.5f ), "Form entfernt: wieder rechteckig" );
			sc               = old;
			s.stage.backWall = bw;
		}
		s.stage.beams           = true;
		SavePng( app, dir + "\\selftest_projector.png", 1280, 720 );
		//seen from the side: picture on the screen and the light cone in the haze
		{
			Camera c = app.cam;
			app.cam.yaw   = 38;
			app.cam.pitch = 8;
			app.cam.dist  = 17;
			app.cam.target = V3( 0, 2.5f, 2.0f );
			SavePng( app, dir + "\\selftest_projector_side.png", 1280, 720 );
			app.cam = c;
		}
		//several frames with haze in between: the screen must stay lit (instanced geometry intact)
		for( int k = 0; k < 3; ++k ) app.RenderViewport( 640, 360 );
		s.stage.beams = false;
		app.RenderViewport( 960, 540 );
		int r2, g2, b2;
		pixelAt( sg.centre + sg.up * 0.6f, r2, g2, b2 );
		s.stage.beams = true;
		T( r2 + g2 + b2 > r0 + g0 + b0 + 60, "Leinwand bleibt nach mehreren Frames mit Haze beleuchtet (%d)", r2 + g2 + b2 );
		//video file through Media Foundation
		std::string vp = dir + "\\selftest_video";
		bool wrote     = WriteTestVideo( vp, 320, 240, 60 );
		T( wrote, "Testvideo geschrieben: %s", FileName( vp ).c_str() );
		if( wrote )
		{
			Projector& p = s.projectors[ pi ];
			p.input      = VideoInput::File;
			p.file       = vp;
			const VideoFrame* f = nullptr;
			for( int k = 0; k < 300 && !f; ++k )
			{
				app.video.Sync( s.projectors );
				f = app.video.Frame( s.projectors[ pi ].uid );
				if( !f ) Sleep( 20 );
			}
			bool topRed = false, botBlue = false;
			if( f && f->w == 320 && f->h == 240 )
			{
				const uint8_t* t = &f->px[ ( 20 * 320 + 40 ) * 4 ];
				const uint8_t* b = &f->px[ ( 220 * 320 + 40 ) * 4 ];
				int R = f->bgra ? 2 : 0, B = f->bgra ? 0 : 2;
				topRed  = t[ R ] > 180 && t[ B ] < 80;
				botBlue = b[ B ] > 180 && b[ R ] < 80;
			}
			T( f && f->w == 320 && f->h == 240 && topRed && botBlue, "Videodatei: %d x %d, oben rot / unten blau (%s)", f ? f->w : 0, f ? f->h : 0, app.video.Status( s.projectors[ pi ].uid ).c_str() );
			uint64_t s0 = f ? f->serial : 0;
			for( int k = 0; k < 40; ++k )
			{
				app.video.Sync( s.projectors );
				Sleep( 15 );
			}
			f = app.video.Frame( s.projectors[ pi ].uid );
			T( f && f->serial > s0 + 5, "Video läuft (%llu neue Bilder in 0.6 s)", f ? (unsigned long long)( f->serial - s0 ) : 0ULL );
			SavePng( app, dir + "\\selftest_projector_video.png", 1280, 720 );
		}
		//NDI loopback (needs an installed NDI runtime)
		std::string where;
		if( NdiAvailable( &where ) )
		{
			Info( "NDI: %s - Sender starten", where.c_str() );
			NdiTestSender snd;
			bool st = snd.Start( "LightStage Selftest", 320, 180, V3( 0.1f, 0.85f, 0.2f ) );
			Info( "NDI: Sender %s, suche Quellen", st ? "laeuft" : "fehlgeschlagen" );
			std::string found;
			for( int k = 0; k < 120 && st && found.empty(); ++k )
			{
				for( auto& n : NdiSources() )
					if( n == snd.fullName || n.find( "LightStage Selftest" ) != std::string::npos ) found = n;
				if( found.empty() ) Sleep( 100 );
			}
			T( st && !found.empty(), "NDI: Testsender im Netzwerk gefunden (%s)", found.c_str() );
			if( !found.empty() )
			{
				s.projectors[ pi ].input = VideoInput::Ndi;
				s.projectors[ pi ].ndi   = found;
				const VideoFrame* f      = nullptr;
				for( int k = 0; k < 400 && !f; ++k )
				{
					app.video.Sync( s.projectors );
					f = app.video.Frame( s.projectors[ pi ].uid );
					if( !f ) Sleep( 20 );
				}
				T( f && f->w == 320 && f->h == 180 && f->avg.y > f->avg.x * 2 && f->avg.y > f->avg.z * 2, "NDI-Empfang: %d x %d, grün (%.2f %.2f %.2f)", f ? f->w : 0, f ? f->h : 0, f ? f->avg.x : 0.f, f ? f->avg.y : 0.f, f ? f->avg.z : 0.f );
				SavePng( app, dir + "\\selftest_projector_ndi.png", 960, 540 );
			}
			s.projectors[ pi ].input = VideoInput::TestCard;
			app.video.Sync( s.projectors );
			snd.Stop();
		}
		else
			Info( "NDI-Runtime nicht gefunden - NDI-Test übersprungen" );
		//show file
		s.projectors[ pi ].throwRatio = 2.345f;
		s.projectors[ pi ].ndi        = "HOST (Kamera 1)";
		s.screens[ si ].width         = 7.5f;
		s.screens[ si ].mount         = 1;
		std::string p = dir + "\\selftest_video.lightstage", err;
		bool ok       = s.Save( p, &err );
		Show s2;
		ok = ok && s2.Load( p, app.lib, &err );
		T( ok && s2.projectors.size() == s.projectors.size() && s2.screens.size() == s.screens.size() && fabsf( s2.projectors.back().throwRatio - 2.345f ) < 1e-4f && s2.projectors.back().ndi == "HOST (Kamera 1)" &&
		       fabsf( s2.screens.back().width - 7.5f ) < 1e-4f && s2.screens.back().mount == 1,
		   "Projektoren und Leinwände in der Show-Datei gespeichert %s", err.c_str() );
		DeleteFileA( p.c_str() );
		s.projectors.resize( np0 );
		s.screens.resize( ns0 );
		app.video.Sync( s.projectors );
		s.master = masterOld;
		Run( app, 0.5f );
		app.cam = camOld;
	}

	//---- phase A: ambient light separate from the fixtures, master up to 1000 %
	{
		const float m0 = s.master, env0 = s.stage.envLight;
		const V3 ac0   = s.stage.ambientColor;
		FixtureInst* sp = nullptr;
		for( auto& f : s.fixtures )
			if( f.name == "Spot 1" ) sp = &f;
		s.SetAttr( *sp, Attr::Dimmer, 1 );
		s.master = 1;
		Run( app, 0.5f );
		float i1 = sp->st.intensity, h1 = 0, h4 = 0;
		SavePng( app, dir + "\\selftest_master100.png", 640, 360, &h1 );
		s.master = 4;
		Run( app, 0.5f );
		float i4 = sp->st.intensity;
		SavePng( app, dir + "\\selftest_master400.png", 640, 360, &h4 );
		T( h4 > h1 * 1.3f && fabsf( i4 - i1 ) < 1e-3f, "Master 400 %%: Bild %.1f -> %.1f, Lampen-Intensität bleibt %.2f (Leistung statt Dimmer)", h1, h4, i4 );
		s.master = 0.5f;
		Run( app, 0.5f );
		T( i1 > 0.01f && fabsf( sp->st.intensity / i1 - powf( 0.5f, 1.8f ) ) < 0.02f, "Master 50 %%: Dimmkurve unverändert (%.3f, erwartet %.3f)", sp->st.intensity / std::max( 1e-4f, i1 ), powf( 0.5f, 1.8f ) );
		s.master = 0;
		s.stage.envLight = 0;
		Run( app, 0.5f );
		float e0 = 0, e3 = 0;
		SavePng( app, dir + "\\selftest_env0.png", 640, 360, &e0 );
		s.stage.envLight = 3;
		SavePng( app, dir + "\\selftest_env3.png", 640, 360, &e3 );
		T( e0 < 3.0f && e3 > e0 + 1.5f, "Umgebung unabhängig vom Master: 0 -> %.1f, 3 -> %.1f", e0, e3 );
		s.stage.ambientColor = V3( 0.3f, 0.4f, 0.9f );
		SavePng( app, dir + "\\selftest_env_blue.png", 640, 360 );
		std::vector< uint8_t > px;
		int W = 0, H = 0;
		app.ren.ReadOutput( px, W, H );
		long rs = 0, bs = 0;
		for( int y = H * 3 / 4; y < H; ++y )
			for( int x = 0; x < W; ++x ) { rs += px[ ( (size_t)y * W + x ) * 4 ]; bs += px[ ( (size_t)y * W + x ) * 4 + 2 ]; }
		T( bs > rs * 1.3, "Umgebungsfarbe Nachtblau: Boden blau (R %ld / B %ld)", rs, bs );
		s.master = 7.5f;
		std::string p = dir + "\\selftest_env.lightstage", err;
		bool ok = s.Save( p, &err );
		Show s2;
		ok = ok && s2.Load( p, app.lib, &err );
		T( ok && fabsf( s2.master - 7.5f ) < 1e-4f && fabsf( s2.stage.envLight - 3 ) < 1e-4f && fabsf( s2.stage.ambientColor.z - 0.9f ) < 1e-4f, "Master 750 %%, Umgebung und Farbe gespeichert %s", err.c_str() );
		DeleteFileA( p.c_str() );
		OscMsg om;
		om.address = "/ls/master/boost";
		om.f       = { 6.0f };
		om.isInt   = { false };
		bool oa = ApplyOsc( s, om );
		T( oa && fabsf( s.master - 6.0f ) < 1e-4f, "OSC /ls/master/boost 6 -> Master %.0f %%", s.master * 100 );
		s.master             = m0;
		s.stage.envLight     = env0;
		s.stage.ambientColor = ac0;
		Run( app, 0.5f );
	}

	//---- phase C: outdoor grounds, sky, moon
	{
		const StageCfg st0 = s.stage;
		const float m0     = s.master;
		const Camera cam0  = app.cam;
		float hRef = 0;
		SavePng( app, dir + "\\selftest_hall_ref.png", 640, 360, &hRef );
		auto region = [ & ]( int y0, int y1, double& r, double& g, double& b ) {
			std::vector< uint8_t > px;
			int W = 0, H = 0;
			app.ren.ReadOutput( px, W, H );
			r = g = b = 0;
			long n = 0;
			for( int y = H * y0 / 100; y < H * y1 / 100; ++y )
				for( int x = 0; x < W; ++x, ++n )
				{
					const uint8_t* q = &px[ ( (size_t)y * W + x ) * 4 ];
					r += q[ 0 ];
					g += q[ 1 ];
					b += q[ 2 ];
				}
			r /= std::max( 1L, n );
			g /= std::max( 1L, n );
			b /= std::max( 1L, n );
		};
		//night sky: blue gradient + stars, looking up a little
		ApplyMood( s.stage, 1 );
		s.master       = 0;
		Run( app, 0.5f );
		app.cam.target = V3( 0, 9, 0 );
		app.cam.pitch  = -6;
		app.cam.dist   = 18;
		SavePng( app, dir + "\\selftest_outdoor_sky.png", 960, 540 );
		double r, g, b;
		region( 0, 30, r, g, b );
		std::vector< uint8_t > px;
		int W = 0, H = 0;
		app.ren.ReadOutput( px, W, H );
		int starPx = 0;
		for( int y = 0; y < H / 3; ++y )
			for( int x = 0; x < W; ++x )
			{
				const uint8_t* q = &px[ ( (size_t)y * W + x ) * 4 ];
				if( q[ 0 ] + q[ 1 ] + q[ 2 ] > ( r + g + b ) * 3 + 60 ) ++starPx;
			}
		T( b > r * 1.2 && starPx >= 3, "Nachthimmel: blau (R %.1f / B %.1f), %d Sternpixel", r, b, starPx );
		//ground types under white ambient light
		app.cam             = cam0;
		s.stage.envLight    = 3;
		s.stage.ambient     = 0.1f;
		s.stage.ambientColor = V3( 1, 1, 1 );
		s.stage.distanceFog = 0;
		s.stage.moon        = 0;
		s.stage.ground      = 1;
		SavePng( app, dir + "\\selftest_outdoor_grass.png", 960, 540 );
		region( 88, 100, r, g, b );
		T( g > r * 1.25 && g > b * 1.25, "Untergrund Rasen: grün (R %.1f G %.1f B %.1f)", r, g, b );
		s.stage.ground = 2;
		SavePng( app, dir + "\\selftest_outdoor_concrete.png", 960, 540 );
		region( 88, 100, r, g, b );
		T( fabs( r - b ) < std::max( 3.0, g * 0.15 ) && g > 5, "Untergrund Beton: neutral grau (R %.1f G %.1f B %.1f)", r, g, b );
		//moon shadow adds one shadow map
		ApplyMood( s.stage, 1 );
		s.stage.moon = 1;
		s.stage.moonShadow = false;
		app.RenderViewport( 640, 360 );
		int sh0 = app.ren.stats.shadows;
		s.stage.moonShadow = true;
		app.RenderViewport( 640, 360 );
		T( app.ren.stats.shadows == sh0 + 1, "Mondschatten optional: Schatten-Maps %d -> %d", sh0, app.ren.stats.shadows );
		s.master = 1;
		Run( app, 0.5f );
		SavePng( app, dir + "\\selftest_outdoor_night.png", 1280, 720 );
		//hall preset again: same picture as before
		s.stage = st0;
		ApplyMood( s.stage, 0 );
		s.stage.width  = st0.width;
		s.master       = m0;
		Run( app, 0.5f );
		float hHall = 0;
		SavePng( app, dir + "\\selftest_hall_again.png", 640, 360, &hHall );
		T( s.stage.environment == 0 && fabsf( hHall - hRef ) < hRef * 0.1f + 1.0f, "Halle-Preset: Bild wie vorher (%.1f / %.1f)", hHall, hRef );
		//save / load
		ApplyMood( s.stage, 3 );
		s.stage.moonShadow = true;
		s.stage.moonAz     = 77;
		std::string p = dir + "\\selftest_outdoor.lightstage", err;
		bool ok = s.Save( p, &err );
		Show s2;
		ok = ok && s2.Load( p, app.lib, &err );
		T( ok && s2.stage.environment == 1 && s2.stage.ground == 3 && s2.stage.moonShadow && fabsf( s2.stage.moonAz - 77 ) < 1e-3f && fabsf( s2.stage.distanceFog - 0.5f ) < 1e-4f,
		   "Outdoor-Einstellungen gespeichert %s", err.c_str() );
		DeleteFileA( p.c_str() );
		s.stage = st0;
		s.master = m0;
		app.cam  = cam0;
		Run( app, 0.5f );
	}

	//---- phase D: engine thread, event-driven DMX output, performance mode
	{
		const OutputCfg o0 = s.output;
		s.output.artnet       = true;
		s.output.artnetTarget = "127.0.0.1";
		s.output.rate         = 1.0f;//1 Hz refresh: only a kick can send quickly
		app.io.Apply();
		MidiMap mm;
		mm.target     = MidiTarget::DmxChannel;
		mm.type       = MidiType::CC;
		mm.channel    = 0;
		mm.number     = 77;
		mm.universe   = 1;
		mm.channelDmx = 480;
		s.midi.push_back( mm );
		{
			std::lock_guard< std::mutex > l( s.dmxLock );
			if( DmxFrame* fr = s.Frame( 1 ) ) ( *fr )[ 479 ] = 0;
		}
		app.StartEngine();
		Sleep( 1300 );//let the 1 Hz refresh pass
		app.io.ClearLatency();
		const uint64_t sent0 = app.io.artnetSent;
		double t0 = Now(), tApplied = -1, tSent = -1;
		{
			std::lock_guard< std::mutex > l( app.midi.lock_ );
			app.midi.queue_.push_back( { 0, 0xB0u | ( 77u << 8 ) | ( 100u << 16 ), t0 } );
		}
		EventSignal( app.EngineEvent() );
		while( Now() - t0 < 0.5 && ( tApplied < 0 || tSent < 0 ) )
		{
			if( tApplied < 0 )
			{
				std::lock_guard< std::mutex > l( s.dmxLock );
				DmxFrame* fr = s.Frame( 1 );
				if( fr && ( *fr )[ 479 ] > 150 ) tApplied = Now();
			}
			if( tSent < 0 && app.io.artnetSent > sent0 ) tSent = Now();
			Sleep( 0 );
		}
		T( tApplied > 0 && ( tApplied - t0 ) * 1000 < 5, "Engine-Thread: MIDI -> DMX-Puffer in %.2f ms, ohne UI-Frame", ( tApplied - t0 ) * 1000 );
		T( tSent > 0 && ( tSent - t0 ) * 1000 < 10, "DMX sofort gesendet: %.2f ms nach dem MIDI-Eingang (Grundtakt 1 Hz)", ( tSent - t0 ) * 1000 );
		Sleep( 30 );
		DmxIO::Latency L = app.io.GetLatency();
		T( L.n >= 1 && L.maxMs < 10, "Latenzmessung: %d Wert(e), Ø %.2f ms, max %.2f ms", L.n, L.avgMs, L.maxMs );
		//burst of 200 CC messages: all applied, last value wins
		{
			std::lock_guard< std::mutex > l( app.midi.lock_ );
			for( int i = 0; i < 200; ++i ) app.midi.queue_.push_back( { 0, 0xB0u | ( 77u << 8 ) | ( (uint32_t)( i % 128 ) << 16 ), Now() } );
			app.midi.queue_.push_back( { 0, 0xB0u | ( 77u << 8 ) | ( 10u << 16 ), Now() } );
		}
		EventSignal( app.EngineEvent() );
		Sleep( 20 );
		int v = -1;
		{
			std::lock_guard< std::mutex > l( s.dmxLock );
			if( DmxFrame* fr = s.Frame( 1 ) ) v = ( *fr )[ 479 ];
		}
		T( v == (int)( 10 / 127.0f * 255 + 0.5f ), "201 MIDI-Nachrichten am Stück verarbeitet, letzter Wert gilt (%d)", v );
		app.StopEngine();
		s.midi.pop_back();
		s.output = o0;
		app.io.Apply();
		//performance mode: viewport rendering limits, show settings untouched
		const bool sh0 = s.stage.shadows;
		app.SetPerfView( 0 );
		app.SetPerfMode( true );
		T( !app.ShouldRenderViewport( Now() ), "Performance-Modus, Visualisierung aus: kein Rendern" );
		app.SetPerfView( 1 );
		app.RenderViewport( 320, 180 );
		bool noNow = !app.ShouldRenderViewport( Now() ), later = app.ShouldRenderViewport( Now() + 0.06 );
		T( noNow && later, "Vorschau: höchstens 20 Bilder/s (sofort nein, nach 60 ms ja)" );
		s.master = 1;
		Run( app, 0.3f );
		app.RenderViewport( 320, 180 );
		int shPerf = app.ren.stats.shadows;
		app.SetPerfMode( false );
		app.RenderViewport( 320, 180 );
		T( shPerf == 0 && app.ren.stats.shadows > 0 && s.stage.shadows == sh0, "Vorschau ohne Schatten (%d), danach wieder %d, Show-Einstellung unverändert", shPerf, app.ren.stats.shadows );
		T( app.FrameInterval() == 0, "Normalbetrieb: UI mit VSync" );
		app.SetPerfMode( true );
		T( app.FrameInterval() >= 1.0 / 31.0, "Performance-Modus: UI gedrosselt (%.0f ms pro Bild)", app.FrameInterval() * 1000 );
		app.SetPerfMode( false );
		app.SetPerfView( 1 );
	}

	//---- phase B: fixture editor (own lamps as OFL JSON)
	{
		const std::string libRoot = app.exeDir + "\\library";
		std::vector< std::string > written;
		auto add = []( EdFixture& e, EdType t ) {
			EdChannel c = MakeChannel( t );
			c.name      = UniqueChannelName( e, c.name );
			e.pool.push_back( c );
			e.modes[ 0 ].ch.push_back( (int)e.pool.size() - 1 );
		};
		EdFixture e;
		e.manufacturer  = "LS Test";
		e.name          = "Test RGBW 9ch";
		e.modes[ 0 ].name = "9ch";
		for( EdType t : { EdType::Dimmer, EdType::ShutterStrobe, EdType::Red, EdType::Green, EdType::Blue, EdType::White, EdType::Zoom, EdType::Pan } ) add( e, t );
		std::string id, path, err;
		bool ok = SaveUserFixture( libRoot, e, id, path, &err );
		written.push_back( path );
		Library L2;
		L2.LoadAll( libRoot );
		const FixtureDef* d = L2.Find( id );
		bool attrs = d && d->modes[ 0 ].ch.size() == 9 && d->modes[ 0 ].ch[ 0 ].attr == Attr::Dimmer && d->modes[ 0 ].ch[ 1 ].attr == Attr::Shutter && d->modes[ 0 ].ch[ 2 ].attr == Attr::Red &&
		             d->modes[ 0 ].ch[ 5 ].attr == Attr::White && d->modes[ 0 ].ch[ 6 ].attr == Attr::Zoom && d->modes[ 0 ].ch[ 7 ].attr == Attr::Pan && d->modes[ 0 ].ch[ 7 ].fine == 8 && d->modes[ 0 ].ch[ 8 ].isFine;
		T( ok && d && attrs && d->kind == Kind::Par && id == "user/ls-test/test-rgbw-9ch", "Eigene Lampe gespeichert und aus library\\user geladen: %s, %d Kanäle, Attribute korrekt %s", id.c_str(), d ? d->modes[ 0 ].Footprint() : 0, err.c_str() );
		//patched and driven like any other lamp
		app.fxEd = e;
		bool saved = app.SaveFixtureEditor( false );
		const FixtureDef* ad = app.lib.Find( id );
		int uni = s.universes[ 0 ].num, adr = s.FreeAddress( uni, 9 );
		FixtureInst* f = saved && ad && adr ? app.AddFixture( ad, 0, uni, adr, V3( 0, 3, 3 ), V3( 0, 0, 0 ), "Editor-Test" ) : nullptr;
		const int fid  = f ? f->id : -1;
		if( f )
		{
			s.SetAttr( *f, Attr::Dimmer, 1 );
			s.SetAttr( *f, Attr::Shutter, 1 );
			s.SetAttr( *f, Attr::Red, 1 );
			s.SetAttr( *f, Attr::Zoom, 0 );
			Run( app, 0.5f );
			f = s.FindFixture( fid );
			float z0 = f->st.zoom;
			s.SetAttr( *f, Attr::Zoom, 1 );
			Run( app, 1.0f );
			f = s.FindFixture( fid );
			T( f->st.intensity > 0.3f && f->st.color.x > 0.9f && f->st.color.y < 0.2f && fabsf( z0 - 10 ) < 1.0f && fabsf( f->st.zoom - 40 ) < 1.0f,
			   "Eigene Lampe in der Show: rot, Intensität %.2f, Zoom %.0f° -> %.0f°", f->st.intensity, z0, f->st.zoom );
		}
		else
			T( false, "Eigene Lampe patchen (gespeichert %d, Adresse %d)", saved, adr );
		//moving head template
		EdFixture mh;
		mh.manufacturer = "LS Test";
		mh.name         = "Test Spot";
		mh.modes[ 0 ].name = "16ch";
		ApplyTemplate( mh, 0, 4 );
		FixtureDef md;
		std::string e2;
		bool pm = ParseOflFixture( BuildOfl( mh ), "user/ls-test/test-spot", "LS Test", md, &e2 );
		T( pm && md.kind == Kind::MovingHead && md.modes[ 0 ].Footprint() == 16 && fabsf( md.panRange - 540 ) < 1 && md.wheels.size() == 2, "Vorlage Moving Head Spot: %d Kanäle, Pan %.0f°, %d Räder %s", pm ? md.modes[ 0 ].Footprint() : 0, md.panRange, (int)md.wheels.size(), e2.c_str() );
		//pixel bar
		EdFixture bar;
		bar.manufacturer  = "LS Test";
		bar.name          = "Test Bar";
		bar.category      = 3;
		bar.pixels        = 8;
		bar.pixelChannels = { EdType::Red, EdType::Green, EdType::Blue };
		add( bar, EdType::Dimmer );
		FixtureDef bd;
		bool pb = ParseOflFixture( BuildOfl( bar ), "user/ls-test/test-bar", "LS Test", bd, &e2 );
		bool pixAttr = pb && bd.modes[ 0 ].ch.size() == 25 && bd.modes[ 0 ].ch[ 1 ].attr == Attr::Red && bd.modes[ 0 ].ch[ 1 ].pixels.size() == 1 && bd.modes[ 0 ].ch[ 24 ].attr == Attr::Blue;
		T( pb && bd.Heads() == 8 && bd.kind == Kind::Bar && pixAttr, "Pixel-Lampe: %d Pixel, %d Kanäle (Dimmer + 8 x RGB) %s", bd.Heads(), pb ? bd.modes[ 0 ].Footprint() : 0, e2.c_str() );
		//editor model round trip
		EdFixture rt;
		bool rr = EdFromJson( BuildOfl( e )[ "lightstageEditor" ], rt );
		T( rr && rt.pool.size() == e.pool.size() && rt.Footprint( 0 ) == e.Footprint( 0 ) && rt.pool[ 7 ].fine16 && rt.pool[ 1 ].ranges.size() == e.pool[ 1 ].ranges.size(), "Editor-Daten in der Datei: Laden ergibt dieselbe Lampe (%d Kanäle)", rt.Footprint( 0 ) );
		//library lamp edited as a copy keeps its channel layout
		const FixtureDef* src = nullptr;
		for( auto& dd : app.lib.defs )
			if( dd->id.rfind( "generic/", 0 ) == 0 && dd->Heads() == 1 && dd->modes[ 0 ].Footprint() >= 6 )
			{
				bool plain = true;//effect / generic channels are not reproduced 1:1
				for( auto& c : dd->modes[ 0 ].ch )
					if( c.attr == Attr::Effect || c.attr == Attr::Generic || c.attr == Attr::None || c.swCtrl >= 0 ) plain = false;
				if( plain ) { src = dd.get(); break; }
			}
		if( src )
		{
			EdFixture cp;
			EdFromDef( *src, cp );
			FixtureDef cd;
			bool pc = ParseOflFixture( BuildOfl( cp ), "user/ls-test/copy", "LS Test", cd, &e2 );
			bool same = pc && cd.modes.size() == src->modes.size() && cd.modes[ 0 ].Footprint() == src->modes[ 0 ].Footprint();
			for( size_t i = 0; same && i < cd.modes[ 0 ].ch.size(); ++i ) same = cd.modes[ 0 ].ch[ i ].attr == src->modes[ 0 ].ch[ i ].attr;
			T( same, "Bibliothekslampe als Kopie (%s): gleiche Kanalbelegung, %d Modi", src->name.c_str(), (int)cd.modes.size() );
		}
		//show file carries the lamp; a missing library file is restored from the show
		std::string sp = dir + "\\selftest_userfx.lightstage";
		ok = s.Save( sp, &err );
		std::string txt;
		Json sj;
		bool emb = ok && ReadText( sp, txt ) && ParseJson( txt, sj ) && sj[ "userFixtures" ].Find( id );
		DeleteFileA( path.c_str() );
		bool reg = emb && app.RegisterEmbeddedFixture( id, sj[ "userFixtures" ][ id ] );
		DWORD at = GetFileAttributesA( path.c_str() );
		T( emb && reg && at != INVALID_FILE_ATTRIBUTES, "Show-Datei enthält die eigene Lampe, fehlende Bibliotheksdatei wird wiederhergestellt" );
		DeleteFileA( sp.c_str() );
		//clean up
		s.fixtures.erase( std::remove_if( s.fixtures.begin(), s.fixtures.end(), [ & ]( const FixtureInst& x ) { return x.id == fid; } ), s.fixtures.end() );
		DeleteFileA( path.c_str() );
		RemoveDirectoryA( ( libRoot + "\\user\\ls-test" ).c_str() );
		{
			//test manufacturer out of library\user\manufacturers.json again
			std::string mp = libRoot + "\\user\\manufacturers.json", mt;
			Json mj;
			if( ReadText( mp, mt ) && ParseJson( mt, mj ) && mj.IsObj() )
			{
				mj.o.erase( std::remove_if( mj.o.begin(), mj.o.end(), []( const std::pair< std::string, Json >& kv ) { return kv.first == "ls-test"; } ), mj.o.end() );
				if( mj.o.empty() ) DeleteFileA( mp.c_str() );
				else WriteText( mp, WriteJson( mj, true ) );
			}
		}
		Run( app, 0.3f );
	}

	//---- Art-Net packets + loopback
	{
		uint8_t d[ 512 ];
		for( int i = 0; i < 512; ++i ) d[ i ] = (uint8_t)( i * 7 );
		auto pkt = BuildArtDmx( 0x1234, 9, d, 512 );
		int port, len;
		const uint8_t* pd;
		bool ok = ParseArtDmx( pkt.data(), (int)pkt.size(), port, pd, len );
		T( ok && port == 0x1234 && len == 512 && memcmp( pd, d, 512 ) == 0 && pkt.size() == 530 && pkt[ 8 ] == 0 && pkt[ 9 ] == 0x50 && pkt[ 11 ] == 14, "ArtDmx-Paket korrekt (530 Byte, OpCode 0x5000, ProtVer 14)" );
		//loopback: input on universe 3 (port 100), send from a separate socket to 127.0.0.1
		int un = s.AddUniverse();
		Universe& u = s.universes[ s.UniIndex( un ) ];
		u.artnetOut = false;
		u.artnetIn  = true;
		u.artnetInPort = 100;
		s.output.artnet       = true;
		s.output.artnetTarget = "127.0.0.1";
		s.output.artnetInput  = true;
		app.io.acceptLoopback = true;
		app.io.Apply();
		Sleep( 300 );
		net::Sock so = net::Udp();
		sockaddr_in to{};
		net::Addr( "127.0.0.1", 6454, to );
		auto p2 = BuildArtDmx( 100, 1, d, 512 );
		bool got = false;
		for( int tries = 0; tries < 20 && !got; ++tries )
		{
			net::SendTo( so, p2.data(), (int)p2.size(), to );
			Sleep( 50 );
			app.io.PollInput();
			DmxFrame* fr = s.Frame( un );
			got          = fr && memcmp( fr->data(), d, 512 ) == 0;
		}
		net::Close( so );
		T( got, "Art-Net Eingang (Loopback 127.0.0.1, Port-Adresse 100) -> Universe %d", un );
		uint64_t sent0 = app.io.artnetSent;
		Sleep( 400 );
		uint64_t sent1 = app.io.artnetSent;
		float rate     = ( sent1 - sent0 ) / 0.4f;
		T( sent1 > sent0, "Art-Net Ausgang läuft (an 127.0.0.1): ~%.0f Pakete/s für %d Universes", rate, 2 );
		Info( "Art-Net Status: %s", app.io.Status().c_str() );
		//restore
		s.output.artnet       = false;
		s.output.artnetInput  = false;
		app.io.acceptLoopback = false;
		app.io.Apply();
		std::lock_guard< std::mutex > l( s.dmxLock );
		int ui = s.UniIndex( un );
		s.universes.erase( s.universes.begin() + ui );
		s.dmx.erase( s.dmx.begin() + ui );
	}
	//---- 1.2: sACN (E1.31) packets + output
	{
		uint8_t d[ 512 ], cid[ 16 ];
		for( int i = 0; i < 512; ++i ) d[ i ] = (uint8_t)( i * 3 + 1 );
		for( int i = 0; i < 16; ++i ) cid[ i ] = (uint8_t)( 0xA0 + i );
		auto pkt = BuildE131( 258, 7, 120, cid, "Test", d, 512 );
		int un = 0, pr = 0, len = 0;
		const uint8_t* pd = nullptr;
		const bool ok     = ParseE131( pkt.data(), (int)pkt.size(), un, pr, pd, len );
		const int rootLen = ( ( pkt[ 16 ] & 0x0f ) << 8 ) | pkt[ 17 ];
		T( ok && pkt.size() == 638 && un == 258 && pr == 120 && len == 512 && memcmp( pd, d, 512 ) == 0 && rootLen == 622 && pkt[ 111 ] == 7 && !memcmp( &pkt[ 22 ], cid, 16 ) && E131Multicast( 258 ) == "239.255.1.2",
		   "sACN-Paket korrekt (638 Byte, Universe 258 -> 239.255.1.2, Priorität 120)" );
		//output to 127.0.0.1:5568; receive it if the port is free (Resolume & co. may hold it)
		net::Sock rs = net::Udp();
		sockaddr_in a{};
		net::Addr( "127.0.0.1", 5568, a );
		net::SetRecvTimeout( rs, 100 );
		const bool bound = net::Bind( rs, a );
		{
			std::lock_guard< std::mutex > l( s.dmxLock );
			s.dmx[ 0 ][ 0 ] = 77;
		}
		s.output.sacn       = true;
		s.output.sacnTarget = "127.0.0.1";
		app.io.Apply();
		const uint64_t s0 = app.io.sacnSent;
		int gotUni = -1, got0 = -1;
		for( int k = 0; k < 20 && bound && gotUni < 0; ++k )
		{
			uint8_t b[ 700 ];
			const int n = net::Recv( rs, b, sizeof( b ) );
			if( n > 0 && ParseE131( b, n, un, pr, pd, len ) ) { gotUni = un; got0 = pd[ 0 ]; }
		}
		Sleep( 300 );
		const uint64_t s1 = app.io.sacnSent;
		net::Close( rs );
		if( bound ) T( s1 > s0 && gotUni == s.universes[ 0 ].num && got0 == 77, "sACN-Ausgang: %llu Pakete, empfangen Universe %d, Kanal 1 = %d", (unsigned long long)( s1 - s0 ), gotUni, got0 );
		else T( s1 > s0, "sACN-Ausgang: %llu Pakete gesendet (Port 5568 von anderem Programm belegt, Empfang nicht geprüft)", (unsigned long long)( s1 - s0 ) );
		s.output.sacn = false;
		s.output.sacnTarget.clear();
		app.io.Apply();
	}
	//---- Enttec framing + port listing (no hardware access)
	{
		uint8_t d[ 512 ] = {};
		d[ 0 ]           = 255;
		d[ 511 ]         = 17;
		auto f           = BuildEnttecDmx( d, 512 );
		T( f.size() == 518 && f[ 0 ] == 0x7E && f[ 1 ] == 6 && f[ 2 ] == 0x01 && f[ 3 ] == 0x02 && f[ 4 ] == 0 && f[ 5 ] == 255 && f[ 516 ] == 17 && f[ 517 ] == 0xE7, "Enttec USB Pro Label-6-Frame korrekt (518 Byte)" );
		auto ports = ListComPorts();
		std::string pl;
		for( auto& p : ports ) pl += p.port + "(" + p.name + ( p.ftdi ? ",FTDI" : "" ) + ") ";
		Info( "COM-Ports: %s", pl.empty() ? "keine" : pl.c_str() );
		std::string nl;
		for( auto& i : net::ListInterfaces() ) nl += i.name + "=" + i.ip + "/" + i.netmask + ( i.up ? "" : "(down)" ) + " ";
		Info( "Netzwerkkarten: %s", nl.empty() ? "keine" : nl.c_str() );
	}
	//---- MIDI via LoopBe1 (if installed)
	{
		auto outs = MidiIn::OutputPorts();
		std::string lb;
		for( auto& o : outs )
			if( ToLower( o ).find( "loopbe" ) != std::string::npos ) lb = o;
		std::string ins;
		for( auto& d : app.midi.devices ) ins += d + "; ";
		Info( "MIDI-Eingänge: %s", ins.empty() ? "keine" : ins.c_str() );
		bool inOpen = false;
		for( auto& d : app.midi.devices )
			if( ToLower( d ).find( "loopbe" ) != std::string::npos ) inOpen = true;
		if( lb.empty() || !inOpen )
			Info( "LoopBe1 nicht verfügbar (Ausgang %s, Eingang %s) - MIDI-Loopback-Test übersprungen", lb.empty() ? "fehlt" : "ok", inOpen ? "ok" : "fehlt" );
		else
		{
			FixtureInst* sp = nullptr;
			for( auto& f : s.fixtures )
				if( f.name == "Spot 1" ) sp = &f;
			MidiMap m;
			m.type     = MidiType::CC;
			m.number   = 21;
			m.channel  = 0;
			m.target   = MidiTarget::FixtureAttr;
			m.fixtures = { sp->id };
			m.attr     = Attr::Dimmer;
			s.midi.push_back( m );
			MidiMap m14 = m;
			m14.type    = MidiType::CC14;
			m14.number  = 7;
			m14.attr    = Attr::Pan;
			s.midi.push_back( m14 );
			s.SetAttr( *sp, Attr::Dimmer, 0 );
			uint64_t rc0 = app.midi.received;
			bool s1 = MidiIn::SendShort( lb, 0xB0, 21, 100 );
			bool s2 = MidiIn::SendShort( lb, 0xB0, 7, 96 );
			bool s3 = MidiIn::SendShort( lb, 0xB0, 39, 64 );
			Info( "SendShort an '%s': %d %d %d", lb.c_str(), s1, s2, s3 );
			float dim = 0, pan = 0;
			for( int i = 0; i < 40; ++i )
			{
				Sleep( 15 );
				std::vector< MidiEvent > ev;
				app.midi.Poll( ev );
				for( auto& e : ev ) ApplyMidi( s, e );
				dim = s.GetAttr( *sp, Attr::Dimmer );
				pan = s.GetAttr( *sp, Attr::Pan );
				if( dim > 0.5f && pan > 0.7f ) break;
			}
			if( app.midi.received == rc0 )
			{
				Info( "LoopBe liefert nichts (Port ist vermutlich stummgeschaltet: Tray-Symbol 'Mute') - gleiche Bytes werden intern eingespeist" );
				{
					std::lock_guard< std::mutex > l( app.midi.lock_ );
					app.midi.queue_.push_back( { 0, 0xB0u | ( 21u << 8 ) | ( 100u << 16 ) } );
					app.midi.queue_.push_back( { 0, 0xB0u | ( 7u << 8 ) | ( 96u << 16 ) } );
					app.midi.queue_.push_back( { 0, 0xB0u | ( 39u << 8 ) | ( 64u << 16 ) } );
				}
				std::vector< MidiEvent > ev;
				app.midi.Poll( ev );
				for( auto& e : ev ) ApplyMidi( s, e );
				dim = s.GetAttr( *sp, Attr::Dimmer );
				pan = s.GetAttr( *sp, Attr::Pan );
			}
			Info( "MIDI empfangen: %llu Nachrichten", (unsigned long long)( app.midi.received - rc0 ) );
			for( auto& l : app.midi.monitor ) Info( "  %s", l.c_str() );
			T( fabsf( dim - 100 / 127.0f ) < 0.01f, "MIDI CC21 -> Dimmer Spot 1: %.3f (erwartet %.3f)", dim, 100 / 127.0f );
			float exp14 = ( 96 * 128 + 64 ) / 16383.0f;
			T( fabsf( pan - exp14 ) < 0.002f, "MIDI 14-bit CC7/39 -> Pan: %.4f (erwartet %.4f)", pan, exp14 );
			s.midi.clear();
		}
	}
	//---- OSC: real UDP packets to our own listener (message, int DMX value, bundle, pixel head)
	{
		FixtureInst* sp = nullptr;
		for( auto& f : s.fixtures )
			if( f.name == "Spot 1" ) sp = &f;
		OscIn oi;
		const int port = 9137;
		bool up = oi.Start( port );
		T( up, "OSC-Empfang auf UDP %d gestartet %s", port, oi.error.c_str() );
		if( up && sp )
		{
			net::Sock so = net::Udp();
			sockaddr_in to{};
			net::Addr( "127.0.0.1", (uint16_t)port, to );
			std::string sendLog;
			auto send = [ & ]( const std::vector< uint8_t >& b ) {
				int r = net::SendTo( so, b.data(), (int)b.size(), to );
				sendLog += std::to_string( r ) + ( r < 0 ? "/" + std::to_string( net::LastError() ) : "" ) + " ";
				Sleep( 2 );
			};
			s.SetAttr( *sp, Attr::Dimmer, 0 );
			send( BuildOsc( "/ls/fixture/" + std::to_string( sp->id ) + "/dimmer", { 0.5f } ) );
			send( BuildOsc( "/ls/dmx/1/500", { 200 }, true ) );
			{
				//bundle with two messages
				auto m1 = BuildOsc( "/ls/master", { 0.8f } );
				auto m2 = BuildOsc( "/ls/fixture/" + std::to_string( sp->id ) + "/zoom", { 0.25f } );
				std::vector< uint8_t > b( { '#', 'b', 'u', 'n', 'd', 'l', 'e', 0, 0, 0, 0, 0, 0, 0, 0, 1 } );
				for( auto* m : { &m1, &m2 } )
				{
					uint32_t n = (uint32_t)m->size();
					b.push_back( n >> 24 ), b.push_back( ( n >> 16 ) & 255 ), b.push_back( ( n >> 8 ) & 255 ), b.push_back( n & 255 );
					b.insert( b.end(), m->begin(), m->end() );
				}
				send( b );
			}
			send( BuildOsc( "/ls/gibtsnicht", { 1 } ) );
			int applied = 0, unknown = 0;
			for( int i = 0; i < 300 && ( applied < 4 || unknown < 1 ); ++i )
			{
				Sleep( 10 );
				std::vector< OscMsg > ms;
				oi.Poll( ms );
				for( auto& m : ms ) ( ApplyOsc( s, m ) ? applied : unknown )++;
			}
			net::Close( so );
			DmxFrame* fr = s.Frame( 1 );
			float dim = s.GetAttr( *sp, Attr::Dimmer ), zm = s.GetAttr( *sp, Attr::Zoom );
			T( applied == 4 && unknown == 1, "OSC: %d Nachrichten angewendet, %d unbekannte Adresse ignoriert (%d fehlerhaft, gesendet: %s)", applied, unknown, (int)oi.bad, sendLog.c_str() );
			T( fabsf( dim - 0.5f ) < 0.01f, "OSC /ls/fixture/%d/dimmer 0.5 -> Dimmer %.3f", sp->id, dim );
			T( fr && ( *fr )[ 499 ] == 200, "OSC /ls/dmx/1/500 200 (int) -> Kanal 1.500 = %d", fr ? ( *fr )[ 499 ] : -1 );
			T( fabsf( s.master - 0.8f ) < 0.01f && fabsf( zm - 0.25f ) < 0.01f, "OSC-Bundle: Master %.2f, Zoom %.2f", s.master, zm );
			s.master = 1.0f;
		}
		oi.Stop();
	}
	//---- Ableton bridge: discovery, HTTP manifest, control, feedback, echo guard, release, timeout
	{
		struct TestOscMsg
		{
			std::vector< uint8_t > b;
			TestOscMsg( const std::string& a, const std::string& t )
			{
				S( a );
				S( "," + t );
			}
			void U( uint32_t v )
			{
				b.push_back( (uint8_t)( v >> 24 ) );
				b.push_back( (uint8_t)( ( v >> 16 ) & 255 ) );
				b.push_back( (uint8_t)( ( v >> 8 ) & 255 ) );
				b.push_back( (uint8_t)( v & 255 ) );
			}
			TestOscMsg& S( const std::string& s )
			{
				b.insert( b.end(), s.begin(), s.end() );
				b.push_back( 0 );
				while( b.size() & 3 ) b.push_back( 0 );
				return *this;
			}
			TestOscMsg& I( int v )
			{
				U( (uint32_t)(int32_t)v );
				return *this;
			}
			TestOscMsg& F( float f )
			{
				uint32_t v;
				memcpy( &v, &f, 4 );
				U( v );
				return *this;
			}
		};
		const int httpPort = 8091, oscPort = 9137, clientPort = 9150;
		Bridge br;
		std::string berr;
		T( br.Start( httpPort, &berr, true ), "Bridge: HTTP-Server auf Port %d gestartet %s", httpPort, berr.c_str() );
		{
			//port busy: the failure is reported once and cleared by Stop (the app retries every 3 s)
			Bridge b2;
			std::string e2;
			b2.Start( httpPort, &e2 );
			for( int i = 0; i < 300 && !b2.Failed() && !b2.Ready(); ++i ) Sleep( 5 );
			const bool failed = b2.Failed();
			b2.Stop();
			T( failed && !b2.Failed() && !b2.Running(), "Bridge: belegter Port wird gemeldet und nach Stop zurückgesetzt" );
		}
		OscIn oi;
		T( oi.Start( oscPort ), "Bridge: OSC-Eingang auf UDP %d", oscPort );
		net::Sock cs = net::Udp();
		sockaddr_in me{}, to{};
		net::Addr( "127.0.0.1", (uint16_t)clientPort, me );
		net::Addr( "127.0.0.1", (uint16_t)oscPort, to );
		net::Bind( cs, me );
		net::SetRecvTimeout( cs, 1 );
		auto send = [ & ]( const std::vector< uint8_t >& b ) { net::SendTo( cs, b.data(), (int)b.size(), to ); };
		std::vector< OscMsg > inbox;
		auto recvAll = [ & ] {
			std::vector< char > buf( 65536 );
			for( ;; )
			{
				int n = net::Recv( cs, buf.data(), (int)buf.size() );
				if( n <= 0 ) break;
				std::vector< OscMsg > ms;
				if( ParseOsc( (const uint8_t*)buf.data(), n, ms ) ) inbox.insert( inbox.end(), ms.begin(), ms.end() );
			}
		};
		auto pumpOnce = [ & ] {//what the engine does: OSC in -> bridge / registers, bridge tick
			std::vector< OscMsg > in;
			oi.Poll( in );
			for( auto& m : in )
				if( IsBridgeMessage( m.address ) ) br.OnOsc( m, s );
				else ApplyOsc( s, m );
			br.Tick( s, Now() );
		};
		auto run = [ & ]( double ms ) {
			const double end = Now() + ms / 1000.0;
			while( Now() < end )
			{
				pumpOnce();
				recvAll();
				Sleep( 2 );
			}
		};
		auto httpGet = [ & ]( const char* path ) -> std::string {
			net::Sock h = net::Tcp();
			sockaddr_in a{};
			net::Addr( "127.0.0.1", (uint16_t)httpPort, a );
			net::SetRecvTimeout( h, 3000 );
			std::string resp;
			if( net::Connect( h, a ) )
			{
				std::string req = std::string( "GET " ) + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
				net::Send( h, req.data(), (int)req.size() );
				char buf[ 8192 ];
				int n;
				while( ( n = net::Recv( h, buf, sizeof( buf ) ) ) > 0 ) resp.append( buf, n );
			}
			net::Close( h );
			const size_t p = resp.find( "\r\n\r\n" );
			return p == std::string::npos ? std::string() : resp.substr( p + 4 );
		};
		(void)httpGet;

		//save what the test changes
		const DmxFrame dmxBefore = *s.Frame( 1 );
		const float masterBefore = s.master, expBefore = s.stage.exposure;
		FixtureInst* sp = nullptr;
		for( auto& f : s.fixtures )
			if( f.name == "Spot 1" ) sp = &f;

		//1 discovery: /ls/hello -> /ls/info with manifest revision
		{
			TestOscMsg h( "/ls/hello", "iisss" );
			h.I( 1 ).I( clientPort ).S( "test" ).S( "t1" ).S( "params,dmx,state" );
			br.Tick( s, Now() );//the app ticks the bridge from the start: manifest revision 1 exists before a device says hello
			send( h.b );
			run( 400 );
			const OscMsg* info = nullptr;
			for( auto& m : inbox )
				if( m.address == "/ls/info" ) info = &m;
			T( info && info->f.size() >= 3 && info->f[ 1 ] >= 1 && (int)info->f[ 2 ] == httpPort, "Ableton: /ls/hello -> /ls/info (Rev %.0f, HTTP %.0f)", info ? info->f[ 1 ] : -1.0f, info && info->f.size() > 2 ? info->f[ 2 ] : -1.0f );
			inbox.clear();
		}
		//2 manifest over HTTP
		{
			const std::string body = httpGet( "/manifest.json" );
			Json mj;
			const bool parsed = !body.empty() && ParseJson( body, mj, nullptr );
			T( parsed && mj[ "format" ].Str() == "grosserd-lightstage-manifest" && mj[ "fixtures" ].Size() == s.fixtures.size(), "Manifest per HTTP: Format ok, %d Lampen", (int)mj[ "fixtures" ].Size() );
			bool chans = parsed;
			int nch    = 0;
			for( size_t i = 0; parsed && i < mj[ "fixtures" ].Size(); ++i )
			{
				const Json& fj = mj[ "fixtures" ][ i ];
				const Mode* m  = s.fixtures[ i ].M();
				if( !m || fj[ "channels" ].Size() != (size_t)m->Footprint() || fj[ "footprint" ].I() != m->Footprint() ) { chans = false; break; }
				for( size_t k = 0; k < fj[ "channels" ].Size(); ++k )
				{
					if( fj[ "channels" ][ k ][ "name" ].Str() != m->ch[ k ].name || fj[ "channels" ][ k ][ "dmx" ].I() != s.fixtures[ i ].address + (int)k ) chans = false;
					++nch;
				}
			}
			T( chans, "Manifest: jede Lampe mit allen DMX-Kanälen und Namen (%d Kanäle)", nch );
			bool hasMaster = false;
			for( size_t i = 0; parsed && i < mj[ "params" ].Size(); ++i )
				if( mj[ "params" ][ i ][ "addr" ].Str() == "/ls/master" ) hasMaster = true;
			T( hasMaster, "Manifest: params enthält /ls/master" );
			std::vector< std::string > keys;
			for( size_t i = 0; parsed && i < mj[ "midiTargets" ].Size(); ++i ) keys.push_back( mj[ "midiTargets" ][ i ][ "key" ].Str() );
			const size_t nKeys = keys.size();
			std::sort( keys.begin(), keys.end() );
			keys.erase( std::unique( keys.begin(), keys.end() ), keys.end() );
			T( nKeys > 0 && keys.size() == nKeys, "Manifest: midiTargets %d Ziele, alle Schlüssel eindeutig", (int)nKeys );
			const size_t nMaps = s.midi.size();
			T( parsed && mj[ "midiMappings" ].Size() == nMaps, "Manifest: midiMappings = %d (Show)", (int)nMaps );
		}
		//3 normalised control: /ls/norm /ls/stage/exposure 0.5
		{
			TestOscMsg n( "/ls/norm", "sf" );
			n.S( "/ls/stage/exposure" ).F( 0.5f );
			send( n.b );
			run( 150 );
			T( fabsf( s.stage.exposure - ( 0.1f + 0.5f * 3.9f ) ) < 0.01f, "/ls/norm /ls/stage/exposure 0.5 -> Belichtung %.3f", s.stage.exposure );
			inbox.clear();
		}
		//4 bulk DMX and lamp slots
		{
			TestOscMsg db( "/ls/dmxblock", "iiiii" );
			db.I( 1 ).I( 1 ).I( 10 ).I( 20 ).I( 30 );
			send( db.b );
			run( 120 );
			DmxFrame* fr = s.Frame( 1 );
			T( fr && ( *fr )[ 0 ] == 10 && ( *fr )[ 1 ] == 20 && ( *fr )[ 2 ] == 30, "/ls/dmxblock 1 1 10 20 30 -> Kanal 1..3 = %d %d %d", fr ? ( *fr )[ 0 ] : -1, fr ? ( *fr )[ 1 ] : -1, fr ? ( *fr )[ 2 ] : -1 );
			if( sp )
			{
				TestOscMsg fb( "/ls/fixblock", "iii" );
				fb.I( sp->id ).I( 1 ).I( 255 );
				send( fb.b );
				run( 120 );
				T( s.GetBase( *sp, 0 ) == 255, "/ls/fixblock %d 1 255 -> erster Kanal der Lampe = %d", sp->id, s.GetBase( *sp, 0 ) );
			}
			inbox.clear();
		}
		//5 feedback: register value and DMX output go back to the device
		{
			inbox.clear();
			s.master = 2.5f;
			{
				std::lock_guard< std::mutex > dl( s.dmxLock );
				( *s.Frame( 1 ) )[ 9 ] = 77;
				s.Bump();
			}
			run( 250 );
			bool fbMaster = false, fbDmx = false;
			for( auto& m : inbox )
			{
				if( m.address == "/ls/fb" && m.strs.size() && m.strs[ 0 ] == "/ls/master" && m.f.size() && fabsf( m.f[ 0 ] - 2.5f ) < 1e-3f ) fbMaster = true;
				if( m.address == "/ls/fbdmx" && m.f.size() > 2 && (int)m.f[ 0 ] == 1 && (int)m.f[ 1 ] <= 10 && (int)m.f[ 1 ] + (int)m.f.size() - 2 > 10 && (int)m.f[ 10 - (int)m.f[ 1 ] + 2 ] == 77 ) fbDmx = true;
			}
			T( fbMaster, "Feedback: Master 2.5 -> /ls/fb innerhalb 250 ms" );
			T( fbDmx, "Feedback: DMX-Kanal 1.010 = 77 -> /ls/fbdmx" );
			inbox.clear();
		}
		//6 no echo: a value sent by the device is not sent back to it
		{
			TestOscMsg n( "/ls/norm", "sf" );
			n.S( "/ls/stage/exposure" ).F( 0.25f );
			send( n.b );
			run( 250 );
			bool echo = false;
			for( auto& m : inbox )
				if( m.address == "/ls/fb" && m.strs.size() && m.strs[ 0 ] == "/ls/stage/exposure" ) echo = true;
			T( !echo, "Kein Echo: /ls/norm-Wert kommt nicht zurück an den Absender" );
			inbox.clear();
		}
		//7 structure change -> /ls/manifest/changed with a higher revision
		{
			const uint32_t rev0 = br.Rev();
			Snapshot sn;
			sn.name = "Selbsttest";
			s.snapshots.push_back( sn );
			run( 200 );
			bool changed = false;
			for( auto& m : inbox )
				if( m.address == "/ls/manifest/changed" && m.f.size() && (uint32_t)m.f[ 0 ] > rev0 ) changed = true;
			T( changed, "Strukturänderung -> /ls/manifest/changed (Rev %u -> %u)", rev0, br.Rev() );
			s.snapshots.pop_back();
			run( 100 );
			inbox.clear();
		}
		//8 "An Ableton senden"
		{
			const Param* pm = FindParam( s, "/ls/master" );
			inbox.clear();
			int n = pm ? br.SendAssign( *pm ) : 0;
			run( 100 );
			bool ok = false;
			for( auto& m : inbox )
				if( m.address == "/ls/assign" && m.strs.size() && m.strs[ 0 ] == "/ls/master" ) ok = true;
			T( ok && n == 1, "An Ableton senden: /ls/assign /ls/master gesendet" );
			inbox.clear();
		}
		//8b knob / button tables both ways: device sync, LightStage assigns, device sets a slot, LightStage wins on sync
		{
			const auto ableBefore = s.ableton;
			s.ableton.clear();
			auto sync = [ & ]( const std::string& k1, const std::string& b1 ) {
				TestOscMsg m( "/ls/map/sync", std::string( 25, 's' ) );
				m.S( "t1" ).S( k1 );
				for( int i = 1; i < 16; ++i ) m.S( "" );
				m.S( b1 );
				for( int i = 1; i < 8; ++i ) m.S( "" );
				send( m.b );
				run( 150 );
			};
			sync( "fx:/ls/master", "" );
			AbletonMap* am = s.FindAbleton( "t1" );
			const bool adopted = am && am->knobs[ 0 ] == "fx:/ls/master" && am->label == "Control 1";
			//LightStage assigns knob 2 -> /ls/map/all to the device
			inbox.clear();
			int n = 0;
			if( am )
			{
				am->knobs[ 1 ] = "haze";
				n              = br.PushMap( *am );
			}
			run( 150 );
			bool pushed = false;
			for( auto& m : inbox )
				if( m.address == "/ls/map/all" && m.strs.size() == 26 && m.strs[ 0 ] == "t1" && m.strs[ 2 ] == "fx:/ls/master" && m.strs[ 3 ] == "haze" ) pushed = true;
			//device sets button 3
			TestOscMsg set( "/ls/map/set", "ssis" );
			set.S( "t1" ).S( "b" ).I( 3 ).S( "snap:1" );
			send( set.b );
			run( 150 );
			am                 = s.FindAbleton( "t1" );
			const bool fromDev = am && am->buttons[ 2 ] == "snap:1";
			//device reconnects with another table: LightStage keeps its own and sends it back
			inbox.clear();
			sync( "master", "" );
			am                = s.FindAbleton( "t1" );
			bool back         = false;
			for( auto& m : inbox )
				if( m.address == "/ls/map/all" && m.strs.size() == 26 && m.strs[ 2 ] == "fx:/ls/master" && m.strs[ 20 ] == "snap:1" ) back = true;
			const bool kept   = am && am->knobs[ 0 ] == "fx:/ls/master";
			//manifest carries the table
			run( 100 );
			const std::string man = httpGet( "/manifest.json" );
			const bool inMan      = man.find( "\"deviceMaps\"" ) != std::string::npos && man.find( "\"t1\"" ) != std::string::npos && man.find( "snap:1" ) != std::string::npos;
			T( adopted && n == 1 && pushed && fromDev && back && kept && inMan, "Ableton-Belegung beide Richtungen: Device-Tabelle übernommen, LightStage -> Device (/ls/map/all), Device -> LightStage (/ls/map/set), bei Sync gewinnt LightStage, Manifest deviceMaps" );
			//save / load
			std::string err;
			const std::string fn = dir + "\\selftest_ableton.lightstage";
			bool rt              = false;
			if( s.Save( fn, &err ) )
			{
				Show s2;
				if( s2.Load( fn, app.lib, &err ) )
					if( AbletonMap* m2 = s2.FindAbleton( "t1" ) ) rt = m2->knobs == am->knobs && m2->buttons == am->buttons && m2->label == am->label;
			}
			T( rt, "Ableton-Belegung in der Show-Datei gespeichert %s", err.c_str() );
			s.ableton = ableBefore;
			inbox.clear();
		}
		//8c named tables for all devices: bind, state in pieces, put, manifest, show id
		{
			const auto ableBefore = s.ableton;
			s.ableton.clear();
			TestOscMsg b1( "/ls/map/bind", "sssi" );
			b1.S( "t1" ).S( "dmx" ).S( "DMX 1" ).I( 1 );
			send( b1.b );
			run( 100 );
			AbletonMap* dm    = s.FindTable( "dmx", "DMX 1" );
			const bool bound  = dm && dm->id == "t1" && s.FindAbleton( "t1" ) == dm;
			const std::string full = "%7B%22mode%22%3A2%2C%22map%22%3A%5B%7B%22f%22%3A1%2C%22s%22%3A17%7D%5D%7D";
			const std::string p1 = full.substr( 0, 20 ), p2 = full.substr( 20, 20 ), p3 = full.substr( 40 );
			for( int part : { 3, 1, 2 } )//out of order
			{
				TestOscMsg st( "/ls/map/state", "siiis" );
				st.S( "t1" ).I( 5 ).I( part ).I( 3 ).S( part == 1 ? p1 : part == 2 ? p2 : p3 );
				send( st.b );
			}
			run( 150 );
			dm               = s.FindTable( "dmx", "DMX 1" );
			const bool state = dm && dm->state == full;
			run( 100 );
			std::string man   = httpGet( "/manifest.json" );
			const bool manOk  = man.find( "\"showId\":\"" + s.showId + "\"" ) != std::string::npos && man.find( "\"kind\":\"dmx\"" ) != std::string::npos &&
			                    man.find( full ) != std::string::npos && man.find( "\"online\":true" ) != std::string::npos;
			//the same device binds to a Control table with its own state (put): the DMX table is released
			TestOscMsg b2( "/ls/map/bind", "sssi" );
			b2.S( "t1" ).S( "control" ).S( "Control 1" ).I( 1 );
			send( b2.b );
			TestOscMsg pt( "/ls/map/put", std::string( 25, 's' ) );
			pt.S( "t1" ).S( "fx:/ls/master" );
			for( int i = 1; i < 24; ++i ) pt.S( "" );
			send( pt.b );
			run( 150 );
			AbletonMap* cm   = s.FindTable( "control", "Control 1" );
			dm               = s.FindTable( "dmx", "DMX 1" );
			const bool moved = cm && cm->id == "t1" && cm->knobs[ 0 ] == "fx:/ls/master" && dm && dm->id.empty() && dm->state == full;
			T( bound && state && manOk && moved, "Belegungen aller Devices: binden %d, Zustand in 3 Stücken %d, Manifest (showId/kind/state/online) %d, Wechsel der Tabelle %d", bound, state, manOk, moved );
			//LightStage-side change while the device is away -> knobsNewer; a state from the device clears it
			if( cm ) cm->knobsNewer = true;
			TestOscMsg st2( "/ls/map/state", "siiis" );
			st2.S( "t1" ).I( 6 ).I( 1 ).I( 1 ).S( "%7B%7D" );
			send( st2.b );
			run( 100 );
			cm = s.FindTable( "control", "Control 1" );
			T( cm && !cm->knobsNewer && cm->state == "%7B%7D", "Zustand vom Device setzt \"Regler hier geändert\" zurück" );
			//save / load: show id, kind, state
			std::string err;
			const std::string fn = dir + "\\selftest_tables.lightstage";
			bool rt = false;
			if( s.Save( fn, &err ) )
			{
				Show s2;
				if( s2.Load( fn, app.lib, &err ) )
				{
					AbletonMap* d2 = s2.FindTable( "dmx", "DMX 1" );
					rt             = s2.showId == s.showId && d2 && d2->state == full && s2.FindTable( "control", "Control 1" );
				}
				//old file without show id: id from the file name, the same on every load
				std::string txt;
				Json j;
				if( rt && ReadText( fn, txt ) && ParseJson( txt, j ) )
				{
					Json o = Json::Obj();
					for( auto& kv : j.o )
						if( kv.first != "showId" ) o.Set( kv.first, kv.second );
					WriteText( fn, WriteJson( o, true ) );
					Show a, b;
					rt = a.Load( fn, app.lib, &err ) && b.Load( fn, app.lib, &err ) && !a.showId.empty() && a.showId == b.showId && a.showId != s.showId;
				}
			}
			T( rt, "Show-ID und Belegungen in der Show-Datei, alte Datei: feste ID aus dem Dateinamen %s", err.c_str() );
			s.ableton = ableBefore;
			inbox.clear();
		}
		//9 ownership: /ls/release gives the channels back, DMX from a device stays until then
		{
			TestOscMsg rel( "/ls/release", "s" );
			rel.S( "t1" );
			send( rel.b );
			run( 150 );
			DmxFrame* fr = s.Frame( 1 );
			bool back = fr && ( *fr )[ 0 ] == dmxBefore[ 0 ] && ( *fr )[ 1 ] == dmxBefore[ 1 ] && ( *fr )[ 2 ] == dmxBefore[ 2 ];
			if( sp ) back = back && s.GetBase( *sp, 0 ) == dmxBefore[ sp->address - 1 ];
			T( back, "/ls/release \"t1\" -> Kanäle wieder auf den Wert vor der Übernahme" );
		}
		//10 timeout: device gone after 8 s, its DMX value stays
		{
			TestOscMsg db( "/ls/dmxblock", "iii" );
			db.I( 1 ).I( 5 ).I( 99 );
			send( db.b );
			run( 150 );
			run( 9000 );
			const bool gone = br.Clients().empty();
			const DmxFrame* fr = s.Frame( 1 );
			T( gone && fr && ( *fr )[ 4 ] == 99, "Timeout 9 s: Gerät weg (%s), DMX bleibt (Kanal 5 = %d)", gone ? "ok" : "noch da", fr ? ( *fr )[ 4 ] : -1 );
		}
		//11 testmanifest for the Ableton side (demo show)
		{
			const std::string ad = dir + "\\ableton";
			const std::string td = ad + "\\test";
			CreateDirectoryW( Widen( ad ).c_str(), nullptr );
			CreateDirectoryW( Widen( td ).c_str(), nullptr );
			const bool ok = WriteText( td + "\\manifest_sample.json", br.ManifestJson( s ) );
			T( ok, "Testmanifest geschrieben: ableton\\test\\manifest_sample.json" );
		}
		//restore the show
		*s.Frame( 1 ) = dmxBefore;
		s.master = masterBefore;
		s.stage.exposure = expBefore;
		s.Bump();
		net::Close( cs );
		oi.Stop();
	}
	//---- effects per OSC / MIDI / bridge: every editor setting is a register entry
	//temporary effects (one per kind) from the built-in presets, removed again at the end of the block
	std::vector< int > fxTmp;
	for( FxKind want : { FxKind::Dimmer, FxKind::Color, FxKind::Position } )
		for( auto& pr : BuiltinFxPresets() )
			if( pr.fx.kind == want )
			{
				const int id = AddEffectFromPreset( s, pr.fx, pr.name, {}, false );
				if( id ) fxTmp.push_back( id );
				break;
			}
	if( !s.effects.empty() )
	{
		const Effect saveFx = s.effects[ 0 ];
		const float fxMasterBefore = s.fxMaster, fxSpeedBefore = s.fxSpeed;
		Effect* dim = nullptr;
		Effect* col = nullptr;
		Effect* pos = nullptr;
		for( auto& e : s.effects )
		{
			if( !dim && e.kind == FxKind::Dimmer ) dim = &e;
			if( !col && e.kind == FxKind::Color ) col = &e;
			if( !pos && e.kind == FxKind::Position ) pos = &e;
		}
		Effect* e0 = &s.effects[ 0 ];
		const std::string b0 = "/ls/fx/" + std::to_string( e0->id );
		auto has = []( Show& sh, const std::string& a ) { return FindParam( sh, a ) != nullptr; };
		const bool dimOk = !dim || ( has( s, "/ls/fx/" + std::to_string( dim->id ) + "/openshutter" ) && has( s, "/ls/fx/" + std::to_string( dim->id ) + "/flash" ) && has( s, "/ls/fx/" + std::to_string( dim->id ) + "/wave" ) &&
		                             has( s, "/ls/fx/" + std::to_string( dim->id ) + "/beatstep" ) && has( s, "/ls/fx/" + std::to_string( dim->id ) + "/fade" ) && has( s, "/ls/fx/" + std::to_string( dim->id ) + "/restart" ) );
		const bool colOk = !col || ( has( s, "/ls/fx/" + std::to_string( col->id ) + "/colormode" ) && has( s, "/ls/fx/" + std::to_string( col->id ) + "/color/1/hue" ) );
		const bool posOk = !pos || ( has( s, "/ls/fx/" + std::to_string( pos->id ) + "/shape" ) && has( s, "/ls/fx/" + std::to_string( pos->id ) + "/sizepan" ) );
		T( dimOk && colOk && posOk && has( s, "/ls/fx/master" ) && has( s, "/ls/fx/speed" ) && has( s, "/ls/fx/hold" ) && has( s, "/ls/fx/selected/level" ) && has( s, "/ls/fx/select/next" ),
		   "Effekt-Register: Shutter, Flash, Welle, Schritte, Fade, Neustart je Typ; global Master/Tempo/Halt; ausgewählter Effekt" );

		//OSC: openshutter via the register (what a MIDI CC does too), flash = on, beatstep, wave, colour
		SetParam( s, b0 + "/openshutter", 0 );
		T( !e0->openShutter || e0->kind != FxKind::Dimmer, "OSC/MIDI %s/openshutter 0 -> Shutter zu (Dimmer-Effekt)", b0.c_str() );
		SetParam( s, b0 + "/openshutter", 1 );
		SetParam( s, b0 + "/flash", 1 );
		T( e0->on, "Flash: Note an -> Effekt an" );
		SetParam( s, b0 + "/flash", 0 );
		T( !e0->on, "Flash: Note aus -> Effekt aus" );
		SetParamNorm( s, b0 + "/beatstep", 4.0f / 8.0f );
		T( fabsf( e0->beats - 4.0f ) < 1e-3f, "beatstep 4 (Norm 0.5) -> beats = %.2f", e0->beats );
		SetParam( s, b0 + "/wave", 4 );
		T( e0->kind != FxKind::Dimmer || e0->wave == FxWave::Square, "wave 4 -> Rechteck" );
		if( col )
		{
			const std::string cb = "/ls/fx/" + std::to_string( col->id );
			SetParam( s, cb + "/color/1/hue", 0.33f );
			const V3 c1 = col->colors[ 0 ];
			T( c1.y > c1.x && c1.y > c1.z, "color/1/hue 0.33 -> Palettenfarbe 1 grün (%.2f %.2f %.2f)", c1.x, c1.y, c1.z );
		}

		//global controls
		SetParam( s, "/ls/fx/master", 0.5f );
		T( fabsf( s.fxMaster - 0.5f ) < 1e-4f, "/ls/fx/master 0.5 -> Effekte gesamt 0.5" );
		SetParam( s, "/ls/fx/speed", 2.0f );
		T( fabsf( s.fxSpeed - 2.0f ) < 1e-4f, "/ls/fx/speed 2 -> Tempo x2" );
		SetParam( s, "/ls/fx/speed", 1.0f );

		//hold freezes the phase
		s.fxHold = true;
		const double ph0 = e0->freePhase;
		e0->on = true;
		RunEffects( s, 0.5 );
		T( fabsf( (float)( e0->freePhase - ph0 ) ) < 1e-9f, "/ls/fx/hold 1 -> Phase steht (0.5 s)" );
		s.fxHold = false;
		//speed scales the free phase
		e0->hz = 1.0f; e0->freePhase = 0; e0->sync = false;
		s.fxSpeed = 1.0f; RunEffects( s, 0.25 ); const double p1 = e0->freePhase;
		e0->freePhase = 0; s.fxSpeed = 2.0f; RunEffects( s, 0.25 ); const double p2 = e0->freePhase;
		s.fxSpeed = 1.0f;
		T( p1 > 0 && fabs( p2 - 2.0 * p1 ) < 1e-6, "Tempo x2 -> doppelte Phasengeschwindigkeit (%.3f / %.3f)", p1, p2 );

		//selected effect: the hardware controls work on the effect chosen in the editor
		SetParam( s, "/ls/fx/select", 1 );
		T( s.fxSelId == s.effects[ 0 ].id, "/ls/fx/select 1 -> erster Effekt ausgewählt" );
		SetParam( s, "/ls/fx/selected/level", 0.2f );
		T( fabsf( e0->level - 0.2f ) < 1e-4f, "/ls/fx/selected/level 0.2 -> wirkt auf den ausgewählten Effekt" );
		SetParam( s, "/ls/fx/select/next", 1 );
		if( s.effects.size() > 1 ) T( s.fxSelId == s.effects[ 1 ].id, "/ls/fx/select/next -> nächster Effekt" );

		//restore
		s.effects[ 0 ] = saveFx;
		s.fxMaster = fxMasterBefore;
		s.fxSpeed  = fxSpeedBefore;
		s.fxSelId  = 0;
		s.Bump();
	}
	s.effects.erase( std::remove_if( s.effects.begin(), s.effects.end(), [ & ]( const Effect& e ) { return std::find( fxTmp.begin(), fxTmp.end(), e.id ) != fxTmp.end(); } ), s.effects.end() );
	s.Bump();
	//---- Phase E: textures with alpha / TIF / search, effects, tempo, parameters, MIDI files, lamp swap
	{
		const std::string td = dir + "\\selftest_tex";
		CreateDirectoryW( Widen( td ).c_str(), nullptr );
		CreateDirectoryW( Widen( td + "\\tex" ).c_str(), nullptr );
		//leaf texture: left half transparent, right half green
		uint8_t leaf[ 16 * 16 * 4 ];
		for( int y = 0; y < 16; ++y )
			for( int x = 0; x < 16; ++x )
			{
				uint8_t* q = &leaf[ ( y * 16 + x ) * 4 ];
				q[ 0 ] = 30; q[ 1 ] = 200; q[ 2 ] = 40; q[ 3 ] = x < 8 ? 0 : 255;
			}
		stbi_write_png( ( td + "\\leaf.png" ).c_str(), 16, 16, 4, leaf, 64 );
		WriteText( td + "\\leaf.gltf", R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}],
"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0},"baseColorFactor":[0,0,0,1],"metallicFactor":1},"alphaMode":"BLEND"}],
"textures":[{"source":0}],"images":[{"uri":"leaf.png"}],
"buffers":[{"uri":"quad.bin","byteLength":140}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":48},{"buffer":0,"byteOffset":96,"byteLength":32},{"buffer":0,"byteOffset":128,"byteLength":12}],
"accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[-1,0,0],"max":[1,2,0]},{"bufferView":1,"componentType":5126,"count":4,"type":"VEC3"},
{"bufferView":2,"componentType":5126,"count":4,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":6,"type":"SCALAR"}]})" );
		MeshData lg;
		std::string le;
		bool okL      = LoadModel( td + "\\leaf.gltf", lg, &le );
		float vcol    = lg.Count() ? lg.v[ 6 ] + lg.v[ 7 ] + lg.v[ 8 ] : 0;
		T( okL && lg.parts.size() == 1 && lg.parts[ 0 ].cutoff > 0 && vcol > 2.9f, "glTF Cinema-4D-Stil (Basisfarbe schwarz, BLEND): Textur weiß gemischt (%.2f), Alpha-Schwelle %.2f %s", vcol, lg.parts.empty() ? 0.0f : lg.parts[ 0 ].cutoff, le.c_str() );
		//uncompressed RGBA TIFF (decoded by WIC), black left / white right as opacity map, stored in tex\ under the name without '~'
		{
			const int w = 16, h = 16;
			std::vector< uint8_t > t;
			auto u16 = [ & ]( int v ) { t.push_back( v & 255 ); t.push_back( ( v >> 8 ) & 255 ); };
			auto u32 = [ & ]( uint32_t v ) { for( int k = 0; k < 4; ++k ) t.push_back( ( v >> ( 8 * k ) ) & 255 ); };
			t = { 'I', 'I', 42, 0 };
			u32( 8 );
			const int n = 11;
			u16( n );
			const uint32_t bitsOff = 8 + 2 + n * 12 + 4, dataOff = bitsOff + 8;
			auto ent = [ & ]( int tag, int type, uint32_t cnt, uint32_t val ) { u16( tag ); u16( type ); u32( cnt ); u32( val ); };
			ent( 256, 4, 1, w );
			ent( 257, 4, 1, h );
			ent( 258, 3, 4, bitsOff );
			ent( 259, 3, 1, 1 );
			ent( 262, 3, 1, 2 );
			ent( 273, 4, 1, dataOff );
			ent( 277, 3, 1, 4 );
			ent( 278, 4, 1, h );
			ent( 279, 4, 1, w * h * 4 );
			ent( 284, 3, 1, 1 );
			ent( 338, 3, 1, 2 );
			u32( 0 );
			for( int k = 0; k < 4; ++k ) u16( 8 );
			for( int y = 0; y < h; ++y )
				for( int x = 0; x < w; ++x )
				{
					uint8_t v = x < 8 ? 0 : 255;
					t.insert( t.end(), { v, v, v, 255 } );
				}
			FILE* f = fopen( ( td + "\\tex\\alpha.tif" ).c_str(), "wb" );
			if( f ) { fwrite( t.data(), 1, t.size(), f ); fclose( f ); }
			MeshImage ti;
			bool okT = DecodeImage( t.data(), t.size(), ti );
			T( okT && ti.w == 16 && ti.rgba[ ( 8 * 16 + 12 ) * 4 ] == 255 && ti.rgba[ ( 8 * 16 + 2 ) * 4 ] == 0, "TIF über Windows-Decoder: %dx%d", ti.w, ti.h );
		}
		WriteText( td + "\\leaf.mtl", "newmtl leaf\nKa 1 1 1\nKd 0 0 0\nmap_d asset:///alpha~.tif\n\nnewmtl miss\nKd 0.8 0.8 0.8\nmap_Kd asset:///gibtsnicht.png\n" );
		WriteText( td + "\\leaf.obj", "mtllib leaf.mtl\nv -1 0 0\nv 1 0 0\nv 1 2 0\nv -1 2 0\nv 2 0 0\nv 3 0 0\nv 3 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\nusemtl leaf\nf 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\nusemtl miss\nf 5/1/1 6/2/1 7/3/1\n" );
		MeshData lo;
		std::string loe;
		bool okLo   = LoadModel( td + "\\leaf.obj", lo, &loe );
		float cutLo = 0, baseLo = 0;
		for( auto& p : lo.parts )
			if( p.cutoff > 0 ) { cutLo = p.cutoff; baseLo = lo.v[ lo.idx[ p.first ] * MeshData::kStride + 6 ]; }
		std::string miss;
		for( auto& m : lo.missing ) miss += m + " ";
		T( okLo && cutLo > 0 && baseLo > 0.5f && lo.missing.size() == 1 && miss.find( "gibtsnicht" ) != std::string::npos, "OBJ map_d (asset:///alpha~.tif in tex\\) als Alpha, Kd 0 -> grau %.2f; fehlend gemeldet: %s", baseLo, miss.c_str() );
		//rendered: left half of the leaf quad is cut out, right half green
		{
			ModelObj mo;
			mo.path  = td + "\\leaf.gltf";
			mo.name  = "leaf";
			mo.pos   = V3( 0, 1.2f, 6.0f );
			s.models.push_back( mo );
			Camera camOld   = app.cam;
			float ambOld    = s.stage.ambient, masterOld = s.master;
			s.stage.ambient = 1.5f;
			s.master        = 0;
			Run( app, 0.3f );
			app.cam.target = V3( 0, 2.2f, 6.0f );
			app.cam.yaw    = 0;
			app.cam.pitch  = 0;
			app.cam.dist   = 4;
			SavePng( app, dir + "\\selftest_alpha.png", 640, 360 );
			std::vector< uint8_t > px;
			int W = 0, H = 0;
			app.ren.ReadOutput( px, W, H );
			auto at = [ & ]( float fx, float fy, int c ) { int x = (int)( fx * W ), y = (int)( fy * H ); return (int)px[ ( (size_t)y * W + x ) * 4 + c ]; };
			int gl = at( 0.40f, 0.5f, 1 ), rl = at( 0.40f, 0.5f, 0 ), gr = at( 0.60f, 0.5f, 1 ), rr = at( 0.60f, 0.5f, 0 );
			T( gr > rr + 60 && !( gl > rl + 60 ), "Alpha im Bild: rechts grün (G %d / R %d), links ausgeschnitten (G %d / R %d)", gr, rr, gl, rl );
			s.models.pop_back();
			s.stage.ambient = ambOld;
			s.master        = masterOld;
			app.cam         = camOld;
		}
		//the user's test models (Downloads\tree.*), diagnostics only
		{
			wchar_t up[ MAX_PATH ] = {};
			GetEnvironmentVariableW( L"USERPROFILE", up, MAX_PATH );
			const std::string dl = Narrow( up ) + "\\Downloads\\";
			for( const char* fn : { "tree.gltf", "tree.glb", "tree.obj", "tree.fbx" } )
			{
				if( GetFileAttributesW( Widen( dl + fn ).c_str() ) == INVALID_FILE_ATTRIBUTES ) continue;
				MeshData m;
				std::string er;
				bool ok = LoadModel( dl + fn, m, &er );
				int cut = 0;
				for( auto& p : m.parts ) cut += p.cutoff > 0 ? 1 : 0;
				std::string ms;
				for( auto& x : m.missing ) ms += x + " ";
				Info( "%s: %s, %d Teile, %d texturiert, %d Bilder, %d mit Alpha, fehlend: %s", fn, ok ? "ok" : er.c_str(), (int)m.parts.size(), m.Textured(), (int)m.images.size(), cut, ms.empty() ? "-" : ms.c_str() );
				if( std::string( fn ) == "tree.gltf" )
				{
					T( ok && m.Textured() > 0 && cut > 0, "tree.gltf: Texturen %d, Alpha-Teile %d", m.Textured(), cut );
					//picture with the work light, tree scaled to 5 m
					ModelObj mo;
					mo.path  = dl + fn;
					mo.name  = "tree";
					const float hgt = std::max( 0.01f, m.bmax.y - m.bmin.y );
					mo.scale = 5.0f / hgt;
					mo.pos   = V3( -( m.bmin.x + m.bmax.x ) * 0.5f * mo.scale, -m.bmin.y * mo.scale, 6.0f - ( m.bmin.z + m.bmax.z ) * 0.5f * mo.scale );
					s.models.push_back( mo );
					Camera camOld = app.cam;
					float mOld    = s.master;
					s.master      = 0;
					s.workLight   = true;
					Run( app, 0.3f );
					app.cam.target = V3( 0, 2.6f, 6.0f );
					app.cam.yaw    = 20;
					app.cam.pitch  = 8;
					app.cam.dist   = 9;
					SavePng( app, dir + "\\selftest_tree.png", 960, 720 );
					s.models.pop_back();
					s.workLight = false;
					s.master    = mOld;
					app.cam     = camOld;
				}
			}
		}
	}
	{
		//fresh show with 4 RGB PARs and a moving head
		app.NewShow();
		const FixtureDef* par = nullptr;
		for( auto& d : app.lib.defs )
		{
			if( d->id.rfind( "generic/", 0 ) != 0 || d->kind != Kind::Par ) continue;
			FixtureInst tmp;
			tmp.def = d.get();
			if( s.HasAttr( tmp, Attr::Dimmer ) && s.HasAttr( tmp, Attr::Red ) ) { par = d.get(); break; }
		}
		const FixtureDef* mh = app.lib.Find( "ofl/martin/mac-aura" );
		T( par && mh, "Effekt-Test: PAR %s, Moving Head %s", par ? par->name.c_str() : "-", mh ? mh->name.c_str() : "-" );
		std::vector< int > pars;
		for( int i = 0; i < 4 && par; ++i )
			if( FixtureInst* f = app.AddFixture( par, 0, 1, 1 + i * 16, V3( -3.0f + 2.0f * i, 6, -2 ), V3( 150, 0, 0 ), "" ) ) pars.push_back( f->id );
		FixtureInst* m1 = mh ? app.AddFixture( mh, 0, 1, 200, V3( 0, 6, 0 ), V3( 180, 0, 0 ), "" ) : nullptr;
		auto preset = [ & ]( const char* n ) -> const Effect* {
			for( auto& p : BuiltinFxPresets() )
				if( p.name == n ) return &p.fx;
			return nullptr;
		};
		s.tempo.source = TempoSource::Internal;
		s.tempo.bpm    = 120;
		s.tempo.beat   = 0;
		auto step = [ & ]( float sec ) {
			for( int i = 0; i < (int)( sec * 60 ); ++i )
			{
				TempoAdvance( s.tempo, 1.0 / 60.0, Now() );
				RunEffects( s, 1.0 / 60.0 );
			}
		};
		for( int id : pars )
			for( Attr a : { Attr::Red, Attr::Green, Attr::Blue } ) s.SetAttr( *s.FindFixture( id ), a, 1 );
		int chase = AddEffectFromPreset( s, *preset( "Lauflicht" ), "Lauflicht", pars, true );
		step( 0.6f );
		s.tempo.beat = 0.1;//phase 0.05 of a 2-beat cycle: element 0 on, the others off
		RunEffects( s, 0.001 );
		int full = 0, dark = 0;
		for( int id : pars )
		{
			float v = s.GetAttr( *s.FindFixture( id ), Attr::Dimmer );
			full += v > 0.95f;
			dark += v < 0.05f;
		}
		const FixtureInst* p0 = s.FindFixture( pars[ 0 ] );
		T( chase && full == 1 && dark == 3 && s.GetAttrBase( *p0, Attr::Dimmer ) < 0.01f, "Lauflicht: %d Lampe an, %d aus, Grundwert bleibt %.2f (Overlay)", full, dark, s.GetAttrBase( *p0, Attr::Dimmer ) );
		Run( app, 0.3f );
		T( s.FindFixture( pars[ 0 ] )->st.intensity > 0.3f, "Simulation sieht den Effekt: Intensität %.2f", s.FindFixture( pars[ 0 ] )->st.intensity );
		s.FindEffect( chase )->on = false;
		step( 0.5f );
		T( !s.fxAny && s.GetAttr( *p0, Attr::Dimmer ) < 0.01f, "Effekt aus: Overlay leer, Dimmer wieder %.2f", s.GetAttr( *p0, Attr::Dimmer ) );
		//colour wave: the lamps get different colours
		for( int id : pars ) s.SetAttr( *s.FindFixture( id ), Attr::Dimmer, 1 );
		int rb = AddEffectFromPreset( s, *preset( "Regenbogen-Welle" ), "Regenbogen", pars, true );
		step( 0.6f );
		float r0 = s.GetAttr( *s.FindFixture( pars[ 0 ] ), Attr::Red ), r2 = s.GetAttr( *s.FindFixture( pars[ 2 ] ), Attr::Red ), g0 = s.GetAttr( *s.FindFixture( pars[ 0 ] ), Attr::Green ), g2 = s.GetAttr( *s.FindFixture( pars[ 2 ] ), Attr::Green );
		T( rb && ( fabsf( r0 - r2 ) > 0.2f || fabsf( g0 - g2 ) > 0.2f ), "Regenbogen-Welle: Lampe 1 R %.2f G %.2f, Lampe 3 R %.2f G %.2f", r0, g0, r2, g2 );
		s.FindEffect( rb )->on = false;
		//circle on the moving head
		int circ = m1 ? AddEffectFromPreset( s, *preset( "Kreis" ), "Kreis", { m1->id }, true ) : 0;
		step( 1.0f );
		float pb = m1 ? s.GetAttrBase( *m1, Attr::Pan ) : 0, pc = m1 ? s.GetAttr( *m1, Attr::Pan ) : 0;
		step( 0.5f );
		float pc2 = m1 ? s.GetAttr( *m1, Attr::Pan ) : 0;
		T( circ && fabsf( pc - pc2 ) > 0.005f && fabsf( pb - 0.5f ) < 0.02f, "Kreis: Pan bewegt sich (%.3f -> %.3f), Grundwert %.3f", pc, pc2, pb );
		StopAllEffects( s );
		step( 0.5f );
		//program template
		int pi = InstantiateProgram( s, BuiltinPrograms()[ 0 ] );
		StartProgram( s, pi, true );
		T( pi >= 0 && ActiveProgram( s ) == pi && s.programs[ pi ].effects.size() >= 2, "Programm '%s': %d Effekte, aktiv", pi >= 0 ? s.programs[ pi ].name.c_str() : "-", pi >= 0 ? (int)s.programs[ pi ].effects.size() : 0 );
		StopAllEffects( s );
		step( 0.5f );
		//tap tempo + MIDI clock
		{
			Tempo t;
			for( int i = 0; i < 4; ++i ) TempoTap( t, 100.0 + i * 0.5 );
			T( fabsf( t.bpm - 120 ) < 0.5f, "Tap-Tempo: %.1f BPM", t.bpm );
			Tempo c;
			c.source = TempoSource::MidiClock;
			double tm = 200.0;
			TempoMidiClock( c, 0xFA, 0, tm );
			const double per = 60.0 / ( 128.0 * 24.0 );
			for( int i = 0; i < 48; ++i ) { tm += per; TempoMidiClock( c, 0xF8, 0, tm ); }
			TempoAdvance( c, 0.001, tm + 0.001 );
			T( fabsf( c.liveBpm - 128 ) < 0.5f && fabs( c.beat - 47.0 / 24.0 ) < 0.06, "MIDI-Clock: %.2f BPM, Beat %.3f (erwartet 1.958)", c.liveBpm, c.beat );
		}
		//MIDI clock end to end over the LoopBe port (if installed)
		{
			MidiOut h;
			if( h.Open( "loopbe" ) )
			{
				std::vector< MidiEvent > ev;
				app.midi.Poll( ev );
				app.midi.clock.clear();
				Tempo c;
				c.source        = TempoSource::MidiClock;
				const double per = 60.0 / ( 120.0 * 24.0 );
				h.Send( 0xFA );
				const double t0 = Now();
				for( int i = 0; i < 72; ++i )
				{
					while( Now() < t0 + ( i + 1 ) * per ) Sleep( 0 );
					h.Send( 0xF8 );
				}
				Sleep( 60 );
				h.Close();
				ev.clear();
				app.midi.Poll( ev );
				int nClock = 0;
				for( auto& m : app.midi.clock )
				{
					TempoMidiClock( c, m.status, m.spp, m.t );
					++nClock;
				}
				app.midi.clock.clear();
				if( nClock == 0 )
				{
					//LoopBe muted: feed the same bytes into the input queue (parser + timing path of MidiIn)
					{
						std::lock_guard< std::mutex > l( app.midi.lock_ );
						const double tb = Now();
						app.midi.queue_.push_back( { 0, 0xFA, tb } );
						for( int i = 0; i < 72; ++i ) app.midi.queue_.push_back( { 0, 0xF8, tb + ( i + 1 ) * per } );
					}
					app.midi.Poll( ev );
					for( auto& m : app.midi.clock )
					{
						TempoMidiClock( c, m.status, m.spp, m.t );
						++nClock;
					}
					app.midi.clock.clear();
					Info( "LoopBe stummgeschaltet - Clock-Bytes intern eingespeist" );
				}
				T( nClock == 73 && fabsf( c.liveBpm - 120 ) < 2.0f && c.ticks == 71, "MIDI-Clock über LoopBe: %d Nachrichten, %.1f BPM gemessen, Tick %lld", nClock, c.liveBpm, c.ticks );
			}
			else
				Info( "MIDI-Clock über LoopBe: kein LoopBe-Ausgang, übersprungen" );
		}
		//parameters: OSC + MIDI
		{
			const size_t np = Params( s ).size();
			SetParam( s, "/ls/stage/exposure", 2.0f );
			OscMsg om;
			om.address = "/ls/worklight";
			om.f       = { 1 };
			om.isInt   = { false };
			bool a1    = ApplyOsc( s, om );
			om.address = "/ls/fx/" + std::to_string( chase ) + "/on";
			bool a2    = ApplyOsc( s, om );
			MidiMap mm;
			mm.type   = MidiType::CC;
			mm.number = 20;
			mm.target = MidiTarget::Param;
			mm.param  = "/ls/haze";
			s.midi.push_back( mm );
			MidiEvent ev;
			ev.type   = MidiType::CC;
			ev.number = 20;
			ev.value  = 0.25f;
			ApplyMidi( s, ev );
			T( np > 80 && fabsf( s.stage.exposure - 2 ) < 1e-4f && a1 && s.workLight && a2 && s.FindEffect( chase )->on && fabsf( s.stage.haze - 0.25f ) < 0.01f,
			   "Parameter: %d Adressen; OSC Arbeitslicht + Effekt an, MIDI-CC -> Haze %.2f, Belichtung %.1f", (int)np, s.stage.haze, s.stage.exposure );
			s.FindEffect( chase )->on = false;
			s.workLight               = false;
			s.stage.exposure          = 1;
			//MIDI mapping file
			const std::string mf = dir + "\\selftest_midi.lsmidi";
			bool ex              = app.ExportMidi( mf );
			s.midi.clear();
			int n = app.ImportMidi( mf, true );
			T( ex && n == 1 && s.midi.size() == 1 && s.midi[ 0 ].param == "/ls/haze" && s.midi[ 0 ].target == MidiTarget::Param, "MIDI-Mapping exportiert/importiert: %d Zuweisung(en), Ziel %s", n, s.midi.empty() ? "-" : s.midi[ 0 ].Describe( s ).c_str() );
		}
		//screen free format
		{
			Screen& sc = s.AddScreen();
			sc.format  = 5;
			sc.height  = 2.0f;
			const int si = (int)s.screens.size();
			SetParam( s, "/ls/screen/" + std::to_string( si ) + "/width", 7.0f );
			bool freeOk = fabsf( s.screens.back().height - 2.0f ) < 1e-4f && fabsf( s.screens.back().width - 7 ) < 1e-4f;
			s.screens.back().format = 0;
			SetParam( s, "/ls/screen/" + std::to_string( si ) + "/width", 3.2f );
			T( freeOk && fabsf( s.screens.back().height - 1.8f ) < 0.01f, "Leinwand: frei 7.0 x 2.0 m, 16:9 -> Höhe %.2f", s.screens.back().height );
			s.screens.pop_back();
		}
		//show file with effects / programs / tempo
		{
			int pi2      = InstantiateProgram( s, BuiltinPrograms()[ 1 ] );
			s.tempo.bpm  = 133;
			StartProgram( s, 0, true );
			std::string err;
			bool sv = s.Save( dir + "\\selftest_fx.lightstage", &err );
			Show t;
			bool ld = t.Load( dir + "\\selftest_fx.lightstage", app.lib, &err );
			T( sv && ld && t.effects.size() == s.effects.size() && t.programs.size() == s.programs.size() && fabsf( t.tempo.bpm - 133 ) < 0.01f && pi2 >= 0,
			   "Show mit %d Effekten, %d Programmen, %.0f BPM gespeichert/geladen %s", (int)t.effects.size(), (int)t.programs.size(), t.tempo.bpm, err.c_str() );
		}
		//swap lamp type: position stays, colliding address moves
		if( mh && par && pars.size() >= 2 )
		{
			FixtureInst* f = s.FindFixture( pars[ 0 ] );
			const V3 pos0  = f->pos;
			int n          = app.SwapFixtureType( { pars[ 0 ] }, mh, 0, true );
			f              = s.FindFixture( pars[ 0 ] );
			T( n == 1 && f->def == mh && Len( f->pos - pos0 ) < 1e-5f && !s.Overlaps( f->universe, f->address, f->Footprint(), f->id ), "Lampe getauscht: %s an gleicher Position, Adresse %d.%03d (%d Kanäle, kein Konflikt)", f->def->name.c_str(), f->universe, f->address, f->Footprint() );
		}
		//delete an own lamp type that is not used
		{
			const FixtureDef* u = nullptr;
			for( auto& d : app.lib.defs )
				if( d->id == "user/ls-test/test-rgbw-9ch" ) u = d.get();
			if( u )
			{
				const std::string src = u->source, id = u->id;
				std::string why;
				bool del = app.DeleteUserFixture( u, &why );
				T( del && !app.lib.Find( id ) && GetFileAttributesW( Widen( src ).c_str() ) == INVALID_FILE_ATTRIBUTES, "Eigene Lampe gelöscht (%s) %s", id.c_str(), why.c_str() );
			}
			else
				T( false, "Eigene Test-Lampe für den Lösch-Test nicht gefunden" );
		}
		//work light: dark stage without lamps becomes readable
		{
			app.NewShow();
			s.stage.ambient = 0.0f;
			Camera camOld   = app.cam;
			app.cam         = Camera();
			float l0 = 0, l1 = 0;
			SavePng( app, dir + "\\selftest_worklight_off.png", 640, 360, &l0 );
			s.workLight = true;
			SavePng( app, dir + "\\selftest_worklight_on.png", 640, 360, &l1 );
			s.workLight = false;
			app.cam     = camOld;
			T( l1 > l0 * 2 + 5, "Arbeitslicht: Helligkeit %.1f -> %.1f", l0, l1 );
		}
		//Ableton Link session (no peers needed)
		{
			LinkSync ln;
			ln.Enable( true );
			double bpm = 0, beat = 0, ph = 0;
			bool cap = ln.Capture( bpm, beat, ph, 4 );
			ln.SetTempo( 97 );
			double bpm2 = 0;
			ln.Capture( bpm2, beat, ph, 4 );
			ln.Enable( false );
			T( cap && fabs( bpm2 - 97 ) < 0.01, "Ableton Link: Sitzung aktiv (%.1f BPM -> %.1f), %d Teilnehmer", bpm, bpm2, ln.Peers() );
		}
		app.BuildDemo();
	}
	//---- snapshots (master, fade, lamp choice, effects, projectors, parallel), group faders, projector lines, editor names
	{
		app.NewShow();
		const FixtureDef* par = nullptr;
		for( auto& d : app.lib.defs )
		{
			if( d->id.rfind( "generic/", 0 ) != 0 || d->kind != Kind::Par ) continue;
			FixtureInst tmp;
			tmp.def = d.get();
			if( s.HasAttr( tmp, Attr::Dimmer ) && s.HasAttr( tmp, Attr::Red ) && s.HasAttr( tmp, Attr::Blue ) ) { par = d.get(); break; }
		}
		std::vector< int > ids;
		for( int i = 0; i < 4 && par; ++i )
			if( FixtureInst* f = app.AddFixture( par, 0, 1, 1 + i * 16, V3( -3.0f + 2.0f * i, 6, -2 ), V3( 150, 0, 0 ), "" ) ) ids.push_back( f->id );
		T( ids.size() == 4, "Snapshot-Test: 4 x %s", par ? par->name.c_str() : "-" );
		if( ids.size() == 4 )
		{
			auto F    = [ & ]( int k ) -> FixtureInst& { return *s.FindFixture( ids[ k ] ); };
			auto at   = [ & ]( int k, Attr a ) { return s.GetAttr( F( k ), a ); };
			auto step = [ & ]( float sec ) {
				for( int i = 0; i < std::max( 1, (int)( sec * 60 ) ); ++i ) RunEffects( s, 1.0 / 60.0 );
			};
			auto look = [ & ]( std::initializer_list< int > ks, float d, float r, float g, float b ) {
				for( int k = 0; k < 4; ++k )
					for( Attr a : { Attr::Dimmer, Attr::Red, Attr::Green, Attr::Blue } ) s.SetAttr( F( k ), a, 0 );
				for( int k : ks )
				{
					s.SetAttr( F( k ), Attr::Dimmer, d );
					s.SetAttr( F( k ), Attr::Red, r );
					s.SetAttr( F( k ), Attr::Green, g );
					s.SetAttr( F( k ), Attr::Blue, b );
				}
			};
			look( { 0, 1 }, 1, 1, 0, 0 );
			const int A = s.TakeSnapshotOf( "A rot", { ids[ 0 ], ids[ 1 ] }, false, false );
			look( { 2, 3 }, 1, 0, 0, 1 );
			const int B = s.TakeSnapshotOf( "B blau", { ids[ 2 ], ids[ 3 ] }, false, false );
			look( { 0, 1, 2, 3 }, 0.3f, 0, 1, 0 );
			const int C = s.TakeSnapshotOf( "C grün", {}, false, false );
			look( {}, 0, 0, 0, 0 );
			T( s.snapshots[ A ].fix.size() == 2 && s.snapshots[ A ].Has( ids[ 1 ] ) && !s.snapshots[ A ].Has( ids[ 2 ] ), "Snapshot nur mit ausgewählten Lampen: A %d Lampen, B %d Lampen", (int)s.snapshots[ A ].fix.size(), (int)s.snapshots[ B ].fix.size() );
			//fade in
			s.snapshots[ A ].fade = 1.0f;
			s.RecallSnapshot( A );
			step( 0.5f );
			const float d05 = at( 0, Attr::Dimmer );
			step( 0.6f );
			const float d1 = at( 0, Attr::Dimmer );
			T( d05 > 0.35f && d05 < 0.65f && d1 > 0.99f && at( 2, Attr::Dimmer ) < 0.01f && s.GetAttrBase( F( 0 ), Attr::Dimmer ) < 0.01f, "Fade 1 s: Dimmer nach 0,5 s %.2f, nach 1,1 s %.2f; andere Lampe %.2f; Programmer bleibt %.2f", d05, d1, at( 2, Attr::Dimmer ), s.GetAttrBase( F( 0 ), Attr::Dimmer ) );
			//parallel
			s.snapshots[ B ].fade = 0;
			s.RecallSnapshot( B );
			step( 0.05f );
			T( s.ActiveSnapshots() == 2 && at( 0, Attr::Red ) > 0.99f && at( 0, Attr::Dimmer ) > 0.99f && at( 3, Attr::Blue ) > 0.99f && at( 3, Attr::Dimmer ) > 0.99f, "Zwei Snapshots parallel: A rot %.2f, B blau %.2f", at( 0, Attr::Red ), at( 3, Attr::Blue ) );
			//strength
			s.snapshots[ A ].master = 0.5f;
			step( 0.05f );
			T( fabsf( at( 0, Attr::Dimmer ) - 0.5f ) < 0.02f && at( 0, Attr::Red ) > 0.99f && at( 3, Attr::Dimmer ) > 0.99f, "Stärke 50 %%: Dimmer A %.2f (Farbe bleibt %.2f), B %.2f", at( 0, Attr::Dimmer ), at( 0, Attr::Red ), at( 3, Attr::Dimmer ) );
			s.snapshots[ A ].master = 1.0f;
			//OSC / register / MIDI
			{
				OscMsg m;
				m.address = "/ls/snapshot/1/master";
				m.f       = { 0.25f };
				m.isInt   = { false };
				ApplyOsc( s, m );
				const float viaOsc = s.snapshots[ A ].master;
				SetParam( s, "/ls/snapshot/1/fade", 2.5f );
				SetParam( s, "/ls/snapshot/2/on", 0 );
				const bool bOff = !s.snapshots[ B ].active;
				SetParam( s, "/ls/snapshot/2/on", 1 );
				MidiMap mm;
				mm.target   = MidiTarget::Snapshot;
				mm.snapshot = B;
				mm.type     = MidiType::Note;
				mm.number   = 61;
				mm.toggle   = true;
				s.midi.push_back( mm );
				MidiEvent ev;
				ev.type   = MidiType::Note;
				ev.number = 61;
				ev.value  = 1;
				ev.noteOn = true;
				ApplyMidi( s, ev );
				const bool midiOff = !s.snapshots[ B ].active;
				ev.noteOn = false;
				ev.value  = 0;
				ApplyMidi( s, ev );
				ev.noteOn = true;
				ev.value  = 1;
				ApplyMidi( s, ev );
				const bool midiOn = s.snapshots[ B ].active;
				s.midi.pop_back();
				T( fabsf( viaOsc - 0.25f ) < 0.001f && fabsf( s.snapshots[ A ].fade - 2.5f ) < 0.001f && bOff && s.snapshots[ B ].active && midiOff && midiOn, "OSC /ls/snapshot/1/master -> %.2f, Fade per Register %.1f s, an/aus per Register, MIDI-Note schaltet um (%d/%d)", viaOsc, s.snapshots[ A ].fade, midiOff, midiOn );
				s.snapshots[ A ].master = 1.0f;
				s.snapshots[ A ].fade   = 1.0f;
			}
			//a later snapshot with the same lamps wins; covered ones end when it is fully in
			s.snapshots[ C ].fade = 0.5f;
			s.RecallSnapshot( C );
			step( 0.25f );
			const float mixG = at( 0, Attr::Green ), mixR = at( 0, Attr::Red );
			step( 0.4f );
			T( mixG > 0.3f && mixG < 0.7f && mixR > 0.3f && mixR < 0.7f && at( 0, Attr::Green ) > 0.99f && at( 0, Attr::Red ) < 0.01f && fabsf( at( 0, Attr::Dimmer ) - 0.3f ) < 0.02f && !s.snapshots[ A ].active && !s.snapshots[ B ].active,
			   "Überblendung A -> C: Mitte R %.2f G %.2f, Ende G %.2f D %.2f; A und B beendet (%d %d)", mixR, mixG, at( 0, Attr::Green ), at( 0, Attr::Dimmer ), s.snapshots[ A ].active, s.snapshots[ B ].active );
			s.ReleaseSnapshot( C );
			step( 1.2f );
			T( at( 0, Attr::Dimmer ) < 0.01f && !s.snapAny && !s.fxAny, "Ausblenden: Dimmer %.2f, Ebene leer", at( 0, Attr::Dimmer ) );
			//Shift: started on top without ending the older one
			{
				const float fa = s.snapshots[ A ].fade, fc = s.snapshots[ C ].fade;
				s.snapshots[ A ].fade = s.snapshots[ C ].fade = 0;
				s.RecallSnapshot( A );
				step( 0.05f );
				s.ToggleSnapshot( C, true );
				step( 0.1f );
				const bool both = s.snapshots[ A ].active && s.snapshots[ C ].active && at( 0, Attr::Green ) > 0.99f;
				s.ReleaseSnapshot( C );
				step( 0.1f );
				const bool aBack = at( 0, Attr::Red ) > 0.99f && s.snapshots[ A ].active;
				s.ReleaseAllSnapshots();
				step( 0.1f );
				s.snapshots[ A ].fade = fa;
				s.snapshots[ C ].fade = fc;
				T( both && aBack, "Shift+Start: C über A, A bleibt an; C aus -> A wieder sichtbar" );
			}
			//effects stored in a snapshot run while it is on
			for( int k = 0; k < 4; ++k ) s.SetAttr( F( k ), Attr::Dimmer, 0 );
			Effect ce;
			ce.kind  = FxKind::Dimmer;
			ce.wave  = FxWave::Square;
			ce.sync  = false;
			ce.hz    = 1;
			ce.fade  = 0;
			ce.order = FxOrder::Selection;
			int eid = AddEffectFromPreset( s, ce, "Snap-Effekt", ids, true );
			step( 0.1f );
			const int E = s.TakeSnapshotOf( "E mit Effekt", { ids[ 0 ] }, true, false );
			StopAllEffects( s );
			step( 0.2f );
			const bool offBefore = !s.fxAny;
			s.snapshots[ E ].fade = 0;
			s.RecallSnapshot( E );
			step( 0.1f );
			Effect* ep = s.FindEffect( eid );
			float lo = 1, hi = 0;
			for( int i = 0; i < 70; ++i )
			{
				RunEffects( s, 1.0 / 60.0 );
				lo = std::min( lo, at( 0, Attr::Dimmer ) );
				hi = std::max( hi, at( 0, Attr::Dimmer ) );
			}
			const float other = at( 2, Attr::Dimmer );
			T( eid && offBefore && s.snapshots[ E ].effects.size() == 1 && ep && ep->snapW > 0.99f && !ep->on && hi - lo > 0.5f && other < 0.01f, "Effekt im Snapshot: gespeichert %d, läuft mit Snapshot auf seiner Lampe (Dimmer %.2f .. %.2f), nicht auf fremden (%.2f), Schalter bleibt aus", (int)s.snapshots[ E ].effects.size(), lo, hi, other );
			s.ReleaseSnapshot( E );
			step( 0.1f );
			T( ep && ep->Drive() < 0.01f && !s.fxAny, "Snapshot aus: Effekt steht wieder (%.2f)", ep ? ep->Drive() : -1.0f );
			//a snapshot taken with the effect off switches a running effect off while it is on
			{
				const int O = s.TakeSnapshotOf( "O Effekt aus", { ids[ 0 ] }, true, false );
				ep->on = true;
				step( 0.2f );
				const bool runs = ep->Drive() > 0.99f;
				s.snapshots[ O ].fade = 0;
				s.RecallSnapshot( O );
				step( 0.1f );
				bool stopped = ep->DriveOf( ids[ 0 ] ) < 0.01f && ep->DriveOf( ids[ 2 ] ) > 0.99f && ep->on;
				float lo2 = 1, hi2 = 0, lo3 = 1, hi3 = 0;
				for( int i = 0; i < 70; ++i )
				{
					RunEffects( s, 1.0 / 60.0 );
					lo2 = std::min( lo2, at( 0, Attr::Dimmer ) );
					hi2 = std::max( hi2, at( 0, Attr::Dimmer ) );
					lo3 = std::min( lo3, at( 2, Attr::Dimmer ) );
					hi3 = std::max( hi3, at( 2, Attr::Dimmer ) );
				}
				stopped = stopped && hi3 - lo3 > 0.5f;//other lamps keep the effect
				s.ReleaseSnapshot( O );
				step( 0.1f );
				const bool back = ep->Drive() > 0.99f;
				ep->on = false;
				step( 0.2f );
				T( runs && stopped && hi2 - lo2 < 0.01f && back && std::find( s.snapshots[ O ].fxOff.begin(), s.snapshots[ O ].fxOff.end(), eid ) != s.snapshots[ O ].fxOff.end(), "Snapshot mit Effekt aus: schaltet laufenden Effekt ab (Dimmer %.2f .. %.2f), danach läuft er wieder", lo2, hi2 );
				s.snapshots.erase( s.snapshots.begin() + O );
			}
			//Davids case: lamps 1+2 red (effect off), lamps 3+4 with the effect on -> both snapshots together
			{
				look( { 0, 1, 2, 3 }, 1, 1, 0, 0 );
				ep->on = false;
				step( 0.1f );
				const int R = s.TakeSnapshotOf( "Rot 1+2", { ids[ 0 ], ids[ 1 ] }, true, false );
				ep->on = true;
				step( 0.1f );
				const int W = s.TakeSnapshotOf( "Effekt 3+4", { ids[ 2 ], ids[ 3 ] }, true, false );
				ep->on = false;
				look( {}, 0, 0, 0, 0 );
				step( 0.1f );
				s.snapshots[ R ].fade = s.snapshots[ W ].fade = 0;
				s.RecallSnapshot( R );
				s.RecallSnapshot( W );
				step( 0.05f );
				float l0 = 1, h0 = 0, l2 = 1, h2 = 0;
				for( int i = 0; i < 70; ++i )
				{
					RunEffects( s, 1.0 / 60.0 );
					l0 = std::min( l0, at( 0, Attr::Dimmer ) );
					h0 = std::max( h0, at( 0, Attr::Dimmer ) );
					l2 = std::min( l2, at( 2, Attr::Dimmer ) );
					h2 = std::max( h2, at( 2, Attr::Dimmer ) );
				}
				const bool red = at( 0, Attr::Red ) > 0.99f;
				s.ReleaseAllSnapshots();
				step( 0.1f );
				T( l0 > 0.99f && red && h2 - l2 > 0.5f, "Zwei Teil-Snapshots zusammen: Lampe 1 rot konstant (%.2f .. %.2f), Lampe 3 mit Effekt (%.2f .. %.2f)", l0, h0, l2, h2 );
				s.snapshots.erase( s.snapshots.begin() + W );
				s.snapshots.erase( s.snapshots.begin() + R );
			}
			//an effect only on other lamps is not part of a partial snapshot
			{
				Effect oe;
				oe.kind = FxKind::Dimmer;
				oe.fade = 0;
				int other = AddEffectFromPreset( s, oe, "Nur Lampe 4", { ids[ 3 ] }, false );
				const int Q = s.TakeSnapshotOf( "Q Teil", { ids[ 0 ] }, true, false );
				const Snapshot& q = s.snapshots[ Q ];
				const bool hasOther = std::find( q.effects.begin(), q.effects.end(), other ) != q.effects.end() || std::find( q.fxOff.begin(), q.fxOff.end(), other ) != q.fxOff.end();
				const bool hasOwn   = std::find( q.fxOff.begin(), q.fxOff.end(), eid ) != q.fxOff.end();
				T( other && !hasOther && hasOwn, "Teil-Snapshot speichert nur Effekte seiner Lampen (fremder Effekt %s, eigener %s)", hasOther ? "drin" : "nicht drin", hasOwn ? "drin" : "fehlt" );
				s.snapshots.erase( s.snapshots.begin() + Q );
				s.effects.erase( std::remove_if( s.effects.begin(), s.effects.end(), [ & ]( const Effect& e ) { return e.id == other; } ), s.effects.end() );
			}
			//projector settings
			{
				Projector& p = s.AddProjector();
				p.lumens     = 20000;
				p.pan        = 10;
				const int P  = s.TakeSnapshotOf( "P Projektor", { ids[ 0 ] }, false, true );
				p.lumens     = 5000;
				p.pan        = 0;
				s.snapshots[ P ].fade = 0;
				s.RecallSnapshot( P );
				step( 0.05f );
				const Projector o1 = s.ProjectorOut( 0 );
				s.snapshots[ P ].master = 0.5f;
				const Projector o2 = s.ProjectorOut( 0 );
				T( s.snapshots[ P ].proj.size() == 1 && fabsf( o1.lumens - 20000 ) < 1 && fabsf( o1.pan - 10 ) < 0.01f && fabsf( o2.lumens - 10000 ) < 1 && s.projectors[ 0 ].lumens == 5000, "Projektor im Snapshot: %.0f lm / Pan %.1f, Stärke 50 %% -> %.0f lm, Grundeinstellung bleibt %.0f lm", o1.lumens, o1.pan, o2.lumens, s.projectors[ 0 ].lumens );
				s.snapshots[ P ].master = 1;
				//projector lines on / off
				app.ren.projLines = true;
				p.frustum         = true;
				app.RenderViewport( 320, 180 );
				const int withL = app.ren.HelperCount();
				app.ren.projLines = false;
				app.RenderViewport( 320, 180 );
				const int noL = app.ren.HelperCount();
				app.ren.projLines = true;
				p.frustum         = false;
				app.ren.selProjector = 0;
				app.RenderViewport( 320, 180 );
				const int selNoL = app.ren.HelperCount();
				app.ren.selProjector = -1;
				p.frustum            = true;
				T( withL >= noL + 8 && selNoL == noL, "Projektor-Linien: an %d, aus %d, ausgewählt mit Linien aus %d Hilfslinien", withL, noL, selNoL );
			}
			s.ReleaseAllSnapshots();
			step( 0.1f );
			//group faders
			{
				GroupFader g;
				g.name     = "Alle Dimmer";
				g.fixtures = ids;
				g.attr     = Attr::Dimmer;
				g.value    = 0.4f;
				s.groups.push_back( g );
				s.ApplyGroup( 0 );
				bool allD = true;
				for( int k = 0; k < 4; ++k ) allD = allD && fabsf( s.GetAttrBase( F( k ), Attr::Dimmer ) - 0.4f ) < 0.01f;
				GroupFader c;
				c.name     = "Farbe";
				c.fixtures = { ids[ 1 ], ids[ 2 ] };
				c.kind     = 1;
				c.hue      = 2.0f / 3.0f;
				c.sat      = 1;
				c.value    = 1;
				s.groups.push_back( c );
				s.ApplyGroup( 1 );
				const bool blue = at( 1, Attr::Blue ) > 0.99f && at( 1, Attr::Red ) < 0.01f && at( 2, Attr::Blue ) > 0.99f && at( 0, Attr::Blue ) < 0.01f;
				SetParam( s, "/ls/group/1/value", 0.8f );
				SetParam( s, "/ls/group/2/hue", 0.0f );
				T( allD && blue && fabsf( s.GetAttrBase( F( 3 ), Attr::Dimmer ) - 0.8f ) < 0.01f && at( 2, Attr::Red ) > 0.99f && at( 2, Attr::Blue ) < 0.01f, "Gruppenfader: Dimmer 40 %% auf 4 Lampen, Farbgruppe blau auf 2 Lampen, per OSC 80 %% und rot" );
				//group fader over a running snapshot: the later action wins, until the snapshot is started again
				const float fadeA     = s.snapshots[ A ].fade;
				s.snapshots[ A ].fade = 0;
				s.RecallSnapshot( A );
				step( 0.05f );
				const float snapD = at( 0, Attr::Dimmer ), snapR = at( 0, Attr::Red );
				SetParam( s, "/ls/group/1/value", 0.25f );
				step( 0.05f );
				const float grpD = at( 0, Attr::Dimmer ), grpR = at( 0, Attr::Red ), otherD = at( 3, Attr::Dimmer );
				const int ovr    = s.SnapshotOverridden( A );
				s.RecallSnapshot( A );
				step( 0.05f );
				const float againD = at( 0, Attr::Dimmer );
				T( ovr == 2 && s.SnapshotOverridden( A ) == 0, "Snapshot-Anzeige: %d Lampen vom Gruppenfader übernommen, nach Neustart %d", ovr, s.SnapshotOverridden( A ) );
				T( snapD > 0.5f && fabsf( grpD - 0.25f ) < 0.01f && grpR > 0.99f && snapR > 0.99f && fabsf( otherD - 0.25f ) < 0.01f && fabsf( againD - snapD ) < 0.01f,
				   "Gruppenfader über Snapshot: Snapshot %.2f -> Gruppenfader 25 %% wirkt (%.2f, Farbe aus Snapshot bleibt %.2f), Snapshot neu gestartet -> %.2f", snapD, grpD, grpR, againD );
				s.snapshots[ A ].fade = fadeA;
				s.ReleaseAllSnapshots();
				step( 0.1f );
				//1.2: dimmer effect "scale" (runs inside the value underneath) and group fader as submaster
				{
					step( fadeA + 0.2f );//snapshot A faded out
					s.SetAttr( F( 0 ), Attr::Dimmer, 0.4f );
					s.SetAttr( F( 1 ), Attr::Dimmer, 0.8f );
					Effect e;
					e.id       = s.NextEffectId();
					e.name     = "Test Skalieren";
					e.kind     = FxKind::Dimmer;
					e.fixtures = { ids[ 0 ] };
					e.lo = e.hi = 0.5f;
					e.fade     = 0;
					e.on       = true;
					s.effects.push_back( e );
					step( 0.05f );
					const float rep = at( 0, Attr::Dimmer );
					s.effects.back().scale = true;
					step( 0.05f );
					const float scl = at( 0, Attr::Dimmer );
					GroupFader sm;
					sm.name     = "Sub";
					sm.fixtures = { ids[ 0 ], ids[ 1 ] };
					sm.mode     = 1;
					sm.value    = 0.5f;
					s.groups.push_back( sm );
					s.ApplyGroup( (int)s.groups.size() - 1 );
					step( 0.05f );
					const float sub0 = at( 0, Attr::Dimmer ), sub1 = at( 1, Attr::Dimmer ), base1 = s.GetAttrBase( F( 1 ), Attr::Dimmer ), other = at( 2, Attr::Dimmer );
					T( fabsf( rep - 0.5f ) < 0.01f && fabsf( scl - 0.2f ) < 0.01f, "Effekt Ersetzen %.2f / Skalieren %.2f (Grundwert 0.40, Effekt 50 %%)", rep, scl );
					T( fabsf( sub0 - 0.1f ) < 0.01f && fabsf( sub1 - 0.4f ) < 0.01f && fabsf( base1 - 0.8f ) < 0.01f && fabsf( other - s.GetAttrBase( F( 2 ), Attr::Dimmer ) ) < 0.01f,
					   "Submaster 50 %%: Effekt-Lampe %.2f, Programmer-Lampe %.2f (Grundwert bleibt %.2f), andere Lampe unberührt", sub0, sub1, base1 );
					s.groups.back().value = 1;
					step( 0.05f );
					const float back1 = at( 1, Attr::Dimmer );
					T( fabsf( back1 - 0.8f ) < 0.01f, "Submaster 100 %%: wieder %.2f", back1 );
					s.groups.pop_back();
					s.effects.pop_back();
					step( 0.05f );
				}
			}
			//1.2: colour palette -> snapshot remembers it, palette change changes the snapshot (and the programmer)
			{
				s.ReleaseAllSnapshots();
				step( 3.0f );
				Palette pr;
				pr.uid   = s.NewPaletteUid();
				pr.name  = "Rot";
				pr.color = V3( 1, 0, 0 );
				s.palettes.push_back( pr );
				const int nAp = s.ApplyPalette( pr.uid, { ids[ 0 ], ids[ 1 ] } );
				s.SetAttr( F( 0 ), Attr::Dimmer, 1 );
				s.SetAttr( F( 1 ), Attr::Dimmer, 1 );
				const int P = s.TakeSnapshotOf( "Pal", { ids[ 0 ], ids[ 1 ] }, false, false );
				s.snapshots[ P ].fade = 0;
				const size_t links    = s.snapshots[ P ].pal.size();
				s.SetColor( F( 1 ), V3( 0, 1, 0 ) );//changed by hand: the programmer does not follow the palette any more
				Palette prev = *s.FindPalette( pr.uid );
				s.FindPalette( pr.uid )->color = V3( 0, 0, 1 );
				const int nUp = s.UpdatePalette( pr.uid, &prev );
				const bool progFollow = s.GetAttrBase( F( 0 ), Attr::Blue ) > 0.99f && s.GetAttrBase( F( 0 ), Attr::Red ) < 0.01f;
				const bool progHand   = s.GetAttrBase( F( 1 ), Attr::Green ) > 0.99f && s.GetAttrBase( F( 1 ), Attr::Blue ) < 0.01f;
				s.RecallSnapshot( P );
				step( 0.05f );
				const bool snapBlue = at( 0, Attr::Blue ) > 0.99f && at( 0, Attr::Red ) < 0.01f && at( 1, Attr::Blue ) > 0.99f;
				T( nAp == 2 && links == 2 && nUp == 1 && snapBlue, "Palette: Snapshot merkt sich die Palette (%d Lampen), Palette rot -> blau ändert den Snapshot", (int)links );
				T( progFollow && progHand, "Palette: Programmer folgt (Lampe 1 blau), von Hand geänderte Lampe bleibt (Lampe 2 grün)" );
				{
					Show t;
					t.FromJson( s.ToJson(), app.lib, "", nullptr );
					T( t.palettes.size() == 1 && t.snapshots[ P ].pal.size() == 2 && t.palettes[ 0 ].color.z > 0.99f, "Paletten gespeichert und geladen" );
				}
				s.ReleaseAllSnapshots();
				step( 0.1f );
				s.DeletePalette( pr.uid );
				const bool unl = s.snapshots[ P ].pal.empty() && s.palettes.empty();
				s.snapshots.erase( s.snapshots.begin() + P );
				T( unl, "Palette gelöscht: Snapshot behält Werte, Verknüpfung entfernt" );
			}
			//1.2: cue list (GO, follow, timecode, back, stop), MIDI timecode, save / load
			{
				s.ReleaseAllSnapshots();
				step( 3.0f );
				Cue c1, c2, c3;
				c1.name = "eins";
				c1.snap = s.snapshots[ A ].uid;
				c1.fade = 0;
				c2.snap    = s.snapshots[ B ].uid;
				c2.fade    = 0;
				c2.trigger = 1;
				c2.wait    = 0.5f;
				c3.snap    = s.snapshots[ C ].uid;
				c3.fade    = 0;
				c3.trigger = 2;
				c3.tc      = 10.0;
				s.cues     = { c1, c2, c3 };
				s.CueGo();
				step( 0.1f );
				const bool go1 = s.cueCur == 0 && s.snapshots[ A ].active && !s.snapshots[ B ].active;
				step( 0.6f );
				const bool fol = s.cueCur == 1 && s.snapshots[ B ].active && !s.snapshots[ A ].active;
				T( go1 && fol, "Cue-Liste: GO startet Cue 1 (A), Cue 2 folgt nach 0,5 s (B, A aus)" );
				s.cueTimecode = true;
				s.tcSec       = 9.0;
				step( 0.05f );
				s.tcSec = 9.5;
				step( 0.05f );
				const bool notYet = s.cueCur == 1;
				s.tcSec = 10.2;
				step( 0.05f );
				const bool tcHit = s.cueCur == 2 && s.snapshots[ C ].active && !s.snapshots[ B ].active;
				s.CueBack();
				step( 0.05f );
				const bool back = s.cueCur == 1 && s.snapshots[ B ].active && !s.snapshots[ C ].active;
				s.tcSec = 30.0;//jump
				step( 0.05f );
				const bool jump = s.cueCur == 2;
				T( notYet && tcHit && back && jump, "Cue-Liste: Timecode 10 s startet Cue 3, Zurück -> Cue 2, Sprung auf 30 s -> Cue 3" );
				s.CueStop();
				step( 0.05f );
				T( s.cueCur == -1 && s.ActiveSnapshots() == 0, "Cue-Liste: Stop blendet alle Cue-Snapshots aus" );
				//fade of the cue instead of the snapshot's
				s.cues[ 0 ].fade = 2.0f;
				s.CueGo( 0 );
				step( 1.0f );
				const float half = s.snapshots[ A ].cur;
				T( half > 0.4f && half < 0.6f, "Cue-Fade 2 s statt Snapshot-Fade: nach 1 s %.2f", half );
				//MIDI timecode quarter frames 00:01:02:10 @ 25 fps
				{
					const int hh = 0, mm = 1, ss = 2, ff = 10, rate = 1;
					const int nib[ 8 ] = { ff & 15, ff >> 4, ss & 15, ss >> 4, mm & 15, mm >> 4, hh & 15, ( hh >> 4 ) | ( rate << 1 ) };
					{
						std::lock_guard< std::mutex > l( app.midi.lock_ );
						for( int k = 0; k < 8; ++k ) app.midi.queue_.push_back( { 0, (uint32_t)( 0xF1 | ( ( ( k << 4 ) | nib[ k ] ) << 8 ) ), Now() } );
					}
					std::vector< MidiEvent > ev;
					app.midi.Poll( ev );
					const double want = 62 + 12 / 25.0;
					T( fabs( app.midi.mtcSec - want ) < 0.001 && app.midi.mtcFps == 25 && ev.empty(), "MIDI-Timecode: 00:01:02:10 @ 25 fps -> %.3f s", app.midi.mtcSec );
					app.midi.mtcAt = 0;
				}
				//save / load
				{
					Json j = s.ToJson();
					Show t;
					std::string er;
					t.FromJson( j, app.lib, "", &er );
					T( t.cues.size() == 3 && t.cues[ 2 ].trigger == 2 && fabs( t.cues[ 2 ].tc - 10.0 ) < 1e-6 && t.cues[ 1 ].wait == 0.5f && t.cueTimecode && t.FindSnapUid( t.cues[ 0 ].snap ) == A && t.cues[ 0 ].name == "eins",
					   "Cue-Liste gespeichert und geladen (%d Cues, Snapshot-Bezug über ID)", (int)t.cues.size() );
				}
				s.cueTimecode = false;
				s.tcSec       = -1;
				s.CueStop();
				s.cues.clear();
				step( 3.0f );
			}
			//2.0: show timeline - curves, group track over the snapshots, programmer priority, colour, cue track, loop, MTC, save / load
			{
				s.ReleaseAllSnapshots();
				step( 3.0f );
				look( {}, 0, 0, 0, 0 );
				Timeline& tl = s.tl;
				tl           = Timeline();
				//curves
				{
					TlLane l;
					TlSetKey( l, 0, 0, nullptr, TlInterp::Linear );
					TlSetKey( l, 2, 1, nullptr, TlInterp::Linear );
					const float lin = TlEval( l, 1.0 );
					l.keys[ 0 ].interp = TlInterp::Step;
					const float stp = TlEval( l, 1.9 );
					l.keys[ 0 ].interp = TlInterp::Smooth;
					const float smo = TlEval( l, 0.5 );
					l.keys[ 0 ].interp = TlInterp::Bezier;
					l.keys[ 0 ].ox = 0.0f; l.keys[ 0 ].oy = 0;
					l.keys[ 1 ].ix = 0.0f; l.keys[ 1 ].iy = 0;
					const float bzl = TlEval( l, 0.5 );//handles on the keys = straight line
					l.keys[ 0 ].ox = 0.3f; l.keys[ 0 ].oy = 0.8f;
					const float bzu = TlEval( l, 0.5 );//pulled up
					const bool hold = TlEval( l, -1 ) == 0 && TlEval( l, 5 ) == 1;
					T( fabsf( lin - 0.5f ) < 1e-4f && stp == 0 && fabsf( smo - 0.15625f ) < 1e-4f && fabsf( bzl - 0.25f ) < 0.01f && bzu > bzl + 0.1f && hold,
					   "Timeline-Kurven: linear %.3f, Stufe %.2f, weich %.4f, Bezier gerade %.3f / angehoben %.3f, vor/nach den Keys gehalten", lin, stp, smo, bzl, bzu );
				}
				//group track: dimmer 0 -> 1 over 2 s on lamps 1 + 2
				TlTrack tr;
				tr.uid      = tl.NewUid();
				tr.fixtures = { ids[ 0 ], ids[ 1 ] };
				TlLane dim;
				dim.attr = Attr::Dimmer;
				TlSetKey( dim, 0, 0, nullptr, TlInterp::Linear );
				TlSetKey( dim, 2, 1, nullptr, TlInterp::Linear );
				tr.lanes.push_back( dim );
				tl.tracks.push_back( tr );
				tl.Locate( s, 1.0 );
				step( 0.02f );
				const float d1 = at( 0, Attr::Dimmer ), d1b = at( 1, Attr::Dimmer ), dOther = at( 2, Attr::Dimmer );
				tl.Play( s, true );
				step( 0.5f );
				const double p15 = tl.rt.pos;
				const float d15  = at( 0, Attr::Dimmer );
				tl.Play( s, false );
				T( fabsf( d1 - 0.5f ) < 0.01f && fabsf( d1b - 0.5f ) < 0.01f && dOther < 0.01f && fabs( p15 - 1.5 ) < 0.05 && fabsf( d15 - 0.75f ) < 0.03f,
				   "Timeline: Gruppenspur Dimmer bei 1 s %.2f / %.2f (Lampe 3 unberührt), Play 0,5 s -> %.2f s, %.2f", d1, d1b, p15, d15 );
				//master scales against what is underneath (programmer 0)
				tl.master = 0.5f;
				step( 0.02f );
				const float dm = at( 0, Attr::Dimmer );
				tl.master      = 1;
				tl.output      = false;
				step( 0.02f );
				const float doff = at( 0, Attr::Dimmer );
				tl.output        = true;
				step( 0.02f );
				T( fabsf( dm - d15 * 0.5f ) < 0.02f && doff < 0.01f, "Timeline-Master 50 %% -> %.2f, Ausgabe aus -> %.2f", dm, doff );
				//programmer move wins on its channel until the next locate / play
				s.SetAttr( F( 0 ), Attr::Dimmer, 0.9f );
				step( 0.02f );
				const float pg = at( 0, Attr::Dimmer ), pg1 = at( 1, Attr::Dimmer );
				tl.Locate( s, 0.5 );
				step( 0.02f );
				const float back = at( 0, Attr::Dimmer );
				T( fabsf( pg - 0.9f ) < 0.01f && fabsf( pg1 - 0.75f ) < 0.03f && fabsf( back - 0.25f ) < 0.02f, "Timeline: Programmer hat Vorrang (%.2f, andere Lampe %.2f), nach Positionswechsel wieder Timeline (%.2f)", pg, pg1, back );
				s.SetAttr( F( 0 ), Attr::Dimmer, 0 );
				//over a snapshot: the timeline takes the dimmer, the snapshot keeps the colour
				s.snapshots[ A ].fade = 0;
				s.RecallSnapshot( A );
				tl.Locate( s, 0.5 );
				step( 0.05f );
				const float sd = at( 0, Attr::Dimmer ), sr = at( 0, Attr::Red );
				T( fabsf( sd - 0.25f ) < 0.02f && sr > 0.99f, "Timeline über Snapshot: Dimmer aus der Timeline %.2f, Rot vom Snapshot %.2f", sd, sr );
				s.ReleaseAllSnapshots();
				step( 0.1f );
				//colour lane: red -> blue
				TlLane col;
				col.kind = TlLaneKind::Color;
				V3 red( 1, 0, 0 ), blue( 0, 0, 1 );
				TlSetKey( col, 0, 0, &red, TlInterp::Linear );
				TlSetKey( col, 2, 0, &blue, TlInterp::Linear );
				tl.tracks[ 0 ].lanes.push_back( col );
				tl.Locate( s, 2.0 );
				step( 0.02f );
				const float cb = at( 1, Attr::Blue ), cr = at( 1, Attr::Red );
				tl.Locate( s, 1.0 );
				step( 0.02f );
				const float hr = at( 1, Attr::Red ), hb = at( 1, Attr::Blue );
				T( cb > 0.99f && cr < 0.01f && fabsf( hr - 0.5f ) < 0.02f && fabsf( hb - 0.5f ) < 0.02f, "Timeline-Farbspur: bei 2 s blau (%.2f), bei 1 s Mischung R %.2f / B %.2f", cb, hr, hb );
				//cue track: the cue at 3 s starts snapshot B when the timeline passes it
				{
					TlTrack ct;
					ct.uid  = tl.NewUid();
					ct.kind = TlTrackKind::Cues;
					tl.tracks.push_back( ct );
					Cue c;
					c.snap    = s.snapshots[ B ].uid;
					c.fade    = 0;
					c.trigger = 2;
					c.tc      = 3.0;
					s.cues    = { c };
					tl.length = 10;
					tl.Locate( s, 2.5 );
					step( 0.1f );
					const bool before = s.cueCur == -1;
					tl.Play( s, true );
					step( 0.6f );
					const bool hit = s.cueCur == 0 && s.snapshots[ B ].active;
					tl.Play( s, false );
					T( before && hit, "Timeline-Cue-Spur: Cue bei 3 s startet Snapshot B beim Abspielen (Position %.2f s)", tl.rt.pos );
					s.CueStop();
					s.cues.clear();
					tl.tracks.pop_back();
					step( 0.1f );
				}
				//loop
				tl.loop  = true;
				tl.loopA = 0;
				tl.loopB = 1;
				tl.Locate( s, 0.9 );
				tl.Play( s, true );
				step( 0.3f );
				const double lp = tl.rt.pos;
				tl.Play( s, false );
				tl.loop = false;
				T( lp > 0.1 && lp < 0.3, "Timeline-Loop 0..1 s: 0,9 s + 0,3 s -> %.2f s", lp );
				//MIDI timecode sync with offset + freewheel
				tl.sync      = TlSync::Mtc;
				tl.offset    = -10;
				tl.freewheel = 0.5f;
				s.tcSec      = 11.0;
				s.tcRun      = true;
				step( 0.05f );
				const double m1 = tl.rt.pos;
				s.tcRun         = false;//quarter frames stop
				step( 0.3f );
				const double m2 = tl.rt.pos;
				const bool run2 = tl.rt.playing;
				step( 0.5f );
				const bool stop3 = !tl.rt.playing;
				T( fabs( m1 - 1.0 ) < 0.01 && m2 > m1 + 0.25 && run2 && stop3, "Timeline-MTC: 11 s - 10 s Versatz -> %.2f s, Freilauf -> %.2f s, danach Stopp", m1, m2 );
				s.tcSec = -1;
				tl.sync = TlSync::Internal;
				//fan over the lamps: time offset + value spread
				{
					tl.tracks[ 0 ].mute = true;
					TlTrack tf;
					tf.uid      = tl.NewUid();
					tf.fixtures = { ids[ 0 ], ids[ 1 ] };
					TlLane fl;
					fl.attr = Attr::Dimmer;
					TlSetKey( fl, 0, 0, nullptr, TlInterp::Linear );
					TlSetKey( fl, 2, 1, nullptr, TlInterp::Linear );
					fl.fanTime = 1;
					tf.lanes.push_back( fl );
					tl.tracks.push_back( tf );
					tl.Locate( s, 2.0 );
					step( 0.02f );
					const float f0 = at( 0, Attr::Dimmer ), f1 = at( 1, Attr::Dimmer );
					TlLane& L2 = tl.tracks.back().lanes[ 0 ];
					L2.keys[ 0 ].v = 0.5f;
					L2.keys[ 1 ].v = 0.5f;
					L2.fanTime     = 0;
					L2.fanValue    = 0.4f;
					tl.Locate( s, 1.0 );
					step( 0.02f );
					const float v0 = at( 0, Attr::Dimmer ), v1 = at( 1, Attr::Dimmer );
					L2.fanOrder = 3;
					step( 0.02f );
					const float r0 = at( 0, Attr::Dimmer );
					T( fabsf( f0 - 1 ) < 0.01f && fabsf( f1 - 0.5f ) < 0.02f && fabsf( v0 - 0.3f ) < 0.02f && fabsf( v1 - 0.7f ) < 0.02f && fabsf( r0 - 0.7f ) < 0.02f,
					   "Timeline-Fan: Zeitversatz 1 s -> %.2f / %.2f, Wert-Spreizung 40 %% -> %.2f / %.2f, umgekehrt %.2f", f0, f1, v0, v1, r0 );
					tl.tracks.pop_back();
					tl.tracks[ 0 ].mute = false;
				}
				//clip track: snapshot block with fades, effect block
				{
					TlTrack tc;
					tc.uid  = tl.NewUid();
					tc.kind = TlTrackKind::Clips;
					TlClip c;
					c.kind    = TlClipKind::Snapshot;
					c.snap    = s.snapshots[ B ].uid;
					c.t       = 1;
					c.len     = 2;
					c.fadeIn  = 0.5f;
					c.fadeOut = 0.5f;
					tc.clips.push_back( c );
					Effect e;
					e.id        = s.NextEffectId();
					e.kind      = FxKind::Dimmer;
					e.type      = FxType::Intensity;
					e.fixtures  = { ids[ 3 ] };
					const int eid = e.id;
					s.effects.push_back( e );
					TlClip ce;
					ce.kind = TlClipKind::Effect;
					ce.fx   = eid;
					ce.t    = 5;
					ce.len  = 2;
					tc.clips.push_back( ce );
					tl.tracks.push_back( tc );
					tl.Locate( s, 1.25 );
					step( 0.02f );
					const float ch = at( 2, Attr::Dimmer ), cbl = at( 2, Attr::Blue );
					tl.Locate( s, 2.0 );
					step( 0.02f );
					const float cf = at( 2, Attr::Dimmer );
					tl.Locate( s, 3.5 );
					step( 0.02f );
					const float co = at( 2, Attr::Dimmer );
					tl.Locate( s, 6.0 );
					step( 0.02f );
					const float dIn = s.FindEffect( eid )->Drive();
					tl.Locate( s, 8.0 );
					step( 0.02f );
					const float dOut = s.FindEffect( eid )->Drive();
					T( fabsf( ch - 0.5f ) < 0.03f && fabsf( cbl - 0.5f ) < 0.03f && cf > 0.99f && co < 0.01f && dIn > 0.99f && dOut < 0.01f,
					   "Timeline-Clips: Snapshot-Block eingeblendet %.2f (Blau %.2f), voll %.2f, danach %.2f; Effekt-Block an %.2f / aus %.2f", ch, cbl, cf, co, dIn, dOut );
					tl.tracks.pop_back();
					s.effects.pop_back();
					step( 0.05f );
				}
				//record: overwrite pass with thinning
				{
					TlLane& L = tl.tracks[ 0 ].lanes[ 0 ];
					L.keys.clear();
					TlSetKey( L, 0, 0.1f, nullptr, TlInterp::Linear );
					TlSetKey( L, 4, 0.1f, nullptr, TlInterp::Linear );
					L.rec      = true;
					tl.record  = true;
					tl.recMode = 0;
					tl.Locate( s, 0 );
					tl.Play( s, true );
					float mid = -1;
					for( int i = 0; i < 60; ++i )
					{
						s.SetAttr( F( 0 ), Attr::Dimmer, i / 59.0f );
						step( 1 / 60.0f );
						if( i == 30 ) mid = at( 0, Attr::Dimmer );
					}
					tl.Play( s, false );
					step( 1 / 60.0f );
					int inPass = 0;
					for( auto& k : L.keys ) inPass += k.t < 1.0;
					const float e05 = TlEval( L, 0.5 ), e3 = TlEval( L, 3.0 );
					T( fabsf( mid - 30 / 59.0f ) < 0.03f && L.recBuf.empty() && inPass >= 2 && inPass <= 6 && fabsf( e05 - 0.5f ) < 0.06f && fabsf( e3 - 0.1f ) < 0.02f,
					   "Timeline-Aufnahme (Überschreiben): Programmer läuft durch (%.2f), %d Keys nach Ausdünnen, Kurve bei 0,5 s %.2f, danach zurück auf %.2f", mid, inPass, e05, e3 );
					//touch: only while moved, then back to the curve
					L.keys.clear();
					TlSetKey( L, 0, 0.2f, nullptr, TlInterp::Linear );
					TlSetKey( L, 4, 0.2f, nullptr, TlInterp::Linear );
					tl.recMode = 1;
					tl.Locate( s, 0 );
					tl.Play( s, true );
					float before = -1, during = -1, after = -1;
					for( int i = 0; i < 96; ++i )
					{
						const double t = i / 60.0;
						if( t >= 0.3 && t < 0.6 ) s.SetAttr( F( 0 ), Attr::Dimmer, 0.8f );
						step( 1 / 60.0f );
						if( i == 12 ) before = at( 0, Attr::Dimmer );
						if( i == 30 ) during = at( 0, Attr::Dimmer );
						if( i == 95 ) after = at( 0, Attr::Dimmer );
					}
					tl.Play( s, false );
					step( 1 / 60.0f );
					const float t45 = TlEval( L, 0.45 ), t15 = TlEval( L, 1.5 ), t3 = TlEval( L, 3.0 );
					T( fabsf( before - 0.2f ) < 0.02f && fabsf( during - 0.8f ) < 0.02f && fabsf( after - 0.2f ) < 0.03f && fabsf( t45 - 0.8f ) < 0.03f && fabsf( t15 - 0.2f ) < 0.03f && fabsf( t3 - 0.2f ) < 0.01f,
					   "Timeline-Aufnahme (Touch): vorher %.2f, beim Bewegen %.2f, nach dem Loslassen %.2f; Kurve 0,45 s %.2f / 1,5 s %.2f / 3 s %.2f", before, during, after, t45, t15, t3 );
					L.rec     = false;
					tl.record = false;
					s.SetAttr( F( 0 ), Attr::Dimmer, 0 );
					std::vector< std::pair< double, V3 > > line;
					for( int i = 0; i < 100; ++i ) line.push_back( { i * 0.01, V3( i * 0.01f, 0, 0 ) } );
					std::vector< int > keep;
					TlThin( line, 0.005f, false, keep );
					T( keep.size() == 2, "Ausdünnen: Gerade aus 100 Punkten -> %d Keys", (int)keep.size() );
				}
				//LTC: decode a generated signal (25 and 30 fps, quiet), timeline follows LTC
				{
					std::vector< float > sig;
					LtcEncode( 0, 1, 2, 10, 25, 48000, 30, 0.5f, sig );
					LtcDecoder d;
					d.Reset( 48000 );
					for( size_t i = 0; i < sig.size(); i += 480 ) d.Feed( sig.data() + i, (int)std::min< size_t >( 480, sig.size() - i ) );
					const bool ok25 = d.frames >= 28 && d.h == 0 && d.m == 1 && d.s == 3 && d.f == 13 && d.fps == 25 && fabs( d.Seconds() - 63.56 ) < 1e-6;//the last frame completes with the next edge
					LtcEncode( 1, 59, 59, 20, 30, 48000, 20, 0.03f, sig );
					LtcDecoder d30;
					d30.Reset( 48000 );
					d30.Feed( sig.data(), (int)sig.size() );
					const bool ok30 = d30.frames >= 18 && d30.h == 2 && d30.m == 0 && d30.s == 0 && d30.f == 8 && d30.fps == 30;
					T( ok25 && ok30, "LTC-Decoder: 25 fps -> %02d:%02d:%02d:%02d (%lld Frames), 30 fps leise -> %02d:%02d:%02d:%02d @ %d fps", d.h, d.m, d.s, d.f, d.frames, d30.h, d30.m, d30.s, d30.f, d30.fps );
					tl.sync   = TlSync::Ltc;
					tl.offset = 0;
					s.ltcSec  = 20.0;
					s.ltcRun  = true;
					step( 0.02f );
					const double lp = tl.rt.pos;
					s.ltcSec        = -1;
					s.ltcRun        = false;
					tl.sync         = TlSync::Internal;
					tl.Play( s, false );
					T( fabs( lp - 20.0 ) < 0.01, "Timeline folgt LTC: %.2f s", lp );
				}
				//audio file: a generated WAV through Media Foundation
				{
					const std::string wav = dir + "\\selftest_tone.wav";
					{
						const int rate = 48000, n = rate;
						std::vector< int16_t > pcm( n * 2 );
						for( int i = 0; i < n; ++i ) pcm[ 2 * i ] = pcm[ 2 * i + 1 ] = (int16_t)( 16000 * sinf( 2 * 3.14159265f * 440 * i / rate ) );
						FILE* fw = fopen( wav.c_str(), "wb" );
						if( fw )
						{
							auto u32 = [ & ]( uint32_t v ) { fwrite( &v, 4, 1, fw ); };
							auto u16 = [ & ]( uint16_t v ) { fwrite( &v, 2, 1, fw ); };
							fwrite( "RIFF", 1, 4, fw ); u32( 36 + n * 4 ); fwrite( "WAVEfmt ", 1, 8, fw ); u32( 16 ); u16( 1 ); u16( 2 ); u32( rate ); u32( rate * 4 ); u16( 4 ); u16( 16 );
							fwrite( "data", 1, 4, fw ); u32( n * 4 ); fwrite( pcm.data(), 2, pcm.size(), fw );
							fclose( fw );
						}
					}
					AudioData ad;
					std::string er;
					const bool ld   = LoadAudioFile( wav, ad, er );
					const float pk  = ld ? ad.PeakAt( 0.2, 0.3 ) : 0;
					T( ld && fabs( ad.Seconds() - 1.0 ) < 0.01 && ad.ch == 2 && pk > 0.45f && pk < 0.52f, "Audiodatei geladen: %.2f s, %d Hz, %d Kanäle, Wellenform-Spitze %.2f %s", ad.Seconds(), ad.rate, ad.ch, pk, er.c_str() );
				}
				//timeline in the parameter register (OSC, MIDI learn, Ableton manifest)
				{
					int n = 0;
					for( const Param& p : Params( s ) ) n += p.addr.rfind( "/ls/tl/", 0 ) == 0;
					T( n >= 8, "Timeline-Parameter für OSC / MIDI / Ableton: %d", n );
				}
				//save / load
				{
					tl.tracks[ 0 ].lanes[ 0 ].keys[ 0 ].interp = TlInterp::Bezier;
					tl.tracks[ 0 ].lanes[ 0 ].keys[ 0 ].oy     = 0.3f;
					tl.markers.push_back( { 4.0, "Drop" } );
					Show t;
					t.FromJson( s.ToJson(), app.lib, "", nullptr );
					const Timeline& u = t.tl;
					const bool ok = u.tracks.size() == 1 && u.tracks[ 0 ].fixtures.size() == 2 && u.tracks[ 0 ].lanes.size() == 2 && u.tracks[ 0 ].lanes[ 1 ].kind == TlLaneKind::Color &&
					                u.tracks[ 0 ].lanes[ 1 ].keys[ 1 ].c.z > 0.99f && u.tracks[ 0 ].lanes[ 0 ].keys[ 0 ].interp == TlInterp::Bezier && fabsf( u.tracks[ 0 ].lanes[ 0 ].keys[ 0 ].oy - 0.3f ) < 1e-4f &&
					                u.markers.size() == 1 && u.markers[ 0 ].name == "Drop" && u.length == 10 && u.sync == TlSync::Internal;
					T( ok, "Timeline gespeichert und geladen (%d Spur, %d Lanes, Bezier, Farbe, Marker)", (int)u.tracks.size(), u.tracks.empty() ? 0 : (int)u.tracks[ 0 ].lanes.size() );
				}
				//2.0: Lightshow export (.lightshow): every frame = what the output sends, re-read identically, cues, scene, 3D data
				{
					const std::string lbp = dir + "\\selftest.lightshow";
					//own, clean timeline: dimmer 0 -> 1 over 2 s on lamps 1 + 2, marker "Drop" at 4 s, loop on (the export ignores it)
					Timeline keepTl = tl;
					tl              = Timeline();
					tl.length       = 10;
					tl.loop         = true;
					tl.loopA        = 0;
					tl.loopB        = 1;
					{
						TlTrack t;
						t.uid      = tl.NewUid();
						t.fixtures = { ids[ 0 ], ids[ 1 ] };
						TlLane d;
						d.attr = Attr::Dimmer;
						TlSetKey( d, 0, 0, nullptr, TlInterp::Linear );
						TlSetKey( d, 2, 1, nullptr, TlInterp::Linear );
						t.lanes.push_back( d );
						tl.tracks.push_back( t );
						tl.markers.push_back( { 4.0, "Drop" } );
					}
					for( int k = 0; k < 4; ++k )//white light (earlier tests left the colours at 0); the dimmer comes from the timeline
						for( Attr a : { Attr::Red, Attr::Green, Attr::Blue } ) s.SetAttr( F( k ), a, 1 );
					tl.Locate( s, 7.0 );
					step( 0.02f );
					const double posWas   = tl.rt.pos;
					const bool playWas    = tl.rt.playing;
					const TlSync syncWas  = tl.sync;
					LbxOptions o;
					o.to = 5.0;
					LbxJob j;
					const bool began = LbxBegin( s, o, lbp, j );
					j.keepFrames     = true;
					const double t0  = Now();
					while( began && LbxStep( s, app.sim, j, 5.0 ) ) {}
					const double secs = Now() - t0;
					const std::vector< std::vector< uint8_t > > kept = j.keptDmx;
					const bool fin = began && LbxFinish( s, j, true );
					const bool restored = fabs( tl.rt.pos - posWas ) < 1e-9 && tl.rt.playing == playWas && tl.sync == syncWas;
					LbxFile lf;
					std::string er;
					const bool rd = fin && LbxRead( lbp, lf, &er );
					bool same = rd && lf.dmx.size() == kept.size() && lf.frames == 200 && lf.fps == 40;
					for( size_t k = 0; same && k < kept.size(); ++k ) same = lf.dmx[ k ] == kept[ k ];
					T( began && fin && same && restored, "Lightshow-Export: %d Bilder (5 s @ 40 fps) in %.2f s, %u Byte, gelesen = gesendet %s, Time Line danach unverändert %s %s",
					   lf.frames, secs, (unsigned)lf.fileBytes, same ? "ja" : "nein", restored ? "ja" : "nein", er.c_str() );
					//reference: the timeline located directly at 1 s sends the same dimmer byte as frame 40
					int dimSlot = -1;
					if( const Mode* m = F( 0 ).M() )
						for( int c = 0; c < (int)m->ch.size() && dimSlot < 0; ++c )
							if( m->ch[ c ].attr == Attr::Dimmer ) dimSlot = c;
					tl.Locate( s, 1.0 );
					step( 0.02f );
					const int ref  = dimSlot >= 0 ? (int)s.Get( F( 0 ), dimSlot ) : -1;
					const int a    = F( 0 ).address - 1 + dimSlot;
					const int f40  = rd && dimSlot >= 0 && lf.dmx.size() > 40 ? lf.dmx[ 40 ][ a ] : -2;
					const int f0   = rd && dimSlot >= 0 ? lf.dmx[ 0 ][ a ] : -2;
					const int f79  = rd && dimSlot >= 0 && lf.dmx.size() > 79 ? lf.dmx[ 79 ][ a ] : -2;
					T( ref >= 0 && abs( f40 - ref ) <= 1 && f0 < f40 && f40 < f79, "Lightshow-Export: Dimmer Lampe 1 bei 0 / 1 / 1,98 s = %d / %d / %d, direkt bei 1 s %d", f0, f40, f79, ref );
					//cues, scene, 3D view data
					const Json& cl = lf.cues[ "cues" ];
					bool drop      = false;
					for( size_t k = 0; k < cl.Size(); ++k ) drop = drop || ( cl[ k ][ "name" ].Str() == "Drop" && fabs( cl[ k ][ "t" ].Num() - 4.0 ) < 1e-6 );
					const Json& fxj = lf.scene[ "fixtures" ];
					const bool scene = fxj.Size() == s.fixtures.size() && fxj[ 0 ][ "intensity" ].Size() >= 1 && fxj[ 0 ][ "intensity" ][ 0 ].I() == dimSlot &&
					                   fxj[ 0 ][ "addr" ].I() == F( 0 ).address && lf.info[ "universes" ].Size() == s.universes.size();
					const int vo   = fxj[ 0 ][ "vis" ].I();
					const bool vis = rd && (int)lf.vis.size() == lf.frames && vo >= 0 && lf.vis[ 79 ][ vo ] > lf.vis[ 0 ][ vo ];
					const bool hv  = rd && vo >= 0 && lf.vis.size() > 79 && (int)lf.vis[ 0 ].size() > vo;
					{
						//patch overview: names + attributes of every slot, profile + mode
						bool chOk = fxj.Size() > 0;
						for( size_t i = 0; i < fxj.Size() && i < s.fixtures.size(); ++i )
							chOk = chOk && (int)fxj[ i ][ "channels" ].Size() == s.fixtures[ i ].Footprint() && (int)fxj[ i ][ "attrs" ].Size() == s.fixtures[ i ].Footprint();
						const bool dimName = fxj.Size() > 0 && fxj[ 0 ][ "attrs" ][ dimSlot ].Str() == AttrName( Attr::Dimmer );
						T( chOk && dimName && !fxj[ 0 ][ "profile" ].Str().empty(), "Lightshow-Export: Kanalnamen je Lampe (%d Lampen, Lampe 1: %s / %s, Kanal %d = %s)",
						   (int)fxj.Size(), fxj[ 0 ][ "profile" ].Str().c_str(), fxj[ 0 ][ "mode" ].Str().c_str(), dimSlot + 1, fxj[ 0 ][ "channels" ][ dimSlot ].Str().c_str() );
					}
					{
						//group faders: function + DMX slots (players apply them live)
						const size_t ng = s.groups.size();
						const Mode* m0  = F( 0 ).M();
						Attr a1         = m0 && m0->ch.size() > 1 ? m0->ch[ 1 ].attr : Attr::Dimmer;
						GroupFader ga;
						ga.name     = "LB Attr";
						ga.fixtures = { F( 0 ).id, F( 1 ).id };
						ga.attr     = a1;
						ga.value    = 0.25f;
						GroupFader gc;
						gc.name     = "LB Farbe";
						gc.fixtures = { F( 2 ).id };
						gc.kind     = 1;
						gc.hue      = 0.6f;
						s.groups.push_back( ga );
						s.groups.push_back( gc );
						LbxOptions go;
						go.to       = 1.0;
						go.fxLayers = false;
						std::string ge;
						LbxFile G;
						const std::string gp = dir + "\\selftest_groups.lightshow";
						const bool gok      = LbxExport( s, app.sim, go, gp, &ge ) && LbxRead( gp, G, &ge );
						s.groups.resize( ng );
						const Json& gl = G.scene[ "groups" ];
						int nA = 0, wantA = 0, nC = 0;
						bool addrOk = true;
						for( size_t i = 0; i < gl.Size(); ++i )
						{
							if( gl[ i ][ "name" ].Str() == "LB Attr" )
							{
								nA = (int)gl[ i ][ "targets" ].Size();
								for( size_t k = 0; k < gl[ i ][ "targets" ].Size(); ++k )
									addrOk = addrOk && gl[ i ][ "targets" ][ k ][ 3 ].Str() == AttrName( a1 ) && gl[ i ][ "targets" ][ k ][ 1 ].I() % 1 == 0;
								addrOk = addrOk && gl[ i ][ "kind" ].I() == 0 && gl[ i ][ "attr" ].Str() == AttrName( a1 ) && fabs( gl[ i ][ "value" ].Num() - 0.25 ) < 1e-6;
								if( nA ) addrOk = addrOk && gl[ i ][ "targets" ][ 0 ][ 1 ].I() == F( 0 ).address - 1 + 1;
							}
							if( gl[ i ][ "name" ].Str() == "LB Farbe" ) nC = (int)gl[ i ][ "targets" ].Size();
						}
						for( int k : { 0, 1 } )
							if( const Mode* m = F( k ).M() )
								for( auto& c : m->ch ) wantA += c.attr == a1 && !c.isFine;
						T( gok && nA == wantA && nA > 0 && nC > 0 && addrOk, "Lightshow-Export: Gruppenfader mit Funktion (%s: %d Kanäle, Farbe: %d Kanäle) %s",
						   AttrName( a1 ), nA, nC, ge.c_str() );
					}
					T( drop && scene && vis, "Lightshow-Export: Marker \"Drop\" bei 4 s %s, Szene %d Lampen (Dimmer-Kanal %d), 3D-Daten (%d Bilder à %d Byte) Helligkeit Lampe 1 %d -> %d",
					   drop ? "ja" : "nein", (int)fxj.Size(), dimSlot, (int)lf.vis.size(), lf.visBytes, hv ? lf.vis[ 0 ][ vo ] : -1, hv ? lf.vis[ 79 ][ vo ] : -1 );
					//Lightshow export: effect layers (EFFX, FXLD, FXLV). Layer "off" = show without the effect, layer "on" = effect
					//forced on. Each must equal a separate export in which the effect has level 0 / runs from the start without fade.
					{
						const Effect* chaseTpl = nullptr;
						for( auto& pr : BuiltinFxPresets() )
							if( !chaseTpl && pr.fx.kind == FxKind::Dimmer ) chaseTpl = &pr.fx;
						const std::vector< Effect > keepFx = s.effects;
						s.effects.clear();
						const int fa = chaseTpl ? AddEffectFromPreset( s, *chaseTpl, "LB an", { F( 0 ).id, F( 1 ).id }, true ) : 0;
						const int fb = chaseTpl ? AddEffectFromPreset( s, *chaseTpl, "LB aus", { F( 2 ).id, F( 3 ).id }, false ) : 0;
						for( auto& e : s.effects ) e.fade = 0;
						auto fxById = [ & ]( int id ) -> Effect* { for( auto& e : s.effects ) if( e.id == id ) return &e; return nullptr; };
						const std::string p1 = dir + "\\selftest_fx1.lightshow", p2 = dir + "\\selftest_fx2.lightshow", p3 = dir + "\\selftest_fx3.lightshow";
						LbxOptions fo;
						fo.to = 3.0;
						std::string e1, e2, e3;
						LbxFile A, B, C;
						bool ok = fa && fb && LbxExport( s, app.sim, fo, p1, &e1 ) && LbxRead( p1, A, &e1 );
						fo.fxLayers = false;
						fxById( fa )->level = 0;//reference "off": a without effect a
						ok = ok && LbxExport( s, app.sim, fo, p2, &e2 ) && LbxRead( p2, B, &e2 );
						fxById( fa )->level = 1;
						fxById( fb )->on    = true;//reference "on": b running from the start
						ok = ok && LbxExport( s, app.sim, fo, p3, &e3 ) && LbxRead( p3, C, &e3 );
						fxById( fb )->on = false;
						const LbxFile::Layer *lo = nullptr, *ln = nullptr;
						for( auto& L : A.layers )
						{
							if( L.fx == fa && !L.on ) lo = &L;
							if( L.fx == fb && L.on ) ln = &L;
						}
						//VIS field layout (u8 / int16) from the scene
						std::vector< uint8_t > kind( A.visBytes, 0 );
						const Json& sf = A.scene[ "fixtures" ];
						for( size_t i = 0; i < sf.Size(); ++i )
							for( int k = 0; k < 6; ++k )
							{
								const int at = sf[ i ][ "vis" ].I() + 4 + 2 * k;
								if( at + 1 < A.visBytes ) { kind[ at ] = 1; kind[ at + 1 ] = 2; }
							}
						auto d16 = []( const std::vector< uint8_t >& f, size_t i ) { return (int)(int16_t)( f[ i ] | ( f[ i + 1 ] << 8 ) ); };
						auto rd16 = []( const std::vector< uint8_t >& f, size_t i ) { return (int)(int16_t)( f[ i ] | ( f[ i + 1 ] << 8 ) ); };
						std::string why;
						auto check = [ & ]( const LbxFile::Layer* L, const LbxFile& R, int& dmxBad, int& visBad, int& changed ) {
							dmxBad = visBad = changed = 0;
							if( !L || (int)L->dmx.size() != A.frames || R.frames != A.frames ) { dmxBad = -1; return; }
							for( int k = 0; k < A.frames; ++k )
							{
								for( int c = 0; c < A.dmxBytes; ++c )
								{
									const int d = d16( L->dmx[ k ], 2 * (size_t)c );
									if( d ) ++changed;
									if( (int)A.dmx[ k ][ c ] + d != (int)R.dmx[ k ][ c ] && ++dmxBad <= 4 )
									{
										char w[ 96 ];
										snprintf( w, sizeof( w ), " [B%d K%d: %d%+d≠%d]", k, c, (int)A.dmx[ k ][ c ], d, (int)R.dmx[ k ][ c ] );
										why += w;
									}
								}
								if( (int)L->vis.size() != A.frames || R.vis.size() != A.vis.size() ) { visBad = -1; continue; }
								for( int b = 0; b < A.visBytes; ++b )
								{
									const int d = d16( L->vis[ k ], 2 * (size_t)b );
									if( kind[ b ] == 0 && (int)A.vis[ k ][ b ] + d != (int)R.vis[ k ][ b ] ) ++visBad;
									if( kind[ b ] == 1 && rd16( A.vis[ k ], b ) + d != rd16( R.vis[ k ], b ) ) ++visBad;
								}
							}
						};
						int ob = 0, ov = 0, oc = 0, nb = 0, nv = 0, nc = 0;
						check( lo, B, ob, ov, oc );
						check( ln, C, nb, nv, nc );
						const Json& fl = A.effects[ "effects" ];
						const bool meta = fl.Size() == 2 && fl[ 0 ][ "id" ].I() == fa && fl[ 0 ][ "off" ].B() && fl[ 0 ][ "share" ].Num() > 0.99 && fl[ 1 ][ "on" ].B() &&
						                  !fl[ 1 ][ "off" ].B() && fl[ 1 ][ "active" ].Size() == 0 && fl[ 0 ][ "fixtures" ].Size() == 2;
						T( ok && lo && ln && ob == 0 && ov == 0 && nb == 0 && nv == 0 && oc > 0 && nc > 0 && meta,
						   "Lightshow-Export Effekt-Ebenen: \"aus\" = Export ohne Effekt (%d DMX-/%d 3D-Abweichungen, %d Werte), \"an\" = Export mit Effekt (%d/%d, %d Werte), EFFX %s, %u Byte %s%s%s%s",
						   ob, ov, oc, nb, nv, nc, meta ? "ok" : "falsch", (unsigned)A.fileBytes, e1.c_str(), e2.c_str(), e3.c_str(), why.c_str() );
						s.effects = keepFx;
					}
					//2.0: Lightshow import - export again from the imported show and compare frame by frame
					{
						const std::string lbp2 = dir + "\\selftest_reimport.lightshow";
						auto reexport = [ & ]( Show& n, int& maxDiff, int& frames ) -> bool {
							LbxOptions ro;
							ro.visual = false;
							std::string e2;
							maxDiff = -1;
							frames  = 0;
							if( !LbxExport( n, app.sim, ro, lbp2, &e2 ) ) { Log( "reexport: %s", e2.c_str() ); return false; }
							LbxFile g;
							if( !LbxRead( lbp2, g, &e2 ) || g.dmxBytes != lf.dmxBytes ) return false;
							frames  = g.frames;
							maxDiff = 0;
							for( int k = 0; k < std::min( g.frames, lf.frames ); ++k )
								for( int c = 0; c < g.dmxBytes; ++c ) maxDiff = std::max( maxDiff, abs( (int)g.dmx[ k ][ c ] - (int)lf.dmx[ k ][ c ] ) );
							return true;
						};
						const bool patch = lf.patch.IsObj() && lf.patch[ "fixtures" ].Size() == s.fixtures.size() && lf.patch[ "timeline" ].IsNull() && lf.patch[ "snapshots" ].IsNull();
						T( patch, "Lightshow-Export: Setup (PTCH) mit %d Lampen, ohne Time Line / Snapshots", (int)lf.patch[ "fixtures" ].Size() );
						//1:1 track into a new show
						{
							Show n;
							LbxImportOptions io;
							io.asTrack = true;
							LbxImportResult ir;
							std::string e2;
							const bool ok = LbxImport( n, app.lib, lf, lbp, io, &ir, &e2 );
							int md = -1, fr = 0;
							const bool rx = ok && reexport( n, md, fr );
							const bool trk = ok && n.tl.tracks.size() == 1 && n.tl.tracks[ 0 ].kind == TlTrackKind::Show && n.fixtures.size() == s.fixtures.size() && n.tl.markers.size() == 1 &&
							                 fabs( n.tl.length - 5.0 ) < 1e-6;
							T( ok && rx && trk && md == 0 && fr == lf.frames, "Lightshow-Import als Lightshow-Spur (neue Show, %d Lampen): erneut exportiert %d Bilder, größte Abweichung %d (soll 0) %s",
							   (int)n.fixtures.size(), fr, md, e2.c_str() );
							//saved and loaded with the show
							Show m;
							m.FromJson( n.ToJson(), app.lib, "", nullptr );
							const bool sl = m.tl.tracks.size() == 1 && m.tl.tracks[ 0 ].kind == TlTrackKind::Show && m.tl.tracks[ 0 ].file == lbp;
							T( sl, "Lightshow-Spur gespeichert und geladen (Datei %s)", sl ? "ja" : "nein" );
						}
						//editable time lanes into a new show
						{
							Show n;
							LbxImportOptions io;
							LbxImportResult ir;
							std::string e2;
							const double t0 = Now();
							const bool ok   = LbxImport( n, app.lib, lf, lbp, io, &ir, &e2 );
							const double sec = Now() - t0;
							int md = -1, fr = 0;
							const bool rx = ok && reexport( n, md, fr );
							bool dmxOnly  = ok && !n.tl.tracks.empty();
							for( auto& t : n.tl.tracks ) dmxOnly = dmxOnly && t.kind == TlTrackKind::Dmx;
							T( ok && rx && dmxOnly && md <= 1 && fr == lf.frames && ir.keys < lf.frames * std::max( 1, ir.lanes ) / 4,
							   "Lightshow-Import als Time Lanes (neue Show): %d Spuren, %d Lanes, %d Keys (%d Bilder), %d feste Kanäle, %.2f s; erneut exportiert größte Abweichung %d (max 1) %s",
							   ir.tracks, ir.lanes, ir.keys, lf.frames, ir.constants, sec, md, e2.c_str() );
						}
						//older file without setup: new show from the scene (generic profiles), lanes still exact
						{
							LbxFile old = lf;
							old.patch   = Json();
							Show n;
							LbxImportOptions io;
							LbxImportResult ir;
							std::string e2;
							const bool ok = LbxImport( n, app.lib, old, lbp, io, &ir, &e2 );
							int md = -1, fr = 0;
							const bool rx = ok && reexport( n, md, fr );
							T( ok && rx && !ir.fromPatch && md <= 1 && (int)n.universes.size() == (int)lf.info[ "universes" ].Size(),
							   "Lightshow-Import ohne Setup: %d Lampen mit Standardprofil, %d ohne, Abweichung %d %s", (int)n.fixtures.size(), ir.skippedFixtures, md, e2.c_str() );
						}
						//into the current show at 20 s
						{
							const size_t trBefore = tl.tracks.size(), mkBefore = tl.markers.size();
							LbxImportOptions io;
							io.newShow = false;
							io.at      = 20;
							LbxImportResult ir;
							std::string e2;
							const bool ok = LbxImport( s, app.lib, lf, lbp, io, &ir, &e2 );
							bool startOk  = ok && tl.tracks.size() > trBefore;
							for( size_t k = trBefore; startOk && k < tl.tracks.size(); ++k )
								for( auto& l : tl.tracks[ k ].lanes ) startOk = startOk && !l.keys.empty() && fabs( l.keys[ 0 ].t - 20.0 ) < 1e-9;
							bool drop = false;
							for( auto& m : tl.markers ) drop = drop || ( m.name == "Drop" && fabs( m.t - 24.0 ) < 1e-6 );
							T( ok && startOk && drop && tl.length >= 25.0 - 1e-9 && tl.markers.size() == mkBefore + 1 && ir.universesAdded == 0,
							   "Lightshow-Import in die aktuelle Show ab 20 s: %d Spuren, %d Lanes, Marker \"Drop\" bei 24 s %s, Time Line %.0f s %s", ir.tracks, ir.lanes, drop ? "ja" : "nein", tl.length, e2.c_str() );
							tl.tracks.resize( trBefore );
						}
						//Lightshow track -> time lanes (track menu)
						{
							TlTrack t;
							t.uid    = tl.NewUid();
							t.kind   = TlTrackKind::Show;
							t.file   = lbp;
							t.offset = 2;
							tl.tracks.push_back( t );
							const size_t before = tl.tracks.size();
							const bool cv = app.TlConvertShowTrack( t.uid );
							bool gone     = !tl.Find( t.uid );
							T( cv && gone && tl.tracks.size() >= before, "Lightshow-Spur in Time Lanes umgewandelt: %d Spuren (Lightshow-Spur entfernt %s)", (int)tl.tracks.size() - (int)before + 1, gone ? "ja" : "nein" );
						}
						DeleteFileA( lbp2.c_str() );
					}
					LbxJob e;
					tl.tracks.clear();
					const bool refused = !LbxBegin( s, LbxOptions(), lbp, e );
					T( refused && !e.err.empty(), "Lightshow-Export ohne Time Line abgelehnt (%s)", e.err.c_str() );
					tl = keepTl;
					step( 0.02f );
					DeleteFileA( lbp.c_str() );
				}
				tl = Timeline();
				step( 0.1f );
				T( at( 0, Attr::Dimmer ) < 0.01f, "Timeline entfernt: Lampen wieder vom Programmer (%.2f)", at( 0, Attr::Dimmer ) );
			}
			//save / load
			{
				s.snapshots[ A ].master = 0.7f;
				s.snapshots[ A ].fade   = 2.5f;
				s.RecallSnapshot( B );
				step( 0.1f );
				const std::string fn = dir + "\\selftest_snap.lightstage";
				std::string err;
				bool sv = s.Save( fn, &err );
				Show s2;
				bool ld = sv && s2.Load( fn, app.lib, &err );
				bool same = ld && s2.snapshots.size() == s.snapshots.size() && s2.groups.size() == 2 && fabsf( s2.snapshots[ A ].master - 0.7f ) < 0.001f && fabsf( s2.snapshots[ A ].fade - 2.5f ) < 0.001f &&
				            s2.snapshots[ A ].fix.size() == 2 && s2.snapshots[ A ].fix[ 0 ].v == s.snapshots[ A ].fix[ 0 ].v && s2.snapshots[ B ].active && s2.snapshots[ B ].cur == 1 && !s2.snapshots[ A ].active &&
				            s2.snapshots[ E ].effects == s.snapshots[ E ].effects && s2.snapshots.back().proj.size() == 1 && s2.groups[ 1 ].kind == 1 && s2.groups[ 0 ].fixtures == ids;
				//old file format: whole universes, no lamp list -> every lamp, switched at once
				std::string txt;
				Json j;
				bool legacy = false;
				if( same && ReadText( fn, txt ) && ParseJson( txt, j ) )
				{
					Json arr = Json::Arr(), o = Json::Obj(), d = Json::Obj();
					std::string hex( 1024, '0' );
					hex[ 0 ] = hex[ 1 ] = 'f';//channel 1 = 255
					d.Set( "1", hex );
					o.Set( "name", "Alt" );
					o.Set( "dmx", d );
					arr.Push( o );
					j.Set( "snapshots", arr );
					WriteText( fn, WriteJson( j, true ) );
					Show s3;
					if( s3.Load( fn, app.lib, &err ) && s3.snapshots.size() == 1 )
					{
						const Snapshot& o3 = s3.snapshots[ 0 ];
						legacy = o3.fix.size() == 4 && o3.fade == 0 && !o3.fix[ 0 ].v.empty() && o3.fix[ 0 ].v[ 0 ] == 255 && o3.fix[ 1 ].v[ 0 ] == 0;
					}
				}
				T( same && legacy, "Snapshots + Gruppen in der Show-Datei (Stärke, Fade, Lampen, Effekte, Projektor, aktiv), alte Snapshots werden übernommen %s", err.c_str() );
			}
			s.ReleaseAllSnapshots();
			step( 2.0f );
		}
		//fixture editor: a duplicate name that was removed from the mode must not block saving
		{
			EdFixture e;
			e.manufacturer = "LS Test";
			e.name         = "Doppelt";
			EdChannel c1   = MakeChannel( EdType::Red, "Rot" ), c2 = MakeChannel( EdType::Green, "Rot" );
			e.pool         = { c1, c2 };
			e.modes[ 0 ].ch = { 0, 1 };
			bool f1 = false, f2 = true, f3 = true;
			auto m1 = EdValidate( e, &f1 );
			e.modes[ 0 ].ch = { 0 };//"x" removes it from the mode, it stays in the pool
			EdValidate( e, &f2 );
			e.modes[ 0 ].ch = { 0, 1 };
			e.pool[ 1 ].name = "Grün";
			EdValidate( e, &f3 );
			T( f1 && !m1.empty() && !f2 && !f3, "Fixture-Editor: doppelter Kanalname blockiert, nach Entfernen/Umbenennen wieder speicherbar (%s)", m1.empty() ? "" : m1[ 0 ].c_str() );
		}
		app.BuildDemo();
	}
	//---- rendering
	{
		app.BuildDemo();
		Run( app, 5.0f );
		app.sim.haze = app.show.stage.haze;
		float lum, br;
		bool ok = SavePng( app, dir + "\\shot_front.png", 1600, 900, &lum, &br );
		T( ok && lum > 6 && lum < 160, "Render Front: mittlere Helligkeit %.1f, %.2f%% sehr hell, %d Lichter, %d Schatten, %d Kegel, %.1f ms", lum, br * 100, app.ren.stats.lights, app.ren.stats.shadows, app.ren.stats.beams, app.ren.stats.ms );
		//beams make a difference
		app.show.stage.beams = false;
		float lumNoBeams;
		SavePng( app, dir + "\\shot_nobeams.png", 1600, 900, &lumNoBeams );
		app.show.stage.beams = true;
		T( lum > lumNoBeams * 1.05f, "Volumetrische Kegel sichtbar (%.1f mit / %.1f ohne)", lum, lumNoBeams );
		app.cam.yaw = 32; app.cam.pitch = 20; app.cam.dist = 21;
		SavePng( app, dir + "\\shot_foh.png", 1600, 900 );
		app.cam.yaw = 90; app.cam.pitch = 6; app.cam.dist = 17;
		SavePng( app, dir + "\\shot_side.png", 1600, 900 );
		app.cam = Camera();
		app.cam.target = V3( 0, 3.4f, 0 );
		app.cam.dist = 19;
		app.cam.pitch = 9;
		app.show.RecallSnapshot( 1 );
		Run( app, 4.0f );
		SavePng( app, dir + "\\shot_look2.png", 1600, 900 );
		app.cam.target = V3( 0, 1.2f, 0 ); app.cam.pitch = 70; app.cam.dist = 13; app.cam.yaw = 0;
		SavePng( app, dir + "\\shot_floor.png", 1600, 900 );
		app.cam = Camera();
		app.cam.target = V3( 0, 3.4f, 0 );
		app.cam.dist = 19;
		app.cam.pitch = 9;
		app.show.RecallSnapshot( 0 );
		Run( app, 4.0f );
		//performance
		double t0 = Now();
		for( int i = 0; i < 30; ++i ) app.RenderViewport( 1600, 900 );
		glFinish();
		double ms = ( Now() - t0 ) / 30 * 1000;
		T( ms < 60, "Renderzeit 1600x900: %.1f ms/Frame", ms );
	}
	//---- 1.2: fan, aim, position palette (demo show: moving heads)
	{
		app.BuildDemo();
		Run( app, 0.2f );
		Show& s = app.show;
		std::vector< int > mh;
		for( auto& f : s.fixtures )
			if( f.def && f.def->kind == Kind::MovingHead && s.HasAttr( f, Attr::Pan ) && s.HasAttr( f, Attr::Tilt ) && mh.size() < 4 ) mh.push_back( f.id );
		if( mh.size() >= 3 )
		{
			for( int id : mh )
			{
				s.SetAttr( *s.FindFixture( id ), Attr::Pan, 0.5f );
				s.SetAttr( *s.FindFixture( id ), Attr::Tilt, 0.5f );
			}
			app.FanBegin( mh );
			app.FanApply( 90, 0 );
			std::vector< std::pair< float, float > > xs;//x, pan
			for( int id : mh ) xs.push_back( { s.FindFixture( id )->pos.x, s.GetAttrBase( *s.FindFixture( id ), Attr::Pan ) } );
			std::sort( xs.begin(), xs.end() );
			const FixtureInst& f0 = *s.FindFixture( mh[ 0 ] );
			const float spanDeg   = fabsf( xs.back().second - xs.front().second ) * f0.def->panRange;
			bool mono = true;
			for( size_t k = 1; k < xs.size(); ++k ) mono = mono && ( xs[ k ].second - xs[ k - 1 ].second ) * ( xs.back().second - xs.front().second ) >= 0;
			T( fabsf( spanDeg - 90 ) < 1.0f && mono, "Fächer Pan 90°: %d Lampen von links nach rechts, Spanne %.1f°", (int)mh.size(), spanDeg );
			//position palette from the fanned look, then home, then apply
			Palette pp;
			pp.uid  = s.NewPaletteUid();
			pp.kind = 1;
			pp.name = "Fan";
			for( int id : mh ) pp.pos.push_back( { id, s.GetAttrBase( *s.FindFixture( id ), Attr::Pan ), s.GetAttrBase( *s.FindFixture( id ), Attr::Tilt ) } );
			s.palettes.push_back( pp );
			const float panBefore = s.GetAttrBase( *s.FindFixture( xs.size() ? mh[ 0 ] : 0 ), Attr::Pan );
			for( int id : mh ) s.Home( *s.FindFixture( id ) );
			const int nAp         = s.ApplyPalette( pp.uid, mh );
			const float panAfter  = s.GetAttrBase( *s.FindFixture( mh[ 0 ] ), Attr::Pan );
			const int P           = s.TakeSnapshotOf( "PosPal", mh, false, false );
			int posLinks          = 0;
			for( auto& lk : s.snapshots[ P ].pal ) posLinks += lk.second == pp.uid ? 1 : 0;
			T( nAp == (int)mh.size() && fabsf( panAfter - panBefore ) < 0.001f && posLinks == (int)mh.size(), "Positionspalette: angewendet auf %d Lampen, Snapshot verknüpft %d", nAp, posLinks );
			//aim at a point on the floor
			app.sel        = mh;
			const int nAim = app.AimSelected( V3( 0, 0, 2 ) );
			Run( app, 3.0f );
			float worst = 0;
			for( int id : mh )
			{
				const FixtureInst& f = *s.FindFixture( id );
				V3 o, d, u;
				BeamRay( f, -1, o, d, u );
				worst = std::max( worst, acosf( Clamp( Dot( d, Norm( V3( 0, 0, 2 ) - o ) ), -1.0f, 1.0f ) ) / kDeg );
			}
			T( nAim == (int)mh.size() && worst < 2.0f, "Zielen: %d Moving Heads zeigen auf den Punkt (größter Fehler %.2f Grad)", nAim, worst );
		}
		else
			T( false, "Demo-Show ohne Moving Heads" );
		app.sel.clear();
	}
	//---- 1.2: Resolume Advanced Output preset from projectors + screens
	{
		const auto keepProj = app.show.projectors;
		const auto keepScr  = app.show.screens;
		app.show.projectors.clear();
		app.show.screens.clear();
		auto inPoly = []( const std::vector< float >& p, float x, float y )
		{
			bool in = false;
			const size_t n = p.size() / 2;
			for( size_t i = 0, j = n - 1; i < n; j = i++ )
			{
				const float xi = p[ i * 2 ], yi = p[ i * 2 + 1 ], xj = p[ j * 2 ], yj = p[ j * 2 + 1 ];
				if( ( yi > y ) != ( yj > y ) && x < ( xj - xi ) * ( y - yi ) / ( yj - yi ) + xi ) in = !in;
			}
			return in;
		};
		//1) 16:9 screen exactly filled by a straight 16:9 projector -> slice corners = picture corners
		Screen sc;
		sc.name   = "Main";
		sc.pos    = V3( 0, 1, -4 );
		sc.width  = 6;
		sc.height = 3.375f;
		Projector pj;
		pj.name       = "Beamer";
		pj.format     = 0;
		pj.throwRatio = 1.5f;
		pj.pos        = V3( 0, 1 + 3.375f * 0.5f, -4 + 6 * 1.5f + 0.155f );
		pj.input      = VideoInput::Ndi;
		pj.ndi        = "DG22 (Arena - Screen 1)";
		app.show.projectors.push_back( pj );
		app.show.screens.push_back( sc );
		ResolumeOpts ro;
		auto L1 = BuildResolumeLayout( app.show, ro );
		float cErr = 1e9f;
		if( L1.size() == 1 && L1[ 0 ].slices.size() == 1 )
		{
			const float* c          = L1[ 0 ].slices[ 0 ].corners;
			const float want[ 8 ]   = { 0, 0, 1920, 0, 1920, 1080, 0, 1080 };
			cErr                    = 0;
			for( int k = 0; k < 8; ++k ) cErr = std::max( cErr, fabsf( c[ k ] - want[ k ] ) );
		}
		T( cErr < 0.5f && L1.size() == 1 && L1[ 0 ].name == "Screen 1" && L1[ 0 ].w == 1920, "Resolume: 16:9-Leinwand füllt das Bild, Ecken genau (%.3f px), Screen \"%s\"", cErr, L1.empty() ? "" : L1[ 0 ].name.c_str() );
		//2) rear projection mirrors the picture
		app.show.projectors[ 0 ].rear = true;
		auto L2                       = BuildResolumeLayout( app.show, ro );
		const bool rearOk             = L2.size() == 1 && L2[ 0 ].slices.size() == 1 && fabsf( L2[ 0 ].slices[ 0 ].corners[ 0 ] - 1920 ) < 0.5f;
		T( rearOk, "Resolume: Rückprojektion spiegelt das Slice (linke obere Ecke bei x = %.1f)", rearOk ? L2[ 0 ].slices[ 0 ].corners[ 0 ] : -1.0f );
		app.show.projectors[ 0 ].rear = false;
		//3) 4:1 screen seen from the side (keystone): warp follows the exact projection, input = 4:1 crop
		app.show.screens[ 0 ].width  = 8;
		app.show.screens[ 0 ].height = 2;
		app.show.screens[ 0 ].yaw    = 12;
		app.show.projectors[ 0 ].pos = V3( 2.5f, 3.2f, 7 );
		app.show.projectors[ 0 ].pan = 12;
		app.show.projectors[ 0 ].tilt = -6;
		app.show.projectors[ 0 ].throwRatio = 1.1f;
		auto L3 = BuildResolumeLayout( app.show, ro );
		float worst = -1;
		bool cropOk = false;
		if( L3.size() == 1 && L3[ 0 ].slices.size() == 1 )
		{
			const ResolumeSlice& sl = L3[ 0 ].slices[ 0 ];
			worst                   = 0;
			uint32_t r              = 12345;
			for( int k = 0; k < 200; ++k )
			{
				r             = r * 1664525u + 1013904223u;
				const float u = ( r >> 8 ) / 16777216.0f;
				r             = r * 1664525u + 1013904223u;
				const float v = ( r >> 8 ) / 16777216.0f;
				float ex, ey, bx, by;
				ProjectorPixel( app.show.projectors[ 0 ], 1920, 1080, ScreenPoint( app.show.screens[ 0 ], u, v ), ex, ey );
				ResolumeEvalNet( sl, u, v, bx, by );
				worst = std::max( worst, hypotf( bx - ex, by - ey ) );
			}
			cropOk = fabsf( sl.in[ 0 ] ) < 0.01f && fabsf( sl.in[ 2 ] - 1920 ) < 0.01f && fabsf( sl.in[ 1 ] - 300 ) < 0.01f && fabsf( sl.in[ 3 ] - 780 ) < 0.01f;
		}
		T( worst >= 0 && worst < 1.0f && cropOk, "Resolume: schräge 4:1-Leinwand, Warp %d x %d, größte Abweichung %.2f px (200 Punkte), Eingang 1920 x 480 mittig", L3.empty() || L3[ 0 ].slices.empty() ? 0 : L3[ 0 ].slices[ 0 ].gw, L3.empty() || L3[ 0 ].slices.empty() ? 0 : L3[ 0 ].slices[ 0 ].gh, worst );
		//4) shaped screen (ring): black parts become masks, the white ring stays free
		{
			const int n = 64;
			std::vector< uint8_t > cells( n * n );
			for( int y = 0; y < n; ++y )
				for( int x = 0; x < n; ++x )
				{
					const float dx = ( x + 0.5f ) / n - 0.5f, dy = ( y + 0.5f ) / n - 0.5f, d = sqrtf( dx * dx + dy * dy );
					cells[ y * n + x ] = d < 0.45f && d > 0.2f ? 1 : 0;
				}
			Screen& s = app.show.screens[ 0 ];
			s.width = s.height = 4;
			s.yaw              = 0;
			SetScreenMask( s, n, cells );
		}
		auto polys = ScreenMaskPolys( app.show.screens[ 0 ] );
		bool inCentre = false, inCorner = false, inRing = false;
		for( auto& p : polys )
		{
			inCentre |= inPoly( p, 0.5f, 0.5f );
			inCorner |= inPoly( p, 0.03f, 0.03f );
			inRing |= inPoly( p, 0.5f + 0.32f, 0.5f );
		}
		auto L4 = BuildResolumeLayout( app.show, ro );
		const int m4 = L4.size() == 1 && L4[ 0 ].slices.size() == 1 ? (int)L4[ 0 ].slices[ 0 ].masks.size() : -1;
		T( polys.size() == 2 && inCentre && inCorner && !inRing && m4 == 2, "Resolume: Ring-Leinwand -> %d Masken (außen + Mitte), Ring frei", (int)polys.size() );
		//5) XML: parses, one slice + masks, NDI output named after the screen; saved for a look in Arena
		const std::string xml = ResolumePresetXml( L4, ro );
		std::string perr;
		auto doc = ParseXml( xml, &perr );
		std::vector< const XmlNode* > sls, mks, ndi;
		if( doc )
		{
			doc->FindAll( "Slice", sls );
			doc->FindAll( "Mask", mks );
			doc->FindAll( "OutputDeviceNDI", ndi );
		}
		std::string werr;
		const bool wr = WriteResolumePreset( dir + "\\selftest_resolume.xml", xml, werr );
		T( doc && sls.size() == 1 && (int)mks.size() == m4 && ndi.size() == 1 && ndi[ 0 ]->Attr( "name" ) == "Screen 1" && wr, "Resolume: Preset-XML gültig (%d KB, %d Slice, %d Masken)%s", (int)( xml.size() / 1024 ), (int)sls.size(), (int)mks.size(), perr.c_str() );
		//6) names: composition output -> projector name; two projectors with the same name stay unique
		Projector pc = pj;
		pc.ndi       = "DG22 (Arena - Composition)";
		T( ResolumeScreenName( pc ) == "Beamer", "Resolume: NDI \"Composition\" -> Screen-Name des Projektors" );
		app.show.projectors = keepProj;
		app.show.screens    = keepScr;
	}
	//---- 1.2: video export (2 s, 1280x720, 25 fps, camera orbit)
	{
		const std::string vp = dir + "\\selftest_export.mp4";
		app.exp_.res     = 0;
		app.exp_.fpsIdx  = 0;
		app.exp_.seconds = 2;
		app.exp_.camMode = 1;
		const double t0  = Now();
		bool started     = app.ExportStart( vp );
		while( started && app.ExportStep( 1.0 ) ) {}
		const bool stepOk = app.exp_.err.empty();
		app.ExportFinish( stepOk );
		const double secs = VideoFileSeconds( vp );
		WIN32_FILE_ATTRIBUTE_DATA fa{};
		GetFileAttributesExW( Widen( vp ).c_str(), GetFileExInfoStandard, &fa );
		T( started && stepOk && fabs( secs - 2.0 ) < 0.15 && fa.nFileSizeLow > 20000 && !app.ExportActive(), "Video-Export: %.2f s MP4, %u KB, %.1f s Rechenzeit%s%s", secs, (unsigned)( fa.nFileSizeLow / 1024 ), Now() - t0, app.exp_.err.empty() ? "" : " - ", app.exp_.err.c_str() );
	}
	//---- 1.2: autosave file + recovery (own file names: the user's autosave is never touched)
	{
		const std::string af = dir + "\\selftest_autosave.lightstage";
		app.show.path        = dir + "\\Meine Show.lightstage";
		app.show.stage.haze  = 0.77f;
		const bool wr        = app.AutosaveWrite( af );
		app.NewShow();
		const bool rec = app.AutosaveRecover( af );
		T( wr && rec && fabsf( app.show.stage.haze - 0.77f ) < 0.001f && app.show.dirty && FileName( app.show.path ) == "Meine Show.lightstage",
		   "Autosave: gesichert und wiederhergestellt (Haze %.2f, Datei %s, als ungespeichert markiert)", app.show.stage.haze, FileName( app.show.path ).c_str() );
		DeleteFileW( Widen( af ).c_str() );
		app.show.path.clear();
	}
	//---- undo / redo (set-up states; live values stay)
	{
		app.BuildDemo();
		Run( app, 0.5f );
		app.UndoReset();
		double t0 = Now();
		for( int i = 0; i < 20; ++i ) app.UndoTick( true );
		const double msState = ( Now() - t0 ) / 20 * 1000;
		T( msState < 20, "Undo: Zustand der Demo-Show in %.2f ms erfasst", msState );
		const size_t nFix = app.show.fixtures.size();
		const float haze0 = app.show.stage.haze;
		//step 1: haze (UI change, settles in the normal frame loop)
		app.show.stage.haze = haze0 > 0.5f ? 0.2f : 0.8f;
		const float haze1   = app.show.stage.haze;
		app.UndoMarkUser();
		size_t afterShort = 9;
		for( double te = Now() + 1.2; Now() < te; )
		{
			app.UndoTick( false );//as at the end of every UI frame
			if( afterShort == 9 && Now() > te - 1.0 ) afterShort = app.UndoCount();
			Sleep( 16 );
		}
		T( afterShort == 0 && app.UndoCount() == 1, "Undo: Änderung im Frame-Ablauf wird nach kurzer Ruhe ein Schritt (%d nach 0.2 s, %d nach 1.2 s)", (int)afterShort, (int)app.UndoCount() );
		//step 2: delete a lamp
		const int delId = app.show.fixtures.back().id;
		app.sel = { delId };
		app.DeleteSelected();
		app.UndoMarkUser();
		app.UndoTick( true );
		T( app.UndoCount() == 2 && app.show.fixtures.size() == nFix - 1, "Undo: Lampe gelöscht = zweiter Schritt" );
		//changes from outside only: no step (a snapshot knob too: found again by name after undo)
		app.show.stage.width += 1.0f;
		if( !app.show.snapshots.empty() ) app.show.snapshots.back().master = 0.37f;
		app.UndoTick( true );
		const float width1 = app.show.stage.width;
		T( app.UndoCount() == 2, "Undo: Änderung ohne Bedienung (MIDI/OSC) erzeugt keinen Schritt" );
		//live values set now must survive undo
		app.show.master = 0.42f;
		FixtureInst& f0 = app.show.fixtures.front();
		app.show.SetAttr( f0, Attr::Dimmer, 0.66f );
		const uint8_t dmx0 = app.show.GetBase( f0, 0 );
		const int f0id     = f0.id;
		Effect* fxOn       = app.show.effects.empty() ? nullptr : &app.show.effects.front();
		const int fxId     = fxOn ? fxOn->id : 0;
		if( fxOn ) fxOn->on = !fxOn->on;
		const bool fxState = fxOn ? fxOn->on : false;
		app.UndoTick( true );
		T( app.UndoCount() == 2, "Undo: Live-Werte (Programmer, Master, Effekt an/aus) erzeugen keinen Schritt" );
		app.Undo();
		FixtureInst* f0b = app.show.FindFixture( f0id );
		Effect* fxB      = fxId ? app.show.FindEffect( fxId ) : nullptr;
		T( app.show.fixtures.size() == nFix && app.show.FindFixture( delId ) && app.show.stage.haze == haze1, "Undo 1: gelöschte Lampe ist wieder da (%d Lampen)", (int)app.show.fixtures.size() );
		T( app.show.master == 0.42f && f0b && app.show.GetBase( *f0b, 0 ) == dmx0 && ( !fxId || ( fxB && fxB->on == fxState ) ), "Undo: Master, Programmer und laufende Effekte bleiben unverändert" );
		T( app.show.stage.width == width1 && ( app.show.snapshots.empty() || app.show.snapshots.back().master == 0.37f ), "Undo: Änderungen von außen bleiben erhalten (Breite %.1f, Snapshot-Stärke)", app.show.stage.width );
		app.Undo();
		T( app.show.stage.haze == haze0 && app.UndoCount() == 0 && app.RedoCount() == 2, "Undo 2: Haze wieder %.2f, 2 Schritte zum Wiederherstellen", app.show.stage.haze );
		app.Undo();
		T( app.show.stage.haze == haze0, "Undo ohne weitere Schritte ändert nichts" );
		app.Redo();
		T( app.show.stage.haze == haze1 && app.show.fixtures.size() == nFix, "Redo 1: Haze wieder %.2f", app.show.stage.haze );
		app.Redo();
		T( app.show.fixtures.size() == nFix - 1 && !app.show.FindFixture( delId ) && app.RedoCount() == 0, "Redo 2: Lampe wieder gelöscht" );
		app.Undo();
		app.show.stage.wallHeight += 0.5f;
		app.UndoMarkUser();
		app.UndoTick( true );
		T( app.RedoCount() == 0 && app.UndoCount() == 2, "Neue Änderung nach Undo verwirft die Redo-Liste" );
		//saving still writes the live values, undo states leave them out
		{
			Json full = app.show.ToJson(), und = app.show.ToJson( true );
			T( full[ "universes" ][ 0 ].Find( "dmx" ) && !und[ "universes" ][ 0 ].Find( "dmx" ) && full.Find( "master" ) && !und.Find( "master" ) && full.Find( "tempo" ) && !und.Find( "tempo" ), "Show-Datei unverändert, Undo-Zustand ohne Live-Werte" );
		}
		app.NewShow();
		T( app.UndoCount() == 0 && app.RedoCount() == 0, "Neue Show: Undo-Liste leer" );
	}
	//---- 1.2: extra decks / risers
	{
		app.BuildDemo();
		const float dh = s.stage.deck ? s.stage.deckHeight : 0.0f;
		s.stage.decks.clear();
		Deck r;
		r.name   = "Riser";
		r.x      = 3;
		r.z      = 2;
		r.width  = 2;
		r.depth  = 1;
		r.height = 0.5f;
		r.onDeck = true;
		r.stairs = 1;
		s.stage.decks.push_back( r );
		T( fabsf( s.FloorAt( 3, 2 ) - ( dh + 0.5f ) ) < 1e-4f && fabsf( s.FloorAt( 3, 2.7f ) - dh ) < 1e-4f, "Podest auf der Bühne: Oberkante %.2f m, daneben %.2f m", s.FloorAt( 3, 2 ), s.FloorAt( 3, 2.7f ) );
		s.stage.decks[ 0 ].yaw = 90;
		T( s.FloorAt( 3, 2.9f ) > dh + 0.4f && s.FloorAt( 3.8f, 2 ) < dh + 0.01f, "Podest um 90° gedreht: Breite liegt jetzt in Z" );
		Deck rd;
		rd.shape  = 1;
		rd.x      = -4;
		rd.z      = 9;
		rd.width  = 2;
		rd.depth  = 2;
		rd.height = 0.8f;
		s.stage.decks.push_back( rd );
		T( s.FloorAt( -3.1f, 9 ) > 0.79f && s.FloorAt( -3.2f, 9.8f ) < 0.01f, "Rundes Podest: Kreis statt Rechteck" );
		float t = 0;
		const bool hit = s.RayDeckTop( V3( -4, 10, 9 ), V3( 0, -1, 0 ), t );
		T( hit && fabsf( t - 9.2f ) < 1e-3f, "Strahl trifft die Podest-Oberkante (t = %.3f)", t );
		Json j = s.ToJson();
		Show back;
		std::string err;
		const bool ok = back.FromJson( j, app.lib, "", &err );
		T( ok && back.stage.decks.size() == 2 && back.stage.decks[ 1 ].shape == 1 && back.stage.decks[ 0 ].stairs == 1 && fabsf( back.stage.decks[ 0 ].yaw - 90 ) < 1e-3f, "Podeste speichern / laden (%d)", (int)back.stage.decks.size() );
		//deck with its own base height (Y), trusses: free ends only when asked or sloped
		s.stage.decks[ 1 ].y = 2.0f;
		T( s.FloorAt( -4, 9 ) > 2.79f, "Podest mit eigener Unterkante Y = 2 m: Oberkante %.2f m", s.FloorAt( -4, 9 ) );
		s.stage.decks[ 1 ].y = 0;
		{
			Show tt;
			Truss flat, slope;
			slope.b.y = 7.5f;
			tt.trusses = { flat, slope };
			Json tj = tt.ToJson();
			Show t2;
			std::string e2;
			t2.FromJson( tj, app.lib, "", &e2 );
			T( t2.trusses.size() == 2 && !t2.trusses[ 0 ].freeEnds && t2.trusses[ 1 ].freeEnds, "Traversen: gerade = eine Position, schräge = beide Enden frei" );
		}
		float lum = 0;
		app.cam.target = V3( 0, 1.0f, 3.0f );
		app.cam.yaw    = 25;
		app.cam.pitch  = 25;
		app.cam.dist   = 22;
		app.cam.fov    = 50;
		s.workLight    = true;
		s.stage.decks[ 0 ].edge      = V3( 0.2f, 0.5f, 1.0f );
		s.stage.decks[ 0 ].edgeLevel = 2;
		Run( app, 0.2f );
		const bool png = SavePng( app, dir + "\\selftest_decks.png", 960, 540, &lum );
		T( png && lum > 0.005f, "Bild mit Podesten gerendert (Helligkeit %.3f)", lum );
		s.workLight = false;
		s.stage.decks.clear();
	}
	//---- 1.2: effect builder (types, LFO, step sequencer, envelope)
	{
		StopAllEffects( s );
		s.effects.clear();
		s.programs.clear();
		app.sel.clear();
		int made = 0;
		std::string missing;
		for( int k = 0; k < (int)FxType::Count; ++k )
		{
			const int id = app.AddEffectOfType( (FxType)k );
			if( id ) ++made;
			else missing += std::string( " " ) + FxTypeName( (FxType)k );
		}
		T( made == (int)FxType::Count, "Effekt-Builder: %d von %d Typen angelegt%s", made, (int)FxType::Count, missing.c_str() );
		bool typesOk = true;
		for( auto& e : s.effects ) typesOk &= ( e.kind == FxKind::Modulator ) == FxIsModulator( e.type );
		T( typesOk, "Typen: LFO, Sequencer, Hüllkurve sind Modulatoren, die übrigen klassische Effekte" );
		bool half = true;
		for( auto& e : s.effects )
			if( e.type != FxType::Sequencer ) half &= fabsf( e.hi - 0.5f ) < 1e-4f;
		T( half, "neue Effekte starten mit Maximum 50 %%" );
		Run( app, 0.5f );
		RunEffects( s, 0.05 );
		T( s.fxAny, "alle Typen laufen gleichzeitig" );
		StopAllEffects( s );
		s.effects.clear();
		//sequencer on a DMX channel: 4 steps, one cycle per second (free)
		const int ui = s.UniIndex( 1 );
		Effect q   = FxMakeType( FxType::Sequencer );
		q.id       = 900;
		q.on       = true;
		q.fade     = 0;
		q.sync     = false;
		q.hz       = 1;
		FxResizeSteps( q, 4 );
		q.lanes[ 0 ].tgt.mode = 2;
		q.lanes[ 0 ].tgt.uni  = 1;
		q.lanes[ 0 ].tgt.ch   = 500;
		q.lanes[ 0 ].v        = { 1.0f, 0.0f, 0.5f, 0.0f };
		s.effects.push_back( q );
		auto at = [ & ]( double ph ) {
			s.effects[ 0 ].freePhase = ph;
			RunEffects( s, 0 );
			return ui >= 0 && ui < (int)s.fxOut.size() ? (int)s.fxOut[ ui ][ 499 ] : -9;
		};
		const int v0 = at( 0.1 ), v1 = at( 0.3 ), v2 = at( 0.6 ), v3 = at( 0.9 );
		T( v0 == 255 && v1 == 0 && v2 == 128 && v3 == 0, "Step-Sequencer auf DMX 1.500: %d %d %d %d (erwartet 255 0 128 0)", v0, v1, v2, v3 );
		s.effects[ 0 ].lanes[ 0 ].glide = true;
		const int g = at( 0.125 );
		T( g > 100 && g < 160, "Gleiten zwischen Schritt 1 und 2: %d", g );
		//second lane, offset per channel
		s.effects[ 0 ].lanes[ 0 ].glide       = false;
		s.effects[ 0 ].lanes[ 0 ].tgt.count  = 2;
		s.effects[ 0 ].stepOffset            = 1;
		at( 0.1 );
		const int c1 = s.fxOut[ ui ][ 499 ], c2 = s.fxOut[ ui ][ 500 ];
		T( c1 == 255 && c2 == 0, "Versatz je Kanal: Kanal 500 = Schritt 1 (%d), Kanal 501 = Schritt 4 (%d)", c1, c2 );
		//LFO on a DMX channel: square 50 %
		s.effects.clear();
		Effect l   = FxMakeType( FxType::Lfo );
		l.id       = 901;
		l.on       = true;
		l.fade     = 0;
		l.sync     = false;
		l.wave     = FxWave::Square;
		l.width    = 0.5f;
		l.tgt.mode = 2;
		l.tgt.uni  = 1;
		l.tgt.ch   = 510;
		l.hi       = 1;
		s.effects.push_back( l );
		s.effects[ 0 ].freePhase = 0.25;
		RunEffects( s, 0 );
		const int la = s.fxOut[ ui ][ 509 ];
		s.effects[ 0 ].freePhase = 0.75;
		RunEffects( s, 0 );
		const int lb = s.fxOut[ ui ][ 509 ];
		T( la == 255 && lb == 0, "LFO Rechteck auf DMX 1.510: %d / %d", la, lb );
		//relative: around the value underneath
		s.dmx[ ui ][ 509 ] = 128;
		s.effects[ 0 ].relative  = true;
		s.effects[ 0 ].depth     = 0.4f;
		s.effects[ 0 ].freePhase = 0.25;
		RunEffects( s, 0 );
		const int ra = s.fxOut[ ui ][ 509 ];
		T( abs( ra - ( 128 + 51 ) ) <= 2, "LFO relativ um den Wert darunter: %d (erwartet ~179)", ra );
		s.dmx[ ui ][ 509 ] = 0;
		//envelope shape
		Effect env = FxMakeType( FxType::Envelope );
		env.atk = 0.1f; env.hold = 0; env.dec = 0.2f; env.sus = 0.5f; env.gate = 0.5f; env.rel = 0.2f; env.curve = 0;
		const float e1 = FxEnvValue( env, 0.05f ), e2 = FxEnvValue( env, 0.2f ), e3 = FxEnvValue( env, 0.4f ), e4 = FxEnvValue( env, 0.6f ), e5 = FxEnvValue( env, 0.8f );
		T( fabsf( e1 - 0.5f ) < 0.01f && fabsf( e2 - 0.75f ) < 0.01f && fabsf( e3 - 0.5f ) < 0.01f && fabsf( e4 - 0.25f ) < 0.01f && e5 < 0.001f, "Hüllkurve A/D/S/R: %.2f %.2f %.2f %.2f %.2f", e1, e2, e3, e4, e5 );
		//envelope by hand: nothing until triggered
		s.effects.clear();
		env.id         = 902;
		env.on         = true;
		env.fade       = 0;
		env.sync       = false;
		env.manualTrig = true;
		env.tgt.mode   = 2;
		env.tgt.uni    = 1;
		env.tgt.ch     = 511;
		env.hi         = 1;
		s.effects.push_back( env );
		RunEffects( s, 0.01 );
		const int before = s.fxOut[ ui ][ 510 ];
		s.effects[ 0 ].trigTime = s.effects[ 0 ].clock - 0.2;
		RunEffects( s, 0 );
		const int after = s.fxOut[ ui ][ 510 ];
		T( before < 0 && abs( after - 191 ) <= 3, "Hüllkurve von Hand ausgelöst: vorher unberührt (%d), 0,2 s danach %d", before, after );
		//save / load
		Effect sq = FxMakeType( FxType::Sequencer );
		FxResizeSteps( sq, 8 );
		sq.lanes.push_back( FxNewLane( sq, 1 ) );
		sq.lanes[ 1 ].tgt.mode = 2;
		sq.lanes[ 1 ].tgt.ch   = 77;
		sq.lanes[ 1 ].v[ 3 ]   = 0.33f;
		sq.lanes[ 1 ].mute     = true;
		sq.stepOffset          = 2;
		Effect sb;
		EffectFromJson( EffectToJson( sq ), sb );
		T( sb.type == FxType::Sequencer && sb.steps == 8 && sb.lanes.size() == 2 && sb.lanes[ 1 ].tgt.ch == 77 && fabsf( sb.lanes[ 1 ].v[ 3 ] - 0.33f ) < 1e-4f && sb.lanes[ 1 ].mute && sb.stepOffset == 2,
		   "Sequencer speichern / laden: %d Schritte, %d Spuren", sb.steps, (int)sb.lanes.size() );
		Json old = EffectToJson( FxMakeType( FxType::Chase ) );
		Effect lg;
		Json o2 = Json::Obj();
		for( auto& kv : old.o )
			if( kv.first != "type" ) o2.Set( kv.first, kv.second );
		EffectFromJson( o2, lg );
		T( lg.type == FxType::Intensity, "Alte Show ohne Effekttyp: Intensität erkannt (%s)", FxTypeName( lg.type ) );
		{
			Json o3 = EffectToJson( FxMakeType( FxType::Rainbow ) );
			o3.Set( "type", (int)FxType::ColorFade );//saved by the first 1.2 builder
			Effect c3;
			EffectFromJson( o3, c3 );
			T( c3.type == FxType::Colour && FxMakeType( FxType::Strobe ).type == FxType::Intensity, "Lauflicht/Strobe/... = Intensität, Farbtypen = Farbe (ein Effekt, Presets)" );
		}
		//drawn curves: linear, step, spline, open (envelope)
		{
			std::vector< FxPoint > c = { { 0, 0, 0 }, { 0.5f, 1, 3 } };
			const float l = FxCurveValue( c, 0.25f, true ), st = FxCurveValue( c, 0.75f, true ), wrap = FxCurveValue( c, 1.25f, true );
			c[ 0 ].shape = 2;
			const float sp = FxCurveValue( c, 0.25f, true );
			c[ 0 ].shape = 1;
			const float ez = FxCurveValue( c, 0.1f, true );
			T( fabsf( l - 0.5f ) < 1e-3f && fabsf( st - 1 ) < 1e-3f && fabsf( wrap - 0.5f ) < 1e-3f && sp > 0.3f && sp < 0.7f && ez < 0.2f,
			   "Gezeichnete Welle: linear %.2f, Stufe %.2f, periodisch %.2f, Kurve %.2f, abgerundet %.2f", l, st, wrap, sp, ez );
			Effect dw  = FxMakeType( FxType::Lfo );
			dw.id      = 950;
			dw.on      = true;
			dw.fade    = 0;
			dw.sync    = false;
			dw.hi      = 1;
			dw.wave    = FxWave::Custom;
			dw.pts     = { { 0, 0, 0 }, { 0.5f, 1, 0 } };
			dw.tgt.mode = 2;
			dw.tgt.uni  = 1;
			dw.tgt.ch   = 505;
			s.effects.clear();
			s.effects.push_back( dw );
			s.effects[ 0 ].freePhase = 0.25;
			RunEffects( s, 0 );
			const int ui2 = s.UniIndex( 1 );
			const int dv  = s.fxOut[ ui2 ][ 504 ];
			Effect ev = FxMakeType( FxType::Envelope );
			ev.envDrawn = true;
			ev.gate     = 2;
			ev.pts      = { { 0, 0, 0 }, { 0.5f, 1, 0 }, { 1, 0, 0 } };
			const float e1 = FxEnvValue( ev, 1.0f ), e2 = FxEnvValue( ev, 0.5f ), e3 = FxEnvValue( ev, 2.5f );
			Effect rb;
			EffectFromJson( EffectToJson( dw ), rb );
			T( abs( dv - 128 ) <= 1 && fabsf( e1 - 1 ) < 1e-3f && fabsf( e2 - 0.5f ) < 1e-3f && e3 == 0 && rb.wave == FxWave::Custom && rb.pts.size() == 2,
			   "LFO gezeichnet auf DMX (%d), Hüllkurve gezeichnet %.2f / %.2f / %.2f, speichern / laden", dv, e1, e2, e3 );
			s.effects.clear();
		}
		StopAllEffects( s );
		s.effects.clear();
		RunEffects( s, 0.1 );
	}
	//---- PATCH order (drag & drop): ripple re-addressing, everything on DMX channels follows the lamps
	{
		app.NewShow();
		app.sel.clear();
		const FixtureDef* par = nullptr;
		for( auto& d : app.lib.defs )
		{
			if( d->id.rfind( "generic/", 0 ) != 0 || d->kind != Kind::Par ) continue;
			FixtureInst tmp;
			tmp.def = d.get();
			if( s.HasAttr( tmp, Attr::Dimmer ) && s.HasAttr( tmp, Attr::Red ) ) { par = d.get(); break; }
		}
		const int P = par ? par->modes[ 0 ].Footprint() : 0;
		std::vector< int > id;
		for( int i = 0; i < 3 && par; ++i )
			if( FixtureInst* f = app.AddFixture( par, 0, 1, 1 + i * P, V3( (float)i, 6, 0 ), V3( 180, 0, 0 ), "" ) ) id.push_back( f->id );
		FixtureInst* dd = par ? app.AddFixture( par, 0, 1, 200, V3( 4, 6, 0 ), V3( 180, 0, 0 ), "" ) : nullptr;
		T( id.size() == 3 && dd && P > 0 && P < 40, "Umsortieren: 4 Lampen (%d Kanäle) angelegt", P );
		if( id.size() == 3 && dd && P > 0 && P < 40 )
		{
			const int dId = dd->id;
			auto F        = [ & ]( int i ) { return s.FindFixture( i ); };
			s.Set( *F( id[ 0 ] ), 0, 11 );
			s.Set( *F( id[ 1 ] ), 0, 55 );
			s.Set( *F( id[ 2 ] ), 0, 77 );
			s.Set( *F( dId ), 0, 99 );
			const int cA = F( id[ 2 ] )->address;
			Effect e;
			e.id       = 9001;
			e.type     = FxType::Lfo;
			e.tgt.mode = 2;
			e.tgt.uni  = 1;
			e.tgt.ch   = cA;
			s.effects.push_back( e );
			MidiMap mm;
			mm.target     = MidiTarget::DmxChannel;
			mm.universe   = 1;
			mm.channelDmx = cA + 1;
			s.midi.push_back( mm );
			TlTrack tt;
			tt.kind = TlTrackKind::Dmx;
			TlLane ln;
			ln.kind = TlLaneKind::Dmx;
			ln.uni  = 1;
			ln.ch   = cA;
			tt.lanes.push_back( ln );
			s.tl.tracks.push_back( tt );
			AbletonMap am;
			am.knobs[ 0 ] = "dmx:1:" + std::to_string( cA );
			am.state      = "x%22uni%3A1%3A" + std::to_string( cA ) + "%22";
			s.ableton.push_back( am );
			//C in front of A: C = 1, A and B move up, D (behind a gap) stays at 200
			const bool ok1 = app.MoveFixturesUi( id[ 2 ], id[ 0 ], false );
			DmxFrame* fr   = s.Frame( 1 );
			T( ok1 && s.fixtures[ 0 ].id == id[ 2 ] && F( id[ 2 ] )->address == 1 && F( id[ 0 ] )->address == 1 + P && F( id[ 1 ] )->address == 1 + 2 * P && F( dId )->address == 200 && ( *fr )[ 0 ] == 77 && ( *fr )[ P ] == 11 && ( *fr )[ 2 * P ] == 55,
			   "Umsortieren C vor A: Adressen %d / %d / %d, D %d, Programmer-Werte folgen (%d %d %d)", F( id[ 2 ] )->address, F( id[ 0 ] )->address, F( id[ 1 ] )->address, F( dId )->address, ( *fr )[ 0 ], ( *fr )[ P ], ( *fr )[ 2 * P ] );
			T( s.effects.back().tgt.ch == 1 && s.midi.back().channelDmx == 2 && s.tl.tracks.back().lanes[ 0 ].ch == 1 && s.ableton.back().knobs[ 0 ] == "dmx:1:1" && s.ableton.back().state == "x%22uni%3A1%3A1%22",
			   "Umsortieren: Modulator-DMX-Ziel %d, MIDI-Map %d, DMX-Time-Lane %d, Ableton %s / %s folgen", s.effects.back().tgt.ch, s.midi.back().channelDmx, s.tl.tracks.back().lanes[ 0 ].ch, s.ableton.back().knobs[ 0 ].c_str(), s.ableton.back().state.c_str() );
			//A behind D: the hole closes (B moves down), the gap before D stays, A = 200 + P
			const bool ok2 = app.MoveFixturesUi( id[ 0 ], dId, true );
			T( ok2 && F( id[ 1 ] )->address == 1 + P && F( dId )->address == 200 && F( id[ 0 ] )->address == 200 + P && ( *fr )[ P ] == 55 && ( *fr )[ 199 ] == 99 && ( *fr )[ 199 + P ] == 11 && s.fixtures.back().id == id[ 0 ],
			   "Umsortieren A hinter D: B %d, D %d (Lücke bleibt), A %d", F( id[ 1 ] )->address, F( dId )->address, F( id[ 0 ] )->address );
			//no room at the end of the universe: refused, nothing changes
			FixtureInst* ee = app.AddFixture( par, 0, 1, 512 - P + 1, V3( 5, 6, 0 ), V3( 180, 0, 0 ), "" );
			const int bA    = F( id[ 1 ] )->address;
			const bool ok3  = ee && app.MoveFixturesUi( id[ 1 ], ee->id, true );
			T( ee && !ok3 && F( id[ 1 ] )->address == bA && ( *fr )[ bA - 1 ] == 55, "Umsortieren ohne Platz wird abgelehnt, nichts geändert" );
			//into another universe: B in front of a lamp in universe 2 (that one moves up), the value goes along
			const int u2    = s.AddUniverse();
			FixtureInst* g2 = app.AddFixture( par, 0, u2, 1, V3( 6, 6, 0 ), V3( 180, 0, 0 ), "" );
			const int g2Id  = g2 ? g2->id : 0;
			const bool ok4  = g2 && app.MoveFixturesUi( id[ 1 ], g2Id, false );
			fr              = s.Frame( 1 );
			DmxFrame* fr2   = s.Frame( u2 );
			T( ok4 && F( id[ 1 ] )->universe == u2 && F( id[ 1 ] )->address == 1 && F( g2Id )->address == 1 + P && fr2 && ( *fr2 )[ 0 ] == 55 && ( *fr )[ bA - 1 ] == 0,
			   "Umsortieren in Universe %d: B %d.%03d, dortige Lampe rückt auf %d, Wert folgt", u2, F( id[ 1 ] )->universe, F( id[ 1 ] )->address, g2Id ? F( g2Id )->address : 0 );
		}
		s.effects.clear();
		s.midi.clear();
		s.tl.tracks.clear();
		s.ableton.clear();
		app.NewShow();
	}
	//---- drag & drop onto the Time Line: snapshot / effect block at a time, audio file
	{
		app.NewShow();
		Timeline& tl = app.show.tl;
		tl.tracks.clear();
		tl.length = 20;
		const uint32_t u1 = app.TimelineAddClip( (int)TlClipKind::Snapshot, 77, 0, 3.0, 0 );
		const uint32_t u2 = app.TimelineAddClip( (int)TlClipKind::Effect, 0, 5, 10.0, u1 );
		TlTrack g2;
		g2.uid  = tl.NewUid();
		g2.kind = TlTrackKind::Fixtures;
		tl.tracks.push_back( g2 );
		const uint32_t u3 = app.TimelineAddClip( (int)TlClipKind::Snapshot, 78, 0, 19.5, g2.uid );//not a clip track: first clip track
		TlTrack* ct = tl.Find( u1 );
		T( u1 && u2 == u1 && u3 == u1 && ct && ct->kind == TlTrackKind::Clips && ct->clips.size() == 3 && ct->clips[ 0 ].snap == 77 && fabs( ct->clips[ 0 ].t - 3.0 ) < 1e-9 && ct->clips[ 1 ].kind == TlClipKind::Effect && ct->clips[ 1 ].fx == 5 && tl.length >= 19.5 + ct->clips[ 2 ].len - 1e-9 && tl.tracks.size() == 2,
		   "Drag & Drop Time Line: Block legt Clip-Spur an, weitere Blöcke in dieselbe Spur (%d Blöcke), Länge wächst auf %.1f s", ct ? (int)ct->clips.size() : -1, tl.length );
		const bool a1 = app.AddAudioTrack( "C:\\x\\eins.wav", 2.0 );
		const bool a2 = app.AddAudioTrack( "C:\\x\\zwei.mp3", -1 );
		int na = 0;
		for( auto& t : tl.tracks ) na += t.kind == TlTrackKind::Audio;
		T( a1 && !a2 && na == 1 && tl.tracks[ 0 ].kind == TlTrackKind::Audio && tl.tracks[ 0 ].name == "zwei" && fabs( tl.tracks[ 0 ].offset - 2.0 ) < 1e-9,
		   "Drag & Drop Audio: erste Datei = neue Audiospur bei 2 s, zweite ersetzt die Datei (%d Audiospur, '%s', Versatz %.1f s)", na, tl.tracks[ 0 ].name.c_str(), tl.tracks[ 0 ].offset );
		tl.tracks.clear();
		app.NewShow();
	}
	//---- sections: blocks of a collapsed track (move, copy, trim, split, join) + range over all tracks
	{
		app.NewShow();
		Show& sh     = app.show;
		Timeline& tl = sh.tl;
		tl.tracks.clear();
		tl.markers.clear();
		tl.length = 30;
		TlTrack tr;
		tr.uid  = tl.NewUid();
		tr.kind = TlTrackKind::Fixtures;
		tr.open = false;
		tr.lanes.resize( 2 );
		tr.lanes[ 1 ].attr = Attr::Pan;
		for( double t : { 0.0, 1.0, 2.0, 10.0, 11.0 } ) TlSetKey( tr.lanes[ 0 ], t, t < 5 ? (float)( t / 2 ) : (float)( t - 10 ), nullptr, TlInterp::Linear );
		for( double t : { 0.5, 1.5, 10.5 } ) TlSetKey( tr.lanes[ 1 ], t, 0.5f, nullptr, TlInterp::Linear );
		tl.tracks.push_back( tr );
		TlTrack& t = tl.tracks[ 0 ];
		std::vector< TlBlock > b;
		auto nKeys = [ & ] { return (int)( t.lanes[ 0 ].keys.size() + t.lanes[ 1 ].keys.size() ); };
		TlBlocks( tl, t, b );
		T( b.size() == 2 && fabs( b[ 0 ].t0 ) < 1e-9 && fabs( b[ 0 ].t1 - 2 ) < 1e-9 && b[ 0 ].keys == 5 && fabs( b[ 1 ].t0 - 10 ) < 1e-9 && fabs( b[ 1 ].t1 - 11 ) < 1e-9, "Abschnitte: Pause > 2 Takte trennt (%d Blöcke, erster %.1f-%.1f s mit %d Keys)", (int)b.size(), b[ 0 ].t0, b[ 0 ].t1, b[ 0 ].keys );
		const float vBefore = TlEval( t.lanes[ 0 ], 1.25 );
		const bool split    = TlSplitBlock( tl, t, 1.25 );
		TlBlocks( tl, t, b );
		T( split && b.size() == 3 && fabs( b[ 0 ].t1 - 1.249 ) < 1e-6 && fabs( b[ 1 ].t0 - 1.25 ) < 1e-9 && fabs( TlEval( t.lanes[ 0 ], 1.25 ) - vBefore ) < 1e-4 && fabs( TlEval( t.lanes[ 0 ], 1.6 ) - 0.8f ) < 1e-3 && !TlSplitBlock( tl, t, 6.0 ),
		   "Abschnitt teilen bei 1,25 s: %d Blöcke, Kurve bleibt (%.3f -> %.3f), außerhalb eines Blocks nichts", (int)b.size(), vBefore, TlEval( t.lanes[ 0 ], 1.25 ) );
		const int n0 = nKeys();
		TlMoveBlocks( tl, t, { { b[ 1 ].t0, b[ 1 ].t1 } }, 3.0, false );
		TlBlocks( tl, t, b );
		T( b.size() == 3 && fabs( b[ 1 ].t0 - 4.25 ) < 1e-9 && fabs( b[ 1 ].t1 - 5.0 ) < 1e-9 && nKeys() == n0 && fabs( TlEval( t.lanes[ 0 ], 5.0 ) - 1.0f ) < 1e-4, "Abschnitt verschieben +3 s: Block bei %.2f-%.2f s, %d Keys wie vorher, bleibt eigener Block", b[ 1 ].t0, b[ 1 ].t1, nKeys() );
		TlMoveBlocks( tl, t, { { 10.0, 11.0 } }, -5.0, true );
		TlBlocks( tl, t, b );
		T( b.size() == 4 && fabs( b[ 2 ].t0 - 5 ) < 1e-9 && fabs( b[ 2 ].t1 - 6 ) < 1e-9 && fabs( b[ 3 ].t0 - 10 ) < 1e-9 && fabs( b[ 1 ].t1 - 5 ) > 0.01 && fabs( TlEval( t.lanes[ 0 ], 6.0 ) - 1.0f ) < 1e-4 && fabs( TlEval( t.lanes[ 0 ], 5.0 ) ) < 1e-4,
		   "Abschnitt kopieren nach 5 s: %d Blöcke, Ziel überschrieben (Wert bei 5 s = %.2f), Original bleibt", (int)b.size(), TlEval( t.lanes[ 0 ], 5.0 ) );
		TlTrimBlock( tl, t, 10.0, 11.0, false, 10.5 );
		TlBlocks( tl, t, b );
		const bool trimOk = b.size() == 4 && fabs( b[ 3 ].t1 - 10.5 ) < 1e-9 && fabs( TlEval( t.lanes[ 0 ], 10.5 ) - 0.5f ) < 1e-4 && fabs( TlEval( t.lanes[ 0 ], 12.0 ) - 0.5f ) < 1e-4;
		TlTrimBlock( tl, t, 10.0, 10.5, true, 9.0 );//extend to the left: the curve stays
		TlBlocks( tl, t, b );
		T( trimOk && b.size() == 4 && fabs( b[ 3 ].t0 - 9.0 ) < 1e-9, "Abschnitt beschneiden: rechte Kante 11 -> 10,5 s (Wert dort %.2f), linke Kante auf %.1f s verlängert", TlEval( t.lanes[ 0 ], 10.5 ), b[ 3 ].t0 );
		{
			Timeline c2;
			TimelineFromJson( c2, TimelineToJson( tl ) );
			std::vector< TlBlock > b2;
			TlBlocks( c2, c2.tracks[ 0 ], b2 );
			const bool joined = TlJoinBlocks( tl, t, b[ 1 ].t1 );
			TlBlocks( tl, t, b );
			T( b2.size() == 4 && joined && b.size() == 3, "Abschnitte: Schnitte überleben Speichern / Undo (%d Blöcke), Verbinden -> %d Blöcke", (int)b2.size(), (int)b.size() );
		}
		//range over all tracks
		tl.markers.push_back( { 9.5, "A" } );
		Cue cu;
		cu.trigger = 2;
		cu.tc      = 9.2;
		sh.cues.push_back( cu );
		const int k0        = nKeys();
		const double first9 = b.back().t0;//9.0
		TlInsertTime( sh, 9.0, 2.0 );
		TlBlocks( tl, t, b );
		const bool insOk = fabs( b.back().t0 - ( first9 + 2 ) ) < 1e-9 && fabs( tl.markers[ 0 ].t - 11.5 ) < 1e-9 && fabs( sh.cues.back().tc - 11.2 ) < 1e-9 && fabs( tl.length - 32 ) < 1e-9 && nKeys() == k0;
		TlRangeClip rc;
		TlRangeCopy( sh, 0.0, 2.0, rc );
		int rcKeys = 0;
		for( auto& e : rc.keys ) rcKeys += (int)e.second.size();
		TlRangePaste( sh, rc, 20.0 );
		const bool pasteOk = nKeys() == k0 + rcKeys && fabs( tl.length - 34 ) < 1e-9 && rcKeys > 0;
		const double at    = TlRangeMove( sh, 11.0, 12.0, 0.0 );//the range with marker + cue to the start
		const bool moveOk  = fabs( at ) < 1e-9 && nKeys() == k0 + rcKeys && fabs( tl.markers[ 0 ].t - 0.5 ) < 1e-9 && sh.cues.back().trigger == 2 && fabs( sh.cues.back().tc - 0.2 ) < 1e-9 && fabs( tl.length - 34 ) < 1e-9;
		const int kBefore  = nKeys();
		TlDeleteTime( sh, 0.0, 1.0 );
		T( insOk && pasteOk && moveOk && nKeys() < kBefore && tl.markers.empty() && sh.cues.back().trigger == 0 && fabs( tl.length - 33 ) < 1e-9,
		   "Bereich über alle Spuren: Zeit einfügen %d, kopieren + einfügen %d (%d Keys), verschieben %d (Marker + Cue mit), ausschneiden: Cue wird GO-Cue, Länge %.0f s", insOk, pasteOk, rcKeys, moveOk, tl.length );
		tl.tracks.clear();
		tl.markers.clear();
		app.NewShow();
	}
	//---- 2.1 colour engine: emitter mixing, colour wheel, colour temperature, classic mode
	{
		app.NewShow();
		const FixtureDef* dRgb  = app.lib.Find( "ofl/cameo/flat-par-can-tri-7x-3w-ir" );
		const FixtureDef* dRgbw = app.lib.Find( "generic/generic/led-par-rgbw" );
		const FixtureDef* dRgba = app.lib.Find( "generic/generic/led-par-rgbwauv" );
		const FixtureDef* dSpot = app.lib.Find( "generic/generic/moving-head-spot" );
		const FixtureDef* dWc   = app.lib.Find( "ofl/generic/cw-ww-fader" );
		auto add = [ & ]( const FixtureDef* d, int mode, int adr ) { FixtureInst* f = app.AddFixture( d, mode, 1, adr, V3( 0, 6, 0 ), V3( 180, 0, 0 ), "" ); return f ? f->id : -1; };
		const int iRgb = add( dRgb, 2, 1 ), iRgbw = add( dRgbw, 0, 10 ), iRgba = add( dRgba, 0, 20 ), iSpot = add( dSpot, 0, 40 ), iWc = add( dWc, 0, 70 );
		T( iRgb >= 0 && iRgbw >= 0 && iRgba >= 0 && iSpot >= 0 && iWc >= 0, "Farb-Test: Lampen angelegt (RGB, RGBW, RGBWAUV, Spot mit Farbrad + CTO, Warm-/Kaltweiß)" );
		auto F = [ & ]( int id ) -> FixtureInst& { return *s.FindFixture( id ); };
		auto gv = [ & ]( int id, Attr a ) { return s.GetAttr( F( id ), a ); };
		auto close3 = []( const V3& a, const V3& b, float tol ) {
			const float ma = std::max( { a.x, a.y, a.z, 1e-4f } ), mb = std::max( { b.x, b.y, b.z, 1e-4f } );
			const V3 d = a / ma - b / mb;
			return fabsf( d.x ) < tol && fabsf( d.y ) < tol && fabsf( d.z ) < tol;
		};
		T( s.colorMix == 1, "Neue Show: Farbmischung Auto" );
		if( iRgb >= 0 && iRgbw >= 0 && iRgba >= 0 && iSpot >= 0 && iWc >= 0 )
		{
			//RGB-only lamps: exactly the colour, in both modes
			bool rgbSame = true;
			for( int mix = 0; mix < 2; ++mix )
			{
				s.colorMix = mix;
				s.SetColor( F( iRgb ), V3( 0.3f, 0.6f, 0.9f ) );
				rgbSame = rgbSame && fabsf( gv( iRgb, Attr::Red ) - 0.3f ) < 0.002f && fabsf( gv( iRgb, Attr::Green ) - 0.6f ) < 0.002f && fabsf( gv( iRgb, Attr::Blue ) - 0.9f ) < 0.002f;
			}
			T( rgbSame, "RGB-Lampe: Farbe 1:1 in beiden Mischarten" );
			//classic = the formula of 2.0 (R/G/B = colour, white = min, others 0) for many colours
			s.colorMix = 0;
			int bad = 0;
			std::vector< std::pair< Attr, float > > tg;
			for( int k = 0; k < 200; ++k )
			{
				const V3 c( ( k * 37 % 101 ) / 100.0f, ( k * 61 % 101 ) / 100.0f, ( k * 83 % 101 ) / 100.0f );
				for( int id : { iRgbw, iRgba } )
				{
					s.ColorTargets( F( id ), c, tg );
					for( auto& p : tg )
					{
						float want = 0;
						if( p.first == Attr::Red ) want = c.x;
						else if( p.first == Attr::Green ) want = c.y;
						else if( p.first == Attr::Blue ) want = c.z;
						else if( p.first == Attr::White ) want = std::min( { c.x, c.y, c.z } );
						if( fabsf( p.second - want ) > 1e-5f ) ++bad;
					}
				}
			}
			T( bad == 0, "Klassische Mischung = Formel von 2.0 (200 Farben, RGBW + RGBWAUV): %d Abweichungen", bad );
			//auto: pastel on RGBW uses white, keeps the hue, full brightness
			s.colorMix = 1;
			s.SetColor( F( iRgbw ), V3( 1, 0.6f, 0.6f ) );
			const float w = gv( iRgbw, Attr::White ), r = gv( iRgbw, Attr::Red ), g = gv( iRgbw, Attr::Green );
			const V3 back = s.GetColor( F( iRgbw ) );
			T( w > 0.4f && g < 0.5f && r > 0.95f && close3( back, V3( 1, 0.6f, 0.6f ), 0.04f ), "RGBW Pastell (1/.6/.6): W %.2f R %.2f G %.2f, Farbton zurück %.2f/%.2f/%.2f", w, r, g, back.x, back.y, back.z );
			s.SetColor( F( iRgbw ), V3( 1, 1, 1 ) );
			T( gv( iRgbw, Attr::White ) > 0.9f && gv( iRgbw, Attr::Red ) > 0.85f && gv( iRgbw, Attr::Blue ) > 0.85f, "RGBW Weiß: alle Emitter voll (W %.2f R %.2f B %.2f)", gv( iRgbw, Attr::White ), gv( iRgbw, Attr::Red ), gv( iRgbw, Attr::Blue ) );
			s.SetColor( F( iRgbw ), V3( 0.5f, 0.25f, 0.25f ) );
			const float pk = std::max( { gv( iRgbw, Attr::Red ), gv( iRgbw, Attr::White ), gv( iRgbw, Attr::Green ) } );
			T( fabsf( pk - 0.5f ) < 0.01f, "Helligkeit bleibt: halbe Farbe -> hellster Emitter %.2f", pk );
			//auto: orange uses amber, UV stays untouched
			s.SetAttr( F( iRgba ), Attr::UV, 0.7f );
			s.SetColor( F( iRgba ), V3( 1, 0.5f, 0 ) );
			const V3 ob = s.GetColor( F( iRgba ) );
			T( gv( iRgba, Attr::Amber ) > 0.5f && fabsf( gv( iRgba, Attr::UV ) - 0.7f ) < 0.01f && close3( ob, V3( 1, 0.5f, 0 ), 0.06f ), "RGBWAUV Orange: Amber %.2f, UV bleibt %.2f, Farbton %.2f/%.2f/%.2f", gv( iRgba, Attr::Amber ), gv( iRgba, Attr::UV ), ob.x, ob.y, ob.z );
			//primaries stay pure
			s.SetColor( F( iRgba ), V3( 0, 0, 1 ) );
			T( gv( iRgba, Attr::Blue ) > 0.99f && gv( iRgba, Attr::Red ) < 0.01f && gv( iRgba, Attr::White ) < 0.01f && gv( iRgba, Attr::Amber ) < 0.01f, "Reines Blau bleibt reines Blau" );
			//colour wheel
			std::string nr, nw, nb;
			s.SetColor( F( iSpot ), V3( 1, 0, 0 ) );
			const V3 cr = s.GetColor( F( iSpot ) );
			s.WheelValueFor( F( iSpot ), V3( 1, 0, 0 ), &nr );
			s.SetColor( F( iSpot ), V3( 0.1f, 0.2f, 1 ) );
			const V3 cb = s.GetColor( F( iSpot ) );
			s.SetColor( F( iSpot ), V3( 1, 1, 1 ) );
			const V3 cw = s.GetColor( F( iSpot ) );
			s.WheelValueFor( F( iSpot ), V3( 1, 1, 1 ), &nw );
			T( cr.x > 0.9f && cr.y < 0.2f && cb.z > 0.9f && cb.x < 0.3f && cw.x > 0.99f && cw.y > 0.99f && cw.z > 0.99f && nw == "Open" && s.HasColor( F( iSpot ) ),
			   "Farbrad: Rot -> %s (%.2f/%.2f/%.2f), Blau -> %.2f/%.2f/%.2f, Weiß -> %s", nr.c_str(), cr.x, cr.y, cr.z, cb.x, cb.y, cb.z, nw.c_str() );
			//group colour on a mixed group: every lamp shows the hue
			GroupFader gf;
			gf.name     = "Farbe";
			gf.kind     = 1;
			gf.fixtures = { iRgb, iRgbw, iRgba, iSpot };
			gf.hue      = 0.33f;
			gf.sat      = 1;
			gf.value    = 1;
			s.groups.push_back( gf );
			s.ApplyGroup( (int)s.groups.size() - 1 );
			const V3 want = Hsv( 0.33f, 1, 1 );
			int ok = 0;
			for( int id : { iRgb, iRgbw, iRgba } ) ok += close3( s.GetColor( F( id ) ), want, 0.08f );
			const V3 sg = s.GetColor( F( iSpot ) );
			ok += sg.y > 0.8f && sg.x < 0.4f && sg.z < 0.5f;
			T( ok == 4, "Gruppenfarbe Grün auf RGB, RGBW, RGBWAUV und Farbrad: %d/4 grün (Farbrad %.2f/%.2f/%.2f)", ok, sg.x, sg.y, sg.z );
			s.groups.pop_back();
			//palette fits a wheel lamp and sets its slot
			Palette pr;
			pr.uid   = s.NewPaletteUid();
			pr.name  = "Rot";
			pr.color = V3( 1, 0, 0 );
			s.palettes.push_back( pr );
			const int nAp = s.ApplyPalette( pr.uid, { iSpot } );
			const V3 cp   = s.GetColor( F( iSpot ) );
			T( nAp == 1 && cp.x > 0.9f && cp.y < 0.2f, "Farbpalette auf Farbrad-Lampe: Slot rot" );
			s.palettes.pop_back();
			//colour temperature
			s.SetKelvin( F( iSpot ), 3200 );
			const float cto32 = gv( iSpot, Attr::CTO );
			s.SetKelvin( F( iSpot ), 6500 );
			const float cto65 = gv( iSpot, Attr::CTO );
			s.SetKelvin( F( iWc ), 2700 );
			const float ww27 = gv( iWc, Attr::WarmWhite ), cw27 = gv( iWc, Attr::ColdWhite );
			s.SetKelvin( F( iWc ), 6500 );
			const float ww65 = gv( iWc, Attr::WarmWhite ), cw65 = gv( iWc, Attr::ColdWhite );
			s.SetColor( F( iRgb ), V3( 1, 1, 1 ) );
			s.SetKelvin( F( iRgb ), 3000 );
			const float kr = gv( iRgb, Attr::Red ), kg = gv( iRgb, Attr::Green ), kb = gv( iRgb, Attr::Blue );
			T( cto32 > 0.95f && cto65 < 0.05f && ww27 > 0.99f && cw27 < 0.01f && cw65 > 0.99f && ww65 < 0.01f && kr > 0.99f && kg < kr && kb < kg && s.HasKelvin( F( iWc ) ),
			   "Farbtemperatur: CTO 3200 K %.2f / 6500 K %.2f, Warm/Kalt 2700 K %.2f/%.2f, 6500 K %.2f/%.2f, RGB 3000 K %.2f/%.2f/%.2f", cto32, cto65, ww27, cw27, ww65, cw65, kr, kg, kb );
			//2.1 pan / tilt in degrees: OSC pan_deg / tilt_deg, kelvin via OSC
			{
				auto osc = [ & ]( const std::string& adr, float v, bool isInt ) { OscMsg om; om.address = adr; om.f = { v }; om.isInt = { isInt }; return ApplyOsc( s, om ); };
				const std::string base = "/ls/fixture/" + std::to_string( iSpot ) + "/";
				const bool o1 = osc( base + "pan_deg", 90, false ), o2 = osc( base + "tilt_deg", -45, false );
				const float pd = s.GetAttrDeg( F( iSpot ), Attr::Pan ), td = s.GetAttrDeg( F( iSpot ), Attr::Tilt );
				const float pr = s.AttrRangeDeg( F( iSpot ), Attr::Pan );
				s.SetAttrDeg( F( iSpot ), Attr::Pan, -pr * 0.5f );
				const float pmin = s.GetAttr( F( iSpot ), Attr::Pan );
				const bool o3 = osc( "/ls/fixture/" + std::to_string( iWc ) + "/kelvin", 2700, true );
				T( o1 && o2 && o3 && fabsf( pd - 90 ) < 0.1f && fabsf( td + 45 ) < 0.1f && pmin < 0.001f && s.GetAttr( F( iWc ), Attr::WarmWhite ) > 0.99f,
				   "Pan/Tilt in Grad: OSC pan_deg 90 -> %.1f°, tilt_deg -45 -> %.1f°, -%.0f° = Anschlag; OSC kelvin 2700 -> Warmweiß", pd, td, pr * 0.5f );
			}
			//file: saved, and shows without the key (before 2.1) load classic
			Json j = s.ToJson();
			const bool saved = j.Find( "colorMix" ) && j[ "colorMix" ].I( -1 ) == 1;
			for( size_t k = 0; k < j.o.size(); ++k )
				if( j.o[ k ].first == "colorMix" ) { j.o.erase( j.o.begin() + k ); break; }
			std::string err;
			const bool ld = s.FromJson( j, app.lib, "", &err );
			T( saved && ld && s.colorMix == 0, "Show-Datei: Farbmischung gespeichert, Shows vor 2.1 laden klassisch (%s)", err.c_str() );
			//speed
			const double t0 = Now();
			for( int k = 0; k < 20000; ++k ) s.ColorTargets( F( iRgba ), V3( ( k % 7 ) / 6.0f, ( k % 5 ) / 4.0f, ( k % 3 ) / 2.0f ), tg );
			s.colorMix = 1;
			for( int k = 0; k < 20000; ++k ) s.ColorTargets( F( iRgba ), V3( ( k % 7 ) / 6.0f, ( k % 5 ) / 4.0f, ( k % 3 ) / 2.0f ), tg );
			const double ms = ( Now() - t0 ) * 1000.0;
			T( ms < 40, "Farbmischung schnell: 40000 Aufrufe in %.1f ms", ms );
		}
		app.NewShow();
	}
	//---- 2.1 quality "high": renders without GL errors, looks different from normal, bounded cost
	{
		app.BuildDemo();
		Run( app, 2.0f );
		const bool offByDefault = !s.stage.floorSurf.reflect && !s.stage.deckSurf.reflect;
		s.stage.floorSurf = { true, 0.7f, 0.85f };//reflections are per surface (off by default)
		s.stage.deckSurf  = { true, 0.5f, 0.4f };
		{
			Json j = s.ToJson();
			Show tmp;
			std::string e;
			tmp.FromJson( j, app.lib, "", &e );
			Json o = j;
			Json& stj = const_cast< Json& >( o[ "stage" ] );
			for( size_t k = 0; k < stj.o.size(); ++k )
				if( stj.o[ k ].first == "floorSurf" ) { stj.o.erase( stj.o.begin() + k ); break; }
			Show old;
			old.FromJson( o, app.lib, "", &e );
			T( offByDefault && tmp.stage.floorSurf.reflect && fabsf( tmp.stage.floorSurf.gloss - 0.85f ) < 1e-4f && fabsf( tmp.stage.deckSurf.amount - 0.5f ) < 1e-4f && !old.stage.floorSurf.reflect,
			   "Spiegelung pro Fläche: Standard aus, Boden/Podest gespeichert, Shows ohne Angabe ohne Spiegelung" );
		}
		std::vector< uint8_t > pn, ph;
		auto shot = [ & ]( bool high, const std::string& file, std::vector< uint8_t >& px ) {
			app.ren.quality      = Renderer::Quality();
			app.ren.quality.high = high;
			SavePng( app, dir + file, 1280, 720 );//also warms up (shadow array, buffers)
			int W = 0, H = 0;
			app.ren.ReadOutput( px, W, H );
			glFinish();
			const double t0 = Now();
			for( int i = 0; i < 6; ++i ) app.RenderViewport( 1280, 720 );
			glFinish();
			return ( Now() - t0 ) / 6 * 1000.0;
		};
		while( glGetError() != GL_NO_ERROR ) {}
		const double tn = shot( false, "\\selftest_quality_normal.png", pn );
		const double th = shot( true, "\\selftest_quality_high.png", ph );
		const GLenum ge = glGetError();
		int diff = 0;
		for( size_t i = 0; i + 2 < pn.size() && i + 2 < ph.size(); i += 4 )
			if( abs( pn[ i ] - ph[ i ] ) + abs( pn[ i + 1 ] - ph[ i + 1 ] ) + abs( pn[ i + 2 ] - ph[ i + 2 ] ) > 12 ) ++diff;
		const float share = pn.empty() ? 0 : diff * 400.0f / pn.size();
		app.ren.quality = Renderer::Quality();
		app.RenderViewport( 640, 360 );//back to the normal shadow array
		T( ge == GL_NO_ERROR && pn.size() == ph.size() && share > 2.0f, "Qualität Hoch rendert (kein GL-Fehler), sichtbar anders als Normal: %.1f %% der Pixel", share );
		T( th < tn * 4.0 + 10.0, "Qualität Hoch kostet begrenzt: Normal %.1f ms, Hoch %.1f ms je Bild (1280x720)", tn, th );
	}
	//---- 16 universes: patch, output addressing, show file round trip
	{
		app.NewShow();
		while( (int)s.universes.size() < 16 ) s.AddUniverse();
		const FixtureDef* par = app.lib.Find( "generic/generic/led-par-rgbw" );
		FixtureInst* f16      = par ? app.AddFixture( par, 0, 16, 505, V3( 0, 6, 0 ), V3( 180, 0, 0 ), "" ) : nullptr;
		const int id16        = f16 ? f16->id : -1;
		if( f16 ) s.SetAttr( *f16, Attr::Dimmer, 1 );
		std::set< int > ports;
		for( auto& u : s.universes ) ports.insert( u.artnetPort );
		const DmxFrame* fr = s.Frame( 16 );
		const bool dmxOk   = fr && ( *fr )[ 504 ] == 255;
		uint8_t cid[ 16 ]  = {};
		const auto pk      = BuildE131( 16, 1, 100, cid, "LS", fr ? fr->data() : nullptr, 512 );
		int un = 0, pr = 0, ln = 0;
		const uint8_t* dp = nullptr;
		const bool sacnOk = fr && ParseE131( pk.data(), (int)pk.size(), un, pr, dp, ln ) && un == 16 && ln == 512 && dp[ 504 ] == 255;
		Json j = s.ToJson();
		std::string err;
		const bool ld = s.FromJson( j, app.lib, "", &err );
		const FixtureInst* back = s.FindFixture( id16 );
		T( (int)s.universes.size() == 16 && ports.size() == 16 && dmxOk && sacnOk && ld && back && back->universe == 16 && back->address == 505,
		   "16 Universes: Lampe auf 16.505, Art-Net-Ports eindeutig (%d), sACN Universe 16, Show-Datei behält alles", (int)ports.size() );
		app.NewShow();
	}
	//---- language (UI texts German -> English)
	{
		const int old = gLang;
		gLang         = kLangEn;
		std::string bad;
		const int n = TrSelfCheck( bad );
		T( n > 800 && bad.empty(), "Übersetzung: %d Einträge, Platzhalter / Listen / IDs passen%s", n, bad.c_str() );
		T( !strcmp( L( "Speichern" ), "Save" ) && !strcmp( L( "Leinwand " ), "Screen " ) && !strcmp( Ls( std::string( "Lauflicht" ) ), "Chase" ) && !strcmp( L( "Nicht übersetzt XYZ" ), "Nicht übersetzt XYZ" ), "Englisch: Speichern -> %s, Leinwand -> %s", L( "Speichern" ), L( "Leinwand " ) );
		const char* fmts = L( "16:9\0" "16:10\0" "4:3\0" "21:9\0" "1:1\0" "frei\0" );
		const char* p    = fmts;
		int items        = 0;
		std::string last;
		while( *p ) { last = p; p += strlen( p ) + 1; ++items; }
		T( items == 6 && last == "free", "Combo-Liste übersetzt: %d Einträge, letzter '%s'", items, last.c_str() );
		const wchar_t* w = LW( L"LightStage Show (*.lightstage)\0*.lightstage\0Alle Dateien\0*.*\0" );
		const wchar_t* w3 = w + wcslen( w ) + 1;
		w3 += wcslen( w3 ) + 1;
		T( !wcscmp( w3, L"All files" ) && !wcscmp( w3 + wcslen( w3 ) + 1, L"*.*" ), "Dateifilter (Windows-Dialog) übersetzt" );
		gLang = kLangDe;
		T( !strcmp( L( "Speichern" ), "Speichern" ), "Deutsch bleibt unverändert" );
		gLang = old;
	}
	fprintf( g_out, "\n%d OK, %d FAIL\n", g_pass, g_fail );
	fclose( g_out );
	return g_fail ? 1 : 0;
}
}//namespace ls
