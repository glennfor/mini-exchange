#!/usr/bin/env bash
# Guard: floating point is forbidden in the layers that carry prices.
#
# Integer ticks are the whole reason the engine is deterministic and the whole
# reason a price comparison is exact. This check is deliberately crude -- a
# grep -- because a crude check that runs on every push beats a sophisticated
# one that does not. Test files are exempt: they may legitimately compute
# ratios and rates for reporting.
set -euo pipefail

dirs=(core book engine feed)
pattern='(^|[^[:alnum:]_"])(float|double|long double)([^[:alnum:]_]|$)'
status=0

for d in "${dirs[@]}"; do
  [ -d "$d" ] || continue
  while IFS= read -r f; do
    case "$f" in
      */tests/*) continue ;;
    esac
    # Strip // comments before matching so prose about floats is allowed.
    if sed 's://.*::' "$f" | grep -nEq "$pattern"; then
      echo "FORBIDDEN floating point in $f:"
      sed 's://.*::' "$f" | grep -nE "$pattern" | sed 's/^/    /'
      status=1
    fi
  done < <(find "$d" -name '*.hpp' -o -name '*.cpp' -o -name '*.h')
done

if [ "$status" -eq 0 ]; then
  echo "ok: no floating point in ${dirs[*]}"
fi
exit "$status"
