#!/bin/bash

set -euo pipefail

export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export DEVKITPPC=$DEVKITPRO/devkitPPC
export DEVKITA64=$DEVKITPRO/devkitA64

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build_switch_GBAStation"
export TMPDIR="${SCRIPT_DIR}/.codex_tmp/msys_tmp"
export TMP="${TMPDIR}"
export TEMP="${TMPDIR}"
mkdir -p "${TMPDIR}"
if [ -z "${MESA_NVK_DIR:-}" ]; then
	for candidate in \
		"${SCRIPT_DIR}/../switchVK/nvk-switch-26.1.4" \
		"${SCRIPT_DIR}/../switchVK/nvk-switch-25.3.6" \
		"/nvk-build"; do
		if [ -d "${candidate}" ]; then
			MESA_NVK_DIR="${candidate}"
			break
		fi
	done
fi
MESA_NVK_DIR="${MESA_NVK_DIR:-/nvk-build}"

if [ -z "${SWITCH_VULKAN_LIBRARY:-}" ]; then
	if [ -f "${MESA_NVK_DIR}/src/nouveau/vulkan/libvulkan.a" ]; then
		SWITCH_VULKAN_LIBRARY="${MESA_NVK_DIR}/src/nouveau/vulkan/libvulkan.a"
	elif [ -f "${MESA_NVK_DIR}/lib/libvulkan.a" ]; then
		SWITCH_VULKAN_LIBRARY="${MESA_NVK_DIR}/lib/libvulkan.a"
	fi
fi

if [ -z "${RCHEEVOS_ROOT:-}" ] && [ ! -f "${SCRIPT_DIR}/rcheevos/src/rapi/rc_api_common.c" ]; then
	if [ -f "${SCRIPT_DIR}/../GBAStation_fbneo/rcheevos/src/rapi/rc_api_common.c" ]; then
		RCHEEVOS_ROOT="${SCRIPT_DIR}/../GBAStation_fbneo/rcheevos"
	fi
fi

echo "=== Building GBAStation PPSSPP Stub NRO ==="
echo "Source: ${SCRIPT_DIR}"
echo "Build:  ${BUILD_DIR}"
if [ -n "${SWITCH_VULKAN_LIBRARY:-}" ]; then
	echo "NVK:    ${SWITCH_VULKAN_LIBRARY}"
else
	echo "NVK:    devkitPro default"
fi
echo ""

FFMPEG_SWITCH_DIR="${SCRIPT_DIR}/ffmpeg/switch_build"
ffmpeg_libs=(
	"${FFMPEG_SWITCH_DIR}/lib/libavcodec.a"
	"${FFMPEG_SWITCH_DIR}/lib/libavformat.a"
	"${FFMPEG_SWITCH_DIR}/lib/libavutil.a"
	"${FFMPEG_SWITCH_DIR}/lib/libswresample.a"
	"${FFMPEG_SWITCH_DIR}/lib/libswscale.a"
)
ffmpeg_missing=0
for lib in "${ffmpeg_libs[@]}"; do
	if [ ! -f "${lib}" ]; then
		ffmpeg_missing=1
		break
	fi
done
if [ "${ffmpeg_missing}" -ne 0 ]; then
	echo "FFmpeg Switch libraries missing; building ${FFMPEG_SWITCH_DIR}..."
	(
		cd "${SCRIPT_DIR}/ffmpeg"
		FFMPEG_SWITCH_PREFIX="${FFMPEG_SWITCH_DIR}" bash ./switch.sh
	)
fi

if [ "${1:-}" = "clean" ]; then
	echo "Cleaning build directory..."
	rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"

if command -v git >/dev/null 2>&1 && [ -d "${SCRIPT_DIR}/.git" ]; then
	export GIT_CONFIG_GLOBAL="${BUILD_DIR}/gitconfig"
	git config --global --add safe.directory "${SCRIPT_DIR}" || true
	AEMU_POSTOFFICE_DIR="${SCRIPT_DIR}/ext/aemu_postoffice"
	AEMU_POSTOFFICE_PATCH="${SCRIPT_DIR}/gitPatches/ext_aemu_postoffice.patch"
	if [ -f "${AEMU_POSTOFFICE_PATCH}" ] && [ -d "${AEMU_POSTOFFICE_DIR}" ]; then
		git config --global --add safe.directory "${AEMU_POSTOFFICE_DIR}" || true
		AEMU_SOCKET_HEADER="${AEMU_POSTOFFICE_DIR}/client/sock_impl.h"
		if grep -Fq 'defined(__SWITCH__)' "${AEMU_SOCKET_HEADER}"; then
			echo "aemu_postoffice Switch socket patch already applied."
		else
			# The submodule content can drift from the patch baseline (CI
			# submodule caches / branch tracking).  Instead of relying on a
			# context-sensitive apply, force the single guard line so the
			# build never depends on the upstream file's exact shape.
			if git -C "${AEMU_POSTOFFICE_DIR}" apply --check "${AEMU_POSTOFFICE_PATCH}" 2>/dev/null; then
				git -C "${AEMU_POSTOFFICE_DIR}" apply "${AEMU_POSTOFFICE_PATCH}"
				echo "Applied aemu_postoffice Switch socket patch."
			else
				sed -i 's/^#if defined(__unix) || defined(__APPLE__) || defined(__PSP__)$/#if defined(__unix) || defined(__APPLE__) || defined(__PSP__) || defined(__SWITCH__)/' \
					"${AEMU_SOCKET_HEADER}"
				if ! grep -Fq 'defined(__SWITCH__)' "${AEMU_SOCKET_HEADER}"; then
					echo "ERROR: could not patch ${AEMU_SOCKET_HEADER} for Switch builds" >&2
					exit 1
				fi
				echo "Applied aemu_postoffice Switch socket patch (forced)."
			fi
		fi
	fi
fi

if [ -z "${PYTHON_EXECUTABLE:-}" ]; then
	for candidate in python3 python /mingw64/bin/python.exe /ucrt64/bin/python.exe; do
		if command -v "${candidate}" >/dev/null 2>&1; then
			PYTHON_EXECUTABLE="$(command -v "${candidate}")"
			break
		fi
	done
fi

CMAKE_GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
	CMAKE_GENERATOR_ARGS=(-G Ninja)
	if [ -f "${BUILD_DIR}/CMakeCache.txt" ] && ! grep -q '^CMAKE_GENERATOR:INTERNAL=Ninja$' "${BUILD_DIR}/CMakeCache.txt"; then
		echo "Existing build directory uses a different CMake generator; recreating it for Ninja."
		rm -rf "${BUILD_DIR}"
	fi
fi

cmake_args=(
	"${CMAKE_GENERATOR_ARGS[@]}"
	-S "${SCRIPT_DIR}"
	-B "${BUILD_DIR}"
	-DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake"
	-DUSE_LIBNX=ON
	-DSWITCH_VULKAN_ONLY=ON
	-DUSE_DISCORD=OFF
	-DUSE_FFMPEG=ON
	-DUSE_SYSTEM_FFMPEG=OFF
	-DFFMPEG_DIR:PATH="${FFMPEG_SWITCH_DIR}"
	-DUSE_MINIUPNPC=OFF
	-DUSE_SYSTEM_FREETYPE=ON
	-DENABLE_PCH=OFF
	-DHEADLESS=ON
	-DLIBRETRO=OFF
	-DUSE_FRONTEND_VFS=ON
	-DCMAKE_BUILD_TYPE=Release
	-U CMAKE_EXE_LINKER_FLAGS_RELEASE
	-DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -g0 -ffunction-sections -fdata-sections -fomit-frame-pointer"
	-DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -g0 -ffunction-sections -fdata-sections -fomit-frame-pointer"
)

if [ -n "${PYTHON_EXECUTABLE:-}" ]; then
	cmake_args+=(-DPYTHON_EXECUTABLE:FILEPATH="${PYTHON_EXECUTABLE}")
fi

if [ -n "${SWITCH_VULKAN_LIBRARY:-}" ]; then
	cmake_args+=(-U SWITCH_VULKAN_LIBRARY -DSWITCH_VULKAN_LIBRARY:FILEPATH="$SWITCH_VULKAN_LIBRARY")
fi

if [ -n "${RCHEEVOS_ROOT:-}" ]; then
	cmake_args+=(-U RCHEEVOS_ROOT -DRCHEEVOS_ROOT:PATH="${RCHEEVOS_ROOT}")
fi

cmake "${cmake_args[@]}"

cmake --build "${BUILD_DIR}" -j"${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}" --target GBAStation-ppsspp_nro

if [ -f "${BUILD_DIR}/GBAStationPPSSPPStub.nro" ]; then
	cp "${BUILD_DIR}/GBAStationPPSSPPStub.nro" "${SCRIPT_DIR}/GBAStationPPSSPPStub.nro"
	echo "======================================"
	echo "Build successful!"
	echo "Output: ${BUILD_DIR}/GBAStationPPSSPPStub.nro"
	echo "Copied to: ${SCRIPT_DIR}/GBAStationPPSSPPStub.nro"
	echo "======================================"
else
	echo "Error: GBAStationPPSSPPStub.nro not found"
	exit 1
fi
