// demo show, aiming helper, demo movement
#include "app.h"

namespace ls
{
static int SlotOf( const FixtureInst& f, const char* name )
{
	const Mode* m = f.M();
	if( !m ) return -1;
	for( int i = 0; i < (int)m->ch.size(); ++i )
		if( m->ch[ i ].name == name ) return i;
	return -1;
}
static void SetNamed( Show& s, FixtureInst& f, const char* name, int v )
{
	int i = SlotOf( f, name );
	if( i >= 0 ) s.Set( f, i, (uint8_t)Clamp( v, 0, 255 ) );
}

bool App::AimAt( Show& s, FixtureInst& f, const V3& target )
{
	if( !f.def || ( f.def->kind != Kind::MovingHead && f.def->kind != Kind::Scanner ) ) return false;
	const Mode* m = f.M();
	if( !m ) return false;
	V3 e  = BodyExtents( *f.def );
	V3 q  = XformP( Inverse( FixtureBase( f ) ), target ) - V3( 0, e.y * 0.62f, 0 );
	q     = Norm( q );
	float t = acosf( Clamp( q.y, -1.0f, 1.0f ) ) / kDeg;
	float p = atan2f( q.x, q.z ) / kDeg;
	float pr = f.def->panRange * 0.5f, tr = f.def->tiltRange * 0.5f;
	//candidates: (p, t) and (p +- 180, -t); pick one inside the ranges, closest to centre
	struct C { float p, t; };
	C cs[ 4 ] = { { p, t }, { p - 360, t }, { p + 180, -t }, { p - 180, -t } };
	float best = 1e9f;
	C pick     = cs[ 0 ];
	for( auto& c : cs )
	{
		if( c.p > 180 + pr ) c.p -= 360;
		if( fabsf( c.p ) > pr + 0.01f || fabsf( c.t ) > tr + 0.01f ) continue;
		float cost = fabsf( c.p ) + fabsf( c.t ) * 0.5f;
		if( cost < best ) { best = cost; pick = c; }
	}
	if( f.invertPan ) pick.p = -pick.p;
	if( f.invertTilt ) pick.t = -pick.t;
	auto write = [ & ]( Attr a, CapK k, float ang ) {
		for( int i = 0; i < (int)m->ch.size(); ++i )
		{
			const Channel& c = m->ch[ i ];
			if( c.isFine || c.attr != a ) continue;
			float mn = 1e9f, mx = -1e9f;
			const Cap* cap = nullptr;
			for( auto& cp : c.caps )
				if( cp.k == k ) { mn = std::min( { mn, cp.a, cp.b } ); mx = std::max( { mx, cp.a, cp.b } ); if( !cap ) cap = &cp; }
			if( !cap || cap->b == cap->a ) continue;
			float abs = ang + ( mn + mx ) * 0.5f;
			float n   = Saturate( ( abs - cap->a ) / ( cap->b - cap->a ) );
			s.Set16( f, i, (uint16_t)( cap->lo + n * ( cap->hi - cap->lo ) ) );
			return;
		}
	};
	write( Attr::Pan, CapK::Pan, pick.p );
	write( Attr::Tilt, CapK::Tilt, pick.t );
	return true;
}

//2.1 start-up demo: a small jazz club (wooden room, round stage, curtain, piano, bass, drums, bistro tables, bar
//as one imported 3D model: assets\jazzclub\jazzclub.glb, built in Blender), warm light, little haze
void App::BuildJazzDemo()
{
	show.Clear();
	show.AddUniverse();
	StageCfg& st  = show.stage;
	st.width      = 11;
	st.depth      = 6;
	st.deck       = false;
	st.deckHeight = 0.4f;
	st.wallHeight = 4.2f;
	st.haze       = 0.14f;
	st.hazeNoise  = 0.6f;
	st.ambient    = 0.012f;
	st.envLight   = 0.9f;
	st.ambientColor = V3( 1.0f, 0.78f, 0.55f );
	st.floorColor = V3( 0.11f, 0.06f, 0.03f );
	st.wallColor  = V3( 0.05f, 0.025f, 0.015f );
	st.floorSurf  = { true, 0.35f, 0.55f };//polished wooden floor (visible with quality "high")
	st.deckSurf   = { true, 0.3f, 0.5f };
	Deck dk;
	dk.name   = L( "Bühne" );
	dk.shape  = 1;
	dk.z      = -1.85f;
	dk.width  = 7.2f;
	dk.depth  = 3.4f;
	dk.height = 0.4f;
	dk.ownColor = true;
	dk.color  = V3( 0.16f, 0.08f, 0.035f );
	dk.edge   = V3( 1.0f, 0.55f, 0.15f );
	dk.edgeLevel = 0.5f;
	dk.surf   = { true, 0.3f, 0.6f };
	st.decks.push_back( dk );
	//furniture, instruments, curtain, bar, walls
	{
		ModelObj m;
		m.path = exeDir + "\\assets\\jazzclub\\jazzclub.glb";
		m.name = "Jazzclub";
		std::string err;
		if( ren.EnsureModel( m, &err ) ) show.models.push_back( m );
		else Log( "jazz club model: %s %s", m.path.c_str(), err.c_str() );
	}
	auto truss = [ & ]( V3 a, V3 b ) { Truss t; t.a = a; t.b = b; show.trusses.push_back( t ); };
	truss( V3( -4.2f, 3.7f, 0.9f ), V3( 4.2f, 3.7f, 0.9f ) );
	truss( V3( -3.4f, 3.7f, -2.9f ), V3( 3.4f, 3.7f, -2.9f ) );
	const FixtureDef* spot = lib.Find( "generic/generic/moving-head-spot" );
	const FixtureDef* wash = lib.Find( "generic/generic/moving-head-wash" );
	const FixtureDef* par  = lib.Find( "generic/generic/led-par-rgbwauv" );
	auto next = [ & ]( const FixtureDef* d ) { return d ? std::max( 1, show.FreeAddress( 1, d->modes[ 0 ].Footprint() ) ) : 1; };
	const float hy = 3.7f - 0.165f;
	std::vector< int > fronts, backs, spots, ups;
	const float fx[ 4 ] = { -3.0f, -1.0f, 1.0f, 3.0f };
	for( int i = 0; i < 4 && wash; ++i ) fronts.push_back( AddFixture( wash, 0, 1, next( wash ), V3( fx[ i ], hy, 0.9f ), V3( 180, 0, 0 ), L( "Front " ) + std::to_string( i + 1 ) )->id );
	for( int i = 0; i < 2 && spot; ++i ) spots.push_back( AddFixture( spot, 0, 1, next( spot ), V3( i ? 2.0f : -2.0f, hy, 0.9f ), V3( 180, 0, 0 ), "Spot " + std::to_string( i + 1 ) )->id );
	for( int i = 0; i < 3 && wash; ++i ) backs.push_back( AddFixture( wash, 0, 1, next( wash ), V3( -2.6f + i * 2.6f, hy, -2.9f ), V3( 180, 0, 0 ), L( "Gegenlicht " ) + std::to_string( i + 1 ) )->id );
	for( int i = 0; i < 4 && par; ++i ) ups.push_back( AddFixture( par, 0, 1, next( par ), V3( -3.0f + i * 2.0f, 0.42f, -3.05f ), V3( -12, 0, 0 ), L( "Vorhang " ) + std::to_string( i + 1 ) )->id );
	show.AutoMount();
	show.UpdateMounts();
	auto F    = [ & ]( int id ) { return show.FindFixture( id ); };
	auto all  = [ & ]() { std::vector< int > r; for( auto& f : show.fixtures ) r.push_back( f.id ); return r; };
	//look: warm front light, amber/blue back light, red curtain, gobo spot on the piano
	auto look = [ & ]( float front, V3 back, V3 curtain, float piano ) {
		const V3 aim[ 4 ] = { V3( -1.5f, 0.6f, -2.0f ), V3( -0.4f, 1.2f, -1.2f ), V3( 0.6f, 1.2f, -2.0f ), V3( 2.0f, 0.8f, -2.2f ) };
		for( size_t i = 0; i < fronts.size(); ++i )
		{
			FixtureInst& f = *F( fronts[ i ] );
			show.Home( f );
			show.SetColor( f, V3( 1.0f, 0.62f, 0.3f ) );
			show.SetAttr( f, Attr::Dimmer, front );
			show.SetAttr( f, Attr::Zoom, 0.35f );
			AimAt( show, f, aim[ i ] );
		}
		for( size_t i = 0; i < backs.size(); ++i )
		{
			FixtureInst& f = *F( backs[ i ] );
			show.Home( f );
			show.SetColor( f, i == 1 ? V3( 1.0f, 0.45f, 0.1f ) : back );
			show.SetAttr( f, Attr::Dimmer, 0.8f );
			show.SetAttr( f, Attr::Zoom, 0.45f );
			AimAt( show, f, V3( -2.4f + i * 2.4f, 0.0f, 0.6f ) );
		}
		for( size_t i = 0; i < spots.size(); ++i )
		{
			FixtureInst& f = *F( spots[ i ] );
			show.Home( f );
			show.SetAttr( f, Attr::Dimmer, i == 0 ? piano : 0.55f );
			SetNamed( show, f, "Color Wheel", i == 0 ? 8 * 8 + 4 : 2 * 8 + 4 );//CTO / orange
			SetNamed( show, f, "Gobo Wheel", i == 0 ? 0 : 3 * 8 + 4 );         //open / breakup
			SetNamed( show, f, "Zoom", i == 0 ? 20 : 120 );
			SetNamed( show, f, "Focus", 120 );
			AimAt( show, f, i == 0 ? V3( -1.5f, 1.1f, -2.0f ) : V3( 0.5f, 1.8f, -3.3f ) );
		}
		for( size_t i = 0; i < ups.size(); ++i )
		{
			FixtureInst& f = *F( ups[ i ] );
			show.SetAttr( f, Attr::Dimmer, 1 );
			show.SetColor( f, curtain );
			show.SetAttr( f, Attr::Shutter, 0 );
		}
	};
	look( 0.75f, V3( 0.15f, 0.25f, 1.0f ), V3( 1.0f, 0.12f, 0.02f ), 0.0f );
	int s1 = show.TakeSnapshotOf( L( "Set 1" ), all(), false, false );
	look( 0.25f, V3( 0.35f, 0.05f, 1.0f ), V3( 0.6f, 0.0f, 0.4f ), 1.0f );
	int s2 = show.TakeSnapshotOf( L( "Piano-Solo" ), all(), false, false );
	look( 0.4f, V3( 0.05f, 0.1f, 0.7f ), V3( 0.3f, 0.05f, 0.9f ), 0.35f );
	int s3 = show.TakeSnapshotOf( L( "Ballade" ), all(), false, false );
	for( int i : { s1, s2, s3 } )
		if( i >= 0 ) show.snapshots[ i ].fade = 3.0f;
	look( 0.9f, V3( 0.15f, 0.25f, 1.0f ), V3( 1.0f, 0.12f, 0.02f ), 0.0f );
	show.colorMix = 1;
	show.dirty    = false;
	cam.yaw       = 8;
	cam.pitch     = 11;
	cam.dist      = 8.0f;
	cam.target    = V3( 0, 1.1f, -1.3f );
}

void App::BuildDemo()
{
	show.Clear();
	show.AddUniverse();
	show.AddUniverse();
	StageCfg& st = show.stage;
	st.width      = 14;
	st.depth      = 9;
	st.deckHeight = 1.0f;
	st.haze       = 0.45f;
	auto truss    = [ & ]( V3 a, V3 b ) { Truss t; t.a = a; t.b = b; show.trusses.push_back( t ); };
	truss( V3( -7.5f, 7.6f, -3.3f ), V3( 7.5f, 7.6f, -3.3f ) );
	truss( V3( -6.5f, 7.9f, 0.3f ), V3( 6.5f, 7.9f, 0.3f ) );
	truss( V3( -7.5f, 7.2f, 3.6f ), V3( 7.5f, 7.2f, 3.6f ) );
	const FixtureDef* spot   = lib.Find( "generic/generic/moving-head-spot" );
	const FixtureDef* wash   = lib.Find( "generic/generic/moving-head-wash" );
	const FixtureDef* beam   = lib.Find( "generic/generic/moving-head-beam" );
	const FixtureDef* strobe = lib.Find( "generic/generic/led-strobe" );
	const FixtureDef* par    = lib.Find( "generic/generic/led-par-rgbwauv" );
	const FixtureDef* bar    = lib.Find( "generic/generic/led-bar-8px" );
	const FixtureDef* blind  = lib.Find( "generic/generic/blinder-4" );
	const FixtureDef* hazer  = lib.Find( "generic/generic/hazer" );
	auto next = [ & ]( int uni, const FixtureDef* d ) { return d ? std::max( 1, show.FreeAddress( uni, d->modes[ 0 ].Footprint() ) ) : 1; };
	float hb = 7.6f - 0.165f, hm = 7.9f - 0.165f, hf = 7.2f - 0.165f;
	std::vector< int > spots, washes, beams, strobes, pars, bars, blinders;
	for( int i = 0; i < 6 && spot; ++i )
		spots.push_back( AddFixture( spot, 0, 1, next( 1, spot ), V3( -6.25f + i * 2.5f, hb, -3.3f ), V3( 180, 0, 0 ), "Spot " + std::to_string( i + 1 ) )->id );
	for( int i = 0; i < 6 && wash; ++i )
		washes.push_back( AddFixture( wash, 0, 1, next( 1, wash ), V3( -6.25f + i * 2.5f, hf, 3.6f ), V3( 180, 0, 0 ), "Wash " + std::to_string( i + 1 ) )->id );
	const float bx[ 4 ] = { -4.6f, -1.55f, 1.55f, 4.6f };
	for( int i = 0; i < 4 && beam; ++i )
		beams.push_back( AddFixture( beam, 0, 1, next( 1, beam ), V3( bx[ i ], hm, 0.3f ), V3( 180, 0, 0 ), "Beam " + std::to_string( i + 1 ) )->id );
	const float sx[ 4 ] = { -6.1f, -3.05f, 3.05f, 6.1f };
	for( int i = 0; i < 4 && strobe; ++i )
		strobes.push_back( AddFixture( strobe, 0, 1, next( 1, strobe ), V3( sx[ i ], hm - 0.05f, 0.3f ), V3( 160, 0, 0 ), "Strobe " + std::to_string( i + 1 ) )->id );
	for( int i = 0; i < 6 && par; ++i )
		pars.push_back( AddFixture( par, 0, 2, next( 2, par ), V3( -5.5f + i * 2.2f, 1.0f, -3.9f ), V3( -16, 0, 0 ), "PAR " + std::to_string( i + 1 ) )->id );
	for( int i = 0; i < 4 && bar; ++i )
		bars.push_back( AddFixture( bar, 0, 2, next( 2, bar ), V3( bx[ i ] * 1.1f, 1.0f, 4.15f ), V3( -28, 0, 0 ), "LED Bar " + std::to_string( i + 1 ) )->id );
	for( int i = 0; i < 2 && blind; ++i )
		blinders.push_back( AddFixture( blind, 0, 2, next( 2, blind ), V3( i ? 7.0f : -7.0f, hf - 0.15f, 3.6f ), V3( 105, 0, 0 ), "Blinder " + std::to_string( i + 1 ) )->id );
	if( hazer ) AddFixture( hazer, 0, 2, next( 2, hazer ), V3( -6.4f, 1.0f, -3.4f ), V3( 0, 30, 0 ), "Hazer" );
	//two side lights on tripod stands in front of the stage, narrow lens
	std::vector< int > sides;
	for( int i = 0; i < 2 && par; ++i )
	{
		Stand sd;
		sd.pos    = V3( i ? 8.6f : -8.6f, 0, 5.4f );
		sd.height = 2.6f;
		sd.yaw    = i ? 35.0f : -35.0f;
		show.stands.push_back( sd );
		FixtureInst* f = AddFixture( par, 0, 2, next( 2, par ), V3( 0, 0, 0 ), V3( 0, 0, 0 ), L( "Seite " ) + std::to_string( i + 1 ) );
		show.PlaceOnStand( { f }, (int)show.stands.size() - 1 );
		f->rot  = V3( -100, i ? 62.0f : -62.0f, 0 );//leicht nach unten, zur Buehnenmitte
		f->beam = 18;
		sides.push_back( f->id );
	}
	show.AutoMount();
	show.UpdateMounts();

	auto F = [ & ]( int id ) { return show.FindFixture( id ); };
	auto look1 = [ & ]() {
		for( size_t i = 0; i < spots.size(); ++i )
		{
			FixtureInst& f = *F( spots[ i ] );
			show.Home( f );
			show.SetAttr( f, Attr::Dimmer, 1 );
			SetNamed( show, f, "Color Wheel", ( i % 2 ? 5 : 8 ) * 8 + 4 );//cyan / CTO
			SetNamed( show, f, "Gobo Wheel", 3 * 8 + 4 );                //breakup
			SetNamed( show, f, "Gobo Rotation", 150 );
			SetNamed( show, f, "Zoom", 70 );
			SetNamed( show, f, "Focus", 120 );
			AimAt( show, f, V3( -4.5f + i * 1.8f, 1.0f, 0.5f ) );
		}
		for( size_t i = 0; i < washes.size(); ++i )
		{
			FixtureInst& f = *F( washes[ i ] );
			show.Home( f );
			show.SetAttr( f, Attr::Dimmer, 0.85f );
			show.SetAttr( f, Attr::Red, i % 2 ? 0.55f : 0.0f );
			show.SetAttr( f, Attr::Green, 0.05f );
			show.SetAttr( f, Attr::Blue, 1.0f );
			show.SetAttr( f, Attr::White, 0 );
			show.SetAttr( f, Attr::Zoom, 0.55f );
			AimAt( show, f, V3( -5.0f + i * 2.0f, 1.0f, 2.2f ) );
		}
		for( size_t i = 0; i < beams.size(); ++i )
		{
			FixtureInst& f = *F( beams[ i ] );
			show.Home( f );
			show.SetAttr( f, Attr::Dimmer, 1 );
			SetNamed( show, f, "Color Wheel", 0 );
			if( i == 1 || i == 2 ) SetNamed( show, f, "Prism", 100 ), SetNamed( show, f, "Prism Rotation", 150 );
			float side = i < 2 ? 1.0f : -1.0f;
			AimAt( show, f, V3( bx[ i ] + side * 9.0f, 0.0f, 9.0f ) );
		}
		for( int id : strobes ) { show.Home( *F( id ) ); show.SetAttr( *F( id ), Attr::Dimmer, 0 ); }
		for( size_t i = 0; i < pars.size(); ++i )
		{
			FixtureInst& f = *F( pars[ i ] );
			show.SetAttr( f, Attr::Dimmer, 1 );
			show.SetAttr( f, Attr::Red, 1 );
			show.SetAttr( f, Attr::Green, i % 2 ? 0.1f : 0.25f );
			show.SetAttr( f, Attr::Blue, 0 );
			show.SetAttr( f, Attr::Amber, 1 );
			show.SetAttr( f, Attr::White, 0 );
			show.SetAttr( f, Attr::UV, i % 2 ? 1.0f : 0.0f );
			show.SetAttr( f, Attr::Shutter, 0 );
		}
		for( size_t i = 0; i < bars.size(); ++i )
		{
			FixtureInst& f = *F( bars[ i ] );
			show.SetAttr( f, Attr::Dimmer, 1 );
			show.SetAttr( f, Attr::Shutter, 0 );
			for( int h = 0; h < 8; ++h )
			{
				V3 c = Hsv( 0.55f + ( i * 8 + h ) / 32.0f * 0.35f, 1, 1 );
				show.SetAttr( f, Attr::Red, c.x, h );
				show.SetAttr( f, Attr::Green, c.y, h );
				show.SetAttr( f, Attr::Blue, c.z, h );
			}
		}
		for( int id : blinders ) show.SetAttr( *F( id ), Attr::Dimmer, 0 );
		for( int id : sides )
		{
			FixtureInst& f = *F( id );
			for( Attr a : { Attr::Red, Attr::Green, Attr::Blue, Attr::Amber, Attr::UV } ) show.SetAttr( f, a, 0 );
			show.SetAttr( f, Attr::White, 1 );
			show.SetAttr( f, Attr::Amber, 0.5f );
			show.SetAttr( f, Attr::Dimmer, 0.7f );
			show.SetAttr( f, Attr::Shutter, 0 );
		}
		if( FixtureInst* hz = show.fixtures.empty() ? nullptr : &show.fixtures.back() ) show.SetAttr( *hz, Attr::Fog, 0.5f );
	};
	look1();
	show.TakeSnapshot( L( "Look 1 - Blau/Amber" ) );
	//look 2: magenta washes, cyan dots, crossing beams, warm blinders
	for( size_t i = 0; i < washes.size(); ++i )
	{
		FixtureInst& f = *F( washes[ i ] );
		show.SetAttr( f, Attr::Red, 1 );
		show.SetAttr( f, Attr::Green, 0 );
		show.SetAttr( f, Attr::Blue, 0.6f );
		AimAt( show, f, V3( -3.0f + i * 1.2f, 1.0f, 0.0f ) );
	}
	for( size_t i = 0; i < spots.size(); ++i )
	{
		FixtureInst& f = *F( spots[ i ] );
		SetNamed( show, f, "Color Wheel", 5 * 8 + 4 );
		SetNamed( show, f, "Gobo Wheel", 1 * 8 + 4 );
		SetNamed( show, f, "Prism", 200 );
		AimAt( show, f, V3( ( i - 2.5f ) * 3.0f, 0.0f, 7.0f ) );
	}
	for( size_t i = 0; i < beams.size(); ++i )
	{
		FixtureInst& f = *F( beams[ i ] );
		SetNamed( show, f, "Color Wheel", 3 * 8 + 4 );
		SetNamed( show, f, "Prism", 0 );
		AimAt( show, f, V3( ( i < 2 ? 6.0f : -6.0f ), 0.0f, 3.0f ) );
	}
	for( int id : blinders ) show.SetAttr( *F( id ), Attr::Dimmer, 0.25f );
	show.TakeSnapshot( "Look 2 - Magenta/Cyan" );
	//blackout
	for( auto& f : show.fixtures )
		if( f.def && f.def->kind != Kind::Hazer )
		{
			show.SetAttr( f, Attr::Dimmer, 0 );
			for( Attr a : { Attr::Red, Attr::Green, Attr::Blue, Attr::White, Attr::Amber, Attr::UV } ) show.SetAttr( f, a, 0 );
		}
	show.TakeSnapshot( "Blackout" );
	show.LoadSnapshotToBase( 0 );//programmer = look 1, no snapshot running
	cam        = Camera();
	cam.target = V3( 0, 3.4f, 0 );
	cam.dist   = 19;
	cam.pitch  = 9;
	sel.clear();
	show.path.clear();
	show.dirty = false;
	UndoReset();//the demo is the starting point
	sim        = SimGlobals();
	sim.haze   = st.haze;
	for( auto& f : show.fixtures ) f.st = FixtureState();
}

void App::DemoFx( float dt )
{
	demoT_ += dt;
	float t = (float)demoT_;
	int si = 0, bi = 0, wi = 0;
	for( auto& f : show.fixtures )
	{
		if( !f.def ) continue;
		const std::string& id = f.def->id;
		if( id.find( "moving-head-spot" ) != std::string::npos )
		{
			AimAt( show, f, V3( sinf( t * 0.7f + si * 0.9f ) * 4.5f, 1.0f, cosf( t * 0.55f + si * 0.7f ) * 2.5f + 0.5f ) );
			++si;
		}
		else if( id.find( "moving-head-beam" ) != std::string::npos )
		{
			float side = bi < 2 ? 1.0f : -1.0f;
			AimAt( show, f, V3( f.pos.x + side * ( 6 + 5 * sinf( t * 0.8f ) ), 0.0f, 6 + 4 * cosf( t * 0.6f + bi ) ) );
			++bi;
		}
		else if( id.find( "moving-head-wash" ) != std::string::npos )
		{
			V3 c = Hsv( t * 0.04f + wi * 0.08f, 1, 1 );
			show.SetAttr( f, Attr::Red, c.x );
			show.SetAttr( f, Attr::Green, c.y * 0.6f );
			show.SetAttr( f, Attr::Blue, c.z );
			++wi;
		}
		else if( id.find( "led-bar-8px" ) != std::string::npos )
		{
			for( int h = 0; h < 8; ++h )
			{
				float k = 0.5f + 0.5f * sinf( t * 4.0f - h * 0.7f - f.pos.x );
				V3 c    = Hsv( t * 0.1f + h * 0.04f, 1, 1 ) * k;
				show.SetAttr( f, Attr::Red, c.x, h );
				show.SetAttr( f, Attr::Green, c.y, h );
				show.SetAttr( f, Attr::Blue, c.z, h );
			}
		}
	}
}

//================================================================ showcase shows (--showcase <dir>)
//five example shows for the release: decks, the effect builder (step sequencer, LFO, envelope, drawn waves),
//groups, snapshots and cue lists working together
namespace
{
struct ShowKit
{
	App& app;
	Show& s;
	std::map< std::string, const FixtureDef* > defs;
	explicit ShowKit( App& a ) : app( a ), s( a.show )
	{
		for( const char* id : { "moving-head-spot", "moving-head-wash", "moving-head-beam", "led-strobe", "led-par-rgbwauv", "led-par-rgbw", "led-bar-8px", "blinder-4", "blinder-2", "hazer", "pixel-matrix-5x5", "par64-dimmer" } )
			defs[ id ] = a.lib.Find( std::string( "generic/generic/" ) + id );
	}
	int Addr( int uni, const FixtureDef* d )
	{
		while( s.UniIndex( uni ) < 0 ) s.AddUniverse();
		return std::max( 1, s.FreeAddress( uni, d->modes[ 0 ].Footprint() ) );
	}
	//puts a lamp into the first universe with room
	FixtureInst* Add( const char* id, const V3& pos, const V3& rot, const std::string& name )
	{
		const FixtureDef* d = defs[ id ];
		if( !d ) return nullptr;
		for( int u = 1; u <= 16; ++u )
		{
			while( s.UniIndex( u ) < 0 ) s.AddUniverse();
			const int a = s.FreeAddress( u, d->modes[ 0 ].Footprint() );
			if( a > 0 ) return app.AddFixture( d, 0, u, a, pos, rot, name );
		}
		return nullptr;
	}
	void Truss( const V3& a, const V3& b, bool free = false )
	{
		ls::Truss t;
		t.a        = a;
		t.b        = b;
		t.freeEnds = free;
		s.trusses.push_back( t );
	}
	Deck& AddDeck( const char* name, int shape, float x, float z, float w, float d, float h, bool onDeck, int stairs = 0, V3 edge = V3( 0, 0, 0 ), float lvl = 0 )
	{
		Deck k;
		k.name      = name;
		k.shape     = shape;
		k.x         = x;
		k.z         = z;
		k.width     = w;
		k.depth     = d;
		k.height    = h;
		k.onDeck    = onDeck;
		k.stairs    = stairs;
		k.edge      = edge;
		k.edgeLevel = lvl;
		s.stage.decks.push_back( k );
		return s.stage.decks.back();
	}
	int Group( const std::string& name, const std::vector< int >& ids, Attr a = Attr::Dimmer )
	{
		GroupFader g;
		g.name     = name;
		g.fixtures = ids;
		g.attr     = a;
		g.value    = 1;
		s.groups.push_back( g );
		return (int)s.groups.size() - 1;
	}
	Effect& Fx( FxType t, const std::string& name, const std::vector< int >& ids )
	{
		Effect e   = FxMakeType( t );
		e.id       = s.NextEffectId();
		e.name     = name;
		e.fixtures = ids;
		e.on       = false;
		s.effects.push_back( e );
		return s.effects.back();
	}
	//intensity / colour effect from a built-in preset (keeps max 50 % unless told otherwise)
	Effect& Preset( const char* preset, const std::string& name, const std::vector< int >& ids, float hi = 0.5f )
	{
		Effect e;
		for( auto& p : BuiltinFxPresets() )
			if( p.name == preset ) e = p.fx;
		e.type     = FxTypeGuess( e );
		e.id       = s.NextEffectId();
		e.name     = name;
		e.fixtures = ids;
		e.hi       = hi;
		e.on       = false;
		s.effects.push_back( e );
		return s.effects.back();
	}
	void Only( const std::vector< int >& on )
	{
		for( auto& e : s.effects ) e.on = std::find( on.begin(), on.end(), e.id ) != on.end();
	}
	uint32_t Snap( const std::string& name, float fade, const std::vector< int >& fxOn )
	{
		Only( fxOn );
		const int i              = s.TakeSnapshotOf( name, {}, true, false );
		s.snapshots[ i ].fade    = fade;
		return s.snapshots[ i ].uid;
	}
	void CueAdd( const std::string& name, uint32_t snap, int trigger = 0, float wait = 0 )
	{
		Cue c;
		c.name    = name;
		c.snap    = snap;
		c.trigger = trigger;
		c.wait    = wait;
		s.cues.push_back( c );
	}
	void Rgb( FixtureInst* f, V3 c, float dim = 1 )
	{
		if( !f ) return;
		s.SetAttr( *f, Attr::Dimmer, dim );
		s.SetAttr( *f, Attr::Shutter, 0 );
		for( Attr a : { Attr::White, Attr::Amber, Attr::UV } ) s.SetAttr( *f, a, 0 );
		const int heads = f->def ? f->def->Heads() : 1;
		for( int h = heads > 1 ? 0 : -1; h < ( heads > 1 ? heads : 0 ); ++h )
		{
			s.SetAttr( *f, Attr::Red, c.x, h );
			s.SetAttr( *f, Attr::Green, c.y, h );
			s.SetAttr( *f, Attr::Blue, c.z, h );
		}
	}
	void Mover( FixtureInst* f, const V3& aim, float dim, const char* colorWheel = nullptr, int cw = 0 )
	{
		if( !f ) return;
		s.Home( *f );
		s.SetAttr( *f, Attr::Dimmer, dim );
		if( colorWheel ) SetNamed( s, *f, colorWheel, cw );
		app.AimAt( s, *f, aim );
	}
	std::vector< int > Ids( const std::vector< FixtureInst* >& v )
	{
		std::vector< int > r;
		for( auto* f : v )
			if( f ) r.push_back( f->id );
		return r;
	}
	void Finish( float bpm )
	{
		s.tempo.bpm = bpm;
		s.AutoMount();
		s.UpdateMounts();
	}
};
}//namespace

bool App::BuildShowcase( int n, std::string& title )
{
	show.Clear();
	ShowKit k( *this );
	Show& s      = show;
	StageCfg& st = s.stage;
	if( !k.defs[ "moving-head-beam" ] || !k.defs[ "moving-head-wash" ] ) return false;
	switch( n )
	{
	//------------------------------------------------------------ 1: club with risers, step sequencer as the drummer
	case 0:
	{
		title         = "01 Club Risers - Step Sequencer";
		st.width      = 12;
		st.depth      = 7;
		st.deckHeight = 0.6f;
		st.haze       = 0.55f;
		st.floorColor = V3( 0.05f, 0.05f, 0.06f );
		k.AddDeck( "Drum riser", 0, -3.2f, -1.6f, 3.0f, 2.4f, 0.5f, true, 1, V3( 0.15f, 0.4f, 1.0f ), 2.5f );
		k.AddDeck( "DJ booth", 0, 1.6f, -0.6f, 2.6f, 1.4f, 0.9f, true, 2, V3( 1.0f, 0.15f, 0.6f ), 2.5f );
		k.AddDeck( "Keys riser", 0, 4.2f, -1.8f, 2.0f, 1.6f, 0.35f, true, 3 );
		k.Truss( V3( -6.5f, 6.0f, -2.8f ), V3( 6.5f, 6.0f, -2.8f ) );
		k.Truss( V3( -6.5f, 6.4f, 2.2f ), V3( 6.5f, 6.4f, 2.2f ) );
		std::vector< FixtureInst* > beams, washes, bars, strobes;
		for( int i = 0; i < 8; ++i ) beams.push_back( k.Add( "moving-head-beam", V3( -5.6f + i * 1.6f, 6.0f - 0.165f, -2.8f ), V3( 180, 0, 0 ), "Beam " + std::to_string( i + 1 ) ) );
		for( int i = 0; i < 6; ++i ) washes.push_back( k.Add( "moving-head-wash", V3( -5.0f + i * 2.0f, 6.4f - 0.165f, 2.2f ), V3( 180, 0, 0 ), "Wash " + std::to_string( i + 1 ) ) );
		for( int i = 0; i < 6; ++i ) bars.push_back( k.Add( "led-bar-8px", V3( -5.0f + i * 2.0f, 0.6f, 3.35f ), V3( -30, 0, 0 ), "Deck bar " + std::to_string( i + 1 ) ) );
		for( int i = 0; i < 4; ++i ) strobes.push_back( k.Add( "led-strobe", V3( -4.8f + i * 3.2f, 6.4f - 0.2f, 2.2f ), V3( 160, 0, 0 ), "Strobe " + std::to_string( i + 1 ) ) );
		k.Finish( 126 );
		for( size_t i = 0; i < beams.size(); ++i ) k.Mover( beams[ i ], V3( ( i < 4 ? 1.0f : -1.0f ) * ( 3.0f + i % 4 ), 0, 8 ), 1, "Color Wheel", 0 );
		for( size_t i = 0; i < washes.size(); ++i ) { k.Mover( washes[ i ], V3( -4.0f + i * 1.6f, 0.6f, 0.5f ), 0.8f ); k.Rgb( washes[ i ], i % 2 ? V3( 0.1f, 0.2f, 1 ) : V3( 0.6f, 0, 1 ), 0.8f ); s.SetAttr( *washes[ i ], Attr::Zoom, 0.6f ); }
		for( auto* b : bars ) k.Rgb( b, V3( 0.1f, 0.3f, 1 ) );
		for( auto* f : strobes ) s.SetAttr( *f, Attr::Dimmer, 0 );
		const auto B = k.Ids( beams ), W = k.Ids( washes ), P = k.Ids( bars ), S = k.Ids( strobes );
		const int gB = k.Group( "Beams", B ), gW = k.Group( "Washes", W ), gS = k.Group( "Strobes", S );
		k.Group( "Deck bars", P );
		//the drummer: kick on the washes, hats on the beams (each beam one step later = running pattern), snare on the strobes
		Effect& seq = k.Fx( FxType::Sequencer, "Drum pattern", B );
		FxResizeSteps( seq, 16 );
		seq.lanes.clear();
		FxLane kick = FxNewLane( seq, 0 ), hats = FxNewLane( seq, 1 ), snare = FxNewLane( seq, 2 );
		kick.tgt.mode = 1, kick.tgt.group = gW;
		hats.tgt.mode = 1, hats.tgt.group = gB;
		snare.tgt.mode = 1, snare.tgt.group = gS;
		for( int i = 0; i < 16; ++i )
		{
			kick.v[ i ]  = i % 4 == 0 ? 1.0f : ( i % 4 == 1 ? 0.45f : 0.12f );
			hats.v[ i ]  = i % 2 ? 1.0f : 0.25f;
			snare.v[ i ] = ( i == 4 || i == 12 ) ? 1.0f : ( i == 15 ? 0.6f : 0.0f );
		}
		hats.glide = false;
		seq.lanes  = { kick, hats, snare };
		seq.stepOffset = 1;
		const int idSeq = seq.id;
		//beams sweep around their position with a drawn wave (fast up, slow down)
		Effect& sweep  = k.Fx( FxType::Lfo, "Beam sweep (drawn)", B );
		sweep.tgt.attr = Attr::Pan;
		sweep.relative = true;
		sweep.depth    = 0.18f;
		sweep.beats    = 8;
		sweep.spread   = 180;
		sweep.mirror   = true;
		sweep.wave     = FxWave::Custom;
		sweep.pts      = { { 0, 0.5f, 2 }, { 0.12f, 1, 2 }, { 0.5f, 0.2f, 1 }, { 0.8f, 0.6f, 2 } };
		const int idSweep = sweep.id;
		//pixel bars along the deck front: rainbow wave over every pixel, chase on top
		Effect& rb = k.Preset( "Regenbogen-Welle", "Deck rainbow", P );
		rb.pixels  = true;
		rb.beats   = 8;
		const int idRb = rb.id;
		Effect& ch = k.Preset( "Lauflicht", "Deck chase", P, 1.0f );
		ch.pixels  = true;
		ch.beats   = 1;
		ch.scale   = true;
		ch.lo      = 0.15f;
		const int idCh = ch.id;
		const uint32_t a = k.Snap( "Groove", 1.0f, { idSeq, idRb } );
		const uint32_t b = k.Snap( "Drop", 0.2f, { idSeq, idSweep, idRb, idCh } );
		for( auto* f : beams ) s.SetAttr( *f, Attr::Dimmer, 0 );
		const uint32_t c = k.Snap( "Breakdown - washes only", 3.0f, { idRb } );
		k.CueAdd( "Groove", a );
		k.CueAdd( "Drop", b );
		k.CueAdd( "Breakdown", c );
		k.CueAdd( "Back to the drop", b, 1, 8 );
		s.LoadSnapshotToBase( 0 );
		k.Only( { idSeq, idRb } );
		break;
	}
	//------------------------------------------------------------ 2: fashion catwalk, envelopes walk down the runway
	case 1:
	{
		title         = "02 Fashion Catwalk - Envelope Ripples";
		st.width      = 10;
		st.depth      = 5;
		st.deckHeight = 0.8f;
		st.haze       = 0.25f;
		st.floorColor = V3( 0.12f, 0.12f, 0.13f );
		st.wallColor  = V3( 0.9f, 0.9f, 0.92f );
		k.AddDeck( "Catwalk", 0, 0, 2.5f + 6.0f, 1.8f, 12.0f, 0.8f, false, 0, V3( 1, 1, 1 ), 1.2f ).ownColor = true;
		s.stage.decks.back().color = V3( 0.75f, 0.75f, 0.78f );
		k.AddDeck( "Runway end", 1, 0, 2.5f + 12.6f, 4.2f, 4.2f, 0.8f, false, 0, V3( 1, 1, 1 ), 1.2f ).ownColor = true;
		s.stage.decks.back().color = V3( 0.75f, 0.75f, 0.78f );
		k.Truss( V3( -3.2f, 6.5f, 2.5f ), V3( -3.2f, 6.5f, 16.5f ) );
		k.Truss( V3( 3.2f, 6.5f, 2.5f ), V3( 3.2f, 6.5f, 16.5f ) );
		k.Truss( V3( -5, 6.5f, -1.5f ), V3( 5, 6.5f, -1.5f ) );
		std::vector< FixtureInst* > spots, washes, back;
		for( int i = 0; i < 7; ++i )
			for( int side = 0; side < 2; ++side )
				spots.push_back( k.Add( "moving-head-spot", V3( side ? 3.2f : -3.2f, 6.5f - 0.165f, 3.5f + i * 2.0f ), V3( 180, 0, 0 ), std::string( "Runway " ) + ( side ? "R" : "L" ) + std::to_string( i + 1 ) ) );
		for( int i = 0; i < 5; ++i ) back.push_back( k.Add( "moving-head-wash", V3( -4 + i * 2.0f, 6.5f - 0.165f, -1.5f ), V3( 180, 0, 0 ), "Back wash " + std::to_string( i + 1 ) ) );
		for( int i = 0; i < 4; ++i ) washes.push_back( k.Add( "led-par-rgbw", V3( -4.4f + i * 2.93f, 0.8f, -2.2f ), V3( -70, 0, 0 ), "Wall uplight " + std::to_string( i + 1 ) ) );
		k.Finish( 100 );
		for( size_t i = 0; i < spots.size(); ++i )
		{
			const int row = (int)i / 2;
			k.Mover( spots[ i ], V3( 0, 0.8f, 3.5f + row * 2.0f ), 1 );
			SetNamed( s, *spots[ i ], "Color Wheel", 0 );
			s.SetAttr( *spots[ i ], Attr::Zoom, 0.35f );
		}
		for( size_t i = 0; i < back.size(); ++i ) { k.Mover( back[ i ], V3( -2 + i, 0.8f, 1.0f ), 0.6f ); k.Rgb( back[ i ], V3( 1, 0.75f, 0.5f ), 0.6f ); }
		for( auto* f : washes ) k.Rgb( f, V3( 0.85f, 0.2f, 1.0f ), 0.9f );
		const auto SP = k.Ids( spots ), BW = k.Ids( back ), UL = k.Ids( washes );
		k.Group( "Runway spots", SP );
		k.Group( "Back washes", BW );
		k.Group( "Wall uplights", UL, Attr::Dimmer );
		//every bar a light pulse walks down the runway: envelope with a delay over the rows (front -> back order)
		Effect& walk  = k.Fx( FxType::Envelope, "Runway walk (every bar)", SP );
		walk.beats    = 4;
		walk.atk      = 0.05f;
		walk.dec      = 0.6f;
		walk.sus      = 0.25f;
		walk.gate     = 0.9f;
		walk.rel      = 0.6f;
		walk.spread   = 300;
		walk.order    = FxOrder::FrontBack;
		walk.reverse  = true;
		walk.lo       = 0.15f;
		walk.hi       = 1.0f;
		const int idWalk = walk.id;
		//a model hits the end: drawn envelope, triggered by hand (button, OSC or MIDI)
		Effect& hit     = k.Fx( FxType::Envelope, "Pose flash (trigger)", SP );
		hit.manualTrig  = true;
		hit.envDrawn    = true;
		hit.gate        = 3;
		hit.pts         = { { 0, 0, 0 }, { 0.04f, 1, 1 }, { 0.15f, 0.35f, 2 }, { 0.3f, 0.8f, 2 }, { 1, 0, 1 } };
		hit.hi          = 1;
		const int idHit = hit.id;
		//uplights fade through fashion colours, back washes breathe
		Effect& wallFx = k.Preset( "Farbverlauf kühl", "Wall colours", UL );
		wallFx.colors  = { V3( 0.85f, 0.2f, 1 ), V3( 1, 0.3f, 0.45f ), V3( 0.25f, 0.4f, 1 ) };
		wallFx.beats   = 32;
		const int idWall = wallFx.id;
		Effect& breathe = k.Preset( "Atmen", "Back breathe", BW );
		const int idBr  = breathe.id;
		const uint32_t a = k.Snap( "Opening", 2.0f, { idWall, idBr } );
		const uint32_t b = k.Snap( "Walk", 1.0f, { idWalk, idWall, idBr, idHit } );
		k.CueAdd( "Opening", a );
		k.CueAdd( "Models walk", b );
		k.CueAdd( "Finale", b, 1, 16 );
		s.LoadSnapshotToBase( 1 );
		k.Only( { idWalk, idWall, idBr, idHit } );
		break;
	}
	//------------------------------------------------------------ 3: open air festival, LFOs and drawn waves at night
	case 2:
	{
		title         = "03 Open Air Festival - LFO & Drawn Waves";
		ApplyMood( st, 1 );//open air night
		st.width      = 18;
		st.depth      = 10;
		st.deckHeight = 1.6f;
		st.haze       = 0.4f;
		st.distanceFog = 0.3f;
		k.AddDeck( "Side wing L", 0, -12, 0, 6, 8, 1.6f, false, 1 );
		k.AddDeck( "Side wing R", 0, 12, 0, 6, 8, 1.6f, false, 1 );
		k.AddDeck( "Drum riser", 0, 0, -2.5f, 4, 3, 0.6f, true, 1, V3( 1.0f, 0.5f, 0.1f ), 2.0f );
		k.Truss( V3( -10, 10, -4 ), V3( 10, 10, -4 ) );
		k.Truss( V3( -10, 10.5f, 0.5f ), V3( 10, 10.5f, 0.5f ) );
		k.Truss( V3( -11, 9.5f, 4.5f ), V3( 11, 9.5f, 4.5f ) );
		k.Truss( V3( -9.5f, 10, -4 ), V3( -9.5f, 1.6f, -4 ), true );//towers
		k.Truss( V3( 9.5f, 10, -4 ), V3( 9.5f, 1.6f, -4 ), true );
		std::vector< FixtureInst* > beams, spots, washes, blinders;
		for( int i = 0; i < 12; ++i ) beams.push_back( k.Add( "moving-head-beam", V3( -8.8f + i * 1.6f, 10 - 0.165f, -4 ), V3( 180, 0, 0 ), "Beam " + std::to_string( i + 1 ) ) );
		for( int i = 0; i < 8; ++i ) spots.push_back( k.Add( "moving-head-spot", V3( -8.4f + i * 2.4f, 10.5f - 0.165f, 0.5f ), V3( 180, 0, 0 ), "Spot " + std::to_string( i + 1 ) ) );
		for( int i = 0; i < 8; ++i ) washes.push_back( k.Add( "moving-head-wash", V3( -9.1f + i * 2.6f, 9.5f - 0.165f, 4.5f ), V3( 180, 0, 0 ), "Wash " + std::to_string( i + 1 ) ) );
		for( int i = 0; i < 4; ++i ) blinders.push_back( k.Add( "blinder-4", V3( -7.5f + i * 5.0f, 9.5f - 0.3f, 4.5f ), V3( 110, 0, 0 ), "Blinder " + std::to_string( i + 1 ) ) );
		k.Finish( 128 );
		for( size_t i = 0; i < beams.size(); ++i ) k.Mover( beams[ i ], V3( ( i - 5.5f ) * 4.0f, 0, 25 ), 1, "Color Wheel", ( i % 3 ) * 8 + 20 );
		for( size_t i = 0; i < spots.size(); ++i ) { k.Mover( spots[ i ], V3( ( i - 3.5f ) * 2.0f, 1.6f, 0 ), 1 ); SetNamed( s, *spots[ i ], "Gobo Wheel", 3 * 8 + 4 ); SetNamed( s, *spots[ i ], "Gobo Rotation", 140 ); }
		for( size_t i = 0; i < washes.size(); ++i ) { k.Mover( washes[ i ], V3( ( i - 3.5f ) * 2.2f, 1.6f, 2.5f ), 0.8f ); k.Rgb( washes[ i ], V3( 1, 0.25f, 0.05f ), 0.8f ); }
		for( auto* f : blinders ) s.SetAttr( *f, Attr::Dimmer, 0 );
		const auto B = k.Ids( beams ), SP = k.Ids( spots ), W = k.Ids( washes ), BL = k.Ids( blinders );
		k.Group( "Beams", B );
		k.Group( "Spots", SP );
		k.Group( "Washes", W );
		k.Group( "Blinders", BL );
		//beams: tilt with a drawn "heartbeat" wave, wave over the truss
		Effect& tilt  = k.Fx( FxType::Lfo, "Beam tilt (drawn)", B );
		tilt.tgt.attr = Attr::Tilt;
		tilt.relative = true;
		tilt.depth    = 0.25f;
		tilt.beats    = 4;
		tilt.spread   = 360;
		tilt.wave     = FxWave::Custom;
		tilt.pts      = { { 0, 0.5f, 1 }, { 0.1f, 1, 0 }, { 0.2f, 0.15f, 2 }, { 0.32f, 0.7f, 2 }, { 0.55f, 0.5f, 1 } };
		const int idTilt = tilt.id;
		//beams: pan fan, mirrored from the middle
		Effect& fan  = k.Fx( FxType::Lfo, "Beam fan", B );
		fan.tgt.attr = Attr::Pan;
		fan.relative = true;
		fan.depth    = 0.2f;
		fan.beats    = 16;
		fan.mirror   = true;
		fan.spread   = 180;
		const int idFan = fan.id;
		//washes zoom breathing with an LFO, colours as a wave
		Effect& zoom  = k.Fx( FxType::Lfo, "Wash zoom", W );
		zoom.tgt.attr = Attr::Zoom;
		zoom.lo       = 0.2f;
		zoom.hi       = 0.9f;
		zoom.beats    = 8;
		zoom.spread   = 360;
		const int idZoom = zoom.id;
		Effect& col = k.Preset( "Feuer", "Wash fire", W );
		const int idCol = col.id;
		//blinders: drawn double hit on beat 1 of every second bar
		Effect& hit = k.Fx( FxType::Envelope, "Blinder double hit", BL );
		hit.beats   = 8;
		hit.envDrawn = true;
		hit.gate    = 1.5f;
		hit.pts     = { { 0, 0, 0 }, { 0.03f, 1, 0 }, { 0.2f, 0.1f, 0 }, { 0.33f, 1, 0 }, { 1, 0, 1 } };
		hit.hi      = 0.8f;
		const int idHit = hit.id;
		const uint32_t a = k.Snap( "Intro sunset", 4.0f, { idCol, idZoom } );
		const uint32_t b = k.Snap( "Main set", 1.0f, { idCol, idZoom, idTilt, idFan } );
		const uint32_t c = k.Snap( "Peak", 0.1f, { idCol, idZoom, idTilt, idFan, idHit } );
		k.CueAdd( "Intro", a );
		k.CueAdd( "Main set", b );
		k.CueAdd( "Peak", c );
		s.LoadSnapshotToBase( 1 );
		k.Only( { idCol, idZoom, idTilt, idFan, idHit } );
		break;
	}
	//------------------------------------------------------------ 4: theatre in the round, tiers and slow movement
	case 3:
	{
		title         = "04 Theatre in the Round - Tiers & Movement";
		st.deck       = false;
		st.backWall   = false;
		st.haze       = 0.3f;
		st.hazeNoise  = 0.7f;
		st.floorColor = V3( 0.06f, 0.05f, 0.05f );
		k.AddDeck( "Tier 1", 1, 0, 0, 9, 9, 0.3f, false, 0, V3( 1.0f, 0.55f, 0.2f ), 0.8f );
		k.AddDeck( "Tier 2", 1, 0, 0, 6.5f, 6.5f, 0.6f, false, 0, V3( 1.0f, 0.55f, 0.2f ), 0.8f );
		k.AddDeck( "Centre", 1, 0, 0, 4, 4, 0.9f, false, 0, V3( 1.0f, 0.45f, 0.1f ), 1.5f ).ownColor = true;
		s.stage.decks.back().color = V3( 0.22f, 0.12f, 0.08f );
		//octagon truss over the ring
		const float R = 6.5f, H = 7.5f;
		for( int i = 0; i < 8; ++i )
		{
			const float a0 = i * kPi / 4, a1 = ( i + 1 ) * kPi / 4;
			k.Truss( V3( cosf( a0 ) * R, H, sinf( a0 ) * R ), V3( cosf( a1 ) * R, H, sinf( a1 ) * R ) );
		}
		std::vector< FixtureInst* > spots, washes;
		std::vector< int > quad[ 4 ];
		for( int i = 0; i < 8; ++i )
		{
			const float a = ( i + 0.5f ) * kPi / 4, r = R * cosf( kPi / 8 );
			FixtureInst* sp = k.Add( "moving-head-spot", V3( cosf( a ) * r, H - 0.165f, sinf( a ) * r ), V3( 180, 0, 0 ), "Ring spot " + std::to_string( i + 1 ) );
			FixtureInst* wa = k.Add( "moving-head-wash", V3( cosf( a + 0.2f ) * r, H - 0.165f, sinf( a + 0.2f ) * r ), V3( 180, 0, 0 ), "Ring wash " + std::to_string( i + 1 ) );
			spots.push_back( sp );
			washes.push_back( wa );
			if( sp ) quad[ i / 2 ].push_back( sp->id );
			if( wa ) quad[ i / 2 ].push_back( wa->id );
		}
		k.Finish( 70 );
		for( size_t i = 0; i < spots.size(); ++i ) { k.Mover( spots[ i ], V3( 0, 0.9f, 0 ), 0.9f ); s.SetAttr( *spots[ i ], Attr::Zoom, 0.25f ); SetNamed( s, *spots[ i ], "Color Wheel", 8 * 8 + 4 ); }
		for( size_t i = 0; i < washes.size(); ++i ) { const float a = ( i + 0.5f ) * kPi / 4; k.Mover( washes[ i ], V3( cosf( a ) * 3.2f, 0.6f, sinf( a ) * 3.2f ), 0.7f ); k.Rgb( washes[ i ], V3( 1, 0.45f, 0.15f ), 0.7f ); }
		const auto SP = k.Ids( spots ), W = k.Ids( washes );
		int gq[ 4 ];
		static const char* kQ[ 4 ] = { "Quarter North", "Quarter West", "Quarter South", "Quarter East" };
		for( int q = 0; q < 4; ++q ) gq[ q ] = k.Group( kQ[ q ], quad[ q ] );
		k.Group( "Ring spots", SP );
		k.Group( "Ring washes", W );
		//spots draw slow circles on the centre stage, all offset: a turning flower
		Effect& circ = k.Preset( "Kreis versetzt", "Centre flower", SP );
		circ.sizePan  = 18;
		circ.sizeTilt = 12;
		circ.beats    = 16;
		const int idCirc = circ.id;
		//the four quarters take turns: sequencer with one lane per quarter group, gliding
		Effect& seq = k.Fx( FxType::Sequencer, "Quarters take turns", W );
		FxResizeSteps( seq, 8 );
		seq.stepBeats = 1;
		seq.lanes.clear();
		for( int q = 0; q < 4; ++q )
		{
			FxLane l     = FxNewLane( seq, q );
			l.tgt.mode   = 1;
			l.tgt.group  = gq[ q ];
			l.glide      = true;
			for( int i = 0; i < 8; ++i ) l.v[ i ] = ( i / 2 == q ) ? 1.0f : 0.2f;
			seq.lanes.push_back( l );
		}
		FxResizeSteps( seq, 8 );
		const int idSeq = seq.id;
		//washes breathe slowly in amber
		Effect& br = k.Fx( FxType::Envelope, "Slow breath", W );
		br.beats   = 8;
		br.atk     = 3;
		br.dec     = 2;
		br.sus     = 0.6f;
		br.gate    = 6;
		br.rel     = 2;
		br.lo      = 0.25f;
		br.hi      = 0.9f;
		br.spread  = 360;
		const int idBr = br.id;
		const uint32_t a = k.Snap( "Audience in", 5.0f, { idBr } );
		const uint32_t b = k.Snap( "Scene 1", 3.0f, { idCirc, idBr } );
		const uint32_t c = k.Snap( "Scene 2 - quarters", 3.0f, { idCirc, idSeq } );
		k.CueAdd( "Audience in", a );
		k.CueAdd( "Scene 1", b );
		k.CueAdd( "Scene 2", c );
		s.LoadSnapshotToBase( 1 );
		k.Only( { idCirc, idSeq } );
		break;
	}
	//------------------------------------------------------------ 5: techno warehouse, pixel matrices, raw DMX lanes
	case 4:
	{
		title         = "05 Techno Warehouse - Pixels & DMX Lanes";
		st.width      = 10;
		st.depth      = 5;
		st.deckHeight = 0.4f;
		st.haze       = 0.8f;
		st.hazeNoise  = 0.8f;
		st.wallHeight = 7;
		st.floorColor = V3( 0.04f, 0.04f, 0.045f );
		st.wallColor  = V3( 0.05f, 0.05f, 0.055f );
		k.AddDeck( "DJ table", 0, 0, 0.2f, 3.2f, 1.2f, 1.0f, true, 0, V3( 1, 0, 0.1f ), 3.0f );
		k.Truss( V3( -6, 5.5f, -1.5f ), V3( 6, 5.5f, -1.5f ) );
		k.Truss( V3( -6, 5.5f, 2 ), V3( 6, 5.5f, 2 ) );
		std::vector< FixtureInst* > mats, beams, strobes, pars;
		for( int i = 0; i < 6; ++i )
		{
			Stand sd;
			sd.pos    = V3( -4.5f + i * 1.8f, 0.4f, -2.0f );
			sd.height = 1.2f + ( i % 2 ) * 1.4f;
			s.stands.push_back( sd );
			FixtureInst* m = k.Add( "pixel-matrix-5x5", V3( 0, 0, 0 ), V3( 0, 0, 0 ), "Matrix " + std::to_string( i + 1 ) );
			if( m ) { s.PlaceOnStand( { m }, (int)s.stands.size() - 1 ); m->rot = V3( 90, 0, 0 ); }
			mats.push_back( m );
		}
		for( int i = 0; i < 8; ++i ) beams.push_back( k.Add( "moving-head-beam", V3( -5.25f + i * 1.5f, 5.5f - 0.165f, -1.5f ), V3( 180, 0, 0 ), "Beam " + std::to_string( i + 1 ) ) );
		for( int i = 0; i < 6; ++i ) strobes.push_back( k.Add( "led-strobe", V3( -5 + i * 2.0f, 5.5f - 0.2f, 2 ), V3( 150, 0, 0 ), "Strobe " + std::to_string( i + 1 ) ) );
		for( int i = 0; i < 4; ++i ) pars.push_back( k.Add( "par64-dimmer", V3( -4.5f + i * 3.0f, 0, 3.6f ), V3( -45, 0, 0 ), "Floor PAR " + std::to_string( i + 1 ) ) );
		k.Finish( 135 );
		for( size_t i = 0; i < beams.size(); ++i ) k.Mover( beams[ i ], V3( ( i % 2 ? 1.0f : -1.0f ) * 4.0f, 0, 6 ), 1, "Color Wheel", 2 * 8 + 4 );
		for( auto* m : mats ) k.Rgb( m, V3( 1, 0.05f, 0.1f ), 1 );
		for( auto* f : strobes ) s.SetAttr( *f, Attr::Dimmer, 0 );
		for( auto* f : pars ) s.SetAttr( *f, Attr::Dimmer, 0 );
		const auto M = k.Ids( mats ), B = k.Ids( beams ), S = k.Ids( strobes ), P = k.Ids( pars );
		const int gM = k.Group( "Matrices", M ), gB = k.Group( "Beams", B ), gS = k.Group( "Strobes", S );
		k.Group( "Floor PARs", P );
		//4-lane sequencer: matrices, beams, strobes and the floor PARs as raw DMX channels
		Effect& seq = k.Fx( FxType::Sequencer, "Warehouse pattern", M );
		FxResizeSteps( seq, 16 );
		seq.lanes.clear();
		FxLane lm = FxNewLane( seq, 0 ), lb = FxNewLane( seq, 1 ), ls_ = FxNewLane( seq, 2 ), lp = FxNewLane( seq, 3 );
		lm.tgt.mode = 1, lm.tgt.group = gM;
		lb.tgt.mode = 1, lb.tgt.group = gB;
		ls_.tgt.mode = 1, ls_.tgt.group = gS;
		if( !pars.empty() && pars[ 0 ] )
		{
			lp.tgt.mode  = 2;
			lp.tgt.uni   = pars[ 0 ]->universe;
			lp.tgt.ch    = pars[ 0 ]->address;
			lp.tgt.count = (int)pars.size();//par64: one channel each, patched one after the other
		}
		for( int i = 0; i < 16; ++i )
		{
			lm.v[ i ]  = i % 4 == 0 ? 1.0f : 0.3f;
			lb.v[ i ]  = ( i % 8 == 2 || i % 8 == 3 || i % 8 == 6 ) ? 1.0f : 0.0f;
			ls_.v[ i ] = i == 14 || i == 15 ? 1.0f : 0.0f;
			lp.v[ i ]  = i % 4 == 2 ? 1.0f : 0.0f;
		}
		seq.lanes      = { lm, lb, ls_, lp };
		seq.stepOffset = 2;
		const int idSeq = seq.id;
		//matrix pixels: drawn saw that runs over every pixel, red/white steps
		Effect& px = k.Fx( FxType::Intensity, "Matrix scanner (drawn)", M );
		px.pixels  = true;
		px.wave    = FxWave::Custom;
		px.pts     = { { 0, 1, 0 }, { 0.25f, 0, 3 }, { 0.6f, 0.4f, 1 } };
		px.beats   = 2;
		px.spread  = 360;
		px.scale   = true;
		px.hi      = 1;
		const int idPx = px.id;
		Effect& col = k.Preset( "Rot / Blau Wechsel", "Matrix red/white", M );
		col.colors  = { V3( 1, 0, 0.05f ), V3( 1, 1, 1 ) };
		col.beats   = 8;
		const int idCol = col.id;
		Effect& rnd = k.Preset( "Strobe zufällig", "Strobe random", S );
		const int idRnd = rnd.id;
		Effect& zb  = k.Fx( FxType::Lfo, "Beam zoom pump", B );
		zb.tgt.attr = Attr::Zoom;
		zb.lo       = 0;
		zb.hi       = 0.6f;
		zb.wave     = FxWave::SawDown;
		zb.beats    = 1;
		const int idZb = zb.id;
		const uint32_t a = k.Snap( "Warm up", 1.0f, { idSeq, idCol } );
		const uint32_t b = k.Snap( "Peak time", 0.1f, { idSeq, idPx, idCol, idZb } );
		const uint32_t c = k.Snap( "Strobe break", 0.0f, { idRnd, idPx } );
		k.CueAdd( "Warm up", a );
		k.CueAdd( "Peak time", b );
		k.CueAdd( "Strobe break", c );
		k.CueAdd( "Peak again", b, 1, 4 );
		s.LoadSnapshotToBase( 1 );
		k.Only( { idSeq, idPx, idCol, idZb } );
		break;
	}
	default: return false;
	}
	s.dirty = false;
	cam     = Camera();
	sel.clear();
	sim      = SimGlobals();
	sim.haze = st.haze;
	for( auto& f : s.fixtures ) f.st = FixtureState();
	return true;
}
}//namespace ls
