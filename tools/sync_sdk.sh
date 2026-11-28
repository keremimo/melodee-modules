#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copy the module SDK's headers the host is built against from a checkout of the SDK's repository (melodee-module-sdk;
# Melodee's firmware takes the same repository as its submodule at firmware/modules/sdk).
#   tools/sync_sdk.sh [SDK_CHECKOUT]      (default ../melodee-module-sdk)
set -eu
src=${1:-$(dirname "$0")/../../melodee-module-sdk}
here=$(cd "$(dirname "$0")/.." && pwd)
for f in mod_abi3.h mod_schema.h; do
    cp "$src/$f" "$here/sdk/$f"
done
echo "sdk: $(cd "$src" && git log -1 --format=%h)"
