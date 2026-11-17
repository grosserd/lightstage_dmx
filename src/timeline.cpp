// LightStage 2.0: show timeline - keyframe evaluation, transport / sync, output layer, file format
#include "show.h"
#include "json.h"
#include "i18n.h"
#include "lbshow.h"
#include <algorithm>
#include <cmath>

namespace ls
{
//---------------------------------------------------------------- timeline object
TlTrack* Timeline::Find( uint32_t uid )
{
	for( auto& t : tracks )
		if( t.uid == uid ) return &t;
	return nullptr;
}
uint32_t Timeline::NewUid() const
{
	uint32_t n = 1;
	for( auto& t : tracks ) n = std::max( n, t.uid + 1 );
	return n;
}
TlTrack* Timeline::CueTrack()
{
	for( auto& t : tracks )
		if( t.kind == TlTrackKind::Cues ) return &t;
	return nullptr;
}
double Timeline::GridStep() const
{
	const double beat = 60.0 / std::max( 1.0f, bpm );
	switch( grid )
	{
	case 1: return beat * std::max( 1, beatsPerBar );
	case 2: return beat;
	case 3: return beat * 0.5;
	case 4: return beat * 0.25;
	case 5: return beat * 0.125;
	case 6: return 1.0;
	case 7: return 0.1;
	case 8: return 1.0 / std::max( 1, fps );
	default: return 0;
	}
}
double Timeline::Snap( double t ) const
{
	const double g = GridStep();
	if( !snap || g <= 0 ) return t;
	return floor( t / g + 0.5 ) * g;
}
void Timeline::Locate( Show& s, double t )
{
	rt.pos     = Clamp( t, 0.0, std::max( 0.001, length ) );
	rt.seq     = ++s.snapSeq;//the timeline takes its channels back from the programmer
	rt.located = 3;
	rt.recT    = -1;
}
void Timeline::Play( Show& s, bool on )
{
	if( on && !rt.playing )
	{
		if( rt.pos >= length - 1e-6 ) rt.pos = loop ? loopA : 0;
		rt.seq = ++s.snapSeq;
	}
	rt.playing = on;
	rt.recT    = -1;
}

float TlClip::Weight( double T ) const
{
	if( T < t || T > t + len || len <= 0 ) return 0;
	float w = level;
	if( fadeIn > 0.001f ) w *= (float)std::min( 1.0, ( T - t ) / fadeIn );
	if( fadeOut > 0.001f ) w *= (float)std::min( 1.0, ( t + len - T ) / fadeOut );
	return Saturate( w );
}

//---------------------------------------------------------------- keys + curves
const char* TlInterpName( TlInterp i )
{
	switch( i )
	{
	case TlInterp::Step: return L( "Stufe (halten)" );
	case TlInterp::Linear: return L( "Linear" );
	case TlInterp::Smooth: return L( "Weich (S-Kurve)" );
	case TlInterp::Bezier: return L( "Bezier (Anfasser)" );
	case TlInterp::EaseIn: return L( "Langsam rein" );
	case TlInterp::EaseOut: return L( "Langsam raus" );
	default: return "?";
	}
}
void TlSort( TlLane& l )
{
	std::stable_sort( l.keys.begin(), l.keys.end(), []( const TlKey& a, const TlKey& b ) { return a.t < b.t; } );
}
//curve shape between key a and b: u = 0..1 in time -> 0..1 in value (Bezier: value offsets in absolute units)
static float Bez( float p0, float p1, float p2, float p3, float s )
{
	const float r = 1 - s;
	return r * r * r * p0 + 3 * r * r * s * p1 + 3 * r * s * s * p2 + s * s * s * p3;
}
static float Shape( const TlKey& a, const TlKey& b, float u, float va, float vb )
{
	switch( a.interp )
	{
	case TlInterp::Step: return va;
	case TlInterp::Linear: return Lerp( va, vb, u );
	case TlInterp::Smooth: return Lerp( va, vb, u * u * ( 3 - 2 * u ) );
	case TlInterp::EaseIn: return Lerp( va, vb, u * u );
	case TlInterp::EaseOut: return Lerp( va, vb, 1 - ( 1 - u ) * ( 1 - u ) );
	case TlInterp::Bezier:
	{
		const float x1 = Saturate( a.ox ), x2 = 1 - Saturate( b.ix );
		//x(s) is monotonic for handles inside the segment: bisection
		float lo = 0, hi = 1, s = u;
		for( int i = 0; i < 24; ++i )
		{
			s             = ( lo + hi ) * 0.5f;
			const float x = Bez( 0, x1, x2, 1, s );
			if( x < u ) lo = s;
			else hi = s;
		}
		return Bez( va, va + a.oy, vb + b.iy, vb, s );
	}
	default: return Lerp( va, vb, u );
	}
}
//segment of t: index of the key at or before t (-1 = before the first), u in the segment
static int Seg( const TlLane& l, double t, float& u )
{
	u            = 0;
	const int n  = (int)l.keys.size();
	if( n == 0 ) return -2;
	if( t <= l.keys[ 0 ].t ) return -1;
	if( t >= l.keys[ n - 1 ].t ) return n - 1;
	//binary search
	int lo = 0, hi = n - 1;
	while( hi - lo > 1 )
	{
		const int m = ( lo + hi ) / 2;
		if( l.keys[ m ].t <= t ) lo = m;
		else hi = m;
	}
	const double d = l.keys[ lo + 1 ].t - l.keys[ lo ].t;
	u              = d > 1e-9 ? (float)( ( t - l.keys[ lo ].t ) / d ) : 1.0f;
	return lo;
}
float TlEval( const TlLane& l, double t, bool* has )
{
	float u;
	const int i = Seg( l, t, u );
	if( has ) *has = i != -2;
	if( i == -2 ) return 0;
	if( i == -1 ) return l.keys[ 0 ].v;
	if( i == (int)l.keys.size() - 1 ) return l.keys[ i ].v;
	return Saturate( Shape( l.keys[ i ], l.keys[ i + 1 ], u, l.keys[ i ].v, l.keys[ i + 1 ].v ) );
}
V3 TlEvalColor( const TlLane& l, double t, bool* has )
{
	float u;
	const int i = Seg( l, t, u );
	if( has ) *has = i != -2;
	if( i == -2 ) return V3( 0, 0, 0 );
	if( i == -1 ) return l.keys[ 0 ].c;
	if( i == (int)l.keys.size() - 1 ) return l.keys[ i ].c;
	const TlKey &a = l.keys[ i ], &b = l.keys[ i + 1 ];
	TlKey a2       = a;
	a2.oy = 0;
	float k = Shape( a2, TlKey(), u, 0, 1 );//shape only (Bezier: time handles)
	if( a.interp == TlInterp::Bezier )
	{
		TlKey b2 = b;
		b2.iy    = 0;
		k        = Shape( a2, b2, u, 0, 1 );
	}
	return V3( Lerp( a.c.x, b.c.x, k ), Lerp( a.c.y, b.c.y, k ), Lerp( a.c.z, b.c.z, k ) );
}
int TlSetKey( TlLane& l, double t, float v, const V3* c, TlInterp interp )
{
	for( int i = 0; i < (int)l.keys.size(); ++i )
		if( fabs( l.keys[ i ].t - t ) < 1e-4 )
		{
			l.keys[ i ].v = Saturate( v );
			if( c ) l.keys[ i ].c = *c;
			return i;
		}
	TlKey k;
	k.t      = std::max( 0.0, t );
	k.v      = Saturate( v );
	k.interp = interp;
	if( c ) k.c = *c;
	l.keys.push_back( k );
	TlSort( l );
	for( int i = 0; i < (int)l.keys.size(); ++i )
		if( l.keys[ i ].t == k.t ) return i;
	return 0;
}
std::string TlLaneName( const TlLane& l )
{
	if( l.kind == TlLaneKind::Color ) return L( "Farbe" );
	if( l.kind == TlLaneKind::Dmx )
	{
		char b[ 48 ];
		snprintf( b, sizeof( b ), "DMX %d.%03d%s", l.uni, l.ch, l.fine ? " 16 bit" : "" );
		return b;
	}
	return AttrName( l.attr );
}

//---------------------------------------------------------------- file format
static Json KeyJson( const TlKey& k, bool col )
{
	Json a = Json::Arr();
	a.Push( k.t );
	a.Push( col ? 0.0f : k.v );
	a.Push( (int)k.interp );
	if( k.interp == TlInterp::Bezier )
	{
		a.Push( k.ox );
		a.Push( k.oy );
		a.Push( k.ix );
		a.Push( k.iy );
	}
	if( col )
	{
		Json c = Json::Arr();
		c.Push( k.c.x );
		c.Push( k.c.y );
		c.Push( k.c.z );
		a.Push( c );
	}
	return a;
}
Json TimelineToJson( const Timeline& tl )
{
	Json j = Json::Obj();
	j.Set( "length", tl.length );
	j.Set( "loop", tl.loop );
	j.Set( "loopA", tl.loopA );
	j.Set( "loopB", tl.loopB );
	j.Set( "sync", (int)tl.sync );
	j.Set( "offset", tl.offset );
	j.Set( "freewheel", tl.freewheel );
	j.Set( "bpm", tl.bpm );
	j.Set( "beatsPerBar", tl.beatsPerBar );
	j.Set( "grid", tl.grid );
	j.Set( "snap", tl.snap );
	j.Set( "master", tl.master );
	j.Set( "output", tl.output );
	j.Set( "fps", tl.fps );
	j.Set( "recMode", tl.recMode );
	j.Set( "recThin", tl.recThin );
	j.Set( "ltcDevice", tl.ltcDevice );
	j.Set( "ltcChannel", tl.ltcChannel );
	Json mk = Json::Arr();
	for( auto& m : tl.markers )
	{
		Json o = Json::Obj();
		o.Set( "t", m.t );
		o.Set( "name", m.name );
		mk.Push( o );
	}
	j.Set( "markers", mk );
	Json tr = Json::Arr();
	for( auto& t : tl.tracks )
	{
		Json o = Json::Obj();
		o.Set( "id", (double)t.uid );
		o.Set( "name", t.name );
		o.Set( "kind", (int)t.kind );
		Json fx = Json::Arr();
		for( int id : t.fixtures ) fx.Push( id );
		o.Set( "fixtures", fx );
		o.Set( "mute", t.mute );
		o.Set( "solo", t.solo );
		o.Set( "open", t.open );
		if( !t.cuts.empty() )
		{
			Json cu = Json::Arr();
			for( double x : t.cuts ) cu.Push( x );
			o.Set( "cuts", cu );
		}
		o.Set( "level", t.level );
		Json c = Json::Arr();
		c.Push( t.color.x );
		c.Push( t.color.y );
		c.Push( t.color.z );
		o.Set( "color", c );
		Json ls = Json::Arr();
		for( auto& l : t.lanes )
		{
			Json lo = Json::Obj();
			lo.Set( "kind", (int)l.kind );
			if( l.kind == TlLaneKind::Attr ) lo.Set( "attr", AttrName( l.attr ) );
			if( l.kind == TlLaneKind::Dmx )
			{
				lo.Set( "uni", l.uni );
				lo.Set( "ch", l.ch );
				lo.Set( "fine", l.fine );
			}
			lo.Set( "mute", l.mute );
			lo.Set( "height", l.height );
			if( l.HasFan() )
			{
				lo.Set( "fanTime", l.fanTime );
				lo.Set( "fanValue", l.fanValue );
				lo.Set( "fanOrder", l.fanOrder );
			}
			if( l.rec ) lo.Set( "rec", true );
			Json ks = Json::Arr();
			for( auto& k : l.keys ) ks.Push( KeyJson( k, l.kind == TlLaneKind::Color ) );
			lo.Set( "keys", ks );
			ls.Push( lo );
		}
		o.Set( "lanes", ls );
		if( t.kind == TlTrackKind::Clips )
		{
			Json cl = Json::Arr();
			for( auto& c : t.clips )
			{
				Json x = Json::Obj();
				x.Set( "t", c.t );
				x.Set( "len", c.len );
				x.Set( "kind", (int)c.kind );
				if( c.kind == TlClipKind::Snapshot ) x.Set( "snap", (double)c.snap );
				else x.Set( "fx", c.fx );
				x.Set( "fadeIn", c.fadeIn );
				x.Set( "fadeOut", c.fadeOut );
				x.Set( "level", c.level );
				cl.Push( x );
			}
			o.Set( "clips", cl );
		}
		if( t.kind == TlTrackKind::Audio )
		{
			o.Set( "file", t.file );
			o.Set( "gain", t.gain );
			o.Set( "offset", t.offset );
		}
		if( t.kind == TlTrackKind::Show )
		{
			o.Set( "file", t.file );
			o.Set( "offset", t.offset );
		}
		tr.Push( o );
	}
	j.Set( "tracks", tr );
	return j;
}
void TimelineFromJson( Timeline& tl, const Json& j )
{
	const TlRuntime rt = tl.rt;
	tl                 = Timeline();
	tl.rt              = rt;
	if( !j.IsObj() ) return;
	tl.length      = Clamp( j[ "length" ].Num( 120 ), 1.0, 360000.0 );
	tl.loop        = j[ "loop" ].B( false );
	tl.loopA       = std::max( 0.0, j[ "loopA" ].Num( 0 ) );
	tl.loopB       = std::max( tl.loopA + 0.01, j[ "loopB" ].Num( 16 ) );
	tl.sync        = (TlSync)Clamp( j[ "sync" ].I( 0 ), 0, 3 );
	tl.offset      = j[ "offset" ].Num( 0 );
	tl.freewheel   = Clamp( j[ "freewheel" ].F( 1 ), 0.0f, 30.0f );
	tl.bpm         = Clamp( j[ "bpm" ].F( 120 ), 20.0f, 400.0f );
	tl.beatsPerBar = Clamp( j[ "beatsPerBar" ].I( 4 ), 1, 16 );
	tl.grid        = Clamp( j[ "grid" ].I( 3 ), 0, 8 );
	tl.snap        = j[ "snap" ].B( true );
	tl.master      = Saturate( j[ "master" ].F( 1 ) );
	tl.output      = j[ "output" ].B( true );
	tl.fps         = Clamp( j[ "fps" ].I( 25 ), 1, 120 );
	tl.recMode     = Clamp( j[ "recMode" ].I( 0 ), 0, 2 );
	tl.recThin     = Clamp( j[ "recThin" ].F( 0.01f ), 0.0f, 0.2f );
	tl.ltcDevice   = j[ "ltcDevice" ].Str();
	tl.ltcChannel  = Clamp( j[ "ltcChannel" ].I( 0 ), 0, 1 );
	for( auto& o : j[ "markers" ].a )
	{
		TlMarker m;
		m.t    = std::max( 0.0, o[ "t" ].Num( 0 ) );
		m.name = o[ "name" ].Str();
		tl.markers.push_back( m );
	}
	for( auto& o : j[ "tracks" ].a )
	{
		TlTrack t;
		t.uid  = (uint32_t)o[ "id" ].Num( 0 );
		t.name = o[ "name" ].Str();
		t.kind = (TlTrackKind)Clamp( o[ "kind" ].I( 0 ), 0, 5 );
		t.file   = o[ "file" ].Str();
		t.gain   = Clamp( o[ "gain" ].F( 1 ), 0.0f, 4.0f );
		t.offset = o[ "offset" ].Num( 0 );
		for( auto& x : o[ "clips" ].a )
		{
			TlClip c;
			c.t       = std::max( 0.0, x[ "t" ].Num( 0 ) );
			c.len     = std::max( 0.01, x[ "len" ].Num( 4 ) );
			c.kind    = (TlClipKind)Clamp( x[ "kind" ].I( 0 ), 0, 1 );
			c.snap    = (uint32_t)x[ "snap" ].Num( 0 );
			c.fx      = x[ "fx" ].I( 0 );
			c.fadeIn  = std::max( 0.0f, x[ "fadeIn" ].F( 0.5f ) );
			c.fadeOut = std::max( 0.0f, x[ "fadeOut" ].F( 0.5f ) );
			c.level   = Saturate( x[ "level" ].F( 1 ) );
			t.clips.push_back( c );
		}
		for( auto& f : o[ "fixtures" ].a ) t.fixtures.push_back( f.I() );
		t.mute  = o[ "mute" ].B( false );
		t.solo  = o[ "solo" ].B( false );
		t.open  = o[ "open" ].B( true );
		for( auto& x : o[ "cuts" ].a ) t.cuts.push_back( x.Num() );
		t.level = Saturate( o[ "level" ].F( 1 ) );
		if( o[ "color" ].Size() == 3 ) t.color = V3( o[ "color" ][ 0 ].F(), o[ "color" ][ 1 ].F(), o[ "color" ][ 2 ].F() );
		for( auto& lo : o[ "lanes" ].a )
		{
			TlLane l;
			l.kind   = (TlLaneKind)Clamp( lo[ "kind" ].I( 0 ), 0, 2 );
			l.attr   = AttrFromName( lo[ "attr" ].Str( "Dimmer" ) );
			l.uni    = std::max( 1, lo[ "uni" ].I( 1 ) );
			l.ch     = Clamp( lo[ "ch" ].I( 1 ), 1, 512 );
			l.fine   = lo[ "fine" ].B( false );
			l.mute   = lo[ "mute" ].B( false );
			l.height = Clamp( lo[ "height" ].I( 1 ), 0, 2 );
			l.fanTime  = Clamp( lo[ "fanTime" ].F( 0 ), -600.0f, 600.0f );
			l.fanValue = Clamp( lo[ "fanValue" ].F( 0 ), -2.0f, 2.0f );
			l.fanOrder = Clamp( lo[ "fanOrder" ].I( 0 ), 0, 5 );
			l.rec      = lo[ "rec" ].B( false );
			for( auto& k : lo[ "keys" ].a )
			{
				TlKey key;
				key.t      = std::max( 0.0, k[ 0 ].Num( 0 ) );
				key.v      = Saturate( k[ 1 ].F( 0 ) );
				key.interp = (TlInterp)Clamp( k[ 2 ].I( 2 ), 0, (int)TlInterp::Count - 1 );
				size_t n   = 3;
				if( key.interp == TlInterp::Bezier )
				{
					key.ox = Saturate( k[ 3 ].F( 0.33f ) );
					key.oy = k[ 4 ].F( 0 );
					key.ix = Saturate( k[ 5 ].F( 0.33f ) );
					key.iy = k[ 6 ].F( 0 );
					n      = 7;
				}
				if( l.kind == TlLaneKind::Color && k[ n ].Size() == 3 ) key.c = V3( k[ n ][ 0 ].F(), k[ n ][ 1 ].F(), k[ n ][ 2 ].F() );
				l.keys.push_back( key );
			}
			TlSort( l );
			t.lanes.push_back( l );
		}
		if( !t.uid || tl.Find( t.uid ) ) t.uid = tl.NewUid();
		tl.tracks.push_back( t );
	}
	if( tl.rt.pos > tl.length ) tl.rt.pos = tl.length;
}

//---------------------------------------------------------------- record
void TlThin( const std::vector< std::pair< double, V3 > >& pts, float eps, bool color, std::vector< int >& keep )
{
	keep.clear();
	const int n = (int)pts.size();
	if( n == 0 ) return;
	if( n <= 2 )
	{
		for( int i = 0; i < n; ++i ) keep.push_back( i );
		return;
	}
	std::vector< char > mark( n, 0 );
	mark[ 0 ] = mark[ n - 1 ] = 1;
	std::vector< std::pair< int, int > > st{ { 0, n - 1 } };
	while( !st.empty() )
	{
		const auto [ a, b ] = st.back();
		st.pop_back();
		if( b - a < 2 ) continue;
		const double ta = pts[ a ].first, tb = pts[ b ].first;
		float worst = -1;
		int wi      = -1;
		for( int i = a + 1; i < b; ++i )
		{
			const float u = tb > ta ? (float)( ( pts[ i ].first - ta ) / ( tb - ta ) ) : 0.0f;
			const V3 &A = pts[ a ].second, &B = pts[ b ].second, &P = pts[ i ].second;
			float d     = fabsf( P.x - Lerp( A.x, B.x, u ) );
			if( color ) d = std::max( { d, fabsf( P.y - Lerp( A.y, B.y, u ) ), fabsf( P.z - Lerp( A.z, B.z, u ) ) } );
			if( d > worst ) { worst = d; wi = i; }
		}
		if( worst > eps && wi > 0 )
		{
			mark[ wi ] = 1;
			st.push_back( { a, wi } );
			st.push_back( { wi, b } );
		}
	}
	for( int i = 0; i < n; ++i )
		if( mark[ i ] ) keep.push_back( i );
}
//writes the recorded samples of one lane into its keys: keys inside the recorded span are replaced
int TlCommitLane( TlLane& l, float eps )
{
	if( l.recBuf.empty() ) return 0;
	const bool col  = l.kind == TlLaneKind::Color;
	const double t0 = l.recBuf.front().first, t1 = l.recBuf.back().first;
	//after the pass the curve goes back to what was there (if there was anything after it)
	const double tBack = t1 + 0.1;
	bool backHas       = false;
	for( auto& k : l.keys ) backHas = backHas || k.t > t1 + 1e-6;
	const float vBack  = TlEval( l, tBack );
	const V3 cBack     = TlEvalColor( l, tBack );
	bool keyThere      = false;
	for( auto& k : l.keys ) keyThere = keyThere || ( k.t > t1 + 1e-6 && k.t <= tBack + 1e-6 );
	l.keys.erase( std::remove_if( l.keys.begin(), l.keys.end(), [ & ]( const TlKey& k ) { return k.t >= t0 - 1e-6 && k.t <= t1 + 1e-6; } ), l.keys.end() );
	std::vector< int > keep;
	TlThin( l.recBuf, eps, col, keep );
	for( int i : keep )
	{
		TlKey k;
		k.t      = l.recBuf[ i ].first;
		k.interp = TlInterp::Linear;
		if( col ) k.c = l.recBuf[ i ].second;
		else k.v = Saturate( l.recBuf[ i ].second.x );
		l.keys.push_back( k );
	}
	if( backHas && !keyThere )
	{
		TlKey k;
		k.t      = tBack;
		k.v      = vBack;
		k.c      = cBack;
		k.interp = TlInterp::Linear;
		l.keys.push_back( k );
	}
	TlSort( l );
	l.recBuf.clear();
	return 1;
}
int TlCommitRecord( Show& s )
{
	int n = 0;
	for( auto& t : s.tl.tracks )
		for( auto& l : t.lanes )
		{
			n += TlCommitLane( l, s.tl.recThin );
			l.recLatch = false;
		}
	if( n )
	{
		++s.tl.rt.recCommits;
		s.dirty = true;
	}
	return n;
}

static void ColorTargets( Show& s, const FixtureInst& f, const V3& c, std::vector< std::pair< Attr, float > >& out );
//every channel a lane writes (universe index, 0-based address); fn returns true to stop
template< class Fn > static void LaneChannels( Show& s, const TlTrack& t, const TlLane& l, Fn fn )
{
	if( l.kind == TlLaneKind::Dmx )
	{
		fn( s.UniIndex( l.uni ), l.ch - 1 );
		return;
	}
	std::vector< int > sl;
	std::vector< std::pair< Attr, float > > cols;
	for( int id : t.fixtures )
	{
		const FixtureInst* f = s.FindFixture( id );
		if( !f ) continue;
		const int ui = s.UniIndex( f->universe );
		if( l.kind == TlLaneKind::Color ) ColorTargets( s, *f, V3( 1, 1, 1 ), cols );
		else cols.assign( 1, { l.attr, 0.0f } );
		for( auto& c : cols )
		{
			s.AttrSlots( *f, c.first, -1, sl );
			for( int k : sl )
				if( fn( ui, f->address - 1 + k ) ) return;
		}
	}
}
//the programmer moved a channel of this lane after the last play / locate (touch record, priority)
bool TlLaneTouched( Show& s, const TlTrack& t, const TlLane& l )
{
	const uint32_t seq = s.tl.rt.seq;
	bool r             = false;
	LaneChannels( s, t, l, [ & ]( int ui, int a ) {
		r = ui >= 0 && ui < (int)s.progSeq.size() && a >= 0 && a < 512 && s.progSeq[ ui ][ a ] > seq;
		return r;
	} );
	return r;
}
void TlLaneRelease( Show& s, const TlTrack& t, const TlLane& l )
{
	LaneChannels( s, t, l, [ & ]( int ui, int a ) {
		if( ui >= 0 && ui < (int)s.progSeq.size() && a >= 0 && a < 512 ) s.progSeq[ ui ][ a ] = 0;
		return false;
	} );
}

void TlFanPos( Show& s, const TlTrack& t, int order, std::vector< float >& out )
{
	const int n = (int)t.fixtures.size();
	out.assign( n, 0.0f );
	if( n < 2 ) return;
	std::vector< int > idx( n );
	for( int i = 0; i < n; ++i ) idx[ i ] = i;
	if( order == 1 || order == 4 )
	{
		auto key = [ & ]( int i ) {
			const FixtureInst* f = s.FindFixture( t.fixtures[ i ] );
			return f ? ( order == 1 ? f->pos.x : -f->pos.z ) : 0.0f;
		};
		std::stable_sort( idx.begin(), idx.end(), [ & ]( int a, int b ) { return key( a ) < key( b ); } );
	}
	for( int r = 0; r < n; ++r )
	{
		float u = (float)r / ( n - 1 );
		if( order == 2 ) u = fabsf( 2 * u - 1 );
		if( order == 5 ) u = 1 - fabsf( 2 * u - 1 );
		if( order == 3 ) u = 1 - u;
		out[ idx[ r ] ] = u;
	}
}

bool TlDegrees( Show& s, const TlTrack& t, Attr a, float v, float& deg )
{
	if( a != Attr::Pan && a != Attr::Tilt ) return false;
	for( int id : t.fixtures )
		if( const FixtureInst* f = s.FindFixture( id ) )
			if( f->def && s.HasAttr( *f, a ) )
			{
				deg = ( v - 0.5f ) * ( a == Attr::Pan ? f->def->panRange : f->def->tiltRange );
				return true;
			}
	return false;
}

//---------------------------------------------------------------- engine
void TimelineAdvance( Show& s, double dt )
{
	Timeline& tl  = s.tl;
	TlRuntime& rt = tl.rt;
	if( rt.located > 0 ) --rt.located;
	const double len = std::max( 0.001, tl.length );
	bool internal    = false;//advances by itself (loop / end apply)
	switch( tl.sync )
	{
	case TlSync::Internal:
		if( rt.playing ) { rt.pos += dt; internal = true; }
		break;
	case TlSync::Mtc:
	case TlSync::Ltc:
	{
		const bool ltc     = tl.sync == TlSync::Ltc;
		const double xSec  = ltc ? s.ltcSec : s.tcSec;
		const bool xRun    = ltc ? s.ltcRun : s.tcRun;
		const int xFps     = ltc ? s.ltcFps : s.tcFps;
		if( xSec >= 0 && xRun )
		{
			const double t = xSec + tl.offset;
			if( !rt.extRun ) rt.seq = ++s.snapSeq;//start
			if( fabs( t - rt.pos ) > 0.5 ) rt.located = 3;//jump: cue track chases
			rt.pos     = t;
			rt.extRun  = true;
			rt.playing = true;
			rt.extIdle = 0;
		}
		else if( rt.extRun )
		{
			rt.extIdle += dt;
			if( rt.extIdle <= tl.freewheel ) rt.pos += dt;//freewheel over drop-outs
			else
			{
				rt.extRun  = false;
				rt.playing = false;
				if( xSec >= 0 ) rt.pos = xSec + tl.offset;
			}
		}
		else if( xSec >= 0 && fabs( xSec + tl.offset - rt.pos ) > 1.0 / std::max( 1, xFps ) && rt.located == 0 && !rt.playing )
		{
			//stopped, but the source located somewhere else (full frame message): follow
			rt.pos     = xSec + tl.offset;
			rt.located = 3;
		}
		break;
	}
	case TlSync::Beat:
	{
		const double spb = 60.0 / std::max( 1.0f, tl.bpm );
		const double b   = s.tempo.beat;
		if( s.tempo.source == TempoSource::MidiClock )
		{
			//absolute song position (start / song position pointer)
			if( s.tempo.playing )
			{
				if( !rt.playing ) rt.seq = ++s.snapSeq;
				const double t = b * spb + tl.offset;
				if( fabs( t - rt.pos ) > 0.5 ) rt.located = 3;
				rt.pos     = t;
				rt.playing = true;
			}
			else
				rt.playing = false;
		}
		else
		{
			//Link / internal tempo: the play button starts it, the beats move it (stops with the Link transport)
			const double d = b - rt.lastBeat;
			if( rt.playing && s.tempo.playing && d > 0 && d < 4 )
			{
				rt.pos += d * spb;
				internal = true;
			}
		}
		rt.lastBeat = b;
		break;
	}
	}
	if( internal )
	{
		if( tl.loop && tl.loopB > tl.loopA + 0.01 && rt.pos >= tl.loopB ) rt.pos = tl.loopA + fmod( rt.pos - tl.loopA, tl.loopB - tl.loopA );
		else if( rt.pos >= len )
		{
			rt.pos     = len;
			rt.playing = false;
		}
	}
	if( rt.pos < 0 ) rt.pos = 0;
	//record: a pass ends with stop, record off or a jump (loop, locate); armed lanes sample the programmer
	const bool recActive = tl.record && rt.playing;
	if( rt.recOn && ( !recActive || rt.pos < rt.recT - 0.05 || rt.pos > rt.recT + 1.0 ) )
	{
		TlCommitRecord( s );
		rt.recOn = false;
		rt.recT  = -1;
	}
	if( recActive )
	{
		if( !rt.recOn ) { rt.recOn = true; rt.recT = -1; }
		if( rt.recT < 0 || rt.pos - rt.recT >= 1.0 / 60.0 )
		{
			for( auto& t : tl.tracks )
			{
				if( t.kind != TlTrackKind::Fixtures && t.kind != TlTrackKind::Dmx ) continue;
				for( auto& l : t.lanes )
				{
					if( !l.rec ) continue;
					if( tl.recMode == 2 && !l.recLatch ) l.recLatch = TlLaneTouched( s, t, l );
					const bool touched = tl.recMode == 1 && TlLaneTouched( s, t, l );
					if( ( tl.recMode == 1 && !touched ) || ( tl.recMode == 2 && !l.recLatch ) )
					{
						if( !l.recBuf.empty() ) TlCommitLane( l, tl.recThin );//touch released: this segment is done
						l.recLastV = -1;
						continue;
					}
					V3 c( 0, 0, 0 );
					const float v = TlLiveValue( s, t, l, &c );
					if( tl.recMode == 1 )
					{
						//touch: the control rests for half a second = let go, the lane plays its curve again
						const float sig = l.kind == TlLaneKind::Color ? c.x + c.y * 3 + c.z * 9 : v;
						if( l.recLastV < 0 || fabsf( sig - l.recLastV ) > 1e-4f ) { l.recLastV = sig; l.recLastChg = rt.pos; }
						else if( rt.pos - l.recLastChg > 0.5 )
						{
							TlCommitLane( l, tl.recThin );
							TlLaneRelease( s, t, l );
							l.recLastV = -1;
							continue;
						}
					}
					l.recBuf.push_back( { rt.pos, l.kind == TlLaneKind::Color ? c : V3( v, 0, 0 ) } );
				}
			}
			rt.recT = rt.pos;
		}
	}
}

bool SnapFadeable( Attr a );

//colour -> channel levels of a lamp (Show::ColorTargets: RGB/W/A/L, CMY, colour wheel)
static void ColorTargets( Show& s, const FixtureInst& f, const V3& c, std::vector< std::pair< Attr, float > >& out )
{
	s.ColorTargets( f, c, out );
}

void TimelineApply( Show& s )
{
	Timeline& tl  = s.tl;
	TlRuntime& rt = tl.rt;
	rt.any        = false;
	if( !tl.output || tl.master <= 0 || tl.tracks.empty() ) return;
	bool solo = false;
	for( auto& t : tl.tracks ) solo = solo || ( t.solo && t.kind != TlTrackKind::Cues );
	const double T = rt.pos;
	std::lock_guard< std::mutex > lk( s.dmxLock );
	bool prepared = false;
	auto prep     = [ & ] {
		if( prepared ) return;
		prepared = true;
		if( s.snapOut.size() != s.universes.size() ) s.snapOut.resize( s.universes.size() );
		if( !s.snapAny )
			for( auto& r : s.snapOut ) r.fill( -1 );
	};
	//one channel (coarse a, fine af or -1) towards v16 with weight w
	auto put = [ & ]( int ui, int a, int af, float v16, float w, bool fade ) {
		if( ui < 0 || ui >= (int)s.snapOut.size() || a < 0 || a >= 512 ) return;
		if( ui < (int)s.progSeq.size() && s.progSeq[ ui ][ a ] > rt.seq ) return;//moved in the programmer after play / locate
		auto& L       = s.snapOut[ ui ];
		const auto& D = s.dmx[ ui ];
		auto rd       = [ & ]( int x ) -> int { return L[ x ] >= 0 ? L[ x ] : D[ x ]; };
		const bool fn = af >= 0 && af < 512;
		const float under = fn ? (float)( ( rd( a ) << 8 ) | rd( af ) ) : rd( a ) * 257.0f;
		const float out   = fade ? Lerp( under, v16, w ) : ( w > 0 ? v16 : under );
		const int o16     = Clamp( (int)lroundf( out ), 0, 65535 );
		if( fn )
		{
			L[ a ]  = (int16_t)( o16 >> 8 );
			L[ af ] = (int16_t)( o16 & 255 );
		}
		else
			L[ a ] = (int16_t)( ( o16 + 128 ) / 257 );
		rt.any = true;
	};
	static thread_local std::vector< int > slots;
	static thread_local std::vector< std::pair< Attr, float > > cols;
	auto putAttr = [ & ]( const FixtureInst& f, Attr a, float v, float w ) {
		const Mode* m = f.M();
		const int ui  = s.UniIndex( f.universe );
		if( !m || ui < 0 ) return;
		s.AttrSlots( f, a, -1, slots );
		for( int k : slots )
		{
			const Channel& c = m->ch[ k ];
			const int fineA  = c.fine >= 0 && c.fine < (int)m->ch.size() ? f.address - 1 + c.fine : -1;
			put( ui, f.address - 1 + k, fineA, Saturate( v ) * 65535.0f, w, SnapFadeable( a ) );
		}
	};
	const bool recOverwrite = tl.record && rt.playing && tl.recMode == 0;
	static thread_local std::vector< float > fan;
	//effects switched on by clips (and by snapshots on clips): drive per lamp like snapshots do
	auto drive = [ & ]( int id, float w ) {
		Effect* e = s.FindEffect( id );
		if( !e || w <= 0 ) return;
		const size_t n = e->fixtures.size();
		if( e->fxD.size() != n )
		{
			e->fxD.assign( n, -1.0f );
			e->fxI.assign( n, 1.0f );
		}
		float m = e->snapW < 0 ? e->cur : e->snapW;
		m       = std::max( m, w );
		for( size_t k = 0; k < n; ++k )
		{
			const float d = e->fxD[ k ] >= 0 ? e->fxD[ k ] : e->cur;
			e->fxD[ k ]   = std::max( d, w );
			m             = std::max( m, e->fxD[ k ] );
		}
		e->snapW = m;
	};
	for( auto& t : tl.tracks )
	{
		if( t.kind == TlTrackKind::Cues || t.kind == TlTrackKind::Audio || t.mute || ( solo && !t.solo ) ) continue;
		const float w = Saturate( tl.master * t.level );
		if( w <= 0 ) continue;
		if( t.kind == TlTrackKind::Show )
		{
			//Lightshow track: the recorded frame at this time, every channel of the file's universes
			auto pl = LbxPlayerGet( t.file );
			if( !pl || pl->frames <= 0 ) continue;
			const double ft = ( T - t.offset ) * pl->fps + 1e-3;//a thousandth of a frame: summed time steps land exactly on the frame
			if( ft < 0 || ft >= pl->frames ) continue;
			static thread_local std::vector< uint8_t > fr;
			if( !pl->Frame( Clamp( (int)floor( ft ), 0, pl->frames - 1 ), fr ) ) continue;
			prep();
			for( size_t u = 0; u < pl->uniNums.size() && ( u + 1 ) * 512 <= fr.size(); ++u )
			{
				const int ui = s.UniIndex( pl->uniNums[ u ] );
				if( ui < 0 ) continue;
				const uint8_t* d = fr.data() + 512 * u;
				for( int c = 0; c < 512; ++c ) put( ui, c, -1, d[ c ] * 257.0f, w, true );
			}
			rt.any = true;
			continue;
		}
		if( t.kind == TlTrackKind::Clips )
		{
			for( auto& c : t.clips )
			{
				const float cw = c.Weight( T ) * w;
				if( cw <= 0 ) continue;
				rt.any = true;
				if( c.kind == TlClipKind::Effect ) { drive( c.fx, cw ); continue; }
				const int si = s.FindSnapUid( c.snap );
				if( si < 0 ) continue;
				const Snapshot& sn = s.snapshots[ si ];
				for( int id : sn.effects ) drive( id, cw );
				prep();
				for( const SnapFixture& sf : sn.fix )
				{
					const FixtureInst* f = s.FindFixture( sf.id );
					const Mode* m        = f ? f->M() : nullptr;
					const int ui         = f ? s.UniIndex( f->universe ) : -1;
					if( !m || ui < 0 ) continue;
					const int n       = std::min( (int)m->ch.size(), (int)sf.v.size() );
					const bool hasDim = s.HasAttr( *f, Attr::Dimmer );
					for( int k = 0; k < n; ++k )
					{
						const Channel& ch = m->ch[ k ];
						if( ch.isFine ) continue;
						const bool fine = ch.fine >= 0 && ch.fine < n;
						float tgt       = fine ? (float)( ( sf.v[ k ] << 8 ) | sf.v[ ch.fine ] ) : sf.v[ k ] * 257.0f;
						if( ch.attr == Attr::Dimmer || ( !hasDim && IsEmitter( ch.attr ) ) ) tgt *= Saturate( sn.master );
						put( ui, f->address - 1 + k, fine ? f->address - 1 + ch.fine : -1, tgt, cw, SnapFadeable( ch.attr ) );
					}
				}
			}
			continue;
		}
		for( auto& l : t.lanes )
		{
			if( l.mute || l.keys.empty() ) continue;
			if( l.rec && recOverwrite ) continue;//recording: the programmer passes through
			prep();
			const bool fanOn = l.HasFan() && t.fixtures.size() > 1;
			if( fanOn ) TlFanPos( s, t, l.fanOrder, fan );
			if( l.kind == TlLaneKind::Dmx )
			{
				const int ui = s.UniIndex( l.uni );
				const float v = TlEval( l, T );
				put( ui, l.ch - 1, l.fine ? l.ch : -1, v * 65535.0f, w, true );
				continue;
			}
			if( l.kind == TlLaneKind::Color )
			{
				const V3 c0 = TlEvalColor( l, T );
				for( size_t k = 0; k < t.fixtures.size(); ++k )
					if( const FixtureInst* f = s.FindFixture( t.fixtures[ k ] ) )
					{
						const V3 c = fanOn ? TlEvalColor( l, T - l.fanTime * fan[ k ] ) : c0;
						ColorTargets( s, *f, c, cols );
						for( auto& p : cols ) putAttr( *f, p.first, p.second, w );
					}
				continue;
			}
			const float v0 = TlEval( l, T );
			for( size_t k = 0; k < t.fixtures.size(); ++k )
				if( const FixtureInst* f = s.FindFixture( t.fixtures[ k ] ) )
				{
					const float v = fanOn ? TlEval( l, T - l.fanTime * fan[ k ] ) + l.fanValue * ( fan[ k ] - 0.5f ) : v0;
					putAttr( *f, l.attr, v, w );
				}
		}
	}
	if( rt.any ) s.snapAny = true;
}

float TlLiveValue( Show& s, const TlTrack& t, const TlLane& l, V3* col )
{
	if( l.kind == TlLaneKind::Dmx )
	{
		const int ui = s.UniIndex( l.uni );
		if( ui < 0 || l.ch < 1 || l.ch > 512 ) return 0;
		const auto& D = s.dmx[ ui ];
		if( l.fine && l.ch < 512 ) return ( ( D[ l.ch - 1 ] << 8 ) | D[ l.ch ] ) / 65535.0f;
		return D[ l.ch - 1 ] / 255.0f;
	}
	for( int id : t.fixtures )
	{
		const FixtureInst* f = s.FindFixture( id );
		if( !f || !f->M() ) continue;
		if( l.kind == TlLaneKind::Color )
		{
			if( !s.HasColor( *f ) ) continue;
			const V3 c = s.GetColor( *f, -1, 1 );
			if( col ) *col = c;
			return std::max( { c.x, c.y, c.z } );
		}
		if( s.HasAttr( *f, l.attr ) ) return s.GetAttrBase( *f, l.attr );
	}
	if( col ) *col = V3( 1, 1, 1 );
	return 0;
}

std::vector< Attr > TlTrackAttrs( Show& s, const std::vector< int >& fixtures )
{
	std::vector< Attr > out;
	for( int id : fixtures )
	{
		const FixtureInst* f = s.FindFixture( id );
		const Mode* m        = f ? f->M() : nullptr;
		if( !m ) continue;
		for( auto& c : m->ch )
		{
			if( c.isFine || c.attr == Attr::None || c.attr == Attr::Generic ) continue;
			if( std::find( out.begin(), out.end(), c.attr ) == out.end() ) out.push_back( c.attr );
		}
	}
	return out;
}
//================================================================ sections: blocks of a collapsed track
static bool TlIn( double t, double a, double b ) { return t >= a - 1e-9 && t <= b + 1e-9; }
static void TlKeyTimes( const TlTrack& t, std::vector< double >& ts )
{
	ts.clear();
	for( auto& l : t.lanes )
		for( auto& k : l.keys ) ts.push_back( k.t );
	std::sort( ts.begin(), ts.end() );
}
//a cut is kept only while it separates two keys
static void TlPruneCuts( TlTrack& t )
{
	std::vector< double > ts;
	TlKeyTimes( t, ts );
	std::sort( t.cuts.begin(), t.cuts.end() );
	std::vector< double > keep;
	for( double c : t.cuts )
	{
		auto it = std::lower_bound( ts.begin(), ts.end(), c - 1e-9 );//first key >= cut
		if( it == ts.begin() || it == ts.end() ) continue;
		if( !keep.empty() && c - keep.back() < 1e-7 ) continue;
		keep.push_back( c );
	}
	t.cuts.swap( keep );
}
double TlBlockGap( const Timeline& tl )
{
	const double bar = 60.0 / std::max( 1.0f, tl.bpm ) * std::max( 1, tl.beatsPerBar );
	return std::max( 1.0, 2 * bar );
}
void TlBlocks( const Timeline& tl, const TlTrack& t, std::vector< TlBlock >& out )
{
	out.clear();
	std::vector< double > ts;
	TlKeyTimes( t, ts );
	if( ts.empty() ) return;
	const double gap = TlBlockGap( tl );
	TlBlock b;
	b.t0 = b.t1 = ts[ 0 ];
	b.keys      = 1;
	for( size_t i = 1; i < ts.size(); ++i )
	{
		bool cut = ts[ i ] - ts[ i - 1 ] > gap + 1e-6;//keys exactly 2 bars apart stay together (rounding)
		if( !cut )
			for( double c : t.cuts )
				if( c > ts[ i - 1 ] + 1e-9 && c <= ts[ i ] + 1e-9 ) { cut = true; break; }
		if( cut )
		{
			out.push_back( b );
			b.t0   = ts[ i ];
			b.keys = 0;
		}
		b.t1 = ts[ i ];
		++b.keys;
	}
	out.push_back( b );
}
void TlMoveBlocks( Timeline& tl, TlTrack& t, const std::vector< std::pair< double, double > >& src, double dt, bool copy )
{
	(void)tl;
	if( src.empty() ) return;
	double mn = 1e18;
	for( auto& s : src ) mn = std::min( mn, s.first );
	if( mn + dt < 0 ) dt = -mn;
	if( fabs( dt ) < 1e-9 ) return;
	auto inSrc = [ & ]( double x ) {
		for( auto& s : src )
			if( TlIn( x, s.first, s.second ) ) return true;
		return false;
	};
	auto inDst = [ & ]( double x ) {
		for( auto& s : src )
			if( TlIn( x, s.first + dt, s.second + dt ) ) return true;
		return false;
	};
	for( auto& l : t.lanes )
	{
		std::vector< TlKey > mv;
		for( auto& k : l.keys )
			if( inSrc( k.t ) ) mv.push_back( k );
		if( !copy ) l.keys.erase( std::remove_if( l.keys.begin(), l.keys.end(), [ & ]( const TlKey& k ) { return inSrc( k.t ); } ), l.keys.end() );
		l.keys.erase( std::remove_if( l.keys.begin(), l.keys.end(), [ & ]( const TlKey& k ) { return inDst( k.t ); } ), l.keys.end() );//the target is overwritten
		for( auto& k : l.keys ) k.sel = false;
		for( auto k : mv )
		{
			k.t += dt;
			k.sel = true;
			l.keys.push_back( k );
		}
		TlSort( l );
	}
	//the moved blocks stay blocks of their own next to their new neighbours (old cuts inside the target would split them)
	t.cuts.erase( std::remove_if( t.cuts.begin(), t.cuts.end(), [ & ]( double c ) {
		              for( auto& s : src )
			              if( c > s.first + dt + 1e-9 && c < s.second + dt + 0.001 ) return true;
		              return false;
	              } ),
	              t.cuts.end() );
	for( auto& s : src )
	{
		t.cuts.push_back( s.first + dt );
		t.cuts.push_back( s.second + dt + 0.0005 );//behind the last key (0.5 ms: survives the 9 digits of the file format)
	}
	TlPruneCuts( t );
}
//value / colour / curve type of a lane at x (the curve type of the segment x lies in)
static void TlAt( const TlLane& l, double x, float& v, V3& c, TlInterp& ip )
{
	v  = TlEval( l, x );
	c  = l.kind == TlLaneKind::Color ? TlEvalColor( l, x ) : V3( 1, 1, 1 );
	ip = TlInterp::Smooth;
	for( auto& k : l.keys )
		if( k.t <= x + 1e-9 ) ip = k.interp;
}
void TlTrimBlock( Timeline& tl, TlTrack& t, double t0, double t1, bool left, double x )
{
	(void)tl;
	x = std::max( 0.0, left ? std::min( x, t1 ) : std::max( x, t0 ) );
	for( auto& l : t.lanes )
	{
		bool has = false;
		for( auto& k : l.keys ) has = has || TlIn( k.t, t0, t1 );
		if( !has ) continue;
		float v;
		V3 c;
		TlInterp ip;
		TlAt( l, x, v, c, ip );
		if( left ) l.keys.erase( std::remove_if( l.keys.begin(), l.keys.end(), [ & ]( const TlKey& k ) { return k.t >= t0 - 1e-9 && k.t < x - 1e-9; } ), l.keys.end() );
		else l.keys.erase( std::remove_if( l.keys.begin(), l.keys.end(), [ & ]( const TlKey& k ) { return k.t > x + 1e-9 && k.t <= t1 + 1e-9; } ), l.keys.end() );
		const int i     = TlSetKey( l, x, v, l.kind == TlLaneKind::Color ? &c : nullptr, ip );
		l.keys[ i ].sel = true;
	}
	{
		//cuts inside the new extent would split the block
		const double n0 = left ? x : t0, n1 = left ? t1 : x;
		t.cuts.erase( std::remove_if( t.cuts.begin(), t.cuts.end(), [ & ]( double c ) { return c > n0 + 1e-9 && c < std::max( n1, t1 ) + 0.001; } ), t.cuts.end() );
	}
	t.cuts.push_back( left ? x : x + 0.0005 );
	TlPruneCuts( t );
}
bool TlSplitBlock( Timeline& tl, TlTrack& t, double at )
{
	std::vector< TlBlock > bs;
	TlBlocks( tl, t, bs );
	const double e = 0.001;//the left half ends 1 ms before the cut
	for( auto& b : bs )
	{
		if( !( at > b.t0 + 2 * e && at < b.t1 - e ) ) continue;
		for( auto& l : t.lanes )
		{
			bool before = false, after = false;
			for( auto& k : l.keys )
				if( TlIn( k.t, b.t0, b.t1 ) )
				{
					if( k.t < at - 1e-9 ) before = true;
					else after = true;
				}
			if( !before || !after ) continue;
			float v;
			V3 c;
			TlInterp ip;
			TlAt( l, at, v, c, ip );
			const V3* cp = l.kind == TlLaneKind::Color ? &c : nullptr;
			bool atKey   = false;
			for( auto& k : l.keys ) atKey = atKey || fabs( k.t - at ) < 1e-4;
			TlSetKey( l, at - e, v, cp, ip );
			if( !atKey ) TlSetKey( l, at, v, cp, ip );
		}
		t.cuts.push_back( at );
		TlPruneCuts( t );
		return true;
	}
	return false;
}
bool TlJoinBlocks( Timeline& tl, TlTrack& t, double t1 )
{
	std::vector< double > ts;
	TlKeyTimes( t, ts );
	auto it = std::upper_bound( ts.begin(), ts.end(), t1 + 1e-9 );
	if( it == ts.end() ) return false;
	const double nt = *it;
	const size_t n  = t.cuts.size();
	t.cuts.erase( std::remove_if( t.cuts.begin(), t.cuts.end(), [ & ]( double c ) { return c > t1 + 1e-9 && c <= nt + 1e-9; } ), t.cuts.end() );
	return t.cuts.size() != n && nt - t1 <= TlBlockGap( tl ) + 1e-6;
}

//================================================================ range over all tracks
void TlRangeCopy( const Show& s, double a, double b, TlRangeClip& out )
{
	out = TlRangeClip();
	if( b <= a ) return;
	out.len = b - a;
	for( auto& t : s.tl.tracks )
	{
		for( int li = 0; li < (int)t.lanes.size(); ++li )
			for( auto k : t.lanes[ li ].keys )
				if( k.t >= a - 1e-9 && k.t < b - 1e-9 )
				{
					k.t -= a;
					k.sel = false;
					out.keys[ { t.uid, li } ].push_back( k );
				}
		for( auto c : t.clips )
			if( c.t >= a - 1e-9 && c.t < b - 1e-9 )
			{
				c.t -= a;
				c.sel = false;
				out.clips.push_back( { t.uid, c } );
			}
	}
	for( auto m : s.tl.markers )
		if( m.t >= a - 1e-9 && m.t < b - 1e-9 )
		{
			m.t -= a;
			out.markers.push_back( m );
		}
	for( int i = 0; i < (int)s.cues.size(); ++i )
		if( s.cues[ i ].trigger == 2 && s.cues[ i ].tc >= a - 1e-9 && s.cues[ i ].tc < b - 1e-9 ) out.cues.push_back( { i, s.cues[ i ].tc - a } );
}
void TlInsertTime( Show& s, double at, double len )
{
	if( len <= 0 ) return;
	Timeline& tl = s.tl;
	for( auto& t : tl.tracks )
	{
		for( auto& l : t.lanes )
			for( auto& k : l.keys )
				if( k.t >= at - 1e-9 ) k.t += len;
		for( auto& c : t.clips )
			if( c.t >= at - 1e-9 ) c.t += len;
		for( auto& c : t.cuts )
			if( c >= at - 1e-9 ) c += len;
		if( t.kind == TlTrackKind::Show && t.offset >= at - 1e-9 ) t.offset += len;
	}
	for( auto& m : tl.markers )
		if( m.t >= at - 1e-9 ) m.t += len;
	for( auto& c : s.cues )
		if( c.trigger == 2 && c.tc >= at - 1e-9 ) c.tc += len;
	tl.length += len;
	s.dirty = true;
}
void TlDeleteTime( Show& s, double a, double b )
{
	if( b <= a ) return;
	Timeline& tl     = s.tl;
	const double len = b - a;
	auto in          = [ & ]( double t ) { return t >= a - 1e-9 && t < b - 1e-9; };
	for( auto& t : tl.tracks )
	{
		for( auto& l : t.lanes )
		{
			l.keys.erase( std::remove_if( l.keys.begin(), l.keys.end(), [ & ]( const TlKey& k ) { return in( k.t ); } ), l.keys.end() );
			for( auto& k : l.keys )
				if( k.t >= b - 1e-9 ) k.t -= len;
		}
		t.clips.erase( std::remove_if( t.clips.begin(), t.clips.end(), [ & ]( const TlClip& c ) { return in( c.t ); } ), t.clips.end() );
		for( auto& c : t.clips )
			if( c.t >= b - 1e-9 ) c.t -= len;
		t.cuts.erase( std::remove_if( t.cuts.begin(), t.cuts.end(), [ & ]( double c ) { return in( c ); } ), t.cuts.end() );
		for( auto& c : t.cuts )
			if( c >= b - 1e-9 ) c -= len;
		t.cuts.push_back( a );//what moved up stays a block of its own
		TlPruneCuts( t );
		if( t.kind == TlTrackKind::Show && t.offset >= b - 1e-9 ) t.offset -= len;
	}
	tl.markers.erase( std::remove_if( tl.markers.begin(), tl.markers.end(), [ & ]( const TlMarker& m ) { return in( m.t ); } ), tl.markers.end() );
	for( auto& m : tl.markers )
		if( m.t >= b - 1e-9 ) m.t -= len;
	for( auto& c : s.cues )
	{
		if( c.trigger != 2 ) continue;
		if( in( c.tc ) ) c.trigger = 0;//the cue stays in the list, it only loses its timecode
		else if( c.tc >= b - 1e-9 ) c.tc -= len;
	}
	tl.length = std::max( 1.0, tl.length - len );
	s.dirty   = true;
}
void TlRangePaste( Show& s, const TlRangeClip& c, double at )
{
	if( c.Empty() ) return;
	Timeline& tl = s.tl;
	at           = std::max( 0.0, at );
	TlInsertTime( s, at, c.len );
	for( auto& e : c.keys )
		if( TlTrack* t = tl.Find( e.first.first ) )
			if( e.first.second < (int)t->lanes.size() )
			{
				TlLane& l = t->lanes[ e.first.second ];
				for( auto k : e.second )
				{
					k.t += at;
					l.keys.push_back( k );
				}
				TlSort( l );
			}
	for( auto& e : c.clips )
		if( TlTrack* t = tl.Find( e.first ) )
		{
			TlClip n = e.second;
			n.t += at;
			t->clips.push_back( n );
		}
	for( auto m : c.markers )
	{
		m.t += at;
		tl.markers.push_back( m );
	}
	for( auto& t : tl.tracks )
	{
		if( t.lanes.empty() ) continue;
		t.cuts.push_back( at );
		t.cuts.push_back( at + c.len );
		TlPruneCuts( t );
	}
	s.dirty = true;
}
double TlRangeMove( Show& s, double a, double b, double to )
{
	if( b <= a ) return a;
	TlRangeClip c;
	TlRangeCopy( s, a, b, c );
	TlDeleteTime( s, a, b );
	double at = to >= b ? to - ( b - a ) : ( to > a ? a : to );
	at        = std::max( 0.0, at );
	TlRangePaste( s, c, at );
	for( auto& q : c.cues )
		if( q.first >= 0 && q.first < (int)s.cues.size() )
		{
			s.cues[ q.first ].trigger = 2;
			s.cues[ q.first ].tc      = at + q.second;
		}
	return at;
}
}//namespace ls
