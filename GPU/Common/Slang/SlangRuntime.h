// Runtime support for slang shaders: filling the reflected UBO and mapping
// texture semantics to PPSSPP render targets.
// GPLv3 licensed (ported frontend), see RetroArch project.

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#include "Common/GPU/thin3d.h"
#include "slang_types.h"

// Texture size slots, keyed by (semantic * 8 + index) like the reflection encoding.
#define SLANG_TEXTURE_SIZE_SLOTS (SLANG_NUM_TEXTURE_SEMANTICS * 8)

// Context needed to fill in the slang UBO each frame.
struct SlangRuntimeContext {
	int passWidth = 0;          // size of the current pass output
	int passHeight = 0;
	int finalWidth = 0;         // size of the final pass output
	int finalHeight = 0;
	int originalWidth = 480;    // original (game) resolution
	int originalHeight = 272;
	float frameCount = 0.0f;    // total frames
	float frameTimeDelta = 0.0f;// seconds since last frame
	float fps = 60.0f;
	float rotation = 0.0f;
	float aspect = 480.0f / 272.0f;
	// Per-texture-semantic sizes, filled by the renderer.
	float textureSizes[SLANG_TEXTURE_SIZE_SLOTS][4] = {};
	// Parameter values, indexed by preset parameter index.
	const float *paramValues = nullptr;
	int numParams = 0;
};

// Fills the std140 UBO described by the pass semantics from the runtime context.
// uboSize must be at least cbuffers[SLANG_CBUFFER_UBO].size.
void SlangFillUniformBuffer(const pass_semantics_t &semantics, const SlangRuntimeContext &ctx, uint8_t *ubo, size_t uboSize);

// Returns the texture slot (0..MAX_TEXTURE_SLOTS-1) for a remapped shader binding.
inline int SlangBindingToSlot(unsigned binding) {
	return (int)binding - 1;
}
