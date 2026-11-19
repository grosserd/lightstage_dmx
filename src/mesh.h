// CPU meshes: procedural primitives and imported models (glTF/GLB via cgltf, FBX/OBJ via ufbx) with base colour textures
#pragma once
#include "common.h"

namespace ls
{
struct MeshImage
{
	int w = 0, h = 0;
	std::vector< uint8_t > rgba;//rows top-down
	std::string name;
};
struct MeshPart
{
	uint32_t first = 0, count = 0;//index range
	int tex          = -1;        //MeshData::images, -1 = material colour only
	float cutoff     = 0;         //alpha test threshold (0 = opaque; leaves, fences: texture alpha below it is cut out)
};
struct MeshData
{
	static const int kStride = 11;//pos3 normal3 color3 uv2
	std::vector< float > v;
	std::vector< uint32_t > idx;
	std::vector< MeshPart > parts;   //empty = one part without texture
	std::vector< MeshImage > images; //base colour textures (alpha = opacity)
	std::vector< std::string > missing;//texture files that were referenced but not found / not readable
	V3 bmin{ 1e9f, 1e9f, 1e9f }, bmax{ -1e9f, -1e9f, -1e9f };
	void Vert( const V3& p, const V3& n, const V3& c = V3( 1, 1, 1 ), float u = 0, float t = 0 );
	uint32_t Count() const { return (uint32_t)( v.size() / kStride ); }
	int Textured() const;//number of parts with a texture
};
MeshData MakeBox();                     //unit cube centred, -0.5..0.5
MeshData MakeCylinder( int seg );       //radius 0.5, y -0.5..0.5, capped
MeshData MakeCone( int seg );           //apex at origin, opens along +Z, radius 1 at z = 1, capped
MeshData MakeQuad();                    //-1..1 xy, for billboards / fullscreen
bool LoadModel( const std::string& path, MeshData& out, std::string* err );
bool DecodeImage( const uint8_t* data, size_t size, MeshImage& out );//stb_image, then the OS decoder (TIF, DDS, ...)
//OS image decoder, rows top-down straight RGBA: Windows WIC (mesh.cpp), macOS ImageIO (image_mac.mm)
bool DecodeImageOs( const uint8_t* data, size_t size, int& w, int& h, std::vector< uint8_t >& rgba );
//looks for a texture referenced by a model: as given, next to the model, in texture subfolders, by name in the model folder tree
bool FindTextureFile( const std::string& modelDir, const std::string& ref, std::vector< uint8_t >& bytes, std::string* foundPath = nullptr );
}//namespace ls
