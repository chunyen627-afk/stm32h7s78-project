#!/usr/bin/env bash
# 編出 racedash-bridge.exe（用 w64devkit 的 gcc，單一個 exe、不用安裝執行環境）
set -e
cd "$(dirname "$0")"
CC=${CC:-/c/w64devkit/bin/gcc}
mkdir -p build
SRC="main.c serial_win.c dsh1.c udp.c tray.c cyd.c $(ls src_*.c 2>/dev/null)"
"$CC" -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 -o build/racedash-bridge.exe $SRC -mwindows -lws2_32 -lwinmm -ladvapi32 -lshell32 -liphlpapi
ls -la build/racedash-bridge.exe | awk '{print "  racedash-bridge.exe", $5, "bytes"}'
