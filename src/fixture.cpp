#include "fixture.h"
#include "xmlzip.h"
#include "../third_party/stb_image.h"
#include <regex>
#include <set>

namespace ls
{
//---------------------------------------------------------------- attributes
static const char* kAttrNames[] = { "None", "Dimmer", "Shutter", "Red", "Green", "Blue", "White", "Warm White", "Cold White", "Amber", "UV", "Lime", "Indigo", "Cyan", "Magenta", "Yellow", "CTO", "Color Wheel", "Color Macro", "Pan", "Tilt", "Pan/Tilt Speed", "Zoom", "Focus", "Gobo", "Gobo Rotation", "Prism", "Prism Rotation", "Frost", "Iris", "Fog", "Effect", "Generic" };
static_assert( sizeof( kAttrNames ) / sizeof( kAttrNames[ 0 ] ) == (size_t)Attr::Count, "attr names" );
const char* AttrName( Attr a ) { return (size_t)a < (size_t)Attr::Count ? kAttrNames[ (size_t)a ] : "?"; }
Attr AttrFromName( const std::string& s )
{
	for( size_t i = 0; i < (size_t)Attr::Count; ++i )
		if( s == kAttrNames[ i ] )
			return (Attr)i;
	return Attr::None;
}
bool IsEmitter( Attr a ) { return a >= Attr::Red && a <= Attr::Yellow; }
V3 EmitterColor( Attr a )
{
	switch( a )
	{
	case Attr::Red: return { 1.0f, 0.015f, 0.01f };
	case Attr::Green: return { 0.03f, 1.0f, 0.06f };
	case Attr::Blue: return { 0.02f, 0.06f, 1.0f };
	case Attr::White: return { 1.0f, 0.97f, 0.92f };
	case Attr::WarmWhite: return { 1.0f, 0.72f, 0.42f };
	case Attr::ColdWhite: return { 0.86f, 0.93f, 1.0f };
	case Attr::Amber: return { 1.0f, 0.45f, 0.02f };
	case Attr::UV: return { 0.22f, 0.0f, 0.55f };
	case Attr::Lime: return { 0.55f, 1.0f, 0.08f };
	case Attr::Indigo: return { 0.2f, 0.0f, 1.0f };
	case Attr::Cyan: return { 0.0f, 0.75f, 1.0f };
	case Attr::Magenta: return { 1.0f, 0.0f, 0.8f };
	case Attr::Yellow: return { 1.0f, 0.85f, 0.0f };
	default: return { 1, 1, 1 };
	}
}
const char* KindName( Kind k )
{
	static const char* n[] = { "PAR", "Moving Head", "Scanner", "Bar", "Strobe", "Blinder", "Hazer", "Dimmer", "Wash", "Laser", "Generic" };
	return n[ (int)k ];
}

bool Wheel::IsColor() const
{
	for( auto& s : slots )
		if( s.type == SlotT::Color )
			return true;
	return false;
}
bool Wheel::IsGobo() const
{
	for( auto& s : slots )
		if( s.type == SlotT::Gobo || s.type == SlotT::Anim )
			return true;
	return false;
}
bool Wheel::IsPrism() const
{
	for( auto& s : slots )
		if( s.type == SlotT::Prism )
			return true;
	return false;
}

const Cap* Channel::Find( uint16_t v ) const
{
	for( auto& c : caps )
		if( v >= c.lo && v <= c.hi )
			return &c;
	return caps.empty() ? nullptr : &caps.back();
}
int FixtureDef::FindMode( const std::string& n ) const
{
	for( size_t i = 0; i < modes.size(); ++i )
		if( modes[ i ].name == n || modes[ i ].shortName == n )
			return (int)i;
	return -1;
}

//---------------------------------------------------------------- entity parsing
namespace
{
std::string Trim( const std::string& s )
{
	size_t a = s.find_first_not_of( " \t" ), b = s.find_last_not_of( " \t" );
	return a == std::string::npos ? "" : s.substr( a, b - a + 1 );
}
//leading number + unit (lowercase, trimmed)
bool NumUnit( const std::string& s, float& v, std::string& unit )
{
	const char* p = s.c_str();
	char* e       = nullptr;
	double d      = strtod( p, &e );
	if( e == p )
		return false;
	v    = (float)d;
	unit = ToLower( Trim( e ) );
	return true;
}
bool Contains( const std::string& s, const char* w ) { return s.find( w ) != std::string::npos; }

//rotation speed in rev/s, signed (CCW negative)
float RotSpeed( const Json& j )
{
	if( j.IsNum() )
		return j.F();
	std::string s = ToLower( j.Str() );
	float sign    = ( Contains( s, "ccw" ) || Contains( s, "reverse" ) ) ? -1.0f : 1.0f;
	if( Contains( s, "stop" ) )
		return 0.0f;
	float v;
	std::string u;
	if( NumUnit( s, v, u ) )
	{
		if( u.rfind( "hz", 0 ) == 0 ) return sign * v;
		if( u.rfind( "rpm", 0 ) == 0 ) return sign * v / 60.0f;
		if( u.rfind( "bpm", 0 ) == 0 ) return sign * v / 60.0f;
		if( u.rfind( "%", 0 ) == 0 ) return v == 0 ? 0.0f : sign * ( 0.03f + v / 100.0f * 1.5f );
		return sign * v;
	}
	if( Contains( s, "slow" ) ) return sign * 0.06f;
	if( Contains( s, "fast" ) ) return sign * 1.5f;
	return 0.0f;
}
//strobe frequency in Hz
float StrobeHz( const Json& j )
{
	if( j.IsNum() )
		return j.F();
	std::string s = ToLower( j.Str() );
	float v;
	std::string u;
	if( NumUnit( s, v, u ) )
	{
		if( u.rfind( "hz", 0 ) == 0 ) return v;
		if( u.rfind( "bpm", 0 ) == 0 || u.rfind( "rpm", 0 ) == 0 ) return v / 60.0f;
		if( u.rfind( "%", 0 ) == 0 ) return 0.8f + v / 100.0f * 22.0f;
		return v;
	}
	if( Contains( s, "slow" ) ) return 0.8f;
	if( Contains( s, "fast" ) ) return 22.0f;
	return 0.0f;
}
float Percentish( const Json& j, float def )
{
	if( j.IsNum() )
		return j.F();
	std::string s = ToLower( j.Str() );
	float v;
	std::string u;
	if( NumUnit( s, v, u ) )
	{
		if( u.rfind( "%", 0 ) == 0 ) return v / 100.0f;
		if( u == "m" ) return Saturate( v / 25.0f );
		return v;
	}
	if( s == "off" || s == "closed" || s == "dark" || s == "none" || s == "near" || s == "weak" || s == "small" || s == "short" ) return s == "dark" ? 0.03f : 0.0f;
	if( s == "low" ) return 0.25f;
	if( s == "high" || s == "strong" || s == "big" || s == "long" ) return 1.0f;
	if( s == "on" || s == "open" || s == "bright" || s == "far" ) return 1.0f;
	return def;
}
float AngleDeg( const Json& j, float narrow, float wide )
{
	if( j.IsNum() )
		return j.F();
	std::string s = ToLower( j.Str() );
	float v;
	std::string u;
	if( NumUnit( s, v, u ) )
	{
		if( u.rfind( "rad", 0 ) == 0 ) return v / kDeg;
		if( u.rfind( "%", 0 ) == 0 ) return Lerp( narrow, wide, v / 100.0f );
		return v;
	}
	if( s == "narrow" ) return narrow;
	if( s == "wide" ) return wide;
	return narrow;
}
float KelvinOf( const Json& j )
{
	if( j.IsNum() )
		return j.F();
	std::string s = ToLower( j.Str() );
	float v;
	std::string u;
	if( NumUnit( s, v, u ) )
	{
		if( u.rfind( "%", 0 ) == 0 ) return Lerp( 2700.0f, 8000.0f, v / 100.0f );
		return v;
	}
	if( s == "warm" ) return 2900.0f;
	if( s == "cto" ) return 3200.0f;
	if( s == "cold" || s == "ctb" ) return 8000.0f;
	return 5600.0f;
}
float DurationS( const Json& j )
{
	if( j.IsNum() )
		return j.F();
	std::string s = ToLower( j.Str() );
	float v;
	std::string u;
	if( NumUnit( s, v, u ) )
	{
		if( u == "ms" ) return v / 1000.0f;
		if( u.rfind( "%", 0 ) == 0 ) return v / 100.0f * 5.0f;
		return v;
	}
	if( s == "instant" || s == "short" ) return 0.0f;
	if( s == "long" ) return 5.0f;
	return 1.0f;
}
//"<base>Start"/"<base>End" or "<base>"
bool Pair( const Json& cap, const std::string& base, const Json*& s, const Json*& e )
{
	s = cap.Find( base + "Start" );
	e = cap.Find( base + "End" );
	if( s && e )
		return true;
	s = e = cap.Find( base );
	return s != nullptr;
}

V3 ColorNameToRgb( const std::string& n, Attr* attrOut )
{
	std::string s = ToLower( n );
	Attr a        = Attr::White;
	if( s == "red" ) a = Attr::Red;
	else if( s == "green" ) a = Attr::Green;
	else if( s == "blue" ) a = Attr::Blue;
	else if( s == "cyan" ) a = Attr::Cyan;
	else if( s == "magenta" ) a = Attr::Magenta;
	else if( s == "yellow" ) a = Attr::Yellow;
	else if( s == "amber" ) a = Attr::Amber;
	else if( s == "warm white" ) a = Attr::WarmWhite;
	else if( s == "cold white" ) a = Attr::ColdWhite;
	else if( s == "uv" ) a = Attr::UV;
	else if( s == "lime" ) a = Attr::Lime;
	else if( s == "indigo" ) a = Attr::Indigo;
	if( attrOut )
		*attrOut = a;
	return EmitterColor( a );
}

uint16_t To16( double v, int res )
{
	if( res <= 1 )
		return (uint16_t)Clamp( v * 257.0, 0.0, 65535.0 );
	if( res == 2 )
		return (uint16_t)Clamp( v, 0.0, 65535.0 );
	return (uint16_t)Clamp( v / 256.0, 0.0, 65535.0 );
}
uint16_t Lo16( double v, int res )
{
	if( res <= 1 )
		return (uint16_t)Clamp( v * 256.0, 0.0, 65535.0 );
	return To16( v, res );
}
uint16_t Hi16( double v, int res )
{
	if( res <= 1 )
		return (uint16_t)Clamp( v * 256.0 + 255.0, 0.0, 65535.0 );
	if( res == 3 )
		return (uint16_t)Clamp( v / 256.0, 0.0, 65535.0 );
	return (uint16_t)Clamp( v, 0.0, 65535.0 );
}

//natural compare for pixel keys ("2" < "10")
bool NaturalLess( const std::string& a, const std::string& b )
{
	size_t i = 0, j = 0;
	while( i < a.size() && j < b.size() )
	{
		if( isdigit( (unsigned char)a[ i ] ) && isdigit( (unsigned char)b[ j ] ) )
		{
			size_t i2 = i, j2 = j;
			while( i2 < a.size() && isdigit( (unsigned char)a[ i2 ] ) ) ++i2;
			while( j2 < b.size() && isdigit( (unsigned char)b[ j2 ] ) ) ++j2;
			long x = atol( a.substr( i, i2 - i ).c_str() ), y = atol( b.substr( j, j2 - j ).c_str() );
			if( x != y )
				return x < y;
			i = i2;
			j = j2;
		}
		else
		{
			char x = (char)tolower( (unsigned char)a[ i ] ), y = (char)tolower( (unsigned char)b[ j ] );
			if( x != y )
				return x < y;
			++i;
			++j;
		}
	}
	return a.size() - i < b.size() - j;
}

std::string Replace( std::string s, const std::string& from, const std::string& to )
{
	size_t p = 0;
	while( ( p = s.find( from, p ) ) != std::string::npos )
	{
		s.replace( p, from.size(), to );
		p += to.size();
	}
	return s;
}

uint32_t Hash( const std::string& s )
{
	uint32_t h = 2166136261u;
	for( unsigned char c : s )
		h = ( h ^ c ) * 16777619u;
	return h;
}
}//namespace

//---------------------------------------------------------------- gobos
namespace
{
float Frac( float x ) { return x - floorf( x ); }
float HashF( int x, int y, uint32_t s )
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + s * 2246822519u;
	h          = ( h ^ ( h >> 13 ) ) * 1274126177u;
	return ( ( h ^ ( h >> 16 ) ) & 0xffff ) / 65535.0f;
}
float VNoise( float x, float y, uint32_t s )
{
	int ix = (int)floorf( x ), iy = (int)floorf( y );
	float fx = x - ix, fy = y - iy;
	fx = fx * fx * ( 3 - 2 * fx );
	fy = fy * fy * ( 3 - 2 * fy );
	float a = HashF( ix, iy, s ), b = HashF( ix + 1, iy, s ), c = HashF( ix, iy + 1, s ), d = HashF( ix + 1, iy + 1, s );
	return Lerp( Lerp( a, b, fx ), Lerp( c, d, fx ), fy );
}
float Fbm( float x, float y, uint32_t s )
{
	float v = 0, a = 0.5f;
	for( int i = 0; i < 4; ++i )
	{
		v += a * VNoise( x, y, s + i );
		x *= 2.03f;
		y *= 2.03f;
		a *= 0.5f;
	}
	return v;
}
//x,y in -1..1; returns 1 = light passes
float GoboShape( int id, float x, float y )
{
	float r = sqrtf( x * x + y * y ), a = atan2f( y, x );
	switch( id )
	{
	case 0: return 1.0f;//open
	case 1://dots ring
	{
		if( r < 0.16f ) return 1.0f;
		for( int i = 0; i < 6; ++i )
		{
			float ang = i * kPi / 3.0f;
			float dx = x - 0.58f * cosf( ang ), dy = y - 0.58f * sinf( ang );
			if( dx * dx + dy * dy < 0.2f * 0.2f ) return 1.0f;
		}
		return 0.0f;
	}
	case 2://star
	{
		float k = cosf( floorf( 0.5f + a / ( 2 * kPi / 5 ) ) * ( 2 * kPi / 5 ) - a );
		float rr = 0.45f + 0.5f * powf( fabsf( cosf( a * 2.5f ) ), 6.0f );
		(void)k;
		return r < rr * 0.95f ? 1.0f : 0.0f;
	}
	case 3://three triangles
	{
		for( int i = 0; i < 3; ++i )
		{
			float ang = i * 2 * kPi / 3 + kPi / 2;
			float cx = 0.5f * cosf( ang ), cy = 0.5f * sinf( ang );
			float px = x - cx, py = y - cy;
			//equilateral triangle sdf-ish
			float k = sqrtf( 3.0f ), s = 0.32f;
			px = fabsf( px ) - s;
			py = py + s / k;
			if( px + k * py > 0 ) { float t = px; px = ( px - k * py ) / 2; py = ( -k * t - py ) / 2; }
			px -= Clamp( px, -2 * s, 0.0f );
			float d = -sqrtf( px * px + py * py ) * ( py < 0 ? -1.0f : 1.0f );
			if( d > 0 ) return 1.0f;
		}
		return 0.0f;
	}
	case 4: return Frac( r * 4.0f ) < 0.45f ? 1.0f : 0.0f;//rings
	case 5: return ( fabsf( x ) < 0.17f || fabsf( y ) < 0.17f ) ? 1.0f : 0.0f;//cross
	case 6: return Frac( x * 3.5f ) < 0.42f ? 1.0f : 0.0f;//bars
	case 7://flower
	{
		float p = 0.35f + 0.55f * fabsf( cosf( a * 4.0f ) );
		return ( r < p && r > 0.1f ) ? 1.0f : 0.0f;
	}
	case 8: return Fbm( x * 3.0f + 7, y * 3.0f + 3, 11 ) > 0.52f ? 1.0f : 0.0f;//breakup leaves
	case 9://spiral
	{
		float t = Frac( ( a / ( 2 * kPi ) ) * 3.0f + r * 2.5f );
		return t < 0.45f ? 1.0f : 0.0f;
	}
	case 10://honeycomb
	{
		float s = 5.0f;
		float qx = x * s, qy = y * s;
		float best = 1e9f;
		for( int j = -1; j <= 1; ++j )
			for( int i = -1; i <= 1; ++i )
			{
				float cy = floorf( qy / 0.866f ) + j;
				float cx = floorf( qx ) + i + ( ( (int)cy & 1 ) ? 0.5f : 0.0f );
				float dx = qx - cx, dy = qy - cy * 0.866f;
				best = std::min( best, sqrtf( dx * dx + dy * dy ) );
			}
		return best < 0.38f ? 1.0f : 0.0f;
	}
	case 11: return ( Frac( x * 3.0f + 0.5f ) < 0.7f && Frac( y * 3.0f + 0.5f ) < 0.7f ) ? 1.0f : 0.0f;//grid
	case 12: return Frac( y * 3.0f + 0.25f * sinf( x * 7.0f ) ) < 0.45f ? 1.0f : 0.0f;//waves
	case 13: return ( Frac( a / ( 2 * kPi ) * 12.0f ) < 0.4f && r > 0.12f ) || r < 0.1f ? 1.0f : 0.0f;//sun rays
	case 14: { float gx = Frac( x * 3.2f ) - 0.5f, gy = Frac( y * 3.2f ) - 0.5f; return gx * gx + gy * gy < 0.09f ? 1.0f : 0.0f; }//dot grid
	case 15: { float dx = x - 0.28f; return ( r < 0.8f && dx * dx + y * y > 0.55f * 0.55f ) ? 1.0f : 0.0f; }//crescent
	case 16: return Fbm( r * 3.0f + a * 0.9f, a * 2.0f, 5 ) * ( 1.2f - r ) > 0.42f ? 1.0f : 0.0f;//galaxy
	case 17: return ( fabsf( x ) > 0.08f && fabsf( y ) > 0.08f && fabsf( x ) < 0.62f && fabsf( y ) < 0.62f ) ? 1.0f : 0.0f;//window
	case 18: return ( Frac( a / ( 2 * kPi ) * 3.0f ) < 0.22f && r > 0.25f ) ? 1.0f : 0.0f;//three spokes
	case 19: return Frac( r * 6.0f - a / kPi ) < 0.4f ? 1.0f : 0.0f;//tunnel
	default: return 1.0f;
	}
}
const char* kGoboNames[] = { "Open", "Dots", "Star", "Triangles", "Rings", "Cross", "Bars", "Flower", "Breakup", "Spiral", "Honeycomb", "Grid", "Waves", "Sun", "Dot Grid", "Crescent", "Galaxy", "Window", "Spokes", "Tunnel" };
constexpr int kNumProcGobos = 20;
}//namespace

GoboBank& Gobos()
{
	static GoboBank b;
	return b;
}
void GoboBank::Init()
{
	if( !layers.empty() )
		return;
	for( int id = 0; id < kNumProcGobos; ++id )
	{
		std::vector< uint8_t > img( kGoboSize * kGoboSize );
		for( int y = 0; y < kGoboSize; ++y )
			for( int x = 0; x < kGoboSize; ++x )
			{
				float acc = 0;
				for( int sy = 0; sy < 3; ++sy )
					for( int sx = 0; sx < 3; ++sx )
					{
						float u = ( ( x + ( sx + 0.5f ) / 3.0f ) / kGoboSize ) * 2.0f - 1.0f;
						float v = ( ( y + ( sy + 0.5f ) / 3.0f ) / kGoboSize ) * 2.0f - 1.0f;
						float rr = sqrtf( u * u + v * v );
						if( rr <= 0.98f )
							acc += GoboShape( id, u / 0.98f, v / 0.98f );
					}
				img[ y * kGoboSize + x ] = (uint8_t)( acc / 9.0f * 255.0f + 0.5f );
			}
		layers.push_back( std::move( img ) );
		names.push_back( kGoboNames[ id ] );
	}
	procedural = kNumProcGobos;
	++version;
}
int GoboBank::ByName( const std::string& n, uint32_t seed )
{
	Init();
	std::string s = ToLower( n );
	struct K { const char* w; int id; };
	static const K keys[] = { { "open", 0 }, { "dot", 1 }, { "star", 2 }, { "triang", 3 }, { "ring", 4 }, { "circle", 4 }, { "cross", 5 }, { "bar", 6 }, { "line", 6 }, { "stripe", 6 }, { "flower", 7 }, { "leaf", 8 }, { "leaves", 8 }, { "breakup", 8 }, { "break", 8 }, { "spiral", 9 }, { "swirl", 9 }, { "hex", 10 }, { "honey", 10 }, { "grid", 11 }, { "square", 11 }, { "wave", 12 }, { "sun", 13 }, { "ray", 13 }, { "crescent", 15 }, { "moon", 15 }, { "galax", 16 }, { "cloud", 16 }, { "window", 17 }, { "spoke", 18 }, { "tunnel", 19 }, { "fan", 18 } };
	for( auto& k : keys )
		if( s.find( k.w ) != std::string::npos )
			return k.id;
	return 1 + (int)( ( Hash( s ) ^ seed ) % ( kNumProcGobos - 1 ) );
}
int GoboBank::Add( const std::string& name, std::vector< uint8_t >&& img )
{
	Init();
	for( size_t i = procedural; i < names.size(); ++i )
		if( names[ i ] == name )
			return (int)i;
	layers.push_back( std::move( img ) );
	names.push_back( name );
	++version;
	return (int)layers.size() - 1;
}

//---------------------------------------------------------------- OFL
namespace
{
struct OflCtx
{
	const Json* root = nullptr;
	FixtureDef* def  = nullptr;
	std::vector< std::string > pixelKeys;//in pixel index order
	std::map< std::string, int > pixelIndex;
	std::map< std::string, std::vector< int > > groupPixels;//group name -> indices
	struct Ref
	{
		const Json* j = nullptr;
		std::string key;  //channel key (resolved, without fine suffix)
		std::string pixel;//pixel key or group
		int fineIdx = 0;  //0 coarse, 1 fine, 2 fine^2
	};
	std::map< std::string, Ref > refs;
	float lensMin = 0, lensMax = 0;
};

int ChannelRes( const Json& ch )
{
	std::string r = ch[ "dmxValueResolution" ].Str();
	if( r == "8bit" ) return 1;
	if( r == "16bit" ) return 2;
	if( r == "24bit" ) return 3;
	return 1 + (int)ch[ "fineChannelAliases" ].Size();
}

int WheelIndex( const FixtureDef& d, const std::string& n )
{
	for( size_t i = 0; i < d.wheels.size(); ++i )
		if( d.wheels[ i ].name == n )
			return (int)i;
	return -1;
}

bool GuessColorFromName( const std::string& name, V3& out )
{
	std::string nm = ToLower( name );
	struct NC { const char* n; V3 c; };
	static const NC nc[] = { { "deep red", { 1, 0.01f, 0.02f } }, { "red", { 1, 0.05f, 0.05f } }, { "dark green", { 0.02f, 0.6f, 0.08f } }, { "green", { 0.1f, 1, 0.15f } }, { "congo", { 0.25f, 0.05f, 1 } }, { "deep blue", { 0.05f, 0.1f, 1 } }, { "light blue", { 0.45f, 0.7f, 1 } }, { "blue", { 0.1f, 0.25f, 1 } }, { "yellow", { 1, 0.9f, 0.1f } }, { "gold", { 1, 0.7f, 0.15f } }, { "magenta", { 1, 0.1f, 0.9f } }, { "fuchsia", { 1, 0.1f, 0.7f } }, { "pink", { 1, 0.45f, 0.7f } }, { "rose", { 1, 0.5f, 0.6f } }, { "cyan", { 0.1f, 0.9f, 1 } }, { "turquoise", { 0.1f, 1, 0.8f } }, { "orange", { 1, 0.45f, 0.05f } }, { "amber", { 1, 0.6f, 0.1f } }, { "purple", { 0.55f, 0.1f, 1 } }, { "lavender", { 0.75f, 0.6f, 1 } }, { "violet", { 0.5f, 0.1f, 1 } }, { "uv", { 0.35f, 0.05f, 0.9f } }, { "cto", { 1, 0.7f, 0.4f } }, { "ctb", { 0.7f, 0.85f, 1 } }, { "lime", { 0.6f, 1, 0.1f } }, { "warm white", { 1, 0.8f, 0.55f } }, { "white", { 1, 1, 1 } } };
	for( auto& x : nc )
		if( nm.find( x.n ) != std::string::npos ) { out = x.c; return true; }
	return false;
}

Cap ParseCap( const Json& c, const std::string& chName, OflCtx& ctx, int res, Attr& emit )
{
	FixtureDef& d = *ctx.def;
	Cap cap;
	const Json& r = c[ "dmxRange" ];
	if( r.Size() == 2 )
	{
		cap.lo = Lo16( r[ 0 ].Num(), res );
		cap.hi = Hi16( r[ 1 ].Num(), res );
	}
	std::string t = c[ "type" ].Str();
	const Json *s = nullptr, *e = nullptr;
	cap.label     = c[ "comment" ].Str();
	if( const Json* sw = c.Find( "switchChannels" ) )
		for( auto& kv : sw->o )
			cap.sw.push_back( { kv.first, kv.second.Str() } );
	auto wheelOf = [ & ]() {
		std::string w = c[ "wheel" ].Str();
		if( w.empty() )
			w = chName;
		int wi = WheelIndex( d, w );
		if( wi < 0 && d.wheels.size() == 1 )
			wi = 0;
		return wi;
	};
	if( t == "Intensity" )
	{
		cap.k = CapK::Intensity;
		if( Pair( c, "brightness", s, e ) ) { cap.a = Percentish( *s, 0 ); cap.b = Percentish( *e, 1 ); }
		else { cap.a = 0; cap.b = 1; }
	}
	else if( t == "ColorIntensity" )
	{
		cap.k = CapK::ColorIntensity;
		ColorNameToRgb( c[ "color" ].Str(), &emit );
		if( Pair( c, "brightness", s, e ) ) { cap.a = Percentish( *s, 0 ); cap.b = Percentish( *e, 1 ); }
		else { cap.a = 0; cap.b = 1; }
		if( cap.label.empty() ) cap.label = c[ "color" ].Str();
	}
	else if( t == "ShutterStrobe" )
	{
		std::string ef = c[ "shutterEffect" ].Str();
		if( ef == "Open" ) cap.k = CapK::ShutterOpen;
		else if( ef == "Closed" ) cap.k = CapK::ShutterClosed;
		else if( ef == "Pulse" ) cap.k = CapK::StrobePulse;
		else if( ef == "RampUp" ) cap.k = CapK::StrobeRampUp;
		else if( ef == "RampDown" || ef == "RampUpDown" ) cap.k = CapK::StrobeRampDown;
		else if( ef == "Lightning" || ef == "Spikes" || ef == "Burst" || c[ "randomTiming" ].B() ) cap.k = CapK::StrobeRandom;
		else cap.k = CapK::Strobe;
		if( Pair( c, "speed", s, e ) ) { cap.a = StrobeHz( *s ); cap.b = StrobeHz( *e ); }
		else { cap.a = 2.0f; cap.b = 18.0f; }
		if( cap.label.empty() ) cap.label = ef + ( cap.k >= CapK::Strobe ? " " : "" );
	}
	else if( t == "StrobeSpeed" )
	{
		cap.k = CapK::Strobe;
		if( Pair( c, "speed", s, e ) ) { cap.a = StrobeHz( *s ); cap.b = StrobeHz( *e ); }
	}
	else if( t == "Pan" || t == "Tilt" )
	{
		cap.k = t == "Pan" ? CapK::Pan : CapK::Tilt;
		if( Pair( c, "angle", s, e ) ) { cap.a = AngleDeg( *s, 0, 0 ); cap.b = AngleDeg( *e, 0, 0 ); }
	}
	else if( t == "PanContinuous" || t == "TiltContinuous" )
	{
		cap.k = t == "PanContinuous" ? CapK::PanCont : CapK::TiltCont;
		if( Pair( c, "speed", s, e ) ) { cap.a = RotSpeed( *s ); cap.b = RotSpeed( *e ); }
	}
	else if( t == "PanTiltSpeed" )
	{
		cap.k = CapK::PanTiltSpeed;
		if( Pair( c, "speed", s, e ) )
		{
			auto sp = []( const Json& j ) {
				std::string v = ToLower( j.Str() );
				if( v == "fast" ) return 1.0f;
				if( v == "slow" ) return 0.05f;
				return Saturate( Percentish( j, 1.0f ) );
			};
			cap.a = sp( *s );
			cap.b = sp( *e );
		}
		else if( Pair( c, "duration", s, e ) ) { cap.a = 1.0f / ( 1.0f + DurationS( *s ) ); cap.b = 1.0f / ( 1.0f + DurationS( *e ) ); }
		else { cap.a = 1; cap.b = 0.05f; }
	}
	else if( t == "WheelSlot" )
	{
		cap.k     = CapK::WheelSlot;
		cap.wheel = wheelOf();
		if( Pair( c, "slotNumber", s, e ) ) { cap.slotA = s->F(); cap.slotB = e->F(); }
	}
	else if( t == "WheelShake" )
	{
		cap.k     = CapK::WheelShake;
		cap.wheel = wheelOf();
		if( Pair( c, "slotNumber", s, e ) ) { cap.slotA = s->F(); cap.slotB = e->F(); }
		if( Pair( c, "shakeSpeed", s, e ) ) { cap.a = fabsf( RotSpeed( *s ) ) * 4; cap.b = fabsf( RotSpeed( *e ) ) * 4; }
		else { cap.a = 2; cap.b = 6; }
	}
	else if( t == "WheelRotation" || t == "WheelSlotRotation" || t == "PrismRotation" || t == "Rotation" )
	{
		cap.k     = t == "WheelRotation" ? CapK::WheelRot : ( t == "PrismRotation" ? CapK::PrismRot : CapK::WheelSlotRot );
		cap.wheel = t == "PrismRotation" ? -1 : wheelOf();
		if( Pair( c, "speed", s, e ) ) { cap.a = RotSpeed( *s ); cap.b = RotSpeed( *e ); }
		else if( Pair( c, "angle", s, e ) ) { cap.angleMode = true; cap.a = AngleDeg( *s, 0, 360 ); cap.b = AngleDeg( *e, 0, 360 ); }
		if( t == "Rotation" ) cap.k = CapK::Generic;
	}
	else if( t == "ColorPreset" )
	{
		cap.k = CapK::ColorPreset;
		auto readCols = [ & ]( const Json& arr, std::vector< V3 >& out ) {
			for( auto& x : arr.a )
			{
				V3 col;
				if( ParseHexColor( x.Str(), col ) )
					out.push_back( col );
			}
		};
		readCols( c[ "colors" ], cap.colors );
		if( cap.colors.empty() )
		{
			readCols( c[ "colorsStart" ], cap.colors );
			readCols( c[ "colorsEnd" ], cap.colorsEnd );
		}
		if( cap.colors.empty() && Pair( c, "colorTemperature", s, e ) )
		{
			cap.k = CapK::ColorTemp;
			cap.a = KelvinOf( *s );
			cap.b = KelvinOf( *e );
		}
		V3 g;
		if( cap.k == CapK::ColorPreset && cap.colors.empty() && GuessColorFromName( c[ "comment" ].Str(), g ) )
			cap.colors.push_back( g );
	}
	else if( t == "ColorTemperature" )
	{
		cap.k = CapK::ColorTemp;
		if( Pair( c, "colorTemperature", s, e ) ) { cap.a = KelvinOf( *s ); cap.b = KelvinOf( *e ); }
		else { cap.a = 6500; cap.b = 3200; }
	}
	else if( t == "Zoom" || t == "BeamAngle" )
	{
		cap.k = CapK::Zoom;
		float n = ctx.lensMin > 0 ? ctx.lensMin : 8, w = ctx.lensMax > 0 ? ctx.lensMax : 40;
		if( Pair( c, "angle", s, e ) ) { cap.a = AngleDeg( *s, n, w ); cap.b = AngleDeg( *e, n, w ); }
		else { cap.a = n; cap.b = w; }
	}
	else if( t == "Focus" )
	{
		cap.k = CapK::Focus;
		if( Pair( c, "distance", s, e ) ) { cap.a = Percentish( *s, 0 ); cap.b = Percentish( *e, 1 ); }
		else { cap.a = 0; cap.b = 1; }
	}
	else if( t == "Iris" )
	{
		cap.k = CapK::Iris;
		if( Pair( c, "openPercent", s, e ) ) { cap.a = Percentish( *s, 1 ); cap.b = Percentish( *e, 0 ); }
		else { cap.a = 1; cap.b = 0; }
	}
	else if( t == "Frost" )
	{
		cap.k = CapK::Frost;
		if( Pair( c, "frostIntensity", s, e ) ) { cap.a = Percentish( *s, 0 ); cap.b = Percentish( *e, 1 ); }
		else { cap.a = 0; cap.b = 1; }
	}
	else if( t == "Prism" )
	{
		cap.k      = CapK::Prism;
		cap.facets = 3;
		if( Pair( c, "speed", s, e ) ) { cap.a = RotSpeed( *s ); cap.b = RotSpeed( *e ); }
		else if( Pair( c, "angle", s, e ) ) { cap.angleMode = true; cap.a = AngleDeg( *s, 0, 360 ); cap.b = AngleDeg( *e, 0, 360 ); }
	}
	else if( t == "Fog" || t == "FogOutput" )
	{
		cap.k = CapK::Fog;
		if( Pair( c, "fogOutput", s, e ) ) { cap.a = Percentish( *s, 0 ); cap.b = Percentish( *e, 1 ); }
		else { cap.a = 0; cap.b = 1; }
	}
	else if( t == "Effect" )
	{
		std::string p = c[ "effectPreset" ].Str();
		if( p == "ColorJump" || p == "ColorFade" )
		{
			cap.k = p == "ColorJump" ? CapK::ColorJump : CapK::ColorFade;
			if( Pair( c, "speed", s, e ) ) { cap.a = fabsf( RotSpeed( *s ) ); cap.b = fabsf( RotSpeed( *e ) ); }
			else { cap.a = 0.3f; cap.b = 0.3f; }
			if( cap.a == 0 && cap.b == 0 ) cap.a = cap.b = 0.3f;
		}
		else
			cap.k = CapK::Generic;
		if( cap.label.empty() ) cap.label = c[ "effectName" ].Str();
	}
	else if( t == "NoFunction" )
		cap.k = CapK::None;
	else
		cap.k = CapK::Generic;

	if( cap.label.empty() )
	{
		cap.label = t;
		if( cap.k == CapK::WheelSlot && cap.wheel >= 0 )
		{
			int sl = (int)floorf( cap.slotA ) - 1;
			auto& w = d.wheels[ cap.wheel ];
			if( sl >= 0 && sl < (int)w.slots.size() )
			{
				cap.label = w.slots[ sl ].name;
				if( cap.slotB != cap.slotA ) cap.label += " ...";
			}
		}
		if( cap.label == "NoFunction" ) cap.label = "-";
	}
	return cap;
}

Attr AttrOfCap( const Cap& c, const FixtureDef& d, Attr emit )
{
	switch( c.k )
	{
	case CapK::Intensity: return Attr::Dimmer;
	case CapK::ColorIntensity: return emit;
	case CapK::ShutterOpen:
	case CapK::ShutterClosed:
	case CapK::Strobe:
	case CapK::StrobePulse:
	case CapK::StrobeRampUp:
	case CapK::StrobeRampDown:
	case CapK::StrobeRandom: return Attr::Shutter;
	case CapK::Pan:
	case CapK::PanCont: return Attr::Pan;
	case CapK::Tilt:
	case CapK::TiltCont: return Attr::Tilt;
	case CapK::PanTiltSpeed: return Attr::PanTiltSpeed;
	case CapK::WheelSlot:
	case CapK::WheelRot:
	case CapK::WheelShake:
		if( c.wheel >= 0 )
		{
			auto& w = d.wheels[ c.wheel ];
			if( w.IsColor() ) return Attr::ColorWheel;
			if( w.IsPrism() ) return Attr::Prism;
			return Attr::Gobo;
		}
		return Attr::Generic;
	case CapK::WheelSlotRot: return Attr::GoboRot;
	case CapK::ColorPreset: return Attr::ColorMacro;
	case CapK::ColorTemp: return Attr::CTO;
	case CapK::Zoom: return Attr::Zoom;
	case CapK::Focus: return Attr::Focus;
	case CapK::Iris: return Attr::Iris;
	case CapK::Frost: return Attr::Frost;
	case CapK::Prism: return Attr::Prism;
	case CapK::PrismRot: return Attr::PrismRot;
	case CapK::Fog: return Attr::Fog;
	case CapK::ColorJump:
	case CapK::ColorFade: return Attr::Effect;
	case CapK::Generic: return Attr::Generic;
	default: return Attr::None;
	}
}

bool BuildChannel( const Json& cj, const std::string& key, const std::string& pixel, OflCtx& ctx, Channel& ch )
{
	FixtureDef& d = *ctx.def;
	int res       = ChannelRes( cj );
	ch.key        = key;
	ch.name       = cj[ "name" ].Str( key );
	if( !pixel.empty() )
		ch.name = Replace( ch.name, "$pixelKey", pixel );
	//default value
	const Json& dv = cj[ "defaultValue" ];
	if( dv.IsNum() )
		ch.def16 = To16( dv.Num(), res );
	else if( dv.IsStr() )
		ch.def16 = (uint16_t)( Saturate( Percentish( dv, 0 ) ) * 65535.0f );
	std::vector< const Json* > caps;
	if( const Json* one = cj.Find( "capability" ) )
		caps.push_back( one );
	else
		for( auto& x : cj[ "capabilities" ].a )
			caps.push_back( &x );
	std::map< Attr, int > score;
	for( auto* cp : caps )
	{
		Attr emit = Attr::None;
		Cap c     = ParseCap( *cp, ch.name, ctx, res, emit );
		if( !cp->Find( "dmxRange" ) )
		{
			c.lo = 0;
			c.hi = 65535;
		}
		Attr a = AttrOfCap( c, d, emit );
		if( a != Attr::None && a != Attr::Generic )
			score[ a ] += 1 + ( c.hi - c.lo ) / 256;
		if( c.wheel >= 0 && ch.wheel < 0 )
			ch.wheel = c.wheel;
		ch.caps.push_back( std::move( c ) );
	}
	//pick dominant attribute
	int best = 0;
	ch.attr  = Attr::Generic;
	for( auto& kv : score )
		if( kv.second > best )
		{
			best    = kv.second;
			ch.attr = kv.first;
		}
	//a "Gobo Wheel Rotation" style channel that only has wheel rotation caps on a gobo wheel = gobo scroll; keep Gobo
	if( !pixel.empty() )
	{
		auto it = ctx.pixelIndex.find( pixel );
		if( it != ctx.pixelIndex.end() )
			ch.pixels.push_back( it->second );
		else
		{
			auto g = ctx.groupPixels.find( pixel );
			if( g != ctx.groupPixels.end() )
				ch.pixels = g->second;
		}
	}
	return true;
}

void BuildPixels( const Json& m, OflCtx& ctx )
{
	FixtureDef& d = *ctx.def;
	struct P { std::string key; int x, y, z; };
	std::vector< P > ps;
	if( const Json* pc = m.Find( "pixelCount" ) )
	{
		d.gx = std::max( 1, ( *pc )[ 0 ].I( 1 ) );
		d.gy = std::max( 1, ( *pc )[ 1 ].I( 1 ) );
		d.gz = std::max( 1, ( *pc )[ 2 ].I( 1 ) );
		int dims = ( d.gx > 1 ) + ( d.gy > 1 ) + ( d.gz > 1 );
		for( int z = 0; z < d.gz; ++z )
			for( int y = 0; y < d.gy; ++y )
				for( int x = 0; x < d.gx; ++x )
				{
					char b[ 64 ];
					if( dims <= 1 )
						snprintf( b, sizeof( b ), "%d", ( d.gx > 1 ? x : ( d.gy > 1 ? y : z ) ) + 1 );
					else if( d.gz == 1 )
						snprintf( b, sizeof( b ), "(%d, %d)", x + 1, y + 1 );
					else
						snprintf( b, sizeof( b ), "(%d, %d, %d)", x + 1, y + 1, z + 1 );
					ps.push_back( { b, x, y, z } );
				}
	}
	else if( const Json* pk = m.Find( "pixelKeys" ) )
	{
		d.gz = std::max< int >( 1, (int)pk->Size() );
		for( int z = 0; z < (int)pk->Size(); ++z )
		{
			const Json& rows = ( *pk )[ z ];
			d.gy             = std::max< int >( d.gy, (int)rows.Size() );
			for( int y = 0; y < (int)rows.Size(); ++y )
			{
				const Json& cols = rows[ y ];
				d.gx             = std::max< int >( d.gx, (int)cols.Size() );
				for( int x = 0; x < (int)cols.Size(); ++x )
					if( cols[ x ].IsStr() )
						ps.push_back( { cols[ x ].Str(), x, y, z } );
			}
		}
	}
	for( auto& p : ps )
	{
		Pixel px;
		px.key = p.key;
		px.pos = V3( ( p.x + 0.5f ) / d.gx - 0.5f, ( p.z + 0.5f ) / d.gz - 0.5f, ( p.y + 0.5f ) / d.gy - 0.5f );
		ctx.pixelIndex[ p.key ] = (int)d.pixels.size();
		d.pixels.push_back( px );
	}
	//groups
	if( const Json* g = m.Find( "pixelGroups" ) )
		for( auto& kv : g->o )
		{
			std::vector< int > idx;
			const Json& v = kv.second;
			if( v.IsStr() && v.s == "all" )
				for( int i = 0; i < (int)d.pixels.size(); ++i )
					idx.push_back( i );
			else if( v.IsArr() )
			{
				for( auto& k : v.a )
				{
					auto it = ctx.pixelIndex.find( k.Str() );
					if( it != ctx.pixelIndex.end() )
						idx.push_back( it->second );
				}
			}
			else if( v.IsObj() )
			{
				for( int i = 0; i < (int)ps.size(); ++i )
				{
					bool ok = true;
					for( auto& cons : v.o )
					{
						int coord = cons.first == "x" ? ps[ i ].x + 1 : ( cons.first == "y" ? ps[ i ].y + 1 : ps[ i ].z + 1 );
						for( auto& rule : cons.second.a )
						{
							std::string r = rule.Str();
							if( cons.first == "name" )
							{
								try { ok = ok && std::regex_search( ps[ i ].key, std::regex( r ) ); }
								catch( ... ) { ok = false; }
								continue;
							}
							if( r == "even" ) ok = ok && coord % 2 == 0;
							else if( r == "odd" ) ok = ok && coord % 2 == 1;
							else if( r.rfind( "<=", 0 ) == 0 ) ok = ok && coord <= atoi( r.c_str() + 2 );
							else if( r.rfind( ">=", 0 ) == 0 ) ok = ok && coord >= atoi( r.c_str() + 2 );
							else if( r.rfind( "<", 0 ) == 0 ) ok = ok && coord < atoi( r.c_str() + 1 );
							else if( r.rfind( ">", 0 ) == 0 ) ok = ok && coord > atoi( r.c_str() + 1 );
							else if( r.rfind( "=", 0 ) == 0 ) ok = ok && coord == atoi( r.c_str() + 1 );
							else if( r.find( 'n' ) != std::string::npos )
							{
								//"3n", "3n+1"
								int k = atoi( r.c_str() ), off = 0;
								size_t pl = r.find( '+' );
								if( pl != std::string::npos ) off = atoi( r.c_str() + pl + 1 );
								if( k > 0 ) ok = ok && ( coord - off ) % k == 0;
							}
						}
					}
					if( ok )
						idx.push_back( i );
				}
			}
			ctx.groupPixels[ kv.first ] = idx;
			d.groups[ kv.first ]        = idx;
		}
}

//order of pixel keys for a repeatFor value
std::vector< std::string > RepeatKeys( const Json& rf, OflCtx& ctx )
{
	FixtureDef& d = *ctx.def;
	std::vector< std::string > keys;
	if( rf.IsArr() )
	{
		for( auto& k : rf.a )
			keys.push_back( k.Str() );
		return keys;
	}
	std::string s = rf.Str();
	if( s == "eachPixelGroup" )
	{
		if( const Json* g = ( *ctx.root )[ "matrix" ].Find( "pixelGroups" ) )
			for( auto& kv : g->o )
				keys.push_back( kv.first );
		return keys;
	}
	std::vector< int > idx( d.pixels.size() );
	for( size_t i = 0; i < idx.size(); ++i )
		idx[ i ] = (int)i;
	if( s == "eachPixelABC" )
		std::sort( idx.begin(), idx.end(), [ & ]( int a, int b ) { return NaturalLess( d.pixels[ a ].key, d.pixels[ b ].key ); } );
	else if( s.size() == 12 && s.rfind( "eachPixel", 0 ) == 0 )
	{
		//first letter changes fastest
		auto coord = [ & ]( int i, char ax ) {
			const V3& p = d.pixels[ i ].pos;
			return ax == 'X' ? p.x : ( ax == 'Y' ? p.z : p.y );//pos: x, z(depth)=pixel y, y = pixel z
		};
		char a0 = s[ 9 ], a1 = s[ 10 ], a2 = s[ 11 ];
		std::stable_sort( idx.begin(), idx.end(), [ & ]( int a, int b ) {
			if( coord( a, a2 ) != coord( b, a2 ) ) return coord( a, a2 ) < coord( b, a2 );
			if( coord( a, a1 ) != coord( b, a1 ) ) return coord( a, a1 ) < coord( b, a1 );
			return coord( a, a0 ) < coord( b, a0 );
		} );
	}
	for( int i : idx )
		keys.push_back( d.pixels[ i ].key );
	return keys;
}

void AddRefs( OflCtx& ctx, const Json& cj, const std::string& key, const std::string& pixel )
{
	ctx.refs[ key ] = { &cj, key, pixel, 0 };
	int i           = 1;
	for( auto& fa : cj[ "fineChannelAliases" ].a )
	{
		std::string n = fa.Str();
		if( !pixel.empty() )
			n = Replace( n, "$pixelKey", pixel );
		ctx.refs[ n ] = { &cj, key, pixel, i++ };
	}
}

V3 Dims( const Json& phys, V3 def )
{
	const Json& dm = phys[ "dimensions" ];
	if( dm.Size() == 3 )
		return V3( dm[ 0 ].F() / 1000.0f, dm[ 1 ].F() / 1000.0f, dm[ 2 ].F() / 1000.0f );
	return def;
}
}//namespace

static Kind KindFromCategories( const std::vector< std::string >& cats, const V3& dims, int pixels )
{
	auto has = [ & ]( const char* c ) { return std::find( cats.begin(), cats.end(), c ) != cats.end(); };
	if( has( "Moving Head" ) ) return Kind::MovingHead;
	if( has( "Scanner" ) || has( "Barrel Scanner" ) ) return Kind::Scanner;
	if( has( "Smoke" ) || has( "Hazer" ) ) return Kind::Hazer;
	if( has( "Blinder" ) ) return Kind::Blinder;
	if( has( "Strobe" ) ) return Kind::Strobe;
	if( has( "Pixel Bar" ) || has( "Matrix" ) ) return Kind::Bar;
	if( has( "Laser" ) ) return Kind::Laser;
	if( has( "Dimmer" ) ) return Kind::Dimmer;
	if( has( "Color Changer" ) )
	{
		if( dims.x > 2.5f * std::max( dims.y, dims.z ) || pixels >= 4 ) return Kind::Bar;
		return Kind::Par;
	}
	return Kind::Generic;
}

static void KindDefaults( FixtureDef& d, bool hasDims, bool hasLens, bool hasBulb )
{
	V3 dm{ 0.25f, 0.3f, 0.25f };
	float bmin = 20, bmax = 20, lm = 2500, k = 6500;
	switch( d.kind )
	{
	case Kind::MovingHead: dm = { 0.38f, 0.55f, 0.28f }; bmin = 4; bmax = 30; lm = 15000; k = 7000; break;
	case Kind::Scanner: dm = { 0.3f, 0.25f, 0.7f }; bmin = 12; bmax = 12; lm = 6000; break;
	case Kind::Bar: dm = { 1.0f, 0.1f, 0.09f }; bmin = 25; bmax = 25; lm = 3000; break;
	case Kind::Strobe: dm = { 0.6f, 0.15f, 0.2f }; bmin = 90; bmax = 90; lm = 40000; k = 5800; break;
	case Kind::Blinder: dm = { 0.6f, 0.15f, 0.3f }; bmin = 55; bmax = 55; lm = 25000; k = 3000; break;
	case Kind::Hazer: dm = { 0.5f, 0.3f, 0.3f }; lm = 0; break;
	case Kind::Dimmer: dm = { 0.25f, 0.35f, 0.25f }; bmin = 25; bmax = 25; lm = 9000; k = 3100; break;
	case Kind::Laser: dm = { 0.2f, 0.15f, 0.3f }; bmin = 2; bmax = 2; lm = 3000; break;
	default: dm = { 0.25f, 0.3f, 0.25f }; bmin = 22; bmax = 22; lm = 3000; break;
	}
	if( !hasDims ) d.dims = dm;
	if( !hasLens ) { d.beamMin = bmin; d.beamMax = bmax; }
	if( !hasBulb ) { d.lumens = lm; d.kelvin = k; }
}

bool ParseOflFixture( const Json& j, const std::string& id, const std::string& mfName, FixtureDef& d, std::string* err )
{
	OflCtx ctx;
	ctx.root       = &j;
	ctx.def        = &d;
	d.id           = id;
	d.name         = j[ "name" ].Str( id );
	d.shortName    = j[ "shortName" ].Str( d.name );
	d.manufacturer = mfName;
	for( auto& c : j[ "categories" ].a )
		d.categories.push_back( c.Str() );
	const Json& phys = j[ "physical" ];
	bool hasDims     = phys[ "dimensions" ].Size() == 3;
	d.dims           = Dims( phys, d.dims );
	bool hasLens     = false, hasBulb = false;
	if( phys[ "lens" ][ "degreesMinMax" ].Size() == 2 )
	{
		ctx.lensMin = phys[ "lens" ][ "degreesMinMax" ][ 0 ].F();
		ctx.lensMax = phys[ "lens" ][ "degreesMinMax" ][ 1 ].F();
		d.beamMin   = std::max( 0.5f, ctx.lensMin );
		d.beamMax   = std::max( d.beamMin, ctx.lensMax );
		hasLens     = true;
	}
	if( phys[ "bulb" ].IsObj() )
	{
		if( phys[ "bulb" ][ "lumens" ].IsNum() ) { d.lumens = phys[ "bulb" ][ "lumens" ].F(); hasBulb = true; }
		if( phys[ "bulb" ][ "colorTemperature" ].IsNum() ) d.kelvin = phys[ "bulb" ][ "colorTemperature" ].F();
	}
	//wheels
	for( auto& kv : j[ "wheels" ].o )
	{
		Wheel w;
		w.name = kv.first;
		for( auto& s : kv.second[ "slots" ].a )
		{
			WheelSlot sl;
			std::string t = s[ "type" ].Str();
			sl.name       = s[ "name" ].Str();
			if( t == "Open" ) { sl.type = SlotT::Open; if( sl.name.empty() ) sl.name = "Open"; }
			else if( t == "Closed" ) { sl.type = SlotT::Closed; if( sl.name.empty() ) sl.name = "Closed"; }
			else if( t == "Color" )
			{
				sl.type = SlotT::Color;
				V3 acc{ 0, 0, 0 };
				int n = 0;
				for( auto& c : s[ "colors" ].a )
				{
					V3 col;
					if( ParseHexColor( c.Str(), col ) ) { acc += col; ++n; }
				}
				if( n ) { sl.color = acc / (float)n; sl.hasColor = true; }
				else if( s[ "colorTemperature" ].IsStr() || s[ "colorTemperature" ].IsNum() )
				{
					sl.kelvin = KelvinOf( s[ "colorTemperature" ] );
					sl.color  = Kelvin( sl.kelvin );
					sl.hasColor = true;
				}
				else if( !sl.name.empty() )
				{
					sl.hasColor = GuessColorFromName( sl.name, sl.color );
				}
				if( sl.name.empty() ) sl.name = "Color";
			}
			else if( t == "Gobo" || t == "AnimationGoboStart" || t == "AnimationGoboEnd" )
			{
				sl.type = t == "Gobo" ? SlotT::Gobo : SlotT::Anim;
				std::string res = s[ "resource" ].IsStr() ? s[ "resource" ].Str() : "";
				std::string key = sl.name.empty() ? res : sl.name;
				sl.gobo = Gobos().ByName( key.empty() ? std::to_string( w.slots.size() ) + w.name : key, Hash( id ) );
				if( sl.name.empty() ) sl.name = res.empty() ? "Gobo " + std::to_string( w.slots.size() ) : FileName( res );
			}
			else if( t == "Prism" )
			{
				sl.type   = SlotT::Prism;
				sl.facets = 3;
				for( auto& f : s[ "facets" ].a ) { (void)f; }
				if( s[ "facets" ].Size() ) sl.facets = (int)s[ "facets" ].Size();
				if( sl.name.empty() ) sl.name = std::to_string( sl.facets ) + "-facet Prism";
			}
			else if( t == "Iris" ) { sl.type = SlotT::Iris; if( sl.name.empty() ) sl.name = "Iris"; }
			else if( t == "Frost" ) { sl.type = SlotT::Frost; if( sl.name.empty() ) sl.name = "Frost"; }
			else { sl.type = SlotT::Other; if( sl.name.empty() ) sl.name = t; }
			w.slots.push_back( sl );
		}
		d.wheels.push_back( std::move( w ) );
	}
	//matrix
	if( j[ "matrix" ].IsObj() )
		BuildPixels( j[ "matrix" ], ctx );
	//channel refs
	for( auto& kv : j[ "availableChannels" ].o )
		AddRefs( ctx, kv.second, kv.first, "" );
	for( auto& kv : j[ "templateChannels" ].o )
	{
		std::vector< std::string > all;
		for( auto& p : d.pixels )
			all.push_back( p.key );
		for( auto& g : ctx.groupPixels )
			all.push_back( g.first );
		for( auto& pk : all )
			AddRefs( ctx, kv.second, Replace( kv.first, "$pixelKey", pk ), pk );
	}
	//modes
	for( auto& mj : j[ "modes" ].a )
	{
		Mode m;
		m.name      = mj[ "name" ].Str();
		m.shortName = mj[ "shortName" ].Str( m.name );
		std::vector< std::string > names;
		std::vector< bool > nulls;
		for( auto& e : mj[ "channels" ].a )
		{
			if( e.IsNull() ) { names.push_back( "" ); nulls.push_back( true ); }
			else if( e.IsStr() ) { names.push_back( e.Str() ); nulls.push_back( false ); }
			else if( e.IsObj() && e[ "insert" ].Str() == "matrixChannels" )
			{
				auto keys = RepeatKeys( e[ "repeatFor" ], ctx );
				std::vector< const Json* > tmpl;
				for( auto& t : e[ "templateChannels" ].a )
					tmpl.push_back( &t );
				bool perChannel = e[ "channelOrder" ].Str() == "perChannel";
				auto add = [ & ]( const Json* t, const std::string& k ) {
					if( t->IsNull() ) { names.push_back( "" ); nulls.push_back( true ); }
					else { names.push_back( Replace( t->Str(), "$pixelKey", k ) ); nulls.push_back( false ); }
				};
				if( perChannel )
					for( auto* t : tmpl ) for( auto& k : keys ) add( t, k );
				else
					for( auto& k : keys ) for( auto* t : tmpl ) add( t, k );
			}
		}
		m.ch.resize( names.size() );
		//pass 1: coarse channels
		for( size_t i = 0; i < names.size(); ++i )
		{
			Channel& c = m.ch[ i ];
			if( nulls[ i ] ) { c.name = "(unused)"; c.attr = Attr::None; continue; }
			auto it = ctx.refs.find( names[ i ] );
			if( it == ctx.refs.end() ) continue;//switching alias or unknown -> pass 3
			if( it->second.fineIdx == 0 )
				BuildChannel( *it->second.j, it->second.key, it->second.pixel, ctx, c );
		}
		//pass 2: fine bytes
		for( size_t i = 0; i < names.size(); ++i )
		{
			if( nulls[ i ] ) continue;
			auto it = ctx.refs.find( names[ i ] );
			if( it == ctx.refs.end() || it->second.fineIdx == 0 ) continue;
			Channel& c = m.ch[ i ];
			c.name     = names[ i ];
			c.isFine   = true;
			for( size_t k = 0; k < names.size(); ++k )
				if( !m.ch[ k ].isFine && m.ch[ k ].key == it->second.key && !m.ch[ k ].caps.empty() )
				{
					c.coarse = (int)k;
					c.attr   = m.ch[ k ].attr;
					c.pixels = m.ch[ k ].pixels;
					if( it->second.fineIdx == 1 ) m.ch[ k ].fine = (int)i;
					else m.ch[ k ].fine2 = (int)i;
					break;
				}
		}
		//pass 3: switching channels
		for( size_t i = 0; i < names.size(); ++i )
		{
			if( nulls[ i ] || ctx.refs.count( names[ i ] ) ) continue;
			Channel& c = m.ch[ i ];
			c.name     = names[ i ];
			c.attr     = Attr::Generic;
			for( size_t k = 0; k < names.size() && c.swCtrl < 0; ++k )
			{
				if( k == i ) continue;
				for( auto& cap : m.ch[ k ].caps )
					for( auto& sw : cap.sw )
						if( sw.first == names[ i ] )
						{
							c.swCtrl = (int)k;
							break;
						}
			}
			if( c.swCtrl < 0 ) continue;
			std::set< std::string > seen;
			for( auto& cap : m.ch[ c.swCtrl ].caps )
				for( auto& sw : cap.sw )
					if( sw.first == names[ i ] && !seen.count( sw.second ) )
					{
						seen.insert( sw.second );
						auto r = ctx.refs.find( sw.second );
						if( r == ctx.refs.end() || r->second.fineIdx != 0 ) continue;
						auto alt = std::make_shared< Channel >();
						BuildChannel( *r->second.j, r->second.key, r->second.pixel, ctx, *alt );
						c.swAlts.push_back( { sw.second, alt } );
					}
			if( !c.swAlts.empty() )
			{
				auto& first = *c.swAlts[ 0 ].second;
				c.caps      = first.caps;
				c.attr      = first.attr;
				c.def16     = first.def16;
				c.wheel     = first.wheel;
				c.pixels    = first.pixels;
				c.key       = names[ i ];
			}
		}
		d.modes.push_back( std::move( m ) );
	}
	if( d.modes.empty() )
	{
		if( err ) *err = "no modes";
		return false;
	}
	//derived properties
	bool rgb = false, cmy = false;
	float pmin = 1e9f, pmax = -1e9f, tmin = 1e9f, tmax = -1e9f, zmin = 1e9f, zmax = -1e9f;
	for( auto& m : d.modes )
		for( auto& c : m.ch )
		{
			if( c.attr == Attr::Red || c.attr == Attr::Green || c.attr == Attr::Blue ) rgb = true;
			if( c.attr == Attr::Cyan || c.attr == Attr::Magenta || c.attr == Attr::Yellow ) cmy = true;
			for( auto& cp : c.caps )
			{
				if( cp.k == CapK::Pan ) { pmin = std::min( { pmin, cp.a, cp.b } ); pmax = std::max( { pmax, cp.a, cp.b } ); }
				if( cp.k == CapK::Tilt ) { tmin = std::min( { tmin, cp.a, cp.b } ); tmax = std::max( { tmax, cp.a, cp.b } ); }
				if( cp.k == CapK::Zoom ) { zmin = std::min( { zmin, cp.a, cp.b } ); zmax = std::max( { zmax, cp.a, cp.b } ); }
			}
		}
	d.subtractive = cmy && !rgb;
	d.kind        = KindFromCategories( d.categories, d.dims, (int)d.pixels.size() );
	KindDefaults( d, hasDims, hasLens, hasBulb );
	if( pmax > pmin ) d.panRange = pmax - pmin;
	if( tmax > tmin ) d.tiltRange = tmax - tmin;
	if( zmax > zmin && zmin > 0 ) { d.beamMin = zmin; d.beamMax = zmax; }
	if( d.kind != Kind::MovingHead && d.kind != Kind::Scanner && pmax <= pmin ) { d.panRange = 0; d.tiltRange = 0; }
	return true;
}

//---------------------------------------------------------------- GDTF
namespace
{
//"128/1" -> 16 bit
uint16_t GdtfVal( const std::string& s, bool hi = false )
{
	if( s.empty() ) return 0;
	double v  = atof( s.c_str() );
	size_t sl = s.find( '/' );
	int n     = sl != std::string::npos ? atoi( s.c_str() + sl + 1 ) : 1;
	if( n <= 1 ) return (uint16_t)Clamp( v * 256.0 + ( hi ? 255 : 0 ), 0.0, 65535.0 );
	if( n == 2 ) return (uint16_t)Clamp( v, 0.0, 65535.0 );
	return (uint16_t)Clamp( v / pow( 256.0, n - 2 ), 0.0, 65535.0 );
}
V3 CieToRgb( const std::string& s )
{
	float x = 0.3127f, y = 0.329f, Y = 100.0f;
	sscanf( s.c_str(), "%f,%f,%f", &x, &y, &Y );
	if( y < 1e-4f ) return { 1, 1, 1 };
	float X = x / y, Z = ( 1 - x - y ) / y;//Y = 1
	V3 c( 3.2406f * X - 1.5372f - 0.4986f * Z, -0.9689f * X + 1.8758f + 0.0415f * Z, 0.0557f * X - 0.2040f + 1.0570f * Z );
	c = MaxV( c, V3( 0, 0, 0 ) );
	float m = std::max( { c.x, c.y, c.z, 1e-4f } );
	return c / m;
}
struct GAttr { const char* prefix; Attr a; CapK k; };
}//namespace

bool ParseGdtf( const std::string& path, FixtureDef& d, std::string* err )
{
	ZipArchive zip;
	if( !zip.Open( path ) )
	{
		if( err ) *err = "cannot open zip";
		return false;
	}
	const ZipArchive::Entry* de = zip.Find( "description.xml" );
	std::vector< uint8_t > xmlb;
	if( !de || !zip.Extract( *de, xmlb ) )
	{
		if( err ) *err = "description.xml missing";
		return false;
	}
	auto doc = ParseXml( std::string( xmlb.begin(), xmlb.end() ), err );
	if( !doc ) return false;
	std::vector< const XmlNode* > fts;
	doc->FindAll( "FixtureType", fts );
	if( fts.empty() )
	{
		if( err ) *err = "no FixtureType";
		return false;
	}
	const XmlNode* ft = fts[ 0 ];
	d.id              = "gdtf/" + FileName( path );
	d.name            = ft->Attr( "LongName" ).empty() ? ft->Attr( "Name" ) : ft->Attr( "LongName" );
	if( d.name.empty() ) d.name = FileStem( path );
	d.shortName    = ft->Attr( "ShortName" ).empty() ? d.name : ft->Attr( "ShortName" );
	d.manufacturer = ft->Attr( "Manufacturer" );
	d.source       = path;
	//wheels
	if( const XmlNode* ws = ft->Child( "Wheels" ) )
		for( auto* w : ws->Children( "Wheel" ) )
		{
			Wheel wh;
			wh.name = w->Attr( "Name" );
			for( auto* s : w->Children( "Slot" ) )
			{
				WheelSlot sl;
				sl.name             = s->Attr( "Name" );
				std::string lname   = ToLower( sl.name );
				std::string media   = s->Attr( "MediaFileName" );
				auto facets         = s->Children( "Facet" );
				if( !facets.empty() ) { sl.type = SlotT::Prism; sl.facets = (int)facets.size(); }
				else if( !media.empty() )
				{
					sl.type = SlotT::Gobo;
					sl.gobo = Gobos().ByName( sl.name, Hash( d.id ) );
					//try to load the gobo image from the archive
					for( const char* ext : { ".png", ".jpg", ".svg" } )
					{
						const ZipArchive::Entry* ge = zip.Find( "wheels/" + media + ext );
						if( !ge || std::string( ext ) == ".svg" ) continue;
						std::vector< uint8_t > img;
						if( !zip.Extract( *ge, img ) ) continue;
						int w2, h2, n2;
						unsigned char* px = stbi_load_from_memory( img.data(), (int)img.size(), &w2, &h2, &n2, 1 );
						if( !px ) continue;
						std::vector< uint8_t > g( kGoboSize * kGoboSize );
						for( int y = 0; y < kGoboSize; ++y )
							for( int x = 0; x < kGoboSize; ++x )
							{
								float u = ( x + 0.5f ) / kGoboSize * 2 - 1, v = ( y + 0.5f ) / kGoboSize * 2 - 1;
								int sx = Clamp( (int)( ( x + 0.5f ) / kGoboSize * w2 ), 0, w2 - 1 );
								int sy = Clamp( (int)( ( y + 0.5f ) / kGoboSize * h2 ), 0, h2 - 1 );
								g[ y * kGoboSize + x ] = u * u + v * v > 0.97f ? 0 : px[ sy * w2 + sx ];
							}
						stbi_image_free( px );
						sl.gobo = Gobos().Add( d.id + "/" + media, std::move( g ) );
						break;
					}
				}
				else if( lname.find( "open" ) != std::string::npos ) sl.type = SlotT::Open;
				else if( lname.find( "closed" ) != std::string::npos ) sl.type = SlotT::Closed;
				else if( s->Has( "Color" ) )
				{
					sl.type     = SlotT::Color;
					sl.color    = CieToRgb( s->Attr( "Color" ) );
					sl.hasColor = true;
				}
				else sl.type = SlotT::Other;
				if( sl.type == SlotT::Open && s->Has( "Color" ) )
				{
					V3 c = CieToRgb( s->Attr( "Color" ) );
					if( std::min( { c.x, c.y, c.z } ) < 0.8f ) { sl.type = SlotT::Color; sl.color = c; sl.hasColor = true; }
				}
				wh.slots.push_back( sl );
			}
			d.wheels.push_back( std::move( wh ) );
		}
	//beam
	std::vector< const XmlNode* > beams;
	ft->FindAll( "Beam", beams );
	bool hasLens = false, hasBulb = false;
	if( !beams.empty() )
	{
		float ba = (float)atof( beams[ 0 ]->Attr( "BeamAngle" ).c_str() );
		if( ba > 0 ) { d.beamMin = d.beamMax = ba; hasLens = true; }
		float lf = (float)atof( beams[ 0 ]->Attr( "LuminousFlux" ).c_str() );
		if( lf > 0 ) { d.lumens = lf; hasBulb = true; }
		float ct = (float)atof( beams[ 0 ]->Attr( "ColorTemperature" ).c_str() );
		if( ct > 0 ) d.kelvin = ct;
	}
	static const GAttr map[] = {
		{ "Dimmer", Attr::Dimmer, CapK::Intensity },
		{ "PanRotate", Attr::Pan, CapK::PanCont },
		{ "TiltRotate", Attr::Tilt, CapK::TiltCont },
		{ "Pan", Attr::Pan, CapK::Pan },
		{ "Tilt", Attr::Tilt, CapK::Tilt },
		{ "Shutter1StrobeRandom", Attr::Shutter, CapK::StrobeRandom },
		{ "Shutter1StrobePulse", Attr::Shutter, CapK::StrobePulse },
		{ "Shutter1StrobeRampUp", Attr::Shutter, CapK::StrobeRampUp },
		{ "Shutter1StrobeRampDown", Attr::Shutter, CapK::StrobeRampDown },
		{ "Shutter1Strobe", Attr::Shutter, CapK::Strobe },
		{ "Shutter1", Attr::Shutter, CapK::ShutterOpen },
		{ "ColorAdd_RY", Attr::Amber, CapK::ColorIntensity },
		{ "ColorAdd_GY", Attr::Lime, CapK::ColorIntensity },
		{ "ColorAdd_BC", Attr::Cyan, CapK::ColorIntensity },
		{ "ColorAdd_BM", Attr::Indigo, CapK::ColorIntensity },
		{ "ColorAdd_WW", Attr::WarmWhite, CapK::ColorIntensity },
		{ "ColorAdd_CW", Attr::ColdWhite, CapK::ColorIntensity },
		{ "ColorAdd_UV", Attr::UV, CapK::ColorIntensity },
		{ "ColorAdd_R", Attr::Red, CapK::ColorIntensity },
		{ "ColorAdd_G", Attr::Green, CapK::ColorIntensity },
		{ "ColorAdd_B", Attr::Blue, CapK::ColorIntensity },
		{ "ColorAdd_W", Attr::White, CapK::ColorIntensity },
		{ "ColorAdd_A", Attr::Amber, CapK::ColorIntensity },
		{ "ColorAdd_L", Attr::Lime, CapK::ColorIntensity },
		{ "ColorAdd_C", Attr::Cyan, CapK::ColorIntensity },
		{ "ColorAdd_M", Attr::Magenta, CapK::ColorIntensity },
		{ "ColorAdd_Y", Attr::Yellow, CapK::ColorIntensity },
		{ "ColorRGB_Red", Attr::Red, CapK::ColorIntensity },
		{ "ColorRGB_Green", Attr::Green, CapK::ColorIntensity },
		{ "ColorRGB_Blue", Attr::Blue, CapK::ColorIntensity },
		{ "ColorSub_C", Attr::Cyan, CapK::ColorIntensity },
		{ "ColorSub_M", Attr::Magenta, CapK::ColorIntensity },
		{ "ColorSub_Y", Attr::Yellow, CapK::ColorIntensity },
		{ "CTO", Attr::CTO, CapK::ColorTemp },
		{ "CTC", Attr::CTO, CapK::ColorTemp },
		{ "CTB", Attr::CTO, CapK::ColorTemp },
		{ "ColorMacro", Attr::ColorMacro, CapK::ColorPreset },
		{ "Color1WheelSpin", Attr::ColorWheel, CapK::WheelRot },
		{ "Color2WheelSpin", Attr::ColorWheel, CapK::WheelRot },
		{ "Color1", Attr::ColorWheel, CapK::WheelSlot },
		{ "Color2", Attr::ColorWheel, CapK::WheelSlot },
		{ "Gobo1SelectShake", Attr::Gobo, CapK::WheelShake },
		{ "Gobo2SelectShake", Attr::Gobo, CapK::WheelShake },
		{ "Gobo1WheelSpin", Attr::Gobo, CapK::WheelRot },
		{ "Gobo2WheelSpin", Attr::Gobo, CapK::WheelRot },
		{ "Gobo1PosRotate", Attr::GoboRot, CapK::WheelSlotRot },
		{ "Gobo2PosRotate", Attr::GoboRot, CapK::WheelSlotRot },
		{ "Gobo1Pos", Attr::GoboRot, CapK::WheelSlotRot },
		{ "Gobo2Pos", Attr::GoboRot, CapK::WheelSlotRot },
		{ "Gobo1", Attr::Gobo, CapK::WheelSlot },
		{ "Gobo2", Attr::Gobo, CapK::WheelSlot },
		{ "Prism1PosRotate", Attr::PrismRot, CapK::PrismRot },
		{ "Prism1Pos", Attr::PrismRot, CapK::PrismRot },
		{ "Prism1", Attr::Prism, CapK::Prism },
		{ "Zoom", Attr::Zoom, CapK::Zoom },
		{ "Focus", Attr::Focus, CapK::Focus },
		{ "Iris", Attr::Iris, CapK::Iris },
		{ "Frost", Attr::Frost, CapK::Frost },
		{ "Fog", Attr::Fog, CapK::Fog },
		{ "Haze", Attr::Fog, CapK::Fog },
		{ "PositionMSpeed", Attr::PanTiltSpeed, CapK::PanTiltSpeed },
		{ "PanTiltSpeed", Attr::PanTiltSpeed, CapK::PanTiltSpeed },
		{ "MSpeed", Attr::PanTiltSpeed, CapK::PanTiltSpeed },
	};
	auto lookup = [ & ]( const std::string& a ) -> const GAttr* {
		for( auto& m : map )
			if( a.rfind( m.prefix, 0 ) == 0 )
				return &m;
		return nullptr;
	};
	bool cmy = false, rgb = false, hasPan = false;
	float pmin = 1e9f, pmax = -1e9f, tmin = 1e9f, tmax = -1e9f;
	const XmlNode* dms = ft->Child( "DMXModes" );
	if( dms )
		for( auto* dm : dms->Children( "DMXMode" ) )
		{
			Mode m;
			m.name = m.shortName = dm->Attr( "Name" );
			const XmlNode* chans = dm->Child( "DMXChannels" );
			if( !chans ) continue;
			struct Tmp { std::vector< int > offs; Channel ch; };
			std::vector< Tmp > tmp;
			int footprint = 0;
			for( auto* dc : chans->Children( "DMXChannel" ) )
			{
				Tmp t;
				std::string off = dc->Attr( "Offset" );
				if( off.empty() && dc->Has( "Coarse" ) )
				{
					off = dc->Attr( "Coarse" );
					if( dc->Has( "Fine" ) ) off += "," + dc->Attr( "Fine" );
					if( dc->Has( "Ultra" ) ) off += "," + dc->Attr( "Ultra" );
				}
				if( off.empty() || off == "None" ) continue;
				for( size_t p = 0; p < off.size(); )
				{
					t.offs.push_back( atoi( off.c_str() + p ) - 1 );
					size_t c = off.find( ',', p );
					if( c == std::string::npos ) break;
					p = c + 1;
				}
				for( int o : t.offs ) footprint = std::max( footprint, o + 1 );
				const XmlNode* lc = dc->Child( "LogicalChannel" );
				if( !lc ) continue;
				std::string la = lc->Attr( "Attribute" );
				t.ch.name      = la;
				t.ch.key       = la;
				auto fns       = lc->Children( "ChannelFunction" );
				for( size_t fi = 0; fi < fns.size(); ++fi )
				{
					auto* fn         = fns[ fi ];
					std::string fa   = fn->Attr( "Attribute" );
					const GAttr* ga  = lookup( fa );
					uint16_t lo      = GdtfVal( fn->Attr( "DMXFrom" ) );
					uint16_t hi      = fi + 1 < fns.size() ? (uint16_t)std::max< int >( lo, GdtfVal( fns[ fi + 1 ]->Attr( "DMXFrom" ) ) - 1 ) : 65535;
					float pf         = fn->Has( "PhysicalFrom" ) ? (float)atof( fn->Attr( "PhysicalFrom" ).c_str() ) : 0.0f;
					float pt         = fn->Has( "PhysicalTo" ) ? (float)atof( fn->Attr( "PhysicalTo" ).c_str() ) : 1.0f;
					std::string wn   = fn->Attr( "Wheel" );
					int wi           = WheelIndex( d, wn );
					if( fi == 0 && fn->Has( "Default" ) ) t.ch.def16 = GdtfVal( fn->Attr( "Default" ) );
					if( fi == 0 && dc->Has( "Default" ) ) t.ch.def16 = GdtfVal( dc->Attr( "Default" ) );
					Cap base;
					base.lo    = lo;
					base.hi    = hi;
					base.k     = ga ? ga->k : CapK::Generic;
					base.label = fn->Attr( "Name" );
					base.wheel = wi;
					base.a     = pf;
					base.b     = pt;
					Attr at    = ga ? ga->a : Attr::Generic;
					switch( base.k )
					{
					case CapK::ShutterOpen:
					{
						std::string nm = ToLower( base.label );
						if( nm.find( "close" ) != std::string::npos || ( pf == 0 && pt == 0 ) ) base.k = CapK::ShutterClosed;
						break;
					}
					case CapK::ColorIntensity:
						if( fa.rfind( "ColorSub", 0 ) == 0 ) cmy = true;
						else rgb = true;
						base.a = 0; base.b = 1;
						break;
					case CapK::ColorTemp:
						if( pt < 100 ) { base.a = Lerp( 6500, 3200, pf ); base.b = Lerp( 6500, 3200, pt ); }
						if( fa == "CTB" ) std::swap( base.a, base.b );
						break;
					case CapK::Pan: hasPan = true; pmin = std::min( { pmin, pf, pt } ); pmax = std::max( { pmax, pf, pt } ); break;
					case CapK::Tilt: tmin = std::min( { tmin, pf, pt } ); tmax = std::max( { tmax, pf, pt } ); break;
					case CapK::WheelSlotRot:
					case CapK::PrismRot:
						base.angleMode = fa.find( "Rotate" ) == std::string::npos;
						if( !base.angleMode ) { base.a = pf; base.b = pt; }
						break;
					case CapK::Intensity: base.a = 0; base.b = 1; break;
					case CapK::Prism: base.facets = 3; break;
					case CapK::Iris: base.a = 1 - pf; base.b = 1 - pt; if( pt > 1 ) { base.a = 1; base.b = 0; } break;
					default: break;
					}
					auto sets = fn->Children( "ChannelSet" );
					bool usedSets = false;
					if( ( base.k == CapK::WheelSlot || base.k == CapK::Prism || base.k == CapK::WheelShake ) && !sets.empty() )
					{
						for( size_t si = 0; si < sets.size(); ++si )
						{
							Cap c        = base;
							c.lo         = std::max( lo, GdtfVal( sets[ si ]->Attr( "DMXFrom" ) ) );
							c.hi         = si + 1 < sets.size() ? (uint16_t)std::max< int >( c.lo, GdtfVal( sets[ si + 1 ]->Attr( "DMXFrom" ) ) - 1 ) : hi;
							int slot     = atoi( sets[ si ]->Attr( "WheelSlotIndex" ).c_str() );
							c.slotA = c.slotB = (float)std::max( 1, slot );
							c.label      = sets[ si ]->Attr( "Name" );
							if( c.k == CapK::Prism && wi >= 0 && slot >= 1 && slot <= (int)d.wheels[ wi ].slots.size() )
							{
								c.facets = d.wheels[ wi ].slots[ slot - 1 ].facets;
								if( c.facets <= 1 ) c.k = CapK::None;
							}
							if( c.label.empty() && wi >= 0 && slot >= 1 && slot <= (int)d.wheels[ wi ].slots.size() ) c.label = d.wheels[ wi ].slots[ slot - 1 ].name;
							t.ch.caps.push_back( c );
						}
						usedSets = true;
					}
					if( !usedSets )
					{
						if( base.k == CapK::WheelSlot ) { base.slotA = base.slotB = 1; }
						t.ch.caps.push_back( base );
					}
					if( fi == 0 || t.ch.attr == Attr::Generic || t.ch.attr == Attr::None )
						if( at != Attr::Generic || fi == 0 ) t.ch.attr = at;
					if( wi >= 0 && t.ch.wheel < 0 ) t.ch.wheel = wi;
				}
				tmp.push_back( std::move( t ) );
			}
			m.ch.resize( footprint );
			for( auto& c : m.ch ) { c.name = "(unused)"; c.attr = Attr::None; }
			for( auto& t : tmp )
			{
				if( t.offs.empty() ) continue;
				int co   = t.offs[ 0 ];
				m.ch[ co ] = t.ch;
				for( size_t k = 1; k < t.offs.size() && k < 3; ++k )
				{
					int fo = t.offs[ k ];
					Channel& f = m.ch[ fo ];
					f.name   = t.ch.name + " fine";
					f.isFine = true;
					f.coarse = co;
					f.attr   = t.ch.attr;
					if( k == 1 ) m.ch[ co ].fine = fo;
					else m.ch[ co ].fine2 = fo;
				}
			}
			d.modes.push_back( std::move( m ) );
		}
	if( d.modes.empty() )
	{
		if( err ) *err = "no DMX modes";
		return false;
	}
	d.subtractive = cmy && !rgb;
	d.kind        = hasPan ? Kind::MovingHead : Kind::Par;
	KindDefaults( d, false, hasLens, hasBulb );
	if( pmax > pmin ) d.panRange = pmax - pmin;
	if( tmax > tmin ) d.tiltRange = tmax - tmin;
	if( !hasPan ) d.panRange = d.tiltRange = 0;
	for( auto& m : d.modes )
		for( auto& c : m.ch )
			for( auto& cp : c.caps )
				if( cp.k == CapK::Zoom && cp.a > 0 && cp.b > 0 ) { d.beamMin = std::min( cp.a, cp.b ); d.beamMax = std::max( cp.a, cp.b ); }
	return true;
}

//---------------------------------------------------------------- library
void Library::LoadAll( const std::string& root )
{
	Gobos().Init();
	double t0 = Now();
	int ok = 0, bad = 0;
	auto loadDir = [ & ]( const std::string& dir, const std::string& prefix ) {
		std::string mtxt;
		Json mj;
		if( ReadText( dir + "\\manufacturers.json", mtxt ) && ParseJson( mtxt, mj ) )
			for( auto& kv : mj.o )
				mfNames[ kv.first ] = kv.second[ "name" ].Str( kv.first );
		for( auto& f : ListFiles( dir, ".json", true ) )
		{
			std::string rel = f.substr( dir.size() + 1 );
			if( rel == "manufacturers.json" || rel.find( '\\' ) == std::string::npos ) continue;
			std::string mf  = rel.substr( 0, rel.find( '\\' ) );
			std::string txt, err;
			Json j;
			if( !ReadText( f, txt ) || !ParseJson( txt, j, &err ) ) { ++bad; Log( "fixture parse error %s: %s", f.c_str(), err.c_str() ); continue; }
			if( j.Find( "redirectTo" ) ) continue;
			auto d    = std::make_unique< FixtureDef >();
			d->source = f;
			auto mfIt = mfNames.find( mf );
			if( ParseOflFixture( j, prefix + mf + "/" + FileStem( f ), mfIt != mfNames.end() ? mfIt->second : mf, *d, &err ) )
			{
				defs.push_back( std::move( d ) );
				++ok;
			}
			else { ++bad; Log( "fixture error %s: %s", f.c_str(), err.c_str() ); }
		}
	};
	loadDir( root + "\\user", "user/" );//own fixtures from the fixture editor
	loadDir( root + "\\generic", "generic/" );
	loadDir( root + "\\ofl", "ofl/" );
	for( auto& f : ListFiles( root + "\\gdtf", ".gdtf", true ) )
	{
		auto d = std::make_unique< FixtureDef >();
		std::string err;
		if( ParseGdtf( f, *d, &err ) ) { defs.push_back( std::move( d ) ); ++ok; }
		else { ++bad; Log( "gdtf error %s: %s", f.c_str(), err.c_str() ); }
	}
	Log( "library: %d fixtures loaded, %d failed, %.0f ms", ok, bad, ( Now() - t0 ) * 1000.0 );
}
const FixtureDef* Library::Find( const std::string& id ) const
{
	for( auto& d : defs )
		if( d->id == id )
			return d.get();
	return nullptr;
}
const FixtureDef* Library::AddOrReplace( std::unique_ptr< FixtureDef > d )
{
	for( auto& e : defs )
		if( e->id == d->id )
		{
			*e = std::move( *d );
			return e.get();
		}
	defs.push_back( std::move( d ) );
	return defs.back().get();
}
const FixtureDef* Library::LoadGdtf( const std::string& path )
{
	auto d = std::make_unique< FixtureDef >();
	if( !ParseGdtf( path, *d, &lastError ) )
		return nullptr;
	if( auto* e = Find( d->id ) )
		return e;
	defs.push_back( std::move( d ) );
	return defs.back().get();
}
}//namespace ls
