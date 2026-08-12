// Copyright (c) 2026 PPSSPP Project.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 or later.

#pragma once

#include <mutex>
#include <string>

#include <romx/romx.h>

#include "Common/File/Path.h"
#include "Core/Loaders.h"

// A FileLoader view over the ROM payload inside a complete ROMX container.
// The PSP core sees only the footer-declared payload bytes; metadata, cover,
// and footer bytes never enter PPSSPP's normal loader path.
class RomxFileLoader final : public FileLoader {
public:
	static RomxFileLoader *TryCreate(const Path &filename);

	~RomxFileLoader() override;

	bool Exists() override;
	bool IsDirectory() override;
	s64 FileSize() override;
	Path GetPath() const override { return filename_; }
	std::string GetFileExtension() const override { return payloadExtension_; }
	size_t ReadAt(s64 absolutePos, size_t bytes, size_t count, void *data, Flags flags = Flags::NONE) override;

private:
	explicit RomxFileLoader(const Path &filename);
	bool Open();

	Path filename_;
	std::string payloadExtension_;
	romx_reader_t *reader_ = nullptr;
	u64 filesize_ = 0;
	std::mutex readLock_;
};
