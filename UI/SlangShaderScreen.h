#pragma once

#include <string>
#include <vector>

#include "Common/UI/PopupScreens.h"
#include "Common/UI/UIScreen.h"
#include "Common/File/Path.h"

namespace UI {
class ListView;
class LinearLayout;
class TextView;
}

// RetroArch slang (.slangp) shader settings screen.
// Interaction model follows the nds_stub shader settings UI:
//  - left: two-level directory browser (category -> preset)
//  - right: parameter sidebar with toggles and sliders (L/R adjust, A resets)
class SlangShaderScreen : public UIScreen {
public:
	SlangShaderScreen(std::string_view title);
	~SlangShaderScreen();

	void CreateViews() override;
	bool key(const KeyInput &key) override;
	const char *tag() const override { return "SlangShaderScreen"; }

private:
	struct ListEntry {
		std::string label;
		bool isDirectory;
	};

	void RefreshEntries();
	void EnterDirectory(int index);
	void SelectPreset(int index);
	void ApplyCurrentSelection();
	void OnParamChanged(UI::EventParams &e);
	void OnToggleChanged(UI::EventParams &e);

	std::string title_;
	std::vector<Path> shaderRoots_;      // Scan roots (shaders dir, custom shaders dir).
	std::vector<Path> currentDir_;       // Relative path stack from the root.
	std::vector<ListEntry> entries_;     // Current list contents.
	int selectedEntry_ = -1;
	std::string selectedSection_;        // Currently selected preset section.
	bool enabled_ = false;
	std::vector<float> paramValues_;     // Values of the selected preset's parameters.
	std::vector<UI::PopupSliderChoiceFloat *> paramSliders_;
};
