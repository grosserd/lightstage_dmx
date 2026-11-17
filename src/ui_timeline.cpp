// LightStage 2.0: show timeline editor - bottom tab "Timeline" or its own big window.
// Tracks: fixture groups (lanes = attributes / colour), DMX channels, the cue list. Keyframes with curves.
#include "app.h"
#include "imgui.h"
#include "sim.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <map>

namespace ls
{
namespace
{
const ImU32 kBg = IM_COL32( 16, 18, 24, 255 ), kRowA = IM_COL32( 20, 23, 30, 255 ), kRowB = IM_COL32( 23, 26, 34, 255 );
const ImU32 kHead = IM_COL32( 29, 33, 43, 255 ), kGrid = IM_COL32( 34, 39, 49, 255 ), kGridHi = IM_COL32( 55, 62, 77, 255 );
const ImU32 kDim = IM_COL32( 120, 128, 145, 255 ), kTxt = IM_COL32( 222, 226, 234, 255 ), kAcc = IM_COL32( 90, 165, 255, 255 );
const ImU32 kSelC = IM_COL32( 255, 196, 70, 255 ), kPlayC = IM_COL32( 255, 86, 66, 255 );

struct Row
{
	int kind = 0;//0 track header, 1 lane, 2 cue track
	int track = 0, lane = -1;
	float y = 0, h = 0;//relative to the top of the track list (before scrolling)
};
struct LaneId
{
	uint32_t track;
	int lane;
	bool operator<( const LaneId& o ) const { return track != o.track ? track < o.track : lane < o.lane; }
};
struct TlUi
{
	bool init     = false;
	double view0  = 0;  //time at the left edge
	float pps     = 0;  //pixels per second
	float scrollY = 0;
	float headW   = 0;
	double userScrollT = -10;//last horizontal scroll / zoom by the user (the playhead follow waits a moment)
	bool vbar     = false;   //dragging the vertical scroll bar
	char lenBuf[ 32 ] = "";
	int drag      = 0;//1 move keys, 2 box select, 3 scrub, 4 Bezier handle, 5 marker, 6 cue, 7 loop range, 8 summary keys
	ImVec2 dragStart;
	double dragT0 = 0;
	float dragV0  = 0;
	int axis      = 0;//Shift while dragging: 1 time only, 2 value only
	std::map< LaneId, std::vector< std::pair< double, float > > > orig;//selected keys before the move (time order)
	LaneId hLane{ 0, -1 };
	int hKey     = -1;
	bool hOut    = true;
	int marker   = -1;
	int cue      = -1;
	double cueTc = 0;
	bool boxAdd  = false;
	uint32_t selTrack = 0;
	//clipboard: keys per lane (times relative to the first copied key)
	std::map< LaneId, std::vector< TlKey > > clip;
	//popups
	LaneId popLane{ 0, -1 };
	int popKey       = -1;
	int popSeg       = -1;//curve segment (index of its first key) for the segment menu
	uint32_t popTrack = 0;
	double popT      = 0;
	char name[ 96 ]  = "";
	int popMarker    = -1;
	int popCue       = -1;
	bool openKeyEdit = false, openRename = false, openMarker = false, openCueAdd = false, openCue = false;
	//clips: dragged clip and what it was before
	uint32_t clipTrack = 0;
	int clipIdx        = -1;
	TlClip clipOrig;
	bool openClipAdd = false, openFan = false;
	LaneId fanLane{ 0, -1 };
	double ltcRetry = 0;
};
TlUi g;

std::string TcStr( double s, int fps )
{
	if( s < 0 ) s = 0;
	const int fr = std::max( 1, fps );
	long long f  = (long long)floor( s * fr + 1e-6 );
	const int ff = (int)( f % fr );
	long long sec = f / fr;
	char b[ 48 ];
	snprintf( b, sizeof( b ), "%02lld:%02lld:%02lld:%02d", sec / 3600, ( sec / 60 ) % 60, sec % 60, ff );
	return b;
}
//length style: m:ss or h:mm:ss
std::string TimeStr( double s )
{
	const long long t = (long long)floor( std::max( 0.0, s ) + 0.5 );
	char b[ 32 ];
	if( t >= 3600 ) snprintf( b, sizeof( b ), "%lld:%02lld:%02lld", t / 3600, ( t / 60 ) % 60, t % 60 );
	else snprintf( b, sizeof( b ), "%02lld:%02lld", t / 60, t % 60 );
	return b;
}
//"90", "1:30", "1:30.5", "0:01:30", "00:01:30:12" (frames) -> seconds; false = not readable
bool ParseTime( const char* str, int fps, double& out )
{
	std::vector< double > p;
	std::string cur;
	for( const char* c = str;; ++c )
	{
		if( *c == ':' || *c == 0 )
		{
			if( cur.empty() ) return false;
			char* e = nullptr;
			for( auto& ch : cur )
				if( ch == ',' ) ch = '.';
			const double v = strtod( cur.c_str(), &e );
			if( !e || *e ) return false;
			p.push_back( v );
			cur.clear();
			if( !*c ) break;
		}
		else if( *c != ' ' ) cur += *c;
	}
	if( p.empty() || p.size() > 4 ) return false;
	double s = 0;
	if( p.size() == 4 ) s = p[ 0 ] * 3600 + p[ 1 ] * 60 + p[ 2 ] + p[ 3 ] / std::max( 1, fps );
	else
		for( double v : p ) s = s * 60 + v;
	if( s < 0 ) return false;
	out = s;
	return true;
}
std::string BarStr( const Timeline& tl, double s )
{
	const double beats = s * std::max( 1.0f, tl.bpm ) / 60.0;
	const int bpb      = std::max( 1, tl.beatsPerBar );
	const long long b  = (long long)floor( beats + 1e-6 );
	char x[ 48 ];
	snprintf( x, sizeof( x ), "%lld.%lld.%d", b / bpb + 1, b % bpb + 1, (int)floor( ( beats - b ) * 4 ) + 1 );
	return x;
}
bool IconButton( const char* id, int icon, bool on, float ui )
{
	const float h = ImGui::GetFrameHeight();
	const ImVec2 sz( h * 1.25f, h );
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const bool hit = ImGui::InvisibleButton( id, sz );
	const bool hov = ImGui::IsItemHovered();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled( p, ImVec2( p.x + sz.x, p.y + sz.y ), on ? IM_COL32( 60, 100, 170, 255 ) : ( hov ? IM_COL32( 50, 60, 80, 255 ) : IM_COL32( 36, 40, 51, 255 ) ), 4 * ui );
	const ImVec2 c( p.x + sz.x * 0.5f, p.y + sz.y * 0.5f );
	const float r   = h * 0.26f;
	const ImU32 col = IM_COL32( 230, 233, 240, 255 );
	switch( icon )
	{
	case 0://to start
		dl->AddRectFilled( ImVec2( c.x - r, c.y - r ), ImVec2( c.x - r + 2.2f * ui, c.y + r ), col );
		dl->AddTriangleFilled( ImVec2( c.x + r, c.y - r ), ImVec2( c.x + r, c.y + r ), ImVec2( c.x - r * 0.4f, c.y ), col );
		break;
	case 1://play
		dl->AddTriangleFilled( ImVec2( c.x - r * 0.7f, c.y - r ), ImVec2( c.x - r * 0.7f, c.y + r ), ImVec2( c.x + r, c.y ), on ? IM_COL32( 120, 255, 150, 255 ) : col );
		break;
	case 2://pause
		dl->AddRectFilled( ImVec2( c.x - r * 0.8f, c.y - r ), ImVec2( c.x - r * 0.2f, c.y + r ), col );
		dl->AddRectFilled( ImVec2( c.x + r * 0.2f, c.y - r ), ImVec2( c.x + r * 0.8f, c.y + r ), col );
		break;
	case 3://stop
		dl->AddRectFilled( ImVec2( c.x - r * 0.8f, c.y - r * 0.8f ), ImVec2( c.x + r * 0.8f, c.y + r * 0.8f ), col );
		break;
	case 5://record
		dl->AddCircleFilled( c, r * 0.85f, on ? IM_COL32( 255, 70, 60, 255 ) : IM_COL32( 200, 80, 75, 255 ) );
		break;
	case 4://loop: two arrows around
		dl->PathArcTo( c, r * 1.05f, 0.4f, 2.9f, 10 );
		dl->PathStroke( col, 0, 1.6f * ui );
		dl->PathArcTo( c, r * 1.05f, 3.55f, 6.05f, 10 );
		dl->PathStroke( col, 0, 1.6f * ui );
		dl->AddTriangleFilled( ImVec2( c.x - r * 1.45f, c.y + r * 0.05f ), ImVec2( c.x - r * 0.65f, c.y + r * 0.05f ), ImVec2( c.x - r * 1.05f, c.y + r * 0.55f ), col );
		dl->AddTriangleFilled( ImVec2( c.x + r * 1.45f, c.y - r * 0.05f ), ImVec2( c.x + r * 0.65f, c.y - r * 0.05f ), ImVec2( c.x + r * 1.05f, c.y - r * 0.55f ), col );
		break;
	}
	return hit;
}
//next toolbar group on the same line when it fits, else on a new line
void GroupBreak( float need, float ui )
{
	ImGui::SameLine( 0, 14 * ui );
	if( ImGui::GetContentRegionAvail().x < need ) ImGui::NewLine();
}
ImU32 ColU( const V3& c, float a = 1 )
{
	return ImGui::ColorConvertFloat4ToU32( ImVec4( Saturate( c.x ), Saturate( c.y ), Saturate( c.z ), a ) );
}
//colour of a timelane: by attribute (pan is always cyan, tilt pink ...), raw DMX lanes from a palette
V3 LaneColor( const TlLane& l, int idx )
{
	static const V3 pal[ 8 ] = { V3( 1.0f, 0.78f, 0.3f ), V3( 0.35f, 0.8f, 1.0f ), V3( 1.0f, 0.45f, 0.78f ), V3( 0.6f, 0.92f, 0.35f ),
		                         V3( 1.0f, 0.58f, 0.25f ), V3( 0.72f, 0.55f, 1.0f ), V3( 0.3f, 0.95f, 0.8f ), V3( 0.95f, 0.95f, 0.55f ) };
	if( l.kind == TlLaneKind::Color ) return V3( 1.0f, 0.5f, 0.85f );
	if( l.kind == TlLaneKind::Dmx ) return pal[ ( idx % 8 + 8 ) % 8 ];
	switch( l.attr )
	{
	case Attr::Dimmer: return V3( 1.0f, 0.82f, 0.35f );
	case Attr::Shutter: return V3( 1.0f, 0.42f, 0.42f );
	case Attr::Pan: return V3( 0.35f, 0.8f, 1.0f );
	case Attr::Tilt: return V3( 1.0f, 0.45f, 0.8f );
	case Attr::PanTiltSpeed: return V3( 0.6f, 0.65f, 0.75f );
	case Attr::Zoom: return V3( 0.6f, 0.92f, 0.35f );
	case Attr::Focus: return V3( 0.4f, 0.95f, 0.75f );
	case Attr::Gobo: case Attr::GoboRot: return V3( 1.0f, 0.6f, 0.25f );
	case Attr::Prism: case Attr::PrismRot: return V3( 0.75f, 0.58f, 1.0f );
	case Attr::Frost: return V3( 0.8f, 0.86f, 0.95f );
	case Attr::Iris: return V3( 0.95f, 0.95f, 0.5f );
	case Attr::CTO: return V3( 1.0f, 0.7f, 0.4f );
	case Attr::ColorWheel: case Attr::ColorMacro: return V3( 1.0f, 0.5f, 0.9f );
	case Attr::Fog: return V3( 0.72f, 0.74f, 0.8f );
	default:
		if( IsEmitter( l.attr ) )
		{
			const V3 e = EmitterColor( l.attr );
			const float mx = std::max( { e.x, e.y, e.z, 0.001f } );
			const V3 n( e.x / mx, e.y / mx, e.z / mx );
			return V3( 0.3f + 0.7f * n.x, 0.3f + 0.7f * n.y, 0.3f + 0.7f * n.z );//lighter: readable on the dark rows
		}
		return pal[ ( idx % 8 + 8 ) % 8 ];
	}
}
float LaneH( int h, float ui ) { return ( h == 0 ? 30.0f : h == 2 ? 120.0f : 62.0f ) * ui; }
void ClearSel( Timeline& tl )
{
	for( auto& t : tl.tracks )
		for( auto& l : t.lanes )
			for( auto& k : l.keys ) k.sel = false;
}
int CountSel( Timeline& tl )
{
	int n = 0;
	for( auto& t : tl.tracks )
		for( auto& l : t.lanes )
			for( auto& k : l.keys ) n += k.sel;
	return n;
}
//value text of a lane value (percent, DMX for raw channels)
std::string ValText( const TlLane& l, float v )
{
	char b[ 32 ];
	if( l.kind == TlLaneKind::Dmx ) snprintf( b, sizeof( b ), l.fine ? "%.0f" : "%.0f", l.fine ? v * 65535.0f : v * 255.0f );
	else snprintf( b, sizeof( b ), "%.0f %%", v * 100.0f );
	return b;
}
//lane value as text, pan / tilt in degrees when the track's lamps have a range
std::string LaneVal( Show& s, const TlTrack& t, const TlLane& l, float v )
{
	float deg;
	if( l.kind == TlLaneKind::Attr && TlDegrees( s, t, l.attr, v, deg ) )
	{
		char b[ 32 ];
		snprintf( b, sizeof( b ), "%.0f%%  %+.0f\xc2\xb0", v * 100, deg );
		return b;
	}
	return ValText( l, v );
}
const char* ClipName( Show& s, const TlClip& c )
{
	if( c.kind == TlClipKind::Effect )
	{
		const Effect* e = s.FindEffect( c.fx );
		return e ? e->name.c_str() : "?";
	}
	const int i = s.FindSnapUid( c.snap );
	return i >= 0 ? s.snapshots[ i ].name.c_str() : "?";
}
}//namespace

//---------------------------------------------------------------- audio, LTC, 3D path
void App::TimelineTick()
{
	Timeline& tl = show.tl;
	tlSelTrack_  = g.selTrack;
	//LTC input runs while the timeline follows LTC
	if( tl.sync == TlSync::Ltc )
	{
		const bool same = ltcIn_.Running() && ltcIn_.Device() == tl.ltcDevice && ltcIn_.Channel() == tl.ltcChannel;
		if( !same && Now() > g.ltcRetry )
		{
			if( !ltcIn_.Start( tl.ltcDevice, tl.ltcChannel ) ) g.ltcRetry = Now() + 2.0;
		}
	}
	else if( ltcIn_.Running() )
		ltcIn_.Stop();
	LbxEnsurePlayers( show );//Lightshow tracks: frames from their files
	//audio track: (re)load the file, play along with the timeline
	TlTrack* at = nullptr;
	for( auto& t : tl.tracks )
		if( t.kind == TlTrackKind::Audio ) { at = &t; break; }
	const std::string file = at ? at->file : std::string();
	if( file != tlAudioFile_ )
	{
		tlAudio_.Stop();
		tlAudioFile_ = file;
		tlAudioData_.reset();
		tlAudioErr_.clear();
		if( !file.empty() )
		{
			auto d = std::make_shared< AudioData >();
			std::string err;
			if( LoadAudioFile( file, *d, err ) )
			{
				tlAudioData_ = d;
				tlAudio_.SetData( d );
			}
			else
				tlAudioErr_ = err;
		}
	}
	bool solo = false;
	for( auto& t : tl.tracks ) solo = solo || t.solo;
	const bool play = at && !at->mute && ( !solo || at->solo ) && tlAudioData_ && tl.rt.playing && !ExportActive();
	if( play )
	{
		const double want = tl.rt.pos - at->offset;
		tlAudio_.SetGain( at->gain );
		if( want < tlAudioData_->Seconds() && ( !tlAudio_.Playing() || fabs( tlAudio_.Position() - want ) > 0.08 ) ) tlAudio_.Play( want );
		else if( tl.sync == TlSync::Internal && tlAudio_.Playing() && tlAudio_.Position() >= 0 )
		{
			//internal clock: the sound card is the master (no drift between light and sound over long shows)
			const double p = tlAudio_.Position() + at->offset;
			if( p > tl.rt.pos - 0.08 && p < tl.rt.pos + 0.08 && p < tl.length ) tl.rt.pos = p;
		}
	}
	else if( tlAudio_.Playing() )
		tlAudio_.Stop();
}

void App::TimelineViewportOverlay( ImDrawList* dl, const ImVec2& p0, const ImVec2& size )
{
	if( !tlPath_ ) return;
	Timeline& tl     = show.tl;
	const TlTrack* t = tl.Find( tlSelTrack_ );
	if( !t || t->kind != TlTrackKind::Fixtures ) return;
	const TlLane *lp = nullptr, *lt = nullptr;
	for( auto& l : t->lanes )
		if( l.kind == TlLaneKind::Attr && !l.keys.empty() )
		{
			if( l.attr == Attr::Pan ) lp = &l;
			if( l.attr == Attr::Tilt ) lt = &l;
		}
	if( !lp && !lt ) return;
	double t0 = 1e18, t1 = -1e18;
	for( const TlLane* l : { lp, lt } )
		if( l )
		{
			t0 = std::min( t0, l->keys.front().t - std::max( 0.0f, l->fanTime ) );
			t1 = std::max( t1, l->keys.back().t - std::min( 0.0f, l->fanTime ) );
		}
	t0 = std::max( 0.0, t0 );
	if( t1 <= t0 ) t1 = t0 + 1;
	const M4& m = ren.viewProj;
	auto proj   = [ & ]( const V3& w, ImVec2& o ) {
		const float cx = m( 0, 0 ) * w.x + m( 0, 1 ) * w.y + m( 0, 2 ) * w.z + m( 0, 3 );
		const float cy = m( 1, 0 ) * w.x + m( 1, 1 ) * w.y + m( 1, 2 ) * w.z + m( 1, 3 );
		const float cw = m( 3, 0 ) * w.x + m( 3, 1 ) * w.y + m( 3, 2 ) * w.z + m( 3, 3 );
		if( cw <= 0.1f ) return false;
		o = ImVec2( p0.x + ( cx / cw * 0.5f + 0.5f ) * size.x, p0.y + ( 0.5f - cy / cw * 0.5f ) * size.y );
		return true;
	};
	std::vector< float > fpP, fpT;
	if( lp ) TlFanPos( show, *t, lp->fanOrder, fpP );
	if( lt ) TlFanPos( show, *t, lt->fanOrder, fpT );
	const ImU32 col = ImGui::ColorConvertFloat4ToU32( ImVec4( t->color.x, t->color.y, t->color.z, 0.85f ) );
	dl->PushClipRect( p0, ImVec2( p0.x + size.x, p0.y + size.y ), true );
	int drawn = 0;
	for( size_t k = 0; k < t->fixtures.size() && drawn < 12; ++k )
	{
		const FixtureInst* f = show.FindFixture( t->fixtures[ k ] );
		if( !f || !f->def || !show.HasAttr( *f, Attr::Pan ) ) continue;
		++drawn;
		const M4 base = FixtureBase( *f );
		const V3 e    = BodyExtents( *f->def );
		const V3 oL( 0, e.y * 0.62f, 0 );
		const V3 o = XformP( base, oL );
		auto hit   = [ & ]( double T, ImVec2& sp ) {
			const float pv = lp ? TlEval( *lp, T - ( lp->HasFan() ? lp->fanTime * fpP[ k ] : 0 ) ) + ( lp->HasFan() ? lp->fanValue * ( fpP[ k ] - 0.5f ) : 0 ) : show.GetAttr( *f, Attr::Pan );
			const float tv = lt ? TlEval( *lt, T - ( lt->HasFan() ? lt->fanTime * fpT[ k ] : 0 ) ) + ( lt->HasFan() ? lt->fanValue * ( fpT[ k ] - 0.5f ) : 0 ) : show.GetAttr( *f, Attr::Tilt );
			float pd = ( Saturate( pv ) - 0.5f ) * f->def->panRange, td = ( Saturate( tv ) - 0.5f ) * f->def->tiltRange;
			if( f->invertPan ) pd = -pd;
			if( f->invertTilt ) td = -td;
			const float P = pd * kDeg, Tt = td * kDeg;
			const V3 q( sinf( Tt ) * sinf( P ), cosf( Tt ), sinf( Tt ) * cosf( P ) );
			V3 d = XformP( base, oL + q ) - o;
			d    = Norm( d );
			float tt = 25;
			float tdk;
			if( show.RayDeckTop( o, d, tdk ) && tdk > 0 ) tt = std::min( tt, tdk );
			if( d.y < -1e-3f ) tt = std::min( tt, -o.y / d.y );
			return proj( o + d * tt, sp );
		};
		ImVec2 prev;
		bool hasPrev = false;
		const int N  = 240;
		for( int i = 0; i <= N; ++i )
		{
			ImVec2 sp;
			const bool ok = hit( t0 + ( t1 - t0 ) * i / N, sp );
			if( ok && hasPrev ) dl->AddLine( prev, sp, col, 1.6f * ui );
			prev    = sp;
			hasPrev = ok;
		}
		ImVec2 cur;
		if( hit( tl.rt.pos, cur ) )
		{
			dl->AddCircleFilled( cur, 5 * ui, IM_COL32( 255, 86, 66, 255 ) );
			dl->AddCircle( cur, 7 * ui, IM_COL32( 255, 255, 255, 180 ), 0, 1.5f * ui );
		}
	}
	dl->PopClipRect();
}

//---------------------------------------------------------------- tracks from the patch / channel lists
uint32_t App::AddTimelineTrack( const std::vector< int >& ids, Attr attr, bool pickLanes )
{
	Timeline& tl = show.tl;
	TlTrack t;
	t.uid  = tl.NewUid();
	t.kind = TlTrackKind::Fixtures;
	t.fixtures = ids;
	t.color    = Hsv( fmodf( t.uid * 0.137f + 0.52f, 1.0f ), 0.55f, 0.9f );
	if( ids.size() == 1 )
	{
		const FixtureInst* f = show.FindFixture( ids[ 0 ] );
		t.name               = f ? f->name : L( "Spur" );
	}
	else
	{
		//name: the lamps' common type, else "n Lampen"
		std::string type;
		bool same = true;
		for( int id : ids )
			if( const FixtureInst* f = show.FindFixture( id ) )
			{
				const std::string n = f->def ? f->def->name : f->name;
				if( type.empty() ) type = n;
				else if( type != n ) same = false;
			}
		t.name = same && !type.empty() ? type + " x" + std::to_string( ids.size() ) : std::to_string( ids.size() ) + L( " Lampen" );
	}
	if( attr != Attr::None )
	{
		TlLane l;
		l.attr = attr;
		t.lanes.push_back( l );
	}
	tl.tracks.push_back( t );
	g.selTrack = t.uid;
	if( pickLanes && attr == Attr::None ) tlLanePick_ = t.uid;
	tlSelectTab_ = true;
	show.dirty   = true;
	SetStatus( L( "Time-Line-Spur angelegt: " ) + t.name );
	return t.uid;
}

void App::TimelineAddMenu( const std::vector< int >& ids, const Channel* ch, int uni, int adr )
{
	Timeline& tl = show.tl;
	if( !ids.empty() )
	{
		char b[ 96 ];
		if( ch ) snprintf( b, sizeof( b ), L( "Neue Spur mit Time Lane \"%s\" (%d Lampe(n))" ), AttrName( ch->attr ), (int)ids.size() );
		else snprintf( b, sizeof( b ), L( "Neue Gruppenspur aus Auswahl (%d Lampe(n))..." ), (int)ids.size() );
		if( ImGui::MenuItem( b ) ) AddTimelineTrack( ids, ch ? ch->attr : Attr::None, true );
		bool sep = false;
		for( auto& t : tl.tracks )
		{
			if( t.kind != TlTrackKind::Fixtures ) continue;
			if( !sep ) { ImGui::Separator(); sep = true; }
			ImGui::PushID( (int)t.uid );
			if( ch ) snprintf( b, sizeof( b ), L( "Neue Time Lane \"%s\" in Spur \"%s\"" ), AttrName( ch->attr ), t.name.c_str() );
			else snprintf( b, sizeof( b ), L( "Zu Spur \"%s\" hinzufügen" ), t.name.c_str() );
			if( ImGui::MenuItem( b ) )
			{
				for( int id : ids )
					if( std::find( t.fixtures.begin(), t.fixtures.end(), id ) == t.fixtures.end() ) t.fixtures.push_back( id );
				if( ch )
				{
					bool has = false;
					for( auto& l : t.lanes ) has = has || ( l.kind == TlLaneKind::Attr && l.attr == ch->attr );
					if( !has )
					{
						TlLane l;
						l.attr = ch->attr;
						t.lanes.push_back( l );
					}
				}
				else
					tlLanePick_ = t.uid;
				t.open       = true;
				g.selTrack   = t.uid;
				tlSelectTab_ = true;
				show.dirty   = true;
			}
			ImGui::PopID();
		}
	}
	if( adr > 0 )
	{
		if( !ids.empty() ) ImGui::Separator();
		char b[ 96 ];
		snprintf( b, sizeof( b ), L( "Neue DMX-Time-Lane (Kanal %d.%03d)" ), uni, adr );
		if( ImGui::MenuItem( b ) )
		{
			TlTrack* dt = nullptr;
			for( auto& t : tl.tracks )
				if( t.kind == TlTrackKind::Dmx ) { dt = &t; break; }
			if( !dt )
			{
				TlTrack t;
				t.uid   = tl.NewUid();
				t.kind  = TlTrackKind::Dmx;
				t.name  = L( "DMX-Kanäle" );
				t.color = V3( 0.75f, 0.75f, 0.8f );
				tl.tracks.push_back( t );
				dt = &tl.tracks.back();
			}
			TlLane l;
			l.kind = TlLaneKind::Dmx;
			l.uni  = uni;
			l.ch   = adr;
			l.fine = ch && ch->fine >= 0;
			dt->lanes.push_back( l );
			dt->open     = true;
			tlSelectTab_ = true;
			show.dirty   = true;
		}
	}
}

void App::TimelineChannelMenu( int uni, int adr )
{
	int slot             = -1;
	FixtureInst* f       = show.FixtureAt( uni, adr, &slot );
	const Mode* m        = f ? f->M() : nullptr;
	const Channel* ch    = nullptr;
	int coarse           = adr;
	if( m && slot >= 0 && slot < (int)m->ch.size() )
	{
		ch = &m->ch[ slot ];
		if( ch->isFine )//fine byte: the lane belongs to its coarse channel
			for( int k = 0; k < (int)m->ch.size(); ++k )
				if( m->ch[ k ].fine == slot ) { ch = &m->ch[ k ]; coarse = f->address + k; break; }
	}
	ImGui::TextDisabled( "%s %d.%03d%s%s", L( "Time Line: Kanal" ), uni, adr, f ? "  " : "", f ? ( f->name + " - " + ( ch ? ch->name : std::string() ) ).c_str() : "" );
	std::vector< int > ids;
	if( f ) ids = IsSel( f->id ) ? sel : std::vector< int >{ f->id };
	TimelineAddMenu( ids, ch && ch->attr != Attr::None && ch->attr != Attr::Generic ? ch : nullptr, uni, coarse );
}

void App::BuildTimelineDemo()
{
	Timeline& tl = show.tl;
	tl           = Timeline();
	tl.length    = 96;
	tl.bpm       = 125;
	tl.grid      = 2;
	const double bar = 60.0 / tl.bpm * 4;
	//moving heads and washes / pars of the show
	std::vector< int > heads, pars;
	for( auto& f : show.fixtures )
	{
		if( !f.def ) continue;
		if( show.HasAttr( f, Attr::Pan ) && show.HasAttr( f, Attr::Tilt ) ) { if( heads.size() < 8 ) heads.push_back( f.id ); }
		else if( show.HasAttr( f, Attr::Red ) && pars.size() < 12 ) pars.push_back( f.id );
	}
	//cue track
	{
		TlTrack t;
		t.uid   = tl.NewUid();
		t.kind  = TlTrackKind::Cues;
		t.name  = L( "Cues" );
		t.color = V3( 1.0f, 0.7f, 0.3f );
		tl.tracks.push_back( t );
		for( int i = 0; i < (int)show.snapshots.size() && i < 3; ++i )
		{
			Cue c;
			c.name    = show.snapshots[ i ].name;
			c.snap    = show.snapshots[ i ].uid;
			c.trigger = 2;
			c.tc      = bar * ( 4 + i * 8 );
			show.cues.push_back( c );
		}
	}
	if( !pars.empty() )
	{
		TlTrack t;
		t.uid      = tl.NewUid();
		t.name     = "Wash";
		t.fixtures = pars;
		t.color    = V3( 0.35f, 0.75f, 1.0f );
		TlLane dim;
		dim.attr = Attr::Dimmer;
		TlSetKey( dim, 0, 0, nullptr, TlInterp::Smooth );
		TlSetKey( dim, bar * 2, 0.85f, nullptr, TlInterp::Linear );
		TlSetKey( dim, bar * 6, 0.85f, nullptr, TlInterp::EaseIn );
		TlSetKey( dim, bar * 8, 0.2f, nullptr, TlInterp::Step );
		TlSetKey( dim, bar * 9, 1.0f, nullptr, TlInterp::Bezier );
		dim.keys.back().ox = 0.15f;
		dim.keys.back().oy = -0.6f;
		TlSetKey( dim, bar * 14, 0.6f, nullptr, TlInterp::Smooth );
		TlSetKey( dim, bar * 18, 0.0f, nullptr, TlInterp::Smooth );
		t.lanes.push_back( dim );
		TlLane col;
		col.kind = TlLaneKind::Color;
		const V3 cs[] = { V3( 0.1f, 0.3f, 1 ), V3( 0.6f, 0.1f, 1 ), V3( 1, 0.1f, 0.4f ), V3( 1, 0.55f, 0.1f ), V3( 0.1f, 0.9f, 0.8f ) };
		for( int i = 0; i < 5; ++i ) TlSetKey( col, bar * i * 4, 0, &cs[ i ], TlInterp::Linear );
		t.lanes.push_back( col );
		tl.tracks.push_back( t );
	}
	if( !heads.empty() )
	{
		TlTrack t;
		t.uid      = tl.NewUid();
		t.name     = "Spots";
		t.fixtures = heads;
		t.color    = V3( 0.6f, 1.0f, 0.5f );
		TlLane pan, tilt, dim;
		pan.attr  = Attr::Pan;
		tilt.attr = Attr::Tilt;
		dim.attr  = Attr::Dimmer;
		for( int i = 0; i <= 8; ++i )
		{
			TlSetKey( pan, bar * i * 2, i % 2 ? 0.35f : 0.65f, nullptr, TlInterp::Smooth );
			TlSetKey( tilt, bar * i * 2 + bar, i % 2 ? 0.55f : 0.4f, nullptr, TlInterp::Smooth );
		}
		TlSetKey( dim, bar * 4, 0, nullptr, TlInterp::Linear );
		TlSetKey( dim, bar * 5, 1, nullptr, TlInterp::Step );
		TlSetKey( dim, bar * 12, 0.5f, nullptr, TlInterp::Linear );
		TlSetKey( dim, bar * 16, 1, nullptr, TlInterp::Linear );
		t.lanes.push_back( dim );
		t.lanes.push_back( pan );
		t.lanes.push_back( tilt );
		tl.tracks.push_back( t );
	}
	//clip track: the snapshots as blocks
	if( !show.snapshots.empty() )
	{
		TlTrack t;
		t.uid   = tl.NewUid();
		t.kind  = TlTrackKind::Clips;
		t.name  = L( "Clips" );
		t.color = V3( 0.75f, 0.5f, 1.0f );
		for( int i = 0; i < (int)show.snapshots.size() && i < 4; ++i )
		{
			TlClip c;
			c.snap    = show.snapshots[ i ].uid;
			c.t       = bar * ( 2 + i * 5 );
			c.len     = bar * 4;
			c.fadeIn  = (float)bar * ( i % 2 ? 0.5f : 1.5f );
			c.fadeOut = (float)bar;
			c.level   = i == 2 ? 0.7f : 1.0f;
			t.clips.push_back( c );
		}
		tl.tracks.push_back( t );
	}
	//fan on the spots' pan, the tilt lane armed (look of the editor)
	if( !heads.empty() )
	{
		TlTrack& sp = tl.tracks[ tl.tracks.size() - ( show.snapshots.empty() ? 1 : 2 ) ];
		for( auto& l : sp.lanes )
		{
			if( l.attr == Attr::Pan ) { l.fanTime = (float)bar * 0.5f; l.fanOrder = 1; }
			if( l.attr == Attr::Tilt ) l.rec = true;
		}
	}
	//audio: a generated click track (125 BPM) in the temp folder
	{
		wchar_t tmp[ MAX_PATH ];
		GetTempPathW( MAX_PATH, tmp );
		const std::string wav = Narrow( tmp ) + "lightstage_demo_click.wav";
		const int rate = 22050, n = (int)( rate * tl.length );
		std::vector< int16_t > pcm( n );
		const double spb = 60.0 / tl.bpm;
		for( int i = 0; i < n; ++i )
		{
			const double t = (double)i / rate, ph = fmod( t, spb );
			const double bar4 = fmod( t, spb * 4 );
			const double env = exp( -ph * 18 ), hat = fmod( t + spb * 0.5, spb ) < 0.03 ? 0.25 * ( ( i * 7919 % 2000 ) / 1000.0 - 1 ) : 0;
			const double build = std::min( 1.0, 0.35 + 0.65 * ( t / tl.length ) ) * ( bar4 < spb * 3.5 ? 1.0 : 0.6 );
			pcm[ i ] = (int16_t)( 26000 * build * ( env * sin( 2 * 3.14159265 * ( 55 + 90 * exp( -ph * 30 ) ) * ph ) + hat ) );
		}
		FILE* fw = fopen( wav.c_str(), "wb" );
		if( fw )
		{
			auto u32 = [ & ]( uint32_t v ) { fwrite( &v, 4, 1, fw ); };
			auto u16 = [ & ]( uint16_t v ) { fwrite( &v, 2, 1, fw ); };
			fwrite( "RIFF", 1, 4, fw ); u32( 36 + n * 2 ); fwrite( "WAVEfmt ", 1, 8, fw ); u32( 16 ); u16( 1 ); u16( 1 ); u32( rate ); u32( rate * 2 ); u16( 2 ); u16( 16 );
			fwrite( "data", 1, 4, fw ); u32( n * 2 ); fwrite( pcm.data(), 2, pcm.size(), fw );
			fclose( fw );
			TlTrack t;
			t.uid   = tl.NewUid();
			t.kind  = TlTrackKind::Audio;
			t.name  = "Click 125 BPM";
			t.file  = wav;
			t.color = V3( 0.55f, 0.6f, 0.7f );
			tl.tracks.insert( tl.tracks.begin(), t );
		}
	}
	tl.markers.push_back( { bar * 4, "Intro" } );
	tl.markers.push_back( { bar * 12, "Drop" } );
	tl.markers.push_back( { bar * 20, "Break" } );
	tl.loopA = bar * 12;
	tl.loopB = bar * 16;
	tl.Locate( show, bar * 10.5 );
	for( auto& t : tl.tracks )
	{
		if( t.name == "Spots" ) g.selTrack = t.uid;
		if( t.name == "Wash" ) t.open = false;
	}
	if( !tl.tracks.empty() && tl.tracks.back().lanes.size() > 1 ) tl.tracks.back().lanes[ 1 ].keys[ 3 ].sel = true;
	tlSelectTab_ = true;
}

void App::TimelineToggle()
{
	Timeline& tl = show.tl;
	tl.Play( show, !tl.rt.playing );
}

//---------------------------------------------------------------- the editor
void App::TabTimeline()
{
	if( tlWin_ )
	{
		ImGui::TextDisabled( L( "Die Time Line ist im eigenen Fenster geöffnet." ) );
		ImGui::SameLine();
		if( ImGui::Button( L( "Hierher zurückholen" ) ) ) tlWin_ = false;
		return;
	}
	TimelinePanel();
}
void App::TimelineWindow()
{
	if( !tlWin_ ) return;
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowPos( ImVec2( ds.x * 0.04f, ds.y * 0.12f ), ImGuiCond_FirstUseEver );
	ImGui::SetNextWindowSize( ImVec2( ds.x * 0.92f, ds.y * 0.8f ), ImGuiCond_FirstUseEver );
	if( ImGui::Begin( L( "Show-Time-Line###tlwin" ), &tlWin_, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse ) ) TimelinePanel();
	ImGui::End();
}

void App::TimelinePanel()
{
	if( tlConvert_ ) { TlConvertShowTrack( tlConvert_ ); tlConvert_ = 0; }
	Timeline& tl  = show.tl;
	TlRuntime& rt = tl.rt;
	ImGuiIO& io   = ImGui::GetIO();
	const float lh = ImGui::GetTextLineHeight();
	if( !g.init )
	{
		g.headW = 250 * ui;
		g.init  = true;
	}
	//================= toolbar
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 5 * ui, 5 * ui ) );
	if( IconButton( "##tlstart", 0, false, ui ) ) { tl.Locate( show, tl.loop ? tl.loopA : 0 ); g.view0 = std::min( g.view0, rt.pos ); }
	if( ImGui::IsItemHovered() ) ImGui::SetTooltip( L( "Zum Anfang (Pos1)" ) );
	ImGui::SameLine();
	if( IconButton( "##tlplay", rt.playing ? 2 : 1, rt.playing, ui ) ) TimelineToggle();
	if( ImGui::IsItemHovered() ) ImGui::SetTooltip( L( "Abspielen / Pause (Leertaste, Maus über der Time Line)" ) );
	ImGui::SameLine();
	if( IconButton( "##tlstop", 3, false, ui ) )
	{
		tl.Play( show, false );
		tl.Locate( show, tl.loop ? tl.loopA : 0 );
	}
	if( ImGui::IsItemHovered() ) ImGui::SetTooltip( L( "Stopp und zurück zum Anfang" ) );
	ImGui::SameLine();
	if( IconButton( "##tlloop", 4, tl.loop, ui ) ) { tl.loop = !tl.loop; show.dirty = true; }
	if( ImGui::IsItemHovered() ) ImGui::SetTooltip( L( "Loop (Bereich: Shift + Ziehen im Zeitlineal)" ) );
	ImGui::SameLine();
	if( IconButton( "##tlrec", 5, tl.record, ui ) )
	{
		tl.record = !tl.record;
		if( tl.record )
		{
			int armed = 0;
			for( auto& t : tl.tracks )
				for( auto& l : t.lanes ) armed += l.rec;
			if( !armed ) SetStatus( L( "Aufnahme: erst Time Lanes scharf schalten (R im Time-Lane-Kopf)" ) );
		}
	}
	if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Aufnahme: scharf geschaltete Time Lanes (R) nehmen beim Abspielen die Programmer-Werte auf\n(Inspector, Gruppenfader, MIDI). Rechtsklick = Modus und Ausdünnung." ) );
	if( ImGui::BeginPopupContextItem( "##tlrecctx" ) )
	{
		ImGui::TextDisabled( "%s", L( "Aufnahme-Modus" ) );
		if( ImGui::RadioButton( L( "Überschreiben (ganzer Durchlauf)" ), tl.recMode == 0 ) ) { tl.recMode = 0; show.dirty = true; }
		if( ImGui::RadioButton( L( "Touch (nur solange bewegt, dann zurück zur Kurve)" ), tl.recMode == 1 ) ) { tl.recMode = 1; show.dirty = true; }
		if( ImGui::RadioButton( L( "Latch (ab der ersten Bewegung bis Stopp)" ), tl.recMode == 2 ) ) { tl.recMode = 2; show.dirty = true; }
		float th = tl.recThin * 100;
		ImGui::SetNextItemWidth( 200 * ui );
		if( ImGui::SliderFloat( L( "Ausdünnen" ), &th, 0, 5, "%.1f %%" ) ) { tl.recThin = th / 100; show.dirty = true; }
		ImGui::EndPopup();
	}
	if( tl.record )
	{
		ImGui::SameLine();
		ImGui::TextColored( ImVec4( 1, 0.35f, 0.3f, 1 ), "%s", tl.recMode == 0 ? "REC" : tl.recMode == 1 ? "REC Touch" : "REC Latch" );
	}
	ImGui::SameLine();
	//time display
	{
		ImGui::PushFont( fontMono );
		const std::string tc = TcStr( rt.pos, tl.fps );
		const std::string br = BarStr( tl, rt.pos );
		const ImVec2 p       = ImGui::GetCursorScreenPos();
		const float w        = ImGui::CalcTextSize( "00:00:00:00   000.0.0" ).x + 16 * ui;
		ImGui::GetWindowDrawList()->AddRectFilled( p, ImVec2( p.x + w, p.y + ImGui::GetFrameHeight() ), IM_COL32( 10, 12, 16, 255 ), 4 * ui );
		ImGui::GetWindowDrawList()->AddText( ImVec2( p.x + 8 * ui, p.y + ( ImGui::GetFrameHeight() - ImGui::GetTextLineHeight() ) * 0.5f ), rt.playing ? IM_COL32( 120, 255, 150, 255 ) : kTxt, tc.c_str() );
		ImGui::GetWindowDrawList()->AddText( ImVec2( p.x + 8 * ui + ImGui::CalcTextSize( "00:00:00:00   " ).x, p.y + ( ImGui::GetFrameHeight() - ImGui::GetTextLineHeight() ) * 0.5f ), kDim, br.c_str() );
		ImGui::Dummy( ImVec2( w, ImGui::GetFrameHeight() ) );
		ImGui::PopFont();
	}
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted( "Sync" );
	ImGui::SameLine();
	ImGui::SetNextItemWidth( 128 * ui );
	{
		int s = (int)tl.sync;
		if( ImGui::Combo( "##tlsync", &s, L( "Intern\0MIDI-Timecode\0Beat\0LTC\0" ) ) )
		{
			tl.sync     = (TlSync)s;
			rt.extRun   = false;
			rt.lastBeat = show.tempo.beat;
			show.dirty  = true;
		}
		if( ImGui::IsItemHovered() )
			ImGui::SetTooltip( "%s", L( "Intern: Play-Knopf / Leertaste.\nMIDI-Timecode: folgt MTC (z. B. aus Ableton), mit Versatz und Freilauf bei Aussetzern.\nBeat: folgt dem Tempo (Ableton Link oder MIDI Clock); Positionen in Takten über das BPM der Time Line.\nLTC: SMPTE-Timecode als Audiosignal an einem Audioeingang.\nZum Testen ohne Signal: auf Intern umschalten." ) );
	}
	if( tl.sync != TlSync::Internal )
	{
		ImGui::SameLine();
		if( tl.sync == TlSync::Mtc || tl.sync == TlSync::Ltc )
		{
			const bool ltc    = tl.sync == TlSync::Ltc;
			const double xs   = ltc ? show.ltcSec : show.tcSec;
			const bool xr     = ltc ? show.ltcRun : show.tcRun;
			const char* name  = ltc ? "LTC" : "MTC";
			//status: locked (green), freewheel with the time left (orange), stopped (grey), no signal (red)
			if( rt.extRun && !xr ) ImGui::TextColored( ImVec4( 1, 0.65f, 0.25f, 1 ), L( "%s Freilauf %.1f s" ), name, std::max( 0.0, tl.freewheel - rt.extIdle ) );
			else if( xs < 0 ) ImGui::TextColored( ImVec4( 1, 0.35f, 0.3f, 1 ), L( "kein %s" ), name );
			else ImGui::TextColored( xr ? ImVec4( 0.5f, 1, 0.6f, 1 ) : ImVec4( 0.8f, 0.8f, 0.85f, 1 ), "%s %s", name, xr ? L( "läuft" ) : L( "steht" ) );
			if( ltc )
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth( 150 * ui );
				if( ImGui::BeginCombo( "##ltcdev", tl.ltcDevice.empty() ? L( "Standard-Eingang" ) : tl.ltcDevice.c_str() ) )
				{
					if( ImGui::Selectable( L( "Standard-Eingang" ), tl.ltcDevice.empty() ) ) { tl.ltcDevice.clear(); show.dirty = true; }
					for( auto& d : LtcInput::Devices() )
						if( ImGui::Selectable( d.c_str(), d == tl.ltcDevice ) ) { tl.ltcDevice = d; show.dirty = true; }
					ImGui::Separator();
					if( ImGui::RadioButton( L( "Linker Kanal" ), tl.ltcChannel == 0 ) ) { tl.ltcChannel = 0; show.dirty = true; }
					if( ImGui::RadioButton( L( "Rechter Kanal" ), tl.ltcChannel == 1 ) ) { tl.ltcChannel = 1; show.dirty = true; }
					ImGui::EndCombo();
				}
				if( ImGui::IsItemHovered() )
					ImGui::SetTooltip( L( "Audioeingang für LTC\nPegel %.0f %%%s%s" ), ltcIn_.Level() * 100, ltcIn_.error.empty() ? "" : "\n", ltcIn_.error.c_str() );
				//level meter
				ImGui::SameLine();
				const ImVec2 lp = ImGui::GetCursorScreenPos();
				const float lw = 6 * ui, lhh = ImGui::GetFrameHeight();
				ImGui::GetWindowDrawList()->AddRectFilled( lp, ImVec2( lp.x + lw, lp.y + lhh ), IM_COL32( 10, 12, 16, 255 ) );
				const float lv = Saturate( ltcIn_.Level() );
				ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( lp.x, lp.y + lhh * ( 1 - lv ) ), ImVec2( lp.x + lw, lp.y + lhh ), lv > 0.9f ? IM_COL32( 255, 80, 60, 255 ) : IM_COL32( 90, 220, 120, 255 ) );
				ImGui::Dummy( ImVec2( lw, lhh ) );
			}
		}
		else
		{
			const char* src = show.tempo.source == TempoSource::Link ? "Link" : show.tempo.source == TempoSource::MidiClock ? "MIDI Clock" : L( "Intern" );
			ImGui::TextColored( show.tempo.playing ? ImVec4( 0.5f, 1, 0.6f, 1 ) : ImVec4( 0.8f, 0.8f, 0.85f, 1 ), "%s %.1f BPM", src, show.tempo.liveBpm > 0 && show.tempo.source != TempoSource::Internal ? show.tempo.liveBpm : show.tempo.bpm );
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth( 86 * ui );
		float off = (float)tl.offset;
		if( ImGui::DragFloat( "##tloff", &off, 0.04f, -36000, 36000, L( "Versatz %.2f s" ) ) ) { tl.offset = off; show.dirty = true; }
		if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Externe Zeit + Versatz = Time-Line-Zeit" ) );
		if( tl.sync == TlSync::Mtc || tl.sync == TlSync::Ltc )
		{
			ImGui::SameLine();
			ImGui::SetNextItemWidth( 86 * ui );
			if( ImGui::DragFloat( "##tlfw", &tl.freewheel, 0.05f, 0, 30, L( "Freilauf %.1f s" ) ) ) show.dirty = true;
			if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "So lange läuft die Time Line weiter, wenn der Timecode aussetzt" ) );
		}
	}
	GroupBreak( 440 * ui, ui );
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted( L( "Raster" ) );
	ImGui::SameLine();
	ImGui::SetNextItemWidth( 84 * ui );
	if( ImGui::Combo( "##tlgrid", &tl.grid, L( "Aus\0" "1 Takt\0" "1 Beat\0" "1/2\0" "1/4\0" "1/8\0" "1 s\0" "0,1 s\0" "Frames\0" ) ) ) show.dirty = true;
	ImGui::SameLine();
	if( ImGui::Checkbox( L( "Einrasten" ), &tl.snap ) ) show.dirty = true;
	if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Keys rasten im Raster ein (Strg beim Ziehen = frei)" ) );
	ImGui::SameLine();
	ImGui::SetNextItemWidth( 74 * ui );
	if( ImGui::DragFloat( "##tlbpm", &tl.bpm, 0.1f, 20, 400, "%.1f BPM" ) ) show.dirty = true;
	if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Tempo der Time Line (Takt-Raster, Beat-Sync)" ) );
	if( ImGui::BeginPopupContextItem( "##tlbpmctx" ) )
	{
		if( ImGui::MenuItem( L( "Vom Tempo übernehmen" ) ) ) { tl.bpm = show.tempo.source != TempoSource::Internal && show.tempo.liveBpm > 0 ? show.tempo.liveBpm : show.tempo.bpm; show.dirty = true; }
		ImGui::SetNextItemWidth( 80 * ui );
		if( ImGui::InputInt( L( "Beats pro Takt" ), &tl.beatsPerBar ) ) { tl.beatsPerBar = Clamp( tl.beatsPerBar, 1, 16 ); show.dirty = true; }
		ImGui::SetNextItemWidth( 80 * ui );
		if( ImGui::InputInt( L( "Frames pro Sekunde" ), &tl.fps ) ) { tl.fps = Clamp( tl.fps, 1, 120 ); show.dirty = true; }
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth( 120 * ui );
	{
		//shown as m:ss; drag = change, double click = type (m:ss, h:mm:ss, seconds or a full timecode)
		float len = (float)tl.length;
		std::string fmt = std::string( L( "Länge" ) ) + " " + TimeStr( tl.length );
		if( ImGui::DragFloat( "##tllen", &len, std::max( 0.5f, (float)tl.length * 0.004f ), 1, 360000, fmt.c_str(), ImGuiSliderFlags_NoInput ) ) { tl.length = std::max( 1.0, (double)floorf( len + 0.5f ) ); show.dirty = true; }
		if( ImGui::IsItemHovered() )
		{
			ImGui::SetTooltip( "%s", L( "Länge der Time Line. Ziehen = ändern, Doppelklick = eintippen (z. B. 3:30 oder 1:02:00)" ) );
			if( ImGui::IsMouseDoubleClicked( 0 ) )
			{
				snprintf( g.lenBuf, sizeof( g.lenBuf ), "%s", TimeStr( tl.length ).c_str() );
				ImGui::OpenPopup( "##tllenedit" );
			}
		}
		if( ImGui::BeginPopup( "##tllenedit" ) )
		{
			if( ImGui::IsWindowAppearing() ) ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth( 120 * ui );
			const bool enter = ImGui::InputText( L( "Länge (m:ss)" ), g.lenBuf, sizeof( g.lenBuf ), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll );
			double v;
			const bool ok = ParseTime( g.lenBuf, tl.fps, v ) && v >= 1;
			if( !ok ) ImGui::TextColored( ImVec4( 1, 0.4f, 0.3f, 1 ), "%s", L( "z. B. 3:30, 1:02:00 oder 210" ) );
			if( enter && ok )
			{
				tl.length = v;
				show.dirty = true;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}
	GroupBreak( 330 * ui, ui );
	ImGui::SetNextItemWidth( 110 * ui );
	{
		float m = tl.master * 100;
		if( ls::rs::SliderFloat( "##tlmaster", &m, 0, 100, "Master %.0f %%" ) ) { tl.master = m / 100; show.dirty = true; }
		OscContext( "/ls/tl/master" );
	}
	ImGui::SameLine();
	if( ImGui::Checkbox( L( "Ausgabe" ), &tl.output ) ) show.dirty = true;
	if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Die Time Line schreibt in die Lampen (über den Snapshots, unter den Effekten).\nIm Programmer bewegte Kanäle haben Vorrang bis zum nächsten Play / Positionswechsel." ) );
	GroupBreak( 360 * ui, ui );
	if( ImGui::Button( L( "+ Spur##tladd" ) ) ) ImGui::OpenPopup( "##tladd" );
	if( ImGui::BeginPopup( "##tladd" ) )
	{
		char b[ 96 ];
		snprintf( b, sizeof( b ), L( "Gruppenspur aus Auswahl (%d Lampe(n))..." ), (int)sel.size() );
		if( ImGui::MenuItem( b, nullptr, false, !sel.empty() ) ) AddTimelineTrack( sel, Attr::None, true );
		if( sel.empty() ) ImGui::TextDisabled( "%s", L( "  (erst Lampen in der Patch-Liste markieren)" ) );
		if( ImGui::MenuItem( L( "Leere DMX-Spur" ) ) )
		{
			TlTrack t;
			t.uid   = tl.NewUid();
			t.kind  = TlTrackKind::Dmx;
			t.name  = L( "DMX-Kanäle" );
			t.color = V3( 0.75f, 0.75f, 0.8f );
			tl.tracks.push_back( t );
			g.selTrack  = t.uid;
			tlLanePick_ = t.uid;
			show.dirty  = true;
		}
		if( ImGui::MenuItem( L( "Cue-Spur (Cue-Liste)" ), nullptr, false, !tl.CueTrack() ) )
		{
			TlTrack t;
			t.uid   = tl.NewUid();
			t.kind  = TlTrackKind::Cues;
			t.name  = L( "Cues" );
			t.color = V3( 1.0f, 0.7f, 0.3f );
			tl.tracks.insert( tl.tracks.begin(), t );
			show.dirty = true;
		}
		if( ImGui::MenuItem( L( "Clip-Spur (Snapshots / Effekte als Blöcke)" ) ) )
		{
			TlTrack t;
			t.uid   = tl.NewUid();
			t.kind  = TlTrackKind::Clips;
			t.name  = L( "Clips" );
			t.color = V3( 0.75f, 0.5f, 1.0f );
			tl.tracks.push_back( t );
			g.selTrack = t.uid;
			show.dirty = true;
		}
		bool hasAudio = false;
		for( auto& t : tl.tracks ) hasAudio = hasAudio || t.kind == TlTrackKind::Audio;
		if( ImGui::MenuItem( L( "Audiospur (Datei laden)..." ), nullptr, false, !hasAudio ) )
		{
			const std::string f = OpenAudioDialog();
			if( !f.empty() )
			{
				TlTrack t;
				t.uid   = tl.NewUid();
				t.kind  = TlTrackKind::Audio;
				t.name  = FileStem( f );
				t.file  = f;
				t.color = V3( 0.55f, 0.6f, 0.7f );
				tl.tracks.insert( tl.tracks.begin(), t );
				show.dirty = true;
			}
		}
		if( ImGui::MenuItem( L( "Lightshow-Spur (Datei laden)..." ) ) )
		{
			const std::string f = OpenLightshowDialog();
			if( !f.empty() )
			{
				auto pl = LbxPlayerLoad( f, true );
				if( !pl->err.empty() ) SetStatus( L( "Lightshow kann nicht gelesen werden: " ) + pl->err );
				else
				{
					TlTrack t;
					t.uid    = tl.NewUid();
					t.kind   = TlTrackKind::Show;
					t.name   = pl->name.empty() ? FileStem( f ) : pl->name;
					t.file   = f;
					t.offset = tl.Snap( rt.pos );
					t.color  = V3( 0.95f, 0.6f, 0.25f );
					tl.tracks.push_back( t );
					g.selTrack = t.uid;
					tl.length  = std::max( tl.length, t.offset + pl->duration );
					show.dirty = true;
				}
			}
		}
		ImGui::Separator();
		if( ImGui::MenuItem( L( "Marker an der Abspielposition" ), "M" ) )
		{
			tl.markers.push_back( { rt.pos, "Marker " + std::to_string( tl.markers.size() + 1 ) } );
			show.dirty = true;
		}
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	if( ImGui::Checkbox( L( "3D-Pfad" ), &tlPath_ ) ) {}
	if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Bewegungspfad (Pan/Tilt) der ausgewählten Spur im 3D-Fenster" ) );
	if( !tlWin_ )
	{
		ImGui::SameLine();
		if( ImGui::Button( L( "Schwebendes Fenster" ) ) ) tlWin_ = true;
	}
	ImGui::PopStyleVar();

	//================= layout
	const ImVec2 area0 = ImGui::GetCursorScreenPos();
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	if( avail.x < 200 * ui || avail.y < 60 * ui ) return;
	ImGui::BeginChild( "##tlarea", avail, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove );
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float rulerH = floorf( lh * 1.9f );
	const float headW  = Clamp( g.headW, 160 * ui, avail.x * 0.5f );
	const float X0 = area0.x + headW, X1 = area0.x + avail.x - 10 * ui;//curve area (10 px: vertical scroll bar)
	const float Y0 = area0.y + rulerH, Y1 = area0.y + avail.y;
	if( g.pps <= 0 ) g.pps = (float)std::max( 4.0, ( X1 - X0 ) / std::max( 10.0, tl.length ) );
	auto X  = [ & ]( double t ) { return (float)( X0 + ( t - g.view0 ) * g.pps ); };
	auto T  = [ & ]( float x ) { return g.view0 + ( x - X0 ) / g.pps; };
	//rows
	std::vector< Row > rows;
	float ty = 0;
	const float trackH = floorf( lh + 12 * ui ), cueH = floorf( lh * 2 + 12 * ui );
	for( int ti = 0; ti < (int)tl.tracks.size(); ++ti )
	{
		TlTrack& t = tl.tracks[ ti ];
		if( t.kind == TlTrackKind::Cues || t.kind == TlTrackKind::Clips )
		{
			const float h = t.kind == TlTrackKind::Cues ? cueH : floorf( lh * 2 + 20 * ui );//clips: two text lines below the fade handles
			rows.push_back( { t.kind == TlTrackKind::Cues ? 2 : 3, ti, -1, ty, h } );
			ty += h;
			continue;
		}
		if( t.kind == TlTrackKind::Audio || t.kind == TlTrackKind::Show )
		{
			const float ah = floorf( lh * 3 + 10 * ui );
			rows.push_back( { 4, ti, -1, ty, ah } );
			ty += ah;
			continue;
		}
		rows.push_back( { 0, ti, -1, ty, trackH } );
		ty += trackH;
		if( !t.open ) continue;
		for( int li = 0; li < (int)t.lanes.size(); ++li )
		{
			const float h = LaneH( t.lanes[ li ].height, ui );
			rows.push_back( { 1, ti, li, ty, h } );
			ty += h;
		}
	}
	const float contentH = ty;
	g.scrollY            = Clamp( g.scrollY, 0.0f, std::max( 0.0f, contentH - ( Y1 - Y0 ) + 20 * ui ) );
	auto RY = [ & ]( const Row& r ) { return Y0 + r.y - g.scrollY; };
	auto KeyY = [ & ]( const Row& r, float v ) {
		const float pad = 5 * ui;
		return RY( r ) + pad + ( 1 - v ) * ( r.h - 2 * pad - 1 );
	};
	auto ValAtY = [ & ]( const Row& r, float y ) {
		const float pad = 5 * ui;
		return Saturate( 1 - ( y - RY( r ) - pad ) / ( r.h - 2 * pad - 1 ) );
	};

	//================= input: curve area (one invisible button over it)
	ImGui::SetCursorScreenPos( ImVec2( X0, Y0 ) );
	ImGui::InvisibleButton( "##tlcurves", ImVec2( std::max( 1.0f, X1 - X0 ), std::max( 1.0f, Y1 - Y0 ) ), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle );
	const bool curvesHov = ImGui::IsItemHovered();
	const bool curvesAct = ImGui::IsItemActive();
	ImGui::SetCursorScreenPos( ImVec2( X0, area0.y ) );
	ImGui::InvisibleButton( "##tlruler", ImVec2( std::max( 1.0f, X1 - X0 ), rulerH ), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight );
	const bool rulerHov = ImGui::IsItemHovered();
	const bool rulerAct = ImGui::IsItemActive();
	const bool panelHov = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem );
	tlHoveredNow_       = tlHoveredNow_ || panelHov;
	const ImVec2 M      = io.MousePos;
	//wheel over the time area: zoom at the mouse; Shift = scroll in time, Ctrl = up / down.
	//Over the track heads the wheel scrolls up / down.
	if( panelHov && io.MouseWheel != 0 && M.x >= area0.x && !ImGui::IsPopupOpen( "", ImGuiPopupFlags_AnyPopupId ) )
	{
		if( M.x < X0 || io.KeyCtrl ) g.scrollY -= io.MouseWheel * 60 * ui;
		else if( io.KeyShift )
		{
			g.view0 -= io.MouseWheel * 80 * ui / g.pps;
			g.userScrollT = Now();
		}
		else
		{
			const double tm = T( M.x );
			g.pps           = Clamp( g.pps * powf( 1.2f, io.MouseWheel ), 0.5f, 4000.0f );
			g.view0         = tm - ( M.x - X0 ) / g.pps;
			g.userScrollT   = Now();
		}
	}
	if( panelHov && io.MouseWheelH != 0 && M.x >= X0 )
	{
		g.view0 -= io.MouseWheelH * 80 * ui / g.pps;
		g.userScrollT = Now();
	}
	if( curvesAct && ImGui::IsMouseDragging( ImGuiMouseButton_Middle, 0 ) )
	{
		g.view0 -= io.MouseDelta.x / g.pps;
		g.scrollY -= io.MouseDelta.y;
		g.userScrollT = Now();
	}
	//dragging past the left / right edge (keys, clips, playhead, loop, selection box) scrolls the time along
	if( g.drag != 0 && ImGui::IsMouseDown( 0 ) )
	{
		const float edge = 24 * ui;
		float over       = 0;
		if( M.x > X1 - edge ) over = M.x - ( X1 - edge );
		else if( M.x < X0 + edge && g.view0 > 0 ) over = M.x - ( X0 + edge );
		if( over != 0 )
		{
			g.view0 += Clamp( over, -200 * ui, 200 * ui ) * 10.0f * io.DeltaTime / g.pps;
			g.userScrollT = Now();
		}
	}
	//while playing the playhead runs to the middle, then the time scrolls under it
	if( rt.playing && g.drag == 0 && Now() - g.userScrollT > 1.5 )
	{
		const float half = ( X1 - X0 ) * 0.5f;
		const float px   = X( rt.pos );
		if( px > X0 + half ) g.view0 = rt.pos - half / g.pps;
		else if( px < X0 ) g.view0 = rt.pos - half / g.pps;
	}
	g.view0 = std::max( -20.0 * ui / g.pps, g.view0 );

	//================= background, grid
	dl->AddRectFilled( area0, ImVec2( area0.x + avail.x, area0.y + avail.y ), kBg );
	dl->PushClipRect( ImVec2( area0.x, Y0 ), ImVec2( area0.x + avail.x, Y1 ), true );
	for( size_t i = 0; i < rows.size(); ++i )
	{
		const Row& r  = rows[ i ];
		const float y = RY( r );
		if( y > Y1 || y + r.h < Y0 ) continue;
		dl->AddRectFilled( ImVec2( X0, y ), ImVec2( X1, y + r.h ), r.kind == 0 ? kHead : ( i % 2 ? kRowA : kRowB ) );
		dl->AddLine( ImVec2( area0.x, y + r.h - 1 ), ImVec2( X1, y + r.h - 1 ), IM_COL32( 10, 11, 15, 255 ) );
	}
	dl->PopClipRect();
	//grid: lines at the editor grid if dense enough, else coarser
	const bool beatsMode = tl.grid >= 1 && tl.grid <= 5;
	const double spb     = 60.0 / std::max( 1.0f, tl.bpm );
	double major, minor;
	{
		static const double secSteps[] = { 0.04, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600 };
		major = 3600;
		for( double s : secSteps )
			if( s * g.pps >= 80 * ui ) { major = s; break; }
		if( beatsMode )
		{
			const double bar = spb * std::max( 1, tl.beatsPerBar );
			major            = bar;
			while( major * g.pps < 80 * ui ) major *= 2;
		}
		minor = tl.GridStep();
		if( minor <= 0 ) minor = major / 4;
		while( minor * g.pps < 8 * ui ) minor *= 2;
		if( minor > major ) minor = major;
	}
	dl->PushClipRect( ImVec2( X0, area0.y ), ImVec2( X1, Y1 ), true );
	{
		const double t0 = std::max( 0.0, T( X0 ) ), t1 = T( X1 );
		for( double t = floor( t0 / minor ) * minor; t <= t1; t += minor )
		{
			const bool mj = fabs( t / major - floor( t / major + 0.5 ) ) < 1e-6;
			dl->AddLine( ImVec2( X( t ), Y0 ), ImVec2( X( t ), Y1 ), mj ? kGridHi : kGrid );
		}
		//end of the timeline
		if( X( tl.length ) < X1 ) dl->AddRectFilled( ImVec2( X( tl.length ), Y0 ), ImVec2( X1, Y1 ), IM_COL32( 0, 0, 0, 90 ) );
		//loop range
		if( tl.loop ) dl->AddRectFilled( ImVec2( X( tl.loopA ), Y0 ), ImVec2( X( tl.loopB ), Y1 ), IM_COL32( 90, 165, 255, 16 ) );
		for( auto& m : tl.markers ) dl->AddLine( ImVec2( X( m.t ), Y0 ), ImVec2( X( m.t ), Y1 ), IM_COL32( 255, 200, 90, 70 ) );
	}
	dl->PopClipRect();

	//================= ruler
	dl->AddRectFilled( ImVec2( area0.x, area0.y ), ImVec2( area0.x + avail.x, Y0 ), IM_COL32( 26, 29, 37, 255 ) );
	dl->PushClipRect( ImVec2( X0, area0.y ), ImVec2( X1, Y0 ), true );
	{
		const double t0 = std::max( 0.0, T( X0 ) ), t1 = T( X1 );
		ImGui::PushFont( fontMono );
		for( double t = floor( t0 / major ) * major; t <= t1; t += major )
		{
			if( t < -1e-9 ) continue;
			const float x = X( t );
			dl->AddLine( ImVec2( x, Y0 - rulerH * 0.45f ), ImVec2( x, Y0 ), kDim );
			char b[ 32 ];
			if( beatsMode ) snprintf( b, sizeof( b ), "%d", (int)floor( t / ( spb * std::max( 1, tl.beatsPerBar ) ) + 0.5 ) + 1 );
			else if( major < 1 ) snprintf( b, sizeof( b ), "%d:%05.2f", (int)( t / 60 ), fmod( t, 60.0 ) );
			else snprintf( b, sizeof( b ), "%d:%02d", (int)( t / 60 ), (int)fmod( t + 1e-6, 60.0 ) );
			dl->AddText( ImVec2( x + 3 * ui, area0.y + 2 * ui ), kDim, b );
		}
		for( double t = floor( t0 / minor ) * minor; t <= t1; t += minor ) dl->AddLine( ImVec2( X( t ), Y0 - rulerH * 0.2f ), ImVec2( X( t ), Y0 ), kGridHi );
		ImGui::PopFont();
		if( tl.loop || g.drag == 7 )
		{
			dl->AddRectFilled( ImVec2( X( tl.loopA ), Y0 - 5 * ui ), ImVec2( X( tl.loopB ), Y0 ), tl.loop ? kAcc : IM_COL32( 90, 165, 255, 110 ) );
		}
		for( int i = 0; i < (int)tl.markers.size(); ++i )
		{
			const float x = X( tl.markers[ i ].t );
			dl->AddTriangleFilled( ImVec2( x, Y0 - 1 ), ImVec2( x, Y0 - 11 * ui ), ImVec2( x + 8 * ui, Y0 - 6 * ui ), IM_COL32( 255, 200, 90, 255 ) );
			dl->AddText( ImVec2( x + 10 * ui, Y0 - 13 * ui - lh * 0.5f ), IM_COL32( 255, 210, 120, 255 ), tl.markers[ i ].name.c_str() );
		}
	}
	dl->PopClipRect();
	//ruler input: click / drag = locate, Shift + drag = loop range, markers can be dragged, right click = menu
	{
		int hovMarker = -1;
		for( int i = 0; i < (int)tl.markers.size(); ++i )
			if( fabsf( M.x - X( tl.markers[ i ].t ) - 4 * ui ) < 7 * ui && M.y > Y0 - 14 * ui && M.y < Y0 ) hovMarker = i;
		if( rulerHov && ImGui::IsMouseClicked( 0 ) )
		{
			if( hovMarker >= 0 && !io.KeyShift )
			{
				g.drag   = 5;
				g.marker = hovMarker;
				if( ImGui::IsMouseDoubleClicked( 0 ) )
				{
					g.popMarker = hovMarker;
					snprintf( g.name, sizeof( g.name ), "%s", tl.markers[ hovMarker ].name.c_str() );
					g.openMarker = true;
					g.drag       = 0;
				}
			}
			else if( io.KeyShift )
			{
				g.drag   = 7;
				g.dragT0 = tl.Snap( std::max( 0.0, T( M.x ) ) );
			}
			else g.drag = 3;
		}
		if( rulerHov && ImGui::IsMouseClicked( 1 ) )
		{
			g.popT      = tl.Snap( std::max( 0.0, T( M.x ) ) );
			g.popMarker = hovMarker;
			ImGui::OpenPopup( "##tlrulerctx" );
		}
		if( g.drag == 3 && rulerAct ) tl.Locate( show, io.KeyCtrl ? std::max( 0.0, T( M.x ) ) : tl.Snap( std::max( 0.0, T( M.x ) ) ) );
		if( g.drag == 5 && rulerAct && g.marker < (int)tl.markers.size() )
		{
			tl.markers[ g.marker ].t = io.KeyCtrl ? std::max( 0.0, T( M.x ) ) : tl.Snap( std::max( 0.0, T( M.x ) ) );
			show.dirty               = true;
		}
		if( g.drag == 7 && rulerAct )
		{
			const double t = tl.Snap( std::max( 0.0, T( M.x ) ) );
			tl.loopA       = std::min( g.dragT0, t );
			tl.loopB       = std::max( g.dragT0, t );
			if( tl.loopB - tl.loopA > 0.01 ) tl.loop = true;
			show.dirty = true;
		}
		if( ImGui::BeginPopup( "##tlrulerctx" ) )
		{
			if( ImGui::MenuItem( L( "Abspielposition hierher" ) ) ) tl.Locate( show, g.popT );
			if( ImGui::MenuItem( L( "Marker hier setzen" ) ) ) { tl.markers.push_back( { g.popT, "Marker " + std::to_string( tl.markers.size() + 1 ) } ); show.dirty = true; }
			if( ImGui::MenuItem( L( "Loop-Anfang hier" ) ) ) { tl.loopA = g.popT; if( tl.loopB <= tl.loopA ) tl.loopB = tl.loopA + 4 * spb; show.dirty = true; }
			if( ImGui::MenuItem( L( "Loop-Ende hier" ) ) ) { tl.loopB = std::max( g.popT, tl.loopA + 0.05 ); show.dirty = true; }
			if( ImGui::MenuItem( L( "Länge der Time Line bis hier" ) ) ) { tl.length = std::max( 1.0, g.popT ); show.dirty = true; }
			if( g.popMarker >= 0 && g.popMarker < (int)tl.markers.size() )
			{
				ImGui::Separator();
				if( ImGui::MenuItem( L( "Marker umbenennen..." ) ) )
				{
					snprintf( g.name, sizeof( g.name ), "%s", tl.markers[ g.popMarker ].name.c_str() );
					g.openMarker = true;
				}
				if( ImGui::MenuItem( L( "Marker löschen" ) ) ) { tl.markers.erase( tl.markers.begin() + g.popMarker ); show.dirty = true; }
			}
			if( !tl.markers.empty() && ImGui::BeginMenu( L( "Springe zu Marker" ) ) )
			{
				for( auto& m : tl.markers )
					if( ImGui::MenuItem( ( m.name + "  " + TcStr( m.t, tl.fps ) ).c_str() ) ) tl.Locate( show, m.t );
				ImGui::EndMenu();
			}
			ImGui::EndPopup();
		}
	}

	//================= lanes: curves, keys, cues
	int hovRow = -1;
	for( int i = 0; i < (int)rows.size(); ++i )
		if( M.y >= RY( rows[ i ] ) && M.y < RY( rows[ i ] ) + rows[ i ].h && M.y >= Y0 && M.y < Y1 ) hovRow = i;
	//hit test: key under the mouse (lane rows) -> (row, key)
	int hitRow = -1, hitKey = -1;
	int hitHandleKey = -1;
	bool hitHandleOut = true;
	if( curvesHov && hovRow >= 0 && rows[ hovRow ].kind == 1 )
	{
		const Row& r   = rows[ hovRow ];
		TlLane& l      = tl.tracks[ r.track ].lanes[ r.lane ];
		const float rr = 6 * ui;
		float best     = rr * rr;
		for( int k = 0; k < (int)l.keys.size(); ++k )
		{
			const TlKey& key = l.keys[ k ];
			const float ky   = l.kind == TlLaneKind::Color ? RY( r ) + r.h * 0.5f : KeyY( r, key.v );
			const float dx = M.x - X( key.t ), dy = M.y - ky;
			if( dx * dx + dy * dy <= best ) { best = dx * dx + dy * dy; hitKey = k; hitRow = hovRow; }
			//Bezier handles of selected keys
			if( l.kind != TlLaneKind::Color && key.sel )
			{
				const double segN = k + 1 < (int)l.keys.size() ? l.keys[ k + 1 ].t - key.t : 0;
				const double segP = k > 0 ? key.t - l.keys[ k - 1 ].t : 0;
				if( key.interp == TlInterp::Bezier && segN > 0 )
				{
					const float hx = X( key.t + key.ox * segN ), hy = KeyY( r, key.v + key.oy );
					if( ( M.x - hx ) * ( M.x - hx ) + ( M.y - hy ) * ( M.y - hy ) < rr * rr ) { hitHandleKey = k; hitHandleOut = true; }
				}
				if( k > 0 && l.keys[ k - 1 ].interp == TlInterp::Bezier && segP > 0 )
				{
					const float hx = X( key.t - key.ix * segP ), hy = KeyY( r, key.v + key.iy );
					if( ( M.x - hx ) * ( M.x - hx ) + ( M.y - hy ) * ( M.y - hy ) < rr * rr ) { hitHandleKey = k; hitHandleOut = false; }
				}
			}
		}
	}
	//summary diamonds on a track header: keys of all lanes at (nearly) the same time
	auto summaryHit = [ & ]( const Row& r, double& tHit ) {
		const TlTrack& t = tl.tracks[ r.track ];
		float best       = 6 * ui;
		bool any         = false;
		for( auto& l : t.lanes )
			for( auto& k : l.keys )
			{
				const float d = fabsf( M.x - X( k.t ) );
				if( d < best ) { best = d; tHit = k.t; any = true; }
			}
		return any;
	};
	//cue blocks: index of the cue under the mouse
	auto cueHit = [ & ]( const Row& r ) {
		int best = -1;
		for( int i = 0; i < (int)show.cues.size(); ++i )
		{
			const Cue& c = show.cues[ i ];
			if( c.trigger != 2 ) continue;
			if( M.x >= X( c.tc ) - 3 * ui && M.x <= X( c.tc ) + 80 * ui && ( best < 0 || c.tc > show.cues[ best ].tc ) && M.x >= X( c.tc ) - 3 * ui ) best = i;
		}
		if( best >= 0 )
		{
			//only the part up to the next cue belongs to it
			for( auto& c : show.cues )
				if( c.trigger == 2 && c.tc > show.cues[ best ].tc && M.x >= X( c.tc ) - 3 * ui ) return -1;
		}
		(void)r;
		return best;
	};
	//clip under the mouse: index + part (0 body, 1 left edge, 2 right edge, 3 fade-in handle, 4 fade-out handle)
	auto clipHit = [ & ]( const Row& r, int& part ) {
		const TlTrack& t = tl.tracks[ r.track ];
		const float ya = RY( r ) + 4 * ui, yb = RY( r ) + r.h - 5 * ui;
		part           = 0;
		if( M.y < ya - 3 * ui || M.y > yb + 3 * ui ) return -1;
		for( int i = (int)t.clips.size() - 1; i >= 0; --i )
		{
			const TlClip& c = t.clips[ i ];
			const float xa = X( c.t ), xb = X( c.t + c.len );
			if( M.x < xa - 4 * ui || M.x > xb + 4 * ui ) continue;
			const float fi = X( c.t + c.fadeIn ), fo = X( c.t + c.len - c.fadeOut );
			if( fabsf( M.y - ya ) < 7 * ui && fabsf( M.x - fi ) < 6 * ui ) part = 3;
			else if( fabsf( M.y - ya ) < 7 * ui && fabsf( M.x - fo ) < 6 * ui ) part = 4;
			else if( M.x < xa + 5 * ui ) part = 1;
			else if( M.x > xb - 5 * ui ) part = 2;
			return i;
		}
		return -1;
	};
	auto clearClipSel = [ & ] {
		for( auto& t : tl.tracks )
			for( auto& c : t.clips ) c.sel = false;
	};

	//---- mouse down in the curve area
	if( curvesHov && ImGui::IsMouseClicked( 0 ) && hovRow >= 0 )
	{
		const Row& r  = rows[ hovRow ];
		TlTrack& trk  = tl.tracks[ r.track ];
		g.dragStart   = M;
		g.axis        = 0;
		g.selTrack    = trk.uid;
		const bool dbl = ImGui::IsMouseDoubleClicked( 0 );
		if( r.kind == 3 )
		{
			int part;
			const int ci = clipHit( r, part );
			ClearSel( tl );
			if( ci < 0 )
			{
				clearClipSel();
				if( dbl )
				{
					g.popT        = tl.Snap( std::max( 0.0, T( M.x ) ) );
					g.popTrack    = trk.uid;
					g.openClipAdd = true;
				}
			}
			else
			{
				if( !io.KeyCtrl && !trk.clips[ ci ].sel ) clearClipSel();
				trk.clips[ ci ].sel = true;
				if( dbl )
				{
					g.popTrack = trk.uid;
					g.clipIdx  = ci;
					ImGui::OpenPopup( "##tlclipctx" );
				}
				else
				{
					g.drag      = 9 + part;
					g.clipTrack = trk.uid;
					g.clipIdx   = ci;
					g.clipOrig  = trk.clips[ ci ];
					g.dragT0    = T( M.x );
				}
			}
		}
		else if( r.kind == 4 )
		{
			if( dbl && trk.kind == TlTrackKind::Show )
			{
				const std::string f = OpenLightshowDialog();
				if( !f.empty() ) { trk.file = f; LbxPlayerLoad( f, true ); show.dirty = true; }
			}
			else if( dbl )
			{
				const std::string f = OpenAudioDialog();
				if( !f.empty() ) { trk.file = f; trk.name = FileStem( f ); show.dirty = true; }
			}
			else if( io.KeyAlt )
			{
				g.drag      = 14;
				g.clipTrack = trk.uid;
				g.dragT0    = T( M.x );
				g.cueTc     = trk.offset;
			}
			else
				g.drag = 3;//scrub on the waveform
		}
		else if( r.kind == 2 )
		{
			const int c = cueHit( r );
			if( dbl && c < 0 )
			{
				g.popT       = tl.Snap( std::max( 0.0, T( M.x ) ) );
				g.openCueAdd = true;
			}
			else if( dbl && c >= 0 )
			{
				g.popCue  = c;
				snprintf( g.name, sizeof( g.name ), "%s", show.cues[ c ].name.c_str() );
				g.openCue = true;
			}
			else if( c >= 0 )
			{
				g.drag   = 6;
				g.cue    = c;
				g.cueTc  = show.cues[ c ].tc;
				g.dragT0 = T( M.x );
			}
		}
		else if( r.kind == 0 )
		{
			double th;
			if( summaryHit( r, th ) )
			{
				//select the keys of all lanes at this time, drag them together
				if( !io.KeyCtrl && !io.KeyShift ) ClearSel( tl );
				for( auto& l : trk.lanes )
					for( auto& k : l.keys )
						if( fabs( k.t - th ) * g.pps < 2 ) k.sel = true;
				g.drag = 1;
			}
			else if( !io.KeyCtrl && !io.KeyShift ) { ClearSel( tl ); g.drag = 2; g.boxAdd = false; }
		}
		else
		{
			TlLane& l = trk.lanes[ r.lane ];
			if( hitHandleKey >= 0 && !dbl )
			{
				g.drag  = 4;
				g.hLane = { trk.uid, r.lane };
				g.hKey  = hitHandleKey;
				g.hOut  = hitHandleOut;
			}
			else if( hitKey >= 0 )
			{
				TlKey& k = l.keys[ hitKey ];
				if( dbl )
				{
					g.popLane     = { trk.uid, r.lane };
					g.popKey      = hitKey;
					g.openKeyEdit = true;
				}
				else
				{
					if( io.KeyCtrl ) k.sel = !k.sel;
					else if( io.KeyShift ) k.sel = true;
					else if( !k.sel ) { ClearSel( tl ); k.sel = true; }
					if( k.sel ) g.drag = 1;
				}
			}
			else if( dbl )
			{
				//new key at the mouse (colour lanes: the colour there)
				const double t = io.KeyCtrl ? std::max( 0.0, T( M.x ) ) : tl.Snap( std::max( 0.0, T( M.x ) ) );
				ClearSel( tl );
				if( l.kind == TlLaneKind::Color )
				{
					bool has;
					V3 c = TlEvalColor( l, t, &has );
					if( !has ) TlLiveValue( show, trk, l, &c );
					const int i     = TlSetKey( l, t, 0, &c, TlInterp::Linear );
					l.keys[ i ].sel = true;
				}
				else
				{
					const int i     = TlSetKey( l, t, ValAtY( r, M.y ), nullptr, l.keys.empty() ? TlInterp::Smooth : l.keys[ std::max( 0, std::min( (int)l.keys.size() - 1, 0 ) ) ].interp );
					l.keys[ i ].sel = true;
				}
				show.dirty = true;
			}
			else
			{
				if( !io.KeyCtrl && !io.KeyShift ) ClearSel( tl );
				g.drag   = 2;
				g.boxAdd = io.KeyCtrl || io.KeyShift;
			}
		}
		if( g.drag == 1 )
		{
			//remember the selected keys (time order per lane) + the grabbed reference
			g.orig.clear();
			for( auto& t : tl.tracks )
				for( int li = 0; li < (int)t.lanes.size(); ++li )
					for( auto& k : t.lanes[ li ].keys )
						if( k.sel ) g.orig[ { t.uid, li } ].push_back( { k.t, k.v } );
			g.dragT0 = T( M.x );
			g.dragV0 = r.kind == 1 ? ValAtY( r, M.y ) : 0;
			if( hitKey >= 0 && r.kind == 1 ) g.dragT0 = trk.lanes[ r.lane ].keys[ hitKey ].t;
			g.dragV0 = hitKey >= 0 && r.kind == 1 ? trk.lanes[ r.lane ].keys[ hitKey ].v : g.dragV0;
			g.hLane  = { trk.uid, r.kind == 1 ? r.lane : -1 };
		}
	}
	//---- dragging
	if( curvesAct && g.drag == 1 && ImGui::IsMouseDragging( 0, 2 ) )
	{
		const ImVec2 d = ImVec2( M.x - g.dragStart.x, M.y - g.dragStart.y );
		if( io.KeyShift && g.axis == 0 && ( fabsf( d.x ) > 6 * ui || fabsf( d.y ) > 6 * ui ) ) g.axis = fabsf( d.x ) >= fabsf( d.y ) ? 1 : 2;
		if( !io.KeyShift ) g.axis = 0;
		double dt = d.x / g.pps;
		//snap the grabbed key's new time
		if( !io.KeyCtrl ) dt = tl.Snap( g.dragT0 + dt ) - g.dragT0;
		//value change in the grabbed lane's scale
		float dv = 0;
		for( auto& r : rows )
			if( r.kind == 1 && tl.tracks[ r.track ].uid == g.hLane.track && r.lane == g.hLane.lane ) dv = -d.y / std::max( 10.0f, r.h - 10 * ui );
		if( g.hLane.lane < 0 ) dv = 0;//summary drag: time only
		if( g.axis == 1 ) dv = 0;
		if( g.axis == 2 ) dt = 0;
		//keep times >= 0
		double minT = 1e18;
		for( auto& o : g.orig )
			for( auto& p : o.second ) minT = std::min( minT, p.first );
		if( minT + dt < 0 ) dt = -minT;
		for( auto& o : g.orig )
		{
			TlTrack* t = tl.Find( o.first.track );
			if( !t || o.first.lane >= (int)t->lanes.size() ) continue;
			TlLane& l = t->lanes[ o.first.lane ];
			TlSort( l );
			size_t n = 0;
			for( auto& k : l.keys )
				if( k.sel && n < o.second.size() )
				{
					k.t = o.second[ n ].first + dt;
					k.v = Saturate( o.second[ n ].second + dv );
					++n;
				}
			TlSort( l );
		}
		show.dirty = true;
	}
	if( curvesAct && g.drag == 4 )
	{
		TlTrack* t = tl.Find( g.hLane.track );
		if( t && g.hLane.lane >= 0 && g.hLane.lane < (int)t->lanes.size() && g.hKey < (int)t->lanes[ g.hLane.lane ].keys.size() )
		{
			TlLane& l = t->lanes[ g.hLane.lane ];
			TlKey& k  = l.keys[ g.hKey ];
			for( auto& r : rows )
				if( r.kind == 1 && tl.tracks[ r.track ].uid == g.hLane.track && r.lane == g.hLane.lane )
				{
					const float v = ValAtY( r, M.y ) - k.v;
					if( g.hOut && g.hKey + 1 < (int)l.keys.size() )
					{
						const double seg = l.keys[ g.hKey + 1 ].t - k.t;
						k.ox             = Clamp( (float)( ( T( M.x ) - k.t ) / seg ), 0.0f, 1.0f );
						k.oy             = Clamp( v, -1.0f, 1.0f );
					}
					else if( !g.hOut && g.hKey > 0 )
					{
						const double seg = k.t - l.keys[ g.hKey - 1 ].t;
						k.ix             = Clamp( (float)( ( k.t - T( M.x ) ) / seg ), 0.0f, 1.0f );
						k.iy             = Clamp( v, -1.0f, 1.0f );
					}
					show.dirty = true;
				}
		}
	}
	if( curvesAct && g.drag == 6 && g.cue >= 0 && g.cue < (int)show.cues.size() )
	{
		double t = g.cueTc + ( T( M.x ) - g.dragT0 );
		t        = io.KeyCtrl ? std::max( 0.0, t ) : std::max( 0.0, tl.Snap( t ) );
		show.cues[ g.cue ].tc = t;
		show.dirty            = true;
	}
	if( curvesAct && g.drag >= 9 && g.drag <= 13 )
		if( TlTrack* t = tl.Find( g.clipTrack ) )
			if( g.clipIdx >= 0 && g.clipIdx < (int)t->clips.size() )
			{
				TlClip& c        = t->clips[ g.clipIdx ];
				const TlClip& o  = g.clipOrig;
				const double d   = T( M.x ) - g.dragT0;
				auto sn          = [ & ]( double x ) { return io.KeyCtrl ? x : tl.Snap( x ); };
				switch( g.drag )
				{
				case 9: c.t = std::max( 0.0, sn( o.t + d ) ); break;
				case 10:
				{
					const double nt = Clamp( sn( o.t + d ), 0.0, o.t + o.len - 0.05 );
					c.len           = o.t + o.len - nt;
					c.t             = nt;
					break;
				}
				case 11: c.len = std::max( 0.05, sn( o.t + o.len + d ) - o.t ); break;
				case 12: c.fadeIn = (float)Clamp( o.fadeIn + d, 0.0, c.len - c.fadeOut ); break;
				case 13: c.fadeOut = (float)Clamp( o.fadeOut - d, 0.0, c.len - c.fadeIn ); break;
				}
				show.dirty = true;
			}
	if( curvesAct && g.drag == 14 )
		if( TlTrack* t = tl.Find( g.clipTrack ) )
		{
			t->offset  = io.KeyCtrl ? g.cueTc + ( T( M.x ) - g.dragT0 ) : tl.Snap( g.cueTc + ( T( M.x ) - g.dragT0 ) );
			show.dirty = true;
		}
	if( curvesAct && g.drag == 3 ) tl.Locate( show, io.KeyCtrl ? std::max( 0.0, T( M.x ) ) : tl.Snap( std::max( 0.0, T( M.x ) ) ) );
	//box select
	if( g.drag == 2 && curvesAct && ImGui::IsMouseDragging( 0, 3 ) )
	{
		const ImVec2 a( std::min( g.dragStart.x, M.x ), std::min( g.dragStart.y, M.y ) ), b( std::max( g.dragStart.x, M.x ), std::max( g.dragStart.y, M.y ) );
		dl->AddRectFilled( a, b, IM_COL32( 90, 165, 255, 30 ) );
		dl->AddRect( a, b, IM_COL32( 90, 165, 255, 160 ) );
		for( auto& r : rows )
		{
			if( r.kind != 1 ) continue;
			TlLane& l = tl.tracks[ r.track ].lanes[ r.lane ];
			for( auto& k : l.keys )
			{
				const float kx = X( k.t ), ky = l.kind == TlLaneKind::Color ? RY( r ) + r.h * 0.5f : KeyY( r, k.v );
				const bool in  = kx >= a.x && kx <= b.x && ky >= a.y && ky <= b.y;
				if( in ) k.sel = true;
				else if( !g.boxAdd ) k.sel = false;
			}
		}
	}
	//click on empty space in a lane / header without dragging: locate there
	if( !ImGui::IsMouseDown( 0 ) && g.drag != 0 )
	{
		if( g.drag == 2 && io.MouseDragMaxDistanceSqr[ 0 ] < 9 && M.x >= X0 && M.x < X1 && M.y > Y0 ) tl.Locate( show, tl.Snap( std::max( 0.0, T( g.dragStart.x ) ) ) );
		g.drag = 0;
		g.orig.clear();
	}
	//right click in the curve area
	if( curvesHov && ImGui::IsMouseClicked( 1 ) && hovRow >= 0 )
	{
		const Row& r = rows[ hovRow ];
		g.popTrack   = tl.tracks[ r.track ].uid;
		g.popLane    = { g.popTrack, r.lane };
		g.popT       = tl.Snap( std::max( 0.0, T( M.x ) ) );
		g.popKey     = -1;
		g.popCue     = r.kind == 2 ? cueHit( r ) : -1;
		g.popSeg     = -1;
		if( r.kind == 1 && hitKey >= 0 )
		{
			TlKey& k = tl.tracks[ r.track ].lanes[ r.lane ].keys[ hitKey ];
			if( !k.sel ) { ClearSel( tl ); k.sel = true; }
			g.popKey = hitKey;
		}
		else if( r.kind == 1 )
		{
			//on the curve between two keys: menu for this segment's curve type
			const TlLane& l = tl.tracks[ r.track ].lanes[ r.lane ];
			const double tm = T( M.x );
			for( int i = 0; i + 1 < (int)l.keys.size(); ++i )
				if( tm >= l.keys[ i ].t && tm < l.keys[ i + 1 ].t )
				{
					const float cy = l.kind == TlLaneKind::Color ? RY( r ) + r.h * 0.5f : KeyY( r, TlEval( l, tm ) );
					if( fabsf( M.y - cy ) < ( l.kind == TlLaneKind::Color ? r.h * 0.5f : 10 * ui ) ) g.popSeg = i;
					break;
				}
		}
		if( r.kind == 3 )
		{
			int part;
			g.clipIdx = clipHit( r, part );
			if( g.clipIdx >= 0 )
			{
				if( !tl.tracks[ r.track ].clips[ g.clipIdx ].sel ) { clearClipSel(); tl.tracks[ r.track ].clips[ g.clipIdx ].sel = true; }
				ImGui::OpenPopup( "##tlclipctx" );
			}
			else
				ImGui::OpenPopup( "##tlclipemptyctx" );
		}
		else if( r.kind == 4 ) ImGui::OpenPopup( "##tlaudioctx" );
		else ImGui::OpenPopup( r.kind == 2 ? "##tlcuectx" : ( g.popKey >= 0 ? "##tlkeyctx" : g.popSeg >= 0 ? "##tlsegctx" : "##tllanectx" ) );
	}

	//---- draw rows
	dl->PushClipRect( ImVec2( X0, Y0 ), ImVec2( X1, Y1 ), true );
	for( int ri = 0; ri < (int)rows.size(); ++ri )
	{
		const Row& r  = rows[ ri ];
		const float y = RY( r );
		if( y > Y1 || y + r.h < Y0 ) continue;
		TlTrack& trk   = tl.tracks[ r.track ];
		const ImU32 tc = ColU( trk.color, trk.mute ? 0.35f : 1.0f );
		if( r.kind == 2 )
		{
			//cue blocks from their time to the next timecode cue
			std::vector< int > idx;
			for( int i = 0; i < (int)show.cues.size(); ++i )
				if( show.cues[ i ].trigger == 2 ) idx.push_back( i );
			std::sort( idx.begin(), idx.end(), [ & ]( int a, int b ) { return show.cues[ a ].tc < show.cues[ b ].tc; } );
			for( size_t n = 0; n < idx.size(); ++n )
			{
				const Cue& c     = show.cues[ idx[ n ] ];
				const float xa   = X( c.tc ), xb = n + 1 < idx.size() ? X( show.cues[ idx[ n + 1 ] ].tc ) : X( tl.length );
				const bool cur   = idx[ n ] == show.cueCur;
				const float fade = show.CueFadeTime( idx[ n ] );
				dl->AddRectFilled( ImVec2( xa, y + 4 * ui ), ImVec2( std::max( xa + 3, xb - 2 ), y + r.h - 5 * ui ), cur ? IM_COL32( 120, 85, 30, 255 ) : IM_COL32( 70, 55, 30, 255 ), 3 * ui );
				//fade ramp
				const float fx = std::min( xb - 2, xa + fade * g.pps );
				dl->AddTriangleFilled( ImVec2( xa, y + r.h - 5 * ui ), ImVec2( fx, y + 4 * ui ), ImVec2( fx, y + r.h - 5 * ui ), IM_COL32( 255, 190, 90, 50 ) );
				dl->AddLine( ImVec2( xa, y + 2 * ui ), ImVec2( xa, y + r.h - 3 * ui ), IM_COL32( 255, 190, 90, 255 ), 2 * ui );
				const int si         = show.FindSnapUid( c.snap );
				const std::string nm = ( c.name.empty() ? std::string( "Cue " ) + std::to_string( idx[ n ] + 1 ) : c.name );
				std::string sn = si >= 0 ? show.snapshots[ si ].name : std::string( L( "(nur freigeben)" ) );
				if( sn == nm )
				{
					char fb[ 48 ];
					snprintf( fb, sizeof( fb ), L( "Fade %.1f s" ), fade );
					sn = fb;
				}
				dl->PushClipRect( ImVec2( xa, y ), ImVec2( std::max( xa + 1, xb - 4 ), y + r.h ), true );
				dl->AddText( ImVec2( xa + 5 * ui, y + 6 * ui ), kTxt, nm.c_str() );
				dl->AddText( ImVec2( xa + 5 * ui, y + 6 * ui + lh ), IM_COL32( 255, 210, 140, 200 ), sn.c_str() );
				dl->PopClipRect();
			}
			if( idx.empty() ) dl->AddText( ImVec2( X0 + 8 * ui, y + ( r.h - lh ) * 0.5f ), kDim, L( "Doppelklick = Cue hier anlegen (startet einen Snapshot). Cues mit Timecode aus der Cue-Liste erscheinen hier." ) );
			continue;
		}
		if( r.kind == 3 )
		{
			const float ya = y + 4 * ui, yb = y + r.h - 5 * ui;
			for( auto& c : trk.clips )
			{
				const float xa = X( c.t ), xb = std::max( xa + 3, X( c.t + c.len ) );
				if( xb < X0 || xa > X1 ) continue;
				const V3 cc    = c.kind == TlClipKind::Effect ? V3( 0.45f, 0.75f, 0.55f ) : trk.color;
				dl->AddRectFilled( ImVec2( xa, ya ), ImVec2( xb, yb ), ColU( cc * 0.35f, trk.mute ? 0.5f : 1.0f ), 3 * ui );
				//envelope: fade in, level, fade out
				const float top = ya + ( 1 - c.level ) * ( yb - ya );
				const ImVec2 env[ 4 ] = { ImVec2( xa, yb ), ImVec2( X( c.t + c.fadeIn ), top ), ImVec2( X( c.t + c.len - c.fadeOut ), top ), ImVec2( xb, yb ) };
				dl->AddConvexPolyFilled( env, 4, ColU( cc, 0.28f ) );
				dl->AddPolyline( env, 4, ColU( cc, 0.9f ), 0, 1.5f * ui );
				dl->AddRect( ImVec2( xa, ya ), ImVec2( xb, yb ), c.sel ? kSelC : ColU( cc, 0.9f ), 3 * ui, 0, c.sel ? 2 * ui : 1 );
				//fade handles
				for( float hx : { X( c.t + c.fadeIn ), X( c.t + c.len - c.fadeOut ) } ) dl->AddRectFilled( ImVec2( hx - 3.5f * ui, ya - 1 ), ImVec2( hx + 3.5f * ui, ya + 6 * ui ), IM_COL32( 235, 238, 245, 230 ) );
				dl->PushClipRect( ImVec2( std::max( xa, X0 ), ya ), ImVec2( std::min( xb, X1 ), yb ), true );
				const char* nm = ClipName( show, c );
				dl->AddText( ImVec2( std::max( xa, X0 ) + 6 * ui, ya + 6 * ui ), kTxt, nm );
				char info[ 64 ];
				snprintf( info, sizeof( info ), "%s  %.0f %%", c.kind == TlClipKind::Effect ? L( "Effekt" ) : "Snapshot", c.level * 100 );
				dl->AddText( ImVec2( std::max( xa, X0 ) + 6 * ui, ya + 6 * ui + lh ), kDim, info );
				dl->PopClipRect();
			}
			if( trk.clips.empty() ) dl->AddText( ImVec2( X0 + 8 * ui, y + ( r.h - lh ) * 0.5f ), kDim, L( "Doppelklick = Snapshot oder Effekt als Block einfügen. Kanten ziehen = Länge, obere Griffe = Fades." ) );
			continue;
		}
		if( r.kind == 4 && trk.kind == TlTrackKind::Show )
		{
			//Lightshow track: block over the recorded range, overview = mean DMX level per pixel column
			auto pl = LbxPlayerGet( trk.file );
			if( pl )
			{
				const double off = trk.offset, end = off + pl->duration;
				const float xa = X( off ), xb = X( end );
				const float ya = y + 3 * ui, yb = y + r.h - 3 * ui;
				if( xb > X0 && xa < X1 )
				{
					dl->AddRectFilled( ImVec2( std::max( xa, X0 ), ya ), ImVec2( std::min( xb, X1 ), yb ), ColU( trk.color * 0.35f, trk.mute ? 0.4f : 0.85f ), 4 * ui );
					const ImU32 wc = ColU( trk.color, trk.mute ? 0.35f : 0.9f );
					for( float x = std::max( X0, xa ); x < std::min( X1, xb ); x += 1 )
					{
						const int f0 = Clamp( (int)( ( T( x ) - off ) * pl->fps ), 0, pl->frames - 1 );
						const int f1 = Clamp( (int)( ( T( x + 1 ) - off ) * pl->fps ), f0, pl->frames - 1 );
						int pk       = 0;
						for( int k = f0; k <= f1; k += std::max( 1, ( f1 - f0 ) / 64 ) ) pk = std::max( pk, (int)pl->activity[ k ] );
						const float a = std::max( 1.0f, pk / 255.0f * ( yb - ya - lh - 4 * ui ) );
						dl->AddLine( ImVec2( x, yb - 2 * ui ), ImVec2( x, yb - 2 * ui - a ), wc );
					}
					dl->AddRect( ImVec2( xa, ya ), ImVec2( xb, yb ), ColU( trk.color, 0.9f ), 4 * ui );
					char info[ 160 ];
					snprintf( info, sizeof( info ), L( "%s  -  Lightshow 1:1, %.1f s, %d Universen" ), FileName( trk.file ).c_str(), pl->duration, (int)pl->uniNums.size() );
					dl->PushClipRect( ImVec2( std::max( xa, X0 ), ya ), ImVec2( std::min( xb, X1 ), yb ), true );
					dl->AddText( ImVec2( std::max( xa, X0 ) + 6 * ui, ya + 2 * ui ), kTxt, info );
					dl->PopClipRect();
				}
			}
			else
			{
				auto bad = LbxPlayerLoad( trk.file );//cached: the error of the last attempt
				const std::string msg = trk.file.empty() ? std::string( L( "Doppelklick = Lightshow-Datei laden" ) ) : FileName( trk.file ) + "  (" + bad->err + ")";
				dl->AddText( ImVec2( X0 + 8 * ui, y + ( r.h - lh ) * 0.5f ), IM_COL32( 255, 110, 90, 255 ), msg.c_str() );
			}
			continue;
		}
		if( r.kind == 4 )
		{
			const float cy = y + r.h * 0.5f, hh = r.h * 0.5f - 5 * ui;
			if( tlAudioData_ && tlAudioFile_ == trk.file )
			{
				const double off = trk.offset;
				const ImU32 wc   = ColU( trk.color, trk.mute ? 0.35f : 0.9f );
				for( float x = std::max( X0, X( off ) ); x < std::min( X1, X( off + tlAudioData_->Seconds() ) ); x += 1 )
				{
					const float pk = tlAudioData_->PeakAt( T( x ) - off, T( x + 1 ) - off ) * trk.gain;
					const float a  = std::max( 1.0f, std::min( 1.0f, pk ) * hh );
					dl->AddLine( ImVec2( x, cy - a ), ImVec2( x, cy + a ), wc );
				}
				dl->AddText( ImVec2( std::max( X0, X( off ) ) + 6 * ui, y + 3 * ui ), kDim, FileName( trk.file ).c_str() );
			}
			else
			{
				const std::string msg = trk.file.empty() ? std::string( L( "Doppelklick = Audiodatei laden" ) ) : FileName( trk.file ) + ( tlAudioErr_.empty() ? std::string( "  ..." ) : "  (" + tlAudioErr_ + ")" );
				dl->AddText( ImVec2( X0 + 8 * ui, cy - lh * 0.5f ), tlAudioErr_.empty() ? kDim : IM_COL32( 255, 110, 90, 255 ), msg.c_str() );
			}
			continue;
		}
		if( r.kind == 0 )
		{
			//summary of all lanes
			std::vector< double > ts;
			bool anySel = false;
			for( auto& l : trk.lanes )
				for( auto& k : l.keys )
				{
					ts.push_back( k.t );
					anySel = anySel || k.sel;
				}
			(void)ts;
			//one thin row per timelane (its colour), so every diamond can be traced to its lane
			const int nl    = std::max( 1, (int)trk.lanes.size() );
			const float pad = 3 * ui, rowH = ( r.h - 2 * pad ) / nl;
			const float s   = Clamp( rowH * 0.5f, 2.2f * ui, 4.5f * ui );
			for( int li = 0; li < (int)trk.lanes.size(); ++li )
			{
				const TlLane& l = trk.lanes[ li ];
				const float cy  = nl == 1 ? y + r.h * 0.5f : y + pad + rowH * ( li + 0.5f );
				const ImU32 lc  = ColU( LaneColor( l, li ), trk.mute || l.mute ? 0.35f : 1.0f );
				double last     = -1e18;
				for( auto& k : l.keys )
				{
					if( ( k.t - last ) * g.pps < 2 && !k.sel ) continue;
					last          = k.t;
					const float x = X( k.t );
					if( x < X0 - 8 || x > X1 + 8 ) continue;
					dl->AddQuadFilled( ImVec2( x, cy - s ), ImVec2( x + s, cy ), ImVec2( x, cy + s ), ImVec2( x - s, cy ), k.sel ? kSelC : lc );
				}
			}
			(void)anySel;
			continue;
		}
		//lane
		TlLane& l      = trk.lanes[ r.lane ];
		const bool dim = l.mute || trk.mute;
		if( l.rec ) dl->AddRectFilled( ImVec2( X0, y ), ImVec2( X1, y + r.h - 1 ), tl.record ? IM_COL32( 120, 20, 20, 60 ) : IM_COL32( 120, 20, 20, 28 ) );
		if( l.recBuf.size() > 1 )
		{
			ImVec2 pp;
			for( size_t i = 0; i < l.recBuf.size(); ++i )
			{
				const float v   = l.kind == TlLaneKind::Color ? std::max( { l.recBuf[ i ].second.x, l.recBuf[ i ].second.y, l.recBuf[ i ].second.z } ) : l.recBuf[ i ].second.x;
				const ImVec2 q( X( l.recBuf[ i ].first ), KeyY( r, v ) );
				if( i ) dl->AddLine( pp, q, IM_COL32( 255, 80, 60, 255 ), 2 * ui );
				pp = q;
			}
		}
		if( l.kind == TlLaneKind::Color )
		{
			const float ya = y + 6 * ui, yb = y + r.h - 6 * ui;
			if( !l.keys.empty() )
				for( float x = std::max( X0, X( 0 ) ); x < X1; x += 3 )
				{
					const V3 c = TlEvalColor( l, T( x ) );
					dl->AddRectFilled( ImVec2( x, ya ), ImVec2( x + 3, yb ), ColU( c, dim ? 0.35f : 1.0f ) );
				}
			else dl->AddText( ImVec2( X0 + 8 * ui, y + ( r.h - lh ) * 0.5f ), kDim, L( "Doppelklick = Farb-Key setzen" ) );
			for( auto& k : l.keys )
			{
				const ImVec2 c( X( k.t ), y + r.h * 0.5f );
				dl->AddCircleFilled( c, 6 * ui, IM_COL32( 10, 10, 12, 255 ) );
				dl->AddCircleFilled( c, 4.5f * ui, ColU( k.c ) );
				dl->AddCircle( c, 6 * ui, k.sel ? kSelC : IM_COL32( 230, 233, 240, 200 ), 0, k.sel ? 2.2f * ui : 1.2f * ui );
			}
			continue;
		}
		const ImU32 curveC = dim ? IM_COL32( 90, 100, 120, 255 ) : ColU( LaneColor( l, r.lane ) );
		if( l.keys.empty() ) dl->AddText( ImVec2( X0 + 8 * ui, y + ( r.h - lh ) * 0.5f ), kDim, L( "Doppelklick = Key setzen, K = Key aus aktuellem Wert" ) );
		else
		{
			//filled curve: sampled per few pixels, steps exact
			const float xa = std::max( X0, X( 0 ) ), step = 2.0f;
			ImVec2 prev( xa, KeyY( r, TlEval( l, T( xa ) ) ) );
			for( float x = xa + step; x <= X1 + step; x += step )
			{
				//a step key inside this pixel span: draw the exact corner
				const double ta = T( x - step ), tb = T( x );
				for( auto& k : l.keys )
					if( k.t > ta && k.t <= tb )
					{
						const float kx = X( k.t );
						const ImVec2 p1( kx, prev.y ), p2( kx, KeyY( r, k.v ) );
						dl->AddLine( prev, p1, curveC, 1.8f * ui );
						dl->AddLine( p1, p2, curveC, 1.8f * ui );
						prev = p2;
					}
				const ImVec2 p( x, KeyY( r, TlEval( l, tb ) ) );
				dl->AddLine( prev, p, curveC, 1.8f * ui );
				dl->AddRectFilled( ImVec2( prev.x, std::min( prev.y, p.y ) ), ImVec2( p.x, y + r.h - 1 ), ( curveC & 0x00FFFFFF ) | 0x16000000 );
				prev = p;
			}
			//keys + handles
			for( int k = 0; k < (int)l.keys.size(); ++k )
			{
				const TlKey& key = l.keys[ k ];
				const ImVec2 c( X( key.t ), KeyY( r, key.v ) );
				if( c.x < X0 - 10 || c.x > X1 + 10 ) continue;
				if( key.sel )
				{
					const double segN = k + 1 < (int)l.keys.size() ? l.keys[ k + 1 ].t - key.t : 0;
					const double segP = k > 0 ? key.t - l.keys[ k - 1 ].t : 0;
					if( key.interp == TlInterp::Bezier && segN > 0 )
					{
						const ImVec2 h( X( key.t + key.ox * segN ), KeyY( r, key.v + key.oy ) );
						dl->AddLine( c, h, IM_COL32( 255, 196, 70, 150 ), 1 );
						dl->AddCircleFilled( h, 3.5f * ui, kSelC );
					}
					if( k > 0 && l.keys[ k - 1 ].interp == TlInterp::Bezier && segP > 0 )
					{
						const ImVec2 h( X( key.t - key.ix * segP ), KeyY( r, key.v + key.iy ) );
						dl->AddLine( c, h, IM_COL32( 255, 196, 70, 150 ), 1 );
						dl->AddCircleFilled( h, 3.5f * ui, kSelC );
					}
				}
				const float s = ( key.sel ? 5.5f : 4.5f ) * ui;
				if( key.interp == TlInterp::Step ) dl->AddRectFilled( ImVec2( c.x - s * 0.8f, c.y - s * 0.8f ), ImVec2( c.x + s * 0.8f, c.y + s * 0.8f ), key.sel ? kSelC : kTxt );
				else dl->AddQuadFilled( ImVec2( c.x, c.y - s ), ImVec2( c.x + s, c.y ), ImVec2( c.x, c.y + s ), ImVec2( c.x - s, c.y ), key.sel ? kSelC : kTxt );
			}
		}
	}
	//playhead
	{
		const float x = X( rt.pos );
		if( x >= X0 && x <= X1 )
		{
			dl->AddLine( ImVec2( x, Y0 ), ImVec2( x, Y1 ), kPlayC, 1.5f * ui );
		}
	}
	dl->PopClipRect();
	{
		const float x = X( rt.pos );
		if( x >= X0 - 6 && x <= X1 + 6 )
		{
			dl->AddTriangleFilled( ImVec2( x - 6 * ui, Y0 - 9 * ui ), ImVec2( x + 6 * ui, Y0 - 9 * ui ), ImVec2( x, Y0 ), kPlayC );
			dl->AddLine( ImVec2( x, area0.y ), ImVec2( x, Y0 ), kPlayC, 1.5f * ui );
		}
	}
	//tooltip: value under the mouse
	if( curvesHov && hitKey >= 0 && g.drag == 0 )
	{
		const TlLane& l = tl.tracks[ rows[ hitRow ].track ].lanes[ rows[ hitRow ].lane ];
		const TlKey& k  = l.keys[ hitKey ];
		if( l.kind == TlLaneKind::Color ) ImGui::SetTooltip( "%s\nRGB %.0f / %.0f / %.0f %%\n%s", TcStr( k.t, tl.fps ).c_str(), k.c.x * 100, k.c.y * 100, k.c.z * 100, TlInterpName( k.interp ) );
		else ImGui::SetTooltip( "%s\n%s\n%s", TcStr( k.t, tl.fps ).c_str(), LaneVal( show, tl.tracks[ rows[ hitRow ].track ], l, k.v ).c_str(), TlInterpName( k.interp ) );
	}
	else if( g.drag == 1 && hovRow >= 0 )
	{
		for( auto& t : tl.tracks )
			for( auto& l : t.lanes )
				for( auto& k : l.keys )
					if( k.sel && t.uid == g.hLane.track ) { ImGui::SetTooltip( "%s  %s", TcStr( k.t, tl.fps ).c_str(), l.kind == TlLaneKind::Color ? "" : ValText( l, k.v ).c_str() ); goto tipDone; }
	tipDone:;
	}

	//================= headers (left column, ImGui widgets)
	dl->AddRectFilled( ImVec2( area0.x, Y0 ), ImVec2( X0 - 1, Y1 ), IM_COL32( 24, 27, 35, 255 ) );
	dl->AddLine( ImVec2( X0 - 1, area0.y ), ImVec2( X0 - 1, Y1 ), IM_COL32( 8, 9, 12, 255 ), 2 );
	//ruler corner: playhead time + help
	dl->AddText( ImVec2( area0.x + 8 * ui, area0.y + ( rulerH - lh ) * 0.5f ), kDim, tl.tracks.empty() ? L( "Lampen markieren, \"+ Spur\"" ) : L( "Spuren" ) );
	ImGui::PushClipRect( ImVec2( area0.x, Y0 ), ImVec2( X0 - 2, Y1 ), true );
	int delTrack = -1, moveTrack = -1, moveDir = 0;
	for( int ri = 0; ri < (int)rows.size(); ++ri )
	{
		const Row& r  = rows[ ri ];
		const float y = RY( r );
		if( y > Y1 || y + r.h < Y0 ) continue;
		TlTrack& trk = tl.tracks[ r.track ];
		ImGui::PushID( (int)trk.uid );
		const bool selT = g.selTrack == trk.uid;
		if( r.kind != 1 )
		{
			dl->AddRectFilled( ImVec2( area0.x, y ), ImVec2( X0 - 2, y + r.h - 1 ), selT ? IM_COL32( 38, 46, 62, 255 ) : kHead );
			dl->AddRectFilled( ImVec2( area0.x, y ), ImVec2( area0.x + 4 * ui, y + r.h - 1 ), ColU( trk.color ) );
			ImGui::SetCursorScreenPos( ImVec2( area0.x + 8 * ui, y + ( trackH - ImGui::GetFrameHeight() ) * 0.5f ) );
			ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 3 * ui, 1 * ui ) );
			ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 3 * ui, 0 ) );
			if( r.kind == 0 )
			{
				if( ImGui::ArrowButton( "##open", trk.open ? ImGuiDir_Down : ImGuiDir_Right ) ) trk.open = !trk.open;
				ImGui::SameLine();
			}
			//name (click = select the track, double click = select its lamps in the patch)
			const float bw   = ImGui::GetFrameHeight() * 1.1f;
			const int nb      = r.kind == 0 ? 3 : r.kind == 2 ? 1 : 2;//buttons on the right
			const float nameW = X0 - 2 - ImGui::GetCursorScreenPos().x - ( bw * nb + ( nb + 1 ) * 3 * ui + 4 * ui );
			if( ImGui::Selectable( ( trk.name + "##nm" ).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2( std::max( 20.0f, nameW ), 0 ) ) )
			{
				g.selTrack = trk.uid;
				if( ImGui::IsMouseDoubleClicked( 0 ) && trk.kind == TlTrackKind::Fixtures ) sel = trk.fixtures;
			}
			if( ImGui::IsItemHovered() )
			{
				if( trk.kind == TlTrackKind::Fixtures ) ImGui::SetTooltip( L( "%s\n%d Lampe(n), %d Time Lane(s)\nDoppelklick = Lampen auswählen, Rechtsklick = Menü" ), trk.name.c_str(), (int)trk.fixtures.size(), (int)trk.lanes.size() );
				else if( trk.kind == TlTrackKind::Cues ) ImGui::SetTooltip( "%s", L( "Cue-Spur: startet die Cues der Cue-Liste an ihrer Timecode-Zeit.\nDoppelklick in die Spur = neuer Cue, Ziehen = verschieben." ) );
				else if( trk.kind == TlTrackKind::Clips ) ImGui::SetTooltip( "%s", L( "Clip-Spur: Snapshots und Effekte als Blöcke mit Ein-/Ausblendung.\nÜberlappende Blöcke blenden ineinander." ) );
				else if( trk.kind == TlTrackKind::Audio ) ImGui::SetTooltip( L( "Audiospur: %s\nläuft mit der Time Line mit. Alt + Ziehen = verschieben, Doppelklick = andere Datei." ), trk.file.c_str() );
				else if( trk.kind == TlTrackKind::Show ) ImGui::SetTooltip( L( "Lightshow-Spur: %s\nspielt die aufgezeichneten DMX-Bilder 1:1 ab (alle Kanäle ihrer Universen).\nAlt + Ziehen = verschieben, Rechtsklick = in Time Lanes umwandeln." ), trk.file.c_str() );
			}
			if( ImGui::BeginPopupContextItem( "##trkctx" ) )
			{
				g.selTrack = trk.uid;
				ImGui::SetNextItemWidth( 180 * ui );
				char nb[ 96 ];
				snprintf( nb, sizeof( nb ), "%s", trk.name.c_str() );
				if( ImGui::InputText( L( "Name" ), nb, sizeof( nb ) ) ) { trk.name = nb; show.dirty = true; }
				ImGui::ColorEdit3( L( "Farbe" ), &trk.color.x, ImGuiColorEditFlags_NoInputs );
				if( trk.kind != TlTrackKind::Cues )
				{
					float lv = trk.level * 100;
					ImGui::SetNextItemWidth( 180 * ui );
					if( ImGui::SliderFloat( L( "Anteil" ), &lv, 0, 100, "%.0f %%" ) ) { trk.level = lv / 100; show.dirty = true; }
				}
				ImGui::Separator();
				if( trk.kind != TlTrackKind::Cues && trk.kind != TlTrackKind::Show && trk.kind != TlTrackKind::Audio && ImGui::MenuItem( L( "Time Lanes wählen..." ) ) ) tlLanePick_ = trk.uid;
				if( trk.kind == TlTrackKind::Show && ImGui::MenuItem( L( "In Time Lanes umwandeln" ) ) ) tlConvert_ = trk.uid;
				if( trk.kind == TlTrackKind::Fixtures )
				{
					if( ImGui::MenuItem( L( "Keys aus aktuellen Werten (alle Time Lanes)" ), "K" ) )
					{
						for( auto& l : trk.lanes )
						{
							V3 c;
							const float v = TlLiveValue( show, trk, l, &c );
							TlSetKey( l, tl.Snap( rt.pos ), v, &c );
						}
						show.dirty = true;
					}
					if( ImGui::MenuItem( L( "Lampen der Spur auswählen" ) ) ) sel = trk.fixtures;
					if( ImGui::MenuItem( L( "Lampen = aktuelle Auswahl" ), nullptr, false, !sel.empty() ) ) { trk.fixtures = sel; show.dirty = true; }
				}
				if( ImGui::MenuItem( L( "Alle Keys der Spur auswählen" ) ) )
				{
					ClearSel( tl );
					for( auto& l : trk.lanes )
						for( auto& k : l.keys ) k.sel = true;
				}
				ImGui::Separator();
				if( ImGui::MenuItem( L( "Nach oben" ), nullptr, false, r.track > 0 ) ) { moveTrack = r.track; moveDir = -1; }
				if( ImGui::MenuItem( L( "Nach unten" ), nullptr, false, r.track + 1 < (int)tl.tracks.size() ) ) { moveTrack = r.track; moveDir = 1; }
				if( ImGui::MenuItem( L( "Spur löschen" ) ) ) delTrack = r.track;
				ImGui::EndPopup();
			}
			if( r.kind == 0 )
			{
				ImGui::SameLine( X0 - area0.x - 2 - bw * 3 - 10 * ui );
				if( ImGui::Button( "+##lane", ImVec2( bw, 0 ) ) ) tlLanePick_ = trk.uid;
				if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Time Lanes (Kanäle) wählen" ) );
				ImGui::SameLine();
			}
			else
				ImGui::SameLine( X0 - area0.x - 2 - bw * nb - ( nb + 1 ) * 3 * ui );
			if( trk.mute ) ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.75f, 0.45f, 0.1f, 1 ) );
			const bool wasMute = trk.mute;
			if( ImGui::Button( "M##m", ImVec2( bw, 0 ) ) ) { trk.mute = !trk.mute; show.dirty = true; }
			if( wasMute ) ImGui::PopStyleColor();
			if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Stumm" ) );
			if( nb >= 2 )
			{
				ImGui::SameLine();
				const bool wasSolo = trk.solo;
				if( wasSolo ) ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.2f, 0.55f, 0.95f, 1 ) );
				if( ImGui::Button( "S##s", ImVec2( bw, 0 ) ) ) { trk.solo = !trk.solo; show.dirty = true; }
				if( wasSolo ) ImGui::PopStyleColor();
				if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Solo" ) );
			}
			ImGui::PopStyleVar( 2 );
		}
		else
		{
			TlLane& l = trk.lanes[ r.lane ];
			ImGui::PushID( r.lane );
			dl->AddRectFilled( ImVec2( area0.x + 4 * ui, y ), ImVec2( area0.x + 6 * ui, y + r.h - 1 ), ColU( trk.color, 0.5f ) );
			{
				//the timelane's colour (curve + its diamonds in the track row)
				const ImU32 lc = ColU( LaneColor( l, r.lane ), l.mute || trk.mute ? 0.4f : 1.0f );
				dl->AddRectFilled( ImVec2( area0.x + 9 * ui, y + 3 * ui ), ImVec2( area0.x + 13 * ui, y + r.h - 4 * ui ), lc, 2 * ui );
			}
			ImGui::SetCursorScreenPos( ImVec2( area0.x + 18 * ui, y + 4 * ui ) );
			ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 3 * ui, 1 * ui ) );
			ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 3 * ui, 0 ) );
			const std::string ln = TlLaneName( l );
			{
				const V3 c = LaneColor( l, r.lane );
				ImGui::TextColored( l.mute ? ImVec4( 0.5f, 0.52f, 0.58f, 1 ) : ImVec4( 0.55f + 0.45f * c.x, 0.55f + 0.45f * c.y, 0.55f + 0.45f * c.z, 1 ), "%s", ln.c_str() );
			}
			if( ImGui::BeginPopupContextItem( "##lanectxh" ) )
			{
				if( ImGui::MenuItem( L( "Stumm" ), nullptr, l.mute ) ) { l.mute = !l.mute; show.dirty = true; }
				if( ImGui::MenuItem( L( "Höhe: klein" ), nullptr, l.height == 0 ) ) l.height = 0;
				if( ImGui::MenuItem( L( "Höhe: normal" ), nullptr, l.height == 1 ) ) l.height = 1;
				if( ImGui::MenuItem( L( "Höhe: groß" ), nullptr, l.height == 2 ) ) l.height = 2;
				if( ImGui::MenuItem( L( "Key aus aktuellem Wert" ), "K" ) )
				{
					V3 c;
					const float v = TlLiveValue( show, trk, l, &c );
					TlSetKey( l, tl.Snap( rt.pos ), v, &c );
					show.dirty = true;
				}
				if( trk.kind == TlTrackKind::Fixtures && ImGui::MenuItem( L( "Fächer / Fan..." ) ) )
				{
					g.fanLane = { trk.uid, r.lane };
					g.openFan = true;
				}
				if( ImGui::MenuItem( L( "Aufnahme scharf" ), nullptr, l.rec ) ) l.rec = !l.rec;
				if( ImGui::MenuItem( L( "Alle Keys löschen" ) ) ) { l.keys.clear(); show.dirty = true; }
				if( ImGui::MenuItem( L( "Time Lane entfernen" ) ) )
				{
					trk.lanes.erase( trk.lanes.begin() + r.lane );
					show.dirty = true;
					ImGui::EndPopup();
					ImGui::PopStyleVar( 2 );
					ImGui::PopID();
					ImGui::PopID();
					break;
				}
				ImGui::EndPopup();
			}
			//current value
			if( l.kind == TlLaneKind::Color )
			{
				bool has;
				const V3 c = TlEvalColor( l, rt.pos, &has );
				if( has )
				{
					const ImVec2 p = ImGui::GetCursorScreenPos();
					dl->AddRectFilled( ImVec2( area0.x + 18 * ui, p.y + 3 * ui ), ImVec2( area0.x + 48 * ui, p.y + lh ), ColU( c ), 2 * ui );
				}
			}
			else
			{
				bool has;
				const float v = TlEval( l, rt.pos, &has );
				ImGui::SetCursorScreenPos( ImVec2( area0.x + 18 * ui, ImGui::GetCursorScreenPos().y ) );
				if( has && r.h > 40 * ui ) ImGui::TextDisabled( "%s", LaneVal( show, trk, l, v ).c_str() );
			}
			//programmer priority / fan
			{
				const bool held = trk.kind != TlTrackKind::Cues && TlLaneTouched( show, trk, l );
				const float fh = ImGui::GetFrameHeight();
				if( r.h > fh * 2 + 12 * ui && ( held || l.HasFan() ) )
				{
					ImGui::SetCursorScreenPos( ImVec2( X0 - 2 - fh * 4 - 13 * ui, y + 8 * ui + fh ) );
					if( held )
					{
						ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.7f, 0.42f, 0.1f, 1 ) );
						if( ImGui::SmallButton( L( "Programmer" ) ) ) TlLaneRelease( show, trk, l );
						ImGui::PopStyleColor();
						if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Im Programmer bewegt: hat Vorrang vor der Time Line.\nKlick = an die Time Line zurückgeben (sonst beim nächsten Play / Positionswechsel)." ) );
						ImGui::SameLine();
					}
					if( l.HasFan() )
					{
						ImGui::TextColored( ImVec4( 0.55f, 0.8f, 1, 1 ), "Fan" );
						if( ImGui::IsItemHovered() ) ImGui::SetTooltip( L( "Fächer: %.2f s Versatz, %.0f %% Spreizung" ), l.fanTime, l.fanValue * 100 );
					}
				}
			}
			//key buttons: previous / set key / next, record arm
			const float bw = ImGui::GetFrameHeight();
			ImGui::SetCursorScreenPos( ImVec2( X0 - 2 - bw * 4 - 13 * ui, y + 4 * ui ) );
			{
				const bool arm = l.rec;
				if( arm ) ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.8f, 0.18f, 0.15f, 1 ) );
				if( ImGui::Button( "R##rec", ImVec2( bw, 0 ) ) ) l.rec = !l.rec;
				if( arm ) ImGui::PopStyleColor();
				if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Aufnahme scharf: nimmt beim Abspielen mit Aufnahme (roter Knopf) die Programmer-Werte auf" ) );
				ImGui::SameLine();
			}
			if( ImGui::ArrowButton( "##prev", ImGuiDir_Left ) )
			{
				double best = -1;
				for( auto& k : l.keys )
					if( k.t < rt.pos - 1e-4 ) best = k.t;
				if( best >= 0 ) tl.Locate( show, best );
			}
			ImGui::SameLine();
			bool atKey = false;
			for( auto& k : l.keys ) atKey = atKey || fabs( k.t - rt.pos ) < 1e-3;
			if( atKey ) ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.75f, 0.55f, 0.15f, 1 ) );
			if( ImGui::Button( "K##k", ImVec2( bw, 0 ) ) )
			{
				V3 c;
				const float v = TlLiveValue( show, trk, l, &c );
				TlSetKey( l, tl.Snap( rt.pos ), v, &c );
				show.dirty = true;
			}
			if( atKey ) ImGui::PopStyleColor();
			if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Key an der Abspielposition aus dem aktuellen Wert (Programmer / Inspector)" ) );
			ImGui::SameLine();
			if( ImGui::ArrowButton( "##next", ImGuiDir_Right ) )
				for( auto& k : l.keys )
					if( k.t > rt.pos + 1e-4 ) { tl.Locate( show, k.t ); break; }
			ImGui::PopStyleVar( 2 );
			ImGui::PopID();
		}
		ImGui::PopID();
	}
	ImGui::PopClipRect();
	//header width splitter
	{
		ImGui::SetCursorScreenPos( ImVec2( X0 - 4 * ui, area0.y ) );
		ImGui::InvisibleButton( "##tlsplit", ImVec2( 6 * ui, rulerH ) );
		if( ImGui::IsItemHovered() || ImGui::IsItemActive() ) ImGui::SetMouseCursor( ImGuiMouseCursor_ResizeEW );
		if( ImGui::IsItemActive() ) g.headW += io.MouseDelta.x;
	}
	if( moveTrack >= 0 && moveDir ) { std::swap( tl.tracks[ moveTrack ], tl.tracks[ moveTrack + moveDir ] ); show.dirty = true; }
	if( delTrack >= 0 ) { tl.tracks.erase( tl.tracks.begin() + delTrack ); show.dirty = true; }
	//vertical scroll bar
	if( contentH > Y1 - Y0 )
	{
		const float vh = Y1 - Y0, bh = std::max( 20 * ui, vh * vh / ( contentH + 20 * ui ) );
		const float range = std::max( 1.0f, contentH + 20 * ui - vh );
		const float by    = Y0 + ( vh - bh ) * g.scrollY / range;
		ImGui::SetCursorScreenPos( ImVec2( X1 + 1 * ui, Y0 ) );
		ImGui::InvisibleButton( "##tlvbar", ImVec2( 9 * ui, vh ) );
		if( ImGui::IsItemActive() )
		{
			//grab: jump so the bar centre follows the mouse
			g.scrollY = Clamp( ( M.y - Y0 - bh * 0.5f ) / std::max( 1.0f, vh - bh ) * range, 0.0f, range );
		}
		const bool hv = ImGui::IsItemHovered() || ImGui::IsItemActive();
		dl->AddRectFilled( ImVec2( X1 + 3 * ui, by ), ImVec2( X1 + 9 * ui, by + bh ), hv ? IM_COL32( 100, 112, 135, 255 ) : IM_COL32( 70, 78, 95, 255 ), 3 * ui );
	}

	//================= keyboard (mouse over the timeline)
	if( panelHov && !io.WantTextInput && !ImGui::IsAnyItemActive() )
	{
		if( ImGui::IsKeyPressed( ImGuiKey_Home, false ) ) tl.Locate( show, tl.loop ? tl.loopA : 0 );
		if( ImGui::IsKeyPressed( ImGuiKey_End, false ) ) tl.Locate( show, tl.length );
		if( ImGui::IsKeyPressed( ImGuiKey_M, false ) && !io.KeyCtrl ) { tl.markers.push_back( { rt.pos, "Marker " + std::to_string( tl.markers.size() + 1 ) } ); show.dirty = true; }
		const double gs = tl.GridStep() > 0 ? tl.GridStep() : 1.0;
		if( ImGui::IsKeyPressed( ImGuiKey_LeftArrow ) ) tl.Locate( show, tl.Snap( rt.pos - gs ) );
		if( ImGui::IsKeyPressed( ImGuiKey_RightArrow ) ) tl.Locate( show, tl.Snap( rt.pos + gs ) );
		if( ImGui::IsKeyPressed( ImGuiKey_K, false ) && !io.KeyCtrl )
			if( TlTrack* t = tl.Find( g.selTrack ) )
			{
				for( auto& l : t->lanes )
				{
					V3 c;
					const float v = TlLiveValue( show, *t, l, &c );
					TlSetKey( l, tl.Snap( rt.pos ), v, &c );
				}
				show.dirty = true;
				SetStatus( L( "Keys gesetzt: " ) + t->name + "  " + TcStr( rt.pos, tl.fps ) );
			}
		//copy / paste selected keys (paste at the playhead)
		if( io.KeyCtrl && ImGui::IsKeyPressed( ImGuiKey_C, false ) && CountSel( tl ) )
		{
			g.clip.clear();
			double t0 = 1e18;
			for( auto& t : tl.tracks )
				for( auto& l : t.lanes )
					for( auto& k : l.keys )
						if( k.sel ) t0 = std::min( t0, k.t );
			for( auto& t : tl.tracks )
				for( int li = 0; li < (int)t.lanes.size(); ++li )
					for( auto k : t.lanes[ li ].keys )
						if( k.sel )
						{
							k.t -= t0;
							g.clip[ { t.uid, li } ].push_back( k );
						}
			SetStatus( L( "Keys kopiert" ) );
		}
		if( io.KeyCtrl && ImGui::IsKeyPressed( ImGuiKey_V, false ) && !g.clip.empty() )
		{
			ClearSel( tl );
			for( auto& c : g.clip )
				if( TlTrack* t = tl.Find( c.first.track ) )
					if( c.first.lane < (int)t->lanes.size() )
					{
						TlLane& l = t->lanes[ c.first.lane ];
						for( auto k : c.second )
						{
							const int i = TlSetKey( l, rt.pos + k.t, k.v, &k.c, k.interp );
							TlKey& n    = l.keys[ i ];
							n.ox = k.ox; n.oy = k.oy; n.ix = k.ix; n.iy = k.iy; n.interp = k.interp;
							n.sel = true;
						}
					}
			show.dirty = true;
		}
		//Delete: selected keys / clips
		bool clipSel = false;
		for( auto& t : tl.tracks )
			for( auto& c : t.clips ) clipSel = clipSel || c.sel;
		if( delKey_ && clipSel )
		{
			delKey_ = false;
			for( auto& t : tl.tracks ) t.clips.erase( std::remove_if( t.clips.begin(), t.clips.end(), []( const TlClip& c ) { return c.sel; } ), t.clips.end() );
			show.dirty = true;
		}
		if( delKey_ && CountSel( tl ) )
		{
			delKey_ = false;
			for( auto& t : tl.tracks )
				for( auto& l : t.lanes ) l.keys.erase( std::remove_if( l.keys.begin(), l.keys.end(), []( const TlKey& k ) { return k.sel; } ), l.keys.end() );
			show.dirty = true;
		}
		else if( delKey_ && hovRow >= 0 ) delKey_ = false;//never delete lamps from inside the timeline
	}

	//================= popups
	if( ImGui::BeginPopup( "##tlkeyctx" ) )
	{
		//the curve type belongs to the segment after a key; the last key of a lane has none, it sets the segment before it
		ImGui::TextDisabled( L( "%d Key(s) - Kurve zum nächsten Key" ), CountSel( tl ) );
		auto segOf = [ & ]( TlLane& l, int k ) -> TlKey* { return k + 1 < (int)l.keys.size() ? &l.keys[ k ] : ( k > 0 ? &l.keys[ k - 1 ] : nullptr ); };
		int cur = -2;//common curve type of the selection (-1 = mixed)
		for( auto& t : tl.tracks )
			for( auto& l : t.lanes )
				for( int k = 0; k < (int)l.keys.size(); ++k )
					if( l.keys[ k ].sel )
						if( TlKey* sk = segOf( l, k ) ) cur = cur == -2 || cur == (int)sk->interp ? (int)sk->interp : -1;
		for( int i = 0; i < (int)TlInterp::Count; ++i )
			if( ImGui::MenuItem( TlInterpName( (TlInterp)i ), nullptr, cur == i ) )
			{
				for( auto& t : tl.tracks )
					for( auto& l : t.lanes )
						for( int k = 0; k < (int)l.keys.size(); ++k )
							if( l.keys[ k ].sel )
								if( TlKey* sk = segOf( l, k ) ) sk->interp = (TlInterp)i;
				show.dirty = true;
			}
		ImGui::Separator();
		if( ImGui::MenuItem( L( "Wert / Zeit bearbeiten..." ) ) ) g.openKeyEdit = true;
		if( ImGui::MenuItem( L( "Kopieren" ), "Strg+C" ) )
		{
			g.clip.clear();
			double t0 = 1e18;
			for( auto& t : tl.tracks )
				for( auto& l : t.lanes )
					for( auto& k : l.keys )
						if( k.sel ) t0 = std::min( t0, k.t );
			for( auto& t : tl.tracks )
				for( int li = 0; li < (int)t.lanes.size(); ++li )
					for( auto k : t.lanes[ li ].keys )
						if( k.sel ) { k.t -= t0; g.clip[ { t.uid, li } ].push_back( k ); }
		}
		if( ImGui::MenuItem( L( "Löschen" ), "Entf" ) )
		{
			for( auto& t : tl.tracks )
				for( auto& l : t.lanes ) l.keys.erase( std::remove_if( l.keys.begin(), l.keys.end(), []( const TlKey& k ) { return k.sel; } ), l.keys.end() );
			show.dirty = true;
		}
		ImGui::EndPopup();
	}
	if( ImGui::BeginPopup( "##tlsegctx" ) )
	{
		TlTrack* t = tl.Find( g.popTrack );
		if( t && g.popLane.lane >= 0 && g.popLane.lane < (int)t->lanes.size() && g.popSeg >= 0 && g.popSeg + 1 < (int)t->lanes[ g.popLane.lane ].keys.size() )
		{
			TlLane& l = t->lanes[ g.popLane.lane ];
			TlKey& k  = l.keys[ g.popSeg ];
			ImGui::TextDisabled( "%s", L( "Kurve dieses Abschnitts" ) );
			for( int i = 0; i < (int)TlInterp::Count; ++i )
				if( ImGui::MenuItem( TlInterpName( (TlInterp)i ), nullptr, (int)k.interp == i ) ) { k.interp = (TlInterp)i; show.dirty = true; }
			ImGui::Separator();
			if( ImGui::MenuItem( L( "Key hier aus aktuellem Wert" ) ) )
			{
				V3 c;
				const float v = TlLiveValue( show, *t, l, &c );
				TlSetKey( l, g.popT, v, &c, k.interp );
				show.dirty = true;
			}
			if( ImGui::MenuItem( L( "Abspielposition hierher" ) ) ) tl.Locate( show, g.popT );
		}
		else
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if( ImGui::BeginPopup( "##tllanectx" ) )
	{
		TlTrack* t = tl.Find( g.popTrack );
		if( t && g.popLane.lane >= 0 && g.popLane.lane < (int)t->lanes.size() )
		{
			TlLane& l = t->lanes[ g.popLane.lane ];
			if( ImGui::MenuItem( L( "Key hier aus aktuellem Wert" ) ) )
			{
				V3 c;
				const float v = TlLiveValue( show, *t, l, &c );
				TlSetKey( l, g.popT, v, &c );
				show.dirty = true;
			}
			if( ImGui::MenuItem( L( "Einfügen (an der Abspielposition)" ), "Strg+V", false, !g.clip.empty() ) )
			{
				for( auto& c : g.clip )
					if( TlTrack* tt = tl.Find( c.first.track ) )
						if( c.first.lane < (int)tt->lanes.size() )
							for( auto k : c.second )
							{
								const int i = TlSetKey( tt->lanes[ c.first.lane ], rt.pos + k.t, k.v, &k.c, k.interp );
								tt->lanes[ c.first.lane ].keys[ i ].ox = k.ox;
								tt->lanes[ c.first.lane ].keys[ i ].oy = k.oy;
								tt->lanes[ c.first.lane ].keys[ i ].ix = k.ix;
								tt->lanes[ c.first.lane ].keys[ i ].iy = k.iy;
							}
				show.dirty = true;
			}
			if( ImGui::MenuItem( L( "Alle Keys der Time Lane auswählen" ) ) )
			{
				ClearSel( tl );
				for( auto& k : l.keys ) k.sel = true;
			}
			if( ImGui::MenuItem( L( "Abspielposition hierher" ) ) ) tl.Locate( show, g.popT );
		}
		else if( t )
		{
			if( ImGui::MenuItem( L( "Time Lanes wählen..." ) ) ) tlLanePick_ = t->uid;
			if( ImGui::MenuItem( L( "Abspielposition hierher" ) ) ) tl.Locate( show, g.popT );
		}
		ImGui::EndPopup();
	}
	if( ImGui::BeginPopup( "##tlcuectx" ) )
	{
		if( g.popCue >= 0 && g.popCue < (int)show.cues.size() )
		{
			Cue& c = show.cues[ g.popCue ];
			ImGui::TextDisabled( "%s", c.name.empty() ? ( "Cue " + std::to_string( g.popCue + 1 ) ).c_str() : c.name.c_str() );
			if( ImGui::MenuItem( L( "Jetzt starten (GO)" ) ) ) show.CueGo( g.popCue );
			if( ImGui::MenuItem( L( "Bearbeiten..." ) ) )
			{
				snprintf( g.name, sizeof( g.name ), "%s", c.name.c_str() );
				g.openCue = true;
			}
			if( ImGui::MenuItem( L( "Aus der Spur nehmen (Trigger: GO)" ) ) ) { c.trigger = 0; show.dirty = true; }
			if( ImGui::MenuItem( L( "Cue löschen" ) ) )
			{
				show.cues.erase( show.cues.begin() + g.popCue );
				if( show.cueCur >= (int)show.cues.size() ) show.cueCur = (int)show.cues.size() - 1;
				show.dirty = true;
			}
		}
		else
		{
			if( ImGui::MenuItem( L( "Cue hier anlegen..." ) ) ) g.openCueAdd = true;
			if( ImGui::MenuItem( L( "Abspielposition hierher" ) ) ) tl.Locate( show, g.popT );
		}
		ImGui::EndPopup();
	}
	if( g.openCueAdd ) { ImGui::OpenPopup( "##tlcueadd" ); g.openCueAdd = false; }
	if( ImGui::BeginPopup( "##tlcueadd" ) )
	{
		ImGui::TextDisabled( L( "Cue bei %s: welcher Snapshot?" ), TcStr( g.popT, tl.fps ).c_str() );
		if( show.snapshots.empty() ) ImGui::TextDisabled( "%s", L( "(noch keine Snapshots - im Tab Snapshots anlegen)" ) );
		for( int i = 0; i < (int)show.snapshots.size(); ++i )
		{
			ImGui::PushID( i );
			if( ImGui::MenuItem( show.snapshots[ i ].name.c_str() ) )
			{
				Cue c;
				c.name    = show.snapshots[ i ].name;
				c.snap    = show.snapshots[ i ].uid;
				c.trigger = 2;
				c.tc      = g.popT;
				//keep the list in time order among the timecode cues
				int at = (int)show.cues.size();
				for( int k = 0; k < (int)show.cues.size(); ++k )
					if( show.cues[ k ].trigger == 2 && show.cues[ k ].tc > c.tc ) { at = k; break; }
				show.cues.insert( show.cues.begin() + at, c );
				if( show.cueCur >= at ) ++show.cueCur;
				show.dirty = true;
			}
			ImGui::PopID();
		}
		ImGui::Separator();
		if( ImGui::MenuItem( L( "Nur freigeben (Snapshots aus)" ) ) )
		{
			Cue c;
			c.name    = L( "Aus" );
			c.trigger = 2;
			c.tc      = g.popT;
			show.cues.push_back( c );
			show.dirty = true;
		}
		ImGui::EndPopup();
	}
	if( g.openCue ) { ImGui::OpenPopup( "##tlcueedit" ); g.openCue = false; }
	if( ImGui::BeginPopup( "##tlcueedit" ) )
	{
		if( g.popCue >= 0 && g.popCue < (int)show.cues.size() )
		{
			Cue& c = show.cues[ g.popCue ];
			ImGui::SetNextItemWidth( 200 * ui );
			if( ImGui::InputText( L( "Name" ), g.name, sizeof( g.name ) ) ) { c.name = g.name; show.dirty = true; }
			const int si = show.FindSnapUid( c.snap );
			ImGui::SetNextItemWidth( 200 * ui );
			if( ImGui::BeginCombo( "Snapshot", si >= 0 ? show.snapshots[ si ].name.c_str() : L( "(nur freigeben)" ) ) )
			{
				if( ImGui::Selectable( L( "(nur freigeben)" ), si < 0 ) ) { c.snap = 0; show.dirty = true; }
				for( auto& s : show.snapshots )
					if( ImGui::Selectable( s.name.c_str(), s.uid == c.snap ) ) { c.snap = s.uid; show.dirty = true; }
				ImGui::EndCombo();
			}
			float tc = (float)c.tc;
			ImGui::SetNextItemWidth( 200 * ui );
			if( ImGui::DragFloat( L( "Zeit (s)" ), &tc, 0.02f, 0, (float)tl.length, "%.2f" ) ) { c.tc = std::max( 0.0f, tc ); show.dirty = true; }
			ImGui::SetNextItemWidth( 200 * ui );
			if( ImGui::DragFloat( L( "Fade (s, -1 = vom Snapshot)" ), &c.fade, 0.02f, -1, 60, "%.2f" ) ) show.dirty = true;
			if( ImGui::Checkbox( L( "Andere Cues freigeben" ), &c.release ) ) show.dirty = true;
		}
		ImGui::EndPopup();
	}
	if( g.openMarker ) { ImGui::OpenPopup( "##tlmarker" ); g.openMarker = false; }
	if( ImGui::BeginPopup( "##tlmarker" ) )
	{
		if( g.popMarker >= 0 && g.popMarker < (int)tl.markers.size() )
		{
			ImGui::SetNextItemWidth( 200 * ui );
			if( ImGui::IsWindowAppearing() ) ImGui::SetKeyboardFocusHere();
			if( ImGui::InputText( L( "Marker" ), g.name, sizeof( g.name ), ImGuiInputTextFlags_EnterReturnsTrue ) ) ImGui::CloseCurrentPopup();
			tl.markers[ g.popMarker ].name = g.name;
		}
		ImGui::EndPopup();
	}
	if( g.openKeyEdit ) { ImGui::OpenPopup( "##tlkeyedit" ); g.openKeyEdit = false; }
	if( ImGui::BeginPopup( "##tlkeyedit" ) )
	{
		//edits the clicked key; the value goes to all selected keys of the same lane
		TlTrack* t = tl.Find( g.popLane.track );
		if( t && g.popLane.lane >= 0 && g.popLane.lane < (int)t->lanes.size() && g.popKey >= 0 && g.popKey < (int)t->lanes[ g.popLane.lane ].keys.size() )
		{
			TlLane& l = t->lanes[ g.popLane.lane ];
			TlKey& k  = l.keys[ g.popKey ];
			ImGui::TextDisabled( "%s - %s", t->name.c_str(), TlLaneName( l ).c_str() );
			float ts = (float)k.t;
			ImGui::SetNextItemWidth( 200 * ui );
			if( ImGui::DragFloat( L( "Zeit (s)" ), &ts, 0.01f, 0, (float)tl.length, "%.3f" ) ) { k.t = std::max( 0.0f, ts ); show.dirty = true; }
			if( ImGui::IsItemDeactivatedAfterEdit() ) TlSort( l );
			if( l.kind == TlLaneKind::Color )
			{
				if( ImGui::ColorPicker3( "##c", &k.c.x, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB ) ) show.dirty = true;
			}
			else if( l.kind == TlLaneKind::Dmx )
			{
				int v = (int)lroundf( k.v * ( l.fine ? 65535 : 255 ) );
				ImGui::SetNextItemWidth( 200 * ui );
				if( ImGui::DragInt( L( "DMX-Wert" ), &v, l.fine ? 64.0f : 0.5f, 0, l.fine ? 65535 : 255 ) ) { k.v = v / ( l.fine ? 65535.0f : 255.0f ); show.dirty = true; }
			}
			else
			{
				float p = k.v * 100;
				ImGui::SetNextItemWidth( 200 * ui );
				if( ImGui::SliderFloat( L( "Wert" ), &p, 0, 100, "%.1f %%" ) ) { k.v = Saturate( p / 100 ); show.dirty = true; }
				float deg;
				if( TlDegrees( show, *t, l.attr, k.v, deg ) )
				{
					float dd = deg;
					ImGui::SetNextItemWidth( 200 * ui );
					if( ImGui::DragFloat( L( "Grad" ), &dd, 0.25f, -1000, 1000, "%+.1f\xc2\xb0" ) )
					{
						float full;
						TlDegrees( show, *t, l.attr, 1.0f, full );//+range/2
						if( full > 0 ) k.v = Saturate( 0.5f + dd / ( 2 * full ) );
						show.dirty = true;
					}
				}
			}
			int ip = (int)k.interp;
			ImGui::SetNextItemWidth( 200 * ui );
			if( ImGui::BeginCombo( L( "Kurve danach" ), TlInterpName( k.interp ) ) )
			{
				for( int i = 0; i < (int)TlInterp::Count; ++i )
					if( ImGui::Selectable( TlInterpName( (TlInterp)i ), ip == i ) ) { k.interp = (TlInterp)i; show.dirty = true; }
				ImGui::EndCombo();
			}
		}
		else
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	//clips
	if( g.openClipAdd ) { ImGui::OpenPopup( "##tlclipadd" ); g.openClipAdd = false; }
	auto addClip = [ & ]( TlClipKind k, uint32_t snap, int fx ) {
		if( TlTrack* t = tl.Find( g.popTrack ) )
		{
			TlClip c;
			c.kind = k;
			c.snap = snap;
			c.fx   = fx;
			c.t    = g.popT;
			c.len  = std::max( 1.0, tl.GridStep() > 0 ? tl.GridStep() * 8 : 4.0 );
			for( auto& o : t->clips ) o.sel = false;
			c.sel = true;
			t->clips.push_back( c );
			show.dirty = true;
		}
	};
	if( ImGui::BeginPopup( "##tlclipadd" ) || ImGui::BeginPopup( "##tlclipemptyctx" ) )
	{
		ImGui::TextDisabled( L( "Block bei %s" ), TcStr( g.popT, tl.fps ).c_str() );
		if( ImGui::BeginMenu( "Snapshot" ) )
		{
			if( show.snapshots.empty() ) ImGui::TextDisabled( "%s", L( "(noch keine Snapshots - im Tab Snapshots anlegen)" ) );
			for( auto& sn : show.snapshots )
				if( ImGui::MenuItem( sn.name.c_str() ) ) addClip( TlClipKind::Snapshot, sn.uid, 0 );
			ImGui::EndMenu();
		}
		if( ImGui::BeginMenu( L( "Effekt" ) ) )
		{
			if( show.effects.empty() ) ImGui::TextDisabled( "%s", L( "(noch keine Effekte)" ) );
			for( auto& e : show.effects )
			{
				ImGui::PushID( e.id );
				if( ImGui::MenuItem( e.name.c_str() ) ) addClip( TlClipKind::Effect, 0, e.id );
				ImGui::PopID();
			}
			ImGui::EndMenu();
		}
		ImGui::EndPopup();
	}
	if( ImGui::BeginPopup( "##tlclipctx" ) )
	{
		TlTrack* t = tl.Find( g.popTrack );
		if( t && g.clipIdx >= 0 && g.clipIdx < (int)t->clips.size() )
		{
			TlClip& c = t->clips[ g.clipIdx ];
			ImGui::TextUnformatted( ClipName( show, c ) );
			float a = (float)c.t, len = (float)c.len, lv = c.level * 100;
			ImGui::SetNextItemWidth( 180 * ui );
			if( ImGui::DragFloat( L( "Start (s)" ), &a, 0.02f, 0, (float)tl.length, "%.2f" ) ) { c.t = std::max( 0.0f, a ); show.dirty = true; }
			ImGui::SetNextItemWidth( 180 * ui );
			if( ImGui::DragFloat( L( "Länge (s)" ), &len, 0.02f, 0.05f, 36000, "%.2f" ) ) { c.len = std::max( 0.05f, len ); show.dirty = true; }
			ImGui::SetNextItemWidth( 180 * ui );
			if( ImGui::DragFloat( L( "Einblenden (s)" ), &c.fadeIn, 0.02f, 0, (float)c.len, "%.2f" ) ) show.dirty = true;
			ImGui::SetNextItemWidth( 180 * ui );
			if( ImGui::DragFloat( L( "Ausblenden (s)" ), &c.fadeOut, 0.02f, 0, (float)c.len, "%.2f" ) ) show.dirty = true;
			ImGui::SetNextItemWidth( 180 * ui );
			if( ImGui::SliderFloat( L( "Stärke" ), &lv, 0, 100, "%.0f %%" ) ) { c.level = lv / 100; show.dirty = true; }
			if( c.kind == TlClipKind::Snapshot && ImGui::BeginCombo( "Snapshot", ClipName( show, c ) ) )
			{
				for( auto& sn : show.snapshots )
					if( ImGui::Selectable( sn.name.c_str(), sn.uid == c.snap ) ) { c.snap = sn.uid; show.dirty = true; }
				ImGui::EndCombo();
			}
			ImGui::Separator();
			if( ImGui::MenuItem( L( "Duplizieren (dahinter)" ) ) )
			{
				TlClip d = c;
				d.t      = c.t + c.len;
				d.sel    = false;
				t->clips.push_back( d );
				show.dirty = true;
			}
			if( ImGui::MenuItem( L( "Abspielposition an den Anfang" ) ) ) tl.Locate( show, c.t );
			if( ImGui::MenuItem( L( "Löschen" ), "Entf" ) )
			{
				t->clips.erase( t->clips.begin() + g.clipIdx );
				show.dirty = true;
			}
		}
		ImGui::EndPopup();
	}
	if( ImGui::BeginPopup( "##tlaudioctx" ) )
	{
		TlTrack* t = tl.Find( g.popTrack );
		if( t && t->kind == TlTrackKind::Show )
		{
			auto pl = LbxPlayerLoad( t->file );
			ImGui::TextUnformatted( t->file.empty() ? L( "(keine Datei)" ) : FileName( t->file ).c_str() );
			if( pl->err.empty() ) ImGui::TextDisabled( L( "\"%s\" - %.1f s, %d fps, %d Universen" ), pl->name.c_str(), pl->duration, pl->fps, (int)pl->uniNums.size() );
			else ImGui::TextColored( ImVec4( 1, 0.45f, 0.35f, 1 ), "%s", pl->err.c_str() );
			if( ImGui::MenuItem( L( "Datei laden..." ) ) )
			{
				const std::string f = OpenLightshowDialog();
				if( !f.empty() ) { t->file = f; LbxPlayerLoad( f, true ); show.dirty = true; }
			}
			if( ImGui::MenuItem( L( "Neu laden" ) ) ) LbxPlayerLoad( t->file, true );
			float off = (float)t->offset;
			ImGui::SetNextItemWidth( 180 * ui );
			if( ImGui::DragFloat( L( "Start auf der Time Line (s)" ), &off, 0.01f, -3600, 36000, "%.2f" ) ) { t->offset = off; show.dirty = true; }
			if( pl->err.empty() && ImGui::MenuItem( L( "Time-Line-Länge = Ende der Lightshow" ) ) ) { tl.length = std::max( 1.0, t->offset + pl->duration ); show.dirty = true; }
			ImGui::Separator();
			if( pl->err.empty() && ImGui::MenuItem( L( "In Time Lanes umwandeln" ) ) ) tlConvert_ = t->uid;
		}
		else if( t )
		{
			ImGui::TextUnformatted( t->file.empty() ? L( "(keine Datei)" ) : FileName( t->file ).c_str() );
			if( tlAudioData_ && tlAudioFile_ == t->file ) ImGui::TextDisabled( "%.1f s, %d Hz, %d ch", tlAudioData_->Seconds(), tlAudioData_->rate, tlAudioData_->ch );
			if( !tlAudioErr_.empty() ) ImGui::TextColored( ImVec4( 1, 0.45f, 0.35f, 1 ), "%s", tlAudioErr_.c_str() );
			if( ImGui::MenuItem( L( "Datei laden..." ) ) )
			{
				const std::string f = OpenAudioDialog();
				if( !f.empty() ) { t->file = f; t->name = FileStem( f ); show.dirty = true; }
			}
			float gdb = t->gain > 0 ? 20 * log10f( t->gain ) : -60;
			ImGui::SetNextItemWidth( 180 * ui );
			if( ImGui::SliderFloat( L( "Lautstärke" ), &gdb, -40, 12, "%.1f dB" ) ) { t->gain = gdb <= -39.9f ? 0 : powf( 10, gdb / 20 ); show.dirty = true; }
			float off = (float)t->offset;
			ImGui::SetNextItemWidth( 180 * ui );
			if( ImGui::DragFloat( L( "Start auf der Time Line (s)" ), &off, 0.01f, -3600, 36000, "%.2f" ) ) { t->offset = off; show.dirty = true; }
			if( tlAudioData_ && tlAudioFile_ == t->file && ImGui::MenuItem( L( "Time-Line-Länge = Audiolänge" ) ) ) { tl.length = std::max( 1.0, t->offset + tlAudioData_->Seconds() ); show.dirty = true; }
		}
		ImGui::EndPopup();
	}
	if( g.openFan ) { ImGui::OpenPopup( "##tlfan" ); g.openFan = false; }
	if( ImGui::BeginPopup( "##tlfan" ) )
	{
		TlTrack* t = tl.Find( g.fanLane.track );
		if( t && g.fanLane.lane >= 0 && g.fanLane.lane < (int)t->lanes.size() )
		{
			TlLane& l = t->lanes[ g.fanLane.lane ];
			ImGui::Text( L( "Fächer: %s (%d Lampen)" ), TlLaneName( l ).c_str(), (int)t->fixtures.size() );
			ImGui::SetNextItemWidth( 220 * ui );
			if( ImGui::Combo( L( "Richtung" ), &l.fanOrder, L( "Spur-Reihenfolge  <\0Links -> rechts\0Mitte nach außen  <>\0Umgekehrt  >\0Vorne -> hinten\0Außen nach innen  ><\0" ) ) ) show.dirty = true;
			ImGui::SetNextItemWidth( 220 * ui );
			if( ImGui::DragFloat( L( "Zeitversatz (s)" ), &l.fanTime, 0.01f, -60, 60, "%.2f s" ) ) show.dirty = true;
			if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Die letzte Lampe läuft so viel später (negativ = früher). Ergibt Wellen / Lauflichter." ) );
			float fv = l.fanValue * 100;
			ImGui::SetNextItemWidth( 220 * ui );
			if( ImGui::DragFloat( L( "Wert-Spreizung" ), &fv, 0.25f, -200, 200, "%.0f %%" ) ) { l.fanValue = fv / 100; show.dirty = true; }
			if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s", L( "Erste Lampe minus halbe Spreizung, letzte plus halbe Spreizung (z. B. Pan-Fächer)." ) );
			if( ImGui::Button( L( "Zurücksetzen" ) ) ) { l.fanTime = 0; l.fanValue = 0; show.dirty = true; }
		}
		ImGui::EndPopup();
	}
	//lane picker: which attributes of the track's lamps are automated
	if( tlLanePick_ ) { g.popTrack = tlLanePick_; tlLanePick_ = 0; ImGui::OpenPopup( "##tllanes" ); }
	if( ImGui::BeginPopup( "##tllanes" ) )
	{
		TlTrack* t = tl.Find( g.popTrack );
		if( !t ) ImGui::CloseCurrentPopup();
		else if( t->kind == TlTrackKind::Fixtures )
		{
			ImGui::TextUnformatted( t->name.c_str() );
			ImGui::TextDisabled( L( "Welche Kanäle sollen automatisiert werden? (%d Lampe(n))" ), (int)t->fixtures.size() );
			ImGui::Separator();
			auto toggle = [ & ]( TlLaneKind kind, Attr a, const char* label ) {
				int at = -1;
				for( int i = 0; i < (int)t->lanes.size(); ++i )
					if( t->lanes[ i ].kind == kind && ( kind != TlLaneKind::Attr || t->lanes[ i ].attr == a ) ) at = i;
				bool on = at >= 0;
				if( ImGui::Checkbox( label, &on ) )
				{
					if( on )
					{
						TlLane l;
						l.kind = kind;
						l.attr = a;
						t->lanes.push_back( l );
					}
					else
						t->lanes.erase( t->lanes.begin() + at );
					t->open    = true;
					show.dirty = true;
				}
			};
			const std::vector< Attr > attrs = TlTrackAttrs( show, t->fixtures );
			bool emit = false;
			for( Attr a : attrs ) emit = emit || IsEmitter( a );
			if( emit ) toggle( TlLaneKind::Color, Attr::None, L( "Farbe (eine Time Lane für die Farbmischung)" ) );
			if( attrs.empty() ) ImGui::TextDisabled( "%s", L( "(keine Lampen mit Kanälen in der Spur)" ) );
			int col = 0;
			for( Attr a : attrs )
			{
				ImGui::PushID( (int)a );
				if( col++ % 3 ) ImGui::SameLine( 12 * ui + ( col - 1 ) % 3 * 150 * ui );
				toggle( TlLaneKind::Attr, a, AttrName( a ) );
				ImGui::PopID();
			}
			ImGui::Separator();
			if( ImGui::Button( L( "Fertig" ) ) ) ImGui::CloseCurrentPopup();
		}
		else if( t->kind == TlTrackKind::Dmx )
		{
			static int u = 1, c = 1, n = 1;
			static bool fine = false;
			ImGui::TextDisabled( "%s", L( "DMX-Kanäle als Time Lanes hinzufügen" ) );
			ImGui::SetNextItemWidth( 120 * ui );
			ImGui::InputInt( "Universe", &u );
			ImGui::SetNextItemWidth( 120 * ui );
			ImGui::InputInt( L( "Erster Kanal" ), &c );
			ImGui::SetNextItemWidth( 120 * ui );
			ImGui::InputInt( L( "Anzahl" ), &n );
			ImGui::Checkbox( L( "16 bit (Kanal + Fein-Kanal)" ), &fine );
			u = std::max( 1, u );
			c = Clamp( c, 1, 512 );
			n = Clamp( n, 1, 64 );
			if( ImGui::Button( L( "Hinzufügen" ) ) )
			{
				for( int i = 0; i < n; ++i )
				{
					TlLane l;
					l.kind = TlLaneKind::Dmx;
					l.uni  = u;
					l.ch   = c + i * ( fine ? 2 : 1 );
					l.fine = fine;
					if( l.ch + ( fine ? 1 : 0 ) <= 512 ) t->lanes.push_back( l );
				}
				t->open    = true;
				show.dirty = true;
				ImGui::CloseCurrentPopup();
			}
			for( int i = 0; i < (int)t->lanes.size(); ++i )
			{
				ImGui::PushID( i );
				ImGui::TextUnformatted( TlLaneName( t->lanes[ i ] ).c_str() );
				ImGui::SameLine();
				if( ImGui::SmallButton( "x" ) ) { t->lanes.erase( t->lanes.begin() + i ); show.dirty = true; ImGui::PopID(); break; }
				ImGui::PopID();
			}
		}
		ImGui::EndPopup();
	}
	ImGui::EndChild();
}
}//namespace ls
