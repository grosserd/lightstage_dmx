// Ableton Link session (tempo + beat phase shared over the network with Ableton Live and other Link apps).
// Compiled in its own translation unit (link_impl.cpp, asio + Link SDK headers).
#pragma once

namespace ls
{
class LinkSync
{
public:
	LinkSync();
	~LinkSync();
	void Enable( bool on );
	bool Enabled() const;
	int Peers() const;
	//tempo + beat position (with quantum, i.e. bar length in beats) right now; false when disabled
	bool Capture( double& bpm, double& beat, double& phase, double quantum ) const;
	void SetTempo( double bpm );
	//start/stop sync (Live transport); playing is only meaningful when enabled
	void EnableStartStop( bool on );
	bool Playing() const;

private:
	void* impl_ = nullptr;
};
}//namespace ls
