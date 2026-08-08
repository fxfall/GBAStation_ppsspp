#include "GBAStationOverlay.h"

#include "GBAStationAudioSfx.h"
#include "GBAStationConfig.h"
#include "GBAStationRetroAchievements.h"
#include "GBAStationTranslationManager.h"
#include "GBAStationUtils.h"
#include <sys/stat.h>
#include "Core/Config.h"
#include "Common/GPU/thin3d.h"
#include "Common/Math/lin/matrix4x4.h"
#include "Common/Render/ManagedTexture.h"
#include "Common/System/Display.h"
#include "ext/imgui/imgui.h"
#include "ext/imgui/imgui_impl_thin3d.h"
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "ext/nanosvg/src/nanosvg.h"
#include "ext/nanosvg/src/nanosvgrast.h"

#include <switch.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <vector>
#include <utility>

namespace GBAStation {
namespace {

constexpr float kOverlayAnimDuration = 0.4f;

struct QuickMenuItem {
	const char *labelKey;
	enum class Action {
		Resume,
		SaveState,
		LoadState,
		Cheats,
		VideoSettings,
		CoreSettings,
		Reset,
		ExitGame,
	} action;
};

constexpr QuickMenuItem kQuickMenuItems[] = {
	{"emulator_resume", QuickMenuItem::Action::Resume},
	{"emulator_save_state", QuickMenuItem::Action::SaveState},
	{"emulator_load_state", QuickMenuItem::Action::LoadState},
	{"emulator_cheats", QuickMenuItem::Action::Cheats},
	{"emulator_video_settings", QuickMenuItem::Action::VideoSettings},
	{"emulator_core_settings", QuickMenuItem::Action::CoreSettings},
	{"emulator_reset", QuickMenuItem::Action::Reset},
	{"emulator_exit_game", QuickMenuItem::Action::ExitGame},
};

constexpr int kAnalogNavThreshold = 16000;
constexpr u64 kAnalogNavRepeatMs = 180;
constexpr int kCheatAnalogNavThreshold = 18000;
constexpr u64 kCheatVerticalNavInitialRepeatMs = 280;
constexpr u64 kCheatVerticalNavRepeatMs = 105;
constexpr u64 kCheatHorizontalNavInitialRepeatMs = 320;
constexpr u64 kCheatHorizontalNavRepeatMs = 180;
constexpr int kCheatPageStep = 10;

constexpr DisplaySize kAspectDisplaySizes[] = {
	DisplaySize::Stretch,
	DisplaySize::_4_3,
	DisplaySize::_16_9,
	DisplaySize::Original,
};

bool ReadFileBytes(const char *path, std::vector<unsigned char> *data) {
	data->clear();
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		return false;
	}
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return false;
	}
	const long fileSize = ftell(fp);
	if (fileSize <= 0) {
		fclose(fp);
		return false;
	}
	rewind(fp);

	data->resize((size_t)fileSize);
	const size_t readSize = fread(data->data(), 1, data->size(), fp);
	fclose(fp);
	if (readSize != data->size()) {
		data->clear();
		return false;
	}
	return true;
}

uint8_t *LoadImGuiFontData(const char *path, size_t *sizeOut) {
	*sizeOut = 0;
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		return nullptr;
	}

	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return nullptr;
	}
	const long fileSize = ftell(fp);
	if (fileSize <= 0) {
		fclose(fp);
		return nullptr;
	}
	rewind(fp);

	uint8_t *data = (uint8_t *)ImGui::MemAlloc((size_t)fileSize);
	if (!data) {
		fclose(fp);
		return nullptr;
	}

	const size_t readSize = fread(data, 1, (size_t)fileSize, fp);
	fclose(fp);
	if (readSize != (size_t)fileSize) {
		ImGui::MemFree(data);
		return nullptr;
	}

	*sizeOut = (size_t)fileSize;
	return data;
}

uint8_t *LoadFirstImGuiFontData(const char *const *paths, size_t pathCount, size_t *sizeOut, const char **loadedPathOut) {
	for (size_t i = 0; i < pathCount; ++i) {
		uint8_t *data = LoadImGuiFontData(paths[i], sizeOut);
		if (data) {
			if (loadedPathOut) {
				*loadedPathOut = paths[i];
			}
			return data;
		}
	}

	if (loadedPathOut) {
		*loadedPathOut = nullptr;
	}
	*sizeOut = 0;
	return nullptr;
}

uint8_t *LoadSwitchChineseFontData(size_t *sizeOut, const char **sourceName) {
#ifdef __SWITCH__
	*sizeOut = 0;
	if (sourceName) *sourceName = nullptr;
	if (R_FAILED(plInitialize(PlServiceType_User)))
		return nullptr;

	const struct {
		PlSharedFontType type;
		const char *name;
	} fonts[] = {
		{PlSharedFontType_ChineseSimplified, "ChineseSimplified"},
		{PlSharedFontType_ExtChineseSimplified, "ExtChineseSimplified"},
		{PlSharedFontType_Standard, "Standard"},
	};

	PlFontData sharedFont{};
	const char *loadedName = nullptr;
	for (const auto &candidate : fonts) {
		if (R_SUCCEEDED(plGetSharedFontByType(&sharedFont, candidate.type)) &&
			sharedFont.address && sharedFont.size > 0) {
			loadedName = candidate.name;
			break;
		}
	}
	if (!loadedName) {
		plExit();
		return nullptr;
	}

	auto *data = static_cast<uint8_t *>(std::malloc(sharedFont.size));
	if (!data) {
		plExit();
		return nullptr;
	}
	std::memcpy(data, sharedFont.address, sharedFont.size);
	plExit();
	*sizeOut = sharedFont.size;
	if (sourceName) *sourceName = loadedName;
	return data;
#else
	(void)sizeOut;
	if (sourceName) *sourceName = nullptr;
	return nullptr;
#endif
}

float EaseOutCubic(float t) {
	t = std::clamp(t, 0.0f, 1.0f);
	return 1.0f - std::pow(1.0f - t, 3.0f);
}

void EncodeUtf8(char *out, int codepoint) {
	if (codepoint <= 0x7F) {
		out[0] = (char)codepoint;
		out[1] = '\0';
	} else if (codepoint <= 0x7FF) {
		out[0] = (char)(0xC0 | (codepoint >> 6));
		out[1] = (char)(0x80 | (codepoint & 0x3F));
		out[2] = '\0';
	} else {
		out[0] = (char)(0xE0 | (codepoint >> 12));
		out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
		out[2] = (char)(0x80 | (codepoint & 0x3F));
		out[3] = '\0';
	}
}

DisplaySize CycleDisplaySize(DisplaySize current, const DisplaySize *sizes, int count, int direction) {
	int index = 0;
	for (int i = 0; i < count; ++i) {
		if (sizes[i] == current) {
			index = i;
			break;
		}
	}

	index = (index + direction) % count;
	if (index < 0) {
		index += count;
	}
	return sizes[index];
}

int GetAvailableIntegerDisplaySizes(DisplaySize *sizes, int sizeCount) {
	if (!sizes || sizeCount <= 0) {
		return 0;
	}

	int count = 0;
	const int maxScale = MaxPpssppIntegerScaleForCurrentDisplay();
	auto addSize = [&](DisplaySize size) {
		if (count < sizeCount) {
			sizes[count++] = size;
		}
	};

	if (maxScale >= 1) addSize(DisplaySize::_1x);
	if (maxScale >= 2) addSize(DisplaySize::_2x);
	if (maxScale >= 3) addSize(DisplaySize::_3x);
	if (maxScale >= 4) addSize(DisplaySize::_4x);
	addSize(DisplaySize::Auto);
	return count;
}

std::string TranslatedDisplayModeLabel(DisplayMode mode) {
	return mode == DisplayMode::Integer ? tr("emulator_integer") : tr("emulator_display");
}

std::string TranslatedDisplaySizeLabel(DisplaySize size) {
	switch (size) {
	case DisplaySize::Stretch: return tr("emulator_stretch");
	case DisplaySize::_4_3: return "4:3";
	case DisplaySize::_16_9: return "16:9";
	case DisplaySize::Original: return tr("emulator_original");
	case DisplaySize::_1x: return "1x";
	case DisplaySize::_2x: return "2x";
	case DisplaySize::_3x: return "3x";
	case DisplaySize::_4x: return "4x";
	case DisplaySize::Auto: return tr("emulator_auto");
	default: return DisplaySizeLabel(size);
	}
}

std::string TruncateToWidth(ImFont *font, float fontSize, const std::string &text, float maxWidth) {
	if (!font || text.empty() || font->CalcTextSizeA(fontSize, 10000.0f, 0.0f, text.c_str()).x <= maxWidth) {
		return text;
	}

	std::string result = text;
	while (result.size() > 4) {
		result.resize(result.size() - 2);
		const std::string candidate = result + "...";
		if (font->CalcTextSizeA(fontSize, 10000.0f, 0.0f, candidate.c_str()).x <= maxWidth) {
			return candidate;
		}
	}
	return "...";
}

u64 CurrentTimeMs() {
	const u64 tickFreq = armGetSystemTickFreq();
	const u64 ticksPerMs = tickFreq / 1000;
	return ticksPerMs > 0 ? armGetSystemTick() / ticksPerMs : 0;
}

bool HeldNavigationTriggered(int heldDir, int &activeDir, u64 &nextRepeatMs, u64 initialRepeatMs, u64 repeatMs, u64 nowMs) {
	if (heldDir == 0) {
		activeDir = 0;
		nextRepeatMs = 0;
		return false;
	}
	if (heldDir != activeDir) {
		activeDir = heldDir;
		nextRepeatMs = nowMs + initialRepeatMs;
		return true;
	}
	if (nowMs >= nextRepeatMs) {
		nextRepeatMs = nowMs + repeatMs;
		return true;
	}
	return false;
}

bool IsSelectableCheatRow(const std::vector<CheatMenuEntry> &cheats, int index) {
	return index >= 0 && index < (int)cheats.size() && cheats[index].toggleable;
}

int FirstSelectableCheatRow(const std::vector<CheatMenuEntry> &cheats) {
	for (int i = 0; i < (int)cheats.size(); ++i) {
		if (IsSelectableCheatRow(cheats, i)) {
			return i;
		}
	}
	return 0;
}

int FindNextSelectableCheatRow(const std::vector<CheatMenuEntry> &cheats, int selection, int direction) {
	const int count = (int)cheats.size();
	if (count <= 0 || direction == 0) {
		return 0;
	}

	int index = std::clamp(selection, 0, count - 1);
	for (int attempts = 0; attempts < count; ++attempts) {
		index = (index + direction + count) % count;
		if (IsSelectableCheatRow(cheats, index)) {
			return index;
		}
	}
	return std::clamp(selection, 0, count - 1);
}

void MoveCheatSelectionWrapped(int &selection, const std::vector<CheatMenuEntry> &cheats, int delta) {
	if (delta == 0) {
		return;
	}
	if (cheats.empty()) {
		selection = 0;
		return;
	}

	if (!IsSelectableCheatRow(cheats, selection)) {
		selection = FindNextSelectableCheatRow(cheats, selection, delta > 0 ? 1 : -1);
		if (IsSelectableCheatRow(cheats, selection)) {
			return;
		}
	}

	const int direction = delta > 0 ? 1 : -1;
	const int steps = std::abs(delta);
	for (int i = 0; i < steps; ++i) {
		const int next = FindNextSelectableCheatRow(cheats, selection, direction);
		if (next == selection) {
			break;
		}
		selection = next;
	}
}

}  // namespace

bool Overlay::Init(Draw::DrawContext *draw, const char *gamePath, LogCallback log) {
	draw_ = draw;
	if (log) {
		log_ = std::move(log);
	}
	if (!draw) {
		return false;
	}
	if (ready_) {
		return true;
	}
	TranslationManager::Instance().Init(log_);

	IMGUI_CHECKVERSION();
	context_ = ImGui::CreateContext();
	if (!context_) {
		return false;
	}

	ImGui::SetCurrentContext(context_);
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	io.DisplaySize = ImVec2((float)std::max(1, g_display.pixel_xres), (float)std::max(1, g_display.pixel_yres));

	ImGui::StyleColorsDark();
	ImGuiStyle &style = ImGui::GetStyle();
	style.WindowRounding = 6.0f;
	style.ChildRounding = 4.0f;
	style.FrameRounding = 4.0f;
	style.GrabRounding = 3.0f;
	style.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.075f, 0.090f, 0.97f);
	style.Colors[ImGuiCol_ChildBg] = ImVec4(0.070f, 0.105f, 0.120f, 1.00f);
	style.Colors[ImGuiCol_Header] = ImVec4(0.050f, 0.420f, 0.390f, 1.00f);
	style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.070f, 0.520f, 0.480f, 1.00f);
	style.Colors[ImGuiCol_Button] = ImVec4(0.050f, 0.420f, 0.390f, 1.00f);
	style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.070f, 0.520f, 0.480f, 1.00f);

	const char *const titleFontPaths[] = {
		"romfs:/fonts/font.ttf",
		"sdmc:/GBAStation/PSP/fonts/font.ttf",
		"sdmc:/GBAStation/PSP/assets/fonts/font.ttf",
		"sdmc:/GBAStation/PSP/assets/font.ttf",
	};
	const char *const descriptionFontPaths[] = {
		"romfs:/fonts/description.ttf",
		"sdmc:/GBAStation/PSP/fonts/description.ttf",
		"sdmc:/GBAStation/PSP/assets/fonts/description.ttf",
		"sdmc:/GBAStation/PSP/assets/description.ttf",
	};
	size_t titleFontSize = 0;
	size_t descriptionFontSize = 0;
	const char *loadedTitleFont = nullptr;
	const char *loadedDescriptionFont = nullptr;
	// Thin3D now builds the Switch shared-font atlas itself so all three shared
	// faces can be merged before its texture upload.  These ROMFS files remain
	// the fallback for a failed pl:u service and non-Switch builds.
	uint8_t *titleFont = LoadFirstImGuiFontData(titleFontPaths,
		sizeof(titleFontPaths) / sizeof(titleFontPaths[0]), &titleFontSize, &loadedTitleFont);
	uint8_t *descriptionFont = titleFont ? nullptr : LoadFirstImGuiFontData(
		descriptionFontPaths, sizeof(descriptionFontPaths) / sizeof(descriptionFontPaths[0]),
		&descriptionFontSize, &loadedDescriptionFont);
	LogMessage(log_, "GBAStation overlay font title=%s size=%u description=%s size=%u",
		loadedTitleFont ? loadedTitleFont : "<default>", (unsigned)titleFontSize,
		loadedDescriptionFont ? loadedDescriptionFont : "<default>", (unsigned)descriptionFontSize);

	if (!ImGui_ImplThin3d_Init(draw, titleFont, titleFontSize, descriptionFont, descriptionFontSize)) {
		ImGui::DestroyContext(context_);
		context_ = nullptr;
		return false;
	}

	const float loadedFontSize = io.Fonts->Fonts.Size > 0 ? io.Fonts->Fonts[0]->FontSize : 21.0f;
	if (loadedFontSize > 0.0f) {
		io.FontGlobalScale = Display::FontSize / loadedFontSize;
	}
	LogMessage(log_, "GBAStation overlay font scale=%.3f base=%.1f target=%.1f",
		io.FontGlobalScale, loadedFontSize, Display::FontSize);

	title_ = GameTitleFromPath(gamePath);
	displaySettings_ = LoadPpssppDisplaySettings(log_);
	LoadFocusTexture(draw);
	ready_ = true;
	LogMessage(log_, "GBAStation overlay initialized title=%s", title_.c_str());
	return true;
}

Draw::Texture *Overlay::LoadRAIconTexture(Draw::DrawContext *draw) {
	if (raIconTexture_ || !draw) {
		return raIconTexture_;
	}

	const char *const raIconPaths[] = {
		"romfs:/assets/ra.svg",
		"sdmc:/GBAStation/PSP/assets/ra.svg",
	};
	for (const char *path : raIconPaths) {
		std::vector<unsigned char> svgData;
		if (!ReadFileBytes(path, &svgData)) {
			continue;
		}
		svgData.push_back('\0');
		NSVGimage *image = nsvgParse((char *)svgData.data(), "px", 96.0f);
		if (!image || image->width <= 0.0f || image->height <= 0.0f) {
			if (image) {
				nsvgDelete(image);
			}
			continue;
		}

		const float targetSize = 96.0f;
		const float svgMax = std::max(image->width, image->height);
		const float rasterScale = targetSize / svgMax;
		const int width = std::max(1, (int)std::ceil(image->width * rasterScale));
		const int height = std::max(1, (int)std::ceil(image->height * rasterScale));
		std::vector<unsigned char> pixels((size_t)width * (size_t)height * 4);
		NSVGrasterizer *rasterizer = nsvgCreateRasterizer();
		if (rasterizer) {
			nsvgRasterize(rasterizer, image, 0.0f, 0.0f, rasterScale, pixels.data(), width, height, width * 4);
			Draw::TextureDesc desc{};
			desc.type = Draw::TextureType::LINEAR2D;
			desc.format = Draw::DataFormat::R8G8B8A8_UNORM;
			desc.width = width;
			desc.height = height;
			desc.depth = 1;
			desc.mipLevels = 1;
			desc.generateMips = false;
			desc.tag = path;
			desc.initData.push_back(pixels.data());
			raIconTexture_ = draw->CreateTexture(desc);
			nsvgDeleteRasterizer(rasterizer);
		}
		nsvgDelete(image);
		if (raIconTexture_) {
			LogMessage(log_, "GBAStation overlay RA icon loaded path=%s", path);
			return raIconTexture_;
		}
	}

	LogMessage(log_, "GBAStation overlay RA icon not found");
	return nullptr;
}

void Overlay::ReleaseRAIconTexture() {
	if (raIconTexture_) {
		raIconTexture_->Release();
		raIconTexture_ = nullptr;
	}
}

void Overlay::LoadFocusTexture(Draw::DrawContext *draw) {
	if (focusTexture_ || !draw) {
		return;
	}
	const char *const focusPaths[] = {
		"romfs:/assets/ui/border_gradient.png",
		"sdmc:/GBAStation/PSP/assets/ui/border_gradient.png",
	};
	for (const char *path : focusPaths) {
		std::vector<unsigned char> pngData;
		if (!ReadFileBytes(path, &pngData)) {
			continue;
		}
		Draw::Texture *texture = CreateTextureFromFileData(draw_, pngData.data(), pngData.size(),
			ImageFileType::PNG, false, path);
		if (texture) {
			focusTexture_ = texture;
			LogMessage(log_, "GBAStation overlay focus texture loaded path=%s", path);
			return;
		}
	}
	LogMessage(log_, "GBAStation overlay focus texture not found");
}

void Overlay::ReleaseFocusTexture() {
	if (focusTexture_) {
		focusTexture_->Release();
		focusTexture_ = nullptr;
	}
}

void Overlay::DrawFlowBorder(ImDrawList *drawList, float x, float y, float w, float h, float thickness) {
	const float rounding = 0.0f;
	if (!focusTexture_) {
		drawList->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(79, 179, 255, 255), rounding, 0, 2.0f);
		return;
	}
	const float borderWidth = std::max(4.0f, thickness * 2.0f);
	const double milliseconds = (double)CurrentTimeMs();
	float uv = (float)std::fmod(milliseconds / 3600.0, 1.0);
	const float topLength = w + borderWidth * 2.0f;
	const float sideLength = h;
	const float advance = 1.0f / 256.0f;
	// uv spans the full gradient row (0..1); each border strip samples a
	// contiguous window so the highlight travels around the rectangle.
	const ImTextureID tex = ImGui_ImplThin3d_AddTextureTemp(focusTexture_);
	float next = uv + topLength * advance;
	const ImVec2 topMin(x - borderWidth, y - borderWidth);
	const ImVec2 topMax(x + w + borderWidth, y);
	drawList->AddImage(tex, topMin, topMax,
		ImVec2(uv, 0.0f), ImVec2(next, 1.0f));
	uv = next;
	next = uv + sideLength * advance;
	const ImVec2 rightMin(x + w, y);
	const ImVec2 rightMax(x + w + borderWidth, y + h);
	drawList->AddImage(tex, rightMin, rightMax,
		ImVec2(uv, 0.0f), ImVec2(next, 1.0f));
	uv = next;
	next = uv + topLength * advance;
	const ImVec2 bottomMin(x - borderWidth, y + h);
	const ImVec2 bottomMax(x + w + borderWidth, y + h + borderWidth);
	drawList->AddImage(tex, bottomMin, bottomMax,
		ImVec2(next, 0.0f), ImVec2(uv, 1.0f));
	uv = next;
	next = uv + sideLength * advance;
	const ImVec2 leftMin(x - borderWidth, y);
	const ImVec2 leftMax(x, y + h);
	drawList->AddImage(tex, leftMin, leftMax,
		ImVec2(next, 0.0f), ImVec2(uv, 1.0f));
	// Rounded corners: cap the square flow-frame corners with accent discs so
	// the focus follows the cell's rounded corners.
	const ImU32 corner = IM_COL32(79, 179, 255, 255);
	drawList->AddCircleFilled(ImVec2(x + rounding, y + rounding), rounding, corner, 16);
	drawList->AddCircleFilled(ImVec2(x + w - rounding, y + rounding), rounding, corner, 16);
	drawList->AddCircleFilled(ImVec2(x + rounding, y + h - rounding), rounding, corner, 16);
	drawList->AddCircleFilled(ImVec2(x + w - rounding, y + h - rounding), rounding, corner, 16);
}

void Overlay::Shutdown() {
	if (!ready_ && !context_) {
		return;
	}

	if (context_) {
		ImGui::SetCurrentContext(context_);
	}
	ReleaseRAIconTexture();
	ReleaseFocusTexture();
	for (SlotThumb &thumb : slotThumbs_) {
		if (thumb.tex) {
			thumb.tex->Release();
			thumb.tex = nullptr;
		}
	}
	if (ready_) {
		ImGui_ImplThin3d_Shutdown();
	}
	if (context_) {
		ImGui::DestroyContext(context_);
	}

	context_ = nullptr;
	ready_ = false;
	visible_ = false;
	comboDown_ = false;
	exitRequested_ = false;
	menu_ = Menu::Quick;
	selection_ = 0;
	tabSelection_ = 0;
	sidebarFocused_ = true;
	settingsSelection_ = 0;
	displaySettings_ = {};
	pendingCommand_ = {};
	slotInUse_.fill(false);
	cheatsEnabled_ = false;
	cheatsAvailable_ = false;
	cheatsLoading_ = false;
	cheatsLoadCommandSent_ = false;
	cheatsLoadingDelayFrames_ = 0;
	cheats_.clear();
	lastAnalogNavMs_ = 0;
	nextCheatVerticalNavMs_ = 0;
	nextCheatHorizontalNavMs_ = 0;
	cheatVerticalNavDir_ = 0;
	cheatHorizontalNavDir_ = 0;
}

void Overlay::SetVisible(bool visible) {
	if (visible_ == visible) {
		return;
	}

	visible_ = visible;
	menu_ = Menu::Quick;
	selection_ = 0;
	tabSelection_ = 0;
	sidebarFocused_ = true;
	settingsSelection_ = 0;
	coreSettingsPage_ = false;
	animTimer_ = 0.0f;
	lastAnalogNavMs_ = 0;
	nextCheatVerticalNavMs_ = 0;
	nextCheatHorizontalNavMs_ = 0;
	cheatVerticalNavDir_ = 0;
	cheatHorizontalNavDir_ = 0;
	if (visible_) {
		TranslationManager::Instance().Init(log_);
		displaySettings_ = LoadPpssppDisplaySettings(log_);
	} else {
		cheatsLoading_ = false;
		cheatsLoadCommandSent_ = false;
		cheatsLoadingDelayFrames_ = 0;
	}
	LogMessage(log_, "GBAStation overlay visible=%d", visible ? 1 : 0);
}

void Overlay::SetSaveStateInfo(int currentSlot, const std::array<bool, Ppsspp::SaveStateSlotCount> &slotInUse,
	const std::array<time_t, Ppsspp::SaveStateSlotCount> &slotMtime) {
	currentStateSlot_ = std::clamp(currentSlot, 0, Ppsspp::SaveStateSlotCount - 1);
	slotInUse_ = slotInUse;
	slotMtime_ = slotMtime;
}

void Overlay::SetCheatsEnabled(bool enabled) {
	cheatsEnabled_ = enabled;
	if (!enabled) {
		cheatsAvailable_ = false;
		cheatsLoading_ = false;
		cheatsLoadCommandSent_ = false;
		cheatsLoadingDelayFrames_ = 0;
		cheats_.clear();
		if (menu_ == Menu::Cheats) {
			menu_ = Menu::Quick;
			selection_ = 0;
			animTimer_ = kOverlayAnimDuration;
		}
	}
}

void Overlay::SetCheatInfo(bool enabled, bool available, const std::vector<CheatMenuEntry> &entries) {
	const bool wasLoading = cheatsLoading_;
	cheatsEnabled_ = enabled;
	cheatsAvailable_ = available;
	cheatsLoading_ = false;
	cheatsLoadCommandSent_ = false;
	cheatsLoadingDelayFrames_ = 0;
	cheats_ = entries;
	if (wasLoading && visible_ && menu_ == Menu::Quick) {
		menu_ = Menu::Cheats;
		selection_ = FirstSelectableCheatRow(cheats_);
		animTimer_ = kOverlayAnimDuration;
	} else if (menu_ == Menu::Cheats && !IsSelectableCheatRow(cheats_, selection_)) {
		selection_ = FirstSelectableCheatRow(cheats_);
	}
}

void Overlay::ReloadDisplaySettings() {
	displaySettings_ = LoadPpssppDisplaySettings(log_);
}

OverlayCommand Overlay::ConsumeCommand() {
	if (pendingCommand_.action == OverlayAction::None && cheatsLoading_ && !cheatsLoadCommandSent_) {
		if (cheatsLoadingDelayFrames_ > 0) {
			cheatsLoadingDelayFrames_--;
			return {};
		}
		cheatsLoadCommandSent_ = true;
		return { OverlayAction::LoadCheats, 0 };
	}
	OverlayCommand command = pendingCommand_;
	pendingCommand_ = {};
	return command;
}

int Overlay::QuickMenuStorageIndex(int visibleIndex) const {
	return std::clamp(visibleIndex, 0,
		static_cast<int>(sizeof(kQuickMenuItems) / sizeof(kQuickMenuItems[0])) - 1);
}

void Overlay::ActivateTab(int tab) {
	tabSelection_ = std::clamp(tab, 0, 7);
	selection_ = 0;
	settingsSelection_ = 0;
	sidebarFocused_ = true;

	switch (tabSelection_) {
	case 1:
		saveStateMode_ = OverlayAction::SaveState;
		menu_ = Menu::SaveStates;
		selection_ = currentStateSlot_;
		break;
	case 2:
		saveStateMode_ = OverlayAction::LoadState;
		menu_ = Menu::SaveStates;
		selection_ = currentStateSlot_;
		break;
	case 3:
		menu_ = Menu::Cheats;
		if (!cheatsLoading_) {
			cheatsLoading_ = true;
			cheatsLoadCommandSent_ = false;
			cheatsLoadingDelayFrames_ = 1;
		}
		break;
	case 4:
		menu_ = Menu::Settings;
		coreSettingsPage_ = false;
		break;
	case 5:
		menu_ = Menu::Settings;
		coreSettingsPage_ = true;
		break;
	default:
		menu_ = Menu::Quick;
		break;
	}
	animTimer_ = kOverlayAnimDuration;
}

int Overlay::ItemCount() const {
	if (menu_ == Menu::Quick) {
		return static_cast<int>(sizeof(kQuickMenuItems) / sizeof(kQuickMenuItems[0]));
	}
	if (menu_ == Menu::SaveStates) {
		return Ppsspp::SaveStateSlotCount;
	}
	if (menu_ == Menu::Cheats) {
		return std::max(1, (int)cheats_.size());
	}
	return coreSettingsPage_ ? 5 : 9;
}

void Overlay::ApplyDisplaySettings(bool save) {
	displaySettings_ = NormalizePpssppDisplaySettingsForCurrentMode(displaySettings_);
	ApplyPpssppDisplaySettings(displaySettings_);
	if (save) {
		SavePpssppDisplaySettings(displaySettings_, log_);
	}
}

void Overlay::CycleSetting(int direction) {
	if (direction == 0) {
		return;
	}

	if (coreSettingsPage_) {
		// 功能设置: 非画面项目（快速内存/硬件变换/跳缓冲 + 快进）。
		switch (settingsSelection_) {
		case 0: g_Config.bFastMemory = !g_Config.bFastMemory; break;
		case 1: g_Config.bHardwareTransform = !g_Config.bHardwareTransform; break;
		case 2: g_Config.bSkipBufferEffects = !g_Config.bSkipBufferEffects; break;
		case 3: {
			static const float kMultipliers[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
			constexpr int kCount = 5;
			float cur = GetPspFastForwardMultiplier();
			int idx = 2;
			for (int i = 0; i < kCount; ++i) {
				if (cur <= kMultipliers[i] + 0.01f) { idx = i; break; }
			}
			idx = (idx + direction + kCount) % kCount;
			SetPspFastForwardMultiplier(kMultipliers[idx]);
			break;
		}
		case 4:
			SetPspFastForwardToggleMode(!GetPspFastForwardToggleMode());
			break;
		default: break;
		}
		coreSettingsChanged_ = true;
		return;
	}
	// 画面设置 page: 渲染分辨率 + 显示模式/比例 + 画面相关核心项。
	if (settingsSelection_ == 0) {
		g_Config.iInternalResolution = std::clamp(g_Config.iInternalResolution + direction, 0, 5);
		coreSettingsChanged_ = true;
		return;
	}
	if (settingsSelection_ == 1) {
		displaySettings_ = NormalizePpssppDisplaySettingsForCurrentMode(displaySettings_);
		displaySettings_.mode = displaySettings_.mode == DisplayMode::Integer ? DisplayMode::Display : DisplayMode::Integer;
		displaySettings_.size = displaySettings_.mode == DisplayMode::Integer ? DisplaySize::Auto : DisplaySize::_16_9;
		ApplyDisplaySettings(true);
		return;
	}
	if (settingsSelection_ == 2) {
		displaySettings_ = NormalizePpssppDisplaySettingsForCurrentMode(displaySettings_);
		if (displaySettings_.mode == DisplayMode::Integer) {
			DisplaySize integerSizes[5];
			const int integerSizeCount = GetAvailableIntegerDisplaySizes(integerSizes, (int)(sizeof(integerSizes) / sizeof(integerSizes[0])));
			displaySettings_.size = CycleDisplaySize(displaySettings_.size, integerSizes, integerSizeCount, direction);
		} else {
			displaySettings_.size = CycleDisplaySize(displaySettings_.size, kAspectDisplaySizes,
				(int)(sizeof(kAspectDisplaySizes) / sizeof(kAspectDisplaySizes[0])), direction);
		}
		ApplyDisplaySettings(true);
		return;
	}
	switch (settingsSelection_) {
	case 3: g_Config.iFrameSkip = (g_Config.iFrameSkip + direction + 6) % 6; break;
	case 4: g_Config.bAutoFrameSkip = !g_Config.bAutoFrameSkip; break;
	case 5: g_Config.bVSync = !g_Config.bVSync; break;
	case 6: g_Config.iTexFiltering = g_Config.iTexFiltering >= 4 ? 1 : g_Config.iTexFiltering + 1; break;
	case 7: g_Config.iAnisotropyLevel = (g_Config.iAnisotropyLevel + direction + 5) % 5; break;
	case 8: g_Config.bTexDeposterize = !g_Config.bTexDeposterize; break;
	default: break;
	}
	coreSettingsChanged_ = true;
}

void Overlay::ExecuteSelection() {
	const int itemCount = ItemCount();
	if (selection_ < 0 || selection_ >= itemCount) {
		return;
	}

	if (menu_ == Menu::Settings) {
		CycleSetting(1);
		return;
	}

	if (menu_ == Menu::SaveStates) {
		if (saveStateMode_ == OverlayAction::LoadState && !slotInUse_[selection_]) {
			LogMessage(log_, "GBAStation load state ignored empty slot=%d", selection_);
			return;
		}
		pendingCommand_ = { saveStateMode_, selection_ };
		SetVisible(false);
		return;
	}

	if (menu_ == Menu::Cheats) {
		if (cheats_.empty() || !cheats_[selection_].toggleable || cheats_[selection_].sourceIndex < 0) {
			return;
		}
		pendingCommand_ = { OverlayAction::ToggleCheat, cheats_[selection_].sourceIndex };
		return;
	}

	const QuickMenuItem &item = kQuickMenuItems[QuickMenuStorageIndex(tabSelection_)];
	if (item.action == QuickMenuItem::Action::SaveState || item.action == QuickMenuItem::Action::LoadState) {
		saveStateMode_ = item.action == QuickMenuItem::Action::SaveState ? OverlayAction::SaveState : OverlayAction::LoadState;
		menu_ = Menu::SaveStates;
		selection_ = currentStateSlot_;
		animTimer_ = kOverlayAnimDuration;
	} else if (item.action == QuickMenuItem::Action::Cheats) {
		if (!cheatsLoading_) {
			cheatsLoading_ = true;
			cheatsLoadCommandSent_ = false;
			cheatsLoadingDelayFrames_ = 1;
			pendingCommand_ = {};
		}
		menu_ = Menu::Cheats;
		selection_ = 0;
		animTimer_ = kOverlayAnimDuration;
	} else if (item.action == QuickMenuItem::Action::VideoSettings ||
		item.action == QuickMenuItem::Action::CoreSettings) {
		menu_ = Menu::Settings;
		coreSettingsPage_ = item.action == QuickMenuItem::Action::CoreSettings;
		selection_ = 0;
		settingsSelection_ = 0;
		animTimer_ = kOverlayAnimDuration;
	} else if (item.action == QuickMenuItem::Action::Resume) {
		SetVisible(false);
	} else if (item.action == QuickMenuItem::Action::Reset) {
		pendingCommand_ = { OverlayAction::Reset, 0 };
		SetVisible(false);
	} else {
		exitRequested_ = true;
	}
}

bool Overlay::HandleInput(u64 buttons, u64 pressed, int leftStickX, int leftStickY, int rightStickX, int rightStickY,
	bool menuTogglePressed) {
	const bool wasVisible = visible_;
	if (menuTogglePressed) {
		if (!visible_) {
			SetVisible(true);
		} else {
			sidebarFocused_ = true;
		}
	}

	if (visible_) {
		const int itemCount = ItemCount();
		bool navUp = (pressed & HidNpadButton_Up) != 0;
		bool navDown = (pressed & HidNpadButton_Down) != 0;
		// The 3DS menu treats the physical L / R shoulders as Left / Right so
		// the LR value selectors can be adjusted without the d-pad.
		bool navLeft = (pressed & (HidNpadButton_Left | HidNpadButton_L)) != 0;
		bool navRight = (pressed & (HidNpadButton_Right | HidNpadButton_R)) != 0;
		// Hold-to-repeat for the d-pad (3DS feel): first press fires on the
		// edge, the first repeat waits 280 ms, then repeats speed up
		// (128 ms -> 48 ms while held).
		const int heldV = ((buttons & HidNpadButton_Up) ? -1 : 0) | ((buttons & HidNpadButton_Down) ? 1 : 0);
		const int pressedV = ((pressed & HidNpadButton_Up) ? -1 : 0) | ((pressed & HidNpadButton_Down) ? 1 : 0);
		const u64 nowMs = CurrentTimeMs();
		if (pressedV != 0) {
			dpadNavDir_ = pressedV;
			nextDPadNavMs_ = nowMs + kDPadNavInitialRepeatMs;
			dpadNavStartMs_ = nowMs;
			navUp = pressedV < 0;
			navDown = pressedV > 0;
		} else if (heldV != 0 && dpadNavDir_ == heldV && nowMs >= nextDPadNavMs_) {
			const u64 heldMs = nowMs - dpadNavStartMs_;
			u64 interval = 128 - heldMs * 12 / 100;
			if (interval < 48) {
				interval = 48;
			}
			nextDPadNavMs_ = nowMs + interval;
			navUp = heldV < 0;
			navDown = heldV > 0;
		}
		if (heldV == 0) {
			dpadNavDir_ = 0;
			nextDPadNavMs_ = 0;
		}
		if (menu_ == Menu::Cheats) {
			navUp = false;
			navDown = false;
			navLeft = false;
			navRight = false;

			const bool analogUp = leftStickY > kCheatAnalogNavThreshold || rightStickY > kCheatAnalogNavThreshold;
			const bool analogDown = leftStickY < -kCheatAnalogNavThreshold || rightStickY < -kCheatAnalogNavThreshold;
			const bool analogLeft = leftStickX < -kCheatAnalogNavThreshold || rightStickX < -kCheatAnalogNavThreshold;
			const bool analogRight = leftStickX > kCheatAnalogNavThreshold || rightStickX > kCheatAnalogNavThreshold;
			const int verticalPressedDir = (pressed & HidNpadButton_Up) ? -1 : ((pressed & HidNpadButton_Down) ? 1 : 0);
			const int verticalHeldDir = ((buttons & HidNpadButton_Up) || analogUp) ? -1 : (((buttons & HidNpadButton_Down) || analogDown) ? 1 : 0);
			const int horizontalPressedDir = (pressed & HidNpadButton_Left) ? -1 : ((pressed & HidNpadButton_Right) ? 1 : 0);
			const int horizontalHeldDir = ((buttons & HidNpadButton_Left) || analogLeft) ? -1 : (((buttons & HidNpadButton_Right) || analogRight) ? 1 : 0);
			const u64 nowMs = CurrentTimeMs();

			if (verticalPressedDir != 0) {
				cheatVerticalNavDir_ = verticalPressedDir;
				nextCheatVerticalNavMs_ = nowMs + kCheatVerticalNavInitialRepeatMs;
				navUp = verticalPressedDir < 0;
				navDown = verticalPressedDir > 0;
			} else if (HeldNavigationTriggered(verticalHeldDir, cheatVerticalNavDir_, nextCheatVerticalNavMs_,
				kCheatVerticalNavInitialRepeatMs, kCheatVerticalNavRepeatMs, nowMs)) {
				navUp = verticalHeldDir < 0;
				navDown = verticalHeldDir > 0;
			}

			if (!navUp && !navDown) {
				if (horizontalPressedDir != 0) {
					cheatHorizontalNavDir_ = horizontalPressedDir;
					nextCheatHorizontalNavMs_ = nowMs + kCheatHorizontalNavInitialRepeatMs;
					navLeft = horizontalPressedDir < 0;
					navRight = horizontalPressedDir > 0;
				} else if (HeldNavigationTriggered(horizontalHeldDir, cheatHorizontalNavDir_, nextCheatHorizontalNavMs_,
					kCheatHorizontalNavInitialRepeatMs, kCheatHorizontalNavRepeatMs, nowMs)) {
					navLeft = horizontalHeldDir < 0;
					navRight = horizontalHeldDir > 0;
				}
			}

			lastAnalogNavMs_ = 0;
		} else {
			cheatVerticalNavDir_ = 0;
			cheatHorizontalNavDir_ = 0;
			nextCheatVerticalNavMs_ = 0;
			nextCheatHorizontalNavMs_ = 0;

			const bool analogUp = leftStickY > kAnalogNavThreshold || rightStickY > kAnalogNavThreshold;
			const bool analogDown = leftStickY < -kAnalogNavThreshold || rightStickY < -kAnalogNavThreshold;
			const bool analogLeft = leftStickX < -kAnalogNavThreshold || rightStickX < -kAnalogNavThreshold;
			const bool analogRight = leftStickX > kAnalogNavThreshold || rightStickX > kAnalogNavThreshold;
			if (analogUp || analogDown || analogLeft || analogRight) {
				const u64 nowMs = CurrentTimeMs();
				if (nowMs == 0 || nowMs - lastAnalogNavMs_ >= kAnalogNavRepeatMs) {
					navUp = navUp || analogUp;
					navDown = navDown || (!analogUp && analogDown);
					if (!analogUp && !analogDown) {
						navLeft = navLeft || analogLeft;
						navRight = navRight || (!analogLeft && analogRight);
					}
					lastAnalogNavMs_ = nowMs;
				}
			} else {
				lastAnalogNavMs_ = 0;
			}
		}

		if (sidebarFocused_) {
			if (navUp) { ActivateTab((tabSelection_ + 7) % 8);  }
			if (navDown) { ActivateTab((tabSelection_ + 1) % 8);  }
			if (navRight && (menu_ == Menu::SaveStates || menu_ == Menu::Cheats || menu_ == Menu::Settings)) {
				sidebarFocused_ = false;
				
			}
			if (pressed & HidNpadButton_A) {
				if (tabSelection_ == 0) { SetVisible(false);  }
				else if (tabSelection_ == 6) { pendingCommand_ = { OverlayAction::Reset, 0 }; SetVisible(false);  }
				else if (tabSelection_ == 7) { exitRequested_ = true;  }
				else if (menu_ == Menu::SaveStates || menu_ == Menu::Cheats || menu_ == Menu::Settings) {
					sidebarFocused_ = false;
					
				}
			}
			if (pressed & HidNpadButton_B) {
				SetVisible(false);
				
			}
			return true;
		}

		if (menu_ == Menu::Cheats) {
			if (navUp) {
				MoveCheatSelectionWrapped(selection_, cheats_, -1);
				
			}
			if (navDown) {
				MoveCheatSelectionWrapped(selection_, cheats_, 1);
				
			}
			if (navLeft) {
				MoveCheatSelectionWrapped(selection_, cheats_, -kCheatPageStep);
				
			}
			if (navRight) {
				MoveCheatSelectionWrapped(selection_, cheats_, kCheatPageStep);
				
			}
		} else {
			if (menu_ == Menu::SaveStates) {
				// Two-column grid: up/down step a whole row, left/right move
				// between columns.
				constexpr int kColumns = 2;
				if (navUp && itemCount > 0) {
					selection_ = (selection_ + itemCount - kColumns) % itemCount;
					
				}
				if (navDown && itemCount > 0) {
					selection_ = (selection_ + kColumns) % itemCount;
					
				}
				if (navLeft && itemCount > 0) {
					const int col = selection_ % kColumns;
					selection_ = (selection_ - 1 + itemCount) % itemCount;
					if (col == 0 && selection_ >= itemCount) {
						selection_ = itemCount - 1;
					}
					
				}
				if (navRight && itemCount > 0) {
					selection_ = (selection_ + 1) % itemCount;
					
				}
			} else {
				if (navUp && itemCount > 0) {
					selection_ = (selection_ + itemCount - 1) % itemCount;
					
				}
				if (navDown && itemCount > 0) {
					selection_ = (selection_ + 1) % itemCount;
					
				}
			}
		}
		if (menu_ == Menu::Settings) {
			settingsSelection_ = selection_;
			if (navLeft) {
				CycleSetting(-1);
				
			}
			if (navRight) {
				CycleSetting(1);
				
			}
		}
		if (pressed & HidNpadButton_A) {
			ExecuteSelection();
			
		}
		if (menu_ == Menu::Quick && (pressed & HidNpadButton_Minus) && !(buttons & HidNpadButton_Plus)) {
			pendingCommand_ = { OverlayAction::Reset, 0 };
			SetVisible(false);
			return true;
		}
		if (pressed & HidNpadButton_B) {
			if (menu_ != Menu::Quick) {
				sidebarFocused_ = true;
			} else {
				SetVisible(false);
			}
			
		}
	}

	return wasVisible || visible_ || menuTogglePressed;
}

void Overlay::DrawBackground(ImDrawList *drawList, ImVec2 displaySize, float ease) {
	const int baseAlpha = (int)(72.0f * ease);
	const int maxAlpha = (int)(110.0f * ease);
	if (baseAlpha <= 0) {
		return;
	}

	const float topH = displaySize.y * 0.20f;
	const float bottomH = displaySize.y * 0.20f;
	const float centerH = displaySize.y - topH - bottomH;
	const ImU32 colMax = IM_COL32(0, 0, 0, maxAlpha);
	const ImU32 colBase = IM_COL32(0, 0, 0, baseAlpha);

	drawList->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), ImVec2(displaySize.x, topH), colMax, colMax, colBase, colBase);
	drawList->AddRectFilled(ImVec2(0.0f, topH), ImVec2(displaySize.x, topH + centerH), colBase);
	drawList->AddRectFilledMultiColor(ImVec2(0.0f, displaySize.y - bottomH), displaySize, colBase, colBase, colMax, colMax);
}

void Overlay::DrawMenu(ImDrawList *drawList, ImVec2 displaySize, float scale, float ease) {
	// GBAStation 3DS shell: left sidebar + right content.  The active tab stays
	// visible while its rows change; entering a page moves focus to the rows
	// and the sidebar falls back to a static highlight.
	const float width = displaySize.x;
	const float height = displaySize.y;
	const ImVec2 min(0.0f, 0.0f);
	const ImVec2 max(min.x + width, min.y + height);
	const std::string tabs[] = {tr("返回游戏"), tr("保存状态"), tr("读取状态"), tr("金手指"), tr("画面设置"), tr("功能设置"), tr("重置游戏"), tr("退出游戏")};
	const int icons[] = {0xE5C4, 0xE161, 0xE2C6, 0xE3AE, 0xE333, 0xE8B8, 0xE5D5, 0xE879};
	const std::string descriptions[] = {tr("继续当前游戏。"), tr("创建即时存档。"), tr("读取即时存档。"), tr("管理游戏金手指。"), tr("调整渲染分辨率和画面比例。"), tr("调整可即时生效的核心选项。"), tr("重新启动当前游戏。"), tr("返回 GBAStation。")};
	const int active = tabSelection_;

	// 3DS palette
	const ImU32 white = IM_COL32(240, 247, 255, (int)(255.0f * ease));
	const ImU32 muted = IM_COL32(184, 204, 224, (int)(199.0f * ease));
	const ImU32 cyan = IM_COL32(112, 204, 255, (int)(255.0f * ease));
	const ImU32 focusBg = IM_COL32(0, 77, 128, (int)(133.0f * ease));
	const ImU32 contentFocusBg = IM_COL32(33, 107, 179, (int)(51.0f * ease));
	const ImU32 rowBg = IM_COL32(255, 255, 255, (int)(11.0f * ease));
	const ImU32 rowBorder = IM_COL32(255, 255, 255, (int)(26.0f * ease));
	const ImU32 focusBorder = IM_COL32(79, 179, 255, (int)(128.0f * ease));

	ImFont *font = ImGui::GetFont();
	const float fontSize = ImGui::GetFontSize();

	// Background: vertical gradient strips like the 3DS shell.
	for (int strip = 0; strip < 8; ++strip) {
		const float t = (float)strip / 7.0f;
		const int r = (int)((20.0f - t * 8.0f) * ease);
		const int g = (int)((25.0f - t * 10.0f) * ease);
		const int b = (int)((33.0f - t * 13.0f) * ease);
		drawList->AddRectFilled(ImVec2(0.0f, strip * (height / 8.0f)),
			ImVec2(width, (strip + 1) * (height / 8.0f)), IM_COL32(r, g, b, 240));
	}

	// Title
	drawList->AddText(font, 26.0f * scale, ImVec2(64.0f * scale, 58.0f * scale), white, tr("游戏菜单").c_str());
	drawList->AddRectFilled(ImVec2(56.0f * scale, 92.0f * scale),
		ImVec2(width - 56.0f * scale, 93.0f * scale), IM_COL32(255, 255, 255, (int)(46.0f * ease)));

	// Sidebar
	const float sidebarX = 48.0f * scale;
	const float sidebarY = 116.0f * scale;
	const float sidebarW = 336.0f * scale;
	const float itemH = 58.0f * scale;
	const float step = 64.0f * scale;
	for (int i = 0; i < 8; ++i) {
		const float y = sidebarY + i * step;
		const bool selected = i == active;
		const bool tabFocused = selected && sidebarFocused_;
		const ImVec2 itemMin(sidebarX, y), itemMax(sidebarX + sidebarW, y + itemH);
		if (selected) {
			drawList->AddRectFilled(itemMin, itemMax, tabFocused ? focusBg : contentFocusBg);
			if (tabFocused) {
				if (focusTexture_) {
					DrawFlowBorder(drawList, sidebarX, y, sidebarW, itemH, 3.0f * scale);
				} else {
					drawList->AddRect(itemMin, itemMax, IM_COL32(79, 179, 255, (int)(255.0f * ease)), 0.0f, 0, 2.0f * scale);
				}
			} else {
				drawList->AddRect(itemMin, itemMax, focusBorder, 0.0f, 0, 1.0f * scale);
			}
		}
		const float textY = y + itemH * 0.5f - 21.0f * scale * 0.43f;
		char iconBuf[8];
		EncodeUtf8(iconBuf, icons[i]);
		drawList->AddText(font, 25.0f * scale, ImVec2(sidebarX + 34.0f * scale, y + itemH * 0.5f - 12.5f * scale),
			selected ? white : muted, iconBuf);
		drawList->AddText(font, 21.0f * scale, ImVec2(sidebarX + 64.0f * scale, textY),
			selected ? white : muted, tabs[i].c_str());
	}
	// Reset separator
	drawList->AddRectFilled(ImVec2(sidebarX + 18.0f * scale, sidebarY + 6.0f * step - 9.0f * scale),
		ImVec2(sidebarX + sidebarW - 18.0f * scale, sidebarY + 6.0f * step - 8.0f * scale),
		IM_COL32(255, 255, 255, (int)(36.0f * ease)));
	// Divider
	drawList->AddRectFilled(ImVec2(404.0f * scale, 110.0f * scale),
		ImVec2(405.0f * scale, 610.0f * scale), IM_COL32(255, 255, 255, (int)(20.0f * ease)));

	// Content area
	const float contentX = 432.0f * scale;
	const float contentW = 790.0f * scale;
	const float contentRight = contentX + contentW;
	const float viewTop = 176.0f * scale;
	const float viewBottom = 664.0f * scale;
	const float targetCenter = 420.0f * scale;
	const float rowH = 48.0f * scale;
	const float rowGap = 4.0f * scale;

	// Header + title underline
	drawList->AddText(font, 27.0f * scale, ImVec2(contentX, 116.0f * scale), white, tabs[active].c_str());
	drawList->AddRectFilled(ImVec2(contentX, 162.0f * scale),
		ImVec2(contentX + contentW, 163.0f * scale), IM_COL32(0, 122, 204, (int)(71.0f * ease)));

	// Rows with the selection kept centred inside [viewTop, viewBottom].
	auto drawRow = [&](int row, bool focused, const char *iconUtf8, const std::string &label,
		const std::string &value, bool selector) {
		const float y = viewTop + row * (rowH + rowGap);
		if (y + rowH < viewTop || y > viewBottom) {
			return;
		}
		const ImVec2 rowMin(contentX, y), rowMax(contentX + contentW, y + rowH);
		drawList->AddRectFilled(rowMin, rowMax, focused ? focusBg : rowBg);
		if (focused) {
			if (focusTexture_) {
				DrawFlowBorder(drawList, contentX, y, contentW, rowH, 3.0f * scale);
			} else {
				drawList->AddRect(rowMin, rowMax, IM_COL32(79, 179, 255, (int)(255.0f * ease)));
			}
		} else {
			drawList->AddRect(rowMin, rowMax, rowBorder, 0.0f, 0, 1.0f * scale);
		}
		drawList->AddText(font, 20.0f * scale, ImVec2(contentX + 24.0f * scale, y + rowH * 0.5f - 20.0f * scale * 0.43f),
			selector ? cyan : (focused ? white : muted), iconUtf8);
		drawList->AddText(font, 20.0f * scale, ImVec2(contentX + 46.0f * scale, y + rowH * 0.5f - 20.0f * scale * 0.43f),
			focused ? white : muted, label.c_str());
		if (selector) {
			// LR value selector: L / value / R like the 3DS page.
			char iconL[8], iconR[8];
			EncodeUtf8(iconL, 0xE0E4);
			EncodeUtf8(iconR, 0xE0E5);
			drawList->AddText(font, 26.0f * scale, ImVec2(contentX + contentW - 208.0f * scale, y + rowH * 0.5f - 26.0f * scale * 0.43f),
				cyan, iconL);
			// Value with truncation + focus-scroll for long text.
			const float valueSize = 18.0f * scale;
			const float valueCenterX = contentX + contentW - 122.0f * scale;
			const float valueMaxW = 86.0f * scale;
			const float valueW = font->CalcTextSizeA(valueSize, 10000.0f, 0.0f, value.c_str()).x;
			if (valueW <= valueMaxW) {
				drawList->AddText(font, valueSize,
					ImVec2(valueCenterX - valueW * 0.5f, y + rowH * 0.5f - valueSize * 0.43f),
					cyan, value.c_str());
			} else if (focused) {
				// Focus-scroll: slide the text through the fixed window.
				const float scroll = std::fmod((float)(CurrentTimeMs() % 8000) / 1000.0f, 1.0f);
				const float travel = valueW + valueMaxW;
				const float offset = (valueW + valueMaxW) * 0.5f - scroll * travel;
				drawList->PushClipRect(ImVec2(valueCenterX - valueMaxW * 0.5f, y),
					ImVec2(valueCenterX + valueMaxW * 0.5f, y + rowH), true);
				drawList->AddText(font, valueSize,
					ImVec2(valueCenterX - valueW * 0.5f + offset, y + rowH * 0.5f - valueSize * 0.43f),
					cyan, value.c_str());
				drawList->PopClipRect();
			} else {
				// Truncate with an ellipsis when idle.
				std::string clipped = value;
				while (!clipped.empty() &&
					font->CalcTextSizeA(valueSize, 10000.0f, 0.0f, (clipped + "...").c_str()).x > valueMaxW) {
					clipped.pop_back();
				}
				clipped += "...";
				const float cw = font->CalcTextSizeA(valueSize, 10000.0f, 0.0f, clipped.c_str()).x;
				drawList->AddText(font, valueSize,
					ImVec2(valueCenterX - cw * 0.5f, y + rowH * 0.5f - valueSize * 0.43f),
					cyan, clipped.c_str());
			}
			drawList->AddText(font, 26.0f * scale, ImVec2(contentX + contentW - 38.0f * scale, y + rowH * 0.5f - 26.0f * scale * 0.43f),
				cyan, iconR);
		} else {
			const float valueW = font->CalcTextSizeA(18.0f * scale, 10000.0f, 0.0f, value.c_str()).x;
			drawList->AddText(font, 18.0f * scale, ImVec2(contentX + contentW - valueW - 18.0f * scale, y + rowH * 0.5f - 18.0f * scale * 0.43f),
				cyan, value.c_str());
		}
	};

	const bool inContent = !sidebarFocused_;
	if (menu_ == Menu::SaveStates) {
		// Two-column scrolling grid of save slots: snapshot thumbnail on the
		// left, slot name + save time on the right.
		const int total = Ppsspp::SaveStateSlotCount;
		constexpr int kColumns = 2;
		const float cellW = (contentW - 14.0f * scale) * 0.5f;
		const float cellH = 112.0f * scale;
		const float cellGapX = 14.0f * scale;
		const float cellGapY = 10.0f * scale;
		const int gridH = (total + kColumns - 1) / kColumns;
		// Visible rows fit the viewport; the focused row scrolls to centre.
		const float viewportH = viewBottom - viewTop;
		const int visibleRows = std::max(1, (int)(viewportH / (cellH + cellGapY)));
		const int kRows = std::min(gridH, visibleRows);
		const int selectedRow = selection_ / kColumns;
		const int firstRow = std::clamp(selectedRow - kRows / 2, 0, std::max(0, gridH - kRows));
		for (int r = 0; r < kRows; ++r) {
			const int row = firstRow + r;
			for (int c = 0; c < kColumns; ++c) {
				const int slot = row * kColumns + c;
				if (slot >= total) {
					continue;
				}
				const float x = contentX + c * (cellW + cellGapX);
				const float y = viewTop + r * (cellH + cellGapY);
				if (y + cellH < viewTop || y > viewBottom) {
					continue;
				}
				const bool focused = inContent && slot == selection_;
				const ImVec2 cellMin(x, y), cellMax(x + cellW, y + cellH);
				drawList->AddRectFilled(cellMin, cellMax, focused ? focusBg : rowBg, 8.0f * scale);
				if (focused) {
					if (focusTexture_) {
						DrawFlowBorder(drawList, x, y, cellW, cellH, 3.0f * scale);
					} else {
						drawList->AddRect(cellMin, cellMax, IM_COL32(79, 179, 255, (int)(255.0f * ease)), 0.0f, 0, 2.0f * scale);
					}
				} else {
					drawList->AddRect(cellMin, cellMax, rowBorder, 8.0f * scale, 0, 1.0f * scale);
				}
				// Snapshot thumbnail on the left (PNG next to the state file).
				const float snapX = x + 8.0f * scale;
				const float snapW = cellW * 0.40f - 8.0f * scale;
				const float snapY = y + 8.0f * scale;
				const float snapH = cellH - 16.0f * scale;
				drawList->AddRectFilled(ImVec2(snapX, snapY), ImVec2(snapX + snapW, snapY + snapH),
					IM_COL32(255, 255, 255, focused ? 18 : 10), 6.0f * scale);
				drawList->AddRect(ImVec2(snapX, snapY), ImVec2(snapX + snapW, snapY + snapH),
					IM_COL32(255, 255, 255, focused ? 60 : 34), 6.0f * scale, 0, 1.0f * scale);
				Draw::Texture *thumbTex = nullptr;
				if (slotInUse_[slot]) {
					// Load / refresh the thumbnail when the state file changed.
					const std::string thumbPath = GetPspSaveStatePath(slot) + ".png";
					struct stat tst {};
					if (stat(thumbPath.c_str(), &tst) == 0) {
						SlotThumb &thumb = slotThumbs_[slot];
						if (thumb.tex == nullptr || thumb.mtime != tst.st_mtime) {
							if (thumb.tex) {
								thumb.tex->Release();
								thumb.tex = nullptr;
							}
							std::vector<unsigned char> pngData;
							if (ReadFileBytes(thumbPath.c_str(), &pngData) && !pngData.empty()) {
								thumb.tex = CreateTextureFromFileData(draw_, pngData.data(), pngData.size(),
									ImageFileType::PNG, false, thumbPath.c_str());
								thumb.mtime = tst.st_mtime;
							}
						}
						thumbTex = thumb.tex;
					}
				} else if (slotThumbs_[slot].tex) {
					slotThumbs_[slot].tex->Release();
					slotThumbs_[slot].tex = nullptr;
				}
				if (thumbTex) {
					const ImTextureID texId = ImGui_ImplThin3d_AddTextureTemp(thumbTex);
					drawList->AddImage(texId, ImVec2(snapX, snapY), ImVec2(snapX + snapW, snapY + snapH));
				} else {
					char snapIcon[8];
					EncodeUtf8(snapIcon, 0xE413);
					const float snapIconSize = 30.0f * scale;
					drawList->AddText(font, snapIconSize,
						ImVec2(snapX + snapW * 0.5f - snapIconSize * 0.5f,
							snapY + snapH * 0.5f - snapIconSize * 0.43f),
						IM_COL32(160, 200, 230, (int)(110.0f * ease)), snapIcon);
				}
				// Right side: slot name + save time.
				const float textX = snapX + snapW + 12.0f * scale;
				const std::string title = tr("存档槽 ") + std::to_string(slot + 1);
				drawList->AddText(font, 20.0f * scale, ImVec2(textX, y + 26.0f * scale),
					focused ? white : muted, title.c_str());
				if (slotInUse_[slot]) {
					char timeBuf[32]{};
					const time_t mtime = slotMtime_[slot];
					std::tm tm{};
					localtime_r(&mtime, &tm);
					std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &tm);
					drawList->AddText(font, 16.0f * scale, ImVec2(textX, y + cellH - 42.0f * scale),
						cyan, timeBuf);
				} else {
					drawList->AddText(font, 16.0f * scale, ImVec2(textX, y + cellH - 42.0f * scale),
						muted, tr("空存档槽").c_str());
				}
			}
		}
		// Scroll position hint above the footer (右下角提示文字上方).
		const float hintY = viewBottom - 26.0f * scale;
		const std::string scrollHint = std::to_string(selectedRow + 1) + " / " + std::to_string(gridH);
		const ImVec2 hintSize = font->CalcTextSizeA(16.0f * scale, 10000.0f, 0.0f, scrollHint.c_str());
		drawList->AddText(font, 16.0f * scale,
			ImVec2(contentX + contentW - hintSize.x, hintY),
			IM_COL32(184, 204, 224, (int)(160.0f * ease)), scrollHint.c_str());
	} else if (menu_ == Menu::Cheats) {
		if (cheats_.empty()) {
			drawList->AddText(font, 21.0f * scale, ImVec2(contentX, viewTop + 28.0f * scale), muted, tr("当前游戏没有可用金手指。").c_str());
			drawList->AddText(font, 20.0f * scale, ImVec2(contentX, viewTop + 100.0f * scale), muted, tr("已加载的金手指会在这里显示").c_str());
		} else {
			const int visible = std::min(9, (int)cheats_.size());
			const int first = std::clamp(selection_ - visible / 2, 0, std::max(0, (int)cheats_.size() - visible));
			for (int row = 0; row < visible; ++row) {
				const int index = first + row;
				char icon[8];
				EncodeUtf8(icon, 0xE3AE);
				drawRow(row, inContent && index == selection_, icon, cheats_[index].name,
					cheats_[index].enabled ? tr("开启") : tr("关闭"), false);
			}
		}
	} else if (menu_ == Menu::Settings) {
		char icon[8];
		if (coreSettingsPage_) {
			// 功能设置: 非画面项目。
			const std::string labels[] = {
				tr("快速内存"), tr("硬件变换"), tr("跳过缓冲区效果"),
				tr("快进倍率"), tr("快进模式")};
			const int rowIcons[] = {0xE896, 0xE3B6, 0xE428, 0xE8B2, 0xE8B8};
			auto enabled = [](bool value) { return value ? std::string(tr("开启")) : std::string(tr("关闭")); };
			auto settingValue = [&](int index) {
				switch (index) {
				case 0: return enabled(g_Config.bFastMemory);
				case 1: return enabled(g_Config.bHardwareTransform);
				case 2: return enabled(g_Config.bSkipBufferEffects);
				case 3: return std::to_string(static_cast<int>(GetPspFastForwardMultiplier())) + "x";
				default: return GetPspFastForwardToggleMode() ? std::string(tr("切换")) : std::string(tr("按住"));
				}
			};
			const int total = 5;
			const int visible = std::min(8, total);
			const int first = std::clamp(selection_ - visible / 2, 0, std::max(0, total - visible));
			for (int row = 0; row < visible; ++row) {
				const int index = first + row;
				EncodeUtf8(icon, rowIcons[index]);
				drawRow(row, inContent && index == selection_, icon, labels[index],
					settingValue(index), true);
			}
		} else {
			// 画面设置: 分辨率/显示模式/比例 + 画面相关核心项。
			const std::string labels[] = {tr("渲染分辨率"), tr("显示模式"), tr("画面比例"),
				tr("跳帧"), tr("自动跳帧"), tr("垂直同步"), tr("纹理过滤"), tr("各向异性过滤"), tr("纹理去色带")};
			const int rowIcons[] = {0xE333, 0xE8F1, 0xE3F4, 0xE8B8, 0xE8E5, 0xE8F1, 0xE3F4, 0xE3F4, 0xE873};
			auto enabled = [](bool value) { return value ? std::string(tr("开启")) : std::string(tr("关闭")); };
			auto settingValue = [&](int index) {
				switch (index) {
				case 0: return g_Config.iInternalResolution == 0 ? std::string(tr("自动")) : std::to_string(g_Config.iInternalResolution) + "x";
				case 1: return TranslatedDisplayModeLabel(displaySettings_.mode);
				case 2: return TranslatedDisplaySizeLabel(displaySettings_.size);
				case 3: return g_Config.iFrameSkip == 0 ? std::string(tr("关闭")) : std::to_string(g_Config.iFrameSkip) + tr(" 帧");
				case 4: return enabled(g_Config.bAutoFrameSkip);
				case 5: return enabled(g_Config.bVSync);
				case 6: {
					const std::string filters[] = {tr("默认"), tr("自动"), tr("最近邻"), tr("线性"), tr("高质量")};
					return std::string(filters[std::clamp(g_Config.iTexFiltering, 0, 4)]);
				}
				case 7: return g_Config.iAnisotropyLevel == 0 ? std::string(tr("关闭")) : std::to_string(1 << g_Config.iAnisotropyLevel) + "x";
				default: return enabled(g_Config.bTexDeposterize);
				}
			};
			const int total = 9;
			const int visible = std::min(8, total);
			const int first = std::clamp(selection_ - visible / 2, 0, std::max(0, total - visible));
			for (int row = 0; row < visible; ++row) {
				const int index = first + row;
				EncodeUtf8(icon, rowIcons[index]);
				drawRow(row, inContent && index == selection_, icon, labels[index],
					settingValue(index), true);
			}
		}
	} else {
		drawList->AddText(font, 20.0f * scale, ImVec2(contentX, 310.0f * scale),
			IM_COL32(204, 230, 250, (int)(219.0f * ease)), descriptions[active].c_str());
	}
}

void Overlay::DrawHelpers(ImDrawList *drawList, ImVec2 displaySize, float scale, float ease) {
	// 3DS-style footer: B and A button hints pinned to the bottom right.
	const std::string bLabel = (sidebarFocused_ || menu_ == Menu::Quick) ? tr("返回") : tr("返回列表");
	std::string aLabel;
	if (menu_ == Menu::SaveStates) {
		aLabel = saveStateMode_ == OverlayAction::SaveState ? tr("保存") : tr("读取");
	} else if (menu_ == Menu::Cheats) {
		aLabel = tr("切换");
	} else if (menu_ == Menu::Settings) {
		aLabel = tr("调整");
	} else {
		aLabel = tr("确定");
	}

	ImFont *font = ImGui::GetFont();
	const ImU32 hintColor = IM_COL32(184, 204, 224, (int)(199.0f * ease));
	char iconB[8], iconA[8];
	EncodeUtf8(iconB, 0xE0E1);
	EncodeUtf8(iconA, 0xE0E0);
	const float baseY = displaySize.y - 42.0f * scale;
	drawList->AddText(font, 27.0f * scale, ImVec2(1020.0f * scale, baseY - 27.0f * scale * 0.5f), hintColor, iconB);
	drawList->AddText(font, 19.0f * scale, ImVec2(1042.0f * scale, baseY - 19.0f * scale * 0.5f), hintColor, bLabel.c_str());
	drawList->AddText(font, 27.0f * scale, ImVec2(1152.0f * scale, baseY - 27.0f * scale * 0.5f), hintColor, iconA);
	drawList->AddText(font, 19.0f * scale, ImVec2(1174.0f * scale, baseY - 19.0f * scale * 0.5f), hintColor, aLabel.c_str());
}

void Overlay::DrawRAAlerts(Draw::DrawContext *draw, ImDrawList *drawList, ImVec2 displaySize, float scale, float deltaTime) {
	auto &notifications = RetroAchievements().Notifications();
	if (notifications.empty()) {
		return;
	}

	for (RANotification &notification : notifications) {
		notification.timer += deltaTime;
	}
	notifications.erase(std::remove_if(notifications.begin(), notifications.end(), [](const RANotification &notification) {
		return notification.timer >= notification.duration;
	}), notifications.end());
	if (notifications.empty()) {
		return;
	}

	const RAAlertPosition position = RetroAchievements().AlertPosition();
	const bool isTop = position == RAAlertPosition::TopLeft || position == RAAlertPosition::TopRight;
	const bool isRight = position == RAAlertPosition::TopRight || position == RAAlertPosition::BottomRight;
	const float margin = 16.0f * scale;
	const float spacing = 8.0f * scale;
	const float alertWidth = std::min(420.0f * scale, displaySize.x - margin * 2.0f);
	const float alertHeight = 100.0f * scale;
	const float padding = 12.0f * scale;
	const float cornerRadius = 14.0f * scale;
	const float badgeSize = 76.0f * scale;
	const float badgeRadius = 4.0f * scale;
	const float badgeMargin = 12.0f * scale;
	const float titleSize = ImGui::GetFontSize() * 0.85f;
	const float descriptionSize = ImGui::GetFontSize() * 0.65f;
	ImFont *font = ImGui::GetFont();
	ImFont *descriptionFont = font;
	if (ImGui::GetIO().Fonts->Fonts.Size > 1) {
		descriptionFont = ImGui::GetIO().Fonts->Fonts[1];
	}

	size_t visibleIndex = 0;
	for (size_t i = 0; i < notifications.size(); ++i) {
		RANotification &notification = notifications[i];
		if (notification.timer < 0.0f) {
			continue;
		}

		float slideProgress = 1.0f;
		if (notification.timer < notification.slideIn) {
			slideProgress = EaseOutCubic(notification.timer / notification.slideIn);
		} else if (notification.timer > notification.duration - notification.slideOut) {
			slideProgress = EaseOutCubic((notification.duration - notification.timer) / notification.slideOut);
		}
		slideProgress = std::clamp(slideProgress, 0.0f, 1.0f);
		const int alpha = (int)(230.0f * slideProgress);
		if (alpha <= 0) {
			continue;
		}

		const float stack = (alertHeight + spacing) * (float)visibleIndex;
		const float anchorX = isRight ? displaySize.x - alertWidth - margin : margin;
		const float anchorY = isTop ? margin + stack : displaySize.y - margin - alertHeight - stack;
		const float slideOffsetY = isTop
			? -(alertHeight + margin + stack) * (1.0f - slideProgress)
			: (alertHeight + margin + stack) * (1.0f - slideProgress);
		const ImVec2 min(anchorX, anchorY + slideOffsetY);
		const ImVec2 max(anchorX + alertWidth, min.y + alertHeight);

		drawList->AddRectFilled(min, max, IM_COL32(35, 35, 40, alpha), cornerRadius);
		drawList->AddRect(min, max, IM_COL32(70, 70, 80, (int)(180.0f * slideProgress)), cornerRadius, 0, 1.5f * scale);

		Draw::Texture *badgeTexture = nullptr;
		const bool isRAIcon = notification.badgeName == "ra_icon";
		if (isRAIcon) {
			badgeTexture = LoadRAIconTexture(draw);
		} else {
			badgeTexture = RetroAchievements().GetBadgeTexture(draw, notification.badgeName);
		}

		float textX = min.x + padding;
		const ImVec2 badgeMin(min.x + badgeMargin, min.y + (alertHeight - badgeSize) * 0.5f);
		const ImVec2 badgeMax(badgeMin.x + badgeSize, badgeMin.y + badgeSize);
		if (badgeTexture) {
			float drawBadgeSize = badgeSize;
			float drawBadgeX = badgeMin.x;
			float drawBadgeY = badgeMin.y;
			if (isRAIcon) {
				drawBadgeSize = badgeSize * 0.70f;
				drawBadgeX += (badgeSize - drawBadgeSize) * 0.5f;
				drawBadgeY += (badgeSize - drawBadgeSize) * 0.5f;
			}
			const ImTextureID textureId = ImGui_ImplThin3d_AddTextureTemp(badgeTexture);
			drawList->AddImageRounded(textureId, ImVec2(drawBadgeX, drawBadgeY),
				ImVec2(drawBadgeX + drawBadgeSize, drawBadgeY + drawBadgeSize),
				ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, alpha), badgeRadius);
			textX = badgeMin.x + badgeSize + badgeMargin;
		} else {
			drawList->AddRectFilled(badgeMin, badgeMax, IM_COL32(45, 45, 52, alpha), badgeRadius);
			const char *ra = "RA";
			const ImVec2 raSize = font->CalcTextSizeA(titleSize, 10000.0f, 0.0f, ra);
			drawList->AddText(font, titleSize, ImVec2((badgeMin.x + badgeMax.x - raSize.x) * 0.5f,
				(badgeMin.y + badgeMax.y - raSize.y) * 0.5f), IM_COL32(230, 230, 238, alpha), ra);
			textX = badgeMin.x + badgeSize + badgeMargin;
		}

		const float maxTextWidth = max.x - textX - padding;
		std::string title = TruncateToWidth(font, titleSize, notification.title, maxTextWidth);
		std::string description = notification.description;
		const float maxDescriptionHeight = descriptionSize * 2.5f;
		if (descriptionFont->CalcTextSizeA(descriptionSize, 10000.0f, maxTextWidth, description.c_str()).y > maxDescriptionHeight) {
			description += "...";
			while (description.size() > 4 &&
				descriptionFont->CalcTextSizeA(descriptionSize, 10000.0f, maxTextWidth, description.c_str()).y > maxDescriptionHeight) {
				description.erase(description.size() - 4, 1);
			}
		}

		const ImVec2 titleTextSize = font->CalcTextSizeA(titleSize, 10000.0f, 0.0f, title.c_str());
		const ImVec2 descriptionTextSize = descriptionFont->CalcTextSizeA(descriptionSize, 10000.0f, maxTextWidth, description.c_str());
		const float textSpacing = 4.0f * scale;
		const float totalTextHeight = titleTextSize.y + textSpacing + descriptionTextSize.y;
		const float titleY = min.y + (alertHeight - totalTextHeight) * 0.5f;
		const float descriptionY = titleY + titleTextSize.y + textSpacing;
		const ImU32 titleColor = IM_COL32(255, 255, 255, alpha);
		const ImU32 descriptionColor = IM_COL32(185, 185, 195, alpha);

		drawList->AddText(font, titleSize, ImVec2(textX + 1.0f, titleY + 1.0f),
			IM_COL32(0, 0, 0, (int)(80.0f * slideProgress)), title.c_str());
		drawList->AddText(font, titleSize, ImVec2(textX, titleY), titleColor, title.c_str());
		drawList->AddText(descriptionFont, descriptionSize, ImVec2(textX, descriptionY), descriptionColor,
			description.c_str(), nullptr, maxTextWidth);
		visibleIndex++;
	}
}

void Overlay::DrawUI(float width, float height, float deltaTime) {
	animTimer_ = std::min(animTimer_ + deltaTime, kOverlayAnimDuration);
	const float ease = EaseOutCubic(animTimer_ / kOverlayAnimDuration);
	ImDrawList *drawList = ImGui::GetForegroundDrawList();
	const ImVec2 displaySize(width, height);
	const float scale = std::max(1.0f, height / 720.0f);

	DrawBackground(drawList, displaySize, ease);
	DrawMenu(drawList, displaySize, scale, ease);
	DrawHelpers(drawList, displaySize, scale, ease);
}

void Overlay::DrawHud(ImDrawList *drawList, float width, float height) {
	if (visible_) {
		return; // HUD is hidden while the menu is open (matches the 3DS core).
	}
	const bool showFps = GetPspShowFps();
	const bool fastForward = GetPspFastForwardActive();
	if (!showFps && !fastForward) {
		return;
	}
	if (!drawList || width <= 0.0f || height <= 0.0f) {
		return;
	}

	// 3DS-style HUD: rounded translucent box top-left with green text.
	const float em = std::max(14.0f, std::round(height / 32.0f));
	const float margin = std::round(em * 0.6f);
	const float pad = std::round(em * 0.35f);
	const float fontScale = em / 21.0f;

	std::string text;
	if (showFps) {
		char buf[24];
		std::snprintf(buf, sizeof(buf), "FPS: %.1f", GetPspCurrentFps());
		text = buf;
	}
	if (fastForward) {
		if (!text.empty()) {
			text += "   ";
		}
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%dx>>", static_cast<int>(GetPspFastForwardMultiplier()));
		text += buf;
	}

	ImFont *font = ImGui::GetFont();
	if (!font) {
		return;
	}
	const ImVec2 textSize = font->CalcTextSizeA(font->FontSize * fontScale, FLT_MAX, 0.0f, text.c_str());
	ImVec2 min(margin, margin);
	ImVec2 max(margin + textSize.x + pad * 2.0f, margin + textSize.y + pad * 2.0f);
	drawList->AddRectFilled(min, max, IM_COL32(0, 0, 0, 140), std::round(em * 0.25f));
	drawList->AddText(font, font->FontSize * fontScale, ImVec2(min.x + pad, min.y + pad),
		IM_COL32(135, 255, 135, 255), text.c_str());
}

void Overlay::Render(Draw::DrawContext *draw) {
	if (!ready_ || !context_ || !draw) {
		return;
	}

	const bool hasRAAlerts = !RetroAchievements().Notifications().empty();
	const bool showHud = GetPspShowFps() || GetPspFastForwardActive();
	if (!visible_ && !hasRAAlerts && !showHud) {
		return;
	}

	ImGui::SetCurrentContext(context_);
	const float width = (float)std::max(1, g_display.pixel_xres);
	const float height = (float)std::max(1, g_display.pixel_yres);
	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2(width, height);
	io.DeltaTime = 1.0f / std::max(1.0f, g_display.display_hz);

	const float orthoW = g_display.dp_xres > 0 ? (float)g_display.dp_xres : width;
	const float orthoH = g_display.dp_yres > 0 ? (float)g_display.dp_yres : height;
	const float scale = std::max(1.0f, height / 720.0f);
	const float loadedFontSize = io.Fonts->Fonts.Size > 0 ? io.Fonts->Fonts[0]->FontSize : 21.0f;
	if (loadedFontSize > 0.0f) {
		io.FontGlobalScale = (Display::FontSize * scale) / loadedFontSize;
	}

	ImGui_ImplThin3d_NewFrame(draw, ComputeOrthoMatrix(orthoW, orthoH, draw->GetDeviceCaps().coordConvention));
	ImGui::NewFrame();
	DrawHud(ImGui::GetForegroundDrawList(), width, height);
	if (visible_) {
		DrawUI(width, height, io.DeltaTime);
	}
	DrawRAAlerts(draw, ImGui::GetForegroundDrawList(), ImVec2(width, height), scale, io.DeltaTime);
	ImGui::Render();

	const Draw::RenderPassInfo overlayPass{
		Draw::RPAction::KEEP,
		Draw::RPAction::KEEP,
		Draw::RPAction::KEEP,
		0x00000000,
		1.0f,
		0,
		"GBAStationOverlay",
	};
	draw->BindFramebufferAsRenderTarget(nullptr, overlayPass, "GBAStationOverlay");
	ImGui_ImplThin3d_RenderDrawData(ImGui::GetDrawData(), draw);
}

}  // namespace GBAStation
