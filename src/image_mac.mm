// macOS image decoder for model textures (fallback after stb_image): ImageIO -> straight RGBA, rows top-down.
// Covers TIFF, HEIC, WebP (macOS 11+), JPEG 2000, BMP variants, PSD ... ; no DDS.
#include "mesh.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <algorithm>

namespace ls
{
bool DecodeImageOs( const uint8_t* data, size_t size, int& w, int& h, std::vector< uint8_t >& rgba )
{
	if( !data || size == 0 || size > 0x7fffffff ) return false;
	bool ok        = false;
	CFDataRef d    = CFDataCreate( nullptr, data, (CFIndex)size );
	CGImageSourceRef src = d ? CGImageSourceCreateWithData( d, nullptr ) : nullptr;
	CGImageRef img = src && CGImageSourceGetCount( src ) > 0 ? CGImageSourceCreateImageAtIndex( src, 0, nullptr ) : nullptr;
	if( img )
	{
		const size_t iw = CGImageGetWidth( img ), ih = CGImageGetHeight( img );
		if( iw > 0 && ih > 0 && (uint64_t)iw * ih < ( 1ull << 28 ) )
		{
			rgba.assign( iw * ih * 4, 0 );
			CGColorSpaceRef cs = CGColorSpaceCreateWithName( kCGColorSpaceSRGB );
			//8 bit RGBA bitmap contexts only exist premultiplied: draw, then undo the premultiplication (WIC delivers straight alpha)
			CGContextRef cx = CGBitmapContextCreate( rgba.data(), iw, ih, 8, iw * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big );
			if( cx )
			{
				CGContextSetBlendMode( cx, kCGBlendModeCopy );
				CGContextDrawImage( cx, CGRectMake( 0, 0, (CGFloat)iw, (CGFloat)ih ), img );//memory row 0 = top of the image
				CGContextRelease( cx );
				for( size_t i = 0; i < rgba.size(); i += 4 )
				{
					const int a = rgba[ i + 3 ];
					if( a == 0 || a == 255 ) continue;
					for( int c = 0; c < 3; ++c ) rgba[ i + c ] = (uint8_t)std::min( 255, ( rgba[ i + c ] * 255 + a / 2 ) / a );
				}
				w  = (int)iw;
				h  = (int)ih;
				ok = true;
			}
			if( cs ) CGColorSpaceRelease( cs );
		}
		CGImageRelease( img );
	}
	if( src ) CFRelease( src );
	if( d ) CFRelease( d );
	if( !ok ) rgba.clear();
	return ok;
}
}//namespace ls
