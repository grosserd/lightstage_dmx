// minimal XML DOM (enough for GDTF description.xml) and a ZIP reader (stored / deflate)
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ls
{
struct XmlNode
{
	std::string name, text;
	std::vector< std::pair< std::string, std::string > > attrs;
	std::vector< std::unique_ptr< XmlNode > > kids;
	const std::string& Attr( const char* k ) const
	{
		static const std::string empty;
		for( auto& a : attrs )
			if( a.first == k )
				return a.second;
		return empty;
	}
	bool Has( const char* k ) const
	{
		for( auto& a : attrs )
			if( a.first == k )
				return true;
		return false;
	}
	const XmlNode* Child( const char* n ) const
	{
		for( auto& c : kids )
			if( c->name == n )
				return c.get();
		return nullptr;
	}
	std::vector< const XmlNode* > Children( const char* n ) const
	{
		std::vector< const XmlNode* > r;
		for( auto& c : kids )
			if( c->name == n )
				r.push_back( c.get() );
		return r;
	}
	//depth-first search for elements of a name
	void FindAll( const char* n, std::vector< const XmlNode* >& out ) const
	{
		for( auto& c : kids )
		{
			if( c->name == n )
				out.push_back( c.get() );
			c->FindAll( n, out );
		}
	}
};
std::unique_ptr< XmlNode > ParseXml( const std::string& text, std::string* err = nullptr );

struct ZipArchive
{
	struct Entry
	{
		std::string name;
		uint32_t method = 0, csize = 0, usize = 0, offset = 0;
	};
	std::vector< uint8_t > data;
	std::vector< Entry > entries;
	bool Open( const std::string& path );
	bool OpenMemory( std::vector< uint8_t >&& bytes );
	const Entry* Find( const std::string& name ) const;//case-insensitive
	bool Extract( const Entry& e, std::vector< uint8_t >& out ) const;
};
}//namespace ls
