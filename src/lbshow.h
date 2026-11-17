// Lightshow export (.lightshow): the finished DMX output of the show timeline, frame by frame, plus cues, tempo,
// channel roles and simplified 3D view data - for playback without LightStage (first player: the Lightbrick app on
// the TrimUI Brick Pro). Format: FORMAT.md in E:\Projekte\Lightbrick. Runs like the video export: engine stopped, the show is
// computed frame by frame (independent of real time), then the state is restored.
#pragma once
#include "show.h"
#include "sim.h"
#include <string>
#include <memory>
#include <mutex>
#include <vector>

namespace ls
{
struct LbxOptions
{
	double from = 0;   //timeline seconds
	double to   = -1;  //< 0 = end of the timeline
	int fps     = 40;
	bool visual = true;//VIS0 stream (3D view on the Brick)
	int substeps = 5;  //effect steps per frame
};
struct LbxJob
{
	bool active = false;
	std::string path, err;
	LbxOptions o;
	int frame = 0, frames = 0;
	double started = 0;
	//saved state
	TlSync sync     = TlSync::Internal;
	bool playing    = false;
	bool loop       = false;//the export runs straight through (the loop range only goes into INFO)
	double pos      = 0;
	//streams
	int dmxBytes = 0, visBytes = 0;
	std::vector< uint8_t > prevDmx, prevVis, dmxData, visData;
	std::vector< uint32_t > dmxKeys, visKeys;
	std::vector< int > visOff, visHeads;//per fixture (show order): record offset, heads stored (0 = single)
	std::vector< int > visIds;
	//for the self test: keep the decoded frames
	bool keepFrames = false;
	std::vector< std::vector< uint8_t > > keptDmx;
};
bool LbxBegin( Show& s, const LbxOptions& o, const std::string& path, LbxJob& j );
bool LbxStep( Show& s, SimGlobals& sim, LbxJob& j, double budgetSec );//false = finished (or failed: j.err)
bool LbxFinish( Show& s, LbxJob& j, bool ok );                       //writes the file when ok, restores the timeline
bool LbxExport( Show& s, SimGlobals& sim, const LbxOptions& o, const std::string& path, std::string* err );//all at once

//reader (self test, tools)
struct LbxFile
{
	Json info, cues, scene, patch;//patch: the show's setup (PTCH chunk, LightStage 2.0+; null in older files)
	int fps = 0, frames = 0, dmxBytes = 0, visBytes = 0, keyEvery = 0;
	std::vector< std::vector< uint8_t > > dmx, vis;//decoded frames
	size_t fileBytes = 0;
};
bool LbxRead( const std::string& path, LbxFile& out, std::string* err );

//---------------------------------------------------------------- import (.lightshow -> LightStage)
//Two ways: as editable time lanes (DMX time lanes per lamp, thinned like a recording) or as a Lightshow track
//(TlTrackKind::Show) that plays the recorded frames 1:1 from the file. Target: a new show (setup from the file)
//or the current show (channels by universe number / address).
struct LbxImportOptions
{
	bool newShow    = true;
	bool asTrack    = false;//false = time lanes, true = Lightshow track (1:1)
	double at       = 0;    //start on the timeline (s)
	float tolerance = 0.5f; //time lanes: allowed deviation in DMX steps (thinning)
	bool markers    = true; //markers + cues of the file as timeline markers
	bool tempo      = true; //BPM, beats per bar, loop range
};
struct LbxImportResult
{
	int tracks = 0, lanes = 0, keys = 0, fixtures = 0, universesAdded = 0, markers = 0, constants = 0;
	bool fromPatch = false;//new show: setup from the PTCH chunk (else rebuilt from SCEN, generic profiles)
	int skippedFixtures = 0;//new show without PTCH: lamps without a fitting generic profile
	std::string warn;
};
//userFixtures of the patch must already be in lib (App registers them). newShow clears s.
bool LbxImport( Show& s, const Library& lib, const LbxFile& f, const std::string& path, const LbxImportOptions& o, LbxImportResult* r, std::string* err );

//Lightshow tracks: the file's DMX frames, decoded on demand (shared by the engine and the UI, keyed by path)
struct LbxPlayer
{
	std::string path, err;
	int fps = 40, frames = 0, frameBytes = 0, keyEvery = 40;
	double duration = 0;
	std::vector< int > uniNums;      //file universe order -> universe number
	std::vector< uint8_t > stream;   //DMX0 frame data (after the key table)
	std::vector< uint32_t > offsets; //per frame: byte offset in stream
	std::vector< uint8_t > activity; //per frame: mean of all DMX bytes (overview in the track)
	std::string name;
	//decode cursor (guarded by mu)
	std::mutex mu;
	int curFrame = -1;
	std::vector< uint8_t > cur;
	bool Frame( int f, std::vector< uint8_t >& out );//copies the decoded frame f
};
std::shared_ptr< LbxPlayer > LbxPlayerGet( const std::string& path );//nullptr = not loaded / failed
std::shared_ptr< LbxPlayer > LbxPlayerLoad( const std::string& path, bool reload = false );//loads (UI thread); failed: err set
void LbxEnsurePlayers( const Show& s );//loads every Lightshow track's file that is not loaded yet (no retry of failures)
}//namespace ls
