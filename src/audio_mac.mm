// macOS backend of the audio module: ExtAudioFile decode, AudioQueue playback and AudioQueue capture (LTC input).
// Compiled as Objective-C++ with ARC (see CMakeLists.txt); only C APIs of AudioToolbox / CoreAudio are used.
#include "audio.h"
#include "common.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <cmath>
#include <unistd.h>

namespace ls
{
static std::string CfString( CFStringRef s )
{
	if( !s ) return "";
	char buf[ 512 ];
	if( CFStringGetCString( s, buf, sizeof( buf ), kCFStringEncodingUTF8 ) ) return buf;
	return "";
}
//16 bit signed interleaved PCM (the format AudioData and the Windows backends use)
static AudioStreamBasicDescription Pcm16( double rate, int ch )
{
	AudioStreamBasicDescription f{};
	f.mSampleRate       = rate;
	f.mFormatID         = kAudioFormatLinearPCM;
	f.mFormatFlags      = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
	f.mBitsPerChannel   = 16;
	f.mChannelsPerFrame = (UInt32)ch;
	f.mBytesPerFrame    = (UInt32)ch * 2;
	f.mFramesPerPacket  = 1;
	f.mBytesPerPacket   = (UInt32)ch * 2;
	return f;
}

//================================================================ file (wav, aiff, mp3, m4a/aac, alac, flac, caf; no WMA on macOS)
bool DecodeAudioFile( const std::string& file, AudioData& out, std::string& err )
{
	out      = AudioData();
	out.file = file;
	CFURLRef url = CFURLCreateFromFileSystemRepresentation( nullptr, (const UInt8*)file.c_str(), (CFIndex)file.size(), false );
	if( !url )
	{
		err = "cannot open the file";
		return false;
	}
	ExtAudioFileRef f = nullptr;
	const OSStatus st = ExtAudioFileOpenURL( url, &f );
	CFRelease( url );
	if( st != noErr || !f )
	{
		err = "cannot open the file";
		return false;
	}
	bool ok = false;
	do
	{
		AudioStreamBasicDescription in{};
		UInt32 sz = sizeof( in );
		if( ExtAudioFileGetProperty( f, kExtAudioFileProperty_FileDataFormat, &sz, &in ) != noErr ) { err = "no audio stream"; break; }
		const int rate = (int)lround( in.mSampleRate ), ch = (int)in.mChannelsPerFrame;
		if( rate <= 0 || ch <= 0 ) { err = "unknown format"; break; }
		AudioStreamBasicDescription cl = Pcm16( in.mSampleRate, ch );
		if( ExtAudioFileSetProperty( f, kExtAudioFileProperty_ClientDataFormat, sizeof( cl ), &cl ) != noErr ) { err = "cannot decode to PCM"; break; }
		out.rate = rate;
		out.ch   = ch;
		const UInt32 kChunk = 16384;
		std::vector< int16_t > buf( (size_t)kChunk * ch );
		for( ;; )
		{
			AudioBufferList bl;
			bl.mNumberBuffers              = 1;
			bl.mBuffers[ 0 ].mNumberChannels = (UInt32)ch;
			bl.mBuffers[ 0 ].mDataByteSize = (UInt32)( buf.size() * 2 );
			bl.mBuffers[ 0 ].mData         = buf.data();
			UInt32 n = kChunk;
			if( ExtAudioFileRead( f, &n, &bl ) != noErr || n == 0 ) break;
			out.pcm.insert( out.pcm.end(), buf.begin(), buf.begin() + (size_t)n * ch );
			if( out.pcm.size() > (size_t)rate * ch * 60 * 120 ) break;//2 hours
		}
		ok = !out.pcm.empty();
		if( !ok ) err = "no audio data";
	} while( false );
	ExtAudioFileDispose( f );
	return ok;
}

//================================================================ playback (AudioQueue, callbacks on the queue's own thread)
namespace
{
struct OutCtx
{
	std::shared_ptr< const AudioData > d;
	int ch          = 2;
	long long cur   = 0;
	long long end   = 0;//last frame worth filling (file end + 1 s, like on Windows)
	int frames      = 2048;
};
}//namespace

void AudioPlayer::Stop()
{
	run_ = false;
	if( th_.joinable() ) th_.join();
}
double AudioPlayer::Position() const
{
	if( !run_ ) return startSec_;
	double frames = (double)played_;
	if( AudioQueueRef q = (AudioQueueRef)dev_.load() )
	{
		//queue time line in sample frames of the queue's format, starts at 0 with AudioQueueStart (= waveOutGetPosition)
		//MACOS-UNTESTED: assumes mSampleTime counts in the data rate, not the device rate
		AudioTimeStamp ts{};
		if( AudioQueueGetCurrentTime( q, nullptr, &ts, nullptr ) == noErr && ( ts.mFlags & kAudioTimeStampSampleTimeValid ) && ts.mSampleTime >= 0 ) frames = ts.mSampleTime;
	}
	return startSec_ + frames / std::max( 1, rate_ );
}
void AudioPlayer::Loop( double sec )
{
	OutCtx cx;
	cx.d                 = data_;
	cx.ch                = std::min( 2, cx.d->ch );
	const int rate       = cx.d->rate;
	cx.cur               = (long long)llround( sec * rate );
	cx.end               = (long long)( cx.d->pcm.size() / cx.d->ch ) + rate;
	struct Self
	{
		AudioPlayer* p;
		OutCtx* cx;
	} self{ this, &cx };
	AudioStreamBasicDescription f = Pcm16( rate, cx.ch );
	AudioQueueOutputCallback cb   = []( void* u, AudioQueueRef q, AudioQueueBufferRef b ) {
		Self* s = (Self*)u;
		s->p->played_ += s->cx->frames;
		if( !s->p->run_ || s->cx->cur > s->cx->end ) return;//stopping / past the end: let it run out
		s->p->Fill( (int16_t*)b->mAudioData, s->cx->frames, s->cx->ch, s->cx->cur, *s->cx->d );
		b->mAudioDataByteSize = (UInt32)( s->cx->frames * s->cx->ch * 2 );
		AudioQueueEnqueueBuffer( q, b, 0, nullptr );
	};
	AudioQueueRef q = nullptr;
	if( AudioQueueNewOutput( &f, cb, &self, nullptr, kCFRunLoopCommonModes, 0, &q ) != noErr || !q )
	{
		error = "no audio output";
		run_  = false;
		return;
	}
	const int kBufs = 4;
	for( int i = 0; i < kBufs; ++i )
	{
		AudioQueueBufferRef b = nullptr;
		if( AudioQueueAllocateBuffer( q, (UInt32)( cx.frames * cx.ch * 2 ), &b ) != noErr ) continue;
		Fill( (int16_t*)b->mAudioData, cx.frames, cx.ch, cx.cur, *cx.d );
		b->mAudioDataByteSize = (UInt32)( cx.frames * cx.ch * 2 );
		AudioQueueEnqueueBuffer( q, b, 0, nullptr );
	}
	dev_ = q;
	if( AudioQueueStart( q, nullptr ) != noErr )
	{
		error = "no audio output";
		run_  = false;
	}
	while( run_ ) usleep( 20000 );
	dev_ = nullptr;
	AudioQueueStop( q, true );
	AudioQueueDispose( q, true );//buffers are freed with the queue
}

//================================================================ LTC input
//input devices: name + UID (UID selects the device of an AudioQueue)
static std::vector< std::pair< std::string, std::string > > InputDevices()
{
	std::vector< std::pair< std::string, std::string > > v;
	AudioObjectPropertyAddress a{ kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	UInt32 sz = 0;
	if( AudioObjectGetPropertyDataSize( kAudioObjectSystemObject, &a, 0, nullptr, &sz ) != noErr || !sz ) return v;
	std::vector< AudioObjectID > ids( sz / sizeof( AudioObjectID ) );
	if( AudioObjectGetPropertyData( kAudioObjectSystemObject, &a, 0, nullptr, &sz, ids.data() ) != noErr ) return v;
	ids.resize( sz / sizeof( AudioObjectID ) );
	for( AudioObjectID id : ids )
	{
		//only devices with input channels
		AudioObjectPropertyAddress sc{ kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain };
		UInt32 bs = 0;
		if( AudioObjectGetPropertyDataSize( id, &sc, 0, nullptr, &bs ) != noErr || bs < sizeof( AudioBufferList ) ) continue;
		std::vector< uint8_t > mem( bs );
		AudioBufferList* bl = (AudioBufferList*)mem.data();
		if( AudioObjectGetPropertyData( id, &sc, 0, nullptr, &bs, bl ) != noErr ) continue;
		UInt32 chans = 0;
		for( UInt32 i = 0; i < bl->mNumberBuffers; ++i ) chans += bl->mBuffers[ i ].mNumberChannels;
		if( !chans ) continue;
		CFStringRef name = nullptr, uid = nullptr;
		UInt32 cs        = sizeof( CFStringRef );
		AudioObjectPropertyAddress na{ kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
		AudioObjectGetPropertyData( id, &na, 0, nullptr, &cs, &name );
		cs = sizeof( CFStringRef );
		AudioObjectPropertyAddress ua{ kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
		AudioObjectGetPropertyData( id, &ua, 0, nullptr, &cs, &uid );
		if( uid ) v.push_back( { name ? CfString( name ) : CfString( uid ), CfString( uid ) } );
		if( name ) CFRelease( name );
		if( uid ) CFRelease( uid );
	}
	return v;
}
std::vector< std::string > LtcInput::Devices()
{
	std::vector< std::string > v;
	for( auto& d : InputDevices() ) v.push_back( d.first );
	return v;
}
//MACOS-UNTESTED: the app bundle needs NSMicrophoneUsageDescription, otherwise macOS delivers silence (or asks once)
void LtcInput::Loop( int devIdx )
{
	const int rate = 48000, ch = 2;
	const int kBufs = 6, kFrames = 480;//10 ms blocks
	struct Self
	{
		LtcInput* p;
		LtcDecoder dec;
		std::vector< float > x;
	} self;
	self.p = this;
	self.dec.Reset( rate );
	self.x.resize( kFrames );
	AudioStreamBasicDescription f = Pcm16( rate, ch );
	AudioQueueInputCallback cb    = []( void* u, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp*, UInt32, const AudioStreamPacketDescription* ) {
		Self* s     = (Self*)u;
		const int n = (int)( b->mAudioDataByteSize / ( 2 * 2 ) );
		if( n > 0 ) s->p->Process( (const int16_t*)b->mAudioData, n, 2, s->dec, s->x );
		if( s->p->run_ ) AudioQueueEnqueueBuffer( q, b, 0, nullptr );
	};
	AudioQueueRef q = nullptr;
	bool ok         = AudioQueueNewInput( &f, cb, &self, nullptr, kCFRunLoopCommonModes, 0, &q ) == noErr && q;
	if( ok && devIdx >= 0 )
	{
		const auto devs = InputDevices();
		if( devIdx < (int)devs.size() )
		{
			CFStringRef uid = CFStringCreateWithCString( nullptr, devs[ devIdx ].second.c_str(), kCFStringEncodingUTF8 );
			ok              = AudioQueueSetProperty( q, kAudioQueueProperty_CurrentDevice, &uid, sizeof( uid ) ) == noErr;
			CFRelease( uid );
		}
	}
	for( int i = 0; ok && i < kBufs; ++i )
	{
		AudioQueueBufferRef b = nullptr;
		if( AudioQueueAllocateBuffer( q, kFrames * ch * 2, &b ) == noErr ) AudioQueueEnqueueBuffer( q, b, 0, nullptr );
	}
	if( ok ) ok = AudioQueueStart( q, nullptr ) == noErr;
	if( !ok )
	{
		error = "cannot open the audio input";
		if( q ) AudioQueueDispose( q, true );
		run_ = false;
		return;
	}
	while( run_ ) usleep( 20000 );
	AudioQueueStop( q, true );
	AudioQueueDispose( q, true );
}
}//namespace ls
