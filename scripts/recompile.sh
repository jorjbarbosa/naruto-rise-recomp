#!/bin/sh
# Builds the patched recompilers and translates the game: game/default.xex into build/ppc,
# game/AI2C.dll into build/ppc_ai2c and the shaders of game/xeshader.sdb into build/shaders. The Linux
# counterpart of recompile.ps1.
#
#   1. Applies patches/ to the submodules (patches/apply.sh).
#   2. Builds XenonAnalyse and XenonRecomp with clang and Ninja into build/tools/XenonRecomp.
#   3. For each module (config/default.toml, config/ai2c.toml), recreates its output directory and
#      runs XenonRecomp. The logs go to build/recompile-<module>.log. Any warning from XenonRecomp
#      makes the script fail, so a config or recompiler regression does not go unnoticed.
#   4. Builds XenosRecomp into build/tools/XenosRecomp and translates every shader of
#      game/xeshader.sdb to SPIR-V (DXIL is made only on Windows), written as
#      build/shaders/shader_cache.cpp. A shader that does not compile makes the script fail (its HLSL
#      is left in build/shaders/*.failed). The log goes to build/recompile-shaders.log.
#
#   ./scripts/recompile.sh [--switch-tables]
#
# --switch-tables also runs XenonAnalyse on each module and writes its jump table output to
# build/<module>_switch_tables.toml, to compare with config/<module>_switch_tables.toml (the
# default.xex one adds the tables XenonAnalyse misses).
set -u

. "$(dirname "$0")/common.sh"

switch_tables=0
case "${1:-}" in
    '') ;;
    --switch-tables) switch_tables=1 ;;
    *) echo "usage: $0 [--switch-tables]" >&2; exit 2 ;;
esac

# Recompiled modules: name, image, XenonRecomp config and output directory (relative to the
# repository root, matching the paths inside the config).
modules='default:game/default.xex:config/default.toml:build/ppc
ai2c:game/AI2C.dll:config/ai2c.toml:build/ppc_ai2c'
submodule="$repo_root/tools/XenonRecomp"
tools_build="$repo_root/build/tools/XenonRecomp"

for module in $modules; do
    image=$(echo "$module" | cut -d: -f2)
    [ -f "$repo_root/$image" ] || fail "$image not found (copy the game files to $repo_root/game)"
done
[ -e "$submodule/.git" ] ||
    fail 'tools/XenonRecomp is not initialized (run: git submodule update --init --recursive)'

# 1. Patches
"$repo_root/patches/apply.sh" || fail 'could not apply the patches'

# 2. Tools
require_build_tools
cmake_build "$submodule" "$tools_build" XenonAnalyse XenonRecomp

for module in $modules; do
    name=$(echo "$module" | cut -d: -f1)
    image=$(echo "$module" | cut -d: -f2)
    config=$(echo "$module" | cut -d: -f3)
    output="$repo_root/$(echo "$module" | cut -d: -f4)"
    log="$repo_root/build/recompile-$name.log"

    if [ "$switch_tables" -eq 1 ]; then
        tables="$repo_root/build/${name}_switch_tables.toml"
        "$tools_build/XenonAnalyse/XenonAnalyse" "$repo_root/$image" "$tables" ||
            fail "XenonAnalyse failed on $image"
        echo "XenonAnalyse output: $tables"
    fi

    # 3. Recompile
    # A leftover file of the previous run would be compiled with the new ones.
    { rm -rf "$output" && mkdir -p "$output"; } || fail "could not recreate $output"

    "$tools_build/XenonRecomp/XenonRecomp" "$repo_root/$config" "$submodule/XenonUtils/ppc_context.h" >"$log"
    status=$?
    [ "$status" -eq 0 ] || fail "XenonRecomp exited with code $status on $image (see $log)"

    echo "$image: generated $(find "$output" -type f | wc -l) files ($(du -sm "$output" | cut -f1) MB) in $output"

    warnings=$(grep -c -v -e '^Recompiling functions' -e '^[[:space:]]*$' "$log")
    if [ "$warnings" -gt 0 ]; then
        grep -v -e '^Recompiling functions' -e '^[[:space:]]*$' "$log" | head -n 20 | sed 's/^/  /'
        fail "XenonRecomp reported $warnings warnings on $image (see $log)"
    fi
    echo 'no warnings'
done

# 4. Shaders
shader_db="$repo_root/game/xeshader.sdb"
xenos_submodule="$repo_root/tools/XenosRecomp"
xenos_build="$repo_root/build/tools/XenosRecomp"
shader_output="$repo_root/build/shaders"
shader_cache="$shader_output/shader_cache.cpp"
shader_log="$repo_root/build/recompile-shaders.log"

[ -f "$shader_db" ] || fail 'game/xeshader.sdb not found'
[ -e "$xenos_submodule/.git" ] ||
    fail 'tools/XenosRecomp is not initialized (run: git submodule update --init --recursive)'

cmake_build "$xenos_submodule" "$xenos_build" XenosRecomp

{ rm -rf "$shader_output" && mkdir -p "$shader_output"; } || fail "could not recreate $shader_output"

"$xenos_build/XenosRecomp/XenosRecomp" "$shader_db" "$shader_cache" "$xenos_submodule/XenosRecomp/shader_common.h" >"$shader_log"
status=$?
grep -v '^Recompiling shaders' "$shader_log"
[ "$status" -eq 0 ] || fail "XenosRecomp exited with code $status (see $shader_log)"
echo "generated $shader_cache ($(du -m "$shader_cache" | cut -f1) MB)"
