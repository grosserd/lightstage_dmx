//window "Resolume Advanced Output": preset from the projector / screen setup
#include "app.h"
#include "imgui.h"
#include <algorithm>
#include <cstring>
#include <windows.h>
#include <commdlg.h>

namespace ls
{
static std::string SafeFileName( std::string s )
{
	for( char& c : s )
		if( strchr( "\\/:*?\"<>|", c ) || (unsigned char)c < 32 ) c = '_';
	while( !s.empty() && ( s.back() == ' ' || s.back() == '.' ) ) s.pop_back();
	return s.empty() ? std::string( "LightStage" ) : s;
}

void App::ResolumeUpdate()
{
	if( resOpt_.skip.size() != show.projectors.size() )
	{
		const size_t old = resOpt_.skip.size();
		resOpt_.skip.resize( show.projectors.size() );
		for( size_t i = old; i < show.projectors.size(); ++i ) resOpt_.skip[ i ] = show.projectors[ i ].on ? 0 : 1;
	}
	resLayout_ = BuildResolumeLayout( show, resOpt_,
	                                  [ this ]( uint32_t uid, int& w, int& h )
	                                  {
		                                  if( const VideoFrame* fr = video.Frame( uid ) )
		                                  {
			                                  w = fr->w;
			                                  h = fr->h;
		                                  }
	                                  } );
	resT_ = Now();
}

bool App::ResolumeSave( const std::string& path )
{
	ResolumeUpdate();
	int slices = 0;
	for( auto& s : resLayout_ ) slices += (int)s.slices.size();
	std::string err;
	if( slices == 0 )
	{
		resMsg_    = L( "Kein Projektor trifft eine Leinwand - nichts zu exportieren." );
		resMsgErr_ = true;
		return false;
	}
	if( !WriteResolumePreset( path, ResolumePresetXml( resLayout_, resOpt_ ), err ) )
	{
		resMsg_    = err + ": " + path;
		resMsgErr_ = true;
		return false;
	}
	resMsg_    = std::string( L( "Gespeichert: " ) ) + path;
	resMsgErr_ = false;
	SetStatus( std::string( L( "Resolume-Preset gespeichert: " ) ) + FileName( path ) );
	return true;
}

//outline of a slice in the preview: the warp net along its border
static void SliceOutline( const ResolumeSlice& sl, std::vector< ImVec2 >& out, const ImVec2& o, float sc )
{
	out.clear();
	const int N = 24;
	auto add = [ & ]( float u, float v )
	{
		float x, y;
		ResolumeEvalNet( sl, u, v, x, y );
		out.push_back( ImVec2( o.x + x * sc, o.y + y * sc ) );
	};
	for( int i = 0; i < N; ++i ) add( (float)i / N, 0 );
	for( int i = 0; i < N; ++i ) add( 1, (float)i / N );
	for( int i = 0; i < N; ++i ) add( 1 - (float)i / N, 1 );
	for( int i = 0; i < N; ++i ) add( 0, 1 - (float)i / N );
}

void App::ResolumeWindow()
{
	if( !resWin_ ) return;
	ImGui::SetNextWindowSize( ImVec2( 660 * ui, 0 ), ImGuiCond_Appearing );
	bool open = true;
	if( !ImGui::Begin( L( "Resolume Advanced Output" ), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize ) )
	{
		ImGui::End();
		if( !open ) resWin_ = false;
		return;
	}
	bool ch = resOpt_.skip.size() != show.projectors.size();
	if( resT_ < 0 && !show.path.empty() )
	{
		std::string n = FileName( show.path );
		const size_t dot = n.rfind( '.' );
		resOpt_.name     = dot == std::string::npos ? n : n.substr( 0, dot );
	}
	ImGui::TextWrapped( "%s", L( "Erzeugt ein Preset für den Advanced Output von Resolume Arena: je Projektor ein Screen (NDI-Ausgang), je getroffene Leinwand ein Slice, das genau auf der Leinwand landet. Leinwände mit Form bekommen Masken." ) );
	ImGui::Spacing();
	char nb[ 128 ];
	strncpy( nb, resOpt_.name.c_str(), sizeof( nb ) - 1 );
	nb[ sizeof( nb ) - 1 ] = 0;
	ImGui::SetNextItemWidth( 260 * ui );
	if( ImGui::InputText( L( "Preset-Name" ), nb, sizeof( nb ) ) ) resOpt_.name = nb;
	int comp[ 2 ] = { resOpt_.compW, resOpt_.compH };
	ImGui::SetNextItemWidth( 260 * ui );
	if( ImGui::DragInt2( L( "Komposition (px)" ), comp, 4, 16, 16384 ) )
	{
		resOpt_.compW = std::clamp( comp[ 0 ], 16, 16384 );
		resOpt_.compH = std::clamp( comp[ 1 ], 16, 16384 );
		ch            = true;
	}
	ImGui::SetItemTooltip( "%s", L( "Größe der Komposition in Resolume (Composition > Settings)" ) );
	ImGui::SetNextItemWidth( 260 * ui );
	ch |= ImGui::Combo( L( "Inhalt" ), &resOpt_.input, L( "Ausschnitt im Seitenverhältnis der Leinwand\0Ganze Komposition (gestreckt)\0" ) );
	ImGui::SetItemTooltip( "%s", L( "Ausschnitt: aus der Mitte der Komposition wird ein Bereich mit dem Format der Leinwand genommen - nichts wird verzerrt.\nGanze Komposition: alles wird auf die Leinwand gestreckt." ) );
	ch |= ImGui::Checkbox( L( "Leinwandform als Masken" ), &resOpt_.masks );
	ImGui::SetItemTooltip( "%s", L( "Schwarze Bereiche einer Leinwand mit Form werden in Resolume ausmaskiert (kein Licht daneben)." ) );
	if( ch || resT_ < 0 || Now() - resT_ > 0.3 ) ResolumeUpdate();

	ImGui::SeparatorText( L( "Projektoren" ) );
	if( show.projectors.empty() ) ImGui::TextDisabled( "%s", L( "Noch kein Projektor. Tab Video / Leinwand: + Projektor" ) );
	int nScreens = 0, nSlices = 0, nMasks = 0;
	for( int pi = 0; pi < (int)show.projectors.size(); ++pi )
	{
		ImGui::PushID( 7100 + pi );
		bool use = !resOpt_.skip[ pi ];
		if( ImGui::Checkbox( show.projectors[ pi ].name.c_str(), &use ) )
		{
			resOpt_.skip[ pi ] = use ? 0 : 1;
			ResolumeUpdate();
		}
		const ResolumeScreen* rs = nullptr;
		for( auto& s : resLayout_ )
			if( s.projector == pi ) rs = &s;
		if( rs )
		{
			ImGui::SameLine();
			ImGui::TextDisabled( L( "-> Screen \"%s\"  %d x %d, NDI" ), rs->name.c_str(), rs->w, rs->h );
			++nScreens;
			//preview of the output picture: frame, slices, masks
			const float cw = 300 * ui, chh = std::min( 240 * ui, cw * rs->h / std::max( 1, rs->w ) );
			const float sc = std::min( cw / rs->w, chh / rs->h );
			ImGui::Indent( 26 * ui );
			const ImVec2 o = ImGui::GetCursorScreenPos();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled( o, ImVec2( o.x + rs->w * sc, o.y + rs->h * sc ), IM_COL32( 8, 8, 10, 255 ) );
			dl->PushClipRect( o, ImVec2( o.x + rs->w * sc, o.y + rs->h * sc ), true );
			std::vector< ImVec2 > ol;
			static const ImU32 cols[ 4 ] = { IM_COL32( 64, 200, 255, 255 ), IM_COL32( 255, 170, 60, 255 ), IM_COL32( 150, 255, 120, 255 ), IM_COL32( 230, 110, 255, 255 ) };
			for( size_t k = 0; k < rs->slices.size(); ++k )
			{
				const ResolumeSlice& sl = rs->slices[ k ];
				const ImU32 c           = cols[ k % 4 ];
				SliceOutline( sl, ol, o, sc );
				dl->AddConcavePolyFilled( ol.data(), (int)ol.size(), ( c & 0x00FFFFFF ) | 0x38000000 );
				//patch lines of the warp
				const int nx = ( sl.gw - 1 ) / 3, ny = ( sl.gh - 1 ) / 3;
				for( int i = 1; i < nx; ++i )
				{
					ImVec2 a, b;
					float x, y;
					ResolumeEvalNet( sl, (float)i / nx, 0, x, y ); a = ImVec2( o.x + x * sc, o.y + y * sc );
					ResolumeEvalNet( sl, (float)i / nx, 1, x, y ); b = ImVec2( o.x + x * sc, o.y + y * sc );
					dl->AddLine( a, b, ( c & 0x00FFFFFF ) | 0x50000000 );
				}
				for( int j = 1; j < ny; ++j )
				{
					ImVec2 a, b;
					float x, y;
					ResolumeEvalNet( sl, 0, (float)j / ny, x, y ); a = ImVec2( o.x + x * sc, o.y + y * sc );
					ResolumeEvalNet( sl, 1, (float)j / ny, x, y ); b = ImVec2( o.x + x * sc, o.y + y * sc );
					dl->AddLine( a, b, ( c & 0x00FFFFFF ) | 0x50000000 );
				}
				for( auto& m : sl.masks )
				{
					std::vector< ImVec2 > mp;
					for( size_t q = 0; q + 1 < m.size(); q += 2 ) mp.push_back( ImVec2( o.x + m[ q ] * sc, o.y + m[ q + 1 ] * sc ) );
					dl->AddPolyline( mp.data(), (int)mp.size(), IM_COL32( 255, 70, 70, 220 ), ImDrawFlags_Closed, 1.2f );
				}
				dl->AddPolyline( ol.data(), (int)ol.size(), c, ImDrawFlags_Closed, 1.6f * ui );
				float cx = 0, cy = 0;
				for( int q = 0; q < 4; ++q ) { cx += sl.corners[ q * 2 ] * 0.25f; cy += sl.corners[ q * 2 + 1 ] * 0.25f; }
				const ImVec2 ts = ImGui::CalcTextSize( sl.name.c_str() );
				dl->AddText( ImVec2( o.x + cx * sc - ts.x * 0.5f, o.y + cy * sc - ts.y * 0.5f ), IM_COL32( 255, 255, 255, 230 ), sl.name.c_str() );
				++nSlices;
				nMasks += (int)sl.masks.size();
			}
			dl->PopClipRect();
			dl->AddRect( o, ImVec2( o.x + rs->w * sc, o.y + rs->h * sc ), IM_COL32( 120, 120, 130, 255 ) );
			ImGui::Dummy( ImVec2( rs->w * sc, rs->h * sc ) );
			ImGui::SameLine();
			ImGui::BeginGroup();
			if( rs->slices.empty() ) ImGui::TextColored( ImVec4( 1, 0.7f, 0.35f, 1 ), "%s", L( "trifft keine Leinwand" ) );
			for( size_t k = 0; k < rs->slices.size(); ++k )
			{
				const ResolumeSlice& sl = rs->slices[ k ];
				ImGui::TextColored( ImGui::ColorConvertU32ToFloat4( cols[ k % 4 ] ), "%s", sl.name.c_str() );
				ImGui::TextDisabled( L( "Warp %d x %d Punkte, Abweichung %.2f px" ), sl.gw, sl.gh, sl.maxErr );
				if( !sl.masks.empty() ) ImGui::TextDisabled( L( "%d Maske(n)" ), (int)sl.masks.size() );
			}
			for( auto& n : rs->notes ) ImGui::TextColored( ImVec4( 1, 0.7f, 0.35f, 1 ), "%s", n.c_str() );
			if( show.projectors[ pi ].input != VideoInput::Ndi ) ImGui::TextDisabled( "%s", L( "Quelle ist noch nicht NDI." ) );
			ImGui::EndGroup();
			ImGui::Unindent( 26 * ui );
		}
		ImGui::PopID();
	}
	ImGui::Separator();
	ImGui::Text( L( "%d Screen(s), %d Slice(s), %d Maske(n)" ), nScreens, nSlices, nMasks );
	const std::string dir  = ResolumePresetDir();
	const std::string path = dir.empty() ? std::string() : dir + "\\" + SafeFileName( resOpt_.name ) + ".xml";
	const bool exists      = !path.empty() && GetFileAttributesW( Widen( path ).c_str() ) != INVALID_FILE_ATTRIBUTES;
	ImGui::BeginDisabled( path.empty() || nSlices == 0 );
	if( ImGui::Button( exists ? L( "In Resolume überschreiben" ) : L( "In Resolume speichern" ), ImVec2( 200 * ui, 0 ) ) ) ResolumeSave( path );
	ImGui::EndDisabled();
	if( !path.empty() ) ImGui::SetItemTooltip( "%s", path.c_str() );
	ImGui::SameLine();
	ImGui::BeginDisabled( nSlices == 0 );
	if( ImGui::Button( L( "Speichern unter..." ) ) )
	{
		wchar_t buf[ MAX_PATH * 4 ] = {};
		wcsncpy( buf, Widen( path.empty() ? SafeFileName( resOpt_.name ) + ".xml" : path ).c_str(), MAX_PATH * 4 - 1 );
		OPENFILENAMEW of{};
		of.lStructSize = sizeof( of );
		of.hwndOwner   = (HWND)hwnd;
		of.lpstrFilter = L"Resolume Advanced Output (*.xml)\0*.xml\0";
		of.lpstrFile   = buf;
		of.nMaxFile    = MAX_PATH * 4;
		of.lpstrDefExt = L"xml";
		of.Flags       = OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_OVERWRITEPROMPT;
		if( GetSaveFileNameW( &of ) ) ResolumeSave( Narrow( buf ) );
	}
	ImGui::EndDisabled();
	if( !resMsg_.empty() ) ImGui::TextColored( resMsgErr_ ? ImVec4( 1, 0.5f, 0.4f, 1 ) : ImVec4( 0.5f, 0.9f, 0.6f, 1 ), "%s", resMsg_.c_str() );
	ImGui::TextDisabled( "%s", L( "In Arena: Output > Advanced... > Presets > Preset wählen.\nDanach in LightStage als NDI-Quelle \"<Rechner> (Arena - <Screen>)\" wählen." ) );
	ImGui::End();
	if( !open ) resWin_ = false;
}
}//namespace ls
