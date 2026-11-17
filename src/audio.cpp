// LightStage 2.0: audio file + playback, LTC decoder / encoder / input
#include "audio.h"
#include "common.h"
#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <cmath>

namespace ls
{
template< class T > static void Rel( T*& p )
{
	if( p ) p->Release();
	p = nullptr;
}

//================================================================ file
float AudioData::PeakAt( double a, double b ) const
{
	if( peaks.empty() || rate <= 0 ) return 0;
	long long i0 = (long long)floor( a * rate / peakStep ), i1 = (long long)ceil( b * rate / peakStep );
	i0 = std::max( 0LL, i0 );
	i1 = std::min( (long long)peaks.size(), std::max( i0 + 1, i1 ) );
	int m = 0;
	for( long long i = i0; i < i1; ++i ) m = std::max( m, (int)peaks[ i ] );
	return m / 255.0f;
}
bool LoadAudioFile( const std::string& file, AudioData& out, std::string& err )
{
	static std::once_flag once;
	std::call_once( once, [] { MFStartup( MF_VERSION, MFSTARTUP_FULL ); } );
	const HRESULT co = CoInitializeEx( nullptr, COINIT_MULTITHREADED );
	IMFSourceReader* rd = nullptr;
	IMFMediaType* mt    = nullptr;
	bool ok             = false;
	out                 = AudioData();
	out.file            = file;
	do
	{
		if( FAILED( MFCreateSourceReaderFromURL( Widen( file ).c_str(), nullptr, &rd ) ) ) { err = "cannot open the file"; break; }
		rd->SetStreamSelection( (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE );
		if( FAILED( rd->SetStreamSelection( (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE ) ) ) { err = "no audio stream"; break; }
		if( FAILED( MFCreateMediaType( &mt ) ) ) break;
		mt->SetGUID( MF_MT_MAJOR_TYPE, MFMediaType_Audio );
		mt->SetGUID( MF_MT_SUBTYPE, MFAudioFormat_PCM );
		mt->SetUINT32( MF_MT_AUDIO_BITS_PER_SAMPLE, 16 );
		if( FAILED( rd->SetCurrentMediaType( (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, mt ) ) ) { err = "cannot decode to PCM"; break; }
		Rel( mt );
		if( FAILED( rd->GetCurrentMediaType( (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &mt ) ) ) break;
		UINT32 rate = 0, ch = 0;
		mt->GetUINT32( MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate );
		mt->GetUINT32( MF_MT_AUDIO_NUM_CHANNELS, &ch );
		if( !rate || !ch ) { err = "unknown format"; break; }
		out.rate = (int)rate;
		out.ch   = (int)ch;
		for( ;; )
		{
			DWORD flags   = 0;
			IMFSample* sm = nullptr;
			if( FAILED( rd->ReadSample( (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sm ) ) ) break;
			if( sm )
			{
				IMFMediaBuffer* b = nullptr;
				if( SUCCEEDED( sm->ConvertToContiguousBuffer( &b ) ) )
				{
					BYTE* p  = nullptr;
					DWORD n  = 0;
					if( SUCCEEDED( b->Lock( &p, nullptr, &n ) ) )
					{
						const int16_t* s = (const int16_t*)p;
						out.pcm.insert( out.pcm.end(), s, s + n / 2 );
						b->Unlock();
					}
					b->Release();
				}
				sm->Release();
			}
			if( flags & MF_SOURCE_READERF_ENDOFSTREAM ) break;
			if( out.pcm.size() > (size_t)rate * ch * 60 * 120 ) break;//2 hours
		}
		ok = !out.pcm.empty();
		if( !ok ) err = "no audio data";
	} while( false );
	Rel( mt );
	Rel( rd );
	if( SUCCEEDED( co ) ) CoUninitialize();
	if( !ok ) return false;
	const size_t frames = out.pcm.size() / out.ch;
	out.peaks.resize( ( frames + out.peakStep - 1 ) / out.peakStep );
	for( size_t i = 0; i < out.peaks.size(); ++i )
	{
		int m           = 0;
		const size_t a  = i * out.peakStep * out.ch, b = std::min( out.pcm.size(), ( i + 1 ) * out.peakStep * out.ch );
		for( size_t k = a; k < b; ++k ) m = std::max( m, abs( (int)out.pcm[ k ] ) );
		out.peaks[ i ] = (uint8_t)std::min( 255, m * 255 / 32767 );
	}
	return true;
}

//================================================================ playback
void AudioPlayer::SetData( std::shared_ptr< const AudioData > d )
{
	Stop();
	data_ = std::move( d );
}
void AudioPlayer::Play( double sec )
{
	Stop();
	if( !data_ || data_->pcm.empty() ) return;
	run_      = true;
	startSec_ = sec;
	played_   = 0;
	rate_     = data_->rate;
	th_       = std::thread( [ this, sec ] { Loop( sec ); } );
}
void AudioPlayer::Stop()
{
	run_ = false;
	if( HWAVEOUT w = (HWAVEOUT)dev_.load() ) waveOutReset( w );
	if( th_.joinable() ) th_.join();
}
double AudioPlayer::Position() const
{
	if( !run_ ) return startSec_;
	double frames = (double)played_;
	if( HWAVEOUT w = (HWAVEOUT)dev_.load() )
	{
		MMTIME t{};
		t.wType = TIME_SAMPLES;
		if( waveOutGetPosition( w, &t, sizeof( t ) ) == MMSYSERR_NOERROR && t.wType == TIME_SAMPLES ) frames = t.u.sample;
	}
	return startSec_ + frames / std::max( 1, rate_ );
}
void AudioPlayer::Loop( double sec )
{
	std::shared_ptr< const AudioData > d = data_;
	const int ch = std::min( 2, d->ch ), rate = d->rate;
	WAVEFORMATEX wf{};
	wf.wFormatTag      = WAVE_FORMAT_PCM;
	wf.nChannels       = (WORD)ch;
	wf.nSamplesPerSec  = rate;
	wf.wBitsPerSample  = 16;
	wf.nBlockAlign     = (WORD)( ch * 2 );
	wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
	HANDLE ev          = CreateEventW( nullptr, FALSE, FALSE, nullptr );
	HWAVEOUT w         = nullptr;
	if( waveOutOpen( &w, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT ) != MMSYSERR_NOERROR )
	{
		error = "no audio output";
		CloseHandle( ev );
		run_ = false;
		return;
	}
	dev_ = w;
	const int kBufs = 4, kFrames = 2048;
	std::vector< int16_t > mem( (size_t)kBufs * kFrames * ch );
	WAVEHDR hdr[ kBufs ]{};
	long long cur = (long long)llround( sec * rate );//frame in the file (negative = silence)
	auto fill     = [ & ]( int i ) {
		int16_t* o    = mem.data() + (size_t)i * kFrames * ch;
		const float g = gain_;
		for( int k = 0; k < kFrames; ++k, ++cur )
			for( int c = 0; c < ch; ++c )
			{
				int v = 0;
				if( cur >= 0 && (size_t)cur * d->ch + c < d->pcm.size() ) v = (int)lroundf( d->pcm[ (size_t)cur * d->ch + std::min( c, d->ch - 1 ) ] * g );
				o[ k * ch + c ] = (int16_t)Clamp( v, -32768, 32767 );
			}
		hdr[ i ].lpData         = (LPSTR)o;
		hdr[ i ].dwBufferLength = kFrames * ch * 2;
		hdr[ i ].dwFlags        = 0;
		waveOutPrepareHeader( w, &hdr[ i ], sizeof( WAVEHDR ) );
		waveOutWrite( w, &hdr[ i ], sizeof( WAVEHDR ) );
	};
	for( int i = 0; i < kBufs; ++i ) fill( i );
	while( run_ )
	{
		WaitForSingleObject( ev, 50 );
		for( int i = 0; i < kBufs && run_; ++i )
			if( hdr[ i ].dwFlags & WHDR_DONE )
			{
				waveOutUnprepareHeader( w, &hdr[ i ], sizeof( WAVEHDR ) );
				played_ += kFrames;
				if( cur > (long long)( d->pcm.size() / d->ch ) + rate ) continue;//past the end: let it run out
				fill( i );
			}
	}
	waveOutReset( w );
	for( int i = 0; i < kBufs; ++i )
		if( hdr[ i ].dwFlags & WHDR_PREPARED ) waveOutUnprepareHeader( w, &hdr[ i ], sizeof( WAVEHDR ) );
	dev_ = nullptr;
	waveOutClose( w );
	CloseHandle( ev );
}

//================================================================ LTC
void LtcDecoder::Reset( int r )
{
	rate   = r;
	per_   = r / 2000.0f;//25 fps * 80 bits
	peak_  = 0.05f;
	count_ = 0;
	sign_  = 0;
	half_  = false;
	w0_    = 0;
	w1_    = 0;
	maxF_  = 0;
}
void LtcDecoder::Bit( int b )
{
	w0_ = ( w0_ << 1 ) | ( w1_ >> 15 );
	w1_ = (uint16_t)( ( w1_ << 1 ) | b );
}
static int LtcField( uint64_t w0, int bit, int n )
{
	int v = 0;
	for( int i = 0; i < n; ++i ) v |= (int)( ( w0 >> ( 63 - ( bit + i ) ) ) & 1 ) << i;
	return v;
}
int LtcDecoder::Feed( const float* x, int n )
{
	int got = 0;
	for( int i = 0; i < n; ++i )
	{
		const float v = x[ i ];
		peak_         = std::max( peak_ * 0.9995f, fabsf( v ) );
		const float th = peak_ * 0.25f;
		++count_;
		int sg = sign_;
		if( v > th ) sg = 1;
		else if( v < -th ) sg = -1;
		if( sg == sign_ || sg == 0 ) continue;
		const bool first = sign_ == 0;
		sign_            = sg;
		const int iv     = count_;
		count_           = 0;
		if( first ) continue;
		if( iv > per_ * 3.0f || iv < per_ * 0.2f )
		{
			//silence / noise: resynchronise
			half_ = false;
			if( iv > per_ * 3.0f && iv < rate / 400 ) per_ = iv * 0.5f;//slower code than assumed
			continue;
		}
		if( iv > per_ * 0.75f )
		{
			half_ = false;
			Bit( 0 );
			per_ = per_ * 0.9f + iv * 0.1f;
		}
		else if( !half_ )
		{
			half_ = true;
			continue;
		}
		else
		{
			half_ = false;
			Bit( 1 );
			per_ = per_ * 0.9f + iv * 2 * 0.1f;
		}
		if( w1_ == 0x3FFD )//sync word: a frame is complete
		{
			const int fu = LtcField( w0_, 0, 4 ), ft = LtcField( w0_, 8, 2 );
			const int su = LtcField( w0_, 16, 4 ), st = LtcField( w0_, 24, 3 );
			const int mu = LtcField( w0_, 32, 4 ), mt = LtcField( w0_, 40, 3 );
			const int hu = LtcField( w0_, 48, 4 ), ht = LtcField( w0_, 56, 2 );
			const int ff = ft * 10 + fu, ss = st * 10 + su, mm = mt * 10 + mu, hh = ht * 10 + hu;
			if( ff < 30 && ss < 60 && mm < 60 && hh < 24 )
			{
				f    = ff;
				s    = ss;
				m    = mm;
				h    = hh;
				drop = LtcField( w0_, 10, 1 ) != 0;
				maxF_ = std::max( maxF_, ff );
				//frame rate: from the bit length, at least what the frame numbers need
				const float est = rate / ( per_ * 80.0f );
				int fr          = est < 24.5f ? 24 : est < 27.5f ? 25 : 30;
				if( maxF_ >= fr ) fr = maxF_ >= 25 ? 30 : 25;
				fps = fr;
				++frames;
				++got;
			}
		}
	}
	return got;
}
double LtcDecoder::Seconds() const { return h * 3600.0 + m * 60.0 + s + ( f + 1.0 ) / std::max( 1, fps ); }

void LtcEncode( int h, int m, int s, int f, int fps, int rate, int frames, float amp, std::vector< float >& out )
{
	out.clear();
	float level   = amp;
	double pos    = 0;
	const double spb = (double)rate / ( fps * 80.0 );
	for( int k = 0; k < frames; ++k )
	{
		int bits[ 80 ] = {};
		auto put       = [ & ]( int at, int n, int v ) {
			for( int i = 0; i < n; ++i ) bits[ at + i ] = ( v >> i ) & 1;
		};
		put( 0, 4, f % 10 );
		put( 8, 2, f / 10 );
		put( 16, 4, s % 10 );
		put( 24, 3, s / 10 );
		put( 32, 4, m % 10 );
		put( 40, 3, m / 10 );
		put( 48, 4, h % 10 );
		put( 56, 2, h / 10 );
		const int sync[ 16 ] = { 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1 };
		for( int i = 0; i < 16; ++i ) bits[ 64 + i ] = sync[ i ];
		for( int b = 0; b < 80; ++b )
		{
			level = -level;//every bit starts with a transition
			const double end = pos + spb, mid = pos + spb * 0.5;
			while( (double)out.size() < end )
			{
				if( bits[ b ] && (double)out.size() >= mid && (double)out.size() - 1 < mid ) level = -level;
				out.push_back( level );
			}
			pos = end;
		}
		if( ++f >= fps )
		{
			f = 0;
			if( ++s >= 60 )
			{
				s = 0;
				if( ++m >= 60 ) { m = 0; h = ( h + 1 ) % 24; }
			}
		}
	}
}

std::vector< std::string > LtcInput::Devices()
{
	std::vector< std::string > v;
	const UINT n = waveInGetNumDevs();
	for( UINT i = 0; i < n; ++i )
	{
		WAVEINCAPSW c{};
		if( waveInGetDevCapsW( i, &c, sizeof( c ) ) == MMSYSERR_NOERROR ) v.push_back( Narrow( c.szPname ) );
	}
	return v;
}
bool LtcInput::Start( const std::string& device, int channel )
{
	Stop();
	error.clear();
	int idx = -1;//WAVE_MAPPER
	if( !device.empty() )
	{
		const auto d = Devices();
		for( int i = 0; i < (int)d.size(); ++i )
			if( d[ i ] == device ) idx = i;
		if( idx < 0 )
		{
			error = "input not found: " + device;
			return false;
		}
	}
	dev_  = device;
	chan_ = channel;
	{
		std::lock_guard< std::mutex > l( mx_ );
		sec_ = -1;
	}
	run_ = true;
	th_  = std::thread( [ this, idx ] { Loop( idx ); } );
	return true;
}
void LtcInput::Stop()
{
	run_ = false;
	if( th_.joinable() ) th_.join();
}
bool LtcInput::Get( double& sec, int& fps, double& at ) const
{
	std::lock_guard< std::mutex > l( mx_ );
	if( sec_ < 0 ) return false;
	sec = sec_;
	fps = fps_;
	at  = at_;
	return true;
}
void LtcInput::Loop( int devIdx )
{
	const int rate = 48000, ch = 2;
	WAVEFORMATEX wf{};
	wf.wFormatTag      = WAVE_FORMAT_PCM;
	wf.nChannels       = ch;
	wf.nSamplesPerSec  = rate;
	wf.wBitsPerSample  = 16;
	wf.nBlockAlign     = ch * 2;
	wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
	HANDLE ev          = CreateEventW( nullptr, FALSE, FALSE, nullptr );
	HWAVEIN w          = nullptr;
	if( waveInOpen( &w, devIdx < 0 ? WAVE_MAPPER : (UINT)devIdx, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT ) != MMSYSERR_NOERROR )
	{
		error = "cannot open the audio input";
		CloseHandle( ev );
		run_ = false;
		return;
	}
	const int kBufs = 6, kFrames = 480;//10 ms blocks
	std::vector< int16_t > mem( (size_t)kBufs * kFrames * ch );
	WAVEHDR hdr[ kBufs ]{};
	for( int i = 0; i < kBufs; ++i )
	{
		hdr[ i ].lpData         = (LPSTR)( mem.data() + (size_t)i * kFrames * ch );
		hdr[ i ].dwBufferLength = kFrames * ch * 2;
		waveInPrepareHeader( w, &hdr[ i ], sizeof( WAVEHDR ) );
		waveInAddBuffer( w, &hdr[ i ], sizeof( WAVEHDR ) );
	}
	waveInStart( w );
	LtcDecoder dec;
	dec.Reset( rate );
	std::vector< float > x( kFrames );
	while( run_ )
	{
		WaitForSingleObject( ev, 50 );
		for( int i = 0; i < kBufs && run_; ++i )
		{
			if( !( hdr[ i ].dwFlags & WHDR_DONE ) ) continue;
			const int16_t* s = (const int16_t*)hdr[ i ].lpData;
			const int n      = (int)hdr[ i ].dwBytesRecorded / ( ch * 2 );
			float pk         = 0;
			for( int k = 0; k < n; ++k )
			{
				x[ k ] = s[ k * ch + Clamp( chan_, 0, ch - 1 ) ] / 32768.0f;
				pk     = std::max( pk, fabsf( x[ k ] ) );
			}
			level_ = std::max( pk, level_ * 0.8f );
			if( dec.Feed( x.data(), n ) > 0 )
			{
				std::lock_guard< std::mutex > l( mx_ );
				sec_ = dec.Seconds();
				fps_ = dec.fps;
				at_  = Now();
			}
			waveInAddBuffer( w, &hdr[ i ], sizeof( WAVEHDR ) );
		}
	}
	waveInReset( w );
	for( int i = 0; i < kBufs; ++i ) waveInUnprepareHeader( w, &hdr[ i ], sizeof( WAVEHDR ) );
	waveInClose( w );
	CloseHandle( ev );
}
}//namespace ls
