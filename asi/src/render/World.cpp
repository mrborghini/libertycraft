// See World.h. SDK-free: the device and the frame come from Render.cpp.
#define LC_MODULE "render"
#include "render/World.h"

#include "render/D3D9Util.h"
#include "render/RenderMath.h"
#include "render/Shaders.h"

#include "Config.h"
#include "Coords.h"
#include "Link.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace lc::render
{
	namespace
	{
		constexpr std::uint32_t kQuadsPerDraw = 16384;  // 65536 vertices: 16-bit indices
		constexpr std::uint32_t kAtlasMipCap = 5;       // 16-pixel sprites stay inside their cell
		constexpr float         kSectionRadius = 13.86f;  // half the diagonal of a 16-block cube

		struct Section
		{
			IDirect3DVertexBuffer9* vb{ nullptr };
			std::uint32_t           opaque{ 0 }, translucent{ 0 };  // vertices (quad corners when quads)
			std::uint32_t           bytes{ 0 };
			bool                    quads{ false };
			std::int32_t            sx{ 0 }, sy{ 0 }, sz{ 0 };
		};

		// Geometry Minecraft's entity renderer drew this frame, by texture.
		struct Mesh
		{
			std::vector<proto::RenBatch> batches;
			std::vector<Vertex>          verts;
			double                       origin[3]{};
		};

		IDirect3DDevice9*            device = nullptr;  // the one our resources belong to
		bool                         initTried = false;
		bool                         ready = false;
		IDirect3DVertexShader9*      vs = nullptr;
		IDirect3DPixelShader9*       ps = nullptr;
		IDirect3DVertexDeclaration9* decl = nullptr;
		IDirect3DIndexBuffer9*       quadIb = nullptr;
		IDirect3DTexture9*           atlas = nullptr;
		std::uint32_t                atlasW = 0, atlasH = 0, atlasLevels = 0;

		std::unordered_map<std::uint64_t, Section>            sections;
		std::unordered_map<std::uint32_t, IDirect3DTexture9*> entityTextures;
		Mesh                                                  avatar, scene;
		proto::WorldEntities                                  entities{};
		EntityBuilder                                         builder;
		SectionMesh                                           scratch;
		std::vector<std::uint8_t>                             mipA, mipB;
		std::vector<std::pair<float, const Section*>>         sorted;
		WorldStats                                            stats;
		std::uint64_t                                         sectionBytes = 0;

		double NowMs()
		{
			static const double freq = [] {
				LARGE_INTEGER f;
				::QueryPerformanceFrequency(&f);
				return double(f.QuadPart) / 1000.0;
			}();
			LARGE_INTEGER t;
			::QueryPerformanceCounter(&t);
			return double(t.QuadPart) / freq;
		}

		void ReleaseSection(Section& a_s)
		{
			SafeRelease(a_s.vb);
			sectionBytes -= a_s.bytes;
			a_s.bytes = 0;
		}

		void ClearSections()
		{
			for (auto& [key, s] : sections) {
				ReleaseSection(s);
			}
			sections.clear();
			sectionBytes = 0;
		}

		void ClearEntities()
		{
			for (auto& [id, t] : entityTextures) {
				SafeRelease(t);
			}
			entityTextures.clear();
			avatar.batches.clear();
			scene.batches.clear();
		}

		// A new device (or none): forget everything that belonged to the old one. Its resources
		// went with it, so the pointers are dropped, not released.
		void Forget()
		{
			vs = nullptr;
			ps = nullptr;
			decl = nullptr;
			quadIb = nullptr;
			atlas = nullptr;
			atlasW = atlasH = atlasLevels = 0;
			sections.clear();
			sectionBytes = 0;
			entityTextures.clear();
			avatar.batches.clear();
			scene.batches.clear();
			ready = false;
			initTried = false;
		}

		bool Init(IDirect3DDevice9* a_device)
		{
			if (device != a_device) {
				if (device) {
					LC_LOG("Direct3D device changed (%p -> %p): dropping Minecraft's GPU data", static_cast<void*>(device), static_cast<void*>(a_device));
					Forget();
				}
				device = a_device;
			}
			if (ready || initTried) {
				return ready;
			}
			initTried = true;
			D3DCAPS9 caps{};
			if (SUCCEEDED(a_device->GetDeviceCaps(&caps))) {
				LC_LOG("device caps: VS %u.%u PS %u.%u, max texture %ux%u, UBYTE4N decl %s, max primitives %u", D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion),
					D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion), D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion), D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion),
					caps.MaxTextureWidth, caps.MaxTextureHeight, (caps.DeclTypes & D3DDTCAPS_UBYTE4N) ? "yes" : "NO", caps.MaxPrimitiveCount);
			}
			std::vector<DWORD> vsCode, psCode;
			if (!CompileShader(shaders::kWorld, "libertycraft_world", "VSMain", "vs_3_0", vsCode) ||
				!CompileShader(shaders::kWorld, "libertycraft_world", "PSMain", "ps_3_0", psCode)) {
				return false;
			}
			if (FAILED(a_device->CreateVertexShader(vsCode.data(), &vs)) || FAILED(a_device->CreatePixelShader(psCode.data(), &ps))) {
				LC_LOG("ERROR: creating the world shaders failed");
				return false;
			}
			const D3DVERTEXELEMENT9 elements[] = {
				{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
				{ 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
				{ 0, 20, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
				{ 0, 24, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
				{ 0, 28, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
				D3DDECL_END(),
			};
			if (FAILED(a_device->CreateVertexDeclaration(elements, &decl))) {
				LC_LOG("ERROR: the RenVertex declaration failed");
				return false;
			}
			// (0 1 2) (0 2 3) per quad, shared by every quad-compressed section.
			const UINT ibBytes = kQuadsPerDraw * 6 * sizeof(std::uint16_t);
			void*      p = nullptr;
			if (FAILED(a_device->CreateIndexBuffer(ibBytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &quadIb, nullptr)) ||
				FAILED(quadIb->Lock(0, 0, &p, 0))) {
				LC_LOG("ERROR: the quad index buffer failed");
				return false;
			}
			auto* idx = static_cast<std::uint16_t*>(p);
			for (std::uint32_t q = 0; q < kQuadsPerDraw; ++q) {
				const auto b = static_cast<std::uint16_t>(q * 4);
				const std::uint16_t six[6] = { b, static_cast<std::uint16_t>(b + 1), static_cast<std::uint16_t>(b + 2), b, static_cast<std::uint16_t>(b + 2),
					static_cast<std::uint16_t>(b + 3) };
				std::memcpy(idx + q * 6, six, sizeof(six));
			}
			quadIb->Unlock();
			ready = true;
			LC_LOG("block renderer ready (vs_3_0/ps_3_0, managed buffers)");
			return true;
		}

		// ---- render ring messages -----------------------------------------------------------
		// Uploads one mip level of a rectangle of 4-byte pixels.
		bool UploadRect(IDirect3DTexture9* a_tex, UINT a_level, UINT a_x, UINT a_y, UINT a_w, UINT a_h, const std::uint8_t* a_src)
		{
			RECT           r{ LONG(a_x), LONG(a_y), LONG(a_x + a_w), LONG(a_y + a_h) };
			D3DLOCKED_RECT lr{};
			if (FAILED(a_tex->LockRect(a_level, &lr, &r, 0))) {
				return false;
			}
			CopyRows(lr.pBits, lr.Pitch, a_src, a_w * 4, a_h);
			a_tex->UnlockRect(a_level);
			return true;
		}

		// Level 0 from a_src, then levels 1..a_levels-1 by box filter (the bytes stay Minecraft's
		// RGBA order; the shader swizzles).
		void UploadWithMips(IDirect3DTexture9* a_tex, UINT a_x, UINT a_y, UINT a_w, UINT a_h, const std::uint8_t* a_src, std::uint32_t a_levels)
		{
			UploadRect(a_tex, 0, a_x, a_y, a_w, a_h, a_src);
			const std::uint8_t* prev = a_src;
			UINT                w = a_w, h = a_h;
			for (std::uint32_t level = 1; level < a_levels; ++level) {
				const UINT nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
				auto&      dst = (level & 1) ? mipA : mipB;
				dst.resize(std::size_t(nw) * nh * 4);
				Downsample(prev, w, h, w * 4, dst.data(), nw * 4);
				UploadRect(a_tex, level, a_x >> level, a_y >> level, nw, nh, dst.data());
				prev = dst.data();
				w = nw;
				h = nh;
			}
		}

		void OnAtlas(const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (a_bytes < sizeof(proto::RenAtlas)) {
				return;
			}
			const auto* hdr = reinterpret_cast<const proto::RenAtlas*>(a_data);
			if (!hdr->width || !hdr->height || hdr->width > 16384 || hdr->height > 16384 ||
				a_bytes < sizeof(proto::RenAtlas) + std::uint64_t(hdr->width) * hdr->height * 4) {
				LC_LOG("atlas message malformed (%ux%u, %u bytes)", hdr->width, hdr->height, a_bytes);
				return;
			}
			const double t0 = NowMs();
			SafeRelease(atlas);
			atlasLevels = MipLevels(hdr->width, hdr->height, kAtlasMipCap);
			if (FAILED(device->CreateTexture(hdr->width, hdr->height, atlasLevels, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &atlas, nullptr))) {
				LC_LOG("ERROR: atlas texture %ux%u (%u levels) failed", hdr->width, hdr->height, atlasLevels);
				atlasW = atlasH = 0;
				return;
			}
			atlasW = hdr->width;
			atlasH = hdr->height;
			UploadWithMips(atlas, 0, 0, atlasW, atlasH, a_data + sizeof(proto::RenAtlas), atlasLevels);
			LC_LOG("received Minecraft's block atlas %ux%u (%u mip levels, %.1f ms)", atlasW, atlasH, atlasLevels, NowMs() - t0);
		}

		// An animated sprite's current frame (water, lava, fire, ...), with its own mips.
		void OnAtlasRegion(const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (!atlas || a_bytes < sizeof(proto::RenAtlasRegion)) {
				return;
			}
			const auto* hdr = reinterpret_cast<const proto::RenAtlasRegion*>(a_data);
			if (!hdr->width || !hdr->height || hdr->x + hdr->width > atlasW || hdr->y + hdr->height > atlasH ||
				a_bytes < sizeof(proto::RenAtlasRegion) + std::uint64_t(hdr->width) * hdr->height * 4) {
				return;
			}
			UploadWithMips(atlas, hdr->x, hdr->y, hdr->width, hdr->height, a_data + sizeof(proto::RenAtlasRegion),
				RegionMipLevels(hdr->x, hdr->y, hdr->width, hdr->height, atlasLevels));
		}

		void OnTexture(const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (a_bytes < sizeof(proto::RenTexture)) {
				return;
			}
			const auto* hdr = reinterpret_cast<const proto::RenTexture*>(a_data);
			if (!hdr->width || !hdr->height || hdr->width > 4096 || hdr->height > 4096 ||
				a_bytes < sizeof(proto::RenTexture) + std::uint64_t(hdr->width) * hdr->height * 4) {
				return;
			}
			auto& tex = entityTextures[hdr->id];
			SafeRelease(tex);
			if (FAILED(device->CreateTexture(hdr->width, hdr->height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) {
				entityTextures.erase(hdr->id);
				return;
			}
			UploadRect(tex, 0, 0, 0, hdr->width, hdr->height, a_data + sizeof(proto::RenTexture));
			LC_LOG_EVERY(2000, "received Minecraft entity texture %u (%ux%u)", hdr->id, hdr->width, hdr->height);
		}

		void OnSection(const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			if (a_bytes < sizeof(proto::RenSection)) {
				return;
			}
			const auto* hdr = reinterpret_cast<const proto::RenSection*>(a_data);
			const auto  key = SectionKey(hdr->sx, hdr->sy, hdr->sz);
			if (auto it = sections.find(key); it != sections.end()) {
				ReleaseSection(it->second);
				sections.erase(it);
			}
			const std::uint32_t count = hdr->vertexCount - hdr->vertexCount % 3;
			if (count == 0 || a_bytes < sizeof(proto::RenSection) + std::uint64_t(count) * sizeof(Vertex)) {
				return;
			}
			BuildSectionMesh(reinterpret_cast<const Vertex*>(a_data + sizeof(proto::RenSection)), count, scratch);
			if (scratch.vertices.empty()) {
				return;
			}
			Section    s;
			const UINT bytes = static_cast<UINT>(scratch.vertices.size() * sizeof(Vertex));
			void*      p = nullptr;
			if (FAILED(device->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &s.vb, nullptr)) || FAILED(s.vb->Lock(0, 0, &p, 0))) {
				SafeRelease(s.vb);
				LC_LOG_EVERY(2000, "ERROR: vertex buffer for section %d %d %d (%u bytes) failed", hdr->sx, hdr->sy, hdr->sz, bytes);
				return;
			}
			std::memcpy(p, scratch.vertices.data(), bytes);
			s.vb->Unlock();
			s.opaque = scratch.opaque;
			s.translucent = scratch.translucent;
			s.quads = scratch.quads;
			s.bytes = bytes;
			s.sx = hdr->sx;
			s.sy = hdr->sy;
			s.sz = hdr->sz;
			sectionBytes += bytes;
			sections.emplace(key, s);
		}

		// RenAvatar / RenScene: batches + vertices, kept on the CPU and drawn with DrawPrimitiveUP.
		void OnMesh(Mesh& a_mesh, const std::uint8_t* a_data, std::uint32_t a_bytes, bool a_hasOrigin)
		{
			a_mesh.batches.clear();
			const std::size_t head = a_hasOrigin ? sizeof(proto::RenScene) : sizeof(proto::RenAvatar);
			if (a_bytes < head) {
				return;
			}
			std::uint32_t batchCount, vertexCount;
			if (a_hasOrigin) {
				const auto* hdr = reinterpret_cast<const proto::RenScene*>(a_data);
				a_mesh.origin[0] = hdr->originX;
				a_mesh.origin[1] = hdr->originY;
				a_mesh.origin[2] = hdr->originZ;
				batchCount = hdr->batchCount;
				vertexCount = hdr->vertexCount;
			} else {
				const auto* hdr = reinterpret_cast<const proto::RenAvatar*>(a_data);
				batchCount = hdr->batchCount;
				vertexCount = hdr->vertexCount;
			}
			const std::uint64_t need = head + std::uint64_t(batchCount) * sizeof(proto::RenBatch) + std::uint64_t(vertexCount) * sizeof(Vertex);
			if (batchCount == 0 || vertexCount == 0 || a_bytes < need) {
				return;
			}
			const auto* batches = reinterpret_cast<const proto::RenBatch*>(a_data + head);
			const auto* verts = reinterpret_cast<const Vertex*>(batches + batchCount);
			a_mesh.verts.assign(verts, verts + vertexCount);
			for (std::uint32_t b = 0; b < batchCount; ++b) {
				if (batches[b].count >= 3 && std::uint64_t(batches[b].first) + batches[b].count <= vertexCount) {
					a_mesh.batches.push_back(batches[b]);
				}
			}
		}

		// ---- drawing --------------------------------------------------------------------------
		void SetOffset(const double a_mcOrigin[3], const double a_cam[3], float a_out[4] = nullptr)
		{
			const GtaVec g = McToGta(a_mcOrigin[0], a_mcOrigin[1], a_mcOrigin[2]);
			const float  off[4] = { float(g.x - a_cam[0]), float(g.y - a_cam[1]), float(g.z - a_cam[2]), 0.0f };
			device->SetVertexShaderConstantF(4, off, 1);
			if (a_out) {
				std::memcpy(a_out, off, sizeof(off));
			}
		}

		void DrawPart(const Section& a_s, std::uint32_t a_first, std::uint32_t a_count)
		{
			if (!a_count) {
				return;
			}
			++stats.drawCalls;
			if (!a_s.quads) {
				device->DrawPrimitive(D3DPT_TRIANGLELIST, a_first, a_count / 3);
				stats.triangles += a_count / 3;
				return;
			}
			const std::uint32_t quads = a_count / 4;
			for (std::uint32_t q = 0; q < quads; q += kQuadsPerDraw) {
				const std::uint32_t n = std::min(kQuadsPerDraw, quads - q);
				device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, INT(a_first + q * 4), 0, n * 4, 0, n * 2);
				stats.triangles += n * 2;
			}
		}

		void DrawUp(const std::vector<Vertex>& a_v, std::size_t a_first, std::size_t a_count, D3DPRIMITIVETYPE a_type = D3DPT_TRIANGLELIST)
		{
			const UINT prims = a_type == D3DPT_LINELIST ? UINT(a_count / 2) : UINT(a_count / 3);
			if (!prims) {
				return;
			}
			device->DrawPrimitiveUP(a_type, prims, a_v.data() + a_first, sizeof(Vertex));
			++stats.drawCalls;
			stats.triangles += prims;
		}

		void DrawMesh(const Mesh& a_mesh, const double a_mcOrigin[3], const double a_cam[3], bool a_blended)
		{
			if (a_mesh.batches.empty()) {
				return;
			}
			SetOffset(a_mcOrigin, a_cam);
			for (const auto& b : a_mesh.batches) {
				if (((b.flags & 1) != 0) != a_blended) {
					continue;
				}
				IDirect3DTexture9* tex = atlas;
				if (b.texture != 0) {
					const auto it = entityTextures.find(b.texture);
					if (it == entityTextures.end()) {
						continue;
					}
					tex = it->second;
				}
				device->SetTexture(0, tex);
				DrawUp(a_mesh.verts, b.first, b.count);
			}
			device->SetTexture(0, atlas);
		}

		void SetCommonState(const FrameSnapshot& a_f, const TargetInfo& a_t)
		{
			auto* d = device;
			d->SetVertexShader(vs);
			d->SetPixelShader(ps);
			d->SetVertexDeclaration(decl);
			float cols[16];
			MatrixColumns(a_f.clip, cols);
			d->SetVertexShaderConstantF(0, cols, 4);
			const float n = std::max(a_f.nearZ, 1e-3f), f = std::max(a_f.farZ, n * 2.0f);
			const float depth[4] = { n, 1.0f / std::log2(f / n), (a_f.flags & kFrameLogDepth) ? 1.0f : 0.0f, 0.0f };
			d->SetVertexShaderConstantF(5, depth, 1);
			const float light[4] = { a_f.dayFactor, 0.5f, 0.04f, a_f.exposure };
			d->SetPixelShaderConstantF(0, light, 1);

			D3DVIEWPORT9 vp{ 0, 0, a_t.width, a_t.height, 0.0f, 1.0f };
			d->SetViewport(&vp);
			d->SetRenderState(D3DRS_ZENABLE, a_t.depthTest ? D3DZB_TRUE : D3DZB_FALSE);
			d->SetRenderState(D3DRS_ZWRITEENABLE, a_t.depthTest ? TRUE : FALSE);
			d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
			d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
			d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
			d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
			d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
			d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
			d->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
			d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
			d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
			d->SetRenderState(D3DRS_FOGENABLE, FALSE);
			d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
			d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
			d->SetRenderState(D3DRS_DEPTHBIAS, 0);
			d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
			d->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
			d->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, FALSE);
			d->SetRenderState(D3DRS_WRAP0, 0);
			d->SetRenderState(D3DRS_CLIPPING, TRUE);
			d->SetTexture(0, atlas);
			d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
			d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
			d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
			d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
			d->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
			d->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
			d->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 1);
			d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
			d->SetIndices(quadIb);
		}
	}

	World& World::Get()
	{
		static World world;
		return world;
	}

	void World::Drain(IDirect3DDevice9* a_device)
	{
		if (!a_device || !Init(a_device)) {
			// Without a renderer still keep the ring moving.
			stats.drainedBytes += Link::Get().DrainRender([](std::uint32_t, const std::uint8_t*, std::uint32_t) {}, 64ull << 20);
			return;
		}
		const double t0 = NowMs();
		stats.drainedBytes += Link::Get().DrainRender(
			[](std::uint32_t a_type, const std::uint8_t* a_data, std::uint32_t a_bytes) {
				if (a_type < 12) {
					++stats.messages[a_type];
					stats.messageBytes[a_type] += a_bytes;
				} else {
					++stats.otherMessages;
				}
				switch (a_type) {
				case proto::kRenAtlas:
					OnAtlas(a_data, a_bytes);
					break;
				case proto::kRenAtlasRegion:
					OnAtlasRegion(a_data, a_bytes);
					break;
				case proto::kRenSection:
					OnSection(a_data, a_bytes);
					break;
				case proto::kRenClearAll:
					LC_LOG("Minecraft cleared its world (%zu sections dropped)", sections.size());
					ClearSections();
					ClearEntities();
					break;
				case proto::kRenTexture:
					OnTexture(a_data, a_bytes);
					break;
				case proto::kRenAvatar:
					OnMesh(avatar, a_data, a_bytes, false);
					break;
				case proto::kRenScene:
					OnMesh(scene, a_data, a_bytes, true);
					break;
				default:
					break;  // kRenLights, kRenSolids, kRenDug, kRenRagdoll: not used yet (counted)
				}
			},
			64ull << 20);
		stats.drainMs += NowMs() - t0;
	}

	void World::Draw(IDirect3DDevice9* a_device, const FrameSnapshot& a_f, const TargetInfo& a_t)
	{
		if (!ready || a_device != device || !(a_f.flags & kFrameCameraValid) || !a_t.width || !a_t.height) {
			return;
		}
		const double t0 = NowMs();
		++stats.frames;
		SetCommonState(a_f, a_t);
		const double* cam = a_f.camPos;
		Mat4          clip;
		std::memcpy(clip.m, a_f.clip, sizeof(clip.m));
		const Frustum frustum = Frustum::FromClip(clip);
		const double  maxDist = std::max(64.0, double(a_f.farZ)) + 16.0;

		// Opaque and cutout blocks.
		sorted.clear();
		if (atlas) {
			for (const auto& [key, s] : sections) {
				const double o[3] = { s.sx * 16.0, s.sy * 16.0, s.sz * 16.0 };
				const GtaVec g = McToGta(o[0], o[1], o[2]);
				const float  c[3] = { float(g.x - cam[0]) + 8.0f, float(g.y - cam[1]) - 8.0f, float(g.z - cam[2]) + 8.0f };
				const float  d2 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
				if (d2 > maxDist * maxDist || !frustum.SphereVisible(c, kSectionRadius)) {
					++stats.sectionsCulled;
					continue;
				}
				if (s.translucent) {
					sorted.emplace_back(d2, &s);
				}
				if (!s.opaque) {
					continue;
				}
				++stats.sectionsDrawn;
				SetOffset(o, cam);
				device->SetStreamSource(0, s.vb, 0, sizeof(Vertex));
				DrawPart(s, 0, s.opaque);
			}
		}

		// Minecraft's world things around an integer origin near the camera, and its entities.
		const McVec  camMc = GtaToMc(cam[0], cam[1], cam[2]);
		const double origin[3] = { std::floor(camMc.x), std::floor(camMc.y), std::floor(camMc.z) };
		builder.Clear();
		if (Link::Get().ReadWorldEntities(entities)) {
			builder.Build(entities, origin);
		}
		if (atlas && !builder.solid.empty()) {
			SetOffset(origin, cam);
			DrawUp(builder.solid, 0, builder.solid.size());
		}
		if (a_f.flags & kFrameAvatar) {
			DrawMesh(avatar, a_f.feet, cam, false);
		}
		DrawMesh(scene, scene.origin, cam, false);

		// Translucent: water, stained glass, ice, back to front; then cracks and the outline.
		device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);  // Minecraft's faces wind counter-clockwise seen from the front
		std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		for (const auto& [d2, s] : sorted) {
			const double o[3] = { s->sx * 16.0, s->sy * 16.0, s->sz * 16.0 };
			SetOffset(o, cam);
			device->SetStreamSource(0, s->vb, 0, sizeof(Vertex));
			DrawPart(*s, s->opaque, s->translucent);
		}
		device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		if (a_f.flags & kFrameAvatar) {
			DrawMesh(avatar, a_f.feet, cam, true);
		}
		DrawMesh(scene, scene.origin, cam, true);
		if (atlas && !builder.cracks.empty()) {
			SetOffset(origin, cam);
			DrawUp(builder.cracks, 0, builder.cracks.size());
		}
		if (!builder.outline.empty()) {
			SetOffset(origin, cam);
			DrawUp(builder.outline, 0, builder.outline.size(), D3DPT_LINELIST);
		}
		const double ms = NowMs() - t0;
		stats.drawMs += ms;
		stats.maxDrawMs = std::max(stats.maxDrawMs, ms);
	}

	WorldStats World::TakeStats()
	{
		WorldStats out = stats;
		out.sections = static_cast<std::uint32_t>(sections.size());
		out.sectionBytes = sectionBytes;
		out.atlas = atlas != nullptr;
		out.atlasW = atlasW;
		out.atlasH = atlasH;
		stats = WorldStats{};
		return out;
	}
}
