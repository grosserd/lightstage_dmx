// fixture definitions: model, OFL loader (incl. matrix/pixels, fine channels, switching channels),
// GDTF loader (basic), procedural gobo images and the fixture library
#pragma once
#include "common.h"
#include "json.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ls
{
enum class Attr : uint8_t
{
	None,
	Dimmer,
	Shutter,
	//additive emitters
	Red,
	Green,
	Blue,
	White,
	WarmWhite,
	ColdWhite,
	Amber,
	UV,
	Lime,
	Indigo,
	Cyan,
	Magenta,
	Yellow,
	CTO,
	ColorWheel,
	ColorMacro,
	Pan,
	Tilt,
	PanTiltSpeed,
	Zoom,
	Focus,
	Gobo,
	GoboRot,
	Prism,
	PrismRot,
	Frost,
	Iris,
	Fog,
	Effect,
	Generic,
	Count
};
const char* AttrName( Attr a );
Attr AttrFromName( const std::string& s );
bool IsEmitter( Attr a );
V3 EmitterColor( Attr a );//linear rgb of an LED emitter at full

enum class CapK : uint8_t
{
	None,
	Intensity,
	ColorIntensity,
	ShutterOpen,
	ShutterClosed,
	Strobe,
	StrobePulse,
	StrobeRampUp,
	StrobeRampDown,
	StrobeRandom,
	Pan,
	Tilt,
	PanCont,
	TiltCont,
	PanTiltSpeed,
	WheelSlot,
	WheelRot,
	WheelShake,
	WheelSlotRot,
	ColorPreset,
	ColorTemp,
	Zoom,
	Focus,
	Iris,
	Frost,
	Prism,
	PrismRot,
	Fog,
	ColorJump,
	ColorFade,
	Generic
};

struct Cap
{
	uint16_t lo = 0, hi = 65535;//16-bit domain
	CapK k  = CapK::None;
	float a = 0, b = 0;    //start/end numeric (unit per kind)
	float sa = 0, sb = 0;  //secondary start/end (e.g. shake speed)
	bool angleMode = false;//rotation caps: a/b are angles (deg) instead of speeds (rev/s)
	int wheel = -1;
	float slotA = 0, slotB = 0;//1-based, may be fractional
	std::vector< V3 > colors, colorsEnd;
	int facets = 0;
	std::string label;
	//switching channels: alias -> real channel key, valid while this cap is active
	std::vector< std::pair< std::string, std::string > > sw;
	float T( uint16_t v ) const { return hi > lo ? Saturate( float( v - lo ) / float( hi - lo ) ) : 0.0f; }
	float Val( uint16_t v ) const { return Lerp( a, b, T( v ) ); }
};

enum class SlotT : uint8_t { Open, Closed, Color, Gobo, Prism, Iris, Frost, Anim, Other };
struct WheelSlot
{
	SlotT type = SlotT::Open;
	std::string name;
	V3 color{ 1, 1, 1 };
	bool hasColor = false;
	int gobo     = 0;//gobo image layer (0 = open)
	int facets   = 0;
	float kelvin = 0;
};
struct Wheel
{
	std::string name;
	std::vector< WheelSlot > slots;
	bool IsColor() const;
	bool IsGobo() const;
	bool IsPrism() const;
};

struct Channel
{
	std::string key, name;
	Attr attr = Attr::None;
	int fine = -1, fine2 = -1;//slot indices (in mode) of fine bytes, -1 = none
	int coarse = -1;          //for fine slots: slot index of the coarse byte
	bool isFine = false;
	std::vector< int > pixels;//head indices this channel controls (empty = all)
	uint16_t def16 = 0;
	std::vector< Cap > caps;
	int wheel = -1;
	//switching channel: controlled by mode slot `swCtrl`; alternatives keyed by the real channel key
	int swCtrl = -1;
	std::vector< std::pair< std::string, std::shared_ptr< Channel > > > swAlts;
	const Cap* Find( uint16_t v ) const;
	bool Is16() const { return fine >= 0; }
};

struct Mode
{
	std::string name, shortName;
	std::vector< Channel > ch;//one entry per DMX slot
	int Footprint() const { return (int)ch.size(); }
};

enum class Kind : uint8_t { Par, MovingHead, Scanner, Bar, Strobe, Blinder, Hazer, Dimmer, Wash, Laser, Generic };
const char* KindName( Kind k );

struct Pixel
{
	std::string key;
	V3 pos;//normalized in -0.5..0.5 per axis across the pixel grid
};

struct FixtureDef
{
	std::string id;//"ofl/<mf>/<key>", "generic/<key>", "gdtf/<file>"
	std::string name, manufacturer, shortName;
	std::vector< std::string > categories;
	Kind kind = Kind::Par;
	V3 dims{ 0.3f, 0.3f, 0.3f };//metres (w,h,d)
	float beamMin = 15, beamMax = 15;//full beam angle degrees
	float lumens = 3000, kelvin = 6500;
	bool subtractive = false;//CMY mixing on a white source
	float panRange = 540, tiltRange = 270;
	std::vector< Wheel > wheels;
	std::vector< Mode > modes;
	std::vector< Pixel > pixels;
	int gx = 1, gy = 1, gz = 1;//pixel grid
	std::map< std::string, std::vector< int > > groups;
	std::string source;//file path
	int Heads() const { return pixels.empty() ? 1 : (int)pixels.size(); }
	int FindMode( const std::string& n ) const;
};

//gobo images (grayscale 256x256); layer 0 is "open"
constexpr int kGoboSize = 256;
struct GoboBank
{
	std::vector< std::vector< uint8_t > > layers;
	std::vector< std::string > names;
	int procedural = 0;
	void Init();
	int ByName( const std::string& n, uint32_t seed );
	int Add( const std::string& name, std::vector< uint8_t >&& img );
	uint32_t version = 0;
};
GoboBank& Gobos();

class Library
{
public:
	std::vector< std::unique_ptr< FixtureDef > > defs;
	std::map< std::string, std::string > mfNames;
	void LoadAll( const std::string& root, const std::string& userRoot = "" );//root/ofl, root/generic, root/gdtf; own fixtures (user, gdtf) also from userRoot if set
	const FixtureDef* Find( const std::string& id ) const;
	const FixtureDef* LoadGdtf( const std::string& path );//adds to defs
	//adds a definition or replaces the one with the same id in place (pointers stay valid)
	const FixtureDef* AddOrReplace( std::unique_ptr< FixtureDef > d );
	std::string lastError;
};

bool ParseOflFixture( const Json& j, const std::string& id, const std::string& mfName, FixtureDef& out, std::string* err );
bool ParseGdtf( const std::string& path, FixtureDef& out, std::string* err );
}//namespace ls
