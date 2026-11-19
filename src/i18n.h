#pragma once
//UI language: the German text in the source is the key, English comes from i18n_table.inc.
//L( "literal" )  - literals, also \0-separated combo lists
//Ls( text )      - text that is only known at run time (preset names, categories ...)
#include <cstddef>
#include <string>
namespace ls
{
enum Lang { kLangDe = 0, kLangEn = 1 };
extern int gLang;
const char* TrN( const char* de, size_t len );//returns de itself when there is no translation
template< size_t N > inline const char* L( const char ( &s )[ N ] ) { return TrN( s, N - 1 ); }
inline const char* Ls( const char* s );
inline const char* Ls( const std::string& s ) { return TrN( s.c_str(), s.size() ); }
const wchar_t* TrW( const wchar_t* de, size_t len );//wide strings (\0 lists allowed); file dialogs take L() lists via FilterList
template< size_t N > inline const wchar_t* LW( const wchar_t ( &s )[ N ] ) { return TrW( s, N - 1 ); }
inline const char* PlN() { return gLang == kLangDe ? "n" : "s"; }//plural ending of Lampe / fixture
const char* LangCode();//"de" / "en"
void SetLang( const std::string& code );
size_t TrCount();
int TrSelfCheck( std::string& bad );//placeholders, \0 lists and ## ids of every entry; returns the number of entries
}//namespace ls
#include <cstring>
inline const char* ls::Ls( const char* s ) { return s ? TrN( s, strlen( s ) ) : s; }
