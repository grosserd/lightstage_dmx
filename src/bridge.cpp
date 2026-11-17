#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "bridge.h"
#include "effects.h"
#include "miditargets.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ls
{
static const int kProtocol        = 1;
static const double kTimeout      = 8.0;   //clients are dropped after this (DMX they set stays, /ls/release frees it)
static const double kTickPeriod   = 1.0 / 30.0;
static const size_t kMaxPacket    = 1400;
static const int kMaxDmxPerMsg    = 200;

//------------------------------------------------------------------ OSC writer
namespace
{
struct OscW
{
	std::vector< uint8_t > b;
	OscW( const std::string& addr, const std::string& tags )
	{
		Str( addr );
		Str( "," + tags );
	}
	void Str( const std::string& s )
	{
		b.insert( b.end(), s.begin(), s.end() );
		b.push_back( 0 );
		while( b.size() & 3 ) b.push_back( 0 );
	}
	void U32( uint32_t v )
	{
		b.push_back( (uint8_t)( v >> 24 ) );
		b.push_back( (uint8_t)( ( v >> 16 ) & 255 ) );
		b.push_back( (uint8_t)( ( v >> 8 ) & 255 ) );
		b.push_back( (uint8_t)( v & 255 ) );
	}
	OscW& I( int v )
	{
		U32( (uint32_t)(int32_t)v );
		return *this;
	}
	OscW& F( float f )
	{
		uint32_t v;
		memcpy( &v, &f, 4 );
		U32( v );
		return *this;
	}
	OscW& S( const std::string& s )
	{
		Str( s );
		return *this;
	}
};

std::string KeyAddr( uint32_t ip, uint16_t port )
{
	in_addr a;
	a.s_addr = ip;
	char b[ 64 ];
	snprintf( b, sizeof( b ), "%s:%d", inet_ntoa( a ), (int)port );
	return b;
}

std::string LocalIpFor( uint32_t ip, uint16_t port )
{
	std::string r = "127.0.0.1";
	SOCKET s      = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	if( s == INVALID_SOCKET ) return r;
	sockaddr_in to{};
	to.sin_family      = AF_INET;
	to.sin_addr.s_addr = ip;
	to.sin_port        = htons( port );
	if( connect( s, (sockaddr*)&to, sizeof( to ) ) == 0 )
	{
		sockaddr_in me{};
		int len = sizeof( me );
		if( getsockname( s, (sockaddr*)&me, &len ) == 0 )
		{
			char b[ 32 ];
			inet_ntop( AF_INET, &me.sin_addr, b, sizeof( b ) );
			r = b;
		}
	}
	closesocket( s );
	return r;
}

//structure signature: changes when lamps, modes, names, effects, programs, snapshots or objects change
std::string StructSig( Show& s )
{
	std::string r;
	for( auto& f : s.fixtures )
		r += std::to_string( f.id ) + "|" + f.defId + "|" + f.name + "|" + std::to_string( f.mode ) + "|" + std::to_string( f.universe ) + "|" + std::to_string( f.address ) + ";";
	for( auto& u : s.universes ) r += "u" + std::to_string( u.num ) + ";";
	for( auto& e : s.effects ) r += "e" + std::to_string( e.id ) + "|" + e.name + "|" + std::to_string( (int)e.kind ) + ";";
	for( auto& p : s.programs )
	{
		r += "g" + p.name + ":";
		for( int id : p.effects ) r += std::to_string( id ) + ",";
		r += ";";
	}
	for( auto& n : s.snapshots ) r += "n" + n.name + ";";
	for( auto& g : s.groups ) r += "q" + g.name + "|" + std::to_string( g.kind ) + "|" + std::to_string( (int)g.attr ) + ";";
	r += "S" + s.showId + ";";
	for( auto& m : s.ableton )
	{
		r += "a" + m.kind + "|" + m.id + "|" + m.label;
		for( auto& k : m.knobs ) r += "," + k;
		for( auto& b : m.buttons ) r += "," + b;
		r += ";";
	}
	r += "c" + std::to_string( s.projectors.size() ) + "/" + std::to_string( s.screens.size() ) + "/" + std::to_string( s.models.size() ) + "/" + std::to_string( s.stands.size() );
	r += "p" + std::to_string( Params( s ).size() ) + "l" + std::to_string( gLang );//language: names in the manifest
	return r;
}

//"/ls/map/all <id> <label> <k1..k16> <b1..b8>"
std::vector< uint8_t > MapMsg( const AbletonMap& m )
{
	const std::string t( 26, 's' );
	OscW w( "/ls/map/all", t.c_str() );
	w.S( m.id ).S( m.label );
	for( auto& k : m.knobs ) w.S( k );
	for( auto& b : m.buttons ) w.S( b );
	return w.b;
}

const char* BridgeParamType( ParamType t ) { return ParamTypeName( t ); }
const char* BridgeMidiType( MidiType t )
{
	switch( t )
	{
	case MidiType::Note: return "Note";
	case MidiType::PitchBend: return "PitchBend";
	case MidiType::CC14: return "CC14";
	default: return "CC";
	}
}
const char* BridgeTempoName( TempoSource t )
{
	switch( t )
	{
	case TempoSource::MidiClock: return "midi";
	case TempoSource::Link: return "link";
	default: return "internal";
	}
}
int BridgeOutValue( const Show& s, int ui, int a )
{
	if( s.fxAny && ui < (int)s.fxOut.size() && s.fxOut[ ui ][ a ] >= 0 ) return s.fxOut[ ui ][ a ];
	return s.dmx[ ui ][ a ];
}

Json BuildManifest( Show& s, uint32_t rev, const std::string& showName, const std::vector< std::string >* online = nullptr )
{
	Json j = Json::Obj();
	j.Set( "format", "grosserd-lightstage-manifest" );
	j.Set( "protocol", kProtocol );
	j.Set( "rev", (int)rev );
	j.Set( "app", "LightStage 1.x" );
	j.Set( "show", showName );
	j.Set( "showId", s.showId );
	Json osc = Json::Obj();
	osc.Set( "port", s.output.oscPort );
	j.Set( "osc", osc );
	Json tempo = Json::Obj();
	tempo.Set( "bpm", s.tempo.bpm );
	tempo.Set( "source", BridgeTempoName( s.tempo.source ) );
	tempo.Set( "beatsPerBar", s.tempo.beatsPerBar );
	j.Set( "tempo", tempo );

	Json unis = Json::Arr();
	for( auto& u : s.universes )
	{
		Json o = Json::Obj();
		o.Set( "num", u.num );
		o.Set( "name", u.name );
		unis.Push( o );
	}
	j.Set( "universes", unis );

	Json params = Json::Arr();
	for( const Param& p : Params( s ) )
	{
		Json o = Json::Obj();
		o.Set( "addr", p.addr );
		o.Set( "label", p.label );
		o.Set( "group", p.group );
		o.Set( "type", BridgeParamType( p.type ) );
		o.Set( "min", p.min );
		o.Set( "max", p.max );
		o.Set( "value", p.type == ParamType::Trigger ? 0.0f : p.get() );
		params.Push( o );
	}
	j.Set( "params", params );

	Json fixtures = Json::Arr();
	for( auto& f : s.fixtures )
	{
		Json o = Json::Obj();
		const Mode* m = f.M();
		o.Set( "id", f.id );
		o.Set( "name", f.name );
		o.Set( "type", f.def ? f.def->name : f.defId );
		o.Set( "manufacturer", f.def ? f.def->manufacturer : std::string() );
		o.Set( "kind", f.def ? KindName( f.def->kind ) : "" );
		o.Set( "universe", f.universe );
		o.Set( "address", f.address );
		o.Set( "footprint", m ? m->Footprint() : 0 );
		o.Set( "heads", f.def ? f.def->Heads() : 1 );
		Json chans = Json::Arr();
		if( m )
			for( int i = 0; i < (int)m->ch.size(); ++i )
			{
				const Channel& c = m->ch[ i ];
				Json cj          = Json::Obj();
				cj.Set( "slot", i + 1 );
				cj.Set( "dmx", f.address + i );
				cj.Set( "name", c.name );
				cj.Set( "attr", c.attr == Attr::None ? std::string() : std::string( AttrName( c.attr ) ) );
				cj.Set( "fine", c.isFine );
				cj.Set( "pixel", c.pixels.empty() ? 0 : c.pixels[ 0 ] + 1 );
				chans.Push( cj );
			}
		o.Set( "channels", chans );
		fixtures.Push( o );
	}
	j.Set( "fixtures", fixtures );

	Json effects = Json::Arr();
	for( auto& e : s.effects )
	{
		Json o = Json::Obj();
		o.Set( "id", e.id );
		o.Set( "name", e.name );
		o.Set( "kind", FxKindName( e.kind ) );
		o.Set( "on", e.on );
		effects.Push( o );
	}
	j.Set( "effects", effects );

	Json progs = Json::Arr();
	for( int i = 0; i < (int)s.programs.size(); ++i )
	{
		Json o = Json::Obj();
		o.Set( "index", i + 1 );
		o.Set( "name", s.programs[ i ].name );
		progs.Push( o );
	}
	j.Set( "programs", progs );

	Json snaps = Json::Arr();
	for( int i = 0; i < (int)s.snapshots.size(); ++i )
	{
		Json o = Json::Obj();
		o.Set( "index", i + 1 );
		o.Set( "name", s.snapshots[ i ].name );
		snaps.Push( o );
	}
	j.Set( "snapshots", snaps );

	Json targets = Json::Arr();
	for( const MidiTargetInfo& t : CollectMidiTargets( s ) )
	{
		Json o = Json::Obj();
		o.Set( "key", t.key );
		o.Set( "category", t.category );
		o.Set( "label", t.label );
		o.Set( "kind", t.kind );
		if( t.kind == "fixtureAttr" )
		{
			Json fx = Json::Arr();
			for( int id : t.fixtures ) fx.Push( Json( id ) );
			o.Set( "fixtures", fx );
			o.Set( "attr", t.attr );
			o.Set( "head", t.head );
		}
		if( t.kind == "dmx" )
		{
			o.Set( "universe", t.universe );
			o.Set( "channel", t.channel );
			o.Set( "sixteen", t.sixteen );
		}
		if( t.kind == "snapshot" ) o.Set( "snapshot", t.snapshot );
		o.Set( "addr", t.addr );
		o.Set( "type", t.type );
		o.Set( "min", t.min );
		o.Set( "max", t.max );
		o.Set( "value", t.value );
		targets.Push( o );
	}
	j.Set( "midiTargets", targets );

	//knob / button tables of the Control devices (both ways, see /ls/map/*)
	Json dm = Json::Arr();
	for( auto& m : s.ableton )
	{
		Json o = Json::Obj();
		o.Set( "id", m.id );
		o.Set( "label", m.label );
		o.Set( "kind", m.kind );
		bool on = false;
		if( online )
			for( auto& x : *online ) on = on || ( !m.id.empty() && x == m.id );
		o.Set( "online", on );
		Json k = Json::Arr(), b = Json::Arr();
		for( auto& x : m.knobs ) k.Push( Json( x ) );
		for( auto& x : m.buttons ) b.Push( Json( x ) );
		o.Set( "knobs", k );
		o.Set( "buttons", b );
		o.Set( "knobsNewer", m.knobsNewer );
		o.Set( "state", m.state );
		dm.Push( o );
	}
	j.Set( "deviceMaps", dm );

	Json maps = Json::Arr();
	for( auto& m : s.midi )
	{
		Json o = Json::Obj();
		o.Set( "device", m.device );
		o.Set( "channel", m.channel );
		o.Set( "type", BridgeMidiType( m.type ) );
		o.Set( "number", m.number );
		o.Set( "targetKey", MidiTargetKey( m ) );
		o.Set( "label", m.Describe( s ) );
		o.Set( "lo", m.lo );
		o.Set( "hi", m.hi );
		o.Set( "invert", m.invert );
		o.Set( "toggle", m.toggle );
		maps.Push( o );
	}
	j.Set( "midiMappings", maps );
	return j;
}
}//namespace

bool IsBridgeMessage( const std::string& a )
{
	return a == "/ls/hello" || a == "/ls/bye" || a == "/ls/norm" || a == "/ls/dmxblock" || a == "/ls/fixblock" || a == "/ls/release" || a == "/ls/map/set" || a == "/ls/map/sync" ||
	       a == "/ls/map/bind" || a == "/ls/map/state" || a == "/ls/map/put";
}

bool BridgeClient::Has( const std::string& k ) const
{
	size_t p = 0;
	while( p <= kinds.size() )
	{
		size_t q = kinds.find( ',', p );
		if( q == std::string::npos ) q = kinds.size();
		std::string t = kinds.substr( p, q - p );
		t.erase( std::remove( t.begin(), t.end(), ' ' ), t.end() );
		if( t == k ) return true;
		p = q + 1;
	}
	return false;
}
std::string BridgeClient::Addr() const { return KeyAddr( ip, port ); }

//------------------------------------------------------------------ network
bool Bridge::Start( int port, std::string* err, bool wait )
{
	Stop();
	error.clear();
	WSADATA wd;
	WSAStartup( MAKEWORD( 2, 2 ), &wd );
	SOCKET o = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );//replies: plain UDP socket, no listen
	SOCKET l = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
	if( l == INVALID_SOCKET || o == INVALID_SOCKET )
	{
		if( l != INVALID_SOCKET ) closesocket( l );
		if( o != INVALID_SOCKET ) closesocket( o );
		error = L( "Netzwerk nicht verfügbar" );
		if( err ) *err = error;
		return false;
	}
	out_      = (uintptr_t)o;
	listen_   = (uintptr_t)l;
	httpPort_ = port;
	ready_    = false;
	failed_   = false;
	run_      = true;
	http_     = std::thread( [ this, l, port ] {
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_port        = htons( (u_short)port );
        a.sin_addr.s_addr = htonl( INADDR_ANY );
        const double t0   = Now();
        if( bind( l, (sockaddr*)&a, sizeof( a ) ) != 0 || listen( l, 8 ) != 0 )
        {
            if( run_ )
            {
                std::lock_guard< std::mutex > g( mx_ );
                error = L( "HTTP-Port " ) + std::to_string( port ) + L( " ist belegt" );
            }
            failed_ = true;
            return;
        }
        if( Now() - t0 > 0.5 ) Log( "bridge: listen() dauerte %.1f s (Firewall/Netzwerkfilter)", Now() - t0 );
        ready_ = true;
        Log( "bridge: http on %d", port );
        HttpLoop();
	} );
	if( wait )
	{
		while( !ready_ && !failed_ ) Sleep( 5 );
		if( failed_ )
		{
			if( err ) *err = error;
			Stop();
			return false;
		}
	}
	return true;
}

void Bridge::Stop()
{
	if( !run_ && listen_ == ~(uintptr_t)0 && out_ == ~(uintptr_t)0 ) return;
	run_ = false;
	//close first: this also ends a listen() that is still held by a network filter
	const uintptr_t l = listen_.exchange( ~(uintptr_t)0 );
	if( l != ~(uintptr_t)0 ) closesocket( (SOCKET)l );
	if( http_.joinable() ) http_.join();
	if( out_ != ~(uintptr_t)0 ) closesocket( (SOCKET)out_ );
	out_      = ~(uintptr_t)0;
	httpPort_ = 0;
	ready_    = false;
	failed_   = false;//else the caller sees the old failure again on every frame
}

void Bridge::HttpLoop()
{
	while( run_ )
	{
		fd_set rs;
		FD_ZERO( &rs );
		FD_SET( (SOCKET)listen_, &rs );
		timeval tv{ 0, 200000 };
		if( select( 0, &rs, nullptr, nullptr, &tv ) <= 0 ) continue;
		SOCKET c = accept( (SOCKET)listen_, nullptr, nullptr );
		if( c == INVALID_SOCKET ) continue;
		DWORD tmo = 2000;
		setsockopt( c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof( tmo ) );
		char buf[ 2048 ];
		int n         = recv( c, buf, sizeof( buf ) - 1, 0 );
		std::string req = n > 0 ? std::string( buf, n ) : std::string();
		bool get        = req.compare( 0, 4, "GET " ) == 0;
		std::string path;
		if( get )
		{
			size_t e = req.find( ' ', 4 );
			path     = req.substr( 4, e == std::string::npos ? std::string::npos : e - 4 );
			size_t q = path.find( '?' );
			if( q != std::string::npos ) path.resize( q );
		}
		int code = 200;
		std::string type = "text/plain; charset=utf-8", body;
		{
			std::lock_guard< std::mutex > l( mx_ );
			if( !get ) { code = 405; body = "GET only"; }
			else if( path == "/manifest.json" ) { type = "application/json; charset=utf-8"; body = manifestCache_; }
			else if( path == "/osc.txt" ) body = oscTxtCache_;
			else if( path == "/" ) { type = "text/html; charset=utf-8"; body = pageCache_; }
			else { code = 404; body = "not found"; }
		}
		const char* reason = code == 200 ? "OK" : code == 404 ? "Not Found" : "Method Not Allowed";
		std::string head   = "HTTP/1.1 " + std::to_string( code ) + " " + reason + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string( body.size() ) +
		                   "\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
		head += body;
		size_t sentBytes = 0;
		while( sentBytes < head.size() )
		{
			int k = send( c, head.data() + sentBytes, (int)( head.size() - sentBytes ), 0 );
			if( k <= 0 ) break;
			sentBytes += k;
		}
		closesocket( c );
	}
}

void Bridge::SendRaw( const BridgeClient& c, const std::vector< uint8_t >& msg )
{
	if( out_ == ~(uintptr_t)0 ) return;
	sockaddr_in to{};
	to.sin_family      = AF_INET;
	to.sin_addr.s_addr = c.ip;
	to.sin_port        = htons( c.port );
	sendto( (SOCKET)out_, (const char*)msg.data(), (int)msg.size(), 0, (sockaddr*)&to, sizeof( to ) );
	++sent;
}

//packs messages into OSC bundles of at most kMaxPacket bytes
void Bridge::SendBundled( const BridgeClient& c, const std::vector< std::vector< uint8_t > >& msgs )
{
	size_t i = 0;
	while( i < msgs.size() )
	{
		std::vector< uint8_t > pk = { '#', 'b', 'u', 'n', 'd', 'l', 'e', 0, 0, 0, 0, 0, 0, 0, 0, 1 };
		size_t j                  = i;
		while( j < msgs.size() && ( j == i || pk.size() + 4 + msgs[ j ].size() <= kMaxPacket ) )
		{
			uint32_t len = (uint32_t)msgs[ j ].size();
			pk.push_back( (uint8_t)( len >> 24 ) );
			pk.push_back( (uint8_t)( ( len >> 16 ) & 255 ) );
			pk.push_back( (uint8_t)( ( len >> 8 ) & 255 ) );
			pk.push_back( (uint8_t)( len & 255 ) );
			pk.insert( pk.end(), msgs[ j ].begin(), msgs[ j ].end() );
			++j;
		}
		SendRaw( c, pk );
		i = j;
	}
}

//------------------------------------------------------------------ clients + ownership
BridgeClient* Bridge::FindLocked( uint32_t ip, uint16_t )
{
	for( auto& c : clients_ )
		if( c.ip == ip ) return &c;
	return nullptr;
}

void Bridge::WriteLocked( Show& show, int u, int ch, uint8_t v, const std::string& owner )
{
	DmxFrame* fr = show.Frame( u );
	if( !fr || ch < 1 || ch > 512 ) return;
	const int64_t key = (int64_t)u * 1000 + ch;
	auto it           = owned_.find( key );
	if( it == owned_.end() ) owned_[ key ] = Owned{ owner, ( *fr )[ ch - 1 ] };
	else it->second.owner = owner;
	( *fr )[ ch - 1 ] = v;
}

void Bridge::ReleaseLocked( Show& show, const std::string& owner )
{
	bool any = false;
	for( auto it = owned_.begin(); it != owned_.end(); )
	{
		if( it->second.owner != owner ) { ++it; continue; }
		const int u = (int)( it->first / 1000 ), ch = (int)( it->first % 1000 );
		if( DmxFrame* fr = show.Frame( u ) ) ( *fr )[ ch - 1 ] = it->second.saved;
		any = true;
		it  = owned_.erase( it );
	}
	if( any ) show.Bump();
}

void Bridge::OnOsc( const OscMsg& m, Show& show )
{
	std::lock_guard< std::mutex > l( mx_ );
	++received;
	const std::string& a = m.address;
	BridgeClient* c      = FindLocked( m.fromIp, m.fromPort );
	const std::string owner = c ? c->id : KeyAddr( m.fromIp, m.fromPort );

	if( a == "/ls/hello" )
	{
		if( m.strs.size() < 3 || m.f.size() < 2 || (int)m.f[ 0 ] != kProtocol ) return;
		const int replyPort = (int)m.f[ 1 ];
		BridgeClient* k     = nullptr;
		for( auto& x : clients_ )
			if( x.id == m.strs[ 1 ] ) k = &x;
		if( !k )
		{
			clients_.push_back( BridgeClient() );
			k = &clients_.back();
			Log( "bridge: device %s (%s) connected", m.strs[ 0 ].c_str(), m.strs[ 1 ].c_str() );
			cacheDirty_ = true;//"online" in the manifest
		}
		k->name = m.strs[ 0 ];
		k->id   = m.strs[ 1 ];
		k->kinds = m.strs[ 2 ];
		k->ip    = m.fromIp;
		k->port  = (uint16_t)replyPort;
		k->lastSeen = Now();
		k->localIp  = LocalIpFor( m.fromIp, (uint16_t)replyPort );
		OscW w( "/ls/info", "issiis" );
		w.I( kProtocol ).S( "LightStage 1.x" ).S( showName_ ).I( (int)rev_ ).I( httpPort_ ).S( k->localIp );
		SendRaw( *k, w.b );
		return;
	}
	if( a == "/ls/bye" )
	{
		if( m.strs.empty() ) return;
		for( auto it = clients_.begin(); it != clients_.end(); ++it )
			if( it->id == m.strs[ 0 ] ) { Log( "bridge: device %s left", it->name.c_str() ); clients_.erase( it ); cacheDirty_ = true; break; }
		return;
	}
	if( a == "/ls/map/bind" )//<id> <kind> <label> <put>: device binds to a named table (created when missing)
	{
		if( m.strs.size() < 3 || m.strs[ 0 ].empty() || m.strs[ 2 ].empty() ) return;
		const std::string& id = m.strs[ 0 ];
		std::string kind      = m.strs[ 1 ];
		if( kind != "dmx" && kind != "cues" ) kind = "control";
		const bool put = !m.f.empty() && m.f[ 0 ] > 0.5f;
		for( auto& t : show.ableton )//one table per device instance
			if( t.id == id && !( t.kind == kind && t.label == m.strs[ 2 ] ) ) t.id.clear();
		AbletonMap* t = show.FindTable( kind, m.strs[ 2 ] );
		bool changed  = false;
		if( !t )
		{
			AbletonMap n;
			n.kind  = kind;
			n.label = m.strs[ 2 ];
			show.ableton.push_back( n );
			t       = &show.ableton.back();
			changed = true;
		}
		if( t->id != id ) { t->id = id; changed = true; }
		if( put && t->knobsNewer ) { t->knobsNewer = false; changed = true; }
		if( changed ) show.dirty = true;//only a real change marks the show as unsaved
		cacheDirty_ = true;
		Log( "bridge: %s bound to %s \"%s\"%s", id.c_str(), kind.c_str(), t->label.c_str(), put ? " (device state)" : "" );
		return;
	}
	if( a == "/ls/map/state" )//<id> <seq> <part> <count> <chunk>: complete assignment state of a device, in pieces
	{
		if( m.strs.size() < 2 || m.f.size() < 3 ) return;
		const std::string& id = m.strs[ 0 ];
		const int seq = (int)m.f[ 0 ], part = (int)m.f[ 1 ], count = (int)m.f[ 2 ];
		if( count < 1 || count > 512 || part < 1 || part > count ) return;
		StateIn& in = stateIn_[ id ];
		if( in.seq != seq || in.count != count )
		{
			in.seq   = seq;
			in.count = count;
			in.got   = 0;
			in.parts.assign( count, std::string( 1, '\x01' ) );//\x01 = not received yet
		}
		if( in.parts[ part - 1 ] == std::string( 1, '\x01' ) ) ++in.got;
		in.parts[ part - 1 ] = m.strs[ 1 ];
		if( in.got < in.count ) return;
		std::string all;
		for( auto& p : in.parts ) all += p;
		stateIn_.erase( id );
		AbletonMap* t = show.FindAbleton( id );
		if( !t ) return;//not bound
		if( t->state != all || t->knobsNewer )
		{
			t->state      = all;
			t->knobsNewer = false;
			show.dirty    = true;
			cacheDirty_   = true;
		}
		return;
	}
	if( a == "/ls/map/put" )//<id> <k1..k16> <b1..b8>: knob table from the device replaces ours
	{
		if( m.strs.size() < 25 ) return;
		AbletonMap* t = show.FindAbleton( m.strs[ 0 ] );
		if( !t ) return;
		AbletonMap n = *t;
		for( int i = 0; i < 16; ++i ) n.knobs[ i ] = m.strs[ 1 + i ];
		for( int i = 0; i < 8; ++i ) n.buttons[ i ] = m.strs[ 17 + i ];
		if( n.knobs != t->knobs || n.buttons != t->buttons || t->knobsNewer )
		{
			t->knobs      = n.knobs;
			t->buttons    = n.buttons;
			t->knobsNewer = false;
			show.dirty    = true;
		}
		return;
	}
	if( a == "/ls/map/set" || a == "/ls/map/sync" )
	{
		if( m.strs.empty() || m.strs[ 0 ].empty() ) return;
		const std::string id = m.strs[ 0 ];
		AbletonMap* am       = show.FindAbleton( id );
		auto create          = [ & ] {
            AbletonMap n;
            n.id = id;
            int k = 1;
            for( bool taken = true; taken; )
            {
                n.label = "Control " + std::to_string( k++ );
                taken   = false;
                for( auto& x : show.ableton ) taken = taken || x.label == n.label;
            }
            show.ableton.push_back( n );
            return &show.ableton.back();
		};
		if( a == "/ls/map/set" )//<id> <k|b> <slot 1..> <key> : one slot changed in the device
		{
			if( m.strs.size() < 3 || m.f.empty() ) return;
			const bool knob = m.strs[ 1 ] != "b";
			const int slot  = (int)m.f[ 0 ] - 1;
			if( slot < 0 || slot >= ( knob ? 16 : 8 ) ) return;
			if( !am ) am = create();
			std::string& dst = knob ? am->knobs[ slot ] : am->buttons[ slot ];
			if( dst != m.strs[ 2 ] )
			{
				dst        = m.strs[ 2 ];
				show.dirty = true;
			}
			return;
		}
		//sync <id> <k1..k16> <b1..b8> after connecting: LightStage keeps its table if it has one, else takes the device's
		if( m.strs.size() < 25 ) return;
		if( am && !am->Empty() )
		{
			for( auto& x : clients_ )
				if( x.id == id ) SendRaw( x, MapMsg( *am ) );
			return;
		}
		AbletonMap in;
		for( int i = 0; i < 16; ++i ) in.knobs[ i ] = m.strs[ 1 + i ];
		for( int i = 0; i < 8; ++i ) in.buttons[ i ] = m.strs[ 17 + i ];
		if( in.Empty() ) return;//nothing on either side
		if( !am ) am = create();
		am->knobs   = in.knobs;
		am->buttons = in.buttons;
		show.dirty  = true;
		return;
	}
	if( a == "/ls/norm" )
	{
		if( m.strs.empty() || m.f.empty() ) return;
		const std::string addr = ToLower( m.strs[ 0 ] );
		SetParamNorm( show, addr, m.f[ 0 ] );
		if( c )
			if( const Param* p = FindParam( show, addr ) ) c->sentParam[ addr ] = p->get();//no echo to the sender
		return;
	}
	if( a == "/ls/dmxblock" )
	{
		if( m.f.size() < 3 ) return;
		const int u = (int)m.f[ 0 ], start = (int)m.f[ 1 ];
		{
			std::lock_guard< std::mutex > dl( show.dmxLock );
			if( !show.Frame( u ) ) return;
			for( size_t k = 2; k < m.f.size(); ++k )
			{
				const int ch = start + (int)( k - 2 );
				if( ch < 1 || ch > 512 ) continue;
				WriteLocked( show, u, ch, (uint8_t)Clamp( (int)lroundf( m.f[ k ] ), 0, 255 ), owner );
			}
			show.Bump();
			if( c )
			{
				const int ui = show.UniIndex( u );
				auto it      = c->sentDmx.find( u );
				if( it == c->sentDmx.end() )
				{
					std::array< int, 512 > arr;
					arr.fill( -1 );
					it = c->sentDmx.emplace( u, arr ).first;
				}
				for( size_t k = 2; k < m.f.size(); ++k )
				{
					const int ch = start + (int)( k - 2 );
					if( ch >= 1 && ch <= 512 && ui >= 0 ) it->second[ ch - 1 ] = BridgeOutValue( show, ui, ch - 1 );
				}
			}
		}
		return;
	}
	if( a == "/ls/fixblock" )
	{
		if( m.f.size() < 3 ) return;
		FixtureInst* fi = show.FindFixture( (int)m.f[ 0 ] );
		if( !fi || !fi->M() ) return;
		const int start = (int)m.f[ 1 ];
		std::lock_guard< std::mutex > dl( show.dmxLock );
		for( size_t k = 2; k < m.f.size(); ++k )
		{
			const int slot = start - 1 + (int)( k - 2 );
			if( slot < 0 || slot >= fi->M()->Footprint() ) continue;
			WriteLocked( show, fi->universe, fi->address + slot, (uint8_t)Clamp( (int)lroundf( m.f[ k ] ), 0, 255 ), owner );
		}
		show.Bump();
		return;
	}
	if( a == "/ls/release" )
	{
		if( m.strs.empty() ) return;
		std::lock_guard< std::mutex > dl( show.dmxLock );
		ReleaseLocked( show, m.strs[ 0 ] );
		return;
	}
}

//------------------------------------------------------------------ per frame
void Bridge::RebuildCaches( Show& show )
{
	std::vector< std::string > online;
	for( auto& c : clients_ ) online.push_back( c.id );
	manifestCache_ = WriteJson( BuildManifest( show, rev_, showName_, &online ), false );
	std::string txt = L( "# LightStage OSC-Adressen, Protokoll " ) + std::to_string( kProtocol ) + ", Manifest-Rev " + std::to_string( rev_ ) + "\n";
	for( const Param& p : Params( show ) )
	{
		char line[ 512 ];
		snprintf( line, sizeof( line ), "%s\t%s\t%s\n", p.addr.c_str(), p.label.c_str(), ParamRange( p ).c_str() );
		txt += line;
	}
	oscTxtCache_ = txt;
}

void Bridge::Tick( Show& show, double now )
{
	if( now - lastTick_ < kTickPeriod ) return;
	lastTick_ = now;
	std::lock_guard< std::mutex > l( mx_ );
	{
		std::string nm = show.path;
		const size_t sl = nm.find_last_of( "\\/" );
		if( sl != std::string::npos ) nm = nm.substr( sl + 1 );
		const size_t dot = nm.rfind( '.' );
		if( dot != std::string::npos && dot > 0 ) nm.resize( dot );
		if( nm.empty() ) nm = L( "Unbenannt" );
		if( nm != showName_ ) { showName_ = nm; sigInit_ = false; }//rename: manifest is rebuilt
	}

	//timeouts (ownership stays until /ls/release)
	for( auto it = clients_.begin(); it != clients_.end(); )
	{
		if( now - it->lastSeen > kTimeout )
		{
			Log( "bridge: device %s timed out", it->name.c_str() );
			it = clients_.erase( it );
			cacheDirty_ = true;
		}
		else
			++it;
	}

	//structure revision: manifest is rebuilt and the devices are told
	const std::string sig = StructSig( show );
	const size_t h        = std::hash< std::string >()( sig );
	if( !sigInit_ || h != sigHash_ )
	{
		sigInit_ = true;
		sigHash_ = h;
		++rev_;
		RebuildCaches( show );
		cacheDirty_ = false;
		for( auto& c : clients_ ) SendRaw( c, OscW( "/ls/manifest/changed", "i" ).I( (int)rev_ ).b );
	}
	else if( cacheDirty_ )//content only (device state, online): same revision
	{
		cacheDirty_ = false;
		RebuildCaches( show );
	}
	if( now - lastPage_ > 1.0 || pageCache_.empty() )
	{
		lastPage_ = now;
		std::string html = "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>LightStage Bridge</title>"
		                   "<style>body{font:15px system-ui;margin:24px;background:#111;color:#ddd}a{color:#8cf}td,th{padding:4px 12px;text-align:left}</style>";
		html += "<h2>LightStage Ableton-Bridge</h2><p>Show: " + showName_ + " · Manifest-Rev " + std::to_string( rev_ ) + L( " · Protokoll " ) + std::to_string( kProtocol ) +
		        L( "</p><p><a href='/manifest.json'>manifest.json</a> · <a href='/osc.txt'>osc.txt</a></p><table><tr><th>Gerät</th><th>Adresse</th><th>Abos</th><th>vor</th></tr>" );
		for( auto& c : clients_ )
		{
			char ago[ 32 ];
			snprintf( ago, sizeof( ago ), "%.1f s", now - c.lastSeen );
			html += "<tr><td>" + c.name + "</td><td>" + c.Addr() + "</td><td>" + c.kinds + "</td><td>" + ago + "</td></tr>";
		}
		if( clients_.empty() ) html += L( "<tr><td colspan=4>keine Geräte verbunden</td></tr>" );
		html += "</table>";
		pageCache_ = html;
	}

	//feedback per device
	for( auto& c : clients_ )
	{
		std::vector< std::vector< uint8_t > > msgs;
		if( c.Has( "params" ) )
		{
			for( const Param& p : Params( show ) )
			{
				if( p.type == ParamType::Trigger ) continue;
				const float v = p.get();
				auto it       = c.sentParam.find( p.addr );
				if( it != c.sentParam.end() && fabsf( it->second - v ) < 1e-4f ) continue;
				c.sentParam[ p.addr ] = v;
				const float n = p.max > p.min ? Saturate( ( v - p.min ) / ( p.max - p.min ) ) : 0.0f;
				msgs.push_back( OscW( "/ls/fb", "sff" ).S( p.addr ).F( v ).F( n ).b );
			}
		}
		if( c.Has( "dmx" ) )
		{
			std::lock_guard< std::mutex > dl( show.dmxLock );
			for( int ui = 0; ui < (int)show.dmx.size(); ++ui )
			{
				const int u = show.universes[ ui ].num;
				auto it     = c.sentDmx.find( u );
				if( it == c.sentDmx.end() )
				{
					std::array< int, 512 > arr;
					arr.fill( -1 );
					it = c.sentDmx.emplace( u, arr ).first;
				}
				auto& sentArr = it->second;
				int a         = 0;
				while( a < 512 )
				{
					if( BridgeOutValue( show, ui, a ) == sentArr[ a ] ) { ++a; continue; }
					const int start = a;
					std::vector< int > vals;
					while( a < 512 && (int)vals.size() < kMaxDmxPerMsg )
					{
						const int o = BridgeOutValue( show, ui, a );
						if( o == sentArr[ a ] ) break;
						sentArr[ a ] = o;
						vals.push_back( o );
						++a;
					}
					OscW w( "/ls/fbdmx", "ii" + std::string( vals.size(), 'i' ) );
					w.I( u ).I( start + 1 );
					for( int v : vals ) w.I( v );
					msgs.push_back( w.b );
				}
			}
		}
		if( c.Has( "state" ) )
		{
			for( auto& e : show.effects )
			{
				const int on = e.on ? 1 : 0;
				auto it      = c.sentEffect.find( e.id );
				if( it != c.sentEffect.end() && it->second == on ) continue;
				c.sentEffect[ e.id ] = on;
				msgs.push_back( OscW( "/ls/state/effect", "ii" ).I( e.id ).I( on ).b );
			}
			const int prog = ActiveProgram( show ) + 1;
			if( prog != c.sentProgram )
			{
				c.sentProgram = prog;
				msgs.push_back( OscW( "/ls/state/program", "i" ).I( prog ).b );
			}
		}
		if( !msgs.empty() ) SendBundled( c, msgs );
	}
	if( now - lastState_ >= 0.5 )
	{
		lastState_ = now;
		for( auto& c : clients_ )
			if( c.Has( "state" ) )
			{
				OscW w( "/ls/state/tempo", "ffi" );
				w.F( TempoBpm( show.tempo ) ).F( (float)show.tempo.beat ).I( show.tempo.playing ? 1 : 0 );
				SendRaw( c, w.b );
			}
	}
}

int Bridge::SendAssign( const Param& p )
{
	std::lock_guard< std::mutex > l( mx_ );
	int n = 0;
	for( auto& c : clients_ )
		if( c.Has( "params" ) )
		{
			OscW w( "/ls/assign", "ssffs" );
			w.S( p.addr ).S( p.label ).F( p.min ).F( p.max ).S( ParamTypeName( p.type ) );
			SendRaw( c, w.b );
			++n;
		}
	return n;
}

int Bridge::PushMap( const AbletonMap& am )
{
	std::lock_guard< std::mutex > l( mx_ );
	int n = 0;
	for( auto& c : clients_ )
		if( c.id == am.id )
		{
			SendRaw( c, MapMsg( am ) );
			++n;
		}
	return n;
}

int Bridge::SendAssignFixture( int id )
{
	std::lock_guard< std::mutex > l( mx_ );
	int n = 0;
	for( auto& c : clients_ )
		if( c.Has( "dmx" ) )
		{
			SendRaw( c, OscW( "/ls/assignfixture", "i" ).I( id ).b );
			++n;
		}
	return n;
}

int Bridge::Subscribers( const std::string& kind )
{
	std::lock_guard< std::mutex > l( mx_ );
	int n = 0;
	for( auto& c : clients_ )
		if( c.Has( kind ) ) ++n;
	return n;
}

std::string Bridge::ManifestJson( Show& show )
{
	return WriteJson( BuildManifest( show, rev_, showName_ ), true );
}

std::vector< BridgeClient > Bridge::Clients()
{
	std::lock_guard< std::mutex > l( mx_ );
	return clients_;
}
}//namespace ls
