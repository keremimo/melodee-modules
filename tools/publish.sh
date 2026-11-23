#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
# Publish this repository's tree as a commit has it (REV, default HEAD) into a checkout of its public repository (DEST,
# default melodee-modules-public beside the main checkout): one commit there, REV's subject unless -m gives another.
# The public repository has a history of its own, and no synth module (.mmod) goes there. Pushing stays yours.
#   tools/publish.sh [-m SUBJECT] [DEST] [REV]
set -eu
msg=
if [ "${1:-}" = "-m" ]; then msg=$2; shift 2; fi
here=$(cd "$(dirname "$0")/.." && pwd)
main=$(dirname "$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)")
dest=${1:-"$(dirname "$main")/melodee-modules-public"}
rev=${2:-HEAD}
sha=$(git -C "$here" rev-parse --short=8 "$rev^{commit}")
if git -C "$here" ls-tree -r --name-only "$sha" | grep -qi '\.mmod$'; then
    echo "publish: $sha holds a synth module (.mmod)" >&2; exit 1
fi
[ "$(git -C "$dest" rev-parse --show-toplevel 2>/dev/null)" = "$(cd "$dest" 2>/dev/null && pwd -P)" ] ||
    { echo "publish: $dest is not a git checkout's top (git init or clone it first)" >&2; exit 1; }
[ -z "$(git -C "$dest" status --porcelain)" ] || { echo "publish: $dest has changes of its own" >&2; exit 1; }
git -C "$dest" rm -rq --ignore-unmatch -- .
git -C "$here" archive "$sha" | tar -x -C "$dest"
git -C "$dest" add -A
if git -C "$dest" diff --cached --quiet; then
    echo "publish: $dest already has $sha"
else
    git -C "$dest" commit -q -m "${msg:-$(git -C "$here" log -1 --format=%s "$sha")}"
    echo "publish: $dest $(git -C "$dest" log -1 --format='%h %s' | cut -c1-100)"
fi
[ "$(git -C "$dest" rev-parse 'HEAD^{tree}')" = "$(git -C "$here" rev-parse "$sha^{tree}")" ] ||
    { echo "publish: $dest's tree is not $sha's (an ignored file the commit tracks?)" >&2; exit 1; }
