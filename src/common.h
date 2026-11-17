// Grosserd LightStage - shared helpers: math, logging, paths
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "i18n.h"

namespace ls
{
constexpr float kPi  = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;

template< class T > inline T Clamp( T v, T a, T b ) { return v < a ? a : ( v > b ? b : v ); }
inline float Saturate( float v ) { return Clamp( v, 0.0f, 1.0f ); }
inline float Lerp( float a, float b, float t ) { return a + ( b - a ) * t; }
inline float Smooth( float x ) { x = Saturate( x ); return x * x * ( 3.0f - 2.0f * x ); }

struct V3
{
	float x = 0, y = 0, z = 0;
	V3() = default;
	constexpr V3( float a, float b, float c ) : x( a ), y( b ), z( c ) {}
	V3 operator+( const V3& o ) const { return { x + o.x, y + o.y, z + o.z }; }
	V3 operator-( const V3& o ) const { return { x - o.x, y - o.y, z - o.z }; }
	V3 operator*( float s ) const { return { x * s, y * s, z * s }; }
	V3 operator*( const V3& o ) const { return { x * o.x, y * o.y, z * o.z }; }
	V3 operator/( float s ) const { return { x / s, y / s, z / s }; }
	V3 operator-() const { return { -x, -y, -z }; }
	V3& operator+=( const V3& o ) { x += o.x; y += o.y; z += o.z; return *this; }
	V3& operator*=( float s ) { x *= s; y *= s; z *= s; return *this; }
	float operator[]( int i ) const { return i == 0 ? x : ( i == 1 ? y : z ); }
	float& operator[]( int i ) { return i == 0 ? x : ( i == 1 ? y : z ); }
};
inline float Dot( const V3& a, const V3& b ) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross( const V3& a, const V3& b ) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float Len( const V3& a ) { return sqrtf( Dot( a, a ) ); }
inline V3 Norm( const V3& a ) { float l = Len( a ); return l > 1e-12f ? a / l : V3( 0, 1, 0 ); }
inline V3 MaxV( const V3& a, const V3& b ) { return { std::max( a.x, b.x ), std::max( a.y, b.y ), std::max( a.z, b.z ) }; }
inline V3 MinV( const V3& a, const V3& b ) { return { std::min( a.x, b.x ), std::min( a.y, b.y ), std::min( a.z, b.z ) }; }

//column-major 4x4 (OpenGL layout): m[col*4+row]
struct M4
{
	float m[ 16 ] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	float& operator()( int r, int c ) { return m[ c * 4 + r ]; }
	float operator()( int r, int c ) const { return m[ c * 4 + r ]; }
};
inline M4 Mul( const M4& a, const M4& b )
{
	M4 r;
	for( int c = 0; c < 4; ++c )
		for( int rr = 0; rr < 4; ++rr )
		{
			float s = 0;
			for( int k = 0; k < 4; ++k )
				s += a( rr, k ) * b( k, c );
			r( rr, c ) = s;
		}
	return r;
}
inline M4 Identity() { return M4(); }
inline M4 Translate( const V3& t ) { M4 r; r( 0, 3 ) = t.x; r( 1, 3 ) = t.y; r( 2, 3 ) = t.z; return r; }
inline M4 Scale( const V3& s ) { M4 r; r( 0, 0 ) = s.x; r( 1, 1 ) = s.y; r( 2, 2 ) = s.z; return r; }
inline M4 RotX( float a ) { M4 r; float c = cosf( a ), s = sinf( a ); r( 1, 1 ) = c; r( 1, 2 ) = -s; r( 2, 1 ) = s; r( 2, 2 ) = c; return r; }
inline M4 RotY( float a ) { M4 r; float c = cosf( a ), s = sinf( a ); r( 0, 0 ) = c; r( 0, 2 ) = s; r( 2, 0 ) = -s; r( 2, 2 ) = c; return r; }
inline M4 RotZ( float a ) { M4 r; float c = cosf( a ), s = sinf( a ); r( 0, 0 ) = c; r( 0, 1 ) = -s; r( 1, 0 ) = s; r( 1, 1 ) = c; return r; }
//euler in degrees, applied Y (yaw) * X (pitch) * Z (roll)
inline M4 RotEuler( const V3& deg ) { return Mul( RotY( deg.y * kDeg ), Mul( RotX( deg.x * kDeg ), RotZ( deg.z * kDeg ) ) ); }
inline V3 XformP( const M4& m, const V3& p ) { return { m( 0, 0 ) * p.x + m( 0, 1 ) * p.y + m( 0, 2 ) * p.z + m( 0, 3 ), m( 1, 0 ) * p.x + m( 1, 1 ) * p.y + m( 1, 2 ) * p.z + m( 1, 3 ), m( 2, 0 ) * p.x + m( 2, 1 ) * p.y + m( 2, 2 ) * p.z + m( 2, 3 ) }; }
inline V3 XformD( const M4& m, const V3& p ) { return { m( 0, 0 ) * p.x + m( 0, 1 ) * p.y + m( 0, 2 ) * p.z, m( 1, 0 ) * p.x + m( 1, 1 ) * p.y + m( 1, 2 ) * p.z, m( 2, 0 ) * p.x + m( 2, 1 ) * p.y + m( 2, 2 ) * p.z }; }
inline M4 Perspective( float fovY, float aspect, float n, float f )
{
	M4 r;
	float t = 1.0f / tanf( fovY * 0.5f );
	r( 0, 0 ) = t / aspect;
	r( 1, 1 ) = t;
	r( 2, 2 ) = ( f + n ) / ( n - f );
	r( 2, 3 ) = 2.0f * f * n / ( n - f );
	r( 3, 2 ) = -1.0f;
	r( 3, 3 ) = 0.0f;
	return r;
}
inline M4 LookAt( const V3& eye, const V3& at, const V3& upHint )
{
	V3 f = Norm( at - eye );
	V3 up = fabsf( Dot( f, Norm( upHint ) ) ) > 0.999f ? V3( 1, 0, 0 ) : upHint;
	V3 s = Norm( Cross( f, up ) );
	V3 u = Cross( s, f );
	M4 r;
	r( 0, 0 ) = s.x; r( 0, 1 ) = s.y; r( 0, 2 ) = s.z;
	r( 1, 0 ) = u.x; r( 1, 1 ) = u.y; r( 1, 2 ) = u.z;
	r( 2, 0 ) = -f.x; r( 2, 1 ) = -f.y; r( 2, 2 ) = -f.z;
	r( 0, 3 ) = -Dot( s, eye );
	r( 1, 3 ) = -Dot( u, eye );
	r( 2, 3 ) = Dot( f, eye );
	return r;
}
M4 Inverse( const M4& m );

//hsv in 0..1 -> rgb 0..1
inline V3 Hsv( float h, float s, float v )
{
	h = h - floorf( h );
	float r = Saturate( fabsf( h * 6.0f - 3.0f ) - 1.0f ), g = Saturate( 2.0f - fabsf( h * 6.0f - 2.0f ) ), b = Saturate( 2.0f - fabsf( h * 6.0f - 4.0f ) );
	return V3( Lerp( 1, r, s ), Lerp( 1, g, s ), Lerp( 1, b, s ) ) * v;
}
//"#rrggbb" -> rgb 0..1 (sRGB values, used as they are)
bool ParseHexColor( const std::string& s, V3& out );
//black body (Kelvin) -> normalized rgb, approximation
V3 Kelvin( float k );

//paths / files
std::string ExeDir();
bool ReadFile( const std::string& path, std::vector< uint8_t >& out );
bool ReadText( const std::string& path, std::string& out );
bool WriteText( const std::string& path, const std::string& text );
std::vector< std::string > ListFiles( const std::string& dir, const std::string& ext, bool recursive );
std::string FileName( const std::string& path );
std::string FileStem( const std::string& path );
std::string ToLower( std::string s );
std::wstring Widen( const std::string& s );
std::string Narrow( const std::wstring& s );

//log to lightstage.log next to the exe (and the debugger)
void Log( const char* fmt, ... );
double Now();//seconds, monotonic
}//namespace ls
