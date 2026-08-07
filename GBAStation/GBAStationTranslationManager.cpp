#include "GBAStationTranslationManager.h"

#include "GBAStation/GBAStationConfig.h"

#include "dep/nlohmann/json.hpp"

#include <array>
#include <fstream>
#include <utility>

namespace GBAStation {
namespace {

using json = nlohmann::json;

constexpr const char *kDefaultLanguage = "English";

const std::pair<const char *, const char *> kEnglishFallback[] = {
	{"emulator_save_state", "Save State"},
	{"emulator_load_state", "Load State"},
	{"emulator_cheats", "Cheats"},
	{"emulator_settings", "Settings"},
	{"emulator_exit_game", "Exit Game"},
	{"emulator_slot", "Slot %d (%s)"},
	{"emulator_in_use", "In Use"},
	{"emulator_empty", "Empty"},
	{"emulator_enabled", "Enabled"},
	{"emulator_disabled", "Disabled"},
	{"emulator_cheat", "Cheat"},
	{"emulator_no_cheat_db", "No DB"},
	{"emulator_no_cheats", "No Cheats"},
	{"emulator_display_mode", "Display Mode"},
	{"emulator_size", "Size"},
	{"emulator_reset", "Reset"},
	{"emulator_integer", "Integer"},
	{"emulator_display", "Display"},
	{"emulator_select", "Select"},
	{"emulator_toggle", "Toggle"},
	{"emulator_back", "Back"},
	{"emulator_change", "Change"},
	{"emulator_auto", "Auto"},
	{"emulator_stretch", "Stretch"},
	{"emulator_original", "Original"},
	{"ra_title", "RetroAchievements"},
	{"ra_login_failed", "Login failed. Check your connection or credentials."},
	{"ra_missing_credentials", "Missing username/password or token."},
	{"ra_auth_failed", "Failed to authenticate."},
	{"ra_playing", "Playing: %s"},
	{"ra_game_identified", "Game identified."},
	{"ra_game_unsupported", "Rom hash doesn't match or game is unsupported."},
	{"ra_hash_support_disabled", "Hash support is not enabled in rcheevos."},
	{"ra_hardcore_mode", "Hardcore Mode"},
	{"ra_hardcore_savestate_disabled", "Save states are disabled in Hardcore Mode."},
	{"ra_game_mastered", "Game Mastered!"},
	{"ra_all_achievements_unlocked", "All achievements unlocked!"},
	{"ra_subset_completed", "Subset Completed"},
	{"ra_all_subset_achievements_unlocked", "All subset achievements unlocked."},
	{"ra_leaderboard_started", "Leaderboard Started"},
	{"ra_leaderboard_failed", "Leaderboard Failed"},
	{"ra_leaderboard", "Leaderboard"},
	{"ra_offline", "Offline. Unlocks will sync when reconnected."},
	{"ra_reconnected", "Reconnected."},
};

}  // namespace

TranslationManager &TranslationManager::Instance() {
	static TranslationManager instance;
	return instance;
}

bool TranslationManager::Init(LogCallback log) {
	if (log) {
		log_ = std::move(log);
	}

	const std::string language = ReadConfiguredLanguage();
	if (language == currentLanguage_ && !translations_.empty()) {
		return true;
	}

	currentLanguage_ = language;
	translations_.clear();
	const std::string fileName = LanguageFileName(language);
	if (!LoadLanguageFile(fileName)) {
		LogMessage(log_, "GBAStation translations fallback language=%s file=%s", language.c_str(), fileName.c_str());
		LoadEnglishFallback();
		return false;
	}

	LogMessage(log_, "GBAStation translations loaded language=%s file=%s count=%u",
		language.c_str(), fileName.c_str(), (unsigned)translations_.size());
	return true;
}

std::string TranslationManager::ReadConfiguredLanguage() const {
	// Prefer the launcher's UI.language (config.cfg, values zh-CN / en-US).
	const char *const uiConfigPaths[] = {
		"sdmc:/GBAStation/config/config.cfg",
		"/GBAStation/config/config.cfg",
	};

	for (const char *path : uiConfigPaths) {
		std::ifstream file(path);
		if (!file.good()) {
			continue;
		}

		std::string line;
		while (std::getline(file, line)) {
			const size_t equals = line.find('=');
			if (equals == std::string::npos) {
				continue;
			}
			const std::string key = line.substr(0, equals);
			if (key != "UI.language") {
				continue;
			}
			std::string value = line.substr(equals + 1);
			if (!value.empty() && value.front() == '"' && value.back() == '"' && value.size() >= 2) {
				value = value.substr(1, value.size() - 2);
			}
			// Launcher config.cfg stores strings with an "s|" type prefix.
			if (value.size() >= 2 && value[0] == 's' && value[1] == '|') {
				value = value.substr(2);
			}
			if (value == "en-US" || value == "en") {
				return "English";
			}
			if (value == "zh-CN" || value == "zh-Hans" || value == "zh") {
				return "Chinese";
			}
		}
	}

	// Fall back to general.jsonc "language" for standalone launches.
	const char *const configPaths[] = {
		"sdmc:/GBAStation/config/general.jsonc",
	};

	for (const char *path : configPaths) {
		std::ifstream file(path);
		if (!file.good()) {
			continue;
		}

		json config = json::parse(file, nullptr, false, true);
		if (!config.is_discarded() && config.contains("language") && config["language"].is_string()) {
			const std::string language = config["language"].get<std::string>();
			if (!language.empty()) {
				return language;
			}
		}
	}

	return kDefaultLanguage;
}

std::string TranslationManager::LanguageFileName(const std::string &language) const {
	if (language == "Portuguese" || language == "Portugues" || language == "pt") {
		return "pt.json";
	}
	if (language == "Espanol" || language == "Spanish" || language == "es") {
		return "es.json";
	}
	if (language == "Japanese" || language == "ja") {
		return "ja.json";
	}
	if (language == "French" || language == "fr") {
		return "fr.json";
	}
	if (language == "German" || language == "Deutsch" || language == "de") {
		return "de.json";
	}
	if (language == "Russian" || language == "Russkiy" || language == "ru") {
		return "ru.json";
	}
	if (language == "Chinese" || language == "Chinese Traditional" || language == "Chinese Simplified" || language == "zh") {
		return "zh.json";
	}
	return "en.json";
}

bool TranslationManager::LoadLanguageFile(const std::string &fileName) {
	const std::array<std::string, 3> paths = {{
		std::string(Paths::Lang) + "/" + fileName,
		std::string("sdmc:/GBAStation/PSP/assets/lang/") + fileName,
		std::string("romfs:/lang/") + fileName,
	}};

	for (const std::string &path : paths) {
		std::ifstream file(path);
		if (!file.good()) {
			continue;
		}

		json parsed = json::parse(file, nullptr, false, true);
		if (parsed.is_discarded() || !parsed.is_object()) {
			LogMessage(log_, "GBAStation translations parse failed path=%s", path.c_str());
			continue;
		}

		for (const auto &entry : parsed.items()) {
			if (entry.value().is_string()) {
				translations_[entry.key()] = entry.value().get<std::string>();
			}
		}
		return !translations_.empty();
	}

	return false;
}

void TranslationManager::LoadEnglishFallback() {
	translations_.clear();
	for (const auto &entry : kEnglishFallback) {
		translations_[entry.first] = entry.second;
	}
	currentLanguage_ = kDefaultLanguage;
}

std::string TranslationManager::GetString(const std::string &key) const {
	const auto it = translations_.find(key);
	return it == translations_.end() ? key : it->second;
}

std::string tr(const std::string &key) {
	return TranslationManager::Instance().GetString(key);
}

std::string tr(const char *key) {
	return key ? TranslationManager::Instance().GetString(key) : std::string();
}

}  // namespace GBAStation
