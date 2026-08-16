// Ported from RetroArch slang frontend (gfx/drivers_shader/slang_process.cpp)
// for PPSSPP. GPLv3 licensed, see RetroArch project.

#include "slang_process.h"

#include <algorithm>
#include <cstring>
#include <exception>

#include "ext/SPIRV-Cross/spirv_glsl.hpp"
#include "ext/SPIRV-Cross/spirv_cross.hpp"
#include "ext/SPIRV-Cross/spirv.hpp"

#include "Common/Log.h"
#include "Common/StringUtils.h"

#include "slang_compile.h"
#include "slang_reflection.hpp"

// Max textures we can bind in PPSSPP's thin3d Vulkan backend (binding 1..MAX_TEXTURE_SLOTS).
static const int kMaxTextureBindings = 3;

static const char *GetSemanticName(const std::unordered_map<std::string, slang_semantic_map> *map, SlangSemantic semantic, unsigned index) {
	for (const auto &m : *map) {
		if (m.second.semantic == semantic && m.second.index == index)
			return m.first.c_str();
	}
	return "";
}

static bool SlangProcessReflection(
	const spirv_cross::Compiler *vsCompiler,
	const spirv_cross::Compiler *psCompiler,
	const spirv_cross::ShaderResources &vsResources,
	const spirv_cross::ShaderResources &psResources,
	SlangPreset *preset,
	unsigned passNumber,
	pass_semantics_t *out) {
	int semantic;
	unsigned i;
	std::vector<texture_sem_t> textures;
	std::vector<uniform_sem_t> uniforms[SLANG_CBUFFER_MAX];
	std::unordered_map<std::string, slang_texture_semantic_map> textureSemanticMap;
	std::unordered_map<std::string, slang_texture_semantic_map> textureSemanticUniformMap;

	for (i = 0; i <= passNumber; i++) {
		if (preset->passes[i].alias.empty())
			continue;

		const std::string &name = preset->passes[i].alias;

		if (!slang_set_unique_map(textureSemanticMap, name, slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_OUTPUT, i }))
			return false;
		if (!slang_set_unique_map(textureSemanticUniformMap, name + "Size", slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_OUTPUT, i }))
			return false;
		if (!slang_set_unique_map(textureSemanticMap, name + "Feedback", slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK, i }))
			return false;
		if (!slang_set_unique_map(textureSemanticUniformMap, name + "FeedbackSize", slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK, i }))
			return false;
	}

	for (i = 0; i < preset->luts.size(); i++) {
		if (!slang_set_unique_map(textureSemanticMap, preset->luts[i].id, slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_USER, i }))
			return false;
		if (!slang_set_unique_map(textureSemanticUniformMap, preset->luts[i].id + "Size", slang_texture_semantic_map{ SLANG_TEXTURE_SEMANTIC_USER, i }))
			return false;
	}

	std::unordered_map<std::string, slang_semantic_map> uniformSemanticMap;

	for (i = 0; i < preset->parameters.size(); i++) {
		if (!slang_set_unique_map(uniformSemanticMap, preset->parameters[i].id, slang_semantic_map{ SLANG_SEMANTIC_FLOAT_PARAMETER, i }))
			return false;
	}

	slang_reflection slReflection;
	slReflection.pass_number = passNumber;
	slReflection.texture_semantic_map = &textureSemanticMap;
	slReflection.texture_semantic_uniform_map = &textureSemanticUniformMap;
	slReflection.semantic_map = &uniformSemanticMap;

	if (!slang_reflect(*vsCompiler, *psCompiler, vsResources, psResources, &slReflection)) {
		ERROR_LOG(Log::G3D, "[Slang] Failed to reflect SPIR-V. Resource usage is inconsistent with expectations.\n");
		return false;
	}

	out->cbuffers[SLANG_CBUFFER_UBO].stage_mask = slReflection.ubo_stage_mask;
	out->cbuffers[SLANG_CBUFFER_UBO].binding = slReflection.ubo_binding;
	out->cbuffers[SLANG_CBUFFER_UBO].size = (unsigned)((slReflection.ubo_size + 0xF) & ~0xF);
	out->cbuffers[SLANG_CBUFFER_PC].stage_mask = slReflection.push_constant_stage_mask;
	out->cbuffers[SLANG_CBUFFER_PC].binding = slReflection.ubo_binding ? 0 : 1;
	out->cbuffers[SLANG_CBUFFER_PC].size = (unsigned)((slReflection.push_constant_size + 0xF) & ~0xF);

	for (semantic = 0; semantic < SLANG_NUM_BASE_SEMANTICS; semantic++) {
		slang_semantic_meta &src = slReflection.semantics[semantic];
		if (src.push_constant || src.uniform) {
			// Encode the semantic id in the data pointer - resolved at runtime.
			uniform_sem_t uniform = { (void *)(uintptr_t)semantic, src.num_components * (unsigned)sizeof(float) };
			SlangSemantic sem = (SlangSemantic)semantic;
			static const char *names[] = {
				"MVP", "OutputSize", "FinalViewportSize", "FrameCount", "FrameDirection",
				"FrameTimeDelta", "OriginalFPS", "Rotation", "OriginalAspect",
				"OriginalAspectRotated", "TotalSubFrames", "CurrentSubFrame",
			};
			int size = (int)(sizeof(names) / sizeof(*names));
			if (semantic < size)
				strncpy(uniform.id, names[sem], sizeof(uniform.id));
			else
				strncpy(uniform.id, GetSemanticName(slReflection.semantic_map, sem, 0), sizeof(uniform.id));

			if (src.push_constant) {
				uniform.offset = (unsigned)src.push_constant_offset;
				uniforms[SLANG_CBUFFER_PC].push_back(uniform);
			} else {
				uniform.offset = (unsigned)src.ubo_offset;
				uniforms[SLANG_CBUFFER_UBO].push_back(uniform);
			}
		}
	}

	for (i = 0; i < slReflection.semantic_float_parameters.size(); i++) {
		slang_semantic_meta &src = slReflection.semantic_float_parameters[i];

		if (src.push_constant || src.uniform) {
			uniform_sem_t uniform = { (void *)(uintptr_t)(i + 0x100), sizeof(float) };
			strncpy(uniform.id, GetSemanticName(slReflection.semantic_map, SLANG_SEMANTIC_FLOAT_PARAMETER, i), sizeof(uniform.id));

			if (src.push_constant) {
				uniform.offset = (unsigned)src.push_constant_offset;
				uniforms[SLANG_CBUFFER_PC].push_back(uniform);
			} else {
				uniform.offset = (unsigned)src.ubo_offset;
				uniforms[SLANG_CBUFFER_UBO].push_back(uniform);
			}
		}
	}

	for (semantic = 0; semantic < SLANG_NUM_TEXTURE_SEMANTICS; semantic++) {
		unsigned index;

		for (index = 0; index < slReflection.semantic_textures[semantic].size(); index++) {
			slang_texture_semantic_meta &src = slReflection.semantic_textures[semantic][index];

			if (src.stage_mask) {
				static const char *names[] = { "Original", "Source", "OriginalHistory", "PassOutput", "PassFeedback" };
				texture_sem_t texture{};
				SlangTextureSemantic sem = (SlangTextureSemantic)semantic;
				texture.semantic = sem;
				texture.index = index;
				if (sem < SLANG_TEXTURE_SEMANTIC_ORIGINAL_HISTORY) {
					strncpy(texture.id, names[sem], sizeof(texture.id));
				} else {
					int size = (int)(sizeof(names) / sizeof(*names));
					if ((int)sem < size) {
						size_t len = strlen(names[sem]);
						memcpy(texture.id, names[sem], len);
						snprintf(texture.id + len, sizeof(texture.id) - len, "%d", index);
					} else {
						// Only user LUTs can end up here (not in names array).
						strncpy(texture.id, preset->luts[index].id.c_str(), sizeof(texture.id));
					}
				}

				if (sem == SLANG_TEXTURE_SEMANTIC_USER) {
					texture.wrap = preset->luts[index].wrap;
					texture.filter = preset->luts[index].filterLinear ? 0 : 1;
				} else {
					texture.wrap = preset->passes[passNumber].wrap;
					texture.filter = preset->passes[passNumber].filterLinear ? 0 : 1;
				}
				texture.stage_mask = src.stage_mask;
				texture.binding = src.binding;

				textures.push_back(texture);

				if (sem == SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK)
					preset->passes[index].feedback = true;

				if (sem == SLANG_TEXTURE_SEMANTIC_ORIGINAL_HISTORY && (unsigned)preset->historySize < index)
					preset->historySize = index;
			}

			if (src.push_constant || src.uniform) {
				uniform_sem_t uniform{};
				uniform.size = 4 * sizeof(float);
				SlangTextureSemantic sem = (SlangTextureSemantic)semantic;
				static const char *names[] = { "OriginalSize", "SourceSize", "OriginalHistorySize", "PassOutputSize", "PassFeedbackSize" };
				if (sem < SLANG_TEXTURE_SEMANTIC_ORIGINAL_HISTORY) {
					strncpy(uniform.id, names[sem], sizeof(uniform.id));
				} else {
					int size = (int)(sizeof(names) / sizeof(*names));
					if ((int)sem < size) {
						size_t len = strlen(names[sem]);
						memcpy(uniform.id, names[sem], len);
						snprintf(uniform.id + len, sizeof(uniform.id) - len, "%d", index);
					} else {
						strncpy(uniform.id, (preset->luts[index].id + "Size").c_str(), sizeof(uniform.id));
					}
				}
				// Encode the texture semantic in the data pointer: 0x200 + semantic * 8 + index.
				uniform.data = (void *)(uintptr_t)(0x200 + semantic * 8 + index);

				if (src.push_constant) {
					uniform.offset = (unsigned)src.push_constant_offset;
					uniforms[SLANG_CBUFFER_PC].push_back(uniform);
				} else {
					uniform.offset = (unsigned)src.ubo_offset;
					uniforms[SLANG_CBUFFER_UBO].push_back(uniform);
				}
			}
		}
	}

	out->texture_count = (int)textures.size();

	if (!textures.empty()) {
		out->textures = (texture_sem_t *)malloc(textures.size() * sizeof(*textures.data()));
		if (!out->textures)
			return false;
		memcpy(out->textures, textures.data(), textures.size() * sizeof(*textures.data()));
	}

	for (i = 0; i < SLANG_CBUFFER_MAX; i++) {
		if (uniforms[i].empty())
			continue;

		out->cbuffers[i].uniform_count = (int)uniforms[i].size();

		out->cbuffers[i].uniforms = (uniform_sem_t *)malloc(uniforms[i].size() * sizeof(*uniforms[i].data()));
		if (!out->cbuffers[i].uniforms)
			return false;

		memcpy(out->cbuffers[i].uniforms, uniforms[i].data(), uniforms[i].size() * sizeof(*uniforms[i].data()));
	}

	return true;
}

bool SlangPreprocessParseParameters(const std::string &shaderPath, SlangPreset *preset) {
	std::vector<std::string> lines;
	if (!SlangReadShaderFile(shaderPath, &lines, true, false))
		return false;

	glslang_meta meta;
	if (!SlangParseMeta(lines, &meta))
		return false;

	// Merge shader-declared parameters into the preset.
	for (auto &p : meta.parameters) {
		auto itr = std::find_if(preset->parameters.begin(), preset->parameters.end(), [&](const SlangParameter &parsedParam) {
			return p.id == parsedParam.id;
		});

		if (itr != preset->parameters.end()) {
			// The .slangp parameter list is intentionally read before its shader
			// sources so a malformed optional pass cannot hide the whole preset
			// from the picker. Replace that lightweight entry with the authoritative
			// pragma metadata when this pass is compiled.
			const bool presetPlaceholder = std::string(itr->desc) == itr->id &&
				itr->initial == 0.0f && itr->minimum == 0.0f && itr->maximum == 1.0f && itr->step == 0.01f;
			if (presetPlaceholder) {
				const float presetValue = itr->current;
				const bool hasPresetValue = itr->hasPresetValue;
				strncpy(itr->desc, p.desc.c_str(), sizeof(itr->desc) - 1);
				itr->initial = p.initial;
				itr->minimum = p.minimum;
				itr->maximum = p.maximum;
				itr->step = p.step;
				itr->current = hasPresetValue ? presetValue : p.initial;
				itr->hasPresetValue = hasPresetValue;
				continue;
			}
			// Allow duplicate #pragma parameter, but only if they are exactly the same.
			if (p.desc != itr->desc || p.initial != itr->initial || p.minimum != itr->minimum || p.maximum != itr->maximum || p.step != itr->step) {
				ERROR_LOG(Log::G3D, "[Slang] Duplicate parameters found for \"%s\", but arguments do not match.\n", p.id.c_str());
				return false;
			}
			continue;
		}

		SlangParameter param{};
		strncpy(param.id, p.id.c_str(), sizeof(param.id) - 1);
		strncpy(param.desc, p.desc.c_str(), sizeof(param.desc) - 1);
		param.initial = p.initial;
		param.minimum = p.minimum;
		param.maximum = p.maximum;
		param.step = p.step;
		param.current = p.initial;
		preset->parameters.push_back(param);
	}

	return true;
}

bool SlangProcess(SlangPreset *preset, unsigned passNumber, SlangPassCompiled *out) {
	if (passNumber >= preset->passes.size())
		return false;

	SlangPass &pass = preset->passes[passNumber];

	std::vector<std::string> lines;
	if (!SlangReadShaderFile(pass.source, &lines, true, false))
		return false;

	glslang_meta meta;
	if (!SlangParseMeta(lines, &meta))
		return false;

	if (!SlangPreprocessParseParameters(pass.source, preset))
		return false;

	if (pass.alias.empty() && !meta.name.empty())
		pass.alias = meta.name;

	out->format = meta.rt_format;
	if (out->format == SlangFormat::UNKNOWN) {
		if (pass.srgbFbo)
			out->format = SlangFormat::R8G8B8A8_SRGB;
		else if (pass.fpFbo)
			out->format = SlangFormat::R16G16B16A16_SFLOAT;
		else
			out->format = SlangFormat::R8G8B8A8_UNORM;
	}

	// Compile both stages to SPIR-V using PPSSPP's glslang.
	std::vector<uint32_t> vsSpirv, fsSpirv;
	{
		std::string vsSource = SlangBuildStageSource(lines, "vertex");
		std::string fsSource = SlangBuildStageSource(lines, "fragment");

		std::string vsError, fsError;
		if (!SlangCompileStageSpirv(true, vsSource, &vsSpirv, &vsError)) {
			ERROR_LOG(Log::G3D, "[Slang] Failed to compile vertex shader stage: %s\n", vsError.c_str());
			return false;
		}
		if (!SlangCompileStageSpirv(false, fsSource, &fsSpirv, &fsError)) {
			ERROR_LOG(Log::G3D, "[Slang] Failed to compile fragment shader stage: %s\n", fsError.c_str());
			return false;
		}
	}

	try {
		spirv_cross::CompilerGLSL vsCompiler(vsSpirv);
		spirv_cross::CompilerGLSL psCompiler(fsSpirv);
		spirv_cross::ShaderResources vsResources = vsCompiler.get_shader_resources();
		spirv_cross::ShaderResources psResources = psCompiler.get_shader_resources();

		// Force the uniform buffer to binding 0, matching PPSSPP's thin3d Vulkan layout.
		if (!vsResources.uniform_buffers.empty())
			vsCompiler.set_decoration(vsResources.uniform_buffers[0].id, spv::DecorationBinding, 0);
		if (!psResources.uniform_buffers.empty())
			psCompiler.set_decoration(psResources.uniform_buffers[0].id, spv::DecorationBinding, 0);

		// Remap texture bindings to 1..N so they match PPSSPP's thin3d Vulkan layout
		// (binding 1 = texture slot 0, etc). Max kMaxTextureBindings textures.
		{
			uint32_t nextBinding = 1;
			// Sort by original binding to keep a stable order.
			std::vector<const spirv_cross::Resource *> samplers;
			for (auto &res : psResources.sampled_images)
				samplers.push_back(&res);
			std::sort(samplers.begin(), samplers.end(), [&](const spirv_cross::Resource *a, const spirv_cross::Resource *b) {
				return psCompiler.get_decoration(a->id, spv::DecorationBinding) < psCompiler.get_decoration(b->id, spv::DecorationBinding);
			});
			for (auto *res : samplers) {
				if (nextBinding > (uint32_t)kMaxTextureBindings) {
					ERROR_LOG(Log::G3D, "[Slang] Shader uses more than %d textures, which PPSSPP's renderer does not support.\n", kMaxTextureBindings);
					return false;
				}
				psCompiler.set_decoration(res->id, spv::DecorationBinding, nextBinding);
				nextBinding++;
			}
		}

		spirv_cross::CompilerGLSL::Options options;
		options.version = 450;
		psCompiler.set_common_options(options);
		vsCompiler.set_common_options(options);

		out->vertexGLSL = vsCompiler.compile();
		out->fragmentGLSL = psCompiler.compile();

		if (!SlangProcessReflection(&vsCompiler, &psCompiler, vsResources, psResources, preset, passNumber, &out->semantics)) {
			ERROR_LOG(Log::G3D, "[Slang] Failed to reflect shader resources.\n");
			return false;
		}
	} catch (const std::exception &e) {
		ERROR_LOG(Log::G3D, "[Slang] SPIRV-Cross threw exception: %s.\n", e.what());
		return false;
	}

	out->alias = pass.alias;
	out->feedback = pass.feedback;
	return true;
}

void SlangFreePassSemantics(pass_semantics_t *semantics) {
	if (!semantics)
		return;
	free(semantics->textures);
	semantics->textures = nullptr;
	semantics->texture_count = 0;
	for (int i = 0; i < SLANG_CBUFFER_MAX; i++) {
		free(semantics->cbuffers[i].uniforms);
		semantics->cbuffers[i].uniforms = nullptr;
		semantics->cbuffers[i].uniform_count = 0;
	}
}

SlangPassCompiled::~SlangPassCompiled() {
	SlangFreePassSemantics(&semantics);
}
