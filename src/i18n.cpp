#include "i18n.h"
#include "common.h"
#include <cstring>
#include <string_view>
#include <mutex>
#include <unordered_map>
namespace ls
{
int gLang = kLangEn;
namespace
{
struct Entry
{
	const char* de;
	size_t len;
	const char* en;
};
#define LS_TR( a, b ) { a, sizeof( a ) - 1, b },
const Entry kTable[] = {
#include "i18n_table.inc"
};
#undef LS_TR
const std::unordered_map< std::string_view, const char* >& Map()
{
	static const std::unordered_map< std::string_view, const char* > m = [] {
		std::unordered_map< std::string_view, const char* > r;
		r.reserve( sizeof( kTable ) / sizeof( kTable[ 0 ] ) * 2 );
		for( const Entry& e : kTable ) r.emplace( std::string_view( e.de, e.len ), e.en );
		return r;
	}();
	return m;
}
}//namespace
const char* TrN( const char* de, size_t len )
{
	if( gLang == kLangDe || !de ) return de;
	const auto& m = Map();
	auto it       = m.find( std::string_view( de, len ) );
	return it == m.end() ? de : it->second;
}
const wchar_t* TrW( const wchar_t* de, size_t len )
{
	if( gLang == kLangDe || !de ) return de;
	static std::mutex mx;
	static std::unordered_map< std::wstring, std::wstring > cache;//wide translation, kept for the program lifetime
	std::lock_guard< std::mutex > lk( mx );
	std::wstring key( de, len );
	auto it = cache.find( key );
	if( it != cache.end() ) return it->second.empty() ? de : it->second.c_str();
	const std::string u = Narrow( key );//keeps the inner \0
	const auto& m = Map();
	auto t        = m.find( std::string_view( u ) );
	std::wstring w;
	if( t != m.end() )
	{
		//length of the English text incl. inner \0 (a list ends with \0\0)
		const char* e = t->second;
		size_t n      = strlen( e );
		if( !u.empty() && u.back() == '\0' )
			while( e[ n + 1 ] ) n += 1 + strlen( e + n + 1 );
		if( !u.empty() && u.back() == '\0' ) ++n;
		w = Widen( std::string( e, n ) );
	}
	auto& r = cache[ key ] = w;
	return r.empty() ? de : r.c_str();
}
const char* LangCode() { return gLang == kLangDe ? "de" : "en"; }
void SetLang( const std::string& code ) { gLang = code == "de" ? kLangDe : kLangEn; }
static std::string Specs( const char* s, size_t n )
{
	std::string r;
	for( size_t i = 0; i < n; ++i )
	{
		if( s[ i ] != '%' ) continue;
		size_t j = i + 1;
		while( j < n && strchr( "-+ #0123456789.lh", s[ j ] ) ) ++j;
		if( j < n ) r.append( s + i, j - i + 1 ), r += ',';
		i = j;
	}
	return r;
}
int TrSelfCheck( std::string& bad )
{
	bad.clear();
	int n = 0;
	for( const Entry& e : kTable )
	{
		++n;
		int zd = 0;
		for( size_t i = 0; i < e.len; ++i ) zd += e.de[ i ] == 0;
		size_t enLen = strlen( e.en );
		int ze       = 0;
		if( zd )
		{
			const char* p = e.en;
			while( *p ) { p += strlen( p ) + 1; ++ze; }
			enLen = (size_t)( p - e.en );
		}
		const char* hd = strstr( e.de, "##" );
		const char* he = strstr( e.en, "##" );
		const bool idOk = ( !hd && !he ) || ( hd && he && strcmp( hd, he ) == 0 );
		if( zd != ze || Specs( e.de, e.len ) != Specs( e.en, enLen ) || !idOk )
		{
			bad += "\n   ";
			bad.append( e.de, strnlen( e.de, 60 ) );
		}
	}
	return n;
}
size_t TrCount() { return sizeof( kTable ) / sizeof( kTable[ 0 ] ); }
}//namespace ls
