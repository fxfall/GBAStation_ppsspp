// Copyright (c) 2026 PPSSPP Project.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 or later.

#include "Core/FileLoaders/RomxFileLoader.h"

#include <algorithm>
#include <limits>

#include "Common/Log.h"

namespace {

std::string PayloadExtension(const Path &filename) {
	std::string extension = filename.GetFileExtension();
	// GBAStation's PSP containers use the conventional payload extension with
	// an additional trailing 'x', for example .isox and .pbpx. Keep the
	// original path while presenting the payload type to PPSSPP's detection.
	if (extension.size() > 2 && extension.back() == 'x') {
		extension.pop_back();
	}
	return extension;
}

}  // namespace

RomxFileLoader::RomxFileLoader(const Path &filename)
	: filename_(filename), payloadExtension_(PayloadExtension(filename)) {
	Open();
}

RomxFileLoader::~RomxFileLoader() {
	if (reader_ != nullptr) {
		romx_reader_close(reader_);
	}
}

RomxFileLoader *RomxFileLoader::TryCreate(const Path &filename) {
	RomxFileLoader *loader = new RomxFileLoader(filename);
	if (!loader->Exists()) {
		delete loader;
		return nullptr;
	}
	return loader;
}

bool RomxFileLoader::Open() {
	if (filename_.empty()) {
		return false;
	}

	romx_error_t error{};
	if (romx_reader_open_path(filename_.c_str(), nullptr, &reader_, &error) != ROMX_OK) {
		reader_ = nullptr;
		return false;
	}

	romx_info_t info = ROMX_INFO_INIT;
	if (romx_reader_get_info(reader_, &info, &error) != ROMX_OK ||
		info.rom.size > static_cast<u64>(std::numeric_limits<s64>::max())) {
		romx_reader_close(reader_);
		reader_ = nullptr;
		return false;
	}

	// Validate only the container and bounded optional regions here. A body
	// SHA-256, when present, covers the complete payload and would force a
	// full large-ISO scan before PPSSPP can start reading it. The frontend and
	// packer may perform that integrity check separately; the direct loader
	// must keep payload access streaming.
	romx_validation_report_t report = ROMX_VALIDATION_REPORT_INIT;
	const romx_validate_flags_t validationFlags =
		ROMX_VALIDATE_METADATA | ROMX_VALIDATE_COVER;
	if (romx_reader_validate(reader_, validationFlags, &report, &error) != ROMX_OK) {
		romx_reader_close(reader_);
		reader_ = nullptr;
		return false;
	}

	filesize_ = static_cast<u64>(info.rom.size);
	INFO_LOG(Log::Loader, "Opened ROMX payload: %s (%llu bytes)", filename_.c_str(),
		static_cast<unsigned long long>(filesize_));
	return true;
}

bool RomxFileLoader::Exists() {
	return reader_ != nullptr;
}

bool RomxFileLoader::IsDirectory() {
	return false;
}

s64 RomxFileLoader::FileSize() {
	return static_cast<s64>(filesize_);
}

size_t RomxFileLoader::ReadAt(s64 absolutePos, size_t bytes, size_t count, void *data, Flags) {
	if (reader_ == nullptr || absolutePos < 0 || bytes == 0 || count == 0 ||
		static_cast<u64>(absolutePos) >= filesize_ || data == nullptr) {
		return 0;
	}
	if (count > std::numeric_limits<size_t>::max() / bytes) {
		return 0;
	}

	const u64 available = filesize_ - static_cast<u64>(absolutePos);
	const u64 requested = static_cast<u64>(bytes) * static_cast<u64>(count);
	const u64 readable = std::min(requested, available);
	const u64 wholeBytes = readable - (readable % static_cast<u64>(bytes));
	if (wholeBytes == 0) {
		return 0;
	}

	std::lock_guard<std::mutex> guard(readLock_);
	// libromx uses the platform uint64_t typedef (unsigned long on
	// devkitA64), while PPSSPP's u64 is always unsigned long long.
	// Keep the out-parameter's exact API type on every target.
	uint64_t bytesRead = 0;
	romx_error_t error{};
	if (romx_reader_read_region(reader_, ROMX_REGION_ROM,
		static_cast<u64>(absolutePos), data, wholeBytes, &bytesRead, &error) != ROMX_OK) {
		return 0;
	}
	return static_cast<size_t>(bytesRead / static_cast<u64>(bytes));
}
