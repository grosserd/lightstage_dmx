// LightStage 2.0: show timeline - tracks of fixture groups / DMX channels / the cue list, keyframe lanes,
// transport and sync (internal, MIDI timecode, beats from Link / MIDI clock).
// The timeline is a layer on top of the snapshots (below the effects): it writes into Show::snapOut.
#pragma once
#include "fixture.h"
#include <string>
#include <vector>

namespace ls
{
struct Json;
struct Show;

//how the curve leaves a key towards the next one
enum class TlInterp : uint8_t { Step, Linear, Smooth, Bezier, EaseIn, EaseOut, Count };
struct TlKey
{
	double t = 0;           //seconds
	float v  = 0;           //0..1 (attribute / DMX lanes)
	V3 c{ 1, 1, 1 };        //colour lanes (linear 0..1)
	TlInterp interp = TlInterp::Smooth;
	//Bezier handles: share of the segment in time (0..1) and value offset; out = towards the next key,
	//in = arriving from the previous key
	float ox = 0.33f, oy = 0;
	float ix = 0.33f, iy = 0;
	bool sel = false;//runtime: selected in the editor
};
enum class TlLaneKind : uint8_t { Attr, Color, Dmx };
struct TlLane
{
	TlLaneKind kind = TlLaneKind::Attr;
	Attr attr       = Attr::Dimmer;//Attr lanes
	int uni = 1, ch = 1;           //Dmx lanes: universe number, 1-based channel
	bool fine = false;             //Dmx lanes: 16 bit (ch = coarse, ch + 1 = fine)
	std::vector< TlKey > keys;     //sorted by time
	bool mute  = false;
	int height = 1;                //0 small, 1 normal, 2 large (editor)
	//fan over the lamps of a group track (lamp order: fanOrder)
	float fanTime  = 0;            //seconds: the last lamp follows this much later (negative = earlier)
	float fanValue = 0;            //value spread: first lamp - fanValue / 2 .. last lamp + fanValue / 2
	int fanOrder   = 0;            //0 track order, 1 left -> right, 2 centre out, 3 reverse, 4 front -> back, 5 outside in
	bool rec       = false;        //armed: records while the timeline plays with record on
	//runtime: recorded samples of the current pass (time, value / colour)
	std::vector< std::pair< double, V3 > > recBuf;
	bool recLatch = false;         //runtime: latch record - touched once in this pass
	float recLastV    = -1;        //runtime: touch record - last sampled value and when it last changed
	double recLastChg = 0;
	bool HasFan() const { return fanTime != 0 || fanValue != 0; }
};
//block on a clip track: a snapshot or an effect, faded in and out
enum class TlClipKind : uint8_t { Snapshot, Effect };
struct TlClip
{
	double t = 0, len = 4;     //seconds
	TlClipKind kind = TlClipKind::Snapshot;
	uint32_t snap = 0;         //snapshot uid
	int fx = 0;                //effect id
	float fadeIn = 0.5f, fadeOut = 0.5f;//seconds
	float level  = 1.0f;
	bool sel     = false;      //runtime
	float Weight( double T ) const;//0..1 at timeline time T (fades)
};
//Show = Lightshow track: plays the recorded DMX frames of a .lightshow file 1:1 (file, offset; lbshow.h)
enum class TlTrackKind : uint8_t { Fixtures, Dmx, Cues, Clips, Audio, Show };
struct TlTrack
{
	uint32_t uid = 0;
	std::string name;
	TlTrackKind kind = TlTrackKind::Fixtures;
	std::vector< int > fixtures;//fixture ids (group track)
	std::vector< TlLane > lanes;
	bool mute = false, solo = false, open = true;
	float level = 1.0f;//share of this track against what is underneath
	V3 color{ 0.3f, 0.75f, 0.85f };
	std::vector< TlClip > clips;//clip track
	//audio track: file (played with the timeline), gain, start of the file on the timeline
	//Lightshow track: file (.lightshow) + start on the timeline (offset)
	std::string file;
	float gain    = 1.0f;
	double offset = 0;
};
struct TlMarker
{
	double t = 0;
	std::string name;
};
enum class TlSync : uint8_t { Internal, Mtc, Beat, Ltc };
//runtime (never saved, kept across undo)
struct TlRuntime
{
	double pos     = 0;    //playhead, seconds
	bool playing   = false;
	uint32_t seq   = 0;    //start number of the last play / locate: programmer moves after it win on their channels
	bool extRun    = false;//external sync is running
	double extIdle = 0;    //seconds since the external source stopped (freewheel)
	double lastBeat = -1e18;
	int located    = 0;    //frames since a locate (cue track follows jumps while stopped)
	bool any       = false;//output written in the last frame
	double recT    = -1;   //record: last recorded time
	bool recOn     = false;//a record pass is running
	int recCommits = 0;    //passes written into the lanes (UI: status)
};
struct Timeline
{
	std::vector< TlTrack > tracks;
	std::vector< TlMarker > markers;
	double length = 120;  //seconds
	bool loop     = false;
	double loopA = 0, loopB = 16;
	TlSync sync     = TlSync::Internal;
	double offset   = 0;  //external time + offset = timeline time
	float freewheel = 1.0f;//seconds the timeline keeps running when the external timecode stops
	float bpm       = 120;//beat grid + Beat sync (beats -> seconds)
	int beatsPerBar = 4;
	int grid        = 3;  //editor grid: 0 off, 1 bar, 2 beat, 3 1/2, 4 1/4 beat, 5 1/8 beat, 6 1 s, 7 0.1 s, 8 frames
	bool snap       = true;
	float master    = 1.0f;
	bool output     = true;//writes to the lamps
	int fps         = 25;  //frame display
	bool record     = false;//armed lanes record while playing
	int recMode     = 0;   //0 overwrite (the whole pass), 1 touch (only while the channel is held), 2 latch (from the first touch to the end of the pass)
	float recThin   = 0.01f;//thinning tolerance (share of the range)
	std::string ltcDevice; //audio input for LTC ("" = default)
	int ltcChannel  = 0;   //0 left, 1 right
	TlRuntime rt;

	TlTrack* Find( uint32_t uid );
	uint32_t NewUid() const;
	TlTrack* CueTrack();
	bool Empty() const { return tracks.empty(); }
	double GridStep() const;//seconds, 0 = off
	double Snap( double t ) const;
	void Locate( Show& s, double t );//moves the playhead (new start number)
	void Play( Show& s, bool on );
};

const char* TlInterpName( TlInterp i );
void TlSort( TlLane& l );
//value of a lane at time t (keys must be sorted); has = false when the lane has no keys
float TlEval( const TlLane& l, double t, bool* has = nullptr );
V3 TlEvalColor( const TlLane& l, double t, bool* has = nullptr );
std::string TlLaneName( const TlLane& l );
//adds a key (or moves the value of a key at the same time); returns its index
int TlSetKey( TlLane& l, double t, float v, const V3* c = nullptr, TlInterp interp = TlInterp::Smooth );

Json TimelineToJson( const Timeline& tl );
void TimelineFromJson( Timeline& tl, const Json& j );

//engine (engine thread / UI with the engine lock)
void TimelineAdvance( Show& s, double dt );//transport + sync, before the cue list runs
void TimelineApply( Show& s );             //after the snapshots: writes the lanes into snapOut
//the programmer's (base) value of a lane - for "set key from the current values"
float TlLiveValue( Show& s, const TlTrack& t, const TlLane& l, V3* col = nullptr );
//attributes a group of lamps can be automated with (in channel order of the first lamp)
std::vector< Attr > TlTrackAttrs( Show& s, const std::vector< int >& fixtures );
//position 0..1 of every lamp of a track in fan order (parallel to t.fixtures)
void TlFanPos( Show& s, const TlTrack& t, int order, std::vector< float >& out );
//record: writes the pass of every armed lane into its keys (thinned); returns lanes written
int TlCommitRecord( Show& s );
int TlCommitLane( TlLane& l, float eps );
//the programmer moved a channel of this lane after the last play / locate (it has priority there)
bool TlLaneTouched( Show& s, const TlTrack& t, const TlLane& l );
void TlLaneRelease( Show& s, const TlTrack& t, const TlLane& l );//gives the lane's channels back to the timeline
//Ramer-Douglas-Peucker on (time, value): indices of the points to keep
void TlThin( const std::vector< std::pair< double, V3 > >& pts, float eps, bool color, std::vector< int >& keep );
//pan / tilt of a lane value in degrees (first lamp of the track with that attribute); false = no range
bool TlDegrees( Show& s, const TlTrack& t, Attr a, float v, float& deg );
}//namespace ls
