// bottom panel tabs (DMX, MIDI, Output, Stage, Snapshots) and the fixture library dialog
#include "app.h"
#include "imgui.h"
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

namespace ls
{
static ImU32 FixtureColorT( int id, float a = 1.0f )
{
	V3 c = Hsv( fmodf( id * 0.1618f, 1.0f ), 0.55f, 0.85f );
	return ImGui::ColorConvertFloat4ToU32( ImVec4( c.x, c.y, c.z, a ) );
}

//palettes: taken out of 1.2 (code and show data stay, tab hidden)
static constexpr bool kPalettesTab = false;
void App::PanelBottom()
{
	if( !ImGui::BeginTabBar( "tabs" ) ) return;
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6, 5 ) );
	//--tab <name> (screenshots / tests): select that tab once
	auto fl = [ & ]( const char* name, const char* en = "" ) { return !startTab.empty() && ( startTab == name || startTab == en ) ? ImGuiTabItemFlags_SetSelected : 0; };//German key or English name
	//a snapshot / effect dragged towards the Time Line: its list disappears when the tab changes, and ImGui drops a payload
	//whose source is no longer drawn - keep it alive while the mouse button is held
	if( const ImGuiPayload* pl = ImGui::GetDragDropPayload() )
		if( pl->IsDataType( "LS_TLCLIP" ) && ImGui::IsMouseDown( 0 ) ) GImGui->DragDropPayload.DataFrameCount = ImGui::GetFrameCount();
	auto tabMark = [ & ]( const char* n ) { UiMark( n, ImGui::GetItemRectMin(), ImGui::GetItemRectMax() ); };
	if( ImGui::BeginTabItem( "DMX", nullptr, fl( "DMX" ) ) ) { TabDmx(); ImGui::EndTabItem(); }
	{
		const bool o = ImGui::BeginTabItem( L( "Effekte" ), nullptr, fl( "Effekte", "Effects" ) );
		tabMark( "tab.effects" );
		if( o ) { TabEffects(); ImGui::EndTabItem(); }
	}
	if( ImGui::BeginTabItem( "MIDI", nullptr, fl( "MIDI" ) ) ) { TabMidi(); ImGui::EndTabItem(); }
	if( ImGui::BeginTabItem( L( "Ausgang / Interface" ), nullptr, fl( "Ausgang", "Output" ) ) ) { lastOutputTabFrame_ = ImGui::GetFrameCount(); TabOutput(); ImGui::EndTabItem(); }
	if( ImGui::BeginTabItem( L( "Bühne" ), nullptr, fl( "Buehne", "Stage" ) ) ) { TabStage(); ImGui::EndTabItem(); }
	if( ImGui::BeginTabItem( L( "Video / Leinwand" ), nullptr, fl( "Video" ) ) )
	{
		videoTab_ = true;
		TabVideo();
		ImGui::EndTabItem();
	}
	else
		videoTab_ = false;
	{
		const bool o = ImGui::BeginTabItem( "Snapshots", nullptr, fl( "Snapshots" ) );
		tabMark( "tab.snapshots" );
		if( o ) { TabSnapshots(); ImGui::EndTabItem(); }
	}
	if( ImGui::BeginTabItem( "Cues", nullptr, fl( "Cues" ) ) ) { TabCues(); ImGui::EndTabItem(); }
	{
		ImGuiTabItemFlags tf = fl( "Timeline" ) | ( tlSelectTab_ && !tlWin_ ? ImGuiTabItemFlags_SetSelected : 0 );
		tlSelectTab_         = false;
		const bool tlOpen = ImGui::BeginTabItem( "Time Line", nullptr, tf );
		tabMark( "tab.timeline" );
		//drag & drop: a dragged snapshot / effect opens the Time Line when it is held over its tab
		if( !tlOpen && ImGui::BeginDragDropTarget() )
		{
			if( ImGui::AcceptDragDropPayload( "LS_TLCLIP", ImGuiDragDropFlags_AcceptPeekOnly | ImGuiDragDropFlags_AcceptNoDrawDefaultRect ) ) tlSelectTab_ = true;
			ImGui::EndDragDropTarget();
		}
		if( tlOpen ) { TabTimeline(); ImGui::EndTabItem(); }
	}
	if( kPalettesTab && ImGui::BeginTabItem( L( "Paletten" ), nullptr, fl( "Paletten", "Palettes" ) ) ) { TabPalettes(); ImGui::EndTabItem(); }
	if( ImGui::BeginTabItem( L( "Gruppen" ), nullptr, fl( "Gruppen", "Groups" ) ) ) { TabGroups(); ImGui::EndTabItem(); }
	if( startTabFrames_++ > 3 ) startTab.clear();
	ImGui::PopStyleVar();
	ImGui::EndTabBar();
}

//---------------------------------------------------------------- DMX grid
void App::TabDmx()
{
	if( show.UniIndex( dmxUni_ ) < 0 && !show.universes.empty() ) dmxUni_ = show.universes[ 0 ].num;
	ImGui::SetNextItemWidth( 140 * ui );
	if( ImGui::BeginCombo( "##uni", ( "Universe " + std::to_string( dmxUni_ ) ).c_str() ) )
	{
		for( auto& u : show.universes )
			if( ImGui::Selectable( ( "Universe " + std::to_string( u.num ) ).c_str(), u.num == dmxUni_ ) ) dmxUni_ = u.num;
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if( ImGui::Button( L( "Alles auf 0" ) ) )
	{
		std::lock_guard< std::mutex > l( show.dmxLock );
		if( DmxFrame* fr = show.Frame( dmxUni_ ) ) fr->fill( 0 );
		show.Bump();
	}
	ImGui::SameLine();
	if( ImGui::Button( L( "Lampen auf Grundstellung" ) ) )
		for( auto& f : show.fixtures )
			if( f.universe == dmxUni_ ) show.Home( f );
	ImGui::SameLine();
	ImGui::TextDisabled( L( "Ziehen = Wert ändern, Doppelklick = 0/255, Klick = Lampe wählen, Rechtsklick = Time Line" ) );
	DmxFrame* fr = show.Frame( dmxUni_ );
	if( !fr ) return;
	ImVec2 avail = ImGui::GetContentRegionAvail();
	const int cols = 32;
	float cw       = std::floor( ( avail.x - 2 ) / cols );
	const float lh = ImGui::GetTextLineHeight();
	float ch       = std::floor( lh * 2.0f + 8.0f );//channel number + value
	ImVec2 p0      = ImGui::GetCursorScreenPos();
	ImGui::BeginChild( "grid", ImVec2( 0, 0 ), ImGuiChildFlags_None );
	p0 = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton( "##grid", ImVec2( cw * cols, ch * 16 ) );
	static int dragCell = -1;
	static float dragAcc = 0;
	ImGuiIO& io_ = ImGui::GetIO();
	auto cellAt  = [ & ]( ImVec2 m ) {
		int cx = (int)( ( m.x - p0.x ) / cw ), cy = (int)( ( m.y - p0.y ) / ch );
		if( cx < 0 || cy < 0 || cx >= cols || cy >= 16 ) return -1;
		return cy * cols + cx;
	};
	if( ImGui::IsItemActivated() ) { dragCell = cellAt( io_.MousePos ); dragAcc = 0; }
	if( ImGui::IsItemActive() && dragCell >= 0 )
	{
		dragAcc -= io_.MouseDelta.y * ( io_.KeyShift ? 0.25f : 1.5f );
		int step = (int)dragAcc;
		if( step )
		{
			dragAcc -= step;
			std::lock_guard< std::mutex > l( show.dmxLock );
			( *fr )[ dragCell ] = (uint8_t)Clamp( (int)( *fr )[ dragCell ] + step, 0, 255 );
			show.Bump();
		}
	}
	if( ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked( 0 ) )
	{
		int c = cellAt( io_.MousePos );
		if( c >= 0 )
		{
			std::lock_guard< std::mutex > l( show.dmxLock );
			( *fr )[ c ] = ( *fr )[ c ] > 127 ? 0 : 255;
			show.Bump();
		}
	}
	if( ImGui::IsItemDeactivated() && dragCell >= 0 && fabsf( io_.MouseDragMaxDistanceSqr[ 0 ] ) < 9 )
	{
		int slot;
		if( FixtureInst* f = show.FixtureAt( dmxUni_, dragCell + 1, &slot ) ) Select( f->id, io_.KeyCtrl, false );
		dragCell = -1;
	}
	//right click on a channel: timeline (new timelane for the lamp there or a raw DMX timelane)
	static int ctxCell = -1;
	if( ImGui::IsItemHovered() && ImGui::IsMouseClicked( 1 ) )
	{
		ctxCell = cellAt( io_.MousePos );
		if( ctxCell >= 0 ) ImGui::OpenPopup( "##dmxctx" );
	}
	if( ImGui::BeginPopup( "##dmxctx" ) )
	{
		if( ctxCell >= 0 ) TimelineChannelMenu( dmxUni_, ctxCell + 1 );
		ImGui::EndPopup();
	}
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImGui::PushFont( fontMono );
	int hovered = ImGui::IsItemHovered() ? cellAt( io_.MousePos ) : -1;
	for( int i = 0; i < 512; ++i )
	{
		int cx = i % cols, cy = i / cols;
		ImVec2 a( p0.x + cx * cw, p0.y + cy * ch ), b( a.x + cw - 1, a.y + ch - 1 );
		int slot;
		FixtureInst* f = show.FixtureAt( dmxUni_, i + 1, &slot );
		ImU32 bg       = f ? FixtureColorT( f->id, IsSel( f->id ) ? 0.55f : 0.22f ) : IM_COL32( 22, 24, 30, 255 );
		dl->AddRectFilled( a, b, bg, 2 );
		//shown: what goes out (effects on top of the base value, orange)
		const int ui_    = show.UniIndex( dmxUni_ );
		const bool fxCh  = show.fxAny && ui_ >= 0 && ui_ < (int)show.fxOut.size() && show.fxOut[ ui_ ][ i ] >= 0;
		uint8_t v        = fxCh ? (uint8_t)show.fxOut[ ui_ ][ i ] : ( *fr )[ i ];
		if( v )
		{
			float hgt = ( ch - 2 ) * v / 255.0f;
			dl->AddRectFilled( ImVec2( a.x + 1, b.y - hgt ), ImVec2( b.x - 1, b.y - 1 ), fxCh ? IM_COL32( 255, 150, 40, 110 ) : IM_COL32( 70, 150, 255, 90 ), 1 );
		}
		if( f && slot == 0 ) dl->AddLine( ImVec2( a.x, a.y ), ImVec2( a.x, b.y ), FixtureColorT( f->id ), 2 );
		char t[ 8 ];
		snprintf( t, sizeof( t ), "%d", i + 1 );
		dl->AddText( ImVec2( a.x + 5, a.y + 3 ), f ? IM_COL32( 170, 178, 195, 200 ) : IM_COL32( 70, 75, 88, 255 ), t );
		snprintf( t, sizeof( t ), "%d", v );
		ImVec2 vs = ImGui::CalcTextSize( t );
		dl->AddText( ImVec2( b.x - vs.x - 5, b.y - lh - 3 ), v ? IM_COL32( 240, 244, 252, 255 ) : IM_COL32( 85, 90, 105, 255 ), t );
		if( i == hovered ) dl->AddRect( a, b, IM_COL32( 255, 255, 255, 200 ), 2 );
	}
	ImGui::PopFont();
	if( hovered >= 0 )
	{
		int slot;
		FixtureInst* f = show.FixtureAt( dmxUni_, hovered + 1, &slot );
		if( f && f->M() )
		{
			const Channel& c = f->M()->ch[ slot ];
			const Cap* cp    = c.isFine ? nullptr : c.Find( show.Get16( *f, slot ) );
			const int ui_   = show.UniIndex( dmxUni_ );
			const int fxv   = show.fxAny && ui_ >= 0 && ui_ < (int)show.fxOut.size() ? show.fxOut[ ui_ ][ hovered ] : -1;
			char fxs[ 48 ]  = "";
			if( fxv >= 0 ) snprintf( fxs, sizeof( fxs ), L( "  (Effekt: %d)" ), fxv );
			ImGui::SetTooltip( "%d.%03d = %d%s\n%s (ID %d)\n%s  [%s]\n%s", dmxUni_, hovered + 1, ( *fr )[ hovered ], fxs, f->name.c_str(), f->id, c.name.c_str(), AttrName( c.attr ), cp ? cp->label.c_str() : "" );
		}
		else
			ImGui::SetTooltip( L( "%d.%03d = %d\n(frei)" ), dmxUni_, hovered + 1, ( *fr )[ hovered ] );
	}
	ImGui::EndChild();
}

//---------------------------------------------------------------- MIDI
static std::string MidiFileDialog( bool save )
{
	wchar_t buf[ MAX_PATH * 4 ] = {};
	OPENFILENAMEW of{};
	of.lStructSize = sizeof( of );
	of.hwndOwner   = GetActiveWindow();
	of.lpstrFilter = LW( L"LightStage MIDI-Mapping (*.lsmidi)\0*.lsmidi\0Alle Dateien\0*.*\0" );
	of.lpstrFile   = buf;
	of.nMaxFile    = MAX_PATH * 4;
	of.lpstrDefExt = L"lsmidi";
	of.Flags       = OFN_NOCHANGEDIR | OFN_EXPLORER | ( save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST );
	BOOL ok        = save ? GetSaveFileNameW( &of ) : GetOpenFileNameW( &of );
	return ok ? Narrow( buf ) : std::string();
}

void App::TabMidi()
{
	ImGui::BeginChild( "mleft", ImVec2( ImGui::GetContentRegionAvail().x * 0.68f, 0 ), ImGuiChildFlags_None );
	ImGui::PushStyleColor( ImGuiCol_Button, learn_ ? ImVec4( 0.85f, 0.6f, 0.1f, 1 ) : ImGui::GetStyle().Colors[ ImGuiCol_Button ] );
	if( ImGui::Button( learn_ ? L( "MIDI Learn: AN" ) : "MIDI Learn", ImVec2( 130 * ui, 0 ) ) ) { learn_ = !learn_; learnArmed_ = false; }
	ImGui::PopStyleColor();
	ImGui::SameLine();
	if( ImGui::Button( L( "Geräte neu suchen" ) ) ) midi.Rescan();
	ImGui::SameLine();
	std::string devs;
	for( auto& d : midi.devices ) devs += ( devs.empty() ? "" : ", " ) + d;
	ImGui::TextDisabled( L( "Eingänge: %s" ), devs.empty() ? L( "keine" ) : devs.c_str() );
	if( !midi.failed.empty() )
	{
		std::string f;
		for( auto& d : midi.failed ) f += ( f.empty() ? "" : ", " ) + d;
		ImGui::TextColored( ImVec4( 1, 0.6f, 0.3f, 1 ), L( "Belegt/nicht geöffnet: %s" ), f.c_str() );
	}
	//mapping files
	if( ImGui::Button( L( "Exportieren..." ) ) )
	{
		std::string p = MidiFileDialog( true );
		if( !p.empty() ) SetStatus( ExportMidi( p ) ? L( "MIDI-Mapping gespeichert: " ) + FileName( p ) : L( "MIDI-Mapping konnte nicht gespeichert werden" ) );
	}
	ImGui::SetItemTooltip( L( "Alle Zuweisungen als Datei (.lsmidi) speichern" ) );
	ImGui::SameLine();
	if( ImGui::Button( L( "Importieren..." ) ) ) ImGui::OpenPopup( "midiimp" );
	if( ImGui::BeginPopup( "midiimp" ) )
	{
		for( int k = 0; k < 2; ++k )
			if( ImGui::MenuItem( k == 0 ? L( "Hinzufügen (bestehende bleiben)" ) : L( "Ersetzen (bestehende löschen)" ) ) )
			{
				std::string p = MidiFileDialog( false );
				if( !p.empty() )
				{
					int n = ImportMidi( p, k == 1 );
					SetStatus( n >= 0 ? std::to_string( n ) + L( " MIDI-Zuweisungen geladen" ) : L( "Datei ist kein LightStage-MIDI-Mapping" ) );
				}
			}
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	if( ImGui::Button( L( "Als Standard speichern" ) ) ) SetStatus( ExportMidi( MidiDefaultFile() ) ? L( "Standard-MIDI-Mapping gespeichert (gilt für neue Shows)" ) : L( "Speichern fehlgeschlagen" ) );
	ImGui::SetItemTooltip( L( "Neue Shows starten automatisch mit diesen Zuweisungen.\nLampen-Zuweisungen gelten für die Lampen-Nummern (IDs)." ) );
	ImGui::SameLine();
	if( ImGui::Button( L( "Standard laden" ) ) )
	{
		int n = ImportMidi( MidiDefaultFile(), true );
		SetStatus( n >= 0 ? std::to_string( n ) + L( " Standard-Zuweisungen geladen" ) : L( "Noch kein Standard-Mapping gespeichert" ) );
	}
	ImGui::SameLine();
	if( ImGui::Button( L( "Alle löschen" ) ) && !show.midi.empty() ) { show.midi.clear(); show.dirty = true; }
	ImGui::SameLine();
	if( ImGui::Button( L( "Lernbare Ziele..." ) ) ) targetsWin_ = true;
	ImGui::SetItemTooltip( L( "Liste aller Ziele (Lampen, DMX, Effekte, Parameter), Doppelklick = MIDI lernen" ) );
	ImGui::TextDisabled( L( "Ableton: MIDI-Spur auf einen virtuellen Port (z.B. loopMIDI / LoopBe1) ausgeben, hier wird er automatisch geöffnet. Zuweisungen werden mit der Show gespeichert." ) );
	if( ImGui::BeginTable( "maps", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp, ImVec2( 0, ImGui::GetContentRegionAvail().y ) ) )
	{
		ImGui::TableSetupScrollFreeze( 0, 1 );
		ImGui::TableSetupColumn( L( "Gerät" ), ImGuiTableColumnFlags_WidthStretch, 1.2f );
		ImGui::TableSetupColumn( "Ch", ImGuiTableColumnFlags_WidthFixed, 52 * ui );
		ImGui::TableSetupColumn( L( "Typ" ), ImGuiTableColumnFlags_WidthFixed, 92 * ui );
		ImGui::TableSetupColumn( L( "Nr" ), ImGuiTableColumnFlags_WidthFixed, 60 * ui );
		ImGui::TableSetupColumn( L( "Ziel" ), ImGuiTableColumnFlags_WidthStretch, 2.0f );
		ImGui::TableSetupColumn( L( "Bereich" ), ImGuiTableColumnFlags_WidthFixed, 110 * ui );
		ImGui::TableSetupColumn( "Inv", ImGuiTableColumnFlags_WidthFixed, 28 * ui );
		ImGui::TableSetupColumn( "Tgl", ImGuiTableColumnFlags_WidthFixed, 28 * ui );
		ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthFixed, 24 * ui );
		ImGui::TableHeadersRow();
		int del = -1;
		for( int i = 0; i < (int)show.midi.size(); ++i )
		{
			MidiMap& m = show.midi[ i ];
			ImGui::PushID( i );
			ImGui::TableNextRow();
			const float rowY0 = ImGui::GetCursorScreenPos().y;
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth( -1 );
			if( ImGui::BeginCombo( "##dev", m.device.empty() ? L( "(alle)" ) : m.device.c_str() ) )
			{
				if( ImGui::Selectable( L( "(alle)" ), m.device.empty() ) ) m.device.clear();
				for( auto& d : midi.devices )
					if( ImGui::Selectable( d.c_str(), d == m.device ) ) m.device = d;
				ImGui::EndCombo();
			}
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth( -1 );
			char chs[ 8 ];
			snprintf( chs, sizeof( chs ), m.channel < 0 ? L( "alle" ) : "%d", m.channel + 1 );
			if( ImGui::BeginCombo( "##ch", chs ) )
			{
				if( ImGui::Selectable( L( "alle" ), m.channel < 0 ) ) m.channel = -1;
				for( int c = 0; c < 16; ++c )
					if( ImGui::Selectable( std::to_string( c + 1 ).c_str(), m.channel == c ) ) m.channel = c;
				ImGui::EndCombo();
			}
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth( -1 );
			int t = (int)m.type;
			if( ImGui::Combo( "##t", &t, "CC\0Note\0Pitch Bend\0CC 14-bit\0" ) ) { m.type = (MidiType)t; show.dirty = true; }
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth( -1 );
			if( m.type != MidiType::PitchBend && ImGui::InputInt( "##n", &m.number, 0 ) ) { m.number = Clamp( m.number, 0, 127 ); show.dirty = true; }
			ImGui::TableNextColumn();
			ImGui::TextUnformatted( m.Describe( show ).c_str() );
			if( m.last >= 0 )
			{
				ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
				ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( a.x, b.y ), ImVec2( a.x + ( b.x - a.x + 40 ) * m.last, b.y + 2 ), IM_COL32( 80, 170, 255, 200 ) );
			}
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth( -1 );
			//2.1: pan / tilt range in degrees from the centre
			const FixtureInst* mf = m.target == MidiTarget::FixtureAttr && ( m.attr == Attr::Pan || m.attr == Attr::Tilt ) && !m.fixtures.empty() ? show.FindFixture( m.fixtures[ 0 ] ) : nullptr;
			const float rng = mf ? show.AttrRangeDeg( *mf, m.attr ) : 0.0f;
			if( rng > 0 )
			{
				float dl = ( m.lo - 0.5f ) * rng, dh = ( m.hi - 0.5f ) * rng;
				if( ls::rs::DragFloatRange2( "##rd", &dl, &dh, 0.5f, -rng * 0.5f, rng * 0.5f, "%.0f°", "%.0f°" ) )
				{
					m.lo       = Saturate( 0.5f + dl / rng );
					m.hi       = Saturate( 0.5f + dh / rng );
					show.dirty = true;
				}
				ImGui::SetItemTooltip( "%s", L( "Bereich in Grad ab Mitte (Pan/Tilt)" ) );
			}
			else if( ls::rs::DragFloatRange2( "##r", &m.lo, &m.hi, 0.005f, 0, m.target == MidiTarget::Master ? 10.0f : 1.0f, "%.2f", "%.2f" ) ) show.dirty = true;
			if( m.target == MidiTarget::Master ) ImGui::SetItemTooltip( L( "Master: 1.00 = 100 %%, bis 10.00 = 1000 %%" ) );
			ImGui::TableNextColumn();
			ImGui::Checkbox( "##i", &m.invert );
			ImGui::TableNextColumn();
			ImGui::Checkbox( "##g", &m.toggle );
			ImGui::TableNextColumn();
			if( ImGui::SmallButton( "x" ) || DelRow( rowY0 ) ) del = i;
			ImGui::PopID();
		}
		if( del >= 0 ) { show.midi.erase( show.midi.begin() + del ); show.dirty = true; }
		if( show.midi.empty() )
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled( L( "Noch keine Zuweisungen." ) );
			ImGui::TableNextColumn();
			ImGui::TableNextColumn();
			ImGui::TableNextColumn();
			ImGui::TableNextColumn();
			ImGui::TextDisabled( L( "MIDI Learn an, Regler im Inspector anklicken, Controller bewegen." ) );
		}
		ImGui::EndTable();
	}
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild( "mmon", ImVec2( 0, 0 ), ImGuiChildFlags_Borders );
	ImGui::TextDisabled( L( "MIDI-Monitor (%llu)" ), (unsigned long long)midi.received );
	ImGui::PushFont( fontMono );
	for( auto it = midi.monitor.rbegin(); it != midi.monitor.rend(); ++it )
		ImGui::TextUnformatted( it->c_str() );
	ImGui::PopFont();
	ImGui::EndChild();
}

//---------------------------------------------------------------- output
void App::TabOutput()
{
	OutputCfg& o = show.output;
	bool changed = false;
	ImGui::BeginChild( "art", ImVec2( ImGui::GetContentRegionAvail().x * 0.5f - 6, 0 ), ImGuiChildFlags_Borders );
	ImGui::TextUnformatted( "Art-Net" );
	changed |= ImGui::Checkbox( L( "Art-Net senden" ), &o.artnet );
	char ip[ 64 ];
	snprintf( ip, sizeof( ip ), "%s", o.artnetTarget.c_str() );
	ImGui::SetNextItemWidth( 180 * ui );
	if( ImGui::InputText( L( "Ziel-IP" ), ip, sizeof( ip ), ImGuiInputTextFlags_EnterReturnsTrue ) || ImGui::IsItemDeactivatedAfterEdit() ) { o.artnetTarget = ip; changed = true; }
	ImGui::SameLine();
	if( ImGui::SmallButton( "Broadcast" ) ) { o.artnetTarget = "255.255.255.255"; changed = true; }
	ImGui::SameLine();
	if( ImGui::SmallButton( "2.x" ) ) { o.artnetTarget = "2.255.255.255"; changed = true; }
	snprintf( ip, sizeof( ip ), "%s", o.artnetBind.c_str() );
	ImGui::SetNextItemWidth( 180 * ui );
	if( ImGui::InputText( L( "Netzwerkkarte (Bind-IP)" ), ip, sizeof( ip ), ImGuiInputTextFlags_EnterReturnsTrue ) || ImGui::IsItemDeactivatedAfterEdit() ) { o.artnetBind = ip; changed = true; }
	changed |= ImGui::Checkbox( L( "Art-Net empfangen (Visualisierung von Pult / Resolume / Software)" ), &o.artnetInput );
	ImGui::TextDisabled( L( "Empfang pro Universe in der Patch-Liste mit 'In' aktivieren. LTP-Merge mit lokalen Reglern." ) );
	ImGui::SetNextItemWidth( 180 * ui );
	changed |= ls::rs::SliderFloat( L( "Senderate (Hz)" ), &o.rate, 10, 44, "%.0f" );
	ImGui::Text( L( "Gesendet: %llu Pakete   Empfangen: %llu" ), (unsigned long long)io.artnetSent.load(), (unsigned long long)io.artnetRecv.load() );
	if( !io.lastInputUniverses.empty() ) ImGui::TextColored( ImVec4( 0.4f, 1, 0.5f, 1 ), L( "Eingang aktiv" ) );
	ImGui::Separator();
	ImGui::TextUnformatted( "sACN (E1.31)" );
	changed |= ImGui::Checkbox( L( "sACN senden" ), &o.sacn );
	ImGui::SetItemTooltip( L( "Jedes Universe mit 'Out' in der Patch-Liste geht auf sACN-Universe = seine Nummer.\nOhne Ziel-IP: Multicast 239.255.x.y (Standard)." ) );
	ImGui::SameLine();
	ImGui::SetNextItemWidth( 110 * ui );
	if( ImGui::InputInt( L( "Priorität" ), &o.sacnPriority, 0 ) ) { o.sacnPriority = Clamp( o.sacnPriority, 0, 200 ); changed = true; }
	snprintf( ip, sizeof( ip ), "%s", o.sacnTarget.c_str() );
	ImGui::SetNextItemWidth( 180 * ui );
	if( ImGui::InputTextWithHint( L( "sACN Ziel-IP" ), L( "leer = Multicast" ), ip, sizeof( ip ), ImGuiInputTextFlags_EnterReturnsTrue ) || ImGui::IsItemDeactivatedAfterEdit() ) { o.sacnTarget = ip; changed = true; }
	if( o.sacn ) ImGui::Text( L( "sACN gesendet: %llu Pakete" ), (unsigned long long)io.sacnSent.load() );
	ImGui::Separator();
	ImGui::TextUnformatted( "OSC" );
	changed |= ImGui::Checkbox( L( "OSC empfangen" ), &o.oscIn );
	ImGui::SameLine();
	ImGui::SetNextItemWidth( 110 * ui );
	if( ImGui::InputInt( L( "UDP-Port" ), &o.oscPort, 0 ) ) { o.oscPort = Clamp( o.oscPort, 1024, 65535 ); changed = true; }
	if( o.oscIn )
	{
		if( osc.Running() ) ImGui::TextColored( ImVec4( 0.4f, 1, 0.5f, 1 ), L( "Hört auf Port %d   Nachrichten: %llu" ), osc.Port(), (unsigned long long)osc.received.load() );
		else ImGui::TextColored( ImVec4( 1, 0.55f, 0.3f, 1 ), "%s", osc.error.empty() ? L( "startet..." ) : osc.error.c_str() );
		if( !osc.monitor.empty() ) ImGui::TextDisabled( L( "Zuletzt: %s" ), osc.monitor.back().c_str() );
	}
	ImGui::TextDisabled( "/ls/fixture/<ID>/dimmer 0..1   /ls/fixture/<ID>/color r g b   /ls/fixture/<ID>/red/<Pixel>" );
	ImGui::TextDisabled( L( "/ls/dmx/<Universe>/<Kanal> 0..1 oder 0..255   /ls/master   /ls/haze   /ls/snapshot <Nr>" ) );
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild( "ent", ImVec2( 0, 0 ), ImGuiChildFlags_Borders );
	ImGui::TextUnformatted( L( "USB-DMX-Interface" ) );
		ImGui::SetItemTooltip( "%s", L( "Alle USB-DMX-Interfaces mit dem DMX-USB-Pro-Protokoll (FTDI, virtueller COM-Port)" ) );
	changed |= ImGui::Checkbox( L( "Über USB-DMX ausgeben" ), &o.enttec );
	ImGui::SetNextItemWidth( 260 * ui );
	if( ImGui::BeginCombo( "Port", o.enttecPort == "auto" ? L( "Automatisch erkennen" ) : o.enttecPort.c_str() ) )
	{
		if( ImGui::Selectable( L( "Automatisch erkennen" ), o.enttecPort == "auto" ) ) { o.enttecPort = "auto"; changed = true; }
		for( auto& p : ports_ )
		{
			std::string l = p.port + "  " + p.name + ( p.ftdi ? "  [FTDI]" : "" );
			if( ImGui::Selectable( l.c_str(), o.enttecPort == p.port ) ) { o.enttecPort = p.port; changed = true; }
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if( ImGui::Button( L( "Ports aktualisieren" ) ) ) ports_ = ListComPorts();
	int u = o.enttecUniverse;
	ImGui::SetNextItemWidth( 120 * ui );
	if( ImGui::InputInt( "Universe", &u ) ) { o.enttecUniverse = std::max( 1, u ); changed = true; }
	if( ImGui::Button( L( "Interface suchen" ) ) )
	{
		std::string info;
		std::string p = io.DetectEnttec( &info );
		detectInfo_   = p.empty() ? L( "Kein USB-DMX-Interface gefunden (USB verbunden? Treiber FTDI VCP?)" ) : L( "Gefunden auf " ) + p + ( info.empty() ? "" : " (" + info + ")" );
		if( !p.empty() ) { o.enttecPort = p; changed = true; }
	}
	if( !detectInfo_.empty() ) { ImGui::SameLine(); ImGui::TextDisabled( "%s", detectInfo_.c_str() ); }
	std::string st = io.Status();
	ImGui::Text( "Status: %s", io.enttecOpen ? ( L( "verbunden, " ) + std::to_string( io.enttecSent.load() ) + L( " Frames" ) ).c_str() : ( st.empty() ? L( "aus" ) : st.c_str() ) );
	ImGui::TextDisabled( L( "Das Interface gibt ein Universe aus (Label 6, 512 Kanäle, ca. 40 Hz)." ) );
	TabBridge();
	ImGui::EndChild();
	if( changed )
	{
		show.dirty = true;
		io.Apply();
	}
}

//---------------------------------------------------------------- bridge (Ableton devices, Lightbrick, ...)
void App::TabBridge()
{
	ImGui::Separator();
	ImGui::TextUnformatted( L( "Bridge" ) );
	if( ImGui::Checkbox( L( "Bridge aktiv" ), &bridgeOn ) )
	{
		if( bridgeOn ) show.output.oscIn = true;
		SaveSettings();
		BridgeListenerUpdate();
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth( 110 * ui );
	if( ImGui::InputInt( L( "HTTP-Port" ), &bridgeHttpPort, 0 ) )
	{
		bridgeHttpPort = Clamp( bridgeHttpPort, 1024, 65535 );
		SaveSettings();
	}
	ImGui::TextDisabled( L( "OSC-Port %d (oben), Manifest: http://<dieser Rechner>:%d/manifest.json" ), show.output.oscPort, bridgeHttpPort );
	if( bridgeOn )
	{
		if( bridge.Running() ) ImGui::TextColored( ImVec4( 0.4f, 1, 0.5f, 1 ), L( "HTTP läuft auf Port %d   Manifest-Rev %u" ), bridge.HttpPort(), bridge.Rev() );
		else ImGui::TextColored( ImVec4( 1, 0.55f, 0.3f, 1 ), "%s", bridge.error.empty() ? L( "startet..." ) : bridge.error.c_str() );
	}
	const auto clients = bridge.Clients();
	if( clients.empty() ) ImGui::TextDisabled( L( "Keine Geräte verbunden." ) );
	for( auto& c : clients )
	{
		const double ago = Now() - c.lastSeen;
		const ImVec4 col = ago < 3 ? ImVec4( 0.4f, 1, 0.5f, 1 ) : ago < 8 ? ImVec4( 1, 0.85f, 0.3f, 1 ) : ImVec4( 1, 0.4f, 0.3f, 1 );
		ImGui::TextColored( col, "%s", c.name.c_str() );
		ImGui::SameLine();
		ImGui::TextDisabled( L( "%s   Abos: %s   vor %.1f s" ), c.Addr().c_str(), c.kinds.c_str(), ago );
	}
	ImGui::TextDisabled( L( "In Ableton das Device \"LightStage Control\" auf eine Spur ziehen - es verbindet sich automatisch." ) );
	{
		int n = 0, on = 0;
		for( auto& t : show.ableton )
		{
			++n;
			on += AbletonConnected( t.id ) ? 1 : 0;
		}
		char bl[ 96 ];
		snprintf( bl, sizeof( bl ), L( "Belegungen der Devices (%d, %d verbunden)..." ), n, on );
		if( ImGui::Button( bl ) ) ablWin_ = true;
		ImGui::SetItemTooltip( L( "Alle Zuordnungen der Ableton-Devices, die LightStage mit der Show speichert" ) );
	}
	if( !clients.empty() && show.tempo.source == TempoSource::Internal && !bridgeHinted_ )
	{
		bridgeHinted_ = true;
		SetStatus( L( "Tipp: Tempo auf Ableton Link stellen (Tab Effekte)" ) );
	}
	if( ImGui::Button( L( "Manifest exportieren..." ) ) ) ExportManifest();
	ImGui::SameLine();
	if( ImGui::Button( L( "Im Browser öffnen" ) ) )
	{
		const std::string url = "http://127.0.0.1:" + std::to_string( bridgeHttpPort ) + "/";
		ShellExecuteA( nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL );
	}
}

//---------------------------------------------------------------- Ableton device tables (BRIDGE_PROTOKOLL 2.6 / 2.7)
//LightStage keeps every assignment of the Max for Live devices in named tables ("Control 1", "DMX 1", "Cues 1").
//Control tables can be edited here (knobs / buttons); DMX and Cues tables are kept as the device sends them.
static std::string NextTableLabel( Show& show, const std::string& kind )
{
	const std::string base = kind == "dmx" ? "DMX" : kind == "cues" ? "Cues" : "Control";
	for( int n = 1;; ++n )
	{
		const std::string l = base + " " + std::to_string( n );
		if( !show.FindTable( kind, l ) ) return l;
	}
}
//Control tables for the "Ableton-Regler" menu as (label, bound device id); a connected Control device without
//a table (old device version) appears as "@<id>" and gets a table on the first assignment
std::vector< std::pair< std::string, std::string > > App::AbletonDevices()
{
	std::vector< std::pair< std::string, std::string > > r;
	for( auto& m : show.ableton )
		if( m.kind == "control" ) r.push_back( { m.label, m.id } );
	for( auto& c : bridge.Clients() )
		if( c.id.compare( 0, 4, "ctl-" ) == 0 && c.id.find( '~' ) == std::string::npos && !show.FindAbleton( c.id ) ) r.push_back( { "@" + c.id, c.id } );
	return r;
}
bool App::AbletonConnected( const std::string& id )
{
	if( id.empty() ) return false;
	for( auto& c : bridge.Clients() )
		if( c.id == id ) return true;
	return false;
}
void App::AbletonAssign( const std::string& label, const std::string& id, bool knob, int slot, const std::string& key )
{
	AbletonMap* m = label.empty() || label[ 0 ] == '@' ? nullptr : show.FindTable( "control", label );
	if( !m )
	{
		AbletonMap n;
		n.kind  = "control";
		n.label = NextTableLabel( show, "control" );
		n.id    = id;
		show.ableton.push_back( n );
		m = &show.ableton.back();
	}
	if( !key.empty() )//one place per table
	{
		for( auto& k : m->knobs )
			if( k == key ) k.clear();
		for( auto& b : m->buttons )
			if( b == key ) b.clear();
	}
	( knob ? m->knobs[ slot ] : m->buttons[ slot ] ) = key;
	show.dirty = true;
	const int n = bridge.PushMap( *m );
	if( !n ) m->knobsNewer = true;//device away: this table wins on its next connect
	const std::string what = key.empty() ? L( "frei" ) : TargetLabel( key );
	SetStatus( m->label + ( knob ? L( " Regler " ) : L( " Taster " ) ) + std::to_string( slot + 1 ) + ": " + what + ( n ? "" : L( "  (Device nicht verbunden: kommt beim Verbinden an)" ) ) );
}
std::string App::TargetLabel( const std::string& key )
{
	if( Now() - tlT_ > 1.0 )
	{
		tl_.clear();
		for( auto& t : CollectMidiTargets( show ) )
		{
			std::string l = t.label;
			if( t.kind == "fixtureAttr" )//"Dimmer" alone says nothing: lamp name from the category ("Lampen › Vorne links")
			{
				const size_t p = t.category.rfind( "\xE2\x80\xBA " );
				const std::string lamp = p == std::string::npos ? t.category : t.category.substr( p + 4 );
				if( !lamp.empty() ) l = lamp + " · " + l;
			}
			tl_[ t.key ] = l;
		}
		tlT_ = Now();
	}
	auto it = tl_.find( key );
	return it == tl_.end() ? std::string() : it->second;
}
void App::AbletonAssignMenu( const std::string& key )
{
	const auto devs = AbletonDevices();
	if( !ImGui::BeginMenu( L( "Ableton-Regler" ), !devs.empty() ) )
	{
		if( devs.empty() ) ImGui::SetItemTooltip( L( "Noch keine Control-Belegung: LightStage Control in Ableton laden und verbinden" ) );
		return;
	}
	auto slotName = [ & ]( const std::string& k ) {
		if( k.empty() ) return std::string( "-" );
		const std::string l = TargetLabel( k );
		return l.empty() ? k : l;
	};
	bool anywhere = false;
	for( auto& d : devs )
	{
		ImGui::PushID( d.first.c_str() );
		const bool fresh = d.first[ 0 ] == '@';
		const std::string head = ( fresh ? L( "Neue Belegung (" ) + d.second + ")" : d.first ) + ( AbletonConnected( d.second ) ? "" : L( "  (nicht verbunden)" ) );
		if( ImGui::BeginMenu( head.c_str() ) )
		{
			const AbletonMap* m = fresh ? nullptr : show.FindTable( "control", d.first );
			int freeK = -1, freeB = -1;
			for( int i = 0; i < 16 && freeK < 0; ++i )
				if( !m || m->knobs[ i ].empty() ) freeK = i;
			for( int i = 0; i < 8 && freeB < 0; ++i )
				if( !m || m->buttons[ i ].empty() ) freeB = i;
			int pick = -1;
			bool knob = true;
			std::string val = key;
			if( ImGui::MenuItem( L( "Nächster freier Regler" ), nullptr, false, freeK >= 0 ) ) pick = freeK;
			if( ImGui::MenuItem( L( "Nächster freier Taster" ), nullptr, false, freeB >= 0 ) )
			{
				pick = freeB;
				knob = false;
			}
			ImGui::Separator();
			for( int i = 0; i < 16; ++i )
			{
				const std::string cur = m ? m->knobs[ i ] : std::string();
				char l[ 200 ];
				snprintf( l, sizeof( l ), L( "Regler %d   %s##k%d" ), i + 1, slotName( cur ).c_str(), i );
				if( ImGui::MenuItem( l, nullptr, cur == key ) )
				{
					pick = i;
					knob = true;
					val  = cur == key ? std::string() : key;
				}
			}
			ImGui::Separator();
			for( int i = 0; i < 8; ++i )
			{
				const std::string cur = m ? m->buttons[ i ] : std::string();
				char l[ 200 ];
				snprintf( l, sizeof( l ), L( "Taster %d   %s##b%d" ), i + 1, slotName( cur ).c_str(), i );
				if( ImGui::MenuItem( l, nullptr, cur == key ) )
				{
					pick = i;
					knob = false;
					val  = cur == key ? std::string() : key;
				}
			}
			ImGui::EndMenu();
			if( pick >= 0 ) AbletonAssign( d.first, d.second, knob, pick, val );
		}
		ImGui::PopID();
	}
	for( auto& m : show.ableton )
		if( m.kind == "control" )
		{
			for( auto& k : m.knobs ) anywhere = anywhere || k == key;
			for( auto& b : m.buttons ) anywhere = anywhere || b == key;
		}
	if( anywhere )
	{
		ImGui::Separator();
		if( ImGui::MenuItem( L( "Zuweisung entfernen" ) ) )
			for( auto& m : show.ableton )
			{
				if( m.kind != "control" ) continue;
				bool ch = false;
				for( auto& k : m.knobs )
					if( k == key ) { k.clear(); ch = true; }
				for( auto& b : m.buttons )
					if( b == key ) { b.clear(); ch = true; }
				if( ch )
				{
					show.dirty = true;
					if( !bridge.PushMap( m ) ) m.knobsNewer = true;
				}
			}
	}
	ImGui::EndMenu();
}
void App::AbletonTable()
{
	if( show.ableton.empty() && AbletonDevices().empty() )
	{
		ImGui::TextDisabled( L( "Noch keine Belegungen. Ein LightStage-Device in Ableton laden - es legt beim Verbinden seine Belegung hier an." ) );
		return;
	}
	if( !ImGui::TreeNodeEx( L( "Belegungen der Ableton-Devices" ), ImGuiTreeNodeFlags_DefaultOpen ) ) return;
	ImGui::TextDisabled( L( "LightStage speichert alle Zuordnungen der Devices mit der Show; ein Device holt sich seine Belegung beim Verbinden.\nControl-Regler zuweisen: Rechtsklick auf Parameter, Lampen-Attribut oder DMX-Kanal > Ableton-Regler." ) );
	ImGui::TextDisabled( L( "Show-ID %s" ), show.showId.c_str() );
	std::string delKind, delLabel;
	for( const char* kind : { "control", "dmx", "cues" } )
		for( auto& t : show.ableton )
		{
			if( t.kind != kind ) continue;
			ImGui::PushID( ( t.kind + "|" + t.label ).c_str() );
			const bool con = AbletonConnected( t.id );
			ImGui::TextColored( con ? ImVec4( 0.4f, 1, 0.5f, 1 ) : ImVec4( 0.55f, 0.58f, 0.64f, 1 ), "%s", con ? L( "verbunden" ) : L( "nicht verbunden" ) );
			ImGui::SameLine();
			ImGui::TextUnformatted( t.label.c_str() );
			ImGui::SameLine();
			ImGui::TextDisabled( L( "%s   %s   Stand %.1f kB%s" ), t.id.empty() ? L( "(kein Device)" ) : t.id.c_str(), t.kind == "control" ? "" : ( t.kind == "dmx" ? L( "Kanalbelegung" ) : L( "Notenbelegung" ) ),
			                     t.state.size() / 1024.0, t.knobsNewer ? L( "   Regler hier geändert" ) : "" );
			if( !con )
			{
				ImGui::SameLine();
				if( ImGui::SmallButton( L( "Belegung löschen" ) ) )
				{
					delKind  = t.kind;
					delLabel = t.label;
				}
			}
			if( t.kind == "control" )
			{
				AbletonMap* m = &t;
				auto cell     = [ & ]( bool knob, int i ) {
                    const std::string k = knob ? m->knobs[ i ] : m->buttons[ i ];
                    if( k.empty() )
                    {
                        ImGui::TextDisabled( "-" );
                        return;
                    }
                    if( ImGui::SmallButton( ( std::string( "x##" ) + ( knob ? "k" : "b" ) + std::to_string( i ) ).c_str() ) )
                    {
                        AbletonAssign( m->label, m->id, knob, i, "" );
                        return;
                    }
                    ImGui::SameLine();
                    const std::string l = TargetLabel( k );
                    if( l.empty() ) ImGui::TextColored( ImVec4( 1, 0.6f, 0.3f, 1 ), L( "%s (fehlt in der Show)" ), k.c_str() );
                    else ImGui::TextUnformatted( l.c_str() );
				};
				const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
				if( ImGui::TreeNodeEx( L( "Regler und Taster" ), ImGuiTreeNodeFlags_DefaultOpen ) )
				{
					if( ImGui::BeginTable( "slots", 4, tf, ImVec2( std::min( ImGui::GetContentRegionAvail().x, 900 * ui ), 0 ) ) )
					{
						ImGui::TableSetupColumn( L( "Regler" ), ImGuiTableColumnFlags_WidthFixed, 56 * ui );
						ImGui::TableSetupColumn( L( "Ziel" ), ImGuiTableColumnFlags_WidthStretch, 1.0f );
						ImGui::TableSetupColumn( L( "Taster" ), ImGuiTableColumnFlags_WidthFixed, 56 * ui );
						ImGui::TableSetupColumn( L( "Ziel " ), ImGuiTableColumnFlags_WidthStretch, 1.0f );
						ImGui::TableHeadersRow();
						for( int r = 0; r < 16; ++r )
						{
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							ImGui::Text( "%d", r + 1 );
							ImGui::TableNextColumn();
							cell( true, r );
							ImGui::TableNextColumn();
							if( r < 8 ) ImGui::Text( "%d", r + 1 );
							ImGui::TableNextColumn();
							if( r < 8 ) cell( false, r );
						}
						ImGui::EndTable();
					}
					ImGui::TreePop();
				}
			}
			ImGui::PopID();
		}
	for( auto& c : bridge.Clients() )//connected devices without a table (old device version)
		if( c.id.find( '~' ) == std::string::npos && !show.FindAbleton( c.id ) )
			ImGui::TextDisabled( L( "%s (%s): noch keine Belegung - Device aktualisieren oder dort Verbinden drücken" ), c.name.c_str(), c.id.c_str() );
	if( !delLabel.empty() )
	{
		show.ableton.erase( std::remove_if( show.ableton.begin(), show.ableton.end(), [ & ]( const AbletonMap& m ) { return m.kind == delKind && m.label == delLabel; } ), show.ableton.end() );
		show.dirty = true;
	}
	ImGui::TreePop();
}

void App::AbletonWindow()
{
	if( !ablWin_ ) return;
	ImGui::SetNextWindowSize( ImVec2( 900 * ui, 760 * ui ), ImGuiCond_FirstUseEver );
	if( ImGui::Begin( L( "Ableton-Belegungen" ), &ablWin_ ) ) AbletonTable();
	ImGui::End();
}

//lists every learnable target of the setup (same list as the manifest); double click = MIDI learn for it
void App::TargetsWindow()
{
	if( !targetsWin_ ) return;
	ImGui::SetNextWindowSize( ImVec2( 820 * ui, 640 * ui ), ImGuiCond_FirstUseEver );
	if( !ImGui::Begin( L( "Lernbare Ziele" ), &targetsWin_ ) )
	{
		ImGui::End();
		return;
	}
	ImGui::TextDisabled( L( "Doppelklick auf ein Ziel = MIDI lernen. Gleiche Liste wie im Ableton-Manifest." ) );
	ImGui::SetNextItemWidth( 360 * ui );
	ImGui::InputTextWithHint( "##tgtfilter", L( "Suchen (Name, Kategorie, Adresse)" ), targetFilter_, sizeof( targetFilter_ ) );
	const std::string flt = ToLower( targetFilter_ );
	const auto all        = CollectMidiTargets( show );
	std::vector< int > idx;
	for( int i = 0; i < (int)all.size(); ++i )
	{
		if( !flt.empty() )
		{
			const std::string hay = ToLower( all[ i ].category + " " + all[ i ].label + " " + all[ i ].addr );
			if( hay.find( flt ) == std::string::npos ) continue;
		}
		idx.push_back( i );
	}
	ImGui::SameLine();
	ImGui::TextDisabled( L( "%d von %d Zielen" ), (int)idx.size(), (int)all.size() );
	if( ImGui::BeginTable( "tgts", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp, ImVec2( 0, ImGui::GetContentRegionAvail().y ) ) )
	{
		ImGui::TableSetupScrollFreeze( 0, 1 );
		ImGui::TableSetupColumn( L( "Kategorie" ), ImGuiTableColumnFlags_WidthStretch, 1.0f );
		ImGui::TableSetupColumn( L( "Ziel" ), ImGuiTableColumnFlags_WidthStretch, 1.6f );
		ImGui::TableSetupColumn( L( "OSC-Adresse" ), ImGuiTableColumnFlags_WidthStretch, 1.3f );
		ImGui::TableHeadersRow();
		ImGuiListClipper clip;
		clip.Begin( (int)idx.size() );
		while( clip.Step() )
			for( int r = clip.DisplayStart; r < clip.DisplayEnd; ++r )
			{
				const MidiTargetInfo& t = all[ idx[ r ] ];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex( 0 );
				ImGui::TextUnformatted( t.category.c_str() );
				ImGui::TableSetColumnIndex( 1 );
				ImGui::PushID( r );
				if( ImGui::Selectable( t.label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick ) && ImGui::IsMouseDoubleClicked( 0 ) )
				{
					learnTmpl_  = MidiMapForTarget( t );
					learnArmed_ = true;
					learn_      = true;
					learnText_  = L( "Bewege jetzt einen MIDI-Regler für: " ) + learnTmpl_.Describe( show );
					SetStatus( L( "MIDI lernen: " ) + t.label );
				}
				ImGui::PopID();
				ImGui::TableSetColumnIndex( 2 );
				ImGui::TextDisabled( "%s", t.addr.c_str() );
			}
		ImGui::EndTable();
	}
	ImGui::End();
}

//---------------------------------------------------------------- stage
//2.1 reflection of one surface: on/off, strength, matt <-> glossy. Only shown by quality "high": greyed out otherwise.
bool App::SurfaceUi( const char* id, const char* label, Surface& sf )
{
	bool ch = false;
	ImGui::PushID( id );
	const bool off = quality_ < 2;
	ImGui::BeginDisabled( off );
	ch |= ImGui::Checkbox( label, &sf.reflect );
	if( off ) ImGui::SetItemTooltip( "%s", L( "Nur bei Darstellungsqualität Hoch (Viewport-Leiste)" ) );
	else ImGui::SetItemTooltip( "%s", L( "Boden spiegelt Lichtkegel, Lampen und Bühne (Darstellung, kein DMX)" ) );
	ImGui::SameLine( std::max( ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 8 * ui, 120 * ui ) );//sliders line up
	ImGui::BeginDisabled( !sf.reflect );
	ImGui::SetNextItemWidth( 80 * ui );
	ch |= ls::rs::SliderFloat( "##amt", &sf.amount, 0, 1, L( "Stärke %.2f" ) );
	ImGui::SetItemTooltip( "%s", L( "Stärke der Spiegelung" ) );
	ImGui::SameLine();
	ImGui::SetNextItemWidth( 80 * ui );
	ch |= ls::rs::SliderFloat( "##gloss", &sf.gloss, 0, 1, L( "Glanz %.2f" ) );
	ImGui::SetItemTooltip( "%s", L( "Oberfläche: 0 = matt (weich verschwommen), 1 = glänzend (scharf wie nasser Boden)" ) );
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	ImGui::PopID();
	return ch;
}
void App::TabStage()
{
	StageCfg& s = show.stage;
	bool ch     = false;
	ImGui::BeginChild( "st1", ImVec2( ImGui::GetContentRegionAvail().x * 0.28f, 0 ), ImGuiChildFlags_Borders );
	ImGui::TextUnformatted( L( "Bühne & Atmosphäre" ) );
	ImGui::PushItemWidth( 170 );
	ch |= ImGui::Checkbox( L( "Podest" ), &s.deck );
	ImGui::SameLine();
	ch |= ImGui::Checkbox( L( "Rückwand" ), &s.backWall );
	ImGui::SameLine();
	ch |= ImGui::Checkbox( L( "Boden" ), &s.floor );
	ch |= ls::rs::DragFloat( L( "Breite (m)" ), &s.width, 0.05f, 1, 60 );
	ch |= ls::rs::DragFloat( L( "Tiefe (m)" ), &s.depth, 0.05f, 1, 40 );
	ch |= ls::rs::DragFloat( L( "Podesthöhe (m)" ), &s.deckHeight, 0.01f, 0, 4 );
	ch |= ls::rs::DragFloat( L( "Wandhöhe (m)" ), &s.wallHeight, 0.05f, 1, 30 );
	ch |= ImGui::ColorEdit3( L( "Bodenfarbe" ), &s.floorColor.x, ImGuiColorEditFlags_NoInputs );
	ImGui::SameLine();
	ch |= ImGui::ColorEdit3( L( "Wandfarbe" ), &s.wallColor.x, ImGuiColorEditFlags_NoInputs );
	ImGui::SeparatorText( quality_ < 2 ? L( "Spiegelung (nur Qualität Hoch)" ) : L( "Spiegelung" ) );
	ch |= SurfaceUi( "floor", L( "Boden" ), s.floorSurf );
	ch |= SurfaceUi( "deck", L( "Podest" ), s.deckSurf );
	ch |= ls::rs::SliderFloat( "Haze", &s.haze, 0, 1 );
	ch |= ls::rs::SliderFloat( L( "Haze-Struktur" ), &s.hazeNoise, 0, 1 );
	ImGui::SeparatorText( L( "Umgebung" ) );
	ch |= ls::rs::SliderFloat( L( "Umgebungslicht" ), &s.ambient, 0, 0.3f );
	ch |= ls::rs::SliderFloat( L( "Umgebung x" ), &s.envLight, 0, 4, "%.2f", ImGuiSliderFlags_Logarithmic );
	ImGui::SetItemTooltip( L( "Helligkeit der Umgebung (Raum, Boden, Himmel) - unabhängig von den Scheinwerfern" ) );
	ch |= ImGui::ColorEdit3( L( "Farbe Umgebung" ), &s.ambientColor.x, ImGuiColorEditFlags_NoInputs );
	{
		struct P { const char* n; V3 c; };
		const P ps[] = { { "Neutral", V3( 1, 1, 1 ) }, { "Warm", V3( 1, 0.85f, 0.7f ) }, { L( "Nacht" ), V3( 0.35f, 0.45f, 0.8f ) }, { L( "Mond" ), V3( 0.6f, 0.7f, 1.0f ) } };
		for( int i = 0; i < 4; ++i )
		{
			if( i ) ImGui::SameLine();
			if( ImGui::SmallButton( ps[ i ].n ) ) { s.ambientColor = ps[ i ].c; ch = true; }
		}
	}
	ImGui::SeparatorText( L( "Ort & Stimmung" ) );
	{
		int mood = -1;
		if( ImGui::BeginCombo( L( "Stimmung" ), L( "(Voreinstellung wählen)" ) ) )
		{
			for( int i = 0; i < 5; ++i )
				if( ImGui::Selectable( Ls( kMoodNames[ i ] ) ) ) mood = i;
			ImGui::EndCombo();
		}
		if( mood >= 0 ) { ApplyMood( s, mood ); ch = true; }
		ch |= ImGui::Combo( L( "Ort" ), &s.environment, L( "Halle\0Outdoor\0" ) );
		ch |= ImGui::Combo( L( "Untergrund" ), &s.ground, L( "Hallenboden\0Rasen\0Beton\0Asphalt\0Erde / Kies\0Holzboden\0" ) );
		ch |= ls::rs::DragFloat( L( "Bodengröße (m)" ), &s.groundSize, 1.0f, 10.0f, 1000.0f, "%.0f" );
		if( s.environment == 1 )
		{
			ch |= ImGui::ColorEdit3( L( "Himmel oben" ), &s.skyZenith.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_HDR );
			ImGui::SameLine();
			ch |= ImGui::ColorEdit3( L( "Horizont" ), &s.skyHorizon.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_HDR );
			ch |= ls::rs::SliderFloat( L( "Sterne" ), &s.stars, 0, 1 );
			ch |= ls::rs::SliderFloat( L( "Mond" ), &s.moon, 0, 1 );
			float az[ 2 ] = { s.moonAz, s.moonEl };
			if( ls::rs::DragFloat2( L( "Mond Richtung / Höhe" ), az, 0.5f, -180, 180, "%.0f°" ) ) { s.moonAz = az[ 0 ]; s.moonEl = Clamp( az[ 1 ], 2.0f, 89.0f ); ch = true; }
			ch |= ImGui::Checkbox( L( "Mond wirft Schatten" ), &s.moonShadow );
			ch |= ls::rs::SliderFloat( L( "Dunst in der Ferne" ), &s.distanceFog, 0, 1 );
		}
	}
	ImGui::SeparatorText( L( "Kamera" ) );
	ch |= ls::rs::SliderFloat( L( "Belichtung" ), &s.exposure, 0.1f, 4 );
	ch |= ls::rs::SliderFloat( "Bloom", &s.bloom, 0, 2 );
	ch |= ls::rs::SliderFloat( L( "Kegel-Qualität" ), &s.beamQuality, 0.25f, 3 );
	ImGui::PopItemWidth();
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild( "st2", ImVec2( ImGui::GetContentRegionAvail().x * 0.62f - 4, 0 ), ImGuiChildFlags_Borders );
	//---- extra decks / risers
	ImGui::TextUnformatted( L( "Podeste" ) );
	ImGui::SameLine();
	if( ImGui::SmallButton( L( "+ Podest" ) ) ) ImGui::OpenPopup( "deckadd" );
	ImGui::SetItemTooltip( L( "Weitere Podeste, Riser, Laufsteg ... für eine komplexere Bühne" ) );
	static int openDeck = -1;
	if( ImGui::BeginPopup( "deckadd" ) )
	{
		const float sd = show.stage.depth * 0.5f, dh = show.stage.deck ? show.stage.deckHeight : 0.0f;
		struct Pre { const char* n; Deck d; };
		auto mk = [ & ]( const char* n, int shape, float x, float z, float w, float d, float h, bool on, int stairs, V3 edge, float lvl ) {
			Deck k;
			k.name      = Ls( n );
			k.shape     = shape;
			k.x         = x;
			k.z         = z;
			k.width     = w;
			k.depth     = d;
			k.height    = h;
			k.onDeck    = on;
			k.stairs    = stairs;
			k.edge      = edge;
			k.edgeLevel = lvl;
			return Pre{ n, k };
		};
		const Pre pres[] = {
			mk( "Riser", 0, 0, -sd * 0.4f, 2.0f, 1.0f, 0.4f, show.stage.deck, 0, V3( 0, 0, 0 ), 0 ),
			mk( "Drum-Riser", 0, 0, -sd * 0.45f, 3.0f, 2.5f, 0.6f, show.stage.deck, 1, V3( 0, 0, 0 ), 0 ),
			mk( "DJ-Pult-Podest", 0, 0, -sd * 0.2f, 2.4f, 1.4f, 0.8f, show.stage.deck, 2, V3( 0.2f, 0.45f, 1.0f ), 1.5f ),
			mk( "Laufsteg", 0, 0, sd + 4.0f, 1.6f, 8.0f, std::max( 0.4f, dh ), false, 0, V3( 0, 0, 0 ), 0 ),
			mk( "Seitenbühne links", 0, -show.stage.width * 0.5f - 2.0f, 0, 4.0f, show.stage.depth * 0.8f, std::max( 0.4f, dh ), false, 1, V3( 0, 0, 0 ), 0 ),
			mk( "Seitenbühne rechts", 0, show.stage.width * 0.5f + 2.0f, 0, 4.0f, show.stage.depth * 0.8f, std::max( 0.4f, dh ), false, 1, V3( 0, 0, 0 ), 0 ),
			mk( "Rundes Podest", 1, 0, 0, 3.0f, 3.0f, 0.3f, show.stage.deck, 0, V3( 1.0f, 0.55f, 0.2f ), 1.2f ),
			mk( "B-Bühne (rund)", 1, 0, sd + 9.0f, 5.0f, 5.0f, std::max( 0.6f, dh ), false, 3, V3( 0, 0, 0 ), 0 ),
		};
		for( auto& p : pres )
			if( ImGui::MenuItem( Ls( p.n ) ) )
			{
				show.stage.decks.push_back( p.d );
				openDeck = (int)show.stage.decks.size() - 1;
				ch       = true;
			}
		ImGui::EndPopup();
	}
	int delDeck = -1, dupDeck = -1;
	for( int i = 0; i < (int)show.stage.decks.size(); ++i )
	{
		Deck& d = show.stage.decks[ i ];
		ImGui::PushID( 3000 + i );
		const float rowY0 = ImGui::GetCursorScreenPos().y;
		if( openDeck == i ) { ImGui::SetNextItemOpen( true ); openDeck = -1; }
		char hdr[ 160 ];
		snprintf( hdr, sizeof( hdr ), "%s  %.1f x %.1f m, H %.2f###deck", d.name.c_str(), d.width, d.depth, d.height );
		const bool open = ImGui::TreeNodeEx( hdr, ImGuiTreeNodeFlags_SpanAvailWidth );
		if( DelRow( rowY0 ) ) delDeck = i;//Delete / Backspace over the deck
		if( ImGui::BeginPopupContextItem( "dctx" ) )
		{
			if( ImGui::MenuItem( L( "Duplizieren" ) ) ) dupDeck = i;
			if( ImGui::MenuItem( L( "Löschen" ) ) ) delDeck = i;
			ImGui::EndPopup();
		}
		if( open )
		{
			const float w3 = 120 * ui;
			char nm[ 64 ];
			snprintf( nm, sizeof( nm ), "%s", d.name.c_str() );
			ImGui::SetNextItemWidth( w3 );
			if( ImGui::InputText( "##n", nm, sizeof( nm ) ) ) { d.name = nm; ch = true; }
			ImGui::SameLine();
			ImGui::SetNextItemWidth( 100 * ui );
			ch |= ImGui::Combo( "##shape", &d.shape, L( "Rechteckig\0Rund\0" ) );
			ImGui::SameLine();
			if( ImGui::Checkbox( L( "auf der Bühne" ), &d.onDeck ) ) { if( !d.onDeck ) d.y = 0; ch = true; }
			ImGui::SetItemTooltip( L( "Steht auf dem Hauptpodest (Höhe zählt ab dessen Oberkante)" ) );
			ImGui::SameLine();
			if( ImGui::SmallButton( "x" ) ) delDeck = i;
			float xyz[ 3 ] = { d.x, show.DeckBase( d ), d.z };
			ImGui::SetNextItemWidth( w3 * 1.4f );
			if( ls::rs::DragFloat3( "##xyz", xyz, 0.05f, 0, 0, "%.2f" ) )
			{
				d.x = xyz[ 0 ];
				d.z = xyz[ 2 ];
				if( fabsf( xyz[ 1 ] - show.DeckBase( d ) ) > 1e-5f ) { d.y = std::max( 0.0f, xyz[ 1 ] ); d.onDeck = false; }//own height: no longer on the main deck
				ch = true;
			}
			ImGui::SetItemTooltip( L( "Position X / Y / Z (m) - Y = Unterkante (0 = Boden)" ) );
			ImGui::SameLine();
			float wd[ 2 ] = { d.width, d.depth };
			ImGui::SetNextItemWidth( w3 );
			if( ls::rs::DragFloat2( "##wd", wd, 0.02f, 0.1f, 100, "%.2f" ) ) { d.width = std::max( 0.1f, wd[ 0 ] ); d.depth = std::max( 0.1f, wd[ 1 ] ); ch = true; }
			ImGui::SetItemTooltip( d.shape == 1 ? L( "Durchmesser X / Z (m)" ) : L( "Breite / Tiefe (m)" ) );
			ImGui::SameLine();
			ImGui::SetNextItemWidth( 72 * ui );
			ch |= ls::rs::DragFloat( "##h", &d.height, 0.01f, 0, 6, "H %.2f m" );
			ImGui::SetItemTooltip( L( "Höhe über dem, worauf es steht" ) );
			ImGui::SameLine();
			ImGui::SetNextItemWidth( 56 * ui );
			ch |= ls::rs::DragFloat( "##yaw", &d.yaw, 0.5f, -180, 180, "%.0f°" );
			ImGui::SetItemTooltip( L( "Drehung" ) );
			ImGui::SetNextItemWidth( w3 );
			ch |= ImGui::Combo( "##stairs", &d.stairs, L( "keine Treppe\0Treppe vorne\0Treppe hinten\0Treppe links\0Treppe rechts\0" ) );
			ImGui::SameLine();
			ch |= ImGui::Checkbox( L( "Blende" ), &d.skirt );
			ImGui::SetItemTooltip( L( "Schwarze Bühnenblende rundum" ) );
			ImGui::SameLine();
			ch |= ImGui::Checkbox( L( "Farbe" ), &d.ownColor );
			if( d.ownColor )
			{
				ImGui::SameLine();
				ch |= ImGui::ColorEdit3( "##col", &d.color.x, ImGuiColorEditFlags_NoInputs );
			}
			ImGui::SameLine();
			ch |= ImGui::ColorEdit3( "##edge", &d.edge.x, ImGuiColorEditFlags_NoInputs );
			ImGui::SetItemTooltip( L( "LED-Kante: Farbe" ) );
			ImGui::SameLine();
			ImGui::SetNextItemWidth( 80 * ui );
			ch |= ls::rs::SliderFloat( "##edgel", &d.edgeLevel, 0, 4, L( "LED %.1f" ) );
			ImGui::SetItemTooltip( L( "LED-Kante vorne (rund: ringsum), 0 = aus" ) );
			ch |= SurfaceUi( "surf", L( "Spiegelung" ), d.surf );
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
	if( dupDeck >= 0 )
	{
		Deck c = show.stage.decks[ dupDeck ];
		c.x += 0.5f;
		c.z += 0.5f;
		c.name += L( " Kopie" );
		show.stage.decks.push_back( c );
		openDeck = (int)show.stage.decks.size() - 1;
		ch       = true;
	}
	if( delDeck >= 0 ) { show.stage.decks.erase( show.stage.decks.begin() + delDeck ); ch = true; }
	if( show.stage.decks.empty() ) ImGui::TextDisabled( L( "Riser, Drum-Riser, Laufsteg, Seitenbühnen ... über '+ Podest'." ) );
	ImGui::Separator();
	ImGui::TextUnformatted( L( "Traversen" ) );
	ImGui::SameLine();
	if( ImGui::SmallButton( L( "+ Traverse" ) ) )
	{
		Truss t;
		t.a = V3( -5, 6, (float)show.trusses.size() * -1.5f + 2 );
		t.b = V3( 5, 6, t.a.z );
		show.trusses.push_back( t );
		ch = true;
	}
	int del = -1;
	bool anyFree = false;
	for( int i = 0; i < (int)show.trusses.size(); ++i )
	{
		Truss& t = show.trusses[ i ];
		ImGui::PushID( i );
		const float rowY0 = ImGui::GetCursorScreenPos().y;
		ImGui::Text( "%d", i + 1 );
		ImGui::SameLine( 26 * ui );
		if( t.freeEnds )
		{
			//both ends separately: start X / Y / Z, end X / Y / Z
			anyFree = true;
			ImGui::SetNextItemWidth( 150 * ui );
			ch |= ls::rs::DragFloat3( "##a", &t.a.x, 0.05f, 0, 0, "%.2f" );
			ImGui::SetItemTooltip( L( "Anfang X / Y / Z (m)" ) );
			ImGui::SameLine();
			ImGui::SetNextItemWidth( 150 * ui );
			ch |= ls::rs::DragFloat3( "##b", &t.b.x, 0.05f, 0, 0, "%.2f" );
			ImGui::SetItemTooltip( L( "Ende X / Y / Z (m)" ) );
		}
		else
		{
			//one position for the whole truss: centre X / Y / Z, length, rotation around the vertical axis
			V3 c          = ( t.a + t.b ) * 0.5f;
			const V3 d    = t.b - t.a;
			float len     = sqrtf( d.x * d.x + d.z * d.z );
			float yaw     = len > 1e-4f ? atan2f( d.z, d.x ) / kDeg : 0.0f;
			bool edit     = false;
			ImGui::SetNextItemWidth( 150 * ui );
			edit |= ls::rs::DragFloat3( "##c", &c.x, 0.05f, 0, 0, "%.2f" );
			ImGui::SetItemTooltip( L( "Mitte X / Y / Z (m)" ) );
			ImGui::SameLine();
			ImGui::SetNextItemWidth( 64 * ui );
			edit |= ls::rs::DragFloat( "##len", &len, 0.05f, 0.2f, 60.0f, L( "L %.1f m" ) );
			ImGui::SetItemTooltip( L( "Länge" ) );
			ImGui::SameLine();
			ImGui::SetNextItemWidth( 48 * ui );
			edit |= ls::rs::DragFloat( "##yaw", &yaw, 0.5f, -180.0f, 180.0f, "%.0f°" );
			ImGui::SetItemTooltip( L( "Drehung um die Hochachse (0° = entlang X)" ) );
			if( edit )
			{
				len          = std::max( 0.2f, len );
				const V3 dir( cosf( yaw * kDeg ), 0, sinf( yaw * kDeg ) );
				t.a = c - dir * ( len * 0.5f );
				t.b = c + dir * ( len * 0.5f );
				ch  = true;
			}
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth( 42 * ui );
		ch |= ls::rs::DragFloat( "##s", &t.size, 0.005f, 0.1f, 1.0f, "%.2f" );
		ImGui::SetItemTooltip( L( "Größe (Querschnitt, m)" ) );
		ImGui::SameLine();
		if( ImGui::Checkbox( L( "frei" ), &t.freeEnds ) )
		{
			if( !t.freeEnds ) t.a.y = t.b.y = ( t.a.y + t.b.y ) * 0.5f;//one height again
			ch = true;
		}
		ImGui::SetItemTooltip( L( "Beide Enden einzeln positionieren (schräge Traversen)" ) );
		ImGui::SameLine();
		if( ImGui::SmallButton( "x" ) || DelRow( rowY0 ) ) del = i;
		ImGui::PopID();
	}
	ImGui::TextDisabled( anyFree ? L( "Mitte X / Y / Z, Länge, Drehung - frei: Anfang X / Y / Z, Ende X / Y / Z (m)" ) : L( "Mitte X / Y / Z, Länge, Drehung, Größe in Metern" ) );
	if( del >= 0 ) { show.RemoveTruss( del ); ch = true; }
	ImGui::Separator();
	ImGui::TextUnformatted( L( "Stative" ) );
	ImGui::SameLine();
	if( ImGui::SmallButton( L( "+ Stativ" ) ) )
	{
		show.AddStand();
		ch = true;
	}
	int dels = -1;
	for( int i = 0; i < (int)show.stands.size(); ++i )
	{
		Stand& sd = show.stands[ i ];
		ImGui::PushID( 1000 + i );
		const float rowY0 = ImGui::GetCursorScreenPos().y;
		ImGui::Text( "%d", i + 1 );
		ImGui::SameLine( 26 * ui );
		ImGui::SetNextItemWidth( 150 * ui );
		if( ls::rs::DragFloat3( "##xyz", &sd.pos.x, 0.05f, 0, 0, "%.2f" ) ) { sd.pos.y = std::max( 0.0f, sd.pos.y ); ch = true; }
		ImGui::SetItemTooltip( L( "Position X / Y / Z (m) - Y = Standfläche (Boden 0, Podest)" ) );
		ImGui::SameLine();
		ImGui::SetNextItemWidth( 64 * ui );
		ch |= ls::rs::DragFloat( "##h", &sd.height, 0.02f, 1.0f, 4.5f, "H %.2f m" );
		ImGui::SetItemTooltip( L( "Höhe des Stativs (Oberkante) über dem Boden" ) );
		ImGui::SameLine();
		ImGui::SetNextItemWidth( 44 * ui );
		ch |= ls::rs::DragFloat( "##yaw", &sd.yaw, 1.0f, -180, 180, "%.0f°" );
		ImGui::SetItemTooltip( L( "Drehung" ) );
		ImGui::SameLine();
		ImGui::SetNextItemWidth( 52 * ui );
		ch |= ls::rs::DragFloat( "##bar", &sd.bar, 0.01f, 0, 2.5f, "T %.2f" );
		ImGui::SetItemTooltip( L( "Breite der T-Bar (0 = eine Lampe direkt oben)" ) );
		ImGui::SameLine();
		bool onDeck = sd.pos.y > 0.01f;
		if( ImGui::Checkbox( L( "Podest" ), &onDeck ) ) { sd.pos.y = onDeck ? std::max( 0.02f, show.FloorAt( sd.pos.x, sd.pos.z ) ) : 0.0f; ch = true; }
		ImGui::SetItemTooltip( L( "Steht auf dem Podest statt auf dem Hallenboden" ) );
		ImGui::SameLine();
		if( ImGui::SmallButton( "x" ) || DelRow( rowY0 ) ) dels = i;
		int nm = show.MountedCount( MountType::Stand, i );
		if( nm ) { ImGui::SameLine(); ImGui::TextDisabled( L( "%d Lampe%s" ), nm, nm > 1 ? PlN() : "" ); }
		ImGui::PopID();
	}
	if( show.stands.empty() ) ImGui::TextDisabled( L( "Lampen per Inspector > Befestigung auf ein Stativ setzen." ) );
	if( dels >= 0 ) { show.RemoveStand( dels ); ch = true; }
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild( "st3", ImVec2( 0, 0 ), ImGuiChildFlags_Borders );
	ImGui::TextUnformatted( L( "3D-Modelle" ) );
	ImGui::SameLine();
	if( ImGui::SmallButton( L( "+ Importieren..." ) ) )
	{
		wchar_t buf[ MAX_PATH * 4 ] = {};
		OPENFILENAMEW of{};
		of.lStructSize = sizeof( of );
		of.hwndOwner   = (HWND)hwnd;
		of.lpstrFilter = LW( L"3D-Modelle (*.glb;*.gltf;*.fbx;*.obj)\0*.glb;*.gltf;*.fbx;*.obj\0" );
		of.lpstrFile   = buf;
		of.nMaxFile    = MAX_PATH * 4;
		of.Flags       = OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST | OFN_EXPLORER;
		if( GetOpenFileNameW( &of ) ) DropFile( Narrow( buf ) );
	}
	int mdel = -1;
	for( int i = 0; i < (int)show.models.size(); ++i )
	{
		ModelObj& m = show.models[ i ];
		ImGui::PushID( 1000 + i );
		const float rowY0 = ImGui::GetCursorScreenPos().y;
		ImGui::Separator();
		ImGui::TextUnformatted( m.name.c_str() );
		if( m.meshId < 0 ) { ImGui::SameLine(); ImGui::TextColored( ImVec4( 1, 0.4f, 0.3f, 1 ), L( "(nicht geladen)" ) ); }
		ImGui::SameLine();
		if( ImGui::SmallButton( "x" ) || DelRow( rowY0 ) ) mdel = i;
		ImGui::SetNextItemWidth( 200 * ui );
		ch |= ls::rs::DragFloat3( "Pos", &m.pos.x, 0.02f );
		ImGui::SetItemTooltip( "X / Y / Z (m)" );
		ImGui::SetNextItemWidth( 200 * ui );
		ch |= ls::rs::DragFloat3( "Rot", &m.rot.x, 0.5f );//rotation (Pos / Rot)
		ImGui::SetNextItemWidth( 90 * ui );
		ch |= ls::rs::DragFloat( L( "Skalierung" ), &m.scale, 0.005f, 0.001f, 1000, "%.3f" );
		ImGui::SameLine();
		ch |= ImGui::Checkbox( L( "Materialien" ), &m.matColors );
		ImGui::SetItemTooltip( L( "Farben und Texturen aus der Datei verwenden (aus = eine einheitliche Farbe)" ) );
		ImGui::SameLine();
		ImGui::BeginDisabled( m.matColors );
		ch |= ImGui::ColorEdit3( L( "Farbe" ), &m.tint.x, ImGuiColorEditFlags_NoInputs );
		ImGui::EndDisabled();
		if( m.textures > 0 ) { ImGui::SameLine(); ImGui::TextDisabled( L( "%d Textur%s" ), m.textures, m.textures > 1 ? ( gLang == kLangDe ? "en" : "s" ) : "" ); }
		ImGui::SameLine();
		if( ImGui::SmallButton( L( "Neu laden" ) ) )
		{
			ren.ForgetModel( m.path );
			for( auto& o : show.models )
				if( o.path == m.path ) o.meshId = -1;
		}
		ImGui::SetItemTooltip( L( "Datei und Texturen erneut von der Festplatte lesen (z.B. nach neuem Export)" ) );
		if( !m.missingTex.empty() )
		{
			std::string l;
			for( size_t k = 0; k < m.missingTex.size() && k < 4; ++k ) l += ( k ? ", " : "" ) + FileName( m.missingTex[ k ] );
			if( m.missingTex.size() > 4 ) l += ", ...";
			ImGui::TextColored( ImVec4( 1, 0.65f, 0.3f, 1 ), L( "Textur fehlt: %s" ), l.c_str() );
			bool c4d = false;
			for( auto& t : m.missingTex ) c4d |= ToLower( t ).find( "asset:" ) != std::string::npos;
			ImGui::SetItemTooltip( "%s", c4d ? L( "Die Datei verweist auf Cinema-4D-Assets (asset:///...), die nicht mit exportiert wurden.\nIn C4D: Datei > Projekt mit Assets speichern (legt die Texturen in einen tex-Ordner),\ndann von dort exportieren oder die Bilder neben das Modell legen und 'Neu laden'." )
			                                   : L( "Die Bilddatei liegt nicht neben dem Modell bzw. in einem Unterordner (tex, textures, maps ...).\nBild dorthin kopieren und 'Neu laden'." ) );
		}
		ImGui::PopID();
	}
	if( mdel >= 0 ) { show.models.erase( show.models.begin() + mdel ); ch = true; }
	if( show.models.empty() ) ImGui::TextDisabled( L( "glTF/GLB, FBX oder OBJ importieren\n(oder ins Fenster ziehen)." ) );
	ImGui::EndChild();
	if( ch ) show.dirty = true;
}

//---------------------------------------------------------------- snapshots
//names of some lamps for tooltips: "1 Vorne links, 2 ..., (+5)"
static std::string LampNames( Show& show, const std::vector< int >& ids, size_t maxN = 8 )
{
	std::string r;
	size_t n = 0;
	for( int id : ids )
	{
		const FixtureInst* f = show.FindFixture( id );
		if( !f ) continue;
		if( n++ < maxN ) r += ( r.empty() ? "" : ", " ) + f->name;
	}
	if( n > maxN ) r += ", (+" + std::to_string( n - maxN ) + ")";
	return r.empty() ? std::string( L( "keine" ) ) : r;
}
//toggle button with the fade level as a bar; returns true when clicked
static bool SnapButton( const char* label, const Snapshot& s, const ImVec2& size )
{
	const ImVec4 off( 0.16f, 0.18f, 0.23f, 1 ), on( 0.16f, 0.42f, 0.80f, 1 );
	ImGui::PushStyleColor( ImGuiCol_Button, s.active ? on : off );
	ImGui::PushStyleColor( ImGuiCol_ButtonHovered, s.active ? ImVec4( 0.22f, 0.5f, 0.9f, 1 ) : ImVec4( 0.24f, 0.27f, 0.33f, 1 ) );
	const bool r = ImGui::Button( label, size );
	ImGui::PopStyleColor( 2 );
	const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
	const float h  = std::max( 3.0f, ( b.y - a.y ) * 0.08f );
	if( s.cur > 0 )
		ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( a.x + 2, b.y - h - 2 ), ImVec2( a.x + 2 + ( b.x - a.x - 4 ) * s.cur * std::max( 0.04f, s.master ), b.y - 2 ), IM_COL32( 255, 200, 90, 230 ), 1.5f );
	return r;
}
void App::TabSnapshots()
{
	//---- new snapshot from the selected lamps
	std::vector< int > ids;
	for( int id : sel )
		if( show.FindFixture( id ) ) ids.push_back( id );
	ImGui::SetNextItemWidth( 180 * ui );
	ImGui::InputText( "##sn", snapName_, sizeof( snapName_ ) );
	ImGui::SameLine();
	char bl[ 96 ];
	if( ids.empty() ) snprintf( bl, sizeof( bl ), L( "Snapshot speichern (alle %d Lampen)" ), (int)show.fixtures.size() );
	else snprintf( bl, sizeof( bl ), L( "Snapshot speichern (%d Lampe%s)" ), (int)ids.size(), ids.size() == 1 ? "" : PlN() );
	if( ImGui::Button( bl ) )
	{
		std::string n = snapName_;
		if( n.empty() ) n = "Look";
		const int i = show.TakeSnapshotOf( n + " " + std::to_string( show.snapshots.size() + 1 ), ids, snapWithFx_, snapWithProj_ && !show.projectors.empty() );
		SetStatus( "Snapshot \"" + show.snapshots[ i ].name + "\": " + std::to_string( show.snapshots[ i ].fix.size() ) + L( " Lampen, " ) + std::to_string( show.snapshots[ i ].effects.size() ) + L( " Effekte" ) );
	}
	ImGui::SetItemTooltip( L( "Speichert die ausgewählten Lampen (Liste links oder Ansicht).\nOhne Auswahl: alle Lampen.\n%s" ), ids.empty() ? "" : LampNames( show, ids ).c_str() );
	ImGui::SameLine();
	ImGui::Checkbox( L( "Effekte" ), &snapWithFx_ );
	ImGui::SetItemTooltip( L( "Effekte mitspeichern: welche an und welche aus sind.\nSolange der Snapshot an ist, werden sie so geschaltet; danach gilt wieder der eigene Schalter." ) );
	ImGui::SameLine();
	ImGui::BeginDisabled( show.projectors.empty() );
	ImGui::Checkbox( L( "Projektoren" ), &snapWithProj_ );
	ImGui::EndDisabled();
	ImGui::SetItemTooltip( L( "Projektor-Einstellungen mitspeichern (an/aus, Helligkeit, Position, Ausrichtung, Zoom, Shift, Unschärfe)" ) );
	ImGui::SameLine();
	const int act = show.ActiveSnapshots();
	ImGui::BeginDisabled( act == 0 );
	if( ImGui::Button( L( "Alle aus" ) ) ) show.ReleaseAllSnapshots();
	ImGui::EndDisabled();
	OscContext( "/ls/snapshot/off" );
	ImGui::TextDisabled( L( "Mehrere Snapshots laufen gleichzeitig. Gleiche Lampen: der zuletzt gestartete gewinnt und beendet den älteren - mit Shift+Klick bleibt der ältere an." ) );
	if( show.snapshots.empty() )
	{
		ImGui::TextDisabled( L( "Noch keine Snapshots. Lampen auswählen, einstellen, dann \"Snapshot speichern\"." ) );
		return;
	}
	//---- list
	int del = -1, up = -1;
	const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
	if( ImGui::BeginTable( "snaps", 6, tf, ImVec2( std::min( ImGui::GetContentRegionAvail().x, 980 * ui ), ImGui::GetContentRegionAvail().y ) ) )
	{
		ImGui::TableSetupScrollFreeze( 0, 1 );
		ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthFixed, 70 * ui );
		ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthFixed, 200 * ui );
		ImGui::TableSetupColumn( L( "Stärke" ), ImGuiTableColumnFlags_WidthFixed, 230 * ui );
		ImGui::TableSetupColumn( "Fade", ImGuiTableColumnFlags_WidthFixed, 140 * ui );
		ImGui::TableSetupColumn( L( "Inhalt" ), ImGuiTableColumnFlags_WidthFixed, 240 * ui );
		ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthFixed, 34 * ui );
		ImGui::TableHeadersRow();
		for( int i = 0; i < (int)show.snapshots.size(); ++i )
		{
			Snapshot& s = show.snapshots[ i ];
			ImGui::PushID( i );
			ImGui::TableNextRow();
			const float rowY0 = ImGui::GetCursorScreenPos().y;
			ImGui::TableNextColumn();
			if( SnapButton( s.active ? L( "AN" ) : "Start", s, ImVec2( -1, 0 ) ) ) show.ToggleSnapshot( i, ImGui::GetIO().KeyShift );
			ImGui::SetItemTooltip( L( "Starten / beenden (Fade %.1f s).\nShift+Klick: dazuschalten, andere Snapshots mit denselben Lampen bleiben an.\nMIDI-Learn: Note schaltet um." ), s.fade );
			{
				MidiMap t;
				t.target   = MidiTarget::Snapshot;
				t.snapshot = i;
				t.type     = MidiType::Note;
				t.toggle   = true;
				LearnHook( t );
			}
			const std::string base = "/ls/snapshot/" + std::to_string( i + 1 );
			ImGui::TableNextColumn();
			char nm[ 64 ];
			snprintf( nm, sizeof( nm ), "%s", s.name.c_str() );
			ImGui::SetNextItemWidth( -1 );
			if( ImGui::InputText( "##n", nm, sizeof( nm ) ) ) { s.name = nm; show.dirty = true; }
			ImGui::TableNextColumn();
			float pct = s.master * 100.0f;
			ImGui::SetNextItemWidth( -1 );
			if( ls::rs::SliderFloat( "##m", &pct, 0, 100, L( "Stärke %.0f %%" ) ) ) { s.master = Saturate( pct / 100.0f ); show.dirty = true; }
			OscContext( base + "/master" );
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth( -1 );
			if( ls::rs::SliderFloat( "##f", &s.fade, 0.0f, 30.0f, s.fade < 0.05f ? L( "sofort" ) : "Fade %.1f s", ImGuiSliderFlags_Logarithmic ) ) { s.fade = Clamp( s.fade, 0.0f, 600.0f ); show.dirty = true; }
			ImGui::SetItemTooltip( L( "Ein- und Ausblendzeit" ) );
			OscContext( base + "/fade" );
			ImGui::TableNextColumn();
			{
				//grip: drag the snapshot onto the Time Line = a block at the mouse (not on the Start button: a hasty click must always start)
				const float gw = 16 * ui, gh = ImGui::GetFrameHeight();
				const ImVec2 gp = ImGui::GetCursorScreenPos();
				ImGui::InvisibleButton( "##grip", ImVec2( gw, gh ) );
				UiMark( "snap.grip." + std::to_string( i ), ImGui::GetItemRectMin(), ImGui::GetItemRectMax() );
				const bool gHov = ImGui::IsItemHovered() || ImGui::IsItemActive();
				if( gHov ) ImGui::SetMouseCursor( ImGuiMouseCursor_Hand );
				for( int gy = 0; gy < 3; ++gy )
					for( int gx = 0; gx < 2; ++gx )
						ImGui::GetWindowDrawList()->AddCircleFilled( ImVec2( gp.x + ( 5 + gx * 5 ) * ui, gp.y + gh * 0.5f + ( gy - 1 ) * 5 * ui ), 1.4f * ui, gHov ? IM_COL32( 255, 190, 100, 255 ) : IM_COL32( 130, 138, 155, 255 ) );
				ImGui::SetItemTooltip( "%s", L( "Auf die Time Line ziehen (Tab oder Fenster) = Block an der Maus" ) );
				if( ImGui::BeginDragDropSource() )
				{
					const TlClipDrag d{ (int)TlClipKind::Snapshot, s.uid, 0 };
					ImGui::SetDragDropPayload( "LS_TLCLIP", &d, sizeof( d ) );
					ImGui::TextUnformatted( s.name.c_str() );
					ImGui::TextDisabled( "%s", L( "Auf die Time Line ziehen (Tab oder Fenster) = Block an der Maus" ) );
					ImGui::EndDragDropSource();
				}
				ImGui::SameLine( 0, 4 * ui );
				ImGui::AlignTextToFramePadding();
			}
			{
				std::vector< int > sids;
				for( auto& f : s.fix ) sids.push_back( f.id );
				if( const int ov = show.SnapshotOverridden( i ) )
				{
					ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.75f, 0.45f, 0.1f, 1 ) );
					if( ImGui::SmallButton( "G" ) ) show.RecallSnapshot( i, s.keep );
					ImGui::PopStyleColor();
					ImGui::SetItemTooltip( L( "%d Lampe%s: ein Gruppenfader wurde nach dem Start bewegt und hat dort Vorrang.\nKlick: Snapshot neu starten - dann gelten wieder alle seine Werte." ), ov, ov == 1 ? "" : PlN() );
					ImGui::SameLine();
				}
				char info[ 96 ];
				const std::string fxs = s.effects.empty() && s.fxOff.empty() ? std::string() : " · FX " + std::to_string( s.effects.size() ) + L( " an / " ) + std::to_string( s.fxOff.size() ) + L( " aus" );
				snprintf( info, sizeof( info ), L( "%d Lampe%s%s%s" ), (int)s.fix.size(), s.fix.size() == 1 ? "" : PlN(), fxs.c_str(), s.proj.empty() ? "" : ( " · " + std::to_string( s.proj.size() ) + " Proj." ).c_str() );
				if( ImGui::Selectable( info, false ) )
				{
					sel = sids;//click: select its lamps
					SetStatus( L( "Lampen von \"" ) + s.name + L( "\" ausgewählt" ) );
				}
				std::string fxn, fxo;
				for( int id : s.effects )
					if( Effect* e = show.FindEffect( id ) ) fxn += ( fxn.empty() ? "" : ", " ) + e->name;
				for( int id : s.fxOff )
					if( Effect* e = show.FindEffect( id ) ) fxo += ( fxo.empty() ? "" : ", " ) + e->name;
				ImGui::SetItemTooltip( L( "Klick: Lampen auswählen\nLampen: %s\nEffekte an: %s\nEffekte aus: %s" ), LampNames( show, sids ).c_str(), !s.withFx ? L( "(nicht gespeichert)" ) : fxn.empty() ? L( "keine" ) : fxn.c_str(), !s.withFx ? L( "(nicht gespeichert)" ) : fxo.empty() ? L( "keine" ) : fxo.c_str() );
			}
			ImGui::TableNextColumn();
			if( ImGui::SmallButton( "..." ) ) ImGui::OpenPopup( "snapmenu" );
			if( ImGui::BeginPopup( "snapmenu" ) )
			{
				std::vector< int > own;
				for( auto& f : s.fix ) own.push_back( f.id );
				if( ImGui::MenuItem( L( "Neu aufnehmen (gleiche Lampen)" ) ) ) { show.RecaptureSnapshot( i ); SetStatus( L( "Snapshot neu aufgenommen: " ) + s.name ); }
				if( ImGui::MenuItem( ( L( "Neu aufnehmen mit Auswahl (" ) + std::to_string( ids.size() ) + ")" ).c_str(), nullptr, false, !ids.empty() ) ) { show.RecaptureSnapshot( i, &ids ); SetStatus( L( "Snapshot neu aufgenommen: " ) + s.name ); }
				if( ImGui::BeginMenu( L( "Effekte im Snapshot" ), !show.effects.empty() ) )
				{
					ImGui::TextDisabled( L( "an = läuft mit dem Snapshot, aus = wird abgeschaltet, - = unberührt" ) );
					for( auto& e : show.effects )
					{
						ImGui::PushID( e.id );
						const bool inOn  = std::find( s.effects.begin(), s.effects.end(), e.id ) != s.effects.end();
						const bool inOff = std::find( s.fxOff.begin(), s.fxOff.end(), e.id ) != s.fxOff.end();
						int st = inOn ? 1 : inOff ? 2 : 0;
						ImGui::TextUnformatted( e.name.c_str() );
						ImGui::SameLine( 200 * ui );
						bool ch = ImGui::RadioButton( "an", &st, 1 );
						ImGui::SameLine();
						ch |= ImGui::RadioButton( L( "aus" ), &st, 2 );
						ImGui::SameLine();
						ch |= ImGui::RadioButton( "-", &st, 0 );
						if( ch )
						{
							s.effects.erase( std::remove( s.effects.begin(), s.effects.end(), e.id ), s.effects.end() );
							s.fxOff.erase( std::remove( s.fxOff.begin(), s.fxOff.end(), e.id ), s.fxOff.end() );
							if( st == 1 ) s.effects.push_back( e.id );
							if( st == 2 ) s.fxOff.push_back( e.id );
							s.withFx   = true;
							show.dirty = true;
						}
						ImGui::PopID();
					}
					ImGui::EndMenu();
				}
				if( ImGui::MenuItem( L( "Effekte mitspeichern" ), nullptr, &s.withFx ) ) show.dirty = true;
				if( ImGui::MenuItem( L( "Projektoren mitspeichern" ), nullptr, &s.withProj, !show.projectors.empty() ) ) show.dirty = true;
				ImGui::TextDisabled( L( "(wirkt beim nächsten Neu-Aufnehmen)" ) );
				ImGui::Separator();
				if( ImGui::MenuItem( L( "Lampen auswählen" ) ) ) sel = own;
				if( ImGui::MenuItem( L( "In den Programmer laden" ) ) ) show.LoadSnapshotToBase( i );
				ImGui::SetItemTooltip( L( "Schreibt die Werte fest in die Grundwerte (wie früher \"Abrufen\")" ) );
				if( ImGui::MenuItem( L( "Nach oben" ), nullptr, false, i > 0 ) ) up = i;
				ImGui::Separator();
				if( ImGui::MenuItem( L( "Löschen" ) ) ) del = i;
				ImGui::EndPopup();
			}
			if( DelRow( rowY0 ) ) del = i;//Delete / Backspace over the row
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if( up > 0 ) { std::swap( show.snapshots[ up ], show.snapshots[ up - 1 ] ); show.dirty = true; }
	if( del >= 0 )
	{
		show.snapshots.erase( show.snapshots.begin() + del );
		//MIDI mappings point to the index
		for( auto it = show.midi.begin(); it != show.midi.end(); )
		{
			if( it->target == MidiTarget::Snapshot && it->snapshot == del ) { it = show.midi.erase( it ); continue; }
			if( it->target == MidiTarget::Snapshot && it->snapshot > del ) --it->snapshot;
			++it;
		}
		show.dirty = true;
	}
}

//---------------------------------------------------------------- cue list
static std::string TcText( double sec, int fps )
{
	if( sec < 0 ) return "--:--:--:--";
	fps             = std::max( 1, fps );
	const long long fr = (long long)floor( sec * fps + 1e-6 );
	char b[ 32 ];
	snprintf( b, sizeof( b ), "%02lld:%02lld:%02lld:%02lld", fr / ( 3600LL * fps ), fr / ( 60LL * fps ) % 60, fr / fps % 60, fr % fps );
	return b;
}
//"hh:mm:ss:ff", "mm:ss", "ss.s" -> seconds; false if unreadable
static bool TcParse( const char* t, int fps, double& out )
{
	int p[ 4 ], n = 0;
	const char* c = t;
	if( !strchr( t, ':' ) )
	{
		char* e  = nullptr;
		double v = strtod( t, &e );
		if( e == t || v < 0 ) return false;
		out = v;
		return true;
	}
	while( *c && n < 4 )
	{
		char* e = nullptr;
		long v  = strtol( c, &e, 10 );
		if( e == c || v < 0 ) return false;
		p[ n++ ] = (int)v;
		c        = e;
		if( *c == ':' || *c == ';' || *c == '.' ) ++c;
		else break;
	}
	if( n == 2 ) out = p[ 0 ] * 60.0 + p[ 1 ];
	else if( n == 3 ) out = p[ 0 ] * 3600.0 + p[ 1 ] * 60.0 + p[ 2 ];
	else if( n == 4 ) out = p[ 0 ] * 3600.0 + p[ 1 ] * 60.0 + p[ 2 ] + p[ 3 ] / (double)std::max( 1, fps );
	else return false;
	return true;
}
void App::TabCues()
{
	//---- transport
	ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.15f, 0.55f, 0.25f, 1 ) );
	ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.2f, 0.68f, 0.32f, 1 ) );
	if( ImGui::Button( "  GO  ", ImVec2( 110 * ui, 0 ) ) ) show.CueGo();
	ImGui::PopStyleColor( 2 );
	ImGui::SetItemTooltip( L( "Nächsten Cue starten (Leertaste)" ) );
	OscContext( "/ls/cue/go" );
	ImGui::SameLine();
	if( ImGui::Button( L( "Zurück" ) ) ) show.CueBack();
	OscContext( "/ls/cue/back" );
	ImGui::SameLine();
	if( ImGui::Button( "Stop" ) ) show.CueStop();
	ImGui::SetItemTooltip( L( "Snapshots der Cue-Liste ausblenden, zurück auf Anfang" ) );
	OscContext( "/ls/cue/stop" );
	ImGui::SameLine();
	if( show.cueCur >= 0 && show.cueCur < (int)show.cues.size() )
		ImGui::Text( L( "Aktuell: %d %s" ), show.cueCur + 1, show.cues[ show.cueCur ].name.c_str() );
	else
		ImGui::TextDisabled( L( "Aktuell: -" ) );
	ImGui::SameLine( 0, 30 * ui );
	if( ImGui::Checkbox( L( "Timecode folgen (MTC)" ), &show.cueTimecode ) ) show.dirty = true;
	ImGui::SetItemTooltip( L( "Cues mit Auslöser \"Timecode\" starten, wenn der MIDI-Timecode ihre Zeit erreicht.\nAbleton: Einstellungen > Link/Tempo/MIDI > MIDI-Port Sync an, Typ MIDI Timecode.\nSprung im Timecode: der passende Cue wird direkt gestartet." ) );
	OscContext( "/ls/cue/timecode" );
	ImGui::SameLine();
	if( show.tcSec >= 0 ) ImGui::Text( "%s  (%d fps)", TcText( show.tcSec, show.tcFps ).c_str(), show.tcFps );
	else ImGui::TextDisabled( L( "kein Timecode" ) );
	//---- add
	const int active = [ & ] {
		int best = -1;
		for( int i = 0; i < (int)show.snapshots.size(); ++i )
			if( show.snapshots[ i ].active && ( best < 0 || show.snapshots[ i ].seq > show.snapshots[ best ].seq ) ) best = i;
		return best;
	}();
	ImGui::BeginDisabled( show.snapshots.empty() );
	if( ImGui::Button( L( "+ Cue" ) ) )
	{
		Cue c;
		const int si = active >= 0 ? active : (int)show.snapshots.size() - 1;
		c.snap       = show.snapshots[ si ].uid;
		c.name       = show.snapshots[ si ].name;
		if( show.cueTimecode && show.tcSec >= 0 )
		{
			c.trigger = 2;
			c.tc      = show.tcSec;
		}
		const int at = show.cueCur >= 0 && show.cueCur < (int)show.cues.size() ? show.cueCur + 1 : (int)show.cues.size();
		show.cues.insert( show.cues.begin() + at, c );
		show.dirty = true;
	}
	ImGui::SetItemTooltip( L( "Neuer Cue hinter dem aktuellen, mit dem zuletzt gestarteten Snapshot.\nMit Timecode: die aktuelle Zeit wird übernommen." ) );
	ImGui::SameLine();
	if( ImGui::Button( L( "+ Cues aus allen Snapshots" ) ) )
	{
		for( auto& s : show.snapshots )
		{
			Cue c;
			c.snap = s.uid;
			c.name = s.name;
			show.cues.push_back( c );
		}
		show.dirty = true;
	}
	ImGui::EndDisabled();
	if( show.snapshots.empty() ) { ImGui::SameLine(); ImGui::TextDisabled( L( "zuerst Snapshots anlegen" ) ); }
	if( show.cues.empty() )
	{
		ImGui::TextDisabled( L( "Eine Cue-Liste spielt Snapshots der Reihe nach ab: mit GO, automatisch nacheinander oder per Timecode aus Ableton." ) );
		return;
	}
	//---- list
	int del = -1, mv = 0, mvI = -1;
	const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
	if( ImGui::BeginTable( "cues", 8, tf, ImVec2( std::min( ImGui::GetContentRegionAvail().x, 1180 * ui ), ImGui::GetContentRegionAvail().y ) ) )
	{
		ImGui::TableSetupScrollFreeze( 0, 1 );
		ImGui::TableSetupColumn( "#", ImGuiTableColumnFlags_WidthFixed, 46 * ui );
		ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthFixed, 180 * ui );
		ImGui::TableSetupColumn( "Snapshot", ImGuiTableColumnFlags_WidthFixed, 190 * ui );
		ImGui::TableSetupColumn( "Fade", ImGuiTableColumnFlags_WidthFixed, 150 * ui );
		ImGui::TableSetupColumn( L( "Auslöser" ), ImGuiTableColumnFlags_WidthFixed, 120 * ui );
		ImGui::TableSetupColumn( L( "Zeit" ), ImGuiTableColumnFlags_WidthFixed, 200 * ui );
		ImGui::TableSetupColumn( L( "Andere aus" ), ImGuiTableColumnFlags_WidthFixed, 90 * ui );
		ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthFixed, 34 * ui );
		ImGui::TableHeadersRow();
		for( int i = 0; i < (int)show.cues.size(); ++i )
		{
			Cue& c = show.cues[ i ];
			ImGui::PushID( i );
			ImGui::TableNextRow();
			const float rowY0 = ImGui::GetCursorScreenPos().y;
			if( i == show.cueCur ) ImGui::TableSetBgColor( ImGuiTableBgTarget_RowBg0, IM_COL32( 40, 90, 50, 255 ) );
			else if( i == show.cueCur + 1 ) ImGui::TableSetBgColor( ImGuiTableBgTarget_RowBg0, IM_COL32( 45, 50, 62, 255 ) );
			ImGui::TableNextColumn();
			char num[ 16 ];
			snprintf( num, sizeof( num ), "%s%d", i == show.cueCur ? "> " : "", i + 1 );
			if( ImGui::Button( num, ImVec2( -1, 0 ) ) ) show.CueGo( i );
			ImGui::SetItemTooltip( L( "Diesen Cue jetzt starten" ) );
			ImGui::TableNextColumn();
			char nm[ 64 ];
			snprintf( nm, sizeof( nm ), "%s", c.name.c_str() );
			ImGui::SetNextItemWidth( -1 );
			if( ImGui::InputText( "##n", nm, sizeof( nm ) ) ) { c.name = nm; show.dirty = true; }
			ImGui::TableNextColumn();
			const int si = show.FindSnapUid( c.snap );
			ImGui::SetNextItemWidth( -1 );
			if( ImGui::BeginCombo( "##s", si >= 0 ? show.snapshots[ si ].name.c_str() : L( "(keiner - nur ausblenden)" ), ImGuiComboFlags_HeightLarge ) )
			{
				if( ImGui::Selectable( L( "(keiner - nur ausblenden)" ), si < 0 ) ) { c.snap = 0; show.dirty = true; }
				for( int k = 0; k < (int)show.snapshots.size(); ++k )
				{
					ImGui::PushID( k );
					if( ImGui::Selectable( show.snapshots[ k ].name.c_str(), k == si ) )
					{
						if( c.name.empty() || ( si >= 0 && c.name == show.snapshots[ si ].name ) ) c.name = show.snapshots[ k ].name;
						c.snap     = show.snapshots[ k ].uid;
						show.dirty = true;
					}
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
			ImGui::TableNextColumn();
			{
				bool own = c.fade >= 0;
				if( ImGui::Checkbox( "##fo", &own ) ) { c.fade = own ? ( si >= 0 ? show.snapshots[ si ].fade : 1.0f ) : -1.0f; show.dirty = true; }
				ImGui::SetItemTooltip( L( "An: eigene Fade-Zeit für diesen Cue (ein- und ausblenden)\nAus: Fade-Zeit des Snapshots" ) );
				ImGui::SameLine();
				ImGui::SetNextItemWidth( -1 );
				if( own )
				{
					if( ls::rs::SliderFloat( "##f", &c.fade, 0.0f, 60.0f, c.fade < 0.05f ? L( "sofort" ) : "%.1f s", ImGuiSliderFlags_Logarithmic ) ) { c.fade = Clamp( c.fade, 0.0f, 600.0f ); show.dirty = true; }
				}
				else
					ImGui::TextDisabled( L( "Snapshot (%.1f s)" ), show.CueFadeTime( i ) );
			}
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth( -1 );
			if( ImGui::Combo( "##t", &c.trigger, L( "GO\0Folgen\0Timecode\0" ) ) ) show.dirty = true;
			ImGui::SetItemTooltip( L( "GO: mit GO / Leertaste\nFolgen: automatisch, sobald der vorige Cue eingeblendet ist (+ Wartezeit)\nTimecode: zur eingetragenen MIDI-Timecode-Zeit" ) );
			ImGui::TableNextColumn();
			if( c.trigger == 1 )
			{
				ImGui::SetNextItemWidth( -1 );
				if( ls::rs::DragFloat( "##w", &c.wait, 0.05f, 0.0f, 3600.0f, L( "warten %.1f s" ) ) ) { c.wait = std::max( 0.0f, c.wait ); show.dirty = true; }
			}
			else if( c.trigger == 2 )
			{
				char tb[ 32 ];
				snprintf( tb, sizeof( tb ), "%s", TcText( c.tc, show.tcFps ).c_str() );
				ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail().x - 52 * ui );
				if( ImGui::InputText( "##tc", tb, sizeof( tb ), ImGuiInputTextFlags_EnterReturnsTrue ) )
				{
					double v;
					if( TcParse( tb, show.tcFps, v ) ) { c.tc = v; show.dirty = true; }
				}
				ImGui::SetItemTooltip( L( "hh:mm:ss:ff (oder mm:ss / Sekunden), Enter übernimmt" ) );
				ImGui::SameLine();
				ImGui::BeginDisabled( show.tcSec < 0 );
				if( ImGui::SmallButton( L( "jetzt" ) ) ) { c.tc = show.tcSec; show.dirty = true; }
				ImGui::EndDisabled();
				ImGui::SetItemTooltip( L( "Aktuelle Timecode-Zeit übernehmen" ) );
			}
			else
				ImGui::TextDisabled( "-" );
			ImGui::TableNextColumn();
			if( ImGui::Checkbox( "##r", &c.release ) ) show.dirty = true;
			ImGui::SetItemTooltip( L( "An: Snapshots der anderen Cues werden ausgeblendet (Cue = ganzes Bild)\nAus: dieser Snapshot kommt dazu" ) );
			ImGui::TableNextColumn();
			if( ImGui::SmallButton( "..." ) ) ImGui::OpenPopup( "cuemenu" );
			if( ImGui::BeginPopup( "cuemenu" ) )
			{
				if( ImGui::MenuItem( L( "Jetzt starten" ) ) ) show.CueGo( i );
				if( ImGui::MenuItem( L( "Nach oben" ), nullptr, false, i > 0 ) ) { mv = -1; mvI = i; }
				if( ImGui::MenuItem( L( "Nach unten" ), nullptr, false, i + 1 < (int)show.cues.size() ) ) { mv = 1; mvI = i; }
				if( ImGui::MenuItem( L( "Duplizieren" ) ) ) { show.cues.insert( show.cues.begin() + i + 1, c ); show.dirty = true; ImGui::EndPopup(); ImGui::PopID(); break; }
				ImGui::Separator();
				if( ImGui::MenuItem( L( "Löschen" ) ) ) del = i;
				ImGui::EndPopup();
			}
			if( DelRow( rowY0 ) ) del = i;//Delete / Backspace over the row
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if( mvI >= 0 && mv )
	{
		std::swap( show.cues[ mvI ], show.cues[ mvI + mv ] );
		if( show.cueCur == mvI ) show.cueCur += mv;
		else if( show.cueCur == mvI + mv ) show.cueCur -= mv;
		show.dirty = true;
	}
	if( del >= 0 )
	{
		show.cues.erase( show.cues.begin() + del );
		if( show.cueCur > del ) --show.cueCur;
		else if( show.cueCur == del ) show.cueCur = del - 1;
		show.dirty = true;
	}
}

//---------------------------------------------------------------- palettes
void App::TabPalettes()
{
	std::vector< int > ids;
	for( int id : sel )
		if( show.FindFixture( id ) ) ids.push_back( id );
	if( ImGui::Button( L( "+ Farbpalette" ) ) )
	{
		Palette p;
		p.uid  = show.NewPaletteUid();
		p.name = L( "Farbe " ) + std::to_string( show.palettes.size() + 1 );
		show.palettes.push_back( p );
		show.dirty = true;
	}
	ImGui::SameLine();
	ImGui::BeginDisabled( ids.empty() );
	if( ImGui::Button( ( L( "+ Positionspalette aus Auswahl (" ) + std::to_string( ids.size() ) + ")" ).c_str() ) )
	{
		Palette p;
		p.uid  = show.NewPaletteUid();
		p.kind = 1;
		p.name = L( "Position " ) + std::to_string( show.palettes.size() + 1 );
		for( int id : ids )
			if( FixtureInst* f = show.FindFixture( id ) )
				if( show.HasAttr( *f, Attr::Pan ) || show.HasAttr( *f, Attr::Tilt ) )
				{
					p.pos.push_back( { id, show.GetAttrBase( *f, Attr::Pan ), show.GetAttrBase( *f, Attr::Tilt ) } );
					show.progPalPos[ id ] = p.uid;
				}
		if( p.pos.empty() ) SetStatus( L( "Keine der ausgewählten Lampen hat Pan/Tilt" ) );
		else
		{
			show.palettes.push_back( p );
			show.dirty = true;
		}
	}
	ImGui::EndDisabled();
	ImGui::TextDisabled( L( "Klick auf \"Anwenden\" setzt die Palette bei den ausgewählten Lampen. Snapshots merken sich Palettenwerte: Palette ändern = alle Snapshots damit ändern sich mit." ) );
	if( show.palettes.empty() ) return;
	uint32_t del = 0;
	const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
	if( ImGui::BeginTable( "pals", 5, tf, ImVec2( std::min( ImGui::GetContentRegionAvail().x, 980 * ui ), ImGui::GetContentRegionAvail().y ) ) )
	{
		ImGui::TableSetupScrollFreeze( 0, 1 );
		ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthFixed, 200 * ui );
		ImGui::TableSetupColumn( L( "Inhalt" ), ImGuiTableColumnFlags_WidthFixed, 260 * ui );
		ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthFixed, 200 * ui );
		ImGui::TableSetupColumn( L( "Verwendet" ), ImGuiTableColumnFlags_WidthFixed, 150 * ui );
		ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthFixed, 34 * ui );
		ImGui::TableHeadersRow();
		for( size_t i = 0; i < show.palettes.size(); ++i )
		{
			Palette& p = show.palettes[ i ];
			ImGui::PushID( (int)p.uid );
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			char nm[ 64 ];
			snprintf( nm, sizeof( nm ), "%s", p.name.c_str() );
			ImGui::SetNextItemWidth( -1 );
			if( ImGui::InputText( "##n", nm, sizeof( nm ) ) ) { p.name = nm; show.dirty = true; }
			ImGui::TableNextColumn();
			if( p.kind == 0 )
			{
				const Palette prev = p;
				ImGui::SetNextItemWidth( -1 );
				if( ImGui::ColorEdit3( "##c", &p.color.x, ImGuiColorEditFlags_PickerHueWheel ) ) show.UpdatePalette( p.uid, &prev );
				ImGui::SetItemTooltip( L( "Ändern wirkt sofort in allen Snapshots mit dieser Farbe" ) );
			}
			else
			{
				char l[ 64 ];
				snprintf( l, sizeof( l ), L( "Position: %d Lampe%s" ), (int)p.pos.size(), p.pos.size() == 1 ? "" : PlN() );
				if( ImGui::Selectable( l, false ) )
				{
					sel.clear();
					for( auto& q : p.pos ) sel.push_back( q.id );
				}
				ImGui::SetItemTooltip( L( "Klick: Lampen auswählen" ) );
			}
			ImGui::TableNextColumn();
			{
				std::vector< int > tgt = ids;
				if( tgt.empty() && p.kind == 1 )
					for( auto& q : p.pos ) tgt.push_back( q.id );
				ImGui::BeginDisabled( tgt.empty() );
				char b[ 64 ];
				snprintf( b, sizeof( b ), L( "Anwenden (%d)" ), (int)tgt.size() );
				if( ImGui::Button( b, ImVec2( -1, 0 ) ) )
				{
					const int n = show.ApplyPalette( p.uid, tgt );
					SetStatus( p.name + ": " + std::to_string( n ) + L( " Lampen" ) );
				}
				ImGui::EndDisabled();
				ImGui::SetItemTooltip( p.kind == 1 ? L( "Auf die ausgewählten Lampen (ohne Auswahl: auf alle Lampen der Palette)" ) : L( "Auf die ausgewählten Lampen" ) );
			}
			ImGui::TableNextColumn();
			{
				int ns = 0;
				for( auto& s : show.snapshots )
					for( auto& lk : s.pal )
						if( lk.second == p.uid ) { ++ns; break; }
				if( ns ) ImGui::Text( L( "in %d Snapshot%s" ), ns, ns == 1 ? "" : "s" );
				else ImGui::TextDisabled( L( "in keinem Snapshot" ) );
			}
			ImGui::TableNextColumn();
			if( ImGui::SmallButton( "..." ) ) ImGui::OpenPopup( "palmenu" );
			if( ImGui::BeginPopup( "palmenu" ) )
			{
				if( p.kind == 1 )
				{
					if( ImGui::MenuItem( ( L( "Aus Auswahl aktualisieren (" ) + std::to_string( ids.size() ) + ")" ).c_str(), nullptr, false, !ids.empty() ) )
					{
						const Palette prev = p;
						for( int id : ids )
							if( FixtureInst* f = show.FindFixture( id ) )
								if( show.HasAttr( *f, Attr::Pan ) || show.HasAttr( *f, Attr::Tilt ) )
								{
									PalPos q{ id, show.GetAttrBase( *f, Attr::Pan ), show.GetAttrBase( *f, Attr::Tilt ) };
									bool found = false;
									for( auto& o : p.pos )
										if( o.id == id ) { o = q; found = true; }
									if( !found ) p.pos.push_back( q );
									show.progPalPos[ id ] = p.uid;
								}
						const int n = show.UpdatePalette( p.uid, &prev );
						SetStatus( p.name + L( " aktualisiert, Snapshots geändert: " ) + std::to_string( n ) );
					}
					ImGui::SetItemTooltip( L( "Übernimmt Pan/Tilt der ausgewählten Lampen in die Palette - alle Snapshots damit ändern sich mit" ) );
				}
				if( ImGui::MenuItem( L( "Nach oben" ), nullptr, false, i > 0 ) ) { std::swap( show.palettes[ i ], show.palettes[ i - 1 ] ); show.dirty = true; ImGui::EndPopup(); ImGui::PopID(); break; }
				ImGui::Separator();
				if( ImGui::MenuItem( L( "Löschen" ) ) ) del = p.uid;
				ImGui::SetItemTooltip( L( "Die Snapshots behalten ihre Werte, folgen der Palette aber nicht mehr" ) );
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if( del ) show.DeletePalette( del );
}

//---------------------------------------------------------------- group faders
void App::TabGroups()
{
	std::vector< int > ids;
	for( int id : sel )
		if( show.FindFixture( id ) ) ids.push_back( id );
	auto firstValue = [ & ]( GroupFader& g ) {
		for( int id : g.fixtures )
			if( FixtureInst* f = show.FindFixture( id ) )
				if( g.kind == 1 || show.HasAttr( *f, g.attr ) )
				{
					if( g.kind == 0 ) g.value = show.GetAttrBase( *f, g.attr );
					return;
				}
	};
	ImGui::BeginDisabled( ids.empty() );
	char bl[ 96 ];
	snprintf( bl, sizeof( bl ), L( "+ Gruppe aus Auswahl (%d)" ), (int)ids.size() );
	if( ImGui::Button( bl ) )
	{
		GroupFader g;
		g.name     = L( "Gruppe " ) + std::to_string( show.groups.size() + 1 );
		g.fixtures = ids;
		firstValue( g );
		show.groups.push_back( g );
		show.dirty = true;
	}
	ImGui::SameLine();
	if( ImGui::Button( ( L( "+ Farbgruppe aus Auswahl (" ) + std::to_string( ids.size() ) + ")" ).c_str() ) )
	{
		GroupFader g;
		g.name     = L( "Farbe " ) + std::to_string( show.groups.size() + 1 );
		g.fixtures = ids;
		g.kind     = 1;
		g.value    = 1;
		g.sat      = 0;
		show.groups.push_back( g );
		show.dirty = true;
	}
	ImGui::EndDisabled();
	if( ids.empty() ) { ImGui::SameLine(); ImGui::TextDisabled( L( "zuerst Lampen auswählen" ) ); }
	ImGui::TextDisabled( L( "Ein Regler setzt das gleiche Attribut (oder eine Farbe) bei allen Lampen der Gruppe. Rechtsklick auf einen Regler: OSC-Adresse / MIDI lernen." ) );
	if( show.groups.empty() ) return;
	int del = -1;
	const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
	if( ImGui::BeginTable( "groups", 5, tf, ImVec2( std::min( ImGui::GetContentRegionAvail().x, 1050 * ui ), ImGui::GetContentRegionAvail().y ) ) )
	{
		ImGui::TableSetupScrollFreeze( 0, 1 );
		ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthFixed, 170 * ui );
		ImGui::TableSetupColumn( L( "Was" ), ImGuiTableColumnFlags_WidthFixed, 150 * ui );
		ImGui::TableSetupColumn( L( "Wert" ), ImGuiTableColumnFlags_WidthFixed, 520 * ui );
		ImGui::TableSetupColumn( L( "Lampen" ), ImGuiTableColumnFlags_WidthFixed, 110 * ui );
		ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthFixed, 34 * ui );
		ImGui::TableHeadersRow();
		for( int i = 0; i < (int)show.groups.size(); ++i )
		{
			GroupFader& g = show.groups[ i ];
			ImGui::PushID( i );
			const std::string base = "/ls/group/" + std::to_string( i + 1 );
			ImGui::TableNextRow();
			const float rowY0 = ImGui::GetCursorScreenPos().y;
			ImGui::TableNextColumn();
			char nm[ 64 ];
			snprintf( nm, sizeof( nm ), "%s", g.name.c_str() );
			ImGui::SetNextItemWidth( -1 );
			if( ImGui::InputText( "##n", nm, sizeof( nm ) ) ) { g.name = nm; show.dirty = true; }
			ImGui::TableNextColumn();
			//what: colour or one attribute that the lamps have
			ImGui::SetNextItemWidth( -1 );
			const char* cur = g.kind == 1 ? L( "Farbe" ) : AttrName( g.attr );
			if( ImGui::BeginCombo( "##k", cur, ImGuiComboFlags_HeightLarge ) )
			{
				if( ImGui::Selectable( L( "Farbe" ), g.kind == 1 ) ) { g.kind = 1; show.dirty = true; }
				ImGui::Separator();
				for( int a = 1; a < (int)Attr::Count; ++a )
				{
					int have = 0;
					for( int id : g.fixtures )
						if( FixtureInst* f = show.FindFixture( id ) ) have += show.HasAttr( *f, (Attr)a ) ? 1 : 0;
					if( !have ) continue;
					char l[ 64 ];
					snprintf( l, sizeof( l ), "%s  (%d)", AttrName( (Attr)a ), have );
					if( ImGui::Selectable( l, g.kind == 0 && g.attr == (Attr)a ) )
					{
						g.kind = 0;
						g.attr = (Attr)a;
						firstValue( g );
						show.dirty = true;
					}
				}
				ImGui::EndCombo();
			}
			ImGui::TableNextColumn();
			bool ch = false;
			if( g.kind == 1 )
			{
				const V3 c = Hsv( g.hue, g.sat, 1.0f );
				ImGui::ColorButton( "##c", ImVec4( c.x, c.y, c.z, 1 ), ImGuiColorEditFlags_NoTooltip, ImVec2( ImGui::GetFrameHeight(), ImGui::GetFrameHeight() ) );
				ImGui::SameLine();
				const float w = ( ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x ) / 3;
				float hd      = g.hue * 360.0f;
				ImGui::SetNextItemWidth( w );
				if( ls::rs::SliderFloat( "##h", &hd, 0, 360, L( "Farbton %.0f" ) ) ) { g.hue = Saturate( hd / 360.0f ); ch = true; }
				OscContext( base + "/hue" );
				ImGui::SameLine();
				float sp = g.sat * 100;
				ImGui::SetNextItemWidth( w );
				if( ls::rs::SliderFloat( "##s", &sp, 0, 100, L( "Sättigung %.0f %%" ) ) ) { g.sat = Saturate( sp / 100 ); ch = true; }
				OscContext( base + "/sat" );
				ImGui::SameLine();
				float vp = g.value * 100;
				ImGui::SetNextItemWidth( w );
				if( ls::rs::SliderFloat( "##v", &vp, 0, 100, L( "Helligkeit %.0f %%" ) ) ) { g.value = Saturate( vp / 100 ); ch = true; }
				OscContext( base + "/value" );
			}
			else
			{
				float vp = g.value * 100;
				ImGui::SetNextItemWidth( -1 );
				if( g.Submaster() ) ImGui::PushStyleColor( ImGuiCol_SliderGrab, ImVec4( 1.0f, 0.62f, 0.2f, 1 ) );
				if( ls::rs::SliderFloat( "##v", &vp, 0, 100, g.Submaster() ? L( "Submaster %.1f %%" ) : "%.1f %%" ) ) { g.value = Saturate( vp / 100 ); ch = true; }
				if( g.Submaster() ) ImGui::PopStyleColor();
				if( g.Submaster() ) ImGui::SetItemTooltip( L( "Submaster: skaliert die fertige Helligkeit der Lampen (Programmer, Snapshots und Effekte)" ) );
				OscContext( base + "/value" );
			}
			if( ch )
			{
				show.ApplyGroup( i );
				show.dirty = true;
			}
			ImGui::TableNextColumn();
			{
				char l[ 48 ];
				snprintf( l, sizeof( l ), L( "%d Lampe%s" ), (int)g.fixtures.size(), g.fixtures.size() == 1 ? "" : PlN() );
				if( ImGui::Selectable( l, false ) ) sel = g.fixtures;
				ImGui::SetItemTooltip( L( "Klick: Lampen auswählen\n%s" ), LampNames( show, g.fixtures ).c_str() );
			}
			ImGui::TableNextColumn();
			if( ImGui::SmallButton( "..." ) ) ImGui::OpenPopup( "grpmenu" );
			if( ImGui::BeginPopup( "grpmenu" ) )
			{
				if( ImGui::MenuItem( ( L( "Lampen = Auswahl (" ) + std::to_string( ids.size() ) + ")" ).c_str(), nullptr, false, !ids.empty() ) ) { g.fixtures = ids; show.dirty = true; }
				if( ImGui::MenuItem( L( "Auswahl hinzufügen" ), nullptr, false, !ids.empty() ) )
				{
					for( int id : ids )
						if( std::find( g.fixtures.begin(), g.fixtures.end(), id ) == g.fixtures.end() ) g.fixtures.push_back( id );
					show.dirty = true;
				}
				if( ImGui::MenuItem( L( "Auswahl entfernen" ), nullptr, false, !ids.empty() ) )
				{
					g.fixtures.erase( std::remove_if( g.fixtures.begin(), g.fixtures.end(), [ & ]( int id ) { return std::find( ids.begin(), ids.end(), id ) != ids.end(); } ), g.fixtures.end() );
					show.dirty = true;
				}
				if( ImGui::MenuItem( L( "Wert jetzt anwenden" ) ) ) show.ApplyGroup( i );
				{
					bool sm = g.mode == 1;
					if( ImGui::MenuItem( L( "Als Submaster (skaliert die Ausgabe)" ), nullptr, &sm, g.kind == 0 && g.attr == Attr::Dimmer ) )
					{
						g.mode     = sm ? 1 : 0;
						show.dirty = true;
						++show.dmxVersion;
					}
					ImGui::SetItemTooltip( L( "Nur für Dimmer-Gruppen.\nAus: der Regler setzt die Dimmer-Werte (der zuletzt bewegte gewinnt gegen Snapshots).\nAn: der Regler skaliert alles, was die Lampen ausgeben - auch Snapshots und Effekte." ) );
				}
				ImGui::Separator();
				if( ImGui::MenuItem( L( "Löschen" ) ) ) del = i;
				ImGui::EndPopup();
			}
			if( DelRow( rowY0 ) ) del = i;//Delete / Backspace over the row
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if( del >= 0 ) { FxGroupRemoved( show, del ); show.groups.erase( show.groups.begin() + del ); show.dirty = true; }
}

//---------------------------------------------------------------- library dialog
void App::LibraryPopup()
{
	if( add_.open )
	{
		ImGui::OpenPopup( L( "Lampenbibliothek" ) );
		add_.open = false;
		add_.lastSearch = "\x01";
		if( add_.universe < 1 || show.UniIndex( add_.universe ) < 0 ) add_.universe = show.universes.empty() ? 1 : show.universes[ 0 ].num;
	}
	ImGui::SetNextWindowSize( ImVec2( 1100 * ui, 680 * ui ), ImGuiCond_Appearing );
	bool openFlag = true;
	if( !ImGui::BeginPopupModal( L( "Lampenbibliothek" ), &openFlag ) ) return;
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6, 5 ) );
	if( add_.replace )
	{
		int n = 0;
		for( int id : add_.replaceIds ) n += show.FindFixture( id ) ? 1 : 0;
		ImGui::TextColored( ImVec4( 1.0f, 0.78f, 0.35f, 1 ), L( "Lampentyp tauschen: %d Lampe%s - Position, Rotation, Befestigung und Adresse bleiben erhalten." ), n, n == 1 ? "" : PlN() );
	}
	//filter
	std::string key = std::string( add_.search ) + "|" + add_.mfFilter;
	if( key != add_.lastSearch )
	{
		add_.lastSearch = key;
		add_.list.clear();
		std::string q = ToLower( add_.search );
		for( auto& d : lib.defs )
		{
			if( !add_.mfFilter.empty() && d->manufacturer != add_.mfFilter ) continue;
			std::string hay = ToLower( d->manufacturer + " " + d->name + " " + KindName( d->kind ) );
			for( auto& c : d->categories ) hay += " " + ToLower( c );
			bool ok = true;
			size_t p = 0;
			while( p < q.size() )
			{
				size_t e = q.find( ' ', p );
				if( e == std::string::npos ) e = q.size();
				if( e > p && hay.find( q.substr( p, e - p ) ) == std::string::npos ) ok = false;
				p = e + 1;
			}
			if( ok ) add_.list.push_back( d.get() );
		}
		std::stable_sort( add_.list.begin(), add_.list.end(), []( const FixtureDef* a, const FixtureDef* b ) {
			bool ga = a->id.rfind( "generic/", 0 ) == 0, gb = b->id.rfind( "generic/", 0 ) == 0;
			if( ga != gb ) return ga;
			if( a->manufacturer != b->manufacturer ) return a->manufacturer < b->manufacturer;
			return a->name < b->name;
		} );
	}
	ImGui::SetNextItemWidth( 300 * ui );
	if( ImGui::IsWindowAppearing() ) ImGui::SetKeyboardFocusHere();
	ImGui::InputTextWithHint( "##search", L( "Suchen (z.B. 'martin aura', 'moving head', 'par')" ), add_.search, sizeof( add_.search ) );
	ImGui::SameLine();
	ImGui::SetNextItemWidth( 220 * ui );
	if( ImGui::BeginCombo( "##mf", add_.mfFilter.empty() ? L( "Alle Hersteller" ) : add_.mfFilter.c_str(), ImGuiComboFlags_HeightLargest ) )
	{
		if( ImGui::Selectable( L( "Alle Hersteller" ), add_.mfFilter.empty() ) ) add_.mfFilter.clear();
		std::vector< std::string > mfs;
		for( auto& d : lib.defs )
			if( std::find( mfs.begin(), mfs.end(), d->manufacturer ) == mfs.end() ) mfs.push_back( d->manufacturer );
		std::sort( mfs.begin(), mfs.end() );
		for( auto& m : mfs )
			if( ImGui::Selectable( m.c_str(), m == add_.mfFilter ) ) add_.mfFilter = m;
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ImGui::TextDisabled( L( "%d Lampen" ), (int)add_.list.size() );
	ImGui::SameLine();
	if( ImGui::Button( L( "GDTF importieren..." ) ) )
	{
		wchar_t buf[ MAX_PATH * 4 ] = {};
		OPENFILENAMEW of{};
		of.lStructSize = sizeof( of );
		of.hwndOwner   = (HWND)hwnd;
		of.lpstrFilter = L"GDTF (*.gdtf)\0*.gdtf\0";
		of.lpstrFile   = buf;
		of.nMaxFile    = MAX_PATH * 4;
		of.Flags       = OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST | OFN_EXPLORER;
		if( GetOpenFileNameW( &of ) )
		{
			std::string p = Narrow( buf );
			//keep a copy in the library so the show can find it again
			std::string dst = exeDir + "\\library\\gdtf\\" + FileName( p );
			CreateDirectoryW( Widen( exeDir + "\\library\\gdtf" ).c_str(), nullptr );
			CopyFileW( Widen( p ).c_str(), Widen( dst ).c_str(), FALSE );
			const FixtureDef* d = lib.LoadGdtf( dst );
			if( d ) { add_.def = d; add_.lastSearch = "\x01"; }
			else SetStatus( L( "GDTF-Fehler: " ) + lib.lastError );
		}
	}
	float listW = 520;
	ImGui::BeginChild( "list", ImVec2( listW, -40 ), ImGuiChildFlags_Borders );
	ImGuiListClipper clip;
	clip.Begin( (int)add_.list.size() );
	while( clip.Step() )
		for( int i = clip.DisplayStart; i < clip.DisplayEnd; ++i )
		{
			const FixtureDef* d = add_.list[ i ];
			char lb[ 300 ];
			snprintf( lb, sizeof( lb ), "%s##%d", d->name.c_str(), i );
			if( ImGui::Selectable( lb, add_.def == d, ImGuiSelectableFlags_AllowDoubleClick ) )
			{
				add_.def  = d;
				add_.mode = 0;
			}
			ImGui::SameLine( 250 * ui );
			ImGui::TextDisabled( "%s", d->manufacturer.c_str() );
			ImGui::SameLine( 420 * ui );
			ImGui::TextDisabled( "%s", KindName( d->kind ) );
		}
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild( "detail", ImVec2( 0, -40 ), ImGuiChildFlags_Borders );
	const FixtureDef* d = add_.def;
	if( d )
	{
		if( fontBig ) ImGui::PushFont( fontBig );
		ImGui::TextUnformatted( d->name.c_str() );
		if( fontBig ) ImGui::PopFont();
		ImGui::TextDisabled( "%s  |  %s", d->manufacturer.c_str(), KindName( d->kind ) );
		ImGui::Text( L( "Abstrahlwinkel %.0f-%.0f Grad   %.0f lm   %.0f K" ), d->beamMin, d->beamMax, d->lumens, d->kelvin );
		if( d->panRange > 0 ) ImGui::Text( L( "Pan %.0f Grad   Tilt %.0f Grad" ), d->panRange, d->tiltRange );
		if( d->Heads() > 1 ) ImGui::Text( "%d Pixel (%dx%d)", d->Heads(), d->gx, d->gy );
		add_.mode = Clamp( add_.mode, 0, (int)d->modes.size() - 1 );
		char ml[ 200 ];
		snprintf( ml, sizeof( ml ), L( "%s (%d Kanäle)" ), d->modes[ add_.mode ].name.c_str(), d->modes[ add_.mode ].Footprint() );
		ImGui::SetNextItemWidth( 320 * ui );
		if( ImGui::BeginCombo( "Mode", ml ) )
		{
			for( int i = 0; i < (int)d->modes.size(); ++i )
			{
				snprintf( ml, sizeof( ml ), L( "%s (%d Kanäle)##%d" ), d->modes[ i ].name.c_str(), d->modes[ i ].Footprint(), i );
				if( ImGui::Selectable( ml, i == add_.mode ) ) add_.mode = i;
			}
			ImGui::EndCombo();
		}
		const Mode& m = d->modes[ add_.mode ];
		ImGui::BeginChild( "chprev", ImVec2( 0, 150 * ui ), ImGuiChildFlags_Borders );
		ImGui::PushFont( fontMono );
		for( int i = 0; i < (int)m.ch.size(); ++i ) ImGui::Text( "%3d  %-26.26s %s", i + 1, m.ch[ i ].name.c_str(), AttrName( m.ch[ i ].attr ) );
		ImGui::PopFont();
		ImGui::EndChild();
		int fp = m.Footprint();
		if( add_.replace )
		{
			ImGui::Checkbox( L( "Bei Adress-Konflikt neu adressieren" ), &add_.readdress );
			ImGui::SetItemTooltip( L( "Ist der neue Typ größer und überlappt die nächste Lampe, bekommt er die nächste freie Adresse" ) );
			int clashes = 0;
			for( int id : add_.replaceIds )
				if( const FixtureInst* f = show.FindFixture( id ) )
					if( show.Overlaps( f->universe, f->address, fp, f->id ) || f->address + fp - 1 > 512 ) ++clashes;
			if( clashes ) ImGui::TextColored( ImVec4( 1, 0.6f, 0.3f, 1 ), L( "%d Lampe%s würde%s sich mit der Adresse überschneiden (%d Kanäle)." ), clashes, clashes == 1 ? "" : PlN(), clashes == 1 || gLang != kLangDe ? "" : PlN(), fp );
			else ImGui::TextDisabled( L( "Adressen passen (%d Kanäle)." ), fp );
		}
		else
		{
		ImGui::SetNextItemWidth( 120 * ui );
		ImGui::InputInt( L( "Anzahl" ), &add_.count );
		add_.count = Clamp( add_.count, 1, 64 );
		ImGui::SetNextItemWidth( 120 * ui );
		ImGui::InputInt( "Universe", &add_.universe );
		add_.universe = std::max( 1, add_.universe );
		ImGui::Checkbox( L( "Adresse automatisch" ), &add_.autoAddr );
		if( add_.autoAddr )
		{
			int a = show.UniIndex( add_.universe ) >= 0 ? show.FreeAddress( add_.universe, fp ) : 1;
			add_.address = a ? a : 1;
			ImGui::SameLine();
			ImGui::TextDisabled( a ? L( "ab %d" ) : L( "(Universe voll, nächstes wird benutzt)" ), a );
		}
		else
		{
			ImGui::SetNextItemWidth( 120 * ui );
			ImGui::InputInt( L( "Startadresse" ), &add_.address );
			add_.address = Clamp( add_.address, 1, 512 );
		}
		const char* placements = L( "An Traverse hängen\0Boden hinten (nach oben)\0Bodenlinie vorne\0Am Kamera-Drehpunkt\0Auf Stativ\0" );
		ImGui::SetNextItemWidth( 260 * ui );
		ImGui::Combo( L( "Platzierung" ), &add_.placement, placements );
		if( add_.placement == 0 )
		{
			if( show.trusses.empty() ) ImGui::TextColored( ImVec4( 1, 0.6f, 0.3f, 1 ), L( "Keine Traverse vorhanden (Tab 'Bühne')" ) );
			else
			{
				add_.truss = Clamp( add_.truss, 0, (int)show.trusses.size() - 1 );
				ImGui::SetNextItemWidth( 120 * ui );
				int t1 = add_.truss + 1;
				if( ls::rs::SliderInt( L( "Traverse" ), &t1, 1, (int)show.trusses.size(), "%d" ) ) add_.truss = t1 - 1;
			}
		}
		else if( add_.placement == 4 )
		{
			add_.stand = Clamp( add_.stand, -1, (int)show.stands.size() - 1 );
			std::string prev = add_.stand < 0 ? std::string( L( "Neues Stativ je Lampe" ) ) : L( "Stativ " ) + std::to_string( add_.stand + 1 );
			ImGui::SetNextItemWidth( 260 * ui );
			if( ImGui::BeginCombo( L( "Stativ" ), prev.c_str() ) )
			{
				if( ImGui::Selectable( L( "Neues Stativ je Lampe" ), add_.stand < 0 ) ) add_.stand = -1;
				for( int i = 0; i < (int)show.stands.size(); ++i )
				{
					int nm        = show.MountedCount( MountType::Stand, i );
					std::string l = L( "Stativ " ) + std::to_string( i + 1 ) + "  (" + std::to_string( nm ) + ( nm == 1 ? L( " Lampe)" ) : L( " Lampen)" ) );
					if( ImGui::Selectable( l.c_str(), add_.stand == i ) ) add_.stand = i;
				}
				ImGui::EndCombo();
			}
			ImGui::TextDisabled( add_.stand < 0 ? L( "Jede Lampe bekommt ihr eigenes Stativ (Tab 'Bühne')." ) : L( "Alle neuen Lampen kommen auf dieses Stativ (T-Bar)." ) );
		}
		else
		{
			ImGui::SetNextItemWidth( 120 * ui );
			ls::rs::DragFloat( L( "Abstand (m)" ), &add_.spacing, 0.02f, 0.1f, 10 );
		}
		}
	}
	else
		ImGui::TextDisabled( L( "Links eine Lampe auswählen." ) );
	ImGui::EndChild();
	ImGui::BeginDisabled( !d );
	if( add_.replace )
	{
		if( ImGui::Button( L( "Tauschen" ), ImVec2( 160 * ui, 0 ) ) && d )
		{
			int n = SwapFixtureType( add_.replaceIds, d, add_.mode, add_.readdress );
			SetStatus( std::to_string( n ) + L( " Lampe(n) getauscht gegen " ) + d->name );
			add_.replace = false;
			ImGui::CloseCurrentPopup();
		}
	}
	else if( ImGui::Button( L( "Hinzufügen" ), ImVec2( 160 * ui, 0 ) ) && d )
	{
		const Mode& m = d->modes[ add_.mode ];
		int fp        = m.Footprint();
		int uni = add_.universe, addr = add_.address;
		while( show.UniIndex( uni ) < 0 ) show.AddUniverse();
		std::vector< int > ids;
		for( int i = 0; i < add_.count; ++i )
		{
			if( add_.autoAddr || i > 0 )
			{
				int a = show.FreeAddress( uni, fp, add_.autoAddr || i > 0 ? 1 : addr );
				if( !a )
				{
					uni = uni + 1;
					while( show.UniIndex( uni ) < 0 ) show.AddUniverse();
					a = show.FreeAddress( uni, fp );
				}
				addr = a ? a : 1;
			}
			V3 pos, rot;
			float u = ( i + 0.5f ) / add_.count;
			bool mh = d->kind == Kind::MovingHead || d->kind == Kind::Scanner;
			switch( add_.placement )
			{
			case 0:
				if( !show.trusses.empty() )
				{
					const Truss& t = show.trusses[ Clamp( add_.truss, 0, (int)show.trusses.size() - 1 ) ];
					pos = t.a + ( t.b - t.a ) * u - V3( 0, t.size * 0.5f + 0.02f, 0 );
					rot = V3( mh ? 180.0f : 150.0f, 0, 0 );
					if( !mh ) pos.y -= 0.05f;
				}
				break;
			case 1:
				pos = V3( ( i - ( add_.count - 1 ) * 0.5f ) * add_.spacing, show.stage.deck ? show.stage.deckHeight : 0.0f, -show.stage.depth * 0.5f + 0.4f );
				rot = V3( mh ? 0.0f : -12.0f, 0, 0 );
				break;
			case 2:
				pos = V3( ( i - ( add_.count - 1 ) * 0.5f ) * add_.spacing, show.stage.deck ? show.stage.deckHeight : 0.0f, show.stage.depth * 0.5f - 0.4f );
				rot = V3( mh ? 0.0f : -20.0f, 0, 0 );
				break;
			case 4: break;//placed on the stand below
			default:
				pos = cam.target + V3( ( i - ( add_.count - 1 ) * 0.5f ) * add_.spacing, 0, 0 );
				break;
			}
			FixtureInst* f = AddFixture( d, add_.mode, uni, addr, pos, rot, "" );
			if( f ) ids.push_back( f->id );
		}
		//mount: truss lamps follow their truss, stand lamps go on top of the stand
		if( add_.placement == 0 && !show.trusses.empty() )
			for( int id : ids )
				if( FixtureInst* f = show.FindFixture( id ) ) show.Attach( *f, MountType::Truss, Clamp( add_.truss, 0, (int)show.trusses.size() - 1 ) );
		if( add_.placement == 4 )
		{
			std::vector< FixtureInst* > fx;
			for( int id : ids )
				if( FixtureInst* f = show.FindFixture( id ) ) fx.push_back( f );
			if( add_.stand < 0 || add_.stand >= (int)show.stands.size() )
				for( auto* f : fx ) show.PlaceOnStand( { f }, show.AddStand() );
			else
				show.PlaceOnStand( fx, add_.stand );
		}
		sel = ids;
		SetStatus( std::to_string( ids.size() ) + "x " + d->name + L( " hinzugefügt" ) );
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if( ImGui::Button( L( "Schließen" ), ImVec2( 120 * ui, 0 ) ) ) { add_.replace = false; ImGui::CloseCurrentPopup(); }
	ImGui::SameLine( 0, 30 * ui );
	if( ImGui::Button( L( "Neue eigene Lampe..." ) ) )
	{
		OpenFixtureEditor( nullptr );
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	ImGui::BeginDisabled( !add_.def );
	const bool own = add_.def && add_.def->id.rfind( "user/", 0 ) == 0;
	if( ImGui::Button( own ? L( "Lampe bearbeiten..." ) : L( "Als eigene Lampe bearbeiten..." ) ) && add_.def )
	{
		OpenFixtureEditor( add_.def );
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	if( own )
	{
		ImGui::SameLine();
		ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.45f, 0.13f, 0.12f, 1 ) );
		if( ImGui::Button( L( "Eigene Lampe löschen..." ) ) ) ImGui::OpenPopup( L( "Lampe löschen?" ) );
		ImGui::PopStyleColor();
		if( ImGui::BeginPopupModal( L( "Lampe löschen?" ), nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
		{
			int used = 0;
			for( auto& f : show.fixtures ) used += f.def == add_.def ? 1 : 0;
			ImGui::Text( L( "\"%s\" (%s) endgültig aus der Bibliothek löschen?" ), add_.def->name.c_str(), add_.def->manufacturer.c_str() );
			if( used ) ImGui::TextColored( ImVec4( 1, 0.6f, 0.3f, 1 ), L( "Wird in dieser Show von %d Lampe%s benutzt - erst löschen oder tauschen." ), used, used == 1 ? "" : PlN() );
			ImGui::BeginDisabled( used > 0 );
			if( ImGui::Button( L( "Löschen" ), ImVec2( 120 * ui, 0 ) ) )
			{
				std::string why;
				const std::string nm = add_.def->name;
				if( DeleteUserFixture( add_.def, &why ) ) SetStatus( L( "Eigene Lampe gelöscht: " ) + nm );
				else SetStatus( L( "Nicht gelöscht: " ) + why );
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if( ImGui::Button( L( "Abbrechen" ), ImVec2( 120 * ui, 0 ) ) ) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}
	ImGui::PopStyleVar();
	ImGui::EndPopup();
	if( !openFlag ) add_.replace = false;
}

//---------------------------------------------------------------- video projectors + screens
#define kProjFormats L( "16:9\0" "16:10\0" "4:3\0" "21:9\0" "1:1\0" "wie Eingang\0" )//current language
static const float kScreenAspects[ 5 ] = { 16.0f / 9.0f, 16.0f / 10.0f, 4.0f / 3.0f, 21.0f / 9.0f, 1.0f };

void App::TabVideo()
{
	if( startScreen >= 0 && startScreen < (int)show.screens.size() ) { screenSel_ = startScreen; projSel_ = -1; }//--screen (screenshots)
	startScreen = -1;
	if( projSel_ >= (int)show.projectors.size() ) projSel_ = -1;
	if( screenSel_ >= (int)show.screens.size() ) screenSel_ = -1;
	bool ch = false;
	ImGui::BeginChild( "vlist", ImVec2( 250 * ui, 0 ), ImGuiChildFlags_Borders );
	ImGui::TextUnformatted( L( "Projektoren" ) );
	ImGui::SameLine( ImGui::GetContentRegionAvail().x - 70 * ui );
	if( ImGui::SmallButton( L( "+ Projektor" ) ) )
	{
		if( show.projectors.size() >= 8 ) SetStatus( L( "Maximal 8 Projektoren werden beleuchtet" ) );
		show.AddProjector();
		projSel_   = (int)show.projectors.size() - 1;
		screenSel_ = -1;
	}
	for( int i = 0; i < (int)show.projectors.size(); ++i )
	{
		Projector& p = show.projectors[ i ];
		ImGui::PushID( 3000 + i );
		const VideoFrame* fr = video.Frame( p.uid );
		ImVec4 dot = !p.on ? ImVec4( 0.4f, 0.4f, 0.4f, 1 ) : fr ? ImVec4( 0.3f, 0.9f, 0.4f, 1 ) : ImVec4( 1.0f, 0.6f, 0.2f, 1 );
		{
			ImVec2 cp     = ImGui::GetCursorScreenPos();
			const float r = 4.0f * ui, lh = ImGui::GetTextLineHeight();
			ImGui::GetWindowDrawList()->AddCircleFilled( ImVec2( cp.x + r, cp.y + lh * 0.5f ), r, ImGui::ColorConvertFloat4ToU32( dot ) );
			ImGui::Dummy( ImVec2( r * 2, lh ) );
		}
		ImGui::SameLine();
		const char* in = p.input == VideoInput::TestCard ? L( "Testbild" ) : p.input == VideoInput::File ? "Video" : "NDI";
		std::string l  = p.name + "   (" + in + ")";
		if( ImGui::Selectable( l.c_str(), projSel_ == i ) ) { projSel_ = i; screenSel_ = -1; }
		ImGui::PopID();
	}
	if( show.projectors.empty() ) ImGui::TextDisabled( L( "Noch kein Projektor." ) );
	else if( ImGui::SmallButton( L( "Resolume-Preset..." ) ) ) resWin_ = true;
	if( !show.projectors.empty() ) ImGui::SetItemTooltip( "%s", L( "Advanced-Output-Preset für Resolume aus Projektoren und Leinwänden erzeugen" ) );
	else if( ImGui::Checkbox( L( "Linien anzeigen" ), &ren.projLines ) ) SaveSettings();
	ImGui::Separator();
	ImGui::TextUnformatted( L( "Leinwände" ) );
	ImGui::SameLine( ImGui::GetContentRegionAvail().x - 70 * ui );
	if( ImGui::SmallButton( L( "+ Leinwand" ) ) )
	{
		show.AddScreen();
		screenSel_ = (int)show.screens.size() - 1;
		projSel_   = -1;
	}
	for( int i = 0; i < (int)show.screens.size(); ++i )
	{
		Screen& s = show.screens[ i ];
		ImGui::PushID( 4000 + i );
		char l[ 128 ];
		snprintf( l, sizeof( l ), "%s   (%.2f x %.2f m)", s.name.c_str(), s.width, s.height );
		if( ImGui::Selectable( l, screenSel_ == i ) ) { screenSel_ = i; projSel_ = -1; }
		ImGui::PopID();
	}
	if( show.screens.empty() ) ImGui::TextDisabled( L( "Noch keine Leinwand." ) );
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild( "vset", ImVec2( 0, 0 ), ImGuiChildFlags_Borders );
	if( projSel_ >= 0 ) ProjectorUi( show.projectors[ projSel_ ], projSel_ );
	else if( screenSel_ >= 0 ) ScreenUi( show.screens[ screenSel_ ], screenSel_ );
	else
	{
		ImGui::TextDisabled( L( "Links einen Projektor oder eine Leinwand wählen, oder mit + neu anlegen." ) );
		ImGui::TextDisabled( L( "Projektoren werfen ihr Bild als Licht in den Raum: auf Leinwände, Bühne, Wände, Modelle und in den Haze." ) );
		ImGui::TextDisabled( L( "Videodateien können auch ins Fenster gezogen werden." ) );
	}
	ImGui::EndChild();
	(void)ch;
}

void App::ProjectorUi( Projector& p, int idx )
{
	bool ch = false;
	ch |= ImGui::Checkbox( L( "An" ), &p.on );
	ImGui::SameLine();
	char nm[ 64 ];
	snprintf( nm, sizeof( nm ), "%s", p.name.c_str() );
	ImGui::SetNextItemWidth( 200 * ui );
	if( ImGui::InputText( "##pname", nm, sizeof( nm ) ) ) { p.name = nm; ch = true; }
	ImGui::SameLine();
	if( ImGui::SmallButton( L( "Duplizieren" ) ) )
	{
		Projector c = p;
		c.uid       = NewProjectorUid();
		c.name      = p.name + L( " Kopie" );
		c.pos.x += 1.0f;
		show.projectors.push_back( c );
		projSel_   = (int)show.projectors.size() - 1;
		show.dirty = true;
		return;
	}
	ImGui::SameLine();
	if( ImGui::SmallButton( L( "Löschen" ) ) )
	{
		show.projectors.erase( show.projectors.begin() + idx );
		projSel_   = -1;
		show.dirty = true;
		return;
	}
	const VideoFrame* fr = video.Frame( p.uid );
	if( ImGui::BeginTable( "pt", 2, ImGuiTableFlags_SizingStretchSame ) )
	{
		ImGui::TableNextColumn();
		//---- input
		ImGui::SeparatorText( L( "Eingang" ) );
		int in = (int)p.input;
		ImGui::SetNextItemWidth( 160 * ui );
		if( ImGui::Combo( L( "Quelle" ), &in, L( "Testbild\0Videodatei\0NDI\0" ) ) ) { p.input = (VideoInput)in; ch = true; }
		if( p.input == VideoInput::File )
		{
			if( ImGui::Button( L( "Datei wählen..." ) ) )
			{
				wchar_t buf[ MAX_PATH * 4 ] = {};
				OPENFILENAMEW of{};
				of.lStructSize = sizeof( of );
				of.hwndOwner   = (HWND)hwnd;
				of.lpstrFilter = LW( L"Videos (*.mp4;*.mov;*.m4v;*.wmv;*.avi;*.mkv)\0*.mp4;*.mov;*.m4v;*.wmv;*.avi;*.mkv\0Alle Dateien\0*.*\0" );
				of.lpstrFile   = buf;
				of.nMaxFile    = MAX_PATH * 4;
				of.Flags       = OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST | OFN_EXPLORER;
				if( GetOpenFileNameW( &of ) ) { p.file = Narrow( buf ); ch = true; }
			}
			ImGui::SameLine();
			ch |= ImGui::Checkbox( L( "Schleife" ), &p.loop );
			ImGui::TextDisabled( "%s", p.file.empty() ? L( "(keine Datei)" ) : FileName( p.file ).c_str() );
			if( !p.file.empty() ) ImGui::SetItemTooltip( "%s", p.file.c_str() );
		}
		else if( p.input == VideoInput::Ndi )
		{
			std::string where;
			if( !NdiAvailable( &where ) )
				ImGui::TextColored( ImVec4( 1, 0.55f, 0.3f, 1 ), L( "NDI-Runtime nicht gefunden.\nNDI Tools installieren (ndi.video/tools)." ) );
			else
			{
				std::vector< std::string > srcs = NdiSources();
				ImGui::SetNextItemWidth( 260 * ui );
				if( ImGui::BeginCombo( L( "NDI-Quelle" ), p.ndi.empty() ? L( "(wählen)" ) : p.ndi.c_str() ) )
				{
					for( auto& s : srcs )
						if( ImGui::Selectable( s.c_str(), s == p.ndi ) ) { p.ndi = s; ch = true; }
					if( srcs.empty() ) ImGui::TextDisabled( L( "suche im Netzwerk ..." ) );
					ImGui::EndCombo();
				}
				ImGui::TextDisabled( L( "%d Quelle(n) gefunden" ), (int)srcs.size() );
				ImGui::SetItemTooltip( L( "NDI-Runtime: %s" ), where.c_str() );
			}
		}
		std::string st = p.on ? video.Status( p.uid ) : L( "aus" );
		ImGui::TextColored( fr || !p.on ? ImVec4( 0.6f, 0.8f, 0.6f, 1 ) : ImVec4( 1, 0.7f, 0.35f, 1 ), "%s", st.c_str() );
		//preview
		GLuint tex = ren.ProjectorTexture( p.uid );
		if( tex && fr )
		{
			float pw = 220 * ui, ph = pw * fr->h / std::max( 1, fr->w );
			ImGui::Image( (ImTextureID)(intptr_t)tex, ImVec2( pw, ph ), ImVec2( p.rear ? 1.0f : 0.0f, 0 ), ImVec2( p.rear ? 0.0f : 1.0f, 1 ) );
		}
		//---- optics
		ImGui::SeparatorText( L( "Optik" ) );
		ImGui::SetNextItemWidth( 120 * ui );
		ch |= ImGui::Combo( "Format", &p.format, kProjFormats );
		ImGui::SetNextItemWidth( 120 * ui );
		ch |= ls::rs::DragFloat( L( "Zoom (Projektionsverhältnis)" ), &p.throwRatio, 0.01f, 0.2f, 10.0f, "%.2f : 1" );
		ImGui::SetItemTooltip( L( "Abstand / Bildbreite. Klein = Weitwinkel (großes Bild), groß = Tele" ) );
		ProjectorGeo g = GetProjectorGeo( p, fr ? fr->w : 0, fr ? fr->h : 0 );
		float dist     = show.ThrowDistance( g );
		ImGui::TextDisabled( L( "Bild bei %.1f m Abstand: %.2f x %.2f m  (Öffnung %.0f°)" ), dist, dist * ( g.r - g.l ), dist * ( g.t - g.b ), 2.0f * atanf( 0.5f / std::max( 0.1f, p.throwRatio ) ) / kDeg );
		float sh[ 2 ] = { p.shiftX * 100.0f, p.shiftY * 100.0f };
		ImGui::SetNextItemWidth( 160 * ui );
		if( ls::rs::DragFloat2( L( "Lens Shift H / V" ), sh, 0.5f, -100.0f, 100.0f, "%.0f %%" ) ) { p.shiftX = sh[ 0 ] / 100.0f; p.shiftY = sh[ 1 ] / 100.0f; ch = true; }
		ImGui::SetNextItemWidth( 160 * ui );
		ch |= ls::rs::DragFloat( L( "Helligkeit (Lumen)" ), &p.lumens, 50.0f, 300.0f, 60000.0f, "%.0f lm" );
		ImGui::SetNextItemWidth( 160 * ui );
		ch |= ls::rs::SliderFloat( L( "Schwarzwert" ), &p.blackLevel, 0.0f, 0.03f, "%.3f" );
		ImGui::SetItemTooltip( L( "Restlicht bei schwarzem Bild (Kontrast des Projektors)" ) );
		ImGui::SetNextItemWidth( 160 * ui );
		ch |= ls::rs::SliderFloat( L( "Unschärfe" ), &p.blur, 0.0f, 1.0f, "%.2f" );
		ch |= ImGui::Checkbox( L( "Rückprojektion (spiegeln)" ), &p.rear );
		ImGui::SameLine();
		ch |= ImGui::Checkbox( L( "Umrisslinien zeigen" ), &p.frustum );
		ImGui::SetItemTooltip( L( "Linien von der Linse zu den Bildecken. Alle Projektoren: Ansicht > Projektor-Linien" ) );
		ImGui::TableNextColumn();
		//---- position
		ImGui::SeparatorText( "Position" );
		ImGui::SetNextItemWidth( 220 * ui );
		ch |= ls::rs::DragFloat3( "Position (m)", &p.pos.x, 0.02f );
		ImGui::SetItemTooltip( "X / Y / Z (m)" );
		ImGui::SetNextItemWidth( 220 * ui );
		float rot[ 3 ] = { p.pan, p.tilt, p.roll };
		if( ls::rs::DragFloat3( "Pan / Tilt / Roll", rot, 0.25f, -180.0f, 180.0f, "%.1f°" ) )
		{
			p.pan  = rot[ 0 ];
			p.tilt = Clamp( rot[ 1 ], -90.0f, 90.0f );
			p.roll = rot[ 2 ];
			ch     = true;
		}
		if( ImGui::SmallButton( L( "Deckenmontage" ) ) ) { p.roll = fabsf( p.roll ) > 90 ? 0.0f : 180.0f; ch = true; }
		ImGui::SetItemTooltip( L( "Kopfüber hängend (Bild wird mitgedreht)" ) );
		ImGui::SeparatorText( L( "Auf Leinwand ausrichten" ) );
		if( show.screens.empty() ) ImGui::TextDisabled( L( "Keine Leinwand vorhanden." ) );
		else
		{
			static int target = 0;
			target            = Clamp( target, 0, (int)show.screens.size() - 1 );
			ImGui::SetNextItemWidth( 180 * ui );
			if( ImGui::BeginCombo( L( "Leinwand" ), show.screens[ target ].name.c_str() ) )
			{
				for( int i = 0; i < (int)show.screens.size(); ++i )
					if( ImGui::Selectable( show.screens[ i ].name.c_str(), i == target ) ) target = i;
				ImGui::EndCombo();
			}
			if( ImGui::Button( L( "Hinzielen + Zoom anpassen" ) ) ) { show.AimProjector( p, show.screens[ target ], false, fr ? fr->w : 0, fr ? fr->h : 0 ); ch = true; }
			ImGui::SetItemTooltip( L( "Bleibt am Platz, dreht sich zur Leinwand und zoomt so, dass das Bild darauf passt" ) );
			ImGui::SameLine();
			if( ImGui::Button( L( "Frontal davor stellen" ) ) ) { show.AimProjector( p, show.screens[ target ], true, fr ? fr->w : 0, fr ? fr->h : 0 ); ch = true; }
			ImGui::SetItemTooltip( L( "Setzt den Projektor gerade vor die Leinwand, Abstand aus dem Zoom (bildfüllend)" ) );
		}
		ImGui::EndTable();
	}
	if( ch ) show.dirty = true;
}

void App::ScreenUi( Screen& s, int idx )
{
	bool ch = false;
	char nm[ 64 ];
	snprintf( nm, sizeof( nm ), "%s", s.name.c_str() );
	ImGui::SetNextItemWidth( 200 * ui );
	if( ImGui::InputText( "##sname", nm, sizeof( nm ) ) ) { s.name = nm; ch = true; }
	ImGui::SameLine();
	if( ImGui::SmallButton( L( "Duplizieren" ) ) )
	{
		Screen c = s;
		c.name   = s.name + L( " Kopie" );
		c.pos.x += s.width + 0.5f;
		show.screens.push_back( c );
		screenSel_ = (int)show.screens.size() - 1;
		show.dirty = true;
		return;
	}
	ImGui::SameLine();
	if( ImGui::SmallButton( L( "Löschen" ) ) )
	{
		show.screens.erase( show.screens.begin() + idx );
		screenSel_ = -1;
		show.dirty = true;
		return;
	}
	if( ImGui::BeginTable( "st", 2, ImGuiTableFlags_SizingStretchSame ) )
	{
		ImGui::TableNextColumn();
		ImGui::SeparatorText( L( "Maße" ) );
		//format: keeps the width, sets the height; "frei" = width and height independent
		int fmt = s.format;
		if( fmt < 0 || fmt > 5 )
		{
			fmt = 5;
			for( int i = 0; i < 5; ++i )
				if( fabsf( s.width / std::max( 0.01f, s.height ) - kScreenAspects[ i ] ) < 0.005f ) fmt = i;
			s.format = fmt;
		}
		ImGui::SetNextItemWidth( 120 * ui );
		if( ImGui::Combo( "Format", &fmt, L( "16:9\0" "16:10\0" "4:3\0" "21:9\0" "1:1\0" "frei\0" ) ) )
		{
			s.format = fmt;
			if( fmt < 5 ) s.height = s.width / kScreenAspects[ fmt ];
			ch = true;
		}
		ImGui::SetItemTooltip( L( "frei = Breite und Höhe unabhängig einstellbar" ) );
		const bool locked = fmt < 5;
		ImGui::SetNextItemWidth( 120 * ui );
		if( ls::rs::DragFloat( L( "Breite (m)" ), &s.width, 0.01f, 0.2f, 40.0f, "%.2f" ) )
		{
			if( locked ) s.height = s.width / kScreenAspects[ fmt ];
			ch = true;
		}
		OscContext( ( "/ls/screen/" + std::to_string( idx + 1 ) + "/width" ).c_str() );
		ImGui::SetNextItemWidth( 120 * ui );
		if( ls::rs::DragFloat( L( "Höhe (m)" ), &s.height, 0.01f, 0.2f, 25.0f, "%.2f" ) )
		{
			if( locked ) s.width = s.height * kScreenAspects[ fmt ];
			ch = true;
		}
		OscContext( ( "/ls/screen/" + std::to_string( idx + 1 ) + "/height" ).c_str() );
		ImGui::TextDisabled( L( "Diagonale %.0f\"  (%.2f m)" ), sqrtf( s.width * s.width + s.height * s.height ) / 0.0254f, sqrtf( s.width * s.width + s.height * s.height ) );
		//---- shape from an image (white = screen)
		ImGui::SeparatorText( L( "Form" ) );
		auto pickMask = [ & ]() {
			wchar_t buf[ MAX_PATH * 4 ] = {};
			OPENFILENAMEW of{};
			of.lStructSize = sizeof( of );
			of.hwndOwner   = (HWND)hwnd;
			of.lpstrFilter = LW( L"Bilder (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0Alle Dateien\0*.*\0" );
			of.lpstrFile   = buf;
			of.nMaxFile    = MAX_PATH * 4;
			of.Flags       = OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST | OFN_EXPLORER;
			return GetOpenFileNameW( &of ) ? Narrow( buf ) : std::string();
		};
		auto loadMask = [ & ]( const std::string& path ) {
			if( path.empty() ) return;
			std::string err, warn;
			if( LoadScreenMask( s, path, err, &warn ) )
			{
				s.format = 4;//1:1, the shape keeps its proportions
				s.height = s.width;
				ch       = true;
				SetStatus( warn.empty() ? L( "Leinwandform geladen: " ) + FileName( path ) : warn );
			}
			else
				SetStatus( err );
		};
		if( !s.HasMask() )
		{
			if( ImGui::Button( L( "Form aus Bild laden..." ) ) ) loadMask( pickMask() );
			ImGui::PushTextWrapPos( ImGui::GetCursorPosX() + 270 * ui );
			ImGui::TextDisabled( "%s", L( "Schwarz-Weiß-Bild im Seitenverhältnis 1:1 (quadratisch, z. B. 1024 x 1024). Weiß = Leinwand, Schwarz = frei - so entstehen beliebige Formen." ) );
			ImGui::PopTextWrapPos();
		}
		else
		{
			//preview of the shape
			const float pv = 72 * ui;
			ImVec2 p0      = ImGui::GetCursorScreenPos();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled( p0, ImVec2( p0.x + pv, p0.y + pv ), IM_COL32( 12, 12, 14, 255 ) );
			for( auto& r : s.maskRects ) dl->AddRectFilled( ImVec2( p0.x + r.x0 * pv, p0.y + r.y0 * pv ), ImVec2( p0.x + r.x1 * pv, p0.y + r.y1 * pv ), IM_COL32( 235, 235, 235, 255 ) );
			dl->AddRect( p0, ImVec2( p0.x + pv, p0.y + pv ), IM_COL32( 90, 95, 105, 255 ) );
			ImGui::Dummy( ImVec2( pv, pv ) );
			ImGui::SameLine();
			ImGui::BeginGroup();
			ImGui::TextUnformatted( s.maskFile.empty() ? L( "(aus der Show)" ) : FileName( s.maskFile ).c_str() );
			ImGui::TextDisabled( L( "%d Flächen, Raster %d x %d" ), (int)s.maskRects.size(), s.maskN, s.maskN );
			if( ImGui::SmallButton( L( "Anderes Bild..." ) ) ) loadMask( pickMask() );
			ImGui::SetItemTooltip( "%s", L( "Schwarz-Weiß-Bild im Seitenverhältnis 1:1 (quadratisch). Weiß = Leinwand, Schwarz = frei." ) );
			ImGui::BeginDisabled( s.maskFile.empty() );
			if( ImGui::SmallButton( L( "Neu laden" ) ) ) loadMask( s.maskFile );
			ImGui::EndDisabled();
			ImGui::SameLine();
			if( ImGui::SmallButton( L( "Form entfernen" ) ) ) { ClearScreenMask( s ); ch = true; }
			ImGui::SetItemTooltip( "%s", L( "Zurück zur rechteckigen Leinwand" ) );
			ImGui::EndGroup();
			if( s.format != 4 ) ImGui::TextColored( ImVec4( 1, 0.75f, 0.35f, 1 ), "%s", L( "Format ist nicht 1:1 - die Form wird verzerrt." ) );
		}
		ImGui::SeparatorText( "Material" );
		ImGui::SetNextItemWidth( 120 * ui );
		ch |= ls::rs::SliderFloat( "Gain", &s.gain, 0.2f, 2.5f, "%.2f" );
		ImGui::SetItemTooltip( L( "Reflexion der Leinwand: 1.0 = mattweiß, < 1 grau, > 1 Hochgain" ) );
		ImGui::BeginDisabled( s.HasMask() );
		ch |= ImGui::Checkbox( L( "Schwarzer Rahmen" ), &s.frame );
		ImGui::EndDisabled();
		if( s.HasMask() ) ImGui::SetItemTooltip( "%s", L( "Nur bei rechteckigen Leinwänden" ) );
		ImGui::TableNextColumn();
		ImGui::SeparatorText( "Position" );
		ImGui::SetNextItemWidth( 210 * ui );
		ch |= ls::rs::DragFloat3( "X / Y / Z (m)", &s.pos.x, 0.02f, 0, 0, "%.2f" );
		ImGui::SetItemTooltip( L( "Y = Höhe der Bildunterkante über dem Hallenboden" ) );
		if( ImGui::SmallButton( L( "auf Boden/Bühne" ) ) ) { s.pos.y = show.FloorAt( s.pos.x, s.pos.z ) + 0.6f; ch = true; }
		ImGui::SetNextItemWidth( 120 * ui );
		ch |= ls::rs::DragFloat( L( "Drehung" ), &s.yaw, 0.25f, -180.0f, 180.0f, "%.1f°" );
		ImGui::SetNextItemWidth( 120 * ui );
		ch |= ls::rs::DragFloat( L( "Neigung" ), &s.tilt, 0.25f, -60.0f, 60.0f, "%.1f°" );
		ImGui::SetNextItemWidth( 160 * ui );
		ch |= ImGui::Combo( L( "Aufbau" ), &s.mount, L( "Fußgestell\0Hängend (Seile)\0Ohne\0" ) );
		ImGui::EndTable();
	}
	if( ch ) show.dirty = true;
}
}//namespace ls
