// RetroArch slang (.slangp) shader settings screen.
// Interaction model follows the nds_stub shader settings UI.

#include "UI/SlangShaderScreen.h"

#include <algorithm>
#include <functional>

#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/StringUtils.h"
#include "Common/System/System.h"
#include "Common/UI/Context.h"
#include "Common/UI/ScrollView.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "GBAStation/PpssppRuntime.h"
#include "GPU/Common/PostShader.h"
#include "GPU/Common/Slang/slang_types.h"

SlangShaderScreen::SlangShaderScreen(std::string_view title) : title_(title) {
	shaderRoots_.push_back(Path("shaders"));
	shaderRoots_.push_back(GetSysDirectory(DIRECTORY_CUSTOM_SHADERS));
}

SlangShaderScreen::~SlangShaderScreen() {
}

void SlangShaderScreen::RefreshEntries() {
	entries_.clear();
	selectedEntry_ = -1;

	// Build the absolute path for the current relative dir.
	Path abs;
	for (size_t r = 0; r < shaderRoots_.size(); r++) {
		Path candidate = shaderRoots_[r];
		for (auto &d : currentDir_)
			candidate = candidate / d.ToString();
		if (File::IsDirectory(candidate)) {
			abs = candidate;
			break;
		}
	}

	// ".." entry when in a subdirectory.
	if (!currentDir_.empty()) {
		entries_.push_back({ "..", true });
		selectedEntry_ = 0;
	}

	if (!abs.empty()) {
		std::vector<File::FileInfo> files;
		File::GetFilesInDir(abs, &files, "slangp:");
		for (auto &f : files) {
			if (f.isDirectory) {
				entries_.push_back({ f.name + "/", true });
			} else if (Path(f.name).GetFileExtension() == ".slangp") {
				std::string title = f.name;
				size_t dot = title.find_last_of('.');
				if (dot != std::string::npos)
					title = title.substr(0, dot);
				entries_.push_back({ title, false });
			}
		}
	}

	// Highlight the currently selected preset if visible.
	int i = 0;
	for (auto &e : entries_) {
		if (!e.isDirectory && e.label == selectedSection_)
			selectedEntry_ = i;
		i++;
	}
}

void SlangShaderScreen::CreateViews() {
	using namespace UI;

	RefreshEntries();

	auto gr = GetI18NCategory(I18NCat::GRAPHICS);
	auto ps = GetI18NCategory(I18NCat::POSTSHADERS);

	root_ = new AnchorLayout(new LayoutParams(FILL_PARENT, FILL_PARENT));

	// Breadcrumb path display.
	std::string pathStr = "slang";
	for (auto &d : currentDir_)
		pathStr += " / " + d.ToString();
	root_->Add(new TextView(pathStr, new AnchorLayoutParams(0.0f, 5.0f, -1.0f, -1.0f, 5.0f, -1.0f)));

	// Left: shader list (directory browser).
	LinearLayout *left = new LinearLayout(ORIENT_VERTICAL, new AnchorLayoutParams(FILL_PARENT, FILL_PARENT, 0.0f, 40.0f, 500.0f, 0.0f));
	root_->Add(left);

	UI::ListView *listView = nullptr;
	if (entries_.empty()) {
		left->Add(new TextView(ps->T("No slang shaders found"), new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
	} else {
		std::vector<std::string> items;
		for (auto &e : entries_)
			items.push_back(e.label);
		auto adaptor = new StringVectorListAdaptor(items, std::max(selectedEntry_, 0));
		listView = new ListView(adaptor, {}, {}, new LinearLayoutParams(FILL_PARENT, FILL_PARENT));
		left->Add(listView);
		listView->OnChoice.Add([this](EventParams &e) {
			int idx = e.a;
			if (idx >= 0 && idx < (int)entries_.size()) {
				if (entries_[idx].isDirectory) {
					EnterDirectory(idx);
				} else {
					SelectPreset(idx);
				}
			}
		});
	}

	// Right: sidebar with enable toggle and parameters of the selected preset.
	LinearLayout *side = new LinearLayout(ORIENT_VERTICAL, new AnchorLayoutParams(500.0f, FILL_PARENT, 0.0f, 40.0f, 0.0f, 0.0f));
	side->SetSpacing(6.0f);
	root_->Add(side);

	// Check whether the current chain contains the selected preset.
	enabled_ = false;
	for (auto &name : g_Config.vPostShaderNames) {
		const ShaderInfo *info = GetPostShaderInfo(name);
		if (info && info->isSlang && info->section == selectedSection_)
			enabled_ = true;
	}

	if (selectedSection_.empty()) {
		side->Add(new TextView(ps->T("Select a shader on the left"), new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
	} else {
		side->Add(new ItemHeader(ps->T("Shader settings")));

		// Enable toggle (nds_stub "shader switch" row).
		Choice *toggle = new Choice(ps->T("Enable"), new LinearLayoutParams(FILL_PARENT, 46.0f));
		toggle->SetSelectedIndicator(!selectedSection_.empty() && enabled_);
		toggle->OnClick.Handle(this, &SlangShaderScreen::OnToggleChanged);
		side->Add(toggle);

		const ShaderInfo *info = GetPostShaderInfo(selectedSection_);
		if (info && info->isSlang && info->slangPreset) {
			const SlangPreset *preset = info->slangPreset.get();
			paramValues_.clear();
			paramSliders_.clear();
			for (int i = 0; i < (int)preset->parameters.size(); i++) {
				const SlangParameter &param = preset->parameters[i];

				std::string key = StringFromFormat("%sSettingCurrentValue%d", selectedSection_.c_str(), i + 1);
				auto it = g_Config.mPostShaderSetting.find(key);
				float value = it != g_Config.mPostShaderSetting.end() ? it->second : param.current;

				paramValues_.push_back(value);
				// Sanity clamp.
				paramValues_.back() = std::clamp(paramValues_.back(), param.minimum, param.maximum);

				std::string label = param.desc[0] ? param.desc : param.id;
				auto slider = new PopupSliderChoiceFloat(&paramValues_.back(), param.minimum, param.maximum, param.initial, label, param.step, screenManager());
				slider->SetLiveUpdate(true);
				slider->SetHasDropShadow(false);
				slider->OnChange.Handle(this, &SlangShaderScreen::OnParamChanged);
				slider->SetEnabledFunc([&] {
					return enabled_ && !g_Config.bSkipBufferEffects;
				});
				side->Add(slider);
				paramSliders_.push_back(slider);
			}
			if (preset->parameters.empty()) {
				side->Add(new TextView(ps->T("This shader has no adjustable parameters"), new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
			}
		}

		side->Add(new TextView(ps->T("Select with A, adjust with L/R"), new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
	}
}

void SlangShaderScreen::EnterDirectory(int index) {
	if (index >= 0 && index < (int)entries_.size()) {
		if (entries_[index].label == "..") {
			if (!currentDir_.empty())
				currentDir_.pop_back();
		} else if (entries_[index].isDirectory) {
			currentDir_.push_back(Path(entries_[index].label.substr(0, entries_[index].label.size() - 1)));
		}
	}
	RecreateViews();
}

void SlangShaderScreen::SelectPreset(int index) {
	if (index < 0 || index >= (int)entries_.size())
		return;
	selectedSection_ = entries_[index].label;
	enabled_ = true;
	ApplyCurrentSelection();
	RecreateViews();
}

void SlangShaderScreen::ApplyCurrentSelection() {
	g_Config.vPostShaderNames.clear();
	if (enabled_ && !selectedSection_.empty())
		g_Config.vPostShaderNames.push_back(selectedSection_);
	FixPostShaderOrder(&g_Config.vPostShaderNames);
	GBAStation::PersistPspGameDbShaderSettings();
	System_PostUIMessage(UIMessage::GPU_CONFIG_CHANGED);
}

void SlangShaderScreen::OnToggleChanged(UI::EventParams &e) {
	enabled_ = !enabled_;
	if (!enabled_)
		selectedSection_.clear();
	ApplyCurrentSelection();
	RecreateViews();
}

void SlangShaderScreen::OnParamChanged(UI::EventParams &e) {
	if (selectedSection_.empty())
		return;
	const ShaderInfo *info = GetPostShaderInfo(selectedSection_);
	if (!info || !info->isSlang || !info->slangPreset)
		return;

	// Find which slider changed and store its value in the config.
	int idx = -1;
	for (size_t i = 0; i < paramSliders_.size(); i++) {
		if (paramSliders_[i] == (UI::View *)e.v) {
			idx = (int)i;
			break;
		}
	}
	if (idx < 0 || idx >= (int)paramValues_.size())
		return;

	std::string key = StringFromFormat("%sSettingCurrentValue%d", selectedSection_.c_str(), idx + 1);
	g_Config.mPostShaderSetting[key] = paramValues_[idx];
	GBAStation::PersistPspGameDbShaderSettings();
	System_PostUIMessage(UIMessage::GPU_CONFIG_CHANGED);
}

bool SlangShaderScreen::key(const KeyInput &key) {
	if (key.deviceId == DEVICE_ID_MOUSE)
		return false;

	// B: go back to parent directory (nds_stub behavior), then close.
	if (key.flags & KeyInputFlags::DOWN) {
		if (key.keyCode == NKCODE_BACK || key.keyCode == NKCODE_ESCAPE) {
			if (!currentDir_.empty()) {
				currentDir_.pop_back();
				RecreateViews();
				return true;
			}
		}
	}
	return UIScreen::key(key);
}
