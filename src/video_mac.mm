// macOS backend of the video module: AVFoundation file decoder (AVAssetReader), H.264 writer (AVAssetWriter), duration.
// Compiled as Objective-C++ with ARC (see CMakeLists.txt).
#include "video.h"
#include "video_int.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ls
{
static NSURL* FileUrl( const std::string& path )
{
	NSString* s = [NSString stringWithUTF8String:path.c_str()];
	return s ? [NSURL fileURLWithPath:s] : nil;
}

//================================================================ decoder
class AvDecoder : public VideoDecoder
{
public:
	~AvDecoder() override
	{
		@autoreleasepool
		{
			[reader_ cancelReading];
			reader_ = nil;
			out_    = nil;
			track_  = nil;
			asset_  = nil;
		}
	}
	int Open( const std::string& path ) override
	{
		@autoreleasepool
		{
			NSURL* url = FileUrl( path );
			if( !url || ![[NSFileManager defaultManager] isReadableFileAtPath:url.path] ) return 1;
			asset_ = [AVURLAsset URLAssetWithURL:url options:@{ AVURLAssetPreferPreciseDurationAndTimingKey : @YES }];
			if( !asset_ ) return 1;
			//MACOS-UNTESTED: synchronous track / duration access (deprecated since macOS 13 in favour of the async load API, still works)
			NSArray< AVAssetTrack* >* tracks = [asset_ tracksWithMediaType:AVMediaTypeVideo];
			if( tracks.count == 0 ) return asset_.readable ? 2 : 1;
			track_          = tracks[ 0 ];
			const CGSize sz = track_.naturalSize;//rotation (preferredTransform) of phone clips is ignored, like on Windows
			w_              = (int)lround( fabs( sz.width ) );
			h_              = (int)lround( fabs( sz.height ) );
			fps_            = track_.nominalFrameRate;
			const double d  = CMTimeGetSeconds( asset_.duration );
			dur_            = std::isfinite( d ) ? d : 0;
			return StartReader( kCMTimeZero ) ? 0 : 2;
		}
	}
	int W() const override { return w_; }
	int H() const override { return h_; }
	double Fps() const override { return fps_; }
	double Duration() const override { return dur_; }
	int Read( std::vector< uint8_t >& bgra, double& ts ) override
	{
		@autoreleasepool
		{
			if( !reader_ || !out_ ) return -1;
			CMSampleBufferRef sb = [out_ copyNextSampleBuffer];
			if( !sb ) return reader_.status == AVAssetReaderStatusFailed ? -1 : 0;
			int ret             = 2;
			CVImageBufferRef ib = CMSampleBufferGetImageBuffer( sb );
			const double t      = CMTimeGetSeconds( CMSampleBufferGetPresentationTimeStamp( sb ) );
			if( ib && std::isfinite( t ) )
			{
				ts  = t;
				ret = 3;
				if( CVPixelBufferLockBaseAddress( ib, kCVPixelBufferLock_ReadOnly ) == kCVReturnSuccess )
				{
					const size_t w = CVPixelBufferGetWidth( ib ), h = CVPixelBufferGetHeight( ib ), bpr = CVPixelBufferGetBytesPerRow( ib );
					const uint8_t* p = (const uint8_t*)CVPixelBufferGetBaseAddress( ib );
					if( p && w && h && bpr >= w * 4 && !CVPixelBufferIsPlanar( ib ) )
					{
						w_ = (int)w;
						h_ = (int)h;
						bgra.resize( w * h * 4 );
						for( size_t y = 0; y < h; ++y ) memcpy( &bgra[ y * w * 4 ], p + y * bpr, w * 4 );//32BGRA rows are top-down
						ret = 1;
					}
					CVPixelBufferUnlockBaseAddress( ib, kCVPixelBufferLock_ReadOnly );
				}
			}
			CFRelease( sb );
			return ret;
		}
	}
	//an AVAssetReader cannot be repositioned: seeking creates a new reader for [sec, end]
	bool Seek( double sec ) override
	{
		@autoreleasepool
		{
			return StartReader( CMTimeMakeWithSeconds( std::max( 0.0, sec ), 600 ) );
		}
	}

private:
	bool StartReader( CMTime from )
	{
		[reader_ cancelReading];
		reader_ = nil;
		out_    = nil;
		if( !asset_ || !track_ ) return false;
		NSError* e = nil;
		reader_    = [AVAssetReader assetReaderWithAsset:asset_ error:&e];
		if( !reader_ ) return false;
		NSDictionary* fmt = @{ (id)kCVPixelBufferPixelFormatTypeKey : @( kCVPixelFormatType_32BGRA ) };
		out_              = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track_ outputSettings:fmt];
		out_.alwaysCopiesSampleData = NO;
		if( ![reader_ canAddOutput:out_] )
		{
			reader_ = nil;
			out_    = nil;
			return false;
		}
		[reader_ addOutput:out_];
		if( CMTimeCompare( from, kCMTimeZero ) > 0 ) reader_.timeRange = CMTimeRangeMake( from, kCMTimePositiveInfinity );
		if( ![reader_ startReading] )
		{
			reader_ = nil;
			out_    = nil;
			return false;
		}
		return true;
	}
	AVURLAsset* asset_               = nil;
	AVAssetTrack* track_             = nil;
	AVAssetReader* reader_           = nil;
	AVAssetReaderTrackOutput* out_   = nil;
	int w_ = 0, h_ = 0;
	double fps_ = 0, dur_ = 0;
};
std::unique_ptr< VideoDecoder > MakeVideoDecoder() { return std::unique_ptr< VideoDecoder >( new AvDecoder ); }

//================================================================ writer
namespace
{
struct AvWriter
{
	AVAssetWriter* w                          = nil;
	AVAssetWriterInput* in                    = nil;
	AVAssetWriterInputPixelBufferAdaptor* ad  = nil;
};
}//namespace

VideoWriter::~VideoWriter()
{
	if( wr_ ) Close();
}
bool VideoWriter::Open( const std::string& path, int w, int h, int fps, int kbps, std::string* err )
{
	@autoreleasepool
	{
		w_      = w & ~1;
		h_      = h & ~1;
		fps_    = std::max( 1, fps );
		frames_ = 0;
		co_     = false;
		NSURL* url = FileUrl( path );
		if( !url || w_ <= 0 || h_ <= 0 )
		{
			if( err ) *err = "AVAssetWriter: invalid path or size";
			return false;
		}
		[[NSFileManager defaultManager] removeItemAtURL:url error:nil];
		NSError* e = nil;
		auto* aw   = new AvWriter;
		aw->w      = [AVAssetWriter assetWriterWithURL:url fileType:AVFileTypeMPEG4 error:&e];
		if( aw->w )
		{
			//same settings as the Media Foundation writer: H.264, average bit rate kbps (at least 500), fps
			NSDictionary* comp = @{
				AVVideoAverageBitRateKey : @( (long long)std::max( 500, kbps ) * 1000 ),
				AVVideoExpectedSourceFrameRateKey : @( fps_ ),
			};
			NSDictionary* set = @{
				AVVideoCodecKey : AVVideoCodecTypeH264,
				AVVideoWidthKey : @( w_ ),
				AVVideoHeightKey : @( h_ ),
				AVVideoCompressionPropertiesKey : comp,
			};
			aw->in                            = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:set];
			aw->in.expectsMediaDataInRealTime = NO;
			NSDictionary* pb = @{
				(id)kCVPixelBufferPixelFormatTypeKey : @( kCVPixelFormatType_32BGRA ),
				(id)kCVPixelBufferWidthKey : @( w_ ),
				(id)kCVPixelBufferHeightKey : @( h_ ),
			};
			aw->ad = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:aw->in sourcePixelBufferAttributes:pb];
		}
		bool ok = aw->w && aw->in && aw->ad && [aw->w canAddInput:aw->in];
		if( ok )
		{
			[aw->w addInput:aw->in];
			ok = [aw->w startWriting];
		}
		if( !ok )
		{
			if( err )
			{
				NSError* we = e ? e : aw->w.error;
				*err        = std::string( "AVAssetWriter H.264: " ) + ( we ? we.localizedDescription.UTF8String : "cannot start" );
			}
			delete aw;
			return false;
		}
		[aw->w startSessionAtSourceTime:kCMTimeZero];
		wr_     = aw;
		stream_ = 0;
		return true;
	}
}
bool VideoWriter::Write( const uint8_t* rgba )
{
	auto* aw = (AvWriter*)wr_;
	if( !aw ) return false;
	@autoreleasepool
	{
		//not real time: wait until the encoder takes more data (bounded, a stuck encoder must not hang the export)
		for( int i = 0; i < 5000 && !aw->in.readyForMoreMediaData; ++i )
		{
			if( aw->w.status != AVAssetWriterStatusWriting ) return false;
			MediaSleepMs( 1 );
		}
		if( !aw->in.readyForMoreMediaData ) return false;
		CVPixelBufferRef pb = nullptr;
		if( !aw->ad.pixelBufferPool || CVPixelBufferPoolCreatePixelBuffer( kCFAllocatorDefault, aw->ad.pixelBufferPool, &pb ) != kCVReturnSuccess || !pb ) return false;
		CVPixelBufferLockBaseAddress( pb, 0 );
		uint8_t* p       = (uint8_t*)CVPixelBufferGetBaseAddress( pb );
		const size_t bpr = CVPixelBufferGetBytesPerRow( pb );
		for( int y = 0; y < h_; ++y )
		{
			const uint8_t* s = rgba + (size_t)y * w_ * 4;
			uint8_t* d       = p + (size_t)y * bpr;
			for( int x = 0; x < w_; ++x, s += 4, d += 4 )
			{
				d[ 0 ] = s[ 2 ];
				d[ 1 ] = s[ 1 ];
				d[ 2 ] = s[ 0 ];
				d[ 3 ] = 255;
			}
		}
		CVPixelBufferUnlockBaseAddress( pb, 0 );
		const BOOL ok = [aw->ad appendPixelBuffer:pb withPresentationTime:CMTimeMake( frames_, fps_ )];
		CVPixelBufferRelease( pb );
		if( ok ) ++frames_;
		return ok;
	}
}
bool VideoWriter::Close()
{
	auto* aw = (AvWriter*)wr_;
	if( !aw ) return false;
	bool ok = false;
	@autoreleasepool
	{
		[aw->in markAsFinished];
		if( frames_ > 0 ) [aw->w endSessionAtSourceTime:CMTimeMake( frames_, fps_ )];
		dispatch_semaphore_t sem = dispatch_semaphore_create( 0 );
		[aw->w finishWritingWithCompletionHandler:^{
			dispatch_semaphore_signal( sem );
		}];
		dispatch_semaphore_wait( sem, dispatch_time( DISPATCH_TIME_NOW, (int64_t)60 * NSEC_PER_SEC ) );
		ok = aw->w.status == AVAssetWriterStatusCompleted;
		if( !ok && aw->w.error ) Log( "AVAssetWriter: %s", aw->w.error.localizedDescription.UTF8String );
	}
	delete aw;
	wr_ = nullptr;
	return ok;
}

//same test clip as on Windows (top half red, bottom half blue, moving white bar), H.264 .mp4 only (no WMV on macOS)
bool WriteTestVideo( std::string& path, int w, int h, int frames )
{
	const std::string file = path + ".mp4";
	VideoWriter vw;
	std::string err;
	if( !vw.Open( file, w, h, 30, 3000, &err ) )
	{
		Log( "test video %s: %s", file.c_str(), err.c_str() );
		return false;
	}
	w = w & ~1;
	h = h & ~1;
	std::vector< uint8_t > px( (size_t)w * h * 4 );
	for( int f = 0; f < frames; ++f )
	{
		int bx = ( f * 8 ) % std::max( 1, w - 16 );
		for( int y = 0; y < h; ++y )
			for( int x = 0; x < w; ++x )
			{
				uint8_t* q = &px[ ( (size_t)y * w + x ) * 4 ];
				bool top   = y < h / 2;
				bool bar   = x >= bx && x < bx + 16 && y > h * 3 / 8 && y < h * 5 / 8;
				q[ 0 ]     = bar ? 255 : ( top ? 235 : 20 );//R
				q[ 1 ]     = bar ? 255 : 20;                //G
				q[ 2 ]     = bar ? 255 : ( top ? 20 : 235 );//B
				q[ 3 ]     = 255;
			}
		if( !vw.Write( px.data() ) ) return false;
	}
	if( !vw.Close() ) return false;
	path = file;
	return true;
}

double VideoFileSeconds( const std::string& path )
{
	@autoreleasepool
	{
		NSURL* url = FileUrl( path );
		if( !url || ![[NSFileManager defaultManager] isReadableFileAtPath:url.path] ) return 0;
		AVURLAsset* a = [AVURLAsset URLAssetWithURL:url options:@{ AVURLAssetPreferPreciseDurationAndTimingKey : @YES }];
		if( !a || !a.readable ) return 0;
		const double d = CMTimeGetSeconds( a.duration );//MACOS-UNTESTED: synchronous (deprecated) duration access
		return std::isfinite( d ) && d > 0 ? d : 0;
	}
}
}//namespace ls
