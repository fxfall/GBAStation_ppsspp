// Copyright (c) 2026 GBAStation contributors.
//
// Read-only PPSSPP FileLoader backed by a ROMX 0.2.0 entrypoint.  The ROMX
// container remains the source file; reads are translated to the entrypoint
// through libromx and are never materialized as a second full-size file.

#pragma once

#include <mutex>
#include <string>

#include "Core/Loaders.h"
#include <romx/romx.h>

class RomxFileLoader final : public FileLoader {
public:
	explicit RomxFileLoader(const Path &filename);
	~RomxFileLoader() override;

	bool Exists() override;
	bool IsDirectory() override;
	s64 FileSize() override;
	Path GetPath() const override;
	size_t ReadAt(s64 absolutePos, size_t bytes, size_t count, void *data,
		Flags flags = Flags::NONE) override;
	std::string LatestError() const override;

private:
	void SetError(const char *fallback, const romx_error_t *error = nullptr);

	romx_reader_t *reader_ = nullptr;
	romx_entry_info_t entry_ = ROMX_ENTRY_INFO_INIT;
	Path virtualPath_;
	s64 fileSize_ = 0;
	mutable std::mutex errorMutex_;
	std::string error_;
};
