// fixture editor: own fixtures with any number of channels, saved as OFL JSON in library\user
#pragma once
#include "fixture.h"
#include "json.h"

namespace ls
{
enum class EdType : uint8_t
{
	Dimmer, Shutter, Strobe, ShutterStrobe,
	Red, Green, Blue, White, WarmWhite, ColdWhite, Amber, UV, Lime, Cyan, Magenta, Yellow,
	CTO, ColorWheel, ColorMacro,
	Pan, Tilt, PanTiltSpeed,
	Zoom, Focus, Iris, Frost,
	GoboWheel, GoboRotation, Prism, PrismRotation,
	Fog, FanSpeed, Effect, EffectSpeed, Control, NoFunction, Generic,
	Count
};
const char* EdTypeName( EdType t );   //German UI name (also the default channel name)
int EdTypeGroup( EdType t );          //0 Helligkeit, 1 Farbe, 2 Bewegung, 3 Strahl, 4 Räder/Effekte, 5 Sonstiges
extern const char* const kEdGroupNames[ 6 ];
bool EdTypeIsColor( EdType t );       //additive emitter (pixel channels)

struct EdSlot
{
	std::string name;
	V3 color{ 1, 1, 1 };
	bool open = false;
};
struct EdRange
{
	int from = 0, to = 255;
	int kind = 0;//0 zu, 1 offen, 2 Strobe langsam->schnell, 3 Puls, 4 Zufall
	std::string label;
};
struct EdChannel
{
	std::string name;
	EdType type   = EdType::Dimmer;
	bool fine16   = false;
	int def       = 0;
	bool invert   = false;
	float a = 0, b = 0;//type dependent: Pan/Tilt range deg (b), Zoom deg a..b, CTO K a..b, Strobe Hz a..b
	bool rainbow  = false;//colour wheel: continuous rotation in the upper half
	std::vector< EdSlot > slots;
	std::vector< EdRange > ranges;
};
struct EdMode
{
	std::string name = "Standard";
	std::vector< int > ch;//indices into EdFixture::pool
	bool pixels = true;   //append the pixel channels (pixel fixtures)
};
struct EdFixture
{
	std::string manufacturer = "Eigene", name = "Neue Lampe", shortName;
	int category = 0;//see kEdCategories
	V3 dimsMm{ 300, 300, 300 };
	float weightKg = 3, powerW = 100, lumens = 3000, kelvin = 6500;
	float beamMin = 25, beamMax = 25;
	std::vector< EdChannel > pool;
	std::vector< EdMode > modes{ EdMode{} };
	int pixels = 0;                    //> 0: pixel fixture (bar)
	std::vector< EdType > pixelChannels;//channels per pixel, e.g. R, G, B
	int Footprint( int mode ) const;
};
extern const char* const kEdCategories[ 10 ];//UI names

EdChannel MakeChannel( EdType t, const std::string& name = "" );//with sensible defaults (ranges, slots)
std::string UniqueChannelName( const EdFixture& f, const std::string& base );
//templates: "RGB", "RGBW", ... ; appends to the pool and to mode `mode`
extern const char* const kEdTemplates[ 8 ];
void ApplyTemplate( EdFixture& f, int mode, int tmpl );

Json BuildOfl( const EdFixture& f );//OFL fixture JSON (+ "lightstageEditor" with the editor model)
Json EdToJson( const EdFixture& f );
bool EdFromJson( const Json& j, EdFixture& f );
bool EdFromDef( const FixtureDef& d, EdFixture& f );//best effort (for "edit as copy")
std::vector< std::string > EdValidate( const EdFixture& f, bool* fatal );
std::string Slug( const std::string& s );
//writes library\user\<mf>\<name>.json (+ manufacturers.json); outId = "user/<mf>/<name>"
void RegisterUserManufacturer( const std::string& libRoot, const std::string& slug, const std::string& name );
bool SaveUserFixture( const std::string& libRoot, const EdFixture& f, std::string& outId, std::string& outPath, std::string* err );
}//namespace ls
