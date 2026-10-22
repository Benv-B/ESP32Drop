#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
mkdir -p .build-host-tests
for suite in iris_tcon iris_image iris_color iris_archive iris_stream_decode stream_ack rx_bench rx_bench_history; do
  # Bash 3.2 (macOS) treats an empty array as unset under set -u.
  suite_includes=(-I ESP32Drop/src)
  if [[ "$suite" == iris_stream_decode ]]; then
    suite_includes+=(-I tools/iris_stream_stubs)
  fi
  "${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${suite_includes[@]}" "tools/${suite}_test.cpp" -o ".build-host-tests/${suite}"
  ".build-host-tests/${suite}"
done
