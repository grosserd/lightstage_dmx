// Windows backend of the video module: Media Foundation file decoder, H.264 writer, duration
#include "video.h"
#include "video_int.h"
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mutex>

namespace ls
{
template< class T > static void Rel( T*& p )
{
	if( p ) p->Release();
	p = nullptr;
}
static void EnsureMf()
{
	static std::once_flag f;
	std::call_once( f, [] { MFStartup( MF_VERSION, MFSTARTUP_FULL ); } );
}

class MfDecoder : public VideoDecoder
{
public:
	~MfDecoder() override
	{
		Rel( rd_ );
		if( co_ ) CoUninitialize();
	}
	int Open( const std::string& path ) override
	{
		co_ = SUCCEEDED( CoInitializeEx( nullptr, COINIT_MULTITHREADED ) );
		EnsureMf();
		IMFAttributes* at = nullptr;
		MFCreateAttributes( &at, 2 );
		at->SetUINT32( MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE );
		HRESULT hr = MFCreateSourceReaderFromURL( Widen( path ).c_str(), at, &rd_ );
		Rel( at );
		if( FAILED( hr ) ) return 1;
		rd_->SetStreamSelection( (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE );
		rd_->SetStreamSelection( (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE );
		IMFMediaType* mt = nullptr;
		MFCreateMediaType( &mt );
		mt->SetGUID( MF_MT_MAJOR_TYPE, MFMediaType_Video );
		mt->SetGUID( MF_MT_SUBTYPE, MFVideoFormat_RGB32 );
		hr = rd_->SetCurrentMediaType( (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt );
		Rel( mt );
		if( FAILED( hr ) ) return 2;
		ReadType();
		PROPVARIANT v;
		PropVariantInit( &v );
		if( SUCCEEDED( rd_->GetPresentationAttribute( (DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v ) ) ) dur_ = v.uhVal.QuadPart / 1e7;
		PropVariantClear( &v );
		return 0;
	}
	int W() const override { return (int)w_; }
	int H() const override { return (int)h_; }
	double Fps() const override { return fps_; }
	double Duration() const override { return dur_; }
	int Read( std::vector< uint8_t >& work, double& ts ) override
	{
		DWORD si = 0, flags = 0;
		LONGLONG t   = 0;
		IMFSample* s = nullptr;
		HRESULT hr   = rd_->ReadSample( (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &si, &flags, &t, &s );
		if( FAILED( hr ) ) return -1;
		if( flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED ) ReadType();
		if( flags & MF_SOURCE_READERF_ENDOFSTREAM )
		{
			Rel( s );
			return 0;
		}
		if( !s ) return 2;
		ts      = t * 1e-7;
		int ret = 3;
		IMFMediaBuffer* b = nullptr;
		if( SUCCEEDED( s->ConvertToContiguousBuffer( &b ) ) )
		{
			BYTE* p   = nullptr;
			DWORD len = 0;
			if( SUCCEEDED( b->Lock( &p, nullptr, &len ) ) )
			{
				const UINT32 w = w_, h = h_;
				LONG as        = stride_ < 0 ? -stride_ : stride_;
				if( as < (LONG)w * 4 ) as = (LONG)w * 4;
				if( w && h && (size_t)as * h <= len )
				{
					work.resize( (size_t)w * h * 4 );
					for( UINT32 y = 0; y < h; ++y )
					{
						const BYTE* src = stride_ < 0 ? p + (size_t)( h - 1 - y ) * as : p + (size_t)y * as;
						memcpy( &work[ (size_t)y * w * 4 ], src, (size_t)w * 4 );
					}
					ret = 1;
				}
				b->Unlock();
			}
			b->Release();
		}
		s->Release();
		return ret;
	}
	bool Seek( double sec ) override
	{
		PROPVARIANT v;
		PropVariantInit( &v );
		v.vt            = VT_I8;
		v.hVal.QuadPart = (LONGLONG)( sec * 1e7 );
		const HRESULT hr = rd_->SetCurrentPosition( GUID_NULL, v );
		PropVariantClear( &v );
		return SUCCEEDED( hr );
	}

private:
	void ReadType()
	{
		IMFMediaType* c = nullptr;
		if( SUCCEEDED( rd_->GetCurrentMediaType( (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &c ) ) )
		{
			MFGetAttributeSize( c, MF_MT_FRAME_SIZE, &w_, &h_ );
			UINT32 s = 0;
			if( SUCCEEDED( c->GetUINT32( MF_MT_DEFAULT_STRIDE, &s ) ) ) stride_ = (LONG)(INT32)s;
			else if( FAILED( MFGetStrideForBitmapInfoHeader( MFVideoFormat_RGB32.Data1, w_, &stride_ ) ) ) stride_ = (LONG)w_ * 4;
			UINT32 n = 0, d = 0;
			if( SUCCEEDED( MFGetAttributeRatio( c, MF_MT_FRAME_RATE, &n, &d ) ) && d ) fps_ = (double)n / d;
			c->Release();
		}
	}
	IMFSourceReader* rd_ = nullptr;
	bool co_             = false;
	UINT32 w_ = 0, h_ = 0;
	LONG stride_ = 0;
	double fps_ = 0, dur_ = 0;
};
std::unique_ptr< VideoDecoder > MakeVideoDecoder() { return std::unique_ptr< VideoDecoder >( new MfDecoder ); }

bool WriteTestVideo( std::string& path, int w, int h, int frames )
{
	HRESULT co = CoInitializeEx( nullptr, COINIT_MULTITHREADED );
	EnsureMf();
	bool ok = false;
	for( int attempt = 0; attempt < 2 && !ok; ++attempt )
	{
		std::string file = path + ( attempt == 0 ? ".mp4" : ".wmv" );
		DeleteFileW( Widen( file ).c_str() );
		IMFSinkWriter* wr = nullptr;
		IMFMediaType *out = nullptr, *in = nullptr;
		DWORD si          = 0;
		HRESULT hr        = MFCreateSinkWriterFromURL( Widen( file ).c_str(), nullptr, nullptr, &wr );
		if( SUCCEEDED( hr ) ) hr = MFCreateMediaType( &out );
		if( SUCCEEDED( hr ) )
		{
			out->SetGUID( MF_MT_MAJOR_TYPE, MFMediaType_Video );
			out->SetGUID( MF_MT_SUBTYPE, attempt == 0 ? MFVideoFormat_H264 : MFVideoFormat_WMV3 );
			out->SetUINT32( MF_MT_AVG_BITRATE, 3000000 );
			out->SetUINT32( MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive );
			MFSetAttributeSize( out, MF_MT_FRAME_SIZE, w, h );
			MFSetAttributeRatio( out, MF_MT_FRAME_RATE, 30, 1 );
			MFSetAttributeRatio( out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1 );
			hr = wr->AddStream( out, &si );
		}
		if( SUCCEEDED( hr ) ) hr = MFCreateMediaType( &in );
		if( SUCCEEDED( hr ) )
		{
			in->SetGUID( MF_MT_MAJOR_TYPE, MFMediaType_Video );
			in->SetGUID( MF_MT_SUBTYPE, MFVideoFormat_RGB32 );
			in->SetUINT32( MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive );
			in->SetUINT32( MF_MT_DEFAULT_STRIDE, (UINT32)( w * 4 ) );//top-down rows
			MFSetAttributeSize( in, MF_MT_FRAME_SIZE, w, h );
			MFSetAttributeRatio( in, MF_MT_FRAME_RATE, 30, 1 );
			MFSetAttributeRatio( in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1 );
			hr = wr->SetInputMediaType( si, in, nullptr );
		}
		if( SUCCEEDED( hr ) ) hr = wr->BeginWriting();
		for( int f = 0; f < frames && SUCCEEDED( hr ); ++f )
		{
			IMFMediaBuffer* b = nullptr;
			IMFSample* s      = nullptr;
			hr                = MFCreateMemoryBuffer( (DWORD)( w * h * 4 ), &b );
			BYTE* p           = nullptr;
			if( SUCCEEDED( hr ) ) hr = b->Lock( &p, nullptr, nullptr );
			if( SUCCEEDED( hr ) )
			{
				int bx = ( f * 8 ) % std::max( 1, w - 16 );
				for( int y = 0; y < h; ++y )
					for( int x = 0; x < w; ++x )
					{
						BYTE* q   = p + ( (size_t)y * w + x ) * 4;
						bool top  = y < h / 2;
						bool bar  = x >= bx && x < bx + 16 && y > h * 3 / 8 && y < h * 5 / 8;
						q[ 0 ]    = bar ? 255 : ( top ? 20 : 235 );//B
						q[ 1 ]    = bar ? 255 : 20;                //G
						q[ 2 ]    = bar ? 255 : ( top ? 235 : 20 );//R
						q[ 3 ]    = 255;
					}
				b->Unlock();
				b->SetCurrentLength( (DWORD)( w * h * 4 ) );
				hr = MFCreateSample( &s );
			}
			if( SUCCEEDED( hr ) ) hr = s->AddBuffer( b );
			if( SUCCEEDED( hr ) ) hr = s->SetSampleTime( (LONGLONG)f * 333333 );
			if( SUCCEEDED( hr ) ) hr = s->SetSampleDuration( 333333 );
			if( SUCCEEDED( hr ) ) hr = wr->WriteSample( si, s );
			Rel( s );
			Rel( b );
		}
		if( SUCCEEDED( hr ) ) hr = wr->Finalize();
		Rel( in );
		Rel( out );
		Rel( wr );
		if( SUCCEEDED( hr ) )
		{
			ok   = true;
			path = file;
		}
		else
			Log( "test video %s: 0x%08x", file.c_str(), (unsigned)hr );
	}
	if( SUCCEEDED( co ) ) CoUninitialize();
	return ok;
}

//================================================================ video export writer
VideoWriter::~VideoWriter()
{
	if( wr_ ) Close();
}
bool VideoWriter::Open( const std::string& path, int w, int h, int fps, int kbps, std::string* err )
{
	co_ = SUCCEEDED( CoInitializeEx( nullptr, COINIT_MULTITHREADED ) );
	EnsureMf();
	w_ = w & ~1;
	h_ = h & ~1;
	fps_    = std::max( 1, fps );
	frames_ = 0;
	DeleteFileW( Widen( path ).c_str() );
	IMFAttributes* at = nullptr;
	MFCreateAttributes( &at, 2 );
	if( at ) at->SetUINT32( MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE );
	IMFSinkWriter* wr = nullptr;
	IMFMediaType *out = nullptr, *in = nullptr;
	DWORD si          = 0;
	HRESULT hr        = MFCreateSinkWriterFromURL( Widen( path ).c_str(), nullptr, at, &wr );
	Rel( at );
	if( SUCCEEDED( hr ) ) hr = MFCreateMediaType( &out );
	if( SUCCEEDED( hr ) )
	{
		out->SetGUID( MF_MT_MAJOR_TYPE, MFMediaType_Video );
		out->SetGUID( MF_MT_SUBTYPE, MFVideoFormat_H264 );
		out->SetUINT32( MF_MT_AVG_BITRATE, (UINT32)std::max( 500, kbps ) * 1000 );
		out->SetUINT32( MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive );
		MFSetAttributeSize( out, MF_MT_FRAME_SIZE, w_, h_ );
		MFSetAttributeRatio( out, MF_MT_FRAME_RATE, fps_, 1 );
		MFSetAttributeRatio( out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1 );
		hr = wr->AddStream( out, &si );
	}
	if( SUCCEEDED( hr ) ) hr = MFCreateMediaType( &in );
	if( SUCCEEDED( hr ) )
	{
		in->SetGUID( MF_MT_MAJOR_TYPE, MFMediaType_Video );
		in->SetGUID( MF_MT_SUBTYPE, MFVideoFormat_RGB32 );
		in->SetUINT32( MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive );
		in->SetUINT32( MF_MT_DEFAULT_STRIDE, (UINT32)( w_ * 4 ) );//top-down rows
		MFSetAttributeSize( in, MF_MT_FRAME_SIZE, w_, h_ );
		MFSetAttributeRatio( in, MF_MT_FRAME_RATE, fps_, 1 );
		MFSetAttributeRatio( in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1 );
		hr = wr->SetInputMediaType( si, in, nullptr );
	}
	if( SUCCEEDED( hr ) ) hr = wr->BeginWriting();
	Rel( in );
	Rel( out );
	if( FAILED( hr ) )
	{
		Rel( wr );
		if( err )
		{
			char b[ 96 ];
			snprintf( b, sizeof( b ), "Media Foundation H.264: 0x%08x", (unsigned)hr );
			*err = b;
		}
		if( co_ ) CoUninitialize();
		co_ = false;
		return false;
	}
	wr_     = wr;
	stream_ = si;
	return true;
}
bool VideoWriter::Write( const uint8_t* rgba )
{
	IMFSinkWriter* wr = (IMFSinkWriter*)wr_;
	if( !wr ) return false;
	const DWORD n     = (DWORD)( w_ * h_ * 4 );
	IMFMediaBuffer* b = nullptr;
	IMFSample* s      = nullptr;
	HRESULT hr        = MFCreateMemoryBuffer( n, &b );
	BYTE* p           = nullptr;
	if( SUCCEEDED( hr ) ) hr = b->Lock( &p, nullptr, nullptr );
	if( SUCCEEDED( hr ) )
	{
		for( DWORD i = 0; i < n; i += 4 )
		{
			p[ i ]     = rgba[ i + 2 ];
			p[ i + 1 ] = rgba[ i + 1 ];
			p[ i + 2 ] = rgba[ i ];
			p[ i + 3 ] = 255;
		}
		b->Unlock();
		b->SetCurrentLength( n );
		hr = MFCreateSample( &s );
	}
	const LONGLONG t0 = (LONGLONG)frames_ * 10000000LL / fps_, t1 = (LONGLONG)( frames_ + 1 ) * 10000000LL / fps_;
	if( SUCCEEDED( hr ) ) hr = s->AddBuffer( b );
	if( SUCCEEDED( hr ) ) hr = s->SetSampleTime( t0 );
	if( SUCCEEDED( hr ) ) hr = s->SetSampleDuration( t1 - t0 );
	if( SUCCEEDED( hr ) ) hr = wr->WriteSample( stream_, s );
	Rel( s );
	Rel( b );
	if( SUCCEEDED( hr ) ) ++frames_;
	return SUCCEEDED( hr );
}
bool VideoWriter::Close()
{
	IMFSinkWriter* wr = (IMFSinkWriter*)wr_;
	if( !wr ) return false;
	const HRESULT hr = wr->Finalize();
	wr->Release();
	wr_ = nullptr;
	if( co_ ) CoUninitialize();
	co_ = false;
	return SUCCEEDED( hr );
}
double VideoFileSeconds( const std::string& path )
{
	HRESULT co = CoInitializeEx( nullptr, COINIT_MULTITHREADED );
	EnsureMf();
	double sec         = 0;
	IMFSourceReader* r = nullptr;
	if( SUCCEEDED( MFCreateSourceReaderFromURL( Widen( path ).c_str(), nullptr, &r ) ) )
	{
		PROPVARIANT v;
		PropVariantInit( &v );
		if( SUCCEEDED( r->GetPresentationAttribute( (DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v ) ) ) sec = v.uhVal.QuadPart / 1e7;
		PropVariantClear( &v );
		r->Release();
	}
	if( SUCCEEDED( co ) ) CoUninitialize();
	return sec;
}
}//namespace ls
