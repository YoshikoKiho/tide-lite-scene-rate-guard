#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="1.0.0"
NAME="scene_rate_guard-v${VERSION}.zip"
OUT="$ROOT/release/$NAME"

mkdir -p "$ROOT/release"
rm -f "$OUT" "$OUT.sha256"

(
  cd "$ROOT/module"
  zip -9 -r "$OUT" \
    module.prop customize.sh service.sh uninstall.sh README.md bin webroot \
    -x '*.DS_Store' '*.log'
)

sha256sum "$OUT" > "$OUT.sha256"
printf 'Built %s\n' "$OUT"
cat "$OUT.sha256"
