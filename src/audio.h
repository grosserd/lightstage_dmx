// LightStage 2.0: audio for the show timeline - audio file (decode, waveform, playback) and LTC timecode
// from an audio input (decoder + capture). Windows: Media Foundation (decode), waveOut / waveIn.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ls
{
//decoded audio file: 16 bit interleaved + waveform peaks
struct AudioData
{
	std::string file;
	int rate = 48000, ch = 2;
	std::vector< int16_t > pcm;
	int peakStep = 256;           //frames per peak
	std::vector< uint8_t > peaks; //max |sample| per block, 0..255
	double Seconds() const { return ch > 0 && rate > 0 ? (double)pcm.size() / ch / rate : 0; }
	float PeakAt( double sec0, double sec1 ) const;//highest peak in a time span, 0..1
};
bool LoadAudioFile( const std::string& file, AudioData& out, std::string& err );

//plays AudioData from a position (own thread, waveOut)
class AudioPlayer
{
public:
	~AudioPlayer() { Stop(); }
	void SetData( std::shared_ptr< const AudioData > d );
	std::shared_ptr< const AudioData > Data() const { return data_; }
	void Play( double sec );//(re)starts at sec (negative = silence first)
	void Stop();
	bool Playing() const { return run_; }
	double Position() const;//seconds in the file
	void SetGain( float g ) { gain_ = g; }
	std::string error;

private:
	void Loop( double sec );
	std::shared_ptr< const AudioData > data_;
	std::thread th_;
	std::atomic< bool > run_{ false };
	std::atomic< float > gain_{ 1.0f };
	std::atomic< double > startSec_{ 0 };
	std::atomic< long long > played_{ 0 };//frames given to the device and finished
	std::atomic< void* > dev_{ nullptr };
	int rate_ = 48000;
};

//LTC (SMPTE linear timecode) decoder: feed mono samples, it reports every complete frame
struct LtcDecoder
{
	void Reset( int rate );
	//returns the number of frames decoded in this block; the last one is in h/m/s/f
	int Feed( const float* x, int n );
	int rate = 48000;
	int h = 0, m = 0, s = 0, f = 0;
	bool drop   = false;
	int fps     = 25;  //estimated from the bit length
	long long frames = 0;//decoded in total
	double Seconds() const;//time code at the end of the last frame (= the start of the next one)
private:
	float per_ = 24, peak_ = 0.05f;
	int count_ = 0, sign_ = 0, maxF_ = 0;
	bool half_ = false;
	uint64_t w0_ = 0;
	uint16_t w1_ = 0;
	void Bit( int b );
};
//test signal: frames of LTC starting at h:m:s:f (biphase mark, +-amp)
void LtcEncode( int h, int m, int s, int f, int fps, int rate, int frames, float amp, std::vector< float >& out );

//audio input -> LTC (own thread, waveIn)
class LtcInput
{
public:
	~LtcInput() { Stop(); }
	bool Start( const std::string& device, int channel );//"" = default input
	void Stop();
	bool Running() const { return run_; }
	const std::string& Device() const { return dev_; }
	int Channel() const { return chan_; }
	//last decoded time code: seconds, frame rate, Now() when it was decoded; false = none yet
	bool Get( double& sec, int& fps, double& at ) const;
	float Level() const { return level_; }//input peak 0..1
	std::string error;
	static std::vector< std::string > Devices();

private:
	void Loop( int devIdx );
	std::thread th_;
	std::atomic< bool > run_{ false };
	std::string dev_;
	int chan_ = 0;
	mutable std::mutex mx_;
	double sec_ = -1, at_ = 0;
	int fps_ = 25;
	std::atomic< float > level_{ 0 };
};
}//namespace ls
