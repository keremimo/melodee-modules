#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copy the module SDK's headers the host is built against from a Melodee firmware checkout.
#   tools/sync_sdk.sh [FIRMWARE_CHECKOUT]      (default ../Felucca)
set -eu
src=${1:-$(dirname "$0")/../../Felucca}
here=$(cd "$(dirname "$0")/.." && pwd)
for f in mod_abi3.h mod_schema.h; do
    cp "$src/firmware/modules/sdk/$f" "$here/sdk/$f"
done
echo "sdk: $(cd "$src" && git log -1 --format=%h -- firmware/modules/sdk)"
