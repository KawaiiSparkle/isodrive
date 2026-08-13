#!/bin/sh
set -e
ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
OUT="$ROOT/isodrive-plus.zip"
rm -f "$OUT"
# Magisk wants module files at zip root
cd "$ROOT/module"
chmod 755 system/bin/isodrive common/sepolicy_live.sh customize.sh post-fs-data.sh service.sh action.sh uninstall.sh \
  META-INF/com/google/android/update-binary 2>/dev/null || true
zip -r "$OUT" . -x '*.DS_Store' -x '*~' -x 'libs/.gitkeep'
echo "Wrote $OUT"
unzip -l "$OUT" | head -40
