#!/bin/bash
set -euo pipefail

UNIT_DIR=/etc/systemd/system
BINARY=/var/tmp/logger

die() { echo "error: $1" >&2; exit "${2:-1}"; }

(( EUID == 0 )) || die "must run as root (use sudo)"

mapfile -t units < <(
  grep -lE "^ExecStart=${BINARY}([[:space:]]|\$)" "$UNIT_DIR"/*.service 2>/dev/null \
    | xargs -r -n1 basename
)

if (( ${#units[@]} == 0 )); then
  echo "no services found using $BINARY"
  exit 0
fi

echo "found ${#units[@]} service(s) using $BINARY:"
printf '  %s\n' "${units[@]}"

failed=()
for u in "${units[@]}"; do
  echo "stopping $u ..."
  systemctl disable --now "$u" 2>/dev/null || true
  rm -f "$UNIT_DIR/$u"
done

systemctl daemon-reload

for u in "${units[@]}"; do
  if systemctl is-active --quiet "$u" 2>/dev/null; then
    failed+=("$u")
  fi
done

if (( ${#failed[@]} > 0 )); then
  echo "FAILED: still active after cleanup:" >&2
  printf '  %s\n' "${failed[@]}" >&2
  exit 1
fi

if [[ -f $BINARY ]]; then
  rm -f "$BINARY"
  echo "removed $BINARY"
else
  echo "$BINARY not present, nothing to remove"
fi

echo "OK: all logger services stopped, disabled, and removed"
