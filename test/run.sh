#!/usr/bin/env bash
# Host tests for 摆件's rules and pictures (no device, no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O1 -o "$OUT/test_orn" test_orn.c ../main/orn.c ../main/orn_draw.c -lm
"$OUT/test_orn"
rm -rf "$OUT"
