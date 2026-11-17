#include "xmlzip.h"
#include "common.h"
#include "../third_party/stb_image.h"

namespace ls
{
namespace
{
std::string Unescape( const char* b, const char* e )
{
	std::string s;
	s.reserve( e - b );
	for( const char* p = b; p < e; ++p )
	{
		if( *p != '&' )
		{
			s += *p;
			continue;
		}
		const char* sc = p;
		while( sc < e && *sc != ';' && sc - p < 10 )
			++sc;
		if( sc >= e || *sc != ';' )
		{
			s += *p;
			continue;
		}
		std::string ent( p + 1, sc );
		if( ent == "amp" ) s += '&';
		else if( ent == "lt" ) s += '<';
		else if( ent == "gt" ) s += '>';
		else if( ent == "quot" ) s += '"';
		else if( ent == "apos" ) s += '\'';
		else if( !ent.empty() && ent[ 0 ] == '#' )
		{
			unsigned cp = ent.size() > 1 && ( ent[ 1 ] == 'x' || ent[ 1 ] == 'X' ) ? (unsigned)strtoul( ent.c_str() + 2, nullptr, 16 ) : (unsigned)strtoul( ent.c_str() + 1, nullptr, 10 );
			if( cp < 0x80 ) s += (char)cp;
			else if( cp < 0x800 ) { s += (char)( 0xC0 | ( cp >> 6 ) ); s += (char)( 0x80 | ( cp & 0x3F ) ); }
			else { s += (char)( 0xE0 | ( cp >> 12 ) ); s += (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) ); s += (char)( 0x80 | ( cp & 0x3F ) ); }
		}
		else
			s.append( p, sc + 1 );
		p = sc;
	}
	return s;
}
bool IsNameCh( char c ) { return isalnum( (unsigned char)c ) || c == '_' || c == ':' || c == '-' || c == '.' || (unsigned char)c >= 0x80; }
}//namespace

std::unique_ptr< XmlNode > ParseXml( const std::string& text, std::string* err )
{
	const char* p = text.data();
	const char* e = p + text.size();
	auto root     = std::make_unique< XmlNode >();
	root->name    = "#document";
	std::vector< XmlNode* > stack{ root.get() };
	auto fail = [ & ]( const char* m ) {
		if( err )
			*err = m;
		return nullptr;
	};
	while( p < e )
	{
		if( *p != '<' )
		{
			const char* t = p;
			while( p < e && *p != '<' )
				++p;
			//keep text only if not whitespace
			bool ws = true;
			for( const char* q = t; q < p; ++q )
				if( !isspace( (unsigned char)*q ) )
					ws = false;
			if( !ws )
				stack.back()->text += Unescape( t, p );
			continue;
		}
		if( e - p >= 4 && !strncmp( p, "<!--", 4 ) )
		{
			const char* c = strstr( p + 4, "-->" );
			p = c ? c + 3 : e;
			continue;
		}
		if( e - p >= 9 && !strncmp( p, "<![CDATA[", 9 ) )
		{
			const char* c = strstr( p + 9, "]]>" );
			const char* end = c ? c : e;
			stack.back()->text.append( p + 9, end );
			p = c ? c + 3 : e;
			continue;
		}
		if( p[ 1 ] == '?' || p[ 1 ] == '!' )
		{
			while( p < e && *p != '>' )
				++p;
			++p;
			continue;
		}
		if( p[ 1 ] == '/' )
		{
			while( p < e && *p != '>' )
				++p;
			++p;
			if( stack.size() > 1 )
				stack.pop_back();
			continue;
		}
		//start tag
		++p;
		const char* n = p;
		while( p < e && IsNameCh( *p ) )
			++p;
		auto node  = std::make_unique< XmlNode >();
		node->name = std::string( n, p );
		bool selfClose = false;
		for( ;; )
		{
			while( p < e && isspace( (unsigned char)*p ) )
				++p;
			if( p >= e )
				return fail( "unterminated tag" );
			if( *p == '/' )
			{
				selfClose = true;
				++p;
				continue;
			}
			if( *p == '>' )
			{
				++p;
				break;
			}
			const char* an = p;
			while( p < e && IsNameCh( *p ) )
				++p;
			std::string key( an, p );
			if( key.empty() )
			{
				++p;
				continue;
			}
			while( p < e && isspace( (unsigned char)*p ) )
				++p;
			std::string val;
			if( p < e && *p == '=' )
			{
				++p;
				while( p < e && isspace( (unsigned char)*p ) )
					++p;
				if( p < e && ( *p == '"' || *p == '\'' ) )
				{
					char q = *p++;
					const char* vb = p;
					while( p < e && *p != q )
						++p;
					val = Unescape( vb, p );
					if( p < e )
						++p;
				}
			}
			node->attrs.push_back( { key, val } );
		}
		XmlNode* raw = node.get();
		stack.back()->kids.push_back( std::move( node ) );
		if( !selfClose )
			stack.push_back( raw );
	}
	return root;
}

//---------------------------------------------------------------- zip
static uint16_t U16( const uint8_t* p ) { return (uint16_t)( p[ 0 ] | ( p[ 1 ] << 8 ) ); }
static uint32_t U32( const uint8_t* p ) { return (uint32_t)p[ 0 ] | ( (uint32_t)p[ 1 ] << 8 ) | ( (uint32_t)p[ 2 ] << 16 ) | ( (uint32_t)p[ 3 ] << 24 ); }

bool ZipArchive::Open( const std::string& path )
{
	std::vector< uint8_t > b;
	if( !ReadFile( path, b ) )
		return false;
	return OpenMemory( std::move( b ) );
}

bool ZipArchive::OpenMemory( std::vector< uint8_t >&& bytes )
{
	data = std::move( bytes );
	entries.clear();
	if( data.size() < 22 )
		return false;
	size_t eocd = std::string::npos;
	for( size_t i = data.size() - 22 + 1; i-- > 0 && data.size() - i < 70000; )
		if( U32( &data[ i ] ) == 0x06054b50 )
		{
			eocd = i;
			break;
		}
	if( eocd == std::string::npos )
		return false;
	uint16_t count = U16( &data[ eocd + 10 ] );
	uint32_t cdOff = U32( &data[ eocd + 16 ] );
	size_t p       = cdOff;
	for( int i = 0; i < count; ++i )
	{
		if( p + 46 > data.size() || U32( &data[ p ] ) != 0x02014b50 )
			return false;
		Entry en;
		en.method       = U16( &data[ p + 10 ] );
		en.csize        = U32( &data[ p + 20 ] );
		en.usize        = U32( &data[ p + 24 ] );
		uint16_t nlen   = U16( &data[ p + 28 ] );
		uint16_t xlen   = U16( &data[ p + 30 ] );
		uint16_t clen   = U16( &data[ p + 32 ] );
		en.offset       = U32( &data[ p + 42 ] );
		if( p + 46 + nlen > data.size() )
			return false;
		en.name.assign( (const char*)&data[ p + 46 ], nlen );
		entries.push_back( en );
		p += 46 + nlen + xlen + clen;
	}
	return true;
}

const ZipArchive::Entry* ZipArchive::Find( const std::string& name ) const
{
	std::string n = ToLower( name );
	for( auto& e : entries )
		if( ToLower( e.name ) == n )
			return &e;
	return nullptr;
}

bool ZipArchive::Extract( const Entry& e, std::vector< uint8_t >& out ) const
{
	size_t p = e.offset;
	if( p + 30 > data.size() || U32( &data[ p ] ) != 0x04034b50 )
		return false;
	size_t start = p + 30 + U16( &data[ p + 26 ] ) + U16( &data[ p + 28 ] );
	if( start + e.csize > data.size() )
		return false;
	if( e.method == 0 )
	{
		out.assign( data.begin() + start, data.begin() + start + e.csize );
		return true;
	}
	if( e.method == 8 )
	{
		int len   = 0;
		char* dec = stbi_zlib_decode_noheader_malloc( (const char*)&data[ start ], (int)e.csize, &len );
		if( !dec )
			return false;
		out.assign( (uint8_t*)dec, (uint8_t*)dec + len );
		free( dec );
		return true;
	}
	return false;
}
}//namespace ls
