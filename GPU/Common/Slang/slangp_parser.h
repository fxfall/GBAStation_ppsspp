// .slangp preset parser, ported from RetroArch's CG-style preset format
// (gfx/video_shader_parse.c) for PPSSPP. GPLv3 licensed, see RetroArch project.

#pragma once

#include <string>
#include <vector>

#include "slang_types.h"

// Parses a .slangp preset file into a SlangPreset.
// Returns false and fills errors on failure.
bool SlangLoadPreset(const std::string &presetPath, SlangPreset *preset, std::vector<std::string> *errors);
