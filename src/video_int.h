// internal: platform video file decoder used by the file source in video.cpp
// (Windows: Media Foundation in video_win.cpp, macOS: AVFoundation in video_mac.mm)
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ls
{
class VideoDecoder
{
public:
	virtual ~VideoDecoder() {}
	//0 = ok, 1 = file not readable, 2 = no video stream / codec not supported.
	//Open, Read, Seek and the destructor are called from the same (decoder) thread.
	virtual int Open( const std::string& path ) = 0;
	virtual int W() const = 0;
	virtual int H() const = 0;
	virtual double Fps() const = 0;     //nominal frame rate (0 = unknown)
	virtual double Duration() const = 0;//seconds (0 = unknown)
	//next frame, rows top-down BGRA (W() x H() x 4) into 'bgra', presentation time in seconds into 'ts'.
	//1 = picture, 3 = sample without usable picture (still paced), 2 = nothing (try again), 0 = end of stream, -1 = read error
	virtual int Read( std::vector< uint8_t >& bgra, double& ts ) = 0;
	virtual bool Seek( double sec ) = 0;
};
std::unique_ptr< VideoDecoder > MakeVideoDecoder();
void MediaSleepMs( int ms );
}//namespace ls
