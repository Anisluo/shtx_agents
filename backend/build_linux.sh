#!/usr/bin/env bash
# 在 Linux 上直接用 g++ 编译后端（无需 cmake）。
# 依赖：g++ (C++17)、libssl-dev、libsqlite3-dev
set -e
cd "$(dirname "$0")"
OUT=build-linux
mkdir -p "$OUT"
echo "编译 shtx_backend ..."
g++ -std=c++17 -O2 -pthread \
    -DCPPHTTPLIB_OPENSSL_SUPPORT \
    -I third_party -I src \
    src/main.cpp src/db.cpp src/auth.cpp src/glm_client.cpp src/product_repo.cpp src/agent.cpp \
    -lssl -lcrypto -lsqlite3 \
    -o "$OUT/shtx_backend"
echo "完成: $OUT/shtx_backend"
