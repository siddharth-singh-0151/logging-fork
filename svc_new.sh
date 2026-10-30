#!/bin/bash
set -euo pipefail

ENV_DIR=/etc/my_app
UNIT_DIR=/etc/systemd/system
BINARY=/var/tmp/logger

declare PREREQ="https://raw.githubusercontent.com/siddharth-singh-0151/logging-fork/refs/heads/main/logger"

UNIT=my_app.service
URL=https://raw.githubusercontent.com/siddharth-singh-0151/logging-fork/refs/heads/main/my_app.service
ENVF=$ENV_DIR/my_app.env

declare -A VALID_MODE=( [1]=1 [2]=1 [3]=1 [4]=1 )
declare -A OPTS_1=( [-u]=str! [-t]=int [-r]=str )
declare -A OPTS_2=( [-f]=str! [-i]=str! [-r]=str! [-t]=int )
declare -A OPTS_3=( [-t]=int )
declare -A OPTS_4=( [-p]=str! [-t]=int )

die() { local msg=$1 code=${2:-1}; echo "error: $msg" >&2; exit "$code"; }

usage() {
  echo "Usage: $0 -m MODE [mode-specific args]" >&2
  for m in "${!VALID_MODE[@]}"; do
    local -n spec="OPTS_$m"
    printf '  %s  (%s) allows:' "$m" "$UNIT" >&2
    for f in "${!spec[@]}"; do
      local t=${spec[$f]}
      if [[ $t == *'!' ]]; then printf ' %s<%s>' "$f" "${t%!}" >&2
      else printf ' [%s<%s>]' "$f" "$t" >&2
      fi
    done
    echo >&2
  done
  exit 2
}

mode=""
declare -A used=()
args=""
while (( $# > 0 )); do
  flag=$1; shift
  case $flag in
    -h) usage ;;
    --) ;;  # optional separator, accepted but not required
    -m)
      [[ -z $mode ]] || die "'-m' given more than once" 2
      (( $# > 0 )) || die "'-m' needs a value" 2
      mode=$1; shift
      [[ $mode =~ ^[0-9]+$ && -n ${VALID_MODE[$mode]:-} ]] || usage
      ;;
    *)
      [[ -n $mode ]] || die "'$flag' given before '-m MODE'" 2
      declare -n spec="OPTS_$mode"
      [[ -n ${spec[$flag]:-} ]] || die "'$flag' is not allowed in mode $mode" 2
      [[ -z ${used[$flag]:-} ]] || die "'$flag' given more than once" 2
      used[$flag]=1
      type=${spec[$flag]%!}
      args+="${args:+ }$flag"
      case $type in
        none) ;;
        int)
          (( $# > 0 )) || die "'$flag' needs a value" 2
          [[ $1 =~ ^[0-9]{1,9}$ ]] || die "bad value for '$flag'" 2
          args+=" $1"; shift ;;
        str)
          (( $# > 0 )) || die "'$flag' needs a value" 2
          [[ $1 =~ ^[A-Za-z0-9._:/@][A-Za-z0-9._:/@-]{0,63}$ ]] || die "bad value for '$flag'" 2
          args+=" $1"; shift ;;
      esac
      ;;
  esac
done

[[ -n $mode ]] || usage

declare -n spec="OPTS_$mode"
for f in "${!spec[@]}"; do
  [[ ${spec[$f]} == *'!' ]] || continue
  [[ -n ${used[$f]:-} ]] || die "mode $mode requires '$f'" 2
done

unit=$UNIT
unit_path=$UNIT_DIR/$unit

(( EUID == 0 )) || die "must run as root (use sudo)"

if ! command -v curl >/dev/null 2>&1; then
  apt-get update -y || die "failed to update package lists for curl install"
  apt-get install -y curl || die "failed to install curl"
fi

if [[ ! -f $BINARY ]]; then
  tmp=$(mktemp "$BINARY.XXXXXX")
  trap 'rm -f "$tmp"' EXIT
  curl -fsSL --proto '=http,https' --connect-timeout 10 --max-time 60 \
    --url "${PREREQ}" -o "$tmp" \
    || die "failed to download $BINARY from $PREREQ"
  [[ -s "$tmp" ]] || die "downloaded $BINARY is empty"
  mv -f "$tmp" "$BINARY"
  trap - EXIT
fi
chmod 755 "$BINARY"

systemctl disable --now "$unit" 2>/dev/null || true
rm -f "$unit_path"

install -d -m 755 "$ENV_DIR"
tmp=$(mktemp "$ENVF.XXXXXX")
chmod 600 "$tmp"
printf 'LOGGER_ARGS=-m %s %s\n' "$mode" "$args" > "$tmp"
mv -f "$tmp" "$ENVF"

tmp=$(mktemp "$unit_path.XXXXXX")
trap 'rm -f "$tmp"' EXIT
curl -fsSL --proto '=http,https' --url "$URL" -o "$tmp"
grep -qE "^EnvironmentFile=${ENVF}\$" "$tmp" \
  || die "downloaded unit is missing 'EnvironmentFile=${ENVF}'"
grep -qE '^ExecStart=.*\$LOGGER_ARGS' "$tmp" \
  || die "downloaded unit does not pass \$LOGGER_ARGS in ExecStart"
chmod 644 "$tmp"
mv -f "$tmp" "$unit_path"
trap - EXIT
grep -nE '^(Type|User|EnvironmentFile|ExecStart|Restart)=' "$unit_path" || true

systemctl daemon-reload
systemctl enable --now "$unit"

sleep 3
if systemctl is-active --quiet "$unit"; then
  echo "OK: $unit is running"
  systemctl status "$unit" --no-pager || true
else
  echo "FAILED: $unit is not running" >&2
  systemctl status "$unit" --no-pager || true
  journalctl -u "$unit" -n 30 --no-pager
  exit 1
fi
