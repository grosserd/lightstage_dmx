#include "fixture_editor.h"
#include <ctime>

namespace ls
{
static const char* const kEdTypeNames[ (int)EdType::Count ] = {
	"Dimmer", "Shutter", "Strobe", "Shutter/Strobe",
	"Rot", "Grün", "Blau", "Weiß", "Warmweiß", "Kaltweiß", "Amber", "UV", "Lime", "Cyan", "Magenta", "Gelb",
	"CTO", "Farbrad", "Farbmakro",
	"Pan", "Tilt", "Pan/Tilt-Speed",
	"Zoom", "Fokus", "Iris", "Frost",
	"Goborad", "Gobo-Rotation", "Prisma", "Prisma-Rotation",
	"Nebel", "Lüfter", "Effekt", "Effekt-Speed", "Steuerung", "Ungenutzt", "Generisch" };
const char* EdTypeName( EdType t ) { return (int)t < (int)EdType::Count ? Ls( kEdTypeNames[ (int)t ] ) : "?"; }//UI language
const char* const kEdGroupNames[ 6 ] = { "Helligkeit", "Farbe", "Bewegung", "Strahl", "Räder / Effekte", "Sonstiges" };
int EdTypeGroup( EdType t )
{
	if( t <= EdType::ShutterStrobe ) return 0;
	if( t <= EdType::ColorMacro ) return 1;
	if( t <= EdType::PanTiltSpeed ) return 2;
	if( t <= EdType::Frost ) return 3;
	if( t <= EdType::PrismRotation ) return 4;
	return 5;
}
bool EdTypeIsColor( EdType t ) { return t >= EdType::Red && t <= EdType::Yellow; }
const char* const kEdCategories[ 10 ] = { "PAR / Wash", "Moving Head", "Scanner", "Bar / Pixel", "Strobe", "Blinder", "Hazer / Nebel", "Dimmer / Halogen", "Laser", "Effekt" };
static const char* const kOflCategories[ 10 ] = { "Color Changer", "Moving Head", "Scanner", "Pixel Bar", "Strobe", "Blinder", "Hazer", "Dimmer", "Laser", "Effect" };
static const char* OflColor( EdType t )
{
	switch( t )
	{
	case EdType::Red: return "Red";
	case EdType::Green: return "Green";
	case EdType::Blue: return "Blue";
	case EdType::White: return "White";
	case EdType::WarmWhite: return "Warm White";
	case EdType::ColdWhite: return "Cold White";
	case EdType::Amber: return "Amber";
	case EdType::UV: return "UV";
	case EdType::Lime: return "Lime";
	case EdType::Cyan: return "Cyan";
	case EdType::Magenta: return "Magenta";
	case EdType::Yellow: return "Yellow";
	default: return "White";
	}
}
//English names for pixel template channels ("Red $pixelKey")
static const char* PixelName( EdType t )
{
	if( EdTypeIsColor( t ) ) return OflColor( t );
	if( t == EdType::Dimmer ) return "Dimmer";
	if( t == EdType::Strobe || t == EdType::Shutter || t == EdType::ShutterStrobe ) return "Strobe";
	return "Generic";
}

int EdFixture::Footprint( int mode ) const
{
	if( mode < 0 || mode >= (int)modes.size() ) return 0;
	int n = 0;
	for( int i : modes[ mode ].ch )
		if( i >= 0 && i < (int)pool.size() ) n += pool[ i ].fine16 ? 2 : 1;
	if( pixels > 0 && modes[ mode ].pixels ) n += pixels * (int)pixelChannels.size();
	return n;
}

static std::vector< EdRange > ShutterTemplate()
{
	return { { 0, 7, 0, L( "zu" ) }, { 8, 15, 1, L( "offen" ) }, { 16, 131, 2, L( "Strobe langsam -> schnell" ) }, { 132, 139, 1, L( "offen" ) },
	         { 140, 181, 3, L( "Puls" ) }, { 182, 189, 1, L( "offen" ) }, { 190, 231, 4, L( "Zufall" ) }, { 232, 255, 1, L( "offen" ) } };
}

EdChannel MakeChannel( EdType t, const std::string& name )
{
	EdChannel c;
	c.type = t;
	c.name = name.empty() ? EdTypeName( t ) : name;
	switch( t )
	{
	case EdType::Pan: c.b = 540; c.fine16 = true; c.def = 128; break;
	case EdType::Tilt: c.b = 270; c.fine16 = true; c.def = 128; break;
	case EdType::Zoom: c.a = 10; c.b = 40; break;
	case EdType::CTO: c.a = 6500; c.b = 3200; break;
	case EdType::Strobe: c.a = 1; c.b = 20; break;
	case EdType::ShutterStrobe: c.ranges = ShutterTemplate(); c.def = 255; break;
	case EdType::Shutter: c.def = 255; break;
	case EdType::ColorWheel:
	{
		const struct { const char* n; V3 c; } cs[] = { { L( "Rot" ), { 1, 0, 0 } }, { "Orange", { 1, 0.45f, 0 } }, { L( "Gelb" ), { 1, 0.9f, 0 } }, { L( "Grün" ), { 0, 1, 0.1f } },
		                                                { "Cyan", { 0, 0.9f, 1 } }, { L( "Blau" ), { 0, 0.15f, 1 } }, { "Magenta", { 1, 0, 0.8f } } };
		c.slots.push_back( EdSlot{ L( "Offen" ), V3( 1, 1, 1 ), true } );
		for( auto& x : cs ) c.slots.push_back( EdSlot{ x.n, x.c, false } );
		break;
	}
	case EdType::ColorMacro:
	{
		const struct { const char* n; V3 c; } cs[] = { { L( "Rot" ), { 1, 0, 0 } }, { L( "Grün" ), { 0, 1, 0 } }, { L( "Blau" ), { 0, 0, 1 } }, { L( "Gelb" ), { 1, 1, 0 } },
		                                                { "Cyan", { 0, 1, 1 } }, { "Magenta", { 1, 0, 1 } }, { L( "Weiß" ), { 1, 1, 1 } } };
		for( auto& x : cs ) c.slots.push_back( EdSlot{ x.n, x.c, false } );
		break;
	}
	case EdType::GoboWheel:
		c.slots.push_back( EdSlot{ L( "Offen" ), V3( 1, 1, 1 ), true } );
		for( int i = 1; i <= 6; ++i ) c.slots.push_back( EdSlot{ "Gobo " + std::to_string( i ), V3( 1, 1, 1 ), false } );
		break;
	default: break;
	}
	return c;
}

std::string UniqueChannelName( const EdFixture& f, const std::string& base )
{
	auto used = [ & ]( const std::string& n ) {
		for( auto& c : f.pool )
			if( c.name == n || c.name + " fine" == n ) return true;
		return false;
	};
	if( !used( base ) ) return base;
	for( int i = 2;; ++i )
		if( !used( base + " " + std::to_string( i ) ) ) return base + " " + std::to_string( i );
}

const char* const kEdTemplates[ 8 ] = { "RGB", "RGBW", "RGBWA+UV", "Dimmer + RGB + Strobe", "Moving Head Spot (16 Kanäle)", "Moving Head Wash (14 Kanäle)", "Dimmer (1 Kanal)", "Hazer (Nebel + Lüfter)" };
void ApplyTemplate( EdFixture& f, int mode, int tmpl )
{
	if( mode < 0 || mode >= (int)f.modes.size() ) return;
	std::vector< EdType > ts;
	using E = EdType;
	switch( tmpl )
	{
	case 0: ts = { E::Red, E::Green, E::Blue }; break;
	case 1: ts = { E::Red, E::Green, E::Blue, E::White }; break;
	case 2: ts = { E::Red, E::Green, E::Blue, E::White, E::Amber, E::UV }; break;
	case 3: ts = { E::Dimmer, E::Red, E::Green, E::Blue, E::Strobe }; break;
	case 4: ts = { E::Pan, E::Tilt, E::PanTiltSpeed, E::Dimmer, E::ShutterStrobe, E::ColorWheel, E::GoboWheel, E::GoboRotation, E::Prism, E::PrismRotation, E::Focus, E::Zoom, E::Frost, E::Control }; break;
	case 5: ts = { E::Pan, E::Tilt, E::PanTiltSpeed, E::Dimmer, E::ShutterStrobe, E::Red, E::Green, E::Blue, E::White, E::CTO, E::Zoom, E::Control }; break;
	case 6: ts = { E::Dimmer }; break;
	case 7: ts = { E::Fog, E::FanSpeed }; break;
	default: return;
	}
	if( ( tmpl == 4 || tmpl == 5 ) && f.category == 0 ) f.category = 1;
	if( tmpl == 7 ) f.category = 6;
	for( auto t : ts )
	{
		EdChannel c = MakeChannel( t );
		c.name      = UniqueChannelName( f, c.name );
		f.pool.push_back( c );
		f.modes[ mode ].ch.push_back( (int)f.pool.size() - 1 );
	}
}

//---------------------------------------------------------------- OFL export
static std::string Hex( const V3& c )
{
	char b[ 16 ];
	snprintf( b, sizeof( b ), "#%02x%02x%02x", (int)( Saturate( c.x ) * 255 + 0.5f ), (int)( Saturate( c.y ) * 255 + 0.5f ), (int)( Saturate( c.z ) * 255 + 0.5f ) );
	return b;
}
static std::string Fmt( float v, const char* unit )
{
	char b[ 32 ];
	snprintf( b, sizeof( b ), "%g%s", v, unit );
	return b;
}
static Json Range( int a, int b )
{
	Json r = Json::Arr();
	r.Push( a );
	r.Push( b );
	return r;
}
static Json Cap( const char* type )
{
	Json c = Json::Obj();
	c.Set( "type", type );
	return c;
}
//equal DMX segments for n slots inside [lo, hi]
static void Segments( int n, int lo, int hi, std::vector< std::pair< int, int > >& out )
{
	out.clear();
	if( n <= 0 ) return;
	float w = ( hi - lo + 1 ) / (float)n;
	for( int i = 0; i < n; ++i )
	{
		int a = lo + (int)( i * w + 0.5f ), b = i == n - 1 ? hi : lo + (int)( ( i + 1 ) * w + 0.5f ) - 1;
		out.push_back( { a, std::max( a, b ) } );
	}
}

static Json ChannelJson( const EdChannel& c, const EdFixture& f, Json& wheels )
{
	Json ch = Json::Obj();
	if( c.fine16 )
	{
		Json fa = Json::Arr();
		fa.Push( c.name + " fine" );
		ch.Set( "fineChannelAliases", fa );
	}
	if( c.def != 0 ) ch.Set( "defaultValue", c.fine16 ? c.def * 256 : c.def );
	auto one = [ & ]( Json cap ) { ch.Set( "capability", cap ); };
	auto two = [ & ]( Json& cap, const char* k, const std::string& s, const std::string& e ) {
		cap.Set( std::string( k ) + "Start", c.invert ? e : s );
		cap.Set( std::string( k ) + "End", c.invert ? s : e );
	};
	Json caps = Json::Arr();
	switch( c.type )
	{
	case EdType::Dimmer:
	{
		Json k = Cap( "Intensity" );
		if( c.invert ) two( k, "brightness", "off", "bright" );
		one( k );
		break;
	}
	case EdType::Shutter:
	{
		Json a = Cap( "ShutterStrobe" ), b = Cap( "ShutterStrobe" );
		a.Set( "dmxRange", Range( 0, 127 ) );
		a.Set( "shutterEffect", "Closed" );
		b.Set( "dmxRange", Range( 128, 255 ) );
		b.Set( "shutterEffect", "Open" );
		caps.Push( a );
		caps.Push( b );
		break;
	}
	case EdType::Strobe:
	{
		Json a = Cap( "ShutterStrobe" ), b = Cap( "ShutterStrobe" );
		a.Set( "dmxRange", Range( 0, 9 ) );
		a.Set( "shutterEffect", "Open" );
		b.Set( "dmxRange", Range( 10, 255 ) );
		b.Set( "shutterEffect", "Strobe" );
		two( b, "speed", Fmt( std::max( 0.1f, c.a ), "Hz" ), Fmt( std::max( 0.2f, c.b ), "Hz" ) );
		caps.Push( a );
		caps.Push( b );
		break;
	}
	case EdType::ShutterStrobe:
		for( auto& r : c.ranges )
		{
			Json k = Cap( "ShutterStrobe" );
			k.Set( "dmxRange", Range( Clamp( r.from, 0, 255 ), Clamp( r.to, 0, 255 ) ) );
			static const char* ef[] = { "Closed", "Open", "Strobe", "Pulse", "Strobe" };
			k.Set( "shutterEffect", ef[ Clamp( r.kind, 0, 4 ) ] );
			if( r.kind >= 2 )
			{
				k.Set( "speedStart", "1Hz" );
				k.Set( "speedEnd", "20Hz" );
			}
			if( r.kind == 4 ) k.Set( "randomTiming", true );
			if( !r.label.empty() ) k.Set( "comment", r.label );
			caps.Push( k );
		}
		break;
	case EdType::Red:
	case EdType::Green:
	case EdType::Blue:
	case EdType::White:
	case EdType::WarmWhite:
	case EdType::ColdWhite:
	case EdType::Amber:
	case EdType::UV:
	case EdType::Lime:
	case EdType::Cyan:
	case EdType::Magenta:
	case EdType::Yellow:
	{
		Json k = Cap( "ColorIntensity" );
		k.Set( "color", OflColor( c.type ) );
		one( k );
		break;
	}
	case EdType::CTO:
	{
		Json k = Cap( "ColorTemperature" );
		two( k, "colorTemperature", Fmt( c.a, "K" ), Fmt( c.b, "K" ) );
		one( k );
		break;
	}
	case EdType::ColorWheel:
	case EdType::GoboWheel:
	{
		Json w = Json::Obj(), sl = Json::Arr();
		for( auto& s : c.slots )
		{
			Json o = Json::Obj();
			if( s.open ) o.Set( "type", "Open" );
			else if( c.type == EdType::ColorWheel )
			{
				o.Set( "type", "Color" );
				o.Set( "name", s.name );
				Json cs = Json::Arr();
				cs.Push( Hex( s.color ) );
				o.Set( "colors", cs );
			}
			else
			{
				o.Set( "type", "Gobo" );
				o.Set( "name", s.name );
			}
			sl.Push( o );
		}
		w.Set( "slots", sl );
		wheels.Set( c.name, w );
		bool rot = c.type == EdType::ColorWheel && c.rainbow;
		std::vector< std::pair< int, int > > seg;
		Segments( (int)c.slots.size(), 0, rot ? 127 : 255, seg );
		for( size_t i = 0; i < seg.size(); ++i )
		{
			size_t si = c.invert ? seg.size() - 1 - i : i;
			Json k    = Cap( "WheelSlot" );
			k.Set( "dmxRange", Range( seg[ i ].first, seg[ i ].second ) );
			k.Set( "slotNumber", (int)si + 1 );
			caps.Push( k );
		}
		if( rot )
		{
			Json k = Cap( "WheelRotation" );
			k.Set( "dmxRange", Range( 128, 255 ) );
			k.Set( "speedStart", "slow CW" );
			k.Set( "speedEnd", "fast CW" );
			caps.Push( k );
		}
		if( caps.a.empty() ) one( Cap( "NoFunction" ) );
		break;
	}
	case EdType::ColorMacro:
	{
		std::vector< std::pair< int, int > > seg;
		Segments( (int)c.slots.size() + 1, 0, 255, seg );
		Json off = Cap( "NoFunction" );
		off.Set( "dmxRange", Range( seg[ 0 ].first, seg[ 0 ].second ) );
		caps.Push( off );
		for( size_t i = 0; i < c.slots.size(); ++i )
		{
			Json k = Cap( "ColorPreset" );
			k.Set( "dmxRange", Range( seg[ i + 1 ].first, seg[ i + 1 ].second ) );
			Json cs = Json::Arr();
			cs.Push( Hex( c.slots[ i ].color ) );
			k.Set( "colors", cs );
			k.Set( "comment", c.slots[ i ].name );
			caps.Push( k );
		}
		break;
	}
	case EdType::Pan:
	case EdType::Tilt:
	{
		Json k = Cap( c.type == EdType::Pan ? "Pan" : "Tilt" );
		two( k, "angle", "0deg", Fmt( c.b > 0 ? c.b : ( c.type == EdType::Pan ? 540.0f : 270.0f ), "deg" ) );
		one( k );
		break;
	}
	case EdType::PanTiltSpeed:
	{
		Json k = Cap( "PanTiltSpeed" );
		two( k, "speed", "fast", "slow" );
		one( k );
		break;
	}
	case EdType::Zoom:
	{
		Json k = Cap( "Zoom" );
		two( k, "angle", Fmt( c.a, "deg" ), Fmt( c.b, "deg" ) );
		one( k );
		break;
	}
	case EdType::Focus:
	{
		Json k = Cap( "Focus" );
		two( k, "distance", "near", "far" );
		one( k );
		break;
	}
	case EdType::Iris:
	{
		Json k = Cap( "Iris" );
		two( k, "openPercent", "100%", "0%" );
		one( k );
		break;
	}
	case EdType::Frost:
	{
		Json k = Cap( "Frost" );
		two( k, "frostIntensity", "0%", "100%" );
		one( k );
		break;
	}
	case EdType::GoboRotation:
	{
		Json k = Cap( "WheelSlotRotation" );
		for( auto& o : f.pool )
			if( o.type == EdType::GoboWheel ) { k.Set( "wheel", o.name ); break; }
		two( k, "speed", "slow CW", "fast CW" );
		one( k );
		break;
	}
	case EdType::Prism:
	{
		Json a = Cap( "NoFunction" ), b = Cap( "Prism" );
		a.Set( "dmxRange", Range( 0, 127 ) );
		b.Set( "dmxRange", Range( 128, 255 ) );
		caps.Push( a );
		caps.Push( b );
		break;
	}
	case EdType::PrismRotation:
	{
		Json k = Cap( "PrismRotation" );
		two( k, "speed", "slow CW", "fast CW" );
		one( k );
		break;
	}
	case EdType::Fog:
	{
		Json k = Cap( "FogOutput" );
		two( k, "fogOutput", "0%", "100%" );
		one( k );
		break;
	}
	case EdType::Effect:
	{
		Json k = Cap( "Effect" );
		k.Set( "effectName", c.name );
		one( k );
		break;
	}
	case EdType::EffectSpeed:
	{
		Json k = Cap( "EffectSpeed" );
		two( k, "speed", "slow", "fast" );
		one( k );
		break;
	}
	case EdType::Control: one( Cap( "Maintenance" ) ); break;
	case EdType::NoFunction: one( Cap( "NoFunction" ) ); break;
	default:
	{
		Json k = Cap( "Generic" );
		k.Set( "comment", c.name );
		one( k );
		break;
	}
	}
	if( !caps.a.empty() ) ch.Set( "capabilities", caps );
	return ch;
}

Json BuildOfl( const EdFixture& f )
{
	Json j = Json::Obj();
	j.Set( "$schema", "https://raw.githubusercontent.com/OpenLightingProject/open-fixture-library/master/schemas/fixture.json" );
	j.Set( "name", f.name );
	j.Set( "shortName", f.shortName.empty() ? f.name : f.shortName );
	Json cats = Json::Arr();
	cats.Push( kOflCategories[ Clamp( f.category, 0, 9 ) ] );
	j.Set( "categories", cats );
	{
		char d[ 16 ];
		time_t t = time( nullptr );
		strftime( d, sizeof( d ), "%Y-%m-%d", localtime( &t ) );
		Json meta = Json::Obj(), au = Json::Arr();
		au.Push( "LightStage" );
		meta.Set( "authors", au );
		meta.Set( "createDate", d );
		meta.Set( "lastModifyDate", d );
		j.Set( "meta", meta );
	}
	Json phys = Json::Obj(), dims = Json::Arr();
	dims.Push( f.dimsMm.x );
	dims.Push( f.dimsMm.y );
	dims.Push( f.dimsMm.z );
	phys.Set( "dimensions", dims );
	phys.Set( "weight", f.weightKg );
	phys.Set( "power", f.powerW );
	Json bulb = Json::Obj();
	bulb.Set( "colorTemperature", f.kelvin );
	bulb.Set( "lumens", f.lumens );
	phys.Set( "bulb", bulb );
	Json lens = Json::Obj(), mm = Json::Arr();
	mm.Push( std::min( f.beamMin, f.beamMax ) );
	mm.Push( std::max( f.beamMin, f.beamMax ) );
	lens.Set( "degreesMinMax", mm );
	phys.Set( "lens", lens );
	j.Set( "physical", phys );
	//only channels used by a mode
	std::vector< bool > used( f.pool.size(), false );
	for( auto& m : f.modes )
		for( int i : m.ch )
			if( i >= 0 && i < (int)f.pool.size() ) used[ i ] = true;
	Json wheels = Json::Obj(), avail = Json::Obj();
	for( size_t i = 0; i < f.pool.size(); ++i )
		if( used[ i ] ) avail.Set( f.pool[ i ].name, ChannelJson( f.pool[ i ], f, wheels ) );
	if( !wheels.o.empty() ) j.Set( "wheels", wheels );
	j.Set( "availableChannels", avail );
	const bool px = f.pixels > 0 && !f.pixelChannels.empty();
	if( px )
	{
		Json mx = Json::Obj(), pc = Json::Arr();
		pc.Push( f.pixels );
		pc.Push( 1 );
		pc.Push( 1 );
		mx.Set( "pixelCount", pc );
		j.Set( "matrix", mx );
		Json tc = Json::Obj();
		for( auto t : f.pixelChannels )
		{
			EdChannel c = MakeChannel( t );
			Json dummy  = Json::Obj();
			tc.Set( std::string( PixelName( t ) ) + " $pixelKey", ChannelJson( c, f, dummy ) );
		}
		j.Set( "templateChannels", tc );
	}
	Json modes = Json::Arr();
	for( auto& m : f.modes )
	{
		Json mo = Json::Obj(), chs = Json::Arr();
		for( int i : m.ch )
		{
			if( i < 0 || i >= (int)f.pool.size() ) continue;
			chs.Push( f.pool[ i ].name );
			if( f.pool[ i ].fine16 ) chs.Push( f.pool[ i ].name + " fine" );
		}
		if( px && m.pixels )
		{
			Json ins = Json::Obj(), tl = Json::Arr();
			ins.Set( "insert", "matrixChannels" );
			ins.Set( "repeatFor", "eachPixelABC" );
			ins.Set( "channelOrder", "perPixel" );
			for( auto t : f.pixelChannels ) tl.Push( std::string( PixelName( t ) ) + " $pixelKey" );
			ins.Set( "templateChannels", tl );
			chs.Push( ins );
		}
		mo.Set( "name", m.name );
		mo.Set( "shortName", std::to_string( f.Footprint( (int)( &m - &f.modes[ 0 ] ) ) ) + "ch" );
		mo.Set( "channels", chs );
		modes.Push( mo );
	}
	j.Set( "modes", modes );
	j.Set( "lightstageEditor", EdToJson( f ) );
	return j;
}

//---------------------------------------------------------------- editor model <-> JSON
static Json V3J( const V3& v )
{
	Json a = Json::Arr();
	a.Push( v.x );
	a.Push( v.y );
	a.Push( v.z );
	return a;
}
static V3 JV3( const Json& j, V3 d ) { return j.Size() == 3 ? V3( j[ 0 ].F(), j[ 1 ].F(), j[ 2 ].F() ) : d; }

Json EdToJson( const EdFixture& f )
{
	Json j = Json::Obj();
	j.Set( "version", 1 );
	j.Set( "manufacturer", f.manufacturer );
	j.Set( "name", f.name );
	j.Set( "shortName", f.shortName );
	j.Set( "category", f.category );
	j.Set( "dimsMm", V3J( f.dimsMm ) );
	j.Set( "weightKg", f.weightKg );
	j.Set( "powerW", f.powerW );
	j.Set( "lumens", f.lumens );
	j.Set( "kelvin", f.kelvin );
	j.Set( "beamMin", f.beamMin );
	j.Set( "beamMax", f.beamMax );
	j.Set( "pixels", f.pixels );
	Json pc = Json::Arr();
	for( auto t : f.pixelChannels ) pc.Push( (int)t );
	j.Set( "pixelChannels", pc );
	Json pool = Json::Arr();
	for( auto& c : f.pool )
	{
		Json o = Json::Obj();
		o.Set( "name", c.name );
		o.Set( "type", (int)c.type );
		o.Set( "fine16", c.fine16 );
		o.Set( "def", c.def );
		o.Set( "invert", c.invert );
		o.Set( "a", c.a );
		o.Set( "b", c.b );
		o.Set( "rainbow", c.rainbow );
		Json sl = Json::Arr();
		for( auto& s : c.slots )
		{
			Json x = Json::Obj();
			x.Set( "name", s.name );
			x.Set( "color", V3J( s.color ) );
			x.Set( "open", s.open );
			sl.Push( x );
		}
		o.Set( "slots", sl );
		Json rg = Json::Arr();
		for( auto& r : c.ranges )
		{
			Json x = Json::Obj();
			x.Set( "from", r.from );
			x.Set( "to", r.to );
			x.Set( "kind", r.kind );
			x.Set( "label", r.label );
			rg.Push( x );
		}
		o.Set( "ranges", rg );
		pool.Push( o );
	}
	j.Set( "pool", pool );
	Json modes = Json::Arr();
	for( auto& m : f.modes )
	{
		Json o = Json::Obj(), ch = Json::Arr();
		o.Set( "name", m.name );
		for( int i : m.ch ) ch.Push( i );
		o.Set( "ch", ch );
		o.Set( "pixels", m.pixels );
		modes.Push( o );
	}
	j.Set( "modes", modes );
	return j;
}

bool EdFromJson( const Json& j, EdFixture& f )
{
	if( !j.IsObj() || j[ "version" ].I( 0 ) < 1 ) return false;
	f              = EdFixture();
	f.manufacturer = j[ "manufacturer" ].Str( f.manufacturer );
	f.name         = j[ "name" ].Str( f.name );
	f.shortName    = j[ "shortName" ].Str();
	f.category     = Clamp( j[ "category" ].I( 0 ), 0, 9 );
	f.dimsMm       = JV3( j[ "dimsMm" ], f.dimsMm );
	f.weightKg     = j[ "weightKg" ].F( f.weightKg );
	f.powerW       = j[ "powerW" ].F( f.powerW );
	f.lumens       = j[ "lumens" ].F( f.lumens );
	f.kelvin       = j[ "kelvin" ].F( f.kelvin );
	f.beamMin      = j[ "beamMin" ].F( f.beamMin );
	f.beamMax      = j[ "beamMax" ].F( f.beamMax );
	f.pixels       = Clamp( j[ "pixels" ].I( 0 ), 0, 512 );
	for( auto& x : j[ "pixelChannels" ].a ) f.pixelChannels.push_back( (EdType)Clamp( x.I( 0 ), 0, (int)EdType::Count - 1 ) );
	for( auto& o : j[ "pool" ].a )
	{
		EdChannel c;
		c.name    = o[ "name" ].Str();
		c.type    = (EdType)Clamp( o[ "type" ].I( 0 ), 0, (int)EdType::Count - 1 );
		c.fine16  = o[ "fine16" ].B();
		c.def     = Clamp( o[ "def" ].I( 0 ), 0, 255 );
		c.invert  = o[ "invert" ].B();
		c.a       = o[ "a" ].F();
		c.b       = o[ "b" ].F();
		c.rainbow = o[ "rainbow" ].B();
		for( auto& s : o[ "slots" ].a ) c.slots.push_back( EdSlot{ s[ "name" ].Str(), JV3( s[ "color" ], V3( 1, 1, 1 ) ), s[ "open" ].B() } );
		for( auto& r : o[ "ranges" ].a ) c.ranges.push_back( EdRange{ r[ "from" ].I(), r[ "to" ].I( 255 ), r[ "kind" ].I(), r[ "label" ].Str() } );
		f.pool.push_back( c );
	}
	f.modes.clear();
	for( auto& o : j[ "modes" ].a )
	{
		EdMode m;
		m.name   = o[ "name" ].Str( "Standard" );
		m.pixels = o[ "pixels" ].B( true );
		for( auto& x : o[ "ch" ].a )
			if( x.I( -1 ) >= 0 && x.I() < (int)f.pool.size() ) m.ch.push_back( x.I() );
		f.modes.push_back( m );
	}
	if( f.modes.empty() ) f.modes.push_back( EdMode{} );
	return true;
}

static EdType TypeOfChannel( const Channel& c, const FixtureDef& d )
{
	switch( c.attr )
	{
	case Attr::Dimmer: return EdType::Dimmer;
	case Attr::Shutter:
	{
		bool strobe = false, closed = false;
		for( auto& cp : c.caps )
		{
			if( cp.k >= CapK::Strobe && cp.k <= CapK::StrobeRandom ) strobe = true;
			if( cp.k == CapK::ShutterClosed ) closed = true;
		}
		return strobe && closed ? EdType::ShutterStrobe : ( strobe ? EdType::Strobe : EdType::Shutter );
	}
	case Attr::Red: return EdType::Red;
	case Attr::Green: return EdType::Green;
	case Attr::Blue: return EdType::Blue;
	case Attr::White: return EdType::White;
	case Attr::WarmWhite: return EdType::WarmWhite;
	case Attr::ColdWhite: return EdType::ColdWhite;
	case Attr::Amber: return EdType::Amber;
	case Attr::UV: return EdType::UV;
	case Attr::Lime: return EdType::Lime;
	case Attr::Cyan: return EdType::Cyan;
	case Attr::Magenta: return EdType::Magenta;
	case Attr::Yellow: return EdType::Yellow;
	case Attr::CTO: return EdType::CTO;
	case Attr::ColorWheel: return EdType::ColorWheel;
	case Attr::ColorMacro: return EdType::ColorMacro;
	case Attr::Pan: return EdType::Pan;
	case Attr::Tilt: return EdType::Tilt;
	case Attr::PanTiltSpeed: return EdType::PanTiltSpeed;
	case Attr::Zoom: return EdType::Zoom;
	case Attr::Focus: return EdType::Focus;
	case Attr::Gobo: return EdType::GoboWheel;
	case Attr::GoboRot: return EdType::GoboRotation;
	case Attr::Prism: return EdType::Prism;
	case Attr::PrismRot: return EdType::PrismRotation;
	case Attr::Frost: return EdType::Frost;
	case Attr::Iris: return EdType::Iris;
	case Attr::Fog: return EdType::Fog;
	case Attr::Effect: return EdType::Effect;
	case Attr::None: return EdType::NoFunction;
	default: (void)d; return EdType::Generic;
	}
}

bool EdFromDef( const FixtureDef& d, EdFixture& f )
{
	f              = EdFixture();
	f.manufacturer = d.manufacturer.empty() ? L( "Eigene" ) : d.manufacturer;
	f.name         = d.name + L( " (Kopie)" );
	f.shortName    = d.shortName;
	switch( d.kind )
	{
	case Kind::MovingHead: f.category = 1; break;
	case Kind::Scanner: f.category = 2; break;
	case Kind::Bar: f.category = 3; break;
	case Kind::Strobe: f.category = 4; break;
	case Kind::Blinder: f.category = 5; break;
	case Kind::Hazer: f.category = 6; break;
	case Kind::Dimmer: f.category = 7; break;
	case Kind::Laser: f.category = 8; break;
	default: f.category = 0; break;
	}
	f.dimsMm  = d.dims * 1000.0f;
	f.lumens  = d.lumens;
	f.kelvin  = d.kelvin;
	f.beamMin = d.beamMin;
	f.beamMax = d.beamMax;
	f.modes.clear();
	const bool pixelDef = d.Heads() > 1;
	if( pixelDef ) f.pixels = d.Heads();
	for( auto& m : d.modes )
	{
		EdMode em;
		em.name = m.name;
		std::vector< EdType > pixelTypes;
		for( size_t i = 0; i < m.ch.size(); ++i )
		{
			const Channel& c = m.ch[ i ];
			if( c.isFine ) continue;
			EdType t = TypeOfChannel( c, d );
			//per-pixel channels become the pixel template (first pixel only)
			if( pixelDef && c.pixels.size() == 1 )
			{
				if( c.pixels[ 0 ] == 0 ) pixelTypes.push_back( t );
				continue;
			}
			EdChannel ec = MakeChannel( t, c.name.empty() ? EdTypeName( t ) : c.name );
			ec.fine16    = c.fine >= 0;
			ec.def       = c.def16 >> 8;
			if( t == EdType::Pan && d.panRange > 0 ) ec.b = d.panRange;
			if( t == EdType::Tilt && d.tiltRange > 0 ) ec.b = d.tiltRange;
			if( t == EdType::Zoom ) { ec.a = d.beamMin; ec.b = d.beamMax; }
			if( ( t == EdType::ColorWheel || t == EdType::GoboWheel ) && c.wheel >= 0 && c.wheel < (int)d.wheels.size() )
			{
				ec.slots.clear();
				for( auto& s : d.wheels[ c.wheel ].slots ) ec.slots.push_back( EdSlot{ s.name, s.color, s.type == SlotT::Open } );
			}
			//same channel in an earlier mode: reuse the pool entry
			int found = -1;
			for( size_t k = 0; k < f.pool.size(); ++k )
				if( f.pool[ k ].name == ec.name && f.pool[ k ].type == ec.type ) found = (int)k;
			if( found < 0 )
			{
				ec.name = UniqueChannelName( f, ec.name );
				f.pool.push_back( ec );
				found = (int)f.pool.size() - 1;
			}
			em.ch.push_back( found );
		}
		if( pixelDef && f.pixelChannels.empty() ) f.pixelChannels = pixelTypes;
		em.pixels = !pixelTypes.empty();
		f.modes.push_back( em );
	}
	if( f.modes.empty() ) f.modes.push_back( EdMode{} );
	if( pixelDef && f.pixelChannels.empty() ) f.pixels = 0;
	return true;
}

std::vector< std::string > EdValidate( const EdFixture& f, bool* fatal )
{
	std::vector< std::string > r;
	bool bad = false;
	if( f.name.empty() ) { r.push_back( L( "Name fehlt" ) ); bad = true; }
	if( f.manufacturer.empty() ) { r.push_back( L( "Hersteller fehlt" ) ); bad = true; }
	//only channels that a mode uses count: a channel removed from its mode stays in the pool
	//(it can be added again), but it is not saved and must not block saving
	std::vector< bool > used( f.pool.size(), false );
	for( auto& m : f.modes )
		for( int i : m.ch )
			if( i >= 0 && i < (int)f.pool.size() ) used[ i ] = true;
	auto trim = []( const std::string& s ) {
		size_t a = s.find_first_not_of( " \t" ), b = s.find_last_not_of( " \t" );
		return a == std::string::npos ? std::string() : s.substr( a, b - a + 1 );
	};
	std::vector< std::string > reported;
	for( size_t i = 0; i < f.pool.size(); ++i )
	{
		if( !used[ i ] ) continue;
		const std::string ni = trim( f.pool[ i ].name );
		if( ni.empty() ) { r.push_back( L( "Kanal ohne Namen" ) ); bad = true; continue; }
		for( size_t k = i + 1; k < f.pool.size(); ++k )
		{
			if( !used[ k ] ) continue;
			const std::string nk = trim( f.pool[ k ].name );
			if( ni == nk || ni + " fine" == nk || nk + " fine" == ni )
			{
				if( std::find( reported.begin(), reported.end(), ni ) == reported.end() )
				{
					r.push_back( L( "Doppelter Kanalname: " ) + ni );
					reported.push_back( ni );
				}
				bad = true;
			}
		}
	}
	for( size_t m = 0; m < f.modes.size(); ++m )
	{
		int fp = f.Footprint( (int)m );
		if( fp == 0 ) { r.push_back( L( "Modus \"" ) + f.modes[ m ].name + L( "\" hat keine Kanäle" ) ); bad = true; }
		if( fp > 512 ) { r.push_back( L( "Modus \"" ) + f.modes[ m ].name + L( "\" hat mehr als 512 Kanäle" ) ); bad = true; }
		for( size_t k = m + 1; k < f.modes.size(); ++k )
			if( f.modes[ m ].name == f.modes[ k ].name ) { r.push_back( L( "Doppelter Modusname: " ) + f.modes[ m ].name ); bad = true; }
	}
	bool light = f.pixels > 0 && !f.pixelChannels.empty();
	for( size_t i = 0; i < f.pool.size(); ++i )
	{
		const auto& c = f.pool[ i ];
		if( used[ i ] && ( c.type == EdType::Dimmer || EdTypeIsColor( c.type ) || c.type == EdType::Fog ) ) light = true;
	}
	if( !light ) r.push_back( L( "Hinweis: kein Dimmer- oder Farbkanal - die Lampe bleibt dunkel" ) );
	if( fatal ) *fatal = bad;
	return r;
}

std::string Slug( const std::string& s )
{
	std::string t;
	for( size_t i = 0; i < s.size(); ++i )
	{
		unsigned char c = (unsigned char)s[ i ];
		if( c == 0xC3 && i + 1 < s.size() )
		{
			unsigned char n = (unsigned char)s[ ++i ];
			if( n == 0xA4 || n == 0x84 ) t += "ae";
			else if( n == 0xB6 || n == 0x96 ) t += "oe";
			else if( n == 0xBC || n == 0x9C ) t += "ue";
			else if( n == 0x9F ) t += "ss";
			else t += '-';
			continue;
		}
		if( isalnum( c ) ) t += (char)tolower( c );
		else if( !t.empty() && t.back() != '-' ) t += '-';
	}
	while( !t.empty() && t.back() == '-' ) t.pop_back();
	return t.empty() ? "lampe" : t;
}

void RegisterUserManufacturer( const std::string& libRoot, const std::string& slug, const std::string& name )
{
	MakeDirs( libRoot + LS_SEP "user" );
	std::string mp = libRoot + LS_SEP "user" LS_SEP "manufacturers.json", txt;
	Json mj        = Json::Obj();
	if( ReadText( mp, txt ) ) ParseJson( txt, mj );
	if( !mj.IsObj() ) mj = Json::Obj();
	Json e = Json::Obj();
	e.Set( "name", name );
	mj.Set( slug, e );
	WriteText( mp, WriteJson( mj, true ) );
}

bool SaveUserFixture( const std::string& libRoot, const EdFixture& f, std::string& outId, std::string& outPath, std::string* err )
{
	bool fatal = false;
	auto msgs  = EdValidate( f, &fatal );
	if( fatal )
	{
		if( err ) *err = msgs.empty() ? L( "ungültig" ) : msgs[ 0 ];
		return false;
	}
	const std::string mf = Slug( f.manufacturer ), key = Slug( f.name );
	const std::string dir = libRoot + LS_SEP "user" LS_SEP + mf;
	MakeDirs( libRoot + LS_SEP "user" );
	MakeDirs( dir );
	RegisterUserManufacturer( libRoot, mf, f.manufacturer );
	outPath = dir + LS_SEP + key + ".json";
	outId   = "user/" + mf + "/" + key;
	if( !WriteText( outPath, WriteJson( BuildOfl( f ), true ) ) )
	{
		if( err ) *err = L( "Datei konnte nicht geschrieben werden: " ) + outPath;
		return false;
	}
	return true;
}
}//namespace ls
