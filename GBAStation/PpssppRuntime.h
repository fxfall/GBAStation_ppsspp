#pragma once

#include <string>

#include "GBAStation/GBAStationMain.h"

namespace GBAStation {

// The Slang settings screen lives in PPSSPP's UI module while the launcher
// GameDB is owned by this runtime.  Keep the bridge intentionally narrow so
// a preset/parameter change is persisted per game instead of leaking into the
// user's global PPSSPP configuration.
void PersistPspGameDbShaderSettings();
void NotifyPspGpuConfigChanged();
std::string GetPspCoreConfigValue(const char *option, const char *fallback);
void SetPspCoreConfigValue(const char *option, const std::string &value);

class PpssppRuntime final : public CoreRuntime {
public:
	explicit PpssppRuntime(LogCallback log = {});
	~PpssppRuntime() override;

	const char *Name() const override { return "ppsspp"; }
	bool Configure(const LaunchInfo &launch) override;
	bool Initialize(const LaunchInfo &launch) override;
	bool LoadContent(const std::string &path) override;
	void HandleInput(const FrameInput &input) override;
	void RunFrame() override;
	void RenderFrame() override;
	bool ShouldExit() const override;
	bool ShouldChainloadLauncher() const override;
	void RequestExit() override;
	void Shutdown() override;

private:
	LogCallback log_;
};

}  // namespace GBAStation
