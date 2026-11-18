# Shared helpers for the Linux build scripts, the counterpart of common.ps1. Source it:
# . "$(dirname "$0")/common.sh"

repo_root=$(cd "$(dirname "$0")/.." && pwd)

fail()
{
    echo "error: $1" >&2
    exit 1
}

# The build uses the system's clang with Ninja; a missing tool is reported by name before CMake runs.
require_build_tools()
{
    for tool in clang clang++ cmake ninja; do
        command -v "$tool" >/dev/null 2>&1 || fail "$tool not found"
    done
}

# cmake_build <source> <build> [target...]: configures (once) and builds a CMake project with clang
# and Ninja.
cmake_build()
{
    source_dir=$1
    build_dir=$2
    shift 2
    if [ ! -f "$build_dir/CMakeCache.txt" ]; then
        cmake -S "$source_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ||
            fail "CMake configuration of $source_dir failed"
    fi
    if [ $# -gt 0 ]; then
        cmake --build "$build_dir" --target "$@" || fail "build of $source_dir failed"
    else
        cmake --build "$build_dir" || fail "build of $source_dir failed"
    fi
}
