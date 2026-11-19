// LightStage 2.0: audio file + playback, LTC decoder / encoder / input (platform parts: audio_win.cpp, audio_mac.mm)
#include "audio.h"
#include "common.h"
#include <algorithm>
#include <cmath>

namespace ls
{
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
	if( !DecodeAudioFile( file, out, err ) ) return false;
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
void AudioPlayer::Fill( int16_t* o, int frames, int ch, long long& cur, const AudioData& d ) const
{
	const float g = gain_;
	for( int k = 0; k < frames; ++k, ++cur )
		for( int c = 0; c < ch; ++c )
		{
			int v = 0;
			if( cur >= 0 && (size_t)cur * d.ch + c < d.pcm.size() ) v = (int)lroundf( d.pcm[ (size_t)cur * d.ch + std::min( c, d.ch - 1 ) ] * g );
			o[ k * ch + c ] = (int16_t)Clamp( v, -32768, 32767 );
		}
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
void LtcInput::Process( const int16_t* s, int n, int ch, LtcDecoder& dec, std::vector< float >& x )
{
	if( (int)x.size() < n ) x.resize( n );
	float pk = 0;
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
}
}//namespace ls
