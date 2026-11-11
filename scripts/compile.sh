#!/usr/bin/env bash
# Runs INSIDE the build container (see build-sdk.sh): the plugin for this CPU (the SDK front end, the sequencer core, the
# Bjorklund pattern generator).
# env: EUC_N (plugin name), EUC_U (uid as 'E','q','D','r'), EUC_S (slug), OUT (the .so), TAG, FLAGS
set -euo pipefail
CXX="g++ -std=c++17 -O2 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -fno-gnu-unique -ffunction-sections -fdata-sections"
O="build/obj-$TAG"; mkdir -p "$O"
$CXX -w -Isrc -c src/bjlund.cpp -o "$O/bjlund.o"
$CXX -Wall -Wextra -Isrc -c src/core/euclid_core.cpp -o "$O/core.o"
$CXX -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation ${FLAGS:-} -I/sdk/framework -Isrc/sdk -Isrc -Ivst \
  -DEUCLIDIER_NAME="\"$EUC_N\"" -DEUCLIDIER_UID="$EUC_U" -DEUCLIDIER_SLUG="\"$EUC_S\"" -c src/sdk/EuclidierPlugin.cpp -o "$O/plugin.o"
g++ -shared -o "$OUT" "$O/plugin.o" "$O/core.o" "$O/bjlund.o" \
  -Wl,--gc-sections -Wl,--version-script=/sdk/tools/exports.map -Wl,--exclude-libs,ALL -static-libstdc++ -static-libgcc -lasound -lpthread -lm
strip "$OUT"
echo "-- $(basename "$OUT"): highest glibc needed (the 1.x arm32 build needs 2.30 and runs on MPC OS 3.9): $(readelf -V "$OUT" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n 1)"
