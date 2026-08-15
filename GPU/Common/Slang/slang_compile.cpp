// Ported from RetroArch slang frontend (gfx/drivers_shader/glslang_util.c
// and glslang_util_cxx.cpp) for PPSSPP.
// GPLv3 licensed, see RetroArch project.

#include "slang_compile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Common/File/FileUtil.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"

static bool StartsWith(const char *str, const char *prefix) {
	size_t len = strlen(prefix);
	return strncmp(str, prefix, len) == 0;
}

// Path resolve relative include: dir(shader_path) + include_file.
static bool ResolveIncludePath(const std::string &shaderPath, const std::string &includeFile, std::string *out) {
	// TODO: If the include is an absolute path or a known shader path, use it directly.
	size_t slash = shaderPath.find_last_of("/\\");
	if (slash == std::string::npos) {
		*out = includeFile;
	} else {
		*out = shaderPath.substr(0, slash + 1) + includeFile;
	}
	return true;
}

static std::string GetIncludeFile(const std::string &line) {
	size_t start = line.find('"');
	if (start == std::string::npos)
		return "";
	start++;
	size_t end = line.find('"', start);
	if (end == std::string::npos)
		return "";
	return line.substr(start, end - start);
}

bool SlangReadShaderFile(const std::string &path, std::vector<std::string> *output, bool rootFile, bool isOptional) {
	if (path.empty() || !output)
		return false;

	size_t lastSlash = path.find_last_of("/\\");
	std::string basename = lastSlash == std::string::npos ? path : path.substr(lastSlash + 1);
	if (basename.empty())
		return false;

	std::string buf;
	if (!File::ReadTextFileToString(Path(path), &buf)) {
		if (!isOptional)
			ERROR_LOG(Log::G3D, "[Slang] Failed to open shader file: \"%s\".\n", path.c_str());
		return false;
	}

	// Remove Windows '\r' chars if we encounter them.
	buf.erase(std::remove(buf.begin(), buf.end(), '\r'), buf.end());

	std::vector<std::string> lines;
	SplitString(buf, '\n', lines);
	if (lines.empty())
		return false;

	// If this is the 'parent' shader file and a slang file, ensure that first line is a VERSION string.
	if (rootFile && Path(path).GetFileExtension() == ".slang") {
		const std::string &line = lines[0];
		if (!StartsWith(line.c_str(), "#version ")) {
			ERROR_LOG(Log::G3D, "[Slang] First line of the shader must contain a valid #version string.\n");
			return false;
		}
		output->push_back(line);
		// Allows us to use #line to make dealing with shader errors easier.
		output->push_back("#extension GL_GOOGLE_cpp_style_line_directive : require");
	}

	// Add defines about supported retroarch features.
	output->push_back("#define _HAS_ORIGINALASPECT_UNIFORMS");
	output->push_back("#define _HAS_FRAMETIME_UNIFORMS");

	// At least VIM treats the first line as line #1, so offset everything by one.
	output->push_back(StringFromFormat("#line %u \"%s\"", rootFile ? 2 : 1, basename.c_str()));

	for (size_t i = rootFile ? 1 : 0; i < lines.size(); i++) {
		const std::string &line = lines[i];

		// Check for include statements.
		bool includeOptional = StartsWith(line.c_str(), "#pragma include_optional ");
		if (StartsWith(line.c_str(), "#include ") || includeOptional) {
			std::string includeFile = GetIncludeFile(line);
			if (includeFile.empty()) {
				ERROR_LOG(Log::G3D, "[Slang] Invalid include statement \"%s\".\n", line.c_str());
				return false;
			}

			std::string includePath;
			ResolveIncludePath(path, includeFile, &includePath);

			// Parse include file.
			if (!SlangReadShaderFile(includePath, output, false, includeOptional)) {
				if (!includeOptional) {
					return false;
				}
				INFO_LOG(Log::G3D, "[Slang] Optional include not found \"%s\".\n", includePath.c_str());
			}

			// After including a file, use line directive to pull it back to current file.
			output->push_back(StringFromFormat("#line %u \"%s\"", (unsigned)(i + 1), basename.c_str()));
		} else if (StartsWith(line.c_str(), "#endif") || StartsWith(line.c_str(), "#pragma")) {
			// #line seems to be ignored if preprocessor tests fail,
			// so we should reapply #line after each #endif.
			output->push_back(line);
			output->push_back(StringFromFormat("#line %u \"%s\"", (unsigned)(i + 2), basename.c_str()));
		} else {
			output->push_back(line);
		}
	}

	return true;
}

std::string SlangBuildStageSource(const std::vector<std::string> &lines, const char *stage) {
	if (lines.empty())
		return "";
	std::string str;
	str.reserve(lines.size() * 64);

	// Version header.
	str.append(lines[0]);
	str.append("\n");

	for (size_t i = 1; i < lines.size(); i++) {
		const std::string &line = lines[i];
		bool active = true;

		if (StartsWith(line.c_str(), "#pragma")) {
			// Identify 'stage' (fragment/vertex).
			if (StartsWith(line.c_str(), "#pragma stage ")) {
				if (stage && *stage) {
					std::string expected = "#pragma stage ";
					expected += stage;
					active = expected == line;
				}
			} else if (StartsWith(line.c_str(), "#pragma name ") || StartsWith(line.c_str(), "#pragma format ")) {
				// Ignore.
			} else if (active) {
				str.append(line);
			}
		} else if (active) {
			str.append(line);
		}

		str.append("\n");
	}

	return str;
}

bool SlangParseMeta(const std::vector<std::string> &lines, glslang_meta *meta) {
	char id[64] = {};
	char desc[64] = {};

	for (size_t i = 0; i < lines.size(); i++) {
		const std::string &line = lines[i];

		if (!StartsWith(line.c_str(), "#pragma"))
			continue;

		// Check for shader identifier.
		if (StartsWith(line.c_str(), "#pragma name ")) {
			if (!meta->name.empty()) {
				ERROR_LOG(Log::G3D, "[Slang] Trying to declare multiple names for file.\n");
				return false;
			}
			const char *str = line.c_str() + strlen("#pragma name ");
			while (*str == ' ')
				str++;
			meta->name = str;
		} else if (StartsWith(line.c_str(), "#pragma parameter ")) {
			float initial, minimum, maximum, step;
			int ret = sscanf(line.c_str(), "#pragma parameter %63s \"%63[^\"]\" %f %f %f %f", id, desc, &initial, &minimum, &maximum, &step);

			if (ret == 5) {
				step = 0.1f * (maximum - minimum);
				ret = 6;
			}

			if (ret == 6) {
				bool parameterFound = false;
				size_t parameterIndex = 0;
				for (size_t j = 0; j < meta->parameters.size(); j++) {
					if (meta->parameters[j].id == id) {
						parameterFound = true;
						parameterIndex = j;
						break;
					}
				}

				// Allow duplicate #pragma parameter, but only if they are exactly the same.
				if (parameterFound) {
					const glslang_parameter *parameter = &meta->parameters[parameterIndex];
					if (parameter->desc != desc || parameter->initial != initial || parameter->minimum != minimum ||
						parameter->maximum != maximum || parameter->step != step) {
						ERROR_LOG(Log::G3D, "[Slang] Duplicate parameters found for \"%s\", but arguments do not match.\n", id);
						return false;
					}
				} else {
					meta->parameters.push_back({ id, desc, initial, minimum, maximum, step });
				}
			} else {
				ERROR_LOG(Log::G3D, "[Slang] Invalid #pragma parameter line: \"%s\".\n", line.c_str());
				return false;
			}
		} else if (StartsWith(line.c_str(), "#pragma format ")) {
			if (meta->rt_format != SlangFormat::UNKNOWN) {
				ERROR_LOG(Log::G3D, "[Slang] Trying to declare format multiple times for file.\n");
				return false;
			}
			const char *str = line.c_str() + strlen("#pragma format ");
			while (*str == ' ')
				str++;
			meta->rt_format = SlangFindFormat(str);
			if (meta->rt_format == SlangFormat::UNKNOWN) {
				ERROR_LOG(Log::G3D, "[Slang] Failed to find format \"%s\".\n", str);
				return false;
			}
		}
	}

	return true;
}

bool SlangCompileStageSpirv(bool vertex, const std::string &source, std::vector<uint32_t> *spirv, std::string *errorMessage) {
	// Reuse PPSSPP's Vulkan GLSL->SPIR-V compiler (glslang), always compiled in.
	// Note: requires glslang::InitializeProcess() to have been called (done by the
	// graphics context init), and never finalized while we might still compile.
	return GLSLtoSPV(vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, source.c_str(), GLSLVariant::VULKAN, *spirv, errorMessage);
}

const char *SlangFormatToString(SlangFormat fmt) {
	static const char *slangFormats[] = {
		"UNKNOWN",

		"R8_UNORM", "R8_UINT", "R8_SINT",
		"R8G8_UNORM", "R8G8_UINT", "R8G8_SINT",
		"R8G8B8A8_UNORM", "R8G8B8A8_UINT", "R8G8B8A8_SINT", "R8G8B8A8_SRGB",
		"A2B10G10R10_UNORM_PACK32", "A2B10G10R10_UINT_PACK32",
		"R16_UINT", "R16_SINT", "R16_SFLOAT",
		"R16G16_UINT", "R16G16_SINT", "R16G16_SFLOAT",
		"R16G16B16A16_UINT", "R16G16B16A16_SINT", "R16G16B16A16_SFLOAT",
		"R32_UINT", "R32_SINT", "R32_SFLOAT",
		"R32G32_UINT", "R32G32_SINT", "R32G32_SFLOAT",
		"R32G32B32A32_UINT", "R32G32B32A32_SINT", "R32G32B32A32_SFLOAT",
	};
	if ((int)fmt < 0 || (int)fmt >= (int)ARRAY_SIZE(slangFormats))
		return "UNKNOWN";
	return slangFormats[(int)fmt];
}

SlangFormat SlangFindFormat(const std::string &fmt) {
#define FMT(x) if (fmt == #x) return SlangFormat::x
	FMT(R8_UNORM);
	FMT(R8_UINT);
	FMT(R8_SINT);
	FMT(R8G8_UNORM);
	FMT(R8G8_UINT);
	FMT(R8G8_SINT);
	FMT(R8G8B8A8_UNORM);
	FMT(R8G8B8A8_UINT);
	FMT(R8G8B8A8_SINT);
	FMT(R8G8B8A8_SRGB);
	FMT(A2B10G10R10_UNORM_PACK32);
	FMT(A2B10G10R10_UINT_PACK32);
	FMT(R16_UINT);
	FMT(R16_SINT);
	FMT(R16_SFLOAT);
	FMT(R16G16_UINT);
	FMT(R16G16_SINT);
	FMT(R16G16_SFLOAT);
	FMT(R16G16B16A16_UINT);
	FMT(R16G16B16A16_SINT);
	FMT(R16G16B16A16_SFLOAT);
	FMT(R32_UINT);
	FMT(R32_SINT);
	FMT(R32_SFLOAT);
	FMT(R32G32_UINT);
	FMT(R32G32_SINT);
	FMT(R32G32_SFLOAT);
	FMT(R32G32B32A32_UINT);
	FMT(R32G32B32A32_SINT);
	FMT(R32G32B32A32_SFLOAT);
#undef FMT
	return SlangFormat::UNKNOWN;
}

unsigned SlangNumMiplevels(unsigned width, unsigned height) {
	unsigned size = std::max(width, height);
	unsigned levels = 0;
	while (size) {
		levels++;
		size >>= 1;
	}
	return levels;
}
