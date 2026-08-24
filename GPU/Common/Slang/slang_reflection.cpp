// Ported from RetroArch slang frontend (gfx/drivers_shader/slang_reflection.cpp)
// for PPSSPP. GPLv3 licensed, see RetroArch project.

#include "slang_reflection.hpp"

#include <algorithm>
#include <cstdio>

#include "ext/SPIRV-Cross/spirv_cross.hpp"
#include "ext/SPIRV-Cross/spirv.hpp"

#include "Common/Log.h"

#include "slang_compile.h"

static const char *texture_semantic_names[] = {
	"Original",
	"Source",
	"OriginalHistory",
	"PassOutput",
	"PassFeedback",
	"User",
	nullptr,
};

static const char *texture_semantic_uniform_names[] = {
	"OriginalSize",
	"SourceSize",
	"OriginalHistorySize",
	"PassOutputSize",
	"PassFeedbackSize",
	"UserSize",
	nullptr,
};

static const char *semantic_uniform_names[] = {
	"MVP",
	"OutputSize",
	"FinalViewportSize",
	"FrameCount",
	"FrameDirection",
	"FrameTimeDelta",
	"OriginalFPS",
	"Rotation",
	"OriginalAspect",
	"OriginalAspectRotated",
	"TotalSubFrames",
	"CurrentSubFrame",
};

static bool SlangTextureSemanticIsArray(SlangTextureSemantic sem) {
	switch (sem) {
	case SLANG_TEXTURE_SEMANTIC_ORIGINAL_HISTORY:
	case SLANG_TEXTURE_SEMANTIC_PASS_OUTPUT:
	case SLANG_TEXTURE_SEMANTIC_PASS_FEEDBACK:
	case SLANG_TEXTURE_SEMANTIC_USER:
		return true;
	default:
		break;
	}
	return false;
}

static SlangTextureSemantic SlangNameToTextureSemanticArray(const std::string &name, const char **names, unsigned *index) {
	unsigned i = 0;
	while (*names) {
		const char *n = *names;
		SlangTextureSemantic semantic = (SlangTextureSemantic)(i);

		if (SlangTextureSemanticIsArray(semantic)) {
			size_t len = strlen(n);
			if (name.compare(0, len, n) == 0) {
				*index = (unsigned)strtoul(name.c_str() + len, nullptr, 0);
				return semantic;
			}
		} else if (name == n) {
			*index = 0;
			return semantic;
		}

		i++;
		names++;
	}
	return SLANG_INVALID_TEXTURE_SEMANTIC;
}

static SlangTextureSemantic SlangNameToTextureSemantic(
	const std::unordered_map<std::string, slang_texture_semantic_map> &semanticMap,
	const std::string &name, unsigned *index) {
	auto itr = semanticMap.find(name);
	if (itr != end(semanticMap)) {
		*index = itr->second.index;
		return itr->second.semantic;
	}

	return SlangNameToTextureSemanticArray(name, texture_semantic_names, index);
}

static SlangTextureSemantic SlangUniformNameToTextureSemantic(
	const std::unordered_map<std::string, slang_texture_semantic_map> &semanticMap,
	const std::string &name, unsigned *index) {
	auto itr = semanticMap.find(name);
	if (itr != end(semanticMap)) {
		*index = itr->second.index;
		return itr->second.semantic;
	}

	return SlangNameToTextureSemanticArray(name, texture_semantic_uniform_names, index);
}

static SlangSemantic SlangUniformNameToSemantic(
	const std::unordered_map<std::string, slang_semantic_map> &semanticMap,
	const std::string &name, unsigned *index) {
	unsigned i = 0;
	auto itr = semanticMap.find(name);

	if (itr != end(semanticMap)) {
		*index = itr->second.index;
		return itr->second.semantic;
	}

	// No builtin semantics are arrayed.
	*index = 0;
	for (auto n : semantic_uniform_names) {
		if (name == n)
			return (SlangSemantic)i;
		i++;
	}

	return SLANG_INVALID_SEMANTIC;
}

template <typename T>
static void ResizeMinimum(T &vec, unsigned minimum) {
	if ((unsigned)vec.size() < minimum)
		vec.resize(minimum);
}

static bool SetUboTextureOffset(slang_reflection *reflection, SlangTextureSemantic semantic, unsigned index, size_t offset, bool pushConstant) {
	ResizeMinimum(reflection->semantic_textures[semantic], index + 1);
	slang_texture_semantic_meta &sem = reflection->semantic_textures[semantic][index];
	bool &active = pushConstant ? sem.push_constant : sem.uniform;
	size_t &activeOffset = pushConstant ? sem.push_constant_offset : sem.ubo_offset;

	if (active) {
		if (activeOffset != offset) {
			ERROR_LOG(Log::G3D, "[Slang] Vertex and fragment have different offsets for same semantic %s #%u (%zu vs. %zu).\n",
				texture_semantic_uniform_names[semantic], index, activeOffset, offset);
			return false;
		}
	}

	active = true;
	activeOffset = offset;
	return true;
}

static bool SetUboFloatParameterOffset(slang_reflection *reflection, unsigned index, size_t offset, unsigned numComponents, bool pushConstant) {
	ResizeMinimum(reflection->semantic_float_parameters, index + 1);
	slang_semantic_meta &sem = reflection->semantic_float_parameters[index];
	bool &active = pushConstant ? sem.push_constant : sem.uniform;
	size_t &activeOffset = pushConstant ? sem.push_constant_offset : sem.ubo_offset;

	if (active) {
		if (activeOffset != offset) {
			ERROR_LOG(Log::G3D, "[Slang] Vertex and fragment have different offsets for same parameter #%u (%zu vs. %zu).\n",
				index, activeOffset, offset);
			return false;
		}
	}

	if ((sem.num_components != numComponents) && (sem.uniform || sem.push_constant)) {
		ERROR_LOG(Log::G3D, "[Slang] Vertex and fragment have different components for same parameter #%u (%u vs. %u).\n",
			index, sem.num_components, numComponents);
		return false;
	}

	active = true;
	activeOffset = offset;
	sem.num_components = numComponents;
	return true;
}

static bool SetUboOffset(slang_reflection *reflection, SlangSemantic semantic, size_t offset, unsigned numComponents, bool pushConstant) {
	slang_semantic_meta &sem = reflection->semantics[semantic];
	bool &active = pushConstant ? sem.push_constant : sem.uniform;
	size_t &activeOffset = pushConstant ? sem.push_constant_offset : sem.ubo_offset;

	if (active) {
		if (activeOffset != offset) {
			ERROR_LOG(Log::G3D, "[Slang] Vertex and fragment have different offsets for same semantic %s (%zu vs. %zu).\n",
				semantic_uniform_names[semantic], activeOffset, offset);
			return false;
		}
	}

	if ((sem.num_components != numComponents) && (sem.uniform || sem.push_constant)) {
		ERROR_LOG(Log::G3D, "[Slang] Vertex and fragment have different components for same semantic %s (%u vs. %u).\n",
			semantic_uniform_names[semantic], sem.num_components, numComponents);
		return false;
	}

	active = true;
	activeOffset = offset;
	sem.num_components = numComponents;
	return true;
}

static bool ValidateTypeForSemantic(const spirv_cross::SPIRType &type, SlangSemantic sem) {
	if (!type.array.empty())
		return false;
	if (type.basetype != spirv_cross::SPIRType::Float &&
		type.basetype != spirv_cross::SPIRType::Int &&
		type.basetype != spirv_cross::SPIRType::UInt)
		return false;

	switch (sem) {
	case SLANG_SEMANTIC_MVP:
		return type.basetype == spirv_cross::SPIRType::Float && type.vecsize == 4 && type.columns == 4;
	case SLANG_SEMANTIC_FRAME_COUNT:
		return type.basetype == spirv_cross::SPIRType::UInt && type.vecsize == 1 && type.columns == 1;
	case SLANG_SEMANTIC_TOTAL_SUBFRAMES:
		return type.basetype == spirv_cross::SPIRType::UInt && type.vecsize == 1 && type.columns == 1;
	case SLANG_SEMANTIC_CURRENT_SUBFRAME:
		return type.basetype == spirv_cross::SPIRType::UInt && type.vecsize == 1 && type.columns == 1;
	case SLANG_SEMANTIC_FRAME_DIRECTION:
		return type.basetype == spirv_cross::SPIRType::Int && type.vecsize == 1 && type.columns == 1;
	case SLANG_SEMANTIC_FRAME_TIME_DELTA:
		return type.basetype == spirv_cross::SPIRType::UInt && type.vecsize == 1 && type.columns == 1;
	case SLANG_SEMANTIC_ORIGINAL_FPS:
		return type.basetype == spirv_cross::SPIRType::Float && type.vecsize == 1 && type.columns == 1;
	case SLANG_SEMANTIC_ROTATION:
		return type.basetype == spirv_cross::SPIRType::UInt && type.vecsize == 1 && type.columns == 1;
	case SLANG_SEMANTIC_CORE_ASPECT:
	case SLANG_SEMANTIC_CORE_ASPECT_ROT:
		return type.basetype == spirv_cross::SPIRType::Float && type.vecsize == 1 && type.columns == 1;
	case SLANG_SEMANTIC_FLOAT_PARAMETER:
		return type.basetype == spirv_cross::SPIRType::Float && type.vecsize == 1 && type.columns == 1;
	default:
		break;
	}
	return type.basetype == spirv_cross::SPIRType::Float && type.vecsize == 4 && type.columns == 1;
}

static bool ValidateTypeForTextureSemantic(const spirv_cross::SPIRType &type) {
	return type.array.empty() && type.basetype == spirv_cross::SPIRType::Float && type.vecsize == 4 && type.columns == 1;
}

static bool AddActiveBufferRanges(const spirv_cross::Compiler &compiler, const spirv_cross::Resource &resource, slang_reflection *reflection, bool pushConstant) {
	// Get which uniforms are actually in use by this shader.
	auto ranges = compiler.get_active_buffer_ranges(resource.id);

	for (size_t i = 0; i < ranges.size(); i++) {
		unsigned semIndex = 0;
		unsigned texSemIndex = 0;
		const std::string &name = compiler.get_member_name(resource.base_type_id, ranges[i].index);
		const spirv_cross::SPIRType &type = compiler.get_type(
			compiler.get_type(resource.base_type_id).member_types[ranges[i].index]);
		SlangSemantic sem = SlangUniformNameToSemantic(*reflection->semantic_map, name, &semIndex);
		SlangTextureSemantic texSem = SlangUniformNameToTextureSemantic(*reflection->texture_semantic_uniform_map, name, &texSemIndex);

		if (texSem == SLANG_TEXTURE_SEMANTIC_PASS_OUTPUT && texSemIndex >= reflection->pass_number) {
			ERROR_LOG(Log::G3D, "[Slang] Non causal filter chain detected. Shader is trying to use output from pass #%u, but this shader is pass #%u.\n",
				texSemIndex, reflection->pass_number);
			return false;
		}

		if (sem != SLANG_INVALID_SEMANTIC) {
			if (!ValidateTypeForSemantic(type, sem)) {
				ERROR_LOG(Log::G3D, "[Slang] Underlying type of semantic is invalid.\n");
				return false;
			}

			switch (sem) {
			case SLANG_SEMANTIC_FLOAT_PARAMETER:
				if (!SetUboFloatParameterOffset(reflection, semIndex, ranges[i].offset, type.vecsize, pushConstant))
					return false;
				break;
			default:
				if (!SetUboOffset(reflection, sem, ranges[i].offset, type.vecsize * type.columns, pushConstant))
					return false;
				break;
			}
		} else if (texSem != SLANG_INVALID_TEXTURE_SEMANTIC) {
			if (!ValidateTypeForTextureSemantic(type)) {
				ERROR_LOG(Log::G3D, "[Slang] Underlying type of texture semantic is invalid.\n");
				return false;
			}

			if (!SetUboTextureOffset(reflection, texSem, texSemIndex, ranges[i].offset, pushConstant))
				return false;
		} else {
			ERROR_LOG(Log::G3D, "[Slang] Unknown semantic found.\n");
			return false;
		}
	}
	return true;
}

slang_reflection::slang_reflection() {
	for (unsigned i = 0; i < SLANG_NUM_TEXTURE_SEMANTICS; i++)
		semantic_textures[i].resize(SlangTextureSemanticIsArray((SlangTextureSemantic)i) ? 0 : 1);
}

bool slang_reflect(const spirv_cross::Compiler &vertex_compiler,
	const spirv_cross::Compiler &fragment_compiler,
	const spirv_cross::ShaderResources &vertex,
	const spirv_cross::ShaderResources &fragment,
	slang_reflection *reflection) {
	uint32_t locationMask = 0;
	uint32_t bindingMask = 0;
	unsigned i = 0;

	// Validate use of unexpected types.
	if (!vertex.sampled_images.empty() || !vertex.storage_buffers.empty() || !vertex.subpass_inputs.empty() ||
		!vertex.storage_images.empty() || !vertex.atomic_counters.empty() || !fragment.storage_buffers.empty() ||
		!fragment.subpass_inputs.empty() || !fragment.storage_images.empty() || !fragment.atomic_counters.empty()) {
		ERROR_LOG(Log::G3D, "[Slang] Invalid resource type detected.\n");
		return false;
	}

	// Validate vertex input.
	if (vertex.stage_inputs.size() != 2) {
		ERROR_LOG(Log::G3D, "[Slang] Vertex must have two attributes.\n");
		return false;
	}

	if (fragment.stage_outputs.size() != 1) {
		ERROR_LOG(Log::G3D, "[Slang] Multiple render targets not supported.\n");
		return false;
	}

	if (fragment_compiler.get_decoration(fragment.stage_outputs[0].id, spv::DecorationLocation) != 0) {
		ERROR_LOG(Log::G3D, "[Slang] Render target must use location = 0.\n");
		return false;
	}

	for (i = 0; i < vertex.stage_inputs.size(); i++)
		locationMask |= 1 << vertex_compiler.get_decoration(vertex.stage_inputs[i].id, spv::DecorationLocation);

	if (locationMask != 0x3) {
		ERROR_LOG(Log::G3D, "[Slang] The two vertex attributes do not use location = 0 and location = 1.\n");
		return false;
	}

	// Validate the single uniform buffer.
	if (vertex.uniform_buffers.size() > 1) {
		ERROR_LOG(Log::G3D, "[Slang] Vertex must use zero or one uniform buffer.\n");
		return false;
	}
	if (fragment.uniform_buffers.size() > 1) {
		ERROR_LOG(Log::G3D, "[Slang] Fragment must use zero or one uniform buffer.\n");
		return false;
	}
	if (vertex.push_constant_buffers.size() > 1) {
		ERROR_LOG(Log::G3D, "[Slang] Vertex must use zero or one push constant buffer.\n");
		return false;
	}
	if (fragment.push_constant_buffers.size() > 1) {
		ERROR_LOG(Log::G3D, "[Slang] Fragment must use zero or one push constant buffer.\n");
		return false;
	}

	uint32_t vertexUbo = vertex.uniform_buffers.empty() ? 0 : (uint32_t)vertex.uniform_buffers[0].id;
	uint32_t fragmentUbo = fragment.uniform_buffers.empty() ? 0 : (uint32_t)fragment.uniform_buffers[0].id;
	uint32_t vertexPush = vertex.push_constant_buffers.empty() ? 0 : (uint32_t)vertex.push_constant_buffers[0].id;
	uint32_t fragmentPush = fragment.push_constant_buffers.empty() ? 0 : (uint32_t)fragment.push_constant_buffers[0].id;

	if (vertexUbo && vertex_compiler.get_decoration(vertexUbo, spv::DecorationDescriptorSet) != 0) {
		ERROR_LOG(Log::G3D, "[Slang] Resources must use descriptor set #0.\n");
		return false;
	}
	if (fragmentUbo && fragment_compiler.get_decoration(fragmentUbo, spv::DecorationDescriptorSet) != 0) {
		ERROR_LOG(Log::G3D, "[Slang] Resources must use descriptor set #0.\n");
		return false;
	}

	unsigned vertexUboBinding = vertexUbo ? vertex_compiler.get_decoration(vertexUbo, spv::DecorationBinding) : -1u;
	unsigned fragmentUboBinding = fragmentUbo ? fragment_compiler.get_decoration(fragmentUbo, spv::DecorationBinding) : -1u;
	bool hasUbo = vertexUbo || fragmentUbo;

	if ((vertexUboBinding != -1u) && (fragmentUboBinding != -1u) && (vertexUboBinding != fragmentUboBinding)) {
		ERROR_LOG(Log::G3D, "[Slang] Vertex and fragment uniform buffer must have same binding.\n");
		return false;
	}

	unsigned uboBinding = (vertexUboBinding != -1u) ? vertexUboBinding : fragmentUboBinding;

	if (hasUbo && uboBinding >= SLANG_NUM_BINDINGS) {
		ERROR_LOG(Log::G3D, "[Slang] Binding %u is out of range.\n", uboBinding);
		return false;
	}

	reflection->ubo_binding = hasUbo ? uboBinding : 0;
	reflection->ubo_stage_mask = 0;
	reflection->ubo_size = 0;
	reflection->push_constant_size = 0;
	reflection->push_constant_stage_mask = 0;

	if (vertexUbo) {
		reflection->ubo_stage_mask |= SLANG_STAGE_VERTEX_MASK;
		size_t y = vertex_compiler.get_declared_struct_size(vertex_compiler.get_type(vertex.uniform_buffers[0].base_type_id));
		reflection->ubo_size = std::max(reflection->ubo_size, y);
	}

	if (fragmentUbo) {
		reflection->ubo_stage_mask |= SLANG_STAGE_FRAGMENT_MASK;
		size_t y = fragment_compiler.get_declared_struct_size(fragment_compiler.get_type(fragment.uniform_buffers[0].base_type_id));
		reflection->ubo_size = std::max(reflection->ubo_size, y);
	}

	if (vertexPush) {
		reflection->push_constant_stage_mask |= SLANG_STAGE_VERTEX_MASK;
		size_t y = vertex_compiler.get_declared_struct_size(vertex_compiler.get_type(vertex.push_constant_buffers[0].base_type_id));
		reflection->push_constant_size = std::max(reflection->push_constant_size, y);
	}

	if (fragmentPush) {
		reflection->push_constant_stage_mask |= SLANG_STAGE_FRAGMENT_MASK;
		size_t y = fragment_compiler.get_declared_struct_size(fragment_compiler.get_type(fragment.push_constant_buffers[0].base_type_id));
		reflection->push_constant_size = std::max(reflection->push_constant_size, y);
	}

	// Validate push constant size against Vulkan's minimum spec to avoid cross-vendor issues.
	if (reflection->push_constant_size > 128) {
		ERROR_LOG(Log::G3D, "[Slang] Exceeded maximum size of 128 bytes for push constant buffer.\n");
		return false;
	}

	// Find all relevant uniforms and push constants.
	if (vertexUbo && !AddActiveBufferRanges(vertex_compiler, vertex.uniform_buffers[0], reflection, false))
		return false;
	if (fragmentUbo && !AddActiveBufferRanges(fragment_compiler, fragment.uniform_buffers[0], reflection, false))
		return false;
	if (vertexPush && !AddActiveBufferRanges(vertex_compiler, vertex.push_constant_buffers[0], reflection, true))
		return false;
	if (fragmentPush && !AddActiveBufferRanges(fragment_compiler, fragment.push_constant_buffers[0], reflection, true))
		return false;

	if (hasUbo)
		bindingMask = 1 << uboBinding;

	// On to textures.
	for (i = 0; i < fragment.sampled_images.size(); i++) {
		unsigned arrayIndex = 0;
		unsigned set = fragment_compiler.get_decoration(fragment.sampled_images[i].id, spv::DecorationDescriptorSet);
		unsigned binding = fragment_compiler.get_decoration(fragment.sampled_images[i].id, spv::DecorationBinding);

		if (set != 0) {
			ERROR_LOG(Log::G3D, "[Slang] Resources must use descriptor set #0.\n");
			return false;
		}
		if (binding >= SLANG_NUM_BINDINGS) {
			ERROR_LOG(Log::G3D, "[Slang] Binding %u is out of range.\n", binding);
			return false;
		}
		if (bindingMask & (1 << binding)) {
			ERROR_LOG(Log::G3D, "[Slang] Binding %u is already in use.\n", binding);
			return false;
		}
		bindingMask |= 1 << binding;

		SlangTextureSemantic index = SlangNameToTextureSemantic(*reflection->texture_semantic_map, fragment.sampled_images[i].name, &arrayIndex);

		if (index == SLANG_TEXTURE_SEMANTIC_PASS_OUTPUT && arrayIndex >= reflection->pass_number) {
			ERROR_LOG(Log::G3D, "[Slang] Non causal filter chain detected. Shader is trying to use output from pass #%u, but this shader is pass #%u.\n",
				arrayIndex, reflection->pass_number);
			return false;
		} else if (index == SLANG_INVALID_TEXTURE_SEMANTIC) {
			ERROR_LOG(Log::G3D, "[Slang] Texture name '%s' not found in semantic map, probably the texture name or pass alias is not defined in the preset (non-semantic textures not supported yet)\n",
				fragment.sampled_images[i].name.c_str());
			return false;
		}

		ResizeMinimum(reflection->semantic_textures[index], arrayIndex + 1);
		slang_texture_semantic_meta &semantic = reflection->semantic_textures[index][arrayIndex];
		semantic.binding = binding;
		semantic.stage_mask = SLANG_STAGE_FRAGMENT_MASK;
		semantic.texture = true;
	}

	return true;
}
