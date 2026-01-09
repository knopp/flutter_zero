#!/bin/sh

set -e

REPO_PATH=$(dirname "$(readlink -f "$0")")/..

cd engine/src

${REPO_PATH}/bin/dart flutter/tools/pkg/github_workflow_generator/bin/run.dart \
    -i flutter/ci/builders/mac_host_engine.json \
    -i flutter/ci/builders/mac_ios_engine_no_ext_safe.json \
    -i flutter/ci/builders/windows_host_engine.json \
    -i flutter/ci/builders/windows_arm_host_engine.json \
    -i flutter/ci/builders/linux_host_engine.json \
    -i flutter/ci/builders/linux_host_desktop_engine.json \
    -i flutter/ci/builders/linux_arm_host_engine.json \
    -i flutter/ci/builders/windows_android_aot_engine.json \
    -i flutter/ci/builders/mac_android_aot_engine.json \
    -i flutter/ci/builders/linux_android_aot_engine.json \
    -i flutter/ci/builders/linux_android_debug_engine.json \
    -i flutter/ci/builders/linux_web_engine_build.json \
    -o "${REPO_PATH}/.github/workflows/build-and-upload-engine-artifacts.yml"
