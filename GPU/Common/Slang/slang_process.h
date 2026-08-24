// Ported from RetroArch slang frontend (gfx/drivers_shader/slang_process.h)
// for PPSSPP. GPLv3 licensed, see RetroArch project.

#pragma once

#include "slang_types.h"

// Compiles a single pass of a slang preset, producing GLSL for both stages
// (Vulkan 450 rules) plus the reflected resource bindings.
// The returned GLSL is ready to feed into PPSSPP's Draw::CreateShaderModule.
struct SlangPassCompiled {
	std::string vertexGLSL;
	std::string fragmentGLSL;
	pass_semantics_t semantics{};
	SlangFormat format = SlangFormat::UNKNOWN;
	std::string alias;
	bool feedback = false;
	~SlangPassCompiled();
};

// Preprocesses the shader file and merges any #pragma parameters into the preset.
bool SlangPreprocessParseParameters(const std::string &shaderPath, SlangPreset *preset);

bool SlangProcess(SlangPreset *preset, unsigned passNumber, SlangPassCompiled *out);

void SlangFreePassSemantics(pass_semantics_t *semantics);
