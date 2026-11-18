#!/bin/sh
# Applies (or reverts) the patches in patches/<submodule> to the submodules. The Linux counterpart of
# apply.ps1, with the same behavior.
#
# Each folder inside patches/ is named after a submodule (the last part of its path in .gitmodules,
# such as tools/XenonRecomp or thirdparty/plume) and holds .patch files generated with "git diff".
# They are applied in alphabetical order. The script is idempotent: patches that are already applied
# are skipped.
#
#   ./patches/apply.sh             apply everything
#   ./patches/apply.sh --revert    revert everything (for example, before updating a submodule)
set -u
LC_ALL=C
export LC_ALL

patches_dir=$(cd "$(dirname "$0")" && pwd)
repo_root=$(dirname "$patches_dir")

revert=0
case "${1:-}" in
    '') ;;
    --revert) revert=1 ;;
    *) echo "usage: $0 [--revert]" >&2; exit 2 ;;
esac

warn()
{
    echo "warning: $1" >&2
    failed=1
}

failed=0
for dir in "$patches_dir"/*/; do
    name=$(basename "$dir")

    # The submodule whose path ends with the folder's name, from .gitmodules.
    path=$(git config --file "$repo_root/.gitmodules" --get-regexp '^submodule\..*\.path$' |
        awk -v name="$name" '{ count = split($2, parts, "/"); if (parts[count] == name) print $2 }')
    if [ -z "$path" ]; then
        warn "$name: no submodule with that name in .gitmodules"
        continue
    fi
    submodule="$repo_root/$path"
    if [ ! -e "$submodule/.git" ]; then
        warn "$name: submodule not initialized at $submodule (run: git submodule update --init --recursive)"
        continue
    fi

    set -- "$dir"*.patch
    [ -e "$1" ] || continue

    # Patches are applied in order, so the last one that reverses cleanly marks every patch before
    # it as applied too. An earlier patch checked on its own can fail to reverse when a later one
    # changed the lines around it.
    applied=0
    index=$#
    while [ "$index" -ge 1 ]; do
        eval "patch=\${$index}"
        if git -C "$submodule" apply --check --reverse "$patch" 2>/dev/null; then
            applied=$index
            break
        fi
        index=$((index - 1))
    done

    if [ "$revert" -eq 1 ]; then
        index=0
        for patch in "$@"; do
            index=$((index + 1))
            [ "$index" -gt "$applied" ] && echo "not applied, skipping: $name/$(basename "$patch")"
        done
        index=$applied
        while [ "$index" -ge 1 ]; do
            eval "patch=\${$index}"
            label="$name/$(basename "$patch")"
            if ! git -C "$submodule" apply --reverse "$patch"; then
                warn "failed to revert: $label"
                break
            fi
            echo "reverted: $label"
            index=$((index - 1))
        done
    else
        index=0
        for patch in "$@"; do
            index=$((index + 1))
            label="$name/$(basename "$patch")"
            if [ "$index" -le "$applied" ]; then
                echo "already applied: $label"
                continue
            fi
            if ! git -C "$submodule" apply --check "$patch" || ! git -C "$submodule" apply "$patch"; then
                warn "does not apply (did the submodule change?): $label"
                break
            fi
            echo "applied: $label"
        done
    fi
done

exit "$failed"
