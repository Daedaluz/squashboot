#!/bin/sh
# Update the vendored copy of the Linux kernel's tools/include/nolibc.
#
# Usage: scripts/update-nolibc.sh [git-ref]
#   git-ref defaults to the latest stable (non -rc) vN.M[.P] tag on
#   torvalds/linux. Pass an explicit tag/branch/commit to pin to something
#   else, e.g.: scripts/update-nolibc.sh v6.13
set -e

REPO_URL="https://github.com/torvalds/linux.git"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
DEST="$REPO_ROOT/third_party/nolibc"
REF="$1"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [ -z "$REF" ]; then
    echo "No ref given, resolving latest stable tag..."
    REF=$(git ls-remote --tags "$REPO_URL" 'v[0-9]*' \
        | grep -v '\^{}' \
        | sed 's#.*refs/tags/##' \
        | grep -Ev -- '-rc[0-9]+$' \
        | sort -V \
        | tail -1)
    if [ -z "$REF" ]; then
        echo "Could not resolve a latest tag; pass one explicitly." >&2
        exit 1
    fi
    echo "Using $REF"
fi

git clone --filter=blob:none --no-checkout --depth=1 --branch "$REF" "$REPO_URL" "$WORK/linux"
cd "$WORK/linux"
git sparse-checkout init --cone
git sparse-checkout set tools/include/nolibc
git checkout "$REF"
COMMIT=$(git rev-parse HEAD)
COMMIT_DATE=$(git log -1 --format=%cI)

rm -rf "$DEST"
cp -r "$WORK/linux/tools/include/nolibc" "$DEST"
rm -f "$DEST/.gitignore" "$DEST/Makefile"

cat > "$DEST/VERSION" <<EOF
Vendored from $REPO_URL tools/include/nolibc
Pinned ref: $REF
Pinned commit: $COMMIT ($COMMIT_DATE)
License: LGPL-2.1 OR MIT (see individual file SPDX headers)

To update: scripts/update-nolibc.sh [git-ref]
EOF

echo "Updated $DEST to $REF ($COMMIT)"
echo "Review the diff, then rebuild and re-test before committing."
