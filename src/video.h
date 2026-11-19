// video inputs for virtual projectors: animated test card, video files (Windows: Media Foundation, video_win.cpp;
// macOS: AVFoundation, video_mac.mm)
// and NDI sources (NDI runtime loaded at run time from NDI Tools / Resolume / TouchDesigner ...)
#pragma once
#include "common.h"
#include <map>
#include <memory>

namespace ls
{
struct Projector;
struct VideoFrame
{
	std::vector< uint8_t > px;//rows top-down, 4 bytes per pixel
	int w = 0, h = 0;
	bool bgra       = false;
	uint64_t serial = 0;//changes with every new picture
	V3 avg{ 0, 0, 0 };  //average colour 0..1 (lens glow)
};

class VideoSource
{
public:
	virtual ~VideoSource() {}
	//hands over the newest picture if it differs from out.serial (buffers are swapped, no copy); true = new picture
	virtual bool Latest( VideoFrame& out ) = 0;
	virtual std::string Status() const = 0;
	virtual bool Ok() const = 0;//false = could not be opened / no runtime
};

std::unique_ptr< VideoSource > MakeTestCard( uint32_t seed );
std::unique_ptr< VideoSource > OpenVideoFile( const std::string& path, bool loop );
std::unique_ptr< VideoSource > OpenNdi( const std::string& sourceName );

//writes a short test clip (top half red, bottom half blue, moving white bar); H.264 .mp4, falls back to .wmv.
//path without extension in, full file name out
bool WriteTestVideo( std::string& path, int w, int h, int frames );

//H.264 .mp4 writer (Media Foundation / AVAssetWriter) for the video export; pictures top-down RGBA
class VideoWriter
{
public:
	~VideoWriter();
	bool Open( const std::string& path, int w, int h, int fps, int kbps, std::string* err );
	bool Write( const uint8_t* rgba );
	bool Close();//finalizes the file
	int Frames() const { return frames_; }
private:
	void* wr_ = nullptr;//IMFSinkWriter / AVAssetWriter wrapper
	unsigned long stream_ = 0;
	int w_ = 0, h_ = 0, fps_ = 30, frames_ = 0;
	bool co_ = false;
};
double VideoFileSeconds( const std::string& path );//duration of a video file (0 = unreadable)

//NDI runtime: true if Processing.NDI.Lib.x64.dll (macOS: libndi.dylib) was found and initialised (where = dll path)
bool NdiAvailable( std::string* where = nullptr );
void NdiShutdown();//stop discovery and close the NDI runtime (program end, after all NDI sources are closed)
std::vector< std::string > NdiSources();//sources on the network (discovery keeps running in the background)

//NDI sender (self test / loopback): sends an animated RGBA picture in its own thread
class NdiTestSender
{
public:
	~NdiTestSender();
	bool Start( const std::string& name, int w, int h, const V3& color );
	void Stop();
	std::string fullName;//"HOST (name)" once known
private:
	struct Impl;
	Impl* impl_ = nullptr;
};

//one source per projector; rebuilt when its input changes
class VideoHub
{
public:
	void Sync( std::vector< Projector >& projectors );//once per frame, main thread
	const VideoFrame* Frame( uint32_t uid ) const;//null = nothing yet
	std::string Status( uint32_t uid ) const;
	void Clear() { slots_.clear(); }

private:
	struct Slot
	{
		std::string key;
		std::unique_ptr< VideoSource > src;
		VideoFrame frame;
	};
	std::map< uint32_t, Slot > slots_;
};
}//namespace ls
