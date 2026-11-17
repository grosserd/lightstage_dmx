// effects tab (presets, effect list, editor, programs, tempo / sync), OSC address window, MIDI mapping files,
// lamp type swap / delete
#include "app.h"
#include "imgui.h"
#include <windows.h>
#include <commdlg.h>

namespace ls
{
//================================================================ engine side
void App::UpdateEffects( double now )
{
	if( fxT_ <= 0 ) fxT_ = now;
	const double dt = now - fxT_;
	if( dt < 1.0 / 60.0 - 0.0005 ) return;//60 Hz effect rate
	fxT_ = now;
	Tempo& t = show.tempo;
	//Ableton Link: created on first use (opens UDP multicast), session tempo + beat
	if( t.source == TempoSource::Link )
	{
		if( !link ) link = std::make_unique< LinkSync >();
		if( !link->Enabled() )
		{
			link->Enable( true );
			link->EnableStartStop( true );
			linkBpmSent_ = t.bpm;
		}
		if( fabsf( t.bpm - linkBpmSent_ ) > 0.001f )
		{
			link->SetTempo( t.bpm );//BPM field / tap / OSC change the session tempo
			linkBpmSent_ = t.bpm;
		}
		double bpm = 120, beat = 0, phase = 0;
		if( link->Capture( bpm, beat, phase, std::max( 1, t.beatsPerBar ) ) )
		{
			t.beat    = beat;
			t.liveBpm = (float)bpm;
			t.peers   = link->Peers();
			t.playing = link->Playing();
			if( fabsf( t.bpm - (float)bpm ) > 0.01f ) t.bpm = linkBpmSent_ = (float)bpm;//follow the session
			t.status = t.peers ? "Link: " + std::to_string( t.peers ) + ( t.peers == 1 ? L( " Teilnehmer" ) : L( " Teilnehmer" ) ) : std::string( L( "Link: allein (Ableton: Link einschalten)" ) );
		}
	}
	else
	{
		if( link && link->Enabled() ) link->Enable( false );
		TempoAdvance( t, std::min( dt, 0.1 ), now );
	}
	RunEffects( show, std::min( dt, 0.1 ) );
}

//================================================================ OSC helpers
void App::OscContext( const std::string& addr )
{
	const std::string ablKey = ablKeyNext_.empty() ? "fx:" + ToLower( addr ) : ablKeyNext_;
	ablKeyNext_.clear();
	if( learn_ )
	{
		MidiMap m;
		m.target = MidiTarget::Param;
		m.param  = addr;
		if( const Param* p = FindParam( show, addr ) ) m.toggle = p->type == ParamType::Bool;
		LearnHook( m );
		return;
	}
	if( ImGui::BeginPopupContextItem( ( "##osc" + addr ).c_str() ) )
	{
		const Param* p = FindParam( show, addr );
		ImGui::TextDisabled( "%s", p ? p->label.c_str() : "Parameter" );
		if( ImGui::MenuItem( ( L( "OSC-Adresse kopieren: " ) + addr ).c_str() ) )
		{
			ImGui::SetClipboardText( addr.c_str() );
			SetStatus( L( "Kopiert: " ) + addr );
		}
		if( p ) ImGui::TextDisabled( L( "Wert: %s" ), ParamRange( *p ).c_str() );
		if( ImGui::MenuItem( L( "MIDI lernen..." ) ) )
		{
			learn_ = true;
			MidiMap m;
			m.target    = MidiTarget::Param;
			m.param     = addr;
			m.toggle    = p && p->type == ParamType::Bool;
			learnTmpl_  = m;
			learnArmed_ = true;
			learnText_  = L( "Bewege jetzt einen MIDI-Regler für: " ) + m.Describe( show );
		}
		if( p && bridge.Subscribers( "params" ) > 0 && ImGui::MenuItem( L( "An Ableton senden" ) ) )
		{
			bridge.SendAssign( *p );
			SetStatus( L( "An Ableton gesendet: " ) + p->label );
		}
		AbletonAssignMenu( ablKey );
		if( ImGui::MenuItem( L( "Alle OSC-Adressen..." ) ) ) oscWin_ = true;
		ImGui::EndPopup();
	}
}

void App::OscWindow()
{
	if( !oscWin_ ) return;
	ImGui::SetNextWindowSize( ImVec2( 900 * ui, 640 * ui ), ImGuiCond_FirstUseEver );
	if( !ImGui::Begin( L( "OSC-Adressen" ), &oscWin_ ) )
	{
		ImGui::End();
		return;
	}
	const auto& ps = Params( show );
	ImGui::TextWrapped( L( "Alle Parameter per OSC (UDP-Port %d, im Tab 'Ausgang / Interface' einschalten). Werte in der angegebenen Einheit; Lampen-Attribute 0..1 oder 0..255 (int). "
	                    "Klick auf eine Zeile kopiert die Adresse. Pixel-Lampen: Adresse + /<Pixel>. Farbe in einer Nachricht: /ls/fixture/<id>/color r g b. DMX direkt: /ls/dmx/<universe>/<kanal>." ),
	                    show.output.oscPort );
	ImGui::TextColored( show.output.oscIn ? ImVec4( 0.5f, 0.9f, 0.5f, 1 ) : ImVec4( 1, 0.6f, 0.3f, 1 ), show.output.oscIn ? L( "OSC-Eingang ist an." ) : L( "OSC-Eingang ist aus." ) );
	ImGui::SameLine();
	if( !show.output.oscIn && ImGui::SmallButton( L( "Einschalten" ) ) ) { show.output.oscIn = true; show.dirty = true; }
	ImGui::SetNextItemWidth( 300 * ui );
	ImGui::InputTextWithHint( "##of", L( "Filter (z.B. fx, stage, fixture/3)" ), oscFilter_, sizeof( oscFilter_ ) );
	ImGui::SameLine();
	std::string q = ToLower( oscFilter_ );
	auto match    = [ & ]( const Param& p ) { return q.empty() || p.addr.find( q ) != std::string::npos || ToLower( p.label ).find( q ) != std::string::npos || ToLower( p.group ).find( q ) != std::string::npos; };
	if( ImGui::Button( L( "Liste exportieren..." ) ) )
	{
		wchar_t buf[ MAX_PATH * 4 ] = L"lightstage_osc.csv";
		OPENFILENAMEW of{};
		of.lStructSize = sizeof( of );
		of.hwndOwner   = (HWND)hwnd;
		of.lpstrFilter = L"CSV (Excel) (*.csv)\0*.csv\0Text (*.txt)\0*.txt\0";
		of.lpstrFile   = buf;
		of.nMaxFile    = MAX_PATH * 4;
		of.lpstrDefExt = L"csv";
		of.Flags       = OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_OVERWRITEPROMPT;
		if( GetSaveFileNameW( &of ) )
		{
			const std::string path = Narrow( buf );
			const bool csv         = ToLower( path ).find( ".csv" ) != std::string::npos;
			std::string t          = csv ? L( "\xEF\xBB\xBF" "Adresse;Parameter;Bereich;Gruppe;aktueller Wert\r\n" ) : L( "Grosserd LightStage - OSC-Adressen (UDP-Port " ) + std::to_string( show.output.oscPort ) + ")\r\n\r\n";
			std::string grp;
			for( auto& p : ps )
			{
				if( !match( p ) ) continue;
				char v[ 32 ];
				snprintf( v, sizeof( v ), "%.3f", p.get ? p.get() : 0.0f );
				if( csv ) t += p.addr + ";" + p.label + ";" + ParamRange( p ) + ";" + p.group + ";" + v + "\r\n";
				else
				{
					if( p.group != grp ) { t += "\r\n[" + p.group + "]\r\n"; grp = p.group; }
					char line[ 512 ];
					snprintf( line, sizeof( line ), "%-42s %-18s %s\r\n", p.addr.c_str(), ParamRange( p ).c_str(), p.label.c_str() );
					t += line;
				}
			}
			if( !csv ) t += L( "\r\n[Lampen, Kurzformen]\r\n/ls/fixture/<id>/color r g b      additive Farbe\r\n/ls/fixture/<id>/<attribut>/<pixel>  einzelner Pixel\r\n/ls/dmx/<universe>/<kanal>          0..1 oder 0..255 (int)\r\n" );
			SetStatus( WriteText( path, t ) ? L( "OSC-Liste gespeichert: " ) + FileName( path ) : L( "Speichern fehlgeschlagen" ) );
		}
	}
	ImGui::SameLine();
	int shown = 0;
	for( auto& p : ps ) shown += match( p ) ? 1 : 0;
	ImGui::TextDisabled( L( "%d von %d" ), shown, (int)ps.size() );
	if( ImGui::BeginTable( "osc", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp, ImVec2( 0, ImGui::GetContentRegionAvail().y ) ) )
	{
		ImGui::TableSetupScrollFreeze( 0, 1 );
		ImGui::TableSetupColumn( L( "Adresse" ), ImGuiTableColumnFlags_WidthStretch, 2.2f );
		ImGui::TableSetupColumn( "Parameter", ImGuiTableColumnFlags_WidthStretch, 2.4f );
		ImGui::TableSetupColumn( L( "Bereich" ), ImGuiTableColumnFlags_WidthStretch, 1.1f );
		ImGui::TableSetupColumn( L( "Wert" ), ImGuiTableColumnFlags_WidthFixed, 70 * ui );
		ImGui::TableHeadersRow();
		std::string grp;
		ImGui::PushFont( fontMono );
		for( size_t i = 0; i < ps.size(); ++i )
		{
			const Param& p = ps[ i ];
			if( !match( p ) ) continue;
			if( p.group != grp )
			{
				grp = p.group;
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::PopFont();
				ImGui::TextColored( ImVec4( 0.55f, 0.75f, 1, 1 ), "%s", grp.c_str() );
				ImGui::PushFont( fontMono );
			}
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID( (int)i );
			if( ImGui::Selectable( p.addr.c_str(), false, ImGuiSelectableFlags_SpanAllColumns ) )
			{
				ImGui::SetClipboardText( p.addr.c_str() );
				SetStatus( L( "Kopiert: " ) + p.addr );
			}
			ImGui::PopID();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted( p.label.c_str() );
			ImGui::TableNextColumn();
			ImGui::TextDisabled( "%s", ParamRange( p ).c_str() );
			ImGui::TableNextColumn();
			if( p.type != ParamType::Trigger && p.get ) ImGui::Text( p.type == ParamType::Float ? "%.3f" : "%.0f", p.get() );
		}
		ImGui::PopFont();
		ImGui::EndTable();
	}
	ImGui::End();
}

//================================================================ MIDI mapping files
bool App::ExportMidi( const std::string& file )
{
	Json j = Json::Obj();
	j.Set( "format", "grosserd-lightstage-midi" );
	j.Set( "version", 1 );
	Json a = Json::Arr();
	for( auto& m : show.midi ) a.Push( MidiMapToJson( m ) );
	j.Set( "mappings", a );
	return WriteText( file, WriteJson( j ) );
}
int App::ImportMidi( const std::string& file, bool replace )
{
	std::string t;
	Json j;
	if( !ReadText( file, t ) || !ParseJson( t, j ) || j[ "format" ].Str() != "grosserd-lightstage-midi" ) return -1;
	if( replace ) show.midi.clear();
	int n = 0;
	for( auto& o : j[ "mappings" ].a )
	{
		MidiMap m = MidiMapFromJson( o );
		show.midi.erase( std::remove_if( show.midi.begin(), show.midi.end(), [ & ]( const MidiMap& x ) { return x.type == m.type && x.number == m.number && x.channel == m.channel && x.device == m.device && x.target == m.target && x.param == m.param && x.attr == m.attr && x.fixtures == m.fixtures && x.snapshot == m.snapshot && x.channelDmx == m.channelDmx; } ), show.midi.end() );
		show.midi.push_back( m );
		++n;
	}
	show.dirty = true;
	return n;
}

//================================================================ lamp types
void App::OpenSwapDialog( const std::vector< int >& ids )
{
	if( ids.empty() ) return;
	add_.open       = true;
	add_.replace    = true;
	add_.replaceIds = ids;
	add_.readdress  = true;
	if( const FixtureInst* f = show.FindFixture( ids[ 0 ] ) )
		if( f->def ) { add_.def = f->def; add_.mode = f->mode; }
}
int App::SwapFixtureType( const std::vector< int >& ids, const FixtureDef* d, int mode, bool readdress )
{
	if( !d ) return 0;
	mode   = Clamp( mode, 0, (int)d->modes.size() - 1 );
	int n  = 0;
	const int fp = d->modes[ mode ].Footprint();
	for( int id : ids )
	{
		FixtureInst* f = show.FindFixture( id );
		if( !f ) continue;
		//old channels back to 0 (they may belong to nobody afterwards)
		{
			std::lock_guard< std::mutex > l( show.dmxLock );
			if( DmxFrame* fr = show.Frame( f->universe ) )
				for( int i = 0; i < f->Footprint(); ++i )
					if( f->address - 1 + i >= 0 && f->address - 1 + i < 512 ) ( *fr )[ f->address - 1 + i ] = 0;
		}
		const bool autoName = !f->def || f->name.empty() || f->name == f->def->name + " " + std::to_string( f->id );
		f->def   = d;
		f->defId = d->id;
		f->mode  = mode;
		f->beam  = 0;//abstrahlwinkel from the new profile
		if( autoName ) f->name = d->name + " " + std::to_string( f->id );
		if( readdress && ( show.Overlaps( f->universe, f->address, fp, f->id ) || f->address + fp - 1 > 512 ) )
		{
			int a = show.FreeAddress( f->universe, fp, f->address, f->id );
			if( !a ) a = show.FreeAddress( f->universe, fp, 1, f->id );
			if( !a )
			{
				f->universe = show.AddUniverse();
				a           = 1;
			}
			f->address = a;
		}
		f->st = FixtureState();
		show.Home( *f );
		++n;
	}
	show.Bump();
	return n;
}
bool App::DeleteUserFixture( const FixtureDef* d, std::string* why )
{
	if( !d || d->id.rfind( "user/", 0 ) != 0 )
	{
		if( why ) *why = L( "keine eigene Lampe" );
		return false;
	}
	for( auto& f : show.fixtures )
		if( f.def == d )
		{
			if( why ) *why = L( "wird in der Show benutzt" );
			return false;
		}
	if( !d->source.empty() ) DeleteFileW( Widen( d->source ).c_str() );
	const std::string id = d->id;
	if( add_.def == d ) add_.def = nullptr;
	add_.list.clear();
	add_.lastSearch = "\x01";
	lib.defs.erase( std::remove_if( lib.defs.begin(), lib.defs.end(), [ & ]( const std::unique_ptr< FixtureDef >& x ) { return x->id == id; } ), lib.defs.end() );
	return true;
}

//================================================================ effects tab (builder)
//cycle lengths in beats; labels as note values / bars (4/4)
static const float kBeatSteps[] = { 0.0625f, 0.125f, 0.25f, 0.5f, 1, 2, 4, 8, 16, 32, 64 };
static std::string BeatLabel( float b )
{
	char t[ 32 ];
	if( b < 1 - 1e-4f ) snprintf( t, sizeof( t ), "1/%d", (int)lroundf( 4.0f / b ) );
	else if( fabsf( b - 1 ) < 1e-4f ) snprintf( t, sizeof( t ), "Beat" );
	else if( fabsf( b - 2 ) < 1e-4f ) snprintf( t, sizeof( t ), "2 Beats" );
	else if( fabsf( b - 4 ) < 1e-4f ) snprintf( t, sizeof( t ), "Bar" );
	else if( fmodf( b, 4 ) < 1e-4f ) snprintf( t, sizeof( t ), "%d Bars", (int)lroundf( b / 4 ) );
	else snprintf( t, sizeof( t ), "%g Beats", b );
	return t;
}
static ImU32 Col( const V3& c, float a = 1 ) { return ImGui::ColorConvertFloat4ToU32( ImVec4( Saturate( c.x ), Saturate( c.y ), Saturate( c.z ), a ) ); }
static ImU32 ColA( ImU32 c, float a ) { return ( c & 0x00ffffff ) | ( (ImU32)( Saturate( a ) * 255 ) << 24 ); }
//lane colours (GIAMO bands)
static const V3 kLaneCol[ 8 ] = { V3( 0.36f, 0.6f, 1.0f ), V3( 1.0f, 0.55f, 0.25f ), V3( 0.35f, 0.85f, 0.45f ), V3( 1.0f, 0.4f, 0.65f ),
	                          V3( 0.68f, 0.5f, 1.0f ), V3( 1.0f, 0.85f, 0.3f ), V3( 0.3f, 0.85f, 0.9f ), V3( 1.0f, 0.35f, 0.35f ) };
static const ImU32 kBg = IM_COL32( 17, 19, 25, 255 ), kGrid = IM_COL32( 42, 47, 58, 255 ), kGridHi = IM_COL32( 62, 69, 84, 255 ), kDim = IM_COL32( 120, 128, 145, 255 );

//small icon of a builder type
static void TypeIcon( ImDrawList* dl, ImVec2 a, ImVec2 b, FxType t, float ui )
{
	const ImU32 c  = Col( FxTypeColor( t ) );
	const float w = b.x - a.x, h = b.y - a.y;
	auto P        = [ & ]( float x, float y ) { return ImVec2( a.x + x * w, a.y + ( 1 - y ) * h ); };
	const float th = 1.6f * ui;
	switch( t )
	{
	case FxType::Chase:
		for( int i = 0; i < 4; ++i )
			dl->AddRectFilled( P( i * 0.26f, 0.75f ), P( i * 0.26f + 0.2f, 0.25f ), i == 1 ? c : ColA( c, 0.22f ), 2 );
		break;
	case FxType::Intensity:
	{
		//chase blocks under a soft wave
		for( int i = 0; i < 4; ++i ) dl->AddRectFilled( P( i * 0.26f, 0.42f ), P( i * 0.26f + 0.2f, 0.05f ), i == 1 ? c : ColA( c, 0.25f ), 2 );
		ImVec2 pts[ 25 ];
		for( int i = 0; i <= 24; ++i ) pts[ i ] = P( i / 24.0f, 0.72f + 0.2f * sinf( i / 24.0f * 2 * kPi ) );
		dl->AddPolyline( pts, 25, c, 0, th );
		break;
	}
	case FxType::Colour:
		for( int i = 0; i < 6; ++i )
			dl->AddRectFilledMultiColor( P( i / 6.0f, 0.85f ), P( ( i + 1 ) / 6.0f, 0.15f ), Col( Hsv( i / 6.0f, 0.85f, 1 ) ), Col( Hsv( ( i + 1 ) / 6.0f, 0.85f, 1 ) ), Col( Hsv( ( i + 1 ) / 6.0f, 0.85f, 1 ) ), Col( Hsv( i / 6.0f, 0.85f, 1 ) ) );
		break;
	case FxType::Wave:
	case FxType::Lfo:
	{
		ImVec2 pts[ 25 ];
		for( int i = 0; i <= 24; ++i ) pts[ i ] = P( i / 24.0f, 0.5f + 0.38f * sinf( i / 24.0f * 2 * kPi ) );
		dl->AddPolyline( pts, 25, c, 0, t == FxType::Lfo ? th * 1.4f : th );
		break;
	}
	case FxType::Strobe:
		for( int i = 0; i < 3; ++i ) dl->AddRectFilled( P( 0.08f + i * 0.33f, 0.92f ), P( 0.16f + i * 0.33f, 0.08f ), c );
		break;
	case FxType::Sparkle:
	{
		const float px[] = { 0.1f, 0.35f, 0.6f, 0.85f, 0.22f, 0.72f }, py[] = { 0.7f, 0.25f, 0.8f, 0.4f, 0.2f, 0.85f };
		for( int i = 0; i < 6; ++i ) dl->AddCircleFilled( P( px[ i ], py[ i ] ), ( i % 2 ? 1.6f : 2.6f ) * ui, ColA( c, i % 2 ? 0.5f : 1 ) );
		break;
	}
	case FxType::ColorChange:
	{
		const V3 cs[] = { V3( 1, 0.1f, 0.2f ), V3( 0.2f, 0.4f, 1 ), V3( 0.2f, 1, 0.5f ) };
		for( int i = 0; i < 3; ++i ) dl->AddRectFilled( P( i * 0.34f, 0.85f ), P( i * 0.34f + 0.3f, 0.15f ), Col( cs[ i ] ), 2 );
		break;
	}
	case FxType::ColorFade:
		dl->AddRectFilledMultiColor( P( 0, 0.85f ), P( 1, 0.15f ), Col( V3( 1, 0, 0.4f ) ), Col( V3( 0.1f, 0.4f, 1 ) ), Col( V3( 0.1f, 0.4f, 1 ) ), Col( V3( 1, 0, 0.4f ) ) );
		break;
	case FxType::Rainbow:
		for( int i = 0; i < 6; ++i )
			dl->AddRectFilledMultiColor( P( i / 6.0f, 0.85f ), P( ( i + 1 ) / 6.0f, 0.15f ), Col( Hsv( i / 6.0f, 1, 1 ) ), Col( Hsv( ( i + 1 ) / 6.0f, 1, 1 ) ), Col( Hsv( ( i + 1 ) / 6.0f, 1, 1 ) ), Col( Hsv( i / 6.0f, 1, 1 ) ) );
		break;
	case FxType::Movement:
		dl->AddCircle( P( 0.5f, 0.5f ), h * 0.38f, c, 20, th );
		dl->AddCircleFilled( P( 0.5f + 0.27f, 0.5f + 0.22f ), 2.5f * ui, c );
		break;
	case FxType::Beam:
		dl->AddTriangleFilled( P( 0.1f, 0.5f ), P( 0.95f, 0.95f ), P( 0.95f, 0.05f ), ColA( c, 0.35f ) );
		dl->AddCircleFilled( P( 0.1f, 0.5f ), 2.5f * ui, c );
		break;
	case FxType::Sequencer:
	{
		const float v[] = { 1, 0.3f, 0.65f, 0.15f, 0.9f, 0.45f };
		for( int i = 0; i < 6; ++i ) dl->AddRectFilled( P( i / 6.0f + 0.02f, v[ i ] * 0.9f ), P( ( i + 1 ) / 6.0f - 0.02f, 0.05f ), ColA( c, 0.4f + 0.6f * v[ i ] ), 1 );
		break;
	}
	case FxType::Envelope:
	{
		ImVec2 pts[] = { P( 0, 0.05f ), P( 0.15f, 0.95f ), P( 0.4f, 0.55f ), P( 0.7f, 0.55f ), P( 1, 0.05f ) };
		dl->AddPolyline( pts, 5, c, 0, th * 1.2f );
		break;
	}
	default: break;
	}
}

//0..1 values shown as percent
static bool PctSlider( const char* id, float* v, float mn = 0, float mx = 1 )
{
	float p = *v * 100.0f;
	if( !ls::rs::SliderFloat( id, &p, mn * 100.0f, mx * 100.0f, "%.0f %%" ) ) return false;
	*v = Clamp( p / 100.0f, mn, mx );
	return true;
}
static bool PctRange( const char* id, float* lo, float* hi )
{
	float a = *lo * 100.0f, b = *hi * 100.0f;
	if( !ls::rs::DragFloatRange2( id, &a, &b, 0.5f, 0, 100, "Min %.0f %%", "Max %.0f %%", ImGuiSliderFlags_AlwaysClamp ) ) return false;
	*lo = Saturate( a / 100.0f );
	*hi = Saturate( b / 100.0f );
	return true;
}

//attributes the lamps of a list have (union)
static std::vector< Attr > AttrsOf( Show& show, const std::vector< int >& ids )
{
	std::vector< Attr > r;
	for( int id : ids )
		if( const FixtureInst* f = show.FindFixture( id ) )
			if( const Mode* m = f->M() )
				for( auto& c : m->ch )
					if( !c.isFine && c.attr != Attr::None && c.attr != Attr::Shutter && std::find( r.begin(), r.end(), c.attr ) == r.end() ) r.push_back( c.attr );
	std::sort( r.begin(), r.end() );
	if( std::find( r.begin(), r.end(), Attr::Dimmer ) == r.end() ) r.insert( r.begin(), Attr::Dimmer );
	return r;
}
static std::string FxTargetLabel( Show& show, const Effect& e, const FxTarget& t )
{
	char b[ 128 ];
	if( t.mode == 2 )
	{
		if( t.count > 1 ) snprintf( b, sizeof( b ), "DMX %d.%03d-%03d", t.uni, t.ch, std::min( 512, t.ch + t.count - 1 ) );
		else snprintf( b, sizeof( b ), "DMX %d.%03d", t.uni, t.ch );
		return b;
	}
	std::string who = t.mode == 1 && t.group >= 0 && t.group < (int)show.groups.size() ? show.groups[ t.group ].name : std::string( L( "Lampen" ) );
	if( t.mode == 0 && e.group >= 0 && e.group < (int)show.groups.size() ) who = show.groups[ e.group ].name;
	return who + " · " + AttrName( t.attr );
}

//target editor: lamps of the effect / a group / DMX channels, + attribute
static bool TargetUi( Show& show, const Effect& e, FxTarget& t, float ui, float lw )
{
	bool ch   = false;
	auto row  = [ & ]( const char* l ) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted( l );
		ImGui::SameLine( lw );
		ImGui::SetNextItemWidth( -1 );
	};
	std::string cur = t.mode == 2 ? std::string( L( "DMX-Kanäle" ) ) : ( t.mode == 1 && t.group < (int)show.groups.size() ? std::string( L( "Gruppe: " ) ) + show.groups[ t.group ].name : std::string( L( "Lampen des Effekts" ) ) );
	row( L( "Ziel" ) );
	if( ImGui::BeginCombo( "##tmode", cur.c_str() ) )
	{
		if( ImGui::Selectable( L( "Lampen des Effekts" ), t.mode == 0 ) ) { t.mode = 0; ch = true; }
		for( int g = 0; g < (int)show.groups.size(); ++g )
		{
			ImGui::PushID( g );
			if( ImGui::Selectable( ( std::string( L( "Gruppe: " ) ) + show.groups[ g ].name ).c_str(), t.mode == 1 && t.group == g ) ) { t.mode = 1; t.group = g; ch = true; }
			ImGui::PopID();
		}
		if( ImGui::Selectable( L( "DMX-Kanäle" ), t.mode == 2 ) ) { t.mode = 2; ch = true; }
		ImGui::EndCombo();
	}
	if( t.mode == 2 )
	{
		row( L( "Universe / Kanal" ) );
		int v[ 2 ] = { t.uni, t.ch };
		if( ImGui::DragInt2( "##uc", v, 0.2f, 1, 512 ) ) { t.uni = Clamp( v[ 0 ], 1, 64 ); t.ch = Clamp( v[ 1 ], 1, 512 ); ch = true; }
		row( L( "Anzahl Kanäle" ) );
		ch |= ls::rs::SliderInt( "##cnt", &t.count, 1, 64 );
		ImGui::SetItemTooltip( L( "Mehrere Kanäle nebeneinander: Phasenversatz / Schritt-Versatz läuft über sie" ) );
	}
	else
	{
		const std::vector< int >& ids = t.mode == 1 && t.group < (int)show.groups.size() ? show.groups[ t.group ].fixtures : FxFixtureIds( show, e );
		auto attrs = AttrsOf( show, ids );
		row( L( "Attribut" ) );
		if( ImGui::BeginCombo( "##tattr", AttrName( t.attr ) ) )
		{
			for( Attr a : attrs )
				if( ImGui::Selectable( AttrName( a ), a == t.attr ) ) { t.attr = a; ch = true; }
			ImGui::EndCombo();
		}
	}
	return ch;
}

//one-click: new effect of a type on the selection (or every lamp that fits)
int App::AddEffectOfType( FxType t )
{
	Effect tmpl = FxMakeType( t );
	tmpl.name   = Ls( FxTypeName( t ) );
	//sequencer lanes start on the effect's lamps (dimmer); a second lane on colour / position when they exist
	const int id = AddEffectFromPreset( show, tmpl, Ls( FxTypeName( t ) ), sel, true );
	if( !id ) return 0;
	Effect* e = show.FindEffect( id );
	if( e && t == FxType::Lfo )
	{
		//moving heads selected: an LFO on pan is the more likely wish
		bool mov = !e->fixtures.empty();
		for( int fid : e->fixtures )
			if( const FixtureInst* f = show.FindFixture( fid ) ) mov &= show.HasAttr( *f, Attr::Pan );
		if( mov ) { e->tgt.attr = Attr::Pan; e->relative = true; e->depth = 0.3f; e->spread = 360; }
	}
	if( e && t == FxType::Sequencer ) e->stepOffset = 0;
	//numbered names: "LFO 2"
	if( e )
	{
		int n = 0;
		for( auto& x : show.effects )
			if( x.type == t ) ++n;
		if( n > 1 ) e->name += " " + std::to_string( n );
	}
	show.fxSelId = id;
	return id;
}

void App::TabEffects()
{
	if( !userPresetsLoaded_ )
	{
		userPresets_       = LoadUserFxPresets( UserFxFile() );
		userPresetsLoaded_ = true;
	}
	Tempo& tp = show.tempo;
	if( !show.FindEffect( show.fxSelId ) )
	{
		show.fxSelId = 0;
		for( auto& e : show.effects )
			if( !show.fxSelId || e.on ) { show.fxSelId = e.id; if( e.on ) break; }
	}
	//---- tempo bar
	{
		const float rightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted( "Tempo" );
		ImGui::SameLine();
		int src = (int)tp.source;
		ImGui::SetNextItemWidth( 140 * ui );
		if( ImGui::Combo( "##src", &src, L( "Intern / Tap\0MIDI-Clock\0Ableton Link\0" ) ) ) { tp.source = (TempoSource)src; show.dirty = true; }
		ImGui::SetItemTooltip( L( "Intern: BPM + Tap. MIDI-Clock: von Ableton/DAW (MIDI-Spur > Sync an loopMIDI). Ableton Link: Netzwerk-Sync (in Live 'Link' einschalten)." ) );
		OscContext( "/ls/tempo/source" );
		ImGui::SameLine();
		float bpm = tp.source == TempoSource::MidiClock ? tp.liveBpm : tp.bpm;
		ImGui::BeginDisabled( tp.source == TempoSource::MidiClock );
		ImGui::SetNextItemWidth( 90 * ui );
		if( ls::rs::DragFloat( "##bpm", &bpm, 0.1f, 20, 300, "%.1f BPM" ) ) { tp.bpm = Clamp( bpm, 20.0f, 300.0f ); show.dirty = true; }
		OscContext( "/ls/tempo/bpm" );
		ImGui::SameLine();
		if( ImGui::Button( "TAP", ImVec2( 56 * ui, 0 ) ) ) TempoTap( tp, Now() );
		OscContext( "/ls/tempo/tap" );
		ImGui::EndDisabled();
		ImGui::SameLine();
		if( ImGui::Button( L( "Taktanfang" ) ) ) TempoResync( tp );
		ImGui::SetItemTooltip( L( "Jetzt ist Schlag 1 (Effekte springen auf den Taktanfang)" ) );
		OscContext( "/ls/tempo/resync" );
		ImGui::SameLine();
		ImGui::SetNextItemWidth( 90 * ui );
		if( ls::rs::SliderFloat( "##fxm", &show.fxMaster, 0, 1, L( "Effekte %.2f" ) ) ) show.dirty = true;
		OscContext( "/ls/fx/master" );
		ImGui::SameLine();
		ImGui::SetNextItemWidth( 110 * ui );
		if( ls::rs::SliderFloat( "##fxs", &show.fxSpeed, 0.25f, 4, "Tempo x%.2f", ImGuiSliderFlags_Logarithmic ) ) show.dirty = true;
		OscContext( "/ls/fx/speed" );
		ImGui::SameLine();
		if( ImGui::Checkbox( L( "Halt" ), &show.fxHold ) ) show.dirty = true;
		OscContext( "/ls/fx/hold" );
		ImGui::SameLine();
		//beat lamps
		const int bpb  = std::max( 1, tp.beatsPerBar );
		const double b = tp.beat;
		const int cur  = ( (int)floor( b ) % bpb + bpb ) % bpb;
		const float fr = (float)( b - floor( b ) );
		ImVec2 p       = ImGui::GetCursorScreenPos();
		const float sz = ImGui::GetFrameHeight() * 0.62f, gap = 5 * ui;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		for( int i = 0; i < bpb; ++i )
		{
			ImVec2 a( p.x + i * ( sz + gap ), p.y + ( ImGui::GetFrameHeight() - sz ) * 0.5f );
			const bool on = i == cur;
			const float k = on ? 1.0f - fr * 0.7f : 0.0f;
			const ImU32 c = on ? ( i == 0 ? IM_COL32( 255, (int)( 150 + 60 * k ), 60, (int)( 120 + 135 * k ) ) : IM_COL32( 90, (int)( 170 + 60 * k ), 255, (int)( 120 + 135 * k ) ) ) : IM_COL32( 60, 66, 80, 255 );
			dl->AddRectFilled( a, ImVec2( a.x + sz, a.y + sz ), c, 3 );
		}
		ImGui::Dummy( ImVec2( bpb * ( sz + gap ), ImGui::GetFrameHeight() ) );
		ImGui::SameLine();
		ImGui::TextDisabled( "%d.%d  %.1f BPM  %s", (int)floor( b / bpb ) + 1, cur + 1, TempoBpm( tp ), tp.status.c_str() );
		const float bw = ImGui::CalcTextSize( L( "Alle Effekte aus" ) ).x + ImGui::GetStyle().FramePadding.x * 2;
		ImGui::SameLine( std::max( ImGui::GetCursorPosX() + 10 * ui, rightX - bw ) );
		ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.42f, 0.14f, 0.12f, 1 ) );
		if( ImGui::Button( L( "Alle Effekte aus" ) ) ) StopAllEffects( show );
		ImGui::PopStyleColor();
		OscContext( "/ls/fx/stop" );
	}
	ImGui::Separator();
	const float H  = ImGui::GetContentRegionAvail().y;
	const float W  = ImGui::GetContentRegionAvail().x;
	const float wA = Clamp( W * 0.21f, 240 * ui, 320 * ui ), wB = Clamp( W * 0.17f, 200 * ui, 270 * ui );
	//---- column A: builder - effect types
	ImGui::BeginChild( "fxA", ImVec2( wA, H ), ImGuiChildFlags_Borders );
	{
		ImGui::TextDisabled( sel.empty() ? L( "Neuer Effekt für alle passenden Lampen" ) : L( "Neuer Effekt für die Auswahl (%d Lampen)" ), (int)sel.size() );
		struct Cat { const char* name; std::vector< FxType > types; };
		static const Cat cats[] = {
			{ "HELLIGKEIT", { FxType::Intensity } },
			{ "FARBE", { FxType::Colour } },
			{ "BEWEGUNG & STRAHL", { FxType::Movement, FxType::Beam } },
			{ "MODULATOREN", { FxType::Lfo, FxType::Sequencer, FxType::Envelope } },
		};
		ImDrawList* dl   = ImGui::GetWindowDrawList();
		const float rowH = ImGui::GetTextLineHeight() * 2 + 10 * ui;
		for( auto& c : cats )
		{
			ImGui::Dummy( ImVec2( 0, 2 * ui ) );
			ImGui::TextColored( ImVec4( 0.55f, 0.6f, 0.7f, 1 ), "%s", Ls( c.name ) );
			for( FxType t : c.types )
			{
				ImGui::PushID( (int)t );
				const ImVec2 a = ImGui::GetCursorScreenPos();
				const float w  = ImGui::GetContentRegionAvail().x;
				const bool hit = ImGui::InvisibleButton( "t", ImVec2( w, rowH ) );
				const bool hov = ImGui::IsItemHovered();
				const ImVec2 b( a.x + w, a.y + rowH );
				const V3 tc = FxTypeColor( t );
				dl->AddRectFilled( a, b, hov ? IM_COL32( 40, 45, 58, 255 ) : IM_COL32( 27, 30, 38, 255 ), 5 * ui );
				dl->AddRectFilled( a, ImVec2( a.x + 3 * ui, b.y ), Col( tc ), 5 * ui, ImDrawFlags_RoundCornersLeft );
				const float ic = rowH - 14 * ui;
				TypeIcon( dl, ImVec2( a.x + 12 * ui, a.y + 7 * ui ), ImVec2( a.x + 12 * ui + ic * 1.3f, a.y + 7 * ui + ic ), t, ui );
				const float tx = a.x + 22 * ui + ic * 1.3f;
				dl->AddText( ImVec2( tx, a.y + 4 * ui ), IM_COL32( 230, 233, 240, 255 ), Ls( FxTypeName( t ) ) );
				dl->PushClipRect( ImVec2( tx, a.y ), ImVec2( b.x - 4 * ui, b.y ), true );
				dl->AddText( ImVec2( tx, a.y + 5 * ui + ImGui::GetTextLineHeight() ), kDim, Ls( FxTypeInfo( t ) ) );
				dl->PopClipRect();
				if( hov ) dl->AddText( ImVec2( b.x - 18 * ui, a.y + 4 * ui ), Col( tc ), "+" );
				if( hit )
				{
					const int id = AddEffectOfType( t );
					if( id ) SetStatus( L( "Effekt '" ) + std::string( Ls( FxTypeName( t ) ) ) + L( "' gestartet" ) );
					else SetStatus( L( "Keine passende Lampe für '" ) + std::string( Ls( FxTypeName( t ) ) ) + "'" );
				}
				ImGui::SetItemTooltip( "%s", Ls( FxTypeInfo( t ) ) );
				ImGui::PopID();
				ImGui::Dummy( ImVec2( 0, 1 * ui ) );
			}
		}
		//programs: whole looks from several effects
		ImGui::Dummy( ImVec2( 0, 4 * ui ) );
		if( ImGui::CollapsingHeader( L( "Programme" ) ) )
		{
			const float progRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
			const int active      = ActiveProgram( show );
			int delP              = -1;
			for( int i = 0; i < (int)show.programs.size(); ++i )
			{
				ImGui::PushID( 5000 + i );
				if( i > 0 ) ImGui::SameLine();
				if( ImGui::GetCursorPosX() + ImGui::CalcTextSize( show.programs[ i ].name.c_str() ).x + 20 * ui > progRight ) ImGui::NewLine();
				const bool on = i == active;
				if( on ) ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.15f, 0.55f, 0.3f, 1 ) );
				if( ImGui::Button( show.programs[ i ].name.c_str() ) ) StartProgram( show, i, true );
				if( on ) ImGui::PopStyleColor();
				ImGui::SetItemTooltip( L( "Startet nur die Effekte dieses Programms (%d)" ), (int)show.programs[ i ].effects.size() );
				OscContext( "/ls/program/" + std::to_string( i + 1 ) );
				if( ImGui::BeginPopupContextItem( "pctx" ) )
				{
					if( ImGui::MenuItem( L( "Programm löschen (Effekte bleiben)" ) ) ) delP = i;
					ImGui::EndPopup();
				}
				ImGui::PopID();
			}
			if( delP >= 0 ) { show.programs.erase( show.programs.begin() + delP ); show.dirty = true; }
			if( show.programs.empty() ) ImGui::TextDisabled( L( "Noch keine Programme." ) );
			int running = 0;
			for( auto& e : show.effects ) running += e.on ? 1 : 0;
			ImGui::BeginDisabled( running == 0 );
			if( ImGui::SmallButton( L( "+ aus laufenden Effekten" ) ) )
			{
				FxProgram p;
				p.name = L( "Programm " ) + std::to_string( show.programs.size() + 1 );
				for( auto& e : show.effects )
					if( e.on ) p.effects.push_back( e.id );
				show.programs.push_back( p );
				show.dirty = true;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if( ImGui::SmallButton( L( "+ Vorlage..." ) ) ) ImGui::OpenPopup( "progtpl" );
			if( ImGui::BeginPopup( "progtpl" ) )
			{
				for( auto& t : BuiltinPrograms() )
					if( ImGui::MenuItem( Ls( t.name ), Ls( t.info ) ) )
					{
						int idx = InstantiateProgram( show, t );
						if( idx >= 0 ) { StartProgram( show, idx, true ); SetStatus( L( "Programm '" ) + std::string( Ls( t.name ) ) + L( "' angelegt und gestartet" ) ); }
						else SetStatus( L( "Keine passenden Lampen für das Programm" ) );
					}
				ImGui::EndPopup();
			}
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();
	//---- column B: rack (the effects of the show)
	ImGui::BeginChild( "fxB", ImVec2( wB, H ), ImGuiChildFlags_Borders );
	{
		ImGui::TextDisabled( L( "Effekte (%d)" ), (int)show.effects.size() );
		int del = -1, dup = -1, up = -1;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		for( int i = 0; i < (int)show.effects.size(); ++i )
		{
			Effect& e = show.effects[ i ];
			ImGui::PushID( e.id );
			const float fh = ImGui::GetFrameHeight();
			const float bh = fh + 22 * ui;//name row + level slider
			const ImVec2 a = ImGui::GetCursorScreenPos();
			const float w  = ImGui::GetContentRegionAvail().x;
			const bool selE = show.fxSelId == e.id;
			const V3 tc     = FxTypeColor( e.type );
			dl->AddRectFilled( a, ImVec2( a.x + w, a.y + bh ), selE ? IM_COL32( 44, 52, 70, 255 ) : IM_COL32( 26, 29, 37, 255 ), 4 * ui );
			if( selE ) dl->AddRect( a, ImVec2( a.x + w, a.y + bh ), ColA( Col( tc ), 0.8f ), 4 * ui );
			//power pill
			ImGui::SetCursorScreenPos( ImVec2( a.x + 4 * ui, a.y + 3 * ui ) );
			const ImVec4 pc = e.on ? ImVec4( tc.x * 0.55f, tc.y * 0.55f, tc.z * 0.55f, 1 ) : ImVec4( 0.2f, 0.22f, 0.27f, 1 );
			ImGui::PushStyleColor( ImGuiCol_Button, pc );
			ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( pc.x + 0.08f, pc.y + 0.08f, pc.z + 0.08f, 1 ) );
			if( ImGui::Button( e.on ? L( "AN" ) : L( "aus" ), ImVec2( 40 * ui, ImGui::GetFrameHeight() ) ) ) { e.on = !e.on; show.dirty = true; }
			ImGui::PopStyleColor( 2 );
			OscContext( "/ls/fx/" + std::to_string( e.id ) + "/on" );
			ImGui::SameLine();
			const ImVec2 la = ImGui::GetCursorScreenPos();
			char lb[ 160 ];
			snprintf( lb, sizeof( lb ), "##sel%d", e.id );
			if( ImGui::InvisibleButton( lb, ImVec2( std::max( 10.0f, a.x + w - la.x - 4 * ui ), ImGui::GetFrameHeight() ) ) ) { show.fxSelId = e.id; delCtx_ = 2; }
			if( ImGui::BeginPopupContextItem( "ectx" ) )
			{
				if( ImGui::MenuItem( L( "Lampen auswählen" ) ) ) sel = FxFixtureIds( show, e );
				if( ImGui::MenuItem( L( "Duplizieren" ) ) ) dup = i;
				if( ImGui::MenuItem( L( "Nach oben" ), nullptr, false, i > 0 ) ) up = i;
				if( ImGui::MenuItem( L( "Löschen" ) ) ) del = i;
				ImGui::EndPopup();
			}
			ImGui::SetItemTooltip( L( "%s | %d Lampen | Nr. %d" ), Ls( FxTypeName( e.type ) ), (int)FxFixtureIds( show, e ).size(), e.id );
			//name + type line + activity
			TypeIcon( dl, ImVec2( la.x, la.y + 3 * ui ), ImVec2( la.x + 20 * ui, la.y + ImGui::GetFrameHeight() - 3 * ui ), e.type, ui );
			dl->PushClipRect( la, ImVec2( a.x + w - 4 * ui, a.y + bh ), true );
			dl->AddText( ImVec2( la.x + 26 * ui, la.y + ( ImGui::GetFrameHeight() - ImGui::GetTextLineHeight() ) * 0.5f ), IM_COL32( 225, 229, 236, 255 ), e.name.c_str() );
			dl->PopClipRect();
			const float act = e.Drive();
			dl->AddRectFilled( ImVec2( la.x + 26 * ui, a.y + fh + 2 * ui ), ImVec2( la.x + 26 * ui + ( a.x + w - la.x - 32 * ui ) * Saturate( act ), a.y + fh + 4 * ui ), Col( tc, 0.85f ), 1 );
			//level: the master of this one effect (same as 'Amount' in the editor)
			{
				const float x0 = a.x + 6 * ui, x1 = a.x + w - 44 * ui;
				const float y0 = a.y + fh + 8 * ui, y1 = a.y + bh - 4 * ui;
				ImGui::SetCursorScreenPos( ImVec2( x0, y0 - 2 * ui ) );
				ImGui::InvisibleButton( "lvl", ImVec2( std::max( 10.0f, a.x + w - 4 * ui - x0 ), y1 - y0 + 4 * ui ) );
				const bool lh = ImGui::IsItemHovered(), la2 = ImGui::IsItemActive();
				if( ImGui::IsItemActivated() ) show.fxSelId = e.id;
				if( lh && ImGui::IsMouseDoubleClicked( 0 ) ) { e.level = 1; show.dirty = true; }
				else if( la2 )
				{
					float v = Saturate( ( ImGui::GetIO().MousePos.x - x0 ) / std::max( 1.0f, x1 - x0 ) );
					if( ImGui::GetIO().KeyShift ) v = roundf( v * 20 ) / 20;
					if( fabsf( v - e.level ) > 1e-4f ) { e.level = v; show.dirty = true; }
				}
				if( lh && ImGui::GetIO().MouseWheel != 0 ) { e.level = Saturate( e.level + ImGui::GetIO().MouseWheel * 0.05f ); show.dirty = true; }
				const float r  = ( y1 - y0 ) * 0.5f;
				const float xv = x0 + ( x1 - x0 ) * Saturate( e.level );
				dl->AddRectFilled( ImVec2( x0, y0 ), ImVec2( x1, y1 ), IM_COL32( 16, 18, 24, 255 ), r );
				if( e.level > 0.001f ) dl->AddRectFilled( ImVec2( x0, y0 ), ImVec2( std::max( x0 + 2 * r, xv ), y1 ), Col( tc * ( e.on ? 0.85f : 0.4f ) ), r );
				dl->AddCircleFilled( ImVec2( Clamp( xv, x0 + r, x1 - r ), ( y0 + y1 ) * 0.5f ), r + ( lh || la2 ? 2.0f : 1.0f ) * ui, IM_COL32( 235, 238, 245, 255 ) );
				char pv[ 16 ];
				snprintf( pv, sizeof( pv ), "%.0f %%", e.level * 100.0f );
				dl->AddText( ImVec2( x1 + 6 * ui, ( y0 + y1 ) * 0.5f - ImGui::GetTextLineHeight() * 0.5f ), lh || la2 ? IM_COL32( 235, 238, 245, 255 ) : kDim, pv );
				ImGui::SetItemTooltip( "%s", L( "Effekt-Level: Master nur für diesen Effekt\nDoppelklick = voll, Shift = in Schritten, Mausrad" ) );
				OscContext( "/ls/fx/" + std::to_string( e.id ) + "/level" );
			}
			if( DelRow( a.y ) ) del = i;//Delete / Backspace over the effect
			ImGui::SetCursorScreenPos( ImVec2( a.x, a.y + bh + 3 * ui ) );
			ImGui::Dummy( ImVec2( 0, 0 ) );
			ImGui::PopID();
		}
		if( dup >= 0 )
		{
			Effect c = show.effects[ dup ];
			c.id     = show.NextEffectId();
			c.name += L( " Kopie" );
			c.on  = false;
			c.cur = 0;
			show.effects.push_back( c );
			show.fxSelId = c.id;
			show.dirty   = true;
		}
		if( up > 0 ) { std::swap( show.effects[ up ], show.effects[ up - 1 ] ); show.dirty = true; }
		if( del >= 0 )
		{
			const int id = show.effects[ del ].id;
			show.effects.erase( show.effects.begin() + del );
			for( auto& p : show.programs ) p.effects.erase( std::remove( p.effects.begin(), p.effects.end(), id ), p.effects.end() );
			show.dirty = true;
		}
		if( show.effects.empty() ) ImGui::TextDisabled( L( "Links einen Effekttyp anklicken -\nder Effekt startet sofort auf\nden ausgewählten Lampen." ) );
	}
	ImGui::EndChild();
	ImGui::SameLine();
	//---- column C: editor
	ImGui::BeginChild( "fxC", ImVec2( 0, H ), ImGuiChildFlags_Borders );
	if( Effect* e = show.FindEffect( show.fxSelId ) ) EffectEditor( *e );
	else ImGui::TextDisabled( L( "Effekt in der Liste auswählen, um ihn zu bearbeiten." ) );
	ImGui::EndChild();
}

//---------------------------------------------------------------- editor graphics
//value of the effect's curve at cycle position t (0..1) for the scope
static float ScopeValue( const Effect& e, float t )
{
	if( e.type == FxType::Envelope ) return FxEnvValue( e, t * FxCycleLength( e ) );
	const float w = FxEffWave( e, t, (uint32_t)( e.id * 977 + 3 ), (long long)floor( t * 7.0f ) );
	//the graph always shows the full wave 0..100 %; Min / Max (or depth) scale it afterwards
	return Saturate( w );
}
//click into the graph = custom: the shape that was shown becomes editable points
static void WaveToCustom( Effect& e )
{
	if( e.wave == FxWave::Custom ) return;
	e.pts  = FxCurveFromWave( e.wave, e.width );
	e.wave = FxWave::Custom;
}
//envelope: custom curve that starts as the ADSR shape over its whole length
static void EnvToDrawn( Effect& e )
{
	if( e.envDrawn ) return;
	const float len = std::max( 0.05f, FxEnvLength( e ) );
	e.pts.clear();
	const float ts[] = { 0, e.atk, e.atk + e.hold + e.dec, std::max( e.gate, e.atk + e.hold + e.dec ), len };
	for( int k = 0; k < 5; ++k )
	{
		FxPoint q;
		q.t     = Saturate( ts[ k ] / len );
		q.v     = FxEnvValue( e, std::min( ts[ k ], len - 1e-4f ) );
		q.shape = 1;
		if( e.pts.empty() || q.t > e.pts.back().t + 0.002f ) e.pts.push_back( q );
	}
	e.pts.back().v = 0;
	e.gate         = len;
	e.envDrawn     = true;
}
//drawn wave / envelope: points in the area a..b (x = 0..1 of the curve), values shown through lo..hi
//click empty = new point, drag = move, right click = shape (linear / rounded / curve / step) or delete, double click = delete
static const char* kPtShape[ 4 ] = { "Linear", "Abgerundet", "Kurve (Bezier)", "Stufe" };
static bool CurveEditor( ImDrawList* dl, ImVec2 a, ImVec2 b, std::vector< FxPoint >& c, bool periodic, const V3& tc, float ui, float lo, float hi )
{
	bool ch          = false;
	const float span = hi - lo;
	auto X           = [ & ]( float t ) { return a.x + ( b.x - a.x ) * t; };
	auto Y           = [ & ]( float v ) { return b.y - ( b.y - a.y ) * Saturate( lo + span * v ); };
	auto invV        = [ & ]( float y ) {
		const float s = Saturate( ( b.y - y ) / std::max( 1.0f, b.y - a.y ) );
		return fabsf( span ) < 1e-4f ? 0.0f : Saturate( ( s - lo ) / span );
	};
	auto invT     = [ & ]( float x ) { return Saturate( ( x - a.x ) / std::max( 1.0f, b.x - a.x ) ); };
	auto lowerIdx = [ & ]( float t ) { return (int)( std::lower_bound( c.begin(), c.end(), t, []( const FxPoint& q, float tt ) { return q.t < tt; } ) - c.begin() ); };
	ImGui::SetCursorScreenPos( a );
	ImGui::InvisibleButton( "curve", ImVec2( std::max( 1.0f, b.x - a.x ), std::max( 1.0f, b.y - a.y ) ), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight );
	const ImVec2 m           = ImGui::GetIO().MousePos;
	static int drag          = -1;
	static int menuIdx       = -1;
	static const void* owner = nullptr;
	if( owner != &c ) { drag = -1; owner = &c; }
	//nearest point under the mouse
	int hot    = -1;
	float best = 9 * ui;
	for( int k = 0; k < (int)c.size(); ++k )
	{
		const float dx = m.x - X( c[ k ].t ), dy = m.y - Y( c[ k ].v );
		const float d  = sqrtf( dx * dx + dy * dy );
		if( d < best ) { best = d; hot = k; }
	}
	const bool hov = ImGui::IsItemHovered();
	if( hov && hot >= 0 && ImGui::IsMouseDoubleClicked( 0 ) && c.size() > 2 )
	{
		c.erase( c.begin() + hot );
		drag = -1;
		hot  = -1;
		ch   = true;
	}
	else if( ImGui::IsItemActivated() && ImGui::IsMouseClicked( 0 ) )
	{
		if( hot >= 0 ) drag = hot;
		else
		{
			FxPoint p;
			p.t        = invT( m.x );
			p.v        = invV( m.y );
			const int at = lowerIdx( p.t );
			p.shape    = c.empty() ? 2 : c[ std::max( 0, at - 1 ) ].shape;
			c.insert( c.begin() + at, p );
			drag = at;
			ch   = true;
		}
	}
	if( drag >= 0 && drag < (int)c.size() && ImGui::IsItemActive() && ImGui::IsMouseDown( 0 ) )
	{
		const float tLo = drag > 0 ? c[ drag - 1 ].t + 0.002f : 0.0f;
		const float tHi = drag + 1 < (int)c.size() ? c[ drag + 1 ].t - 0.002f : ( periodic ? 0.998f : 1.0f );
		const float nt  = Clamp( invT( m.x ), tLo, std::max( tLo, tHi ) );
		const float nv  = ImGui::GetIO().KeyShift ? roundf( invV( m.y ) * 8 ) / 8 : invV( m.y );
		if( fabsf( nt - c[ drag ].t ) > 1e-5f || fabsf( nv - c[ drag ].v ) > 1e-5f ) { c[ drag ].t = nt; c[ drag ].v = nv; ch = true; }
	}
	if( !ImGui::IsMouseDown( 0 ) ) drag = -1;
	if( hov && hot >= 0 && ImGui::IsMouseClicked( 1 ) )
	{
		menuIdx = hot;
		ImGui::OpenPopup( "ptmenu" );
	}
	if( ImGui::BeginPopup( "ptmenu" ) )
	{
		if( menuIdx >= 0 && menuIdx < (int)c.size() )
		{
			ImGui::TextDisabled( L( "Verlauf ab diesem Punkt" ) );
			for( int s = 0; s < 4; ++s )
				if( ImGui::MenuItem( Ls( kPtShape[ s ] ), nullptr, c[ menuIdx ].shape == s ) ) { c[ menuIdx ].shape = s; ch = true; }
			if( ImGui::MenuItem( L( "Alle Punkte so" ) ) )
			{
				const int sh = c[ menuIdx ].shape;
				for( auto& q : c ) q.shape = sh;
				ch = true;
			}
			ImGui::Separator();
			if( ImGui::MenuItem( L( "Punkt löschen" ), nullptr, false, c.size() > 2 ) ) { c.erase( c.begin() + menuIdx ); menuIdx = -1; ch = true; }
		}
		ImGui::EndPopup();
	}
	//points: square = linear, ring = rounded, dot = curve, bar = step
	for( int k = 0; k < (int)c.size(); ++k )
	{
		const ImVec2 p( X( c[ k ].t ), Y( c[ k ].v ) );
		const float r   = ( k == hot || k == drag ? 6.0f : 4.5f ) * ui;
		const ImU32 col = Col( tc );
		switch( c[ k ].shape )
		{
		case 0:
			dl->AddRectFilled( ImVec2( p.x - r, p.y - r ), ImVec2( p.x + r, p.y + r ), kBg );
			dl->AddRect( ImVec2( p.x - r, p.y - r ), ImVec2( p.x + r, p.y + r ), col, 0, 0, 2 * ui );
			break;
		case 1:
			dl->AddCircleFilled( p, r, kBg );
			dl->AddCircle( p, r, col, 0, 2 * ui );
			break;
		case 3: dl->AddRectFilled( ImVec2( p.x - r, p.y - r * 0.45f ), ImVec2( p.x + r, p.y + r * 0.45f ), col ); break;
		default: dl->AddCircleFilled( p, r, col ); break;
		}
	}
	if( hov && hot >= 0 && drag < 0 )
		ImGui::SetTooltip( L( "%.0f %%  %s\nRechtsklick: Verlauf / löschen, Doppelklick: löschen" ), c[ hot ].v * 100.0f, Ls( kPtShape[ Clamp( c[ hot ].shape, 0, 3 ) ] ) );
	else if( hov && hot < 0 && drag < 0 )
		ImGui::SetTooltip( "%s", L( "Klick: Punkt setzen, ziehen: verschieben, Shift: Raster" ) );
	return ch;
}
//grid of beats over one cycle
static void BeatGrid( ImDrawList* dl, ImVec2 a, ImVec2 b, const Effect& e, int bpb )
{
	const float len = e.sync ? FxCycleLength( e ) : 0;
	if( len > 0 && len <= 64 )
	{
		const int n = std::max( 1, std::min( 64, (int)lroundf( len * ( len <= 1.01f ? 4 : 1 ) ) ) );//sub-beats for short cycles
		for( int i = 0; i <= n; ++i )
		{
			const float x  = a.x + ( b.x - a.x ) * i / n;
			const bool bar = len > 1.01f ? ( i % std::max( 1, bpb ) == 0 ) : ( i % 4 == 0 );
			dl->AddLine( ImVec2( x, a.y ), ImVec2( x, b.y ), bar ? kGridHi : kGrid );
		}
	}
	for( int i = 1; i < 4; ++i )
	{
		const float y = a.y + ( b.y - a.y ) * i / 4;
		dl->AddLine( ImVec2( a.x, y ), ImVec2( b.x, y ), i == 2 ? kGrid : IM_COL32( 30, 34, 43, 255 ) );
	}
}

void App::EffectEditor( Effect& e )
{
	ls::rs::Salt rsSalt( 0x51ED27ull + (uint64_t)e.id );
	bool ch                = false;
	const std::string oscB = "/ls/fx/" + std::to_string( e.id );
	const V3 tc            = FxTypeColor( e.type );
	const bool mod         = e.kind == FxKind::Modulator;
	ImDrawList* dl         = ImGui::GetWindowDrawList();
	//---- header: type badge, name, run, trigger, presets
	{
		const ImVec2 a = ImGui::GetCursorScreenPos();
		const char* tn = Ls( FxTypeName( e.type ) );
		const float tw = ImGui::CalcTextSize( tn ).x + 16 * ui;
		const float fh = ImGui::GetFrameHeight();
		dl->AddRectFilled( a, ImVec2( a.x + tw, a.y + fh ), Col( tc * 0.35f ), 4 * ui );
		dl->AddRect( a, ImVec2( a.x + tw, a.y + fh ), Col( tc ), 4 * ui );
		dl->AddText( ImVec2( a.x + 8 * ui, a.y + ( fh - ImGui::GetTextLineHeight() ) * 0.5f ), Col( tc ), tn );
		ImGui::Dummy( ImVec2( tw, fh ) );
		ImGui::SameLine();
		char nm[ 96 ];
		snprintf( nm, sizeof( nm ), "%s", e.name.c_str() );
		ImGui::SetNextItemWidth( 200 * ui );
		if( ImGui::InputText( "##nm", nm, sizeof( nm ) ) ) { e.name = nm; ch = true; }
		ImGui::SameLine();
		ImGui::PushStyleColor( ImGuiCol_Button, e.on ? ImVec4( tc.x * 0.5f, tc.y * 0.5f, tc.z * 0.5f, 1 ) : ImVec4( 0.25f, 0.27f, 0.33f, 1 ) );
		if( ImGui::Button( e.on ? L( "Läuft - stoppen" ) : L( "Starten" ), ImVec2( 120 * ui, 0 ) ) ) e.on = !e.on;
		ImGui::PopStyleColor();
		OscContext( oscB + "/on" );
		ImGui::SameLine();
		if( ImGui::Button( L( "Neu starten" ) ) ) { e.syncBeat = 0; e.freePhase = 0; e.clock = 0; }
		OscContext( oscB + "/restart" );
		if( e.type == FxType::Envelope )
		{
			ImGui::SameLine();
			ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.55f, 0.28f, 0.16f, 1 ) );
			if( ImGui::Button( L( "Auslösen" ) ) ) { e.trigBeat = e.syncBeat; e.trigTime = e.clock; if( !e.on ) e.on = true; }
			ImGui::PopStyleColor();
			OscContext( oscB + "/trigger" );
		}
		ImGui::SameLine();
		if( ImGui::Button( L( "Presets" ) ) ) ImGui::OpenPopup( "fxpre" );
		if( ImGui::BeginPopup( "fxpre" ) )
		{
			auto load = [ & ]( const FxPreset& p ) {
				Effect n     = p.fx;
				n.id         = e.id;
				n.on         = e.on;
				n.cur        = e.cur;
				n.fixtures   = e.fixtures;
				n.group      = e.group;
				n.syncBeat   = e.syncBeat;
				n.lastTempo  = e.lastTempo;
				n.clockInit  = e.clockInit;
				n.freePhase  = e.freePhase;
				n.name       = Ls( p.name );
				n.level      = e.level;//the effect's own master stays
				if( !p.user )
				{
					n.lo   = e.lo;//keep the chosen range (max 50 % ...)
					n.hi   = e.hi;
					n.type = FxTypeGuess( n );
				}
				e  = n;
				ch = true;
			};
			ImGui::TextDisabled( L( "Einstellungen laden (Lampen bleiben)" ) );
			std::string grp;
			for( auto& p : BuiltinFxPresets() )
			{
				if( p.fx.kind != e.kind ) continue;
				if( p.group != grp ) { grp = p.group; ImGui::SeparatorText( Ls( grp ) ); }
				ImGui::PushID( &p );
				if( ImGui::MenuItem( Ls( p.name ) ) ) load( p );
				ImGui::PopID();
			}
			if( !userPresets_.empty() )
			{
				ImGui::SeparatorText( L( "Eigene Presets" ) );
				std::string delName;
				for( auto& p : userPresets_ )
				{
					if( p.fx.kind != e.kind || ( e.kind == FxKind::Modulator && p.fx.type != e.type ) ) continue;
					ImGui::PushID( &p );
					if( ImGui::MenuItem( p.name.c_str(), Ls( FxTypeName( p.fx.type ) ) ) ) load( p );
					if( ImGui::BeginPopupContextItem( "up" ) )
					{
						if( ImGui::MenuItem( L( "Preset löschen" ) ) ) delName = p.name;
						ImGui::EndPopup();
					}
					ImGui::PopID();
				}
				if( !delName.empty() ) { DeleteUserFxPreset( UserFxFile(), delName ); userPresetsLoaded_ = false; }
			}
			ImGui::Separator();
			ImGui::SetNextItemWidth( 180 * ui );
			ImGui::InputTextWithHint( "##pn", L( "Preset-Name" ), presetName_, sizeof( presetName_ ) );
			ImGui::SameLine();
			if( ImGui::Button( L( "Als Preset speichern" ) ) )
			{
				std::string n = presetName_[ 0 ] ? presetName_ : e.name;
				CreateDirectoryW( Widen( exeDir + "\\library\\user" ).c_str(), nullptr );
				if( SaveUserFxPreset( UserFxFile(), n, e ) ) { SetStatus( L( "Preset gespeichert: " ) + n ); userPresetsLoaded_ = false; }
				else SetStatus( L( "Preset konnte nicht gespeichert werden" ) );
			}
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		ImGui::TextDisabled( L( "Nr. %d  OSC %s" ), e.id, oscB.c_str() );
	}
	//---- display (GIAMO style): scope / colour band / XY pad / step grid / envelope
	std::vector< FxPreviewEl > pv;
	PreviewEffect( show, e, pv );
	const float availH = ImGui::GetContentRegionAvail().y;
	const float leftW  = Clamp( ImGui::GetContentRegionAvail().x * ( e.type == FxType::Sequencer ? 0.62f : 0.5f ), 320 * ui, 1100 * ui );
	ImGui::BeginChild( "fxdisp", ImVec2( leftW, availH ), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );
	dl                = ImGui::GetWindowDrawList();
	const float dispH = std::max( 120 * ui, ImGui::GetContentRegionAvail().y - 2 * ui );
	{
		const ImVec2 a0 = ImGui::GetCursorScreenPos();
		const float aw  = ImGui::GetContentRegionAvail().x;
		const ImVec2 b0( a0.x + aw, a0.y + dispH );
		dl->AddRectFilled( a0, b0, kBg, 6 * ui );
		const double phase = FxBasePhase( e );
		const float ph     = (float)( phase - floor( phase ) );
		const int bpb      = std::max( 1, show.tempo.beatsPerBar );
		const float stripH = e.type == FxType::Sequencer ? 0 : Clamp( dispH * 0.24f, 36 * ui, 120 * ui );//element strip under the graph
		const ImVec2 a( a0.x + 8 * ui, a0.y + 8 * ui ), b( b0.x - 8 * ui, b0.y - stripH - ( stripH > 0 ? 28 : 20 ) * ui );
		if( e.type == FxType::Sequencer )
		{
			//---- step grid: one row per lane
			const float hdrW = 160 * ui;
			const ImVec2 ga( a.x + hdrW, a.y ), gb( b0.x - 8 * ui, b.y );
			const int nl     = std::max( 1, (int)e.lanes.size() );
			const float lh   = ( gb.y - ga.y ) / nl;
			const int ns     = std::max( 1, e.steps );
			const float sw   = ( gb.x - ga.x ) / ns;
			const int curStep = FxSeqStep( e );
			for( int s = 0; s < ns; ++s )
			{
				const float x = ga.x + s * sw;
				if( s % 4 == 0 ) dl->AddRectFilled( ImVec2( x, ga.y ), ImVec2( x + sw * std::min( 4, ns - s ), gb.y ), ( s / 4 ) % 2 ? IM_COL32( 22, 25, 32, 255 ) : IM_COL32( 26, 29, 37, 255 ) );
			}
			if( e.on ) dl->AddRectFilled( ImVec2( ga.x + curStep * sw, ga.y ), ImVec2( ga.x + ( curStep + 1 ) * sw, gb.y ), IM_COL32( 255, 255, 255, 18 ) );
			ImGui::SetCursorScreenPos( ga );
			ImGui::InvisibleButton( "grid", ImVec2( gb.x - ga.x, gb.y - ga.y ), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight );
			const bool active = ImGui::IsItemActive();
			const ImVec2 m    = ImGui::GetIO().MousePos;
			if( active && !e.lanes.empty() )
			{
				static int dragLane = -1;
				if( ImGui::IsMouseClicked( 0 ) || ImGui::IsMouseClicked( 1 ) ) dragLane = Clamp( (int)( ( m.y - ga.y ) / lh ), 0, nl - 1 );
				const int li = Clamp( dragLane, 0, (int)e.lanes.size() - 1 );
				const int s  = Clamp( (int)( ( m.x - ga.x ) / sw ), 0, ns - 1 );
				float v      = Saturate( 1.0f - ( m.y - ( ga.y + li * lh + 3 * ui ) ) / std::max( 1.0f, lh - 6 * ui ) );
				if( ImGui::GetIO().KeyShift ) v = roundf( v * 4 ) / 4;
				if( ImGui::IsMouseDown( 1 ) ) v = 0;
				if( (int)e.lanes[ li ].v.size() == ns && fabsf( e.lanes[ li ].v[ s ] - v ) > 1e-4f ) { e.lanes[ li ].v[ s ] = v; ch = true; }
			}
			for( int li = 0; li < (int)e.lanes.size(); ++li )
			{
				const FxLane& l = e.lanes[ li ];
				const V3 lc     = kLaneCol[ li % 8 ];
				const float y0 = ga.y + li * lh, y1 = y0 + lh;
				if( li ) dl->AddLine( ImVec2( a.x, y0 ), ImVec2( gb.x, y0 ), kGridHi );
				for( int s = 0; s < ns && s < (int)l.v.size(); ++s )
				{
					const float x0 = ga.x + s * sw + 2 * ui, x1 = ga.x + ( s + 1 ) * sw - 2 * ui;
					const float v  = l.v[ s ];
					const bool now = e.on && s == curStep;
					dl->AddRectFilled( ImVec2( x0, y0 + 3 * ui ), ImVec2( x1, y1 - 3 * ui ), IM_COL32( 34, 38, 48, 255 ), 2 * ui );
					if( v > 0.001f )
					{
						const float yt = y1 - 3 * ui - ( lh - 6 * ui ) * v;
						dl->AddRectFilled( ImVec2( x0, yt ), ImVec2( x1, y1 - 3 * ui ), Col( lc * ( l.mute ? 0.35f : ( now ? 1.15f : 0.8f ) ) ), 2 * ui );
						dl->AddLine( ImVec2( x0, yt ), ImVec2( x1, yt ), Col( l.mute ? lc * 0.5f : V3( 1, 1, 1 ) * 0.92f ), 1.5f * ui );
					}
				}
				//lane header
				dl->AddRectFilled( ImVec2( a.x, y0 + 3 * ui ), ImVec2( a.x + 4 * ui, y1 - 3 * ui ), Col( lc ), 2 * ui );
				char t[ 16 ];
				snprintf( t, sizeof( t ), "%d", li + 1 );
				dl->AddText( ImVec2( a.x + 10 * ui, y0 + 4 * ui ), Col( lc ), t );
				const std::string tl = FxTargetLabel( show, e, l.tgt );
				dl->PushClipRect( ImVec2( a.x + 24 * ui, y0 ), ImVec2( ga.x - 56 * ui, y1 ), true );
				dl->AddText( ImVec2( a.x + 24 * ui, y0 + 4 * ui ), l.mute ? kDim : IM_COL32( 220, 224, 232, 255 ), tl.c_str() );
				dl->PopClipRect();
				if( lh > 20 * ui )
				{
					ImGui::PushID( li );
					ImGui::SetCursorScreenPos( ImVec2( ga.x - 54 * ui, y0 + 3 * ui ) );
					FxLane& lw = e.lanes[ li ];
					ImGui::PushStyleColor( ImGuiCol_Button, lw.mute ? ImVec4( 0.55f, 0.2f, 0.2f, 1 ) : ImVec4( 0.2f, 0.22f, 0.27f, 1 ) );
					if( ImGui::SmallButton( "M" ) ) { lw.mute = !lw.mute; ch = true; }
					ImGui::PopStyleColor();
					ImGui::SetItemTooltip( L( "Spur stumm" ) );
					ImGui::SameLine();
					ImGui::PushStyleColor( ImGuiCol_Button, lw.glide ? ImVec4( lc.x * 0.5f, lc.y * 0.5f, lc.z * 0.5f, 1 ) : ImVec4( 0.2f, 0.22f, 0.27f, 1 ) );
					if( ImGui::SmallButton( "G" ) ) { lw.glide = !lw.glide; ch = true; }
					ImGui::PopStyleColor();
					ImGui::SetItemTooltip( L( "Gleiten: weich zum nächsten Schritt" ) );
					ImGui::PopID();
				}
			}
			for( int s = 0; s < ns; s += ( ns > 16 ? 4 : 1 ) )
			{
				char t[ 8 ];
				snprintf( t, sizeof( t ), "%d", s + 1 );
				dl->AddText( ImVec2( ga.x + s * sw + 3 * ui, gb.y + 2 * ui ), s == curStep && e.on ? Col( tc ) : kDim, t );
			}
			ImGui::SetCursorScreenPos( ImVec2( a0.x, b0.y ) );
			ImGui::Dummy( ImVec2( aw, 4 * ui ) );
		}
		else
		{
			BeatGrid( dl, a, b, e, bpb );
			if( e.kind == FxKind::Color )
			{
				//colour band over one cycle
				const int segs = 96;
				for( int i = 0; i < segs; ++i )
				{
					const float t0 = (float)i / segs, t1 = (float)( i + 1 ) / segs;
					V3 c0, c1;
					{
						//palette colour as the engine computes it
						auto pal = [ & ]( float t ) -> V3 {
							if( e.colorMode == FxColorMode::Rainbow || e.colors.empty() ) return Hsv( t, 1, 1 );
							const int n = (int)e.colors.size();
							if( e.colorMode == FxColorMode::Steps ) return e.colors[ Clamp( (int)( t * n ), 0, n - 1 ) ];
							const float xx = t * n;
							const int k    = Clamp( (int)xx, 0, n - 1 );
							const V3& p0   = e.colors[ k ];
							const V3& p1   = e.colors[ ( k + 1 ) % n ];
							return p0 + ( p1 - p0 ) * Smooth( xx - k );
						};
						c0 = pal( t0 );
						c1 = pal( t1 );
					}
					const float x0 = a.x + ( b.x - a.x ) * t0, x1 = a.x + ( b.x - a.x ) * t1;
					dl->AddRectFilledMultiColor( ImVec2( x0, a.y + ( b.y - a.y ) * 0.2f ), ImVec2( x1 + 0.5f, b.y - ( b.y - a.y ) * 0.2f ), Col( c0 ), Col( c1 ), Col( c1 ), Col( c0 ) );
				}
			}
			else if( e.kind == FxKind::Position )
			{
				//XY pad with the shape path
				const float sz = std::min( b.x - a.x, b.y - a.y );
				const ImVec2 c( ( a.x + b.x ) * 0.5f, ( a.y + b.y ) * 0.5f );
				const float sx = sz * 0.42f * Clamp( e.sizePan / 180.0f, 0.15f, 1.0f ), sy = sz * 0.42f * Clamp( e.sizeTilt / 90.0f, 0.15f, 1.0f );
				dl->AddLine( ImVec2( c.x - sz * 0.48f, c.y ), ImVec2( c.x + sz * 0.48f, c.y ), kGridHi );
				dl->AddLine( ImVec2( c.x, c.y - sz * 0.48f ), ImVec2( c.x, c.y + sz * 0.48f ), kGridHi );
				if( e.shape != FxShape::Random )
				{
					ImVec2 pts[ 129 ];
					for( int i = 0; i <= 128; ++i )
					{
						const float t = i / 128.0f, an = 2 * kPi * t;
						float ox = 0, oy = 0;
						switch( e.shape )
						{
						case FxShape::Circle: ox = sinf( an ); oy = cosf( an ); break;
						case FxShape::Eight: ox = sinf( an ); oy = sinf( 2 * an ); break;
						case FxShape::PanSwing: ox = sinf( an ); break;
						case FxShape::TiltSwing: oy = sinf( an ); break;
						case FxShape::Triangle:
						case FxShape::Square:
						{
							static const float tri[ 3 ][ 2 ] = { { 0, 1 }, { 0.866f, -0.5f }, { -0.866f, -0.5f } };
							static const float sq[ 4 ][ 2 ]  = { { -1, 1 }, { 1, 1 }, { 1, -1 }, { -1, -1 } };
							const int n = e.shape == FxShape::Triangle ? 3 : 4;
							const float( *p )[ 2 ] = e.shape == FxShape::Triangle ? tri : sq;
							const float xx = t * n;
							const int k    = Clamp( (int)xx, 0, n - 1 );
							ox = Lerp( p[ k ][ 0 ], p[ ( k + 1 ) % n ][ 0 ], xx - k );
							oy = Lerp( p[ k ][ 1 ], p[ ( k + 1 ) % n ][ 1 ], xx - k );
							break;
						}
						default: break;
						}
						pts[ i ] = ImVec2( c.x + ox * sx, c.y - oy * sy );
					}
					dl->AddPolyline( pts, 129, Col( tc, 0.6f ), 0, 2 * ui );
				}
				for( size_t i = 0; i < pv.size() && i < 64; ++i )
					dl->AddCircleFilled( ImVec2( c.x + pv[ i ].ox * sx, c.y - pv[ i ].oy * sy ), 4.5f * ui, Col( Hsv( (float)i / std::max< size_t >( 1, pv.size() ), 0.45f, 1 ) ) );
			}
			else if( e.type == FxType::Envelope )
			{
				//ADSR with handles; x = one cycle (or the envelope length when triggered by hand)
				const float len = e.manualTrig ? std::max( FxEnvLength( e ) * 1.15f, 0.25f ) : FxCycleLength( e );
				auto X          = [ & ]( float tt ) { return a.x + ( b.x - a.x ) * Saturate( tt / len ); };
				auto Y          = [ & ]( float v ) { return b.y - ( b.y - a.y ) * Saturate( v ); };
				ImVec2 pts[ 161 ];
				for( int i = 0; i <= 160; ++i )
				{
					const float tt = len * i / 160.0f;
					pts[ i ]       = ImVec2( X( tt ), Y( FxEnvValue( e, tt ) ) );
				}
				for( int i = 0; i < 160; ++i )
					dl->AddQuadFilled( ImVec2( pts[ i ].x, b.y ), pts[ i ], pts[ i + 1 ], ImVec2( pts[ i + 1 ].x, b.y ), Col( tc, 0.16f ) );
				dl->AddPolyline( pts, 161, Col( tc ), 0, 2.2f * ui );
				if( !e.envDrawn && ImGui::IsMouseClicked( 0 ) && ImGui::IsMouseHoveringRect( a, b ) && ImGui::IsWindowHovered() )
				{
					//click into the graph (not on a handle) = custom curve, the click already sets / grabs a point
					const float tA = e.atk, tD = e.atk + e.hold + e.dec, tG = std::max( e.gate, tD ), tR = tG + e.rel;
					const ImVec2 hq[ 4 ] = { ImVec2( X( tA ), Y( 1 ) ), ImVec2( X( tD ), Y( e.sus ) ), ImVec2( X( tG ), Y( e.sus ) ), ImVec2( X( tR ), Y( 0 ) ) };
					const ImVec2 mq      = ImGui::GetIO().MousePos;
					bool nearH           = false;
					for( int k = 0; k < 4; ++k )
						if( ( mq.x - hq[ k ].x ) * ( mq.x - hq[ k ].x ) + ( mq.y - hq[ k ].y ) * ( mq.y - hq[ k ].y ) < 18 * ui * 18 * ui ) nearH = true;
					if( !nearH ) { EnvToDrawn( e ); ch = true; }
				}
				if( e.envDrawn )
				{
					//drawn: the curve spans the gate length (x = 0 .. gate)
					ImGui::PushID( "envc" );
					ch |= CurveEditor( dl, a, ImVec2( X( std::max( 0.01f, e.gate ) ), b.y ), e.pts, false, tc, ui, 0.0f, 1.0f );
					ImGui::PopID();
				}
				else
				{
				//handles: attack peak, decay end (sustain), gate end, release end
				const float tA = e.atk, tD = e.atk + e.hold + e.dec, tG = std::max( e.gate, tD ), tR = tG + e.rel;
				const ImVec2 hp[ 4 ] = { ImVec2( X( tA ), Y( 1 ) ), ImVec2( X( tD ), Y( e.sus ) ), ImVec2( X( tG ), Y( e.sus ) ), ImVec2( X( tR ), Y( 0 ) ) };
				static const char* hl[ 4 ] = { "A", "D", "S", "R" };
				ImGui::SetCursorScreenPos( a );
				ImGui::InvisibleButton( "env", ImVec2( b.x - a.x, b.y - a.y ) );
				static int dragH = -1;
				const ImVec2 m   = ImGui::GetIO().MousePos;
				if( ImGui::IsItemActivated() )
				{
					dragH      = -1;
					float best = 18 * ui;
					for( int k = 0; k < 4; ++k )
					{
						const float d = sqrtf( ( m.x - hp[ k ].x ) * ( m.x - hp[ k ].x ) + ( m.y - hp[ k ].y ) * ( m.y - hp[ k ].y ) );
						if( d < best ) { best = d; dragH = k; }
					}
				}
				if( ImGui::IsItemActive() && dragH >= 0 )
				{
					const float tt = Saturate( ( m.x - a.x ) / std::max( 1.0f, b.x - a.x ) ) * len;
					const float v  = Saturate( ( b.y - m.y ) / std::max( 1.0f, b.y - a.y ) );
					if( dragH == 0 ) e.atk = std::max( 0.0f, tt );
					else if( dragH == 1 ) { e.dec = std::max( 0.0f, tt - e.atk - e.hold ); e.sus = v; }
					else if( dragH == 2 ) { e.gate = std::max( e.atk + e.hold + e.dec, tt ); e.sus = v; }
					else e.rel = std::max( 0.0f, tt - tG );
					ch = true;
				}
				for( int k = 0; k < 4; ++k )
				{
					dl->AddCircleFilled( hp[ k ], 5 * ui, kBg );
					dl->AddCircle( hp[ k ], 5 * ui, Col( tc ), 0, 2 * ui );
					dl->AddText( ImVec2( hp[ k ].x + 6 * ui, hp[ k ].y - 16 * ui ), kDim, hl[ k ] );
				}
				}
				//where each element is in its envelope
				if( e.on )
				{
					float tt = (float)( e.manualTrig ? ( e.sync ? e.syncBeat - e.trigBeat : e.clock - e.trigTime ) : ph * len );
					if( tt >= 0 && tt <= len ) dl->AddLine( ImVec2( X( tt ), a.y ), ImVec2( X( tt ), b.y ), IM_COL32( 255, 255, 255, 120 ), 1.5f * ui );
				}
			}
			else
			{
				//scope: one cycle of the wave, filled
				ImVec2 pts[ 241 ];
				for( int i = 0; i <= 240; ++i )
				{
					const float t = i / 240.0f;
					pts[ i ]      = ImVec2( a.x + ( b.x - a.x ) * t, b.y - ( b.y - a.y ) * Saturate( ScopeValue( e, t ) ) );
				}
				for( int i = 0; i < 240; ++i )
					dl->AddQuadFilled( ImVec2( pts[ i ].x, b.y ), pts[ i ], pts[ i + 1 ], ImVec2( pts[ i + 1 ].x, b.y ), Col( tc, 0.14f ) );
				dl->AddPolyline( pts, 241, Col( tc ), 0, 2.2f * ui );
				if( e.wave != FxWave::Custom && ImGui::IsMouseClicked( 0 ) && ImGui::IsMouseHoveringRect( a, b ) && ImGui::IsWindowHovered() )
				{
					//click into the graph = custom, the click already sets / grabs a point
					WaveToCustom( e );
					ch = true;
				}
				if( e.wave == FxWave::Custom )
				{
					const float clo = 0.0f, chi = 1.0f;//full range, independent of Min / Max
					ImGui::PushID( "wavec" );
					ch |= CurveEditor( dl, a, b, e.pts, true, tc, ui, clo, chi );
					ImGui::PopID();
				}
				if( e.kind == FxKind::Modulator && e.relative )
				{
					const float ym = ( a.y + b.y ) * 0.5f;
					dl->AddLine( ImVec2( a.x, ym ), ImVec2( b.x, ym ), IM_COL32( 255, 255, 255, 50 ), 1 );
					dl->AddText( ImVec2( a.x + 4 * ui, ym - ImGui::GetTextLineHeight() - 1 ), kDim, L( "Wert darunter" ) );
				}
			}
			//playhead
			if( e.on && e.type != FxType::Envelope && e.kind != FxKind::Position )
			{
				const float x = a.x + ( b.x - a.x ) * ph;
				dl->AddLine( ImVec2( x, a.y ), ImVec2( x, b.y ), IM_COL32( 255, 255, 255, 140 ), 1.5f * ui );
				if( e.kind != FxKind::Color ) dl->AddCircleFilled( ImVec2( x, b.y - ( b.y - a.y ) * Saturate( ScopeValue( e, ph ) ) ), 4 * ui, IM_COL32( 255, 255, 255, 230 ) );
			}
			//cycle label
			char lb[ 64 ];
			if( e.sync ) snprintf( lb, sizeof( lb ), L( "Zyklus %s" ), BeatLabel( FxCycleLength( e ) ).c_str() );
			else snprintf( lb, sizeof( lb ), L( "Zyklus %.2f s" ), FxCycleLength( e ) );
			dl->AddText( ImVec2( a.x, b.y + 2 * ui ), kDim, lb );
			//element strip: every lamp / pixel / channel now
			const ImVec2 sa( a.x, b.y + 22 * ui ), sb( b0.x - 8 * ui, b0.y - 6 * ui );
			dl->AddRectFilled( sa, sb, IM_COL32( 13, 15, 20, 255 ), 4 * ui );
			const int n = (int)pv.size();
			if( n > 0 )
			{
				const int cols  = std::max( 1, (int)ceilf( sqrtf( (float)n * ( sb.x - sa.x ) / std::max( 1.0f, sb.y - sa.y ) ) ) );
				const int rows  = ( n + cols - 1 ) / cols;
				const float cw = ( sb.x - sa.x - 6 * ui ) / cols, chh = std::min( cw, ( sb.y - sa.y - 6 * ui ) / rows );
				for( int i = 0; i < n; ++i )
				{
					const ImVec2 ca( sa.x + 3 * ui + ( i % cols ) * cw, sa.y + 3 * ui + ( i / cols ) * chh );
					const float k  = e.on ? Saturate( e.Drive() ) : 0.25f;
					V3 c           = e.kind == FxKind::Color ? pv[ i ].col : ( e.kind == FxKind::Position ? V3( 0.4f, 0.7f, 1.0f ) : tc * ( 0.1f + 0.9f * Saturate( pv[ i ].v ) ) );
					dl->AddRectFilled( ImVec2( ca.x + 1, ca.y + 1 ), ImVec2( ca.x + cw - 2, ca.y + chh - 2 ), Col( c * ( 0.25f + 0.75f * k ) ), 3 * ui );
				}
				char t[ 48 ];
				snprintf( t, sizeof( t ), mod && e.tgt.mode == 2 ? L( "%d Kanäle" ) : L( "%d Elemente" ), n );
				dl->AddText( ImVec2( sb.x - ImGui::CalcTextSize( t ).x, b.y + 2 * ui ), kDim, t );
			}
			else
				dl->AddText( ImVec2( sa.x + 6 * ui, sa.y + 6 * ui ), kDim, L( "Keine passenden\nLampen" ) );
			ImGui::SetCursorScreenPos( ImVec2( a0.x, b0.y ) );
			ImGui::Dummy( ImVec2( aw, 1 ) );
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();
	//---- settings (right of the graph)
	ImGui::BeginChild( "fxset", ImVec2( 0, availH ), ImGuiChildFlags_None );
	const int setCols = ImGui::GetContentRegionAvail().x > 760 * ui ? 2 : 1;//narrow: one column, scrolls
	if( ImGui::BeginTable( "fxed", setCols, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV ) )
	{
		ImGui::TableNextColumn();
		const float lw = Clamp( ImGui::GetContentRegionAvail().x * 0.38f, 90 * ui, 150 * ui );
		auto row       = [ & ]( const char* l ) {
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted( l );
			ImGui::SameLine( lw );
			ImGui::SetNextItemWidth( -1 );
		};
		auto waveCombo = [ & ]() {
			int w = (int)e.wave;
			row( L( "Welle" ) );
			if( ImGui::Combo( "##wave", &w, L( "Sinus\0Dreieck\0Sägezahn auf\0Sägezahn ab\0Rechteck\0Puls\0Zufall\0Zufall weich\0Eigene\0" ) ) )
			{
				//drawn: start from the wave that was there
				if( (FxWave)w == FxWave::Custom && e.pts.size() < 2 ) e.pts = FxCurveFromWave( e.wave, e.width );
				e.wave = (FxWave)w;
				ch     = true;
			}
			if( e.wave != FxWave::Custom ) ImGui::TextDisabled( L( "Klick in die Grafik: eigene Kurve zeichnen" ) );
			if( e.wave == FxWave::Custom )
			{
				ImGui::TextDisabled( L( "Links in der Grafik: Klick = Punkt, ziehen, Rechtsklick = Verlauf" ) );
				if( ImGui::SmallButton( L( "Kurve zurücksetzen" ) ) ) { e.pts = FxCurveFromWave( FxWave::Sine, 0.5f ); ch = true; }
			}
			OscContext( oscB + "/wave" );
		};
		auto rangeRow = [ & ]( const char* l ) {
			row( l );
			ch |= PctRange( "##range", &e.lo, &e.hi );
			OscContext( oscB + "/lo" );
		};
		//==== column 1: the shape (presets: menu in the header)
		ImGui::SeparatorText( L( "Form" ) );
		switch( e.type )
		{
		case FxType::Chase:
		case FxType::Wave:
		case FxType::Strobe:
		case FxType::Sparkle:
		case FxType::Intensity:
			waveCombo();
			if( e.wave == FxWave::Square || e.wave == FxWave::Pulse )
			{
				row( e.type == FxType::Strobe ? L( "Blitzlänge" ) : L( "Breite" ) );
				ch |= PctSlider( "##width", &e.width, 0.02f, 1.0f );
				OscContext( oscB + "/width" );
			}
			rangeRow( L( "Helligkeit min/max" ) );
			ch |= ImGui::Checkbox( L( "Shutter öffnen" ), &e.openShutter );
			OscContext( oscB + "/openshutter" );
			ImGui::SetItemTooltip( L( "Lampen mit Shutter-Kanal: Shutter offen, solange der Effekt läuft" ) );
			ImGui::SameLine();
			ch |= ImGui::Checkbox( L( "Skalieren" ), &e.scale );
			OscContext( oscB + "/scale" );
			ImGui::SetItemTooltip( L( "Aus: der Effekt ersetzt den Dimmer.\nAn: der Effekt läuft innerhalb des Werts darunter (Programmer, Gruppenfader, Snapshot) -\nbei 50 %% Grundhelligkeit pendelt er zwischen 0 und 50 %%." ) );
			break;
		case FxType::Colour:
		case FxType::ColorChange:
		case FxType::ColorFade:
		case FxType::Rainbow:
		{
			int cm = (int)e.colorMode;
			row( L( "Farben" ) );
			if( ImGui::Combo( "##cm", &cm, L( "Regenbogen\0Verlauf (Palette)\0Stufen (Palette)\0" ) ) ) { e.colorMode = (FxColorMode)cm; ch = true; }
			OscContext( oscB + "/colormode" );
			if( e.colorMode != FxColorMode::Rainbow )
			{
				int delC = -1;
				for( int i = 0; i < (int)e.colors.size(); ++i )
				{
					ImGui::PushID( i );
					if( i % 8 ) ImGui::SameLine();
					ch |= ImGui::ColorEdit3( "##c", &e.colors[ i ].x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel );
					OscContext( oscB + "/color/" + std::to_string( i + 1 ) + "/hue" );
					if( ImGui::BeginPopupContextItem( "cc" ) )
					{
						if( ImGui::MenuItem( L( "Farbe entfernen" ), nullptr, false, e.colors.size() > 1 ) ) delC = i;
						ImGui::EndPopup();
					}
					ImGui::PopID();
				}
				ImGui::SameLine();
				if( e.colors.size() < 12 && ImGui::SmallButton( "+" ) ) { e.colors.push_back( Hsv( e.colors.size() * 0.17f, 1, 1 ) ); ch = true; }
				if( delC >= 0 ) { e.colors.erase( e.colors.begin() + delC ); ch = true; }
				ImGui::TextDisabled( L( "Rechtsklick auf eine Farbe = entfernen" ) );
			}
			break;
		}
		case FxType::Movement:
		{
			int sh = (int)e.shape;
			row( L( "Form" ) );
			if( ImGui::Combo( "##shape", &sh, L( "Kreis\0Acht\0Pan-Schwenk\0Tilt-Nicken\0Dreieck\0Quadrat\0Zufall\0" ) ) ) { e.shape = (FxShape)sh; ch = true; }
			OscContext( oscB + "/shape" );
			row( L( "Größe Pan" ) );
			ch |= ls::rs::SliderFloat( "##sp", &e.sizePan, 0, 360, "%.0f°" );
			OscContext( oscB + "/sizepan" );
			row( L( "Größe Tilt" ) );
			ch |= ls::rs::SliderFloat( "##st", &e.sizeTilt, 0, 180, "%.0f°" );
			OscContext( oscB + "/sizetilt" );
			break;
		}
		case FxType::Beam:
		{
			auto attrs = AttrsOf( show, FxFixtureIds( show, e ) );
			row( L( "Attribut" ) );
			if( ImGui::BeginCombo( "##attr", AttrName( e.attr ) ) )
			{
				for( Attr at : attrs )
					if( ImGui::Selectable( AttrName( at ), at == e.attr ) ) { e.attr = at; ch = true; }
				ImGui::EndCombo();
			}
			OscContext( oscB + "/attr" );
			waveCombo();
			if( e.wave == FxWave::Square || e.wave == FxWave::Pulse )
			{
				row( L( "Breite" ) );
				ch |= PctSlider( "##width", &e.width, 0.02f, 1.0f );
			}
			rangeRow( L( "Bereich min/max" ) );
			break;
		}
		case FxType::Lfo:
			waveCombo();
			if( e.wave == FxWave::Square || e.wave == FxWave::Pulse )
			{
				row( L( "Pulsbreite" ) );
				ch |= PctSlider( "##width", &e.width, 0.02f, 1.0f );
				OscContext( oscB + "/width" );
			}
			row( L( "Modus" ) );
			{
				int rm = e.relative ? 1 : 0;
				if( ImGui::Combo( "##rel", &rm, L( "Bereich (absolut)\0Um den Wert darunter\0" ) ) ) { e.relative = rm == 1; ch = true; }
				ImGui::SetItemTooltip( L( "Absolut: pendelt zwischen Minimum und Maximum.\nUm den Wert darunter: schwingt um den Programmer- / Snapshot-Wert (z. B. Pan um die aktuelle Position)." ) );
			}
			if( e.relative )
			{
				row( L( "Tiefe" ) );
				ch |= PctSlider( "##depth", &e.depth );
				OscContext( oscB + "/depth" );
			}
			else
				rangeRow( L( "Minimum / Maximum" ) );
			break;
		case FxType::Envelope:
		{
			const char* u = e.sync ? "%.2f Beats" : "%.2f s";
			const float mx = e.sync ? 16.0f : 10.0f;
			{
				int dm = e.envDrawn ? 1 : 0;
				row( L( "Form" ) );
				if( ImGui::Combo( "##envm", &dm, L( "ADSR\0Eigene\0" ) ) )
				{
					if( dm == 1 ) EnvToDrawn( e );
					else e.envDrawn = false;
					ch         = true;
				}
			}
			if( !e.envDrawn ) ImGui::TextDisabled( L( "Klick in die Grafik: eigene Kurve zeichnen" ) );
			if( e.envDrawn )
			{
				row( L( "Länge" ) );
				ch |= ls::rs::SliderFloat( "##gate", &e.gate, 0.01f, mx, u, ImGuiSliderFlags_Logarithmic );
				ImGui::TextDisabled( L( "Links in der Grafik: Klick = Punkt, ziehen, Rechtsklick = Verlauf" ) );
				int rm = e.relative ? 1 : 0;
				row( L( "Modus" ) );
				if( ImGui::Combo( "##rel", &rm, L( "Bereich (absolut)\0Um den Wert darunter\0" ) ) ) { e.relative = rm == 1; ch = true; }
				if( e.relative )
				{
					row( L( "Tiefe" ) );
					ch |= PctSlider( "##depth", &e.depth );
				}
				else
					rangeRow( L( "Minimum / Maximum" ) );
				break;
			}
			row( "Attack" );
			ch |= ls::rs::SliderFloat( "##atk", &e.atk, 0, mx, u, ImGuiSliderFlags_Logarithmic );
			row( "Hold" );
			ch |= ls::rs::SliderFloat( "##hold", &e.hold, 0, mx, u, ImGuiSliderFlags_Logarithmic );
			row( "Decay" );
			ch |= ls::rs::SliderFloat( "##dec", &e.dec, 0, mx, u, ImGuiSliderFlags_Logarithmic );
			row( "Sustain" );
			ch |= PctSlider( "##sus", &e.sus );
			row( L( "Gate" ) );
			ch |= ls::rs::SliderFloat( "##gate", &e.gate, 0, mx, u, ImGuiSliderFlags_Logarithmic );
			ImGui::SetItemTooltip( L( "So lange hält Sustain (ab Auslösung), dann folgt Release" ) );
			row( "Release" );
			ch |= ls::rs::SliderFloat( "##rel", &e.rel, 0, mx, u, ImGuiSliderFlags_Logarithmic );
			row( L( "Kurve" ) );
			ch |= ls::rs::SliderFloat( "##curve", &e.curve, 0, 1, e.curve < 0.05f ? L( "linear" ) : "%.2f" );
			{
				int rm = e.relative ? 1 : 0;
				row( L( "Modus" ) );
				if( ImGui::Combo( "##rel", &rm, L( "Bereich (absolut)\0Um den Wert darunter\0" ) ) ) { e.relative = rm == 1; ch = true; }
			}
			if( e.relative )
			{
				row( L( "Tiefe" ) );
				ch |= PctSlider( "##depth", &e.depth );
			}
			else
				rangeRow( L( "Minimum / Maximum" ) );
			ImGui::TextDisabled( L( "Punkte in der Kurve oben ziehen" ) );
			break;
		}
		case FxType::Sequencer:
		{
			int st = e.steps;
			row( L( "Schritte" ) );
			if( ls::rs::SliderInt( "##steps", &st, 1, 64 ) ) { FxResizeSteps( e, st ); ch = true; }
			{
				static const float kStep[] = { 0.0625f, 0.125f, 0.25f, 0.5f, 1, 2, 4 };
				row( L( "Schrittlänge" ) );
				if( ImGui::BeginCombo( "##sl", BeatLabel( e.stepBeats ).c_str() ) )
				{
					for( float x : kStep )
						if( ImGui::Selectable( BeatLabel( x ).c_str(), fabsf( x - e.stepBeats ) < 1e-4f ) ) { e.stepBeats = x; e.beats = e.steps * x; ch = true; }
					ImGui::EndCombo();
				}
			}
			row( L( "Versatz je Lampe" ) );
			ch |= ls::rs::SliderInt( "##soff", &e.stepOffset, -16, 16, e.stepOffset ? L( "%d Schritte" ) : L( "aus" ) );
			ImGui::SetItemTooltip( L( "Jede nächste Lampe (bzw. jeder nächste Kanal) startet so viele Schritte später -\nein Muster läuft so über die Lampen." ) );
			ImGui::TextDisabled( L( "Zeichnen: linke Maus, löschen: rechte Maus, Shift = Raster 25 %%" ) );
			//pattern tools
			if( ImGui::SmallButton( L( "Alle Spuren leeren" ) ) ) { for( auto& l : e.lanes ) std::fill( l.v.begin(), l.v.end(), 0.0f ); ch = true; }
			ImGui::SameLine();
			if( ImGui::SmallButton( L( "Zufall" ) ) )
			{
				for( auto& l : e.lanes )
					for( auto& x : l.v ) x = ( rand() % 3 == 0 ) ? 0.4f + 0.6f * ( rand() % 100 ) / 100.0f : 0.0f;
				ch = true;
			}
			ImGui::SameLine();
			if( ImGui::SmallButton( L( "Verschieben >" ) ) )
			{
				for( auto& l : e.lanes )
					if( !l.v.empty() ) std::rotate( l.v.rbegin(), l.v.rbegin() + 1, l.v.rend() );
				ch = true;
			}
			break;
		}
		default: break;
		}
		//==== timing + output (same column)
		ImGui::SeparatorText( "Tempo" );
		ch |= ImGui::Checkbox( L( "Zum Tempo synchron" ), &e.sync );
		OscContext( oscB + "/sync" );
		ImGui::SetItemTooltip( L( "an: Zykluslänge in Beats (Intern/Tap, MIDI-Clock oder Ableton Link)\naus: freie Frequenz in Hz" ) );
		if( e.type != FxType::Sequencer )
		{
			if( e.sync )
			{
				row( e.type == FxType::Envelope ? L( "Auslösen alle" ) : L( "Zyklus" ) );
				if( ImGui::BeginCombo( "##beats", BeatLabel( e.beats ).c_str() ) )
				{
					for( float bb : kBeatSteps )
						if( ImGui::Selectable( BeatLabel( bb ).c_str(), fabsf( bb - e.beats ) < 1e-4f ) ) { e.beats = bb; ch = true; }
					ImGui::EndCombo();
				}
				OscContext( oscB + "/beats" );
			}
			else
			{
				row( L( "Frequenz" ) );
				ch |= ls::rs::SliderFloat( "##hz", &e.hz, 0.02f, 20.0f, "%.2f Hz", ImGuiSliderFlags_Logarithmic );
				OscContext( oscB + "/hz" );
			}
		}
		else if( !e.sync )
		{
			row( L( "Durchläufe" ) );
			ch |= ls::rs::SliderFloat( "##hz", &e.hz, 0.02f, 8.0f, "%.2f Hz", ImGuiSliderFlags_Logarithmic );
			OscContext( oscB + "/hz" );
		}
		if( e.type == FxType::Envelope )
		{
			ch |= ImGui::Checkbox( L( "Nur von Hand auslösen" ), &e.manualTrig );
			ImGui::SetItemTooltip( L( "Aus: startet jeden Zyklus neu.\nAn: nur Knopf 'Auslösen', OSC %s/trigger oder MIDI-Learn darauf." ), oscB.c_str() );
		}
		ImGui::SeparatorText( L( "Ausgabe" ) );
		row( L( "Anteil" ) );
		ch |= PctSlider( "##level", &e.level );
		ImGui::SetItemTooltip( L( "0 = nur die Grundwerte (Programmer / Pult), 1 = voller Effekt" ) );
		OscContext( oscB + "/level" );
		row( L( "Ein-/Ausblenden" ) );
		ch |= ls::rs::SliderFloat( "##fade", &e.fade, 0, 5, "%.1f s" );
		OscContext( oscB + "/fade" );
		if( mod && e.type != FxType::Sequencer && e.tgt.attr == Attr::Dimmer && e.tgt.mode != 2 )
		{
			ch |= ImGui::Checkbox( L( "Shutter öffnen" ), &e.openShutter );
			ImGui::SameLine();
			ch |= ImGui::Checkbox( L( "Skalieren" ), &e.scale );
		}
		//==== column 3: lamps / target + distribution
		ImGui::TableNextColumn();
		ImGui::SeparatorText( mod ? L( "Ziel" ) : L( "Lampen" ) );
		{
			//lamps of the effect: own list or a group
			std::string gl = e.group >= 0 && e.group < (int)show.groups.size() ? show.groups[ e.group ].name : std::string( L( "Eigene Liste" ) );
			row( L( "Lampen" ) );
			if( ImGui::BeginCombo( "##grp", gl.c_str() ) )
			{
				if( ImGui::Selectable( L( "Eigene Liste" ), e.group < 0 ) ) { e.group = -1; ch = true; }
				for( int g = 0; g < (int)show.groups.size(); ++g )
				{
					ImGui::PushID( g );
					if( ImGui::Selectable( ( std::string( L( "Gruppe: " ) ) + show.groups[ g ].name ).c_str(), e.group == g ) ) { e.group = g; ch = true; }
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
			ImGui::SetItemTooltip( L( "Gruppe: der Effekt folgt den Lampen der Gruppe (Tab Gruppen)" ) );
			if( e.group < 0 )
			{
				ImGui::Text( L( "%d Lampen" ), (int)e.fixtures.size() );
				ImGui::SameLine();
				ImGui::BeginDisabled( sel.empty() );
				if( ImGui::SmallButton( L( "Auswahl übernehmen" ) ) )
				{
					e.fixtures.clear();
					for( int id : sel )
						if( const FixtureInst* f = show.FindFixture( id ) )
							if( mod || FxSupports( show, *f, e ) ) e.fixtures.push_back( id );
					ch = true;
				}
				ImGui::SameLine();
				if( ImGui::SmallButton( L( "+ Auswahl" ) ) )
				{
					for( int id : sel )
						if( const FixtureInst* f = show.FindFixture( id ) )
							if( ( mod || FxSupports( show, *f, e ) ) && std::find( e.fixtures.begin(), e.fixtures.end(), id ) == e.fixtures.end() ) e.fixtures.push_back( id );
					ch = true;
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
			}
			if( ImGui::SmallButton( L( "zeigen" ) ) ) sel = FxFixtureIds( show, e );
		}
		if( mod && e.type != FxType::Sequencer )
		{
			ImGui::PushID( "tgt" );
			ch |= TargetUi( show, e, e.tgt, ui, lw );
			ImGui::PopID();
		}
		if( e.type == FxType::Sequencer )
		{
			int delL = -1;
			for( int li = 0; li < (int)e.lanes.size(); ++li )
			{
				ImGui::PushID( 700 + li );
				const float rowY0 = ImGui::GetCursorScreenPos().y;
				const V3 lc = kLaneCol[ li % 8 ];
				ImGui::TextColored( ImVec4( lc.x, lc.y, lc.z, 1 ), L( "Spur %d" ), li + 1 );
				ImGui::SameLine();
				if( ImGui::SmallButton( "x" ) || DelRow( rowY0 ) ) delL = li;
				ch |= TargetUi( show, e, e.lanes[ li ].tgt, ui, lw );
				ImGui::PopID();
			}
			if( delL >= 0 ) { e.lanes.erase( e.lanes.begin() + delL ); ch = true; }
			if( e.lanes.size() < 8 && ImGui::SmallButton( L( "+ Spur" ) ) )
			{
				FxLane l = FxNewLane( e, (int)e.lanes.size() );
				if( !e.lanes.empty() ) l.tgt = e.lanes.back().tgt;
				//next useful attribute of the lamps
				auto attrs = AttrsOf( show, FxFixtureIds( show, e ) );
				for( Attr at : { Attr::Red, Attr::Pan, Attr::Zoom, Attr::Tilt, Attr::Blue, Attr::Green } )
				{
					bool used = false;
					for( auto& x : e.lanes ) used |= x.tgt.attr == at;
					if( !used && std::find( attrs.begin(), attrs.end(), at ) != attrs.end() ) { l.tgt.attr = at; break; }
				}
				e.lanes.push_back( l );
				ch = true;
			}
		}
		ImGui::SeparatorText( L( "Verteilung" ) );
		if( e.type != FxType::Sequencer )
		{
			row( e.type == FxType::Envelope ? L( "Verzögerung" ) : L( "Phasenversatz" ) );
			ch |= ls::rs::SliderFloat( "##spread", &e.spread, 0, 720, "%.0f°" );
			ImGui::SetItemTooltip( e.type == FxType::Envelope ? L( "Zeitversatz der Lampen: 360° = über einen ganzen Zyklus verteilt" ) : L( "0° = alle gleich, 360° = eine volle Welle über alle Lampen" ) );
			OscContext( oscB + "/spread" );
		}
		int o = (int)e.order;
		row( L( "Reihenfolge" ) );
		if( ImGui::Combo( "##order", &o, L( "Auswahl-Reihenfolge\0Links -> Rechts\0Vorne -> Hinten\0Mitte -> Außen\0Zufall\0" ) ) ) { e.order = (FxOrder)o; ch = true; }
		OscContext( oscB + "/order" );
		row( L( "Gruppen" ) );
		ch |= ls::rs::SliderInt( "##groups", &e.groups, 0, 8, e.groups ? L( "%d (jede n-te gleich)" ) : L( "aus" ) );
		OscContext( oscB + "/groups" );
		ImGui::SetItemTooltip( L( "2 = gerade/ungerade abwechselnd, 3 = jede dritte Lampe gleich ..." ) );
		ch |= ImGui::Checkbox( L( "Spiegeln (von der Mitte)" ), &e.mirror );
		OscContext( oscB + "/mirror" );
		ImGui::SameLine();
		ch |= ImGui::Checkbox( L( "Rückwärts" ), &e.reverse );
		OscContext( oscB + "/reverse" );
		bool anyPixel = false;
		for( int id : FxFixtureIds( show, e ) )
			if( const FixtureInst* f = show.FindFixture( id ) ) anyPixel |= f->def && f->def->Heads() > 1;
		if( anyPixel && e.kind != FxKind::Position )
		{
			ch |= ImGui::Checkbox( L( "Pixel einzeln" ), &e.pixels );
			OscContext( oscB + "/pixels" );
			ImGui::SetItemTooltip( L( "Pixel-Lampen (LED-Bars): jeder Pixel ist ein eigenes Element des Effekts" ) );
		}
		ImGui::EndTable();
	}
	ImGui::EndChild();
	if( ch ) show.dirty = true;
}
}//namespace ls
