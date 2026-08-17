#include "PpssppRuntime.h"

#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(SWITCH)

#if !defined(PPSSPP_SWITCH_VULKAN_ONLY)
#error "GBAStation PPSSPP runtime is Vulkan-only. Build with PPSSPP_SWITCH_VULKAN_ONLY."
#endif

#include "GBAStation/GBAStationGraphicsHost.h"
#include "GBAStation/PpssppGBAStationConfig.h"
#include "GBAStation/GBAStationAssetInstaller.h"
#include "GBAStation/GBAStationAudioSfx.h"
#include "GBAStation/GBAStationOverlay.h"
#include "GBAStation/GBAStationRetroAchievements.h"

#include <sys/stat.h>
#include <ctime>

#include "Common/CPUDetect.h"
#include "Common/File/FileUtil.h"
#include "Common/File/VFS/DirectoryReader.h"
#include "Common/File/VFS/VFS.h"
#include "Common/GPU/thin3d.h"
#include "Common/GraphicsContext.h"
#include "Common/Log/LogManager.h"
#include "Common/Profiler/Profiler.h"
#include "Common/StringUtils.h"
#include "Common/System/Display.h"
#include "Common/System/Request.h"
#include "Common/System/System.h"
#include "Common/Thread/ThreadManager.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/CoreParameter.h"
#include "Core/CoreTiming.h"
#include "Core/CwCheat.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/ELF/PBPReader.h"
#include "Core/FileSystems/MetaFileSystem.h"
#include "Core/Loaders.h"
#include "Core/FrameTiming.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/sceDisplay.h"
#include "Core/HLE/sceUtility.h"
#include "Core/HW/Display.h"
#include "Core/HW/StereoResampler.h"
#include "Core/MemMapHelpers.h"
#include "Core/SaveState.h"
#include "dep/nlohmann/json.hpp"
#include "Core/Screenshot.h"
#include "GPU/Common/FramebufferManagerCommon.h"
#include "GPU/Common/PostShader.h"
#include "GPU/GPU.h"
#include "Core/Screenshot.h"
#include "Core/System.h"
#include "Core/Util/PathUtil.h"
#include "GPU/GPUCommon.h"

#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <zlib.h>

namespace GBAStation {
static void WriteConfigValue(const char *key, const std::string &value);
namespace {

constexpr int kAudioSampleRate = 48000;
constexpr int kAudioSamples = 1024;
constexpr int kAudioChannels = 2;
constexpr int kAudioBytesPerSample = sizeof(s16);
constexpr int kAudioBufferCount = 4;
constexpr size_t kAudioDataBytes = kAudioSamples * kAudioChannels * kAudioBytesPerSample;
constexpr size_t kAudioBufferBytes = (kAudioDataBytes + 0xFFF) & ~(size_t)0xFFF;
constexpr size_t kAudioThreadStackSize = 0x10000;
constexpr int kAudioThreadPriority = 0x28;
constexpr int kAudioThreadCore = 1;
constexpr size_t kCheatLoadThreadStackSize = 0x20000;
constexpr int kCheatLoadThreadPriority = 0x3B;
constexpr int kCheatLoadThreadCore = -2;
constexpr size_t kCheatMetadataLineChars = 68;

struct RuntimeState {
	LogCallback log;
	bool running = true;
	bool booted = false;
	bool frameOpen = false;
	bool hostFrameOpen = false;
	bool ppssppShutdown = false;
	// A reset requested while a PSP savedata dialog is completing is held until
	// the dialog has finished its worker-thread write and guest handshake.
	bool resetPending = false;
	bool chainloadLauncher = false;
	bool runtimeSettingsDirty = false;
	bool runtimeSettingsSavePending = false;
	bool gameDisplaySettingsSavePending = false;
	bool settingsRenderResized = false;
	bool settingsJitClear = false;
	std::string contentPath;
	// Play stats + auto save/load (launcher config.cfg keys).
	int playCount = 0;
	int playTimeTotal = 0;
	bool playStatsFound = false;
	int sessionPlaySeconds = 0;
	double playTimeFraction = 0.0;
	double playTimeLastMs = 0.0;
	int autoLoadStateSlot = 0;
	int autoSaveOnExitSlot = 0;
	// A menu-initiated exit keeps the normal frame loop alive until its queued
	// SaveState operation and thumbnail PNG have both finished.
	bool exitSavePending = false;
	bool exitSaveFinished = false;
	u64 exitSaveStartedMs = 0;
	std::string exitSavePath;
	// Native PSP saves use sceUtility's worker thread and guest shutdown
	// handshake.  Exit waits for it before pausing the core for our savestate.
	bool exitWaitingForNativeSave = false;
	u64 exitNativeSaveClearMs = 0;
	std::string savePath; // per-game save dir from the launcher GameDB (savePath field)
	// Cached GameDB entry (loaded once at boot; title/logo updates mutate it in
	// memory and a single flush writes it back).
	nlohmann::json gameDbData;
	size_t gameDbIndex = 0;
	bool gameDbLoaded = false;
	bool gameDbDirty = false;
	std::string gameDbPath;
	GBAStationGraphicsHost graphicsHost;
	GraphicsContext *graphicsContext = nullptr;
	Draw::DrawContext *draw = nullptr;
	Overlay overlay;
	DisplaySettings displaySettings;
	bool displaySettingsLoaded = false;
	std::mutex mainThreadMutex;
	std::vector<std::function<void()>> mainThreadQueue;
	StereoResampler resampler;
	std::array<bool, Ppsspp::SaveStateSlotCount> saveStateSlots{};
	u64 lastSaveStateScanMs = 0;
	u32 lastPspButtons = 0;
	bool audioReady = false;
	int audioSampleRate = kAudioSampleRate;
	int audioBufferSamples = kAudioSamples;
	AudioOutBuffer audioBuffers[kAudioBufferCount]{};
	u8 *audioBufferData[kAudioBufferCount]{};
	Thread audioThread{};
	std::atomic<bool> audioThreadStop{false};
	bool audioThreadCreated = false;
	Thread cheatLoadThread{};
	std::atomic<bool> cheatLoadThreadActive{false};
	bool cheatLoadThreadCreated = false;
	int frameCount = 0;
	std::unordered_map<std::string, std::string> gbastationConfig;
	// Fast forward: toggle latch + multiplier (from launcher config.cfg).
	bool fastForwardToggle = false;
	bool fastForwardToggleMode = false;
	float fastForwardMultiplier = 2.0f;
	bool fastForwardSettingsSavePending = false;
	// State thumbnail: captured two frames after the menu closes (pure gameplay).
	// Menu-open thumbnail: captured into memory when the menu button is pressed,
	// written to a PNG next to the state file when a state is saved.
	std::vector<uint8_t> thumbMemory;
	uint32_t thumbW = 0;
	uint32_t thumbH = 0;
	bool menuPendingThumb = false;
	// Frames to suppress game input after the menu closes (button bleed-through).
	int pspInputSuppressFrames = 0;
	bool fastForwardActive = false;
	// FPS counter for the HUD.
	double fps = 60.0;
};

RuntimeState g_state;

void Log(const char *fmt, ...) {
	if (!g_state.log || !fmt) {
		return;
	}

	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	g_state.log(buffer);
}

void DrainMainThreadQueue() {
	std::vector<std::function<void()>> work;
	{
		std::lock_guard<std::mutex> lock(g_state.mainThreadMutex);
		work.swap(g_state.mainThreadQueue);
	}
	for (auto &fn : work) {
		fn();
	}
}

void QueueMainThread(std::function<void()> func) {
	std::lock_guard<std::mutex> lock(g_state.mainThreadMutex);
	g_state.mainThreadQueue.push_back(std::move(func));
}

float ClampAnalog(float value) {
	return std::clamp(value, -1.0f, 1.0f);
}

float NormalizeStickAxis(int value) {
	float normalized = ClampAnalog(value / 32767.0f);
	if (std::fabs(normalized) < g_Config.fAnalogDeadzone) {
		return 0.0f;
	}
	return ClampAnalog(normalized * g_Config.fAnalogSensitivity);
}

std::string Trim(std::string_view value) {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos) {
		return {};
	}
	const auto last = value.find_last_not_of(" \t\r\n");
	return std::string(value.substr(first, last - first + 1));
}

std::string DecodeConfigValue(std::string_view encoded) {
	std::string value = Trim(encoded);
	if (value.size() > 2 && value[1] == '|') {
		const char type = value[0];
		value.erase(0, 2);
		// Boolean and numeric launcher settings use b| / i| / f|.  They do
		// not need string unescaping, but the type prefix must never leak into
		// consumers such as display.showFps.
		if (type != 's') {
			return value;
		}
		std::string decoded;
		decoded.reserve(value.size());
		bool escaped = false;
		for (char c : value) {
			if (escaped) {
				decoded.push_back(c);
				escaped = false;
			} else if (c == '\\') {
				escaped = true;
			} else {
				decoded.push_back(c);
			}
		}
		if (escaped) {
			decoded.push_back('\\');
		}
		return decoded;
	}
	return value;
}

void LoadGBAStationConfig() {
	g_state.gbastationConfig.clear();
	const char *paths[] = {"sdmc:/GBAStation/config/config.cfg", "/GBAStation/config/config.cfg"};
	for (const char *path : paths) {
		std::ifstream in(path);
		if (!in) {
			continue;
		}
		std::string line;
		while (std::getline(in, line)) {
			const std::size_t equal = line.find('=');
			if (equal == std::string::npos) {
				continue;
			}
			g_state.gbastationConfig[Trim(std::string_view(line).substr(0, equal))] =
				DecodeConfigValue(std::string_view(line).substr(equal + 1));
		}
		Log("loaded GBAStation config path=%s values=%u", path,
			(unsigned)g_state.gbastationConfig.size());
		break;
	}

	// Fast forward defaults from the launcher (same keys as the 3DS core).
	if (const auto it = g_state.gbastationConfig.find("fastforward.multiplier");
		it != g_state.gbastationConfig.end()) {
		try {
			const float value = std::stof(it->second);
			g_state.fastForwardMultiplier = value <= 0.001f ? 0.0f : std::clamp(value, 0.5f, 5.0f);
		} catch (...) {
		}
	}
	if (const auto it = g_state.gbastationConfig.find("fastforward.mode");
		it != g_state.gbastationConfig.end()) {
		g_state.fastForwardToggleMode = (it->second == "toggle");
	}
}

u64 TokenHidMask(std::string_view token) {
	const std::string t = Trim(token);
	if (t == "PAD_A") return HidNpadButton_A;
	if (t == "PAD_B") return HidNpadButton_B;
	if (t == "PAD_X") return HidNpadButton_X;
	if (t == "PAD_Y") return HidNpadButton_Y;
	if (t == "PAD_UP") return HidNpadButton_Up;
	if (t == "PAD_DOWN") return HidNpadButton_Down;
	if (t == "PAD_LEFT") return HidNpadButton_Left;
	if (t == "PAD_RIGHT") return HidNpadButton_Right;
	if (t == "PAD_LB") return HidNpadButton_L;
	if (t == "PAD_RB") return HidNpadButton_R;
	if (t == "PAD_LT" || t == "PAD_ZL") return HidNpadButton_ZL;
	if (t == "PAD_RT" || t == "PAD_ZR") return HidNpadButton_ZR;
	if (t == "PAD_START") return HidNpadButton_Plus;
	if (t == "PAD_BACK") return HidNpadButton_Minus;
	if (t == "PAD_LSB" || t == "PAD_L3") return HidNpadButton_StickL;
	if (t == "PAD_RSB" || t == "PAD_R3") return HidNpadButton_StickR;
	if (t == "PAD_LEFTSTICKUP") return HidNpadButton_StickLUp;
	if (t == "PAD_LEFTSTICKDOWN") return HidNpadButton_StickLDown;
	if (t == "PAD_LEFTSTICKLEFT") return HidNpadButton_StickLLeft;
	if (t == "PAD_LEFTSTICKRIGHT") return HidNpadButton_StickLRight;
	if (t == "PAD_RIGHTSTICKUP") return HidNpadButton_StickRUp;
	if (t == "PAD_RIGHTSTICKDOWN") return HidNpadButton_StickRDown;
	if (t == "PAD_RIGHTSTICKLEFT") return HidNpadButton_StickRLeft;
	if (t == "PAD_RIGHTSTICKRIGHT") return HidNpadButton_StickRRight;
	return 0;
}

u64 ParseComboMask(std::string_view combo) {
	const std::string value = Trim(combo);
	if (value.empty() || value == "none") {
		return 0;
	}
	u64 mask = 0;
	std::size_t begin = 0;
	while (begin < value.size()) {
		const std::size_t end = value.find('+', begin);
		const std::string_view token = std::string_view(value).substr(
			begin, end == std::string::npos ? value.size() - begin : end - begin);
		mask |= TokenHidMask(token);
		if (end == std::string::npos) {
			break;
		}
		begin = end + 1;
	}
	return mask;
}

std::vector<u64> BindingMasks(const char *key, const char *fallback) {
	const auto it = g_state.gbastationConfig.find(key);
	const std::string value = it == g_state.gbastationConfig.end() ? fallback : it->second;
	std::vector<u64> masks;
	std::size_t begin = 0;
	while (begin <= value.size()) {
		const std::size_t end = value.find('|', begin);
		const std::string_view combo = std::string_view(value).substr(
			begin, end == std::string::npos ? value.size() - begin : end - begin);
		const u64 mask = ParseComboMask(combo);
		if (mask != 0) {
			masks.push_back(mask);
		}
		if (end == std::string::npos) {
			break;
		}
		begin = end + 1;
	}
	return masks;
}

bool BindingHeld(const char *key, const char *fallback, u64 held) {
	for (const u64 mask : BindingMasks(key, fallback)) {
		if ((held & mask) == mask) {
			return true;
		}
	}
	return false;
}

bool BindingPressedEdge(const char *key, const char *fallback, u64 held, u64 pressed) {
	for (const u64 mask : BindingMasks(key, fallback)) {
		if ((held & mask) == mask && (pressed & mask) != 0) {
			return true;
		}
	}
	return false;
}

std::string ConfigValue(const char *key, const char *fallback) {
	const auto it = g_state.gbastationConfig.find(key);
	return it == g_state.gbastationConfig.end() ? std::string(fallback) : it->second;
}

bool ConfigBool(const char *key, bool fallback) {
	const std::string value = ConfigValue(key, fallback ? "1" : "0");
	std::string lower;
	lower.reserve(value.size());
	for (char c : value) {
		lower.push_back((char)std::tolower((unsigned char)c));
	}
	return lower != "0" && lower != "false" && lower != "off" && lower != "disabled" && lower != "no";
}

int ConfigInt(const char *key, int fallback) {
	const std::string value = ConfigValue(key, "");
	char *end = nullptr;
	const long parsed = std::strtol(value.c_str(), &end, 10);
	return end != value.c_str() ? (int)parsed : fallback;
}

void ApplyGBAStationPpssppCoreSettings() {
	g_Config.iInternalResolution =
		std::clamp(ConfigInt("core.ppsspp.rendering_resolution", g_Config.iInternalResolution), 1, 10);
	g_Config.iFrameSkip =
		std::clamp(ConfigInt("core.ppsspp.frameskip", g_Config.iFrameSkip), 0, 8);
	g_Config.bAutoFrameSkip = ConfigBool("core.ppsspp.auto_frameskip", g_Config.bAutoFrameSkip);
	g_Config.bRenderDuplicateFrames = ConfigBool("core.ppsspp.render_duplicate_frames", g_Config.bRenderDuplicateFrames);
	g_Config.bFastMemory = ConfigBool("core.ppsspp.fast_memory", g_Config.bFastMemory);
	g_Config.iIOTimingMethod = ConfigBool("core.ppsspp.io_thread", true) ? IOTIMING_FAST : IOTIMING_HOST;
	Log("applied GBAStation PPSSPP config res=%d frameskip=%d auto=%d fastmem=%d io=%d",
		g_Config.iInternalResolution, g_Config.iFrameSkip, g_Config.bAutoFrameSkip ? 1 : 0,
		g_Config.bFastMemory ? 1 : 0, g_Config.iIOTimingMethod);
}

void SaveGBAStationPpssppRuntimeSettings() {
	PpssppCoreConfig config(g_state.log);
	config.Load();
	CoreConfig &raw = config.RawConfig();
	auto enabled = [](bool value) { return value ? "enabled" : "disabled"; };
	static constexpr const char *kFiltering[] = {"Auto", "Auto", "Nearest", "Linear", "Auto max quality"};
	static constexpr const char *kAnisotropy[] = {"Off", "2x", "4x", "8x", "16x"};

	raw.SetValue("ppsspp_internal_resolution", std::to_string(std::max(0, g_Config.iInternalResolution)));
	raw.SetValue("ppsspp_frameskip", std::to_string(std::max(0, g_Config.iFrameSkip)));
	raw.SetValue("ppsspp_auto_frameskip", enabled(g_Config.bAutoFrameSkip));
	raw.SetValue("ppsspp_render_duplicate_frames", enabled(g_Config.bRenderDuplicateFrames));
	raw.SetValue("ppsspp_fast_memory", enabled(g_Config.bFastMemory));
	raw.SetValue("ppsspp_gpu_hardware_transform", enabled(g_Config.bHardwareTransform));
	raw.SetValue("ppsspp_skip_buffer_effects", enabled(g_Config.bSkipBufferEffects));
	raw.SetValue("ppsspp_vsync", enabled(g_Config.bVSync));
	raw.SetValue("ppsspp_texture_filtering", kFiltering[std::clamp(g_Config.iTexFiltering, 0, 4)]);
	raw.SetValue("ppsspp_texture_anisotropic_filtering", kAnisotropy[std::clamp(g_Config.iAnisotropyLevel, 0, 4)]);
	raw.SetValue("ppsspp_texture_deposterize", enabled(g_Config.bTexDeposterize));
	raw.Save();
	WriteConfigValue("core.ppsspp.rendering_resolution", std::to_string(std::max(1, g_Config.iInternalResolution)));
	WriteConfigValue("core.ppsspp.frameskip", std::to_string(std::max(0, g_Config.iFrameSkip)));
	WriteConfigValue("core.ppsspp.auto_frameskip", enabled(g_Config.bAutoFrameSkip));
	WriteConfigValue("core.ppsspp.render_duplicate_frames", enabled(g_Config.bRenderDuplicateFrames));
	WriteConfigValue("core.ppsspp.fast_memory", enabled(g_Config.bFastMemory));
	WriteConfigValue("core.ppsspp.ppsspp_gpu_hardware_transform", enabled(g_Config.bHardwareTransform));
	WriteConfigValue("core.ppsspp.ppsspp_skip_buffer_effects", enabled(g_Config.bSkipBufferEffects));
	WriteConfigValue("core.ppsspp.ppsspp_vsync", enabled(g_Config.bVSync));
	WriteConfigValue("core.ppsspp.ppsspp_texture_filtering", kFiltering[std::clamp(g_Config.iTexFiltering, 0, 4)]);
	WriteConfigValue("core.ppsspp.ppsspp_texture_anisotropic_filtering", kAnisotropy[std::clamp(g_Config.iAnisotropyLevel, 0, 4)]);
	WriteConfigValue("core.ppsspp.ppsspp_texture_deposterize", enabled(g_Config.bTexDeposterize));
	Log("saved PPSSPP runtime core settings");
}

void ApplyGBAStationPpssppDisplaySettings(DisplaySettings &settings) {
	const std::string mode = ConfigValue("core.ppsspp.display_mode", "");
	if (mode == "Integer") {
		settings.mode = DisplayMode::Integer;
	} else if (mode == "Custom") {
		settings.mode = DisplayMode::Custom;
	} else if (mode == "Display") {
		settings.mode = DisplayMode::Display;
	}

	const std::string size = ConfigValue("core.ppsspp.display_size", "");
	if (size == "Stretch") settings.size = DisplaySize::Stretch;
	else if (size == "4:3") settings.size = DisplaySize::_4_3;
	else if (size == "16:9") settings.size = DisplaySize::_16_9;
	else if (size == "Original") settings.size = DisplaySize::Original;
	else if (size == "1x") settings.size = DisplaySize::_1x;
	else if (size == "2x") settings.size = DisplaySize::_2x;
	else if (size == "3x") settings.size = DisplaySize::_3x;
	else if (size == "4x") settings.size = DisplaySize::_4x;
	else if (size == "Auto") settings.size = DisplaySize::Auto;

	settings = NormalizePpssppDisplaySettingsForCurrentMode(settings);
	Log("applied GBAStation PPSSPP display mode=%s size=%s",
		mode.empty() ? "(core-config)" : mode.c_str(),
		size.empty() ? "(core-config)" : size.c_str());
}

void ClearPspInput() {
	if (g_state.lastPspButtons != 0) {
		__CtrlUpdateButtons(0, g_state.lastPspButtons);
		g_state.lastPspButtons = 0;
	}
	__CtrlSetAnalogXY(CTRL_STICK_LEFT, 0.0f, 0.0f);
	__CtrlSetAnalogXY(CTRL_STICK_RIGHT, 0.0f, 0.0f);
}

void UpdatePspInput(const FrameInput &input) {
	u32 currentButtons = 0;
	const u64 buttons = input.buttons;
	if (BindingHeld("psp.handle.b", "PAD_B", buttons)) currentButtons |= CTRL_CROSS;
	if (BindingHeld("psp.handle.a", "PAD_A", buttons)) currentButtons |= CTRL_CIRCLE;
	if (BindingHeld("psp.handle.y", "PAD_Y", buttons)) currentButtons |= CTRL_SQUARE;
	if (BindingHeld("psp.handle.x", "PAD_X", buttons)) currentButtons |= CTRL_TRIANGLE;
	if (BindingHeld("psp.handle.up", "PAD_UP", buttons)) currentButtons |= CTRL_UP;
	if (BindingHeld("psp.handle.down", "PAD_DOWN", buttons)) currentButtons |= CTRL_DOWN;
	if (BindingHeld("psp.handle.left", "PAD_LEFT", buttons)) currentButtons |= CTRL_LEFT;
	if (BindingHeld("psp.handle.right", "PAD_RIGHT", buttons)) currentButtons |= CTRL_RIGHT;
	if (BindingHeld("psp.handle.start", "PAD_START", buttons)) currentButtons |= CTRL_START;
	if (BindingHeld("psp.handle.select", "PAD_BACK", buttons)) currentButtons |= CTRL_SELECT;
	if (BindingHeld("psp.handle.l", "PAD_LB", buttons)) currentButtons |= CTRL_LTRIGGER;
	if (BindingHeld("psp.handle.r", "PAD_RB", buttons)) currentButtons |= CTRL_RTRIGGER;
	// The PSP has no L2/R2/L3/R3 inputs; those button bindings were copied
	// from another core and must not be parsed.
	__CtrlUpdateButtons(currentButtons & ~g_state.lastPspButtons, g_state.lastPspButtons & ~currentButtons);
	g_state.lastPspButtons = currentButtons;
	__CtrlSetAnalogXY(CTRL_STICK_LEFT, NormalizeStickAxis(input.leftStickX), NormalizeStickAxis(input.leftStickY));
	__CtrlSetAnalogXY(CTRL_STICK_RIGHT, 0.0f, 0.0f);
}

void FreeAudioBuffers() {
	for (int i = 0; i < kAudioBufferCount; ++i) {
		free(g_state.audioBufferData[i]);
		g_state.audioBufferData[i] = nullptr;
		g_state.audioBuffers[i] = {};
	}
}

void FillAudioBuffer(AudioOutBuffer *buffer) {
	if (!buffer || !buffer->buffer) {
		return;
	}

	if (g_state.audioReady) {
		g_state.resampler.Mix(reinterpret_cast<s16 *>(buffer->buffer), kAudioSamples, false, g_state.audioSampleRate);
	} else {
		std::memset(buffer->buffer, 0, kAudioDataBytes);
	}
	TrophySfx().Mix(reinterpret_cast<s16 *>(buffer->buffer), kAudioSamples, g_state.audioSampleRate);
	buffer->data_offset = 0;
	buffer->data_size = kAudioDataBytes;
	armDCacheClean(buffer->buffer, buffer->buffer_size);
}

void AudioThreadEntry(void *) {
	SetCurrentThreadName("GBAStationAudio");
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, kAudioThreadCore, 1ULL << kAudioThreadCore);

	while (!g_state.audioThreadStop.load(std::memory_order_acquire)) {
		AudioOutBuffer *releasedBuffer = nullptr;
		u32 releasedCount = 0;
		Result rc = audoutWaitPlayFinish(&releasedBuffer, &releasedCount, 100000000ULL);
		if (g_state.audioThreadStop.load(std::memory_order_acquire)) {
			break;
		}
		if (R_FAILED(rc) || releasedCount == 0 || !releasedBuffer) {
			continue;
		}

		FillAudioBuffer(releasedBuffer);
		rc = audoutAppendAudioOutBuffer(releasedBuffer);
		if (R_FAILED(rc)) {
			break;
		}
	}
}

bool InitAudio() {
	g_state.audioReady = false;
	g_state.audioSampleRate = kAudioSampleRate;
	g_state.audioBufferSamples = kAudioSamples;

	Result rc = audoutInitialize();
	if (R_FAILED(rc)) {
		Log("audoutInitialize failed rc=0x%x", (unsigned)rc);
		return false;
	}

	const u32 sampleRate = audoutGetSampleRate();
	const u32 channelCount = audoutGetChannelCount();
	const PcmFormat pcmFormat = audoutGetPcmFormat();
	Log("audout device rate=%u channels=0x%x format=%d state=%d",
		(unsigned)sampleRate, (unsigned)channelCount, (int)pcmFormat, (int)audoutGetDeviceState());
	if (sampleRate == 0 || pcmFormat != PcmFormat_Int16) {
		Log("audout unsupported format");
		audoutExit();
		return false;
	}
	g_state.audioSampleRate = (int)sampleRate;

	for (int i = 0; i < kAudioBufferCount; ++i) {
		g_state.audioBufferData[i] = static_cast<u8 *>(memalign(0x1000, kAudioBufferBytes));
		if (!g_state.audioBufferData[i]) {
			Log("audout buffer allocation failed index=%d", i);
			FreeAudioBuffers();
			audoutExit();
			return false;
		}
		std::memset(g_state.audioBufferData[i], 0, kAudioBufferBytes);
		g_state.audioBuffers[i].next = nullptr;
		g_state.audioBuffers[i].buffer = g_state.audioBufferData[i];
		g_state.audioBuffers[i].buffer_size = kAudioBufferBytes;
		g_state.audioBuffers[i].data_size = kAudioDataBytes;
		g_state.audioBuffers[i].data_offset = 0;
		armDCacheClean(g_state.audioBuffers[i].buffer, g_state.audioBuffers[i].buffer_size);
	}

	rc = audoutStartAudioOut();
	if (R_FAILED(rc)) {
		Log("audoutStartAudioOut failed rc=0x%x", (unsigned)rc);
		FreeAudioBuffers();
		audoutExit();
		return false;
	}

	for (int i = 0; i < kAudioBufferCount; ++i) {
		rc = audoutAppendAudioOutBuffer(&g_state.audioBuffers[i]);
		if (R_FAILED(rc)) {
			Log("audoutAppendAudioOutBuffer failed index=%d rc=0x%x", i, (unsigned)rc);
			audoutStopAudioOut();
			FreeAudioBuffers();
			audoutExit();
			return false;
		}
	}

	g_state.audioThreadStop.store(false, std::memory_order_release);
	rc = threadCreate(&g_state.audioThread, AudioThreadEntry, nullptr, nullptr, kAudioThreadStackSize, kAudioThreadPriority, kAudioThreadCore);
	if (R_FAILED(rc)) {
		Log("audio threadCreate failed rc=0x%x", (unsigned)rc);
		audoutStopAudioOut();
		FreeAudioBuffers();
		audoutExit();
		return false;
	}
	g_state.audioThreadCreated = true;

	rc = threadStart(&g_state.audioThread);
	if (R_FAILED(rc)) {
		Log("audio threadStart failed rc=0x%x", (unsigned)rc);
		threadClose(&g_state.audioThread);
		g_state.audioThreadCreated = false;
		audoutStopAudioOut();
		FreeAudioBuffers();
		audoutExit();
		return false;
	}

	g_state.audioReady = true;
	g_state.audioBufferSamples = kAudioSamples;
	return true;
}

void ShutdownAudio() {
	g_state.audioReady = false;
	g_state.audioThreadStop.store(true, std::memory_order_release);
	if (g_state.audioThreadCreated) {
		audoutStopAudioOut();
		threadWaitForExit(&g_state.audioThread);
		threadClose(&g_state.audioThread);
		g_state.audioThread = {};
		g_state.audioThreadCreated = false;
	}
	audoutExit();
	FreeAudioBuffers();
	g_state.audioSampleRate = kAudioSampleRate;
	g_state.audioBufferSamples = kAudioSamples;
}

void UpdateDisplayMode() {
	static AppletOperationMode lastMode = (AppletOperationMode)-1;
	const AppletOperationMode mode = appletGetOperationMode();
	if (mode != lastMode) {
		if (mode == AppletOperationMode_Handheld) {
			const Result dimRc = nwindowSetDimensions(nwindowGetDefault(), 1280, 720);
			const Result cropRc = nwindowSetCrop(nwindowGetDefault(), 0, 0, 1280, 720);
			g_display.Recalculate(1280, 720, 1.0f, 1.0f, 1.0f);
			Log("display mode=handheld dim=0x%x crop=0x%x", (unsigned)dimRc, (unsigned)cropRc);
		} else {
			const Result dimRc = nwindowSetDimensions(nwindowGetDefault(), 1920, 1080);
			const Result cropRc = nwindowSetCrop(nwindowGetDefault(), 0, 0, 1920, 1080);
			g_display.Recalculate(1920, 1080, 1.0f, 1.0f, 1.0f);
			Log("display mode=docked dim=0x%x crop=0x%x", (unsigned)dimRc, (unsigned)cropRc);
		}
		g_display.display_hz = 60.0f;
		lastMode = mode;
		if (g_state.displaySettingsLoaded) {
			g_state.displaySettings = LoadPpssppDisplaySettings(g_state.log);
			if (g_state.displaySettings.mode == DisplayMode::Integer) {
				g_state.displaySettings.size = DisplaySize::Auto;
			}
			ApplyGBAStationPpssppDisplaySettings(g_state.displaySettings);
			SavePpssppDisplaySettings(g_state.displaySettings, g_state.log);
			ApplyPpssppDisplaySettings(g_state.displaySettings);
			g_state.overlay.ReloadDisplaySettings();
		}
	}

	if (PSP_GetBootState() == BootState::Complete) {
		PSP_CoreParameter().pixelWidth = g_display.pixel_xres;
		PSP_CoreParameter().pixelHeight = g_display.pixel_yres;
	}
}

void InitializeConfig() {
	g_Config.RestoreDefaults(RestoreSettingsBits::SETTINGS | RestoreSettingsBits::CONTROLS, true);
	PpssppCoreConfig config(g_state.log);
	config.Load();
	// The launcher owns config.cfg.  Mirror every source PPSSPP option exposed
	// there (`core.ppsspp.ppsspp_*`) into the core configuration before it is
	// applied, while preserving the legacy short keys below for compatibility.
	constexpr std::string_view kCorePrefix = "core.ppsspp.";
	unsigned overrideCount = 0;
	for (const auto &entry : g_state.gbastationConfig) {
		if (entry.first.compare(0, kCorePrefix.size(), kCorePrefix) != 0)
			continue;
		const std::string option = entry.first.substr(kCorePrefix.size());
		if (option.rfind("ppsspp_", 0) != 0)
			continue;
		config.RawConfig().SetValue(option, entry.second);
		overrideCount++;
	}
	if (overrideCount)
		Log("applied %u PPSSPP core option overrides from config.cfg", overrideCount);
	config.Apply(g_state.audioReady);
	g_state.displaySettings = LoadPpssppDisplaySettings(g_state.log);
	ApplyGBAStationPpssppDisplaySettings(g_state.displaySettings);
	g_state.displaySettingsLoaded = true;
	SavePpssppDisplaySettings(g_state.displaySettings, g_state.log);
	ApplyPpssppDisplaySettings(g_state.displaySettings);
	CreateSysDirectories();
	g_VFS.Register("", new DirectoryReader(::Path(Paths::PpssppDataRoot)));
	UpdateUIState(UISTATE_INGAME);
}

char *ReadCheatDbLine(char *buffer, int size, FILE *fp) {
	char *line = fgets(buffer, size, fp);
	if (!line) {
		return nullptr;
	}

	size_t length = strlen(line);
	while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
		line[--length] = '\0';
	}
	return line;
}

Path GetCheatDbPath() {
	const Path cheatDir = GetSysDirectory(DIRECTORY_CHEATS);
	const Path cheatDb = cheatDir / "cheat.db";
	if (File::Exists(cheatDb)) {
		return cheatDb;
	}

	const Path cheatsDb = cheatDir / "cheats.db";
	if (File::Exists(cheatsDb)) {
		return cheatsDb;
	}
	return Path();
}

bool CheatNameExists(const std::vector<CheatFileInfo> &fileInfo, const std::string &name) {
	for (const CheatFileInfo &existing : fileInfo) {
		if (existing.name == name) {
			return true;
		}
	}
	return false;
}

bool ImportCheatsFromDb(const Path &cheatDbPath) {
	const std::string gameID = g_paramSFO.GetDiscID();
	if (gameID.length() != 9) {
		Log("GBAStation cheats import skipped invalid game id=%s", gameID.c_str());
		return false;
	}

	CWCheatEngine engine(gameID);
	engine.CreateCheatFile();
	const std::vector<CheatFileInfo> fileInfo = engine.FileInfo();
	const bool hasExistingCheats = !fileInfo.empty();
	const std::string dbGameID = StringFromFormat("_S %s-%s", gameID.substr(0, 4).c_str(), gameID.substr(4).c_str());

	FILE *in = File::OpenCFile(cheatDbPath, "rt");
	if (!in) {
		Log("GBAStation cheats db open failed path=%s", cheatDbPath.ToString().c_str());
		return hasExistingCheats;
	}

	std::vector<std::string> title;
	std::vector<std::string> newList;
	char lineBuffer[2048]{};
	bool parseGameEntry = false;
	bool parseCheatEntry = false;

	while (!feof(in)) {
		char *line = ReadCheatDbLine(lineBuffer, sizeof(lineBuffer), in);
		if (!line || line[0] == '\0') {
			continue;
		}

		if (line[0] == '_' && line[1] == 'S') {
			parseGameEntry = dbGameID == line;
			parseCheatEntry = false;
		} else if (parseGameEntry && line[0] == '_' && line[1] == 'C') {
			parseCheatEntry = !CheatNameExists(fileInfo, std::string(line).substr(4));
		}

		if (!parseGameEntry) {
			if (!newList.empty()) {
				break;
			}
			continue;
		}

		if (line[0] == '_' && (line[1] == 'S' || line[1] == 'G') && title.size() < 2) {
			title.push_back(line);
		} else if (parseCheatEntry && ((line[0] == '_' && (line[1] == 'C' || line[1] == 'L')) || line[0] == '/' || line[0] == '#')) {
			newList.push_back(line);
		}
	}
	fclose(in);

	if (newList.empty()) {
		Log("GBAStation cheats import no new entries path=%s game=%s existing=%u",
			cheatDbPath.ToString().c_str(), gameID.c_str(), (unsigned)fileInfo.size());
		return hasExistingCheats;
	}

	std::string firstCheatLine;
	FILE *existingFile = File::OpenCFile(engine.CheatFilename(), "rt");
	if (existingFile) {
		char temp[2048]{};
		char *line = ReadCheatDbLine(temp, sizeof(temp), existingFile);
		if (line) {
			firstCheatLine = line;
		}
		fclose(existingFile);
	}

	if (firstCheatLine.empty() || firstCheatLine[0] != '_' || firstCheatLine[1] != 'S') {
		for (int i = (int)title.size(); i > 0; --i) {
			newList.insert(newList.begin(), title[i - 1]);
		}
	}

	FILE *append = File::OpenCFile(engine.CheatFilename(), "at");
	if (!append) {
		Log("GBAStation cheats file append failed path=%s", engine.CheatFilename().ToString().c_str());
		return false;
	}

	fputc('\n', append);
	for (int i = 0; i < (int)newList.size(); ++i) {
		fprintf(append, "%s", newList[i].c_str());
		if (i < (int)newList.size() - 1) {
			fputc('\n', append);
		}
	}
	fclose(append);

	Log("GBAStation cheats imported lines=%u db=%s game=%s file=%s",
		(unsigned)newList.size(), cheatDbPath.ToString().c_str(), gameID.c_str(), engine.CheatFilename().ToString().c_str());
	return true;
}

std::string TrimAscii(std::string_view text) {
	size_t begin = 0;
	size_t end = text.size();
	while (begin < end && std::isspace((unsigned char)text[begin])) {
		begin++;
	}
	while (end > begin && std::isspace((unsigned char)text[end - 1])) {
		end--;
	}
	return std::string(text.substr(begin, end - begin));
}

void TrimCheatDecorators(std::string *text) {
	if (!text) {
		return;
	}

	*text = TrimAscii(*text);
	while (text->size() >= 3 && text->compare(0, 3, ">>>") == 0) {
		text->erase(0, 3);
		*text = TrimAscii(*text);
	}
	while (text->size() >= 3 && text->compare(text->size() - 3, 3, "<<<") == 0) {
		text->erase(text->size() - 3);
		*text = TrimAscii(*text);
	}

	const auto isDecorator = [](char ch) {
		return ch == '_' || ch == '[' || ch == ']' || ch == '<' || ch == '>' ||
			ch == '#' || ch == '^' || ch == 'v' || ch == 'V' || ch == '?';
	};
	while (!text->empty() && isDecorator(text->front())) {
		text->erase(text->begin());
		*text = TrimAscii(*text);
	}
	while (!text->empty() && isDecorator(text->back())) {
		text->pop_back();
		*text = TrimAscii(*text);
	}
}

std::vector<std::string> SplitCheatMetadataLines(const std::string &text) {
	std::vector<std::string> lines;
	std::string remaining = TrimAscii(text);
	while (remaining.size() > kCheatMetadataLineChars) {
		size_t split = remaining.rfind(' ', kCheatMetadataLineChars);
		if (split == std::string::npos || split < kCheatMetadataLineChars / 2) {
			split = remaining.find(' ', kCheatMetadataLineChars);
		}
		if (split == std::string::npos) {
			split = kCheatMetadataLineChars;
		}

		std::string line = TrimAscii(std::string_view(remaining).substr(0, split));
		if (!line.empty()) {
			lines.push_back(std::move(line));
		}
		const bool splitAtSpace = split < remaining.size() && remaining[split] == ' ';
		const size_t next = split + (splitAtSpace ? 1 : 0);
		remaining = next < remaining.size() ? TrimAscii(std::string_view(remaining).substr(next)) : "";
	}
	if (!remaining.empty()) {
		lines.push_back(std::move(remaining));
	}
	if (lines.empty() && !text.empty()) {
		lines.push_back(text);
	}
	return lines;
}

CheatMenuEntry MakeCheatMenuEntry(const CheatFileInfo &info, int sourceIndex) {
	CheatMenuEntry entry;
	entry.name = info.name;
	entry.enabled = info.enabled;
	entry.toggleable = true;
	entry.sourceIndex = sourceIndex;

	std::string label = TrimAscii(info.name);
	const size_t tipStart = label.find("[#");
	const bool arrowTip = label.size() >= 2 &&
		(label[0] == '^' || label[0] == 'v' || label[0] == 'V' || label[0] == '?') &&
		(label[1] == '[' || label[1] == '#');
	if (tipStart != std::string::npos || arrowTip || (!label.empty() && label[0] == '#')) {
		entry.toggleable = false;
		entry.sourceIndex = -1;
		entry.kind = CheatMenuEntryKind::Tip;
		size_t textStart = 0;
		if (tipStart != std::string::npos) {
			textStart = tipStart + 2;
		} else if (label[0] == '#') {
			textStart = 1;
		} else {
			textStart = label[1] == '#' ? 2 : 1;
		}
		label = label.substr(textStart);
		const size_t close = label.find(']');
		if (close != std::string::npos) {
			label.resize(close);
		}
		TrimCheatDecorators(&label);
		if (!label.empty()) {
			entry.name = label;
		}
		return entry;
	}

	const size_t arrowOpen = label.find(">>>");
	const size_t arrowClose = label.find("<<<");
	const int underscoreCount = (int)std::count(label.begin(), label.end(), '_');
	const size_t bracketOpen = label.find('[');
	const size_t bracketClose = label.rfind(']');
	if (arrowOpen != std::string::npos || arrowClose != std::string::npos ||
		(underscoreCount >= 6 && bracketOpen != std::string::npos && bracketClose != std::string::npos && bracketClose > bracketOpen)) {
		entry.toggleable = false;
		entry.sourceIndex = -1;
		entry.kind = CheatMenuEntryKind::Section;
		if (arrowOpen != std::string::npos) {
			const size_t textStart = arrowOpen + 3;
			const size_t textEnd = arrowClose != std::string::npos && arrowClose > textStart ? arrowClose : label.size();
			label = label.substr(textStart, textEnd - textStart);
		} else {
			label = label.substr(bracketOpen + 1, bracketClose - bracketOpen - 1);
		}
		TrimCheatDecorators(&label);
		if (!label.empty()) {
			entry.name = label;
		}
	}

	return entry;
}

std::vector<CheatMenuEntry> LoadCheatEntries() {
	std::vector<CheatMenuEntry> entries;
	const std::string gameID = g_paramSFO.GetDiscID();
	if (gameID.length() != 9) {
		return entries;
	}

	CWCheatEngine engine(gameID);
	engine.CreateCheatFile();
	const std::vector<CheatFileInfo> fileInfo = engine.FileInfo();
	entries.reserve(fileInfo.size());
	for (int i = 0; i < (int)fileInfo.size(); ++i) {
		const CheatFileInfo &info = fileInfo[i];
		if (info.name.empty()) {
			continue;
		}
		CheatMenuEntry entry = MakeCheatMenuEntry(info, i);
		if (!entry.toggleable) {
			for (const std::string &line : SplitCheatMetadataLines(entry.name)) {
				CheatMenuEntry lineEntry = entry;
				lineEntry.name = line;
				entries.push_back(std::move(lineEntry));
			}
		} else {
			entries.push_back(std::move(entry));
		}
	}
	return entries;
}

struct CheatInfoResult {
	bool enabled = false;
	bool available = false;
	std::vector<CheatMenuEntry> entries;
};

CheatInfoResult BuildCheatInfo(bool importFromDb) {
	CheatInfoResult result;
	if (!g_Config.bEnableCheats) {
		return result;
	}

	const Path cheatDbPath = GetCheatDbPath();
	if (importFromDb && !cheatDbPath.empty()) {
		ImportCheatsFromDb(cheatDbPath);
	}

	result.enabled = true;
	result.entries = LoadCheatEntries();
	result.available = !cheatDbPath.empty() || !result.entries.empty();
	return result;
}

void RefreshCheatInfo(bool importFromDb = false) {
	CheatInfoResult result = BuildCheatInfo(importFromDb);
	g_state.overlay.SetCheatInfo(result.enabled, result.available, result.entries);
}

void CloseCheatLoadThreadHandle() {
	if (g_state.cheatLoadThreadCreated) {
		threadWaitForExit(&g_state.cheatLoadThread);
		threadClose(&g_state.cheatLoadThread);
		g_state.cheatLoadThread = {};
		g_state.cheatLoadThreadCreated = false;
	}
	g_state.cheatLoadThreadActive.store(false, std::memory_order_release);
}

void WaitForCheatLoadThread() {
	if (!g_state.cheatLoadThreadCreated && !g_state.cheatLoadThreadActive.load(std::memory_order_acquire)) {
		return;
	}
	CloseCheatLoadThreadHandle();
}

void CheatLoadThreadEntry(void *) {
	SetCurrentThreadName("GBAStationCheats");
	std::shared_ptr<CheatInfoResult> result = std::make_shared<CheatInfoResult>(BuildCheatInfo(true));
	QueueMainThread([result]() {
		CloseCheatLoadThreadHandle();
		g_state.overlay.SetCheatInfo(result->enabled, result->available, result->entries);
	});
}

void StartAsyncCheatLoad() {
	bool expected = false;
	if (!g_state.cheatLoadThreadActive.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
		return;
	}

	Result rc = threadCreate(&g_state.cheatLoadThread, CheatLoadThreadEntry, nullptr, nullptr,
		kCheatLoadThreadStackSize, kCheatLoadThreadPriority, kCheatLoadThreadCore);
	if (R_FAILED(rc)) {
		Log("cheat load threadCreate failed rc=0x%x", (unsigned)rc);
		g_state.cheatLoadThreadActive.store(false, std::memory_order_release);
		RefreshCheatInfo(true);
		return;
	}
	g_state.cheatLoadThreadCreated = true;

	rc = threadStart(&g_state.cheatLoadThread);
	if (R_FAILED(rc)) {
		Log("cheat load threadStart failed rc=0x%x", (unsigned)rc);
		threadClose(&g_state.cheatLoadThread);
		g_state.cheatLoadThread = {};
		g_state.cheatLoadThreadCreated = false;
		g_state.cheatLoadThreadActive.store(false, std::memory_order_release);
		RefreshCheatInfo(true);
	}
}

void RefreshCheatAvailability() {
	g_state.overlay.SetCheatsEnabled(g_Config.bEnableCheats);
}

bool ToggleCheatLine(int index) {
	const std::string gameID = g_paramSFO.GetDiscID();
	if (gameID.length() != 9) {
		Log("GBAStation cheat toggle skipped invalid game id=%s", gameID.c_str());
		return false;
	}

	CWCheatEngine engine(gameID);
	engine.CreateCheatFile();
	std::vector<CheatFileInfo> fileInfo = engine.FileInfo();
	if (index < 0 || index >= (int)fileInfo.size()) {
		Log("GBAStation cheat toggle skipped invalid index=%d count=%u", index, (unsigned)fileInfo.size());
		return false;
	}

	CheatFileInfo target = fileInfo[index];
	target.enabled = !target.enabled;

	FILE *in = File::OpenCFile(engine.CheatFilename(), "rt");
	if (!in) {
		Log("GBAStation cheat toggle open failed path=%s", engine.CheatFilename().ToString().c_str());
		return false;
	}

	std::vector<std::string> lines;
	while (!feof(in)) {
		char temp[2048]{};
		char *line = ReadCheatDbLine(temp, sizeof(temp), in);
		if (!line) {
			break;
		}
		lines.push_back(line);
	}
	fclose(in);

	const size_t lineIndex = target.lineNum > 0 ? (size_t)target.lineNum - 1 : (size_t)-1;
	if (lineIndex >= lines.size()) {
		Log("GBAStation cheat toggle line out of range index=%d line=%d count=%u", index, target.lineNum, (unsigned)lines.size());
		return false;
	}

	std::string &line = lines[lineIndex];
	if (line.find("_C") == std::string::npos || line.find(target.name) == std::string::npos) {
		Log("GBAStation cheat toggle line mismatch index=%d line=%d name=%s", index, target.lineNum, target.name.c_str());
		return false;
	}
	line = (target.enabled ? "_C1 " : "_C0 ") + target.name;

	FILE *out = File::OpenCFile(engine.CheatFilename(), "wt");
	if (!out) {
		Log("GBAStation cheat toggle write failed path=%s", engine.CheatFilename().ToString().c_str());
		return false;
	}

	for (size_t i = 0; i < lines.size(); ++i) {
		fprintf(out, "%s", lines[i].c_str());
		if (i + 1 < lines.size()) {
			fputc('\n', out);
		}
	}
	fclose(out);

	g_Config.bReloadCheats = true;
	Log("GBAStation cheat toggled index=%d enabled=%d name=%s", index, target.enabled ? 1 : 0, target.name.c_str());
	return true;
}

void ToggleCheatFromQuickMenu(int index) {
	if (!g_Config.bEnableCheats) {
		Log("GBAStation cheat toggle skipped: cheats disabled in core config");
		RefreshCheatInfo(false);
		return;
	}
	if (ToggleCheatLine(index)) {
		RefreshCheatInfo(false);
	}
}


// Writes the in-memory menu-open thumbnail as a PNG next to the state file.
// Bilinear scale an RGBA image so its width is at most kThumbMaxWidth.
// Thumbnails are menu previews; keeping them small keeps PNGs well under
// 500 KB even at high internal resolutions.
void ScaleRgbaForThumb(const uint8_t *src, int sw, int sh, std::vector<uint8_t> &dst, int &dw, int &dh) {
	const int maxW = 320;
	if (sw <= maxW) {
		dst.assign(src, src + static_cast<size_t>(sw) * sh * 4);
		dw = sw;
		dh = sh;
		return;
	}
	dw = maxW;
	dh = std::max(1, static_cast<int>(static_cast<double>(sh) * maxW / sw + 0.5));
	dst.resize(static_cast<size_t>(dw) * dh * 4);
	const double sx = static_cast<double>(sw) / dw;
	const double sy = static_cast<double>(sh) / dh;
	for (int y = 0; y < dh; ++y) {
		double fy = (y + 0.5) * sy - 0.5;
		int y0 = static_cast<int>(fy);
		if (y0 < 0) y0 = 0;
		int y1 = y0 + 1;
		if (y1 >= sh) y1 = sh - 1;
		const double ty = fy - y0;
		const uint8_t *row0 = src + static_cast<size_t>(y0) * sw * 4;
		const uint8_t *row1 = src + static_cast<size_t>(y1) * sw * 4;
		uint8_t *out = dst.data() + static_cast<size_t>(y) * dw * 4;
		for (int x = 0; x < dw; ++x) {
			double fx = (x + 0.5) * sx - 0.5;
			int x0 = static_cast<int>(fx);
			if (x0 < 0) x0 = 0;
			int x1 = x0 + 1;
			if (x1 >= sw) x1 = sw - 1;
			const double tx = fx - x0;
			for (int c = 0; c < 4; ++c) {
				const double v = (1.0 - tx) * (1.0 - ty) * row0[x0 * 4 + c] +
				                 tx * (1.0 - ty) * row0[x1 * 4 + c] +
				                 (1.0 - tx) * ty * row1[x0 * 4 + c] +
				                 tx * ty * row1[x1 * 4 + c];
				out[x * 4 + c] = static_cast<uint8_t>(v + 0.5);
			}
		}
	}
}

void WriteStateThumbnail(const std::string &statePath) {
	if (g_state.thumbMemory.empty() || g_state.thumbW == 0 || g_state.thumbH == 0) {
		Log("GBAStation state thumbnail: no captured frame in memory");
		return;
	}
	// Downscale to a compact thumbnail before encoding.
	std::vector<uint8_t> scaled;
	int w = 0, h = 0;
	ScaleRgbaForThumb(g_state.thumbMemory.data(), g_state.thumbW, g_state.thumbH, scaled, w, h);
	const std::string outPath = statePath + ".png";

	std::vector<uint8_t> raw;
	raw.reserve(static_cast<size_t>(w) * (h + 1) * 3 / 2);
	for (uint32_t y = 0; y < static_cast<uint32_t>(h); ++y) {
		raw.push_back(0); // filter: None
		// Force opaque alpha: the swapchain readback alpha may be 0.
		const uint8_t *row = scaled.data() + static_cast<size_t>(y) * w * 4;
		for (uint32_t x = 0; x < static_cast<uint32_t>(w); ++x) {
			const uint8_t *p = row + static_cast<size_t>(x) * 4;
			raw.push_back(p[0]);
			raw.push_back(p[1]);
			raw.push_back(p[2]);
			raw.push_back(255);
		}
	}

	uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
	std::vector<uint8_t> compressed(compressedSize);
	if (compress2(compressed.data(), &compressedSize, raw.data(), static_cast<uLong>(raw.size()),
		Z_BEST_COMPRESSION) != Z_OK) {
		Log("GBAStation state thumbnail: compress failed");
		return;
	}
	compressed.resize(compressedSize);

	FILE *fp = fopen(outPath.c_str(), "wb");
	if (!fp) {
		Log("GBAStation state thumbnail: cannot open %s", outPath.c_str());
		return;
	}
	auto writeU32 = [&](uint32_t v) {
		const uint8_t b[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
			static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
		fwrite(b, 1, 4, fp);
	};
	auto writeChunk = [&](const char tag[4], const uint8_t *data, uint32_t len) {
		writeU32(len);
		fwrite(tag, 1, 4, fp);
		fwrite(data, 1, len, fp);
		uint32_t crc = crc32(0, reinterpret_cast<const uint8_t *>(tag), 4);
		if (len) {
			crc = crc32(crc, data, len);
		}
		writeU32(crc);
	};

	const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	fwrite(signature, 1, 8, fp);

	uint8_t ihdr[13];
	ihdr[0] = static_cast<uint8_t>(w >> 24); ihdr[1] = static_cast<uint8_t>(w >> 16);
	ihdr[2] = static_cast<uint8_t>(w >> 8);  ihdr[3] = static_cast<uint8_t>(w);
	ihdr[4] = static_cast<uint8_t>(h >> 24); ihdr[5] = static_cast<uint8_t>(h >> 16);
	ihdr[6] = static_cast<uint8_t>(h >> 8);  ihdr[7] = static_cast<uint8_t>(h);
	ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
	writeChunk("IHDR", ihdr, sizeof(ihdr));
	writeChunk("IDAT", compressed.data(), static_cast<uint32_t>(compressed.size()));
	writeChunk("IEND", nullptr, 0);
	fclose(fp);
	Log("GBAStation state thumbnail written %ux%u -> %s", w, h, outPath.c_str());
}

// ---- Play stats / GameDB helpers (mirrors nds_stub) ----

std::string NormalizePsRomPath(std::string path) {
    for (char& ch : path) {
        if (ch == '\\') ch = '/';
    }
    if (path.rfind("sdmc:", 0) == 0) path.erase(0, 5);
    while (path.size() > 1 && path[0] == '/' && path[1] == '/') path.erase(0, 1);
    return path;
}

std::string NormalizeGameDbPath(std::string path) {
	path = NormalizePsRomPath(std::move(path));
	return path;
}

std::string JsonStringOr(const nlohmann::json &item, const char *key, const std::string &fallback = {}) {
	const auto it = item.find(key);
	return it != item.end() && it->is_string() ? it->get<std::string>() : fallback;
}

int JsonIntOr(const nlohmann::json &item, const char *key, int fallback = 0) {
	const auto it = item.find(key);
	return it != item.end() && (it->is_number_integer() || it->is_number_unsigned()) ? it->get<int>() : fallback;
}

bool JsonBoolOr(const nlohmann::json &item, const char *key, bool fallback = false) {
	const auto it = item.find(key);
	return it != item.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

bool WriteGameDbAtomically(const std::string &path, const nlohmann::json &data) {
	const std::string tempPath = path + ".tmp";
	{
		std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
		if (!out) {
			Log("GBAStation GameDB temporary write failed: %s", tempPath.c_str());
			return false;
		}
		out << data.dump(4);
		out.flush();
		if (!out) {
			out.close();
			std::remove(tempPath.c_str());
			Log("GBAStation GameDB temporary flush failed: %s", tempPath.c_str());
			return false;
		}
	}
	if (std::rename(tempPath.c_str(), path.c_str()) == 0) {
		return true;
	}
	// Horizon's FAT layer does not replace an existing destination on rename.
	// Move the old complete file aside and restore it if promoting the prepared
	// temporary file fails, so a failed update never leaves GameDB truncated.
	const std::string backupPath = path + ".bak";
	if (std::rename(path.c_str(), backupPath.c_str()) != 0) {
		const int error = errno;
		std::remove(tempPath.c_str());
		Log("GBAStation GameDB replace could not stage existing file path=%s errno=%d", path.c_str(), error);
		return false;
	}
	if (std::rename(tempPath.c_str(), path.c_str()) == 0) {
		std::remove(backupPath.c_str());
		return true;
	}
	const int error = errno;
	if (std::rename(backupPath.c_str(), path.c_str()) != 0) {
		Log("GBAStation GameDB restore failed; backup retained path=%s", backupPath.c_str());
	}
	std::remove(tempPath.c_str());
	Log("GBAStation GameDB replace failed path=%s errno=%d", path.c_str(), error);
	return false;
}

std::string CurrentPsTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%y-%m-%d %H-%M-%S", &local);
    return buf;
}

// 启动时一次读入并缓存 GameDB 匹配条目；title/logoPath 的更新直接改缓存，
// 最后由 FlushPspGameDb 统一写回一次。
void FlushPspGameDb() {
	if (!g_state.gameDbLoaded || !g_state.gameDbDirty || g_state.gameDbPath.empty()) {
		return;
	}
	if (WriteGameDbAtomically(g_state.gameDbPath, g_state.gameDbData)) {
		Log("GBAStation GameDB flushed %s", g_state.gameDbPath.c_str());
		g_state.gameDbDirty = false;
	} else {
		Log("GBAStation GameDB write failed: %s", g_state.gameDbPath.c_str());
	}
}

void ApplyPspGameDbDisplaySettings(const nlohmann::json &item) {
	// These names match the generic GameDB schema already used by FBNeo and
	// 3DS.  Missing values intentionally leave the user's global defaults in
	// place, preserving backward compatibility with existing entries.
	const int displayMode = JsonIntOr(item, "displayMode", -1);
	const int internalResolution = JsonIntOr(item, "ndsInternalResolution", -1);
	std::string screenLayout = JsonStringOr(item, "ndsScreenLayout");
	int integerScale = 0;
	if (const auto stored = item.find("integerAspectRatio"); stored != item.end() && stored->is_number())
		integerScale = std::clamp((int)std::lround(stored->get<float>()), 1, 4);
	else if (const auto legacy = item.find("ndsIntegerScale"); legacy != item.end() &&
		(legacy->is_number_integer() || legacy->is_number_unsigned()))
		// One-way migration for older builds that incorrectly wrote the
		// multiplier into the boolean NDS metadata field.
		integerScale = std::clamp(JsonIntOr(item, "ndsIntegerScale", 1), 1, 4);
	if (displayMode == static_cast<int>(DisplayMode::Integer) && integerScale >= 1)
		screenLayout = std::to_string(integerScale) + "x";
	const float customScale = std::clamp(item.value("customScale", 1.0f), 0.5f, 5.0f);
	// GameDB custom offsets are pixel deltas from the centred position.
	const float customOffsetX = 0.5f + item.value("customOffsetX", 0.0f) / std::max(1, g_display.pixel_xres);
	const float customOffsetY = 0.5f + item.value("customOffsetY", 0.0f) / std::max(1, g_display.pixel_yres);
	if (displayMode < 0 && internalResolution < 0 && screenLayout.empty())
		return;
	if (g_state.overlay.IsReady())
		g_state.overlay.SetGameDisplaySettings(displayMode, screenLayout, internalResolution,
			customScale, customOffsetX, customOffsetY);
}

void ApplyPspGameDbShaderSettings(const nlohmann::json &item) {
	// Keep the selected filter separately from its enabled state, so the menu
	// switch can disable it without forgetting the choice needed to restore it.
	const bool enabled = JsonBoolOr(item, "shaderEnabled");
	const std::string preset = NormalizeGameDbPath(JsonStringOr(item, "shaderPath"));
	if (!preset.empty() && preset.size() >= 7 && preset.compare(preset.size() - 7, 7, ".slangp") == 0) {
		std::string error;
		if (!RegisterSlangPresetPath(preset, &error))
			Log("GBAStation Slang GameDB register failed file=%s error=%s", preset.c_str(), error.c_str());
		else
			ReloadAllPostShaderInfo(nullptr);
	}
	if (!enabled) {
		g_Config.vPostShaderNames.clear();
		if (g_state.overlay.IsReady())
			g_state.overlay.SetGameShaderSettings(false, preset);
		return;
	}
	if (preset.empty()) {
		g_Config.vPostShaderNames.clear();
		if (g_state.overlay.IsReady())
			g_state.overlay.SetGameShaderSettings(true, "");
		return;
	}
	const ShaderInfo *info = GetPostShaderInfo(preset);
	// The GameDB stores the user-facing built-in filter name.  Accept the old
	// section identifier as well so existing entries migrate on their next save.
	if (!info) {
		for (const ShaderInfo &candidate : GetAllPostShaderInfo()) {
			if (!candidate.isSlang && candidate.name == preset) {
				info = &candidate;
				break;
			}
		}
	}
	if (!info || info->section == "Off") {
		g_Config.vPostShaderNames.clear();
		if (g_state.overlay.IsReady())
			g_state.overlay.SetGameShaderSettings(true, preset);
		return;
	}
	g_Config.vPostShaderNames.assign(1, info->section);
	FixPostShaderOrder(&g_Config.vPostShaderNames);
	if (g_state.overlay.IsReady())
		g_state.overlay.SetGameShaderSettings(true, info->section);
	const auto names = item.value("shaderParaNames", std::vector<std::string>{});
	const auto values = item.value("shaderParaValues", std::vector<float>{});
	for (size_t i = 0; i < names.size() && i < values.size(); ++i) {
		const std::string key = StringFromFormat("%sSettingCurrentValue%u", info->section.c_str(), (unsigned)i + 1);
		g_Config.mPostShaderSetting[key] = values[i];
	}
}

void SavePspGameDbDisplaySettings() {
	if (!g_state.gameDbLoaded || g_state.gameDbIndex >= g_state.gameDbData.size())
		return;
	auto &item = g_state.gameDbData[g_state.gameDbIndex];
	if (!item.is_object())
		return;
	item["displayMode"] = g_state.overlay.GameDisplayModeIndex();
	item["ndsScreenLayout"] = g_state.overlay.GameScreenLayout();
	// The shared launcher schema clamps this NDS-named storage field to 1..4.
	// PPSSPP can still use 5x for the live session, but no fifth persistent
	// GameDB representation exists without inventing a new field.
	item["ndsInternalResolution"] = std::clamp(g_Config.iInternalResolution, 1, 4);
	item["customScale"] = g_state.overlay.GameCustomDisplayScale();
	item["customOffsetX"] = (g_state.overlay.GameCustomDisplayOffsetX() - 0.5f) * std::max(1, g_display.pixel_xres);
	item["customOffsetY"] = (g_state.overlay.GameCustomDisplayOffsetY() - 0.5f) * std::max(1, g_display.pixel_yres);
	const std::string layout = g_state.overlay.GameScreenLayout();
	const float integerScale = layout == "1x" ? 1.0f : layout == "2x" ? 2.0f : layout == "3x" ? 3.0f : layout == "4x" ? 4.0f : 1.0f;
	// The launcher deserializes this field as bool; the multiplier belongs in
	// the schema-approved float integerAspectRatio.
	item["ndsIntegerScale"] = JsonBoolOr(item, "ndsIntegerScale", true);
	item["integerAspectRatio"] = integerScale;
	// These were an earlier PPSSPP-only extension.  Keep GameDB on the shared
	// schema instead of introducing parallel custom-display keys.
	item.erase("ppssppCustomScale");
	item.erase("ppssppCustomOffsetX");
	item.erase("ppssppCustomOffsetY");
	g_state.gameDbDirty = true;
	FlushPspGameDb();
}

void SavePspGameDbShaderSettings() {
	if (!g_state.gameDbLoaded || g_state.gameDbIndex >= g_state.gameDbData.size())
		return;
	auto &item = g_state.gameDbData[g_state.gameDbIndex];
	if (!item.is_object())
		return;
	const std::string section = g_state.overlay.GameShaderSection();
	const ShaderInfo *shader = section.empty() ? nullptr : GetPostShaderInfo(section);
	const bool validShader = shader && shader->section != "Off";
	item["shaderEnabled"] = g_state.overlay.IsGameShaderEnabled();
	// This existing schema string now stores the selected built-in filter's
	// display name, rather than a custom shader file path.
	item["shaderPath"] = validShader ? shader->name : std::string();
	// The shared schema reserves this legacy field for launcher parameter files;
	// Slang parameters are stored in the arrays below.
	item["shaderParaPath"] = "";
	std::vector<std::string> names;
	std::vector<float> values;
	if (shader) {
		for (size_t i = 0; i < ARRAY_SIZE(shader->settings); ++i) {
			if (shader->settings[i].name.empty())
				continue;
			names.emplace_back(shader->settings[i].name);
			const std::string key = StringFromFormat("%sSettingCurrentValue%u", shader->section.c_str(), (unsigned)i + 1);
			auto it = g_Config.mPostShaderSetting.find(key);
			values.push_back(it == g_Config.mPostShaderSetting.end() ? shader->settings[i].value : it->second);
		}
	}
	item["shaderParaNames"] = std::move(names);
	item["shaderParaValues"] = std::move(values);
	g_state.gameDbDirty = true;
	FlushPspGameDb();
}

template <typename Apply>
void SyncPspGameDb(const char *label, Apply apply) {
	if (!g_state.gameDbLoaded || g_state.gameDbIndex >= g_state.gameDbData.size()) {
		Log("GBAStation GameDB %s sync skipped: no current entry", label);
		return;
	}
	int updated = 0;
	for (size_t index = 0; index < g_state.gameDbData.size(); ++index) {
		if (index == g_state.gameDbIndex || !g_state.gameDbData[index].is_object()) {
			continue;
		}
		apply(g_state.gameDbData[index]);
		++updated;
	}
	if (updated == 0) {
		Log("GBAStation GameDB %s sync: no other PSP entries", label);
		return;
	}
	g_state.gameDbDirty = true;
	FlushPspGameDb();
	Log("GBAStation GameDB synced %s to %d other PSP entries", label, updated);
}

void SyncPspGameDbDisplaySettings() {
	SavePspGameDbDisplaySettings();
	if (!g_state.gameDbLoaded || g_state.gameDbIndex >= g_state.gameDbData.size()) return;
	const auto &source = g_state.gameDbData[g_state.gameDbIndex];
	const int mode = JsonIntOr(source, "displayMode", -1);
	const int resolution = JsonIntOr(source, "ndsInternalResolution", -1);
	const std::string layout = JsonStringOr(source, "ndsScreenLayout");
	const float integerScale = source.value("integerAspectRatio", 1.0f);
	const float customScale = source.value("customScale", 1.0f);
	const float customOffsetX = source.value("customOffsetX", 0.0f);
	const float customOffsetY = source.value("customOffsetY", 0.0f);
	SyncPspGameDb("display settings", [=](nlohmann::json &item) {
		item["displayMode"] = mode;
		item["ndsInternalResolution"] = resolution;
		item["ndsScreenLayout"] = layout;
		item["customScale"] = customScale;
		item["customOffsetX"] = customOffsetX;
		item["customOffsetY"] = customOffsetY;
		item["integerAspectRatio"] = integerScale;
		item["ndsIntegerScale"] = JsonBoolOr(item, "ndsIntegerScale", true);
		item.erase("ppssppCustomScale");
		item.erase("ppssppCustomOffsetX");
		item.erase("ppssppCustomOffsetY");
	});
}

void SyncPspGameDbShaderSettings() {
	SavePspGameDbShaderSettings();
	if (!g_state.gameDbLoaded || g_state.gameDbIndex >= g_state.gameDbData.size()) return;
	const auto &source = g_state.gameDbData[g_state.gameDbIndex];
	const bool enabled = JsonBoolOr(source, "shaderEnabled");
	const std::string path = NormalizeGameDbPath(JsonStringOr(source, "shaderPath"));
	const auto names = source.value("shaderParaNames", std::vector<std::string>{});
	const auto values = source.value("shaderParaValues", std::vector<float>{});
	SyncPspGameDb("post shader settings", [=](nlohmann::json &item) {
		item["shaderEnabled"] = enabled;
		item["shaderPath"] = path;
		item["shaderParaPath"] = "";
		item["shaderParaNames"] = names;
		item["shaderParaValues"] = values;
	});
}

void LoadPspPlayStats(const std::string& romPath) {
    g_state.playCount = 0;
    g_state.playTimeTotal = 0;
    g_state.playStatsFound = false;
    g_state.savePath.clear();
    g_state.gameDbLoaded = false;
    g_state.gameDbDirty = false;
    g_state.gameDbIndex = 0;
    g_state.gameDbPath.clear();
    g_state.gameDbData = nlohmann::json();
    if (romPath.empty()) return;

    const char* dbPaths[] = {
        "sdmc:/GBAStation/data/GameData_PSP.json",
        "/GBAStation/data/GameData_PSP.json",
    };
    const std::string normalized = NormalizePsRomPath(romPath);
    for (const char* dbPath : dbPaths) {
        std::ifstream file(dbPath, std::ios::binary);
        if (!file.is_open()) continue;
        nlohmann::json data;
        try {
            file >> data;
        } catch (...) {
            continue;
        }
        if (!data.is_array()) continue;
        for (size_t idx = 0; idx < data.size(); ++idx) {
            auto& item = data[idx];
            if (!item.is_object()) continue;
            const std::string itemPath = JsonStringOr(item, "path");
            if (itemPath != romPath && NormalizePsRomPath(itemPath) != normalized) continue;
            g_state.playStatsFound = true;
            g_state.playCount = JsonIntOr(item, "playCount") + 1;
            g_state.playTimeTotal = JsonIntOr(item, "playTime");
            g_state.savePath = NormalizeGameDbPath(JsonStringOr(item, "savePath"));
            // Repair the common sdmc: spelling while this entry is already
            // being updated for its play counter.  The launcher consumes the
            // normalized schema paths on its next scan.
            item["savePath"] = g_state.savePath;
			item["shaderPath"] = NormalizeGameDbPath(JsonStringOr(item, "shaderPath"));
            item["shaderParaPath"] = "";
            item["playCount"] = g_state.playCount;
            g_state.gameDbData = std::move(data);
            g_state.gameDbIndex = idx;
            g_state.gameDbPath = dbPath;
            g_state.gameDbLoaded = true;
			g_state.gameDbDirty = true;
			const auto &cachedItem = g_state.gameDbData[g_state.gameDbIndex];
			ApplyPspGameDbDisplaySettings(cachedItem);
			ApplyPspGameDbShaderSettings(cachedItem);
            Log("GBAStation play stats start playCount=%d playTime=%d", g_state.playCount, g_state.playTimeTotal);
            return;
        }
    }
}

// 按归一化路径（不带 sdmc: 前缀）匹配 GameDB：
// 在已缓存的 GameDB 条目上更新 title/logoPath（内存操作，不读写文件）：
// - TITLE 仅在 GameDB 的 path 字段文件名（无扩展）与 title 相同时覆盖；
// - 封面仅当仍是默认资源图（romfs:/ 或空）时更新为 ICON0；用户自定义不覆盖。
// 变更由 FlushPspGameDb 统一写回。
void UpdatePspGameDbTitleAndLogo(const std::string& realTitle, const std::string& iconPath) {
    if (!g_state.gameDbLoaded || g_state.gameDbIndex >= g_state.gameDbData.size())
        return;
    auto& item = g_state.gameDbData[g_state.gameDbIndex];
    if (!item.is_object())
        return;
    bool changed = false;
    const std::string itemPath = item.value("path", std::string());
    std::string itemStem = itemPath;
    const size_t itemSlash = itemStem.find_last_of("/\\");
    if (itemSlash != std::string::npos)
        itemStem = itemStem.substr(itemSlash + 1);
    const size_t itemDot = itemStem.find_last_of('.');
    if (itemDot != std::string::npos)
        itemStem = itemStem.substr(0, itemDot);
    const std::string curTitle = item.value("title", std::string());
    if (!realTitle.empty() && curTitle == itemStem) {
        item["title"] = realTitle;
        changed = true;
    }
    const std::string curLogo = item.value("logoPath", std::string());
    const bool isDefaultLogo = curLogo.empty() || curLogo.rfind("romfs:/", 0) == 0;
    if (isDefaultLogo && !iconPath.empty() && curLogo != iconPath) {
        item["logoPath"] = iconPath;
        changed = true;
    }
    if (changed) {
        g_state.gameDbDirty = true;
        Log("GBAStation PSP media updated GameDB title=%s logo=%s", realTitle.c_str(), iconPath.c_str());
    }
}

// 当前 ROM 的文件名（无扩展）。
std::string PspContentStem() {
    std::string stem = g_state.contentPath;
    const size_t lastSlash = stem.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        stem = stem.substr(lastSlash + 1);
    const size_t lastDot = stem.find_last_of('.');
    if (lastDot != std::string::npos)
        stem = stem.substr(0, lastDot);
    return stem;
}

// savePath 下 ICON0 的目标路径（与提取时一致）。
std::string PspIcon0Path() {
    const std::string stem = PspContentStem();
    return stem.empty() ? std::string() : g_state.savePath + "/" + stem + ".icon0.png";
}

// 首次运行（savePath 下尚无 ICON0）时提取 PSP 内置媒体到存档目录：
// ICON0 封面 + PIC1 背景。title/logoPath 的 GameDB 更新在退出时进行。
void ExtractPspMediaIfNeeded() {
    if (g_state.savePath.empty() || g_state.contentPath.empty())
        return;
    const std::string stem = PspContentStem();
    if (stem.empty())
        return;
    const std::string iconPath = PspIcon0Path();
    struct stat st {};
    if (stat(iconPath.c_str(), &st) == 0) {
        Log("GBAStation PSP media already extracted %s", iconPath.c_str());
        return;
    }

    std::vector<u8> icon0;
    std::vector<u8> pic1;
    std::string filename = Path(g_state.contentPath).GetFilename();
    std::transform(filename.begin(), filename.end(), filename.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const bool isPbp = filename.size() >= 4 && filename.compare(filename.size() - 4, 4, ".pbp") == 0;

    if (isPbp) {
        std::unique_ptr<FileLoader> loader(ConstructFileLoader(Path(g_state.contentPath)));
        if (loader) {
            PBPReader pbp(loader.get());
            if (pbp.IsValid() && !pbp.IsELF()) {
                pbp.GetSubFile(PBP_ICON0_PNG, &icon0);
                pbp.GetSubFile(PBP_PIC1_PNG, &pic1);
            }
        }
    } else {
        PSPFileInfo iconInfo = pspFileSystem.GetFileInfo("disc0:/PSP_GAME/ICON0.PNG");
        if (iconInfo.exists)
            pspFileSystem.ReadEntireFile("disc0:/PSP_GAME/ICON0.PNG", icon0);
        PSPFileInfo pic1Info = pspFileSystem.GetFileInfo("disc0:/PSP_GAME/PIC1.PNG");
        if (pic1Info.exists)
            pspFileSystem.ReadEntireFile("disc0:/PSP_GAME/PIC1.PNG", pic1);
    }

    if (icon0.empty()) {
        Log("GBAStation PSP media extract failed: no ICON0 in %s", g_state.contentPath.c_str());
        return;
    }
    File::CreateFullPath(Path(g_state.savePath));
    if (!File::WriteDataToFile(false, icon0.data(), icon0.size(), Path(iconPath))) {
        Log("GBAStation PSP media extract failed: cannot write %s", iconPath.c_str());
        return;
    }
    if (!pic1.empty()) {
        const std::string pic1Path = g_state.savePath + "/" + stem + ".pic1.png";
        File::WriteDataToFile(false, pic1.data(), pic1.size(), Path(pic1Path));
    }
    Log("GBAStation PSP media extracted icon0=%s pic1=%d", iconPath.c_str(), (int)pic1.size());
}

// 在存档目录生成 metadata.json（PARAM.SFO 元数据，key 小写），每次启动刷新。
void WritePspMetadataJson() {
    if (g_state.savePath.empty() || g_state.contentPath.empty())
        return;
    nlohmann::json meta;
    meta["title"] = g_paramSFO.GetValueString("TITLE");
    meta["disc_id"] = g_paramSFO.GetValueString("DISC_ID");
    meta["disc_version"] = g_paramSFO.GetValueString("DISC_VERSION");
    meta["category"] = g_paramSFO.GetValueString("CATEGORY");
    meta["psp_system_ver"] = g_paramSFO.GetValueString("PSP_SYSTEM_VER");
    meta["region"] = g_paramSFO.GetValueString("REGION");
    meta["parental_level"] = g_paramSFO.GetValueInt("PARENTAL_LEVEL");
    meta["bootable"] = g_paramSFO.GetValueInt("BOOTABLE");
    meta["use_usb"] = g_paramSFO.GetValueInt("USE_USB");

    const std::string metaPath = g_state.savePath + "/metadata.json";
    File::CreateFullPath(Path(g_state.savePath));
    std::ofstream out(metaPath, std::ios::trunc);
    if (!out) {
        Log("GBAStation PSP metadata write failed %s", metaPath.c_str());
        return;
    }
    out << meta.dump(4);
    Log("GBAStation PSP metadata written %s", metaPath.c_str());
}
void SavePspPlayStats(const std::string& romPath) {
    if (!g_state.playStatsFound || romPath.empty()) return;
    if (g_state.playTimeFraction >= 0.5) ++g_state.sessionPlaySeconds;
    const int totalPlayTime = g_state.playTimeTotal + std::max(0, g_state.sessionPlaySeconds);
    const std::string lastPlayed = CurrentPsTimestamp();

    const char* dbPaths[] = {
        "sdmc:/GBAStation/data/GameData_PSP.json",
        "/GBAStation/data/GameData_PSP.json",
    };
    const std::string normalized = NormalizePsRomPath(romPath);
    for (const char* dbPath : dbPaths) {
        std::ifstream file(dbPath, std::ios::binary);
        if (!file.is_open()) continue;
        nlohmann::json data;
        try {
            file >> data;
        } catch (...) {
            continue;
        }
        if (!data.is_array()) continue;
        for (auto& item : data) {
            if (!item.is_object()) continue;
            const std::string itemPath = item.value("path", std::string());
            if (itemPath != romPath && NormalizePsRomPath(itemPath) != normalized) continue;
            item["playCount"] = g_state.playCount;
            item["playTime"] = std::max(0, totalPlayTime);
            item["lastPlayed"] = lastPlayed;
            // Close the read stream first: the Switch stdio/fs layer refuses a
            // second handle (write/trunc) on a file that is still open for read.
            file.close();
            std::ofstream out(dbPath, std::ios::trunc);
            if (out) {
                out << data.dump(4);
                Log("GBAStation play stats exit playCount=%d playTime=%d lastPlayed=%s", g_state.playCount, totalPlayTime, lastPlayed.c_str());
            } else {
                Log("GBAStation play stats write failed: %s", dbPath);
            }
            return;
        }
    }
}
std::string GetLegacySaveStateBaseName() {

	std::string romName = g_state.contentPath;

	size_t lastSlash = romName.find_last_of("/\\");
	if (lastSlash != std::string::npos) {
		romName = romName.substr(lastSlash + 1);
	}

	size_t lastDot = romName.find_last_of('.');
	if (lastDot != std::string::npos) {
		romName = romName.substr(0, lastDot);
	}

	return romName;
}

Path GetLegacySaveStatePath(int slot) {
	const std::string romName = GetLegacySaveStateBaseName();
	if (romName.empty()) {
		return Path();
	}

	const int safeSlot = std::clamp(slot, 0, Ppsspp::SaveStateSlotCount - 1);
	// Save dir comes from the launcher GameDB (savePath field); fall back to
	// the same default layout the launcher would have generated otherwise.
	std::string saveDir = g_state.savePath;
	if (saveDir.empty()) {
		saveDir = std::string(Paths::PpssppSaveStates) + "/" + romName;
	}
	File::CreateFullPath(Path(saveDir));
	return Path(saveDir) / (romName + ".ss" + std::to_string(safeSlot));
}

u64 GetSystemMs() {
	const u64 tickFreq = armGetSystemTickFreq();
	const u64 ticksPerMs = tickFreq / 1000;
	return ticksPerMs > 0 ? armGetSystemTick() / ticksPerMs : 0;
}

void RefreshSaveStateSlots(bool force) {
	const u64 nowMs = GetSystemMs();
	if (!force && nowMs != 0 && nowMs - g_state.lastSaveStateScanMs < 500) {
		return;
	}

	File::CreateFullPath(Path(Paths::PpssppSaveStates));
	std::array<time_t, Ppsspp::SaveStateSlotCount> slotMtime{};
	for (int i = 0; i < Ppsspp::SaveStateSlotCount; ++i) {
		const Path statePath = GetLegacySaveStatePath(i);
		const bool exists = !statePath.empty() && File::Exists(statePath);
		g_state.saveStateSlots[i] = exists;
		if (exists) {
			struct stat st{};
			if (stat(statePath.c_str(), &st) == 0) {
				slotMtime[i] = st.st_mtime;
			}
		}
	}
	g_state.overlay.SetSaveStateInfo(g_Config.iCurrentStateSlot, g_state.saveStateSlots, slotMtime);
	g_state.lastSaveStateScanMs = nowMs;
}

void AfterSaveStateAction(SaveState::Status status, std::string_view message) {
	Log("GBAStation savestate status=%d message=%.*s", (int)status, (int)message.size(), message.data());
	RefreshSaveStateSlots(true);
}

void AfterExitSaveStateAction(SaveState::Status status, std::string_view message) {
	AfterSaveStateAction(status, message);
	if (status != SaveState::Status::FAILURE && !g_state.exitSavePath.empty()) {
		// SaveState callbacks run from SaveState::Process() on the emulation
		// thread, before Shutdown can release the captured framebuffer.
		WriteStateThumbnail(g_state.exitSavePath);
	}
	g_state.exitSavePending = false;
	g_state.exitSaveFinished = true;
	Log("GBAStation exit auto save finished status=%d thumbnail=%s", (int)status,
		status != SaveState::Status::FAILURE ? "written" : "skipped");
}

// Returns true when an asynchronous exit save was queued and exit must wait.
bool BeginExitAutoSave() {
	if (g_state.exitSavePending || g_state.exitSaveFinished) {
		return true;
	}
	if (g_state.autoSaveOnExitSlot <= 0) {
		return false;
	}

	const int slot = std::clamp(g_state.autoSaveOnExitSlot - 1, 0, Ppsspp::SaveStateSlotCount - 1);
	const std::string statePath = GetPspSaveStatePath(slot);
	if (statePath.empty()) {
		Log("GBAStation exit auto save ignored: empty path slot=%d", slot);
		return false;
	}

	File::CreateFullPath(Path(Paths::PpssppSaveStates));
	g_state.exitSavePath = statePath;
	g_state.exitSaveStartedMs = GetSystemMs();
	g_state.exitSavePending = true;
	g_state.exitSaveFinished = false;
	g_state.overlay.SetExitSaving(true);
	Log("GBAStation exit auto save start slot=%d path=%s", slot, statePath.c_str());
	SaveState::Save(Path(statePath), slot, &AfterExitSaveStateAction);
	return true;
}

bool WaitForPspBoot(const char *tag, std::string *errorString) {
	int bootLoopCount = 0;
	while (g_state.running && appletMainLoop() && PSP_InitUpdate(errorString) == BootState::Booting) {
		if (bootLoopCount == 0) {
			Log("%s loop entered", tag);
		}
		bootLoopCount++;
		DrainMainThreadQueue();
		RetroAchievements().Idle();
		if (g_state.graphicsContext) {
			g_state.graphicsContext->Poll();
		}
		sleep_ms(1, "GBAStation-boot-poll");
	}
	Log("%s loop exited count=%d state=%d", tag, bootLoopCount, (int)PSP_GetBootState());
	return PSP_IsInited();
}

void ResetContent() {
	if (!PSP_IsInited()) {
		Log("GBAStation reset ignored: PSP is not initialized");
		return;
	}
	if (UtilitySavedataIsActive()) {
		// Do not destroy the emulated filesystem while a game save is still
		// completing.  The next frames let the guest finish its savedata dialog;
		// RunFrame will restart only after that handoff has ended.
		g_state.resetPending = true;
		Log("GBAStation reset deferred: PSP savedata dialog is active");
		return;
	}

	const std::string path = g_state.contentPath;
	Log("GBAStation reset content path=%s", path.c_str());
	RetroAchievements().UnloadGame();
	PSP_Shutdown(true);
	g_state.ppssppShutdown = false;
	g_state.booted = false;
	Core_SetGraphicsContext(g_state.graphicsContext);
	PSP_CoreParameter().graphicsContext = g_state.graphicsContext;

	if (!PSP_InitStart(PSP_CoreParameter())) {
		Log("GBAStation reset PSP_InitStart failed");
		g_state.running = false;
		Core_Stop();
		return;
	}

	std::string errorString;
	if (!WaitForPspBoot("reset", &errorString)) {
		Log("GBAStation reset failed: %s", errorString.c_str());
		g_state.running = false;
		Core_Stop();
		return;
	}

	System_Notify(SystemNotification::BOOT_DONE);
	coreState = CORE_RUNNING_CPU;
	g_state.booted = true;
	RetroAchievements().SetGame(path);
	RefreshSaveStateSlots(true);
	Log("GBAStation reset complete");
}

void ExecuteOverlayCommand(OverlayCommand command) {
	if (command.action == OverlayAction::None) {
		return;
	}

	if (command.action == OverlayAction::Reset) {
		ResetContent();
		return;
	}
	if (command.action == OverlayAction::LoadCheats) {
		StartAsyncCheatLoad();
		return;
	}
	if (command.action == OverlayAction::ToggleCheat) {
		ToggleCheatFromQuickMenu(command.slot);
		return;
	}

	const int slot = std::clamp(command.slot, 0, Ppsspp::SaveStateSlotCount - 1);
	const Path statePath = GetLegacySaveStatePath(slot);
	const std::string statePathString = statePath.ToString();
	g_Config.iCurrentStateSlot = slot;
	if (RetroAchievements().WarnIfHardcoreModeActive(command.action == OverlayAction::SaveState || command.action == OverlayAction::LoadState)) {
		return;
	}
	if (statePath.empty()) {
		Log("GBAStation savestate ignored: empty content path slot=%d", slot);
		return;
	}

	File::CreateFullPath(Path(Paths::PpssppSaveStates));

	if (command.action == OverlayAction::SaveState) {
		Log("GBAStation save state slot=%d path=%s", slot, statePathString.c_str());
		SaveState::Save(statePath, slot, &AfterSaveStateAction);
		// Write the menu-open thumbnail (captured into memory before the menu
		// rendered) next to the state file.
		WriteStateThumbnail(statePathString);
		g_state.overlay.SetVisible(false);
	} else if (command.action == OverlayAction::LoadState) {
		if (!File::Exists(statePath)) {
			Log("GBAStation load state missing slot=%d path=%s", slot, statePathString.c_str());
			RefreshSaveStateSlots(true);
			return;
		}
		Log("GBAStation load state slot=%d path=%s", slot, statePathString.c_str());
		SaveState::Load(statePath, slot, &AfterSaveStateAction);
	}
}

}  // namespace

std::string GetPspSaveStatePath(int slot) {
	const int safeSlot = std::clamp(slot, 0, Ppsspp::SaveStateSlotCount - 1);
	return GetLegacySaveStatePath(safeSlot).ToString();
}

// Rewrite a single key in config.cfg (keeping the launcher's "s|" prefix).
// Rewrite a single key in config.cfg (keeping the launcher's "s|" prefix).
static void WriteConfigValue(const char *key, const std::string &value) {
	const char *paths[] = {"sdmc:/GBAStation/config/config.cfg", "/GBAStation/config/config.cfg"};
	std::string cfgPath;
	for (const char *path : paths) {
		std::ifstream in(path);
		if (in.good()) {
			cfgPath = path;
			break;
		}
	}
	if (cfgPath.empty()) {
		return;
	}

	std::vector<std::string> lines;
	{
		std::ifstream in(cfgPath);
		std::string line;
		while (std::getline(in, line)) {
			lines.push_back(line);
		}
	}

	const std::string keyPrefix = std::string(key) + "=";
	const std::string encoded = "s|" + value;
	bool replaced = false;
	for (std::string &line : lines) {
		if (line.compare(0, keyPrefix.size(), keyPrefix) == 0) {
			line = keyPrefix + encoded;
			replaced = true;
			break;
		}
	}
	if (!replaced) {
		lines.push_back(keyPrefix + encoded);
	}

	std::ofstream out(cfgPath, std::ios::trunc);
	if (!out) {
		return;
	}
	for (const std::string &line : lines) {
		out << line << "\n";
	}
}

float GetPspFastForwardMultiplier() {
	return g_state.fastForwardMultiplier;
}

void SetPspFastForwardMultiplier(float multiplier) {
	g_state.fastForwardMultiplier = multiplier <= 0.001f ? 0.0f : std::clamp(multiplier, 0.5f, 5.0f);
	g_state.fastForwardSettingsSavePending = true;
}

void SavePspFastForwardSettings() {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.2f", g_state.fastForwardMultiplier);
	WriteConfigValue("fastforward.multiplier", buf);
	WriteConfigValue("fastforward.mode", g_state.fastForwardToggleMode ? "toggle" : "hold");
	g_state.fastForwardSettingsSavePending = false;
}

bool GetPspFastForwardToggleMode() {
	return g_state.fastForwardToggleMode;
}

void SetPspFastForwardToggleMode(bool toggleMode) {
	g_state.fastForwardToggleMode = toggleMode;
	if (!toggleMode) {
		g_state.fastForwardToggle = false;
	}
	g_state.fastForwardSettingsSavePending = true;
}

double GetPspCurrentFps() {
	return g_state.fps;
}

bool GetPspFastForwardActive() {
	return g_state.fastForwardActive;
}

bool GetPspShowFps() {
	const auto it = g_state.gbastationConfig.find("display.showFps");
	if (it == g_state.gbastationConfig.end()) {
		return true;
	}
	std::string value = Trim(it->second);
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return (char)std::tolower(c);
	});
	if (value == "false" || value == "0" || value == "off" || value == "disabled") {
		return false;
	}
	// Preserve the Flycast/launcher default for true, typed, or malformed
	// values so a missing/legacy key never makes the FPS HUD disappear.
	return true;
}

PpssppRuntime::PpssppRuntime(LogCallback log) : log_(std::move(log)) {
	g_state.log = log_;
}

PpssppRuntime::~PpssppRuntime() = default;

bool PpssppRuntime::Configure(const LaunchInfo &launch) {
	g_state.running = true;
	g_state.booted = false;
	g_state.ppssppShutdown = false;
	g_state.resetPending = false;
	g_state.chainloadLauncher = false;
	g_state.frameOpen = false;
	g_state.hostFrameOpen = false;
	g_state.runtimeSettingsDirty = false;
	g_state.contentPath = launch.contentPath;
	g_state.log = log_;
	PROFILE_INIT();
	TimeInit();
	LoadGBAStationConfig();
	Log("ppsspp runtime configure content=%s", g_state.contentPath.c_str());
	return !g_state.contentPath.empty();
}

bool PpssppRuntime::Initialize(const LaunchInfo &) {
	UpdateDisplayMode();
	Log("initial display=%dx%d", g_display.pixel_xres, g_display.pixel_yres);

	InitAudio();
	Log("InitAudio ready=%d rate=%d samples=%d", g_state.audioReady ? 1 : 0, g_state.audioSampleRate, g_state.audioBufferSamples);
	if (!InstallPpssppAssets(g_state.log)) {
		Log("InstallPpssppAssets failed");
		return false;
	}
	TrophySfx().Load(g_state.log);
	InitializeConfig();
	ApplyGBAStationPpssppCoreSettings();
	Log("InitializeConfig backend=%d mt=%d inflight=%d", g_Config.iGPUBackend, g_Config.bRenderMultiThreading ? 1 : 0, g_Config.iInflightFrames);

	g_logManager.Init(&g_Config.bEnableLogging, false);
	if (g_Config.bEnableLogging && g_Config.bEnableFileLogging) {
		g_logManager.SetAllLogLevels(LogLevel::LINFO);
		g_logManager.EnableOutput(LogOutput::File);
		g_logManager.SetFileLogPath(::Path(Paths::PpssppDataRoot) / "SYSTEM/DUMP/log.txt");
		Log("ppsspp log=%s", g_logManager.GetLogFilePath().c_str());
	} else {
		Log("ppsspp file log disabled");
	}
	g_threadManager.Init(cpu_info.num_cores, cpu_info.logical_cpu_count);
	Log("thread manager cores=%d logical=%d", cpu_info.num_cores, cpu_info.logical_cpu_count);
	RetroAchievements().Initialize(g_state.log);

	std::string errorString;
	const GPUCore gpuCore = GPUCORE_VULKAN;
	Log("graphics init start gpuCore=%d", (int)gpuCore);
	Log("graphics host invoke");
	const bool graphicsInitialized =
		InitializeGBAStationGraphicsHost(&errorString, &g_state.graphicsContext, gpuCore);
	Log("graphics host returned ok=%d", graphicsInitialized ? 1 : 0);
	if (!graphicsInitialized) {
		std::fprintf(stderr, "Graphics initialization failed: %s\n", errorString.c_str());
		Log("graphics init failed: %s", errorString.c_str());
		return false;
	}
	g_state.draw = g_state.graphicsContext ? g_state.graphicsContext->GetDrawContext() : nullptr;
	Log("graphics init ok ctx=%p draw=%p", g_state.graphicsContext, g_state.draw);
	return true;
}

bool PpssppRuntime::LoadContent(const std::string &path) {
	CoreParameter coreParameter{};
	coreParameter.cpuCore = CPUCore::JIT;
	coreParameter.gpuCore = GPUCORE_VULKAN;
	coreParameter.graphicsContext = g_state.graphicsContext;
	coreParameter.enableSound = g_state.audioReady;
	coreParameter.fileToStart = ::Path(path);
	coreParameter.headLess = false;
	coreParameter.renderScaleFactor = 1;
	coreParameter.renderWidth = 480;
	coreParameter.renderHeight = 272;
	coreParameter.pixelWidth = g_display.pixel_xres;
	coreParameter.pixelHeight = g_display.pixel_yres;
	coreParameter.fastForward = false;

	Core_SetGraphicsContext(g_state.graphicsContext);
	Log("PSP_InitStart file=%s", path.c_str());
	if (!PSP_InitStart(coreParameter)) {
		std::fprintf(stderr, "Failed to start PSP core for '%s'\n", path.c_str());
		Log("PSP_InitStart failed");
		return false;
	}

	std::string errorString;
	if (!WaitForPspBoot("boot", &errorString)) {
		std::fprintf(stderr, "Startup failed: %s\n", errorString.c_str());
		Log("startup failed: %s", errorString.c_str());
		return false;
	}

	System_Notify(SystemNotification::BOOT_DONE);
	coreState = CORE_RUNNING_CPU;
	g_state.booted = true;
	RetroAchievements().SetGame(path);
	Log("boot complete");

	// Launcher play stats: bump run count now, write playTime/lastPlayed on exit.
	LoadPspPlayStats(path);
	// PSP metadata (PARAM.SFO) -> savePath/metadata.json, refreshed every start.
	WritePspMetadataJson();
	// First run (no ICON0 in the save dir yet): extract PSP media and update the
	// cached GameDB entry in memory.
	ExtractPspMediaIfNeeded();
	// Single writeback for playCount / title / logoPath changes.
	FlushPspGameDb();
	g_state.autoLoadStateSlot = ConfigInt("save.autoLoadState0", 0);
	g_state.autoSaveOnExitSlot = std::clamp(ConfigInt("save.autoSaveOnExit", 0), 0, Ppsspp::SaveStateSlotCount);
	if (g_state.autoLoadStateSlot > 0) {
		const int slot = std::clamp(g_state.autoLoadStateSlot - 1, 0, Ppsspp::SaveStateSlotCount - 1);
		const std::string statePath = GetPspSaveStatePath(slot);
		Log("GBAStation auto load state slot=%d path=%s", slot, statePath.c_str());
		if (File::Exists(Path(statePath))) {
			SaveState::Load(Path(statePath), slot, &AfterSaveStateAction);
		}
	}
	return true;
}

void PpssppRuntime::HandleInput(const FrameInput &input) {
	const bool overlayTogglePressed = BindingPressedEdge(
		"psp.hotkey.menu.pad", "PAD_LT+PAD_RT", input.buttons, input.pressed);
	const bool overlayToggleHeld = BindingHeld(
		"psp.hotkey.menu.pad", "PAD_LT+PAD_RT", input.buttons);
	if (overlayTogglePressed) {
		// Capture the pure gameplay frame into memory before the menu renders;
		// used as the state thumbnail when a state is saved.
		g_state.menuPendingThumb = true;
	}
	if (g_state.overlay.IsVisible() || overlayTogglePressed) {
		RefreshSaveStateSlots(overlayTogglePressed);
		if (overlayTogglePressed) {
			RefreshCheatAvailability();
		}
	}
	// Gameplay bindings may deliberately move PSP directions onto either stick.
	// The GBAStation menu is a frontend surface and always navigates with the
	// physical Switch controls, while its open chord remains configurable.
	const int prevResolution = g_Config.iInternalResolution;
	const bool prevSkipBufferEffects = g_Config.bSkipBufferEffects;
	const bool prevFastMemory = g_Config.bFastMemory;
	const bool wasOverlayVisible = g_state.overlay.IsVisible();
	const bool inputConsumedByOverlay = g_state.overlay.HandleInput(input.buttons, input.pressed,
		input.leftStickX, input.leftStickY, input.rightStickX, input.rightStickY, overlayTogglePressed);
	// The menu just closed: suppress game input for a few frames so the
	// confirm/back button press does not bleed into the game.
	if (wasOverlayVisible && !g_state.overlay.IsVisible()) {
		g_state.pspInputSuppressFrames = 3;
	}
	if (g_state.overlay.ConsumeCoreSettingsChanged()) {
		// Keep LR adjustment entirely in memory while the menu is open.  The
		// final value is applied and persisted only when the menu closes (or
		// during Shutdown for a direct exit).
		g_state.runtimeSettingsSavePending = true;
		g_state.settingsRenderResized = g_state.settingsRenderResized ||
			g_Config.bSkipBufferEffects != prevSkipBufferEffects;
		g_state.settingsJitClear = g_state.settingsJitClear || g_Config.bFastMemory != prevFastMemory;
		Log("queued PPSSPP runtime core settings (debounced)");
	}
	if (g_state.overlay.ConsumeGameDisplaySettingsSaveRequest()) {
		g_state.gameDisplaySettingsSavePending = true;
		// Per-game resolution changes still need the live GPU targets to be
		// recreated, but do not require saving the global PPSSPP configuration.
		if (g_Config.iInternalResolution != prevResolution) {
			g_state.runtimeSettingsDirty = true;
			g_state.settingsRenderResized = true;
		}
	}
	if (g_state.overlay.ConsumeGameShaderSettingsSaveRequest()) {
		SavePspGameDbShaderSettings();
	}
	if (g_state.overlay.ConsumeSyncDisplaySettingsRequest()) {
		SyncPspGameDbDisplaySettings();
	}
	if (g_state.overlay.ConsumeSyncShaderSettingsRequest()) {
		SyncPspGameDbShaderSettings();
	}
	if (wasOverlayVisible && !g_state.overlay.IsVisible()) {
		if (g_state.runtimeSettingsSavePending) {
			SaveGBAStationPpssppRuntimeSettings();
			g_state.runtimeSettingsSavePending = false;
			g_state.runtimeSettingsDirty = true;
			Log("applied PPSSPP runtime settings after menu close");
		}
		if (g_state.gameDisplaySettingsSavePending) {
			SavePspGameDbDisplaySettings();
			g_state.gameDisplaySettingsSavePending = false;
			Log("saved PSP GameDB display settings after menu close");
		}
		if (g_state.fastForwardSettingsSavePending) {
			SavePspFastForwardSettings();
			Log("saved fast forward settings after menu close");
		}
	}
	ExecuteOverlayCommand(g_state.overlay.ConsumeCommand());
	if (g_state.overlay.ShouldExitGame()) {
		Log("GBAStation overlay exit requested");
		g_state.overlay.ClearExitRequest();
		g_state.chainloadLauncher = true;
		// A native PSP save is not complete merely because its UI has reported
		// success.  Let sceUtility finish its worker and guest-side shutdown
		// before we pause the game for the exit savestate.
		if (UtilitySavedataIsActive()) {
			g_state.exitWaitingForNativeSave = true;
			g_state.exitNativeSaveClearMs = 0;
			g_state.overlay.SetExitSaving(true, true);
			ClearPspInput();
			g_state.fastForwardActive = false;
			PSP_CoreParameter().fastForward = false;
			PSP_CoreParameter().fpsLimit = FPSLimit::NORMAL;
			Log("GBAStation exit deferred: PSP native savedata dialog is active");
		} else if (!BeginExitAutoSave()) {
			RequestExit();
		}
		return;
	}

	if (inputConsumedByOverlay || g_state.pspInputSuppressFrames > 0) {
		ClearPspInput();
		g_state.fastForwardActive = false;
		PSP_CoreParameter().fastForward = false;
		PSP_CoreParameter().fpsLimit = FPSLimit::NORMAL;
		if (g_state.pspInputSuppressFrames > 0) {
			--g_state.pspInputSuppressFrames;
			// Renew while the confirm/back button is still held so the release
			// does not bleed into the game.
			if (input.buttons & (HidNpadButton_B | HidNpadButton_A))
				g_state.pspInputSuppressFrames = 3;
		}
	} else {
		UpdatePspInput(input);
		const bool ffHeld = BindingHeld(
			"psp.handle.fastforward", "PAD_LSB", input.buttons);
		const bool ffPressed = BindingPressedEdge(
			"psp.handle.fastforward", "PAD_LSB", input.buttons, input.pressed);
		if (g_state.fastForwardToggleMode) {
			if (ffPressed) {
				g_state.fastForwardToggle = !g_state.fastForwardToggle;
			}
		}
		const bool ffActive = g_state.fastForwardToggleMode
			? g_state.fastForwardToggle
			: ffHeld;
		g_state.fastForwardActive = ffActive;
		// In PPSSPP, fastForward=true has priority over CUSTOM1 and removes
		// the display limiter altogether.  Reserve it for the explicit
		// "unlimited" option; finite GBAStation multipliers use CUSTOM1 only.
		PSP_CoreParameter().fastForward = ffActive && g_state.fastForwardMultiplier <= 0.001f;
		if (ffActive && g_state.fastForwardMultiplier > 1.001f) {
			PSP_CoreParameter().fpsLimit = FPSLimit::CUSTOM1;
			g_Config.iFpsLimit1 = std::max(1, static_cast<int>(std::lround(60.0f * g_state.fastForwardMultiplier)));
		} else {
			PSP_CoreParameter().fpsLimit = FPSLimit::NORMAL;
		}
	}
}

void PpssppRuntime::RunFrame() {
	if (!g_state.running || !g_state.booted) {
		return;
	}

	// Play time: accumulate wall time while the game is actually running;
	// menu open pauses the session so menu time is not counted.
	const double nowPlayMs = time_now_d() * 1000.0;
	if (!g_state.overlay.IsVisible()) {
		if (g_state.playTimeLastMs > 0.0) {
			const double deltaMs = nowPlayMs - g_state.playTimeLastMs;
			if (deltaMs >= 0.0 && deltaMs < 1000.0) {
				g_state.playTimeFraction += deltaMs / 1000.0;
			}
		}
		g_state.playTimeLastMs = nowPlayMs;
	} else {
		g_state.playTimeLastMs = 0.0;
	}

	if (g_state.frameCount == 0) {
		Log("main loop entered");
	}
	g_state.frameCount++;
	UpdateDisplayMode();
	if (g_state.graphicsContext) {
		g_state.graphicsContext->Poll();
	}
	DrainMainThreadQueue();
	RetroAchievements().Idle();
	if (g_state.resetPending && !UtilitySavedataIsActive()) {
		g_state.resetPending = false;
		Log("GBAStation deferred reset proceeding after savedata completion");
		ResetContent();
		return;
	}
	PSP_UpdateDebugStats((DebugOverlay)g_Config.iDebugOverlay == DebugOverlay::DEBUG_STATS || g_Config.bLogFrameDrops);

	const DisplayLayoutConfig &displayLayoutConfig = g_Config.GetDisplayLayoutConfig(DeviceOrientation::Landscape);
	__DisplaySetDisplayLayoutConfig(displayLayoutConfig);

	// Save/load requests are queued by SaveState::Save/Load and must be
	// flushed explicitly each frame, matching PPSSPP's normal EmuScreen path.
	SaveState::Process();
	if (g_state.exitWaitingForNativeSave) {
		constexpr u64 kNativeSaveSettleMs = 400;
		const u64 nowMs = GetSystemMs();
		if (UtilitySavedataIsActive()) {
			// The dialog owns an asynchronous file writer; reset the stability
			// timer whenever it is still active.
			g_state.exitNativeSaveClearMs = 0;
		} else if (g_state.exitNativeSaveClearMs == 0) {
			g_state.exitNativeSaveClearMs = nowMs;
			Log("GBAStation native savedata completed; waiting %llums before exit", (unsigned long long)kNativeSaveSettleMs);
		} else if (nowMs == 0 || nowMs - g_state.exitNativeSaveClearMs >= kNativeSaveSettleMs) {
			g_state.exitWaitingForNativeSave = false;
			g_state.overlay.SetExitSaving(true);
			Log("GBAStation native savedata settle complete; starting exit savestate");
			if (!BeginExitAutoSave()) {
				g_state.overlay.SetExitSaving(false);
				RequestExit();
				return;
			}
		}
	}
	if (g_state.exitSaveFinished) {
		// Keep the confirmation visible briefly even when the state write was
		// very fast, so it is clear that the game is deliberately waiting.
		constexpr u64 kExitSaveDialogMinMs = 400;
		const u64 nowMs = GetSystemMs();
		if (nowMs == 0 || nowMs - g_state.exitSaveStartedMs >= kExitSaveDialogMinMs) {
			g_state.overlay.SetExitSaving(false);
			Log("GBAStation exit auto save complete; leaving game");
			RequestExit();
			return;
		}
	}

	if (g_state.draw) {
		g_state.draw->BeginFrame(Draw::DebugFlags::NONE);
		g_state.frameOpen = true;
		if (!g_state.overlay.IsReady()) {
			g_state.overlay.Init(g_state.draw, g_state.contentPath.c_str(), g_state.log);
			if (g_state.gameDbLoaded && g_state.gameDbIndex < g_state.gameDbData.size()) {
				ApplyPspGameDbDisplaySettings(g_state.gameDbData[g_state.gameDbIndex]);
				ApplyPspGameDbShaderSettings(g_state.gameDbData[g_state.gameDbIndex]);
			}
			RefreshCheatAvailability();
		}
	}
	if (gpu) {
		gpu->BeginHostFrame(displayLayoutConfig);
		g_state.hostFrameOpen = true;
		if (g_state.runtimeSettingsDirty) {
			gpu->NotifyConfigChanged();
			gpu->CheckConfigChanged(displayLayoutConfig);
			if (g_state.settingsRenderResized) {
				gpu->NotifyRenderResized(displayLayoutConfig);
				g_state.settingsRenderResized = false;
			}
			if (g_state.settingsJitClear) {
				if (currentMIPS) {
					currentMIPS->ClearJitCache();
				}
				g_state.settingsJitClear = false;
			}
			g_state.runtimeSettingsDirty = false;
			Log("applied PPSSPP runtime core settings");
		}
	}

	if (g_state.overlay.IsVisible() && !g_state.exitWaitingForNativeSave) {
		sleep_ms(1, "GBAStation-overlay-pause");
	} else {
		RetroAchievements().FrameUpdate();
		PSP_RunLoopWhileState();
	}
	// Use PPSSPP's emulated VBlank rate for the HUD, rather than the host
	// render-loop rate.  The latter is normally capped by the Switch display at
	// 60 Hz even while the emulator is running at 2x/3x/etc.
	float emulatedVps = 0.0f;
	__DisplayGetFPS(&emulatedVps, nullptr, nullptr);
	if (std::isfinite(emulatedVps) && emulatedVps > 0.0f) {
		g_state.fps = emulatedVps;
	}

	if (coreState == CORE_NEXTFRAME) {
		coreState = CORE_RUNNING_CPU;
	}
	if (gpu) {
		gpu->EndHostFrame();
		g_state.hostFrameOpen = false;
		if (!gpu->PresentedThisFrame()) {
			gpu->PrepareCopyDisplayToOutput(displayLayoutConfig);
		}
	}
	if (coreState == CORE_POWERDOWN) {
		RequestExit();
	}
}

void PpssppRuntime::RenderFrame() {
	if (!g_state.frameOpen || !g_state.draw) {
		return;
	}

	const DisplayLayoutConfig &displayLayoutConfig = g_Config.GetDisplayLayoutConfig(DeviceOrientation::Landscape);
	if (gpu && !gpu->PresentedThisFrame()) {
		g_state.draw->BindFramebufferAsRenderTarget(nullptr, { Draw::RPAction::CLEAR, Draw::RPAction::CLEAR, Draw::RPAction::CLEAR }, "GBAStationPpsspp");
		gpu->CopyDisplayToOutput(displayLayoutConfig);
	}

	// Menu-open thumbnail: when the menu button was just pressed, capture the
	// pure gameplay backbuffer (game image already composited, menu not yet
	// drawn) into memory. The BLOCK readback submits the game + readback steps
	// in a single flush (the same pattern ppsspp uses for AVI recording), so
	// this is safe; the menu is delayed one frame to keep the frame clean.
	if (g_state.menuPendingThumb) {
		GPUDebugBuffer buf;
		if (::GetOutputFramebuffer(g_state.draw, buf)) {
			const uint32_t w = static_cast<uint32_t>(buf.GetStride());
			const uint32_t h = static_cast<uint32_t>(buf.GetHeight());
			const size_t size = static_cast<size_t>(w) * h * 4;
			g_state.thumbMemory.resize(size);
			const uint8_t *src = buf.GetData();
			if (buf.GetFormat() == GPU_DBG_FORMAT_8888_BGRA) {
				// BGRA -> RGBA so the PNG writer can store it directly.
				for (size_t i = 0; i < size; i += 4) {
					g_state.thumbMemory[i] = src[i + 2];
					g_state.thumbMemory[i + 1] = src[i + 1];
					g_state.thumbMemory[i + 2] = src[i];
					g_state.thumbMemory[i + 3] = src[i + 3];
				}
			} else {
				std::memcpy(g_state.thumbMemory.data(), src, size);
			}
			g_state.thumbW = w;
			g_state.thumbH = h;
			Log("GBAStation menu thumbnail captured %ux%u", w, h);
		}
		g_state.menuPendingThumb = false;
		// Skip the menu this frame so the captured backbuffer stays pure
		// gameplay; it will be shown on the next frame.
		g_state.draw->EndFrame();
		g_state.frameOpen = false;
		g_frameTiming.PostSubmit();
		g_state.draw->Present(Draw::PresentMode::FIFO);
		return;
	}

	g_state.overlay.Render(g_state.draw);
	g_state.overlay.RefreshSlotThumbs(g_state.draw);
	g_state.draw->EndFrame();
	g_state.frameOpen = false;
	g_frameTiming.PostSubmit();
	g_state.draw->Present(Draw::PresentMode::FIFO);
}

bool PpssppRuntime::ShouldExit() const {
	return !g_state.running || coreState == CORE_POWERDOWN;
}

bool PpssppRuntime::ShouldChainloadLauncher() const {
	return g_state.chainloadLauncher;
}

void PpssppRuntime::RequestExit() {
	g_state.running = false;
	Core_Stop();
}

void PpssppRuntime::Shutdown() {
	Log("ppsspp runtime shutdown start frames=%d coreState=%d running=%d", g_state.frameCount, (int)coreState, g_state.running ? 1 : 0);
	// Do not lose the last selector value when the user exits before its
	// debounce window expires.
	if (g_state.runtimeSettingsSavePending) {
		SaveGBAStationPpssppRuntimeSettings();
		g_state.runtimeSettingsSavePending = false;
	}
	if (g_state.gameDisplaySettingsSavePending) {
		SavePspGameDbDisplaySettings();
		g_state.gameDisplaySettingsSavePending = false;
	}
	if (g_state.fastForwardSettingsSavePending) {
		SavePspFastForwardSettings();
	}
	// Direct system exits bypass the menu's progress dialog.  Keep this
	// fallback, but skip it after the menu path has already saved the slot.
	if (g_state.autoSaveOnExitSlot > 0 && !g_state.exitSaveFinished) {
		const int slot = std::clamp(g_state.autoSaveOnExitSlot - 1, 0, Ppsspp::SaveStateSlotCount - 1);
		const std::string statePath = GetPspSaveStatePath(slot);
		Log("GBAStation auto save on exit slot=%d path=%s", slot, statePath.c_str());
		if (!statePath.empty()) {
			File::CreateFullPath(Path(Paths::PpssppSaveStates));
			SaveState::Save(Path(statePath), slot, &AfterSaveStateAction);
			SaveState::Process();
			WriteStateThumbnail(statePath);
		}
	}
	// Exit: update title/logoPath in the cached GameDB entry. These follow their
	// own rules (default-filename title / default-resource logo) and do not
	// depend on whether ICON0 was actually extracted.
	if (g_state.gameDbLoaded) {
		const std::string realTitle = g_paramSFO.GetValueString("TITLE");
		UpdatePspGameDbTitleAndLogo(realTitle, PspIcon0Path());
		FlushPspGameDb();
	}
	SavePspPlayStats(g_state.contentPath);
	WaitForCheatLoadThread();
	DrainMainThreadQueue();
	if (g_state.hostFrameOpen && gpu) {
		gpu->EndHostFrame();
		g_state.hostFrameOpen = false;
	}
	if (g_state.frameOpen && g_state.draw) {
		g_state.draw->EndFrame();
		g_state.frameOpen = false;
	}
	RetroAchievements().Shutdown();
	if (PSP_IsInited() && !g_state.ppssppShutdown) {
		PSP_Shutdown(true);
		g_state.ppssppShutdown = true;
		Log("PSP_Shutdown complete");
	}
	g_state.overlay.Shutdown();
	TrophySfx().Shutdown();
	ShutdownAudio();
	ShutdownGBAStationGraphicsHost();
	g_state.graphicsContext = nullptr;
	g_state.draw = nullptr;
	g_VFS.Clear();
	g_logManager.Shutdown();
	g_threadManager.Teardown();
}

int RuntimeAudioSampleRate() {
	return g_state.audioSampleRate > 0 ? g_state.audioSampleRate : kAudioSampleRate;
}

int RuntimeAudioBufferSamples() {
	return g_state.audioBufferSamples > 0 ? g_state.audioBufferSamples : kAudioSamples;
}

void RuntimeRequestExit() {
	g_state.running = false;
	Core_Stop();
}

void RuntimeRunOnMainThread(std::function<void()> func) {
	QueueMainThread(std::move(func));
}

void RuntimeAudioGetDebugStats(char *buf, size_t bufSize) {
	if (buf) {
		g_state.resampler.GetAudioDebugStats(buf, bufSize);
	} else {
		g_state.resampler.ResetStatCounters();
	}
}

void RuntimeAudioClear() {
	g_state.resampler.Clear();
}

void RuntimeAudioPushSamples(const s32 *audio, int numSamples, float volume) {
	if (audio && g_state.audioReady) {
		g_state.resampler.PushSamples(audio, numSamples, volume);
	} else {
		g_state.resampler.Clear();
	}
}

void PersistPspGameDbShaderSettings() {
	SavePspGameDbShaderSettings();
}

void NotifyPspGpuConfigChanged() {
	const char *first = g_Config.vPostShaderNames.empty() ? "<off>" : g_Config.vPostShaderNames.front().c_str();
	Log("GBAStation GPU config changed postShaderCount=%u first=%s gpu=%p",
		(unsigned)g_Config.vPostShaderNames.size(), first, (void *)gpu);
	if (gpu) {
		gpu->NotifyConfigChanged();
	}
}

}  // namespace GBAStation

void NativeFrame(GraphicsContext *) {
}

void NativeResized() {
}

void System_Toast(std::string_view text) {
	std::fprintf(stderr, "%.*s\n", (int)text.size(), text.data());
}

void System_ShowKeyboard() {
}

void System_Vibrate(int) {
}

void System_LaunchUrl(LaunchUrlType, std::string_view) {
}

std::string System_GetProperty(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_NAME:
		return "PPSSPP GBAStation";
	case SYSPROP_AUDIO_DEVICE_LIST:
		return "";
	default:
		return "";
	}
}

std::vector<std::string> System_GetPropertyStringVec(SystemProperty) {
	return {};
}

int64_t System_GetPropertyInt(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_SYSTEMVERSION:
		return 31;
	case SYSPROP_DISPLAY_XRES:
		return g_display.pixel_xres;
	case SYSPROP_DISPLAY_YRES:
		return g_display.pixel_yres;
	case SYSPROP_DISPLAY_COUNT:
		return 1;
	case SYSPROP_DISPLAY_DPI:
	case SYSPROP_DISPLAY_LOGICAL_DPI:
		return 160;
	case SYSPROP_AUDIO_SAMPLE_RATE:
	case SYSPROP_AUDIO_OPTIMAL_SAMPLE_RATE:
		return GBAStation::RuntimeAudioSampleRate();
	case SYSPROP_AUDIO_FRAMES_PER_BUFFER:
	case SYSPROP_AUDIO_OPTIMAL_FRAMES_PER_BUFFER:
		return GBAStation::RuntimeAudioBufferSamples();
	case SYSPROP_DEVICE_TYPE:
		return DEVICE_TYPE_DESKTOP;
	default:
		return -1;
	}
}

float System_GetPropertyFloat(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_DISPLAY_REFRESH_RATE:
		return 60.0f;
	case SYSPROP_DISPLAY_DPI:
	case SYSPROP_DISPLAY_LOGICAL_DPI:
		return 160.0f;
	case SYSPROP_DISPLAY_SAFE_INSET_LEFT:
	case SYSPROP_DISPLAY_SAFE_INSET_RIGHT:
	case SYSPROP_DISPLAY_SAFE_INSET_TOP:
	case SYSPROP_DISPLAY_SAFE_INSET_BOTTOM:
		return 0.0f;
	default:
		return -1.0f;
	}
}

bool System_GetPropertyBool(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_CAN_JIT:
		return true;
	case SYSPROP_SKIP_UI:
		return false;
	case SYSPROP_SUPPORTS_HTTPS:
		return false;
	case SYSPROP_SUPPORTS_PERMISSIONS:
	case SYSPROP_HAS_BACK_BUTTON:
	case SYSPROP_HAS_KEYBOARD:
	case SYSPROP_HAS_ACCELEROMETER:
	case SYSPROP_APP_GOLD:
		return false;
	default:
		return false;
	}
}

void System_Notify(SystemNotification) {
}

void System_PostUIMessage(UIMessage message, std::string_view) {
	// The standalone GBAStation host has no NativeApp message loop.  PPSSPP UI
	// screens normally use this route for dynamic post-processing changes, so
	// forwarding it here is essential for Slang enable/disable to rebuild the
	// presentation chain on the next host frame.
	if (!gpu) {
		return;
	}
	switch (message) {
	case UIMessage::GPU_CONFIG_CHANGED:
		GBAStation::NotifyPspGpuConfigChanged();
		break;
	case UIMessage::GPU_RENDER_RESIZED:
		gpu->NotifyRenderResized(g_Config.GetDisplayLayoutConfig(g_display.GetDeviceOrientation()));
		break;
	case UIMessage::GPU_DISPLAY_RESIZED:
		gpu->NotifyDisplayResized();
		break;
	default:
		break;
	}
}

void System_RunOnMainThread(std::function<void()> func) {
	GBAStation::RuntimeRunOnMainThread(std::move(func));
}

bool System_MakeRequest(SystemRequestType type, int, const std::string &param1, const std::string &, int64_t, int64_t) {
	switch (type) {
	case SystemRequestType::SEND_DEBUG_OUTPUT:
		if (!param1.empty()) {
			fwrite(param1.data(), sizeof(char), param1.size(), stdout);
		}
		return true;
	case SystemRequestType::EXIT_APP:
	case SystemRequestType::RESTART_APP:
		GBAStation::RuntimeRequestExit();
		return true;
	default:
		return false;
	}
}

void System_AskForPermission(SystemPermission) {
}

PermissionStatus System_GetPermissionStatus(SystemPermission) {
	return PERMISSION_STATUS_GRANTED;
}

void System_AudioGetDebugStats(char *buf, size_t bufSize) {
	GBAStation::RuntimeAudioGetDebugStats(buf, bufSize);
}

void System_AudioClear() {
	GBAStation::RuntimeAudioClear();
}

void System_AudioPushSamples(const s32 *audio, int numSamples, float volume) {
	GBAStation::RuntimeAudioPushSamples(audio, numSamples, volume);
}

bool NativeSaveSecret(std::string_view, std::string_view) {
	return false;
}

std::string NativeLoadSecret(std::string_view) {
	return "";
}

#endif
