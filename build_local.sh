#!/usr/bin/env bash
# Local Nintendo Switch release build wrapper.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
JOBS=${JOBS:-}
CLEAN=0

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
    for candidate in "$SCRIPT_DIR/../switchVK"/nvk-switch-*; do
        [[ -f "$candidate/lib/libvulkan.a" ]] || continue
        SWITCH_NVK_ROOT=$candidate
        break
    done
fi

# Keep the local PPSSPP workflow consistent with the 3DS core: when the
# sibling SDK project is present, build its public-NXVK package automatically
# instead of requiring a private Release download first. CI can still provide
# SWITCH_NVK_ROOT explicitly and will skip this path.
if [[ -z "${SWITCH_NVK_ROOT:-}" && "${SWITCHVK_AUTOBUILD:-1}" != 0 ]]; then
    SWITCHVK_DIR="$SCRIPT_DIR/../switchVK"
    if [[ -x "$SWITCHVK_DIR/build_local.sh" ]]; then
        bash "$SWITCHVK_DIR/build_local.sh" -j "${SWITCHVK_JOBS:-$(nproc)}"
        for candidate in "$SWITCHVK_DIR"/nvk-switch-*; do
            [[ -f "$candidate/lib/libvulkan.a" ]] || continue
            SWITCH_NVK_ROOT=$candidate
            break
        done
    fi
fi

if [[ ! -f "${SWITCH_NVK_ROOT:-}/lib/libvulkan.a" ]] ||
   [[ ! -f "${SWITCH_NVK_ROOT:-}/include/vulkan/vulkan.h" ]]; then
	echo "Missing complete switchVK SDK. Set SWITCH_NVK_ROOT to an SDK containing include/ and lib/libvulkan.a." >&2
	exit 1
fi

export MESA_NVK_DIR="$SWITCH_NVK_ROOT"
export CMAKE_BUILD_PARALLEL_LEVEL=${JOBS:-$(nproc)}
if [[ "$CLEAN" == 1 ]]; then
    exec bash "$SCRIPT_DIR/build_ppsspp_GBAStation_nro.sh" clean
fi
exec bash "$SCRIPT_DIR/build_ppsspp_GBAStation_nro.sh"
