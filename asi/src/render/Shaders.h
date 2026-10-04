// HLSL of the block renderer and the overlay, compiled at runtime with D3DCompile (vs_3_0 /
// ps_3_0). Under Proton that is Wine's d3dcompiler_47 (vkd3d-shader), so the code sticks to plain
// SM3 HLSL: no integer ops, no gradients or texture fetches inside branches, no early returns.
#pragma once

// ---- shared blocks (adjacent string literals: pasted into the shaders that use them) ----------
// GTA's lighting, pixel shader c1-c13 (render/Lighting.h LightingParams, see kWorld's comment).
#define LC_HLSL_LIGHT R"(
float4 sunDir : register(c1);
float4 sunColor : register(c2);
float4 ambient0 : register(c3);
float4 ambient1 : register(c4);
float4 fogParams : register(c5);
float4 fogColor : register(c6);
float4 fogColorN : register(c7);
float4 depthFx : register(c8);
float4 tone : register(c9);
float4 grade : register(c10);
float4 shift : register(c11);
float4 misc : register(c12);
float4 bloom : register(c13);
)"

// GTA's sun shadow (render/Shadows.h, which mirrors ShadowLookup): pixel shader c14-c27
// (ShadowParams in register order; Render.cpp's watch on GTA's constants ignores our own writes), s2 GTA's cascade atlas, s3 ours (the blocks; same layout).
// SunShadow(camera-relative position, normal, pixel centre) -> x lit by GTA's world (1 = lit), y lit
// by GTA's world and the blocks. The maths, the taps and the Poisson disk are those of FusionFix's
// deferred sun pass (deferred_lighting.fxc).
#define LC_HLSL_SHADOW R"(
float4 shFwd : register(c14);
float4 shSplit : register(c15);
float4 shMx : register(c16);
float4 shMy : register(c17);
float4 shMz : register(c18);
float4 shCasc0 : register(c19);
float4 shCasc1 : register(c20);
float4 shCasc2 : register(c21);
float4 shCasc3 : register(c22);
float4 shBounds : register(c23);
float4 shFilter : register(c24);
float4 shMisc : register(c25);
float4 shChss : register(c26);
float4 shNdl : register(c27);
sampler2D shGta : register(s2);
sampler2D shOurs : register(s3);

static const float4 kPoisson[8] = {
	float4(0.189936, 0.0270871, -0.212612, 0.233913), float4(0.0477178, -0.366684, 0.297731, 0.39826),
	float4(-0.509063, -0.0652868, 0.507855, -0.287598), float4(-0.152306, 0.642612, -0.302402, -0.580507),
	float4(0.697802, 0.277117, -0.699096, 0.321096), float4(0.356514, -0.706641, 0.26689, 0.836019),
	float4(-0.751586, -0.416099, 0.910294, -0.170145), float4(-0.534347, 0.805859, -0.113327, -0.949003) };

float2 ShRot(float2 o, float2 cs)
{
	return float2(o.x * cs.x - o.y * cs.y, o.x * cs.y + o.y * cs.x);
}

// The two atlases at one tap (x clamped into the cascade's quarter): x GTA's texel, y the nearer of
// GTA's and ours (our cleared texels, 1.0, hold nothing).
float2 ShTexels(float2 uv, float2 cu)
{
	float4 c = float4(clamp(uv.x, cu.x, cu.y), uv.y, 0.0, 0.0);
	float g = tex2Dlod(shGta, c).r;
	float o = tex2Dlod(shOurs, c).r;
	o = o + step(1.0, o) * 1e6;
	return float2(g, min(g, o));
}

float2 ShTap(float2 uv, float zr, float2 cu)
{
	float2 t = ShTexels(uv, cu);
	return step(float2(zr, zr), t);  // texel >= zr: lit
}

float2 SunShadow(float3 rel, float3 nrm, float2 pix)
{
	float d = dot(shFwd.xyz, rel);
	float4 f = float4(1.0, step(shSplit.xyz, float3(d, d, d)));
	float4 oh = abs(f - float4(f.yzw, 0.0));
	float4 so = oh.x * shCasc0 + oh.y * shCasc1 + oh.z * shCasc2 + oh.w * shCasc3;
	float4 p = float4(rel, 1.0);
	float3 sc = float3(dot(p, shMx), dot(p, shMy), dot(p, shMz));
	float3 nsc = float3(dot(nrm, shMx.xyz), dot(nrm, shMy.xyz), dot(nrm, shMz.xyz));
	float farB = dot(oh, shBounds);
	float nextB = dot(oh, shBounds.yzww);
	float width = dot(oh, shBounds - float4(0.0, shBounds.xyz));
	float blendW = max(width * shFilter.w, 1e-4);
	float2 tt = saturate((float2(d, d) - float2(farB - width, farB - blendW)) / float2(width, blendW));
	float2 r20 = lerp(float2(farB, 1.0), float2(nextB, nextB / farB), tt);
	float r26y = shBounds.x / (r20.y * farB);
	r20 *= shFilter.x;
	sc += nsc * (r20.x * shFilter.z * shMisc.x);
	float zr = 0.5 - 0.0005 * sc.z;
	float2 rad = float2(r20.y * shMisc.w, r20.y);
	float2 cu = float2(dot(oh, float4(-1.0, 0.25, 0.5, 0.75)), dot(oh, float4(0.25, 0.5, 0.75, 2.0)) - shFilter.x * 0.25);
	float2 uv = sc.xy * so.xy + so.zw;
	float a = frac(frac(dot(pix, float2(0.0671106, 0.00583715))) * 52.9829) * 6.28319;
	float2 cs = float2(cos(a), sin(a));
	float k = shFilter.y;
	[branch] if (shMisc.z > 0.5)
	{
		// CHSS: the blockers' distance widens the filter.
		float2 srad = rad * shChss.x * r26y;
		float2 blk = float2(0.0, 0.0);
		[unroll] for (int i = 0; i < 8; ++i)
		{
			float t0 = ShTexels(uv + ShRot(kPoisson[i].xy, cs) * srad, cu).y;
			float t1 = ShTexels(uv + ShRot(kPoisson[i].zw, cs) * srad, cu).y;
			float b0 = step(t0, zr), b1 = step(t1, zr);
			blk += float2(t0 * b0 + t1 * b1, b0 + b1);
		}
		float avg = blk.x / max(blk.y, 1e-6);
		float pen = blk.y > 0.5 ? abs((zr - avg) / avg) * shChss.y : 0.0;
		float pc = min(pen + shFilter.y, shChss.x);
		k = max(pc * r26y, shFilter.y);
		zr += (pc - shFilter.y) * shBounds.x * shFilter.x * -0.0008 * shMisc.x;
	}
	rad *= k;
	float2 lit = float2(0.0, 0.0);
	[branch] if (shMisc.y > 0.5)
	{
		[unroll] for (int j = 0; j < 8; ++j)
		{
			lit += ShTap(uv + ShRot(kPoisson[j].xy, cs) * rad, zr, cu);
			lit += ShTap(uv + ShRot(kPoisson[j].zw, cs) * rad, zr, cu);
		}
		lit *= 0.0625;
	}
	else
	{
		float b = frac(dot(pix, float2(3.0, 7.138)) * 0.159155 + 0.5) * 6.28319 - 3.14159;
		float2 q = float2(cos(b), sin(b));
		lit = ShTap(uv + float2(-0.25 * q.y, -0.25 * q.x) * rad, zr, cu) + ShTap(uv + float2(q.x, -q.y) * rad, zr, cu) +
		      ShTap(uv + float2(0.75 * q.y, 0.75 * q.x) * rad, zr, cu) + ShTap(uv + float2(-0.5 * q.x, 0.5 * q.y) * rad, zr, cu);
		lit *= 0.25;
	}
	float fd = saturate(length(rel) / shSplit.w);
	return lerp(lit, float2(1.0, 1.0), fd * fd);
}
)"

namespace lc::render::shaders
{
	// ---- world: Minecraft's block meshes and entities ----------------------------------------
	// Vertex: RenVertex (32 B): float3 pos, float2 uv, UBYTE4N colour (RGBA8), UBYTE4 light
	// (block, sky), UBYTE4 flags (byte 0: the protocol's flag bits).
	//
	// VS constants: c0-c3 the camera-relative clip matrix by column (clip.i = dot(rel, ci)),
	// c4.xyz the mesh's Minecraft origin in camera-relative GTA metres, c5 depth: x near, y
	// 1/log2(far/near), z 1 = write FusionFix's logarithmic depth (z/w = log2(w/near)/log2(far/near)).
	// PS constants: c0 Minecraft's own lighting (RenderLighting=minecraft, or GTA's values missing):
	// x day factor (sky light), y gamma (Minecraft's 0.5), z minimum brightness, w exposure.
	// c1-c13 GTA's lighting (render/Lighting.h LightingParams; light in GTA's HDR units): c1 xyz
	// towards the sun/moon, w 1 = GTA lighting on; c2 sun colour; c3 ambient; c4 down-face ambient;
	// c5 fog start, end, offset, near-ramp weight; c6 far fog colour, w 1 = fog on; c7 near fog
	// colour; c8 GTA's distance desaturation: far saturation, far gamma, near, far; c9 tone mapping:
	// x Exposure x key, y saturation, z gamma, w adapted luminance (without the texture); c10 rgb
	// ColorCorrect x 2, w 1 = adapted luminance from s1 (GTA's 1x1 texture); c11 ColorShift; c12 x
	// RenderExposure, y wetness, z block light strength; c13 bloom: x threshold, y intensity.
	// c14-c27 GTA's sun shadow (LC_HLSL_SHADOW; c14.w 0 = none), s2/s3 the cascade atlases: the sun
	// is shaded by GTA's world and the blocks.
	inline constexpr char kWorld[] = R"(
float4 clipX : register(c0);
float4 clipY : register(c1);
float4 clipZ : register(c2);
float4 clipW : register(c3);
float4 meshOffset : register(c4);
float4 depthParams : register(c5);

float4 lightParams : register(c0);
sampler2D atlas : register(s0);
sampler2D adaptedLum : register(s1);
)" LC_HLSL_LIGHT LC_HLSL_SHADOW R"(

struct VSIn
{
	float3 pos : POSITION;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
	float4 light : TEXCOORD1;
	float4 flags : TEXCOORD2;
};

struct VSOut
{
	float4 pos : POSITION;
	float4 color : COLOR0;
	float2 uv : TEXCOORD0;
	float4 lf : TEXCOORD1;   // block light, sky light (0..1), face shade (-1: from the triangle), flag byte
	float3 rel : TEXCOORD2;  // camera-relative GTA position
	float4 nd : TEXCOORD3;   // xyz the face normal (GTA axes; 0: none), w view depth (m)
};

// Bit n of a flag byte held in a float (0..255): 1 or 0.
float Bit(float f, float scale)
{
	return step(0.25, frac(floor(f / scale) * 0.5));
}

VSOut VSMain(VSIn i)
{
	VSOut o;
	float4 rel = float4(meshOffset.xyz + float3(i.pos.x, -i.pos.z, i.pos.y), 1.0);  // Minecraft -> GTA axes
	float4 p = float4(dot(rel, clipX), dot(rel, clipY), dot(rel, clipZ), dot(rel, clipW));
	float n = depthParams.x;
	float logZ = log2(max(p.w, n) / n) * depthParams.y * p.w;
	float z = p.w > n ? logZ : p.w - n;  // in front of the near plane: z < 0, clipped
	p.z = depthParams.z > 0.5 ? z : p.z;
	o.pos = p;
	o.color = i.color;
	o.uv = i.uv;
	float f = i.flags.x;
	float ni = fmod(floor(f / 16.0), 8.0);
	float shade = 1.0;
	shade = abs(ni - 1.0) < 0.5 ? 0.5 : shade;               // down
	shade = (ni > 2.5 && ni < 4.5) ? 0.8 : shade;             // north, south
	shade = (ni > 4.5 && ni < 6.5) ? 0.6 : shade;             // west, east
	shade = ni > 6.5 ? -1.0 : shade;                          // from the triangle's own normal
	// Minecraft's Direction ordinal + 1 -> GTA axes (x east, y north = Minecraft -z, z up).
	float3 nv = float3(0.0, 0.0, 0.0);
	nv = abs(ni - 1.0) < 0.5 ? float3(0.0, 0.0, -1.0) : nv;
	nv = abs(ni - 2.0) < 0.5 ? float3(0.0, 0.0, 1.0) : nv;
	nv = abs(ni - 3.0) < 0.5 ? float3(0.0, 1.0, 0.0) : nv;
	nv = abs(ni - 4.0) < 0.5 ? float3(0.0, -1.0, 0.0) : nv;
	nv = abs(ni - 5.0) < 0.5 ? float3(-1.0, 0.0, 0.0) : nv;
	nv = abs(ni - 6.0) < 0.5 ? float3(1.0, 0.0, 0.0) : nv;
	o.lf = float4(i.light.x / 15.0, i.light.y / 15.0, shade, f);
	o.rel = rel.xyz;
	o.nd = float4(nv, p.w);
	return o;
}

float Curve(float l)
{
	return l / (4.0 - 3.0 * l);  // Minecraft's light-level falloff
}

static const float3 kLum = float3(0.2125, 0.7154, 0.0721);

float4 PSMain(VSOut i, float2 vpos : VPOS) : COLOR0
{
	float f = i.lf.w;
	float translucent = Bit(f, 2.0);
	float untextured = Bit(f, 4.0);
	float nomip = Bit(f, 8.0);
	float4 t = lerp(tex2D(atlas, i.uv), tex2Dlod(atlas, float4(i.uv, 0.0, 0.0)), nomip).bgra;  // RGBA bytes in an ARGB texture
	float3 albedo = t.rgb * i.color.rgb;
	float3 tri = normalize(cross(ddy(i.rel), ddx(i.rel)));
	tri = dot(tri, i.rel) > 0.0 ? -tri : tri;  // the side facing the camera

	// Minecraft's own lighting: face shade x the light-map curve of max(block, sky x day).
	float3 n2 = tri * tri;
	float faceShade = n2.x * 0.6 + n2.y * 0.8 + n2.z * (tri.z > 0.0 ? 1.0 : 0.5);
	float shade = i.lf.z < 0.0 ? faceShade : i.lf.z;
	float b = max(Curve(saturate(i.lf.x)), Curve(saturate(i.lf.y * lightParams.x)));
	float g = 1.0 - (1.0 - b) * (1.0 - b) * (1.0 - b) * (1.0 - b);
	b = lerp(b, g, lightParams.y);
	b = lightParams.z + (1.0 - lightParams.z) * b;
	float3 mcLit = albedo * (shade * b * lightParams.w);

	// GTA IV's lighting, in its HDR units: its sun/moon and ambient as its deferred pass applies them,
	// Minecraft's sky light as the occlusion (roofed-over blocks get less ambient, no sun); GTA's
	// distance desaturation and fog; then its tone mapping. Block light (torches, glowstone) is a
	// warm floor on the screen.
	float known = (i.lf.z < 0.0 || dot(i.nd.xyz, i.nd.xyz) > 0.5) ? 1.0 : 0.0;
	float3 nrm = i.lf.z < 0.0 ? tri : i.nd.xyz;
	float sky = Curve(saturate(i.lf.y));
	float blk = Curve(saturate(i.lf.x));
	float ndl = known > 0.5 ? saturate(dot(nrm, sunDir.xyz) * shNdl.x + shNdl.y) : 0.35 + 0.4 * saturate(sunDir.z);  // GTA's (FusionFix's) N.L remap
	float down = known > 0.5 ? saturate(0.5 - 0.5 * nrm.z) : 0.5;
	float2 shd = float2(1.0, 1.0);
	[branch] if (shFwd.w > 0.5 && sunDir.w > 0.5)
	{
		shd = SunShadow(i.rel, nrm * known, vpos + 0.5);
	}
	float3 light = (ambient0.rgb + ambient1.rgb * down) * (0.3 + 0.7 * sky) + sunColor.rgb * (ndl * sky * sky * shd.y);
	float wet = misc.y * saturate(nrm.z) * known * sky * 0.3;  // rain darkens what faces the sky
	float3 c = albedo * light * (1.0 - wet);
	float d = i.nd.w;
	float tfx = saturate((d - depthFx.z) / max(depthFx.w - depthFx.z, 0.001));
	float lumC = dot(c, kLum);
	c = lerp(float3(lumC, lumC, lumC), c, lerp(1.0, depthFx.x, tfx)) * pow(max(lumC, 1e-7), tfx * (depthFx.y - 1.0));
	float ramp = saturate((d - fogParams.x) / max(fogParams.y - fogParams.x, 0.001));
	float nearRamp = saturate(d / max(fogParams.x, 0.001));
	float fog = saturate(fogParams.w * nearRamp + (1.0 - fogParams.w) * ramp + fogParams.z) * fogColor.w;
	c = lerp(c, lerp(fogColorN.rgb, fogColor.rgb, ramp), fog);
	float adapted = grade.w > 0.5 ? tex2Dlod(adaptedLum, float4(0.5, 0.5, 0.0, 0.0)).r : tone.w;
	c *= tone.x / max(adapted, 1e-6);
	c += max(c - bloom.x, 0.0) * bloom.y;  // GTA's bloom, the pixel's own light standing in for its blurred surroundings
	float lumT = dot(c, kLum);
	c = lerp(float3(lumT, lumT, lumT), c, tone.y);
	float a = lumT * shift.w;
	c = saturate(a) * (c - shift.rgb * a) + shift.rgb * a;
	c = max(c * grade.rgb * pow(max(saturate(lumT), 1e-6), tone.z - 1.0), 0.0) * misc.x;
	c = max(c, albedo * blk * misc.z * float3(1.0, 0.85, 0.65));
	float3 lit = sunDir.w > 0.5 ? c : mcLit;

	float alpha = translucent > 0.5 ? t.a * i.color.a : 1.0;
	clip(untextured > 0.5 ? 1.0 : (translucent > 0.5 ? 1.0 : t.a - 0.5));  // cutout: alpha test
	float4 o = float4(saturate(lit), alpha);
	o = shChss.w > 0.5 ? float4(shd, 0.5, alpha) : o;  // DebugShadowView: red GTA's shadow term, green with the blocks
	return untextured > 0.5 ? i.color : o;
}
)";

	// ---- overlay: Minecraft's GUI over the whole back buffer ---------------------------------
	// Vertex: float4 position (clip space), float2 uv. PS constants: c0 xy cursor (back buffer
	// pixels), z cursor on, w rows bottom-up; c1 the crosshair rect x0 y0 x1 y1 (back buffer
	// pixels; drawn in the invert pass, left out of the main one); c2 x 1 = invert pass.
	inline constexpr char kOverlay[] = R"(
float4 cursor : register(c0);
float4 invertRect : register(c1);
float4 mode : register(c2);
sampler2D overlay : register(s0);

struct VSOut
{
	float4 pos : POSITION;
	float2 uv : TEXCOORD0;
};

VSOut VSMain(float4 pos : POSITION, float2 uv : TEXCOORD0)
{
	VSOut o;
	o.pos = pos;
	o.uv = uv;
	return o;
}

float4 PSMain(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR0
{
	uv.y = cursor.w > 0.5 ? 1.0 - uv.y : uv.y;
	float4 c = tex2D(overlay, uv).bgra;  // premultiplied RGBA bytes in an ARGB texture
	float inRect = (vpos.x >= invertRect.x && vpos.y >= invertRect.y && vpos.x < invertRect.z && vpos.y < invertRect.w) ? 1.0 : 0.0;
	float2 p = vpos - cursor.xy;
	float onArrow = (cursor.z > 0.5 && p.x >= 0.0 && p.y >= 0.0 && p.y < 18.0 && p.x <= p.y * 0.6) ? 1.0 : 0.0;
	float edge = (p.x < 1.5 || p.x > p.y * 0.6 - 1.5 || p.y > 16.5) ? 1.0 : 0.0;
	float4 arrow = float4(edge > 0.5 ? float3(0.0, 0.0, 0.0) : float3(1.0, 1.0, 1.0), 1.0);
	float4 mainPass = inRect > 0.5 ? float4(0.0, 0.0, 0.0, 0.0) : (onArrow > 0.5 ? arrow : c);
	clip(mode.x > 0.5 ? inRect - 0.5 : 1.0);
	return mode.x > 0.5 ? float4(c.rgb, 0.0) : mainPass;
}
)";

	// ---- shadow casters: Minecraft's geometry into our cascade atlas ------------------------------
	// The world's vertex layout. VS constants: c0, c1 clip x and y of cascade k (render/Shadows.h
	// CasterRows: dot(float4(rel, 1), row)), c2 the light depth row (sc.z), c3 x the caster bias (light
	// depth units, pushes the caster away from the light), c4.xyz the mesh's origin (camera-relative GTA
	// metres). The colour target (R32F) gets zr = 0.5 - 0.0005 * sc.z as GTA's sun pass compares it; the
	// depth test keeps the one nearest the light. Cutout is alpha-tested, translucent casts nothing.
	inline constexpr char kCaster[] = R"(
float4 rowX : register(c0);
float4 rowY : register(c1);
float4 rowZ : register(c2);
float4 casterParams : register(c3);
float4 meshOffset : register(c4);
sampler2D atlas : register(s0);

struct VSIn
{
	float3 pos : POSITION;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
	float4 light : TEXCOORD1;
	float4 flags : TEXCOORD2;
};

struct VSOut
{
	float4 pos : POSITION;
	float2 uv : TEXCOORD0;
	float2 zf : TEXCOORD1;  // zr, flag byte
};

float Bit(float f, float scale)
{
	return step(0.25, frac(floor(f / scale) * 0.5));
}

VSOut VSMain(VSIn i)
{
	VSOut o;
	float4 rel = float4(meshOffset.xyz + float3(i.pos.x, -i.pos.z, i.pos.y), 1.0);
	float zr = 0.5 - 0.0005 * (dot(rel, rowZ) - casterParams.x);
	o.pos = float4(dot(rel, rowX), dot(rel, rowY), saturate(zr), 1.0);
	o.uv = i.uv;
	o.zf = float2(zr, i.flags.x);
	return o;
}

float4 PSMain(VSOut i) : COLOR0
{
	float f = i.zf.y;
	float a = tex2D(atlas, i.uv).a;
	clip(Bit(f, 4.0) > 0.5 ? -1.0 : (Bit(f, 2.0) > 0.5 ? -1.0 : a - 0.5));
	return float4(i.zf.x, 0.0, 0.0, 1.0);
}
)";

	// ---- GTA's world in the blocks' shadow: a full-screen pass over GTA's frame ------------------
	// Multiplies the frame (blend: dest x src) by render/Shadows.h DarkenFactor where the blocks shade
	// what GTA lit: GTA's depth buffer (s0) gives each pixel's position, its neighbours the normal.
	// PS constants: c1-c13 GTA's lighting (c9.y its tone mapping's saturation, c9.z its gamma), c14-c27 the shadow, c28-c30 the camera-relative ray at view
	// depth 1 (ndc.x * c28 + ndc.y * c29 + c30), c31 depth: x near, y log2(far/near), z 1 =
	// FusionFix's logarithmic depth, w far; c32 x 1/width, y 1/height, z strength, w 1 = s1 holds GTA's
	// G-buffer 2, whose z scales GTA's ambient (its sun pass: ambient * gbuffer2.z).
	inline constexpr char kDarken[] = LC_HLSL_LIGHT LC_HLSL_SHADOW R"(
float4 dkRay0 : register(c28);
float4 dkRay1 : register(c29);
float4 dkRay2 : register(c30);
float4 dkDepth : register(c31);
float4 dkScreen : register(c32);
sampler2D gtaDepth : register(s0);
sampler2D gtaGBuffer2 : register(s1);
static const float3 kLum = float3(0.2125, 0.7154, 0.0721);  // GTA's luminance weights

float4 VSMain(float4 pos : POSITION) : POSITION
{
	return pos;
}

// Camera-relative position at a pixel centre, and the raw depth.
float4 RelAt(float2 pc)
{
	float2 uv = pc * dkScreen.xy;
	float d = tex2Dlod(gtaDepth, float4(uv, 0.0, 0.0)).r;
	float wl = dkDepth.x * exp2(d * dkDepth.y);
	float ws = dkDepth.x * dkDepth.w / max(dkDepth.w - d * (dkDepth.w - dkDepth.x), 1e-6);
	float w = dkDepth.z > 0.5 ? wl : ws;
	float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	return float4((ndc.x * dkRay0.xyz + ndc.y * dkRay1.xyz + dkRay2.xyz) * w, d);
}

float4 PSMain(float2 vpos : VPOS) : COLOR0
{
	float2 pc = vpos + 0.5;
	float4 c = RelAt(pc);
	float3 o = float3(1.0, 1.0, 1.0);
	[branch] if (c.w < 0.999999 && dot(c.xyz, c.xyz) < shSplit.w * shSplit.w)
	{
		// The normal from the neighbour on the nearer side (no smearing across silhouettes).
		float3 l = RelAt(pc - float2(1.0, 0.0)).xyz;
		float3 r = RelAt(pc + float2(1.0, 0.0)).xyz;
		float3 u = RelAt(pc - float2(0.0, 1.0)).xyz;
		float3 b = RelAt(pc + float2(0.0, 1.0)).xyz;
		float dc = dot(shFwd.xyz, c.xyz);
		float3 dx = abs(dot(shFwd.xyz, r) - dc) < abs(dc - dot(shFwd.xyz, l)) ? r - c.xyz : c.xyz - l;
		float3 dy = abs(dot(shFwd.xyz, b) - dc) < abs(dc - dot(shFwd.xyz, u)) ? b - c.xyz : c.xyz - u;
		float3 n = normalize(cross(dy, dx));
		n = dot(n, c.xyz) > 0.0 ? -n : n;
		float2 s = SunShadow(c.xyz, n, pc);
		float ndl = saturate(dot(n, sunDir.xyz) * shNdl.x + shNdl.y);
		float down = saturate(0.5 - 0.5 * n.z);
		float ao = dkScreen.w > 0.5 ? tex2Dlod(gtaGBuffer2, float4(pc * dkScreen.xy, 0.0, 0.0)).z : 1.0;
		float3 amb = max(ambient0.rgb + ambient1.rgb * down, 0.0) * ao;
		float3 sun = max(sunColor.rgb, 0.0) * ndl;
		float3 lit = amb + sun * s.x;
		float3 shaded = amb + sun * min(s.y, s.x);
		float ramp = saturate((dc - fogParams.x) / max(fogParams.y - fogParams.x, 0.001));
		float nearRamp = saturate(dc / max(fogParams.x, 0.001));
		float fog = saturate(fogParams.w * nearRamp + (1.0 - fogParams.w) * ramp + fogParams.z) * fogColor.w;
		shaded = lerp(shaded, lit, fog);
		// As GTA's tone mapping sees both: its saturation step and its luminance gamma (DarkenFactor).
		float lumL = dot(lit, kLum);
		float lumS = dot(shaded, kLum);
		float rl = clamp(lumS / max(lumL, 1e-6), 1e-4, 1.0);
		float g = clamp(tone.z, 0.1, 4.0);
		float neutral = pow(rl, g);
		float sat = saturate(tone.y);
		float3 tl = lumL * (1.0 - sat) + lit * sat;
		float3 ts = lumS * (1.0 - sat) + shaded * sat;
		float3 ratio = ts / max(tl, 1e-6) * pow(rl, g - 1.0);
		ratio = lerp(float3(neutral, neutral, neutral), ratio, step(1e-4 * lumL, tl));  // a channel without light: the grey answer
		ratio = clamp(ratio, 0.5 * neutral, min(2.0 * neutral, 1.0));
		ratio = lumL > 1e-4 ? ratio : float3(1.0, 1.0, 1.0);
		o = lerp(float3(1.0, 1.0, 1.0), ratio, saturate(dkScreen.z));
	}
	return float4(o, 1.0);
}
)";
}
