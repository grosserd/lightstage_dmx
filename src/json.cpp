#include "json.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ls
{
namespace
{
struct Parser
{
	const char* p;
	const char* e;
	std::string err;
	void Ws()
	{
		while( p < e )
		{
			if( *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' )
				++p;
			else if( p + 1 < e && p[ 0 ] == '/' && p[ 1 ] == '/' )
			{
				while( p < e && *p != '\n' )
					++p;
			}
			else
				break;
		}
	}
	bool Fail( const char* m )
	{
		if( err.empty() )
			err = m;
		return false;
	}
	static void Utf8( std::string& s, unsigned cp )
	{
		if( cp < 0x80 )
			s += (char)cp;
		else if( cp < 0x800 )
		{
			s += (char)( 0xC0 | ( cp >> 6 ) );
			s += (char)( 0x80 | ( cp & 0x3F ) );
		}
		else if( cp < 0x10000 )
		{
			s += (char)( 0xE0 | ( cp >> 12 ) );
			s += (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
			s += (char)( 0x80 | ( cp & 0x3F ) );
		}
		else
		{
			s += (char)( 0xF0 | ( cp >> 18 ) );
			s += (char)( 0x80 | ( ( cp >> 12 ) & 0x3F ) );
			s += (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
			s += (char)( 0x80 | ( cp & 0x3F ) );
		}
	}
	bool Str( std::string& s )
	{
		if( p >= e || *p != '"' )
			return Fail( "string expected" );
		++p;
		while( p < e && *p != '"' )
		{
			if( *p == '\\' )
			{
				++p;
				if( p >= e )
					return Fail( "bad escape" );
				char c = *p++;
				switch( c )
				{
				case 'n': s += '\n'; break;
				case 't': s += '\t'; break;
				case 'r': s += '\r'; break;
				case 'b': s += '\b'; break;
				case 'f': s += '\f'; break;
				case 'u':
				{
					if( e - p < 4 )
						return Fail( "bad \\u" );
					unsigned cp = (unsigned)strtoul( std::string( p, 4 ).c_str(), nullptr, 16 );
					p += 4;
					if( cp >= 0xD800 && cp <= 0xDBFF && e - p >= 6 && p[ 0 ] == '\\' && p[ 1 ] == 'u' )
					{
						unsigned lo = (unsigned)strtoul( std::string( p + 2, 4 ).c_str(), nullptr, 16 );
						p += 6;
						cp = 0x10000 + ( ( cp - 0xD800 ) << 10 ) + ( lo - 0xDC00 );
					}
					Utf8( s, cp );
					break;
				}
				default: s += c; break;
				}
			}
			else
				s += *p++;
		}
		if( p >= e )
			return Fail( "unterminated string" );
		++p;
		return true;
	}
	bool Val( Json& j, int depth )
	{
		if( depth > 200 )
			return Fail( "too deep" );
		Ws();
		if( p >= e )
			return Fail( "unexpected end" );
		char c = *p;
		if( c == '{' )
		{
			++p;
			j.type = Json::Object;
			Ws();
			if( p < e && *p == '}' )
			{
				++p;
				return true;
			}
			for( ;; )
			{
				Ws();
				std::string k;
				if( !Str( k ) )
					return false;
				Ws();
				if( p >= e || *p != ':' )
					return Fail( "':' expected" );
				++p;
				j.o.push_back( { k, Json() } );
				if( !Val( j.o.back().second, depth + 1 ) )
					return false;
				Ws();
				if( p < e && *p == ',' )
				{
					++p;
					continue;
				}
				if( p < e && *p == '}' )
				{
					++p;
					return true;
				}
				return Fail( "'}' expected" );
			}
		}
		if( c == '[' )
		{
			++p;
			j.type = Json::Array;
			Ws();
			if( p < e && *p == ']' )
			{
				++p;
				return true;
			}
			for( ;; )
			{
				j.a.push_back( Json() );
				if( !Val( j.a.back(), depth + 1 ) )
					return false;
				Ws();
				if( p < e && *p == ',' )
				{
					++p;
					continue;
				}
				if( p < e && *p == ']' )
				{
					++p;
					return true;
				}
				return Fail( "']' expected" );
			}
		}
		if( c == '"' )
		{
			j.type = Json::String;
			return Str( j.s );
		}
		if( e - p >= 4 && !strncmp( p, "true", 4 ) )
		{
			p += 4;
			j = Json( true );
			return true;
		}
		if( e - p >= 5 && !strncmp( p, "false", 5 ) )
		{
			p += 5;
			j = Json( false );
			return true;
		}
		if( e - p >= 4 && !strncmp( p, "null", 4 ) )
		{
			p += 4;
			j = Json();
			return true;
		}
		if( c == '-' || ( c >= '0' && c <= '9' ) )
		{
			char* end = nullptr;
			std::string tmp( p, std::min< size_t >( 64, e - p ) );
			double v = strtod( tmp.c_str(), &end );
			if( end == tmp.c_str() )
				return Fail( "bad number" );
			p += end - tmp.c_str();
			j = Json( v );
			return true;
		}
		return Fail( "unexpected character" );
	}
};

void Esc( std::string& out, const std::string& s )
{
	out += '"';
	for( unsigned char c : s )
	{
		switch( c )
		{
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if( c < 0x20 )
			{
				char b[ 8 ];
				snprintf( b, sizeof( b ), "\\u%04x", c );
				out += b;
			}
			else
				out += (char)c;
		}
	}
	out += '"';
}

void Write( std::string& out, const Json& j, bool pretty, int ind )
{
	auto nl = [ & ]( int d ) {
		if( pretty )
		{
			out += '\n';
			out.append( (size_t)d * 2, ' ' );
		}
	};
	switch( j.type )
	{
	case Json::Null: out += "null"; break;
	case Json::Bool: out += j.b ? "true" : "false"; break;
	case Json::Number:
	{
		char b[ 40 ];
		if( j.n == (double)(long long)j.n && fabs( j.n ) < 1e15 )
			snprintf( b, sizeof( b ), "%lld", (long long)j.n );
		else
			snprintf( b, sizeof( b ), "%.9g", j.n );
		out += b;
		break;
	}
	case Json::String: Esc( out, j.s ); break;
	case Json::Array:
	{
		//short arrays of numbers stay on one line
		bool flat = true;
		for( auto& x : j.a )
			if( x.type == Json::Array || x.type == Json::Object )
				flat = false;
		out += '[';
		for( size_t i = 0; i < j.a.size(); ++i )
		{
			if( i )
				out += pretty && flat ? ", " : ",";
			if( !flat )
				nl( ind + 1 );
			Write( out, j.a[ i ], pretty, ind + 1 );
		}
		if( !flat && !j.a.empty() )
			nl( ind );
		out += ']';
		break;
	}
	case Json::Object:
		out += '{';
		for( size_t i = 0; i < j.o.size(); ++i )
		{
			if( i )
				out += ',';
			nl( ind + 1 );
			Esc( out, j.o[ i ].first );
			out += pretty ? ": " : ":";
			Write( out, j.o[ i ].second, pretty, ind + 1 );
		}
		if( !j.o.empty() )
			nl( ind );
		out += '}';
		break;
	}
}
}//namespace

bool ParseJson( const std::string& text, Json& out, std::string* err )
{
	Parser ps{ text.data(), text.data() + text.size(), {} };
	out = Json();
	bool ok = ps.Val( out, 0 );
	if( !ok && err )
		*err = ps.err;
	return ok;
}

std::string WriteJson( const Json& j, bool pretty )
{
	std::string s;
	Write( s, j, pretty, 0 );
	return s;
}
}//namespace ls
