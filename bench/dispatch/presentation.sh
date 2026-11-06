#!/usr/bin/env bash
# Render the deck charts in the chosen presentation style.
#
# The benchmark scripts keep the repository's dark/mono look; this command
# re-renders the same logs through the presentation style (`brutal`). Add a
# chart here as it migrates to `styles.py`.
set -euo pipefail

out=${1:-presentation/assets}
[ $# -gt 0 ] && shift || true

mkdir -p "$out"
exec bench/charts/cache.py --chosen presentation --out "$out" "$@"
