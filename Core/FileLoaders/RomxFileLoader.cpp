// Copyright (c) 2026 GBAStation contributors.

#include "ppsspp_config.h"

#include "Core/FileLoaders/RomxFileLoader.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <romx/romx.h>

namespace {

const char *FallbackExtension(uint16_t formatId) {
	switch (formatId) {
	case ROMX_FORMAT_ISO:
		return ".iso";
	case ROMX_FORMAT_CSO:
		return ".cso";
	case ROMX_FORMAT_ZSO:
		return ".zso";
	case ROMX_FORMAT_CHD:
		return ".chd";
	case ROMX_FORMAT_PBP:
		return ".pbp";
	default:
		return ".bin";
	}
}

std::string EntryPath(const romx_entry_info_t &entry) {
	const size_t pathSize = std::min<size_t>(entry.path_size, ROMX_RIDX_PATH_CAPACITY);
	if (pathSize != 0) {
		return std::string(entry.path, pathSize);
	}
	return std::string("entrypoint") + FallbackExtension(entry.format_id);
}

}  // namespace

RomxFileLoader::RomxFileLoader(const Path &filename) : virtualPath_(filename) {
	romx_error_t error{};
	romx_result_t result = romx_reader_open_path(filename.c_str(), nullptr, &reader_, &error);
	if (result != ROMX_OK) {
		SetError("failed to open ROMX container", &error);
		return;
	}

	entry_ = ROMX_ENTRY_INFO_INIT;
	result = romx_reader_get_entrypoint(reader_, &entry_, &error);
	if (result != ROMX_OK || entry_.data_size > static_cast<uint64_t>(std::numeric_limits<s64>::max())) {
		SetError("ROMX entrypoint is invalid", &error);
		romx_reader_close(reader_);
		reader_ = nullptr;
		return;
	}

	fileSize_ = static_cast<s64>(entry_.data_size);
	virtualPath_ = Path(EntryPath(entry_));
}

RomxFileLoader::~RomxFileLoader() {
	if (reader_ != nullptr) {
		romx_reader_close(reader_);
		reader_ = nullptr;
	}
}

bool RomxFileLoader::Exists() {
	return reader_ != nullptr;
}

bool RomxFileLoader::IsDirectory() {
	return false;
}

s64 RomxFileLoader::FileSize() {
	return fileSize_;
}

Path RomxFileLoader::GetPath() const {
	// Return the entrypoint's virtual filename so PPSSPP's existing loader
	// selection sees .iso/.cso/.pbp instead of the outer .romx suffix.
	return virtualPath_;
}

size_t RomxFileLoader::ReadAt(s64 absolutePos, size_t bytes, size_t count, void *data, Flags) {
	if (reader_ == nullptr || absolutePos < 0 || bytes == 0 || count == 0 || data == nullptr) {
		return 0;
	}
	if (count > std::numeric_limits<size_t>::max() / bytes) {
		SetError("ROMX read size overflows");
		return 0;
	}

	const size_t requested = bytes * count;
	if (static_cast<uint64_t>(absolutePos) >= entry_.data_size) {
		return 0;
	}
	const uint64_t available = entry_.data_size - static_cast<uint64_t>(absolutePos);
	const uint64_t bounded = std::min<uint64_t>(available, requested);
	romx_error_t error{};
	uint64_t actual = 0;
	const romx_result_t result = romx_reader_read_entry(
		reader_, entry_.index, static_cast<uint64_t>(absolutePos), data, bounded, &actual, &error);
	if (result != ROMX_OK) {
		SetError("failed to read ROMX entrypoint", &error);
		return 0;
	}
	return static_cast<size_t>(actual / bytes);
}

std::string RomxFileLoader::LatestError() const {
	std::lock_guard<std::mutex> lock(errorMutex_);
	return error_;
}

void RomxFileLoader::SetError(const char *fallback, const romx_error_t *error) {
	std::string message = fallback != nullptr ? fallback : "ROMX error";
	if (error != nullptr && error->message[0] != '\0') {
		message += ": ";
		message += error->message;
	}
	std::lock_guard<std::mutex> lock(errorMutex_);
	error_ = std::move(message);
}
