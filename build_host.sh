#!/bin/bash
# Build remote_host.exe bằng MSYS2 UCRT64 (g++ + windres), không cần thư viện ngoài.
set -euo pipefail
cd "$(dirname "$0")"
export PATH=/c/msys64/ucrt64/bin:$PATH
windres remote_host.rc -O coff -o remote_host.res.o
g++ -O2 -std=c++20 -Wall -Wno-missing-field-initializers -mwindows remote_host.cpp remote_host.res.o -o remote_host.exe -static \
  -ld3d11 -ldxgi -lmfplat -lmfuuid -lole32 -loleaut32 -lws2_32 -lsecur32 -lcrypt32 -lncrypt -lbcrypt -lwinmm -luuid \
  -lstrmiids -ldnsapi -lcomctl32 -lgdiplus -ldwmapi -lwinhttp
rm remote_host.res.o
echo "Xong: remote_host.exe"
