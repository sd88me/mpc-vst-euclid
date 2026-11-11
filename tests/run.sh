#!/usr/bin/env bash
# Offline x86 tests under ASan/UBSan (no device). Each must print its OK line:
#   CORE OK   tests/core_equiv.cpp: the in-process sequencer (src/core) against the 1.x engine's lane class, then its note bookkeeping
#   OK        tests/host_test.cpp: the SDK front end with the core inside, incl. the end-to-end timing section
#   GRID OK   vst/grid_test.cpp: the song-position clock (vst/transport_grid.h)
set -euo pipefail
cd "$(dirname "$0")/.."
SDK="${MPC_SDK:-$HOME/mpc-310-private/mpc-plugin-builder-sdk}"
python3 tools/gen_sdk_params.py >/dev/null
docker image inspect euclidier-sdk-x64 >/dev/null 2>&1 || docker build -q -t euclidier-sdk-x64 --build-arg BASE=gcc:12 scripts >/dev/null
mkdir -p build/test
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/b -v "$SDK":/sdk:ro -w /b -e ASAN_OPTIONS=detect_leaks=0 \
  euclidier-sdk-x64 bash -euc '
  S="-fsanitize=address,undefined -O1 -g -Wall -Wextra -Wno-unused-parameter"
  g++ $S -w -std=c++17 -Itests/stub -Isrc -Isrc/core -o build/test/core_equiv tests/core_equiv.cpp src/eqseq.cpp src/bjlund.cpp src/core/euclid_core.cpp
  ./build/test/core_equiv
  g++ $S -std=c++17 -DEUCLIDIER_TEST -I/sdk/framework -Isrc/sdk -Isrc -Ivst -c src/sdk/EuclidierPlugin.cpp -o build/test/plugin.o
  g++ $S -std=c++17 -Isrc -c src/core/euclid_core.cpp -o build/test/core.o
  g++ $S -w -std=c++17 -Isrc -c src/bjlund.cpp -o build/test/bjlund.o
  g++ $S -std=c++17 -Isrc/sdk -c tests/host_test.cpp -o build/test/host_test.o
  g++ $S -o build/test/etest build/test/plugin.o build/test/core.o build/test/bjlund.o build/test/host_test.o -lasound -lpthread -lm
  ./build/test/etest
  g++ -O1 -g -fsanitize=address,undefined -std=c++17 -Wall -Wextra -o build/test/grid_test vst/grid_test.cpp
  ./build/test/grid_test' | tail -${TAIL:-80}
