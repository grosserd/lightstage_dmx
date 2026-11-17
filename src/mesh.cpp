#include "mesh.h"
#include "../third_party/cgltf.h"
#include "../third_party/ufbx/ufbx.h"
#include "../third_party/stb_image.h"
#include <filesystem>
#include <map>
#include <windows.h>
#include <wincodec.h>
#pragma comment( lib, "windowscodecs.lib" )

namespace ls
{
void MeshData::Vert( const V3& p, const V3& n, const V3& c, float u, float t )
{
	v.insert( v.end(), { p.x, p.y, p.z, n.x, n.y, n.z, c.x, c.y, c.z, u, t } );
	bmin = MinV( bmin, p );
	bmax = MaxV( bmax, p );
}

int MeshData::Textured() const
{
	int n = 0;
	for( auto& p : parts )
		if( p.tex >= 0 ) ++n;
	return n;
}

//Windows Imaging Component: TIF/TIFF, BMP variants, ICO, JPEG-XR, DDS (Win10+), HEIC/WEBP when the codec is installed
static bool DecodeWic( const uint8_t* data, size_t size, int& w, int& h, std::vector< uint8_t >& rgba )
{
	if( size == 0 || size > 0x7fffffff ) return false;
	HRESULT co = CoInitializeEx( nullptr, COINIT_MULTITHREADED );//S_FALSE / RPC_E_CHANGED_MODE: COM is already there
	bool ok    = false;
	IWICImagingFactory* f      = nullptr;
	IWICStream* st             = nullptr;
	IWICBitmapDecoder* dec     = nullptr;
	IWICBitmapFrameDecode* fr  = nullptr;
	IWICFormatConverter* cv    = nullptr;
	if( SUCCEEDED( CoCreateInstance( CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS( &f ) ) ) && SUCCEEDED( f->CreateStream( &st ) ) &&
	    SUCCEEDED( st->InitializeFromMemory( (BYTE*)data, (DWORD)size ) ) && SUCCEEDED( f->CreateDecoderFromStream( st, nullptr, WICDecodeMetadataCacheOnDemand, &dec ) ) &&
	    SUCCEEDED( dec->GetFrame( 0, &fr ) ) && SUCCEEDED( f->CreateFormatConverter( &cv ) ) &&
	    SUCCEEDED( cv->Initialize( fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom ) ) )
	{
		UINT uw = 0, uh = 0;
		if( SUCCEEDED( cv->GetSize( &uw, &uh ) ) && uw > 0 && uh > 0 && (uint64_t)uw * uh < ( 1ull << 28 ) )
		{
			rgba.resize( (size_t)uw * uh * 4 );
			if( SUCCEEDED( cv->CopyPixels( nullptr, uw * 4, (UINT)rgba.size(), rgba.data() ) ) )
			{
				w  = (int)uw;
				h  = (int)uh;
				ok = true;
			}
		}
	}
	if( cv ) cv->Release();
	if( fr ) fr->Release();
	if( dec ) dec->Release();
	if( st ) st->Release();
	if( f ) f->Release();
	if( SUCCEEDED( co ) ) CoUninitialize();
	return ok;
}

bool DecodeImage( const uint8_t* data, size_t size, MeshImage& out )
{
	int w = 0, h = 0, n = 0;
	std::vector< uint8_t > img;
	if( stbi_uc* px = stbi_load_from_memory( data, (int)size, &w, &h, &n, 4 ) )
	{
		img.assign( px, px + (size_t)w * h * 4 );
		stbi_image_free( px );
	}
	else if( !DecodeWic( data, size, w, h, img ) )
		return false;
	//very large textures: halve until the long side fits 4096 (keeps memory and upload time sane)
	while( std::max( w, h ) > 4096 )
	{
		int nw = std::max( 1, w / 2 ), nh = std::max( 1, h / 2 );
		std::vector< uint8_t > s( (size_t)nw * nh * 4 );
		for( int y = 0; y < nh; ++y )
			for( int x = 0; x < nw; ++x )
				for( int c = 0; c < 4; ++c )
				{
					int sx = std::min( w - 1, x * 2 ), sy = std::min( h - 1, y * 2 );
					int a = img[ ( (size_t)sy * w + sx ) * 4 + c ], b = img[ ( (size_t)sy * w + std::min( w - 1, sx + 1 ) ) * 4 + c ];
					int d = img[ ( (size_t)std::min( h - 1, sy + 1 ) * w + sx ) * 4 + c ], e = img[ ( (size_t)std::min( h - 1, sy + 1 ) * w + std::min( w - 1, sx + 1 ) ) * 4 + c ];
					s[ ( (size_t)y * nw + x ) * 4 + c ] = (uint8_t)( ( a + b + d + e + 2 ) / 4 );
				}
		img.swap( s );
		w = nw;
		h = nh;
	}
	out.w = w;
	out.h = h;
	out.rgba.swap( img );
	return true;
}

//all visible pixels have (almost) the same colour, e.g. a white texture that only carries alpha
static bool FlatRgb( const MeshImage& im )
{
	int mn[ 3 ] = { 255, 255, 255 }, mx[ 3 ] = { 0, 0, 0 };
	const size_t step = std::max< size_t >( 1, im.rgba.size() / 4 / 20000 ) * 4;
	for( size_t i = 0; i + 3 < im.rgba.size(); i += step )
	{
		if( im.rgba[ i + 3 ] < 16 ) continue;
		for( int c = 0; c < 3; ++c )
		{
			mn[ c ] = std::min< int >( mn[ c ], im.rgba[ i + c ] );
			mx[ c ] = std::max< int >( mx[ c ], im.rgba[ i + c ] );
		}
	}
	return mx[ 0 ] - mn[ 0 ] < 12 && mx[ 1 ] - mn[ 1 ] < 12 && mx[ 2 ] - mn[ 2 ] < 12;
}
//tangent space normal map: bluish, red/green around the middle
static bool LooksLikeNormalMap( const MeshImage& im )
{
	double s[ 3 ] = { 0, 0, 0 };
	size_t n = 0;
	const size_t step = std::max< size_t >( 1, im.rgba.size() / 4 / 20000 ) * 4;
	for( size_t i = 0; i + 3 < im.rgba.size(); i += step, ++n )
		for( int c = 0; c < 3; ++c ) s[ c ] += im.rgba[ i + c ];
	if( !n ) return false;
	for( double& v : s ) v /= n * 255.0;
	return s[ 2 ] > 0.7 && fabs( s[ 0 ] - 0.5 ) < 0.15 && fabs( s[ 1 ] - 0.5 ) < 0.15;
}

static bool HasAlpha( const MeshImage& im )
{
	size_t cut = 0;
	for( size_t i = 3; i < im.rgba.size(); i += 4 )
		if( im.rgba[ i ] < 250 && ++cut > im.rgba.size() / 4000 ) return true;//more than a few stray pixels
	return false;
}

//colour image (or plain white) with the opacity map in its alpha channel (alpha of the map, else its brightness)
static MeshImage MergeOpacity( const MeshImage* col, const MeshImage& op )
{
	MeshImage r;
	r.w    = col ? col->w : op.w;
	r.h    = col ? col->h : op.h;
	r.name = ( col ? col->name : std::string( L( "weiss" ) ) ) + " + " + op.name;
	r.rgba = col ? col->rgba : std::vector< uint8_t >( (size_t)r.w * r.h * 4, 255 );
	const bool useA = HasAlpha( op );
	for( int y = 0; y < r.h; ++y )
		for( int x = 0; x < r.w; ++x )
		{
			int sx = std::min( op.w - 1, x * op.w / std::max( 1, r.w ) ), sy = std::min( op.h - 1, y * op.h / std::max( 1, r.h ) );
			const uint8_t* s = &op.rgba[ ( (size_t)sy * op.w + sx ) * 4 ];
			int a = useA ? s[ 3 ] : ( s[ 0 ] * 54 + s[ 1 ] * 183 + s[ 2 ] * 19 ) >> 8;
			uint8_t& d = r.rgba[ ( (size_t)y * r.w + x ) * 4 + 3 ];
			d = (uint8_t)( d * a / 255 );
		}
	return r;
}

namespace fs = std::filesystem;
static bool ReadPath( const fs::path& p, std::vector< uint8_t >& out )
{
	std::error_code ec;
	if( !fs::is_regular_file( p, ec ) ) return false;
	return ReadFile( Narrow( p.wstring() ), out );
}
bool FindTextureFile( const std::string& modelDir, const std::string& ref, std::vector< uint8_t >& bytes, std::string* foundPath )
{
	if( ref.empty() ) return false;
	auto done = [ & ]( const fs::path& p ) {
		if( !ReadPath( p, bytes ) ) return false;
		if( foundPath ) *foundPath = Narrow( p.wstring() );
		return true;
	};
	std::error_code ec;
	const fs::path dir = fs::u8path( modelDir.empty() ? std::string( "." ) : modelDir );
	//1. as written (absolute or relative to the model)
	std::string clean = ref;
	for( const char* pre : { "asset:///", "asset://", "file:///", "file://" } )
		if( ToLower( clean ).rfind( pre, 0 ) == 0 ) { clean = clean.substr( strlen( pre ) ); break; }
	for( auto& c : clean )
		if( c == '/' ) c = '\\';
	if( clean.find( ':' ) != std::string::npos || ( clean.size() > 1 && clean[ 0 ] == '\\' ) )
	{
		if( done( fs::u8path( clean ) ) ) return true;
	}
	else if( done( dir / fs::u8path( clean ) ) )
		return true;
	//2. by file name next to the model and in the usual texture folders; also without a trailing '~' (Cinema 4D asset names)
	std::vector< std::string > names = { FileName( clean ) };
	{
		std::string st = FileStem( names[ 0 ] ), ext = names[ 0 ].substr( st.size() );
		if( !st.empty() && st.back() == '~' ) names.push_back( st.substr( 0, st.size() - 1 ) + ext );
	}
	static const char* subs[] = { "", "tex", "textures", "Textures", "texture", "maps", "Maps", "images", "materials", "..\\textures", "..\\tex" };
	for( auto& n : names )
		for( const char* sd : subs )
			if( done( dir / fs::u8path( sd ) / fs::u8path( n ) ) ) return true;
	//3. search the model folder (2 levels): same name (any case), or same stem with another image extension
	std::vector< std::string > lnames, lstems;
	for( auto& n : names )
	{
		lnames.push_back( ToLower( n ) );
		lstems.push_back( ToLower( FileStem( n ) ) );
	}
	static const char* exts[] = { ".png", ".jpg", ".jpeg", ".tif", ".tiff", ".tga", ".bmp", ".psd", ".webp", ".dds", ".exr", ".hdr" };
	fs::path byStem;
	int seen = 0;
	for( fs::recursive_directory_iterator it( dir, fs::directory_options::skip_permission_denied, ec ), end; it != end && seen < 6000; it.increment( ec ) )
	{
		if( ec ) break;
		++seen;
		if( it.depth() > 2 ) { it.disable_recursion_pending(); continue; }
		if( !it->is_regular_file( ec ) ) continue;
		const std::string fn = ToLower( Narrow( it->path().filename().wstring() ) );
		for( size_t k = 0; k < lnames.size(); ++k )
		{
			if( fn == lnames[ k ] && done( it->path() ) ) return true;
			if( byStem.empty() && ToLower( FileStem( fn ) ) == lstems[ k ] )
			{
				const std::string e = fn.substr( FileStem( fn ).size() );
				for( const char* x : exts )
					if( e == x ) byStem = it->path();
			}
		}
	}
	return !byStem.empty() && done( byStem );
}

static std::string DirOf( const std::string& path )
{
	size_t s = path.find_last_of( "\\/" );
	return s == std::string::npos ? std::string() : path.substr( 0, s + 1 );
}

MeshData MakeBox()
{
	MeshData m;
	const V3 n[ 6 ] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	for( int f = 0; f < 6; ++f )
	{
		V3 N = n[ f ];
		V3 u = fabsf( N.y ) > 0.5f ? V3( 1, 0, 0 ) : V3( 0, 1, 0 );
		V3 t = Cross( u, N );
		u    = Cross( N, t );
		uint32_t b = m.Count();
		V3 c       = N * 0.5f;
		m.Vert( c - t * 0.5f - u * 0.5f, N );
		m.Vert( c + t * 0.5f - u * 0.5f, N );
		m.Vert( c + t * 0.5f + u * 0.5f, N );
		m.Vert( c - t * 0.5f + u * 0.5f, N );
		//winding: counter-clockwise seen from outside
		V3 e1 = ( c + t * 0.5f - u * 0.5f ) - ( c - t * 0.5f - u * 0.5f );
		V3 e2 = ( c + t * 0.5f + u * 0.5f ) - ( c - t * 0.5f - u * 0.5f );
		if( Dot( Cross( e1, e2 ), N ) > 0 )
			m.idx.insert( m.idx.end(), { b, b + 1, b + 2, b, b + 2, b + 3 } );
		else
			m.idx.insert( m.idx.end(), { b, b + 2, b + 1, b, b + 3, b + 2 } );
	}
	return m;
}

MeshData MakeCylinder( int seg )
{
	MeshData m;
	for( int i = 0; i <= seg; ++i )
	{
		float a = i * 2 * kPi / seg;
		V3 n( cosf( a ), 0, sinf( a ) );
		m.Vert( V3( n.x * 0.5f, -0.5f, n.z * 0.5f ), n );
		m.Vert( V3( n.x * 0.5f, 0.5f, n.z * 0.5f ), n );
	}
	for( int i = 0; i < seg; ++i )
	{
		uint32_t a = i * 2, b = a + 1, c = a + 2, d = a + 3;
		m.idx.insert( m.idx.end(), { a, b, d, a, d, c } );
	}
	for( int cap = 0; cap < 2; ++cap )
	{
		float y = cap ? 0.5f : -0.5f;
		V3 n( 0, cap ? 1.0f : -1.0f, 0 );
		uint32_t c0 = m.Count();
		m.Vert( V3( 0, y, 0 ), n );
		for( int i = 0; i <= seg; ++i )
		{
			float a = i * 2 * kPi / seg;
			m.Vert( V3( cosf( a ) * 0.5f, y, sinf( a ) * 0.5f ), n );
		}
		for( int i = 0; i < seg; ++i )
		{
			if( cap ) m.idx.insert( m.idx.end(), { c0, c0 + 2 + i, c0 + 1 + i } );
			else m.idx.insert( m.idx.end(), { c0, c0 + 1 + i, c0 + 2 + i } );
		}
	}
	return m;
}

MeshData MakeCone( int seg )
{
	MeshData m;
	uint32_t apex = 0;
	m.Vert( V3( 0, 0, 0 ), V3( 0, 0, -1 ) );
	for( int i = 0; i <= seg; ++i )
	{
		float a = i * 2 * kPi / seg;
		m.Vert( V3( cosf( a ), sinf( a ), 1 ), Norm( V3( cosf( a ), sinf( a ), -1 ) ) );
	}
	for( int i = 0; i < seg; ++i )
		m.idx.insert( m.idx.end(), { apex, (uint32_t)( 1 + i ), (uint32_t)( 2 + i ) } );
	uint32_t c0 = m.Count();
	m.Vert( V3( 0, 0, 1 ), V3( 0, 0, 1 ) );
	for( int i = 0; i <= seg; ++i )
	{
		float a = i * 2 * kPi / seg;
		m.Vert( V3( cosf( a ), sinf( a ), 1 ), V3( 0, 0, 1 ) );
	}
	for( int i = 0; i < seg; ++i )
		m.idx.insert( m.idx.end(), { c0, c0 + 2 + i, c0 + 1 + i } );
	return m;
}

MeshData MakeQuad()
{
	MeshData m;
	m.Vert( V3( -1, -1, 0 ), V3( 0, 0, 1 ) );
	m.Vert( V3( 1, -1, 0 ), V3( 0, 0, 1 ) );
	m.Vert( V3( 1, 1, 0 ), V3( 0, 0, 1 ) );
	m.Vert( V3( -1, 1, 0 ), V3( 0, 0, 1 ) );
	m.idx = { 0, 1, 2, 0, 2, 3 };
	return m;
}

//---------------------------------------------------------------- import
static V3 Mat( const float* m, const V3& p, float w )
{
	return V3( m[ 0 ] * p.x + m[ 4 ] * p.y + m[ 8 ] * p.z + m[ 12 ] * w, m[ 1 ] * p.x + m[ 5 ] * p.y + m[ 9 ] * p.z + m[ 13 ] * w, m[ 2 ] * p.x + m[ 6 ] * p.y + m[ 10 ] * p.z + m[ 14 ] * w );
}

static bool LoadGltf( const std::string& path, MeshData& out, std::string* err )
{
	std::vector< uint8_t > bytes;
	if( !ReadFile( path, bytes ) )
	{
		if( err ) *err = "cannot read file";
		return false;
	}
	cgltf_options opt{};
	cgltf_data* data = nullptr;
	if( cgltf_parse( &opt, bytes.data(), bytes.size(), &data ) != cgltf_result_success )
	{
		if( err ) *err = "not a valid glTF/GLB";
		return false;
	}
	if( cgltf_load_buffers( &opt, data, path.c_str() ) != cgltf_result_success )
	{
		cgltf_free( data );
		if( err ) *err = "cannot load glTF buffers";
		return false;
	}
	//base colour images: embedded (GLB buffer view / data URI) or files next to the .gltf
	std::map< const cgltf_image*, int > imgIdx;
	const std::string dir = DirOf( path );
	auto imageOf = [ & ]( const cgltf_image* im ) -> int {
		auto it = imgIdx.find( im );
		if( it != imgIdx.end() ) return it->second;
		MeshImage mi;
		bool ok = false;
		if( im->buffer_view && im->buffer_view->buffer && im->buffer_view->buffer->data )
			ok = DecodeImage( (const uint8_t*)im->buffer_view->buffer->data + im->buffer_view->offset, im->buffer_view->size, mi );
		else if( im->uri && strncmp( im->uri, "data:", 5 ) == 0 )
		{
			const char* b64 = strstr( im->uri, ";base64," );
			if( b64 )
			{
				b64 += 8;
				size_t len = strlen( b64 ), pad = 0;
				while( len > pad && b64[ len - 1 - pad ] == '=' ) ++pad;
				void* bin = nullptr;
				size_t sz = len / 4 * 3 - pad;
				if( cgltf_load_buffer_base64( &opt, sz, b64, &bin ) == cgltf_result_success )
				{
					ok = DecodeImage( (const uint8_t*)bin, sz, mi );
					free( bin );
				}
			}
		}
		else if( im->uri )
		{
			std::string u = im->uri;
			std::vector< char > dec( u.begin(), u.end() );
			dec.push_back( 0 );
			cgltf_decode_uri( dec.data() );
			std::vector< uint8_t > fb;
			if( FindTextureFile( dir, dec.data(), fb ) ) ok = DecodeImage( fb.data(), fb.size(), mi );
		}
		int idx = -1;
		if( ok )
		{
			mi.name = im->name ? im->name : ( im->uri && strncmp( im->uri, "data:", 5 ) ? im->uri : "embedded" );
			idx     = (int)out.images.size();
			out.images.push_back( std::move( mi ) );
		}
		else
		{
			Log( "model %s: texture %s could not be loaded", path.c_str(), im->uri ? im->uri : ( im->name ? im->name : "?" ) );
			out.missing.push_back( im->uri && strncmp( im->uri, "data:", 5 ) ? im->uri : ( im->name ? im->name : L( "eingebettetes Bild" ) ) );
		}
		imgIdx[ im ] = idx;
		return idx;
	};
	std::map< std::pair< int, int >, int > combined;
	for( cgltf_size ni = 0; ni < data->nodes_count; ++ni )
	{
		cgltf_node* node = &data->nodes[ ni ];
		if( !node->mesh ) continue;
		float w[ 16 ];
		cgltf_node_transform_world( node, w );
		for( cgltf_size pi = 0; pi < node->mesh->primitives_count; ++pi )
		{
			cgltf_primitive& pr = node->mesh->primitives[ pi ];
			if( pr.type != cgltf_primitive_type_triangles ) continue;
			V3 base( 0.8f, 0.8f, 0.8f );
			int tex = -1, uvSet = 0;
			float cutoff = 0;
			if( pr.material )
			{
				if( pr.material->alpha_mode == cgltf_alpha_mode_mask ) cutoff = pr.material->alpha_cutoff > 0 ? pr.material->alpha_cutoff : 0.5f;
				if( pr.material->alpha_mode == cgltf_alpha_mode_blend ) cutoff = 0.4f;//no sorting: blended foliage etc. becomes a clean cut-out
			}
			if( pr.material && pr.material->has_pbr_metallic_roughness )
			{
				const float* f = pr.material->pbr_metallic_roughness.base_color_factor;
				base           = V3( f[ 0 ], f[ 1 ], f[ 2 ] );
				const cgltf_texture_view& tv = pr.material->pbr_metallic_roughness.base_color_texture;
				if( tv.texture && tv.texture->image )
				{
					tex   = imageOf( tv.texture->image );
					uvSet = (int)tv.texcoord;
				}
			}
			else if( pr.material && pr.material->has_pbr_specular_glossiness )
			{
				const float* f = pr.material->pbr_specular_glossiness.diffuse_factor;
				base           = V3( f[ 0 ], f[ 1 ], f[ 2 ] );
				const cgltf_texture_view& tv = pr.material->pbr_specular_glossiness.diffuse_texture;
				if( tv.texture && tv.texture->image )
				{
					tex   = imageOf( tv.texture->image );
					uvSet = (int)tv.texcoord;
				}
			}
			const cgltf_accessor *pos = nullptr, *nrm = nullptr, *col = nullptr, *uv = nullptr;
			for( cgltf_size a = 0; a < pr.attributes_count; ++a )
			{
				if( pr.attributes[ a ].type == cgltf_attribute_type_position ) pos = pr.attributes[ a ].data;
				if( pr.attributes[ a ].type == cgltf_attribute_type_normal ) nrm = pr.attributes[ a ].data;
				if( pr.attributes[ a ].type == cgltf_attribute_type_color && pr.attributes[ a ].index == 0 ) col = pr.attributes[ a ].data;
				if( pr.attributes[ a ].type == cgltf_attribute_type_texcoord && pr.attributes[ a ].index == uvSet ) uv = pr.attributes[ a ].data;
			}
			if( !pos ) continue;
			if( !uv ) tex = -1;
			//some exporters (Cinema 4D) write a black base colour factor next to the texture: show the texture
			if( tex >= 0 && base.x + base.y + base.z < 0.003f )
			{
				base = V3( 1, 1, 1 );
				//Cinema 4D leaves: base texture = white + alpha only, the real leaf picture went to the "normal" (bump) slot
				const cgltf_texture_view& nv = pr.material->normal_texture;
				if( cutoff > 0 && nv.texture && nv.texture->image && nv.texcoord == uvSet && FlatRgb( out.images[ tex ] ) )
				{
					const int ni = imageOf( nv.texture->image );
					if( ni >= 0 && ni != tex && out.images[ ni ].w == out.images[ tex ].w && out.images[ ni ].h == out.images[ tex ].h && !LooksLikeNormalMap( out.images[ ni ] ) )
					{
						auto key = std::make_pair( tex, ni );
						auto it  = combined.find( key );
						if( it == combined.end() )
						{
							MeshImage c = out.images[ ni ];
							c.name      = c.name + " + Alpha";
							for( size_t k = 3; k < c.rgba.size(); k += 4 ) c.rgba[ k ] = out.images[ tex ].rgba[ k ];
							out.images.push_back( std::move( c ) );
							it = combined.emplace( key, (int)out.images.size() - 1 ).first;
						}
						tex = it->second;
					}
				}
			}
			if( tex < 0 ) cutoff = 0;
			uint32_t b0     = out.Count();
			uint32_t first  = (uint32_t)out.idx.size();
			for( cgltf_size i = 0; i < pos->count; ++i )
			{
				float p[ 4 ] = { 0, 0, 0, 1 }, n[ 4 ] = { 0, 1, 0, 0 }, c[ 4 ] = { 1, 1, 1, 1 }, t[ 2 ] = { 0, 0 };
				cgltf_accessor_read_float( pos, i, p, 3 );
				if( nrm ) cgltf_accessor_read_float( nrm, i, n, 3 );
				if( col ) cgltf_accessor_read_float( col, i, c, 4 );
				if( uv ) cgltf_accessor_read_float( uv, i, t, 2 );
				V3 P = Mat( w, V3( p[ 0 ], p[ 1 ], p[ 2 ] ), 1 );
				V3 N = Norm( Mat( w, V3( n[ 0 ], n[ 1 ], n[ 2 ] ), 0 ) );
				out.Vert( P, N, base * V3( c[ 0 ], c[ 1 ], c[ 2 ] ), t[ 0 ], t[ 1 ] );//glTF: uv origin top-left = first texture row
			}
			if( pr.indices )
				for( cgltf_size i = 0; i < pr.indices->count; ++i )
					out.idx.push_back( b0 + (uint32_t)cgltf_accessor_read_index( pr.indices, i ) );
			else
				for( cgltf_size i = 0; i < pos->count; ++i )
					out.idx.push_back( b0 + (uint32_t)i );
			if( !nrm )
			{
				//flat-ish normals: accumulate face normals
				for( size_t i = out.idx.size() - ( pr.indices ? pr.indices->count : pos->count ); i + 2 < out.idx.size(); i += 3 )
				{
					uint32_t a = out.idx[ i ], b = out.idx[ i + 1 ], c = out.idx[ i + 2 ];
					const int S = MeshData::kStride;
					V3 pa( out.v[ a * S ], out.v[ a * S + 1 ], out.v[ a * S + 2 ] ), pb( out.v[ b * S ], out.v[ b * S + 1 ], out.v[ b * S + 2 ] ), pc( out.v[ c * S ], out.v[ c * S + 1 ], out.v[ c * S + 2 ] );
					V3 fn = Norm( Cross( pb - pa, pc - pa ) );
					for( uint32_t k : { a, b, c } )
					{
						out.v[ k * S + 3 ] = fn.x;
						out.v[ k * S + 4 ] = fn.y;
						out.v[ k * S + 5 ] = fn.z;
					}
				}
			}
			out.parts.push_back( MeshPart{ first, (uint32_t)out.idx.size() - first, tex, cutoff } );
		}
	}
	cgltf_free( data );
	return out.Count() > 0;
}

static bool LoadUfbx( const std::string& path, MeshData& out, std::string* err )
{
	std::vector< uint8_t > bytes;
	if( !ReadFile( path, bytes ) )
	{
		if( err ) *err = "cannot read file";
		return false;
	}
	ufbx_load_opts opts            = {};
	opts.target_axes               = ufbx_axes_right_handed_y_up;
	opts.target_unit_meters        = 1.0f;
	opts.space_conversion          = UFBX_SPACE_CONVERSION_TRANSFORM_ROOT;
	opts.generate_missing_normals  = true;
	opts.ignore_missing_external_files = true;
	opts.filename.data             = path.c_str();
	opts.filename.length           = path.size();
	std::string lp                 = ToLower( path );
	if( lp.size() > 4 && lp.compare( lp.size() - 4, 4, ".obj" ) == 0 )
	{
		opts.file_format                = UFBX_FILE_FORMAT_OBJ;
		opts.load_external_files        = true;
		opts.obj_search_mtl_by_filename = true;
		opts.obj_axes                   = ufbx_axes_right_handed_y_up;
	}
	ufbx_error e;
	ufbx_scene* scene = ufbx_load_memory( bytes.data(), bytes.size(), &opts, &e );
	if( !scene )
	{
		char buf[ 512 ];
		ufbx_format_error( buf, sizeof( buf ), &e );
		if( err ) *err = buf;
		return false;
	}
	//base colour textures: embedded content or the file (absolute, relative, or next to the model)
	std::map< const ufbx_texture*, int > texIdx;
	const std::string dir = DirOf( path );
	auto textureOf = [ & ]( const ufbx_texture* tx ) -> int {
		if( !tx ) return -1;
		if( tx->type != UFBX_TEXTURE_FILE && tx->file_textures.count > 0 ) tx = tx->file_textures.data[ 0 ];
		auto it = texIdx.find( tx );
		if( it != texIdx.end() ) return it->second;
		MeshImage im;
		bool ok = false;
		if( tx->content.size > 0 ) ok = DecodeImage( (const uint8_t*)tx->content.data, tx->content.size, im );
		std::vector< uint8_t > fb;
		const std::string refs[ 3 ] = { std::string( tx->absolute_filename.data, tx->absolute_filename.length ), std::string( tx->relative_filename.data, tx->relative_filename.length ),
		                                std::string( tx->filename.data, tx->filename.length ) };
		for( auto& r : refs )
		{
			if( ok ) break;
			fb.clear();
			if( FindTextureFile( dir, r, fb ) ) ok = DecodeImage( fb.data(), fb.size(), im );
		}
		int idx = -1;
		const std::string nm = tx->filename.length ? std::string( tx->filename.data, tx->filename.length ) : std::string( tx->relative_filename.data, tx->relative_filename.length );
		if( ok )
		{
			im.name = FileName( nm );
			idx     = (int)out.images.size();
			out.images.push_back( std::move( im ) );
		}
		else
		{
			Log( "model %s: texture %s not found", path.c_str(), nm.c_str() );
			if( std::find( out.missing.begin(), out.missing.end(), nm ) == out.missing.end() ) out.missing.push_back( nm );
		}
		texIdx[ tx ] = idx;
		return idx;
	};
	//colour texture (may be -1) + opacity texture -> one image with alpha
	std::map< std::pair< int, int >, int > merged;
	auto withOpacity = [ & ]( int col, int op ) -> int {
		if( op < 0 ) return col;
		auto it = merged.find( { col, op } );
		if( it != merged.end() ) return it->second;
		MeshImage m = MergeOpacity( col >= 0 ? &out.images[ col ] : nullptr, out.images[ op ] );
		out.images.push_back( std::move( m ) );
		return merged[ { col, op } ] = (int)out.images.size() - 1;
	};
	std::map< int, bool > alphaOf;
	auto texAlpha = [ & ]( int t ) {
		if( t < 0 ) return false;
		auto it = alphaOf.find( t );
		if( it != alphaOf.end() ) return it->second;
		return alphaOf[ t ] = HasAlpha( out.images[ t ] );
	};
	std::vector< uint32_t > tri;
	for( size_t mi = 0; mi < scene->meshes.count; ++mi )
	{
		const ufbx_mesh* mesh = scene->meshes.data[ mi ];
		tri.resize( mesh->max_face_triangles * 3 );
		for( size_t ii = 0; ii < mesh->instances.count; ++ii )
		{
			const ufbx_node* node = mesh->instances.data[ ii ];
			for( size_t pi = 0; pi < mesh->material_parts.count; ++pi )
			{
				const ufbx_mesh_part& part = mesh->material_parts.data[ pi ];
				const ufbx_material* mat   = pi < node->materials.count ? node->materials.data[ pi ] : ( pi < mesh->materials.count ? mesh->materials.data[ pi ] : nullptr );
				V3 base( 0.8f, 0.8f, 0.8f );
				int tex      = -1;
				float cutoff = 0;
				bool colTex  = false;
				if( mat )
				{
					if( mat->pbr.base_color.has_value ) base = V3( (float)mat->pbr.base_color.value_vec4.x, (float)mat->pbr.base_color.value_vec4.y, (float)mat->pbr.base_color.value_vec4.z );
					else if( mat->fbx.diffuse_color.has_value ) base = V3( (float)mat->fbx.diffuse_color.value_vec3.x, (float)mat->fbx.diffuse_color.value_vec3.y, (float)mat->fbx.diffuse_color.value_vec3.z );
					if( mesh->vertex_uv.exists )
					{
						tex     = textureOf( mat->pbr.base_color.texture ? mat->pbr.base_color.texture : mat->fbx.diffuse_color.texture );
						colTex  = tex >= 0;
						//opacity map (OBJ map_d, FBX TransparentColor / opacity) goes into the alpha channel
						const ufbx_texture* opt = mat->pbr.opacity.texture ? mat->pbr.opacity.texture : ( mat->fbx.transparency_color.texture ? mat->fbx.transparency_color.texture : mat->fbx.transparency_factor.texture );
						const int op            = opt ? textureOf( opt ) : -1;
						if( op >= 0 && op != tex )
						{
							tex    = withOpacity( tex, op );
							cutoff = 0.5f;
						}
						else if( texAlpha( tex ) )
							cutoff = 0.5f;//diffuse PNG/TGA/TIF with transparency
					}
					if( colTex ) base = V3( 1, 1, 1 );//with a map the FBX diffuse colour is usually a leftover grey: show the texture as it is
					else if( base.x + base.y + base.z < 0.003f && tex >= 0 ) base = V3( 0.8f, 0.8f, 0.8f );//only an opacity map, black colour (C4D): neutral grey
				}
				uint32_t first = (uint32_t)out.idx.size();
				for( size_t fi = 0; fi < part.face_indices.count; ++fi )
				{
					ufbx_face face = mesh->faces.data[ part.face_indices.data[ fi ] ];
					uint32_t nt    = ufbx_triangulate_face( tri.data(), tri.size(), mesh, face );
					for( uint32_t k = 0; k < nt * 3; ++k )
					{
						uint32_t ix = tri[ k ];
						ufbx_vec3 p = ufbx_transform_position( &node->geometry_to_world, ufbx_get_vertex_vec3( &mesh->vertex_position, ix ) );
						ufbx_vec3 n = mesh->vertex_normal.exists ? ufbx_transform_direction( &node->geometry_to_world, ufbx_get_vertex_vec3( &mesh->vertex_normal, ix ) ) : ufbx_vec3{ 0, 1, 0 };
						ufbx_vec2 t = mesh->vertex_uv.exists ? ufbx_get_vertex_vec2( &mesh->vertex_uv, ix ) : ufbx_vec2{ 0, 0 };
						//FBX/OBJ: v points up, texture rows are stored top-down
						out.Vert( V3( (float)p.x, (float)p.y, (float)p.z ), Norm( V3( (float)n.x, (float)n.y, (float)n.z ) ), base, (float)t.x, 1.0f - (float)t.y );
						out.idx.push_back( out.Count() - 1 );
					}
				}
				out.parts.push_back( MeshPart{ first, (uint32_t)out.idx.size() - first, tex, cutoff } );
			}
		}
	}
	ufbx_free_scene( scene );
	return out.Count() > 0;
}

bool LoadModel( const std::string& path, MeshData& out, std::string* err )
{
	std::string l = ToLower( path );
	bool ok;
	if( l.size() > 5 && ( l.compare( l.size() - 4, 4, ".glb" ) == 0 || l.compare( l.size() - 5, 5, ".gltf" ) == 0 ) )
		ok = LoadGltf( path, out, err );
	else
		ok = LoadUfbx( path, out, err );
	if( ok && out.Count() == 0 )
	{
		if( err ) *err = "no geometry";
		return false;
	}
	return ok;
}
}//namespace ls
