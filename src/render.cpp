#include "render.h"

namespace ls
{
//visual lumens: compress the range between small LED fixtures and big moving heads / blinders a little
static float EffLumens( float lm ) { return 3000.0f * powf( std::max( lm, 1.0f ) / 3000.0f, 0.7f ); }
static float Lum3( const V3& c ) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
//================================================================ shaders
static const char* kCommon = R"GLSL(
#version 430 core
struct Light { vec4 posRange; vec4 dirTan; vec4 upSoft; vec4 color; vec4 gobo; vec4 prism; mat4 shadow; };
layout(std140, binding = 0) uniform LightBlock { Light lights[96]; };
uniform sampler2DArray uGobo;
uniform sampler2DArrayShadow uShadow;
uniform float uShadowTexel;
float h3(vec3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
float vnoise(vec3 x)
{
	vec3 i = floor(x), f = fract(x);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(h3(i + vec3(0, 0, 0)), h3(i + vec3(1, 0, 0)), f.x), mix(h3(i + vec3(0, 1, 0)), h3(i + vec3(1, 1, 0)), f.x), f.y),
	           mix(mix(h3(i + vec3(0, 0, 1)), h3(i + vec3(1, 0, 1)), f.x), mix(h3(i + vec3(0, 1, 1)), h3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float fbm2(vec2 p)
{
	float a = 0.5, s = 0.0;
	for (int i = 0; i < 4; ++i) { s += a * vnoise(vec3(p, 0.37)); p = p * 2.03 + vec2(1.7, 9.2); a *= 0.5; }
	return s / 0.9375;
}
struct Proj { mat4 vp; vec4 posInt; vec4 dirShadow; vec4 misc; vec4 extra; };
layout(std140, binding = 1) uniform ProjBlock { Proj projs[8]; };
uniform sampler2D uProjTex[8];
uniform int uNumProj;

//light of video projector i arriving at p (before distance falloff); d2 = squared distance to the lens
vec3 ProjColor(int i, vec3 p, float lod, out float d2)
{
	Proj q = projs[i];
	vec3 dv = p - q.posInt.xyz;
	d2 = dot(dv, dv);
	vec4 c = q.vp * vec4(p, 1.0);
	if (c.w <= 0.0) return vec3(0.0);
	vec2 n = c.xy / c.w;
	if (abs(n.x) >= 1.0 || abs(n.y) >= 1.0) return vec3(0.0);
	vec2 uv = n * 0.5 + 0.5;
	float e = q.misc.z;
	float edge = smoothstep(0.0, e, uv.x) * smoothstep(0.0, e, 1.0 - uv.x) * smoothstep(0.0, e, uv.y) * smoothstep(0.0, e, 1.0 - uv.y);
	if (q.misc.x > 0.5) uv.x = 1.0 - uv.x;
	//letterbox: input with another aspect than the projector format
	vec2 tuv = (uv - 0.5) / q.extra.zw + 0.5;
	vec3 tex = vec3(0.0);
	if (tuv.x >= 0.0 && tuv.x <= 1.0 && tuv.y >= 0.0 && tuv.y <= 1.0) tex = textureLod(uProjTex[i], vec2(tuv.x, 1.0 - tuv.y), lod).rgb;
	return (tex + vec3(q.misc.y)) * (edge * q.posInt.w);
}
float ProjShadow(Proj q, vec3 p, float bias, bool pcf)
{
	if (q.dirShadow.w < -0.5) return 1.0;
	vec4 sc = q.vp * vec4(p, 1.0);
	if (sc.w <= 0.0) return 1.0;
	vec3 s = sc.xyz / sc.w * 0.5 + 0.5;
	if (s.x <= 0.0 || s.x >= 1.0 || s.y <= 0.0 || s.y >= 1.0 || s.z >= 1.0) return 1.0;
	if (!pcf) return texture(uShadow, vec4(s.xy, q.dirShadow.w, s.z - bias));
	float r = 0.0;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
			r += texture(uShadow, vec4(s.xy + vec2(x, y) * uShadowTexel, q.dirShadow.w, s.z - bias));
	return r / 9.0;
}

//beam cross-section at world point p: soft edge, hot spot, prism facets, gobo
float BeamProfile(Light l, vec3 p, out float z, out float dist2)
{
	vec3 q = p - l.posRange.xyz;
	vec3 D = l.dirTan.xyz, U = l.upSoft.xyz, R = cross(D, U);
	z = dot(q, D);
	dist2 = dot(q, q);
	if (z <= 0.002) return 0.0;
	vec2 xy = vec2(dot(q, R), dot(q, U)) / z;
	float tanB = l.dirTan.w, soft = l.upSoft.w;
	int facets = int(l.prism.x + 0.5);
	int nf = max(facets, 1);
	float sum = 0.0;
	for (int k = 0; k < nf; ++k)
	{
		vec2 c = vec2(0.0);
		if (facets > 0) { float a = l.prism.y + 6.2831853 * float(k) / float(facets); c = vec2(cos(a), sin(a)) * l.prism.z; }
		vec2 uv = (xy - c) / tanB;
		float r = length(uv);
		if (r >= 1.0) continue;
		float edge = 1.0 - smoothstep(1.0 - soft, 1.0, r);
		float hot = mix(1.0, max(0.0, 1.0 - 0.75 * r * r), l.prism.w);
		float g = 1.0;
		if (l.gobo.x > 0.5)
		{
			float cr = cos(l.gobo.y), sr = sin(l.gobo.y);
			vec2 ru = vec2(cr * uv.x - sr * uv.y, sr * uv.x + cr * uv.y);
			g = textureLod(uGobo, vec3(ru * 0.5 + 0.5, l.gobo.x), l.gobo.z).r;
		}
		sum += edge * hot * g;
	}
	return sum / float(nf);
}
float ShadowAt(Light l, vec3 p, float bias)
{
	if (l.color.w < -0.5) return 1.0;
	vec4 sc = l.shadow * vec4(p, 1.0);
	if (sc.w <= 0.0) return 1.0;
	vec3 s = sc.xyz / sc.w * 0.5 + 0.5;
	if (s.x <= 0.0 || s.x >= 1.0 || s.y <= 0.0 || s.y >= 1.0 || s.z >= 1.0) return 1.0;
	return texture(uShadow, vec4(s.xy, l.color.w, s.z - bias));
}
float ShadowPcf(Light l, vec3 p, float bias)
{
	if (l.color.w < -0.5) return 1.0;
	vec4 sc = l.shadow * vec4(p, 1.0);
	if (sc.w <= 0.0) return 1.0;
	vec3 s = sc.xyz / sc.w * 0.5 + 0.5;
	if (s.x <= 0.0 || s.x >= 1.0 || s.y <= 0.0 || s.y >= 1.0 || s.z >= 1.0) return 1.0;
	float r = 0.0;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
			r += texture(uShadow, vec4(s.xy + vec2(x, y) * uShadowTexel, l.color.w, s.z - bias));
	return r / 9.0;
}
)GLSL";

static const char* kSceneVs = R"GLSL(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec3 aCol;
layout(location = 3) in vec4 iM0;
layout(location = 4) in vec4 iM1;
layout(location = 5) in vec4 iM2;
layout(location = 6) in vec4 iM3;
layout(location = 7) in vec4 iCol;
layout(location = 8) in vec4 iEmi;
layout(location = 9) in vec2 aUv;
uniform mat4 uVP;
uniform int uInstanced;
uniform int uIgnoreVCol;
uniform mat4 uModel;
uniform vec4 uCol;
uniform vec4 uEmi;
out vec2 vUv;
out vec3 vPos;
out vec3 vNrm;
out vec3 vAlb;
out vec4 vEmi;
out float vSpec;
void main()
{
	mat4 M = uInstanced != 0 ? mat4(iM0, iM1, iM2, iM3) : uModel;
	vec4 col = uInstanced != 0 ? iCol : uCol;
	vec4 emi = uInstanced != 0 ? iEmi : uEmi;
	vec4 wp = M * vec4(aPos, 1.0);
	vPos = wp.xyz;
	vNrm = normalize(transpose(inverse(mat3(M))) * aNrm);
	vAlb = col.rgb * (uIgnoreVCol != 0 ? vec3(1.0) : aCol);
	vUv = aUv;
	vSpec = col.a;
	vEmi = emi;
	gl_Position = uVP * wp;
}
)GLSL";

static const char* kSceneFs = R"GLSL(
in vec3 vPos;
in vec3 vNrm;
in vec3 vAlb;
in vec4 vEmi;
in float vSpec;
in vec2 vUv;
uniform int uNumLights;
uniform vec3 uCamPos;
uniform vec3 uAmbient;
uniform float uEnv;
uniform int uUseTex;   //1 colour + alpha test, 2 alpha test only (single tint)
uniform float uCutoff;
uniform float uWork;   //work light (overview), 0 = off
uniform sampler2D uAlbedo;
uniform vec3 uMoonDir;
uniform vec3 uMoonCol;
uniform mat4 uMoonVP;
uniform float uMoonLayer;
out vec4 oColor;
//procedural ground types (outdoor): 1 grass, 2 concrete, 3 asphalt, 4 earth/gravel, 5 wooden floor
void Ground(int gt, vec2 xz, inout vec3 alb, inout float spec)
{
	if (gt == 1)
	{
		float big = fbm2(xz * 0.35), fine = vnoise(vec3(xz * 18.0, 0.5));
		alb = vec3(0.030, 0.060, 0.018) * mix(0.6, 1.25, big) * mix(0.8, 1.15, fine);
		float dry = smoothstep(0.62, 0.8, fbm2(xz * 0.08 + 3.1));
		alb = mix(alb, vec3(0.06, 0.055, 0.025), dry * 0.7);
		spec = 0.04;
	}
	else if (gt == 2)
	{
		alb = vec3(0.16, 0.16, 0.155) * mix(0.85, 1.1, fbm2(xz * 1.5)) * mix(0.94, 1.06, vnoise(vec3(xz * 60.0, 1.0)));
		vec2 q = abs(fract(xz / 3.0 + 0.5) - 0.5) * 3.0;
		alb *= mix(0.45, 1.0, smoothstep(0.004, 0.009, min(q.x, q.y)));
		spec = 0.12;
	}
	else if (gt == 3)
	{
		alb = vec3(0.045) * mix(0.75, 1.25, vnoise(vec3(xz * 90.0, 2.0))) * mix(0.9, 1.1, fbm2(xz * 0.5));
		spec = 0.25;
	}
	else if (gt == 4)
	{
		alb = vec3(0.07, 0.05, 0.035) * mix(0.8, 1.2, fbm2(xz * 2.0));
		float st = smoothstep(0.80, 0.86, vnoise(vec3(xz * 25.0, 3.0)));
		alb = mix(alb, vec3(0.14, 0.13, 0.12), st);
		spec = 0.05;
	}
	else if (gt == 5)
	{
		float idx = floor(xz.y / 0.18);
		float grain = sin(xz.x * 40.0 + fbm2(vec2(xz.x * 0.5, idx * 7.0)) * 6.0) * 0.5 + 0.5;
		alb = vec3(0.11, 0.06, 0.03) * mix(0.8, 1.15, h3(vec3(idx, 1.3, 7.1))) * mix(0.9, 1.05, grain);
		alb *= mix(0.4, 1.0, smoothstep(0.0, 0.03, fract(xz.y / 0.18)));
		spec = 0.3;
	}
}
void main()
{
	float foot = length(fwidth(vPos));//pixel footprint in metres (projector mip level)
	vec3 alb = vAlb;
	if (uUseTex != 0)
	{
		vec4 tc = texture(uAlbedo, vUv);
		if (uCutoff > 0.0 && tc.a < uCutoff) discard;
		if (uUseTex == 1) alb *= tc.rgb;
	}
	float specK = vSpec;
	int gt = int(vEmi.a + 0.5) - 1;//-1 = no ground, 0 = hall floor with grid, 1.. = outdoor grounds
	if (gt >= 1) Ground(gt, vPos.xz, alb, specK);
	vec3 N = normalize(vNrm);
	vec3 V = normalize(uCamPos - vPos);
	if (dot(N, V) < 0.0) N = -N;
	vec3 diff = uAmbient * (0.6 + 0.4 * N.y);
	vec3 spec = vec3(0.0);
	float shin = mix(16.0, 90.0, clamp(specK, 0.0, 1.0));
	for (int i = 0; i < uNumLights; ++i)
	{
		Light l = lights[i];
		vec3 Lv = l.posRange.xyz - vPos;
		float d = length(Lv);
		vec3 L = Lv / max(d, 1e-4);
		float ndl = dot(N, L);
		if (ndl <= 0.0) continue;
		float z, d2;
		float prof = BeamProfile(l, vPos, z, d2);
		if (prof <= 0.0) continue;
		float sh = ShadowPcf(l, vPos + N * 0.02, 0.0004 + 0.0015 * (1.0 - ndl));
		if (sh <= 0.0) continue;
		vec3 E = l.color.rgb * (prof * sh / max(d2, 0.04));
		diff += E * ndl;
		vec3 H = normalize(L + V);
		spec += E * pow(max(dot(N, H), 0.0), shin) * ndl * specK * 2.0;
	}
	//video projectors
	for (int i = 0; i < uNumProj; ++i)
	{
		vec3 Lv = projs[i].posInt.xyz - vPos;
		float d = length(Lv);
		vec3 L = Lv / max(d, 1e-4);
		float ndl = dot(N, L);
		if (ndl <= 0.0) continue;
		float lod = log2(max(1.0, foot * projs[i].extra.y / max(d * projs[i].extra.x, 1e-4))) + projs[i].misc.w;
		float d2;
		vec3 E0 = ProjColor(i, vPos, lod, d2);
		if (max(E0.r, max(E0.g, E0.b)) <= 0.0) continue;
		float sh = ProjShadow(projs[i], vPos + N * 0.02, 0.0004 + 0.0015 * (1.0 - ndl), true);
		if (sh <= 0.0) continue;
		vec3 E = E0 * (sh / max(d2, 0.04));
		diff += E * ndl;
		vec3 H = normalize(L + V);
		spec += E * pow(max(dot(N, H), 0.0), shin) * ndl * specK * 2.0;
	}
	//moonlight (outdoor), optional shadow
	if (uMoonCol.r + uMoonCol.g + uMoonCol.b > 0.0)
	{
		float ndl = dot(N, uMoonDir);
		if (ndl > 0.0)
		{
			float sh = 1.0;
			if (uMoonLayer >= 0.0)
			{
				vec4 sc = uMoonVP * vec4(vPos + N * 0.03, 1.0);
				vec3 s = sc.xyz / sc.w * 0.5 + 0.5;
				if (s.x > 0.0 && s.x < 1.0 && s.y > 0.0 && s.y < 1.0 && s.z < 1.0)
				{
					sh = 0.0;
					for (int y = -1; y <= 1; ++y)
						for (int x = -1; x <= 1; ++x)
							sh += texture(uShadow, vec4(s.xy + vec2(x, y) * uShadowTexel, uMoonLayer, s.z - 0.0006));
					sh /= 9.0;
				}
			}
			diff += uMoonCol * (ndl * sh);
			spec += uMoonCol * (pow(max(dot(N, normalize(uMoonDir + V)), 0.0), shin) * ndl * specK * sh);
		}
	}
	//work light: neutral fill from the viewer + sky, everything readable without fixtures
	if (uWork > 0.0) diff += vec3(uWork) * (0.25 + 0.55 * max(dot(N, V), 0.0) + 0.3 * max(N.y, 0.0));
	vec3 c = alb * diff + spec + vEmi.rgb;
	//floor grid
	if (gt == 0)
	{
		vec2 g = abs(fract(vPos.xz + 0.5) - 0.5) / fwidth(vPos.xz);
		float line = 1.0 - min(min(g.x, g.y), 1.0);
		c += vec3(0.007, 0.0075, 0.009) * uEnv * line * clamp(1.0 - length(vPos - uCamPos) / 60.0, 0.0, 1.0);
	}
	oColor = vec4(c, 1.0);
}
)GLSL";

static const char* kShadowVs = R"GLSL(
#version 430 core
layout(location = 0) in vec3 aPos;
layout(location = 3) in vec4 iM0;
layout(location = 4) in vec4 iM1;
layout(location = 5) in vec4 iM2;
layout(location = 6) in vec4 iM3;
layout(location = 9) in vec2 aUv;
uniform mat4 uVP;
uniform int uInstanced;
uniform mat4 uModel;
out vec2 vUv;
void main()
{
	mat4 M = uInstanced != 0 ? mat4(iM0, iM1, iM2, iM3) : uModel;
	vUv = aUv;
	gl_Position = uVP * M * vec4(aPos, 1.0);
}
)GLSL";
static const char* kShadowFs = R"GLSL(
#version 430 core
in vec2 vUv;
uniform int uUseTex;
uniform float uCutoff;
uniform sampler2D uAlbedo;
void main()
{
	//cut-out textures (leaves) cast the right shadow
	if (uUseTex != 0 && uCutoff > 0.0 && texture(uAlbedo, vUv).a < uCutoff) discard;
}
)GLSL";

static const char* kBeamVs = R"GLSL(
layout(location = 0) in vec3 aPos;
uniform mat4 uVP;
uniform mat4 uModel;
void main() { gl_Position = uVP * uModel * vec4(aPos, 1.0); }
)GLSL";

static const char* kBeamFs = R"GLSL(
uniform int uLight;
uniform vec3 uApex;
uniform vec3 uBeamColor;
uniform float uTanOuter;
uniform float uRange;
uniform sampler2D uDepth;
uniform mat4 uInvVP;
uniform vec3 uCamPos;
uniform vec2 uSize;
uniform float uHaze;
uniform float uNoise;
uniform float uTime;
uniform int uSteps;
out vec4 oColor;

bool Inside(vec3 p, vec3 A, vec3 D, float cos2, float L)
{
	vec3 q = p - A;
	float z = dot(q, D);
	return z > 0.0 && z <= L && z * z >= dot(q, q) * cos2;
}
void main()
{
	vec2 uv = gl_FragCoord.xy / uSize;
	float dz = texture(uDepth, uv).r;
	vec4 pf = uInvVP * vec4(uv * 2.0 - 1.0, dz * 2.0 - 1.0, 1.0);
	pf /= pf.w;
	vec3 O = uCamPos;
	vec3 V = normalize(pf.xyz - O);
	float tScene = dz >= 0.99999 ? 1e4 : dot(pf.xyz - O, V);
	Light l = lights[uLight];
	vec3 A = uApex, D = l.dirTan.xyz;
	float cos2 = 1.0 / (1.0 + uTanOuter * uTanOuter);
	//candidate interval ends: 0, scene, cone roots, cap plane
	float t[6];
	int n = 0;
	t[n++] = 0.0;
	t[n++] = tScene;
	vec3 co = O - A;
	float vd = dot(V, D), cd = dot(co, D);
	float a = vd * vd - cos2, b = 2.0 * (vd * cd - dot(V, co) * cos2), c = cd * cd - dot(co, co) * cos2;
	if (abs(a) > 1e-7)
	{
		float disc = b * b - 4.0 * a * c;
		if (disc >= 0.0) { float s = sqrt(disc); t[n++] = (-b - s) / (2.0 * a); t[n++] = (-b + s) / (2.0 * a); }
	}
	if (abs(vd) > 1e-6) t[n++] = (uRange - cd) / vd;
	for (int i = 0; i < n; ++i) t[i] = clamp(t[i], 0.0, tScene);
	for (int i = 0; i < n; ++i)
		for (int j = i + 1; j < n; ++j)
			if (t[j] < t[i]) { float x = t[i]; t[i] = t[j]; t[j] = x; }
	float t0 = 1e9, t1 = -1.0;
	for (int i = 0; i + 1 < n; ++i)
	{
		if (t[i + 1] - t[i] < 1e-4) continue;
		if (Inside(O + V * (0.5 * (t[i] + t[i + 1])), A, D, cos2, uRange)) { t0 = min(t0, t[i]); t1 = max(t1, t[i + 1]); }
	}
	if (t1 <= t0) discard;
	float len = t1 - t0;
	int steps = clamp(int(float(uSteps) * clamp(len / 6.0, 0.35, 1.6)), 6, 96);
	float dt = len / float(steps);
	float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	vec3 acc = vec3(0.0);
	const float g = 0.35;
	Light lb = l;
	lb.posRange.xyz = A;
	for (int i = 0; i < steps; ++i)
	{
		vec3 p = O + V * (t0 + (float(i) + jit) * dt);
		float z, d2;
		float prof = BeamProfile(lb, p, z, d2);
		if (prof <= 0.0) continue;
		float sh = ShadowAt(l, p, 0.0006);
		if (sh <= 0.0) continue;
		vec3 Ld = normalize(p - A);
		float ct = dot(Ld, -V);
		float phase = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * ct, 1.5);
		float nz = 1.0;
		if (uNoise > 0.0)
		{
			vec3 np = p * 0.45 + vec3(uTime * 0.07, uTime * 0.025, uTime * 0.04);
			nz = vnoise(np) * 0.65 + vnoise(np * 2.3) * 0.35;
			nz = max(0.0, 1.0 + uNoise * (nz * 2.0 - 1.0) * 1.2);
		}
		acc += prof * sh * phase * nz / max(d2, 0.05);
	}
	oColor = vec4(uBeamColor * acc * dt * uHaze, 1.0);
}
)GLSL";

//outdoor sky: gradient, stars, moon (drawn behind everything)
static const char* kSkyFs = R"GLSL(
#version 430 core
in vec2 vUv;
uniform mat4 uInvVP;
uniform vec3 uCamPos;
uniform vec3 uZenith;
uniform vec3 uHorizon;
uniform vec3 uMoonDir;
uniform float uStars;
uniform float uMoon;
uniform float uEnv;
uniform float uTime;
out vec4 oColor;
float hh(vec3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
void main()
{
	vec4 p = uInvVP * vec4(vUv * 2.0 - 1.0, 1.0, 1.0);
	vec3 d = normalize(p.xyz / p.w - uCamPos);
	vec3 c = d.y >= 0.0 ? mix(uHorizon, uZenith, pow(d.y, 0.5)) : uHorizon;
	if (uStars > 0.0 && d.y > 0.02)
	{
		vec3 g = d * 600.0;
		vec3 cell = floor(g);
		float r = hh(cell);
		if (r > 0.9988)
		{
			float dd = length(fract(g) - 0.5);
			float tw = 0.7 + 0.3 * sin(uTime * (1.5 + r * 4.0) + r * 60.0);
			float mag = hh(cell + 3.1);
			c += vec3(1.0, 0.96, 0.9) * (uStars * (0.15 + 0.6 * mag * mag) * tw * smoothstep(0.5, 0.1, dd) * smoothstep(0.02, 0.15, d.y));
		}
	}
	if (uMoon > 0.0)
	{
		float md = dot(d, uMoonDir);
		c += vec3(0.9, 0.95, 1.0) * (uMoon * 2.5 * smoothstep(0.99990, 0.99995, md));
		c += vec3(0.6, 0.7, 1.0) * (uMoon * (0.05 * pow(max(md, 0.0), 300.0) + 0.006 * pow(max(md, 0.0), 12.0)));
	}
	oColor = vec4(c * uEnv, 1.0);
}
)GLSL";

//projector light in the haze: the frustum box is rasterised, only its exit face marches (camera inside works too)
static const char* kProjBeamVs = R"GLSL(
layout(location = 0) in vec3 aPos;
uniform mat4 uVP;
uniform mat4 uInvProj;
void main()
{
	vec4 w = uInvProj * vec4(aPos * 2.0, 1.0);
	w /= w.w;
	gl_Position = uVP * vec4(w.xyz, 1.0);
}
)GLSL";
static const char* kProjBeamFs = R"GLSL(
uniform int uProj;
uniform sampler2D uDepth;
uniform mat4 uInvVP;
uniform vec3 uCamPos;
uniform vec2 uSize;
uniform float uHaze;
uniform float uNoise;
uniform float uTime;
uniform int uSteps;
out vec4 oColor;
vec4 Row(mat4 m, int r) { return vec4(m[0][r], m[1][r], m[2][r], m[3][r]); }
void main()
{
	vec2 uv = gl_FragCoord.xy / uSize;
	float dz = texture(uDepth, uv).r;
	vec4 pf = uInvVP * vec4(uv * 2.0 - 1.0, dz * 2.0 - 1.0, 1.0);
	pf /= pf.w;
	vec3 O = uCamPos;
	vec3 V = normalize(pf.xyz - O);
	float tScene = dz >= 0.99999 ? 1e4 : dot(pf.xyz - O, V);
	vec4 ff = uInvVP * vec4(uv * 2.0 - 1.0, gl_FragCoord.z * 2.0 - 1.0, 1.0);
	ff /= ff.w;
	float tFrag = dot(ff.xyz - O, V);
	Proj q = projs[uProj];
	vec4 r0 = Row(q.vp, 0), r1 = Row(q.vp, 1), r2 = Row(q.vp, 2), r3 = Row(q.vp, 3);
	vec4 pl[6] = vec4[6](r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2);
	float t0 = 0.0, t1 = 1e5;
	for (int k = 0; k < 6; ++k)
	{
		float a = dot(pl[k].xyz, O) + pl[k].w, b = dot(pl[k].xyz, V);
		if (abs(b) < 1e-9) { if (a < 0.0) discard; }
		else { float t = -a / b; if (b > 0.0) t0 = max(t0, t); else t1 = min(t1, t); }
	}
	if (t1 <= t0) discard;
	if (abs(tFrag - t1) > 0.03 * t1 + 0.05) discard;//one face per pixel: the exit
	t1 = min(t1, tScene);
	if (t1 <= t0) discard;
	float len = t1 - t0;
	int steps = clamp(int(float(uSteps) * clamp(len / 6.0, 0.35, 1.6)), 6, 96);
	float dt = len / float(steps);
	float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	vec3 acc = vec3(0.0);
	const float g = 0.35;
	for (int i = 0; i < steps; ++i)
	{
		vec3 p = O + V * (t0 + (float(i) + jit) * dt);
		float d2;
		vec3 c = ProjColor(uProj, p, 3.0 + q.misc.w, d2);
		if (max(c.r, max(c.g, c.b)) <= 0.0) continue;
		float sh = ProjShadow(q, p, 0.0006, false);
		if (sh <= 0.0) continue;
		vec3 Ld = normalize(p - q.posInt.xyz);
		float ct = dot(Ld, -V);
		float phase = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * ct, 1.5);
		float nz = 1.0;
		if (uNoise > 0.0)
		{
			vec3 np = p * 0.45 + vec3(uTime * 0.07, uTime * 0.025, uTime * 0.04);
			nz = vnoise(np) * 0.65 + vnoise(np * 2.3) * 0.35;
			nz = max(0.0, 1.0 + uNoise * (nz * 2.0 - 1.0) * 1.2);
		}
		//soft near zone: right at the lens the picture is tiny and the 1/d^2 peak would flood the view
		acc += c * (sh * phase * nz / max(d2, 2.5));
	}
	oColor = vec4(acc * dt * uHaze * 0.3, 1.0);
}
)GLSL";

static const char* kGlowVs = R"GLSL(
#version 430 core
layout(location = 0) in vec3 aPos;
uniform mat4 uVP;
uniform vec3 uPos;
uniform vec3 uRight;
uniform vec3 uUp;
uniform float uSize;
out vec2 vUv;
void main()
{
	vUv = aPos.xy;
	vec3 p = uPos + (uRight * aPos.x + uUp * aPos.y) * uSize;
	gl_Position = uVP * vec4(p, 1.0);
}
)GLSL";
static const char* kGlowFs = R"GLSL(
#version 430 core
in vec2 vUv;
uniform vec3 uColor;
out vec4 oColor;
void main()
{
	float r = length(vUv);
	if (r >= 1.0) discard;
	float k = exp(-r * r * 9.0) * 0.8 + exp(-r * 4.5) * 0.2 * (1.0 - r);
	oColor = vec4(uColor * k, 1.0);
}
)GLSL";

static const char* kFullVs = R"GLSL(
#version 430 core
layout(location = 0) in vec3 aPos;
out vec2 vUv;
void main() { vUv = aPos.xy * 0.5 + 0.5; gl_Position = vec4(aPos.xy, 0.0, 1.0); }
)GLSL";
static const char* kCombFs = R"GLSL(
#version 430 core
in vec2 vUv;
uniform sampler2D uScene;
uniform sampler2D uBeam;
uniform sampler2D uDepth;
uniform mat4 uInvVP;
uniform vec3 uCamPos;
uniform float uFog;
uniform vec3 uFogCol;
out vec4 oColor;
uniform vec2 uBeamTexel;
void main()
{
	vec3 sc = texture(uScene, vUv).rgb;
	if (uFog > 0.0)
	{
		//distance haze outdoors (geometry only, the sky already has its colour)
		float dz = texture(uDepth, vUv).r;
		if (dz < 0.99999)
		{
			vec4 p = uInvVP * vec4(vUv * 2.0 - 1.0, dz * 2.0 - 1.0, 1.0);
			float dist = length(p.xyz / p.w - uCamPos);
			sc = mix(sc, uFogCol, 1.0 - exp(-dist * 0.008 * uFog));
		}
	}
	//soft tent upsample of the half resolution beams removes raymarch jitter
	vec3 b = texture(uBeam, vUv).rgb * 4.0;
	b += texture(uBeam, vUv + vec2(uBeamTexel.x, 0)).rgb * 2.0;
	b += texture(uBeam, vUv - vec2(uBeamTexel.x, 0)).rgb * 2.0;
	b += texture(uBeam, vUv + vec2(0, uBeamTexel.y)).rgb * 2.0;
	b += texture(uBeam, vUv - vec2(0, uBeamTexel.y)).rgb * 2.0;
	b += texture(uBeam, vUv + uBeamTexel).rgb;
	b += texture(uBeam, vUv - uBeamTexel).rgb;
	b += texture(uBeam, vUv + vec2(uBeamTexel.x, -uBeamTexel.y)).rgb;
	b += texture(uBeam, vUv + vec2(-uBeamTexel.x, uBeamTexel.y)).rgb;
	oColor = vec4(sc + b / 16.0, 1.0);
}
)GLSL";
static const char* kDownFs = R"GLSL(
#version 430 core
in vec2 vUv;
uniform sampler2D uSrc;
uniform vec2 uTexel;
uniform int uFirst;
out vec4 oColor;
vec3 S(vec2 o) { vec3 c = texture(uSrc, vUv + o * uTexel).rgb; return uFirst != 0 ? min(c, vec3(60.0)) : c; }
void main()
{
	vec3 a = S(vec2(-2, 2)), b = S(vec2(0, 2)), c = S(vec2(2, 2));
	vec3 d = S(vec2(-2, 0)), e = S(vec2(0, 0)), f = S(vec2(2, 0));
	vec3 g = S(vec2(-2, -2)), h = S(vec2(0, -2)), i = S(vec2(2, -2));
	vec3 j = S(vec2(-1, 1)), k = S(vec2(1, 1)), l = S(vec2(-1, -1)), m = S(vec2(1, -1));
	vec3 r = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
	oColor = vec4(r, 1.0);
}
)GLSL";
static const char* kUpFs = R"GLSL(
#version 430 core
in vec2 vUv;
uniform sampler2D uSrc;
uniform vec2 uTexel;
out vec4 oColor;
void main()
{
	vec3 r = vec3(0.0);
	r += texture(uSrc, vUv + vec2(-1, 1) * uTexel).rgb * 1.0;
	r += texture(uSrc, vUv + vec2(0, 1) * uTexel).rgb * 2.0;
	r += texture(uSrc, vUv + vec2(1, 1) * uTexel).rgb * 1.0;
	r += texture(uSrc, vUv + vec2(-1, 0) * uTexel).rgb * 2.0;
	r += texture(uSrc, vUv).rgb * 4.0;
	r += texture(uSrc, vUv + vec2(1, 0) * uTexel).rgb * 2.0;
	r += texture(uSrc, vUv + vec2(-1, -1) * uTexel).rgb * 1.0;
	r += texture(uSrc, vUv + vec2(0, -1) * uTexel).rgb * 2.0;
	r += texture(uSrc, vUv + vec2(1, -1) * uTexel).rgb * 1.0;
	oColor = vec4(r / 16.0, 1.0);
}
)GLSL";
static const char* kFinalFs = R"GLSL(
#version 430 core
in vec2 vUv;
uniform sampler2D uHdr;
uniform sampler2D uBloom;
uniform float uExposure;
uniform float uBloomAmt;
out vec4 oColor;
vec3 Aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
void main()
{
	vec3 c = texture(uHdr, vUv).rgb;
	vec3 b = texture(uBloom, vUv).rgb;
	c = mix(c, b, clamp(uBloomAmt * 0.12, 0.0, 0.6)) + b * uBloomAmt * 0.05;
	c *= uExposure;
	vec2 q = vUv - 0.5;
	c *= 1.0 - dot(q, q) * 0.35;
	c = Aces(c);
	c = pow(c, vec3(1.0 / 2.2));
	float dn = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	c += (dn - 0.5) / 255.0;
	oColor = vec4(c, 1.0);
}
)GLSL";

//================================================================ helpers
static M4 Ortho( float l, float r, float b, float t, float n, float f )
{
	M4 m;
	m( 0, 0 ) = 2 / ( r - l );
	m( 1, 1 ) = 2 / ( t - b );
	m( 2, 2 ) = -2 / ( f - n );
	m( 0, 3 ) = -( r + l ) / ( r - l );
	m( 1, 3 ) = -( t + b ) / ( t - b );
	m( 2, 3 ) = -( f + n ) / ( f - n );
	return m;
}
static M4 Frustum( float l, float r, float b, float t, float n, float f )
{
	M4 m;
	m( 0, 0 ) = 2 * n / ( r - l );
	m( 0, 2 ) = ( r + l ) / ( r - l );
	m( 1, 1 ) = 2 * n / ( t - b );
	m( 1, 2 ) = ( t + b ) / ( t - b );
	m( 2, 2 ) = -( f + n ) / ( f - n );
	m( 2, 3 ) = -2 * f * n / ( f - n );
	m( 3, 2 ) = -1;
	m( 3, 3 ) = 0;
	return m;
}
V3 Camera::Eye() const
{
	float y = yaw * kDeg, p = pitch * kDeg;
	return target + V3( sinf( y ) * cosf( p ), sinf( p ), cosf( y ) * cosf( p ) ) * dist;
}
M4 Camera::View() const { return LookAt( Eye(), target, V3( 0, 1, 0 ) ); }
M4 Camera::Proj( float aspect ) const { return Perspective( fov * kDeg, aspect, 0.05f, 1200.0f ); }

void Fbo::Create( int W, int H, GLenum fmt, bool withDepth, bool depthTexture )
{
	if( fbo && W == w && H == h ) return;
	Destroy();
	w = W;
	h = H;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glGenTextures( 1, &color );
	glBindTexture( GL_TEXTURE_2D, color );
	glTexStorage2D( GL_TEXTURE_2D, 1, fmt, w, h );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0 );
	if( withDepth )
	{
		glGenTextures( 1, &depth );
		glBindTexture( GL_TEXTURE_2D, depth );
		glTexStorage2D( GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, w, h );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0 );
	}
	(void)depthTexture;
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
}
void Fbo::Destroy()
{
	if( color ) glDeleteTextures( 1, &color );
	if( depth ) glDeleteTextures( 1, &depth );
	if( fbo ) glDeleteFramebuffers( 1, &fbo );
	fbo = color = depth = 0;
	w = h = 0;
}

static GLuint Compile( GLenum type, const std::string& src, const char* name, std::string* err )
{
	GLuint s      = glCreateShader( type );
	const char* p = src.c_str();
	glShaderSource( s, 1, &p, nullptr );
	glCompileShader( s );
	GLint ok = 0;
	glGetShaderiv( s, GL_COMPILE_STATUS, &ok );
	if( !ok )
	{
		char log[ 4096 ];
		glGetShaderInfoLog( s, sizeof( log ), nullptr, log );
		Log( "shader %s (%s) error:\n%s", name, type == GL_VERTEX_SHADER ? "vs" : "fs", log );
		if( err ) *err += std::string( name ) + ": " + log;
		glDeleteShader( s );
		return 0;
	}
	return s;
}
static std::string g_shaderErr;
GLuint Renderer::Program( const char* vs, const char* fs, const char* name )
{
	auto withCommon = []( const char* s ) {
		std::string x = s;
		size_t p      = x.find_first_not_of( " \r\n\t" );
		if( p != std::string::npos && x.compare( p, 8, "#version" ) == 0 ) return x;
		return std::string( kCommon ) + x;
	};
	GLuint v = Compile( GL_VERTEX_SHADER, withCommon( vs ), name, &g_shaderErr );
	GLuint f = Compile( GL_FRAGMENT_SHADER, withCommon( fs ), name, &g_shaderErr );
	if( !v || !f ) return 0;
	GLuint p = glCreateProgram();
	glAttachShader( p, v );
	glAttachShader( p, f );
	glLinkProgram( p );
	glDeleteShader( v );
	glDeleteShader( f );
	GLint ok = 0;
	glGetProgramiv( p, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		char log[ 4096 ];
		glGetProgramInfoLog( p, sizeof( log ), nullptr, log );
		Log( "program %s link error:\n%s", name, log );
		g_shaderErr += std::string( name ) + " link: " + log;
		return 0;
	}
	return p;
}

GpuMesh Renderer::Upload( const MeshData& m )
{
	GpuMesh g;
	glGenVertexArrays( 1, &g.vao );
	glBindVertexArray( g.vao );
	glGenBuffers( 1, &g.vbo );
	glBindBuffer( GL_ARRAY_BUFFER, g.vbo );
	glBufferData( GL_ARRAY_BUFFER, m.v.size() * 4, m.v.data(), GL_STATIC_DRAW );
	glEnableVertexAttribArray( 0 );
	const GLsizei S = MeshData::kStride * 4;
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, S, (void*)0 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 3, GL_FLOAT, GL_FALSE, S, (void*)12 );
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 3, GL_FLOAT, GL_FALSE, S, (void*)24 );
	glEnableVertexAttribArray( 9 );
	glVertexAttribPointer( 9, 2, GL_FLOAT, GL_FALSE, S, (void*)36 );
	glGenBuffers( 1, &g.ibo );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, g.ibo );
	glBufferData( GL_ELEMENT_ARRAY_BUFFER, m.idx.size() * 4, m.idx.data(), GL_STATIC_DRAW );
	g.count = (GLsizei)m.idx.size();
	g.bmin  = m.bmin;
	g.bmax  = m.bmax;
	g.parts = m.parts;
	if( g.parts.empty() ) g.parts.push_back( MeshPart{ 0, (uint32_t)m.idx.size(), -1, 0 } );
	g.missing = m.missing;
	for( auto& im : m.images )
	{
		GLuint t = 0;
		glGenTextures( 1, &t );
		glBindTexture( GL_TEXTURE_2D, t );
		int levels = 1;
		for( int s = std::max( im.w, im.h ); s > 1; s >>= 1 ) ++levels;
		glTexStorage2D( GL_TEXTURE_2D, levels, GL_SRGB8_ALPHA8, im.w, im.h );
		glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, im.w, im.h, GL_RGBA, GL_UNSIGNED_BYTE, im.rgba.data() );
		glGenerateMipmap( GL_TEXTURE_2D );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );
		if( GLEW_EXT_texture_filter_anisotropic ) glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.0f );
		g.tex.push_back( t );
	}
	//instance attributes (bound to the shared instance buffer)
	if( instVbo_ )
	{
		glBindBuffer( GL_ARRAY_BUFFER, instVbo_ );
		for( int i = 0; i < 6; ++i )
		{
			glEnableVertexAttribArray( 3 + i );
			glVertexAttribPointer( 3 + i, 4, GL_FLOAT, GL_FALSE, sizeof( Inst ), (void*)( size_t )( i * 16 ) );
			glVertexAttribDivisor( 3 + i, 1 );
		}
	}
	glBindVertexArray( 0 );
	return g;
}

bool Renderer::Init( std::string* err )
{
	g_shaderErr.clear();
	glGenBuffers( 1, &instVbo_ );
	glBindBuffer( GL_ARRAY_BUFFER, instVbo_ );
	glBufferData( GL_ARRAY_BUFFER, sizeof( Inst ) * 16, nullptr, GL_DYNAMIC_DRAW );
	box_  = Upload( MakeBox() );
	cyl_  = Upload( MakeCylinder( 28 ) );
	cone_ = Upload( MakeCone( 40 ) );
	quad_ = Upload( MakeQuad() );
	progScene_  = Program( kSceneVs, kSceneFs, "scene" );
	progShadow_ = Program( kShadowVs, kShadowFs, "shadow" );
	progBeam_   = Program( kBeamVs, kBeamFs, "beam" );
	progGlow_   = Program( kGlowVs, kGlowFs, "glow" );
	progComb_   = Program( kFullVs, kCombFs, "combine" );
	progDown_   = Program( kFullVs, kDownFs, "down" );
	progUp_     = Program( kFullVs, kUpFs, "up" );
	progFinal_  = Program( kFullVs, kFinalFs, "final" );
	progProjBeam_ = Program( kProjBeamVs, kProjBeamFs, "projector haze" );
	progSky_      = Program( kFullVs, kSkyFs, "sky" );
	if( !progSky_ )
	{
		if( err ) *err = g_shaderErr;
		return false;
	}
	projBox_      = Upload( MakeBox() );
	glBindVertexArray( projBox_.vao );
	for( int i = 3; i <= 8; ++i ) glDisableVertexAttribArray( i );
	glBindVertexArray( 0 );
	if( !progScene_ || !progShadow_ || !progBeam_ || !progGlow_ || !progComb_ || !progDown_ || !progUp_ || !progFinal_ || !progProjBeam_ )
	{
		if( err ) *err = g_shaderErr;
		return false;
	}
	glGenBuffers( 1, &ubo_ );
	glBindBuffer( GL_UNIFORM_BUFFER, ubo_ );
	glBufferData( GL_UNIFORM_BUFFER, sizeof( LightGpu ) * 96, nullptr, GL_DYNAMIC_DRAW );
	glBindBufferBase( GL_UNIFORM_BUFFER, 0, ubo_ );
	glGenBuffers( 1, &uboProj_ );
	glBindBuffer( GL_UNIFORM_BUFFER, uboProj_ );
	glBufferData( GL_UNIFORM_BUFFER, sizeof( ProjGpu ) * 8, nullptr, GL_DYNAMIC_DRAW );
	glBindBufferBase( GL_UNIFORM_BUFFER, 1, uboProj_ );
	{
		const uint8_t black[ 4 ] = { 0, 0, 0, 255 };
		glGenTextures( 1, &blackTex_ );
		glBindTexture( GL_TEXTURE_2D, blackTex_ );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	}
	//shadow array
	glGenTextures( 1, &shadowTex_ );
	glBindTexture( GL_TEXTURE_2D_ARRAY, shadowTex_ );
	glTexStorage3D( GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH_COMPONENT24, shadowRes, shadowRes, maxShadows_ );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL );
	glGenFramebuffers( 1, &shadowFbo_ );
	UploadGobos();
	GLenum e = glGetError();
	if( e != GL_NO_ERROR ) Log( "renderer init: GL error 0x%x", e );
	return true;
}

void Renderer::Shutdown()
{
	scene_.Destroy();
	beam_.Destroy();
	comb_.Destroy();
	out_.Destroy();
	for( auto& b : bloom_ ) b.Destroy();
	bloom_.clear();
}

void Renderer::UploadGobos()
{
	GoboBank& gb = Gobos();
	gb.Init();
	if( goboTex_ && goboVersion_ == gb.version ) return;
	if( goboTex_ ) glDeleteTextures( 1, &goboTex_ );
	goboLayers_ = (int)gb.layers.size();
	glGenTextures( 1, &goboTex_ );
	glBindTexture( GL_TEXTURE_2D_ARRAY, goboTex_ );
	int levels = 1;
	for( int s = kGoboSize; s > 1; s >>= 1 ) ++levels;
	glTexStorage3D( GL_TEXTURE_2D_ARRAY, levels, GL_R8, kGoboSize, kGoboSize, goboLayers_ );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	for( int i = 0; i < goboLayers_; ++i )
		glTexSubImage3D( GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, kGoboSize, kGoboSize, 1, GL_RED, GL_UNSIGNED_BYTE, gb.layers[ i ].data() );
	glGenerateMipmap( GL_TEXTURE_2D_ARRAY );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER );
	float border[ 4 ] = { 0, 0, 0, 0 };
	glTexParameterfv( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border );
	goboVersion_ = gb.version;
}

void Renderer::ForgetModel( const std::string& path )
{
	auto it = models_.find( path );
	if( it != models_.end() )
	{
		GpuMesh& g = it->second;
		if( !g.tex.empty() ) glDeleteTextures( (GLsizei)g.tex.size(), g.tex.data() );
		glDeleteBuffers( 1, &g.vbo );
		glDeleteBuffers( 1, &g.ibo );
		glDeleteVertexArrays( 1, &g.vao );
		models_.erase( it );
	}
	modelErrors_.erase( path );
}

bool Renderer::EnsureModel( ModelObj& m, std::string* err )
{
	auto it = models_.find( m.path );
	if( it != models_.end() ) { m.meshId = 1; m.textures = (int)it->second.tex.size(); m.missingTex = it->second.missing; return true; }
	auto e = modelErrors_.find( m.path );
	if( e != modelErrors_.end() )
	{
		if( err ) *err = e->second;
		return false;
	}
	MeshData md;
	std::string er;
	if( !LoadModel( m.path, md, &er ) )
	{
		modelErrors_[ m.path ] = er;
		Log( "model %s: %s", m.path.c_str(), er.c_str() );
		if( err ) *err = er;
		return false;
	}
	models_[ m.path ] = Upload( md );
	m.meshId          = 1;
	m.textures        = (int)md.images.size();
	m.missingTex      = md.missing;
	return true;
}

//================================================================ scene building
void Renderer::Box( const M4& m, const V3& col, float spec, const V3& emi, float flag )
{
	Inst i;
	i.m        = m;
	i.col[ 0 ] = col.x;
	i.col[ 1 ] = col.y;
	i.col[ 2 ] = col.z;
	i.col[ 3 ] = spec;
	i.emi[ 0 ] = emi.x;
	i.emi[ 1 ] = emi.y;
	i.emi[ 2 ] = emi.z;
	i.emi[ 3 ] = flag;
	boxes_.push_back( i );
}
void Renderer::Cyl( const M4& m, const V3& col, float spec, const V3& emi )
{
	Inst i;
	i.m        = m;
	i.col[ 0 ] = col.x;
	i.col[ 1 ] = col.y;
	i.col[ 2 ] = col.z;
	i.col[ 3 ] = spec;
	i.emi[ 0 ] = emi.x;
	i.emi[ 1 ] = emi.y;
	i.emi[ 2 ] = emi.z;
	i.emi[ 3 ] = 0;
	cyls_.push_back( i );
}

//cylinder between two points
static M4 Between( const V3& a, const V3& b, float r )
{
	V3 d   = b - a;
	float l = Len( d );
	V3 y   = l > 1e-6f ? d / l : V3( 0, 1, 0 );
	V3 x   = Norm( Cross( fabsf( y.y ) < 0.9f ? V3( 0, 1, 0 ) : V3( 1, 0, 0 ), y ) );
	V3 z   = Cross( x, y );
	M4 m;
	m( 0, 0 ) = x.x * 2 * r; m( 1, 0 ) = x.y * 2 * r; m( 2, 0 ) = x.z * 2 * r;
	m( 0, 1 ) = y.x * l; m( 1, 1 ) = y.y * l; m( 2, 1 ) = y.z * l;
	m( 0, 2 ) = z.x * 2 * r; m( 1, 2 ) = z.y * 2 * r; m( 2, 2 ) = z.z * 2 * r;
	V3 c      = ( a + b ) * 0.5f;
	m( 0, 3 ) = c.x; m( 1, 3 ) = c.y; m( 2, 3 ) = c.z;
	return m;
}
static M4 Bx( const M4& base, const V3& c, const V3& s ) { return Mul( base, Mul( Translate( c ), Scale( s ) ) ); }

void Renderer::AddFixture( const Show& show, const FixtureInst& f, bool sel )
{
	(void)show;
	M4 b = FixtureBase( f );
	V3 body( 0.035f, 0.035f, 0.04f ), metal( 0.12f, 0.12f, 0.13f );
	V3 selE = sel ? V3( 0.05f, 0.16f, 0.32f ) : V3( 0, 0, 0 );
	if( !f.def )
	{
		Box( Bx( b, V3( 0, 0.15f, 0 ), V3( 0.3f, 0.3f, 0.3f ) ), V3( 0.3f, 0.02f, 0.02f ), 0.1f, selE );
		return;
	}
	const FixtureDef& d   = *f.def;
	const FixtureState& s = f.st;
	V3 e                  = BodyExtents( d );
	auto lensE            = [ & ]( const V3& c ) { return c * 22.0f + V3( 0.004f, 0.004f, 0.005f ); };
	if( d.kind == Kind::MovingHead || d.kind == Kind::Scanner )
	{
		float pan  = ( f.invertPan ? -1.0f : 1.0f ) * ( s.pan + s.panSpin );
		float tilt = ( f.invertTilt ? -1.0f : 1.0f ) * ( s.tilt + s.tiltSpin );
		//base
		Box( Bx( b, V3( 0, e.y * 0.11f, 0 ), V3( e.x, e.y * 0.22f, e.z ) ), body, 0.35f, selE );
		M4 yoke = Mul( b, RotY( pan * kDeg ) );
		Box( Bx( yoke, V3( 0, e.y * 0.25f, 0 ), V3( e.x * 0.92f, e.y * 0.06f, e.z * 0.42f ) ), body, 0.35f, selE );
		for( int sgn = -1; sgn <= 1; sgn += 2 )
			Box( Bx( yoke, V3( sgn * e.x * 0.43f, e.y * 0.47f, 0 ), V3( e.x * 0.08f, e.y * 0.46f, e.z * 0.38f ) ), body, 0.35f, selE );
		M4 head = Mul( yoke, Mul( Translate( V3( 0, e.y * 0.62f, 0 ) ), RotX( tilt * kDeg ) ) );
		float hr = e.x * 0.33f;
		Cyl( Mul( head, Scale( V3( hr * 2, e.y * 0.56f, hr * 2 ) ) ), body, 0.45f, selE );
		Cyl( Mul( head, Mul( Translate( V3( 0, e.y * 0.285f, 0 ) ), Scale( V3( hr * 2.05f, e.y * 0.03f, hr * 2.05f ) ) ) ), metal, 0.6f, selE );
		V3 lc = s.color * s.intensity;
		Cyl( Mul( head, Mul( Translate( V3( 0, e.y * 0.3f, 0 ) ), Scale( V3( hr * 1.6f, 0.006f, hr * 1.6f ) ) ) ), V3( 0.02f, 0.02f, 0.02f ), 0.9f, lensE( lc ) );
		return;
	}
	if( d.kind == Kind::Par || d.kind == Kind::Dimmer || d.kind == Kind::Wash || d.kind == Kind::Generic || d.kind == Kind::Laser )
	{
		float r = std::min( e.x, e.z ) * 0.48f;
		Cyl( Bx( b, V3( 0, e.y * 0.5f, 0 ), V3( r * 2, e.y, r * 2 ) ), body, 0.4f, selE );
		Cyl( Bx( b, V3( 0, e.y * 0.98f, 0 ), V3( r * 2.06f, e.y * 0.05f, r * 2.06f ) ), metal, 0.5f, selE );
		//bracket
		Box( Bx( b, V3( 0, e.y * 0.35f, 0 ), V3( r * 2.3f, 0.025f, 0.05f ) ), metal, 0.3f, selE );
		for( int sgn = -1; sgn <= 1; sgn += 2 )
			Box( Bx( b, V3( sgn * r * 1.12f, e.y * 0.17f, 0 ), V3( 0.02f, e.y * 0.36f, 0.05f ) ), metal, 0.3f, selE );
		Box( Bx( b, V3( 0, -0.03f, 0 ), V3( r * 2.3f, 0.04f, 0.05f ) ), metal, 0.3f, selE );
		V3 lc = s.color * s.intensity;
		Cyl( Bx( b, V3( 0, e.y * 1.003f, 0 ), V3( r * 1.75f, 0.006f, r * 1.75f ) ), V3( 0.02f, 0.02f, 0.02f ), 0.9f, lensE( lc ) );
		return;
	}
	if( d.kind == Kind::Hazer )
	{
		Box( Bx( b, V3( 0, e.y * 0.5f, 0 ), V3( e.x, e.y, e.z ) ), V3( 0.05f, 0.05f, 0.06f ), 0.3f, selE + V3( 0, 0, s.fog * 0.05f ) );
		Cyl( Mul( b, Mul( Translate( V3( 0, e.y * 0.5f, e.z * 0.5f ) ), Mul( RotX( kPi / 2 ), Scale( V3( 0.06f, 0.04f, 0.06f ) ) ) ) ), metal, 0.3f, selE );
		return;
	}
	//box-shaped emitters: bar, matrix, strobe, blinder
	Box( Bx( b, V3( 0, e.y * 0.5f, 0 ), V3( e.x, e.y, e.z ) ), body, 0.35f, selE );
	if( d.kind == Kind::Strobe && d.pixels.empty() )
	{
		V3 lc = s.color * s.intensity;
		Box( Bx( b, V3( 0, e.y + 0.002f, 0 ), V3( e.x * 0.86f, 0.006f, e.z * 0.7f ) ), V3( 0.3f, 0.3f, 0.3f ), 0.9f, lensE( lc ) * 1.5f );
		return;
	}
	int nh = d.Heads();
	if( nh <= 1 )
	{
		V3 lc = s.color * s.intensity;
		float r = std::min( e.x, e.z ) * 0.42f;
		Cyl( Bx( b, V3( 0, e.y + 0.002f, 0 ), V3( r * 2, 0.006f, r * 2 ) ), V3( 0.02f, 0.02f, 0.02f ), 0.9f, lensE( lc ) );
		return;
	}
	float cw = e.x / std::max( 1, d.gx ), cd = e.z / std::max( 1, d.gy );
	float r  = std::min( cw, cd ) * 0.4f;
	for( int h = 0; h < nh && h < (int)s.heads.size(); ++h )
	{
		const V3& p = d.pixels[ h ].pos;
		V3 c( p.x * e.x * 0.92f, e.y + 0.002f, p.z * e.z * 0.92f );
		Cyl( Bx( b, c, V3( r * 2, 0.006f, r * 2 ) ), V3( 0.02f, 0.02f, 0.02f ), 0.9f, lensE( s.heads[ h ] ) * ( d.kind == Kind::Blinder ? 0.6f : 1.0f ) );
	}
}

static float Soft( const FixtureDef& d, const FixtureState& s, float& hot )
{
	bool hasGobo = false;
	for( auto& w : d.wheels )
		if( w.IsGobo() ) hasGobo = true;
	float soft;
	switch( d.kind )
	{
	case Kind::MovingHead:
	case Kind::Scanner:
		soft = hasGobo ? 0.05f : 0.32f;
		hot  = hasGobo ? 0.15f : 0.75f;
		break;
	case Kind::Dimmer: soft = 0.3f; hot = 0.9f; break;
	case Kind::Laser: soft = 0.02f; hot = 0.0f; break;
	default: soft = 0.45f; hot = 1.0f; break;
	}
	soft += s.frost * 0.7f + fabsf( s.focus - 0.5f ) * ( hasGobo ? 0.25f : 0.0f );
	return Clamp( soft, 0.01f, 0.98f );
}

void Renderer::BuildScene( Show& show, const std::vector< int >& sel, float time )
{
	boxes_.clear();
	cyls_.clear();
	helperCyls_.clear();
	lights_.clear();
	beams_.clear();
	glows_.clear();
	lightPower_.clear();
	const StageCfg& st = show.stage;
	//floor, deck, wall
	if( st.floor )
	{
		const float gs = Clamp( st.groundSize, 10.0f, 1000.0f );
		Box( Mul( Translate( V3( 0, -0.05f, 0 ) ), Scale( V3( gs, 0.1f, gs ) ) ), st.floorColor, 0.25f, V3( 0, 0, 0 ), 1.0f + (float)Clamp( st.ground, 0, 5 ) );
	}
	if( st.deck && st.deckHeight > 0.01f )
	{
		Box( Mul( Translate( V3( 0, st.deckHeight * 0.5f, 0 ) ), Scale( V3( st.width, st.deckHeight, st.depth ) ) ), st.floorColor * 1.1f, 0.35f );
		//front skirt
		Box( Mul( Translate( V3( 0, st.deckHeight * 0.5f, st.depth * 0.5f + 0.01f ) ), Scale( V3( st.width, st.deckHeight, 0.02f ) ) ), V3( 0.015f, 0.015f, 0.016f ), 0.05f );
	}
	//risers / extra decks: top, dark skirt, stairs, LED edge
	for( auto& d : st.decks )
	{
		const float h = std::max( 0.02f, d.height ), base = show.DeckBase( d );
		const float w = std::max( 0.1f, d.width ), dp = std::max( 0.1f, d.depth );
		const M4 frame = Mul( Translate( V3( d.x, base, d.z ) ), RotY( d.yaw * kDeg ) );
		auto local     = [ & ]( const V3& c, const V3& sz ) { return Mul( frame, Mul( Translate( c ), Scale( sz ) ) ); };
		const V3 top   = d.ownColor ? d.color : st.floorColor * 1.35f;
		const V3 skirt( 0.015f, 0.015f, 0.016f );
		const bool round = d.shape == 1;
		auto solid       = [ & ]( const M4& m, const V3& c, float spec, const V3& emi = V3( 0, 0, 0 ) ) {
			if( round ) Cyl( m, c, spec, emi );
			else Box( m, c, spec, emi );
		};
		if( d.skirt && h > 0.06f )
		{
			solid( local( V3( 0, h * 0.5f, 0 ), V3( w, h, dp ) ), top, 0.35f );
			solid( local( V3( 0, ( h - 0.025f ) * 0.5f, 0 ), V3( w + 0.02f, h - 0.025f, dp + 0.02f ) ), skirt, 0.05f );
		}
		else
			solid( local( V3( 0, h * 0.5f, 0 ), V3( w, h, dp ) ), top, 0.35f );
		if( d.edgeLevel > 0.001f && ( d.edge.x + d.edge.y + d.edge.z ) > 0.001f )
		{
			const V3 emi = d.edge * d.edgeLevel;
			if( round ) Cyl( local( V3( 0, h - 0.035f, 0 ), V3( w + 0.035f, 0.03f, dp + 0.035f ) ), d.edge, 0.1f, emi );
			else Box( local( V3( 0, h - 0.035f, dp * 0.5f + 0.016f ), V3( w * 0.98f, 0.03f, 0.012f ) ), d.edge, 0.1f, emi );
		}
		if( d.stairs >= 1 && d.stairs <= 4 && h > 0.15f )
		{
			const int n      = std::max( 1, (int)ceilf( h / 0.19f ) - 1 );
			const float run  = 0.28f;
			const float side = d.stairs <= 2 ? w : dp;
			const float sw   = Clamp( side * 0.4f, 0.8f, 1.6f );
			for( int j = 1; j <= n; ++j )
			{
				const float hj   = h * j / ( n + 1 );
				const float dist = ( n - j + 0.5f ) * run;
				V3 c, sz;
				switch( d.stairs )
				{
				case 1: c = V3( 0, hj * 0.5f, dp * 0.5f + dist ); sz = V3( sw, hj, run ); break;
				case 2: c = V3( 0, hj * 0.5f, -dp * 0.5f - dist ); sz = V3( sw, hj, run ); break;
				case 3: c = V3( -w * 0.5f - dist, hj * 0.5f, 0 ); sz = V3( run, hj, sw ); break;
				default: c = V3( w * 0.5f + dist, hj * 0.5f, 0 ); sz = V3( run, hj, sw ); break;
				}
				Box( local( c, sz ), j % 2 ? top * 0.9f : top, 0.3f );
			}
		}
	}
	if( st.backWall )
		Box( Mul( Translate( V3( 0, st.wallHeight * 0.5f, -st.depth * 0.5f - 0.6f ) ), Scale( V3( st.width + 10, st.wallHeight, 0.2f ) ) ), st.wallColor, 0.05f );
	//trusses: box truss, 4 chords + diagonals
	V3 alu( 0.55f, 0.56f, 0.58f );
	for( auto& t : show.trusses )
	{
		V3 d   = t.b - t.a;
		float L = Len( d );
		if( L < 0.05f ) continue;
		V3 ax  = d / L;
		V3 s1  = Norm( Cross( ax, fabsf( ax.y ) < 0.9f ? V3( 0, 1, 0 ) : V3( 1, 0, 0 ) ) );
		V3 s2  = Cross( s1, ax );
		float h = t.size * 0.5f;
		V3 c[ 4 ] = { s1 * h + s2 * h, s1 * -h + s2 * h, s1 * -h + s2 * -h, s1 * h + s2 * -h };
		for( int i = 0; i < 4; ++i ) Cyl( Between( t.a + c[ i ], t.b + c[ i ], 0.024f ), alu, 0.7f );
		int segs = std::max( 1, (int)( L / t.size ) );
		for( int k = 0; k < segs; ++k )
		{
			float u0 = (float)k / segs * L, u1 = (float)( k + 1 ) / segs * L;
			for( int i = 0; i < 4; ++i )
			{
				V3 p0 = t.a + ax * u0 + c[ i ], p1 = t.a + ax * u1 + c[ ( i + 1 ) % 4 ];
				if( k & 1 ) { p0 = t.a + ax * u0 + c[ ( i + 1 ) % 4 ]; p1 = t.a + ax * u1 + c[ i ]; }
				Cyl( Between( p0, p1, 0.009f ), alu, 0.6f );
			}
		}
	}
	//tripod stands: three legs with braces, telescopic pole, optional T-bar
	for( auto& sd : show.stands )
	{
		const V3 blk( 0.035f, 0.035f, 0.04f ), tube( 0.09f, 0.09f, 0.095f );
		const float H = std::max( 0.6f, sd.height );
		const V3 base = sd.pos;
		const float cy = std::min( 0.9f, H * 0.42f );
		const V3 collar = base + V3( 0, cy, 0 );
		const float spread = 0.5f + 0.18f * std::min( 1.0f, H / 3.0f );
		for( int k = 0; k < 3; ++k )
		{
			float a  = ( sd.yaw + 90.0f + k * 120.0f ) * kDeg;
			V3 foot  = base + V3( cosf( a ) * spread, 0.015f, -sinf( a ) * spread );
			Cyl( Between( foot, collar, 0.015f ), blk, 0.35f );
			Cyl( Between( base + V3( 0, cy * 0.3f, 0 ), ( foot + collar ) * 0.5f, 0.007f ), blk, 0.35f );
			Box( Mul( Translate( foot ), Scale( V3( 0.05f, 0.03f, 0.05f ) ) ), blk, 0.2f );
		}
		const V3 top   = base + V3( 0, H, 0 );
		const V3 split = collar + ( top - collar ) * 0.45f;
		Cyl( Between( base + V3( 0, cy * 0.3f, 0 ), split, 0.022f ), blk, 0.4f );
		Cyl( Between( split, top, 0.016f ), tube, 0.6f );
		Cyl( Between( split - V3( 0, 0.03f, 0 ), split + V3( 0, 0.05f, 0 ), 0.03f ), blk, 0.3f );
		Cyl( Between( collar - V3( 0, 0.04f, 0 ), collar + V3( 0, 0.04f, 0 ), 0.032f ), blk, 0.3f );
		if( sd.bar > 0.05f )
		{
			V3 X( cosf( sd.yaw * kDeg ), 0, -sinf( sd.yaw * kDeg ) );
			V3 c = top - V3( 0, 0.02f, 0 );
			Cyl( Between( c - X * ( sd.bar * 0.5f + 0.05f ), c + X * ( sd.bar * 0.5f + 0.05f ), 0.018f ), tube, 0.6f );
		}
	}
	BuildScreens( show );
	//fixtures
	for( auto& f : show.fixtures )
		AddFixture( show, f, std::find( sel.begin(), sel.end(), f.id ) != sel.end() );

	//lights
	struct Cand { LightGpu l; float power; int fixture; std::vector< std::pair< V3, V3 > > beamHeads; bool glow; V3 glowDir; float lensR; };
	std::vector< Cand > cands;
	const float kL = 1.0e-3f;
	ambient_       = st.ambientColor * ( st.ambient * st.envLight );
	const float boost = std::max( 1.0f, show.master );//master above 100 %: more light out of every fixture
	V3 scatter( 0, 0, 0 );
	for( auto& f : show.fixtures )
	{
		if( !f.def || f.def->kind == Kind::Hazer ) continue;
		const FixtureDef& d   = *f.def;
		const FixtureState& s = f.st;
		if( s.intensity < 1e-4f ) continue;
		float hot;
		float soft  = Soft( d, s, hot );
		float zoom  = Clamp( s.zoom, 0.5f, 170.0f );
		float tanB  = tanf( zoom * 0.5f * kDeg ) * Lerp( 0.12f, 1.0f, s.iris );
		tanB        = std::max( tanB, 0.004f );
		V3 o, dir, up;
		int nh      = d.Heads();
		bool pixelBeams = nh > 1 && d.kind != Kind::MovingHead && d.kind != Kind::Scanner;
		auto mk = [ & ]( const V3& pos, const V3& col, float intensity ) {
			Cand c;
			memset( &c.l, 0, sizeof( c.l ) );
			c.l.posRange[ 0 ] = pos.x; c.l.posRange[ 1 ] = pos.y; c.l.posRange[ 2 ] = pos.z; c.l.posRange[ 3 ] = 60;
			c.l.dirTan[ 0 ] = dir.x; c.l.dirTan[ 1 ] = dir.y; c.l.dirTan[ 2 ] = dir.z; c.l.dirTan[ 3 ] = tanB;
			c.l.upSoft[ 0 ] = up.x; c.l.upSoft[ 1 ] = up.y; c.l.upSoft[ 2 ] = up.z; c.l.upSoft[ 3 ] = soft;
			float scale = EffLumens( d.lumens ) * intensity * kL * boost / ( kPi * tanB * tanB + 1e-4f );
			c.l.color[ 0 ] = col.x * scale; c.l.color[ 1 ] = col.y * scale; c.l.color[ 2 ] = col.z * scale; c.l.color[ 3 ] = -1;
			float blur = fabsf( s.focus - 0.5f ) * 2.0f * 3.5f + s.frost * 6.0f;
			c.l.gobo[ 0 ] = (float)s.gobo; c.l.gobo[ 1 ] = s.goboRot; c.l.gobo[ 2 ] = blur;
			c.l.prism[ 0 ] = (float)s.prism; c.l.prism[ 1 ] = s.prismRot; c.l.prism[ 2 ] = s.prism ? std::max( tanB * 1.7f, 0.06f ) : 0.0f; c.l.prism[ 3 ] = hot;
			c.power   = Lum3( col ) * d.lumens * intensity;
			c.fixture = f.id;
			c.glow    = true;
			c.glowDir = dir;
			c.lensR   = std::min( d.dims.x, d.dims.z ) * 0.35f;
			return c;
		};
		if( !pixelBeams )
		{
			BeamRay( f, -1, o, dir, up );
			Cand c = mk( o + dir * 0.02f, s.color, s.intensity );
			c.beamHeads.push_back( { o + dir * 0.02f, s.color * ( s.intensity * EffLumens( d.lumens ) * kL * boost / ( kPi * tanB * tanB + 1e-4f ) ) } );
			cands.push_back( c );
		}
		else if( nh <= 4 )
		{
			for( int h = 0; h < nh; ++h )
			{
				V3 hc = s.heads[ h ];
				float hi = std::max( { hc.x, hc.y, hc.z } );
				if( hi < 1e-4f ) continue;
				BeamRay( f, h, o, dir, up );
				Cand c = mk( o + dir * 0.02f, hc / hi, hi / nh );
				c.beamHeads.push_back( { o + dir * 0.02f, V3( c.l.color[ 0 ], c.l.color[ 1 ], c.l.color[ 2 ] ) } );
				c.lensR = std::min( d.dims.x / std::max( 1, d.gx ), d.dims.y ) * 0.4f;
				cands.push_back( c );
			}
		}
		else
		{
			//aggregate surface light at the centre, beams per pixel
			BeamRay( f, -1, o, dir, up );
			Cand c = mk( o + dir * 0.02f, s.color, s.intensity );
			c.glow = false;
			for( int h = 0; h < nh && h < 64; ++h )
			{
				V3 hc = s.heads[ h ];
				if( std::max( { hc.x, hc.y, hc.z } ) < 1e-4f ) continue;
				V3 ho, hd, hu;
				BeamRay( f, h, ho, hd, hu );
				c.beamHeads.push_back( { ho + hd * 0.02f, hc * ( EffLumens( d.lumens ) / nh * kL * boost / ( kPi * tanB * tanB + 1e-4f ) ) } );
			}
			cands.push_back( c );
		}
	}
	//strongest lights first
	std::sort( cands.begin(), cands.end(), []( const Cand& a, const Cand& b ) { return a.power > b.power; } );
	if( cands.size() > 96 ) cands.resize( 96 );
	int shadowCount = 0;
	for( auto& c : cands )
	{
		int li = (int)lights_.size();
		V3 pos( c.l.posRange[ 0 ], c.l.posRange[ 1 ], c.l.posRange[ 2 ] );
		V3 dir( c.l.dirTan[ 0 ], c.l.dirTan[ 1 ], c.l.dirTan[ 2 ] );
		V3 up( c.l.upSoft[ 0 ], c.l.upSoft[ 1 ], c.l.upSoft[ 2 ] );
		float tanOuter = c.l.dirTan[ 3 ] * ( 1.0f + c.l.upSoft[ 3 ] * 0.2f ) + c.l.prism[ 2 ] + 0.002f;
		if( st.shadows && quality.shadows && shadowCount < maxShadows_ )
		{
			float fov = 2.0f * atanf( tanOuter * 1.08f );
			fov       = Clamp( fov, 2.0f * kDeg, 150.0f * kDeg );
			M4 vp     = Mul( Perspective( fov, 1.0f, 0.05f, 60.0f ), LookAt( pos, pos + dir, up ) );
			memcpy( c.l.shadow, vp.m, 64 );
			c.l.color[ 3 ] = (float)shadowCount++;
		}
		lights_.push_back( c.l );
		lightPower_.push_back( c.power );
		scatter += V3( c.l.color[ 0 ], c.l.color[ 1 ], c.l.color[ 2 ] ) * ( c.l.dirTan[ 3 ] * c.l.dirTan[ 3 ] );
		for( auto& bh : c.beamHeads )
			beams_.push_back( Beam{ li, bh.first, bh.second, tanOuter, 45.0f } );
		if( lensGlow && quality.glow )
		{
			if( c.glow )
				glows_.push_back( Glow{ pos, dir, V3( c.l.color[ 0 ], c.l.color[ 1 ], c.l.color[ 2 ] ) * ( kPi * c.l.dirTan[ 3 ] * c.l.dirTan[ 3 ] ), c.lensR, 1.0f / sqrtf( 1.0f + tanOuter * tanOuter * 2.5f ) } );
			else
				for( auto& bh : c.beamHeads )
					glows_.push_back( Glow{ bh.first, dir, bh.second * ( kPi * c.l.dirTan[ 3 ] * c.l.dirTan[ 3 ] ), c.lensR * 0.35f, 1.0f / sqrtf( 1.0f + tanOuter * tanOuter * 2.5f ) } );
		}
	}
	BuildProjectors( show, shadowCount, scatter );
	//moon: directional light with an optional orthographic shadow map over the stage area
	moonLayer_ = -1;
	if( st.environment == 1 && st.moon > 0.001f && st.moonShadow && st.shadows && quality.shadows && shadowCount < maxShadows_ )
	{
		V3 md     = MoonDir( st );
		V3 c( 0, 0, 0 );
		V3 up     = fabsf( md.y ) > 0.95f ? V3( 0, 0, -1 ) : V3( 0, 1, 0 );
		moonVP_   = Mul( Ortho( -32, 32, -32, 32, 1, 180 ), LookAt( c + md * 90.0f, c, up ) );
		moonLayer_ = shadowCount++;
	}
	//light scattered back from haze and surfaces lifts the ambient a little
	ambient_ += scatter * ( 0.0006f + 0.0025f * st.haze );
	stats.lights  = (int)lights_.size();
	stats.shadows = shadowCount;
	stats.beams   = (int)beams_.size();
	time_         = time;
}

//---------------------------------------------------------------- screens
void Renderer::BuildScreens( const Show& show )
{
	const V3 black( 0.012f, 0.012f, 0.013f ), metal( 0.08f, 0.08f, 0.085f );
	for( size_t si = 0; si < show.screens.size(); ++si )
	{
		const Screen& s = show.screens[ si ];
		ScreenGeo g     = GetScreenGeo( s );
		V3 selE         = (int)si == selScreen ? V3( 0.01f, 0.03f, 0.06f ) : V3( 0, 0, 0 );
		M4 Rm;
		Rm( 0, 0 ) = g.right.x; Rm( 1, 0 ) = g.right.y; Rm( 2, 0 ) = g.right.z;
		Rm( 0, 1 ) = g.up.x; Rm( 1, 1 ) = g.up.y; Rm( 2, 1 ) = g.up.z;
		Rm( 0, 2 ) = g.normal.x; Rm( 1, 2 ) = g.normal.y; Rm( 2, 2 ) = g.normal.z;
		Rm( 0, 3 ) = g.centre.x; Rm( 1, 3 ) = g.centre.y; Rm( 2, 3 ) = g.centre.z;
		const float W = std::max( 0.1f, s.width ), H = std::max( 0.1f, s.height );
		auto sb = [ & ]( const V3& c, const V3& sz, const V3& col, float spec, const V3& emi ) { Box( Mul( Rm, Mul( Translate( c ), Scale( sz ) ) ), col, spec, emi ); };
		const V3 cloth = V3( 0.8f, 0.8f, 0.8f ) * Clamp( s.gain, 0.1f, 2.5f );
		const bool shaped = s.HasMask();
		if( shaped )
		{
			//shape from the image: one thin box per merged white rectangle
			for( auto& r : s.maskRects )
			{
				const float cx = ( ( r.x0 + r.x1 ) * 0.5f - 0.5f ) * W, cy = ( 0.5f - ( r.y0 + r.y1 ) * 0.5f ) * H;
				sb( V3( cx, cy, -0.012f ), V3( ( r.x1 - r.x0 ) * W, ( r.y1 - r.y0 ) * H, 0.02f ), cloth, 0.02f, selE );
			}
		}
		else
			sb( V3( 0, 0, -0.012f ), V3( W, H, 0.02f ), cloth, 0.02f, selE );
		if( s.frame && !shaped )
		{
			const float fw = 0.08f;
			sb( V3( 0, H * 0.5f + fw * 0.5f, -0.01f ), V3( W + 2 * fw, fw, 0.035f ), black, 0.15f, selE );
			sb( V3( 0, -H * 0.5f - fw * 0.5f, -0.01f ), V3( W + 2 * fw, fw, 0.035f ), black, 0.15f, selE );
			sb( V3( W * 0.5f + fw * 0.5f, 0, -0.01f ), V3( fw, H, 0.035f ), black, 0.15f, selE );
			sb( V3( -W * 0.5f - fw * 0.5f, 0, -0.01f ), V3( fw, H, 0.035f ), black, 0.15f, selE );
		}
		//legs / cables at the outer edges of the (shaped) cloth
		const float bx0 = shaped ? ( s.maskBox[ 0 ] - 0.5f ) * W : -W * 0.5f, bx1 = shaped ? ( s.maskBox[ 2 ] - 0.5f ) * W : W * 0.5f;
		const float byT = shaped ? ( 0.5f - s.maskBox[ 1 ] ) * H : H * 0.5f, byB = shaped ? ( 0.5f - s.maskBox[ 3 ] ) * H : -H * 0.5f;
		if( bx1 - bx0 < 0.01f ) continue;
		const float inset = std::min( 0.3f, ( bx1 - bx0 ) * 0.1f );
		for( int sgn = -1; sgn <= 1; sgn += 2 )
		{
			const float lx = sgn < 0 ? bx0 + inset : bx1 - inset;
			V3 top = XformP( Rm, V3( lx, byT, -0.06f ) );
			V3 bot = XformP( Rm, V3( lx, byB, -0.06f ) );
			if( s.mount == 0 )
			{
				//fast-fold legs: from the floor up to the top edge, T-feet
				float fy = show.FloorAt( bot.x, bot.z );
				V3 foot( bot.x, fy, bot.z );
				if( foot.y < top.y ) Cyl( Between( foot, top, 0.018f ), metal, 0.4f );
				V3 n = Norm( V3( g.normal.x, 0, g.normal.z ) );
				Cyl( Between( foot - n * 0.55f + V3( 0, 0.02f, 0 ), foot + n * 0.45f + V3( 0, 0.02f, 0 ), 0.016f ), metal, 0.4f );
			}
			else if( s.mount == 1 )
				Cyl( Between( top, top + V3( 0, std::max( 1.0f, show.stage.wallHeight + 1.0f - top.y ), 0 ), 0.004f ), V3( 0.25f, 0.25f, 0.26f ), 0.5f );
		}
	}
}

//---------------------------------------------------------------- projectors
void Renderer::UpdateProjectorTextures( const Show& show )
{
	std::map< uint32_t, ProjTexture > keep;
	for( auto& p : show.projectors )
	{
		auto it = projTex_.find( p.uid );
		ProjTexture t;
		if( it != projTex_.end() )
		{
			t = it->second;
			projTex_.erase( it );
		}
		const VideoFrame* fr = video ? video->Frame( p.uid ) : nullptr;
		if( fr && fr->serial != t.serial )
		{
			if( !t.tex || t.w != fr->w || t.h != fr->h )
			{
				if( t.tex ) glDeleteTextures( 1, &t.tex );
				glGenTextures( 1, &t.tex );
				glBindTexture( GL_TEXTURE_2D, t.tex );
				int levels = 1;
				for( int s = std::max( fr->w, fr->h ); s > 1; s >>= 1 ) ++levels;
				glTexStorage2D( GL_TEXTURE_2D, levels, GL_SRGB8_ALPHA8, fr->w, fr->h );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
				t.w = fr->w;
				t.h = fr->h;
			}
			glBindTexture( GL_TEXTURE_2D, t.tex );
			glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
			glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, fr->w, fr->h, fr->bgra ? GL_BGRA : GL_RGBA, GL_UNSIGNED_BYTE, fr->px.data() );
			glGenerateMipmap( GL_TEXTURE_2D );
			t.serial = fr->serial;
		}
		keep[ p.uid ] = t;
	}
	for( auto& kv : projTex_ )
		if( kv.second.tex ) glDeleteTextures( 1, &kv.second.tex );
	projTex_.swap( keep );
}

void Renderer::BuildProjectors( const Show& show, int& shadowCount, V3& scatter )
{
	projs_.clear();
	projBind_.clear();
	const float kL = 1.0e-3f;
	for( size_t pi = 0; pi < show.projectors.size(); ++pi )
	{
		const Projector p    = show.ProjectorOut( (int)pi );//with the running snapshots
		const VideoFrame* fr = video ? video->Frame( p.uid ) : nullptr;
		ProjectorGeo g       = GetProjectorGeo( p, fr ? fr->w : 0, fr ? fr->h : 0 );
		const bool selP      = (int)pi == selProjector;
		V3 selE              = selP ? V3( 0.05f, 0.16f, 0.32f ) : V3( 0, 0, 0 );
		//body: housing behind the lens, feet, lens barrel glowing with the picture
		M4 B          = Mul( Translate( p.pos ), RotEuler( V3( p.tilt, p.pan, p.roll ) ) );
		const V3 dark( 0.05f, 0.05f, 0.055f );
		Box( Mul( B, Mul( Translate( V3( 0, 0, 0.12f ) ), Scale( V3( 0.5f, 0.18f, 0.42f ) ) ) ), dark, 0.35f, selE );
		Box( Mul( B, Mul( Translate( V3( 0, 0.0f, 0.335f ) ), Scale( V3( 0.46f, 0.15f, 0.012f ) ) ) ), V3( 0.02f, 0.02f, 0.02f ), 0.1f, selE );
		for( int sx = -1; sx <= 1; sx += 2 )
			for( int sz = 0; sz < 2; ++sz ) Box( Mul( B, Mul( Translate( V3( sx * 0.19f, -0.1f, sz ? 0.29f : -0.05f ) ), Scale( V3( 0.04f, 0.025f, 0.04f ) ) ) ), V3( 0.02f, 0.02f, 0.02f ), 0.1f );
		const float area = std::max( 1e-4f, ( g.r - g.l ) * ( g.t - g.b ) );
		const float inten = EffLumens( p.lumens ) * kL / area;
		V3 avg            = fr && p.on ? fr->avg : V3( 0, 0, 0 );
		Cyl( Between( XformP( B, V3( 0, 0, -0.09f ) ), XformP( B, V3( 0, 0, -0.15f ) ), 0.055f ), dark, 0.5f, selE );
		Cyl( Between( XformP( B, V3( 0, 0, -0.15f ) ), XformP( B, V3( 0, 0, -0.152f ) ), 0.045f ), V3( 0.02f, 0.02f, 0.02f ), 0.9f, avg * ( p.lumens * 0.0004f ) + V3( 0.002f, 0.002f, 0.003f ) );
		if( !p.on ) continue;
		const V3 lens = XformP( B, V3( 0, 0, -0.155f ) );
		g.o           = lens;
		//outline of the light cone up to where the picture lands
		if( projLines && p.frustum )
		{
			float dist          = show.ThrowDistance( g );
			const float cx[ 4 ] = { g.l, g.r, g.r, g.l }, cy[ 4 ] = { g.b, g.b, g.t, g.t };
			V3 e[ 4 ];
			for( int k = 0; k < 4; ++k ) e[ k ] = g.o + ( g.right * cx[ k ] + g.up * cy[ k ] + g.dir ) * dist;
			V3 lc = selP ? V3( 0.05f, 0.25f, 0.4f ) : V3( 0.01f, 0.05f, 0.08f );
			size_t c0 = cyls_.size();
			for( int k = 0; k < 4; ++k )
			{
				Cyl( Between( g.o, e[ k ], 0.004f ), V3( 0, 0, 0 ), 0.0f, lc );
				Cyl( Between( e[ k ] + g.dir * -0.01f, e[ ( k + 1 ) % 4 ] + g.dir * -0.01f, 0.006f ), V3( 0, 0, 0 ), 0.0f, lc );
			}
			helperCyls_.insert( helperCyls_.end(), cyls_.begin() + c0, cyls_.end() );
			cyls_.resize( c0 );
		}
		if( projs_.size() >= 8 ) continue;
		ProjGpu q;
		memset( &q, 0, sizeof( q ) );
		const float n = 0.2f, f = 60.0f;
		M4 vp         = Mul( Frustum( g.l * n, g.r * n, g.b * n, g.t * n, n, f ), LookAt( g.o, g.o + g.dir, g.up ) );
		memcpy( q.vp, vp.m, 64 );
		q.posInt[ 0 ]    = g.o.x;
		q.posInt[ 1 ]    = g.o.y;
		q.posInt[ 2 ]    = g.o.z;
		q.posInt[ 3 ]    = inten;
		q.dirShadow[ 0 ] = g.dir.x;
		q.dirShadow[ 1 ] = g.dir.y;
		q.dirShadow[ 2 ] = g.dir.z;
		q.dirShadow[ 3 ] = -1;
		if( show.stage.shadows && quality.shadows && shadowCount < maxShadows_ ) q.dirShadow[ 3 ] = (float)shadowCount++;
		q.misc[ 0 ]  = p.rear ? 1.0f : 0.0f;
		q.misc[ 1 ]  = Clamp( p.blackLevel, 0.0f, 0.2f );
		q.misc[ 2 ]  = 0.002f + Clamp( p.blur, 0.0f, 1.0f ) * 0.04f;
		q.misc[ 3 ]  = Clamp( p.blur, 0.0f, 1.0f ) * 5.0f;
		q.extra[ 0 ] = g.r - g.l;
		q.extra[ 1 ] = fr ? (float)fr->w : 1.0f;
		//letterbox when the input aspect differs from the projector format
		float srcA   = fr && fr->h > 0 ? (float)fr->w / fr->h : g.aspect;
		q.extra[ 2 ] = srcA < g.aspect ? srcA / g.aspect : 1.0f;
		q.extra[ 3 ] = srcA > g.aspect ? g.aspect / srcA : 1.0f;
		auto tit     = projTex_.find( p.uid );
		projBind_.push_back( tit != projTex_.end() && tit->second.tex ? tit->second.tex : blackTex_ );
		projs_.push_back( q );
		scatter += avg * ( EffLumens( p.lumens ) * kL / kPi * 0.5f );
		if( lensGlow && quality.glow && fr )
		{
			float mt = std::max( { fabsf( g.l ), fabsf( g.r ), fabsf( g.b ), fabsf( g.t ) } );
			glows_.push_back( Glow{ g.o, g.dir, avg * ( EffLumens( p.lumens ) * kL ), 0.045f, 1.0f / sqrtf( 1.0f + mt * mt * 2.5f ) } );
		}
	}
}

void Renderer::DrawInstances( GLuint prog, bool shadow )
{
	(void)shadow;
	GLint loc = glGetUniformLocation( prog, "uInstanced" );
	glUniform1i( loc, 1 );
	auto draw = [ & ]( const GpuMesh& m, const std::vector< Inst >& inst ) {
		if( inst.empty() ) return;
		glBindBuffer( GL_ARRAY_BUFFER, instVbo_ );
		glBufferData( GL_ARRAY_BUFFER, inst.size() * sizeof( Inst ), inst.data(), GL_STREAM_DRAW );
		glBindVertexArray( m.vao );
		glDrawElementsInstanced( GL_TRIANGLES, m.count, GL_UNSIGNED_INT, nullptr, (GLsizei)inst.size() );
	};
	draw( box_, boxes_ );
	draw( cyl_, cyls_ );
	if( !shadow ) draw( cyl_, helperCyls_ );
	glUniform1i( loc, 0 );
}

//================================================================ frame
void Renderer::Render( Show& show, const SimGlobals& g, const Camera& cam, const std::vector< int >& sel, int w, int h )
{
	double t0 = Now();
	++renderCount;
	w         = std::max( 16, w );
	h         = std::max( 16, h );
	UploadGobos();
	scene_.Create( w, h, GL_RGBA16F, true, true );
	beam_.Create( std::max( 8, w / 2 ), std::max( 8, h / 2 ), GL_RGBA16F, false, false );
	comb_.Create( w, h, GL_RGBA16F, false, false );
	out_.Create( w, h, GL_RGBA8, false, false );
	if( bloom_.empty() || bloom_[ 0 ].w != std::max( 8, w / 2 ) || bloom_[ 0 ].h != std::max( 8, h / 2 ) )
	{
		for( auto& b : bloom_ ) b.Destroy();
		bloom_.clear();
		int bw = w / 2, bh = h / 2;
		for( int i = 0; i < 6 && bw >= 8 && bh >= 8; ++i )
		{
			Fbo f;
			f.Create( bw, bh, GL_R11F_G11F_B10F, false, false );
			bloom_.push_back( f );
			bw /= 2;
			bh /= 2;
		}
	}
	UpdateProjectorTextures( show );
	BuildScene( show, sel, (float)g.time );
	stats.instances = (int)( boxes_.size() + cyls_.size() );

	//models (non-instanced)
	struct ModelDraw { const GpuMesh* m; M4 x; V3 tint; bool mat; };
	std::vector< ModelDraw > mdraws;
	for( auto& m : show.models )
	{
		auto it = models_.find( m.path );
		if( it == models_.end() ) continue;
		M4 x = Mul( Translate( m.pos ), Mul( RotEuler( m.rot ), Scale( V3( m.scale, m.scale, m.scale ) ) ) );
		mdraws.push_back( { &it->second, x, m.tint, m.matColors } );
	}
	auto drawModels = [ & ]( GLuint prog ) {
		GLint lm = glGetUniformLocation( prog, "uModel" ), lc = glGetUniformLocation( prog, "uCol" ), le = glGetUniformLocation( prog, "uEmi" );
		GLint lt = glGetUniformLocation( prog, "uUseTex" ), lv = glGetUniformLocation( prog, "uIgnoreVCol" ), lcut = glGetUniformLocation( prog, "uCutoff" );
		for( auto& md : mdraws )
		{
			glUniformMatrix4fv( lm, 1, GL_FALSE, md.x.m );
			//materials from the file: colour = vertex/material colour (x texture); otherwise one tint
			V3 c = md.mat ? V3( 1, 1, 1 ) : md.tint;
			if( lc >= 0 ) glUniform4f( lc, c.x, c.y, c.z, 0.25f );
			if( le >= 0 ) glUniform4f( le, 0, 0, 0, 0 );
			if( lv >= 0 ) glUniform1i( lv, md.mat ? 0 : 1 );
			glBindVertexArray( md.m->vao );
			for( int i = 3; i <= 8; ++i ) glDisableVertexAttribArray( i );
			if( lt < 0 )
			{
				glDrawElements( GL_TRIANGLES, md.m->count, GL_UNSIGNED_INT, nullptr );
				continue;
			}
			for( auto& part : md.m->parts )
			{
				const bool has   = part.tex >= 0 && part.tex < (int)md.m->tex.size();
				const bool alpha = has && part.cutoff > 0;
				const int mode   = has && md.mat ? 1 : ( alpha ? 2 : 0 );//single tint: keep the cut-out shape
				glUniform1i( lt, mode );
				if( lcut >= 0 ) glUniform1f( lcut, alpha ? part.cutoff : 0.0f );
				if( mode )
				{
					glActiveTexture( GL_TEXTURE3 );
					glBindTexture( GL_TEXTURE_2D, md.m->tex[ part.tex ] );
				}
				glDrawElements( GL_TRIANGLES, part.count, GL_UNSIGNED_INT, (void*)( (size_t)part.first * 4 ) );
			}
			glUniform1i( lt, 0 );
			if( lcut >= 0 ) glUniform1f( lcut, 0.0f );
			if( lv >= 0 ) glUniform1i( lv, 0 );
		}
	};

	//lights UBO
	glBindBuffer( GL_UNIFORM_BUFFER, ubo_ );
	if( !lights_.empty() ) glBufferSubData( GL_UNIFORM_BUFFER, 0, lights_.size() * sizeof( LightGpu ), lights_.data() );
	glBindBufferBase( GL_UNIFORM_BUFFER, 0, ubo_ );
	glBindBuffer( GL_UNIFORM_BUFFER, uboProj_ );
	if( !projs_.empty() ) glBufferSubData( GL_UNIFORM_BUFFER, 0, projs_.size() * sizeof( ProjGpu ), projs_.data() );
	glBindBufferBase( GL_UNIFORM_BUFFER, 1, uboProj_ );
	//projector pictures on units 4..11
	const GLint projUnits[ 8 ] = { 4, 5, 6, 7, 8, 9, 10, 11 };
	auto bindProjTex = [ & ]( GLuint prog ) {
		for( int i = 0; i < 8; ++i )
		{
			glActiveTexture( GL_TEXTURE4 + i );
			glBindTexture( GL_TEXTURE_2D, i < (int)projBind_.size() ? projBind_[ i ] : blackTex_ );
		}
		glUniform1iv( glGetUniformLocation( prog, "uProjTex" ), 8, projUnits );
		glUniform1i( glGetUniformLocation( prog, "uNumProj" ), (int)projs_.size() );
		glActiveTexture( GL_TEXTURE0 );
	};

	glEnable( GL_DEPTH_TEST );
	glDepthMask( GL_TRUE );
	glDisable( GL_BLEND );
	glEnable( GL_CULL_FACE );
	glCullFace( GL_BACK );
	glFrontFace( GL_CCW );

	//---- shadows
	if( stats.shadows > 0 )
	{
		glUseProgram( progShadow_ );
		glBindFramebuffer( GL_FRAMEBUFFER, shadowFbo_ );
		glViewport( 0, 0, shadowRes, shadowRes );
		glDisable( GL_CULL_FACE );
		glEnable( GL_POLYGON_OFFSET_FILL );
		glPolygonOffset( 1.5f, 2.0f );
		GLint lvp = glGetUniformLocation( progShadow_, "uVP" );
		glUniform1i( glGetUniformLocation( progShadow_, "uAlbedo" ), 3 );
		glUniform1i( glGetUniformLocation( progShadow_, "uUseTex" ), 0 );
		glUniform1f( glGetUniformLocation( progShadow_, "uCutoff" ), 0.0f );
		for( auto& l : lights_ )
		{
			if( l.color[ 3 ] < 0 ) continue;
			glFramebufferTextureLayer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, shadowTex_, 0, (GLint)l.color[ 3 ] );
			glDrawBuffer( GL_NONE );
			glClear( GL_DEPTH_BUFFER_BIT );
			glUniformMatrix4fv( lvp, 1, GL_FALSE, l.shadow );
			DrawInstances( progShadow_, true );
			drawModels( progShadow_ );
		}
		if( moonLayer_ >= 0 )
		{
			glFramebufferTextureLayer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, shadowTex_, 0, moonLayer_ );
			glDrawBuffer( GL_NONE );
			glClear( GL_DEPTH_BUFFER_BIT );
			glUniformMatrix4fv( lvp, 1, GL_FALSE, moonVP_.m );
			DrawInstances( progShadow_, true );
			drawModels( progShadow_ );
		}
		for( auto& q : projs_ )
		{
			if( q.dirShadow[ 3 ] < 0 ) continue;
			glFramebufferTextureLayer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, shadowTex_, 0, (GLint)q.dirShadow[ 3 ] );
			glDrawBuffer( GL_NONE );
			glClear( GL_DEPTH_BUFFER_BIT );
			glUniformMatrix4fv( lvp, 1, GL_FALSE, q.vp );
			DrawInstances( progShadow_, true );
			drawModels( progShadow_ );
		}
		glDisable( GL_POLYGON_OFFSET_FILL );
		glEnable( GL_CULL_FACE );
	}

	//---- main pass
	float aspect = (float)w / (float)h;
	M4 view      = cam.View();
	M4 proj      = cam.Proj( aspect );
	viewProj     = Mul( proj, view );
	eye          = cam.Eye();
	glBindFramebuffer( GL_FRAMEBUFFER, scene_.fbo );
	glViewport( 0, 0, w, h );
	{
		V3 bg = show.stage.ambientColor * ( 0.0025f * show.stage.envLight );
		glClearColor( bg.x, bg.y, bg.z, 1 );
	}
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	const StageCfg& stc = show.stage;
	const bool outdoor  = stc.environment == 1;
	const V3 moonDir    = MoonDir( stc );
	if( outdoor )
	{
		M4 invSky = Inverse( viewProj );
		glUseProgram( progSky_ );
		glDisable( GL_DEPTH_TEST );
		glDepthMask( GL_FALSE );
		glUniformMatrix4fv( glGetUniformLocation( progSky_, "uInvVP" ), 1, GL_FALSE, invSky.m );
		glUniform3f( glGetUniformLocation( progSky_, "uCamPos" ), eye.x, eye.y, eye.z );
		glUniform3f( glGetUniformLocation( progSky_, "uZenith" ), stc.skyZenith.x, stc.skyZenith.y, stc.skyZenith.z );
		glUniform3f( glGetUniformLocation( progSky_, "uHorizon" ), stc.skyHorizon.x, stc.skyHorizon.y, stc.skyHorizon.z );
		glUniform3f( glGetUniformLocation( progSky_, "uMoonDir" ), moonDir.x, moonDir.y, moonDir.z );
		glUniform1f( glGetUniformLocation( progSky_, "uStars" ), stc.stars );
		glUniform1f( glGetUniformLocation( progSky_, "uMoon" ), stc.moon );
		glUniform1f( glGetUniformLocation( progSky_, "uEnv" ), stc.envLight );
		glUniform1f( glGetUniformLocation( progSky_, "uTime" ), (float)g.time );
		glBindVertexArray( quad_.vao );
		for( int i = 3; i <= 8; ++i ) glDisableVertexAttribArray( i );
		glDrawElements( GL_TRIANGLES, quad_.count, GL_UNSIGNED_INT, nullptr );
		glDepthMask( GL_TRUE );
		glEnable( GL_DEPTH_TEST );
	}
	glUseProgram( progScene_ );
	{
		V3 mc = outdoor ? V3( 0.55f, 0.65f, 1.0f ) * ( stc.moon * 0.04f * stc.envLight ) : V3( 0, 0, 0 );
		glUniform3f( glGetUniformLocation( progScene_, "uMoonDir" ), moonDir.x, moonDir.y, moonDir.z );
		glUniform3f( glGetUniformLocation( progScene_, "uMoonCol" ), mc.x, mc.y, mc.z );
		glUniformMatrix4fv( glGetUniformLocation( progScene_, "uMoonVP" ), 1, GL_FALSE, moonVP_.m );
		glUniform1f( glGetUniformLocation( progScene_, "uMoonLayer" ), (float)moonLayer_ );
	}
	glUniformMatrix4fv( glGetUniformLocation( progScene_, "uVP" ), 1, GL_FALSE, viewProj.m );
	glUniform1i( glGetUniformLocation( progScene_, "uNumLights" ), (int)lights_.size() );
	glUniform3f( glGetUniformLocation( progScene_, "uCamPos" ), eye.x, eye.y, eye.z );
	glUniform3f( glGetUniformLocation( progScene_, "uAmbient" ), ambient_.x, ambient_.y, ambient_.z );
	glUniform1f( glGetUniformLocation( progScene_, "uEnv" ), show.stage.envLight );
	glUniform1f( glGetUniformLocation( progScene_, "uShadowTexel" ), 1.0f / shadowRes );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D_ARRAY, goboTex_ );
	glUniform1i( glGetUniformLocation( progScene_, "uGobo" ), 0 );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D_ARRAY, shadowTex_ );
	glUniform1i( glGetUniformLocation( progScene_, "uShadow" ), 1 );
	glUniform1i( glGetUniformLocation( progScene_, "uAlbedo" ), 3 );
	glUniform1i( glGetUniformLocation( progScene_, "uUseTex" ), 0 );
	glUniform1f( glGetUniformLocation( progScene_, "uCutoff" ), 0.0f );
	glUniform1f( glGetUniformLocation( progScene_, "uWork" ), workLight ? 0.32f : 0.0f );
	glUniform1i( glGetUniformLocation( progScene_, "uIgnoreVCol" ), 0 );
	bindProjTex( progScene_ );
	glDisable( GL_CULL_FACE );
	DrawInstances( progScene_, false );
	drawModels( progScene_ );

	//---- lens glow (additive, depth tested)
	if( !glows_.empty() )
	{
		glUseProgram( progGlow_ );
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE );
		glDepthMask( GL_FALSE );
		glUniformMatrix4fv( glGetUniformLocation( progGlow_, "uVP" ), 1, GL_FALSE, viewProj.m );
		V3 right( view( 0, 0 ), view( 0, 1 ), view( 0, 2 ) ), upv( view( 1, 0 ), view( 1, 1 ), view( 1, 2 ) );
		glUniform3f( glGetUniformLocation( progGlow_, "uRight" ), right.x, right.y, right.z );
		glUniform3f( glGetUniformLocation( progGlow_, "uUp" ), upv.x, upv.y, upv.z );
		GLint lp = glGetUniformLocation( progGlow_, "uPos" ), lc = glGetUniformLocation( progGlow_, "uColor" ), ls = glGetUniformLocation( progGlow_, "uSize" );
		glBindVertexArray( quad_.vao );
		for( int i = 3; i <= 8; ++i ) glDisableVertexAttribArray( i );
		for( auto& gl : glows_ )
		{
			V3 toCam  = Norm( eye - gl.pos );
			float fac = Dot( gl.dir, toCam );
			float k   = Smooth( ( fac - gl.cosOuter * 0.85f ) / std::max( 0.02f, 1.0f - gl.cosOuter * 0.85f ) );
			if( k <= 0.001f ) continue;
			float dist = Len( eye - gl.pos );
			V3 c       = gl.color * ( k * 0.4f );
			float size = gl.radius * 2.2f + dist * 0.035f * Saturate( Lum3( c ) * 0.5f );
			glUniform3f( lp, gl.pos.x + gl.dir.x * 0.01f, gl.pos.y + gl.dir.y * 0.01f, gl.pos.z + gl.dir.z * 0.01f );
			glUniform3f( lc, c.x, c.y, c.z );
			glUniform1f( ls, size );
			glDrawElements( GL_TRIANGLES, quad_.count, GL_UNSIGNED_INT, nullptr );
		}
		glDepthMask( GL_TRUE );
		glDisable( GL_BLEND );
	}

	//---- volumetric beams (half resolution)
	glBindFramebuffer( GL_FRAMEBUFFER, beam_.fbo );
	glViewport( 0, 0, beam_.w, beam_.h );
	glClearColor( 0, 0, 0, 1 );
	glClear( GL_COLOR_BUFFER_BIT );
	if( show.stage.beams && show.stage.beamLevel > 0.001f && !beams_.empty() && g.haze > 0.001f )
	{
		glUseProgram( progBeam_ );
		glDisable( GL_DEPTH_TEST );
		glEnable( GL_CULL_FACE );
		glCullFace( GL_FRONT );
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE );
		M4 inv = Inverse( viewProj );
		glUniformMatrix4fv( glGetUniformLocation( progBeam_, "uVP" ), 1, GL_FALSE, viewProj.m );
		glUniformMatrix4fv( glGetUniformLocation( progBeam_, "uInvVP" ), 1, GL_FALSE, inv.m );
		glUniform3f( glGetUniformLocation( progBeam_, "uCamPos" ), eye.x, eye.y, eye.z );
		glUniform2f( glGetUniformLocation( progBeam_, "uSize" ), (float)beam_.w, (float)beam_.h );
		glUniform1f( glGetUniformLocation( progBeam_, "uHaze" ), g.haze * 0.09f * show.stage.beamLevel );
		glUniform1f( glGetUniformLocation( progBeam_, "uNoise" ), show.stage.hazeNoise );
		glUniform1f( glGetUniformLocation( progBeam_, "uTime" ), (float)g.time );
		glUniform1i( glGetUniformLocation( progBeam_, "uSteps" ), (int)( 28 * Clamp( quality.beamQuality > 0 ? std::min( quality.beamQuality, show.stage.beamQuality ) : show.stage.beamQuality, 0.25f, 3.0f ) ) );
		glUniform1f( glGetUniformLocation( progBeam_, "uShadowTexel" ), 1.0f / shadowRes );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D_ARRAY, goboTex_ );
		glUniform1i( glGetUniformLocation( progBeam_, "uGobo" ), 0 );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D_ARRAY, shadowTex_ );
		glUniform1i( glGetUniformLocation( progBeam_, "uShadow" ), 1 );
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, scene_.depth );
		glUniform1i( glGetUniformLocation( progBeam_, "uDepth" ), 2 );
		GLint lLight = glGetUniformLocation( progBeam_, "uLight" ), lApex = glGetUniformLocation( progBeam_, "uApex" ), lCol = glGetUniformLocation( progBeam_, "uBeamColor" );
		GLint lTan = glGetUniformLocation( progBeam_, "uTanOuter" ), lRange = glGetUniformLocation( progBeam_, "uRange" ), lModel = glGetUniformLocation( progBeam_, "uModel" );
		glBindVertexArray( cone_.vao );
		for( int i = 3; i <= 8; ++i ) glDisableVertexAttribArray( i );
		for( auto& b : beams_ )
		{
			const LightGpu& l = lights_[ b.light ];
			V3 D( l.dirTan[ 0 ], l.dirTan[ 1 ], l.dirTan[ 2 ] ), U( l.upSoft[ 0 ], l.upSoft[ 1 ], l.upSoft[ 2 ] );
			V3 R = Cross( D, U );
			float r = b.tanOuter * b.range * 1.03f;
			M4 m;
			m( 0, 0 ) = R.x * r; m( 1, 0 ) = R.y * r; m( 2, 0 ) = R.z * r;
			m( 0, 1 ) = U.x * r; m( 1, 1 ) = U.y * r; m( 2, 1 ) = U.z * r;
			m( 0, 2 ) = D.x * b.range; m( 1, 2 ) = D.y * b.range; m( 2, 2 ) = D.z * b.range;
			V3 a = b.apex - D * 0.05f;
			m( 0, 3 ) = a.x; m( 1, 3 ) = a.y; m( 2, 3 ) = a.z;
			glUniform1i( lLight, b.light );
			glUniform3f( lApex, b.apex.x, b.apex.y, b.apex.z );
			glUniform3f( lCol, b.color.x, b.color.y, b.color.z );
			glUniform1f( lTan, b.tanOuter );
			glUniform1f( lRange, b.range );
			glUniformMatrix4fv( lModel, 1, GL_FALSE, m.m );
			glDrawElements( GL_TRIANGLES, cone_.count, GL_UNSIGNED_INT, nullptr );
		}
		glDisable( GL_BLEND );
		glCullFace( GL_BACK );
		glDisable( GL_CULL_FACE );
	}
	//projector light in the haze
	if( show.stage.beams && quality.projHaze && !projs_.empty() && g.haze > 0.001f )
	{
		glUseProgram( progProjBeam_ );
		glDisable( GL_DEPTH_TEST );
		glDisable( GL_CULL_FACE );
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE );
		M4 inv = Inverse( viewProj );
		glUniformMatrix4fv( glGetUniformLocation( progProjBeam_, "uVP" ), 1, GL_FALSE, viewProj.m );
		glUniformMatrix4fv( glGetUniformLocation( progProjBeam_, "uInvVP" ), 1, GL_FALSE, inv.m );
		glUniform3f( glGetUniformLocation( progProjBeam_, "uCamPos" ), eye.x, eye.y, eye.z );
		glUniform2f( glGetUniformLocation( progProjBeam_, "uSize" ), (float)beam_.w, (float)beam_.h );
		glUniform1f( glGetUniformLocation( progProjBeam_, "uHaze" ), g.haze * 0.09f * show.stage.beamLevel );
		glUniform1f( glGetUniformLocation( progProjBeam_, "uNoise" ), show.stage.hazeNoise );
		glUniform1f( glGetUniformLocation( progProjBeam_, "uTime" ), (float)g.time );
		glUniform1i( glGetUniformLocation( progProjBeam_, "uSteps" ), (int)( 28 * Clamp( quality.beamQuality > 0 ? std::min( quality.beamQuality, show.stage.beamQuality ) : show.stage.beamQuality, 0.25f, 3.0f ) ) );
		glUniform1f( glGetUniformLocation( progProjBeam_, "uShadowTexel" ), 1.0f / shadowRes );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D_ARRAY, shadowTex_ );
		glUniform1i( glGetUniformLocation( progProjBeam_, "uShadow" ), 1 );
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, scene_.depth );
		glUniform1i( glGetUniformLocation( progProjBeam_, "uDepth" ), 2 );
		bindProjTex( progProjBeam_ );
		GLint lProj = glGetUniformLocation( progProjBeam_, "uProj" ), lInv = glGetUniformLocation( progProjBeam_, "uInvProj" );
		glBindVertexArray( projBox_.vao );
		for( int i = 0; i < (int)projs_.size(); ++i )
		{
			M4 pvp;
			memcpy( pvp.m, projs_[ i ].vp, 64 );
			M4 ip = Inverse( pvp );
			glUniform1i( lProj, i );
			glUniformMatrix4fv( lInv, 1, GL_FALSE, ip.m );
			glDrawElements( GL_TRIANGLES, projBox_.count, GL_UNSIGNED_INT, nullptr );
		}
		glDisable( GL_BLEND );
	}
	glDisable( GL_DEPTH_TEST );

	//---- combine
	glBindVertexArray( quad_.vao );
	for( int i = 3; i <= 8; ++i ) glDisableVertexAttribArray( i );
	auto fullscreen = [ & ]() { glDrawElements( GL_TRIANGLES, quad_.count, GL_UNSIGNED_INT, nullptr ); };
	glBindFramebuffer( GL_FRAMEBUFFER, comb_.fbo );
	glViewport( 0, 0, w, h );
	glUseProgram( progComb_ );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, scene_.color );
	glUniform1i( glGetUniformLocation( progComb_, "uScene" ), 0 );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, beam_.color );
	glUniform1i( glGetUniformLocation( progComb_, "uBeam" ), 1 );
	glUniform2f( glGetUniformLocation( progComb_, "uBeamTexel" ), 0.75f / beam_.w, 0.75f / beam_.h );
	{
		const StageCfg& sf = show.stage;
		M4 invC            = Inverse( viewProj );
		V3 fc              = sf.skyHorizon * sf.envLight;
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, scene_.depth );
		glUniform1i( glGetUniformLocation( progComb_, "uDepth" ), 2 );
		glUniformMatrix4fv( glGetUniformLocation( progComb_, "uInvVP" ), 1, GL_FALSE, invC.m );
		glUniform3f( glGetUniformLocation( progComb_, "uCamPos" ), eye.x, eye.y, eye.z );
		glUniform1f( glGetUniformLocation( progComb_, "uFog" ), sf.environment == 1 ? sf.distanceFog : 0.0f );
		glUniform3f( glGetUniformLocation( progComb_, "uFogCol" ), fc.x, fc.y, fc.z );
		glActiveTexture( GL_TEXTURE0 );
	}
	fullscreen();

	//---- bloom
	glUseProgram( progDown_ );
	glUniform1i( glGetUniformLocation( progDown_, "uSrc" ), 0 );
	GLint dTex = glGetUniformLocation( progDown_, "uTexel" ), dFirst = glGetUniformLocation( progDown_, "uFirst" );
	for( size_t i = 0; i < bloom_.size(); ++i )
	{
		const Fbo& src = i == 0 ? comb_ : bloom_[ i - 1 ];
		glBindFramebuffer( GL_FRAMEBUFFER, bloom_[ i ].fbo );
		glViewport( 0, 0, bloom_[ i ].w, bloom_[ i ].h );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, src.color );
		glUniform2f( dTex, 1.0f / src.w, 1.0f / src.h );
		glUniform1i( dFirst, i == 0 ? 1 : 0 );
		fullscreen();
	}
	glUseProgram( progUp_ );
	glUniform1i( glGetUniformLocation( progUp_, "uSrc" ), 0 );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE );
	for( int i = (int)bloom_.size() - 1; i > 0; --i )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, bloom_[ i - 1 ].fbo );
		glViewport( 0, 0, bloom_[ i - 1 ].w, bloom_[ i - 1 ].h );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, bloom_[ i ].color );
		glUniform2f( glGetUniformLocation( progUp_, "uTexel" ), 1.0f / bloom_[ i ].w, 1.0f / bloom_[ i ].h );
		fullscreen();
	}
	glDisable( GL_BLEND );

	//---- final
	glBindFramebuffer( GL_FRAMEBUFFER, out_.fbo );
	glViewport( 0, 0, w, h );
	glUseProgram( progFinal_ );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, comb_.color );
	glUniform1i( glGetUniformLocation( progFinal_, "uHdr" ), 0 );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, bloom_.empty() ? comb_.color : bloom_[ 0 ].color );
	glUniform1i( glGetUniformLocation( progFinal_, "uBloom" ), 1 );
	glUniform1f( glGetUniformLocation( progFinal_, "uExposure" ), show.stage.exposure );
	glUniform1f( glGetUniformLocation( progFinal_, "uBloomAmt" ), quality.bloom ? show.stage.bloom : 0.0f );
	fullscreen();

	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	glBindVertexArray( 0 );
	glUseProgram( 0 );
	glActiveTexture( GL_TEXTURE0 );
	stats.ms = (float)( ( Now() - t0 ) * 1000.0 );
}

bool Renderer::ReadOutput( std::vector< uint8_t >& rgba, int& w, int& h )
{
	if( !out_.fbo ) return false;
	w = out_.w;
	h = out_.h;
	rgba.resize( (size_t)w * h * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, out_.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data() );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	//flip rows
	std::vector< uint8_t > row( (size_t)w * 4 );
	for( int y = 0; y < h / 2; ++y )
	{
		uint8_t* a = &rgba[ (size_t)y * w * 4 ];
		uint8_t* b = &rgba[ (size_t)( h - 1 - y ) * w * 4 ];
		memcpy( row.data(), a, row.size() );
		memcpy( a, b, row.size() );
		memcpy( b, row.data(), row.size() );
	}
	return true;
}
}//namespace ls
