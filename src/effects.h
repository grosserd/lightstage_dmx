// light effects (chases, waves, colour and movement patterns) on top of the DMX base values,
// tempo clock (internal / tap, MIDI clock, Ableton Link via link.h), presets and programs
#pragma once
#include "json.h"
#include "show.h"

namespace ls
{
//---------------------------------------------------------------- tempo
void TempoAdvance( Tempo& t, double dt, double now );//internal + MIDI clock (interpolated between ticks)
void TempoMidiClock( Tempo& t, uint8_t status, int spp, double time );//0xF8 tick, 0xFA start, 0xFB continue, 0xFC stop, 0xF2 song position
void TempoTap( Tempo& t, double now );
void TempoResync( Tempo& t );//now = start of a bar
float TempoBpm( const Tempo& t );//the tempo the effects run at

//---------------------------------------------------------------- effects
//fades + overlay; returns true when show.fxOut changed (caller kicks the output)
bool RunEffects( Show& show, double dt );
float FxWaveValue( FxWave w, float t, float width, uint32_t seed, long long cycle );
float FxCurveValue( const std::vector< FxPoint >& c, float t, bool periodic );//drawn wave / envelope
float FxEffWave( const Effect& e, float t, uint32_t seed, long long cycle );//the effect's wave (drawn or built in)
std::vector< FxPoint > FxCurveFromWave( FxWave w, float width );
FxType FxMergedType( FxType t );//Chase/Wave/Strobe/Sparkle -> Intensity, colour types -> Colour
//per-element preview for the editor: value 0..1 (dimmer / attribute), colour, position offset
struct FxPreviewEl
{
	int fixture = 0, head = -1;
	float v = 0;
	V3 col{ 1, 1, 1 };
	float ox = 0, oy = 0;
};
void PreviewEffect( Show& show, const Effect& e, std::vector< FxPreviewEl >& out );
//which lamps an effect of this kind can drive
bool FxSupports( const Show& show, const FixtureInst& f, const Effect& e );

const char* FxKindName( FxKind k );
const char* FxTypeName( FxType t );//German key (display: Ls)
const char* FxTypeInfo( FxType t );
V3 FxTypeColor( FxType t );
bool FxIsModulator( FxType t );
FxType FxTypeGuess( const Effect& e );
Effect FxMakeType( FxType t );//new effect of a builder type with good defaults (no lamps)
FxLane FxNewLane( const Effect& e, int idx );
void FxResizeSteps( Effect& e, int steps );
const std::vector< int >& FxFixtureIds( const Show& show, const Effect& e );
void FxGroupRemoved( Show& show, int idx );//call before erasing a group
//modulator helpers (editor graphics)
float FxCycleLength( const Effect& e );
double FxBasePhase( const Effect& e );
float FxEnvValue( const Effect& e, float t );
float FxEnvLength( const Effect& e );
float FxSeqValue( const Effect& e, const FxLane& l, int idx, int* step );
int FxSeqStep( const Effect& e );
float FxModPreview( Show& show, const Effect& e, int i, int n );
const char* FxWaveName( FxWave w );
const char* FxShapeName( FxShape s );
const char* FxOrderName( FxOrder o );
const char* FxColorModeName( FxColorMode m );

//---------------------------------------------------------------- presets + programs
struct FxPreset
{
	std::string group, name;
	Effect fx;
	bool user = false;
};
const std::vector< FxPreset >& BuiltinFxPresets();
std::vector< FxPreset > LoadUserFxPresets( const std::string& file );
bool SaveUserFxPreset( const std::string& file, const std::string& name, const Effect& fx );
bool DeleteUserFxPreset( const std::string& file, const std::string& name );
//new effect from a preset for the given lamps (empty = every lamp that supports it); returns its id (0 = no lamp fits)
int AddEffectFromPreset( Show& show, const Effect& tmpl, const std::string& name, const std::vector< int >& fixtures, bool start );

enum class FxFilter : uint8_t { All, Moving, Static, Pixel };
struct ProgramTemplate
{
	std::string name, info;
	std::vector< std::pair< FxFilter, std::string > > parts;//filter + preset name
};
const std::vector< ProgramTemplate >& BuiltinPrograms();
//creates the effects of a template for the lamps of the show; returns the program index (-1 = nothing fits)
int InstantiateProgram( Show& show, const ProgramTemplate& t );
void StartProgram( Show& show, int idx, bool exclusive = true );
void StopAllEffects( Show& show );
int ActiveProgram( const Show& show );//program whose effects are exactly the running ones, -1 = none

Json EffectToJson( const Effect& e, bool withFixtures = true );
void EffectFromJson( const Json& j, Effect& e );
}//namespace ls
