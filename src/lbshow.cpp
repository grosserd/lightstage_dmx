// Lightshow export (.lightshow) - see lbshow.h and FORMAT.md (E:\Projekte\Lightbrick)
#include "lbshow.h"
#include "effects.h"
#include "json.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>

namespace ls
{
namespace
{
void Put16( std::vector< uint8_t >& o, uint16_t v ) { o.push_back( (uint8_t)v ); o.push_back( (uint8_t)( v >> 8 ) ); }
void Put32( std::vector< uint8_t >& o, uint32_t v ) { for( int i = 0; i < 4; ++i ) o.push_back( (uint8_t)( v >> ( 8 * i ) ) ); }
void Set32( std::vector< uint8_t >& o, size_t at, uint32_t v ) { for( int i = 0; i < 4; ++i ) o[ at + i ] = (uint8_t)( v >> ( 8 * i ) ); }
uint16_t Get16( const uint8_t* p ) { return (uint16_t)( p[ 0 ] | ( p[ 1 ] << 8 ) ); }
uint32_t Get32( const uint8_t* p ) { return (uint32_t)p[ 0 ] | ( (uint32_t)p[ 1 ] << 8 ) | ( (uint32_t)p[ 2 ] << 16 ) | ( (uint32_t)p[ 3 ] << 24 ); }
uint8_t U8( float v ) { return (uint8_t)std::lround( std::min( 1.0f, std::max( 0.0f, v ) ) * 255.0f ); }
void PutI16( std::vector< uint8_t >& o, size_t at, float v )
{
	const long q = std::lround( std::min( 32767.0f, std::max( -32767.0f, v ) ) );
	o[ at ]     = (uint8_t)( q & 0xFF );
	o[ at + 1 ] = (uint8_t)( ( q >> 8 ) & 0xFF );
}

//one frame against the previous one: 0 unchanged, 1 full, 2 changed runs (see FORMAT.md)
void EncodeFrame( const std::vector< uint8_t >& prev, const std::vector< uint8_t >& cur, bool key, std::vector< uint8_t >& out )
{
	const size_t n = cur.size();
	if( !key && prev.size() == n && memcmp( prev.data(), cur.data(), n ) == 0 )
	{
		out.push_back( 0 );
		return;
	}
	if( !key && prev.size() == n )
	{
		//runs of changed bytes; gaps of up to 4 equal bytes are bridged (cheaper than a new run header)
		std::vector< std::pair< uint32_t, uint32_t > > runs;
		size_t i = 0;
		while( i < n )
		{
			if( cur[ i ] == prev[ i ] ) { ++i; continue; }
			size_t a = i, last = i;
			while( i < n && i - last <= 4 && i - a < 65535 )
			{
				if( cur[ i ] != prev[ i ] ) last = i;
				++i;
			}
			runs.push_back( { (uint32_t)a, (uint32_t)( last - a + 1 ) } );
			i = last + 1;
		}
		size_t bytes = 3;
		for( auto& r : runs ) bytes += 6 + r.second;
		if( runs.size() < 65535 && bytes < n + 1 )
		{
			out.push_back( 2 );
			Put16( out, (uint16_t)runs.size() );
			for( auto& r : runs )
			{
				Put32( out, r.first );
				Put16( out, (uint16_t)r.second );
				out.insert( out.end(), cur.begin() + r.first, cur.begin() + r.first + r.second );
			}
			return;
		}
	}
	out.push_back( 1 );
	out.insert( out.end(), cur.begin(), cur.end() );
}

double TlEnd( const Show& s, const LbxOptions& o ) { return o.to >= 0 ? o.to : s.tl.length; }

void CaptureDmx( Show& s, std::vector< uint8_t >& buf )
{
	buf.assign( 512 * s.universes.size(), 0 );
	std::lock_guard< std::mutex > l( s.dmxLock );
	for( size_t u = 0; u < s.universes.size() && u < s.dmx.size(); ++u )
	{
		uint8_t* d = buf.data() + 512 * u;
		memcpy( d, s.dmx[ u ].data(), 512 );
		//effect / snapshot / timeline overlay - exactly what the output thread sends
		if( s.fxAny && u < s.fxOut.size() )
		{
			const auto& o = s.fxOut[ u ];
			for( int c = 0; c < 512; ++c )
				if( o[ c ] >= 0 ) d[ c ] = (uint8_t)o[ c ];
		}
	}
}

void CaptureVis( Show& s, const LbxJob& j, std::vector< uint8_t >& buf )
{
	buf.assign( j.visBytes, 0 );
	for( size_t i = 0; i < s.fixtures.size() && i < j.visOff.size(); ++i )
	{
		FixtureInst& f = s.fixtures[ i ];
		const size_t at = (size_t)j.visOff[ i ];
		V3 o, d, up;
		BeamRay( f, 0, o, d, up );
		buf[ at ]     = U8( f.st.intensity );
		buf[ at + 1 ] = U8( f.st.color.x );
		buf[ at + 2 ] = U8( f.st.color.y );
		buf[ at + 3 ] = U8( f.st.color.z );
		PutI16( buf, at + 4, o.x * 100.0f );
		PutI16( buf, at + 6, o.y * 100.0f );
		PutI16( buf, at + 8, o.z * 100.0f );
		PutI16( buf, at + 10, d.x * 32767.0f );
		PutI16( buf, at + 12, d.y * 32767.0f );
		PutI16( buf, at + 14, d.z * 32767.0f );
		buf[ at + 16 ] = (uint8_t)std::lround( std::min( 255.0f, std::max( 0.0f, f.st.zoom ) ) );
		const int heads = j.visHeads[ i ];
		for( int h = 0; h < heads; ++h )
		{
			const V3 c = h < (int)f.st.heads.size() ? f.st.heads[ h ] : V3( 0, 0, 0 );
			buf[ at + 17 + h * 3 ]     = U8( c.x );
			buf[ at + 17 + h * 3 + 1 ] = U8( c.y );
			buf[ at + 17 + h * 3 + 2 ] = U8( c.z );
		}
	}
}

Json V3J( const V3& v ) { Json a = Json::Arr(); a.Push( v.x ); a.Push( v.y ); a.Push( v.z ); return a; }

Json SceneJson( Show& s, const LbxJob& j )
{
	Json root = Json::Obj();
	Json fx   = Json::Arr();
	for( size_t i = 0; i < s.fixtures.size(); ++i )
	{
		const FixtureInst& f = s.fixtures[ i ];
		const Mode* m        = f.M();
		Json o               = Json::Obj();
		o.Set( "id", f.id );
		o.Set( "name", f.name );
		o.Set( "kind", f.def ? KindName( f.def->kind ) : "Generic" );
		o.Set( "uni", f.universe );
		o.Set( "addr", f.address );
		o.Set( "footprint", f.Footprint() );
		Json inten = Json::Arr(), emit = Json::Arr();
		if( m )
			for( int c = 0; c < (int)m->ch.size(); ++c )
			{
				if( m->ch[ c ].attr == Attr::Dimmer ) inten.Push( c );
				else if( IsEmitter( m->ch[ c ].attr ) ) emit.Push( c );
			}
		o.Set( "intensity", inten );
		o.Set( "emitters", inten.Size() ? Json::Arr() : emit );
		//patch overview for players: profile, mode, name + attribute of every DMX slot
		o.Set( "profile", f.def ? ( f.def->manufacturer.empty() ? f.def->name : f.def->manufacturer + " " + f.def->name ) : std::string() );
		o.Set( "mode", m ? m->name : std::string() );
		Json chn = Json::Arr(), cha = Json::Arr();
		if( m )
			for( auto& c : m->ch )
			{
				chn.Push( c.name.empty() ? std::string( AttrName( c.attr ) ) : c.name );
				cha.Push( AttrName( c.attr ) );
			}
		o.Set( "channels", chn );
		o.Set( "attrs", cha );
		o.Set( "pos", V3J( f.pos ) );
		const M4 b = FixtureBase( f );
		Json ax    = Json::Arr();
		for( int c = 0; c < 3; ++c ) ax.Push( V3J( V3( b( 0, c ), b( 1, c ), b( 2, c ) ) ) );
		o.Set( "axes", ax );
		o.Set( "dims", V3J( f.def ? f.def->dims : V3( 0.3f, 0.3f, 0.3f ) ) );
		Json beam = Json::Arr();
		beam.Push( f.beam > 0 ? f.beam : ( f.def ? f.def->beamMin : 20.0f ) );
		beam.Push( f.beam > 0 ? f.beam : ( f.def ? f.def->beamMax : 20.0f ) );
		o.Set( "beam", beam );
		const int heads = f.def ? f.def->Heads() : 1;
		o.Set( "heads", heads );
		Json hp = Json::Arr();
		if( heads > 1 && f.def )
		{
			const V3 ext = BodyExtents( *f.def );
			for( auto& p : f.def->pixels ) hp.Push( V3J( V3( p.pos.x * ext.x, p.pos.y * ext.y, p.pos.z * ext.z ) ) );
		}
		o.Set( "headPos", hp );
		o.Set( "vis", i < j.visOff.size() ? j.visOff[ i ] : -1 );
		fx.Push( o );
	}
	root.Set( "fixtures", fx );
	Json gr = Json::Arr();
	for( auto& g : s.groups )
	{
		Json o = Json::Obj();
		o.Set( "name", g.name );
		Json ids = Json::Arr();
		for( int id : g.fixtures ) ids.Push( id );
		o.Set( "fixtures", ids );
		//function of the fader (players apply it live): kind 0 attribute / 1 colour, mode 1 = dimmer submaster
		o.Set( "kind", g.kind );
		o.Set( "attr", AttrName( g.attr ) );
		o.Set( "mode", g.Submaster() ? 1 : 0 );
		o.Set( "value", g.value );
		o.Set( "hue", g.hue );
		o.Set( "sat", g.sat );
		//DMX slots the fader writes: [universe, coarse, fine (-1), attribute] (address 0-based in the universe)
		static const Attr colorAttrs[] = { Attr::Red, Attr::Green, Attr::Blue, Attr::Cyan, Attr::Magenta, Attr::Yellow, Attr::White, Attr::WarmWhite, Attr::ColdWhite, Attr::Amber, Attr::UV, Attr::Lime, Attr::Indigo };
		Json tg = Json::Arr();
		for( int id : g.fixtures )
		{
			const FixtureInst* f = s.FindFixture( id );
			const Mode* m        = f ? f->M() : nullptr;
			if( !m ) continue;
			for( int c = 0; c < (int)m->ch.size(); ++c )
			{
				const Channel& ch = m->ch[ c ];
				if( ch.isFine ) continue;
				bool want = false;
				if( g.kind == 1 )
					for( Attr a : colorAttrs ) want = want || ch.attr == a;
				else
					want = ch.attr == g.attr;
				if( !want ) continue;
				Json t = Json::Arr();
				t.Push( f->universe );
				t.Push( f->address - 1 + c );
				t.Push( ch.fine >= 0 ? f->address - 1 + ch.fine : -1 );
				t.Push( AttrName( ch.attr ) );
				tg.Push( t );
			}
		}
		o.Set( "targets", tg );
		gr.Push( o );
	}
	root.Set( "groups", gr );
	Json st = Json::Obj();
	st.Set( "width", s.stage.width );
	st.Set( "depth", s.stage.depth );
	st.Set( "deck", s.stage.deck );
	st.Set( "deckHeight", s.stage.deckHeight );
	root.Set( "stage", st );
	Json tr = Json::Arr();
	for( auto& t : s.trusses )
	{
		Json o = Json::Obj();
		o.Set( "a", V3J( t.a ) );
		o.Set( "b", V3J( t.b ) );
		o.Set( "size", t.size );
		tr.Push( o );
	}
	root.Set( "trusses", tr );
	Json sd = Json::Arr();
	for( auto& t : s.stands )
	{
		Json o = Json::Obj();
		o.Set( "pos", V3J( t.pos ) );
		o.Set( "height", t.height );
		o.Set( "yaw", t.yaw );
		o.Set( "bar", t.bar );
		sd.Push( o );
	}
	root.Set( "stands", sd );
	return root;
}

Json CuesJson( Show& s, const LbxJob& j )
{
	struct C { double t; std::string name, kind; };
	std::vector< C > v;
	const double a = j.o.from, b = a + (double)j.frames / j.o.fps;
	for( auto& m : s.tl.markers )
		if( m.t >= a - 1e-9 && m.t < b ) v.push_back( { m.t - a, m.name, "marker" } );
	if( s.tl.CueTrack() )
		for( auto& c : s.cues )
			if( c.trigger == 2 && c.tc >= a - 1e-9 && c.tc < b ) v.push_back( { c.tc - a, c.name, "cue" } );
	std::stable_sort( v.begin(), v.end(), []( const C& x, const C& y ) { return x.t < y.t; } );
	Json arr = Json::Arr();
	for( auto& c : v )
	{
		Json o = Json::Obj();
		o.Set( "t", c.t );
		o.Set( "name", c.name );
		o.Set( "kind", c.kind );
		o.Set( "action", "continue" );
		arr.Push( o );
	}
	Json root = Json::Obj();
	root.Set( "cues", arr );
	return root;
}

//the show's setup (what a new show needs to look the same): universes, lamps (profile + mode), own lamp profiles,
//trusses, stands, models, projectors, screens, stage, output. Without programmer values, snapshots, effects, cues,
//timeline, MIDI (the frames already contain their result).
Json PatchJson( Show& s )
{
	Json full = s.ToJson( false );
	Json p    = Json::Obj();
	for( auto& kv : full.o )
	{
		static const char* keep[] = { "format", "version", "universes", "fixtures", "userFixtures", "trusses", "stands", "models",
		                              "projectors", "screens", "stage", "output", "master" };
		bool k = false;
		for( const char* n : keep ) k = k || kv.first == n;
		if( !k ) continue;
		if( kv.first == "universes" )
		{
			Json us = Json::Arr();
			for( auto u : kv.second.a )
			{
				Json o = Json::Obj();
				for( auto& x : u.o )
					if( x.first != "dmx" ) o.Set( x.first, x.second );
				us.Push( o );
			}
			p.Set( kv.first, us );
		}
		else
			p.Set( kv.first, kv.second );
	}
	return p;
}

void Chunk( std::vector< uint8_t >& f, const char id[ 4 ], const std::vector< uint8_t >& data )
{
	f.insert( f.end(), id, id + 4 );
	Put32( f, (uint32_t)data.size() );
	f.insert( f.end(), data.begin(), data.end() );
	while( f.size() % 4 ) f.push_back( 0 );
}
std::vector< uint8_t > Bytes( const std::string& s ) { return std::vector< uint8_t >( s.begin(), s.end() ); }
std::vector< uint8_t > Stream( int frames, int frameBytes, int keyEvery, const std::vector< uint32_t >& keys, const std::vector< uint8_t >& data )
{
	std::vector< uint8_t > o;
	Put32( o, (uint32_t)frames );
	Put32( o, (uint32_t)frameBytes );
	Put16( o, (uint16_t)keyEvery );
	Put16( o, 0 );
	Put32( o, (uint32_t)keys.size() );
	for( uint32_t k : keys ) Put32( o, k );
	o.insert( o.end(), data.begin(), data.end() );
	return o;
}

//---------------------------------------------------------------- effect layers
void SaveState( Show& s, LbxState& t )
{
	{
		std::lock_guard< std::mutex > l( s.dmxLock );
		t.dmx     = s.dmx;
		t.fxOut   = s.fxOut;
		t.snapOut = s.snapOut;
	}
	t.fxAny     = s.fxAny;
	t.snapAny   = s.snapAny;
	t.snapSeq   = s.snapSeq;
	t.grpSeq    = s.grpSeq;
	t.progSeq   = s.progSeq;
	t.effects   = s.effects;
	t.snapshots = s.snapshots;
	t.groups    = s.groups;
	t.tempo     = s.tempo;
	t.cueCur    = s.cueCur;
	t.cueT      = s.cueT;
	t.tlrt      = s.tl.rt;
	t.st.clear();
	for( auto& f : s.fixtures ) t.st.push_back( f.st );
}
void RestoreState( Show& s, const LbxState& t )
{
	{
		std::lock_guard< std::mutex > l( s.dmxLock );
		s.dmx     = t.dmx;
		s.fxOut   = t.fxOut;
		s.snapOut = t.snapOut;
	}
	s.fxAny     = t.fxAny;
	s.snapAny   = t.snapAny;
	s.snapSeq   = t.snapSeq;
	s.grpSeq    = t.grpSeq;
	s.progSeq   = t.progSeq;
	s.effects   = t.effects;
	s.snapshots = t.snapshots;
	s.groups    = t.groups;
	s.tempo     = t.tempo;
	s.cueCur    = t.cueCur;
	s.cueT      = t.cueT;
	s.tl.rt     = t.tlrt;
	for( size_t i = 0; i < s.fixtures.size() && i < t.st.size(); ++i ) s.fixtures[ i ].st = t.st[ i ];
}
//decodes the next frame of an encoded stream body (EncodeFrame format) into cur
bool DecodeNext( const std::vector< uint8_t >& d, size_t& at, std::vector< uint8_t >& cur )
{
	if( at >= d.size() ) return false;
	const uint8_t t = d[ at++ ];
	if( t == 0 ) return true;
	if( t == 1 )
	{
		if( at + cur.size() > d.size() ) return false;
		memcpy( cur.data(), d.data() + at, cur.size() );
		at += cur.size();
		return true;
	}
	if( t != 2 || at + 2 > d.size() ) return false;
	const int runs = Get16( d.data() + at );
	at += 2;
	for( int r = 0; r < runs; ++r )
	{
		if( at + 6 > d.size() ) return false;
		const uint32_t a = Get32( d.data() + at );
		const uint16_t l = Get16( d.data() + at + 4 );
		at += 6;
		if( (size_t)a + l > cur.size() || at + l > d.size() ) return false;
		memcpy( cur.data() + a, d.data() + at, l );
		at += l;
	}
	return true;
}
void PutDelta( std::vector< uint8_t >& o, size_t i, int v )
{
	v        = std::min( 32767, std::max( -32767, v ) );
	o[ i ]     = (uint8_t)( v & 0xFF );
	o[ i + 1 ] = (uint8_t)( ( v >> 8 ) & 0xFF );
}
int16_t I16( const uint8_t* p ) { return (int16_t)( p[ 0 ] | ( p[ 1 ] << 8 ) ); }

//starts the pass of layer j.layer: same start state, same locate as the main pass, effect muted / forced
void StartLayer( Show& s, LbxJob& j )
{
	LbxLayer& L = j.layers[ j.layer ];
	RestoreState( s, j.start );
	s.fxMuteId  = L.on ? 0 : L.fx;
	s.fxForceId = L.on ? L.fx : 0;
	s.tl.Locate( s, j.o.from );
	s.tl.Play( s, true );
	RunEffects( s, 1e-6 );
	j.layerFrame = 0;
	j.mainAt     = 0;
	j.mainVisAt  = 0;
	j.mainCur.assign( j.dmxBytes, 0 );
	j.mainVisCur.assign( j.visBytes, 0 );
}
//after the main pass: which layers to compute (false = none)
bool PlanLayers( Show& s, LbxJob& j )
{
	j.layers.clear();
	if( !j.o.fxLayers ) return false;
	for( size_t i = 0; i < s.effects.size(); ++i )
	{
		const Effect& e = s.effects[ i ];
		if( e.level <= 0 ) continue;
		if( i < j.fxActive.size() && !j.fxActive[ i ].empty() )
		{
			LbxLayer off;
			off.fx = e.id;
			j.layers.push_back( off );
		}
		LbxLayer on;
		on.fx = e.id;
		on.on = true;
		j.layers.push_back( on );
	}
	if( j.layers.empty() ) return false;
	//VIS field layout: u8 intensity, r, g, b · i16 origin x y z · i16 dir x y z · u8 zoom · heads × u8 r g b
	j.visKind.assign( j.visBytes, 0 );
	for( size_t i = 0; i < j.visOff.size(); ++i )
		for( int k = 0; k < 6; ++k )
		{
			const size_t at = (size_t)j.visOff[ i ] + 4 + 2 * k;
			if( at + 1 < j.visKind.size() ) { j.visKind[ at ] = 1; j.visKind[ at + 1 ] = 2; }
		}
	j.layer = 0;
	StartLayer( s, j );
	return true;
}
//one frame of the running layer pass
void LayerFrame( Show& s, SimGlobals& sim, LbxJob& j )
{
	LbxLayer& L      = j.layers[ j.layer ];
	const float frame = 1.0f / j.o.fps;
	const int sub     = std::max( 1, j.o.substeps );
	const float dt    = frame / sub;
	if( j.layerFrame > 0 )
		for( int k = 0; k < sub; ++k )
		{
			s.tempo.beat += dt * std::max( 1.0f, s.tempo.bpm ) / 60.0;
			RunEffects( s, dt );
		}
	const bool key = j.layerFrame % j.o.fps == 0;
	std::vector< uint8_t > cur, d;
	CaptureDmx( s, cur );
	DecodeNext( j.dmxData, j.mainAt, j.mainCur );
	d.assign( 2 * (size_t)j.dmxBytes, 0 );
	for( int c = 0; c < j.dmxBytes; ++c )
	{
		const int v = (int)cur[ c ] - (int)j.mainCur[ c ];
		if( v ) { PutDelta( d, 2 * (size_t)c, v ); L.any = true; }
	}
	if( key ) L.dmxKeys.push_back( (uint32_t)L.dmxData.size() );
	EncodeFrame( L.prevDmx, d, key, L.dmxData );
	L.prevDmx.swap( d );
	if( j.visBytes > 0 )
	{
		if( j.layerFrame > 0 ) Simulate( s, sim, frame );
		else
			for( int k = 0; k < 120; ++k ) Simulate( s, sim, 1.0f / 60.0f );
		CaptureVis( s, j, cur );
		DecodeNext( j.visData, j.mainVisAt, j.mainVisCur );
		d.assign( 2 * (size_t)j.visBytes, 0 );
		for( int b = 0; b < j.visBytes; ++b )
		{
			int v = 0;
			if( j.visKind[ b ] == 0 ) v = (int)cur[ b ] - (int)j.mainVisCur[ b ];
			else if( j.visKind[ b ] == 1 ) v = (int)I16( cur.data() + b ) - (int)I16( j.mainVisCur.data() + b );
			if( v ) { PutDelta( d, 2 * (size_t)b, v ); L.any = true; }
		}
		if( key ) L.visKeys.push_back( (uint32_t)L.visData.size() );
		EncodeFrame( L.prevVis, d, key, L.visData );
		L.prevVis.swap( d );
	}
	++j.layerFrame;
}
Json EffectsJson( Show& s, const LbxJob& j )
{
	static const char* types[] = { "chase", "wave", "strobe", "sparkle", "colorchange", "colorfade", "rainbow", "movement", "beam", "lfo", "sequencer", "envelope", "intensity", "colour" };
	Json root = Json::Obj();
	Json arr  = Json::Arr();
	for( size_t i = 0; i < s.effects.size(); ++i )
	{
		const Effect& e = s.effects[ i ];
		Json o          = Json::Obj();
		o.Set( "id", e.id );
		o.Set( "name", e.name );
		o.Set( "type", types[ std::min( (int)e.type, (int)FxType::Count - 1 ) ] );
		o.Set( "label", FxTypeName( e.type ) );
		o.Set( "color", V3J( FxTypeColor( e.type ) ) );
		Json ids = Json::Arr();
		for( int id : FxFixtureIds( s, e ) ) ids.Push( id );
		o.Set( "fixtures", ids );
		Json act = Json::Arr();
		int n    = 0;
		if( i < j.fxActive.size() )
			for( auto& r : j.fxActive[ i ] )
			{
				Json p = Json::Arr();
				p.Push( (double)r.first / j.o.fps );
				p.Push( (double)( r.second + 1 ) / j.o.fps );
				act.Push( p );
				n += r.second - r.first + 1;
			}
		o.Set( "active", act );
		o.Set( "share", j.frames > 0 ? (double)n / j.frames : 0.0 );
		bool off = false, on = false;
		for( auto& L : j.layers )
			if( L.fx == e.id && L.any ) ( L.on ? on : off ) = true;
		o.Set( "off", off );
		o.Set( "on", on );
		arr.Push( o );
	}
	root.Set( "effects", arr );
	return root;
}
std::vector< uint8_t > LayerChunk( const LbxLayer& L, int frames, int frameBytes, int keyEvery, const std::vector< uint32_t >& keys, const std::vector< uint8_t >& data )
{
	std::vector< uint8_t > o;
	Put32( o, (uint32_t)L.fx );
	o.push_back( L.on ? 1 : 0 );
	o.push_back( 0 );
	o.push_back( 0 );
	o.push_back( 0 );
	const std::vector< uint8_t > st = Stream( frames, frameBytes, keyEvery, keys, data );
	o.insert( o.end(), st.begin(), st.end() );
	return o;
}

bool DecodeStream( const uint8_t* p, size_t n, std::vector< std::vector< uint8_t > >& out, int* frameBytes, int* keyEvery, std::string* err )
{
	if( n < 16 ) { if( err ) *err = "stream too short"; return false; }
	const uint32_t frames = Get32( p ), fb = Get32( p + 4 );
	const int ke          = Get16( p + 8 );
	const uint32_t kc     = Get32( p + 12 );
	if( frameBytes ) *frameBytes = (int)fb;
	if( keyEvery ) *keyEvery = ke;
	size_t at = 16 + 4 * (size_t)kc;
	if( at > n ) { if( err ) *err = "key table"; return false; }
	const size_t base = at;
	std::vector< uint8_t > cur( fb, 0 );
	out.clear();
	out.reserve( frames );
	for( uint32_t f = 0; f < frames; ++f )
	{
		if( ke > 0 && f % ke == 0 )
		{
			const uint32_t k = f / ke;
			if( k >= kc || Get32( p + 16 + 4 * k ) != at - base ) { if( err ) *err = "key offset of frame " + std::to_string( f ); return false; }
		}
		if( at >= n ) { if( err ) *err = "truncated at frame " + std::to_string( f ); return false; }
		const uint8_t t = p[ at++ ];
		if( t == 1 )
		{
			if( at + fb > n ) { if( err ) *err = "truncated full frame"; return false; }
			memcpy( cur.data(), p + at, fb );
			at += fb;
		}
		else if( t == 2 )
		{
			if( ke > 0 && f % ke == 0 ) { if( err ) *err = "key frame not full"; return false; }
			if( at + 2 > n ) { if( err ) *err = "truncated runs"; return false; }
			const int runs = Get16( p + at );
			at += 2;
			for( int r = 0; r < runs; ++r )
			{
				if( at + 6 > n ) { if( err ) *err = "truncated run"; return false; }
				const uint32_t st = Get32( p + at );
				const uint16_t ln = Get16( p + at + 4 );
				at += 6;
				if( (size_t)st + ln > fb || at + ln > n ) { if( err ) *err = "run out of range"; return false; }
				memcpy( cur.data() + st, p + at, ln );
				at += ln;
			}
		}
		else if( t != 0 ) { if( err ) *err = "frame type " + std::to_string( t ); return false; }
		out.push_back( cur );
	}
	if( at != n ) { if( err ) *err = "trailing bytes"; return false; }
	return true;
}
}//namespace

bool LbxBegin( Show& s, const LbxOptions& o, const std::string& path, LbxJob& j )
{
	j        = LbxJob();
	j.path   = path;
	j.o      = o;
	j.o.fps  = std::max( 1, std::min( 100, o.fps ) );
	j.o.from = std::max( 0.0, o.from );
	const double end = TlEnd( s, j.o );
	if( s.tl.tracks.empty() ) { j.err = "Time Line ist leer"; return false; }
	LbxEnsurePlayers( s );//Lightshow tracks of the timeline
	if( s.universes.empty() ) { j.err = "keine Universen"; return false; }
	if( end <= j.o.from ) { j.err = "Zeitbereich leer"; return false; }
	j.frames   = std::max( 1, (int)std::ceil( ( end - j.o.from ) * j.o.fps - 1e-6 ) );
	j.dmxBytes = 512 * (int)s.universes.size();
	j.visOff.clear();
	j.visHeads.clear();
	int off = 0;
	for( auto& f : s.fixtures )
	{
		const int heads = f.def ? f.def->Heads() : 1;
		j.visOff.push_back( off );
		j.visHeads.push_back( heads > 1 ? heads : 0 );
		off += 17 + ( heads > 1 ? 3 * heads : 0 );
	}
	j.visBytes = j.o.visual ? off : 0;
	SaveState( s, j.start );
	j.fxActive.assign( s.effects.size(), {} );
	s.fxMuteId = s.fxForceId = 0;
	j.sync     = s.tl.sync;
	j.playing  = s.tl.rt.playing;
	j.pos      = s.tl.rt.pos;
	j.loop     = s.tl.loop;
	s.tl.loop  = false;
	s.tl.sync  = TlSync::Internal;
	s.tl.Locate( s, j.o.from );
	s.tl.Play( s, true );
	RunEffects( s, 1e-6 );//applies the located state (frame 0)
	j.started = (double)clock() / CLOCKS_PER_SEC;
	j.active  = true;
	return true;
}

bool LbxStep( Show& s, SimGlobals& sim, LbxJob& j, double budgetSec )
{
	if( !j.active ) return false;
	const double t0   = (double)clock() / CLOCKS_PER_SEC;
	const float frame = 1.0f / j.o.fps;
	const int sub     = std::max( 1, j.o.substeps );
	const float dt    = frame / sub;
	std::vector< uint8_t > cur;
	do
	{
		if( j.layer >= 0 )
		{
			//effect layer passes
			if( j.layerFrame >= j.frames )
			{
				if( ++j.layer >= (int)j.layers.size() ) return false;
				StartLayer( s, j );
			}
			LayerFrame( s, sim, j );
			continue;
		}
		if( j.frame >= j.frames ) return PlanLayers( s, j );
		if( j.frame == 0 ) j.start.sim = sim;
		if( j.frame > 0 )
			for( int k = 0; k < sub; ++k )
			{
				s.tempo.beat += dt * std::max( 1.0f, s.tempo.bpm ) / 60.0;
				RunEffects( s, dt );
			}
		//which effects drive in this frame (EFFX: "active")
		for( size_t i = 0; i < s.effects.size() && i < j.fxActive.size(); ++i )
		{
			const Effect& e = s.effects[ i ];
			if( !( e.Drive() > 0 && e.level > 0 ) ) continue;
			auto& r = j.fxActive[ i ];
			if( !r.empty() && r.back().second == j.frame - 1 ) r.back().second = j.frame;
			else r.push_back( { j.frame, j.frame } );
		}
		const bool key = j.frame % j.o.fps == 0;
		CaptureDmx( s, cur );
		if( key ) j.dmxKeys.push_back( (uint32_t)j.dmxData.size() );
		EncodeFrame( j.prevDmx, cur, key, j.dmxData );
		if( j.keepFrames ) j.keptDmx.push_back( cur );
		j.prevDmx.swap( cur );
		if( j.visBytes > 0 )
		{
			if( j.frame > 0 ) Simulate( s, sim, frame );
			else
				for( int k = 0; k < 120; ++k ) Simulate( s, sim, 1.0f / 60.0f );//motors arrive before frame 0
			CaptureVis( s, j, cur );
			if( key ) j.visKeys.push_back( (uint32_t)j.visData.size() );
			EncodeFrame( j.prevVis, cur, key, j.visData );
			j.prevVis.swap( cur );
		}
		++j.frame;
	} while( (double)clock() / CLOCKS_PER_SEC - t0 < budgetSec );
	return true;
}

bool LbxFinish( Show& s, LbxJob& j, bool ok )
{
	if( !j.active ) return false;
	j.active = false;
	s.fxMuteId = s.fxForceId = 0;
	RestoreState( s, j.start );//effects, snapshots, tempo ... as before the export (also after the layer passes)
	s.tl.Play( s, false );
	s.tl.sync = j.sync;
	s.tl.loop = j.loop;
	s.tl.Locate( s, j.pos );
	s.tl.rt.pos = j.pos;
	s.tl.Play( s, j.playing );
	if( !ok || j.frame < j.frames || ( j.layer >= 0 && ( j.layer < (int)j.layers.size() - 1 || j.layerFrame < j.frames ) ) )
	{
		if( j.err.empty() ) j.err = "abgebrochen";
		return false;
	}
	Json info = Json::Obj();
	info.Set( "format", "lightstage-lightshow" );
	info.Set( "version", 1 );
	info.Set( "app", "LightStage 2.1" );
	std::string name = s.path.empty() ? j.path : s.path;//unsaved show: name of the export file
	const size_t sl  = name.find_last_of( "\\/" );
	if( sl != std::string::npos ) name = name.substr( sl + 1 );
	const size_t dot = name.rfind( '.' );
	if( dot != std::string::npos ) name = name.substr( 0, dot );
	info.Set( "name", name.empty() ? std::string( "Show" ) : name );
	{
		char b[ 32 ];
		const time_t now = time( nullptr );
		tm lt{};
		localtime_s( &lt, &now );
		strftime( b, sizeof( b ), "%Y-%m-%dT%H:%M:%S", &lt );
		info.Set( "created", b );
	}
	info.Set( "fps", j.o.fps );
	info.Set( "frames", j.frames );
	info.Set( "start", j.o.from );
	info.Set( "duration", (double)j.frames / j.o.fps );
	info.Set( "bpm", s.tl.bpm );
	info.Set( "beatsPerBar", s.tl.beatsPerBar );
	Json loop = Json::Obj();
	loop.Set( "on", s.tl.loop );
	loop.Set( "a", s.tl.loopA );
	loop.Set( "b", s.tl.loopB );
	info.Set( "loop", loop );
	Json unis = Json::Arr();
	for( auto& u : s.universes )
	{
		Json o = Json::Obj();
		o.Set( "num", u.num );
		o.Set( "name", u.name );
		o.Set( "artnet", u.artnetPort );
		o.Set( "out", u.artnetOut );
		unis.Push( o );
	}
	info.Set( "universes", unis );
	Json st = Json::Arr();
	st.Push( "DMX0" );
	if( j.visBytes > 0 ) st.Push( "VIS0" );
	info.Set( "streams", st );

	std::vector< uint8_t > f;
	f.insert( f.end(), { 'L', 'S', 'H', 'W' } );
	Put16( f, 1 );
	Put16( f, 0 );
	const size_t cntAt = f.size();
	Put32( f, 0 );
	Put32( f, 0 );
	uint32_t chunks = 0;
	Chunk( f, "INFO", Bytes( WriteJson( info, true ) ) ), ++chunks;
	Chunk( f, "CUES", Bytes( WriteJson( CuesJson( s, j ), true ) ) ), ++chunks;
	Chunk( f, "SCEN", Bytes( WriteJson( SceneJson( s, j ), false ) ) ), ++chunks;
	Chunk( f, "PTCH", Bytes( WriteJson( PatchJson( s ), false ) ) ), ++chunks;
	Chunk( f, "DMX0", Stream( j.frames, j.dmxBytes, j.o.fps, j.dmxKeys, j.dmxData ) ), ++chunks;
	if( j.visBytes > 0 ) Chunk( f, "VIS0", Stream( j.frames, j.visBytes, j.o.fps, j.visKeys, j.visData ) ), ++chunks;
	if( !s.effects.empty() ) Chunk( f, "EFFX", Bytes( WriteJson( EffectsJson( s, j ), false ) ) ), ++chunks;
	for( auto& L : j.layers )
	{
		if( !L.any ) continue;//this layer changes nothing
		Chunk( f, "FXLD", LayerChunk( L, j.frames, 2 * j.dmxBytes, j.o.fps, L.dmxKeys, L.dmxData ) ), ++chunks;
		if( j.visBytes > 0 ) Chunk( f, "FXLV", LayerChunk( L, j.frames, 2 * j.visBytes, j.o.fps, L.visKeys, L.visData ) ), ++chunks;
	}
	Set32( f, cntAt, chunks );
	FILE* fp = nullptr;
	if( _wfopen_s( &fp, Widen( j.path ).c_str(), L"wb" ) != 0 || !fp )
	{
		j.err = "Datei kann nicht geschrieben werden";
		return false;
	}
	const bool w = fwrite( f.data(), 1, f.size(), fp ) == f.size();
	fclose( fp );
	if( !w ) j.err = "Schreibfehler";
	j.dmxData.clear();
	j.visData.clear();
	return w;
}

bool LbxExport( Show& s, SimGlobals& sim, const LbxOptions& o, const std::string& path, std::string* err )
{
	LbxJob j;
	if( !LbxBegin( s, o, path, j ) )
	{
		if( err ) *err = j.err;
		return false;
	}
	while( LbxStep( s, sim, j, 1.0 ) ) {}
	const bool ok = LbxFinish( s, j, true );
	if( err ) *err = j.err;
	return ok;
}

bool LbxRead( const std::string& path, LbxFile& out, std::string* err )
{
	out = LbxFile();
	FILE* fp = nullptr;
	if( _wfopen_s( &fp, Widen( path ).c_str(), L"rb" ) != 0 || !fp ) { if( err ) *err = "open"; return false; }
	std::vector< uint8_t > d;
	uint8_t buf[ 65536 ];
	size_t n;
	while( ( n = fread( buf, 1, sizeof( buf ), fp ) ) > 0 ) d.insert( d.end(), buf, buf + n );
	fclose( fp );
	out.fileBytes = d.size();
	if( d.size() < 16 || memcmp( d.data(), "LSHW", 4 ) != 0 || Get16( d.data() + 4 ) != 1 ) { if( err ) *err = "header"; return false; }
	const uint32_t chunks = Get32( d.data() + 8 );
	size_t at = 16;
	for( uint32_t c = 0; c < chunks; ++c )
	{
		if( at + 8 > d.size() ) { if( err ) *err = "chunk header"; return false; }
		const std::string id( (const char*)d.data() + at, 4 );
		const uint32_t sz = Get32( d.data() + at + 4 );
		at += 8;
		if( at + sz > d.size() ) { if( err ) *err = "chunk " + id; return false; }
		const uint8_t* p = d.data() + at;
		std::string e;
		if( id == "INFO" || id == "CUES" || id == "SCEN" || id == "PTCH" || id == "EFFX" )
		{
			Json j;
			if( !ParseJson( std::string( (const char*)p, sz ), j, &e ) ) { if( err ) *err = id + ": " + e; return false; }
			( id == "INFO" ? out.info : id == "CUES" ? out.cues : id == "SCEN" ? out.scene : id == "EFFX" ? out.effects : out.patch ) = j;
		}
		else if( ( id == "FXLD" || id == "FXLV" ) && sz >= 8 )
		{
			const int fxId  = (int)Get32( p );
			const bool on   = p[ 4 ] != 0;
			LbxFile::Layer* L = nullptr;
			for( auto& x : out.layers )
				if( x.fx == fxId && x.on == on ) L = &x;
			if( !L )
			{
				out.layers.push_back( LbxFile::Layer() );
				L     = &out.layers.back();
				L->fx = fxId;
				L->on = on;
			}
			if( !DecodeStream( p + 8, sz - 8, id == "FXLD" ? L->dmx : L->vis, nullptr, nullptr, &e ) ) { if( err ) *err = id + ": " + e; return false; }
		}
		else if( id == "DMX0" )
		{
			if( !DecodeStream( p, sz, out.dmx, &out.dmxBytes, &out.keyEvery, &e ) ) { if( err ) *err = "DMX0: " + e; return false; }
		}
		else if( id == "VIS0" )
		{
			if( !DecodeStream( p, sz, out.vis, &out.visBytes, nullptr, &e ) ) { if( err ) *err = "VIS0: " + e; return false; }
		}
		at += sz;
		while( at % 4 ) ++at;
	}
	out.fps    = out.info[ "fps" ].I();
	out.frames = out.info[ "frames" ].I();
	if( (int)out.dmx.size() != out.frames ) { if( err ) *err = "frame count"; return false; }
	return true;
}
//================================================================ Lightshow tracks: frames on demand
namespace
{
std::mutex gPlayersMu;
std::map< std::string, std::shared_ptr< LbxPlayer > > gPlayers;

//applies one encoded frame (at stream offset) on top of cur
bool ApplyEncoded( const std::vector< uint8_t >& st, uint32_t at, std::vector< uint8_t >& cur )
{
	if( at >= st.size() ) return false;
	const uint8_t* p = st.data() + at;
	const size_t n   = st.size() - at;
	const size_t fb  = cur.size();
	if( p[ 0 ] == 0 ) return true;
	if( p[ 0 ] == 1 )
	{
		if( 1 + fb > n ) return false;
		memcpy( cur.data(), p + 1, fb );
		return true;
	}
	if( p[ 0 ] != 2 || n < 3 ) return false;
	const int runs = Get16( p + 1 );
	size_t k       = 3;
	for( int r = 0; r < runs; ++r )
	{
		if( k + 6 > n ) return false;
		const uint32_t a = Get32( p + k );
		const uint16_t l = Get16( p + k + 4 );
		k += 6;
		if( (size_t)a + l > fb || k + l > n ) return false;
		memcpy( cur.data() + a, p + k, l );
		k += l;
	}
	return true;
}
//byte length of the encoded frame at p (0 = broken)
size_t EncodedLen( const uint8_t* p, size_t n, size_t fb )
{
	if( n < 1 ) return 0;
	if( p[ 0 ] == 0 ) return 1;
	if( p[ 0 ] == 1 ) return 1 + fb <= n ? 1 + fb : 0;
	if( p[ 0 ] != 2 || n < 3 ) return 0;
	const int runs = Get16( p + 1 );
	size_t k       = 3;
	for( int r = 0; r < runs; ++r )
	{
		if( k + 6 > n ) return 0;
		k += 6 + Get16( p + k + 4 );
		if( k > n ) return 0;
	}
	return k;
}

bool LoadPlayer( const std::string& path, LbxPlayer& pl )
{
	std::vector< uint8_t > d;
	if( !ReadFile( path, d ) ) { pl.err = "Datei nicht gefunden"; return false; }
	if( d.size() < 16 || memcmp( d.data(), "LSHW", 4 ) != 0 || Get16( d.data() + 4 ) != 1 ) { pl.err = "keine Lightshow-Datei"; return false; }
	const uint32_t chunks = Get32( d.data() + 8 );
	size_t at             = 16;
	Json info;
	const uint8_t* dmx = nullptr;
	size_t dmxN        = 0;
	for( uint32_t c = 0; c < chunks; ++c )
	{
		if( at + 8 > d.size() ) break;
		const std::string id( (const char*)d.data() + at, 4 );
		const uint32_t sz = Get32( d.data() + at + 4 );
		at += 8;
		if( at + sz > d.size() ) { pl.err = "Datei unvollständig"; return false; }
		if( id == "INFO" ) ParseJson( std::string( (const char*)d.data() + at, sz ), info );
		if( id == "DMX0" ) { dmx = d.data() + at; dmxN = sz; }
		at += sz;
		while( at % 4 ) ++at;
	}
	if( !dmx || dmxN < 16 || !info.IsObj() ) { pl.err = "keine DMX-Daten"; return false; }
	pl.fps        = std::max( 1, info[ "fps" ].I( 40 ) );
	pl.name       = info[ "name" ].Str();
	pl.frames     = (int)Get32( dmx );
	pl.frameBytes = (int)Get32( dmx + 4 );
	pl.keyEvery   = std::max( 1, (int)Get16( dmx + 8 ) );
	const uint32_t kc = Get32( dmx + 12 );
	const size_t base = 16 + 4 * (size_t)kc;
	const Json& us    = info[ "universes" ];
	for( size_t u = 0; u < us.Size(); ++u ) pl.uniNums.push_back( us[ u ][ "num" ].I( (int)u + 1 ) );
	if( base > dmxN || pl.frameBytes != 512 * (int)pl.uniNums.size() || pl.frames <= 0 ) { pl.err = "DMX-Daten fehlerhaft"; return false; }
	pl.stream.assign( dmx + base, dmx + dmxN );
	pl.offsets.resize( pl.frames );
	pl.activity.resize( pl.frames );
	std::vector< uint8_t > cur( pl.frameBytes, 0 );
	std::vector< float > mean( pl.frames, 0.0f );
	size_t k = 0;
	for( int f = 0; f < pl.frames; ++f )
	{
		const size_t len = EncodedLen( pl.stream.data() + k, pl.stream.size() - k, cur.size() );
		if( !len || ( f % pl.keyEvery == 0 && pl.stream[ k ] != 1 ) ) { pl.err = "DMX-Bild " + std::to_string( f ) + " fehlerhaft"; return false; }
		pl.offsets[ f ] = (uint32_t)k;
		ApplyEncoded( pl.stream, (uint32_t)k, cur );
		uint64_t sum = 0;
		for( uint8_t b : cur ) sum += b;
		mean[ f ] = (float)sum / std::max< size_t >( 1, cur.size() );
		k += len;
	}
	//overview: mean DMX level, scaled to the brightest frame
	const float mx = std::max( 1e-3f, *std::max_element( mean.begin(), mean.end() ) );
	for( int f = 0; f < pl.frames; ++f ) pl.activity[ f ] = (uint8_t)std::lround( 255.0f * mean[ f ] / mx );
	pl.duration = (double)pl.frames / pl.fps;
	pl.cur.assign( pl.frameBytes, 0 );
	pl.curFrame = -1;
	return true;
}
}//namespace

bool LbxPlayer::Frame( int f, std::vector< uint8_t >& out )
{
	std::lock_guard< std::mutex > l( mu );
	if( f < 0 || f >= frames ) return false;
	if( curFrame < 0 || f < curFrame || f - curFrame > keyEvery )
	{
		const int k = f / keyEvery * keyEvery;
		if( !ApplyEncoded( stream, offsets[ k ], cur ) ) return false;
		curFrame = k;
	}
	while( curFrame < f )
	{
		++curFrame;
		if( !ApplyEncoded( stream, offsets[ curFrame ], cur ) ) { curFrame = -1; return false; }
	}
	out = cur;
	return true;
}

std::shared_ptr< LbxPlayer > LbxPlayerGet( const std::string& path )
{
	std::lock_guard< std::mutex > l( gPlayersMu );
	auto it = gPlayers.find( ToLower( path ) );
	return it != gPlayers.end() && it->second->err.empty() ? it->second : nullptr;
}
std::shared_ptr< LbxPlayer > LbxPlayerLoad( const std::string& path, bool reload )
{
	const std::string key = ToLower( path );
	if( !reload )
	{
		std::lock_guard< std::mutex > l( gPlayersMu );
		auto it = gPlayers.find( key );
		if( it != gPlayers.end() ) return it->second;
	}
	auto pl  = std::make_shared< LbxPlayer >();
	pl->path = path;
	if( path.empty() ) pl->err = "keine Datei";
	else if( !LoadPlayer( path, *pl ) && pl->err.empty() ) pl->err = "Fehler";
	if( !pl->err.empty() ) Log( "lightshow track %s: %s", path.c_str(), pl->err.c_str() );
	std::lock_guard< std::mutex > l( gPlayersMu );
	gPlayers[ key ] = pl;
	return pl;
}
void LbxEnsurePlayers( const Show& s )
{
	for( auto& t : s.tl.tracks )
	{
		if( t.kind != TlTrackKind::Show || t.file.empty() ) continue;
		bool known;
		{
			std::lock_guard< std::mutex > l( gPlayersMu );
			known = gPlayers.count( ToLower( t.file ) ) > 0;
		}
		if( !known ) LbxPlayerLoad( t.file );
	}
}

//================================================================ import
namespace
{
//Euler angles (degrees, RotEuler order Y * X * Z) of a rotation given by its columns
V3 EulerFromAxes( const V3& x, const V3& y, const V3& z )
{
	//R = [x y z] (columns): R(1,2) = z.y = -sin(ax), R(0,2) = z.x, R(2,2) = z.z, R(1,0) = x.y, R(1,1) = y.y
	const float sx = Clamp( -z.y, -1.0f, 1.0f );
	const float ax = asinf( sx );
	float ay, az;
	if( fabsf( sx ) < 0.9999f )
	{
		ay = atan2f( z.x, z.z );
		az = atan2f( x.y, y.y );
	}
	else
	{
		ay = atan2f( -x.z, x.x );
		az = 0;
	}
	return V3( ax, ay, az ) * ( 180.0f / 3.14159265f );
}
V3 JV3( const Json& j, const V3& d ) { return j.Size() == 3 ? V3( j[ 0 ].F(), j[ 1 ].F(), j[ 2 ].F() ) : d; }

//new show from the SCEN chunk (files without PTCH): universes, stage, trusses, stands, lamps with a generic profile of the same kind and footprint
void SceneToShow( Show& s, const Library& lib, const LbxFile& f, LbxImportResult& r )
{
	s.Clear();
	{
		std::lock_guard< std::mutex > l( s.dmxLock );
		const Json& us = f.info[ "universes" ];
		for( size_t u = 0; u < us.Size(); ++u )
		{
			Universe x;
			x.num          = us[ u ][ "num" ].I( (int)u + 1 );
			x.name         = us[ u ][ "name" ].Str();
			if( x.name.empty() ) x.name = "Universe " + std::to_string( x.num );
			x.artnetPort   = us[ u ][ "artnet" ].I( x.num - 1 );
			x.artnetOut    = us[ u ][ "out" ].B( true );
			x.artnetInPort = x.num - 1;
			s.universes.push_back( x );
			s.dmx.push_back( DmxFrame{} );
		}
	}
	const Json& sc = f.scene;
	s.stage.width      = sc[ "stage" ][ "width" ].F( s.stage.width );
	s.stage.depth      = sc[ "stage" ][ "depth" ].F( s.stage.depth );
	s.stage.deck       = sc[ "stage" ][ "deck" ].B( s.stage.deck );
	s.stage.deckHeight = sc[ "stage" ][ "deckHeight" ].F( s.stage.deckHeight );
	for( auto& o : sc[ "trusses" ].a )
	{
		Truss t;
		t.a    = JV3( o[ "a" ], t.a );
		t.b    = JV3( o[ "b" ], t.b );
		t.size = o[ "size" ].F( t.size );
		s.trusses.push_back( t );
	}
	for( auto& o : sc[ "stands" ].a )
	{
		Stand t;
		t.pos    = JV3( o[ "pos" ], t.pos );
		t.height = o[ "height" ].F( t.height );
		t.yaw    = o[ "yaw" ].F( 0 );
		t.bar    = o[ "bar" ].F( 0 );
		s.stands.push_back( t );
	}
	for( auto& o : sc[ "fixtures" ].a )
	{
		const int fp         = o[ "footprint" ].I( 0 );
		const std::string kd = o[ "kind" ].Str();
		const FixtureDef* best = nullptr;
		int bestMode = -1, bestScore = -1;
		for( auto& d : lib.defs )
		{
			if( d->id.rfind( "generic/", 0 ) != 0 ) continue;
			for( int m = 0; m < (int)d->modes.size(); ++m )
				if( d->modes[ m ].Footprint() == fp )
				{
					const int score = KindName( d->kind ) == kd ? 2 : 1;
					if( score > bestScore ) { best = d.get(); bestMode = m; bestScore = score; }
				}
		}
		if( !best ) { ++r.skippedFixtures; continue; }
		FixtureInst x;
		x.id       = o[ "id" ].I( s.NextFixtureId() );
		x.name     = o[ "name" ].Str( best->name );
		x.def      = best;
		x.defId    = best->id;
		x.mode     = bestMode;
		x.universe = o[ "uni" ].I( 1 );
		x.address  = o[ "addr" ].I( 1 );
		x.pos      = JV3( o[ "pos" ], x.pos );
		const Json& ax = o[ "axes" ];
		if( ax.Size() == 3 ) x.rot = EulerFromAxes( JV3( ax[ 0 ], V3( 1, 0, 0 ) ), JV3( ax[ 1 ], V3( 0, 1, 0 ) ), JV3( ax[ 2 ], V3( 0, 0, 1 ) ) );
		if( s.FindFixture( x.id ) ) x.id = s.NextFixtureId();
		s.fixtures.push_back( x );
	}
	s.AutoMount();
}

//one channel (8 bit, or 16 bit with the fine byte in the next channel) over all frames -> lane keys or a constant
struct ChanSeries
{
	int ui = 0, ch = 0;//file universe index, 0-based channel
	bool fine = false;
};
}//namespace

bool LbxImport( Show& s, const Library& lib, const LbxFile& f, const std::string& path, const LbxImportOptions& o, LbxImportResult* rOut, std::string* err )
{
	LbxImportResult r;
	auto fail = [ & ]( const std::string& e ) { if( err ) *err = e; return false; };
	const Json& us = f.info[ "universes" ];
	const int nU   = (int)us.Size();
	if( f.fps <= 0 || f.frames <= 0 || (int)f.dmx.size() != f.frames ) return fail( "keine Bilder" );
	if( nU <= 0 || f.dmxBytes != 512 * nU ) return fail( "Universen passen nicht zu den DMX-Daten" );
	const double at  = std::max( 0.0, o.at );
	const double dur = (double)f.frames / f.fps;
	const double t0  = f.info[ "start" ].Num( 0 );//timeline time of frame 0 in the source show
	if( o.newShow )
	{
		if( f.patch.IsObj() )
		{
			std::string e;
			if( !s.FromJson( f.patch, lib, "", &e ) ) return fail( "Setup: " + e );
			r.fromPatch = true;
			r.warn      = e;
		}
		else
			SceneToShow( s, lib, f, r );
		s.path.clear();
	}
	//every universe of the file must exist (by number)
	std::vector< int > uniNum( nU );
	for( int u = 0; u < nU; ++u )
	{
		uniNum[ u ] = us[ u ][ "num" ].I( u + 1 );
		if( s.UniIndex( uniNum[ u ] ) >= 0 ) continue;
		Universe x;
		x.num          = uniNum[ u ];
		x.name         = us[ u ][ "name" ].Str();
		if( x.name.empty() ) x.name = "Universe " + std::to_string( x.num );
		x.artnetPort   = us[ u ][ "artnet" ].I( x.num - 1 );
		x.artnetOut    = us[ u ][ "out" ].B( true );
		x.artnetInPort = x.num - 1;
		std::lock_guard< std::mutex > l( s.dmxLock );
		s.universes.push_back( x );
		s.dmx.push_back( DmxFrame{} );
		++r.universesAdded;
	}
	Timeline& tl = s.tl;
	std::string name = f.info[ "name" ].Str( "Lightshow" );
	if( name.empty() ) name = "Lightshow";
	if( o.tempo )
	{
		tl.bpm         = Clamp( f.info[ "bpm" ].F( tl.bpm ), 20.0f, 400.0f );
		tl.beatsPerBar = Clamp( f.info[ "beatsPerBar" ].I( tl.beatsPerBar ), 1, 16 );
		const Json& lp = f.info[ "loop" ];
		if( o.newShow && lp.IsObj() )
		{
			const double a = lp[ "a" ].Num( 0 ) - t0 + at, b = lp[ "b" ].Num( 16 ) - t0 + at;
			if( b > a )
			{
				tl.loop  = lp[ "on" ].B( false );
				tl.loopA = std::max( 0.0, a );
				tl.loopB = std::max( tl.loopA + 0.01, b );
			}
		}
	}
	if( o.markers )
	{
		const Json& cl = f.cues[ "cues" ];
		for( auto& c : cl.a )
		{
			//a cue at the same time as a marker (cue track + marker): one marker, the marker's name
			bool dup = false;
			if( c[ "kind" ].Str() == "cue" )
				for( auto& x : cl.a ) dup = dup || ( x[ "kind" ].Str() == "marker" && fabs( x[ "t" ].Num() - c[ "t" ].Num() ) < 1e-3 );
			if( dup ) continue;
			TlMarker m;
			m.t    = at + std::max( 0.0, c[ "t" ].Num( 0 ) );
			m.name = c[ "name" ].Str();
			tl.markers.push_back( m );
			++r.markers;
		}
		std::stable_sort( tl.markers.begin(), tl.markers.end(), []( const TlMarker& a, const TlMarker& b ) { return a.t < b.t; } );
	}
	if( o.asTrack )
	{
		auto pl = LbxPlayerLoad( path, true );
		if( !pl->err.empty() ) return fail( pl->err );
		TlTrack t;
		t.uid    = tl.NewUid();
		t.kind   = TlTrackKind::Show;
		t.name   = name;
		t.file   = path;
		t.offset = at;
		t.color  = V3( 0.95f, 0.6f, 0.25f );
		tl.tracks.push_back( t );
		r.tracks = 1;
	}
	else
	{
		//which lamp owns which channel (lamps of the file's scene); fine bytes from the profile when the lamp is known here
		const int frames = f.frames;
		const float eps  = std::max( 0.05f, o.tolerance ) / 255.0f;
		std::vector< std::vector< char > > owned( nU, std::vector< char >( 512, 0 ) );
		std::vector< std::pair< double, V3 > > pts;
		std::vector< int > keep;
		std::vector< char > mark;
		float hue = 0.07f;
		//builds one lane (or a constant / nothing); returns true when a lane was added to t
		auto series = [ & ]( TlTrack& t, int u, int c, bool fine ) -> bool {
			const int ui = s.UniIndex( uniNum[ u ] );
			const int a  = 512 * u + c;
			auto raw     = [ & ]( int k ) -> int { return fine ? ( f.dmx[ k ][ a ] << 8 ) | f.dmx[ k ][ a + 1 ] : f.dmx[ k ][ a ]; };
			const int v0 = raw( 0 );
			bool same    = true;
			for( int k = 1; k < frames && same; ++k ) same = raw( k ) == v0;
			const float scale = fine ? 65535.0f : 255.0f;
			TlLane l;
			l.kind = TlLaneKind::Dmx;
			l.uni  = uniNum[ u ];
			l.ch   = c + 1;
			l.fine = fine;
			l.height = 0;
			if( same )
			{
				if( ui < 0 ) return false;
				const int base = fine ? ( s.dmx[ ui ][ c ] << 8 ) | s.dmx[ ui ][ c + 1 ] : s.dmx[ ui ][ c ];
				if( o.newShow )
				{
					if( v0 == 0 ) return false;
					std::lock_guard< std::mutex > lk( s.dmxLock );
					if( fine )
					{
						s.dmx[ ui ][ c ]     = (uint8_t)( v0 >> 8 );
						s.dmx[ ui ][ c + 1 ] = (uint8_t)( v0 & 255 );
					}
					else
						s.dmx[ ui ][ c ] = (uint8_t)v0;
					++r.constants;
					return false;
				}
				if( v0 == base ) return false;
				TlKey k;
				k.t      = at;
				k.v      = v0 / scale;
				k.interp = TlInterp::Step;
				l.keys.push_back( k );
			}
			else
			{
				pts.resize( frames );
				for( int k = 0; k < frames; ++k ) pts[ k ] = { at + (double)k / f.fps, V3( raw( k ) / scale, 0, 0 ) };
				//thinning in windows (Ramer-Douglas-Peucker is quadratic on noise)
				mark.assign( frames, 0 );
				const int W = 400;
				std::vector< std::pair< double, V3 > > win;
				for( int w0 = 0; w0 < frames - 1; w0 += W )
				{
					const int w1 = std::min( frames - 1, w0 + W );
					win.assign( pts.begin() + w0, pts.begin() + w1 + 1 );
					TlThin( win, fine ? eps * ( 255.0f / 256.0f ) : eps, false, keep );
					for( int i : keep ) mark[ w0 + i ] = 1;
				}
				mark[ 0 ] = mark[ frames - 1 ] = 1;
				for( int k = 0; k < frames; ++k )
					if( mark[ k ] )
					{
						TlKey key;
						key.t      = pts[ k ].first;
						key.v      = pts[ k ].second.x;
						key.interp = TlInterp::Linear;
						l.keys.push_back( key );//linear between kept frames: exact at every kept frame, within eps in between
					}
			}
			r.keys += (int)l.keys.size();
			++r.lanes;
			t.lanes.push_back( std::move( l ) );
			return true;
		};
		auto newTrack = [ & ]( const std::string& nm ) {
			TlTrack t;
			t.uid   = tl.NewUid();
			t.kind  = TlTrackKind::Dmx;
			t.name  = nm;
			t.open  = false;
			t.color = Hsv( hue, 0.55f, 0.9f );
			hue     = fmodf( hue + 0.618034f, 1.0f );
			return t;
		};
		for( auto& fo : f.scene[ "fixtures" ].a )
		{
			const int num = fo[ "uni" ].I( 1 ), addr = fo[ "addr" ].I( 1 ), fp = fo[ "footprint" ].I( 0 );
			int u = -1;
			for( int k = 0; k < nU; ++k )
				if( uniNum[ k ] == num ) u = k;
			if( u < 0 || addr < 1 || fp <= 0 ) continue;
			//profile of the same lamp here (fine bytes)
			const Mode* m = nullptr;
			if( FixtureInst* x = o.newShow && r.fromPatch ? s.FindFixture( fo[ "id" ].I( -1 ) ) : nullptr )
				if( x->universe == num && x->address == addr && x->Footprint() == fp ) m = x->M();
			if( !m )
			{
				int slot = -1;
				if( FixtureInst* x = s.FixtureAt( num, addr, &slot ) )
					if( slot == 0 && x->Footprint() == fp ) m = x->M();
			}
			TlTrack t = newTrack( fo[ "name" ].Str( "Lampe" ) );
			for( int k = 0; k < fp; ++k )
			{
				const int c = addr - 1 + k;
				if( c >= 512 ) break;
				owned[ u ][ c ] = 1;
				if( m && k < (int)m->ch.size() && m->ch[ k ].isFine ) continue;//written with its coarse channel
				const bool fine = m && k < (int)m->ch.size() && m->ch[ k ].fine == k + 1 && c + 1 < 512;
				series( t, u, c, fine );
			}
			if( !t.lanes.empty() ) { tl.tracks.push_back( std::move( t ) ); ++r.tracks; ++r.fixtures; }
		}
		//channels without a lamp
		for( int u = 0; u < nU; ++u )
		{
			TlTrack t = newTrack( "DMX " + std::to_string( uniNum[ u ] ) );
			for( int c = 0; c < 512; ++c )
				if( !owned[ u ][ c ] ) series( t, u, c, false );
			if( !t.lanes.empty() ) { tl.tracks.push_back( std::move( t ) ); ++r.tracks; }
		}
	}
	tl.length = o.newShow ? std::max( 1.0, at + dur ) : std::max( tl.length, at + dur );
	if( tl.loopB > tl.length ) tl.loopB = tl.length;
	s.Bump();
	if( rOut ) *rOut = r;
	return true;
}

}//namespace ls
