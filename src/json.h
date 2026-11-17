// minimal JSON DOM: parse + write (UTF-8 kept as is)
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ls
{
struct Json
{
	enum Type { Null, Bool, Number, String, Array, Object } type = Null;
	bool b   = false;
	double n = 0.0;
	std::string s;
	std::vector< Json > a;
	std::vector< std::pair< std::string, Json > > o;//keeps key order

	Json() = default;
	Json( bool v ) : type( Bool ), b( v ) {}
	Json( int v ) : type( Number ), n( v ) {}
	Json( unsigned v ) : type( Number ), n( v ) {}
	Json( float v ) : type( Number ), n( v ) {}
	Json( double v ) : type( Number ), n( v ) {}
	Json( const char* v ) : type( String ), s( v ) {}
	Json( const std::string& v ) : type( String ), s( v ) {}
	static Json Arr() { Json j; j.type = Array; return j; }
	static Json Obj() { Json j; j.type = Object; return j; }

	bool IsNull() const { return type == Null; }
	bool IsObj() const { return type == Object; }
	bool IsArr() const { return type == Array; }
	bool IsStr() const { return type == String; }
	bool IsNum() const { return type == Number; }
	const Json* Find( const std::string& k ) const
	{
		if( type != Object )
			return nullptr;
		for( auto& kv : o )
			if( kv.first == k )
				return &kv.second;
		return nullptr;
	}
	const Json& operator[]( const std::string& k ) const
	{
		static const Json nul;
		const Json* j = Find( k );
		return j ? *j : nul;
	}
	const Json& operator[]( size_t i ) const
	{
		static const Json nul;
		return type == Array && i < a.size() ? a[ i ] : nul;
	}
	size_t Size() const { return type == Array ? a.size() : ( type == Object ? o.size() : 0 ); }
	std::string Str( const std::string& def = "" ) const { return type == String ? s : def; }
	double Num( double def = 0.0 ) const { return type == Number ? n : ( type == Bool ? ( b ? 1.0 : 0.0 ) : def ); }
	float F( float def = 0.0f ) const { return (float)Num( def ); }
	int I( int def = 0 ) const { return (int)Num( def ); }
	bool B( bool def = false ) const { return type == Bool ? b : ( type == Number ? n != 0.0 : def ); }
	Json& Set( const std::string& k, const Json& v )
	{
		type = Object;
		for( auto& kv : o )
			if( kv.first == k )
			{
				kv.second = v;
				return kv.second;
			}
		o.push_back( { k, v } );
		return o.back().second;
	}
	Json& Push( const Json& v )
	{
		type = Array;
		a.push_back( v );
		return a.back();
	}
};

bool ParseJson( const std::string& text, Json& out, std::string* err = nullptr );
std::string WriteJson( const Json& j, bool pretty = true );
}//namespace ls
