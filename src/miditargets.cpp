#include "miditargets.h"
#include <algorithm>

namespace ls
{
static std::string KeyOf( const std::string& s )
{
	std::string r;
	for( char c : ToLower( s ) )
		if( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) ) r += c;
	return r;
}

const char* ParamTypeName( ParamType t )
{
	switch( t )
	{
	case ParamType::Int: return "int";
	case ParamType::Bool: return "bool";
	case ParamType::Trigger: return "trigger";
	default: return "float";
	}
}

std::string MidiTargetKey( const MidiMap& m )
{
	char b[ 96 ];
	switch( m.target )
	{
	case MidiTarget::FixtureAttr:
		snprintf( b, sizeof( b ), "attr:%d:%s:%d", m.fixtures.empty() ? 0 : m.fixtures[ 0 ], AttrName( m.attr ), m.head );
		return b;
	case MidiTarget::DmxChannel:
		snprintf( b, sizeof( b ), "dmx:%d:%d%s", m.universe, m.channelDmx, m.sixteen ? ":16" : "" );
		return b;
	case MidiTarget::Snapshot: snprintf( b, sizeof( b ), "snap:%d", m.snapshot + 1 ); return b;
	case MidiTarget::Haze: return "haze";
	case MidiTarget::Master: return "master";
	case MidiTarget::Param: return "fx:" + ToLower( m.param );
	}
	return "";
}

MidiMap MidiMapForTarget( const MidiTargetInfo& t ) { return t.tmpl; }

std::vector< MidiTargetInfo > CollectMidiTargets( Show& show )
{
	std::vector< MidiTargetInfo > out;

	//---- lamps: every attribute, global and per pixel head
	for( auto& f : show.fixtures )
	{
		const Mode* m = f.M();
		if( !m ) continue;
		const std::string cat = L( "Lampen › " ) + f.name;
		std::vector< Attr > attrs;
		for( auto& c : m->ch )
			if( !c.isFine && c.attr != Attr::None && std::find( attrs.begin(), attrs.end(), c.attr ) == attrs.end() ) attrs.push_back( c.attr );
		for( Attr a : attrs )
		{
			std::vector< int > heads;
			bool global = false;
			for( auto& c : m->ch )
			{
				if( c.isFine || c.attr != a ) continue;
				if( c.pixels.empty() ) global = true;
				else
					for( int h : c.pixels )
						if( std::find( heads.begin(), heads.end(), h ) == heads.end() ) heads.push_back( h );
			}
			std::sort( heads.begin(), heads.end() );
			auto add = [ & ]( int head ) {
				MidiTargetInfo t;
				t.kind     = "fixtureAttr";
				t.key      = "attr:" + std::to_string( f.id ) + ":" + AttrName( a ) + ":" + std::to_string( head );
				t.category = cat;
				t.label    = AttrName( a );
				if( head >= 0 ) t.label += " (Pixel " + std::to_string( head + 1 ) + ")";
				t.addr     = "/ls/fixture/" + std::to_string( f.id ) + "/" + KeyOf( AttrName( a ) ) + ( head >= 0 ? "/" + std::to_string( head + 1 ) : "" );
				t.type     = "float";
				t.value    = show.GetAttr( f, a, head );
				t.fixtures = { f.id };
				t.attr     = AttrName( a );
				t.head     = head;
				t.tmpl.target   = MidiTarget::FixtureAttr;
				t.tmpl.fixtures = { f.id };
				t.tmpl.attr     = a;
				t.tmpl.head     = head;
				out.push_back( t );
			};
			if( global ) add( -1 );
			for( int h : heads ) add( h );
		}
	}

	//---- DMX channels of every universe that has a lamp: all 512 channels, name = lamp · channel or "frei"
	std::vector< int > used;
	for( auto& f : show.fixtures )
		if( std::find( used.begin(), used.end(), f.universe ) == used.end() ) used.push_back( f.universe );
	std::sort( used.begin(), used.end() );
	for( int u : used )
	{
		const int ui = show.UniIndex( u );
		for( int ch = 1; ch <= 512; ++ch )
		{
			const FixtureInst* fi = nullptr;
			int slot              = -1;
			for( auto& f : show.fixtures )
			{
				if( f.universe != u || ch < f.address || f.M() == nullptr || ch >= f.address + f.M()->Footprint() ) continue;
				fi   = &f;
				slot = ch - f.address;
				break;
			}
			char num[ 16 ];
			snprintf( num, sizeof( num ), "%d.%03d", u, ch );
			std::string name = L( "frei" );
			const Channel* chan = nullptr;
			if( fi )
			{
				chan = &fi->M()->ch[ slot ];
				name = fi->name + " · " + chan->name;
			}
			MidiTargetInfo t;
			t.kind     = "dmx";
			t.category = "DMX › Universe " + std::to_string( u );
			t.label    = std::string( num ) + " " + name;
			t.key      = "dmx:" + std::to_string( u ) + ":" + std::to_string( ch );
			t.addr     = "/ls/dmx/" + std::to_string( u ) + "/" + std::to_string( ch );
			t.type     = "int";
			t.min = 0, t.max = 255;
			t.value    = ui >= 0 ? show.dmx[ ui ][ ch - 1 ] : 0;
			t.universe = u;
			t.channel  = ch;
			t.tmpl.target     = MidiTarget::DmxChannel;
			t.tmpl.universe   = u;
			t.tmpl.channelDmx = ch;
			out.push_back( t );
			if( chan && !chan->isFine && chan->fine >= 0 )
			{
				MidiTargetInfo t16 = t;
				t16.key            = t.key + ":16";
				t16.label          = t.label + " (16 Bit)";
				t16.sixteen        = true;
				t16.type           = "int";
				t16.min = 0, t16.max = 65535;
				t16.addr           = t.addr;
				t16.tmpl.sixteen   = true;
				out.push_back( t16 );
			}
		}
	}

	//---- snapshots, haze, master
	for( int i = 0; i < (int)show.snapshots.size(); ++i )
	{
		MidiTargetInfo t;
		t.kind     = "snapshot";
		t.category = "Snapshots";
		t.key      = "snap:" + std::to_string( i + 1 );
		t.label    = "Snapshot " + std::to_string( i + 1 ) + " " + show.snapshots[ i ].name;
		t.addr     = "/ls/snapshot/" + std::to_string( i + 1 );
		t.type     = "trigger";
		t.snapshot = i + 1;
		t.tmpl.target   = MidiTarget::Snapshot;
		t.tmpl.snapshot = i;
		out.push_back( t );
	}
	{
		MidiTargetInfo t;
		t.kind     = "haze";
		t.category = L( "Allgemein" );
		t.key      = "haze";
		t.label    = "Haze";
		t.addr     = "/ls/haze";
		t.type     = "float";
		t.value    = show.stage.haze;
		t.tmpl.target = MidiTarget::Haze;
		out.push_back( t );
	}
	{
		MidiTargetInfo t;
		t.kind     = "master";
		t.category = L( "Allgemein" );
		t.key      = "master";
		t.label    = "Master";
		t.addr     = "/ls/master";
		t.type     = "float";
		t.min = 0, t.max = 10;
		t.value    = show.master;
		t.tmpl.target = MidiTarget::Master;
		out.push_back( t );
	}

	//---- parameter register (stage, effects, tempo, projectors, objects ...)
	for( const Param& p : Params( show ) )
	{
		if( p.addr == "/ls/master" || p.addr == "/ls/haze" ) continue;//listed above
		if( p.addr.compare( 0, 13, "/ls/snapshot/" ) == 0 && p.addr.find_first_not_of( "0123456789", 13 ) == std::string::npos ) continue;//snapshot trigger: "snap:" above
		MidiTargetInfo t;
		t.kind     = "param";
		t.category = p.group;
		t.key      = "fx:" + p.addr;
		t.label    = p.label;
		t.addr     = p.addr;
		t.type     = ParamTypeName( p.type );
		t.min      = p.min;
		t.max      = p.max;
		t.value    = p.type == ParamType::Trigger ? 0.0f : p.get();
		t.tmpl.target = MidiTarget::Param;
		t.tmpl.param  = p.addr;
		if( p.type == ParamType::Bool ) t.tmpl.toggle = true;
		out.push_back( t );
	}
	return out;
}
}//namespace ls
