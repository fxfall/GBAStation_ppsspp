// Ported from RetroArch slang frontend (gfx/drivers_shader/) for PPSSPP.
// GPLv3 licensed, see RetroArch project.

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "Common/Common.h"
#include "Common/GPU/Shader.h"

// Texture formats used by slang shaders (subset of Vulkan formats).
enum class SlangFormat : int {
	UNKNOWN = 0,

	// 8-bit
	R8_UNORM,
	R8_UINT,
	R8_SINT,
	R8G8_UNORM,
	R8G8_UINT,
	R8G8_SINT,
	R8G8B8A8_UNORM,
	R8G8B8A8_UINT,
	R8G8B8A8_SINT,
	R8G8B8A8_SRGB,

	// 10-bit
	A2B10G10R10_UNORM_PACK32,
	A2B10G10R10_UINT_PACK32,

	// 16-bit
	R16_UINT,
	R16_SINT,
	R16_SFLOAT,
	R16G16_UINT,
	R16G16_SINT,
	R16G16_SFLOAT,
	R16G16B16A16_UINT,
	R16G16B16A16_SINT,
	R16G16B16A16_SFLOAT,

	// 32-bit
	R32_UINT,
	R32_SINT,
	R32_SFLOAT,
	R32G32_UINT,
	R32G32_SINT,
	R32G32_SFLOAT,
	R32G32B32A32_UINT,
	R32G32B32A32_SINT,
	R32G32B32A32_SFLOAT,

	MAX,
};

enum class SlangWrapType {
	BORDER,
	REPEAT,
	MIRRORED_REPEAT,
	EDGE,
};

// Textures with built-in meaning.
enum SlangTextureSemantic {
	// The input texture to the filter chain. Canonical name: "Original".
	SLANG_TEXTURE_SEMANTIC_ORIGINAL = 0,
	// The output from pass N - 1 if executing pass N, or ORIGINAL if pass #0.
	// Canonical name: "Source".
	SLANG_TEXTURE_SEMANTIC_SOURCE = 1,
	// The original inputs with a history back in time.
	// Canonical name: "OriginalHistory#".
	SLANG_TEXTURE_SEMANTIC_ORIGINAL_HISTORY = 2,
	// The output from pass #N. Canonical name: "PassOutput#".
	SLANG_TEXTURE_SEMANTIC_PASS_OUTPUT = 3,
	// The output from pass #N, one frame ago. Canonical name: "PassFeedback#".
	SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK = 4,
	// Inputs from static textures, defined by the user (LUTs).
	SLANG_TEXTURE_SEMANTIC_USER = 5,

	SLANG_NUM_TEXTURE_SEMANTICS,
	SLANG_INVALID_TEXTURE_SEMANTIC = -1,
};

enum SlangSemantic {
	SLANG_SEMANTIC_MVP = 0,              // mat4
	SLANG_SEMANTIC_OUTPUT = 1,           // vec4, viewport size of current pass
	SLANG_SEMANTIC_FINAL_VIEWPORT = 2,   // vec4, viewport size of final pass
	SLANG_SEMANTIC_FRAME_COUNT = 3,      // uint
	SLANG_SEMANTIC_FRAME_DIRECTION = 4,  // int
	SLANG_SEMANTIC_FRAME_TIME_DELTA = 5, // uint
	SLANG_SEMANTIC_ORIGINAL_FPS = 6,     // float
	SLANG_SEMANTIC_ROTATION = 7,         // uint
	SLANG_SEMANTIC_CORE_ASPECT = 8,      // float
	SLANG_SEMANTIC_CORE_ASPECT_ROT = 9,  // float
	SLANG_SEMANTIC_TOTAL_SUBFRAMES = 10, // uint
	SLANG_SEMANTIC_CURRENT_SUBFRAME = 11,// uint
	SLANG_NUM_BASE_SEMANTICS,

	SLANG_SEMANTIC_FLOAT_PARAMETER = 12, // float, user defined parameter, arrayed

	SLANG_NUM_SEMANTICS,
	SLANG_INVALID_SEMANTIC = -1,
};

enum SlangStage {
	SLANG_STAGE_VERTEX_MASK = 1 << 0,
	SLANG_STAGE_FRAGMENT_MASK = 1 << 1,
};

enum SlangConstantBuffer {
	SLANG_CBUFFER_UBO = 0,
	SLANG_CBUFFER_PC,
	SLANG_CBUFFER_MAX,
};

// Vulkan maximum texture bindings inside shader. D3D11 has hard limit of 16.
#define SLANG_NUM_BINDINGS 16
#define SLANG_CBUFFER_MAX 2

struct slang_texture_semantic_map {
	enum SlangTextureSemantic semantic;
	unsigned index;
};

struct slang_semantic_map {
	enum SlangSemantic semantic;
	unsigned index;
};

struct uniform_sem_t {
	void *data;      // For base semantics: semantic id (uintptr). For parameters: (uintptr)(index + 0x100).
	unsigned size;
	unsigned offset;
	char id[64];
};

struct texture_sem_t {
	SlangTextureSemantic semantic;
	unsigned index;       // Which instance of the semantic (array index).
	SlangWrapType wrap;
	unsigned filter;      // 0 = linear, 1 = nearest.
	unsigned stage_mask;
	unsigned binding;     // Remapped binding (1..MAX_TEXTURE_SLOTS+1).
	char id[64];
};

struct cbuffer_sem_t {
	unsigned stage_mask;
	unsigned binding;
	unsigned size;
	int uniform_count;
	uniform_sem_t *uniforms;
};

struct pass_semantics_t {
	int texture_count;
	texture_sem_t *textures;
	cbuffer_sem_t cbuffers[SLANG_CBUFFER_MAX];
	SlangFormat format;
};

struct SlangParameter {
	char id[64];
	char desc[64];
	float initial;
	float minimum;
	float maximum;
	float step;
	float current;
};

struct SlangPass {
	std::string source;    // path to the .slang file
	std::string alias;
	bool srgbFbo = false;
	bool fpFbo = false;
	bool feedback = false;
	SlangWrapType wrap = SlangWrapType::EDGE;
	bool filterLinear = true;
};

struct SlangLut {
	std::string id;
	std::string path;
	SlangWrapType wrap = SlangWrapType::EDGE;
	bool filterLinear = true;
};

// Simplified equivalent of RetroArch's struct video_shader, only keeping
// what the slang frontend needs.
struct SlangPreset {
	std::vector<SlangPass> passes;
	std::vector<SlangParameter> parameters;
	std::vector<SlangLut> luts;
	int historySize = 0;
};

struct glslang_parameter {
	std::string id;
	std::string desc;
	float initial;
	float minimum;
	float maximum;
	float step;
};

struct glslang_meta {
	std::vector<glslang_parameter> parameters;
	std::string name;
	SlangFormat rt_format;

	glslang_meta() {
		rt_format = SlangFormat::UNKNOWN;
	}
};

struct glslang_output {
	std::vector<uint32_t> vertex;
	std::vector<uint32_t> fragment;
	glslang_meta meta;
};
