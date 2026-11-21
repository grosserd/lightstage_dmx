// application state + ImGui user interface
#pragma once
#include "dmxio.h"
#include "midi.h"
#include "osc.h"
#include "render.h"
#include "fixture_editor.h"
#include "effects.h"
#include "link.h"
#include "params.h"
#include "bridge.h"
#include "miditargets.h"
#include "resolume.h"
#include "json.h"
#include "ui_reset.h"
#include "audio.h"
#include "lbshow.h"
#include <map>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

struct ImFont;
struct ImDrawList;
namespace ls
{
struct AddDialog
{
	bool open = false;
	char search[ 128 ] = {};
	std::string mfFilter;
	const FixtureDef* def = nullptr;
	int mode = 0, count = 1, universe = 1, address = 1, truss = 0, placement = 0;
	int stand = -1;//"Auf Stativ": -1 = a new stand for every lamp, else all on this stand
	bool autoAddr = true;
	//swap mode: replace the type of these lamps (position, mount, address stay)
	bool replace = false;
	std::vector< int > replaceIds;
	bool readdress = true;//addresses that collide with the new footprint get the next free range
	char name[ 64 ] = {};
	float spacing = 1.2f;
	std::vector< const FixtureDef* > list;
	std::string lastSearch;
};

class App
{
public:
	bool Init( std::string* err );//the main window comes from the platform layer (PlatformInit)
	void Shutdown();
	void Frame( float dt );//simulation + UI (call between ImGui::NewFrame and ImGui::Render)
	bool WantsQuit() const { return quit_; }
	bool ConfirmClose();//true = ok to close
	void DropFile( const std::string& path );

	Library lib;
	Show show;
	SimGlobals sim;
	Renderer ren;
	DmxIO io;
	MidiIn midi;
	OscIn osc;
	Bridge bridge;//Ableton / remote control (HTTP manifest + UDP, see BRIDGE_PROTOKOLL.md)
	bool bridgeOn = false;//settings.json
	int bridgeHttpPort = 8001;
	void ExportManifest();
	VideoHub video;//projector inputs
	Camera cam;
	std::vector< int > sel;
	ImFont* fontUi   = nullptr;
	ImFont* fontMono = nullptr;
	float ui = 1.0f;//display scale (DPI / 96)
	bool uiScaled_ = false;
	ImFont* fontBig  = nullptr;
	std::string exeDir;
	std::string userDir;//writable: settings, autosave, own fixtures / effects (= exeDir except in a macOS app bundle)
	bool demoFx    = false;
	std::string startTab;//select this bottom tab on start (--tab)
	int startScreen = -1;//select this screen in the Video tab (--screen)
	int startTabFrames_ = 0;
	bool showLabels = true;
	float fps      = 0;
	std::string status;
	double statusTime = 0;

	//helpers shared with tests / demo
	void BuildDemo();
	void BuildJazzDemo();//2.1 start-up demo (jazz club)
	bool BuildShowcase( int n, std::string& title );//example shows 0..5 (--showcase <dir>)
	FixtureInst* AddFixture( const FixtureDef* def, int mode, int uni, int addr, const V3& pos, const V3& rot, const std::string& name );
	static bool AimAt( Show& s, FixtureInst& f, const V3& target );
	void NewShow();
	bool OpenShow( const std::string& path );
	bool SaveShow( const std::string& path );
	void SetStatus( const std::string& s );
	int AddEffectOfType( FxType t );//effect builder: new effect of a type on the selection (empty = all that fit), started
	void RenderViewport( int w, int h );
	//engine: MIDI / OSC / Art-Net in -> DMX, own thread, independent of the UI frame rate
	void StartEngine();
	void StopEngine();
	bool EngineRunning() const { return engineRun_; }
	void* EngineEvent() const { return engineEvent_; }
	float engineHz = 0;
	//performance mode (live)
	void SetPerfMode( bool on );
	void SetPerfView( int v );//0 off, 1 preview, 2 full
	bool PerfMode() const { return perfMode_; }
	bool ShouldRenderViewport( double now ) const;
	double FrameInterval() const;//main loop pacing: 0 = vsync, else seconds per UI frame
	//fixture editor
	void OpenFixtureEditor( const FixtureDef* d );//null = new lamp
	bool SaveFixtureEditor( bool test );
	bool RegisterEmbeddedFixture( const std::string& id, const Json& ofl );//from a show file -> library\user
	EdFixture fxEd;
	bool fxEdOpen = false;
	void LoadSettings();
	void SaveSettings();
	void DuplicateSelected();//copies keep their mount; lamps on a stand get their own (copied) stand
	//lamp types: swap the type of lamps in place, delete own lamp types
	int SwapFixtureType( const std::vector< int >& ids, const FixtureDef* d, int mode, bool readdress );//returns lamps changed
	bool DeleteUserFixture( const FixtureDef* d, std::string* why );
	void OpenSwapDialog( const std::vector< int >& ids );
	//MIDI mappings as files
	bool ExportMidi( const std::string& file );
	int ImportMidi( const std::string& file, bool replace );//number of mappings read, -1 = error
	std::string MidiDefaultFile() const { return userDir + LS_SEP "midi_default.lsmidi"; }
	//effects + tempo (engine thread or UI when the engine is off)
	void UpdateEffects( double now );
	std::string UserFxFile() const { return userDir + LS_SEP "library" LS_SEP "user" LS_SEP "effects.json"; }
	//right-click menu on the last widget: copy the OSC address / MIDI learn; in learn mode it is learnable
	void OscContext( const std::string& addr );
	//Ableton Control-device knobs / buttons (both ways, see Show::ableton)
	std::vector< std::pair< std::string, std::string > > AbletonDevices();//id, label: tables of the show + connected Control devices
	bool AbletonConnected( const std::string& id );
	void AbletonAssign( const std::string& id, const std::string& label, bool knob, int slot, const std::string& key );
	void AbletonAssignMenu( const std::string& key );//submenu "Ableton-Regler" inside a context menu
	std::string TargetLabel( const std::string& key );//label of a midiTargets key ("" = not in the show)
	void AbletonTable();
	std::string ablKeyNext_;//set by LearnHook for the next OscContext (fixture attributes use their attr: key)
	std::map< std::string, std::string > tl_;
	double tlT_ = -10;
	//2.0 show timeline (ui_timeline.cpp)
	//block on a clip track at time t: the track uid if it is a clip track, else the first clip track, else a new one. Returns the track uid
	uint32_t TimelineAddClip( int kind, uint32_t snap, int fx, double t, uint32_t trackUid );
	//audio file onto the Time Line: new audio track (true) or the file of the existing one is replaced (false); offset < 0 = keep / 0
	bool AddAudioTrack( const std::string& file, double offset );
	uint32_t AddTimelineTrack( const std::vector< int >& ids, Attr attr, bool pickLanes );//group track; returns uid
	void TimelineAddMenu( const std::vector< int >& ids, const Channel* ch, int uni, int adr );//items for context menus
	void TimelineChannelMenu( int uni, int adr );//timeline items for one DMX address (Inspector channel list, DMX tab)
	void TimelineToggle();//play / pause
	void ShowTimelineWindow() { tlWin_ = true; }
	void BuildTimelineDemo();
	void SetBottomHeight( float h ) { bottomH_ = h; }//example timeline on the current show (screenshots, --tldemo)
	bool tlWin_ = false, tlSelectTab_ = false, tlHovered_ = false, tlHoveredNow_ = false;
	uint32_t tlLanePick_ = 0;//open the lane picker for this track
	//scripted UI test (--uitest, uitest.cpp): the UI reports places on screen by name, the script feeds the mouse
	bool uiTest_ = false;
	std::map< std::string, ImVec4 > uiMarks_;
	struct UiTestState* ut_ = nullptr;
	void UiMark( const std::string& name, const ImVec2& a, const ImVec2& b ) { if( uiTest_ ) uiMarks_[ name ] = ImVec4( a.x, a.y, b.x, b.y ); }
	void UiTestStart( const std::string& logPath );
	void UiTestInput();//before ImGui::NewFrame
	bool UiTestDone() const;
	int UiTestFails() const;
	//a file dropped at a place of the window (tests; DropFile uses the real mouse)
	void DropFileAt( const std::string& path, float x, float y ) { dropPt_ = true; dropX_ = x; dropY_ = y; DropFile( path ); dropPt_ = false; }
	bool dropPt_ = false;
	float dropX_ = 0, dropY_ = 0;
	//drag & drop onto the Time Line: payload "LS_TLCLIP" (snapshot / effect from its list)
	struct TlClipDrag { int kind; uint32_t snap; int fx; };
	struct { bool on = false; TlClipDrag d{}; double t = 0; uint32_t track = 0; } tlDrop_;//dropped: applied at the start of the next frame
	struct { float x0 = 0, y0 = 0, x1 = 0, y1 = 0, pps = 0; double view0 = 0; int frame = -10; } tlGeo_;//time area on screen (files dropped from Windows)
	uint32_t tlConvert_ = 0; //Lightshow track to convert into time lanes (next frame)
	bool TlConvertShowTrack( uint32_t uid );
	std::string OpenLightshowDialog();
	uint32_t tlSelTrack_ = 0;//track selected in the timeline (3D path, K)
	bool tlPath_         = true;//movement path of the selected track in the viewport
	void TimelineTick();     //UI frame: audio track playback, LTC input
	void TimelineViewportOverlay( ImDrawList* dl, const ImVec2& p0, const ImVec2& size );
	std::string OpenAudioDialog();
	AudioPlayer tlAudio_;
	std::shared_ptr< AudioData > tlAudioData_;
	std::string tlAudioFile_, tlAudioErr_;
	LtcInput ltcIn_;
	void ShowOscWindow() { oscWin_ = true; }
	void ShowResolumeWindow() { resWin_ = true; }
	void ShowLbxWindow() { lbxWin_ = true; }
	std::string lbxUiTest;//test: start the Lightshow export from the UI frame like the button (--lightshowui)
	int lbxUiState = 0;   //0 waiting, 1 running, 2 finished, 3 start failed
	//2.0: Lightshow import (.lightshow -> new show / current show, as time lanes or as a Lightshow track)
	bool LbiOpen( const std::string& path );//reads the file and opens the import window
	bool LbiRun();                           //imports with the window's options
	LbxImportOptions lbiOpt_;
	LbxImportResult lbiRes_;
	void ShowAbletonWindow() { ablWin_ = true; }
	std::unique_ptr< LinkSync > link;
	void DeleteSelected();   //stands left empty by deleted lamps are removed too
	//Delete / Backspace: a hovered list row takes it (DelRow), else what was selected last (DeleteByContext)
	bool DelRow( float y0 );
	bool noAutosave = false;//screenshots / tools: no autosave, no recovery prompt (a running LightStage keeps its file)
	void DeleteByContext();
	bool delKey_ = false;
	int delCtx_  = 0;//1 lamps, 2 effect, 3 projector, 4 screen
	std::vector< int > prevSel_;
	int prevFxSel_ = 0, prevProj_ = -1, prevScreen_ = -1;
	//1.2: position tools for the selection
	void FanBegin( const std::vector< int >& ids );//remembers pan / tilt as the starting point
	int FanApply( float panDeg, float tiltDeg );    //spreads the lamps left -> right by these degrees (negative = mirrored)
	int AimSelected( const V3& target );            //moving heads / scanners point at the target
	bool RayStage( float mx, float my, float w, float h, V3& hit );//viewport point -> stage deck / floor
	//1.2: video export (offline: the show is calculated frame by frame, independent of the real time)
	struct ExportJob
	{
		bool active = false;
		std::string path, err;
		int res = 1, fpsIdx = 1;//1920x1080, 30 fps
		int w = 1920, h = 1080, fps = 30;
		float seconds = 10;
		int camMode   = 0;//0 current view, 1 orbit 360°, 2 swing +-45°
		bool cueStart = false;//cue list from the start, timecode simulated from tcStart
		bool timeline = true; //play the show timeline from tlStart (when it has tracks)
		double tlStart = 0;
		TlSync tlSync = TlSync::Internal;
		bool tlPlaying = false;
		double tcStart = 0;
		int frame = 0, frames = 0;
		Camera cam0;
		bool engineWas = false, projLines = true;
		double started = 0;
		std::unique_ptr< VideoWriter > vw;
	};
	ExportJob exp_;
	bool ExportStart( const std::string& path );
	bool ExportStep( double budgetSec );//false = finished (or failed)
	void ExportFinish( bool ok );
	bool ExportActive() const { return exp_.active; }
	//1.2: Resolume Advanced Output preset from the projector / screen setup (ui_resolume.cpp)
	bool resWin_ = false;
	ResolumeOpts resOpt_;
	std::vector< ResolumeScreen > resLayout_;
	double resT_ = -1;
	std::string resMsg_;
	bool resMsgErr_ = false;
	void ResolumeWindow();
	void ResolumeUpdate();//rebuilds resLayout_ from the show
	bool ResolumeSave( const std::string& path );
	//1.2: autosave (normal runs only, not the self test): every few minutes while there are unsaved changes;
	//after a crash the next start offers to restore it
	int autosaveMin = 2;//0 = off (settings.json)
	std::string AutosaveFile() const { return userDir + LS_SEP "autosave.lightstage"; }
	std::string RecoverFile() const { return userDir + LS_SEP "autosave_recover.lightstage"; }
	bool AutosaveWrite( const std::string& file );
	bool AutosaveRecover( const std::string& file );//loads it, keeps the original file name, marks unsaved
	void AutosaveClear();
	//undo / redo: whole states of the show set-up (Show::ToJson( true )), live values stay as they are.
	//A change made in the UI becomes one step once it has settled (slider let go, typing paused);
	//changes from MIDI / OSC / Art-Net / Ableton alone are taken over without a step.
	void Undo();
	void Redo();
	bool CanUndo() const { return !undo_.empty(); }
	bool CanRedo() const { return !redo_.empty(); }
	void UndoReset();            //new starting point (new / opened show), empties both lists
	void UndoTick( bool force ); //per frame; force = take a pending change now (before undo / redo, tests)
	size_t UndoCount() const { return undo_.size(); }
	size_t RedoCount() const { return redo_.size(); }
	void UndoMarkUser() { undoTouched_ = true; }//tests: the next change counts as made in the UI

private:
	void Menu();
	void PanelPatch();
	void PanelInspector();
	void PanelBottom();
	void PanelViewport();
	void TabDmx();
	void TabMidi();
	void TabOutput();
	void TabStage();
	void TabSnapshots();
	void TabCues();
	void TabPalettes();
	void TabGroups();
	void TabTimeline();
	void TimelinePanel();
	void TimelineWindow();
	void TabVideo();
	void TabEffects();
	void EffectEditor( Effect& e );
	void OscWindow();
	void TabBridge();
	void ProjectorUi( Projector& p, int idx );
	void ScreenUi( Screen& s, int idx );
	void LibraryPopup();
	void Programmer( std::vector< FixtureInst* >& fx );
	void ChannelList( FixtureInst& f );
	bool LearnHook( const MidiMap& tmpl );
	void HandleMidi();
	void HandleOsc();
	void DemoFx( float dt );
	void Pick( float mx, float my, float w, float h, bool add );
	void FrameSelection();
	void Splitter( bool vertical, float* size, float minv, float maxv, float sign, float length );
	bool IsSel( int id ) const;
	void Select( int id, bool add, bool range );

	bool quit_ = false;
	float leftW_ = 330, rightW_ = 400, bottomH_ = 340;
	AddDialog add_;
	int dmxUni_ = 1;
	bool learn_ = false;
	bool learnArmed_ = false;
	MidiMap learnTmpl_;
	std::string learnText_;
	int lastClicked_ = -1;
	int pixelSel_    = -1;
	std::vector< ComPort > ports_;
	std::string detectInfo_;
	bool dragging_ = false, orbiting_ = false;
	float dragDist_ = 0;
	double demoT_   = 0;
	char snapName_[ 64 ] = "Look";
	bool snapWithFx_ = true, snapWithProj_ = false;
	int tab_ = 0;
	std::vector< MidiEvent > events_;
	bool showStats_ = true;
	bool vpBarFold_ = false;//viewport toolbar folded to master + LIVE (settings.json)
	uint64_t lastSent_ = 0, lastRecv_ = 0;
	float sentRate_ = 0, recvRate_ = 0;
	double rateT_ = 0;
	bool viewportHovered_ = false;
	int projSel_ = -1, screenSel_ = -1;//Video tab selection
	//engine
	void EngineLoop();
	void PollOsc();
	void OscListenerUpdate();
	void BridgeListenerUpdate();
	bool bridgeHinted_ = false;
	bool targetsWin_   = false;
	char targetFilter_[ 128 ] = "";
	void TargetsWindow();
	bool ablWin_ = false;//window "Ableton-Belegungen"
	void AbletonWindow();
	bool expWin_ = false;
	void ExportWindow();
	//2.0: Lightshow export (.lightshow, lbshow.h) - runs like the video export (engine stopped, frame by frame)
	bool lbxWin_ = false;
	int lbxUiWait_ = 0;
	LbxOptions lbxOpt_;
	LbxJob lbx_;
	bool lbxEngineWas_ = false;
	float lbxFrom_ = 0, lbxTo_ = 0;//UI: range (to 0 = end of the timeline)
	bool LbxStart( const std::string& path );
public:
	bool MoveFixturesUi( int dragged, int anchorId, bool after );//PATCH list drag & drop (also the self test)
private:
	void LbxEnd( bool ok );
	void LbxWindow();
	bool lbiWin_ = false;
	LbxFile lbiFile_;
	std::string lbiPath_;
	void LbiWindow();
	void AutosaveTick();
	double autosaveT_   = 0;
	bool autosaveInit_  = false, recoverAsk_ = false;
	std::string recoverInfo_;
	void PanelLive( float w, float h );
	void PerfWindow();
	void FixtureEditor();
	std::string fxEdId_, fxEdMsg_;
	int fxEdSel_ = -1, fxEdMode_ = 0, fxEdTestId_ = -1;
	void UpdateCpu();
	std::recursive_mutex engineLock_;
	std::unique_lock< std::recursive_mutex >* frameLock_ = nullptr;
	std::thread engine_;
	std::atomic< bool > engineRun_{ false }, engineBoost_{ false };
	void* engineEvent_  = nullptr;
	double lastInputT_  = 0;//earliest unsent input time (latency)
	uint32_t lastKickVer_ = 0;
	//performance mode
	bool perfMode_ = false, perfWin_ = false, settingsLoaded_ = false;
	//2.1: visual quality levels (0 draft, 1 normal, 2 high) + automatic throttling; status bar
public:
	int quality_ = 1;          //chosen level 0 draft, 1 normal (= 2.0), 2 high (settings.json "renderQuality")
	bool qualityAuto_ = true;  //step down while the frame rate is low (settings.json "qualityAuto")
	int qualityEff_ = 1;       //level in use (<= quality_)
	void ApplyQuality();       //renderer limits from performance mode + quality level (export: always high)
	void QualityTick( float dt );
	void StatusBar();
	bool SurfaceUi( const char* id, const char* label, Surface& sf );//2.1 stage tab: reflection of a floor / deck
	int lastOutputTabFrame_ = -10;//frame in which the Output tab was drawn (UI test)
private:
	float qLow_ = 0, qHigh_ = 0;//seconds below / above the frame rate limits
	uint64_t oscSeen_ = 0;
	double oscT_ = -10;
	std::string languageSaved_;//settings.json "language" ("de" / "en"), empty = default English
	int perfView_  = 1;
	float perfFps_ = 20, perfScale_ = 0.5f;
	double lastViewRender_ = 0, lastUiInput_ = 0;
	float cpu_ = 0, blackoutPrev_ = 1;
	int liveSnap_ = -1;
	std::string powerPlan_;
	bool videoTab_ = false;
	//effects
	double fxT_ = 0;
	float linkBpmSent_ = -1;
	std::vector< FxPreset > userPresets_;
	bool userPresetsLoaded_ = false;
	char presetName_[ 64 ] = "";
	bool oscWin_ = false;
	char oscFilter_[ 128 ] = "";
	int viewW_ = 0, viewH_ = 0;
	//position tools
	std::vector< std::pair< int, std::pair< float, float > > > fanBase_;//id, (pan, tilt) left -> right
	float fanPan_ = 0, fanTilt_ = 0;
	V3 aimPt_{ 0, 0, 0 };
	bool aimPick_ = false;
	//undo
	struct UndoEntry
	{
		std::string state;
		uint64_t seq = 0;//changes from outside up to this number are contained
	};
	std::string UndoState();
	void RestoreState( const UndoEntry& e );
	std::vector< UndoEntry > undo_, redo_;
	uint64_t undoSeq_ = 0;
	//changes from outside (path -> number, value): put onto older states when they come back
	std::map< std::string, std::pair< uint64_t, Json > > undoExt_;
	std::string undoCur_, undoSeen_;//current committed state, pending changed state
	double undoCheckT_ = 0, undoChgT_ = 0, undoPendT_ = 0;
	bool undoTouched_ = false;//the user worked in the UI since the last step
};
}//namespace ls
