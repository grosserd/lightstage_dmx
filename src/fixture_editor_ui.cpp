// fixture editor window (App methods)
#include "app.h"
#include "imgui.h"
#include <windows.h>

namespace ls
{
static bool InputStr( const char* label, std::string& s, float w = -1 )
{
	char b[ 256 ];
	snprintf( b, sizeof( b ), "%s", s.c_str() );
	if( w != 0 ) ImGui::SetNextItemWidth( w );
	if( ImGui::InputText( label, b, sizeof( b ) ) ) { s = b; return true; }
	return false;
}
static bool TypeCombo( const char* id, EdType& t, float w )
{
	bool ch = false;
	ImGui::SetNextItemWidth( w );
	if( ImGui::BeginCombo( id, EdTypeName( t ), ImGuiComboFlags_HeightLarge ) )
	{
		for( int g = 0; g < 6; ++g )
		{
			ImGui::SeparatorText( Ls( kEdGroupNames[ g ] ) );
			for( int i = 0; i < (int)EdType::Count; ++i )
				if( EdTypeGroup( (EdType)i ) == g && ImGui::Selectable( EdTypeName( (EdType)i ), t == (EdType)i ) )
				{
					t  = (EdType)i;
					ch = true;
				}
		}
		ImGui::EndCombo();
	}
	return ch;
}

void App::OpenFixtureEditor( const FixtureDef* d )
{
	fxEdOpen = true;
	fxEdSel_ = -1;
	fxEdMode_ = 0;
	fxEdMsg_.clear();
	fxEdId_.clear();
	fxEd = EdFixture();
	if( !d )
	{
		ApplyTemplate( fxEd, 0, 3 );//start with Dimmer + RGB + Strobe
		return;
	}
	//own fixtures: the editor model is stored in the file
	if( d->id.rfind( "user/", 0 ) == 0 && !d->source.empty() )
	{
		std::string txt;
		Json j;
		if( ReadText( d->source, txt ) && ParseJson( txt, j ) && EdFromJson( j[ "lightstageEditor" ], fxEd ) )
		{
			fxEdId_ = d->id;
			return;
		}
	}
	EdFromDef( *d, fxEd );//library fixture: edit a copy
	fxEdMsg_ = L( "Kopie von \"" ) + d->name + L( "\" - wird als eigene Lampe gespeichert" );
}

bool App::SaveFixtureEditor( bool test )
{
	std::string id, path, err;
	if( !SaveUserFixture( exeDir + "\\library", fxEd, id, path, &err ) )
	{
		fxEdMsg_ = L( "Nicht gespeichert: " ) + err;
		return false;
	}
	std::string txt;
	Json j;
	auto nd = std::make_unique< FixtureDef >();
	if( !ReadText( path, txt ) || !ParseJson( txt, j, &err ) || !ParseOflFixture( j, id, fxEd.manufacturer, *nd, &err ) )
	{
		fxEdMsg_ = L( "Gespeichert, aber nicht lesbar: " ) + err;
		return false;
	}
	nd->source               = path;
	const FixtureDef* def    = lib.AddOrReplace( std::move( nd ) );
	lib.mfNames[ Slug( fxEd.manufacturer ) ] = fxEd.manufacturer;
	//lamps of this type in the show: keep their DMX, re-check modes
	int n = 0;
	for( auto& f : show.fixtures )
		if( f.defId == id )
		{
			f.def  = def;
			f.mode = Clamp( f.mode, 0, (int)def->modes.size() - 1 );
			++n;
		}
	fxEdId_          = id;
	add_.lastSearch  = "\x01";//refresh the library list
	fxEdMsg_         = L( "Gespeichert: " ) + path + ( n ? "  (" + std::to_string( n ) + L( " Lampe(n) in der Show aktualisiert)" ) : "" );
	SetStatus( L( "Eigene Lampe gespeichert: " ) + fxEd.name );
	if( test )
	{
		//one lamp of this type in front of the stage, selected
		if( fxEdTestId_ >= 0 )
		{
			const int tid = fxEdTestId_;
			show.fixtures.erase( std::remove_if( show.fixtures.begin(), show.fixtures.end(), [ & ]( const FixtureInst& f ) { return f.id == tid; } ), show.fixtures.end() );
		}
		int fp  = def->modes[ 0 ].Footprint();
		int uni = show.universes.empty() ? show.AddUniverse() : show.universes[ 0 ].num;
		int a   = show.FreeAddress( uni, fp );
		if( !a ) { uni = show.AddUniverse(); a = 1; }
		bool mh = def->kind == Kind::MovingHead || def->kind == Kind::Scanner;
		FixtureInst* f = AddFixture( def, 0, uni, a, V3( 0, show.FloorAt( 0, show.stage.depth * 0.5f - 0.6f ), show.stage.depth * 0.5f - 0.6f ), V3( mh ? 0.0f : -35.0f, 0, 0 ), "Test: " + fxEd.name );
		if( f )
		{
			fxEdTestId_ = f->id;
			sel         = { f->id };
			show.SetAttr( *f, Attr::Dimmer, 1 );
			show.SetAttr( *f, Attr::Shutter, 1 );
			fxEdMsg_ += L( "  - Testlampe vorne auf der Bühne (Inspector steuert sie)" );
		}
	}
	return true;
}

void App::FixtureEditor()
{
	if( !fxEdOpen )
	{
		if( fxEdTestId_ >= 0 )
		{
			//editor closed: the temporary test lamp goes away
			const int tid = fxEdTestId_;
			show.fixtures.erase( std::remove_if( show.fixtures.begin(), show.fixtures.end(), [ & ]( const FixtureInst& f ) { return f.id == tid; } ), show.fixtures.end() );
			sel.erase( std::remove( sel.begin(), sel.end(), tid ), sel.end() );
			fxEdTestId_ = -1;
		}
		return;
	}
	ImGui::SetNextWindowSize( ImVec2( 1180 * ui, 720 * ui ), ImGuiCond_Appearing );
	std::string title = L( "Fixture-Editor - " ) + fxEd.name + "###fxed";
	if( !ImGui::Begin( title.c_str(), &fxEdOpen, ImGuiWindowFlags_NoCollapse ) )
	{
		ImGui::End();
		return;
	}
	EdFixture& f = fxEd;
	const float footerH = ImGui::GetFrameHeightWithSpacing() * 3.2f;
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6 * ui, 5 * ui ) );
	//---------------------------------------------------------------- left: general
	ImGui::BeginChild( "fxl", ImVec2( 300 * ui, -footerH ), ImGuiChildFlags_Borders );
	ImGui::SeparatorText( L( "Allgemein" ) );
	const float lw = 170 * ui;
	InputStr( L( "Hersteller" ), f.manufacturer, lw );
	InputStr( "Name", f.name, lw );
	InputStr( L( "Kurzname" ), f.shortName, lw );
	ImGui::SetNextItemWidth( lw );
	if( ImGui::BeginCombo( L( "Typ" ), Ls( kEdCategories[ Clamp( f.category, 0, 9 ) ] ) ) )
	{
		for( int i = 0; i < 10; ++i )
			if( ImGui::Selectable( Ls( kEdCategories[ i ] ), f.category == i ) ) f.category = i;
		ImGui::EndCombo();
	}
	ImGui::SeparatorText( L( "Physik" ) );
	ImGui::SetNextItemWidth( lw );
	ls::rs::DragFloat3( L( "Maße mm" ), &f.dimsMm.x, 1, 20, 3000, "%.0f" );
	ImGui::SetItemTooltip( L( "Breite / Höhe / Tiefe in Millimetern" ) );
	ImGui::SetNextItemWidth( lw );
	ls::rs::DragFloat( L( "Gewicht kg" ), &f.weightKg, 0.1f, 0, 200, "%.1f" );
	ImGui::SetNextItemWidth( lw );
	ls::rs::DragFloat( L( "Leistung W" ), &f.powerW, 1, 0, 5000, "%.0f" );
	ImGui::SetNextItemWidth( lw );
	ls::rs::DragFloat( "Lumen", &f.lumens, 50, 0, 200000, "%.0f lm" );
	ImGui::SetNextItemWidth( lw );
	ls::rs::DragFloat( L( "Farbtemperatur" ), &f.kelvin, 50, 1500, 12000, "%.0f K" );
	float bm[ 2 ] = { f.beamMin, f.beamMax };
	ImGui::SetNextItemWidth( lw );
	if( ls::rs::DragFloat2( L( "Abstrahlwinkel" ), bm, 0.2f, 1, 120, "%.0f°" ) ) { f.beamMin = bm[ 0 ]; f.beamMax = bm[ 1 ]; }
	ImGui::SetItemTooltip( L( "min / max (ohne Zoom-Kanal gilt min)" ) );
	ImGui::SeparatorText( L( "Pixel (Bar / Matrix)" ) );
	ImGui::SetNextItemWidth( lw );
	ImGui::InputInt( "Pixel", &f.pixels );
	f.pixels = Clamp( f.pixels, 0, 128 );
	ImGui::SetItemTooltip( L( "0 = keine Pixel. Sonst werden die Kanäle unten für jedes Pixel wiederholt." ) );
	if( f.pixels > 0 )
	{
		ImGui::TextDisabled( L( "Kanäle je Pixel:" ) );
		int del = -1;
		for( int i = 0; i < (int)f.pixelChannels.size(); ++i )
		{
			ImGui::PushID( 9000 + i );
			ImGui::SmallButton( EdTypeName( f.pixelChannels[ i ] ) );
			if( ImGui::IsItemClicked( ImGuiMouseButton_Right ) ) del = i;
			ImGui::SetItemTooltip( L( "Rechtsklick: entfernen" ) );
			ImGui::SameLine();
			ImGui::PopID();
		}
		ImGui::NewLine();
		if( del >= 0 ) f.pixelChannels.erase( f.pixelChannels.begin() + del );
		const EdType pt[] = { EdType::Dimmer, EdType::Red, EdType::Green, EdType::Blue, EdType::White, EdType::Amber, EdType::UV, EdType::Lime, EdType::WarmWhite, EdType::ColdWhite, EdType::Strobe };
		ImGui::SetNextItemWidth( lw );
		if( ImGui::BeginCombo( "##addpx", L( "+ Pixel-Kanal" ) ) )
		{
			for( auto t : pt )
				if( ImGui::Selectable( EdTypeName( t ) ) ) f.pixelChannels.push_back( t );
			if( ImGui::Selectable( L( "Vorlage RGB" ) ) ) f.pixelChannels = { EdType::Red, EdType::Green, EdType::Blue };
			if( ImGui::Selectable( L( "Vorlage RGBW" ) ) ) f.pixelChannels = { EdType::Red, EdType::Green, EdType::Blue, EdType::White };
			ImGui::EndCombo();
		}
		if( f.category == 0 ) f.category = 3;
	}
	ImGui::EndChild();
	ImGui::SameLine();
	//---------------------------------------------------------------- middle: modes + channels
	ImGui::BeginChild( "fxm", ImVec2( ImGui::GetContentRegionAvail().x - 340 * ui, -footerH ), ImGuiChildFlags_Borders );
	fxEdMode_ = Clamp( fxEdMode_, 0, (int)f.modes.size() - 1 );
	if( ImGui::BeginTabBar( "modes", ImGuiTabBarFlags_FittingPolicyScroll ) )
	{
		for( int m = 0; m < (int)f.modes.size(); ++m )
		{
			std::string tl = f.modes[ m ].name + " (" + std::to_string( f.Footprint( m ) ) + " ch)###m" + std::to_string( m );
			if( ImGui::BeginTabItem( tl.c_str() ) )
			{
				fxEdMode_ = m;
				ImGui::EndTabItem();
			}
		}
		if( ImGui::TabItemButton( "+", ImGuiTabItemFlags_Trailing ) )
		{
			EdMode nm = f.modes[ fxEdMode_ ];
			nm.name   = L( "Modus " ) + std::to_string( f.modes.size() + 1 );
			f.modes.push_back( nm );
		}
		ImGui::EndTabBar();
	}
	EdMode& md = f.modes[ fxEdMode_ ];
	InputStr( L( "Modusname" ), md.name, 200 * ui );
	ImGui::SameLine();
	if( ImGui::SmallButton( L( "Modus löschen" ) ) && f.modes.size() > 1 )
	{
		f.modes.erase( f.modes.begin() + fxEdMode_ );
		fxEdMode_ = 0;
		ImGui::EndChild();
		ImGui::SameLine();
		ImGui::PopStyleVar();
		ImGui::End();
		return;
	}
	ImGui::SameLine();
	ImGui::TextDisabled( L( "%d Kanäle" ), f.Footprint( fxEdMode_ ) );
	int moveFrom = -1, moveTo = -1, removeAt = -1;
	if( ImGui::BeginTable( "chs", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2( 0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.3f ) ) )
	{
		ImGui::TableSetupScrollFreeze( 0, 1 );
		ImGui::TableSetupColumn( L( "Adr" ), ImGuiTableColumnFlags_WidthFixed, 44 * ui );
		ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthStretch, 1.4f );
		ImGui::TableSetupColumn( L( "Typ" ), ImGuiTableColumnFlags_WidthStretch, 1.2f );
		ImGui::TableSetupColumn( "16 bit", ImGuiTableColumnFlags_WidthFixed, 46 * ui );
		ImGui::TableSetupColumn( L( "Standard" ), ImGuiTableColumnFlags_WidthFixed, 70 * ui );
		ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthFixed, 74 * ui );
		ImGui::TableHeadersRow();
		int adr = 1;
		for( int r = 0; r < (int)md.ch.size(); ++r )
		{
			int pi = md.ch[ r ];
			if( pi < 0 || pi >= (int)f.pool.size() ) continue;
			EdChannel& c = f.pool[ pi ];
			ImGui::PushID( pi );
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			char lb[ 32 ];
			if( c.fine16 ) snprintf( lb, sizeof( lb ), "%d+%d", adr, adr + 1 );
			else snprintf( lb, sizeof( lb ), "%d", adr );
			if( ImGui::Selectable( lb, fxEdSel_ == pi, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap ) ) fxEdSel_ = pi;
			adr += c.fine16 ? 2 : 1;
			ImGui::TableNextColumn();
			InputStr( "##n", c.name );
			if( ImGui::IsItemActivated() ) fxEdSel_ = pi;
			ImGui::TableNextColumn();
			EdType t = c.type;
			if( TypeCombo( "##t", t, -1 ) )
			{
				//new type: fresh defaults, keep a custom name
				bool defaultName = c.name == EdTypeName( c.type ) || c.name.rfind( std::string( EdTypeName( c.type ) ) + " ", 0 ) == 0;
				EdChannel nc     = MakeChannel( t, defaultName ? "" : c.name );
				if( defaultName )
				{
					c.name  = "";
					nc.name = UniqueChannelName( f, nc.name );
				}
				c        = nc;
				fxEdSel_ = pi;
			}
			ImGui::TableNextColumn();
			ImGui::Checkbox( "##f", &c.fine16 );
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth( -1 );
			ls::rs::DragInt( "##d", &c.def, 1, 0, 255 );
			ImGui::TableNextColumn();
			if( ImGui::ArrowButton( "##u", ImGuiDir_Up ) && r > 0 ) { moveFrom = r; moveTo = r - 1; }
			ImGui::SameLine( 0, 2 );
			if( ImGui::ArrowButton( "##dn", ImGuiDir_Down ) && r + 1 < (int)md.ch.size() ) { moveFrom = r; moveTo = r + 1; }
			ImGui::SameLine( 0, 2 );
			if( ImGui::SmallButton( "x" ) ) removeAt = r;
			ImGui::PopID();
		}
		if( f.pixels > 0 && md.pixels && !f.pixelChannels.empty() )
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled( "%d-%d", adr, adr + f.pixels * (int)f.pixelChannels.size() - 1 );
			ImGui::TableNextColumn();
			std::string pc;
			for( auto t : f.pixelChannels ) pc += std::string( pc.empty() ? "" : " " ) + EdTypeName( t );
			ImGui::TextDisabled( "%d Pixel x (%s)", f.pixels, pc.c_str() );
		}
		ImGui::EndTable();
	}
	if( moveFrom >= 0 ) std::swap( md.ch[ moveFrom ], md.ch[ moveTo ] );
	if( removeAt >= 0 ) md.ch.erase( md.ch.begin() + removeAt );
	if( ImGui::Button( L( "+ Kanal" ) ) )
	{
		EdChannel c = MakeChannel( EdType::Dimmer );
		c.name      = UniqueChannelName( f, c.name );
		f.pool.push_back( c );
		md.ch.push_back( (int)f.pool.size() - 1 );
		fxEdSel_ = (int)f.pool.size() - 1;
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth( 210 * ui );
	if( ImGui::BeginCombo( "##tmpl", L( "+ Vorlage" ), ImGuiComboFlags_HeightLarge ) )
	{
		for( int i = 0; i < 8; ++i )
			if( ImGui::Selectable( Ls( kEdTemplates[ i ] ) ) ) ApplyTemplate( f, fxEdMode_, i );
		ImGui::EndCombo();
	}
	//channels from the pool not used in this mode (e.g. made in another mode)
	ImGui::SameLine();
	ImGui::SetNextItemWidth( 210 * ui );
	if( ImGui::BeginCombo( "##pool", L( "+ Kanal aus anderem Modus" ) ) )
	{
		bool any = false;
		for( int i = 0; i < (int)f.pool.size(); ++i )
			if( std::find( md.ch.begin(), md.ch.end(), i ) == md.ch.end() )
			{
				any = true;
				if( ImGui::Selectable( ( f.pool[ i ].name + "  (" + EdTypeName( f.pool[ i ].type ) + ")" ).c_str() ) ) md.ch.push_back( i );
			}
		if( !any ) ImGui::TextDisabled( L( "keine" ) );
		ImGui::EndCombo();
	}
	if( f.pixels > 0 )
	{
		ImGui::SameLine();
		ImGui::Checkbox( L( "Pixel in diesem Modus" ), &md.pixels );
	}
	ImGui::EndChild();
	ImGui::SameLine();
	//---------------------------------------------------------------- right: details of the selected channel
	ImGui::BeginChild( "fxr", ImVec2( 0, -footerH ), ImGuiChildFlags_Borders );
	if( fxEdSel_ >= 0 && fxEdSel_ < (int)f.pool.size() )
	{
		EdChannel& c = f.pool[ fxEdSel_ ];
		ImGui::SeparatorText( ( c.name + "  (" + EdTypeName( c.type ) + ")" ).c_str() );
		const float rw = 150 * ui;
		bool canInvert = c.type == EdType::Dimmer || c.type == EdType::Zoom || c.type == EdType::Focus || c.type == EdType::Iris || c.type == EdType::Frost || c.type == EdType::Pan ||
		                 c.type == EdType::Tilt || c.type == EdType::CTO || c.type == EdType::ColorWheel || c.type == EdType::GoboWheel || c.type == EdType::Strobe;
		if( canInvert ) ImGui::Checkbox( L( "Umgekehrt (255 -> 0)" ), &c.invert );
		switch( c.type )
		{
		case EdType::Pan:
		case EdType::Tilt:
			ImGui::SetNextItemWidth( rw );
			ls::rs::DragFloat( L( "Bereich" ), &c.b, 1, 10, 720, "%.0f°" );
			ImGui::TextDisabled( L( "16 bit (Fein-Kanal) für weiche Bewegung empfohlen." ) );
			break;
		case EdType::Zoom:
		{
			float z[ 2 ] = { c.a, c.b };
			ImGui::SetNextItemWidth( rw );
			if( ls::rs::DragFloat2( L( "Winkel von / bis" ), z, 0.2f, 1, 120, "%.0f°" ) ) { c.a = z[ 0 ]; c.b = z[ 1 ]; }
			break;
		}
		case EdType::CTO:
		{
			float z[ 2 ] = { c.a, c.b };
			ImGui::SetNextItemWidth( rw );
			if( ls::rs::DragFloat2( L( "Kelvin von / bis" ), z, 20, 1500, 12000, "%.0f K" ) ) { c.a = z[ 0 ]; c.b = z[ 1 ]; }
			break;
		}
		case EdType::Strobe:
		{
			float z[ 2 ] = { c.a, c.b };
			ImGui::SetNextItemWidth( rw );
			if( ls::rs::DragFloat2( L( "Hz von / bis" ), z, 0.1f, 0.1f, 40, "%.1f Hz" ) ) { c.a = z[ 0 ]; c.b = z[ 1 ]; }
			ImGui::TextDisabled( L( "0-9 offen, 10-255 Strobe langsam -> schnell" ) );
			break;
		}
		case EdType::Shutter: ImGui::TextDisabled( L( "0-127 zu, 128-255 offen" ) ); break;
		case EdType::Prism: ImGui::TextDisabled( L( "0-127 aus, 128-255 Prisma" ) ); break;
		case EdType::ShutterStrobe:
		{
			const char* kinds = L( "zu\0offen\0Strobe\0Puls\0Zufall\0" );
			int del = -1;
			for( int i = 0; i < (int)c.ranges.size(); ++i )
			{
				EdRange& r = c.ranges[ i ];
				ImGui::PushID( 500 + i );
				int fr[ 2 ] = { r.from, r.to };
				ImGui::SetNextItemWidth( 90 * ui );
				if( ImGui::DragInt2( "##r", fr, 0.5f, 0, 255 ) ) { r.from = fr[ 0 ]; r.to = std::max( fr[ 0 ], fr[ 1 ] ); }
				ImGui::SameLine();
				ImGui::SetNextItemWidth( 80 * ui );
				ImGui::Combo( "##k", &r.kind, kinds );
				ImGui::SameLine();
				if( ImGui::SmallButton( "x" ) ) del = i;
				ImGui::PopID();
			}
			if( del >= 0 ) c.ranges.erase( c.ranges.begin() + del );
			if( ImGui::SmallButton( L( "+ Bereich" ) ) ) c.ranges.push_back( EdRange{ c.ranges.empty() ? 0 : std::min( 255, c.ranges.back().to + 1 ), 255, 1, "" } );
			ImGui::SameLine();
			if( ImGui::SmallButton( L( "Standard-Vorlage" ) ) ) c = MakeChannel( EdType::ShutterStrobe, c.name );
			break;
		}
		case EdType::ColorWheel:
		case EdType::ColorMacro:
		case EdType::GoboWheel:
		{
			const bool gobo = c.type == EdType::GoboWheel;
			if( c.type == EdType::ColorWheel ) ImGui::Checkbox( L( "Regenbogen-Lauf (obere Hälfte)" ), &c.rainbow );
			ImGui::TextDisabled( L( "%s (gleich große DMX-Bereiche):" ), gobo ? "Gobos" : L( "Farben" ) );
			int del = -1;
			for( int i = 0; i < (int)c.slots.size(); ++i )
			{
				EdSlot& s = c.slots[ i ];
				ImGui::PushID( 700 + i );
				if( !gobo && !s.open )
				{
					ImGui::ColorEdit3( "##c", &s.color.x, ImGuiColorEditFlags_NoInputs );
					ImGui::SameLine();
				}
				InputStr( "##sn", s.name, 140 * ui );
				ImGui::SameLine();
				if( c.type != EdType::ColorMacro ) ImGui::Checkbox( L( "offen" ), &s.open );
				ImGui::SameLine();
				if( ImGui::SmallButton( "x" ) ) del = i;
				ImGui::PopID();
			}
			if( del >= 0 ) c.slots.erase( c.slots.begin() + del );
			if( ImGui::SmallButton( gobo ? "+ Gobo" : L( "+ Farbe" ) ) ) c.slots.push_back( EdSlot{ gobo ? "Gobo " + std::to_string( c.slots.size() ) : L( "Farbe" ), V3( 1, 1, 1 ), false } );
			break;
		}
		default: ImGui::TextDisabled( L( "Keine weiteren Einstellungen für diesen Kanaltyp." ) ); break;
		}
	}
	else
		ImGui::TextDisabled( L( "Kanal in der Liste anklicken für Details\n(Bereiche, Farben, Gobos, Strobe)." ) );
	ImGui::EndChild();
	//---------------------------------------------------------------- footer
	bool fatal = false;
	auto msgs  = EdValidate( f, &fatal );
	for( auto& m : msgs )
		ImGui::TextColored( ( m.rfind( "Hinweis", 0 ) == 0 || m.rfind( L( "Hinweis" ), 0 ) == 0 ) ? ImVec4( 1, 0.8f, 0.35f, 1 ) : ImVec4( 1, 0.4f, 0.35f, 1 ), "%s", m.c_str() );
	if( !fxEdMsg_.empty() ) ImGui::TextColored( ImVec4( 0.55f, 0.85f, 0.6f, 1 ), "%s", fxEdMsg_.c_str() );
	ImGui::BeginDisabled( fatal );
	if( ImGui::Button( L( "Speichern" ), ImVec2( 130 * ui, 0 ) ) ) SaveFixtureEditor( false );
	ImGui::SameLine();
	if( ImGui::Button( L( "Speichern + Testen" ), ImVec2( 170 * ui, 0 ) ) ) SaveFixtureEditor( true );
	ImGui::SetItemTooltip( L( "Speichert und stellt eine Testlampe vorne auf die Bühne (wird beim Schließen entfernt)" ) );
	ImGui::EndDisabled();
	ImGui::SameLine();
	if( ImGui::Button( L( "Als neue Lampe" ), ImVec2( 140 * ui, 0 ) ) )
	{
		fxEdId_.clear();
		fxEd.name += " 2";
		fxEdMsg_ = L( "Neuer Name - mit Speichern als eigene Lampe anlegen" );
	}
	ImGui::SameLine();
	if( ImGui::Button( L( "Neu" ), ImVec2( 80 * ui, 0 ) ) ) OpenFixtureEditor( nullptr );
	ImGui::SameLine();
	ImGui::TextDisabled( L( "Datei: library\\user\\%s\\%s.json" ), Slug( f.manufacturer ).c_str(), Slug( f.name ).c_str() );
	ImGui::PopStyleVar();
	ImGui::End();
}
}//namespace ls
