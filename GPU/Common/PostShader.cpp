// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.


// Postprocessing shader manager

#include <string>
#include <vector>
#include <algorithm>

#include "Common/Log.h"
#include "Common/Data/Format/IniFile.h"
#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/File/VFS/VFS.h"
#include "Common/GPU/OpenGL/GLFeatures.h"
#include "Common/GPU/thin3d.h"
#include "Common/StringUtils.h"

#include "Core/System.h"
#include "GPU/Common/PostShader.h"
#include "GPU/Common/Slang/slangp_parser.h"

static std::vector<ShaderInfo> shaderInfo;
// Okay, not really "post" shaders, but related.
static std::vector<TextureShaderInfo> textureShaderInfo;
// Keeps the shared preset alive for all passes of the same .slangp.
static std::vector<std::shared_ptr<SlangPreset>> slangPresetCache;
static std::vector<std::string> slangSectionList;
static std::vector<std::string> registeredSlangPresetPaths;

static void ClearSlangCache() {
	slangPresetCache.clear();
	slangSectionList.clear();
}

static bool LoadSlangShaderInfoFile(const std::string &presetPath, std::string *error = nullptr) {
		auto preset = std::make_shared<SlangPreset>();
		std::vector<std::string> errors;
		if (!SlangLoadPreset(presetPath, preset.get(), &errors)) {
			WARN_LOG(Log::G3D, "Failed to load slang preset %s: %s", presetPath.c_str(), errors.empty() ? "unknown error" : errors[0].c_str());
			if (error)
				*error = errors.empty() ? "unknown error" : errors[0];
			return false;
		}

		// The section is the persistent PPSSPP identifier, so it must be the
		// complete preset path.  File names alone collide frequently in shader
		// packs and cannot be resolved after a GameDB reload.
		std::string title = Path(presetPath).GetFilename();
		size_t dot = title.find_last_of('.');
		if (dot != std::string::npos)
			title = title.substr(0, dot);
		std::string section = presetPath;
		if (std::find(slangSectionList.begin(), slangSectionList.end(), section) != slangSectionList.end())
			return true;

		for (size_t p = 0; p < preset->passes.size(); ++p) {
			ShaderInfo info{};
			info.iniFile = Path(presetPath);
			info.section = section;
			info.name = section;
			info.visible = p == 0;
			info.fragmentShaderFile = Path(preset->passes[p].source);
			info.isSlang = true;
			info.slangPassIndex = (int)p;
			info.slangPreset = preset;
			info.usePreviousFrame = false;

			// Expose up to 4 parameters to the standard settings UI as well.
			for (size_t i = 0; i < ARRAY_SIZE(info.settings); ++i) {
				auto &setting = info.settings[i];
				setting.name.clear();
				setting.value = 0.0f;
				setting.minValue = 0.0f;
				setting.maxValue = 1.0f;
				setting.step = 0.01f;
				if (p == 0 && i < preset->parameters.size()) {
					const SlangParameter &param = preset->parameters[i];
					setting.name = param.desc[0] ? param.desc : param.id;
					setting.value = param.current;
					setting.minValue = param.minimum;
					setting.maxValue = param.maximum;
					setting.step = param.step;
				}
			}

			shaderInfo.push_back(info);
		}

		slangPresetCache.push_back(preset);
		slangSectionList.push_back(section);
		return true;
	}

// Recursively scans a directory for .slangp presets and registers one ShaderInfo
// per pass. Only the first pass is visible in menus.
static void LoadSlangShaderInfo(const std::string &directory) {
	std::vector<File::FileInfo> fileInfo;
	File::GetFilesInDir(Path(directory), &fileInfo, "slangp:");

	for (const auto &fi : fileInfo) {
		if (fi.isDirectory) {
			LoadSlangShaderInfo(fi.fullName.ToString());
			continue;
		}
		LoadSlangShaderInfoFile(fi.fullName.ToString());
	}
}

bool RegisterSlangPresetPath(const std::string &path, std::string *error) {
	// Validate now so the picker can report a useful error instead of merely
	// leaving the preset absent from the shader registry after a reload.
	SlangPreset probe;
	std::vector<std::string> errors;
	if (!SlangLoadPreset(path, &probe, &errors)) {
		if (error)
			*error = errors.empty() ? "unknown error" : errors.front();
		return false;
	}

	if (std::find(registeredSlangPresetPaths.begin(), registeredSlangPresetPaths.end(), path) == registeredSlangPresetPaths.end())
		registeredSlangPresetPaths.push_back(path);
	return true;
}

static Draw::GPUVendor VendorFromString(const std::string &vendor) {
	Draw::GPUVendor::VENDOR_UNKNOWN;
	// TODO: This should probably be a function somewhere.
	if (vendor == "ARM") {
		return Draw::GPUVendor::VENDOR_ARM;
	} else if (vendor == "Qualcomm") {
		return Draw::GPUVendor::VENDOR_QUALCOMM;
	} else if (vendor == "IMGTEC") {
		return Draw::GPUVendor::VENDOR_IMGTEC;
	} else if (vendor == "NVIDIA") {
		return Draw::GPUVendor::VENDOR_NVIDIA;
	} else if (vendor == "AMD") {
		return Draw::GPUVendor::VENDOR_AMD;
	} else if (vendor == "Broadcom") {
		return Draw::GPUVendor::VENDOR_BROADCOM;
	} else if (vendor == "Apple") {
		return Draw::GPUVendor::VENDOR_APPLE;
	} else if (vendor == "Intel") {
		return Draw::GPUVendor::VENDOR_INTEL;
	} else if (vendor == "Mesa") {
		return Draw::GPUVendor::VENDOR_MESA;
	} else if (vendor == "Vivante") {
		return Draw::GPUVendor::VENDOR_VIVANTE;
	}
	return Draw::GPUVendor::VENDOR_UNKNOWN;
}

// Scans the directories for shader ini files and collects info about all the shaders found.

void LoadPostShaderInfo(Draw::DrawContext *draw, const std::vector<Path> &directories) {
	std::vector<ShaderInfo> notVisible;

	Draw::GPUVendor gpuVendor = Draw::GPUVendor::VENDOR_UNKNOWN;
	if (draw) {
		gpuVendor = draw->GetDeviceCaps().vendor;
	}

	shaderInfo.clear();
	textureShaderInfo.clear();
	ClearSlangCache();

	auto appendShader = [&](const ShaderInfo &info) {
		auto beginErase = std::remove(shaderInfo.begin(), shaderInfo.end(), info.name);
		if (beginErase != shaderInfo.end()) {
			shaderInfo.erase(beginErase, shaderInfo.end());
		}
		shaderInfo.push_back(info);
	};

	auto appendTextureShader = [&](const TextureShaderInfo &info) {
		auto beginErase = std::remove(textureShaderInfo.begin(), textureShaderInfo.end(), info.name);
		if (beginErase != textureShaderInfo.end()) {
			textureShaderInfo.erase(beginErase, textureShaderInfo.end());
		}
		textureShaderInfo.push_back(info);
	};

	for (size_t d = 0; d < directories.size(); d++) {
		std::vector<File::FileInfo> fileInfo;
		g_VFS.GetFileListing(directories[d].c_str(), &fileInfo, "ini:");

		if (fileInfo.empty()) {
			File::GetFilesInDir(directories[d], &fileInfo, "ini:");
		}

		for (size_t f = 0; f < fileInfo.size(); f++) {
			IniFile ini;
			bool success = false;
			if (fileInfo[f].isDirectory)
				continue;

			Path name = fileInfo[f].fullName;
			Path path = directories[d];
			// Hack around Android VFS path bug. really need to redesign this.
			if (name.ToString().substr(0, 7) == "assets/")
				name = Path(name.ToString().substr(7));
			if (path.ToString().substr(0, 7) == "assets/")
				path = Path(path.ToString().substr(7));

			if (ini.LoadFromVFS(g_VFS, name.ToString()) || ini.Load(fileInfo[f].fullName)) {
				success = true;
				// vsh load. meh.
			}

			if (!success)
				continue;

			// Alright, let's loop through the sections and see if any is a shader.
			for (size_t i = 0; i < ini.Sections().size(); i++) {
				Section &section = *(ini.Sections()[i].get());
				std::string shaderType = "render";
				section.Get("Type", &shaderType);

				std::vector<std::string> vendorBlacklist;
				section.Get("VendorBlacklist", &vendorBlacklist);
				bool skipped = false;
				for (auto &item : vendorBlacklist) {
					const Draw::GPUVendor blacklistedVendor = VendorFromString(item);
					if (blacklistedVendor == gpuVendor && blacklistedVendor != Draw::GPUVendor::VENDOR_UNKNOWN) {
						skipped = true;
						break;
					}
				}
				if (skipped) {
					continue;
				}

				if (section.HasKey("Fragment") && section.HasKey("Vertex") &&
					(strncasecmp(shaderType.c_str(), "render", shaderType.size()) == 0 ||
					 strncasecmp(shaderType.c_str(), "StereoToMono", shaderType.size()) == 0)) {
					// Valid shader!
					ShaderInfo info{};
					std::string temp;
					info.section = section.name();
					info.name = section.name();
					info.visible = true;
					section.Get("Name", &info.name);
					section.Get("Parent", &info.parent);
					section.Get("Visible", &info.visible);
					temp.clear();
					section.Get("Fragment", &temp);
					info.fragmentShaderFile = path / temp;
					temp.clear();
					section.Get("Vertex", &temp);
					info.vertexShaderFile = path / temp;
					section.Get("OutputResolution", &info.outputResolution);
					section.Get("Upscaling", &info.isUpscalingFilter);
					section.Get("SSAA", &info.SSAAFilterLevel);
					section.Get("60fps", &info.requires60fps);
					section.Get("UsePreviousFrame", &info.usePreviousFrame);

					if (info.parent == "Off")
						info.parent.clear();

					if (strncasecmp(shaderType.c_str(), "stereotomono", shaderType.size()) == 0) {
						info.isStereo = true;
						info.isUpscalingFilter = false;
						info.parent.clear();
					}

					for (size_t i = 0; i < ARRAY_SIZE(info.settings); ++i) {
						auto &setting = info.settings[i];
						setting.name.clear();
						setting.value = 0.0;
						setting.minValue = -1.0f;
						setting.maxValue = 1.0f;
						setting.step = 0.01f;
						section.Get(StringFromFormat("SettingName%d", i + 1).c_str(), &setting.name);
						section.Get(StringFromFormat("SettingDefaultValue%d", i + 1).c_str(), &setting.value);
						section.Get(StringFromFormat("SettingMinValue%d", i + 1).c_str(), &setting.minValue);
						section.Get(StringFromFormat("SettingMaxValue%d", i + 1).c_str(), &setting.maxValue);
						section.Get(StringFromFormat("SettingStep%d", i + 1).c_str(), &setting.step);
					}

					// Let's ignore shaders we can't support. TODO: Not a very good check
					if (gl_extensions.IsGLES && !gl_extensions.GLES3) {
						bool requiresIntegerSupport = false;
						section.Get("RequiresIntSupport", &requiresIntegerSupport);
						if (requiresIntegerSupport)
							continue;
					}

					if (info.visible) {
						appendShader(info);
					} else {
						notVisible.push_back(info);
					}
				} else if (section.HasKey("Compute") && strncasecmp(shaderType.c_str(), "texture", shaderType.size()) == 0) {
					// This is a texture shader.
					TextureShaderInfo info{};
					std::string temp;
					info.section = section.name();
					info.name = section.name();
					info.scaleFactor = 0;
					section.Get("Name", &info.name);
					section.Get("Scale", &info.scaleFactor);
					bool hidden = false;
					section.Get("Hidden", &info.hidden);
					std::string cbufferFilename;
					if (section.Get("ConstantBuffer", &cbufferFilename)) {
						Path cbufferPath = path / cbufferFilename;
						info.constantBuffer = cbufferPath;
					}
					if (section.Get("Compute", &temp)) {
						info.computeShaderFile = path / temp;
						info.computeShaderFiles.push_back(info.computeShaderFile);
						for (int computeIndex = 2; computeIndex <= 4; ++computeIndex) {
							temp.clear();
							if (section.Get(StringFromFormat("Compute%d", computeIndex).c_str(), &temp)) {
								info.computeShaderFiles.push_back(path / temp);
							}
						}
						if (info.scaleFactor >= 2 && info.scaleFactor < 8) {
							appendTextureShader(info);
						}
					} else {
						ERROR_LOG(Log::G3D, "Compute field missing for compute shader");
					}
				} else if (!section.name().empty()) {
					WARN_LOG(Log::G3D, "Unrecognized shader type '%s' or invalid shader in section '%s'", shaderType.c_str(), section.name().c_str());
				}
			}
		}
	}

	// Sort shaders alphabetically.
	std::sort(shaderInfo.begin(), shaderInfo.end());
	std::sort(textureShaderInfo.begin(), textureShaderInfo.end());

	// Scan for RetroArch .slangp presets (one entry per pass, in order).
	for (size_t d = 0; d < directories.size(); d++) {
		std::string dir = directories[d].ToString();
		// On Switch the sdmc:/ custom-shader root is listable but IsDirectory()
		// may reject its virtual path spelling. GetFilesInDir() is the authority
		// here and safely returns an empty list for non-directories.
		LoadSlangShaderInfo(dir);
	}

	// The Switch file picker returns absolute sdmc:/ paths. Keep selected
	// presets authoritative even if recursive custom-directory enumeration is
	// unavailable for that virtual mount.
	for (const std::string &path : registeredSlangPresetPaths)
		LoadSlangShaderInfoFile(path);

	// Re-sort, now including the slang entries.
	std::sort(shaderInfo.begin(), shaderInfo.end());

	ShaderInfo off{};
	off.visible = true;
	off.name = "Off";
	off.section = "Off";
	for (size_t i = 0; i < ARRAY_SIZE(off.settings); ++i) {
		off.settings[i].name.clear();
		off.settings[i].value = 0.0f;
		off.settings[i].minValue = -1.0f;
		off.settings[i].maxValue = 1.0f;
		off.settings[i].step = 0.01f;
	}

	TextureShaderInfo textureOff{};
	textureOff.name = "Off";
	textureOff.section = "Off";
	textureShaderInfo.insert(textureShaderInfo.begin(), textureOff);

	// We always want the not visible ones at the end.  Makes menus easier.
	shaderInfo.reserve(notVisible.size() + 1);
	shaderInfo.insert(shaderInfo.begin(), off);
	for (const auto &info : notVisible) {
		appendShader(info);
	}
}

// Scans the directories for shader ini files and collects info about all the shaders found.
void ReloadAllPostShaderInfo(Draw::DrawContext *draw) {
	std::vector<Path> directories;
	directories.push_back(Path("shaders"));  // For VFS
	directories.push_back(GetSysDirectory(DIRECTORY_CUSTOM_SHADERS));
	LoadPostShaderInfo(draw, directories);
}

void RemoveUnknownPostShaders(std::vector<std::string> *names) {
	for (auto iter = names->begin(); iter != names->end(); ) {
		if (GetPostShaderInfo(*iter) == nullptr) {
			iter = names->erase(iter);
		} else {
			++iter;
		}
	}
}

const ShaderInfo *GetPostShaderInfo(std::string_view name) {
	for (size_t i = 0; i < shaderInfo.size(); i++) {
		if (shaderInfo[i].section == name)
			return &shaderInfo[i];
	}
	return nullptr;
}

std::vector<const ShaderInfo *> GetPostShaderChain(const std::string &name) {
	std::vector<const ShaderInfo *> backwards;
	const ShaderInfo *shaderInfo = GetPostShaderInfo(name);

	// Slang presets expand to all their passes.
	if (shaderInfo && shaderInfo->isSlang) {
		for (const auto &info : ::shaderInfo) {
			if (info.section == name && info.isSlang)
				backwards.push_back(&info);
		}
		if (backwards.empty())
			backwards.push_back(shaderInfo);
		return backwards;
	}

	while (shaderInfo) {
		backwards.push_back(shaderInfo);

		if (!shaderInfo->parent.empty()) {
			shaderInfo = GetPostShaderInfo(shaderInfo->parent);
		} else {
			shaderInfo = nullptr;
		}
		auto dup = std::find(backwards.begin(), backwards.end(), shaderInfo);
		if (dup != backwards.end()) {
			// Don't loop forever.
			break;
		}
	}

	if (!backwards.empty())
		std::reverse(backwards.begin(), backwards.end());
	// Not backwards anymore.
	return backwards;
}

std::vector<const ShaderInfo *> GetFullPostShadersChain(const std::vector<std::string> &names) {
	std::vector<const ShaderInfo *> fullChain;
	for (const auto &shaderName : names) {
		const auto &shaderChain = GetPostShaderChain(shaderName);
		fullChain.insert(fullChain.end(), shaderChain.begin(), shaderChain.end());
	}
	return fullChain;
}

bool PostShaderChainRequires60FPS(const std::vector<const ShaderInfo *> &chain) {
	for (auto shaderInfo : chain) {
		if (shaderInfo->requires60fps)
			return true;
	}
	return false;
}

const std::vector<ShaderInfo> &GetAllPostShaderInfo() {
	return shaderInfo;
}

const TextureShaderInfo *GetTextureShaderInfo(std::string_view name) {
	for (auto &info : textureShaderInfo) {
		if (info.section == name) {
			return &info;
		}
	}
	return nullptr;
}
const std::vector<TextureShaderInfo> &GetAllTextureShaderInfo() {
	return textureShaderInfo;
}

void FixPostShaderOrder(std::vector<std::string> *names) {
	// There's one rule only that we enforce - only one shader can use UsePreviousFrame,
	// and it has to be the last one. So we simply remove any we find from the list,
	// and then append it to the end if there is one.
	std::string prevFrameShader;
	for (auto iter = names->begin(); iter != names->end(); ) {
		const ShaderInfo *info = GetPostShaderInfo(*iter);
		if (info) {
			if (info->usePreviousFrame) {
				prevFrameShader = *iter;
				iter = names->erase(iter++);
				continue;
			}
		}
		++iter;
	}

	if (!prevFrameShader.empty()) {
		names->push_back(prevFrameShader);
	}
}
