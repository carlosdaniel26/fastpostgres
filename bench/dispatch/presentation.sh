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
# Nothing has migrated to styles.py yet. The lab that drove the candidate
# looks was built over the cache figure and went with it; the repository
# charts still draw through theme.py, so there is no deck set to render.
echo "presentation: no chart has migrated to styles.py yet; nothing to render"
echo "  (see bench/charts/STYLE-LAB.md: the lab needs rebuilding over the ADBC figure)"
