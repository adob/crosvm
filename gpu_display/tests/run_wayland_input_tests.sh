#!/usr/bin/env bash
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
set -euo pipefail

DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf -- "$TMP"' EXIT

for protocol in aura-shell linux-dmabuf-unstable-v1 xdg-shell viewporter virtio-gpu-metadata-v1; do
    wayland-scanner client-header \
        "$DIR/../protocol/$protocol.xml" "$TMP/$protocol.h"
    wayland-scanner private-code \
        "$DIR/../protocol/$protocol.xml" "$TMP/$protocol.c"
done
read -r -a WAYLAND_FLAGS <<< "$(pkg-config --cflags --libs wayland-client)"
FLAGS=()
if [[ "${SANITIZE:-0}" == 1 ]]; then
    FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections "${FLAGS[@]}" -I"$TMP" \
    "$DIR/display_wl_input_test.c" "$TMP/"*.c -Wl,--gc-sections "${WAYLAND_FLAGS[@]}" -lm \
    -o "$TMP/display_wl_input_test"
"$TMP/display_wl_input_test"
