// OpenGL 4.3 renderer: stage + fixtures (instanced), spot lights with shadow maps and gobos,
// raymarched volumetric beams in haze, lens glow, bloom and ACES tonemapping
#pragma once
#include "mesh.h"
#include "sim.h"
#include "video.h"
#include <GL/glew.h>
#include <map>

namespace ls
{
struct Camera
{
	V3 target{ 0, 2.5f, 0 };
	float yaw = 0, pitch = 12, dist = 17, fov = 50;
	V3 Eye() const;
	M4 View() const;
	M4 Proj( float aspect ) const;
};

struct Fbo
{
	GLuint fbo = 0, color = 0, depth = 0;
	int w = 0, h = 0;
	void Create( int w, int h, GLenum fmt, bool withDepth, bool depthTexture );
	void Destroy();
};

struct GpuMesh
{
	GLuint vao = 0, vbo = 0, ibo = 0;
	GLsizei count = 0;
	V3 bmin, bmax;
	std::vector< MeshPart > parts;//models: index ranges with their texture
	std::vector< GLuint > tex;    //models: base colour textures
	std::vector< std::string > missing;//models: textures that were not found
};

struct RenderStats
{
	int lights = 0, shadows = 0, beams = 0, instances = 0;
	float ms = 0;
};

class Renderer
{
public:
	bool Init( std::string* err );
	void Shutdown();
	void Render( Show& show, const SimGlobals& g, const Camera& cam, const std::vector< int >& selected, int w, int h );
	GLuint Output() const { return out_.color; }
	int Width() const { return out_.w; }
	int Height() const { return out_.h; }
	bool ReadOutput( std::vector< uint8_t >& rgba, int& w, int& h );
	//models: returns false if the file could not be loaded (error logged once)
	bool EnsureModel( ModelObj& m, std::string* err );
	void ForgetModel( const std::string& path );//drops the cached mesh + textures (next EnsureModel reloads)
	M4 viewProj;
	V3 eye;
	RenderStats stats;
	int shadowRes = 768;
	bool lensGlow = true;
	bool workLight = false;//overview light (visual only)
	const VideoHub* video = nullptr;//projector pictures
	//performance mode: temporary quality limits (the show settings stay untouched)
	struct Quality
	{
		bool shadows = true, bloom = true, glow = true, projHaze = true;
		float beamQuality = 0;//> 0: upper limit for stage.beamQuality
		bool high = false;    //2.1 "high": reflections, ambient occlusion, soft 1024 px shadows, GGX, finer cones
	} quality;
	uint64_t renderCount = 0;
	int selProjector = -1, selScreen = -1;//highlighted in the scene
	bool projLines   = true;//outline lines of the projector light cones (view option, settings.json)
	int HelperCount() const { return (int)helperCyls_.size(); }//outline cylinders of the last frame (tests)
	int ProjectorsLit() const { return (int)projs_.size(); }
	//for tests: the projector texture of a uid (0 = none)
	GLuint ProjectorTexture( uint32_t uid ) const { auto it = projTex_.find( uid ); return it == projTex_.end() ? 0 : it->second.tex; }

private:
	struct Inst
	{
		M4 m;
		float col[ 4 ];
		float emi[ 4 ];
	};
	struct LightGpu
	{
		float posRange[ 4 ], dirTan[ 4 ], upSoft[ 4 ], color[ 4 ], gobo[ 4 ], prism[ 4 ];
		float shadow[ 16 ];
	};
	struct ProjGpu
	{
		float vp[ 16 ];
		float posInt[ 4 ];   //lens position, intensity
		float dirShadow[ 4 ];//axis, shadow layer (-1 = none)
		float misc[ 4 ];     //mirror, black level, edge softness, blur (mip bias)
		float extra[ 4 ];    //picture width at 1 m, texture width, letterbox scale x / y
	};
	struct ProjTexture
	{
		GLuint tex = 0;
		int w = 0, h = 0;
		uint64_t serial = 0;
	};
	struct Beam
	{
		int light;
		V3 apex;
		V3 color;
		float tanOuter, range;
	};
	struct Glow
	{
		V3 pos, dir, color;
		float radius, cosOuter;
	};
	void BuildScene( Show& show, const std::vector< int >& sel, float time );
	void AddFixture( const Show& show, const FixtureInst& f, bool selected );
	void Box( const M4& m, const V3& col, float spec = 0.2f, const V3& emi = V3( 0, 0, 0 ), float flag = 0 );
	void Cyl( const M4& m, const V3& col, float spec = 0.2f, const V3& emi = V3( 0, 0, 0 ), float flag = 0 );
	void DrawInstances( GLuint prog, bool shadow );
	void UploadGobos();
	GpuMesh Upload( const MeshData& m );
	GLuint Program( const char* vs, const char* fs, const char* name );

	void UpdateProjectorTextures( const Show& show );
	void BuildProjectors( const Show& show, int& shadowCount, V3& scatter );
	void BuildScreens( const Show& show );
	GLuint progProjBeam_ = 0, uboProj_ = 0, blackTex_ = 0;
	GpuMesh projBox_;
	GLuint progSky_ = 0;
	int moonLayer_ = -1;//shadow array layer of the moon (-1 = none)
	M4 moonVP_;//own cube for the projector haze pass (the instanced box VAO must keep its instance attributes)
	std::vector< ProjGpu > projs_;
	std::vector< GLuint > projBind_;//texture per projs_ entry
	std::map< uint32_t, ProjTexture > projTex_;
	GLuint progScene_ = 0, progShadow_ = 0, progBeam_ = 0, progGlow_ = 0, progComb_ = 0, progDown_ = 0, progUp_ = 0, progFinal_ = 0;
	GpuMesh box_, cyl_, cone_, quad_;
	GLuint instVbo_ = 0, ubo_ = 0;
	GLuint shadowTex_ = 0, shadowFbo_ = 0, goboTex_ = 0;
	uint32_t goboVersion_ = 0;
	int goboLayers_ = 0;
	Fbo scene_, beam_, comb_, out_;
	std::vector< Fbo > bloom_;
	std::vector< Inst > boxes_, cyls_;
	std::vector< Inst > helperCyls_;//outlines: visible, but no shadows
	std::vector< LightGpu > lights_;
	std::vector< Beam > beams_;
	std::vector< Glow > glows_;
	std::vector< float > lightPower_;
	std::map< std::string, GpuMesh > models_;
	std::map< std::string, std::string > modelErrors_;
	int maxShadows_ = 48;
	int shadowCur_  = 0;//resolution of the allocated shadow array (768 normal, 1024 high)
	void EnsureShadowRes( int res );
	float time_     = 0;
	V3 ambient_;
};
}//namespace ls
