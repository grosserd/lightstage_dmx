// Grosserd LightStage - value widgets with "double-click = back to the original value"
// The original is the value a widget showed the first time it was drawn since the show was
// opened / saved / undone (per object: the key holds the widget id, the value's address and a salt).
#pragma once
#include <cstdint>
#include <unordered_map>
#include "imgui.h"
#include "imgui_internal.h"

namespace ls
{
namespace rs
{
struct Orig
{
	float f[ 4 ];
};
inline std::unordered_map< uint64_t, Orig >& Map()
{
	static std::unordered_map< uint64_t, Orig > m;
	return m;
}
inline uint64_t& SaltRef()
{
	static uint64_t s = 0;
	return s;
}
//forget all originals (new / opened / saved show, undo, redo)
inline void ResetOriginals() { Map().clear(); }
//editors that show different objects with the same widgets (inspector, effect editor) set a salt
struct Salt
{
	uint64_t old;
	explicit Salt( uint64_t s ) : old( SaltRef() ) { SaltRef() = s; }
	~Salt() { SaltRef() = old; }
};
inline uint64_t Key( const void* p )
{
	uint64_t k = (uint64_t)ImGui::GetItemID() * 0x9E3779B97F4A7C15ull;
	k ^= (uint64_t)(uintptr_t)p + 0x632BE59BD9B4E019ull + ( k << 6 ) + ( k >> 2 );
	k ^= SaltRef() * 0xBF58476D1CE4E5B9ull;
	return k;
}
//after the widget: remember the first value, double-click restores it (returns "changed")
//before the widget: was it under the mouse last frame (a drag field turns into a text field on double-click,
//so IsItemHovered afterwards is not reliable)
inline bool WasHovered( const char* label )
{
	return GImGui->HoveredIdPreviousFrame != 0 && GImGui->HoveredIdPreviousFrame == ImGui::GetID( label );
}
template< class T >
inline bool Fin( T* const* v, const T* pre, int n, bool changed, bool wasHov = false )
{
	auto& m  = Map();
	const uint64_t k = Key( v[ 0 ] );
	auto it = m.find( k );
	if( it == m.end() )
	{
		if( m.size() > 60000 ) m.clear();
		Orig o{};
		for( int i = 0; i < n; ++i ) o.f[ i ] = (float)pre[ i ];
		it = m.emplace( k, o ).first;
	}
	if( GImGui->IO.MouseClickedCount[ 0 ] == 2 && GImGui->IO.MouseClicked[ 0 ] && !ImGui::GetIO().KeyCtrl && ( wasHov || ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenBlockedByActiveItem ) ) )
	{
		for( int i = 0; i < n; ++i ) *v[ i ] = (T)it->second.f[ i ];
		if( ImGui::IsItemActive() || ( GImGui->ActiveId != 0 && GImGui->ActiveId == GImGui->TempInputId ) ) ImGui::ClearActiveID();//no text input / drag after the reset
		return true;
	}
	return changed;
}
inline bool SliderFloat( const char* l, float* v, float a, float b, const char* fmt = "%.3f", ImGuiSliderFlags f = 0 )
{
	const bool wh = WasHovered( l );
	const float p = *v;
	const bool r  = ImGui::SliderFloat( l, v, a, b, fmt, f );
	float* vs[]   = { v };
	return Fin( vs, &p, 1, r, wh );
}
inline bool SliderInt( const char* l, int* v, int a, int b, const char* fmt = "%d", ImGuiSliderFlags f = 0 )
{
	const bool wh = WasHovered( l );
	const int p  = *v;
	const bool r = ImGui::SliderInt( l, v, a, b, fmt, f );
	int* vs[]    = { v };
	return Fin( vs, &p, 1, r, wh );
}
inline bool DragFloat( const char* l, float* v, float sp = 1.0f, float a = 0.0f, float b = 0.0f, const char* fmt = "%.3f", ImGuiSliderFlags f = 0 )
{
	const bool wh = WasHovered( l );
	const float p = *v;
	const bool r  = ImGui::DragFloat( l, v, sp, a, b, fmt, f );
	float* vs[]   = { v };
	return Fin( vs, &p, 1, r, wh );
}
inline bool DragFloat2( const char* l, float v[ 2 ], float sp = 1.0f, float a = 0.0f, float b = 0.0f, const char* fmt = "%.3f", ImGuiSliderFlags f = 0 )
{
	const bool wh = WasHovered( l );
	const float p[ 2 ] = { v[ 0 ], v[ 1 ] };
	const bool r       = ImGui::DragFloat2( l, v, sp, a, b, fmt, f );
	float* vs[]        = { &v[ 0 ], &v[ 1 ] };
	return Fin( vs, p, 2, r, wh );
}
inline bool DragFloat3( const char* l, float v[ 3 ], float sp = 1.0f, float a = 0.0f, float b = 0.0f, const char* fmt = "%.3f", ImGuiSliderFlags f = 0 )
{
	const bool wh = WasHovered( l );
	const float p[ 3 ] = { v[ 0 ], v[ 1 ], v[ 2 ] };
	const bool r       = ImGui::DragFloat3( l, v, sp, a, b, fmt, f );
	float* vs[]        = { &v[ 0 ], &v[ 1 ], &v[ 2 ] };
	return Fin( vs, p, 3, r, wh );
}
inline bool DragInt( const char* l, int* v, float sp = 1.0f, int a = 0, int b = 0, const char* fmt = "%d", ImGuiSliderFlags f = 0 )
{
	const bool wh = WasHovered( l );
	const int p  = *v;
	const bool r = ImGui::DragInt( l, v, sp, a, b, fmt, f );
	int* vs[]    = { v };
	return Fin( vs, &p, 1, r, wh );
}
inline bool DragFloatRange2( const char* l, float* lo, float* hi, float sp = 1.0f, float a = 0.0f, float b = 0.0f, const char* fmt = "%.3f", const char* fmtMax = nullptr, ImGuiSliderFlags f = 0 )
{
	const bool wh = WasHovered( l );
	const float p[ 2 ] = { *lo, *hi };
	const bool r       = ImGui::DragFloatRange2( l, lo, hi, sp, a, b, fmt, fmtMax, f );
	float* vs[]        = { lo, hi };
	return Fin( vs, p, 2, r, wh );
}
}//namespace rs
}//namespace ls

namespace ls
{
//switch as a chip (pill button): lit = on. For toolbars - smaller and calmer than a check box. Returns "clicked"
inline float ChipWidth( const char* label ) { return ImGui::CalcTextSize( label, nullptr, true ).x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetFontSize() * 0.4f; }
inline bool Chip( const char* label, bool* v, const ImVec4& on = ImVec4( 0.35f, 0.65f, 1.0f, 1 ) )
{
	const bool a = *v;
	ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.5f );
	ImGui::PushStyleColor( ImGuiCol_Button, a ? ImVec4( on.x * 0.5f, on.y * 0.5f, on.z * 0.5f, 0.95f ) : ImVec4( 1, 1, 1, 0.06f ) );
	ImGui::PushStyleColor( ImGuiCol_ButtonHovered, a ? ImVec4( on.x * 0.62f, on.y * 0.62f, on.z * 0.62f, 1 ) : ImVec4( 1, 1, 1, 0.13f ) );
	ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4( on.x * 0.7f, on.y * 0.7f, on.z * 0.7f, 1 ) );
	ImGui::PushStyleColor( ImGuiCol_Text, a ? ImVec4( 1, 1, 1, 1 ) : ImVec4( 0.66f, 0.70f, 0.78f, 1 ) );
	const bool c = ImGui::Button( label, ImVec2( ChipWidth( label ), 0 ) );
	ImGui::PopStyleColor( 4 );
	ImGui::PopStyleVar();
	if( c ) *v = !*v;
	return c;
}
//section header (inspector, editors): neutral instead of the blue selection colour, so a selection stays the only blue thing
inline bool SectionHeader( const char* label, ImGuiTreeNodeFlags flags = 0 )
{
	ImGui::PushStyleColor( ImGuiCol_Header, ImVec4( 1, 1, 1, 0.055f ) );
	ImGui::PushStyleColor( ImGuiCol_HeaderHovered, ImVec4( 1, 1, 1, 0.10f ) );
	ImGui::PushStyleColor( ImGuiCol_HeaderActive, ImVec4( 1, 1, 1, 0.14f ) );
	const bool open = ImGui::CollapsingHeader( label, flags );
	ImGui::PopStyleColor( 3 );
	return open;
}
}//namespace ls
