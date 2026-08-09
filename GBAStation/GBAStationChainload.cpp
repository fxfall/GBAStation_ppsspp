#include "GBAStationChainload.h"

#include "ppsspp_config.h"

#include "GBAStation/GBAStationConfig.h"

#if PPSSPP_PLATFORM(SWITCH)
#include <switch.h>
#endif

#include <cstdio>
#include <string>
#include <sys/stat.h>

namespace GBAStation {
namespace {
std::string g_returnPath;
std::string g_sessionToken;

bool FileExists(const char *path) {
	struct stat st {};
	return path && stat(path, &st) == 0;
}

const char *FindLauncherNro() {
	if (!g_returnPath.empty() && FileExists(g_returnPath.c_str())) {
		return g_returnPath.c_str();
	}
	const char *candidates[] = {
		Paths::LauncherNro,
		Paths::LauncherNroFallback,
	};
	for (const char *candidate : candidates) {
		if (FileExists(candidate)) {
			return candidate;
		}
	}
	return nullptr;
}

}  // namespace

void SetLauncherReturnPath(const char *path) {
	if (path && path[0]) {
		g_returnPath = path;
	}
}

bool HasLauncherReturnPath() {
	return !g_returnPath.empty();
}


void SetExternalSessionToken(const char *token) {
	if (token && token[0]) {
		g_sessionToken = token;
	}
}

bool ChainloadLauncher(LogCallback log) {
#if PPSSPP_PLATFORM(SWITCH)
	const char *nextNro = FindLauncherNro();
	if (!nextNro) {
		LogMessage(log, "GBAStation chainload skipped missing targets=%s,%s", Paths::LauncherNro, Paths::LauncherNroFallback);
		return false;
	}

	char args[512];
	if (!g_sessionToken.empty()) {
		std::snprintf(args, sizeof(args), "%s --external-return %s", nextNro, g_sessionToken.c_str());
	} else {
		std::snprintf(args, sizeof(args), "%s --resume", nextNro);
	}
	envSetNextLoad(nextNro, args);
	LogMessage(log, "GBAStation chainload next=%s args=%s", nextNro, args);
	return true;
#else
	LogMessage(log, "GBAStation chainload skipped unsupported platform");
	return false;
#endif
}

}  // namespace GBAStation
