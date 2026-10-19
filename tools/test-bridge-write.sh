#!/usr/bin/env bash
# Run the same frame writer as the firmware with scripted USB faults and clocks.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
c++ -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined -o /tmp/bridge_write_test tools/bridge_write_test.cpp
exec /tmp/bridge_write_test
