// show data: universes + DMX buffers, patched fixtures, stage, MIDI mappings, snapshots, output settings
#pragma once
#include "fixture.h"
#include "timeline.h"
#include <array>
#include <map>
#include <mutex>

namespace ls
{
struct Json;
using DmxFrame = std::array< uint8_t, 512 >;

struct Universe
{
	int num = 1;//1-based display number
	std::string name;
	bool artnetOut = true;
	int artnetPort = 0;//Art-Net port address (net/subnet/universe, 15 bit)
	bool artnetIn  = false;
	int artnetInPort = 0;
};

//runtime output of the simulation for one fixture
struct FixtureState
{
	float pan = 0, tilt = 0;            //current, degrees relative to home
	float panTarget = 0, tiltTarget = 0;//degrees
	float panSpin = 0, tiltSpin = 0;    //continuous rotation accumulators
	float zoom = 20, zoomTarget = 20;   //full beam angle in degrees
	float focus = 0.5f, frost = 0, iris = 1;
	int gobo = 0;      //gobo layer
	float goboRot = 0; //radians
	float goboShake = 0;
	int prism = 0;     //facets, 0 = off
	float prismRot = 0;//radians
	V3 color{ 1, 1, 1 };//beam colour (linear, normalized)
	float intensity = 0;//0..1 light output incl. dimmer curve and shutter
	float smoothDim = 0;
	std::vector< V3 > heads;//per head colour * intensity
	float fog = 0;          //hazer output 0..1
	//accumulators
	double strobePhase = 0;
	long long strobeLast = -1;
	float effectPhase = 0;
	float wheelPos[ 4 ] = { 0, 0, 0, 0 };
	float goboSpinAcc = 0, prismSpinAcc = 0;
	bool init = false;
};

//where a lamp is fixed: free in the room, on a truss or on a tripod stand (it follows when they move)
enum class MountType : uint8_t { Free, Truss, Stand };

struct FixtureInst
{
	int id = 1;
	std::string name;
	std::string defId;
	const FixtureDef* def = nullptr;
	int mode     = 0;
	int universe = 1;//universe number
	int address  = 1;//1-based
	V3 pos{ 0, 0, 0 };
	V3 rot{ 0, 0, 0 };//degrees
	bool invertPan = false, invertTilt = false;
	MountType mount = MountType::Free;
	int mountIdx     = -1;//truss / stand index
	float beam       = 0;//beam angle override in degrees for lamps without zoom (0 = from the profile)
	//runtime: offset in the mount's frame, rebased whenever the lamp itself is moved
	V3 mountLocal{ 0, 0, 0 }, mountLastPos{ 0, 0, 0 };
	float mountYaw = 0, mountLastYaw = 0;
	bool mountInit = false;
	FixtureState st;
	const Mode* M() const { return def && mode >= 0 && mode < (int)def->modes.size() ? &def->modes[ mode ] : nullptr; }
	int Footprint() const { const Mode* m = M(); return m ? m->Footprint() : 0; }
};

struct Truss
{
	V3 a{ -5, 6, 0 }, b{ 5, 6, 0 };
	float size = 0.29f;
	bool freeEnds = false;//UI: both ends positioned separately (else one centre + length + rotation)
};

//tripod stand: stands on the floor (pos.y = floor level), only the height changes; optional T-bar on top
struct Stand
{
	V3 pos{ 0, 0, 0 };
	float height = 2.5f;//top of the stand above pos.y
	float yaw    = 0;   //degrees
	float bar    = 0;   //T-bar width in metres, 0 = single spigot
};

//origin + axes of a truss or stand, for lamps fixed to it
struct MountFrame
{
	V3 o, x, y, z;
	float yaw = 0;//degrees about the world up axis
	V3 ToWorld( const V3& l ) const { return o + x * l.x + y * l.y + z * l.z; }
	V3 ToLocal( const V3& p ) const { V3 d = p - o; return V3( Dot( d, x ), Dot( d, y ), Dot( d, z ) ); }
};

struct ModelObj
{
	std::string path, name;
	V3 pos{ 0, 0, 0 }, rot{ 0, 0, 0 };
	float scale = 1.0f;
	V3 tint{ 0.6f, 0.6f, 0.62f };
	bool matColors = true;//true: colours + textures from the file, false: one colour (tint)
	int meshId     = -1;  //renderer handle (runtime)
	int textures   = 0;   //runtime: number of loaded textures
	std::vector< std::string > missingTex;//runtime: referenced but not found
};

//---------------------------------------------------------------- video projection
enum class VideoInput : uint8_t { TestCard, File, Ndi };
uint32_t NewProjectorUid();
//virtual video projector: throws its input (test card, video file or NDI) into the room as light
struct Projector
{
	uint32_t uid = NewProjectorUid();//runtime: video source slot
	std::string name = "Projektor";
	bool on = true;
	V3 pos{ 0, 3.2f, 9.0f };          //lens position
	float pan = 0, tilt = 0, roll = 0;//degrees; 0/0 looks at the stage (-Z), tilt + = up
	int format       = 0;             //0 16:9, 1 16:10, 2 4:3, 3 21:9, 4 1:1, 5 like the input
	float throwRatio = 1.6f;          //distance / picture width ("Zoom")
	float shiftX = 0, shiftY = 0;     //lens shift in picture widths / heights
	float lumens     = 12000;
	float blackLevel = 0.004f;//light with a black picture (contrast)
	float blur       = 0;     //0..1 out of focus
	bool rear        = false; //mirrored (rear projection)
	bool frustum     = true;  //outline of the light cone
	VideoInput input = VideoInput::TestCard;
	std::string file, ndi;
	bool loop = true;
};
float ProjectorAspect( const Projector& p, int srcW = 0, int srcH = 0 );
struct ProjectorGeo
{
	V3 o, dir, up, right;
	float l = 0, r = 0, b = 0, t = 0;//picture edges at distance 1 (lens shift included)
	float aspect = 16.0f / 9.0f;
};
ProjectorGeo GetProjectorGeo( const Projector& p, int srcW = 0, int srcH = 0 );

//projection screen (Leinwand): pos = centre of the bottom edge
struct Screen
{
	std::string name = "Leinwand";
	V3 pos{ 0, 1.3f, -3.6f };
	float yaw = 0, tilt = 0;//degrees; faces the audience (+Z) at 0, tilt + = top leans back
	float width = 6.0f, height = 3.375f;
	float gain = 1.0f;//reflectivity (1 = matt white)
	int format = -1;  //0 16:9, 1 16:10, 2 4:3, 3 21:9, 4 1:1, 5 free; -1 = detect from width/height
	bool frame = true;//black border
	int mount  = 0;   //0 legs on the floor, 1 hanging on cables, 2 none
	//shape from a black/white image (1:1): white = screen. Empty = the full rectangle.
	int maskN = 0;                //cells per side (kScreenMaskN)
	std::vector< uint8_t > mask;  //maskN * maskN, row 0 = top, 1 = screen
	std::string maskFile;         //source image (shown in the UI, "reload")
	struct Rect { float x0, y0, x1, y1; };//derived: merged white cells, 0..1, y down
	std::vector< Rect > maskRects;
	float maskBox[ 4 ] = { 0, 0, 1, 1 };//derived: bounding box of the white cells (x0, y0, x1, y1)
	bool HasMask() const { return maskN > 0 && !mask.empty(); }
	bool MaskAt( float u, float v ) const;//u, v 0..1 (v down); true = screen
};
constexpr int kScreenMaskN = 256;
//loads a black/white image as the screen shape; non-square images are fitted into the square (warning text)
bool LoadScreenMask( Screen& s, const std::string& file, std::string& err, std::string* warn = nullptr );
void SetScreenMask( Screen& s, int n, std::vector< uint8_t > cells );//also builds the derived rectangles
void ClearScreenMask( Screen& s );
std::string ScreenMaskToString( const Screen& s );   //base64 of the packed bits
bool ScreenMaskFromString( Screen& s, int n, const std::string& b64 );
struct ScreenGeo
{
	V3 centre, right, up, normal;
};
ScreenGeo GetScreenGeo( const Screen& s );

//extra deck / riser (Stage tab): rectangle or round, standing on the floor or on the main deck
struct Deck
{
	std::string name = "Deck";
	int shape    = 0;           //0 rectangle, 1 round (ellipse)
	float x = 0, z = 0;         //centre (m)
	float y = 0;                //base height above the ground (m); onDeck: the main deck's top instead
	float width = 3, depth = 2; //m (round: diameters)
	float height = 0.6f;        //above what it stands on (m)
	float yaw    = 0;           //degrees
	bool onDeck  = false;       //stands on the main deck (its top = main deck + height)
	bool ownColor = false;
	V3 color{ 0.1f, 0.1f, 0.11f };
	bool skirt   = true;        //dark skirt around the sides
	int stairs   = 0;           //0 none, 1 front, 2 back, 3 left, 4 right
	V3 edge{ 0, 0, 0 };         //LED edge on the front (emissive colour, 0 = off)
	float edgeLevel = 0;        //0..4
};
struct StageCfg
{
	float width = 12, depth = 8, deckHeight = 1.0f;
	bool deck = true, backWall = true, floor = true;
	float wallHeight = 9.0f;
	float haze = 0.35f;   //base haze density 0..1
	float hazeNoise = 0.5f;
	float ambient = 0.02f;
	float envLight = 1.0f;      //Umgebung: multiplier for ambient light, sky and room brightening (0..4)
	V3 ambientColor{ 1, 1, 1 };//colour of the ambient light
	float exposure = 1.0f;
	float bloom = 0.6f;
	float beamQuality = 1.0f;//raymarch steps scale
	bool shadows = true, beams = true;
	float beamLevel = 1.0f;//strength of the beams in the haze 0..2
	//place: hall or outdoor (sky, moon, ground types)
	int environment  = 0;   //0 Halle, 1 Outdoor
	int ground       = 0;   //0 Hallenboden (Raster), 1 Rasen, 2 Beton, 3 Asphalt, 4 Erde/Kies, 5 Holzboden
	float groundSize = 80;  //edge length of the ground plane (m)
	V3 skyZenith{ 0.003f, 0.005f, 0.018f };//linear
	V3 skyHorizon{ 0.015f, 0.02f, 0.04f };
	float stars = 0.7f;     //0..1
	float moon  = 0.5f;     //0..1 brightness of moon + moonlight
	float moonAz = 35, moonEl = 30;//degrees; azimuth 0 = behind the stage (-Z), 90 = stage right (+X)
	bool moonShadow   = false;
	float distanceFog = 0;  //0..1 haze in the distance (outdoor)
	V3 floorColor{ 0.09f, 0.09f, 0.1f };
	V3 wallColor{ 0.09f, 0.09f, 0.095f };
	std::vector< Deck > decks;//extra risers / platforms next to or on the main deck
};

//---------------------------------------------------------------- effects (effects.cpp)
enum class FxKind : uint8_t { Dimmer, Color, Position, Attribute, Modulator };
enum class FxWave : uint8_t { Sine, Triangle, SawUp, SawDown, Square, Pulse, Random, Smooth, Custom };
//point of a drawn wave / envelope: position in the cycle 0..1, value 0..1, how the curve leaves it
struct FxPoint
{
	float t = 0, v = 0;
	int shape = 2;//0 linear, 1 eased (rounded), 2 curve (smooth spline through the points), 3 step (hold)
};
//what the builder made the effect as (the left list of the Effects tab); decides which settings are shown.
//Modulators (LFO, step sequencer, envelope) drive any attribute of the lamps or raw DMX channels.
//Chase .. Rainbow are kept for older files and as preset templates: they load as Intensity / Colour
enum class FxType : uint8_t { Chase, Wave, Strobe, Sparkle, ColorChange, ColorFade, Rainbow, Movement, Beam, Lfo, Sequencer, Envelope, Intensity, Colour, Count };
//target of a modulator (and of every sequencer lane)
struct FxTarget
{
	int mode  = 0;           //0 the effect's lamps, 1 a group (Groups tab), 2 DMX channels
	int group = 0;           //mode 1: index in Show::groups
	Attr attr = Attr::Dimmer;//mode 0 / 1
	int uni = 1, ch = 1, count = 1;//mode 2: universe, first channel, number of channels
};
//one row of the step sequencer
struct FxLane
{
	FxTarget tgt;
	std::vector< float > v;//value per step 0..1 (size = Effect::steps)
	bool mute  = false;
	bool glide = false;    //slide to the next step instead of jumping
};
enum class FxShape : uint8_t { Circle, Eight, PanSwing, TiltSwing, Triangle, Square, Random };
enum class FxOrder : uint8_t { Selection, LeftRight, FrontBack, CenterOut, Random };
enum class FxColorMode : uint8_t { Rainbow, Fade, Steps };
//a running pattern over a set of lamps; it writes into an overlay above the programmer / input values
struct Effect
{
	int id           = 0;
	std::string name = "Effekt";
	bool on          = false;
	FxKind kind      = FxKind::Dimmer;
	FxWave wave      = FxWave::Sine;
	FxShape shape    = FxShape::Circle;
	Attr attr        = Attr::Zoom;//FxKind::Attribute
	FxColorMode colorMode = FxColorMode::Rainbow;
	std::vector< V3 > colors{ V3( 1, 0, 0 ), V3( 0, 0.2f, 1 ) };
	std::vector< int > fixtures;//fixture ids
	bool pixels  = false;       //pixel lamps: every pixel is its own element
	bool sync    = true;        //tempo from the clock (beats) or free (Hz)
	float beats  = 4;           //one cycle in beats
	float hz     = 0.5f;        //free running
	float spread = 360;         //phase offset over all elements (degrees)
	FxOrder order = FxOrder::LeftRight;
	int groups   = 0;           //> 0: elements i % groups share a phase (odd/even ...)
	bool mirror  = false;       //wings: symmetric from the middle
	bool reverse = false;
	float width  = 0.5f;        //square / pulse width 0..1
	float lo = 0, hi = 1;       //dimmer / attribute range
	float sizePan = 60, sizeTilt = 30;//position: swing in degrees (peak to peak)
	float level  = 1;           //share of the effect against the base values 0..1
	float fade   = 0.3f;        //seconds to fade in / out
	//builder type + modulators
	FxType type   = FxType::Wave;
	int group     = -1;         //>= 0: the lamps come from this group (Groups tab) instead of the list
	FxTarget tgt;               //LFO / envelope
	bool relative = false;      //LFO: swing around the value underneath (depth) instead of lo..hi
	float depth   = 0.5f;       //relative swing (peak to peak, 0..1)
	float strobeOn = 0.15f;     //strobe: flash length (share of the cycle)
	//step sequencer
	int steps       = 16;
	float stepBeats = 0.25f;    //length of one step in beats (synced)
	int stepOffset  = 0;        //each next lamp / channel starts this many steps later (chase over the lamps)
	std::vector< FxLane > lanes;
	//envelope (times in beats when synced, else seconds); retriggered every cycle or by hand
	float atk = 0.05f, hold = 0.0f, dec = 0.4f, sus = 0.0f, rel = 0.3f, gate = 0.5f;
	float curve = 0.5f;         //0 linear .. 1 strongly exponential
	bool manualTrig = false;    //only the trigger button / OSC / MIDI starts it
	bool envDrawn   = false;    //envelope: drawn curve over the gate length instead of ADSR
	std::vector< FxPoint > pts;//FxWave::Custom (one cycle) and the drawn envelope
	double trigBeat = -1e9;     //runtime: beat (synced) or seconds of the last manual trigger
	double trigTime = -1e9;
	bool openShutter = true;    //dimmer effects: shutter open while running
	bool scale       = false;   //dimmer effects: false = replace the dimmer, true = scale what is underneath
	                            //(programmer / group fader / snapshot is the ceiling of the effect)
	//runtime
	float cur        = 0;//current fade level
	double freePhase = 0;
	double lastBeat  = 0;
	double syncBeat  = 0;//own beat clock: follows the tempo, scaled by fxSpeed, frozen by hold
	double lastTempo = 0;
	double clock     = 0;//seconds the effect ran (× tempo factor, frozen by hold): free envelope trigger
	bool clockInit   = false;
	float snapW      = -1;//snapshots switch it: highest drive over its lamps 0..1 (-1 = no snapshot touches it, own switch counts)
	float snapI      = 1; //unused (kept for compatibility)
	//per lamp (parallel to fixtures): drive set by snapshots (-1 = untouched, own switch counts) and their strength.
	//A snapshot switches an effect only on its own lamps.
	std::vector< float > fxD, fxI;
	float Drive() const { return snapW >= 0 ? snapW : cur; }
	float DriveOf( int fixtureId, float* strength = nullptr ) const
	{
		if( strength ) *strength = 1;
		if( snapW < 0 || fxD.size() != fixtures.size() ) return cur;
		for( size_t k = 0; k < fixtures.size(); ++k )
			if( fixtures[ k ] == fixtureId )
			{
				if( fxD[ k ] < 0 ) return cur;
				if( strength ) *strength = fxI[ k ];
				return fxD[ k ];
			}
		return cur;
	}
};
struct FxProgram
{
	std::string name;
	std::vector< int > effects;//effect ids
};
//tempo source for synced effects
enum class TempoSource : uint8_t { Internal, MidiClock, Link };
struct Tempo
{
	TempoSource source = TempoSource::Internal;
	float bpm          = 120;
	int beatsPerBar    = 4;
	//runtime
	double beat      = 0;    //running beat position
	float liveBpm    = 120;  //measured (MIDI clock) / session (Link)
	bool playing     = true; //MIDI clock: start/stop, Link: transport
	int peers        = 0;    //Link
	double lastTick  = 0;    //MIDI clock: time of the last 0xF8
	long long ticks  = 0;    //MIDI clock: ticks since start (24 per beat)
	std::vector< double > taps;
	std::string status;
};

enum class MidiTarget : uint8_t { FixtureAttr, DmxChannel, Snapshot, Haze, Master, Param };
enum class MidiType : uint8_t { CC, Note, PitchBend, CC14 };
struct MidiMap
{
	std::string device;//empty = any
	int channel = -1;  //0..15, -1 = any
	MidiType type = MidiType::CC;
	int number = 0;
	MidiTarget target = MidiTarget::FixtureAttr;
	std::vector< int > fixtures;//fixture ids (FixtureAttr)
	Attr attr = Attr::Dimmer;
	int head = -1;//-1 = all heads
	int universe = 1, channelDmx = 1;
	bool sixteen = false;
	int snapshot = 0;
	float lo = 0, hi = 1;
	bool invert = false, toggle = false;
	std::string param;//MidiTarget::Param: OSC address of the parameter (params.cpp)
	float last = -1;//runtime
	bool toggled = false;
	std::string Describe( const struct Show& s ) const;
};

//snapshot = stored look of some lamps (+ running effects, + projector settings). Several can run at once:
//they sit above the programmer (base DMX) and below the effects. Mixing per channel in start order:
//intensity (dimmer; colour on lamps without dimmer) -> value * master, faded in; fadeable attributes
//(colour, position, zoom ...) -> faded in; switching attributes (gobo, wheel, shutter, macro) -> at once.
//With the same lamps in two snapshots the later started one wins.
struct SnapFixture
{
	int id = 0;
	std::vector< uint8_t > v;//DMX of the lamp's footprint at the time of the snapshot
};
struct SnapProjector
{
	int idx = 0;//index in Show::projectors
	bool on = true;
	float lumens = 12000, blackLevel = 0.004f, blur = 0;
	V3 pos{ 0, 0, 0 };
	float pan = 0, tilt = 0, roll = 0, throwRatio = 1.6f, shiftX = 0, shiftY = 0;
};
struct Snapshot
{
	uint32_t uid = 0;//stable id (cue list refers to it)
	std::string name;
	std::vector< SnapFixture > fix;
	std::vector< int > effects;//effect ids that ran when the snapshot was taken (switched on while it is on)
	std::vector< int > fxOff;  //effect ids that were off (switched off while it is on)
	std::vector< SnapProjector > proj;
	bool withFx = true, withProj = false;//what "Neu aufnehmen" captures again
	float master = 1.0f;//strength 0..1 (intensity)
	float fade   = 1.0f;//seconds to fade in and out
	//runtime (active is saved with the show)
	bool active  = false;
	float cur    = 0;//fade level 0..1
	uint32_t seq = 0;//start order: higher = later = wins
	bool keep    = false;//started with Shift: does not end older snapshots with the same lamps
	float fadeOv = -1;   //runtime: fade time of the current start / release (cue list), < 0 = own fade
	std::vector< std::pair< int, uint32_t > > pal;//(fixture id, palette uid): these values come from a palette
	std::vector< std::pair< int, DmxFrame > > data;//legacy files: whole universes (converted on load)
	bool Has( int fixtureId ) const
	{
		for( auto& f : fix )
			if( f.id == fixtureId ) return true;
		return false;
	}
};

//palette: a colour (for any lamp) or positions (pan / tilt per lamp). Snapshots that were taken with
//palette values remember that; changing the palette changes them too.
struct PalPos
{
	int id     = 0;
	float pan  = 0.5f, tilt = 0.5f;//0..1 of the channel range
};
struct Palette
{
	uint32_t uid = 0;
	std::string name;
	int kind = 0;//0 colour, 1 position
	V3 color{ 1, 1, 1 };
	std::vector< PalPos > pos;
	const PalPos* PosOf( int id ) const
	{
		for( auto& p : pos )
			if( p.id == id ) return &p;
		return nullptr;
	}
};

//cue list: snapshots in order, started with GO, by following the previous cue or by timecode (MTC)
struct Cue
{
	std::string name;
	uint32_t snap = 0;   //snapshot uid (0 / not found = no snapshot: the cue only releases)
	float fade    = -1;  //< 0: the snapshot's own fade time
	int trigger   = 0;   //0 GO, 1 follow (after the previous cue's fade + wait), 2 timecode
	float wait    = 0;   //follow: seconds after the previous cue has faded in
	double tc     = 0;   //timecode: seconds
	bool release  = true;//releases the snapshots of the other cues
};

//group fader: one value for the same attribute (or a colour) of several lamps; writes the programmer
struct GroupFader
{
	std::string name = "Gruppe";
	std::vector< int > fixtures;//fixture ids
	int kind    = 0;//0 attribute, 1 colour
	Attr attr   = Attr::Dimmer;
	float value = 1.0f;//attribute 0..1; colour: brightness
	float hue = 0, sat = 1;//colour (hue 0..1)
	//0 = programmer: the fader writes the value into the lamps (latest action wins over snapshots);
	//1 = submaster (dimmer only): scales the finished output - programmer, snapshots and effects
	int mode = 0;
	bool Submaster() const { return mode == 1 && kind == 0 && attr == Attr::Dimmer; }
};

struct OutputCfg
{
	bool artnet = false;
	std::string artnetTarget = "255.255.255.255";
	std::string artnetBind = "0.0.0.0";
	bool artnetInput = false;
	bool enttec = false;
	std::string enttecPort = "auto";
	int enttecUniverse = 1;
	float rate = 40.0f;
	bool sendOnlyChanges = false;
	bool oscIn = false;
	int oscPort = 8000;
	//sACN (E1.31): every universe with "Out" on its own universe number; target empty = multicast 239.255.x.y
	bool sacn = false;
	std::string sacnTarget;
	int sacnPriority = 100;
};

//Ableton "LightStage Control" device: which target (midiTargets key) sits on which knob / button.
//Kept with the show and synced both ways (assigned here or in the device), keyed by the device's instance id.
//Named table per device kind ("Control 1", "DMX 1", "Cues 1"): LightStage keeps every assignment of the
//Max for Live devices; a device binds to a table and fetches it on connect (BRIDGE_PROTOKOLL 2.6 / 2.7).
struct AbletonMap
{
	std::string id;//instance id of the device bound last ("" = none)
	std::string label = "Control";
	std::string kind  = "control";//control, dmx, cues
	std::array< std::string, 16 > knobs;
	std::array< std::string, 8 > buttons;//Control: target keys, "" = free
	std::string state;      //complete assignment state of the device (encodeURIComponent(JSON)), kept as it is
	bool knobsNewer = false;//knob table changed here while the device was away: wins over state on the next bind
	bool Empty() const
	{
		for( auto& k : knobs )
			if( !k.empty() ) return false;
		for( auto& b : buttons )
			if( !b.empty() ) return false;
		return true;
	}
};

struct Show
{
	std::vector< Universe > universes;
	std::vector< DmxFrame > dmx;//parallel to universes
	std::vector< FixtureInst > fixtures;
	std::vector< Truss > trusses;
	std::vector< Stand > stands;
	std::vector< ModelObj > models;
	std::vector< Projector > projectors;
	std::vector< Screen > screens;
	StageCfg stage;
	std::vector< MidiMap > midi;
	std::vector< Snapshot > snapshots;
	std::vector< GroupFader > groups;
	std::vector< AbletonMap > ableton;
	std::string showId;//stable id of this show (devices notice a different show)
	AbletonMap* FindAbleton( const std::string& id )//table bound to this device instance
	{
		if( id.empty() ) return nullptr;
		for( auto& m : ableton )
			if( m.id == id ) return &m;
		return nullptr;
	}
	AbletonMap* FindTable( const std::string& kind, const std::string& label )
	{
		for( auto& m : ableton )
			if( m.kind == kind && m.label == label ) return &m;
		return nullptr;
	}
	std::vector< Effect > effects;
	std::vector< FxProgram > programs;
	Tempo tempo;
	bool workLight = false;//runtime: overview light (visual only)
	//effect overlay: -1 = no effect on this channel, else the value sent out / simulated instead of dmx
	std::vector< std::array< int16_t, 512 > > fxOut;
	bool fxAny = false;
	//snapshot layer (between base and effects): -1 = no snapshot on this channel; fxOut includes it
	std::vector< std::array< int16_t, 512 > > snapOut;
	bool snapAny     = false;
	uint32_t snapSeq = 0;
	//runtime: per channel the start number of the last group fader move. Higher than a snapshot's number =
	//the group fader was moved later and wins on this channel until the snapshot is started again
	std::vector< std::array< uint32_t, 512 > > grpSeq;
	//palettes
	std::vector< Palette > palettes;
	std::map< int, uint32_t > progPalColor, progPalPos;//runtime: fixture id -> palette last applied to the programmer
	//cue list
	std::vector< Cue > cues;
	bool cueTimecode = false;//follow MIDI timecode (cues with trigger 2)
	int cueCur       = -1;   //runtime: last started cue
	double cueT      = 0;    //runtime: seconds since it started
	double tcSec     = -1;   //runtime: current timecode (seconds), < 0 = none
	int tcFps        = 25;   //runtime: frame rate of the timecode source
	double tcPrev    = -1;   //runtime
	bool tcRun       = false;//runtime: timecode quarter frames are arriving (running)
	double ltcSec    = -1;   //runtime: LTC from the audio input (seconds), < 0 = none
	bool ltcRun      = false;
	int ltcFps       = 25;
	//2.0 show timeline (layer above the snapshots, below the effects)
	Timeline tl;
	//runtime: per channel the start number of the last programmer write (Set). Higher than the timeline's
	//play / locate number = the programmer wins on this channel until the timeline is started / located again
	std::vector< std::array< uint32_t, 512 > > progSeq;
	OutputCfg output;
	std::string path;
	float master = 1.0f;//visual grand master 0..10 (above 1 = more light output than the fixtures' rating)
	//effect controls (fxMaster / fxSpeed are saved with the show, the rest is runtime)
	float fxMaster = 1.0f;//all effects, 0..1
	float fxSpeed  = 1.0f;//tempo factor for all effects 0.25..4
	bool fxHold    = false;//freeze all effects
	int fxSelId    = 0;//effect selected in the editor (the hardware "selected effect" controls use it)
	bool dirty = false;
	std::mutex dmxLock;//guards dmx for the output thread
	uint32_t dmxVersion = 0;

	void Clear();
	int UniIndex( int num ) const;
	int AddUniverse();//returns number
	DmxFrame* Frame( int num );
	FixtureInst* FindFixture( int id );
	int NextFixtureId() const;
	//first free address range in a universe for a footprint (0 if none)
	int FreeAddress( int uni, int footprint, int start = 1, int ignoreId = -1 ) const;
	bool Overlaps( int uni, int addr, int footprint, int ignoreId ) const;
	FixtureInst* FixtureAt( int uni, int addr, int* slot );

	//DMX access for a fixture (Get = what goes out: effect overlay, else base; Set = base)
	uint8_t Get( const FixtureInst& f, int slot ) const;
	uint8_t GetBase( const FixtureInst& f, int slot ) const;
	//base + snapshots (what the effects work on)
	uint8_t GetLayer( const FixtureInst& f, int slot ) const;
	uint16_t Get16Layer( const FixtureInst& f, int slot ) const;
	float GetAttrLayer( const FixtureInst& f, Attr a, int head = -1 ) const;
	uint16_t Get16Base( const FixtureInst& f, int slot ) const;
	float GetAttrBase( const FixtureInst& f, Attr a, int head = -1 ) const;
	//effect overlay writes (caller holds dmxLock)
	void FxSet16( const FixtureInst& f, int slot, uint16_t v );
	void FxSetAttr( const FixtureInst& f, Attr a, float v, int head = -1 );
	//channel slots SetAttr would write for (attr, head)
	void AttrSlots( const FixtureInst& f, Attr a, int head, std::vector< int >& slots ) const;
	Effect* FindEffect( int id );
	int NextEffectId() const;
	void Set( FixtureInst& f, int slot, uint8_t v );
	uint16_t Get16( const FixtureInst& f, int slot ) const;//coarse slot, combines fine
	void Set16( FixtureInst& f, int slot, uint16_t v );
	//attribute level helpers (value 0..1 over the whole channel range)
	bool HasAttr( const FixtureInst& f, Attr a ) const;
	float GetAttr( const FixtureInst& f, Attr a, int head = -1 ) const;
	void SetAttr( FixtureInst& f, Attr a, float v, int head = -1 );
	void SetDefaults( FixtureInst& f );
	void Home( FixtureInst& f );//pan/tilt centre, shutter open, dimmer 0

	bool Save( const std::string& file, std::string* err );
	bool Load( const std::string& file, const Library& lib, std::string* err );
	//the show as JSON (what Save writes). undo = true: without the live state (DMX programmer values,
	//tempo, masters, which snapshots / effects run, group fader positions, own fixture files, show id)
	Json ToJson( bool undo = false );
	bool FromJson( const Json& j, const Library& lib, const std::string& file, std::string* err );
	void Bump() { ++dmxVersion; dirty = true; }
	void TakeSnapshot( const std::string& name );//all lamps + running effects
	//returns the index; fixtureIds empty = all lamps
	int TakeSnapshotOf( const std::string& name, const std::vector< int >& fixtureIds, bool withFx, bool withProj );
	void CaptureSnapshot( Snapshot& s, const std::vector< int >& fixtureIds );//values of base + snapshot layer
	void RecaptureSnapshot( int i, const std::vector< int >* fixtureIds = nullptr );//same options, new values
	void ReleaseSnapshot( int i );//fade out
	int SnapshotOverridden( int i );
	uint32_t NewSnapUid() const;
	//palettes
	uint32_t NewPaletteUid() const;
	Palette* FindPalette( uint32_t uid );
	bool PaletteFits( const Palette& p, const FixtureInst& f ) const;
	//writes the palette values of a lamp into a footprint buffer (snapshot data); false = nothing to write
	bool EncodePalette( const Palette& p, const FixtureInst& f, std::vector< uint8_t >& v ) const;
	int ApplyPalette( uint32_t uid, const std::vector< int >& ids );//into the programmer; returns lamps changed
	//after an edit: snapshots, and lamps that still show the old values (before) in the programmer; returns snapshots changed
	int UpdatePalette( uint32_t uid, const Palette* before = nullptr );
	void LinkPalettes( Snapshot& s );//which of the snapshot's values are palette values
	void DeletePalette( uint32_t uid );
	int FindSnapUid( uint32_t uid ) const;//index or -1
	//cue list
	void CueGo( int i = -1 );//-1 = next
	void CueBack();
	void CueStop();
	float CueFadeTime( int i ) const;//fade of cue i (own or the snapshot's)
	void RunCues( double dt );//running snapshot: lamps on which a group fader moved later (its values win there)
	void ToggleSnapshot( int i, bool keepOthers = false );
	void ReleaseAllSnapshots();
	void LoadSnapshotToBase( int i );//writes the stored values into the programmer
	bool RunSnapshots( double dt );//fades + layer (engine thread, called by RunEffects)
	Projector ProjectorOut( int i ) const;//projector with the snapshots applied (render)
	int ActiveSnapshots() const;
	//group faders
	void ApplyGroup( int i );
	void SetColor( FixtureInst& f, const V3& c, int head = -1 );//programmer colour (RGB/W, CMY)
	//mounting
	bool GetMountFrame( MountType t, int idx, MountFrame& fr ) const;
	void UpdateMounts();//lamps follow their truss / stand (call every frame)
	void Attach( FixtureInst& f, MountType t, int idx );//keeps the lamp where it is
	void HangOnTruss( FixtureInst& f, int idx );//attach + move under the truss if it is far away
	void PlaceOnStand( const std::vector< FixtureInst* >& fx, int idx );//attach + put on top (spread on the T-bar)
	void RemoveTruss( int i );
	void RemoveStand( int i );
	int AddStand();                         //new stand beside the stage (free spot), turned towards the stage centre; returns index
	int CloneStand( int i, const V3& off ); //copy of stand i, shifted; returns index
	void AutoMount( float maxDist = 0.7f );//free lamps close to a truss get attached to it
	int MountedCount( MountType t, int idx ) const;
	//video projection
	float FloorAt( float x, float z ) const;//highest deck top (main deck, risers) at x/z, else 0
	float DeckBase( const Deck& d ) const;   //what a riser stands on (0 or the main deck)
	static bool DeckContains( const Deck& d, float x, float z );
	//nearest top surface of the main deck / a riser along a ray (t > 0); false = none
	bool RayDeckTop( const V3& o, const V3& dir, float& t ) const;
	float ThrowDistance( const ProjectorGeo& g ) const;//along the lens axis to the first screen / floor / wall
	//aim at a screen: frontal = put the projector straight in front (keeps the zoom), else keep the place and turn + zoom
	void AimProjector( Projector& p, const Screen& s, bool frontal, int srcW = 0, int srcH = 0 ) const;
	Projector& AddProjector();//in front of the stage, aimed at the first screen
	Screen& AddScreen();      //on the stage, next to the existing ones
	void RecallSnapshot( int i, bool keepOthers = false );//start (fade in) and put on top; keepOthers: older ones stay on
};

bool SnapFadeable( Attr a );//attributes that crossfade (others switch)
const char* MidiTypeName( MidiType t );
struct Json;
Json MidiMapToJson( const MidiMap& m );
MidiMap MidiMapFromJson( const Json& o );
//mood presets for StageCfg (place, ground, sky, ambient): 0 Halle neutral, 1 Open Air Nacht, 2 Blaue Stunde, 3 Stadtnacht, 4 Komplett dunkel
extern const char* const kMoodNames[ 5 ];
void ApplyMood( StageCfg& s, int mood );
V3 MoonDir( const StageCfg& s );//unit vector towards the moon
const char* MidiTargetName( MidiTarget t );
}//namespace ls
