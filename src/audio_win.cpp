// Windows backend of the audio module: Media Foundation decode, waveOut playback, waveIn capture
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
bool DecodeAudioFile( const std::string& file, AudioData& out, std::string& err )
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
	return ok;
}

//================================================================ playback
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
		int16_t* o = mem.data() + (size_t)i * kFrames * ch;
		Fill( o, kFrames, ch, cur, *d );
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

//================================================================ LTC input
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
			Process( s, n, ch, dec, x );
			waveInAddBuffer( w, &hdr[ i ], sizeof( WAVEHDR ) );
		}
	}
	waveInReset( w );
	for( int i = 0; i < kBufs; ++i ) waveInUnprepareHeader( w, &hdr[ i ], sizeof( WAVEHDR ) );
	waveInClose( w );
	CloseHandle( ev );
}
}//namespace ls
