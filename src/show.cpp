#include "show.h"
#include "effects.h"
#include "params.h"
#include <atomic>
#include <random>
#include "../third_party/stb_image.h"

namespace ls
{
const char* MidiTypeName( MidiType t )
{
	static const char* n[] = { "CC", "Note", "Pitch Bend", "CC 14-bit" };
	return n[ (int)t ];
}
const char* MidiTargetName( MidiTarget t )
{
	static const char* n[] = { "Fixture Attribute", "DMX Channel", "Snapshot", "Haze", "Master", "Parameter" };
	return n[ (int)t ];
}

Json MidiMapToJson( const MidiMap& m )
{
	Json o = Json::Obj();
	o.Set( "device", m.device );
	o.Set( "channel", m.channel );
	o.Set( "type", (int)m.type );
	o.Set( "number", m.number );
	o.Set( "target", (int)m.target );
	Json ids = Json::Arr();
	for( int id : m.fixtures ) ids.Push( id );
	o.Set( "fixtures", ids );
	o.Set( "attr", AttrName( m.attr ) );
	o.Set( "head", m.head );
	o.Set( "universe", m.universe );
	o.Set( "dmxChannel", m.channelDmx );
	o.Set( "sixteen", m.sixteen );
	o.Set( "snapshot", m.snapshot );
	o.Set( "lo", m.lo );
	o.Set( "hi", m.hi );
	o.Set( "invert", m.invert );
	o.Set( "toggle", m.toggle );
	if( !m.param.empty() ) o.Set( "param", m.param );
	return o;
}
MidiMap MidiMapFromJson( const Json& o )
{
	MidiMap m;
	m.device     = o[ "device" ].Str();
	m.channel    = o[ "channel" ].I( -1 );
	m.type       = (MidiType)Clamp( o[ "type" ].I(), 0, 3 );
	m.number     = o[ "number" ].I();
	m.target     = (MidiTarget)Clamp( o[ "target" ].I(), 0, 5 );
	for( auto& id : o[ "fixtures" ].a ) m.fixtures.push_back( id.I() );
	m.attr       = AttrFromName( o[ "attr" ].Str( "Dimmer" ) );
	m.head       = o[ "head" ].I( -1 );
	m.universe   = o[ "universe" ].I( 1 );
	m.channelDmx = o[ "dmxChannel" ].I( 1 );
	m.sixteen    = o[ "sixteen" ].B();
	m.snapshot   = o[ "snapshot" ].I();
	m.lo         = o[ "lo" ].F( 0 );
	m.hi         = o[ "hi" ].F( 1 );
	m.invert     = o[ "invert" ].B();
	m.toggle     = o[ "toggle" ].B();
	m.param      = o[ "param" ].Str();
	return m;
}

std::string MidiMap::Describe( const Show& s ) const
{
	char b[ 256 ];
	switch( target )
	{
	case MidiTarget::FixtureAttr:
	{
		std::string fx;
		for( size_t i = 0; i < fixtures.size() && i < 4; ++i )
			fx += ( i ? "," : "" ) + std::to_string( fixtures[ i ] );
		if( fixtures.size() > 4 ) fx += ",...";
		if( head >= 0 )
			snprintf( b, sizeof( b ), "Fx %s  %s  (Pixel %d)", fx.c_str(), AttrName( attr ), head + 1 );
		else
			snprintf( b, sizeof( b ), "Fx %s  %s", fx.c_str(), AttrName( attr ) );
		break;
	}
	case MidiTarget::DmxChannel: snprintf( b, sizeof( b ), "DMX %d.%03d%s", universe, channelDmx, sixteen ? " (16 bit)" : "" ); break;
	case MidiTarget::Snapshot: snprintf( b, sizeof( b ), "Snapshot %d %s", snapshot + 1, snapshot < (int)s.snapshots.size() ? s.snapshots[ snapshot ].name.c_str() : "" ); break;
	case MidiTarget::Haze: snprintf( b, sizeof( b ), "Haze" ); break;
	case MidiTarget::Master: snprintf( b, sizeof( b ), "Master" ); break;
	case MidiTarget::Param:
	{
		const Param* p = FindParam( const_cast< Show& >( s ), param );
		snprintf( b, sizeof( b ), "%s", p ? p->label.c_str() : ( param + L( " (fehlt)" ) ).c_str() );
		break;
	}
	}
	return b;
}

//---------------------------------------------------------------- mounting
bool Show::GetMountFrame( MountType t, int idx, MountFrame& fr ) const
{
	if( t == MountType::Truss )
	{
		if( idx < 0 || idx >= (int)trusses.size() ) return false;
		const Truss& tr = trusses[ idx ];
		V3 d            = tr.b - tr.a;
		if( Len( d ) < 1e-4f ) return false;
		fr.o = tr.a;
		fr.x = Norm( d );
		fr.z = Norm( Cross( fr.x, fabsf( fr.x.y ) < 0.99f ? V3( 0, 1, 0 ) : V3( 1, 0, 0 ) ) );
		fr.y = Cross( fr.z, fr.x );
		fr.yaw = ( fabsf( fr.x.x ) + fabsf( fr.x.z ) > 1e-4f ) ? atan2f( -fr.x.z, fr.x.x ) / kDeg : 0.0f;
		return true;
	}
	if( t == MountType::Stand )
	{
		if( idx < 0 || idx >= (int)stands.size() ) return false;
		const Stand& s = stands[ idx ];
		const float a  = s.yaw * kDeg;
		fr.o   = s.pos + V3( 0, s.height, 0 );
		fr.x   = V3( cosf( a ), 0, -sinf( a ) );
		fr.y   = V3( 0, 1, 0 );
		fr.z   = Cross( fr.x, fr.y );
		fr.yaw = s.yaw;
		return true;
	}
	return false;
}

void Show::UpdateMounts()
{
	//a lamp on a stand was moved by hand: the stand goes with it (x/z = stand position, y = stand height)
	std::vector< char > standMoved( stands.size(), 0 );
	for( auto& f : fixtures )
	{
		if( f.mount != MountType::Stand || !f.mountInit || f.mountIdx < 0 || f.mountIdx >= (int)stands.size() || standMoved[ f.mountIdx ] ) continue;
		V3 d = f.pos - f.mountLastPos;
		if( Len( d ) <= 1e-5f ) continue;
		Stand& s = stands[ f.mountIdx ];
		s.pos.x += d.x;
		s.pos.z += d.z;
		s.height = Clamp( s.height + d.y, 0.5f, 6.0f );
		standMoved[ f.mountIdx ] = 1;
		dirty = true;
	}
	for( auto& f : fixtures )
	{
		if( f.mount == MountType::Free ) continue;
		MountFrame fr;
		if( !GetMountFrame( f.mount, f.mountIdx, fr ) )
		{
			f.mount    = MountType::Free;
			f.mountIdx = -1;
			continue;
		}
		//the lamp itself was moved (inspector, distribute, load): keep the new place relative to the mount;
		//on a stand the stand has moved along instead, the lamp keeps its place on top
		const bool moved  = Len( f.pos - f.mountLastPos ) > 1e-5f;
		const bool turned = fabsf( f.rot.y - f.mountLastYaw ) > 1e-4f;
		const bool follow = f.mount == MountType::Stand && f.mountInit && standMoved[ f.mountIdx ];
		if( !f.mountInit || ( moved && !follow ) ) f.mountLocal = fr.ToLocal( f.pos );
		if( !f.mountInit || turned ) f.mountYaw = f.rot.y - fr.yaw;
		f.mountInit = true;
		f.pos          = fr.ToWorld( f.mountLocal );
		f.rot.y        = fr.yaw + f.mountYaw;
		f.mountLastPos = f.pos;
		f.mountLastYaw = f.rot.y;
	}
}

void Show::Attach( FixtureInst& f, MountType t, int idx )
{
	MountFrame fr;
	if( t != MountType::Free && !GetMountFrame( t, idx, fr ) ) return;
	f.mount     = t;
	f.mountIdx  = t == MountType::Free ? -1 : idx;
	f.mountInit = false;
	dirty       = true;
}

void Show::HangOnTruss( FixtureInst& f, int idx )
{
	if( idx < 0 || idx >= (int)trusses.size() ) return;
	const Truss& t = trusses[ idx ];
	V3 d           = t.b - t.a;
	float L2       = std::max( 1e-6f, Dot( d, d ) );
	float u        = Clamp( Dot( f.pos - t.a, d ) / L2, 0.0f, 1.0f );
	V3 c           = t.a + d * u;
	if( Len( f.pos - c ) > t.size * 0.5f + 0.6f )
	{
		//far away: hang it under the truss at the closest point
		bool mh = f.def && ( f.def->kind == Kind::MovingHead || f.def->kind == Kind::Scanner );
		f.pos   = c - V3( 0, t.size * 0.5f + 0.02f + ( mh ? 0.0f : 0.05f ), 0 );
		f.rot   = V3( mh ? 180.0f : 150.0f, f.rot.y, 0 );
	}
	Attach( f, MountType::Truss, idx );
}

void Show::PlaceOnStand( const std::vector< FixtureInst* >& fx, int idx )
{
	if( idx < 0 || idx >= (int)stands.size() || fx.empty() ) return;
	Stand& s = stands[ idx ];
	int n    = (int)fx.size();
	if( n > 1 && s.bar < 0.35f * ( n - 1 ) ) s.bar = 0.35f * ( n - 1 );//several lamps: a T-bar wide enough
	MountFrame fr;
	GetMountFrame( MountType::Stand, idx, fr );
	for( int i = 0; i < n; ++i )
	{
		FixtureInst& f = *fx[ i ];
		float x        = n == 1 ? 0.0f : -s.bar * 0.5f + s.bar * i / ( n - 1 );
		bool mh        = f.def && ( f.def->kind == Kind::MovingHead || f.def->kind == Kind::Scanner );
		f.pos          = fr.ToWorld( V3( x, 0, 0 ) );
		f.rot          = V3( mh ? 0.0f : -100.0f, s.yaw, 0 );//spots / PARs: horizontal, slightly down, towards where the stand faces
		Attach( f, MountType::Stand, idx );
	}
}

void Show::RemoveTruss( int i )
{
	if( i < 0 || i >= (int)trusses.size() ) return;
	for( auto& f : fixtures )
		if( f.mount == MountType::Truss )
		{
			if( f.mountIdx == i ) { f.mount = MountType::Free; f.mountIdx = -1; }//stays where it is
			else if( f.mountIdx > i ) --f.mountIdx;
		}
	trusses.erase( trusses.begin() + i );
	dirty = true;
}

//================================================================ moods
const char* const kMoodNames[ 5 ] = { "Halle neutral", "Open Air Nacht", "Blaue Stunde", "Stadtnacht", "Komplett dunkel" };
void ApplyMood( StageCfg& s, int mood )
{
	struct M { int env, ground; V3 ac; float amb; V3 zen, hor; float stars, moon, fog; };
	static const M ms[ 5 ] = {
		{ 0, 0, { 1, 1, 1 }, 0.02f, { 0.003f, 0.005f, 0.018f }, { 0.015f, 0.02f, 0.04f }, 0.7f, 0.5f, 0.0f },
		{ 1, 1, { 0.35f, 0.45f, 0.85f }, 0.03f, { 0.003f, 0.005f, 0.018f }, { 0.015f, 0.02f, 0.04f }, 0.7f, 0.5f, 0.25f },
		{ 1, 1, { 0.4f, 0.5f, 0.9f }, 0.08f, { 0.02f, 0.04f, 0.12f }, { 0.12f, 0.10f, 0.14f }, 0.1f, 0.0f, 0.35f },
		{ 1, 3, { 0.8f, 0.6f, 0.45f }, 0.04f, { 0.01f, 0.01f, 0.02f }, { 0.06f, 0.04f, 0.03f }, 0.05f, 0.0f, 0.5f },
		{ 1, 2, { 1, 1, 1 }, 0.0f, { 0, 0, 0 }, { 0, 0, 0 }, 0.0f, 0.0f, 0.0f },
	};
	const M& m    = ms[ Clamp( mood, 0, 4 ) ];
	s.environment = m.env;
	s.ground      = m.ground;
	s.ambientColor = m.ac;
	s.ambient     = m.amb;
	s.skyZenith   = m.zen;
	s.skyHorizon  = m.hor;
	s.stars       = m.stars;
	s.moon        = m.moon;
	s.distanceFog = m.fog;
	s.envLight    = 1.0f;
	s.backWall    = m.env == 0;
	s.groundSize  = m.env == 0 ? 80.0f : 1000.0f;
}
V3 MoonDir( const StageCfg& s )
{
	float a = s.moonAz * kDeg, e = s.moonEl * kDeg;
	return Norm( V3( sinf( a ) * cosf( e ), sinf( e ), -cosf( a ) * cosf( e ) ) );
}

//================================================================ video projection
uint32_t NewProjectorUid()
{
	static std::atomic< uint32_t > n{ 0 };
	return ++n;
}

float ProjectorAspect( const Projector& p, int w, int h )
{
	static const float a[ 5 ] = { 16.0f / 9.0f, 16.0f / 10.0f, 4.0f / 3.0f, 21.0f / 9.0f, 1.0f };
	if( p.format >= 0 && p.format < 5 ) return a[ p.format ];
	return w > 0 && h > 0 ? (float)w / (float)h : 16.0f / 9.0f;
}

ProjectorGeo GetProjectorGeo( const Projector& p, int w, int h )
{
	ProjectorGeo g;
	M4 R     = RotEuler( V3( p.tilt, p.pan, p.roll ) );
	g.o      = p.pos;
	g.dir    = Norm( XformD( R, V3( 0, 0, -1 ) ) );
	g.up     = Norm( XformD( R, V3( 0, 1, 0 ) ) );
	g.right  = Norm( XformD( R, V3( 1, 0, 0 ) ) );
	g.aspect = ProjectorAspect( p, w, h );
	float tr = Clamp( p.throwRatio, 0.1f, 20.0f );
	g.l      = ( -0.5f + p.shiftX ) / tr;
	g.r      = ( 0.5f + p.shiftX ) / tr;
	g.b      = ( -0.5f + p.shiftY ) / ( tr * g.aspect );
	g.t      = ( 0.5f + p.shiftY ) / ( tr * g.aspect );
	return g;
}

ScreenGeo GetScreenGeo( const Screen& s )
{
	ScreenGeo g;
	M4 R     = Mul( RotY( s.yaw * kDeg ), RotX( -s.tilt * kDeg ) );
	g.right  = XformD( R, V3( 1, 0, 0 ) );
	g.up     = XformD( R, V3( 0, 1, 0 ) );
	g.normal = XformD( R, V3( 0, 0, 1 ) );
	g.centre = s.pos + g.up * ( s.height * 0.5f );
	return g;
}

//---------------------------------------------------------------- screen shape from an image
bool Screen::MaskAt( float u, float v ) const
{
	if( !HasMask() ) return u >= 0 && u <= 1 && v >= 0 && v <= 1;
	if( u < 0 || u > 1 || v < 0 || v > 1 ) return false;
	const int x = std::min( maskN - 1, (int)( u * maskN ) ), y = std::min( maskN - 1, (int)( v * maskN ) );
	return mask[ (size_t)y * maskN + x ] != 0;
}
void SetScreenMask( Screen& s, int n, std::vector< uint8_t > cells )
{
	s.maskRects.clear();
	s.maskN = 0;
	s.mask.clear();
	if( n <= 0 || (int)cells.size() != n * n ) return;
	s.maskN = n;
	s.mask  = std::move( cells );
	//merge: runs per row, a run that continues with the same span in the next row grows downwards
	struct Open { int x0, x1, y0; };
	std::vector< Open > open, next;
	const float inv = 1.0f / n;
	int bx0 = n, by0 = n, bx1 = -1, by1 = -1;
	for( int y = 0; y <= n; ++y )
	{
		next.clear();
		std::vector< std::pair< int, int > > runs;
		if( y < n )
			for( int x = 0; x < n; )
			{
				if( !s.mask[ (size_t)y * n + x ] ) { ++x; continue; }
				int e = x;
				while( e < n && s.mask[ (size_t)y * n + e ] ) ++e;
				runs.push_back( { x, e } );
				bx0 = std::min( bx0, x ); bx1 = std::max( bx1, e ); by0 = std::min( by0, y ); by1 = std::max( by1, y + 1 );
				x = e;
			}
		for( auto& o : open )
		{
			bool cont = false;
			for( auto& r : runs )
				if( r.first == o.x0 && r.second == o.x1 ) { cont = true; break; }
			if( cont ) next.push_back( o );
			else s.maskRects.push_back( { o.x0 * inv, o.y0 * inv, o.x1 * inv, y * inv } );
		}
		for( auto& r : runs )
		{
			bool have = false;
			for( auto& o : next )
				if( o.x0 == r.first && o.x1 == r.second ) { have = true; break; }
			if( !have ) next.push_back( { r.first, r.second, y } );
		}
		open.swap( next );
	}
	if( bx1 > bx0 )
	{
		s.maskBox[ 0 ] = bx0 * inv; s.maskBox[ 1 ] = by0 * inv; s.maskBox[ 2 ] = bx1 * inv; s.maskBox[ 3 ] = by1 * inv;
	}
	else
	{
		s.maskBox[ 0 ] = s.maskBox[ 1 ] = 0; s.maskBox[ 2 ] = s.maskBox[ 3 ] = 0;//all black: nothing to show
	}
}
void ClearScreenMask( Screen& s )
{
	s.maskN = 0;
	s.mask.clear();
	s.maskRects.clear();
	s.maskFile.clear();
	s.maskBox[ 0 ] = s.maskBox[ 1 ] = 0;
	s.maskBox[ 2 ] = s.maskBox[ 3 ] = 1;
}
bool LoadScreenMask( Screen& s, const std::string& file, std::string& err, std::string* warn )
{
	std::vector< uint8_t > data;
	if( !ReadFile( file, data ) || data.empty() ) { err = L( "Datei nicht lesbar: " ) + FileName( file ); return false; }
	int w = 0, h = 0, n = 0;
	stbi_uc* px = stbi_load_from_memory( data.data(), (int)data.size(), &w, &h, &n, 4 );
	if( !px || w < 2 || h < 2 )
	{
		if( px ) stbi_image_free( px );
		err = L( "Kein lesbares Bild (PNG, JPG oder BMP): " ) + FileName( file );
		return false;
	}
	//fit into the square (centred, the rest is black), then average every cell
	const int N    = kScreenMaskN;
	const int side = std::max( w, h ), ox = ( side - w ) / 2, oy = ( side - h ) / 2;
	std::vector< uint8_t > cells( (size_t)N * N, 0 );
	int white = 0;
	for( int cy = 0; cy < N; ++cy )
		for( int cx = 0; cx < N; ++cx )
		{
			//cell area in image pixels (at least one sample)
			const int sx0 = (int)( (double)cx * side / N ) - ox, sx1 = std::max( sx0 + 1, (int)( (double)( cx + 1 ) * side / N ) - ox );
			const int sy0 = (int)( (double)cy * side / N ) - oy, sy1 = std::max( sy0 + 1, (int)( (double)( cy + 1 ) * side / N ) - oy );
			double sum = 0;
			int cnt    = 0;
			for( int y = sy0; y < sy1; ++y )
				for( int x = sx0; x < sx1; ++x )
				{
					++cnt;
					if( x < 0 || y < 0 || x >= w || y >= h ) continue;//padding = black
					const stbi_uc* p = px + ( (size_t)y * w + x ) * 4;
					sum += ( 0.299 * p[ 0 ] + 0.587 * p[ 1 ] + 0.114 * p[ 2 ] ) * p[ 3 ] / ( 255.0 * 255.0 );//transparent = black
				}
			const bool on                 = cnt > 0 && sum / cnt >= 0.5;
			cells[ (size_t)cy * N + cx ] = on ? 1 : 0;
			white += on;
		}
	stbi_image_free( px );
	if( white == 0 ) { err = L( "Das Bild hat keine weißen Flächen - Weiß = Leinwand." ); return false; }
	SetScreenMask( s, N, std::move( cells ) );
	s.maskFile = file;
	if( warn )
	{
		warn->clear();
		if( w != h )
		{
			char b[ 160 ];
			snprintf( b, sizeof( b ), L( "Bild ist nicht 1:1 (%d x %d) - es wurde mittig ins Quadrat eingepasst." ), w, h );
			*warn = b;
		}
	}
	return true;
}
static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string ScreenMaskToString( const Screen& s )
{
	if( !s.HasMask() ) return std::string();
	std::vector< uint8_t > bits( ( s.mask.size() + 7 ) / 8, 0 );
	for( size_t i = 0; i < s.mask.size(); ++i )
		if( s.mask[ i ] ) bits[ i >> 3 ] |= (uint8_t)( 0x80 >> ( i & 7 ) );
	std::string r;
	r.reserve( ( bits.size() + 2 ) / 3 * 4 );
	for( size_t i = 0; i < bits.size(); i += 3 )
	{
		uint32_t v = (uint32_t)bits[ i ] << 16 | ( i + 1 < bits.size() ? (uint32_t)bits[ i + 1 ] << 8 : 0 ) | ( i + 2 < bits.size() ? bits[ i + 2 ] : 0 );
		r += kB64[ ( v >> 18 ) & 63 ];
		r += kB64[ ( v >> 12 ) & 63 ];
		r += i + 1 < bits.size() ? kB64[ ( v >> 6 ) & 63 ] : '=';
		r += i + 2 < bits.size() ? kB64[ v & 63 ] : '=';
	}
	return r;
}
bool ScreenMaskFromString( Screen& s, int n, const std::string& b64 )
{
	if( n <= 0 || n > 1024 || b64.empty() ) return false;
	std::vector< uint8_t > bits;
	uint32_t acc = 0;
	int k        = 0;
	for( char c : b64 )
	{
		const char* p = c ? strchr( kB64, c ) : nullptr;
		if( !p ) continue;//'=' and line breaks
		acc = acc << 6 | (uint32_t)( p - kB64 );
		if( ( k += 6 ) >= 8 )
		{
			k -= 8;
			bits.push_back( (uint8_t)( acc >> k ) );
		}
	}
	const size_t cellsN = (size_t)n * n;
	if( bits.size() * 8 < cellsN ) return false;
	std::vector< uint8_t > cells( cellsN );
	for( size_t i = 0; i < cellsN; ++i ) cells[ i ] = ( bits[ i >> 3 ] >> ( 7 - ( i & 7 ) ) ) & 1;
	SetScreenMask( s, n, std::move( cells ) );
	return s.HasMask();
}

float Show::DeckBase( const Deck& d ) const
{
	return d.onDeck && stage.deck ? stage.deckHeight : std::max( 0.0f, d.y );
}
bool Show::DeckContains( const Deck& d, float x, float z )
{
	const float a = -d.yaw * kDeg, c = cosf( a ), s = sinf( a );
	const float dx = x - d.x, dz = z - d.z;
	const float lx = dx * c + dz * s, lz = -dx * s + dz * c;//into the deck's frame
	const float hw = std::max( 0.05f, d.width ) * 0.5f, hd = std::max( 0.05f, d.depth ) * 0.5f;
	if( d.shape == 1 ) return ( lx * lx ) / ( hw * hw ) + ( lz * lz ) / ( hd * hd ) <= 1.0f;
	return fabsf( lx ) <= hw && fabsf( lz ) <= hd;
}
float Show::FloorAt( float x, float z ) const
{
	float h = 0.0f;
	if( stage.deck && fabsf( x ) <= stage.width * 0.5f && fabsf( z ) <= stage.depth * 0.5f ) h = stage.deckHeight;
	for( auto& d : stage.decks )
		if( DeckContains( d, x, z ) ) h = std::max( h, DeckBase( d ) + std::max( 0.0f, d.height ) );
	return h;
}
bool Show::RayDeckTop( const V3& o, const V3& dir, float& t ) const
{
	if( fabsf( dir.y ) < 1e-5f ) return false;
	float best = 1e30f;
	if( stage.deck && stage.deckHeight > 0.01f )
	{
		const float td = ( stage.deckHeight - o.y ) / dir.y;
		const V3 p     = o + dir * td;
		if( td > 0 && fabsf( p.x ) <= stage.width * 0.5f && fabsf( p.z ) <= stage.depth * 0.5f ) best = td;
	}
	for( auto& d : stage.decks )
	{
		const float top = DeckBase( d ) + std::max( 0.0f, d.height );
		const float td  = ( top - o.y ) / dir.y;
		const V3 p      = o + dir * td;
		if( td > 0 && td < best && DeckContains( d, p.x, p.z ) ) best = td;
	}
	if( best > 1e29f ) return false;
	t = best;
	return true;
}

//distance along the ray to a screen (inside its rectangle), -1 if none
static float HitScreen( const Screen& s, const V3& o, const V3& d )
{
	ScreenGeo g = GetScreenGeo( s );
	float den   = Dot( d, g.normal );
	if( fabsf( den ) < 1e-5f ) return -1;
	float t = Dot( g.centre - o, g.normal ) / den;
	if( t <= 0.05f ) return -1;
	V3 p = o + d * t - g.centre;
	if( s.HasMask() ) return s.MaskAt( Dot( p, g.right ) / std::max( 0.01f, s.width ) + 0.5f, 0.5f - Dot( p, g.up ) / std::max( 0.01f, s.height ) ) ? t : -1;
	return fabsf( Dot( p, g.right ) ) <= s.width * 0.5f + 0.01f && fabsf( Dot( p, g.up ) ) <= s.height * 0.5f + 0.01f ? t : -1;
}

float Show::ThrowDistance( const ProjectorGeo& g ) const
{
	float best = 1e9f;
	for( auto& s : screens )
	{
		float t = HitScreen( s, g.o, g.dir );
		if( t > 0 ) best = std::min( best, t );
	}
	if( g.dir.y < -1e-3f )
	{
		float t = ( 0.0f - g.o.y ) / g.dir.y;
		if( t > 0 ) best = std::min( best, t );
		float td = 0;
		if( RayDeckTop( g.o, g.dir, td ) ) best = std::min( best, td );
	}
	if( stage.backWall && g.dir.z < -1e-3f )
	{
		float t = ( -stage.depth * 0.5f - 0.5f - g.o.z ) / g.dir.z;
		if( t > 0 ) best = std::min( best, t );
	}
	return best > 1e8f ? 12.0f : std::min( best, 60.0f );
}

void Show::AimProjector( Projector& p, const Screen& s, bool frontal, int w, int h ) const
{
	ScreenGeo sg = GetScreenGeo( s );
	p.shiftX = p.shiftY = 0;
	p.roll              = 0;
	V3 dir;
	if( frontal )
	{
		float a  = ProjectorAspect( p, w, h );
		float pw = std::min( s.width, s.height * a );//widest picture that fits the screen
		p.pos    = sg.centre + sg.normal * ( Clamp( p.throwRatio, 0.1f, 20.0f ) * pw );
		dir      = sg.normal * -1.0f;
	}
	else
	{
		V3 v    = sg.centre - p.pos;
		float d = Len( v );
		if( d < 0.1f ) return;
		dir = v / d;
	}
	p.tilt = asinf( Clamp( dir.y, -1.0f, 1.0f ) ) / kDeg;
	p.pan  = atan2f( -dir.x, -dir.z ) / kDeg;
	if( frontal ) return;
	//zoom: the biggest picture whose (keystoned) corners all stay on the screen
	auto fits = [ & ]( float tr ) {
		Projector q  = p;
		q.throwRatio = tr;
		ProjectorGeo g = GetProjectorGeo( q, w, h );
		const float cx[ 4 ] = { g.l, g.r, g.r, g.l }, cy[ 4 ] = { g.b, g.b, g.t, g.t };
		for( int k = 0; k < 4; ++k )
		{
			V3 d    = Norm( g.right * cx[ k ] + g.up * cy[ k ] + g.dir );
			float den = Dot( d, sg.normal );
			if( fabsf( den ) < 1e-5f ) return false;
			float t = Dot( sg.centre - g.o, sg.normal ) / den;
			if( t <= 0 ) return false;
			V3 q2 = g.o + d * t - sg.centre;
			if( fabsf( Dot( q2, sg.right ) ) > s.width * 0.5f + 1e-3f || fabsf( Dot( q2, sg.up ) ) > s.height * 0.5f + 1e-3f ) return false;
		}
		return true;
	};
	float lo = 0.1f, hi = 20.0f;
	if( !fits( hi ) ) return;
	for( int i = 0; i < 40; ++i )
	{
		float m = 0.5f * ( lo + hi );
		if( fits( m ) ) hi = m;
		else lo = m;
	}
	p.throwRatio = hi;
}

Projector& Show::AddProjector()
{
	Projector p;
	p.name = L( "Projektor " ) + std::to_string( projectors.size() + 1 );
	int n  = (int)projectors.size();
	p.pos  = V3( n == 0 ? 0.0f : ( n % 2 ? 1.0f : -1.0f ) * 1.2f * ( ( n + 1 ) / 2 ), 3.2f, stage.depth * 0.5f + 6.0f );
	if( !screens.empty() ) AimProjector( p, screens[ std::min( n, (int)screens.size() - 1 ) ], false );
	else
	{
		//aim at the back wall centre
		V3 c   = V3( 0, std::max( 2.0f, stage.wallHeight * 0.35f ), -stage.depth * 0.5f - 0.5f );
		V3 d   = Norm( c - p.pos );
		p.tilt = asinf( d.y ) / kDeg;
		p.pan  = atan2f( -d.x, -d.z ) / kDeg;
	}
	projectors.push_back( p );
	dirty = true;
	return projectors.back();
}

Screen& Show::AddScreen()
{
	Screen s;
	int n  = (int)screens.size();
	s.name = L( "Leinwand " ) + std::to_string( n + 1 );
	float z = -stage.depth * 0.5f + 0.4f;
	if( n > 0 )
	{
		//side screens, turned a little towards the audience
		float side = n % 2 ? 1.0f : -1.0f;
		s.width    = 3.0f;
		s.height   = 1.6875f;
		s.pos      = V3( side * ( stage.width * 0.5f + 1.0f + 3.5f * ( ( n - 1 ) / 2 ) ), 0, stage.depth * 0.5f - 1.0f );
		s.yaw      = -side * 20.0f;
		s.pos.y    = FloorAt( s.pos.x, s.pos.z ) + 2.2f;
	}
	else
		s.pos = V3( 0, FloorAt( 0, z ) + 0.6f, z );
	screens.push_back( s );
	dirty = true;
	return screens.back();
}

int Show::AddStand()
{
	Stand sd;
	const int n = (int)stands.size();
	for( int k = 0; k < 64; ++k )
	{
		const int j     = n + k;
		const float sgn = j % 2 ? 1.0f : -1.0f;
		sd.pos          = V3( sgn * ( stage.width * 0.5f + 1.2f ), 0, stage.depth * 0.5f - 0.8f - ( j / 2 ) * 1.6f );
		bool freeSpot   = true;
		for( auto& o : stands )
			if( Len( V3( o.pos.x - sd.pos.x, 0, o.pos.z - sd.pos.z ) ) < 0.9f ) freeSpot = false;
		if( freeSpot ) break;
	}
	sd.yaw = atan2f( sd.pos.x, sd.pos.z ) * 57.2957795f;//front (-Z of the lamp) towards the stage centre
	stands.push_back( sd );
	dirty = true;
	return (int)stands.size() - 1;
}

int Show::CloneStand( int i, const V3& off )
{
	if( i < 0 || i >= (int)stands.size() ) return AddStand();
	Stand sd = stands[ i ];
	sd.pos += off;
	stands.push_back( sd );
	dirty = true;
	return (int)stands.size() - 1;
}

void Show::RemoveStand( int i )
{
	if( i < 0 || i >= (int)stands.size() ) return;
	for( auto& f : fixtures )
		if( f.mount == MountType::Stand )
		{
			if( f.mountIdx == i ) { f.mount = MountType::Free; f.mountIdx = -1; }
			else if( f.mountIdx > i ) --f.mountIdx;
		}
	stands.erase( stands.begin() + i );
	dirty = true;
}

void Show::AutoMount( float maxDist )
{
	for( auto& f : fixtures )
	{
		if( f.mount != MountType::Free ) continue;
		int best    = -1;
		float bestD = maxDist;
		for( int i = 0; i < (int)trusses.size(); ++i )
		{
			const Truss& t = trusses[ i ];
			V3 d           = t.b - t.a;
			float u        = Clamp( Dot( f.pos - t.a, d ) / std::max( 1e-6f, Dot( d, d ) ), 0.0f, 1.0f );
			float dist     = Len( f.pos - ( t.a + d * u ) );
			if( dist < bestD ) { bestD = dist; best = i; }
		}
		if( best >= 0 ) Attach( f, MountType::Truss, best );
	}
}

int Show::MountedCount( MountType t, int idx ) const
{
	int n = 0;
	for( auto& f : fixtures )
		if( f.mount == t && f.mountIdx == idx ) ++n;
	return n;
}

void Show::Clear()
{
	std::lock_guard< std::mutex > l( dmxLock );
	universes.clear();
	dmx.clear();
	fixtures.clear();
	trusses.clear();
	stands.clear();
	models.clear();
	projectors.clear();
	screens.clear();
	midi.clear();
	snapshots.clear();
	groups.clear();
	ableton.clear();
	{
		std::random_device rd;
		char b[ 24 ];
		snprintf( b, sizeof( b ), "%08x%08x", (unsigned)rd(), (unsigned)rd() );
		showId = b;
	}
	effects.clear();
	programs.clear();
	{
		const TempoSource src = tempo.source;//the sync source is a setup choice, keep it
		tempo        = Tempo();
		tempo.source = src;
	}
	fxOut.clear();
	fxAny  = false;
	snapOut.clear();
	snapAny = false;
	snapSeq = 0;
	grpSeq.clear();
	progSeq.clear();
	tl = Timeline();
	tcRun = false;
	cues.clear();
	palettes.clear();
	progPalColor.clear();
	progPalPos.clear();
	cueTimecode = false;
	cueCur      = -1;
	cueT        = 0;
	tcPrev      = -1;
	stage  = StageCfg();
	output = OutputCfg();
	path.clear();
	master = 1;
	dirty  = false;
	++dmxVersion;
}

int Show::UniIndex( int num ) const
{
	for( size_t i = 0; i < universes.size(); ++i )
		if( universes[ i ].num == num )
			return (int)i;
	return -1;
}
int Show::AddUniverse()
{
	int n = 1;
	for( auto& u : universes )
		n = std::max( n, u.num + 1 );
	Universe u;
	u.num        = n;
	u.name       = "Universe " + std::to_string( n );
	u.artnetPort = n - 1;
	u.artnetInPort = n - 1;
	std::lock_guard< std::mutex > l( dmxLock );
	universes.push_back( u );
	DmxFrame f{};
	dmx.push_back( f );
	return n;
}
DmxFrame* Show::Frame( int num )
{
	int i = UniIndex( num );
	return i >= 0 ? &dmx[ i ] : nullptr;
}
FixtureInst* Show::FindFixture( int id )
{
	for( auto& f : fixtures )
		if( f.id == id )
			return &f;
	return nullptr;
}
int Show::NextFixtureId() const
{
	int n = 1;
	for( auto& f : fixtures )
		n = std::max( n, f.id + 1 );
	return n;
}
bool Show::Overlaps( int uni, int addr, int fp, int ignoreId ) const
{
	for( auto& f : fixtures )
	{
		if( f.id == ignoreId || f.universe != uni )
			continue;
		int a = f.address, b = f.address + f.Footprint() - 1;
		if( addr <= b && addr + fp - 1 >= a )
			return true;
	}
	return false;
}
int Show::FreeAddress( int uni, int fp, int start, int ignoreId ) const
{
	for( int a = std::max( 1, start ); a + fp - 1 <= 512; ++a )
		if( !Overlaps( uni, a, fp, ignoreId ) )
			return a;
	return 0;
}
FixtureInst* Show::FixtureAt( int uni, int addr, int* slot )
{
	for( auto& f : fixtures )
		if( f.universe == uni && addr >= f.address && addr < f.address + f.Footprint() )
		{
			if( slot ) *slot = addr - f.address;
			return &f;
		}
	return nullptr;
}

uint8_t Show::Get( const FixtureInst& f, int slot ) const
{
	int ui = UniIndex( f.universe );
	int a  = f.address - 1 + slot;
	if( ui < 0 || slot < 0 || a < 0 || a >= 512 ) return 0;
	if( fxAny && ui < (int)fxOut.size() && fxOut[ ui ][ a ] >= 0 ) return (uint8_t)fxOut[ ui ][ a ];
	return dmx[ ui ][ a ];
}
uint8_t Show::GetBase( const FixtureInst& f, int slot ) const
{
	int ui = UniIndex( f.universe );
	int a  = f.address - 1 + slot;
	if( ui < 0 || slot < 0 || a < 0 || a >= 512 ) return 0;
	return dmx[ ui ][ a ];
}
uint8_t Show::GetLayer( const FixtureInst& f, int slot ) const
{
	int ui = UniIndex( f.universe );
	int a  = f.address - 1 + slot;
	if( ui < 0 || slot < 0 || a < 0 || a >= 512 ) return 0;
	if( snapAny && ui < (int)snapOut.size() && snapOut[ ui ][ a ] >= 0 ) return (uint8_t)snapOut[ ui ][ a ];
	return dmx[ ui ][ a ];
}
uint16_t Show::Get16Layer( const FixtureInst& f, int slot ) const
{
	const Mode* m = f.M();
	if( !m || slot < 0 || slot >= (int)m->ch.size() ) return 0;
	const Channel& c = m->ch[ slot ];
	uint8_t hi       = GetLayer( f, slot );
	if( c.fine >= 0 ) return (uint16_t)( ( hi << 8 ) | GetLayer( f, c.fine ) );
	return (uint16_t)( hi * 257 );
}
uint16_t Show::Get16Base( const FixtureInst& f, int slot ) const
{
	const Mode* m = f.M();
	if( !m || slot < 0 || slot >= (int)m->ch.size() ) return 0;
	const Channel& c = m->ch[ slot ];
	uint8_t hi       = GetBase( f, slot );
	if( c.fine >= 0 ) return (uint16_t)( ( hi << 8 ) | GetBase( f, c.fine ) );
	return (uint16_t)( hi * 257 );
}
void Show::FxSet16( const FixtureInst& f, int slot, uint16_t v )
{
	const Mode* m = f.M();
	int ui        = UniIndex( f.universe );
	if( !m || ui < 0 || ui >= (int)fxOut.size() || slot < 0 || slot >= (int)m->ch.size() ) return;
	auto put = [ & ]( int s, int b ) {
		int a = f.address - 1 + s;
		if( a >= 0 && a < 512 ) fxOut[ ui ][ a ] = (int16_t)b;
	};
	put( slot, v >> 8 );
	if( m->ch[ slot ].fine >= 0 ) put( m->ch[ slot ].fine, v & 255 );
}
Effect* Show::FindEffect( int id )
{
	for( auto& e : effects )
		if( e.id == id ) return &e;
	return nullptr;
}
int Show::NextEffectId() const
{
	int n = 1;
	for( auto& e : effects ) n = std::max( n, e.id + 1 );
	return n;
}
void Show::Set( FixtureInst& f, int slot, uint8_t v )
{
	int ui = UniIndex( f.universe );
	int a  = f.address - 1 + slot;
	if( ui < 0 || slot < 0 || a < 0 || a >= 512 ) return;
	std::lock_guard< std::mutex > l( dmxLock );
	dmx[ ui ][ a ] = v;
	if( progSeq.size() != universes.size() ) progSeq.resize( universes.size(), std::array< uint32_t, 512 >{} );
	progSeq[ ui ][ a ] = ++snapSeq;
	Bump();
}
uint16_t Show::Get16( const FixtureInst& f, int slot ) const
{
	const Mode* m = f.M();
	if( !m || slot < 0 || slot >= (int)m->ch.size() ) return 0;
	const Channel& c = m->ch[ slot ];
	uint8_t hi       = Get( f, slot );
	if( c.fine >= 0 ) return (uint16_t)( ( hi << 8 ) | Get( f, c.fine ) );
	return (uint16_t)( hi * 257 );
}
void Show::Set16( FixtureInst& f, int slot, uint16_t v )
{
	const Mode* m = f.M();
	if( !m || slot < 0 || slot >= (int)m->ch.size() ) return;
	const Channel& c = m->ch[ slot ];
	Set( f, slot, (uint8_t)( v >> 8 ) );
	if( c.fine >= 0 ) Set( f, c.fine, (uint8_t)( v & 255 ) );
}

bool Show::HasAttr( const FixtureInst& f, Attr a ) const
{
	const Mode* m = f.M();
	if( !m ) return false;
	for( auto& c : m->ch )
		if( !c.isFine && c.attr == a )
			return true;
	return false;
}
static bool HeadMatch( const Channel& c, int head )
{
	if( head < 0 ) return true;
	if( c.pixels.empty() ) return true;
	return std::find( c.pixels.begin(), c.pixels.end(), head ) != c.pixels.end();
}
float Show::GetAttrBase( const FixtureInst& f, Attr a, int head ) const
{
	const Mode* m = f.M();
	if( !m ) return 0;
	int best = -1;
	for( int i = 0; i < (int)m->ch.size(); ++i )
	{
		const Channel& c = m->ch[ i ];
		if( c.isFine || c.attr != a || !HeadMatch( c, head ) ) continue;
		if( best < 0 || ( head >= 0 && !c.pixels.empty() && m->ch[ best ].pixels.empty() ) ) best = i;
		if( head < 0 && c.pixels.empty() ) { best = i; break; }
	}
	return best >= 0 ? Get16Base( f, best ) / 65535.0f : 0.0f;
}
float Show::GetAttrLayer( const FixtureInst& f, Attr a, int head ) const
{
	const Mode* m = f.M();
	if( !m ) return 0;
	int best = -1;
	for( int i = 0; i < (int)m->ch.size(); ++i )
	{
		const Channel& c = m->ch[ i ];
		if( c.isFine || c.attr != a || !HeadMatch( c, head ) ) continue;
		if( best < 0 || ( head >= 0 && !c.pixels.empty() && m->ch[ best ].pixels.empty() ) ) best = i;
		if( head < 0 && c.pixels.empty() ) { best = i; break; }
	}
	return best >= 0 ? Get16Layer( f, best ) / 65535.0f : 0.0f;
}
void Show::AttrSlots( const FixtureInst& f, Attr a, int head, std::vector< int >& slots ) const
{
	slots.clear();
	const Mode* m = f.M();
	if( !m ) return;
	bool pixelCh = false;
	for( auto& o : m->ch )
		if( !o.isFine && o.attr == a && !o.pixels.empty() ) pixelCh = true;
	for( int i = 0; i < (int)m->ch.size(); ++i )
	{
		const Channel& c = m->ch[ i ];
		if( c.isFine || c.attr != a ) continue;
		if( head >= 0 && !c.pixels.empty() && !HeadMatch( c, head ) ) continue;
		if( head >= 0 && c.pixels.empty() && pixelCh ) continue;
		slots.push_back( i );
	}
}
void Show::FxSetAttr( const FixtureInst& f, Attr a, float v, int head )
{
	static thread_local std::vector< int > sl;
	AttrSlots( f, a, head, sl );
	const uint16_t v16 = (uint16_t)( Saturate( v ) * 65535.0f + 0.5f );
	for( int i : sl ) FxSet16( f, i, v16 );
}
//value range of the "main" function of an attribute channel (e.g. dimmer = whole range, pan = whole range)
float Show::GetAttr( const FixtureInst& f, Attr a, int head ) const
{
	const Mode* m = f.M();
	if( !m ) return 0;
	//prefer a channel that controls exactly this head, then a global one
	int best = -1;
	for( int i = 0; i < (int)m->ch.size(); ++i )
	{
		const Channel& c = m->ch[ i ];
		if( c.isFine || c.attr != a || !HeadMatch( c, head ) ) continue;
		if( best < 0 || ( head >= 0 && !c.pixels.empty() && m->ch[ best ].pixels.empty() ) ) best = i;
		if( head < 0 && c.pixels.empty() ) { best = i; break; }
	}
	return best >= 0 ? Get16( f, best ) / 65535.0f : 0.0f;
}
void Show::SetAttr( FixtureInst& f, Attr a, float v, int head )
{
	const Mode* m = f.M();
	if( !m ) return;
	uint16_t v16 = (uint16_t)( Saturate( v ) * 65535.0f + 0.5f );
	bool any     = false;
	//head given: only channels for that head (pixel channels), else global channels of that attr
	for( int i = 0; i < (int)m->ch.size(); ++i )
	{
		const Channel& c = m->ch[ i ];
		if( c.isFine || c.attr != a ) continue;
		if( head >= 0 && !c.pixels.empty() && !HeadMatch( c, head ) ) continue;
		if( head >= 0 && c.pixels.empty() )
		{
			//only use a global channel when no pixel channel exists for this attr
			bool pixelCh = false;
			for( auto& o : m->ch )
				if( !o.isFine && o.attr == a && !o.pixels.empty() ) pixelCh = true;
			if( pixelCh ) continue;
		}
		Set16( f, i, v16 );
		any = true;
	}
	(void)any;
}
void Show::SetDefaults( FixtureInst& f )
{
	const Mode* m = f.M();
	if( !m ) return;
	for( int i = 0; i < (int)m->ch.size(); ++i )
		if( !m->ch[ i ].isFine )
			Set16( f, i, m->ch[ i ].def16 );
}
void Show::Home( FixtureInst& f )
{
	const Mode* m = f.M();
	if( !m ) return;
	for( int i = 0; i < (int)m->ch.size(); ++i )
	{
		const Channel& c = m->ch[ i ];
		if( c.isFine ) continue;
		uint16_t v = c.def16;
		switch( c.attr )
		{
		case Attr::Pan:
		case Attr::Tilt:
		{
			float mn = 1e9f, mx = -1e9f;
			uint16_t lo = 0, hi = 65535;
			for( auto& cp : c.caps )
				if( cp.k == CapK::Pan || cp.k == CapK::Tilt ) { mn = std::min( { mn, cp.a, cp.b } ); mx = std::max( { mx, cp.a, cp.b } ); lo = cp.lo; hi = cp.hi; break; }
			v = (uint16_t)( ( lo + hi ) / 2 );
			(void)mn; (void)mx;
			break;
		}
		case Attr::Shutter:
			for( auto& cp : c.caps )
				if( cp.k == CapK::ShutterOpen ) { v = (uint16_t)( ( cp.lo + cp.hi ) / 2 ); break; }
			break;
		case Attr::Dimmer: v = 0; break;
		case Attr::Zoom: v = 0; break;
		case Attr::Focus: v = 32768; break;
		default: break;
		}
		Set16( f, i, v );
	}
}

//---------------------------------------------------------------- snapshots
void Show::TakeSnapshot( const std::string& name )
{
	TakeSnapshotOf( name, {}, true, false );
}
int Show::TakeSnapshotOf( const std::string& name, const std::vector< int >& fixtureIds, bool withFx, bool withProj )
{
	Snapshot s;
	s.uid      = NewSnapUid();
	s.name     = name;
	s.withFx   = withFx;
	s.withProj = withProj;
	CaptureSnapshot( s, fixtureIds );
	snapshots.push_back( std::move( s ) );
	dirty = true;
	return (int)snapshots.size() - 1;
}
void Show::CaptureSnapshot( Snapshot& s, const std::vector< int >& fixtureIds )
{
	s.fix.clear();
	s.effects.clear();
	s.proj.clear();
	s.data.clear();
	{
		std::lock_guard< std::mutex > l( dmxLock );
		for( auto& f : fixtures )
		{
			if( !fixtureIds.empty() && std::find( fixtureIds.begin(), fixtureIds.end(), f.id ) == fixtureIds.end() ) continue;
			SnapFixture sf;
			sf.id        = f.id;
			const int fp = f.Footprint();
			sf.v.resize( fp );
			for( int k = 0; k < fp; ++k ) sf.v[ k ] = GetLayer( f, k );
			s.fix.push_back( std::move( sf ) );
		}
	}
	LinkPalettes( s );
	s.fxOff.clear();
	if( s.withFx )
		for( auto& e : effects )
		{
			//only effects on the snapshot's lamps: a snapshot of a part of the rig leaves the other effects alone
			bool touches = s.fix.size() >= fixtures.size();
			for( int id : e.fixtures ) touches = touches || s.Has( id );
			if( !touches ) continue;
			//running on the snapshot's lamps (snapshots may switch it per lamp)
			bool running = false;
			for( int id : e.fixtures )
				if( s.Has( id ) && ( e.snapW >= 0 ? e.DriveOf( id ) > 0.5f : e.on ) ) running = true;
			( running ? s.effects : s.fxOff ).push_back( e.id );
		}
	if( s.withProj )
		for( int i = 0; i < (int)projectors.size(); ++i )
		{
			const Projector p = ProjectorOut( i );
			SnapProjector q;
			q.idx        = i;
			q.on         = p.on;
			q.lumens     = p.lumens;
			q.blackLevel = p.blackLevel;
			q.blur       = p.blur;
			q.pos        = p.pos;
			q.pan        = p.pan;
			q.tilt       = p.tilt;
			q.roll       = p.roll;
			q.throwRatio = p.throwRatio;
			q.shiftX     = p.shiftX;
			q.shiftY     = p.shiftY;
			s.proj.push_back( q );
		}
}
void Show::RecaptureSnapshot( int i, const std::vector< int >* fixtureIds )
{
	if( i < 0 || i >= (int)snapshots.size() ) return;
	Snapshot& s = snapshots[ i ];
	std::vector< int > ids;
	if( fixtureIds ) ids = *fixtureIds;
	else
		for( auto& f : s.fix ) ids.push_back( f.id );
	if( ids.empty() )//nothing left: all lamps
		for( auto& f : fixtures ) ids.push_back( f.id );
	//capture without this snapshot (else it would record itself)
	const bool act = s.active;
	const float cur = s.cur;
	s.active = false;
	s.cur    = 0;
	RunSnapshots( 0 );
	CaptureSnapshot( s, ids );
	s.active = act;
	s.cur    = cur;
	RunSnapshots( 0 );
	dirty = true;
}
void Show::RecallSnapshot( int i, bool keepOthers )
{
	if( i < 0 || i >= (int)snapshots.size() ) return;
	snapshots[ i ].fadeOv = -1;
	snapshots[ i ].keep   = keepOthers;
	snapshots[ i ].active = true;
	snapshots[ i ].seq    = ++snapSeq;//on top
}
uint32_t Show::NewSnapUid() const
{
	uint32_t u = 1;
	for( auto& s : snapshots ) u = std::max( u, s.uid + 1 );
	return u;
}
int Show::FindSnapUid( uint32_t uid ) const
{
	if( !uid ) return -1;
	for( int i = 0; i < (int)snapshots.size(); ++i )
		if( snapshots[ i ].uid == uid ) return i;
	return -1;
}
//---------------------------------------------------------------- palettes
static const Attr kPalAdd[] = { Attr::Red, Attr::Green, Attr::Blue, Attr::White, Attr::WarmWhite, Attr::ColdWhite, Attr::Amber, Attr::UV, Attr::Lime, Attr::Indigo };
uint32_t Show::NewPaletteUid() const
{
	uint32_t u = 1;
	for( auto& p : palettes ) u = std::max( u, p.uid + 1 );
	return u;
}
Palette* Show::FindPalette( uint32_t uid )
{
	for( auto& p : palettes )
		if( p.uid == uid ) return &p;
	return nullptr;
}
bool Show::PaletteFits( const Palette& p, const FixtureInst& f ) const
{
	if( !f.def ) return false;
	if( p.kind == 1 ) return p.PosOf( f.id ) && ( HasAttr( f, Attr::Pan ) || HasAttr( f, Attr::Tilt ) );
	for( Attr a : kPalAdd )
		if( HasAttr( f, a ) ) return true;
	return HasAttr( f, Attr::Cyan );
}
//same channels and values as SetColor / SetAttr( head -1 ) write
bool Show::EncodePalette( const Palette& p, const FixtureInst& f, std::vector< uint8_t >& v ) const
{
	const Mode* m = f.M();
	if( !m || !PaletteFits( p, f ) ) return false;
	bool any = false;
	auto put = [ & ]( Attr a, float x ) {
		const uint16_t v16 = (uint16_t)( Saturate( x ) * 65535.0f + 0.5f );
		for( int i = 0; i < (int)m->ch.size(); ++i )
		{
			const Channel& c = m->ch[ i ];
			if( c.isFine || c.attr != a ) continue;
			if( i < (int)v.size() ) v[ i ] = (uint8_t)( v16 >> 8 );
			if( c.fine >= 0 && c.fine < (int)v.size() ) v[ c.fine ] = (uint8_t)( v16 & 255 );
			any = true;
		}
	};
	if( p.kind == 1 )
	{
		const PalPos* q = p.PosOf( f.id );
		if( HasAttr( f, Attr::Pan ) ) put( Attr::Pan, q->pan );
		if( HasAttr( f, Attr::Tilt ) ) put( Attr::Tilt, q->tilt );
		return any;
	}
	const V3& c = p.color;
	if( f.def->subtractive || ( !HasAttr( f, Attr::Red ) && HasAttr( f, Attr::Cyan ) ) )
	{
		put( Attr::Cyan, 1 - c.x );
		put( Attr::Magenta, 1 - c.y );
		put( Attr::Yellow, 1 - c.z );
	}
	else
	{
		const float w = std::min( { c.x, c.y, c.z } );
		for( Attr a : kPalAdd )
		{
			if( !HasAttr( f, a ) ) continue;
			float t = 0;
			if( a == Attr::Red ) t = c.x;
			else if( a == Attr::Green ) t = c.y;
			else if( a == Attr::Blue ) t = c.z;
			else if( a == Attr::White ) t = w;
			put( a, t );
		}
	}
	for( Attr a : { Attr::ColorWheel, Attr::ColorMacro, Attr::CTO } )
		if( HasAttr( f, a ) ) put( a, 0 );
	return any;
}
int Show::ApplyPalette( uint32_t uid, const std::vector< int >& ids )
{
	const Palette* p = FindPalette( uid );
	if( !p ) return 0;
	int n = 0;
	for( int id : ids )
	{
		FixtureInst* f = FindFixture( id );
		if( !f || !PaletteFits( *p, *f ) ) continue;
		if( p->kind == 1 )
		{
			const PalPos* q = p->PosOf( id );
			if( HasAttr( *f, Attr::Pan ) ) SetAttr( *f, Attr::Pan, q->pan );
			if( HasAttr( *f, Attr::Tilt ) ) SetAttr( *f, Attr::Tilt, q->tilt );
			progPalPos[ id ] = uid;
		}
		else
		{
			SetColor( *f, p->color );
			progPalColor[ id ] = uid;
		}
		++n;
	}
	return n;
}
void Show::LinkPalettes( Snapshot& s )
{
	s.pal.clear();
	std::vector< uint8_t > buf;
	for( const SnapFixture& sf : s.fix )
	{
		const FixtureInst* f = FindFixture( sf.id );
		if( !f ) continue;
		for( int kind = 0; kind < 2; ++kind )
		{
			//the palette last applied to this lamp first, then the others
			uint32_t hit = 0;
			const auto& last = kind ? progPalPos : progPalColor;
			auto it          = last.find( sf.id );
			auto test        = [ & ]( const Palette& p ) {
				if( p.kind != kind ) return false;
				buf = sf.v;
				return EncodePalette( p, *f, buf ) && buf == sf.v;
			};
			if( it != last.end() )
				if( const Palette* p = FindPalette( it->second ) )
					if( test( *p ) ) hit = p->uid;
			for( size_t k = 0; !hit && k < palettes.size(); ++k )
				if( test( palettes[ k ] ) ) hit = palettes[ k ].uid;
			if( hit ) s.pal.push_back( { sf.id, hit } );
		}
	}
}
int Show::UpdatePalette( uint32_t uid, const Palette* before )
{
	const Palette* p = FindPalette( uid );
	if( !p ) return 0;
	int n = 0;
	{
		std::lock_guard< std::mutex > l( dmxLock );
		for( auto& s : snapshots )
		{
			bool ch = false;
			for( auto& lk : s.pal )
			{
				if( lk.second != uid ) continue;
				const FixtureInst* f = FindFixture( lk.first );
				if( !f ) continue;
				for( auto& sf : s.fix )
					if( sf.id == lk.first ) ch |= EncodePalette( *p, *f, sf.v );
			}
			n += ch ? 1 : 0;
		}
	}
	//lamps that show this palette in the programmer follow too
	std::vector< int > ids;
	std::vector< uint8_t > cur, old;
	for( auto& kv : p->kind ? progPalPos : progPalColor )
	{
		if( kv.second != uid ) continue;
		const FixtureInst* f = FindFixture( kv.first );
		if( !f ) continue;
		if( before )
		{
			cur.resize( f->Footprint() );
			for( int k = 0; k < (int)cur.size(); ++k ) cur[ k ] = GetBase( *f, k );
			old = cur;
			EncodePalette( *before, *f, old );
			if( old != cur ) continue;//changed by hand since
		}
		ids.push_back( kv.first );
	}
	ApplyPalette( uid, ids );
	dirty = true;
	++dmxVersion;
	return n;
}
void Show::DeletePalette( uint32_t uid )
{
	for( auto& s : snapshots )
		s.pal.erase( std::remove_if( s.pal.begin(), s.pal.end(), [ & ]( const std::pair< int, uint32_t >& x ) { return x.second == uid; } ), s.pal.end() );
	for( auto* m : { &progPalColor, &progPalPos } )
		for( auto it = m->begin(); it != m->end(); )
			it = it->second == uid ? m->erase( it ) : std::next( it );
	palettes.erase( std::remove_if( palettes.begin(), palettes.end(), [ & ]( const Palette& p ) { return p.uid == uid; } ), palettes.end() );
	dirty = true;
}
//---------------------------------------------------------------- cue list
float Show::CueFadeTime( int i ) const
{
	if( i < 0 || i >= (int)cues.size() ) return 0;
	if( cues[ i ].fade >= 0 ) return cues[ i ].fade;
	const int s = FindSnapUid( cues[ i ].snap );
	return s >= 0 ? snapshots[ s ].fade : 0.0f;
}
void Show::CueGo( int i )
{
	if( i < 0 ) i = cueCur + 1;
	if( i < 0 || i >= (int)cues.size() ) return;
	const Cue& c   = cues[ i ];
	const int si   = FindSnapUid( c.snap );
	const float fd = c.fade;
	if( c.release )
		for( int j = 0; j < (int)cues.size(); ++j )
		{
			const int o = FindSnapUid( cues[ j ].snap );
			if( o < 0 || o == si || !snapshots[ o ].active ) continue;
			snapshots[ o ].fadeOv = fd;
			snapshots[ o ].active = false;
		}
	if( si >= 0 )
	{
		RecallSnapshot( si, !c.release );
		snapshots[ si ].fadeOv = fd;
	}
	cueCur = i;
	cueT   = 0;
	++dmxVersion;
}
void Show::CueBack()
{
	if( cueCur > 0 ) CueGo( cueCur - 1 );
}
void Show::CueStop()
{
	for( auto& c : cues )
	{
		const int o = FindSnapUid( c.snap );
		if( o >= 0 && snapshots[ o ].active )
		{
			snapshots[ o ].fadeOv = -1;
			snapshots[ o ].active = false;
		}
	}
	cueCur = -1;
	cueT   = 0;
	++dmxVersion;
}
void Show::RunCues( double dt )
{
	if( cues.empty() ) return;
	cueT += dt;
	//follow: next cue after this one has faded in + its wait
	const int nx = cueCur + 1;
	if( cueCur >= 0 && nx < (int)cues.size() && cues[ nx ].trigger == 1 && cueT >= CueFadeTime( cueCur ) + cues[ nx ].wait ) CueGo( nx );
	//timecode: the timeline's cue track (it plays or was located) or MIDI timecode
	const TlTrack* ct  = tl.CueTrack();
	const bool tlDrive = ct && !ct->mute && ( tl.rt.playing || tl.rt.located > 0 );
	if( !tlDrive && ( !cueTimecode || tcSec < 0 ) )
	{
		tcPrev = -1;
		return;
	}
	const double tc = tlDrive ? tl.rt.pos : tcSec;
	if( tcPrev < 0 || tc < tcPrev - 0.01 || tc > tcPrev + 1.0 )
	{
		//start / jump: the last timecode cue at or before this time (only if it is not the current one)
		int best = -1;
		for( int i = 0; i < (int)cues.size(); ++i )
			if( cues[ i ].trigger == 2 && cues[ i ].tc <= tc + 1e-6 && ( best < 0 || cues[ i ].tc >= cues[ best ].tc ) ) best = i;
		if( best >= 0 && best != cueCur ) CueGo( best );
	}
	else
	{
		int fire = -1;//cues passed since the last check, the last one wins
		for( int i = 0; i < (int)cues.size(); ++i )
			if( cues[ i ].trigger == 2 && cues[ i ].tc > tcPrev + 1e-6 && cues[ i ].tc <= tc + 1e-6 && ( fire < 0 || cues[ i ].tc >= cues[ fire ].tc ) ) fire = i;
		if( fire >= 0 ) CueGo( fire );
	}
	tcPrev = tc;
}
int Show::SnapshotOverridden( int i )
{
	if( i < 0 || i >= (int)snapshots.size() ) return 0;
	const Snapshot& s = snapshots[ i ];
	if( !s.active && s.cur <= 0 ) return 0;
	int n = 0;
	for( const SnapFixture& sf : s.fix )
	{
		const FixtureInst* f = FindFixture( sf.id );
		const Mode* m        = f ? f->M() : nullptr;
		const int ui         = f ? UniIndex( f->universe ) : -1;
		if( !m || ui < 0 || ui >= (int)grpSeq.size() ) continue;
		for( int k = 0; k < (int)m->ch.size(); ++k )
		{
			const int a = f->address - 1 + k;
			if( a >= 0 && a < 512 && grpSeq[ ui ][ a ] > s.seq ) { ++n; break; }
		}
	}
	return n;
}
void Show::ReleaseSnapshot( int i )
{
	if( i < 0 || i >= (int)snapshots.size() ) return;
	snapshots[ i ].active = false;
}
void Show::ToggleSnapshot( int i, bool keepOthers )
{
	if( i < 0 || i >= (int)snapshots.size() ) return;
	if( snapshots[ i ].active ) ReleaseSnapshot( i );
	else RecallSnapshot( i, keepOthers );
}
void Show::ReleaseAllSnapshots()
{
	for( auto& s : snapshots ) s.active = false;
}
int Show::ActiveSnapshots() const
{
	int n = 0;
	for( auto& s : snapshots ) n += s.active ? 1 : 0;
	return n;
}
void Show::LoadSnapshotToBase( int i )
{
	if( i < 0 || i >= (int)snapshots.size() ) return;
	std::lock_guard< std::mutex > l( dmxLock );
	for( auto& sf : snapshots[ i ].fix )
	{
		const FixtureInst* f = FindFixture( sf.id );
		if( !f ) continue;
		const int ui = UniIndex( f->universe );
		if( ui < 0 ) continue;
		const int n = std::min( f->Footprint(), (int)sf.v.size() );
		for( int k = 0; k < n; ++k )
		{
			const int a = f->address - 1 + k;
			if( a >= 0 && a < 512 ) dmx[ ui ][ a ] = sf.v[ k ];
		}
	}
	Bump();
}
//attributes that can be crossfaded; the others (wheels, gobos, shutter, macros) switch at once
bool SnapFadeable( Attr a )
{
	switch( a )
	{
	case Attr::Dimmer:
	case Attr::Red:
	case Attr::Green:
	case Attr::Blue:
	case Attr::White:
	case Attr::WarmWhite:
	case Attr::ColdWhite:
	case Attr::Amber:
	case Attr::UV:
	case Attr::Lime:
	case Attr::Indigo:
	case Attr::Cyan:
	case Attr::Magenta:
	case Attr::Yellow:
	case Attr::CTO:
	case Attr::Pan:
	case Attr::Tilt:
	case Attr::Zoom:
	case Attr::Focus:
	case Attr::Frost:
	case Attr::Iris:
	case Attr::Fog: return true;
	default: return false;
	}
}
bool Show::RunSnapshots( double dt )
{
	//fades; a snapshot that has fully faded in replaces older ones whose lamps (and projectors) it covers completely
	static thread_local std::vector< int > full, order;
	full.clear();
	order.clear();
	for( int i = 0; i < (int)snapshots.size(); ++i )
	{
		Snapshot& s        = snapshots[ i ];
		const float target = s.active ? 1.0f : 0.0f;
		const float fd     = s.fadeOv >= 0 ? s.fadeOv : s.fade;
		const float step   = fd > 0.001f ? (float)( dt / fd ) : 1.0f;
		const bool wasFull = s.cur >= 1.0f;
		if( dt > 0 || fd <= 0.001f ) s.cur = s.cur < target ? std::min( target, s.cur + step ) : std::max( target, s.cur - step );
		if( !s.active && s.cur <= 0 ) s.fadeOv = -1;
		if( s.active && s.cur >= 1.0f && !wasFull ) full.push_back( i );
	}
	for( int i : full )
	{
		const Snapshot& top = snapshots[ i ];
		if( top.keep ) continue;//started with Shift: the others stay on
		for( int j = 0; j < (int)snapshots.size(); ++j )
		{
			Snapshot& o = snapshots[ j ];
			if( j == i || !o.active || o.seq >= top.seq || ( o.fix.empty() && o.proj.empty() ) ) continue;
			bool covered = true;
			for( auto& f : o.fix )
				if( !top.Has( f.id ) ) { covered = false; break; }
			for( auto& p : o.proj )
			{
				bool has = false;
				for( auto& q : top.proj ) has = has || q.idx == p.idx;
				if( !has ) { covered = false; break; }
			}
			if( covered ) o.active = false;
		}
	}
	for( int i = 0; i < (int)snapshots.size(); ++i )
		if( snapshots[ i ].cur > 0 ) order.push_back( i );
	std::sort( order.begin(), order.end(), [ & ]( int a, int b ) { return snapshots[ a ].seq < snapshots[ b ].seq; } );
	//effects held by snapshots
	//effects switched by snapshots (on and off), in start order: the later one wins, faded with its level
	//per lamp: a snapshot switches an effect only on its own lamps
	for( auto& e : effects )
	{
		e.snapW = -1;
		e.snapI = 1;
		e.fxD.assign( e.fixtures.size(), -1.0f );
		e.fxI.assign( e.fixtures.size(), 1.0f );
	}
	bool touched = false;
	for( int i : order )
	{
		const Snapshot& sn = snapshots[ i ];
		for( int pass = 0; pass < 2; ++pass )
			for( int id : pass ? sn.fxOff : sn.effects )
				if( Effect* e = FindEffect( id ) )
					for( size_t k = 0; k < e->fixtures.size(); ++k )
					{
						if( !sn.fix.empty() && !sn.Has( e->fixtures[ k ] ) ) continue;
						const float d = e->fxD[ k ] >= 0 ? e->fxD[ k ] : e->cur;
						e->fxD[ k ]   = Lerp( d, pass ? 0.0f : 1.0f, sn.cur );
						if( !pass ) e->fxI[ k ] = sn.master;
						e->snapW = 0;//marks "touched", the maximum follows
						touched  = true;
					}
	}
	if( touched )
		for( auto& e : effects )
		{
			if( e.snapW < 0 ) continue;
			float m = 0;
			for( size_t k = 0; k < e.fixtures.size(); ++k ) m = std::max( m, e.fxD[ k ] >= 0 ? e.fxD[ k ] : e.cur );
			e.snapW = m;
		}
	//DMX layer
	std::lock_guard< std::mutex > l( dmxLock );
	if( snapOut.size() != universes.size() ) snapOut.resize( universes.size() );
	bool any = false;
	for( int i : order )
		if( !snapshots[ i ].fix.empty() ) any = true;
	if( !any )
	{
		if( !snapAny ) return false;
		for( auto& r : snapOut ) r.fill( -1 );
		snapAny = false;
		return true;
	}
	for( auto& r : snapOut ) r.fill( -1 );
	for( int i : order )
	{
		const Snapshot& s = snapshots[ i ];
		const float w     = s.cur;
		for( const SnapFixture& sf : s.fix )
		{
			const FixtureInst* f = FindFixture( sf.id );
			const Mode* m        = f ? f->M() : nullptr;
			const int ui         = f ? UniIndex( f->universe ) : -1;
			if( !m || ui < 0 ) continue;
			const int n       = std::min( (int)m->ch.size(), (int)sf.v.size() );
			const bool hasDim = HasAttr( *f, Attr::Dimmer );
			auto& L           = snapOut[ ui ];
			const auto& D     = dmx[ ui ];
			auto rd = [ & ]( int k ) -> int {
				const int a = f->address - 1 + k;
				if( a < 0 || a >= 512 ) return 0;
				return L[ a ] >= 0 ? L[ a ] : D[ a ];
			};
			auto wr = [ & ]( int k, int b ) {
				const int a = f->address - 1 + k;
				if( a >= 0 && a < 512 ) L[ a ] = (int16_t)Clamp( b, 0, 255 );
			};
			const uint32_t* G = ui < (int)grpSeq.size() ? grpSeq[ ui ].data() : nullptr;
			for( int k = 0; k < n; ++k )
			{
				const Channel& c = m->ch[ k ];
				if( c.isFine ) continue;
				//moved by a group fader after this snapshot started: the group fader's value stays
				if( G && f->address - 1 + k >= 0 && f->address - 1 + k < 512 && G[ f->address - 1 + k ] > s.seq ) continue;
				const bool fine  = c.fine >= 0 && c.fine < n;
				const float under = fine ? (float)( ( rd( k ) << 8 ) | rd( c.fine ) ) : rd( k ) * 257.0f;
				const float tgt   = fine ? (float)( ( sf.v[ k ] << 8 ) | sf.v[ c.fine ] ) : sf.v[ k ] * 257.0f;
				float out;
				if( c.attr == Attr::Dimmer || ( !hasDim && IsEmitter( c.attr ) ) ) out = Lerp( under, tgt * Saturate( s.master ), w );
				else if( SnapFadeable( c.attr ) ) out = Lerp( under, tgt, w );
				else out = w > 0 ? tgt : under;
				const int o16 = Clamp( (int)lroundf( out ), 0, 65535 );
				if( fine )
				{
					wr( k, o16 >> 8 );
					wr( c.fine, o16 & 255 );
				}
				else
					wr( k, ( o16 + 128 ) / 257 );
			}
		}
	}
	snapAny = true;
	return true;
}
Projector Show::ProjectorOut( int i ) const
{
	Projector p = projectors[ i ];
	const Snapshot* list[ 64 ];
	int n = 0;
	for( auto& s : snapshots )
		if( s.cur > 0 && n < 64 )
			for( auto& q : s.proj )
				if( q.idx == i ) { list[ n++ ] = &s; break; }
	if( !n ) return p;
	std::sort( list, list + n, []( const Snapshot* a, const Snapshot* b ) { return a->seq < b->seq; } );
	float I = p.on ? p.lumens : 0.0f;
	for( int k = 0; k < n; ++k )
	{
		const Snapshot& s = *list[ k ];
		const float w     = s.cur;
		for( auto& q : s.proj )
		{
			if( q.idx != i ) continue;
			I            = Lerp( I, ( q.on ? q.lumens : 0.0f ) * Saturate( s.master ), w );
			p.pos        = p.pos + ( q.pos - p.pos ) * w;
			p.pan        = Lerp( p.pan, q.pan, w );
			p.tilt       = Lerp( p.tilt, q.tilt, w );
			p.roll       = Lerp( p.roll, q.roll, w );
			p.throwRatio = Lerp( p.throwRatio, q.throwRatio, w );
			p.shiftX     = Lerp( p.shiftX, q.shiftX, w );
			p.shiftY     = Lerp( p.shiftY, q.shiftY, w );
			p.blackLevel = Lerp( p.blackLevel, q.blackLevel, w );
			p.blur       = Lerp( p.blur, q.blur, w );
			break;
		}
	}
	p.on     = I > 0.5f;
	p.lumens = std::max( I, 0.0f );
	return p;
}

//---------------------------------------------------------------- group faders
void Show::SetColor( FixtureInst& f, const V3& c, int head )
{
	if( !f.def ) return;
	if( f.def->subtractive || ( !HasAttr( f, Attr::Red ) && HasAttr( f, Attr::Cyan ) ) )
	{
		SetAttr( f, Attr::Cyan, 1 - c.x, head );
		SetAttr( f, Attr::Magenta, 1 - c.y, head );
		SetAttr( f, Attr::Yellow, 1 - c.z, head );
	}
	else
	{
		const float w = std::min( { c.x, c.y, c.z } );
		const Attr em[] = { Attr::Red, Attr::Green, Attr::Blue, Attr::White, Attr::WarmWhite, Attr::ColdWhite, Attr::Amber, Attr::UV, Attr::Lime, Attr::Indigo };
		for( Attr a : em )
		{
			if( !HasAttr( f, a ) ) continue;
			float t = 0;
			if( a == Attr::Red ) t = c.x;
			else if( a == Attr::Green ) t = c.y;
			else if( a == Attr::Blue ) t = c.z;
			else if( a == Attr::White ) t = w;
			SetAttr( f, a, t, head );
		}
	}
	for( Attr a : { Attr::ColorWheel, Attr::ColorMacro, Attr::CTO } )
		if( HasAttr( f, a ) ) SetAttr( f, a, 0, head );
}
void Show::ApplyGroup( int i )
{
	if( i < 0 || i >= (int)groups.size() ) return;
	const GroupFader& g = groups[ i ];
	if( g.Submaster() )//scales the output in RunEffects, writes nothing
	{
		++dmxVersion;
		return;
	}
	for( int id : g.fixtures )
		if( FixtureInst* f = FindFixture( id ) )
		{
			if( g.kind == 1 ) SetColor( *f, Hsv( g.hue, g.sat, g.value ) );
			else if( HasAttr( *f, g.attr ) ) SetAttr( *f, g.attr, g.value );
		}
	//the channels just written win over the snapshots running now (latest action wins)
	static const Attr colorAttrs[] = { Attr::Cyan, Attr::Magenta, Attr::Yellow, Attr::Red, Attr::Green, Attr::Blue, Attr::White, Attr::WarmWhite, Attr::ColdWhite, Attr::Amber, Attr::UV, Attr::Lime, Attr::Indigo, Attr::ColorWheel, Attr::ColorMacro, Attr::CTO };
	std::vector< int > sl;
	std::lock_guard< std::mutex > l( dmxLock );
	if( grpSeq.size() != universes.size() ) grpSeq.resize( universes.size(), std::array< uint32_t, 512 >{} );
	const uint32_t stamp = ++snapSeq;
	for( int id : g.fixtures )
	{
		const FixtureInst* f = FindFixture( id );
		const int ui         = f ? UniIndex( f->universe ) : -1;
		if( ui < 0 || ui >= (int)grpSeq.size() ) continue;
		auto mark = [ & ]( Attr a ) {
			AttrSlots( *f, a, -1, sl );
			for( int k : sl )
			{
				const int adr = f->address - 1 + k;
				if( adr >= 0 && adr < 512 ) grpSeq[ ui ][ adr ] = stamp;
			}
		};
		if( g.kind == 1 )
			for( Attr a : colorAttrs ) mark( a );
		else
			mark( g.attr );
	}
}

//---------------------------------------------------------------- save / load
static Json V( const V3& v )
{
	Json a = Json::Arr();
	a.Push( v.x );
	a.Push( v.y );
	a.Push( v.z );
	return a;
}
static V3 V( const Json& j, V3 def )
{
	if( j.Size() != 3 ) return def;
	return V3( j[ 0 ].F(), j[ 1 ].F(), j[ 2 ].F() );
}
static std::string Hex( const DmxFrame& f )
{
	static const char* h = "0123456789abcdef";
	std::string s;
	s.reserve( 1024 );
	for( uint8_t b : f ) { s += h[ b >> 4 ]; s += h[ b & 15 ]; }
	return s;
}
static std::string HexV( const std::vector< uint8_t >& v )
{
	static const char* h = "0123456789abcdef";
	std::string s;
	s.reserve( v.size() * 2 );
	for( uint8_t b : v ) { s += h[ b >> 4 ]; s += h[ b & 15 ]; }
	return s;
}
static void UnhexV( const std::string& s, std::vector< uint8_t >& v )
{
	auto nib = []( char c ) { return c >= 'a' ? c - 'a' + 10 : ( c >= 'A' ? c - 'A' + 10 : c - '0' ); };
	v.clear();
	for( size_t i = 0; i + 1 < s.size() && v.size() < 512; i += 2 ) v.push_back( (uint8_t)( nib( s[ i ] ) * 16 + nib( s[ i + 1 ] ) ) );
}
static void Unhex( const std::string& s, DmxFrame& f )
{
	f.fill( 0 );
	auto nib = []( char c ) { return c >= 'a' ? c - 'a' + 10 : ( c >= 'A' ? c - 'A' + 10 : c - '0' ); };
	for( size_t i = 0; i + 1 < s.size() && i / 2 < 512; i += 2 )
		f[ i / 2 ] = (uint8_t)( nib( s[ i ] ) * 16 + nib( s[ i + 1 ] ) );
}

Json Show::ToJson( bool undo )
{
	Json j = Json::Obj();
	j.Set( "format", "grosserd-lightstage" );
	j.Set( "version", 1 );
	Json us = Json::Arr();
	{
		std::lock_guard< std::mutex > l( dmxLock );
		for( size_t i = 0; i < universes.size(); ++i )
		{
			auto& u = universes[ i ];
			Json o  = Json::Obj();
			o.Set( "num", u.num );
			o.Set( "name", u.name );
			o.Set( "artnetOut", u.artnetOut );
			o.Set( "artnetPort", u.artnetPort );
			o.Set( "artnetIn", u.artnetIn );
			o.Set( "artnetInPort", u.artnetInPort );
			if( !undo ) o.Set( "dmx", Hex( dmx[ i ] ) );
			us.Push( o );
		}
	}
	j.Set( "universes", us );
	Json fx = Json::Arr();
	for( auto& f : fixtures )
	{
		Json o = Json::Obj();
		o.Set( "id", f.id );
		o.Set( "name", f.name );
		o.Set( "def", f.defId );
		o.Set( "mode", f.def && f.mode < (int)f.def->modes.size() ? f.def->modes[ f.mode ].name : std::to_string( f.mode ) );
		o.Set( "universe", f.universe );
		o.Set( "address", f.address );
		o.Set( "pos", V( f.pos ) );
		o.Set( "rot", V( f.rot ) );
		o.Set( "invertPan", f.invertPan );
		o.Set( "invertTilt", f.invertTilt );
		o.Set( "mount", (int)f.mount );
		o.Set( "mountIdx", f.mountIdx );
		if( f.beam > 0 ) o.Set( "beam", f.beam );
		fx.Push( o );
	}
	j.Set( "fixtures", fx );
	//own fixtures (fixture editor) travel with the show
	if( !undo )
	{
		Json uf = Json::Obj();
		for( auto& f : fixtures )
			if( f.def && f.defId.rfind( "user/", 0 ) == 0 && !f.def->source.empty() && !uf.Find( f.defId ) )
			{
				std::string t;
				Json fj;
				if( ReadText( f.def->source, t ) && ParseJson( t, fj ) ) uf.Set( f.defId, fj );
			}
		if( !uf.o.empty() ) j.Set( "userFixtures", uf );
	}
	Json tr = Json::Arr();
	for( auto& t : trusses )
	{
		Json o = Json::Obj();
		o.Set( "a", V( t.a ) );
		o.Set( "b", V( t.b ) );
		o.Set( "size", t.size );
		if( t.freeEnds ) o.Set( "free", true );
		tr.Push( o );
	}
	j.Set( "trusses", tr );
	Json sd = Json::Arr();
	for( auto& s : stands )
	{
		Json o = Json::Obj();
		o.Set( "pos", V( s.pos ) );
		o.Set( "height", s.height );
		o.Set( "yaw", s.yaw );
		o.Set( "bar", s.bar );
		sd.Push( o );
	}
	j.Set( "stands", sd );
	Json md = Json::Arr();
	for( auto& m : models )
	{
		Json o = Json::Obj();
		o.Set( "path", m.path );
		o.Set( "name", m.name );
		o.Set( "pos", V( m.pos ) );
		o.Set( "rot", V( m.rot ) );
		o.Set( "scale", m.scale );
		o.Set( "tint", V( m.tint ) );
		o.Set( "matColors", m.matColors );
		md.Push( o );
	}
	j.Set( "models", md );
	Json pj = Json::Arr();
	for( auto& p : projectors )
	{
		Json o = Json::Obj();
		o.Set( "name", p.name );
		o.Set( "on", p.on );
		o.Set( "pos", V( p.pos ) );
		o.Set( "pan", p.pan );
		o.Set( "tilt", p.tilt );
		o.Set( "roll", p.roll );
		o.Set( "format", (float)p.format );
		o.Set( "throwRatio", p.throwRatio );
		o.Set( "shiftX", p.shiftX );
		o.Set( "shiftY", p.shiftY );
		o.Set( "lumens", p.lumens );
		o.Set( "blackLevel", p.blackLevel );
		o.Set( "blur", p.blur );
		o.Set( "rear", p.rear );
		o.Set( "frustum", p.frustum );
		o.Set( "input", (float)(int)p.input );
		o.Set( "file", p.file );
		o.Set( "ndi", p.ndi );
		o.Set( "loop", p.loop );
		pj.Push( o );
	}
	j.Set( "projectors", pj );
	Json sc = Json::Arr();
	for( auto& s : screens )
	{
		Json o = Json::Obj();
		o.Set( "name", s.name );
		o.Set( "pos", V( s.pos ) );
		o.Set( "yaw", s.yaw );
		o.Set( "tilt", s.tilt );
		o.Set( "width", s.width );
		o.Set( "height", s.height );
		o.Set( "gain", s.gain );
		o.Set( "format", s.format );
		o.Set( "frame", s.frame );
		o.Set( "mount", (float)s.mount );
		if( s.HasMask() )
		{
			o.Set( "maskN", s.maskN );
			o.Set( "mask", ScreenMaskToString( s ) );
			o.Set( "maskFile", s.maskFile );
		}
		sc.Push( o );
	}
	j.Set( "screens", sc );
	Json st = Json::Obj();
	st.Set( "width", stage.width );
	st.Set( "depth", stage.depth );
	st.Set( "deckHeight", stage.deckHeight );
	st.Set( "deck", stage.deck );
	st.Set( "backWall", stage.backWall );
	st.Set( "floor", stage.floor );
	st.Set( "wallHeight", stage.wallHeight );
	st.Set( "haze", stage.haze );
	st.Set( "hazeNoise", stage.hazeNoise );
	st.Set( "ambient", stage.ambient );
	st.Set( "envLight", stage.envLight );
	st.Set( "environment", (float)stage.environment );
	st.Set( "ground", (float)stage.ground );
	st.Set( "groundSize", stage.groundSize );
	st.Set( "skyZenith", V( stage.skyZenith ) );
	st.Set( "skyHorizon", V( stage.skyHorizon ) );
	st.Set( "stars", stage.stars );
	st.Set( "moon", stage.moon );
	st.Set( "moonAz", stage.moonAz );
	st.Set( "moonEl", stage.moonEl );
	st.Set( "moonShadow", stage.moonShadow );
	st.Set( "distanceFog", stage.distanceFog );
	st.Set( "ambientColor", V( stage.ambientColor ) );
	st.Set( "exposure", stage.exposure );
	st.Set( "bloom", stage.bloom );
	st.Set( "beamQuality", stage.beamQuality );
	st.Set( "shadows", stage.shadows );
	st.Set( "beams", stage.beams );
	st.Set( "beamLevel", stage.beamLevel );
	st.Set( "floorColor", V( stage.floorColor ) );
	st.Set( "wallColor", V( stage.wallColor ) );
	if( !stage.decks.empty() )
	{
		Json ds = Json::Arr();
		for( auto& d : stage.decks )
		{
			Json o = Json::Obj();
			o.Set( "name", d.name );
			o.Set( "shape", d.shape );
			o.Set( "x", d.x );
			o.Set( "y", d.y );
			o.Set( "z", d.z );
			o.Set( "width", d.width );
			o.Set( "depth", d.depth );
			o.Set( "height", d.height );
			o.Set( "yaw", d.yaw );
			o.Set( "onDeck", d.onDeck );
			o.Set( "ownColor", d.ownColor );
			o.Set( "color", V( d.color ) );
			o.Set( "skirt", d.skirt );
			o.Set( "stairs", d.stairs );
			o.Set( "edge", V( d.edge ) );
			o.Set( "edgeLevel", d.edgeLevel );
			ds.Push( o );
		}
		st.Set( "decks", ds );
	}
	j.Set( "stage", st );
	Json mi = Json::Arr();
	for( auto& m : midi ) mi.Push( MidiMapToJson( m ) );
	j.Set( "midi", mi );
	{
		Json ef = Json::Arr();
		for( auto& e : effects )
		{
			Json o = EffectToJson( e );
			if( undo ) o.Set( "on", false );//running or not is live state
			ef.Push( o );
		}
		j.Set( "effects", ef );
		Json pr = Json::Arr();
		for( auto& p : programs )
		{
			Json o = Json::Obj();
			o.Set( "name", p.name );
			Json ids = Json::Arr();
			for( int id : p.effects ) ids.Push( id );
			o.Set( "effects", ids );
			pr.Push( o );
		}
		j.Set( "programs", pr );
		if( !undo )
		{
		Json t = Json::Obj();
		t.Set( "bpm", tempo.bpm );
		t.Set( "beatsPerBar", tempo.beatsPerBar );
		t.Set( "source", (int)tempo.source );
		j.Set( "tempo", t );
		}
	}
	Json sn = Json::Arr();
	for( auto& s : snapshots )
	{
		Json o = Json::Obj();
		o.Set( "uid", (double)s.uid );
		o.Set( "name", s.name );
		o.Set( "master", s.master );
		o.Set( "fade", s.fade );
		if( !undo ) o.Set( "active", s.active );
		o.Set( "withFx", s.withFx );
		o.Set( "withProj", s.withProj );
		Json fa = Json::Arr();
		for( auto& f : s.fix )
		{
			Json x = Json::Obj();
			x.Set( "id", f.id );
			x.Set( "v", HexV( f.v ) );
			fa.Push( x );
		}
		o.Set( "fixtures", fa );
		if( !s.pal.empty() )
		{
			Json pl = Json::Arr();
			for( auto& lk : s.pal )
			{
				Json x = Json::Arr();
				x.Push( lk.first );
				x.Push( (double)lk.second );
				pl.Push( x );
			}
			o.Set( "palettes", pl );
		}
		Json ea = Json::Arr();
		for( int id : s.effects ) ea.Push( id );
		o.Set( "effects", ea );
		Json eo = Json::Arr();
		for( int id : s.fxOff ) eo.Push( id );
		o.Set( "effectsOff", eo );
		Json pa = Json::Arr();
		for( auto& q : s.proj )
		{
			Json x = Json::Obj();
			x.Set( "idx", q.idx );
			x.Set( "on", q.on );
			x.Set( "lumens", q.lumens );
			x.Set( "blackLevel", q.blackLevel );
			x.Set( "blur", q.blur );
			x.Set( "pos", V( q.pos ) );
			x.Set( "pan", q.pan );
			x.Set( "tilt", q.tilt );
			x.Set( "roll", q.roll );
			x.Set( "throwRatio", q.throwRatio );
			x.Set( "shiftX", q.shiftX );
			x.Set( "shiftY", q.shiftY );
			pa.Push( x );
		}
		o.Set( "projectors", pa );
		sn.Push( o );
	}
	j.Set( "snapshots", sn );
	{
		Json pa = Json::Arr();
		for( auto& p : palettes )
		{
			Json o = Json::Obj();
			o.Set( "uid", (double)p.uid );
			o.Set( "name", p.name );
			o.Set( "kind", p.kind );
			o.Set( "color", V( p.color ) );
			Json ps = Json::Arr();
			for( auto& q : p.pos )
			{
				Json x = Json::Arr();
				x.Push( q.id );
				x.Push( q.pan );
				x.Push( q.tilt );
				ps.Push( x );
			}
			o.Set( "pos", ps );
			pa.Push( o );
		}
		j.Set( "palettes", pa );
	}
	{
		Json cl = Json::Arr();
		for( auto& c : cues )
		{
			Json o = Json::Obj();
			o.Set( "name", c.name );
			o.Set( "snap", (double)c.snap );
			o.Set( "fade", c.fade );
			o.Set( "trigger", c.trigger );
			o.Set( "wait", c.wait );
			o.Set( "tc", c.tc );
			o.Set( "release", c.release );
			cl.Push( o );
		}
		j.Set( "cues", cl );
		j.Set( "cueTimecode", cueTimecode );
	}
	j.Set( "timeline", TimelineToJson( tl ) );
	Json gr = Json::Arr();
	for( auto& g : groups )
	{
		Json o = Json::Obj();
		o.Set( "name", g.name );
		Json ids = Json::Arr();
		for( int id : g.fixtures ) ids.Push( id );
		o.Set( "fixtures", ids );
		o.Set( "kind", g.kind );
		o.Set( "attr", AttrName( g.attr ) );
		if( g.mode ) o.Set( "mode", g.mode );
		if( !undo )//fader positions are live state
		{
			o.Set( "value", g.value );
			o.Set( "hue", g.hue );
			o.Set( "sat", g.sat );
		}
		gr.Push( o );
	}
	j.Set( "groups", gr );
	if( !undo ) j.Set( "showId", showId );
	Json ab = Json::Arr();
	for( auto& m : ableton )
	{
		Json o = Json::Obj();
		o.Set( "id", m.id );
		o.Set( "label", m.label );
		o.Set( "kind", m.kind );
		if( !undo ) o.Set( "state", m.state );
		o.Set( "knobsNewer", m.knobsNewer );
		Json k = Json::Arr(), b = Json::Arr();
		for( auto& x : m.knobs ) k.Push( Json( x ) );
		for( auto& x : m.buttons ) b.Push( Json( x ) );
		o.Set( "knobs", k );
		o.Set( "buttons", b );
		ab.Push( o );
	}
	j.Set( "ableton", ab );
	Json out = Json::Obj();
	out.Set( "artnet", output.artnet );
	out.Set( "artnetTarget", output.artnetTarget );
	out.Set( "artnetBind", output.artnetBind );
	out.Set( "artnetInput", output.artnetInput );
	out.Set( "enttec", output.enttec );
	out.Set( "enttecPort", output.enttecPort );
	out.Set( "enttecUniverse", output.enttecUniverse );
	out.Set( "oscIn", output.oscIn );
	out.Set( "oscPort", output.oscPort );
	out.Set( "rate", output.rate );
	out.Set( "sacn", output.sacn );
	out.Set( "sacnTarget", output.sacnTarget );
	out.Set( "sacnPriority", output.sacnPriority );
	j.Set( "output", out );
	if( !undo )
	{
		j.Set( "master", master );
		j.Set( "fxMaster", fxMaster );
		j.Set( "fxSpeed", fxSpeed );
	}
	return j;
}

bool Show::Save( const std::string& file, std::string* err )
{
	if( !WriteText( file, WriteJson( ToJson(), true ) ) )
	{
		if( err ) *err = "cannot write " + file;
		return false;
	}
	path  = file;
	dirty = false;
	return true;
}

bool Show::Load( const std::string& file, const Library& lib, std::string* err )
{
	std::string txt;
	if( !ReadText( file, txt ) )
	{
		if( err ) *err = "cannot read " + file;
		return false;
	}
	Json j;
	if( !ParseJson( txt, j, err ) ) return false;
	return FromJson( j, lib, file, err );
}

bool Show::FromJson( const Json& j, const Library& lib, const std::string& file, std::string* err )
{
	if( j[ "format" ].Str() != "grosserd-lightstage" )
	{
		if( err ) *err = "not a LightStage show";
		return false;
	}
	Clear();
	{
		std::lock_guard< std::mutex > l( dmxLock );
		for( auto& o : j[ "universes" ].a )
		{
			Universe u;
			u.num          = o[ "num" ].I( 1 );
			u.name         = o[ "name" ].Str( "Universe " + std::to_string( u.num ) );
			u.artnetOut    = o[ "artnetOut" ].B( true );
			u.artnetPort   = o[ "artnetPort" ].I( u.num - 1 );
			u.artnetIn     = o[ "artnetIn" ].B( false );
			u.artnetInPort = o[ "artnetInPort" ].I( u.num - 1 );
			DmxFrame f;
			Unhex( o[ "dmx" ].Str(), f );
			universes.push_back( u );
			dmx.push_back( f );
		}
	}
	std::string missing;
	for( auto& o : j[ "fixtures" ].a )
	{
		FixtureInst f;
		f.id       = o[ "id" ].I( NextFixtureId() );
		f.name     = o[ "name" ].Str();
		f.defId    = o[ "def" ].Str();
		f.def      = lib.Find( f.defId );
		f.universe = o[ "universe" ].I( 1 );
		f.address  = o[ "address" ].I( 1 );
		f.pos      = V( o[ "pos" ], f.pos );
		f.rot      = V( o[ "rot" ], f.rot );
		f.invertPan  = o[ "invertPan" ].B();
		f.invertTilt = o[ "invertTilt" ].B();
		f.mount      = (MountType)Clamp( o[ "mount" ].I( 0 ), 0, 2 );
		f.mountIdx   = o[ "mountIdx" ].I( -1 );
		f.beam       = o[ "beam" ].F( 0 );
		if( f.def )
		{
			std::string mn = o[ "mode" ].Str();
			f.mode         = std::max( 0, f.def->FindMode( mn ) );
		}
		else
			missing += f.defId + " ";
		fixtures.push_back( f );
	}
	for( auto& o : j[ "trusses" ].a )
	{
		Truss t;
		t.a    = V( o[ "a" ], t.a );
		t.b    = V( o[ "b" ], t.b );
		t.size = o[ "size" ].F( 0.29f );
		//free: saved as such, or ends at different heights (a centre + length + rotation can't show that)
		t.freeEnds = o[ "free" ].B( false ) || fabsf( t.a.y - t.b.y ) > 1e-3f;
		trusses.push_back( t );
	}
	for( auto& o : j[ "stands" ].a )
	{
		Stand s;
		s.pos    = V( o[ "pos" ], s.pos );
		s.height = o[ "height" ].F( s.height );
		s.yaw    = o[ "yaw" ].F( 0 );
		s.bar    = o[ "bar" ].F( 0 );
		stands.push_back( s );
	}
	for( auto& o : j[ "models" ].a )
	{
		ModelObj m;
		m.path  = o[ "path" ].Str();
		m.name  = o[ "name" ].Str( FileName( m.path ) );
		m.pos   = V( o[ "pos" ], m.pos );
		m.rot   = V( o[ "rot" ], m.rot );
		m.scale = o[ "scale" ].F( 1 );
		m.tint  = V( o[ "tint" ], m.tint );
		m.matColors = o[ "matColors" ].B( true );
		models.push_back( m );
	}
	for( auto& o : j[ "projectors" ].a )
	{
		Projector p;
		p.name       = o[ "name" ].Str( p.name );
		p.on         = o[ "on" ].B( true );
		p.pos        = V( o[ "pos" ], p.pos );
		p.pan        = o[ "pan" ].F( 0 );
		p.tilt       = o[ "tilt" ].F( 0 );
		p.roll       = o[ "roll" ].F( 0 );
		p.format     = Clamp( o[ "format" ].I( 0 ), 0, 5 );
		p.throwRatio = o[ "throwRatio" ].F( p.throwRatio );
		p.shiftX     = o[ "shiftX" ].F( 0 );
		p.shiftY     = o[ "shiftY" ].F( 0 );
		p.lumens     = o[ "lumens" ].F( p.lumens );
		p.blackLevel = o[ "blackLevel" ].F( p.blackLevel );
		p.blur       = o[ "blur" ].F( 0 );
		p.rear       = o[ "rear" ].B( false );
		p.frustum    = o[ "frustum" ].B( true );
		p.input      = (VideoInput)Clamp( o[ "input" ].I( 0 ), 0, 2 );
		p.file       = o[ "file" ].Str();
		p.ndi        = o[ "ndi" ].Str();
		p.loop       = o[ "loop" ].B( true );
		projectors.push_back( p );
	}
	for( auto& o : j[ "screens" ].a )
	{
		Screen s;
		s.name   = o[ "name" ].Str( s.name );
		s.pos    = V( o[ "pos" ], s.pos );
		s.yaw    = o[ "yaw" ].F( 0 );
		s.tilt   = o[ "tilt" ].F( 0 );
		s.width  = o[ "width" ].F( s.width );
		s.height = o[ "height" ].F( s.height );
		s.gain   = o[ "gain" ].F( 1 );
		s.format = Clamp( o[ "format" ].I( -1 ), -1, 5 );
		s.frame  = o[ "frame" ].B( true );
		s.mount  = Clamp( o[ "mount" ].I( 0 ), 0, 2 );
		if( o.Find( "mask" ) )//the shape lives in the show file, the image is not needed any more
		{
			ScreenMaskFromString( s, o[ "maskN" ].I( 0 ), o[ "mask" ].Str( "" ) );
			s.maskFile = o[ "maskFile" ].Str( "" );
		}
		screens.push_back( s );
	}
	const Json& st    = j[ "stage" ];
	stage.width       = st[ "width" ].F( stage.width );
	stage.depth       = st[ "depth" ].F( stage.depth );
	stage.deckHeight  = st[ "deckHeight" ].F( stage.deckHeight );
	stage.deck        = st[ "deck" ].B( stage.deck );
	stage.backWall    = st[ "backWall" ].B( stage.backWall );
	stage.floor       = st[ "floor" ].B( stage.floor );
	stage.wallHeight  = st[ "wallHeight" ].F( stage.wallHeight );
	stage.haze        = st[ "haze" ].F( stage.haze );
	stage.hazeNoise   = st[ "hazeNoise" ].F( stage.hazeNoise );
	stage.ambient     = st[ "ambient" ].F( stage.ambient );
	stage.envLight    = Clamp( st[ "envLight" ].F( 1.0f ), 0.0f, 4.0f );
	{
		StageCfg d;
		stage.environment = Clamp( st[ "environment" ].I( 0 ), 0, 1 );
		stage.ground      = Clamp( st[ "ground" ].I( 0 ), 0, 5 );
		stage.groundSize  = Clamp( st[ "groundSize" ].F( d.groundSize ), 10.0f, 1000.0f );
		stage.skyZenith   = V( st[ "skyZenith" ], d.skyZenith );
		stage.skyHorizon  = V( st[ "skyHorizon" ], d.skyHorizon );
		stage.stars       = st[ "stars" ].F( d.stars );
		stage.moon        = st[ "moon" ].F( d.moon );
		stage.moonAz      = st[ "moonAz" ].F( d.moonAz );
		stage.moonEl      = st[ "moonEl" ].F( d.moonEl );
		stage.moonShadow  = st[ "moonShadow" ].B( false );
		stage.distanceFog = st[ "distanceFog" ].F( 0 );
	}
	stage.ambientColor = V( st[ "ambientColor" ], V3( 1, 1, 1 ) );
	stage.exposure    = st[ "exposure" ].F( stage.exposure );
	stage.bloom       = st[ "bloom" ].F( stage.bloom );
	stage.beamQuality = st[ "beamQuality" ].F( stage.beamQuality );
	stage.shadows     = st[ "shadows" ].B( stage.shadows );
	stage.beams       = st[ "beams" ].B( stage.beams );
	stage.beamLevel   = st[ "beamLevel" ].F( stage.beamLevel );
	stage.floorColor  = V( st[ "floorColor" ], stage.floorColor );
	stage.wallColor   = V( st[ "wallColor" ], stage.wallColor );
	stage.decks.clear();
	if( const Json* ds = st.Find( "decks" ) )
		for( auto& o : ds->a )
		{
			Deck d;
			d.name      = o[ "name" ].Str( d.name );
			d.shape     = Clamp( o[ "shape" ].I( 0 ), 0, 1 );
			d.x         = o[ "x" ].F( d.x );
			d.y         = std::max( 0.0f, o[ "y" ].F( 0 ) );
			d.z         = o[ "z" ].F( d.z );
			d.width     = Clamp( o[ "width" ].F( d.width ), 0.1f, 100.0f );
			d.depth     = Clamp( o[ "depth" ].F( d.depth ), 0.1f, 100.0f );
			d.height    = Clamp( o[ "height" ].F( d.height ), 0.0f, 10.0f );
			d.yaw       = o[ "yaw" ].F( 0 );
			d.onDeck    = o[ "onDeck" ].B( false );
			d.ownColor  = o[ "ownColor" ].B( false );
			d.color     = V( o[ "color" ], d.color );
			d.skirt     = o[ "skirt" ].B( true );
			d.stairs    = Clamp( o[ "stairs" ].I( 0 ), 0, 4 );
			d.edge      = V( o[ "edge" ], d.edge );
			d.edgeLevel = Clamp( o[ "edgeLevel" ].F( 0 ), 0.0f, 4.0f );
			stage.decks.push_back( d );
		}
	for( auto& o : j[ "midi" ].a ) midi.push_back( MidiMapFromJson( o ) );
	//effects, programs, tempo
	for( auto& o : j[ "effects" ].a )
	{
		Effect e;
		EffectFromJson( o, e );
		if( e.id <= 0 || FindEffect( e.id ) ) e.id = NextEffectId();
		effects.push_back( e );
	}
	for( auto& o : j[ "programs" ].a )
	{
		FxProgram p;
		p.name = o[ "name" ].Str( "Programm" );
		for( auto& x : o[ "effects" ].a ) p.effects.push_back( x.I() );
		programs.push_back( p );
	}
	if( const Json* t = j.Find( "tempo" ) )
	{
		tempo.bpm         = Clamp( ( *t )[ "bpm" ].F( 120 ), 20.0f, 300.0f );
		tempo.liveBpm     = tempo.bpm;
		tempo.beatsPerBar = Clamp( ( *t )[ "beatsPerBar" ].I( 4 ), 1, 16 );
		tempo.source      = (TempoSource)Clamp( ( *t )[ "source" ].I( (int)tempo.source ), 0, 2 );
	}
	for( auto& o : j[ "snapshots" ].a )
	{
		Snapshot s;
		s.uid  = (uint32_t)o[ "uid" ].Num( 0 );
		s.name = o[ "name" ].Str();
		if( o.Find( "fixtures" ) )
		{
			s.master   = Saturate( o[ "master" ].F( 1 ) );
			s.fade     = Clamp( o[ "fade" ].F( 1 ), 0.0f, 600.0f );
			s.active   = o[ "active" ].B( false );
			s.withFx   = o[ "withFx" ].B( true );
			s.withProj = o[ "withProj" ].B( false );
			for( auto& x : o[ "fixtures" ].a )
			{
				SnapFixture sf;
				sf.id = x[ "id" ].I();
				UnhexV( x[ "v" ].Str(), sf.v );
				s.fix.push_back( std::move( sf ) );
			}
			for( auto& x : o[ "effects" ].a ) s.effects.push_back( x.I() );
			for( auto& x : o[ "effectsOff" ].a ) s.fxOff.push_back( x.I() );
			for( auto& x : o[ "palettes" ].a ) s.pal.push_back( { x[ 0 ].I(), (uint32_t)x[ 1 ].Num( 0 ) } );
			for( auto& x : o[ "projectors" ].a )
			{
				SnapProjector q;
				q.idx        = x[ "idx" ].I();
				q.on         = x[ "on" ].B( true );
				q.lumens     = x[ "lumens" ].F( q.lumens );
				q.blackLevel = x[ "blackLevel" ].F( q.blackLevel );
				q.blur       = x[ "blur" ].F( 0 );
				q.pos        = V( x[ "pos" ], q.pos );
				q.pan        = x[ "pan" ].F( 0 );
				q.tilt       = x[ "tilt" ].F( 0 );
				q.roll       = x[ "roll" ].F( 0 );
				q.throwRatio = x[ "throwRatio" ].F( q.throwRatio );
				q.shiftX     = x[ "shiftX" ].F( 0 );
				q.shiftY     = x[ "shiftY" ].F( 0 );
				s.proj.push_back( q );
			}
		}
		else
		{
			//old file: whole universes -> every lamp, switched at once (fade 0) like before
			for( auto& kv : o[ "dmx" ].o )
			{
				DmxFrame f;
				Unhex( kv.second.Str(), f );
				s.data.push_back( { atoi( kv.first.c_str() ), f } );
			}
			for( auto& f : fixtures )
				for( auto& d : s.data )
				{
					if( d.first != f.universe ) continue;
					SnapFixture sf;
					sf.id        = f.id;
					const int fp = f.Footprint();
					for( int k = 0; k < fp && f.address - 1 + k < 512; ++k ) sf.v.push_back( d.second[ f.address - 1 + k ] );
					s.fix.push_back( std::move( sf ) );
					break;
				}
			s.data.clear();
			s.withFx = false;
			s.fade   = 0;
		}
		if( s.active )
		{
			s.cur = 1;
			s.seq = ++snapSeq;
		}
		snapshots.push_back( std::move( s ) );
	}
	//snapshot ids: missing (old files) or doubled -> new ones
	for( size_t i = 0; i < snapshots.size(); ++i )
	{
		bool dup = snapshots[ i ].uid == 0;
		for( size_t k = 0; k < i && !dup; ++k ) dup = snapshots[ k ].uid == snapshots[ i ].uid;
		if( dup ) snapshots[ i ].uid = NewSnapUid();
	}
	for( auto& o : j[ "palettes" ].a )
	{
		Palette p;
		p.uid   = (uint32_t)o[ "uid" ].Num( 0 );
		p.name  = o[ "name" ].Str( "Palette" );
		p.kind  = Clamp( o[ "kind" ].I( 0 ), 0, 1 );
		p.color = V( o[ "color" ], p.color );
		for( auto& x : o[ "pos" ].a ) p.pos.push_back( { x[ 0 ].I(), Saturate( x[ 1 ].F( 0.5f ) ), Saturate( x[ 2 ].F( 0.5f ) ) } );
		if( !p.uid || FindPalette( p.uid ) ) p.uid = NewPaletteUid();
		palettes.push_back( p );
	}
	for( auto& o : j[ "cues" ].a )
	{
		Cue c;
		c.name    = o[ "name" ].Str();
		c.snap    = (uint32_t)o[ "snap" ].Num( 0 );
		c.fade    = o[ "fade" ].F( -1 );
		c.trigger = Clamp( o[ "trigger" ].I( 0 ), 0, 2 );
		c.wait    = std::max( 0.0f, o[ "wait" ].F( 0 ) );
		c.tc      = std::max( 0.0, o[ "tc" ].Num( 0 ) );
		c.release = o[ "release" ].B( true );
		cues.push_back( c );
	}
	cueTimecode = j[ "cueTimecode" ].B( false );
	TimelineFromJson( tl, j[ "timeline" ] );
	//show id: saved one, else derived from the file name (old files: stable until saved once)
	if( j.Find( "showId" ) && !j[ "showId" ].Str().empty() ) showId = j[ "showId" ].Str();
	else
	{
		char b[ 24 ];
		snprintf( b, sizeof( b ), "f%015llx", (unsigned long long)( std::hash< std::string >()( ToLower( FileName( file ) ) ) & 0xFFFFFFFFFFFFFFFull ) );
		showId = b;
	}
	for( auto& o : j[ "ableton" ].a )
	{
		AbletonMap m;
		m.id         = o[ "id" ].Str();
		m.label      = o[ "label" ].Str( "Control" );
		m.kind       = o[ "kind" ].Str( "control" );
		m.state      = o[ "state" ].Str();
		m.knobsNewer = o[ "knobsNewer" ].B( false );
		for( size_t i = 0; i < m.knobs.size() && i < o[ "knobs" ].a.size(); ++i ) m.knobs[ i ] = o[ "knobs" ].a[ i ].Str();
		for( size_t i = 0; i < m.buttons.size() && i < o[ "buttons" ].a.size(); ++i ) m.buttons[ i ] = o[ "buttons" ].a[ i ].Str();
		if( !m.label.empty() && !FindTable( m.kind, m.label ) ) ableton.push_back( m );
	}
	for( auto& o : j[ "groups" ].a )
	{
		GroupFader g;
		g.name = o[ "name" ].Str( "Gruppe" );
		for( auto& x : o[ "fixtures" ].a ) g.fixtures.push_back( x.I() );
		g.kind  = Clamp( o[ "kind" ].I( 0 ), 0, 1 );
		g.attr  = AttrFromName( o[ "attr" ].Str( "Dimmer" ) );
		if( g.attr == Attr::None ) g.attr = Attr::Dimmer;
		g.mode  = Clamp( o[ "mode" ].I( 0 ), 0, 1 );
		g.value = Saturate( o[ "value" ].F( 1 ) );
		g.hue   = Saturate( o[ "hue" ].F( 0 ) );
		g.sat   = Saturate( o[ "sat" ].F( 1 ) );
		groups.push_back( g );
	}
	const Json& out       = j[ "output" ];
	output.artnet         = out[ "artnet" ].B( output.artnet );
	output.artnetTarget   = out[ "artnetTarget" ].Str( output.artnetTarget );
	output.artnetBind     = out[ "artnetBind" ].Str( output.artnetBind );
	output.artnetInput    = out[ "artnetInput" ].B( output.artnetInput );
	output.enttec         = out[ "enttec" ].B( output.enttec );
	output.enttecPort     = out[ "enttecPort" ].Str( output.enttecPort );
	output.enttecUniverse = out[ "enttecUniverse" ].I( output.enttecUniverse );
	output.oscIn          = out[ "oscIn" ].B( output.oscIn );
	output.oscPort        = out[ "oscPort" ].I( output.oscPort );
	output.rate           = out[ "rate" ].F( output.rate );
	output.sacn           = out[ "sacn" ].B( false );
	output.sacnTarget     = out[ "sacnTarget" ].Str( "" );
	output.sacnPriority   = Clamp( out[ "sacnPriority" ].I( 100 ), 0, 200 );
	master                = Clamp( j[ "master" ].F( 1 ), 0.0f, 10.0f );
	fxMaster              = Clamp( j[ "fxMaster" ].F( 1 ), 0.0f, 1.0f );
	fxSpeed               = Clamp( j[ "fxSpeed" ].F( 1 ), 0.25f, 4.0f );
	if( universes.empty() ) AddUniverse();
	path  = file;
	dirty = false;
	++dmxVersion;
	if( !missing.empty() && err ) *err = "unknown fixture types: " + missing;
	return true;
}
}//namespace ls
