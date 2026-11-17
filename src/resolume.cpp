//Resolume Arena "Advanced Output" preset from the projector / screen setup.
//Preset format as written by Grosserd SliceForge (checked against Arena 7.28): pixel coordinates,
//origin top left; warps are bicubic Bezier nets of (3n+1) x (3m+1) control points.
#include "resolume.h"
#include "i18n.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <unordered_map>
#include <windows.h>
#include <shlobj.h>

namespace ls
{
//================================================================ geometry
V3 ScreenPoint( const Screen& s, float u, float v )
{
	const ScreenGeo g = GetScreenGeo( s );
	return g.centre + g.right * ( ( u - 0.5f ) * s.width ) + g.up * ( ( 0.5f - v ) * s.height );
}

bool ProjectorPixel( const Projector& p, int w, int h, const V3& x, float& px, float& py )
{
	ProjectorGeo g = GetProjectorGeo( p, w, h );
	const M4 B     = Mul( Translate( p.pos ), RotEuler( V3( p.tilt, p.pan, p.roll ) ) );
	g.o            = XformP( B, V3( 0, 0, -0.155f ) );//lens, as in the renderer
	const V3 d     = x - g.o;
	const float z  = Dot( d, g.dir );
	if( z <= 1e-4f ) return false;
	const float sx = Dot( d, g.right ) / z, sy = Dot( d, g.up ) / z;
	float u = ( sx - g.l ) / ( g.r - g.l ), v = ( sy - g.b ) / ( g.t - g.b );
	if( p.rear ) u = 1.0f - u;
	//letterbox when the picture has another aspect than the projector format (as the renderer does)
	const float srcA = w > 0 && h > 0 ? (float)w / (float)h : g.aspect;
	const float ex   = srcA < g.aspect ? srcA / g.aspect : 1.0f;
	const float ey   = srcA > g.aspect ? g.aspect / srcA : 1.0f;
	u  = ( u - 0.5f ) / ex + 0.5f;
	v  = ( v - 0.5f ) / ey + 0.5f;
	px = u * (float)w;
	py = ( 1.0f - v ) * (float)h;
	return true;
}

void ResolumeOutputSize( const Projector& p, int fw, int fh, int& w, int& h )
{
	static const int s[ 6 ][ 2 ] = { { 1920, 1080 }, { 1920, 1200 }, { 1600, 1200 }, { 2560, 1080 }, { 1080, 1080 }, { 1920, 1080 } };
	if( fw > 0 && fh > 0 && ( p.input == VideoInput::Ndi || p.format == 5 ) )
	{
		w = fw;
		h = fh;
		return;
	}
	const int f = p.format >= 0 && p.format < 6 ? p.format : 0;
	w           = s[ f ][ 0 ];
	h           = s[ f ][ 1 ];
}

static std::string Trim( std::string s )
{
	while( !s.empty() && (unsigned char)s.back() <= ' ' ) s.pop_back();
	size_t a = 0;
	while( a < s.size() && (unsigned char)s[ a ] <= ' ' ) ++a;
	return s.substr( a );
}

std::string ResolumeScreenName( const Projector& p )
{
	if( p.input == VideoInput::Ndi )
	{
		const size_t a = p.ndi.rfind( '(' ), b = p.ndi.rfind( ')' );
		if( a != std::string::npos && b != std::string::npos && b > a )
		{
			std::string in = p.ndi.substr( a + 1, b - a - 1 );
			for( const char* pre : { "Arena - ", "Avenue - ", "Arena ", "Avenue " } )
				if( in.rfind( pre, 0 ) == 0 )
				{
					in = in.substr( strlen( pre ) );
					break;
				}
			in = Trim( in );
			if( !in.empty() && in != "Composition" ) return in;
		}
	}
	const std::string n = Trim( p.name );
	return n.empty() ? std::string( "Projector" ) : n;
}

//================================================================ bezier net
static void Interp4( const float* p0, const float* q1, const float* q2, const float* p3, float out[ 4 ][ 2 ] )
{
	for( int k = 0; k < 2; ++k )
	{
		out[ 0 ][ k ] = p0[ k ];
		out[ 1 ][ k ] = ( -5 * p0[ k ] + 18 * q1[ k ] - 9 * q2[ k ] + 2 * p3[ k ] ) / 6;
		out[ 2 ][ k ] = ( 2 * p0[ k ] - 9 * q1[ k ] + 18 * q2[ k ] - 5 * p3[ k ] ) / 6;
		out[ 3 ][ k ] = p3[ k ];
	}
}
//samples (w x h, x y pairs) -> bezier controls that interpolate the samples at t = 0, 1/3, 2/3, 1
static std::vector< float > SamplesToBezier( const std::vector< float >& s, int w, int h )
{
	std::vector< float > rows( s.size() ), out( s.size() );
	float r[ 4 ][ 2 ];
	for( int j = 0; j < h; ++j )
		for( int i = 0; i + 3 < w; i += 3 )
		{
			const size_t b = (size_t)j * w + i;
			Interp4( &s[ b * 2 ], &s[ ( b + 1 ) * 2 ], &s[ ( b + 2 ) * 2 ], &s[ ( b + 3 ) * 2 ], r );
			for( int k = 0; k < 4; ++k ) { rows[ ( b + k ) * 2 ] = r[ k ][ 0 ]; rows[ ( b + k ) * 2 + 1 ] = r[ k ][ 1 ]; }
		}
	for( int i = 0; i < w; ++i )
		for( int j = 0; j + 3 < h; j += 3 )
		{
			auto at = [ & ]( int jj ) { return &rows[ ( (size_t)jj * w + i ) * 2 ]; };
			Interp4( at( j ), at( j + 1 ), at( j + 2 ), at( j + 3 ), r );
			for( int k = 0; k < 4; ++k )
			{
				const size_t o = ( (size_t)( j + k ) * w + i ) * 2;
				out[ o ]       = r[ k ][ 0 ];
				out[ o + 1 ]   = r[ k ][ 1 ];
			}
		}
	return out;
}
static void Bern( float t, float b[ 4 ] )
{
	const float s = 1 - t;
	b[ 0 ]        = s * s * s;
	b[ 1 ]        = 3 * t * s * s;
	b[ 2 ]        = 3 * t * t * s;
	b[ 3 ]        = t * t * t;
}
static void EvalPatch( const std::vector< float >& net, int w, int pi, int pj, float u, float v, float& x, float& y )
{
	float bu[ 4 ], bv[ 4 ];
	Bern( u, bu );
	Bern( v, bv );
	x = y = 0;
	for( int b = 0; b < 4; ++b )
		for( int a = 0; a < 4; ++a )
		{
			const size_t k  = ( (size_t)( pj * 3 + b ) * w + pi * 3 + a ) * 2;
			const float wgt = bu[ a ] * bv[ b ];
			x += net[ k ] * wgt;
			y += net[ k + 1 ] * wgt;
		}
}
void ResolumeEvalNet( const ResolumeSlice& sl, float u, float v, float& x, float& y )
{
	const int nx = std::max( 1, ( sl.gw - 1 ) / 3 ), ny = std::max( 1, ( sl.gh - 1 ) / 3 );
	const float fu = std::clamp( u, 0.0f, 1.0f ) * nx, fv = std::clamp( v, 0.0f, 1.0f ) * ny;
	const int pi = std::min( nx - 1, (int)fu ), pj = std::min( ny - 1, (int)fv );
	EvalPatch( sl.net, sl.gw, pi, pj, fu - pi, fv - pj, x, y );
}

using MapFn = std::function< bool( float u, float v, float& x, float& y ) >;
//net with nx * ny patches; false = a sample is behind the lens
static bool FitNet( const MapFn& f, int nx, int ny, ResolumeSlice& sl )
{
	const int w = 3 * nx + 1, h = 3 * ny + 1;
	std::vector< float > s( (size_t)w * h * 2 );
	for( int j = 0; j < h; ++j )
		for( int i = 0; i < w; ++i )
			if( !f( (float)i / ( w - 1 ), (float)j / ( h - 1 ), s[ ( (size_t)j * w + i ) * 2 ], s[ ( (size_t)j * w + i ) * 2 + 1 ] ) ) return false;
	sl.gw     = w;
	sl.gh     = h;
	sl.net    = SamplesToBezier( s, w, h );
	sl.maxErr = 0;
	static const float tp[ 9 ][ 2 ] = { { 1 / 6.f, 1 / 6.f }, { 0.5f, 0.5f }, { 5 / 6.f, 1 / 6.f }, { 1 / 6.f, 5 / 6.f }, { 5 / 6.f, 5 / 6.f }, { 0.5f, 1 / 6.f }, { 1 / 6.f, 0.5f }, { 0.5f, 5 / 6.f }, { 5 / 6.f, 0.5f } };
	for( int pj = 0; pj < ny; ++pj )
		for( int pi = 0; pi < nx; ++pi )
			for( auto& t : tp )
			{
				float ex, ey, bx, by;
				if( !f( ( pi + t[ 0 ] ) / nx, ( pj + t[ 1 ] ) / ny, ex, ey ) ) continue;
				EvalPatch( sl.net, w, pi, pj, t[ 0 ], t[ 1 ], bx, by );
				sl.maxErr = std::max( sl.maxErr, hypotf( bx - ex, by - ey ) );
			}
	return true;
}
static bool AdaptiveNet( const MapFn& f, float aspect, float tol, ResolumeSlice& sl )
{
	const int maxP = 16;
	for( int n = 1; n <= maxP; ++n )
	{
		const int nx = std::clamp( (int)lroundf( aspect >= 1 ? (float)n : n * aspect ), 1, maxP );
		const int ny = std::clamp( (int)lroundf( aspect >= 1 ? n / aspect : (float)n ), 1, maxP );
		if( !FitNet( f, nx, ny, sl ) ) return false;
		if( sl.maxErr <= tol ) break;
	}
	return true;
}

//================================================================ screen shape -> polygons (marching squares)
struct Pt
{
	float x, y;
};
using Loop = std::vector< Pt >;

static std::vector< Loop > TraceContours( const std::vector< uint8_t >& m, int w, int h )
{
	auto at = [ & ]( int x, int y ) -> int { return x < 0 || y < 0 || x >= w || y >= h ? 0 : m[ (size_t)y * w + x ]; };
	struct Seg { float x0, y0, x1, y1; };
	std::vector< Seg > segs;
	std::unordered_map< int64_t, size_t > idx;
	auto key = []( float x, float y ) -> int64_t { return ( (int64_t)lroundf( x * 2 ) + 16 ) * 1000003LL + ( (int64_t)lroundf( y * 2 ) + 16 ); };
	auto add = [ & ]( Pt p, Pt q, Pt ic, bool inside )
	{
		const float cr = ( q.x - p.x ) * ( ic.y - p.y ) - ( q.y - p.y ) * ( ic.x - p.x );
		if( ( cr > 0 ) != inside ) std::swap( p, q );
		const int64_t k = key( p.x, p.y );
		auto it         = idx.find( k );
		if( it != idx.end() ) segs[ it->second ] = { p.x, p.y, q.x, q.y };
		else
		{
			idx[ k ] = segs.size();
			segs.push_back( { p.x, p.y, q.x, q.y } );
		}
	};
	for( int cy = -1; cy < h; ++cy )
		for( int cx = -1; cx < w; ++cx )
		{
			const int tl = at( cx, cy ), tr = at( cx + 1, cy ), br = at( cx + 1, cy + 1 ), bl = at( cx, cy + 1 );
			const int c = tl * 8 + tr * 4 + br * 2 + bl;
			if( c == 0 || c == 15 ) continue;
			const float X0 = cx + 0.5f, Y0 = cy + 0.5f, X1 = X0 + 1, Y1 = Y0 + 1;
			const Pt T{ X0 + 0.5f, Y0 }, R{ X1, Y0 + 0.5f }, B{ X0 + 0.5f, Y1 }, Lm{ X0, Y0 + 0.5f }, TL{ X0, Y0 };
			const bool tlIn = tl != 0;
			switch( c )
			{
				case 8: case 7: add( T, Lm, TL, tlIn ); break;
				case 4: case 11: add( T, R, TL, tlIn ); break;
				case 2: case 13: add( R, B, TL, tlIn ); break;
				case 1: case 14: add( B, Lm, TL, tlIn ); break;
				case 12: case 3: add( Lm, R, TL, tlIn ); break;
				case 9: case 6: add( T, B, TL, tlIn ); break;
				case 10: add( T, R, TL, true ); add( B, Lm, TL, true ); break;
				case 5: add( T, Lm, TL, false ); add( R, B, TL, false ); break;
			}
		}
	std::vector< Loop > loops;
	std::vector< uint8_t > used( segs.size(), 0 );
	for( size_t i0 = 0; i0 < segs.size(); ++i0 )
	{
		if( used[ i0 ] ) continue;
		Loop loop;
		size_t i  = i0;
		int guard = 0;
		while( !used[ i ] && guard++ < 10000000 )
		{
			used[ i ] = 1;
			loop.push_back( { segs[ i ].x0, segs[ i ].y0 } );
			auto it = idx.find( key( segs[ i ].x1, segs[ i ].y1 ) );
			if( it == idx.end() ) break;
			i = it->second;
		}
		if( loop.size() >= 3 ) loops.push_back( std::move( loop ) );
	}
	return loops;
}
static float SignedArea( const Loop& p )
{
	double a = 0;
	for( size_t i = 0, n = p.size(); i < n; ++i )
	{
		const Pt &a0 = p[ i ], &a1 = p[ ( i + 1 ) % n ];
		a += (double)a0.x * a1.y - (double)a1.x * a0.y;
	}
	return (float)( a / 2 );
}
static bool PointInPoly( Pt p, const Loop& poly )
{
	bool in = false;
	for( size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++ )
	{
		const Pt &a = poly[ i ], &b = poly[ j ];
		if( ( a.y > p.y ) != ( b.y > p.y ) && p.x < ( b.x - a.x ) * ( p.y - a.y ) / ( b.y - a.y ) + a.x ) in = !in;
	}
	return in;
}
struct Shape
{
	Loop outer;
	float area = 0;
	std::vector< Loop > holes;
};
static std::vector< Shape > BuildShapes( const std::vector< Loop >& loops, float minArea )
{
	std::vector< Shape > shapes;
	std::vector< std::pair< const Loop*, float > > holes;
	for( auto& l : loops )
	{
		const float a = SignedArea( l );
		if( fabsf( a ) < minArea ) continue;
		if( a > 0 ) shapes.push_back( { l, a, {} } );
		else holes.push_back( { &l, -a } );
	}
	std::sort( shapes.begin(), shapes.end(), []( const Shape& a, const Shape& b ) { return a.area < b.area; } );
	for( auto& h : holes )
		for( auto& s : shapes )
			if( s.area > h.second && PointInPoly( ( *h.first )[ 0 ], s.outer ) )
			{
				s.holes.push_back( *h.first );
				break;
			}
	return shapes;
}
static Loop SmoothClosed( const Loop& in, int it )
{
	Loop p = in;
	for( int k = 0; k < it; ++k )
	{
		const size_t n = p.size();
		Loop q( n );
		for( size_t i = 0; i < n; ++i )
		{
			const Pt &a = p[ ( i + n - 1 ) % n ], &b = p[ i ], &c = p[ ( i + 1 ) % n ];
			q[ i ]      = { ( a.x + 2 * b.x + c.x ) / 4, ( a.y + 2 * b.y + c.y ) / 4 };
		}
		p.swap( q );
	}
	return p;
}
static Loop Dp( const Loop& pts, float eps )
{
	const size_t n = pts.size();
	if( n < 3 ) return pts;
	std::vector< uint8_t > keep( n, 0 );
	keep[ 0 ] = keep[ n - 1 ] = 1;
	std::vector< std::pair< size_t, size_t > > st{ { 0, n - 1 } };
	while( !st.empty() )
	{
		auto [ s, e ] = st.back();
		st.pop_back();
		const Pt a = pts[ s ], b = pts[ e ];
		const float dx = b.x - a.x, dy = b.y - a.y, len = std::max( 1e-9f, hypotf( dx, dy ) );
		float md = -1;
		size_t mi = 0;
		for( size_t i = s + 1; i < e; ++i )
		{
			const float d = fabsf( dy * pts[ i ].x - dx * pts[ i ].y + b.x * a.y - b.y * a.x ) / len;
			if( d > md ) { md = d; mi = i; }
		}
		if( md > eps )
		{
			keep[ mi ] = 1;
			st.push_back( { s, mi } );
			st.push_back( { mi, e } );
		}
	}
	Loop r;
	for( size_t i = 0; i < n; ++i )
		if( keep[ i ] ) r.push_back( pts[ i ] );
	return r;
}
static Loop SimplifyClosed( const Loop& pts, float eps )
{
	if( pts.size() <= 4 || eps <= 0 ) return pts;
	size_t i0 = 0, i1 = 0;
	float best = -1;
	for( size_t i = 0; i < pts.size(); ++i )
	{
		const float d = ( pts[ i ].x - pts[ 0 ].x ) * ( pts[ i ].x - pts[ 0 ].x ) + ( pts[ i ].y - pts[ 0 ].y ) * ( pts[ i ].y - pts[ 0 ].y );
		if( d > best ) { best = d; i1 = i; }
	}
	best = -1;
	for( size_t i = 0; i < pts.size(); ++i )
	{
		const float d = ( pts[ i ].x - pts[ i1 ].x ) * ( pts[ i ].x - pts[ i1 ].x ) + ( pts[ i ].y - pts[ i1 ].y ) * ( pts[ i ].y - pts[ i1 ].y );
		if( d > best ) { best = d; i0 = i; }
	}
	const size_t a = std::min( i0, i1 ), b = std::max( i0, i1 );
	Loop pa( pts.begin() + a, pts.begin() + b + 1 ), pb( pts.begin() + b, pts.end() );
	pb.insert( pb.end(), pts.begin(), pts.begin() + a + 1 );
	pa = Dp( pa, eps );
	pb = Dp( pb, eps );
	Loop r( pa.begin(), pa.end() - 1 );
	r.insert( r.end(), pb.begin(), pb.end() - 1 );
	return r;
}
static bool SegX( Pt a, Pt b, Pt c, Pt d )
{
	auto o = []( Pt p, Pt q, Pt r ) { const float v = ( q.x - p.x ) * ( r.y - p.y ) - ( q.y - p.y ) * ( r.x - p.x ); return v > 0 ? 1 : v < 0 ? -1 : 0; };
	return o( a, b, c ) * o( a, b, d ) < 0 && o( c, d, a ) * o( c, d, b ) < 0;
}
static bool CrossesAny( Pt a, Pt b, const Loop& poly, long skip1 = -1, long skip2 = -1 )
{
	const long n = (long)poly.size();
	for( long i = 0; i < n; ++i )
	{
		const long j = ( i + 1 ) % n;
		if( i == skip1 || j == skip1 || i == skip2 || j == skip2 ) continue;
		if( SegX( a, b, poly[ i ], poly[ j ] ) ) return true;
	}
	return false;
}
//joins the holes into the outline with zero-width cuts ("keyholes") -> one simple contour
static Loop Keyhole( const Loop& outer, std::vector< Loop > holes )
{
	Loop poly = outer;
	auto maxX = []( const Loop& l ) { float m = -1e30f; for( auto& p : l ) m = std::max( m, p.x ); return m; };
	std::sort( holes.begin(), holes.end(), [ & ]( const Loop& a, const Loop& b ) { return maxX( a ) > maxX( b ); } );
	for( auto& h : holes )
	{
		size_t hi = 0;
		for( size_t i = 1; i < h.size(); ++i )
			if( h[ i ].x > h[ hi ].x ) hi = i;
		const Pt hp = h[ hi ];
		std::vector< std::pair< size_t, float > > cand;
		for( size_t i = 0; i < poly.size(); ++i ) cand.push_back( { i, ( poly[ i ].x - hp.x ) * ( poly[ i ].x - hp.x ) + ( poly[ i ].y - hp.y ) * ( poly[ i ].y - hp.y ) } );
		std::sort( cand.begin(), cand.end(), []( auto& a, auto& b ) { return a.second < b.second; } );
		size_t target = cand[ 0 ].first;
		for( size_t c = 0; c < cand.size() && c < 64; ++c )
		{
			const size_t i = cand[ c ].first;
			if( !CrossesAny( hp, poly[ i ], poly, (long)i ) && !CrossesAny( hp, poly[ i ], h, -1, (long)hi ) )
			{
				target = i;
				break;
			}
		}
		Loop np( poly.begin(), poly.begin() + target + 1 );
		np.insert( np.end(), h.begin() + hi, h.end() );
		np.insert( np.end(), h.begin(), h.begin() + hi + 1 );
		np.push_back( poly[ target ] );
		np.insert( np.end(), poly.begin() + target + 1, poly.end() );
		poly.swap( np );
	}
	return poly;
}

std::vector< std::vector< float > > ScreenMaskPolys( const Screen& s )
{
	std::vector< std::vector< float > > out;
	if( !s.HasMask() ) return out;
	const int n = s.maskN, P = 2, W = n + 2 * P;//black frame around the square: outside the screen counts as black
	std::vector< uint8_t > m( (size_t)W * W, 1 );
	bool anyBlack = false;
	for( int y = 0; y < n; ++y )
		for( int x = 0; x < n; ++x )
		{
			const bool white                   = s.mask[ (size_t)y * n + x ] != 0;
			m[ (size_t)( y + P ) * W + x + P ] = white ? 0 : 1;
			anyBlack |= !white;
		}
	if( !anyBlack ) return out;
	auto proc = []( const Loop& l )
	{
		Loop r = SimplifyClosed( SmoothClosed( l, 2 ), 0.6f );
		return r.size() < 3 || SignedArea( r ) == 0 ? l : r;
	};
	for( auto& sh : BuildShapes( TraceContours( m, W, W ), 1.0f ) )
	{
		std::vector< Loop > holes;
		for( auto& h : sh.holes )
		{
			Loop q = proc( h );
			if( q.size() >= 3 ) holes.push_back( std::move( q ) );
		}
		const Loop poly = holes.empty() ? proc( sh.outer ) : Keyhole( proc( sh.outer ), holes );
		std::vector< float > uv;
		uv.reserve( poly.size() * 2 );
		for( auto& p : poly )
		{
			uv.push_back( ( p.x - P ) / n );
			uv.push_back( ( p.y - P ) / n );
		}
		out.push_back( std::move( uv ) );
	}
	return out;
}

//================================================================ layout
std::vector< ResolumeScreen > BuildResolumeLayout( const Show& show, const ResolumeOpts& o, const FrameSizeFn& frame )
{
	std::vector< ResolumeScreen > out;
	std::map< std::string, int > names;
	const int compW = std::max( 16, o.compW ), compH = std::max( 16, o.compH );
	for( int pi = 0; pi < (int)show.projectors.size(); ++pi )
	{
		if( pi < (int)o.skip.size() && o.skip[ pi ] ) continue;
		const Projector& p = show.projectors[ pi ];
		int fw = 0, fh = 0;
		if( frame ) frame( p.uid, fw, fh );
		ResolumeScreen rs;
		rs.projector = pi;
		rs.name      = ResolumeScreenName( p );
		if( int k = ++names[ rs.name ]; k > 1 ) rs.name += " " + std::to_string( k );
		ResolumeOutputSize( p, fw, fh, rs.w, rs.h );
		const int W = rs.w, H = rs.h;
		for( int si = 0; si < (int)show.screens.size(); ++si )
		{
			const Screen& s = show.screens[ si ];
			//does the projector reach this screen?
			const int S = 24;
			int tested = 0, inside = 0, behind = 0;
			for( int j = 0; j <= S; ++j )
				for( int i = 0; i <= S; ++i )
				{
					const float u = (float)i / S, v = (float)j / S;
					if( s.HasMask() && !s.MaskAt( std::min( u, 0.9999f ), std::min( v, 0.9999f ) ) ) continue;
					++tested;
					float x, y;
					if( !ProjectorPixel( p, W, H, ScreenPoint( s, u, v ), x, y ) ) { ++behind; continue; }
					if( x >= 0 && x <= W && y >= 0 && y <= H ) ++inside;
				}
			if( inside == 0 ) continue;
			char b[ 512 ];
			MapFn f = [ & ]( float u, float v, float& x, float& y ) { return ProjectorPixel( p, W, H, ScreenPoint( s, u, v ), x, y ); };
			ResolumeSlice sl;
			bool cornersOk = true;
			static const float cu[ 4 ] = { 0, 1, 1, 0 }, cv[ 4 ] = { 0, 0, 1, 1 };
			for( int k = 0; k < 4; ++k ) cornersOk &= f( cu[ k ], cv[ k ], sl.corners[ k * 2 ], sl.corners[ k * 2 + 1 ] );
			if( behind > 0 || !cornersOk )
			{
				snprintf( b, sizeof( b ), L( "Leinwand \"%s\" liegt teilweise hinter dem Projektor und wurde ausgelassen." ), s.name.c_str() );
				rs.notes.push_back( b );
				continue;
			}
			const float* c = sl.corners;
			const float pw = 0.5f * ( hypotf( c[ 2 ] - c[ 0 ], c[ 3 ] - c[ 1 ] ) + hypotf( c[ 4 ] - c[ 6 ], c[ 5 ] - c[ 7 ] ) );
			const float ph = 0.5f * ( hypotf( c[ 6 ] - c[ 0 ], c[ 7 ] - c[ 1 ] ) + hypotf( c[ 4 ] - c[ 2 ], c[ 5 ] - c[ 3 ] ) );
			if( !AdaptiveNet( f, pw / std::max( 1.0f, ph ), o.tol, sl ) )
			{
				snprintf( b, sizeof( b ), L( "Leinwand \"%s\" liegt teilweise hinter dem Projektor und wurde ausgelassen." ), s.name.c_str() );
				rs.notes.push_back( b );
				continue;
			}
			sl.name   = s.name;
			sl.screen = si;
			//input: the part of the composition that should land on the screen
			const float a = s.width / std::max( 1e-3f, s.height ), A = (float)compW / compH;
			if( o.input == 1 ) { sl.in[ 0 ] = 0; sl.in[ 1 ] = 0; sl.in[ 2 ] = (float)compW; sl.in[ 3 ] = (float)compH; }
			else if( a >= A )
			{
				const float hh = compW / a;
				sl.in[ 0 ] = 0; sl.in[ 2 ] = (float)compW;
				sl.in[ 1 ] = ( compH - hh ) * 0.5f; sl.in[ 3 ] = ( compH + hh ) * 0.5f;
			}
			else
			{
				const float ww = compH * a;
				sl.in[ 1 ] = 0; sl.in[ 3 ] = (float)compH;
				sl.in[ 0 ] = ( compW - ww ) * 0.5f; sl.in[ 2 ] = ( compW + ww ) * 0.5f;
			}
			if( o.masks && s.HasMask() )
				for( auto& poly : ScreenMaskPolys( s ) )
				{
					std::vector< float > op;
					bool ok = true;
					for( size_t k = 0; k + 1 < poly.size() && ok; k += 2 )
					{
						float x, y;
						ok = f( poly[ k ], poly[ k + 1 ], x, y );
						op.push_back( x );
						op.push_back( y );
					}
					if( ok && op.size() >= 6 ) sl.masks.push_back( std::move( op ) );
				}
			if( inside < tested )
			{
				snprintf( b, sizeof( b ), L( "Leinwand \"%s\" ist nur teilweise im Projektorbild." ), s.name.c_str() );
				rs.notes.push_back( b );
			}
			rs.slices.push_back( std::move( sl ) );
		}
		out.push_back( std::move( rs ) );
	}
	return out;
}

//================================================================ XML
static std::string F( double n )
{
	if( !std::isfinite( n ) ) n = 0;
	const double r = std::round( n * 1e4 ) / 1e4;
	char b[ 64 ];
	snprintf( b, sizeof( b ), "%.4f", r );
	std::string s = b;
	if( s.find( '.' ) != std::string::npos )
	{
		while( s.back() == '0' ) s.pop_back();
		if( s.back() == '.' ) s.pop_back();
	}
	if( s == "-0" ) s = "0";
	return s;
}
static std::string Esc( const std::string& s )
{
	std::string r;
	for( char c : s )
	{
		if( c == '&' ) r += "&amp;";
		else if( c == '<' ) r += "&lt;";
		else if( c == '>' ) r += "&gt;";
		else if( c == '"' ) r += "&quot;";
		else r += c;
	}
	return r;
}
static std::string Uid()
{
	static std::mt19937_64 rng( std::random_device{}() );
	static uint64_t n = 0;
	return std::to_string( 1790000000000ULL + rng() % 9000000000ULL + n++ );
}
static std::string Range( const char* name, double def, double val, double mn, double mx, const char* alt = nullptr )
{
	std::string s = std::string( "<ParamRange name=\"" ) + name + "\"";
	if( alt ) s += std::string( " altName=\"" ) + alt + "\"";
	s += " T=\"DOUBLE\" default=\"" + F( def ) + "\" value=\"" + F( val ) + "\">\n";
	s += "<PhaseSourceStatic name=\"PhaseSourceStatic\"/>\n<BehaviourDouble name=\"BehaviourDouble\"/>\n";
	for( const char* r : { "defaultRange", "minMax", "startStop" } ) s += std::string( "<ValueRange name=\"" ) + r + "\" min=\"" + F( mn ) + "\" max=\"" + F( mx ) + "\"/>\n";
	return s + "</ParamRange>";
}
static std::string ColorParams()
{
	std::string s;
	for( const char* n : { "Brightness", "Contrast", "Red", "Green", "Blue" } ) s += Range( n, 0, 0, -1, 1 ) + "\n";
	s.pop_back();
	return s;
}
static std::string Verts( const std::vector< float >& xy )
{
	std::string s;
	for( size_t k = 0; k + 1 < xy.size(); k += 2 ) s += "<v x=\"" + F( xy[ k ] ) + "\" y=\"" + F( xy[ k + 1 ] ) + "\"/>\n";
	if( !s.empty() ) s.pop_back();
	return s;
}
static std::vector< float > RectPts( float x0, float y0, float x1, float y1 ) { return { x0, y0, x1, y0, x1, y1, x0, y1 }; }
static void BBox( const std::vector< float >& xy, float b[ 4 ] )
{
	b[ 0 ] = b[ 1 ] = 1e30f;
	b[ 2 ] = b[ 3 ] = -1e30f;
	for( size_t k = 0; k + 1 < xy.size(); k += 2 )
	{
		b[ 0 ] = std::min( b[ 0 ], xy[ k ] );
		b[ 1 ] = std::min( b[ 1 ], xy[ k + 1 ] );
		b[ 2 ] = std::max( b[ 2 ], xy[ k ] );
		b[ 3 ] = std::max( b[ 3 ], xy[ k + 1 ] );
	}
}
static std::string Common( const std::string& name )
{
	return "<Params name=\"Common\">\n<Param name=\"Name\" T=\"STRING\" default=\"Layer\" value=\"" + Esc( name ) + "\"/>\n<Param name=\"Enabled\" T=\"BOOL\" default=\"1\" value=\"1\"/>\n</Params>";
}
static std::string InputParams( bool softEdge )
{
	std::string s = "<Params name=\"Input\">\n<ParamChoice name=\"Input Source\" default=\"0:1\" value=\"0:1\" storeChoices=\"0\"/>\n"
	                "<Param name=\"Input Opacity\" T=\"BOOL\" default=\"1\" value=\"1\"/>\n<Param name=\"Input Bypass/Solo\" T=\"BOOL\" default=\"1\" value=\"1\"/>";
	if( softEdge ) s += "\n<Param name=\"SoftEdgeEnable\" T=\"BOOL\" default=\"0\" value=\"0\"/>";
	return s + "\n</Params>";
}
static std::string SliceXml( const ResolumeSlice& sl )
{
	float ob[ 4 ];
	BBox( sl.net, ob );
	const std::vector< float > in = RectPts( sl.in[ 0 ], sl.in[ 1 ], sl.in[ 2 ], sl.in[ 3 ] ), outR = RectPts( ob[ 0 ], ob[ 1 ], ob[ 2 ], ob[ 3 ] );
	std::string s = "<Slice uniqueId=\"" + Uid() + "\">\n" + Common( sl.name ) + "\n" + InputParams( true ) + "\n";
	s += "<Params name=\"Output\">\n<Param name=\"Flip\" T=\"UINT8\" default=\"0\" value=\"0\"/>\n" + ColorParams() + "\n";
	s += "<Param name=\"Is Key\" T=\"BOOL\" default=\"0\" value=\"0\"/>\n<Param name=\"Black BG\" T=\"BOOL\" default=\"0\" value=\"0\"/>\n";
	s += Range( "BRed", 0, 0, 0, 0.4, "Red" ) + "\n" + Range( "BGreen", 0, 0, 0, 0.4, "Green" ) + "\n" + Range( "BBlue", 0, 0, 0, 0.4, "Blue" ) + "\n</Params>\n";
	s += "<InputRect orientation=\"0\">\n" + Verts( in ) + "\n</InputRect>\n";
	s += "<OutputRect orientation=\"0\">\n" + Verts( outR ) + "\n</OutputRect>\n";
	s += "<Warper>\n<Params name=\"Warper\">\n<ParamChoice name=\"Point Mode\" default=\"PM_LINEAR\" value=\"PM_BEZIER\" storeChoices=\"0\"/>\n";
	s += "<Param name=\"Flip\" T=\"UINT8\" default=\"0\" value=\"0\"/>\n</Params>\n";
	s += "<BezierWarper controlWidth=\"" + std::to_string( sl.gw ) + "\" controlHeight=\"" + std::to_string( sl.gh ) + "\">\n<vertices>\n" + Verts( sl.net ) + "\n</vertices>\n</BezierWarper>\n";
	s += "<Homography>\n<src>\n" + Verts( outR ) + "\n</src>\n<dst>\n" + Verts( outR ) + "\n</dst>\n</Homography>\n</Warper>\n</Slice>";
	return s;
}
static std::string MaskXml( const std::string& name, const std::vector< float >& pts )
{
	float b[ 4 ];
	BBox( pts, b );
	const std::vector< float > r = RectPts( b[ 0 ], b[ 1 ], b[ 2 ], b[ 3 ] );
	const std::string seg( pts.size() / 2, 'L' );
	std::string s = "<Mask uniqueId=\"" + Uid() + "\">\n" + Common( name ) + "\n" + InputParams( false ) + "\n";
	s += "<Params name=\"Output\">\n<Param name=\"Flip\" T=\"UINT8\" default=\"0\" value=\"0\"/>\n" + ColorParams() + "\n";
	s += "<Param name=\"Invert\" T=\"BOOL\" default=\"1\" value=\"0\"/>\n</Params>\n";
	s += "<InputRect orientation=\"0\">\n<v x=\"-0.5\" y=\"-0.5\"/>\n<v x=\"0.5\" y=\"-0.5\"/>\n<v x=\"0.5\" y=\"0.5\"/>\n<v x=\"-0.5\" y=\"0.5\"/>\n</InputRect>\n";
	s += "<OutputRect orientation=\"0\">\n" + Verts( r ) + "\n</OutputRect>\n";
	s += "<ShapeObject>\n<Params name=\"Shape\">\n<ParamChoice name=\"Point Mode\" default=\"PM_LINEAR\" value=\"PM_LINEAR\" storeChoices=\"0\"/>\n</Params>\n";
	s += "<Rect orientation=\"0\">\n" + Verts( r ) + "\n</Rect>\n";
	s += "<Shape>\n<Contour closed=\"1\">\n<points>\n" + Verts( pts ) + "\n</points>\n<segments>" + seg + "</segments>\n</Contour>\n</Shape>\n</ShapeObject>\n</Mask>";
	return s;
}
static std::string Guide( int t )
{
	return "<ScreenGuide name=\"ScreenGuide\" type=\"" + std::to_string( t ) + "\">\n<Params name=\"Params\">\n<ParamPixels name=\"Image\"/>\n" + Range( "Opacity", 0.25, 0.25, 0, 1 ) + "\n</Params>\n</ScreenGuide>";
}
static std::string ScreenXml( const ResolumeScreen& S )
{
	std::string layers;
	//masks on top: they black out the parts of the screens below them in the list
	for( auto& sl : S.slices )
		for( size_t k = 0; k < sl.masks.size(); ++k ) layers += MaskXml( sl.name + " Mask " + std::to_string( k + 1 ), sl.masks[ k ] ) + "\n";
	for( auto& sl : S.slices ) layers += SliceXml( sl ) + "\n";
	if( !layers.empty() ) layers.pop_back();
	const std::string n = Esc( S.name );
	std::string s = "<Screen name=\"" + n + "\" uniqueId=\"" + Uid() + "\">\n<Params name=\"Params\">\n";
	s += "<Param name=\"Name\" T=\"STRING\" default=\"\" value=\"" + n + "\"/>\n<Param name=\"Enabled\" T=\"BOOL\" default=\"1\" value=\"1\"/>\n<Param name=\"Hidden\" T=\"BOOL\" default=\"0\" value=\"0\"/>\n</Params>\n";
	s += "<Params name=\"Output\">\n" + Range( "Opacity", 1, 1, 0, 1 ) + "\n" + ColorParams() + "\n</Params>\n";
	s += "<guides>\n" + Guide( 0 ) + "\n" + Guide( 1 ) + "\n</guides>\n<layers>\n" + layers + "\n</layers>\n";
	s += "<OutputDevice>\n<OutputDeviceNDI name=\"" + n + "\" deviceId=\"NDI" + n + "\" idHash=\"" + Uid() + "\" width=\"" + std::to_string( S.w ) + "\" height=\"" + std::to_string( S.h ) + "\">\n";
	s += "<Params name=\"Params\">\n" + Range( "Width", 1920, S.w, 1, 32768 ) + "\n" + Range( "Height", 1080, S.h, 1, 32768 ) + "\n";
	s += "<Param name=\"Alpha Output\" T=\"BOOL\" default=\"1\" value=\"1\"/>\n<ParamChoice name=\"Bit depth\" T=\"INT32\" default=\"0\" value=\"0\" storeChoices=\"0\"/>\n</Params>\n";
	s += "</OutputDeviceNDI>\n</OutputDevice>\n</Screen>";
	return s;
}
static std::string Indent( const std::string& xml )
{
	std::string out;
	int d     = 0;
	size_t p0 = 0;
	while( p0 <= xml.size() )
	{
		size_t e = xml.find( '\n', p0 );
		if( e == std::string::npos ) e = xml.size();
		const std::string t = Trim( xml.substr( p0, e - p0 ) );
		p0                  = e + 1;
		if( t.empty() ) continue;
		if( t.rfind( "</", 0 ) == 0 ) --d;
		out.append( (size_t)std::max( 0, d ), '\t' );
		out += t;
		out += "\r\n";
		const size_t k          = t.rfind( "</" );
		const bool closesInline = k != std::string::npos && k > 0 && t.back() == '>';
		if( t[ 0 ] == '<' && t.rfind( "</", 0 ) != 0 && t.rfind( "<?", 0 ) != 0 && !( t.size() >= 2 && t.compare( t.size() - 2, 2, "/>" ) == 0 ) && !closesInline ) ++d;
	}
	return out;
}

std::string ResolumePresetXml( const std::vector< ResolumeScreen >& screens, const ResolumeOpts& o )
{
	std::string scr;
	for( auto& s : screens ) scr += ScreenXml( s ) + "\n";
	std::string b = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<XmlState name=\"" + Esc( o.name ) + "\">\n";
	b += "<versionInfo name=\"Resolume Arena\" majorVersion=\"7\" minorVersion=\"28\" microVersion=\"0\" revision=\"24303\"/>\n";
	b += "<ScreenSetup name=\"ScreenSetup\">\n<Params name=\"ScreenSetupParams\"/>\n";
	b += "<CurrentCompositionTextureSize width=\"" + std::to_string( o.compW ) + "\" height=\"" + std::to_string( o.compH ) + "\"/>\n";
	b += "<screens>\n" + scr + "</screens>\n<SoftEdging>\n<Params name=\"Soft Edge\">\n";
	b += Range( "Gamma Red", 2, 2, 1, 3 ) + "\n" + Range( "Gamma Green", 2, 2, 1, 3 ) + "\n" + Range( "Gamma Blue", 2, 2, 1, 3 ) + "\n";
	b += Range( "Gamma", 1, 1, 0, 1 ) + "\n" + Range( "Luminance", 0.5, 0.5, 0, 1 ) + "\n" + Range( "Power", 2, 2, 0.1, 7 ) + "\n";
	b += "</Params>\n</SoftEdging>\n</ScreenSetup>\n</XmlState>\n";
	return Indent( b );
}

std::string ResolumePresetDir()
{
	wchar_t p[ MAX_PATH ] = {};
	if( FAILED( SHGetFolderPathW( nullptr, CSIDL_PERSONAL, nullptr, 0, p ) ) ) return "";
	return Narrow( p ) + "\\Resolume Arena\\Presets\\Advanced Output";
}

bool WriteResolumePreset( const std::string& path, const std::string& xml, std::string& err )
{
	const size_t sl = path.find_last_of( "\\/" );
	if( sl != std::string::npos ) SHCreateDirectoryExW( nullptr, Widen( path.substr( 0, sl ) ).c_str(), nullptr );
	FILE* f = _wfopen( Widen( path ).c_str(), L"wb" );
	if( !f )
	{
		err = L( "Datei kann nicht geschrieben werden" );
		return false;
	}
	const bool ok = fwrite( xml.data(), 1, xml.size(), f ) == xml.size();
	fclose( f );
	if( !ok ) err = L( "Datei kann nicht geschrieben werden" );
	return ok;
}
}//namespace ls
