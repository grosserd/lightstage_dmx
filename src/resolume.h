//Resolume Arena "Advanced Output" preset from the projector / screen setup
#pragma once
#include "show.h"
#include <functional>
#include <string>
#include <vector>

namespace ls
{
//Every projector becomes a Resolume screen (NDI output, named like the NDI source LightStage receives),
//every LightStage screen it hits becomes a slice whose warp grid lands exactly on that screen.
//Shaped screens (black/white image) additionally get output masks for their black parts.
struct ResolumeOpts
{
	std::string name = "LightStage";
	int compW = 1920, compH = 1080;
	int input  = 0;              //0 part of the composition with the aspect of the screen (centred), 1 whole composition
	bool masks = true;           //screen shape -> masks
	float tol  = 0.5f;           //max. warp error in output pixels
	std::vector< uint8_t > skip; //per projector: 1 = leave out
};
struct ResolumeSlice
{
	std::string name;
	int screen = -1;
	float in[ 4 ] = { 0, 0, 0, 0 };          //input rect, composition pixels x0 y0 x1 y1
	int gw = 0, gh = 0;                      //bezier control net (3n+1) x (3m+1)
	std::vector< float > net;                //gw * gh * 2, output pixels, row 0 = top
	float corners[ 8 ] = {};                 //exact projected corners TL TR BR BL
	float maxErr = 0;                        //largest deviation of the warp from the exact projection (px)
	std::vector< std::vector< float > > masks;//output polygons (x y pairs), black parts of the screen
};
struct ResolumeScreen
{
	std::string name;//Resolume screen and NDI output name
	int projector = -1;
	int w = 1920, h = 1080;
	std::vector< ResolumeSlice > slices;
	std::vector< std::string > notes;//German keys, translated by the UI
};
using FrameSizeFn = std::function< void( uint32_t uid, int& w, int& h ) >;

std::vector< ResolumeScreen > BuildResolumeLayout( const Show& show, const ResolumeOpts& o, const FrameSizeFn& frame = nullptr );
std::string ResolumePresetXml( const std::vector< ResolumeScreen >& screens, const ResolumeOpts& o );
std::string ResolumePresetDir();//Documents\Resolume Arena\Presets\Advanced Output ("" = no Documents folder)
bool WriteResolumePreset( const std::string& path, const std::string& xml, std::string& err );//creates the folder

std::string ResolumeScreenName( const Projector& p );                      //"DG22 (Arena - Screen 1)" -> "Screen 1"
void ResolumeOutputSize( const Projector& p, int frameW, int frameH, int& w, int& h );
bool ProjectorPixel( const Projector& p, int w, int h, const V3& x, float& px, float& py );//false = behind the lens
V3 ScreenPoint( const Screen& s, float u, float v );                        //u right, v down, 0..1
std::vector< std::vector< float > > ScreenMaskPolys( const Screen& s );     //black parts, u v pairs (0..1, v down)
//bezier net evaluation (also used by the preview and the self test)
void ResolumeEvalNet( const ResolumeSlice& sl, float u, float v, float& x, float& y );//u, v 0..1 over the whole slice
}//namespace ls
