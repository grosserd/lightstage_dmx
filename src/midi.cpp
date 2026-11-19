#include "platform.h"
#include "midi.h"
#include "params.h"
#include <thread>

namespace ls
{
void MidiIn::OnShort( int dev, uint32_t msg, double t )
{
	std::lock_guard< std::mutex > l( lock_ );
	if( queue_.size() < 8192 ) queue_.push_back( { dev, msg, t } );
	if( wake )
	{
		if( wakeFn ) wakeFn( wake );
		else EventSignal( wake );
	}
}
void MidiIn::OnSysEx( int, const uint8_t*, size_t, double )
{
	//SysEx input is not mapped (and never delivered by the winmm backend)
}

void MidiIn::Open()
{
	Close();
	const auto names = midiport::InputNames();
	for( int i = 0; i < (int)names.size(); ++i )
	{
		if( void* h = midiport::OpenInput( i, (int)devices.size(), this ) )
		{
			handles_.push_back( h );
			devices.push_back( names[ i ] );
			Log( "midi in: opened %s", names[ i ].c_str() );
		}
		else
		{
			failed.push_back( names[ i ] );
			Log( "midi in: cannot open %s (in use?)", names[ i ].c_str() );
		}
	}
}
void MidiIn::Close()
{
	for( void* h : handles_ ) midiport::CloseInput( h );
	handles_.clear();
	devices.clear();
	failed.clear();
	std::lock_guard< std::mutex > l( lock_ );
	queue_.clear();
}

void MidiIn::Poll( std::vector< MidiEvent >& out )
{
	std::vector< Raw > q;
	{
		std::lock_guard< std::mutex > l( lock_ );
		q.swap( queue_ );
	}
	for( auto& r : q )
	{
		uint8_t st = r.msg & 0xff, d1 = ( r.msg >> 8 ) & 0x7f, d2 = ( r.msg >> 16 ) & 0x7f;
		//MIDI timecode quarter frame: 8 pieces = hh:mm:ss:ff (+ rate); the complete time is 2 frames old
		if( st == 0xF1 )
		{
			const int piece      = ( d1 >> 4 ) & 7;
			mtcPiece_[ piece ]   = d1 & 15;
			if( piece == 0 && mtcLast_ != 7 ) mtcGot_ = 0;//new run (or backwards: restart)
			mtcGot_ |= 1 << piece;
			mtcLast_ = piece;
			if( piece == 7 && mtcGot_ == 0xFF )
			{
				static const int rates[ 4 ] = { 24, 25, 30, 30 };
				const int rate = ( mtcPiece_[ 7 ] >> 1 ) & 3;
				const int ff = mtcPiece_[ 0 ] | ( mtcPiece_[ 1 ] << 4 ), ss = mtcPiece_[ 2 ] | ( mtcPiece_[ 3 ] << 4 ), mm = mtcPiece_[ 4 ] | ( mtcPiece_[ 5 ] << 4 ), hh = mtcPiece_[ 6 ] | ( ( mtcPiece_[ 7 ] & 1 ) << 4 );
				mtcFps    = rates[ rate ];
				const double fr = rate == 2 ? 29.97 : (double)mtcFps;
				mtcSec    = hh * 3600.0 + mm * 60.0 + ss + ( ff + 2 ) / fr;
				mtcAt     = r.t > 0 ? r.t : Now();
				mtcDevice = r.dev < (int)devices.size() ? devices[ r.dev ] : "?";
				mtcGot_   = 0;
			}
			continue;
		}
		//system real time / song position: tempo clock, not mapped
		if( st >= 0xF8 || st == 0xF2 )
		{
			if( st == 0xF8 || st == 0xFA || st == 0xFB || st == 0xFC || st == 0xF2 )
			{
				const std::string dn = r.dev < (int)devices.size() ? devices[ r.dev ] : "?";
				clock.push_back( { dn, st, st == 0xF2 ? ( d1 | ( d2 << 7 ) ) : 0, r.t > 0 ? r.t : Now() } );
				lastClock[ dn ] = Now();
				if( st != 0xF8 )
				{
					static const char* nm[] = { "Start", "Continue", "Stop" };
					char line[ 120 ];
					if( st == 0xF2 ) snprintf( line, sizeof( line ), "%-22.22s Song Position %d", dn.c_str(), d1 | ( d2 << 7 ) );
					else snprintf( line, sizeof( line ), "%-22.22s Clock %s", dn.c_str(), nm[ st - 0xFA ] );
					monitor.push_back( line );
					while( monitor.size() > 200 ) monitor.pop_front();
				}
			}
			continue;
		}
		int type = st & 0xf0, ch = st & 0x0f;
		MidiEvent e;
		e.device  = r.dev < (int)devices.size() ? devices[ r.dev ] : "?";
		e.t       = r.t > 0 ? r.t : Now();
		lastActivity[ e.device ] = Now();
		e.channel = ch;
		char line[ 160 ];
		++received;
		if( type == 0xB0 )
		{
			e.type   = MidiType::CC;
			e.number = d1;
			e.raw    = d2;
			e.value  = d2 / 127.0f;
			out.push_back( e );
			auto& tab = ccState_[ r.dev * 16 + ch ];
			tab[ d1 ] = d2;
			if( d1 < 64 )
			{
				int msb = d1 < 32 ? d1 : d1 - 32;
				MidiEvent e14 = e;
				e14.type      = MidiType::CC14;
				e14.number    = msb;
				e14.raw       = ( tab[ msb ] << 7 ) | tab[ msb + 32 ];
				//when only the MSB moved, LSB is stale: reset it so the value follows coarse controllers
				if( d1 < 32 ) { tab[ msb + 32 ] = 0; e14.raw = tab[ msb ] << 7; }
				e14.value = e14.raw / 16383.0f;
				out.push_back( e14 );
			}
			snprintf( line, sizeof( line ), "%-22.22s Ch%-2d CC %3d = %3d", e.device.c_str(), ch + 1, d1, d2 );
		}
		else if( type == 0x90 || type == 0x80 )
		{
			e.type   = MidiType::Note;
			e.number = d1;
			e.noteOn = type == 0x90 && d2 > 0;
			e.raw    = e.noteOn ? d2 : 0;
			e.value  = e.raw / 127.0f;
			out.push_back( e );
			snprintf( line, sizeof( line ), "%-22.22s Ch%-2d Note %3d %s %3d", e.device.c_str(), ch + 1, d1, e.noteOn ? "on " : "off", d2 );
		}
		else if( type == 0xE0 )
		{
			e.type   = MidiType::PitchBend;
			e.number = 0;
			e.raw    = ( d2 << 7 ) | d1;
			e.value  = e.raw / 16383.0f;
			out.push_back( e );
			snprintf( line, sizeof( line ), "%-22.22s Ch%-2d PitchBend %5d", e.device.c_str(), ch + 1, e.raw );
		}
		else
			continue;
		monitor.push_back( line );
		while( monitor.size() > 200 ) monitor.pop_front();
	}
}

std::vector< std::string > MidiIn::OutputPorts()
{
	return midiport::OutputNames();
}
static int FindOutput( const std::string& sub )
{
	const auto names = midiport::OutputNames();
	for( int i = 0; i < (int)names.size(); ++i )
		if( ToLower( names[ i ] ).find( ToLower( sub ) ) != std::string::npos ) return i;
	return -1;
}
bool MidiIn::SendShort( const std::string& sub, uint8_t st, uint8_t d1, uint8_t d2 )
{
	const int i = FindOutput( sub );
	if( i < 0 ) return false;
	void* h = midiport::OpenOutput( i );
	if( !h ) return false;
	midiport::SendShort( h, st, d1, d2 );
	std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
	midiport::CloseOutput( h );
	return true;
}
bool MidiIn::SendSysEx( const std::string& sub, const std::vector< uint8_t >& data )
{
	const int i = FindOutput( sub );
	if( i < 0 ) return false;
	void* h = midiport::OpenOutput( i );
	if( !h ) return false;
	const bool ok = midiport::SendSysEx( h, data.data(), data.size() );
	midiport::CloseOutput( h );
	return ok;
}

bool MidiOut::Open( const std::string& sub )
{
	Close();
	const auto names = midiport::OutputNames();
	for( int i = 0; i < (int)names.size() && !h_; ++i )
		if( ToLower( names[ i ] ).find( ToLower( sub ) ) != std::string::npos && ( h_ = midiport::OpenOutput( i ) ) ) name = names[ i ];
	return h_ != nullptr;
}
void MidiOut::Close()
{
	if( h_ ) midiport::CloseOutput( h_ );
	h_ = nullptr;
	name.clear();
}
bool MidiOut::Send( uint8_t st, uint8_t d1, uint8_t d2 ) { return midiport::SendShort( h_, st, d1, d2 ); }
bool MidiOut::SendSysEx( const std::vector< uint8_t >& data ) { return midiport::SendSysEx( h_, data.data(), data.size() ); }

bool MidiMatches( const MidiMap& m, const MidiEvent& e )
{
	if( m.type != e.type ) return false;
	if( m.channel >= 0 && m.channel != e.channel ) return false;
	if( m.type != MidiType::PitchBend && m.number != e.number ) return false;
	if( !m.device.empty() && m.device != e.device ) return false;
	return true;
}

int ApplyMidi( Show& show, const MidiEvent& e )
{
	int hits = 0;
	for( auto& m : show.midi )
	{
		if( !MidiMatches( m, e ) ) continue;
		++hits;
		if( m.target == MidiTarget::Snapshot )
		{
			//press (note on / CC above half): start, or with "Umschalten" start/stop
			const bool press = e.type == MidiType::Note ? e.noteOn : e.value > 0.5f;
			const bool was   = m.last > 0.5f;
			m.last           = press ? 1.0f : 0.0f;
			if( press && !was )
			{
				if( m.toggle ) show.ToggleSnapshot( m.snapshot );
				else show.RecallSnapshot( m.snapshot );
			}
			continue;
		}
		float v = e.value;
		if( m.toggle )
		{
			bool press = e.type == MidiType::Note ? e.noteOn : v > 0.5f;
			bool was   = m.last > 0.5f;
			if( press && !( e.type != MidiType::Note && was ) ) m.toggled = !m.toggled;
			m.last = e.type == MidiType::Note ? ( e.noteOn ? 1.0f : 0.0f ) : v;
			v      = m.toggled ? 1.0f : 0.0f;
		}
		else
			m.last = v;
		if( m.invert ) v = 1 - v;
		v = Lerp( m.lo, m.hi, v );
		switch( m.target )
		{
		case MidiTarget::FixtureAttr:
			for( int id : m.fixtures )
				if( FixtureInst* f = show.FindFixture( id ) )
					show.SetAttr( *f, m.attr, v, m.head );
			break;
		case MidiTarget::DmxChannel:
		{
			DmxFrame* fr = show.Frame( m.universe );
			if( !fr ) break;
			int a = Clamp( m.channelDmx, 1, 512 ) - 1;
			std::lock_guard< std::mutex > l( show.dmxLock );
			if( m.sixteen && a < 511 )
			{
				uint16_t x       = (uint16_t)( Saturate( v ) * 65535 );
				( *fr )[ a ]     = (uint8_t)( x >> 8 );
				( *fr )[ a + 1 ] = (uint8_t)( x & 255 );
			}
			else
				( *fr )[ a ] = (uint8_t)( Saturate( v ) * 255 + 0.5f );
			show.Bump();
			break;
		}
		case MidiTarget::Snapshot: break;//handled above
		case MidiTarget::Haze: show.stage.haze = Saturate( v ); break;
		case MidiTarget::Master: show.master = Clamp( v, 0.0f, 10.0f ); break;
		case MidiTarget::Param: SetParamNorm( show, m.param, v ); break;
		}
	}
	return hits;
}

#if !defined( _WIN32 ) && !defined( __APPLE__ )
//no MIDI backend on this platform (Linux build): empty device lists
namespace midiport
{
std::vector< std::string > InputNames() { return {}; }
std::vector< std::string > OutputNames() { return {}; }
void* OpenInput( int, int, Sink* ) { return nullptr; }
void CloseInput( void* ) {}
void* OpenOutput( int ) { return nullptr; }
bool SendShort( void*, uint8_t, uint8_t, uint8_t ) { return false; }
bool SendSysEx( void*, const uint8_t*, size_t ) { return false; }
void CloseOutput( void* ) {}
void SignalWake( void* ) {}
}//namespace midiport
#endif
}//namespace ls
