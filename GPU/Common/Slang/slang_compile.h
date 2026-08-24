// Ported from RetroArch slang frontend (gfx/drivers_shader/glslang_util.*) for PPSSPP.
// GPLv3 licensed, see RetroArch project.

#pragma once

#include <string>
#include <vector>

#include "slang_types.h"

// Reads a shader file and outputs its contents as lines.
// Handles '#include' statements by recursively parsing included files and appending their contents.
// If rootFile, expects the first line to be a valid '#version' string.
bool SlangReadShaderFile(const std::string &path, std::vector<std::string> *output, bool rootFile, bool isOptional);

// Splits combined source into the GLSL for one stage, filtering #pragma stage blocks.
std::string SlangBuildStageSource(const std::vector<std::string> &lines, const char *stage);

// Parses #pragma name/parameter/format meta info from a file.
bool SlangParseMeta(const std::vector<std::string> &lines, glslang_meta *meta);

// Compiles one stage of GLSL (Vulkan rules) into SPIR-V. Uses PPSSPP's GLSLtoSPV.
bool SlangCompileStageSpirv(bool vertex, const std::string &source, std::vector<uint32_t> *spirv, std::string *errorMessage);

const char *SlangFormatToString(SlangFormat fmt);
SlangFormat SlangFindFormat(const std::string &fmt);

unsigned SlangNumMiplevels(unsigned width, unsigned height);
