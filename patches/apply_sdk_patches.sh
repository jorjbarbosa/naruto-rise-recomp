#!/usr/bin/env bash
# Applies patches/sdk/*.patch to the SDK submodule, in name order, skipping the
# ones already applied (the shell twin of apply_sdk_patches.ps1).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk="$root/sdk"

if [ ! -e "$sdk/.git" ]; then
  echo "SDK not found at $sdk. Run: git submodule update --init --recursive" >&2
  exit 1
fi

shopt -s nullglob
patches=("$root"/patches/sdk/*.patch)
if [ ${#patches[@]} -eq 0 ]; then
  echo "No SDK patches to apply."
  exit 0
fi

for patch in "${patches[@]}"; do
  name="$(basename "$patch")"
  echo "Applying SDK patch: $name ..."
  # Already applied when it reverses cleanly.
  if git -C "$sdk" apply --reverse --check "$patch" 2>/dev/null; then
    echo "  already applied."
    continue
  fi
  if ! git -C "$sdk" apply --check "$patch" 2>/dev/null; then
    echo "Patch $name does not apply cleanly" >&2
    exit 1
  fi
  git -C "$sdk" apply "$patch"
  echo "  applied."
done
