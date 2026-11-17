// Ableton Link wrapper (needs third_party\link, GPL-2.0 / Ableton license)
#include "link.h"
#include <ableton/Link.hpp>

namespace ls
{
LinkSync::LinkSync() { impl_ = new ableton::Link( 120.0 ); }
LinkSync::~LinkSync()
{
	auto* l = (ableton::Link*)impl_;
	l->enable( false );
	delete l;
}
void LinkSync::Enable( bool on ) { ( (ableton::Link*)impl_ )->enable( on ); }
bool LinkSync::Enabled() const { return ( (ableton::Link*)impl_ )->isEnabled(); }
int LinkSync::Peers() const { return (int)( (ableton::Link*)impl_ )->numPeers(); }
bool LinkSync::Capture( double& bpm, double& beat, double& phase, double quantum ) const
{
	auto* l = (ableton::Link*)impl_;
	if( !l->isEnabled() ) return false;
	auto st         = l->captureAppSessionState();
	const auto time = l->clock().micros();
	bpm             = st.tempo();
	beat            = st.beatAtTime( time, quantum );
	phase           = st.phaseAtTime( time, quantum );
	return true;
}
void LinkSync::SetTempo( double bpm )
{
	auto* l = (ableton::Link*)impl_;
	auto st = l->captureAppSessionState();
	st.setTempo( bpm, l->clock().micros() );
	l->commitAppSessionState( st );
}
void LinkSync::EnableStartStop( bool on ) { ( (ableton::Link*)impl_ )->enableStartStopSync( on ); }
bool LinkSync::Playing() const { return ( (ableton::Link*)impl_ )->captureAppSessionState().isPlaying(); }
}//namespace ls
