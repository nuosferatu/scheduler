#!/usr/bin/env bash
# 库开发者使用: 一键编译 examples/scheduler_demo,
# 顺便把 compile_commands.json 软链到仓根供 clangd 使用。
#
# 下游项目集成 scheduler 时不需要这个脚本——它只服务于本仓的开发与调试。
# 重复执行是幂等的: 已存在的 build 目录会被 cmake 复用; 软链总是重建。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXAMPLE_DIR="${SCRIPT_DIR}/examples"
BUILD_DIR="${EXAMPLE_DIR}/build"
JOBS="$(nproc 2>/dev/null || echo 4)"

echo ">>> 配置 examples"
cmake -S "${EXAMPLE_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

echo ">>> 编译"
cmake --build "${BUILD_DIR}" -j "${JOBS}"

# clangd 从 include/*.h 向上查找时第一站就是仓根，把数据库软链过去
LINK_TARGET="${SCRIPT_DIR}/compile_commands.json"
rm -f "${LINK_TARGET}"
ln -s "${BUILD_DIR}/compile_commands.json" "${LINK_TARGET}"

echo ">>> 完成"
echo "    可执行文件: ${BUILD_DIR}/scheduler_demo"
echo "    clangd db : ${LINK_TARGET} -> ${BUILD_DIR}/compile_commands.json"
