#!/usr/bin/env bash
# 用法:
#   ./build.sh              # Release 构建
#   ./build.sh debug        # Debug 构建
#   ./build.sh clean        # 清理 build 目录
#   ./build.sh install      # 构建并安装到 ./install
#   BUILD_TYPE=Debug ./build.sh
#   JOBS=4 ./build.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

BUILD_DIR="${BUILD_DIR:-build}"
INSTALL_DIR="${INSTALL_DIR:-${SCRIPT_DIR}/install}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

ACTION="${1:-build}"

case "${ACTION}" in
    clean)
        echo ">>> 清理 ${BUILD_DIR} 和 ${INSTALL_DIR}"
        rm -rf "${BUILD_DIR}" "${INSTALL_DIR}"
        exit 0
        ;;
    debug)
        BUILD_TYPE=Debug
        ACTION=build
        ;;
    release)
        BUILD_TYPE=Release
        ACTION=build
        ;;
esac

echo ">>> 配置 (BUILD_TYPE=${BUILD_TYPE}, JOBS=${JOBS})"
cmake -S . -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"

echo ">>> 编译"
cmake --build "${BUILD_DIR}" -j "${JOBS}"

if [[ "${ACTION}" == "install" ]]; then
    echo ">>> 安装到 ${INSTALL_DIR}"
    cmake --install "${BUILD_DIR}"
fi

echo ">>> 完成，产物位于 ${BUILD_DIR}/"
