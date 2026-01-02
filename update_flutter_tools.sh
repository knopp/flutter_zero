#!/bin/sh

set -e

REPO_PATH=$(dirname "$(readlink -f "$0")")

mkdir -p "$REPO_PATH/_tmp";
cd "$REPO_PATH/_tmp";

git clone --no-checkout https://github.com/flutter/flutter.git --depth=1
cd flutter
git sparse-checkout init --cone
git sparse-checkout set packages/flutter_tools
git checkout master
# get the checkout out revision of _tmp/flutter
FLUTTER_TOOLS_REVISON=$(git rev-parse HEAD)
cd ../..

rm -rf packages/flutter_tools
mv _tmp/flutter/packages/flutter_tools packages/flutter_tools
rm -rf _tmp

git apply packages/flutter_tools_patches/*.patch
rm -rf packages/flutter_tools/test/widget_preview_scaffold.shard
git add packages/flutter_tools
git commit -m "Updated flutter_tools to $FLUTTER_TOOLS_REVISON."
