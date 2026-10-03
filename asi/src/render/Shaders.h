// HLSL of the block renderer and the overlay, compiled at runtime with D3DCompile (vs_3_0 /
// ps_3_0). Under Proton that is Wine's d3dcompiler_47 (vkd3d-shader), so the code sticks to plain
// SM3 HLSL: no integer ops, no gradients or texture fetches inside branches, no early returns.
#pragma once

namespace lc::render::shaders
{
	// ---- world: Minecraft's block meshes and entities ----------------------------------------
	// Vertex: RenVertex (32 B): float3 pos, float2 uv, UBYTE4N colour (RGBA8), UBYTE4 light
	// (block, sky), UBYTE4 flags (byte 0: the protocol's flag bits).
	//
	// VS constants: c0-c3 the camera-relative clip matrix by column (clip.i = dot(rel, ci)),
	// c4.xyz the mesh's Minecraft origin in camera-relative GTA metres, c5 depth: x near, y
	// 1/log2(far/near), z 1 = write FusionFix's logarithmic depth (z/w = log2(w/near)/log2(far/near)).
	// PS constants: c0 x day factor (sky light), y gamma (Minecraft's 0.5), z minimum brightness,
	// w exposure (a multiplier to sit with GTA's tone-mapped picture).
	inline constexpr char kWorld[] = R"(
float4 clipX : register(c0);
float4 clipY : register(c1);
float4 clipZ : register(c2);
float4 clipW : register(c3);
float4 meshOffset : register(c4);
float4 depthParams : register(c5);

float4 lightParams : register(c0);
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
	float4 color : COLOR0;
	float2 uv : TEXCOORD0;
	float4 lf : TEXCOORD1;   // block light, sky light (0..1), face shade (-1: from the triangle), flag byte
	float3 rel : TEXCOORD2;  // camera-relative GTA position
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
	o.lf = float4(i.light.x / 15.0, i.light.y / 15.0, shade, f);
	o.rel = rel.xyz;
	return o;
}

float Curve(float l)
{
	return l / (4.0 - 3.0 * l);  // Minecraft's light-level falloff
}

float4 PSMain(VSOut i) : COLOR0
{
	float f = i.lf.w;
	float translucent = Bit(f, 2.0);
	float untextured = Bit(f, 4.0);
	float nomip = Bit(f, 8.0);
	float4 t = lerp(tex2D(atlas, i.uv), tex2Dlod(atlas, float4(i.uv, 0.0, 0.0)), nomip).bgra;  // RGBA bytes in an ARGB texture
	float3 nrm = normalize(cross(ddy(i.rel), ddx(i.rel)));
	float3 n2 = nrm * nrm;
	float faceShade = n2.x * 0.6 + n2.y * 0.8 + n2.z * (nrm.z > 0.0 ? 1.0 : 0.5);
	float shade = i.lf.z < 0.0 ? faceShade : i.lf.z;
	float b = max(Curve(saturate(i.lf.x)), Curve(saturate(i.lf.y * lightParams.x)));
	float g = 1.0 - (1.0 - b) * (1.0 - b) * (1.0 - b) * (1.0 - b);
	b = lerp(b, g, lightParams.y);
	b = lightParams.z + (1.0 - lightParams.z) * b;
	float3 lit = t.rgb * i.color.rgb * (shade * b * lightParams.w);
	float alpha = translucent > 0.5 ? t.a * i.color.a : 1.0;
	clip(untextured > 0.5 ? 1.0 : (translucent > 0.5 ? 1.0 : t.a - 0.5));  // cutout: alpha test
	float4 c = float4(saturate(lit), alpha);
	return untextured > 0.5 ? i.color : c;
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
}
