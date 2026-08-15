// .slangp preset parser, ported from RetroArch's CG-style preset format
// (gfx/video_shader_parse.c) for PPSSPP. GPLv3 licensed, see RetroArch project.

#include "slangp_parser.h"

#include <cctype>
#include <cstring>

#include "Common/File/FileUtil.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"

namespace {

struct KeyValue {
	std::string key;
	std::string value;
};

// Splits "key = value" lines, handling quoted values and comments (# or ;).
bool ParseKeyValue(const std::string &line, KeyValue *kv) {
	std::string trimmed = line;
	// Strip comments.
	size_t hash = trimmed.find('#');
	if (hash != std::string::npos)
		trimmed = trimmed.substr(0, hash);

	// Trim whitespace.
	size_t start = trimmed.find_first_not_of(" \t\r\n");
	if (start == std::string::npos)
		return false;
	size_t end = trimmed.find_last_not_of(" \t\r\n");
	trimmed = trimmed.substr(start, end - start + 1);
	if (trimmed.empty())
		return false;

	size_t eq = trimmed.find('=');
	if (eq == std::string::npos)
		return false;

	kv->key = trimmed.substr(0, eq);
	kv->value = trimmed.substr(eq + 1);

	auto trim = [](std::string &s) {
		size_t a = s.find_first_not_of(" \t\r\n");
		size_t b = s.find_last_not_of(" \t\r\n\"");
		if (a == std::string::npos) {
			s.clear();
			return;
		}
		s = s.substr(a, b - a + 1);
	};
	trim(kv->key);
	trim(kv->value);
	return !kv->key.empty();
}

bool ParseBool(const std::string &v) {
	if (v == "true" || v == "1" || v == "yes")
		return true;
	return false;
}

float ParseFloat(const std::string &v, float def) {
	if (v.empty())
		return def;
	return (float)atof(v.c_str());
}

int ParseInt(const std::string &v, int def) {
	if (v.empty())
		return def;
	return atoi(v.c_str());
}

SlangWrapType ParseWrap(const std::string &v) {
	if (v == "repeat")
		return SlangWrapType::REPEAT;
	if (v == "mirrored_repeat")
		return SlangWrapType::MIRRORED_REPEAT;
	if (v == "border")
		return SlangWrapType::BORDER;
	return SlangWrapType::EDGE;
}

std::string DirOf(const std::string &path) {
	size_t slash = path.find_last_of("/\\");
	if (slash == std::string::npos)
		return "";
	return path.substr(0, slash + 1);
}

void SplitSemicolon(const std::string &s, std::vector<std::string> *out) {
	std::string cur;
	for (char c : s) {
		if (c == ';' || c == ',') {
			if (!cur.empty())
				out->push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty())
		out->push_back(cur);
}

}  // namespace

bool SlangLoadPreset(const std::string &presetPath, SlangPreset *preset, std::vector<std::string> *errors) {
	std::string buf;
	if (!File::ReadTextFileToString(Path(presetPath), &buf)) {
		errors->push_back("Failed to read preset file: " + presetPath);
		return false;
	}

	std::string baseDir = DirOf(presetPath);

	std::vector<KeyValue> entries;
	{
		std::vector<std::string> lines;
		SplitString(buf, '\n', lines);
		for (auto &line : lines) {
			KeyValue kv;
			if (ParseKeyValue(line, &kv))
				entries.push_back(std::move(kv));
		}
	}

	auto find = [&](const std::string &key, std::string *out) -> bool {
		for (auto &e : entries) {
			if (e.key == key) {
				*out = e.value;
				return true;
			}
		}
		return false;
	};

	// Basic sanity: need a shaders count.
	std::string shadersStr;
	if (!find("shaders", &shadersStr)) {
		errors->push_back("Preset has no 'shaders' key.");
		return false;
	}
	int numPasses = ParseInt(shadersStr, 0);
	if (numPasses <= 0 || numPasses > 16) {
		errors->push_back("Invalid shader pass count: " + shadersStr);
		return false;
	}

	preset->passes.clear();
	preset->parameters.clear();
	preset->luts.clear();

	std::string id;

	for (int i = 0; i < numPasses; i++) {
		std::string prefix = StringFromFormat("shader%d", i);
		std::string path;
		if (!find(prefix, &path)) {
			errors->push_back("Missing key: " + prefix);
			return false;
		}

		SlangPass pass;
		pass.source = baseDir + path;
		// shader path might be absolute.
		if (!path.empty() && (path[0] == '/' || (path.size() > 2 && path[1] == ':')))
			pass.source = path;

		if (find(prefix + "_filter_linear", &id))
			pass.filterLinear = ParseBool(id);
		if (find(prefix + "_wrap_mode", &id))
			pass.wrap = ParseWrap(id);
		if (find(prefix + "_float_framebuffer", &id))
			pass.fpFbo = ParseBool(id);
		if (find(prefix + "_srgb_framebuffer", &id))
			pass.srgbFbo = ParseBool(id);
		if (find(prefix + "_alias", &id))
			pass.alias = id;

		preset->passes.push_back(pass);
	}

	// Parameters. Can be declared in the preset (parameters = "a; b" + per-parameter keys).
	std::vector<std::string> paramNames;
	if (find("parameters", &id)) {
		SplitSemicolon(id, &paramNames);
		for (auto &name : paramNames) {
			SlangParameter param{};
			strncpy(param.id, name.c_str(), sizeof(param.id) - 1);
			strncpy(param.desc, name.c_str(), sizeof(param.desc) - 1);
			param.initial = 0.0f;
			param.minimum = 0.0f;
			param.maximum = 1.0f;
			param.step = 0.01f;

			std::string v;
			if (find(name, &v))
				param.initial = ParseFloat(v, 0.0f);
			if (find(name + "_min", &v))
				param.minimum = ParseFloat(v, 0.0f);
			if (find(name + "_max", &v))
				param.maximum = ParseFloat(v, 1.0f);
			if (find(name + "_step", &v))
				param.step = ParseFloat(v, 0.01f);
			param.current = param.initial;

			preset->parameters.push_back(param);
		}
	}

	// LUTs.
	std::vector<std::string> textureNames;
	if (find("textures", &id)) {
		SplitSemicolon(id, &textureNames);
		for (auto &name : textureNames) {
			std::string path;
			if (!find(name, &path))
				continue;
			SlangLut lut;
			lut.id = name;
			lut.path = baseDir + path;
			std::string filterId;
			if (find(name + "_filter_linear", &filterId))
				lut.filterLinear = ParseBool(filterId);
			if (find(name + "_wrap_mode", &filterId))
				lut.wrap = ParseWrap(filterId);
			preset->luts.push_back(std::move(lut));
		}
	}

	if (preset->passes.empty()) {
		errors->push_back("Preset contains no shader passes.");
		return false;
	}

	return true;
}
