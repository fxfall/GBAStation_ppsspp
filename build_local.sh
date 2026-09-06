#!/usr/bin/env bash
# Local Nintendo Switch release build wrapper.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
JOBS=${JOBS:-}
CLEAN=0

SWITCHVK_HELPER="$SCRIPT_DIR/../switchVK/switchvk-version.sh"
if [[ -f "$SWITCHVK_HELPER" ]]; then
    # shellcheck disable=SC1090
    source "$SWITCHVK_HELPER"
fi

usage() {
    cat <<'EOF'
Usage: ./build_local.sh [-j JOBS] [--clean]

Requires a sibling switchVK checkout and an SDK selected by SWITCH_NVK_ROOT
(or the default sibling nvk-switch-* directory). The output is
GBAStationPPSSPPStub.nro in the repository root.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -j|--jobs) JOBS=${2:?missing job count}; shift 2 ;;
        --clean) CLEAN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -n "$JOBS" && ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "Invalid job count: $JOBS" >&2
    exit 2
fi

if [[ -z "${SWITCH_NVK_ROOT:-}" ]]; then
    for sibling in "$SCRIPT_DIR/../switchVK" "$SCRIPT_DIR/../switch-nvk"; do
        [[ -d "$sibling" ]] || continue
        if declare -F switchvk_find_sdk >/dev/null 2>&1; then
            SWITCH_NVK_ROOT=$(switchvk_find_sdk "$sibling" release || true)
        fi
        [[ -n "${SWITCH_NVK_ROOT:-}" ]] && break
    done
fi

if [[ ! -f "${SWITCH_NVK_ROOT:-}/lib/libvulkan.a" ]] ||
   [[ ! -f "${SWITCH_NVK_ROOT:-}/include/vulkan/vulkan.h" ]] ||
   [[ ! -f "${SWITCH_NVK_ROOT:-}/include/vk_video/vulkan_video_codec_h264std.h" ]]; then
	echo "Missing complete switchVK SDK. Set SWITCH_NVK_ROOT to an SDK containing include/ and lib/libvulkan.a." >&2
	exit 1
fi

export MESA_NVK_DIR="$SWITCH_NVK_ROOT"
export CMAKE_BUILD_PARALLEL_LEVEL=${JOBS:-$(switchvk_default_jobs 2>/dev/null || echo 1)}
if [[ "$CLEAN" == 1 ]]; then
    exec bash "$SCRIPT_DIR/build_ppsspp_GBAStation_nro.sh" clean
fi
exec bash "$SCRIPT_DIR/build_ppsspp_GBAStation_nro.sh"
