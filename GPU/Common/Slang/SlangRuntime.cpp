// Runtime support for slang shaders: filling the reflected UBO and mapping
// texture semantics to PPSSPP render targets.
// GPLv3 licensed (ported frontend), see RetroArch project.

#include "SlangRuntime.h"

#include <cstring>

#include "Common/Log.h"

static void WriteFloat(float *dst, size_t count, float value) {
	for (size_t i = 0; i < count; i++)
		dst[i] = value;
}

void SlangFillUniformBuffer(const pass_semantics_t &semantics, const SlangRuntimeContext &ctx, uint8_t *ubo, size_t uboSize) {
	memset(ubo, 0, uboSize);
	const cbuffer_sem_t &cb = semantics.cbuffers[SLANG_CBUFFER_UBO];
	if (cb.uniform_count <= 0 || cb.size == 0)
		return;

	for (int i = 0; i < cb.uniform_count; i++) {
		const uniform_sem_t &u = cb.uniforms[i];
		uintptr_t id = (uintptr_t)u.data;

		if (id >= 0x200) {
			// Texture size uniform: 0x200 + semantic * 8 + index.
			uintptr_t sem = (id - 0x200) / 8;
			uintptr_t index = (id - 0x200) % 8;
			if (sem < SLANG_NUM_TEXTURE_SEMANTICS && index < 8) {
				float w = ctx.textureSizes[sem * 8 + index][0];
				float h = ctx.textureSizes[sem * 8 + index][1];
				float out[4] = { w, h, w > 0.0f ? 1.0f / w : 0.0f, h > 0.0f ? 1.0f / h : 0.0f };
				if (u.offset + sizeof(out) <= uboSize)
					memcpy(ubo + u.offset, out, sizeof(out));
			}
			continue;
		}

		if (id >= 0x100) {
			// User parameter: 0x100 + index.
			uintptr_t paramIndex = id - 0x100;
			float value = 0.0f;
			if (paramIndex < (uintptr_t)ctx.numParams)
				value = ctx.paramValues[paramIndex];
			if (u.offset + sizeof(float) <= uboSize)
				memcpy(ubo + u.offset, &value, sizeof(float));
			continue;
		}

		if (id >= SLANG_NUM_BASE_SEMANTICS)
			continue;

		switch ((SlangSemantic)id) {
		case SLANG_SEMANTIC_MVP: {
			// Identity MVP: vertices are already in NDC.
			static const float identity[16] = {
				1, 0, 0, 0,
				0, 1, 0, 0,
				0, 0, 1, 0,
				0, 0, 0, 1,
			};
			if (u.offset + sizeof(identity) <= uboSize)
				memcpy(ubo + u.offset, identity, sizeof(identity));
			break;
		}
		case SLANG_SEMANTIC_OUTPUT: {
			float out[4] = { (float)ctx.passWidth, (float)ctx.passHeight, 1.0f / ctx.passWidth, 1.0f / ctx.passHeight };
			if (u.offset + sizeof(out) <= uboSize)
				memcpy(ubo + u.offset, out, sizeof(out));
			break;
		}
		case SLANG_SEMANTIC_FINAL_VIEWPORT: {
			float out[4] = { (float)ctx.finalWidth, (float)ctx.finalHeight, 1.0f / ctx.finalWidth, 1.0f / ctx.finalHeight };
			if (u.offset + sizeof(out) <= uboSize)
				memcpy(ubo + u.offset, out, sizeof(out));
			break;
		}
		case SLANG_SEMANTIC_FRAME_COUNT: {
			uint32_t fc = (uint32_t)ctx.frameCount;
			if (u.offset + sizeof(fc) <= uboSize)
				memcpy(ubo + u.offset, &fc, sizeof(fc));
			break;
		}
		case SLANG_SEMANTIC_FRAME_DIRECTION: {
			int32_t dir = 1;
			if (u.offset + sizeof(dir) <= uboSize)
				memcpy(ubo + u.offset, &dir, sizeof(dir));
			break;
		}
		case SLANG_SEMANTIC_FRAME_TIME_DELTA: {
			uint32_t delta = (uint32_t)(ctx.frameTimeDelta * 1000.0f);
			if (u.offset + sizeof(delta) <= uboSize)
				memcpy(ubo + u.offset, &delta, sizeof(delta));
			break;
		}
		case SLANG_SEMANTIC_ORIGINAL_FPS: {
			float fps = ctx.fps;
			if (u.offset + sizeof(fps) <= uboSize)
				memcpy(ubo + u.offset, &fps, sizeof(fps));
			break;
		}
		case SLANG_SEMANTIC_ROTATION: {
			uint32_t rot = (uint32_t)ctx.rotation;
			if (u.offset + sizeof(rot) <= uboSize)
				memcpy(ubo + u.offset, &rot, sizeof(rot));
			break;
		}
		case SLANG_SEMANTIC_CORE_ASPECT:
		case SLANG_SEMANTIC_CORE_ASPECT_ROT: {
			float aspect = ctx.aspect;
			if (u.offset + sizeof(aspect) <= uboSize)
				memcpy(ubo + u.offset, &aspect, sizeof(aspect));
			break;
		}
		case SLANG_SEMANTIC_TOTAL_SUBFRAMES: {
			uint32_t sub = 1;
			if (u.offset + sizeof(sub) <= uboSize)
				memcpy(ubo + u.offset, &sub, sizeof(sub));
			break;
		}
		case SLANG_SEMANTIC_CURRENT_SUBFRAME: {
			uint32_t sub = 0;
			if (u.offset + sizeof(sub) <= uboSize)
				memcpy(ubo + u.offset, &sub, sizeof(sub));
			break;
		}
		default:
			break;
		}
	}
}
