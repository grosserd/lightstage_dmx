#include "effects.h"
#include "sim.h"
#include <algorithm>
#include <map>

namespace ls
{
bool SnapFadeable( Attr a );
//================================================================ tempo
float TempoBpm( const Tempo& t )
{
	return t.source == TempoSource::Internal ? t.bpm : t.liveBpm;
}

void TempoAdvance( Tempo& t, double dt, double now )
{
	switch( t.source )
	{
	case TempoSource::Internal:
		t.liveBpm = t.bpm;
		t.beat += dt * t.bpm / 60.0;
		t.status.clear();
		break;
	case TempoSource::MidiClock:
	{
		const bool alive = t.lastTick > 0 && now - t.lastTick < 0.6;
		if( !alive || !t.playing )
		{
			//no clock (or stopped): run on with the last tempo, so nothing freezes on stage
			t.beat += dt * t.liveBpm / 60.0;
			t.status = t.lastTick <= 0 ? L( "wartet auf MIDI-Clock" ) : ( t.playing ? L( "Clock weg - läuft frei weiter" ) : L( "Clock gestoppt - läuft frei weiter" ) );
			break;
		}
		//between ticks: interpolate with the measured tempo, never past the next tick
		const double tickBeat = t.ticks / 24.0;
		const double ahead    = std::min( ( now - t.lastTick ) * t.liveBpm / 60.0, 1.0 / 24.0 );
		const double target   = tickBeat + std::max( 0.0, ahead );
		if( fabs( target - t.beat ) > 0.25 ) t.beat = target;//start / song position: jump
		else t.beat = std::max( t.beat, target );            //monotonic
		t.status = L( "MIDI-Clock" );
		break;
	}
	case TempoSource::Link:
		break;//App::UpdateTempo reads the Link session
	}
}

void TempoMidiClock( Tempo& t, uint8_t st, int spp, double time )
{
	switch( st )
	{
	case 0xF8:
	{
		//tempo from the last ~2 beats of ticks (smooths USB / driver jitter)
		t.taps.push_back( time );//reused as tick history while on MIDI clock
		if( t.taps.size() > 49 ) t.taps.erase( t.taps.begin(), t.taps.begin() + ( t.taps.size() - 49 ) );
		if( t.taps.size() >= 7 && time - t.taps.front() > 0.02 )
		{
			//drop history across a gap (clock was off)
			for( size_t i = t.taps.size() - 1; i > 0; --i )
				if( t.taps[ i ] - t.taps[ i - 1 ] > 0.25 ) { t.taps.erase( t.taps.begin(), t.taps.begin() + i ); break; }
			if( t.taps.size() >= 7 )
			{
				const double per = ( t.taps.back() - t.taps.front() ) / ( t.taps.size() - 1 );//seconds per tick
				if( per > 0.0005 ) t.liveBpm = (float)Clamp( 60.0 / ( per * 24.0 ), 20.0, 400.0 );
			}
		}
		t.lastTick = time;
		++t.ticks;
		break;
	}
	case 0xFA://start: the next tick is beat 0
		t.ticks    = -1;
		t.playing  = true;
		t.lastTick = time;
		t.beat     = -1.0 / 24.0;
		break;
	case 0xFB: t.playing = true; break;
	case 0xFC: t.playing = false; break;
	case 0xF2://song position in 16th notes
		t.ticks = (long long)spp * 6 - 1;
		t.beat  = t.ticks / 24.0;
		break;
	}
}

void TempoTap( Tempo& t, double now )
{
	if( t.source == TempoSource::MidiClock ) t.taps.clear();
	if( !t.taps.empty() && now - t.taps.back() > 2.0 ) t.taps.clear();
	t.taps.push_back( now );
	if( t.taps.size() > 8 ) t.taps.erase( t.taps.begin() );
	if( t.taps.size() >= 2 )
	{
		const double per = ( t.taps.back() - t.taps.front() ) / ( t.taps.size() - 1 );
		if( per > 0.15 && per < 3.0 ) t.bpm = (float)Clamp( 60.0 / per, 20.0, 300.0 );
	}
	//the tap is a beat
	t.beat = floor( t.beat + 0.5 );
}

void TempoResync( Tempo& t )
{
	const int bpb = std::max( 1, t.beatsPerBar );
	t.beat        = floor( t.beat / bpb + 0.5 ) * bpb;
}

//================================================================ names
const char* FxKindName( FxKind k )
{
	static const char* n[] = { "Helligkeit", "Farbe", "Bewegung", "Attribut", "Modulator" };
	return n[ Clamp( (int)k, 0, 4 ) ];
}
const char* FxWaveName( FxWave w )
{
	static const char* n[] = { "Sinus", "Dreieck", "Sägezahn auf", "Sägezahn ab", "Rechteck", "Puls", "Zufall", "Zufall weich", "Eigene" };
	return n[ Clamp( (int)w, 0, 8 ) ];
}
const char* FxTypeName( FxType t )
{
	static const char* n[] = { "Lauflicht", "Welle", "Strobe", "Funkeln", "Farbwechsel", "Farbverlauf", "Regenbogen", "Bewegung", "Strahl", "LFO", "Step-Sequencer", "Hüllkurve", "Intensität", "Farbe" };
	return n[ Clamp( (int)t, 0, (int)FxType::Count - 1 ) ];
}
const char* FxTypeInfo( FxType t )
{
	static const char* n[] = {
		"Licht läuft von Lampe zu Lampe",
		"weiche Helligkeitswelle über die Lampen",
		"Blitze im Takt, alle oder zufällig",
		"zufälliges Glitzern",
		"springt durch eine Farbliste",
		"überblendet durch eine Farbliste",
		"Farbkreis, gleich oder als Welle",
		"Kreis, Acht, Schwenk ... für Moving Heads",
		"Zoom, Iris, Fokus, Frost pulsieren",
		"Wellenform auf jedes Attribut oder DMX-Kanäle",
		"Schritte zeichnen, bis 8 Spuren mit eigenem Ziel",
		"Attack / Decay / Sustain / Release im Takt",
		"Lauflicht, Welle, Strobe, Funkeln ... (Presets)",
		"Farbwechsel, Verlauf, Regenbogen ... (Presets)",
	};
	return n[ Clamp( (int)t, 0, (int)FxType::Count - 1 ) ];
}
V3 FxTypeColor( FxType t )
{
	switch( t )
	{
	case FxType::Chase:
	case FxType::Wave:
	case FxType::Strobe:
	case FxType::Sparkle:
	case FxType::Intensity: return V3( 1.0f, 0.78f, 0.36f );
	case FxType::ColorChange:
	case FxType::ColorFade:
	case FxType::Rainbow:
	case FxType::Colour: return V3( 0.95f, 0.42f, 0.78f );
	case FxType::Movement:
	case FxType::Beam: return V3( 0.36f, 0.72f, 1.0f );
	case FxType::Lfo: return V3( 0.35f, 0.9f, 0.62f );
	case FxType::Sequencer: return V3( 0.62f, 0.55f, 1.0f );
	case FxType::Envelope: return V3( 1.0f, 0.55f, 0.35f );
	default: return V3( 0.8f, 0.8f, 0.8f );
	}
}
bool FxIsModulator( FxType t ) { return t == FxType::Lfo || t == FxType::Sequencer || t == FxType::Envelope; }
//type of an effect saved before the builder (no "type" in the file)
FxType FxTypeGuess( const Effect& e )
{
	switch( e.kind )
	{
	case FxKind::Dimmer: return FxType::Intensity;
	case FxKind::Color: return FxType::Colour;
	case FxKind::Position: return FxType::Movement;
	case FxKind::Attribute: return FxType::Beam;
	case FxKind::Modulator: return FxType::Lfo;
	}
	return FxType::Wave;
}
static FxLane MakeLane( int steps, int idx )
{
	FxLane l;
	l.v.assign( (size_t)std::max( 1, steps ), 0.0f );
	//a pleasant start pattern: four-on-the-floor in lane 1, offbeats in lane 2, ...
	for( int i = 0; i < steps; ++i )
	{
		if( idx == 0 ) l.v[ i ] = i % 4 == 0 ? 1.0f : ( i % 4 == 2 ? 0.35f : 0.0f );
		else if( idx == 1 ) l.v[ i ] = i % 4 == 2 ? 1.0f : 0.0f;
		else l.v[ i ] = i % 8 == 7 ? 1.0f : 0.0f;
	}
	return l;
}
void FxResizeSteps( Effect& e, int steps )
{
	e.steps = Clamp( steps, 1, 64 );
	for( auto& l : e.lanes ) l.v.resize( (size_t)e.steps, 0.0f );
	if( e.sync ) e.beats = e.steps * e.stepBeats;
}
FxLane FxNewLane( const Effect& e, int idx ) { return MakeLane( e.steps, idx ); }
Effect FxMakeType( FxType t )
{
	Effect e;
	e.type   = t;
	e.name   = FxTypeName( t );
	e.spread = 0;
	switch( t )
	{
	case FxType::Chase: e.kind = FxKind::Dimmer; e.wave = FxWave::Square; e.width = 0.25f; e.beats = 2; e.spread = 360; break;
	case FxType::Wave:
	case FxType::Intensity: e.kind = FxKind::Dimmer; e.wave = FxWave::Sine; e.beats = 4; e.spread = 360; break;
	case FxType::Strobe: e.kind = FxKind::Dimmer; e.wave = FxWave::Square; e.width = 0.15f; e.beats = 0.25f; e.fade = 0; break;
	case FxType::Sparkle: e.kind = FxKind::Dimmer; e.wave = FxWave::Random; e.beats = 0.5f; e.spread = 360; e.order = FxOrder::Random; break;
	case FxType::ColorChange: e.kind = FxKind::Color; e.colorMode = FxColorMode::Steps; e.colors = { V3( 1, 0, 0 ), V3( 0, 0.15f, 1 ), V3( 1, 0, 0.7f ), V3( 0, 1, 0.4f ) }; e.beats = 4; break;
	case FxType::ColorFade: e.kind = FxKind::Color; e.colorMode = FxColorMode::Fade; e.colors = { V3( 1, 0, 0.35f ), V3( 0.5f, 0, 1 ), V3( 0, 0.35f, 1 ), V3( 0, 0.9f, 1 ) }; e.beats = 16; e.spread = 180; break;
	case FxType::Rainbow:
	case FxType::Colour: e.kind = FxKind::Color; e.colorMode = FxColorMode::Rainbow; e.beats = 16; e.spread = 360; break;
	case FxType::Movement: e.kind = FxKind::Position; e.shape = FxShape::Circle; e.beats = 8; e.spread = 360; break;
	case FxType::Beam: e.kind = FxKind::Attribute; e.attr = Attr::Zoom; e.wave = FxWave::Sine; e.beats = 4; break;
	case FxType::Lfo: e.kind = FxKind::Modulator; e.wave = FxWave::Sine; e.beats = 4; e.tgt.attr = Attr::Dimmer; break;
	case FxType::Sequencer:
		e.kind      = FxKind::Modulator;
		e.steps     = 16;
		e.stepBeats = 0.25f;
		e.beats     = 4;
		e.fade      = 0.05f;
		e.lanes     = { MakeLane( 16, 0 ) };
		break;
	case FxType::Envelope: e.kind = FxKind::Modulator; e.beats = 1; e.atk = 0.02f; e.dec = 0.35f; e.sus = 0; e.rel = 0.1f; e.gate = 0.4f; break;
	default: break;
	}
	//new effects start gentle: maximum 50 % (the sequencer's drawn steps are the values themselves)
	if( t != FxType::Sequencer ) e.hi = 0.5f;
	//the builder shows one type per engine: the old presets types become Intensity / Colour
	e.type = FxMergedType( t );
	if( e.name.empty() ) e.name = FxTypeName( e.type );
	return e;
}
const char* FxShapeName( FxShape s )
{
	static const char* n[] = { "Kreis", "Acht", "Pan-Schwenk", "Tilt-Nicken", "Dreieck", "Quadrat", "Zufall" };
	return n[ Clamp( (int)s, 0, 6 ) ];
}
const char* FxOrderName( FxOrder o )
{
	static const char* n[] = { "Auswahl-Reihenfolge", "Links -> Rechts", "Vorne -> Hinten", "Mitte -> Außen", "Zufall" };
	return n[ Clamp( (int)o, 0, 4 ) ];
}
const char* FxColorModeName( FxColorMode m )
{
	static const char* n[] = { "Regenbogen", "Verlauf (Palette)", "Stufen (Palette)" };
	return n[ Clamp( (int)m, 0, 2 ) ];
}

//================================================================ waves
static uint32_t Hash( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352d;
	x ^= x >> 15;
	x *= 0x846ca68b;
	x ^= x >> 16;
	return x;
}
static float Hash01( uint32_t a, long long b )
{
	return ( Hash( a * 0x9E3779B1u ^ Hash( (uint32_t)( b & 0xffffffff ) + 0x632BE5ABu ) ) & 0xffffff ) / 16777215.0f;
}
float FxWaveValue( FxWave w, float t, float width, uint32_t seed, long long cyc )
{
	width = Clamp( width, 0.01f, 1.0f );
	switch( w )
	{
	case FxWave::Sine: return 0.5f - 0.5f * cosf( 2 * kPi * t );
	case FxWave::Triangle: return t < 0.5f ? t * 2 : 2 - t * 2;
	case FxWave::SawUp: return t;
	case FxWave::SawDown: return 1 - t;
	case FxWave::Square: return t < width ? 1.0f : 0.0f;
	case FxWave::Pulse: return t < width ? sinf( kPi * t / width ) : 0.0f;
	case FxWave::Random: return Hash01( seed, cyc );
	case FxWave::Smooth:
	{
		//smooth random: cosine between this cycle's and the next cycle's random value
		const float a = Hash01( seed, cyc ), b = Hash01( seed, cyc + 1 );
		return a + ( b - a ) * ( 0.5f - 0.5f * cosf( kPi * t ) );
	}
	}
	return 0;
}
//drawn curve: periodic (one cycle, wraps from the last point to the first) or open (envelope, 0 outside)
float FxCurveValue( const std::vector< FxPoint >& c, float t, bool periodic )
{
	const int n = (int)c.size();
	if( n == 0 ) return 0;
	if( n == 1 ) return Saturate( c[ 0 ].v );
	if( periodic ) t -= floorf( t );
	//segment i: from point i to i+1 (periodic: the last one to the first + 1)
	int i = -1;
	for( int k = 0; k < n; ++k )
		if( c[ k ].t <= t ) i = k;
	float t0, t1;
	int a, b;
	if( i < 0 )
	{
		if( !periodic ) return Saturate( c[ 0 ].v );
		a = n - 1, b = 0, t0 = c[ n - 1 ].t - 1, t1 = c[ 0 ].t;
	}
	else if( i == n - 1 )
	{
		if( !periodic ) return Saturate( c[ n - 1 ].v );
		a = n - 1, b = 0, t0 = c[ n - 1 ].t, t1 = c[ 0 ].t + 1;
	}
	else
		a = i, b = i + 1, t0 = c[ i ].t, t1 = c[ i + 1 ].t;
	const float u  = t1 - t0 > 1e-6f ? Saturate( ( t - t0 ) / ( t1 - t0 ) ) : 1.0f;
	const float va = c[ a ].v, vb = c[ b ].v;
	switch( c[ a ].shape )
	{
	case 0: return Saturate( va + ( vb - va ) * u );
	case 1: return Saturate( va + ( vb - va ) * Smooth( u ) );
	case 3: return Saturate( va );
	default:
	{
		//Catmull-Rom through the neighbours (open curve: ends repeat)
		const int ap = periodic ? ( a - 1 + n ) % n : std::max( 0, a - 1 );
		const int bn = periodic ? ( b + 1 ) % n : std::min( n - 1, b + 1 );
		const float p0 = c[ ap ].v, p3 = c[ bn ].v;
		const float u2 = u * u, u3 = u2 * u;
		const float v  = 0.5f * ( 2 * va + ( vb - p0 ) * u + ( 2 * p0 - 5 * va + 4 * vb - p3 ) * u2 + ( 3 * va - p0 - 3 * vb + p3 ) * u3 );
		return Saturate( v );
	}
	}
}
//wave of an effect (drawn or built in)
float FxEffWave( const Effect& e, float t, uint32_t seed, long long cyc )
{
	if( e.wave == FxWave::Custom ) return FxCurveValue( e.pts, t, true );
	return FxWaveValue( e.wave, t, e.width, seed, cyc );
}
//a drawn wave to start from: the current wave sampled at a few points
std::vector< FxPoint > FxCurveFromWave( FxWave w, float width )
{
	std::vector< FxPoint > c;
	switch( w )
	{
	case FxWave::Square: c = { { 0, 1, 3 }, { Saturate( width ), 0, 3 } }; break;
	case FxWave::SawUp: c = { { 0, 0, 0 }, { 0.999f, 1, 3 } }; break;
	case FxWave::SawDown: c = { { 0, 1, 0 }, { 0.999f, 0, 3 } }; break;
	case FxWave::Triangle: c = { { 0, 0, 0 }, { 0.5f, 1, 0 } }; break;
	default: c = { { 0, 0, 2 }, { 0.25f, 0.5f, 2 }, { 0.5f, 1, 2 }, { 0.75f, 0.5f, 2 } }; break;
	}
	return c;
}
FxType FxMergedType( FxType t )
{
	switch( t )
	{
	case FxType::Chase:
	case FxType::Wave:
	case FxType::Strobe:
	case FxType::Sparkle: return FxType::Intensity;
	case FxType::ColorChange:
	case FxType::ColorFade:
	case FxType::Rainbow: return FxType::Colour;
	default: return t;
	}
}

//smooth noise for random movement
static float Noise1( uint32_t seed, double x )
{
	long long i = (long long)floor( x );
	float f     = (float)( x - i );
	float a = Hash01( seed, i ) * 2 - 1, b = Hash01( seed, i + 1 ) * 2 - 1;
	return Lerp( a, b, Smooth( f ) );
}

//================================================================ elements
struct FxEl
{
	const FixtureInst* f = nullptr;
	int head = -1;
	V3 pos;
	int ui = -1, ch = -1;//DMX element: universe index, channel 0..511
};
static bool HasDimmerLike( const Show& show, const FixtureInst& f )
{
	return show.HasAttr( f, Attr::Dimmer ) || show.HasAttr( f, Attr::Red ) || show.HasAttr( f, Attr::White ) || show.HasAttr( f, Attr::Cyan );
}
static bool TargetFits( const Show& show, const FixtureInst& f, const FxTarget& t )
{
	if( t.mode == 2 ) return false;
	return t.attr == Attr::Dimmer ? HasDimmerLike( show, f ) : show.HasAttr( f, t.attr );
}
bool FxSupports( const Show& show, const FixtureInst& f, const Effect& e )
{
	if( !f.def ) return false;
	if( e.kind == FxKind::Modulator )
	{
		if( e.type == FxType::Sequencer )
		{
			for( auto& l : e.lanes )
				if( l.tgt.mode == 0 && TargetFits( show, f, l.tgt ) ) return true;
			return e.lanes.empty();
		}
		return TargetFits( show, f, e.tgt );
	}
	switch( e.kind )
	{
	case FxKind::Dimmer: return show.HasAttr( f, Attr::Dimmer ) || show.HasAttr( f, Attr::Red ) || show.HasAttr( f, Attr::White ) || show.HasAttr( f, Attr::Cyan );
	case FxKind::Color: return show.HasColor( f );
	case FxKind::Position: return show.HasAttr( f, Attr::Pan ) || show.HasAttr( f, Attr::Tilt );
	case FxKind::Attribute: return show.HasAttr( f, e.attr );
	default: break;
	}
	return false;
}
//lamps of an effect: its group (Groups tab) or its own list
const std::vector< int >& FxFixtureIds( const Show& show, const Effect& e )
{
	if( e.group >= 0 && e.group < (int)show.groups.size() ) return show.groups[ e.group ].fixtures;
	return e.fixtures;
}
static void SortElements( const Effect& e, std::vector< FxEl >& els );
//elements of a target: lamps (pixels) with the attribute, or DMX channels
static void TargetElements( Show& show, const Effect& e, const FxTarget& t, std::vector< FxEl >& els )
{
	els.clear();
	if( t.mode == 2 )
	{
		const int ui = show.UniIndex( t.uni );
		if( ui < 0 ) return;
		for( int c = 0; c < std::max( 1, t.count ); ++c )
		{
			const int ch = t.ch - 1 + c;
			if( ch < 0 || ch >= 512 ) break;
			FxEl el;
			el.ui  = ui;
			el.ch  = ch;
			el.pos = V3( c - ( std::max( 1, t.count ) - 1 ) * 0.5f, 0, 0 );
			els.push_back( el );
		}
		SortElements( e, els );
		return;
	}
	const std::vector< int >& ids = t.mode == 1 && t.group >= 0 && t.group < (int)show.groups.size() ? show.groups[ t.group ].fixtures : FxFixtureIds( show, e );
	for( int id : ids )
	{
		const FixtureInst* f = show.FindFixture( id );
		if( !f || !TargetFits( show, *f, t ) ) continue;
		const int heads = f->def->Heads();
		if( e.pixels && heads > 1 )
		{
			const M4 base = FixtureBase( *f );
			for( int h = 0; h < heads; ++h )
			{
				const Pixel& px = f->def->pixels[ h ];
				FxEl el;
				el.f    = f;
				el.head = h;
				el.pos  = XformP( base, V3( px.pos.x * f->def->dims.x, px.pos.y * f->def->dims.y, px.pos.z * f->def->dims.z ) );
				els.push_back( el );
			}
		}
		else
		{
			FxEl el;
			el.f   = f;
			el.pos = f->pos;
			els.push_back( el );
		}
	}
	SortElements( e, els );
}
static void BuildElements( Show& show, const Effect& e, std::vector< FxEl >& els )
{
	els.clear();
	if( e.kind == FxKind::Modulator )
	{
		TargetElements( show, e, e.type == FxType::Sequencer && !e.lanes.empty() ? e.lanes[ 0 ].tgt : e.tgt, els );
		return;
	}
	for( int id : FxFixtureIds( show, e ) )
	{
		const FixtureInst* f = show.FindFixture( id );
		if( !f || !FxSupports( show, *f, e ) ) continue;
		const int heads = f->def->Heads();
		if( e.pixels && heads > 1 && e.kind != FxKind::Position )
		{
			const M4 base = FixtureBase( *f );
			for( int h = 0; h < heads; ++h )
			{
				const Pixel& px = f->def->pixels[ h ];
				els.push_back( { f, h, XformP( base, V3( px.pos.x * f->def->dims.x, px.pos.y * f->def->dims.y, px.pos.z * f->def->dims.z ) ) } );
			}
		}
		else
			els.push_back( { f, -1, f->pos } );
	}
	SortElements( e, els );
}
static void SortElements( const Effect& e, std::vector< FxEl >& els )
{
	switch( e.order )
	{
	case FxOrder::Selection: break;
	case FxOrder::LeftRight: std::stable_sort( els.begin(), els.end(), []( const FxEl& a, const FxEl& b ) { return a.pos.x < b.pos.x - 1e-3f; } ); break;
	case FxOrder::FrontBack: std::stable_sort( els.begin(), els.end(), []( const FxEl& a, const FxEl& b ) { return a.pos.z > b.pos.z + 1e-3f; } ); break;
	case FxOrder::CenterOut: std::stable_sort( els.begin(), els.end(), []( const FxEl& a, const FxEl& b ) { return fabsf( a.pos.x ) < fabsf( b.pos.x ) - 1e-3f; } ); break;
	case FxOrder::Random:
	{
		uint32_t s = Hash( (uint32_t)e.id * 7919u + 17 );
		for( size_t i = els.size(); i > 1; --i )
		{
			s = Hash( s + (uint32_t)i );
			std::swap( els[ i - 1 ], els[ s % i ] );
		}
		break;
	}
	}
}
//phase of element k of n (0..1) and its cycle number
static void ElementPhase( const Show& show, const Effect& e, int k, int n, float& t, long long& cyc )
{
	int idx = k, cnt = n;
	if( e.mirror && n > 1 )
	{
		idx = std::min( k, n - 1 - k );
		cnt = ( n + 1 ) / 2;
	}
	if( e.groups > 0 )
	{
		idx = idx % e.groups;
		cnt = e.groups;
	}
	double off = cnt > 0 ? (double)idx / cnt * ( e.spread / 360.0 ) : 0.0;
	if( e.reverse ) off = -off;
	const double base = e.sync ? e.syncBeat / std::max( 1.0 / 64.0, (double)e.beats ) : e.freePhase;
	const double ph   = base - off;
	cyc               = (long long)floor( ph );
	t                 = (float)( ph - (double)cyc );
}
static V3 PaletteColor( const Effect& e, float t )
{
	if( e.colorMode == FxColorMode::Rainbow || e.colors.empty() ) return Hsv( t, 1, 1 );
	const int n = (int)e.colors.size();
	if( e.colorMode == FxColorMode::Steps ) return e.colors[ Clamp( (int)( t * n ), 0, n - 1 ) ];
	const float x = t * n;
	const int i   = Clamp( (int)x, 0, n - 1 );
	const float f = x - i;
	const V3& a = e.colors[ i ];
	const V3& b = e.colors[ ( i + 1 ) % n ];
	return a + ( b - a ) * Smooth( f );
}
static void ShapeOffset( const Effect& e, float t, uint32_t seed, double ph, float& ox, float& oy )
{
	const float a = 2 * kPi * t;
	switch( e.shape )
	{
	case FxShape::Circle: ox = sinf( a ); oy = cosf( a ); break;
	case FxShape::Eight: ox = sinf( a ); oy = sinf( 2 * a ); break;
	case FxShape::PanSwing: ox = sinf( a ); oy = 0; break;
	case FxShape::TiltSwing: ox = 0; oy = sinf( a ); break;
	case FxShape::Triangle:
	case FxShape::Square:
	{
		static const float tri[ 3 ][ 2 ] = { { 0, 1 }, { 0.866f, -0.5f }, { -0.866f, -0.5f } };
		static const float sq[ 4 ][ 2 ]  = { { -1, 1 }, { 1, 1 }, { 1, -1 }, { -1, -1 } };
		const int n         = e.shape == FxShape::Triangle ? 3 : 4;
		const float( *p )[ 2 ] = e.shape == FxShape::Triangle ? tri : sq;
		const float x       = t * n;
		const int i         = Clamp( (int)x, 0, n - 1 );
		const float f       = x - i;
		ox = Lerp( p[ i ][ 0 ], p[ ( i + 1 ) % n ][ 0 ], f );
		oy = Lerp( p[ i ][ 1 ], p[ ( i + 1 ) % n ][ 1 ], f );
		break;
	}
	case FxShape::Random:
		ox = Noise1( seed, ph * 2.0 );
		oy = Noise1( seed + 101, ph * 2.0 + 0.37 );
		break;
	}
}
static uint16_t ShutterOpen16( const Channel& c )
{
	for( auto& cp : c.caps )
		if( cp.k == CapK::ShutterOpen ) return (uint16_t)( ( cp.lo + cp.hi ) / 2 );
	return c.def16;
}

static const Attr kColorAttrs[] = { Attr::Red, Attr::Green, Attr::Blue, Attr::White, Attr::WarmWhite, Attr::ColdWhite, Attr::Amber, Attr::UV, Attr::Lime, Attr::Indigo, Attr::Cyan, Attr::Magenta, Attr::Yellow };

//---------------------------------------------------------------- modulators
//index of element k of n after mirror / groups, and how many distinct positions there are
static void ElementIndex( const Effect& e, int k, int n, int& idx, int& cnt )
{
	idx = k;
	cnt = n;
	if( e.mirror && n > 1 )
	{
		idx = std::min( k, n - 1 - k );
		cnt = ( n + 1 ) / 2;
	}
	if( e.groups > 0 )
	{
		idx = idx % e.groups;
		cnt = e.groups;
	}
	if( e.reverse ) idx = cnt - 1 - idx;
}
float FxCycleLength( const Effect& e )//beats (synced) or seconds
{
	if( e.type == FxType::Sequencer ) return e.sync ? std::max( 1.0f / 64.0f, e.steps * e.stepBeats ) : 1.0f / std::max( 0.001f, e.hz );
	return e.sync ? std::max( 1.0f / 64.0f, e.beats ) : 1.0f / std::max( 0.001f, e.hz );
}
double FxBasePhase( const Effect& e )//cycles since start (fraction = position in the cycle)
{
	if( e.type == FxType::Sequencer && e.sync ) return e.syncBeat / std::max( 1.0 / 64.0, (double)e.steps * e.stepBeats );
	return e.sync ? e.syncBeat / std::max( 1.0 / 64.0, (double)e.beats ) : e.freePhase;
}
static float Shape( float x, float curve )//0..1 -> 0..1, fast start with curve
{
	x             = Saturate( x );
	const float p = 1.0f + Saturate( curve ) * 4.0f;
	return 1.0f - powf( 1.0f - x, p );
}
float FxEnvValue( const Effect& e, float tt )
{
	if( tt < 0 ) return 0;
	if( e.envDrawn )
	{
		const float len = std::max( 0.01f, e.gate );
		return tt > len ? 0.0f : FxCurveValue( e.pts, tt / len, false );
	}
	const float a = std::max( 0.0f, e.atk ), h = std::max( 0.0f, e.hold ), d = std::max( 0.0f, e.dec ), r = std::max( 0.0f, e.rel );
	const float sus = Saturate( e.sus );
	float level     = 0;
	float t         = tt;
	if( t < a ) return Shape( t / std::max( 1e-4f, a ), e.curve );
	t -= a;
	if( t < h ) return 1;
	t -= h;
	if( t < d ) return 1 - ( 1 - sus ) * Shape( t / std::max( 1e-4f, d ), e.curve );
	t -= d;
	const float susLen = std::max( 0.0f, e.gate - ( a + h + d ) );
	if( t < susLen ) return sus;
	t -= susLen;
	level = sus;
	if( susLen <= 0 && d <= 0 ) level = 1;
	if( t < r ) return level * ( 1 - Shape( t / std::max( 1e-4f, r ), e.curve ) );
	return 0;
}
float FxEnvLength( const Effect& e )
{
	if( e.envDrawn ) return std::max( 0.01f, e.gate );
	return std::max( e.gate, std::max( 0.0f, e.atk ) + std::max( 0.0f, e.hold ) + std::max( 0.0f, e.dec ) ) + std::max( 0.0f, e.rel );
}
//time since the trigger for element idx of cnt (beats or seconds)
static float EnvTime( const Effect& e, int idx, int cnt )
{
	const float len   = FxCycleLength( e );
	const float delay = cnt > 0 ? (float)idx / cnt * ( e.spread / 360.0f ) * len : 0.0f;
	if( e.manualTrig ) return (float)( e.sync ? e.syncBeat - e.trigBeat : e.clock - e.trigTime ) - delay;
	const double pos = FxBasePhase( e ) * len - delay;
	double m         = fmod( pos, (double)len );
	if( m < 0 ) m += len;
	return (float)m;
}
//sequencer: value of a lane for element idx (each next element stepOffset steps later)
float FxSeqValue( const Effect& e, const FxLane& l, int idx, int* stepOut )
{
	const int n = std::max( 1, e.steps );
	double ph   = FxBasePhase( e ) - (double)idx * e.stepOffset / n;
	ph -= floor( ph );
	const double x = ph * n;
	const int s    = Clamp( (int)x, 0, n - 1 );
	if( stepOut ) *stepOut = s;
	if( l.v.empty() ) return 0;
	const float a = l.v[ (size_t)s % l.v.size() ];
	if( !l.glide ) return a;
	const float b = l.v[ (size_t)( s + 1 ) % l.v.size() ];
	return a + ( b - a ) * Smooth( (float)( x - s ) );
}
int FxSeqStep( const Effect& e )
{
	int s = 0;
	static const FxLane none;
	FxSeqValue( e, e.lanes.empty() ? none : e.lanes[ 0 ], 0, &s );
	return s;
}
//writes a modulator value (0..1 of the target) with mix k (caller holds dmxLock)
static void ModWrite( Show& show, const Effect& e, const FxEl& el, Attr attr, float w, float k )
{
	static thread_local std::vector< int > slots;
	if( el.ch >= 0 )
	{
		auto& O         = show.fxOut[ el.ui ];
		const int baseV = show.snapAny && el.ui < (int)show.snapOut.size() && show.snapOut[ el.ui ][ el.ch ] >= 0 ? show.snapOut[ el.ui ][ el.ch ] : show.dmx[ el.ui ][ el.ch ];
		const float under = baseV / 255.0f;
		const float out   = e.relative ? Saturate( under + ( w - 0.5f ) * e.depth ) : Lerp( e.lo, e.hi, w );
		O[ el.ch ]        = (int16_t)Clamp( (int)lroundf( Lerp( under, out, k ) * 255.0f ), 0, 255 );
		return;
	}
	const FixtureInst& f = *el.f;
	if( attr == Attr::Dimmer )
	{
		show.AttrSlots( f, Attr::Dimmer, el.head, slots );
		if( slots.empty() )
		{
			//no dimmer: scale the colour emitters
			const float out = e.relative ? Saturate( 1.0f + ( w - 0.5f ) * e.depth ) : Lerp( e.lo, e.hi, w );
			for( Attr a : kColorAttrs )
				if( show.HasAttr( f, a ) ) show.FxSetAttr( f, a, show.GetAttrLayer( f, a, el.head ) * Lerp( 1.0f, out, k ), el.head );
		}
		else
		{
			const float under = show.GetAttrLayer( f, Attr::Dimmer, el.head );
			const float out   = e.relative ? Saturate( under + ( w - 0.5f ) * e.depth ) : Lerp( e.lo, e.hi, w );
			show.FxSetAttr( f, Attr::Dimmer, e.scale ? under * Lerp( 1.0f, out, k ) : Lerp( under, out, k ), el.head );
		}
		if( e.openShutter && el.head < 0 )
		{
			const Mode* m = f.M();
			for( int s = 0; m && s < (int)m->ch.size(); ++s )
				if( !m->ch[ s ].isFine && m->ch[ s ].attr == Attr::Shutter ) show.FxSet16( f, s, ShutterOpen16( m->ch[ s ] ) );
		}
		return;
	}
	const bool posAttr = attr == Attr::Pan || attr == Attr::Tilt;
	const int head     = posAttr ? -1 : el.head;
	const float under  = show.GetAttrLayer( f, attr, head );
	const float out    = e.relative ? Saturate( under + ( w - 0.5f ) * e.depth ) : Lerp( e.lo, e.hi, w );
	show.FxSetAttr( f, attr, Lerp( under, out, k ), head );
}
static float ModDrive( Show& show, const Effect& e, const FxEl& el )
{
	float iScl        = 1;
	const float drive = el.f ? e.DriveOf( el.f->id, &iScl ) : e.Drive();
	return Saturate( drive * e.level * show.fxMaster );
}
static void ApplyModulator( Show& show, Effect& e )
{
	static thread_local std::vector< FxEl > els;
	auto run = [ & ]( const FxTarget& t, auto value ) {
		TargetElements( show, e, t, els );
		const int n = (int)els.size();
		for( int i = 0; i < n; ++i )
		{
			const float k = ModDrive( show, e, els[ i ] );
			if( k <= 0 ) continue;
			int idx, cnt;
			ElementIndex( e, i, n, idx, cnt );
			ModWrite( show, e, els[ i ], t.attr, Saturate( value( i, n, idx, cnt ) ), k );
		}
	};
	if( e.type == FxType::Sequencer )
	{
		for( auto& l : e.lanes )
			if( !l.mute ) run( l.tgt, [ & ]( int, int, int idx, int ) { return FxSeqValue( e, l, idx, nullptr ); } );
		return;
	}
	if( e.type == FxType::Envelope )
	{
		if( e.manualTrig )
		{
			//triggered by hand = a flash on top: the envelope is the mix towards the maximum (relative: + depth);
			//before the trigger and after the release the lamps keep what they had
			TargetElements( show, e, e.tgt, els );
			const int n     = (int)els.size();
			const float len = FxEnvLength( e );
			for( int i = 0; i < n; ++i )
			{
				const float k = ModDrive( show, e, els[ i ] );
				if( k <= 0 ) continue;
				int idx, cnt;
				ElementIndex( e, i, n, idx, cnt );
				const float tt = EnvTime( e, idx, cnt );
				if( tt < 0 || tt > len ) continue;
				const float v = Saturate( FxEnvValue( e, tt ) );
				if( v <= 1e-4f ) continue;
				ModWrite( show, e, els[ i ], e.tgt.attr, 1.0f, k * v );
			}
			return;
		}
		run( e.tgt, [ & ]( int, int, int idx, int cnt ) { return FxEnvValue( e, EnvTime( e, idx, cnt ) ); } );
		return;
	}
	//LFO: wave with phase spread over the elements
	run( e.tgt, [ & ]( int i, int n, int, int ) {
		float t;
		long long cyc;
		ElementPhase( show, e, i, n, t, cyc );
		const uint32_t seed = (uint32_t)( e.id * 131 + i * 31 + 7 );
		return FxEffWave( e, t, seed, cyc );
	} );
}
//preview value of element i of n (modulators; sequencer: lane 0)
float FxModPreview( Show& show, const Effect& e, int i, int n )
{
	int idx, cnt;
	ElementIndex( e, i, n, idx, cnt );
	if( e.type == FxType::Sequencer ) return e.lanes.empty() ? 0.0f : FxSeqValue( e, e.lanes[ 0 ], idx, nullptr );
	if( e.type == FxType::Envelope ) return FxEnvValue( e, EnvTime( e, idx, cnt ) );
	float t;
	long long cyc;
	ElementPhase( show, e, i, n, t, cyc );
	return FxEffWave( e, t, (uint32_t)( e.id * 131 + i * 31 + 7 ), cyc );
}

//one effect into the overlay (caller holds dmxLock)
static void ApplyEffect( Show& show, Effect& e )
{
	if( e.kind == FxKind::Modulator )
	{
		ApplyModulator( show, e );
		return;
	}
	static thread_local std::vector< FxEl > els;
	static thread_local std::vector< int > slots;
	BuildElements( show, e, els );
	const int n   = (int)els.size();
	for( int i = 0; i < n; ++i )
	{
		const FxEl& el       = els[ i ];
		const FixtureInst& f = *el.f;
		//per lamp: own switch, or what the snapshots on this lamp say (dimmer values scaled by their strength)
		float iScl        = 1;
		const float drive = e.DriveOf( f.id, &iScl );
		iScl              = Saturate( iScl );
		const float k     = Saturate( drive * e.level * show.fxMaster );
		if( k <= 0 ) continue;
		float t;
		long long cyc;
		ElementPhase( show, e, i, n, t, cyc );
		const uint32_t seed = (uint32_t)( e.id * 131 + f.id * 31 + el.head + 7 );
		switch( e.kind )
		{
		case FxKind::Dimmer:
		{
			const float val = Lerp( e.lo, e.hi, FxEffWave( e, t, seed, cyc ) ) * iScl;
			show.AttrSlots( f, Attr::Dimmer, el.head, slots );
			if( !slots.empty() )
			{
				const float under = show.GetAttrLayer( f, Attr::Dimmer, el.head );
				show.FxSetAttr( f, Attr::Dimmer, e.scale ? under * Lerp( 1.0f, val, k ) : Lerp( under, val, k ), el.head );
			}
			else
				for( Attr a : kColorAttrs )
					if( show.HasAttr( f, a ) ) show.FxSetAttr( f, a, show.GetAttrLayer( f, a, el.head ) * Lerp( 1.0f, val, k ), el.head );
			if( e.openShutter && k > 0 && el.head < 0 )
			{
				const Mode* m = f.M();
				for( int s = 0; m && s < (int)m->ch.size(); ++s )
					if( !m->ch[ s ].isFine && m->ch[ s ].attr == Attr::Shutter ) show.FxSet16( f, s, ShutterOpen16( m->ch[ s ] ) );
			}
			break;
		}
		case FxKind::Color:
		{
			const V3 c = PaletteColor( e, t );
			static thread_local std::vector< std::pair< Attr, float > > tg;
			show.ColorTargets( f, c, tg );
			for( auto& p : tg )
			{
				if( !SnapFadeable( p.first ) || p.first == Attr::CTO )//wheel, macro, CTO: switch half way
				{
					if( k > 0.5f ) show.FxSetAttr( f, p.first, p.second, el.head );
				}
				else
					show.FxSetAttr( f, p.first, Lerp( show.GetAttrLayer( f, p.first, el.head ), p.second, k ), el.head );
			}
			break;
		}
		case FxKind::Position:
		{
			float ox = 0, oy = 0;
			ShapeOffset( e, t, seed, (double)cyc + t, ox, oy );
			const float pr = f.def->panRange > 1 ? f.def->panRange : 540.0f, tr = f.def->tiltRange > 1 ? f.def->tiltRange : 270.0f;
			if( show.HasAttr( f, Attr::Pan ) ) show.FxSetAttr( f, Attr::Pan, show.GetAttrLayer( f, Attr::Pan ) + ox * e.sizePan * 0.5f / pr * k * ( f.invertPan ? -1.0f : 1.0f ) );
			if( show.HasAttr( f, Attr::Tilt ) ) show.FxSetAttr( f, Attr::Tilt, show.GetAttrLayer( f, Attr::Tilt ) + oy * e.sizeTilt * 0.5f / tr * k * ( f.invertTilt ? -1.0f : 1.0f ) );
			break;
		}
		case FxKind::Attribute:
		{
			const float val = Lerp( e.lo, e.hi, FxEffWave( e, t, seed, cyc ) );
			show.FxSetAttr( f, e.attr, Lerp( show.GetAttrLayer( f, e.attr, el.head ), val, k ), el.head );
			break;
		}
		}
	}
}

//group faders in submaster mode: scale the finished output (overlay, else base) of their lamps' dimmers;
//lamps without a dimmer channel: their colour emitters. Caller holds dmxLock.
static void ApplySubmasters( Show& show )
{
	static thread_local std::vector< int > slots;
	static thread_local std::vector< float > scale;//per fixture id: product of all submasters
	int maxId = 0;
	for( auto& f : show.fixtures ) maxId = std::max( maxId, f.id );
	scale.assign( (size_t)maxId + 1, 1.0f );
	for( auto& g : show.groups )
		if( g.Submaster() )
		{
			std::vector< int > seen;
			for( int id : g.fixtures )
				if( id >= 0 && id <= maxId && std::find( seen.begin(), seen.end(), id ) == seen.end() )
				{
					seen.push_back( id );
					scale[ id ] *= Saturate( g.value );
				}
		}
	for( auto& f : show.fixtures )
	{
		const float k = scale[ f.id ];
		if( k >= 0.9999f ) continue;
		const Mode* m = f.M();
		const int ui  = show.UniIndex( f.universe );
		if( !m || ui < 0 || ui >= (int)show.fxOut.size() ) continue;
		auto& O       = show.fxOut[ ui ];
		const auto& D = show.dmx[ ui ];
		auto rd       = [ & ]( int s ) -> int {
			const int a = f.address - 1 + s;
			if( a < 0 || a >= 512 ) return 0;
			return O[ a ] >= 0 ? O[ a ] : D[ a ];
		};
		auto scaleAttr = [ & ]( Attr a ) {
			show.AttrSlots( f, a, -1, slots );
			for( int s : slots )
			{
				const Channel& c = m->ch[ s ];
				const bool fine  = c.fine >= 0 && c.fine < (int)m->ch.size();
				const int v16    = fine ? ( rd( s ) << 8 ) | rd( c.fine ) : rd( s ) * 257;
				show.FxSet16( f, s, (uint16_t)Clamp( (int)lroundf( v16 * k ), 0, 65535 ) );
			}
		};
		show.AttrSlots( f, Attr::Dimmer, -1, slots );
		if( !slots.empty() ) scaleAttr( Attr::Dimmer );
		else
			for( Attr a : { Attr::Red, Attr::Green, Attr::Blue, Attr::White, Attr::WarmWhite, Attr::ColdWhite, Attr::Amber, Attr::UV, Attr::Lime, Attr::Indigo } )
				scaleAttr( a );
	}
}

bool RunEffects( Show& show, double dt )
{
	TimelineAdvance( show, dt );//2.0 timeline transport / sync (its cue track drives the cue list)
	show.RunCues( dt );     //cue list: GO / follow / timecode start snapshots
	show.RunSnapshots( dt );//snapshot layer + which effects the snapshots hold
	TimelineApply( show );  //timeline lanes on top of the snapshot layer
	bool any = false;
	const double beat = show.tempo.beat;
	for( auto& e : show.effects )
	{
		const float target = e.on ? 1.0f : 0.0f;
		if( !e.clockInit )
		{
			e.clockInit = true;
			e.syncBeat  = beat;
			e.lastTempo = beat;
		}
		if( !show.fxHold )//hold: values and phases stand still
		{
			const float step = e.fade > 0.001f ? (float)( dt / e.fade ) : 1.0f;
			e.cur            = e.cur < target ? std::min( target, e.cur + step ) : std::max( target, e.cur - step );
			e.freePhase += dt * e.hz * show.fxSpeed;
			e.clock += dt * show.fxSpeed;
			if( e.freePhase > 1e6 ) e.freePhase -= floor( e.freePhase );
			//own beat clock: the tempo's beats, scaled by fxSpeed; a jump of the clock (transport, Link) is followed at once
			const double d = beat - e.lastTempo;
			if( d >= 0 && d < 4.0 ) e.syncBeat += d * show.fxSpeed;
			else e.syncBeat = beat;
		}
		e.lastTempo = beat;
		if( e.id == show.fxForceId )//Lightshow export: effect layer "on" - full drive on all its lamps, no fade
		{
			e.cur   = 1;
			e.snapW = -1;
		}
		if( e.id != show.fxMuteId && e.Drive() > 0 && e.level > 0 ) any = true;
	}
	bool sub = false;
	for( auto& g : show.groups )
		if( g.Submaster() && g.value < 0.9999f && !g.fixtures.empty() ) sub = true;
	std::lock_guard< std::mutex > l( show.dmxLock );
	if( show.fxOut.size() != show.universes.size() ) show.fxOut.resize( show.universes.size() );
	if( !any && !show.snapAny && !sub )
	{
		if( !show.fxAny ) return false;
		for( auto& r : show.fxOut ) r.fill( -1 );
		show.fxAny = false;
		++show.dmxVersion;
		return true;
	}
	//output overlay = snapshot layer, effects on top (they work on base + snapshots)
	for( size_t u = 0; u < show.fxOut.size(); ++u )
	{
		if( show.snapAny && u < show.snapOut.size() ) show.fxOut[ u ] = show.snapOut[ u ];
		else show.fxOut[ u ].fill( -1 );
	}
	for( auto& e : show.effects )
		if( e.id != show.fxMuteId && e.Drive() > 0 && e.level > 0 ) ApplyEffect( show, e );
	if( sub ) ApplySubmasters( show );
	show.fxAny = true;
	++show.dmxVersion;
	return true;
}

void PreviewEffect( Show& show, const Effect& e, std::vector< FxPreviewEl >& out )
{
	out.clear();
	std::vector< FxEl > els;
	BuildElements( show, e, els );
	const int n = (int)els.size();
	for( int i = 0; i < n; ++i )
	{
		float t;
		long long cyc;
		ElementPhase( show, e, i, n, t, cyc );
		const int fid       = els[ i ].f ? els[ i ].f->id : els[ i ].ch + 1;
		const uint32_t seed = (uint32_t)( e.id * 131 + fid * 31 + els[ i ].head + 7 );
		FxPreviewEl p;
		p.fixture = fid;
		p.head    = els[ i ].head;
		p.v       = e.kind == FxKind::Modulator ? FxModPreview( show, e, i, n ) : Lerp( e.lo, e.hi, FxEffWave( e, t, seed, cyc ) );
		p.col     = PaletteColor( e, t );
		ShapeOffset( e, t, seed, (double)cyc + t, p.ox, p.oy );
		out.push_back( p );
	}
}

//================================================================ presets
static Effect P( FxKind k )
{
	Effect e;
	e.kind = k;
	return e;
}
const std::vector< FxPreset >& BuiltinFxPresets()
{
	static std::vector< FxPreset > v;
	if( !v.empty() ) return v;
	auto add = [ & ]( const char* g, const char* n, const Effect& e ) {
		FxPreset p;
		p.group  = g;
		p.name   = n;
		p.fx     = e;
		p.fx.name = n;
		v.push_back( p );
	};
	Effect e;
	//brightness
	e = P( FxKind::Dimmer ); e.wave = FxWave::Square; e.width = 0.25f; e.beats = 2; e.spread = 360; add( "Helligkeit", "Lauflicht", e );
	e = P( FxKind::Dimmer ); e.wave = FxWave::Pulse; e.width = 0.35f; e.beats = 2; e.spread = 360; add( "Helligkeit", "Lauflicht weich", e );
	e = P( FxKind::Dimmer ); e.wave = FxWave::Pulse; e.width = 0.3f; e.beats = 2; e.spread = 360; e.mirror = true; add( "Helligkeit", "Von außen nach innen", e );
	e = P( FxKind::Dimmer ); e.wave = FxWave::Sine; e.beats = 4; e.spread = 360; add( "Helligkeit", "Welle", e );
	e = P( FxKind::Dimmer ); e.wave = FxWave::Sine; e.beats = 8; e.spread = 0; e.lo = 0.15f; add( "Helligkeit", "Atmen", e );
	e = P( FxKind::Dimmer ); e.wave = FxWave::SawDown; e.beats = 1; e.spread = 0; add( "Helligkeit", "Puls auf Beat", e );
	e = P( FxKind::Dimmer ); e.wave = FxWave::Square; e.width = 0.5f; e.beats = 2; e.spread = 180; e.groups = 2; add( "Helligkeit", "Abwechselnd", e );
	e = P( FxKind::Dimmer ); e.wave = FxWave::Random; e.beats = 0.5f; e.spread = 360; e.order = FxOrder::Random; add( "Helligkeit", "Funkeln", e );
	e = P( FxKind::Dimmer ); e.wave = FxWave::Square; e.width = 0.12f; e.beats = 1; e.spread = 360; add( "Helligkeit", "Blitz-Lauf", e );
	e = P( FxKind::Dimmer ); e.wave = FxWave::Square; e.width = 0.15f; e.beats = 0.25f; e.spread = 0; e.fade = 0; add( "Helligkeit", "Strobe", e );
	e = P( FxKind::Dimmer ); e.wave = FxWave::Square; e.width = 0.1f; e.beats = 0.25f; e.spread = 360; e.order = FxOrder::Random; e.fade = 0; add( "Helligkeit", "Strobe zufällig", e );
	//colour
	e = P( FxKind::Color ); e.colorMode = FxColorMode::Rainbow; e.beats = 16; e.spread = 0; add( "Farbe", "Regenbogen", e );
	e = P( FxKind::Color ); e.colorMode = FxColorMode::Rainbow; e.beats = 8; e.spread = 360; add( "Farbe", "Regenbogen-Welle", e );
	e = P( FxKind::Color ); e.colorMode = FxColorMode::Fade; e.colors = { V3( 1, 0, 0.35f ), V3( 0.5f, 0, 1 ), V3( 0, 0.35f, 1 ), V3( 0, 0.9f, 1 ) }; e.beats = 16; e.spread = 180; add( "Farbe", "Farbverlauf kühl", e );
	e = P( FxKind::Color ); e.colorMode = FxColorMode::Fade; e.colors = { V3( 1, 0.12f, 0 ), V3( 1, 0.45f, 0 ), V3( 1, 0.05f, 0.02f ), V3( 1, 0.7f, 0.1f ) }; e.beats = 4; e.spread = 360; e.order = FxOrder::Random; add( "Farbe", "Feuer", e );
	e = P( FxKind::Color ); e.colorMode = FxColorMode::Steps; e.colors = { V3( 1, 0, 0 ), V3( 0, 0.1f, 1 ) }; e.beats = 2; e.spread = 180; e.groups = 2; add( "Farbe", "Rot / Blau Wechsel", e );
	e = P( FxKind::Color ); e.colorMode = FxColorMode::Steps; e.colors = { V3( 1, 0, 0 ), V3( 1, 0.5f, 0 ), V3( 1, 1, 0 ), V3( 0, 1, 0 ), V3( 0, 0.3f, 1 ), V3( 0.6f, 0, 1 ) }; e.beats = 4; e.spread = 0; add( "Farbe", "Farbsprünge", e );
	e = P( FxKind::Color ); e.colorMode = FxColorMode::Steps; e.colors = { V3( 1, 0, 0 ), V3( 0, 0.15f, 1 ), V3( 1, 0, 0.7f ), V3( 0, 1, 0.4f ) }; e.beats = 4; e.spread = 0; add( "Farbe", "Farbwechsel", e );
	e = P( FxKind::Color ); e.colorMode = FxColorMode::Fade; e.colors = { V3( 1, 0.55f, 0.15f ), V3( 1, 0.25f, 0.05f ), V3( 1, 0.8f, 0.45f ) }; e.beats = 16; e.spread = 120; add( "Farbe", "Warmton", e );
	//movement
	e = P( FxKind::Position ); e.shape = FxShape::Circle; e.beats = 8; e.spread = 0; add( "Bewegung", "Kreis", e );
	e = P( FxKind::Position ); e.shape = FxShape::Circle; e.beats = 8; e.spread = 360; add( "Bewegung", "Kreis versetzt", e );
	e = P( FxKind::Position ); e.shape = FxShape::Eight; e.beats = 8; e.spread = 180; e.sizePan = 80; add( "Bewegung", "Acht", e );
	e = P( FxKind::Position ); e.shape = FxShape::PanSwing; e.beats = 8; e.spread = 0; e.sizePan = 90; add( "Bewegung", "Pan-Schwenk", e );
	e = P( FxKind::Position ); e.shape = FxShape::PanSwing; e.beats = 8; e.spread = 180; e.mirror = true; e.sizePan = 70; add( "Bewegung", "Fächer", e );
	e = P( FxKind::Position ); e.shape = FxShape::TiltSwing; e.beats = 4; e.spread = 0; e.sizeTilt = 40; add( "Bewegung", "Tilt-Nicken", e );
	e = P( FxKind::Position ); e.shape = FxShape::Random; e.beats = 4; e.spread = 360; e.sizePan = 120; e.sizeTilt = 60; add( "Bewegung", "Ballyhoo", e );
	//beam
	e = P( FxKind::Attribute ); e.attr = Attr::Zoom; e.wave = FxWave::Sine; e.beats = 4; e.spread = 0; add( "Strahl", "Zoom-Puls", e );
	e = P( FxKind::Attribute ); e.attr = Attr::Zoom; e.wave = FxWave::Sine; e.beats = 4; e.spread = 360; add( "Strahl", "Zoom-Welle", e );
	e = P( FxKind::Attribute ); e.attr = Attr::Iris; e.wave = FxWave::Triangle; e.beats = 2; e.spread = 0; add( "Strahl", "Iris-Puls", e );
	return v;
}

std::vector< FxPreset > LoadUserFxPresets( const std::string& file )
{
	std::vector< FxPreset > r;
	std::string t;
	Json j;
	if( !ReadText( file, t ) || !ParseJson( t, j ) ) return r;
	for( auto& o : j[ "presets" ].a )
	{
		FxPreset p;
		p.group = "Eigene";//display: Ls()
		p.name  = o[ "name" ].Str( "Preset" );
		p.user  = true;
		EffectFromJson( o[ "effect" ], p.fx );
		p.fx.name = p.name;
		r.push_back( p );
	}
	return r;
}
static bool WritePresets( const std::string& file, const std::vector< FxPreset >& ps )
{
	Json j  = Json::Obj();
	Json arr = Json::Arr();
	for( auto& p : ps )
	{
		Json o = Json::Obj();
		o.Set( "name", p.name );
		o.Set( "effect", EffectToJson( p.fx, false ) );
		arr.Push( o );
	}
	j.Set( "presets", arr );
	return WriteText( file, WriteJson( j ) );
}
bool SaveUserFxPreset( const std::string& file, const std::string& name, const Effect& fx )
{
	auto ps = LoadUserFxPresets( file );
	FxPreset np;
	np.name = name;
	np.fx   = fx;
	np.user = true;
	bool rep = false;
	for( auto& p : ps )
		if( p.name == name ) { p = np; rep = true; }
	if( !rep ) ps.push_back( np );
	return WritePresets( file, ps );
}
bool DeleteUserFxPreset( const std::string& file, const std::string& name )
{
	auto ps = LoadUserFxPresets( file );
	ps.erase( std::remove_if( ps.begin(), ps.end(), [ & ]( const FxPreset& p ) { return p.name == name; } ), ps.end() );
	return WritePresets( file, ps );
}

int AddEffectFromPreset( Show& show, const Effect& tmpl, const std::string& name, const std::vector< int >& fixtures, bool start )
{
	Effect e    = tmpl;
	e.id        = show.NextEffectId();
	e.name      = name;
	e.cur       = 0;
	e.freePhase = 0;
	e.fixtures.clear();
	const std::vector< int >* src = &fixtures;
	std::vector< int > all;
	if( fixtures.empty() )
	{
		for( auto& f : show.fixtures ) all.push_back( f.id );
		src = &all;
	}
	for( int id : *src )
		if( const FixtureInst* f = show.FindFixture( id ) )
			if( FxSupports( show, *f, e ) ) e.fixtures.push_back( id );
	if( e.fixtures.empty() && e.kind != FxKind::Modulator ) return 0;
	//pixel bars: chases and colours run over the pixels by default
	for( int id : e.fixtures )
		if( const FixtureInst* f = show.FindFixture( id ) )
			if( f->def->Heads() > 1 && e.kind != FxKind::Position && e.kind != FxKind::Modulator && e.spread > 0 ) e.pixels = true;
	e.on = start;
	show.effects.push_back( e );
	show.dirty = true;
	return e.id;
}

//a group is about to be deleted: effects that used it keep its lamps, later indices move down
void FxGroupRemoved( Show& show, int idx )
{
	if( idx < 0 || idx >= (int)show.groups.size() ) return;
	const std::vector< int > ids = show.groups[ idx ].fixtures;
	auto fixT = [ & ]( FxTarget& t ) {
		if( t.mode != 1 ) return;
		if( t.group == idx ) t.mode = 0;
		else if( t.group > idx ) --t.group;
	};
	for( auto& e : show.effects )
	{
		if( e.group == idx ) { e.fixtures = ids; e.group = -1; }
		else if( e.group > idx ) --e.group;
		fixT( e.tgt );
		for( auto& l : e.lanes ) fixT( l.tgt );
	}
}

//================================================================ programs
const std::vector< ProgramTemplate >& BuiltinPrograms()
{
	static std::vector< ProgramTemplate > v = {
		{ "Party", "Regenbogen-Welle, Kreis versetzt (Moving Heads), Welle (übrige)", { { FxFilter::All, "Regenbogen-Welle" }, { FxFilter::Moving, "Kreis versetzt" }, { FxFilter::Static, "Welle" } } },
		{ "Techno", "Rot/Blau, Ballyhoo, Lauflicht", { { FxFilter::All, "Rot / Blau Wechsel" }, { FxFilter::Moving, "Ballyhoo" }, { FxFilter::Static, "Lauflicht" } } },
		{ "Chill", "Farbverlauf kühl, Atmen, Acht", { { FxFilter::All, "Farbverlauf kühl" }, { FxFilter::All, "Atmen" }, { FxFilter::Moving, "Acht" } } },
		{ "Ballade", "Warmton, Atmen, Pan-Schwenk", { { FxFilter::All, "Warmton" }, { FxFilter::All, "Atmen" }, { FxFilter::Moving, "Pan-Schwenk" } } },
		{ "Feuer", "Feuer, Funkeln", { { FxFilter::All, "Feuer" }, { FxFilter::Static, "Funkeln" } } },
	};
	return v;
}
static bool FilterMatch( const FixtureInst& f, FxFilter flt )
{
	if( !f.def ) return false;
	const bool mov = f.def->kind == Kind::MovingHead || f.def->kind == Kind::Scanner;
	switch( flt )
	{
	case FxFilter::All: return true;
	case FxFilter::Moving: return mov;
	case FxFilter::Static: return !mov;
	case FxFilter::Pixel: return f.def->Heads() > 1;
	}
	return false;
}
int InstantiateProgram( Show& show, const ProgramTemplate& t )
{
	FxProgram p;
	p.name = Ls( t.name );
	for( auto& part : t.parts )
	{
		const FxPreset* pr = nullptr;
		for( auto& x : BuiltinFxPresets() )
			if( x.name == part.second ) pr = &x;
		if( !pr ) continue;
		std::vector< int > ids;
		for( auto& f : show.fixtures )
			if( FilterMatch( f, part.first ) ) ids.push_back( f.id );
		if( ids.empty() ) continue;
		int id = AddEffectFromPreset( show, pr->fx, std::string( Ls( t.name ) ) + ": " + Ls( pr->name ), ids, false );
		if( id ) p.effects.push_back( id );
	}
	if( p.effects.empty() ) return -1;
	show.programs.push_back( p );
	show.dirty = true;
	return (int)show.programs.size() - 1;
}
void StartProgram( Show& show, int idx, bool exclusive )
{
	if( idx < 0 || idx >= (int)show.programs.size() ) return;
	const auto& ids = show.programs[ idx ].effects;
	for( auto& e : show.effects )
	{
		const bool in = std::find( ids.begin(), ids.end(), e.id ) != ids.end();
		if( in ) e.on = true;
		else if( exclusive ) e.on = false;
	}
}
void StopAllEffects( Show& show )
{
	for( auto& e : show.effects ) e.on = false;
}
int ActiveProgram( const Show& show )
{
	for( int i = 0; i < (int)show.programs.size(); ++i )
	{
		const auto& ids = show.programs[ i ].effects;
		if( ids.empty() ) continue;
		bool ok = true;
		for( auto& e : show.effects )
			if( e.on != ( std::find( ids.begin(), ids.end(), e.id ) != ids.end() ) ) { ok = false; break; }
		if( ok ) return i;
	}
	return -1;
}

//================================================================ json
static Json V( const V3& v )
{
	Json a = Json::Arr();
	a.Push( v.x );
	a.Push( v.y );
	a.Push( v.z );
	return a;
}
Json EffectToJson( const Effect& e, bool withFixtures )
{
	Json o = Json::Obj();
	if( withFixtures )
	{
		o.Set( "id", e.id );
		o.Set( "name", e.name );
		o.Set( "on", e.on );
		Json ids = Json::Arr();
		for( int id : e.fixtures ) ids.Push( id );
		o.Set( "fixtures", ids );
	}
	o.Set( "kind", (int)e.kind );
	o.Set( "wave", (int)e.wave );
	o.Set( "shape", (int)e.shape );
	o.Set( "attr", AttrName( e.attr ) );
	o.Set( "colorMode", (int)e.colorMode );
	Json cs = Json::Arr();
	for( auto& c : e.colors ) cs.Push( V( c ) );
	o.Set( "colors", cs );
	o.Set( "pixels", e.pixels );
	o.Set( "sync", e.sync );
	o.Set( "beats", e.beats );
	o.Set( "hz", e.hz );
	o.Set( "spread", e.spread );
	o.Set( "order", (int)e.order );
	o.Set( "groups", e.groups );
	o.Set( "mirror", e.mirror );
	o.Set( "reverse", e.reverse );
	o.Set( "width", e.width );
	o.Set( "lo", e.lo );
	o.Set( "hi", e.hi );
	o.Set( "sizePan", e.sizePan );
	o.Set( "sizeTilt", e.sizeTilt );
	o.Set( "level", e.level );
	o.Set( "fade", e.fade );
	o.Set( "openShutter", e.openShutter );
	o.Set( "scale", e.scale );
	o.Set( "type", (int)e.type );
	if( !e.pts.empty() )
	{
		Json cs = Json::Arr();
		for( auto& p : e.pts )
		{
			Json q = Json::Arr();
			q.Push( p.t );
			q.Push( p.v );
			q.Push( p.shape );
			cs.Push( q );
		}
		o.Set( "points", cs );
	}
	if( e.envDrawn ) o.Set( "envDrawn", true );
	if( withFixtures ) o.Set( "group", e.group );
	auto tj = []( const FxTarget& t ) {
		Json j = Json::Obj();
		j.Set( "mode", t.mode );
		j.Set( "group", t.group );
		j.Set( "attr", AttrName( t.attr ) );
		j.Set( "uni", t.uni );
		j.Set( "ch", t.ch );
		j.Set( "count", t.count );
		return j;
	};
	if( e.kind == FxKind::Modulator )
	{
		o.Set( "target", tj( e.tgt ) );
		o.Set( "relative", e.relative );
		o.Set( "depth", e.depth );
		if( e.type == FxType::Sequencer )
		{
			o.Set( "steps", e.steps );
			o.Set( "stepBeats", e.stepBeats );
			o.Set( "stepOffset", e.stepOffset );
			Json ls = Json::Arr();
			for( auto& l : e.lanes )
			{
				Json lj = Json::Obj();
				lj.Set( "target", tj( l.tgt ) );
				lj.Set( "mute", l.mute );
				lj.Set( "glide", l.glide );
				Json vs = Json::Arr();
				for( float x : l.v ) vs.Push( x );
				lj.Set( "v", vs );
				ls.Push( lj );
			}
			o.Set( "lanes", ls );
		}
		if( e.type == FxType::Envelope )
		{
			o.Set( "atk", e.atk );
			o.Set( "hold", e.hold );
			o.Set( "dec", e.dec );
			o.Set( "sus", e.sus );
			o.Set( "rel", e.rel );
			o.Set( "gate", e.gate );
			o.Set( "curve", e.curve );
			o.Set( "manualTrig", e.manualTrig );
		}
	}
	return o;
}
void EffectFromJson( const Json& o, Effect& e )
{
	e.id   = o[ "id" ].I( e.id );
	e.name = o[ "name" ].Str( e.name );
	e.on   = o[ "on" ].B( false );
	if( o.Find( "fixtures" ) )
	{
		e.fixtures.clear();
		for( auto& x : o[ "fixtures" ].a ) e.fixtures.push_back( x.I() );
	}
	e.kind      = (FxKind)Clamp( o[ "kind" ].I( (int)e.kind ), 0, 4 );
	e.wave      = (FxWave)Clamp( o[ "wave" ].I( (int)e.wave ), 0, 8 );
	e.shape     = (FxShape)Clamp( o[ "shape" ].I( (int)e.shape ), 0, 6 );
	e.attr      = AttrFromName( o[ "attr" ].Str( AttrName( e.attr ) ) );
	e.colorMode = (FxColorMode)Clamp( o[ "colorMode" ].I( (int)e.colorMode ), 0, 2 );
	if( o.Find( "colors" ) )
	{
		e.colors.clear();
		for( auto& c : o[ "colors" ].a ) e.colors.push_back( V3( c[ 0 ].F(), c[ 1 ].F(), c[ 2 ].F() ) );
	}
	e.pixels      = o[ "pixels" ].B( e.pixels );
	e.sync        = o[ "sync" ].B( e.sync );
	e.beats       = Clamp( o[ "beats" ].F( e.beats ), 1.0f / 16.0f, 256.0f );
	e.hz          = Clamp( o[ "hz" ].F( e.hz ), 0.001f, 50.0f );
	e.spread      = o[ "spread" ].F( e.spread );
	e.order       = (FxOrder)Clamp( o[ "order" ].I( (int)e.order ), 0, 4 );
	e.groups      = Clamp( o[ "groups" ].I( e.groups ), 0, 64 );
	e.mirror      = o[ "mirror" ].B( e.mirror );
	e.reverse     = o[ "reverse" ].B( e.reverse );
	e.width       = o[ "width" ].F( e.width );
	e.lo          = o[ "lo" ].F( e.lo );
	e.hi          = o[ "hi" ].F( e.hi );
	e.sizePan     = o[ "sizePan" ].F( e.sizePan );
	e.sizeTilt    = o[ "sizeTilt" ].F( e.sizeTilt );
	e.level       = o[ "level" ].F( e.level );
	e.fade        = o[ "fade" ].F( e.fade );
	e.openShutter = o[ "openShutter" ].B( e.openShutter );
	e.scale       = o[ "scale" ].B( false );
	e.type        = FxMergedType( o.Find( "type" ) ? (FxType)Clamp( o[ "type" ].I( 0 ), 0, (int)FxType::Count - 1 ) : FxTypeGuess( e ) );
	if( const Json* cj = o.Find( "points" ) )
	{
		e.pts.clear();
		for( auto& p : cj->a )
		{
			FxPoint q;
			q.t     = Saturate( p[ 0 ].F() );
			q.v     = Saturate( p[ 1 ].F() );
			q.shape = Clamp( p[ 2 ].I( 2 ), 0, 3 );
			e.pts.push_back( q );
		}
		std::sort( e.pts.begin(), e.pts.end(), []( const FxPoint& a, const FxPoint& b ) { return a.t < b.t; } );
	}
	e.envDrawn = o[ "envDrawn" ].B( false );
	if( o.Find( "group" ) ) e.group = o[ "group" ].I( -1 );
	auto tr = []( const Json& j, FxTarget& t ) {
		t.mode  = Clamp( j[ "mode" ].I( t.mode ), 0, 2 );
		t.group = std::max( 0, j[ "group" ].I( t.group ) );
		t.attr  = AttrFromName( j[ "attr" ].Str( AttrName( t.attr ) ) );
		if( t.attr == Attr::None ) t.attr = Attr::Dimmer;
		t.uni   = std::max( 1, j[ "uni" ].I( t.uni ) );
		t.ch    = Clamp( j[ "ch" ].I( t.ch ), 1, 512 );
		t.count = Clamp( j[ "count" ].I( t.count ), 1, 512 );
	};
	if( e.kind == FxKind::Modulator )
	{
		if( o.Find( "target" ) ) tr( o[ "target" ], e.tgt );
		e.relative = o[ "relative" ].B( e.relative );
		e.depth    = Saturate( o[ "depth" ].F( e.depth ) );
		if( e.type == FxType::Sequencer )
		{
			e.steps      = Clamp( o[ "steps" ].I( e.steps ), 1, 64 );
			e.stepBeats  = Clamp( o[ "stepBeats" ].F( e.stepBeats ), 1.0f / 16.0f, 16.0f );
			e.stepOffset = Clamp( o[ "stepOffset" ].I( e.stepOffset ), -64, 64 );
			if( o.Find( "lanes" ) )
			{
				e.lanes.clear();
				for( auto& lj : o[ "lanes" ].a )
				{
					FxLane l;
					if( lj.Find( "target" ) ) tr( lj[ "target" ], l.tgt );
					l.mute  = lj[ "mute" ].B( false );
					l.glide = lj[ "glide" ].B( false );
					for( auto& x : lj[ "v" ].a ) l.v.push_back( Saturate( x.F() ) );
					l.v.resize( (size_t)e.steps, 0.0f );
					e.lanes.push_back( l );
				}
			}
		}
		if( e.type == FxType::Envelope )
		{
			e.atk        = Clamp( o[ "atk" ].F( e.atk ), 0.0f, 64.0f );
			e.hold       = Clamp( o[ "hold" ].F( e.hold ), 0.0f, 64.0f );
			e.dec        = Clamp( o[ "dec" ].F( e.dec ), 0.0f, 64.0f );
			e.sus        = Saturate( o[ "sus" ].F( e.sus ) );
			e.rel        = Clamp( o[ "rel" ].F( e.rel ), 0.0f, 64.0f );
			e.gate       = Clamp( o[ "gate" ].F( e.gate ), 0.0f, 64.0f );
			e.curve      = Saturate( o[ "curve" ].F( e.curve ) );
			e.manualTrig = o[ "manualTrig" ].B( e.manualTrig );
		}
	}
}
}//namespace ls
