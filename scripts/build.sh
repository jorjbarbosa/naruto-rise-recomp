#!/bin/sh
# Builds the runtime (build/runtime/roan-xenon) from the generated code in build/ppc, the Linux
# counterpart of build.ps1. Run scripts/recompile.sh first. The first build compiles the whole
# generated code and takes a few minutes; later builds only recompile what changed.
#
#   ./scripts/build.sh
#   ./build/runtime/roan-xenon
set -u

. "$(dirname "$0")/common.sh"

[ -f "$repo_root/build/ppc/ppc_func_mapping.cpp" ] || fail 'build/ppc is empty; run scripts/recompile.sh first'

require_build_tools
cmake_build "$repo_root" "$repo_root/build/runtime" roan-xenon
echo "built $repo_root/build/runtime/roan-xenon"
