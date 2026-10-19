#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
cc -O2 -Wall -Wextra -fsanitize=address,undefined -o /tmp/dedup_test tools/dedup_test.c
exec /tmp/dedup_test
