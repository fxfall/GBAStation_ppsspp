#pragma once

#include "GBAStation/GBAStationMain.h"

struct retro_vfs_interface;

namespace GBAStation {

class PpssppRuntime final : public CoreRuntime {
public:
	explicit PpssppRuntime(LogCallback log = {});
	~PpssppRuntime() override;

	const char *Name() const override { return "ppsspp"; }
	// Optional native-frontend file VFS. The PPSSPP core remains unaware of
	// the frontend's content format and only receives standard VFS callbacks.
	void SetFrontendVFS(retro_vfs_interface *vfs);
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
	retro_vfs_interface *frontendVfs_ = nullptr;
};

}  // namespace GBAStation
