#!/usr/bin/env bash
# tools/common.sh — 모든 PiZZa 스크립트가 source 하는 공통 정의
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLI_CONFIG="$PROJECT_ROOT/arduino-cli.yaml"
SKETCHBOOK="$PROJECT_ROOT/sketches"
BUILD_DIR="$PROJECT_ROOT/build"
IMAGES_DIR="$PROJECT_ROOT/images"
UPSTREAM_DIR="$PROJECT_ROOT/upstream/PiZZa"

# Raspberry Pi Zero 2 W (BCM2710)
FQBN="pizza:zephyr:pizza"
BOARD_LABEL="Raspberry Pi Zero 2 W (BCM2710)"

# 콘솔 기본 보율. PIZZA_BAUD 로 재정의할 수 있습니다.
#   PIZZA_BAUD=9600 ./tools/console.sh
#
# `Serial` 은 USB CDC ACM 이라 이 값이 실제 처리량을 정하지 않습니다 (USB 벌크).
# 다만 이 값이 CDC line coding 으로 장치에 전달되고, 스케치는 그걸 읽어
# 출력량을 맞춥니다 (PiZZaThreadMonitor 참고).
#
# 업로더가 쓰는 1200 은 데이터 속도가 아니라 "업로드 모드로 들어가라"는
# 노크 신호입니다. 여기서 바꾸는 값과 무관하고, 바꿀 수도 없습니다.
BAUD="${PIZZA_BAUD:-115200}"

# 릴리스 이미지 (부트 메뉴 카드 — Arduino 로더 포함)
IMAGE_TAG="pizza-v0.7.1"
IMAGE_NAME="pizza-menu-rpi_zero_2w-v0.7.1.img.xz"
IMAGE_URL="https://github.com/jetpax/PiZZa/releases/download/${IMAGE_TAG}/${IMAGE_NAME}"

BOARD_INDEX_URL="https://raw.githubusercontent.com/jetpax/PiZZa/main/apps/Arduino/package_pizza_index.json"

if [[ -t 1 ]]; then
  C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'
  C_BLU=$'\033[34m'; C_DIM=$'\033[2m';  C_OFF=$'\033[0m'
else
  C_RED=''; C_GRN=''; C_YEL=''; C_BLU=''; C_DIM=''; C_OFF=''
fi

info() { printf '%s[INFO]%s %s\n' "$C_BLU" "$C_OFF" "$*"; }
ok()   { printf '%s[ OK ]%s %s\n' "$C_GRN" "$C_OFF" "$*"; }
warn() { printf '%s[WARN]%s %s\n' "$C_YEL" "$C_OFF" "$*" >&2; }
die()  { printf '%s[FAIL]%s %s\n' "$C_RED" "$C_OFF" "$*" >&2; exit 1; }

acli() { arduino-cli --config-file "$CLI_CONFIG" "$@"; }

# PiZZa 보드의 USB-CDC 포트를 찾는다. 여러 개면 실패시키고 사용자가 고르게 한다.
find_port() {
  local ports=()
  while IFS= read -r p; do
    [[ -e "$p" ]] && ports+=("$p")
  done < <(ls /dev/cu.usbmodem* 2>/dev/null || true)

  if [[ ${#ports[@]} -eq 0 ]]; then
    die "USB-CDC 포트를 찾지 못했습니다 (/dev/cu.usbmodem*).
     - Pi 의 '안쪽' USB 포트(데이터, PWR 아님)에 연결했는지 확인
     - 충전 전용이 아닌 데이터 케이블인지 확인
     - 전원 인가 후 포트가 뜨기까지 약 6초 대기
     - 부트 메뉴에서 'Arduino' 항목을 선택했는지 확인"
  fi

  if [[ ${#ports[@]} -gt 1 ]]; then
    warn "USB-CDC 포트가 여러 개입니다: ${ports[*]}"
    warn "PIZZA_PORT 환경변수로 지정하세요.  예) PIZZA_PORT=${ports[0]} $0"
    die "포트 모호성으로 중단"
  fi

  printf '%s' "${ports[0]}"
}

resolve_port() {
  if [[ -n "${PIZZA_PORT:-}" ]]; then
    [[ -e "$PIZZA_PORT" ]] || die "PIZZA_PORT=$PIZZA_PORT 가 존재하지 않습니다"
    printf '%s' "$PIZZA_PORT"
  else
    find_port
  fi
}

# Arduino IDE 가 떠 있으면 업로드가 반드시 깨진다 — 경고가 아니라 차단한다.
#
# IDE 는 serial-discovery + Serial Monitor 하위 프로세스로 포트를 상시 감시하다가
# 포트가 나타나는 즉시 붙잡습니다. 1200-bps 터치 직후 보드가 재열거되는 바로 그
# 순간과 충돌해서, cdc-upload 가 응답을 영영 못 받고 멈춥니다. 케이블을 다시
# 꽂아도 포트가 돌아오는 순간 IDE 가 또 잡으므로 무한 반복됩니다.
#
# 실제로 이 프로젝트에서 IDE 실행 중 업로드가 성공한 사례는 0건입니다.
# 굳이 강행하려면 PIZZA_ALLOW_IDE=1 을 주세요 (권장하지 않음).
check_ide_not_running() {
  local pids
  pids="$(pgrep -f 'Arduino IDE.app/Contents/MacOS' 2>/dev/null || true)"
  [[ -z "$pids" ]] && return 0

  if [[ "${PIZZA_ALLOW_IDE:-0}" == "1" ]]; then
    warn "Arduino IDE 실행 중 — PIZZA_ALLOW_IDE=1 로 강행합니다 (멈출 수 있음)"
    return 0
  fi

  die "Arduino IDE 가 실행 중입니다 (PID: $(echo "$pids" | tr '\n' ' '))

     IDE 가 떠 있으면 업로드는 100% 실패합니다. 창을 닫는 것으로는 부족하고
     ⌘Q 로 애플리케이션을 완전히 종료해야 합니다.

     종료 후 확인:
       lsof /dev/cu.usbmodem*     ← 아무것도 안 나와야 정상

     IDE 로 계속 작업하시려면 IDE 안에서 업로드하세요. 단, 그 경우에도
     Serial Monitor 를 닫아야 하며 이 스크립트는 쓰지 마세요.
     (정말 강행하려면 PIZZA_ALLOW_IDE=1 $0 ...)"
}

# 이전 시도가 남긴 cdc-upload 가 있으면 포트가 이미 엉킨 상태다.
# 그 위에 새 업로드를 얹으면 똑같이 매달리므로 시작 전에 막는다.
#
# 핵심: STAT 이 `U`(uninterruptible sleep)인 프로세스는 커널 드라이버 안에서
# 멈춘 것이라 kill -9 로도 못 죽입니다. USB 장치를 물리적으로 분리해야만
# 드라이버가 대기를 풀고 정리됩니다.
check_no_stale_upload() {
  local pids
  pids="$(pgrep -f 'cdc-upload' 2>/dev/null || true)"
  [[ -z "$pids" ]] && return 0

  # 죽일 수 있는 것만 정리해 본다. U 상태는 시그널이 안 듣는다.
  kill $pids 2>/dev/null || true
  /bin/sleep 2
  kill -9 $pids 2>/dev/null || true
  /bin/sleep 2

  # STAT 문자열은 해석하지 않는다. `?E`(정리 중)에 대해 ps 가 빈 값을 돌려줘
  # "정리됨"으로 오판하는 버그가 있었다. 존재 여부만 본다 — 하나라도 남아
  # 있으면 포트는 아직 엉킨 상태다.
  pids="$(pgrep -f 'cdc-upload' 2>/dev/null || true)"
  [[ -z "$pids" ]] && { info "이전 cdc-upload 잔존 프로세스를 정리했습니다"; return 0; }

  die "정리되지 않는 cdc-upload 가 남아 있습니다: $(echo "$pids" | tr '\n' ' ')

$(ps -o pid,etime,stat -p $(echo "$pids" | tr '\n' ',' | sed 's/,$//') 2>/dev/null | sed 's/^/       /')

     STAT 이 U 면 커널 드라이버 안에서 멈춘 것이라 kill -9 로도 안 죽습니다.
     포트가 이미 엉킨 상태라 지금 업로드하면 똑같이 매달리고 하나 더 쌓입니다.

     ▶ USB 케이블을 뽑고 3초 뒤 다시 꽂으세요. 소프트웨어로는 못 풉니다.
       장치가 사라져야 드라이버가 대기를 풀고 전부 정리됩니다.

     그 뒤 확인:
       pgrep -f cdc-upload      ← 아무것도 안 나와야 정상
       lsof /dev/cu.usbmodem*   ← 아무것도 안 나와야 정상"
}

# cdc-upload 가 응답 없이 매달리면 좀비가 되어 포트를 영구 점유한다.
# 제한 시간을 넘기면 죽여서 다음 시도를 살린다. 단 U 상태는 못 죽인다.
UPLOAD_TIMEOUT="${PIZZA_UPLOAD_TIMEOUT:-60}"

watchdog_cdc_upload() {
  local deadline=$((SECONDS + UPLOAD_TIMEOUT))
  while (( SECONDS < deadline )); do
    pgrep -f 'cdc-upload' >/dev/null 2>&1 || return 0
    /bin/sleep 1
  done
  local stuck
  stuck="$(pgrep -f 'cdc-upload' 2>/dev/null || true)"
  if [[ -n "$stuck" ]]; then
    warn "cdc-upload 가 ${UPLOAD_TIMEOUT}초를 넘겼습니다 — 정리합니다 (PID: $(echo "$stuck" | tr '\n' ' '))"
    kill $stuck 2>/dev/null || true
    /bin/sleep 2
    kill -9 $stuck 2>/dev/null || true
  fi
}

# 스케치 이름 → 경로. 인자 없으면 기본 스케치.
resolve_sketch() {
  local name="${1:-PiZZaBlink}"
  local path="$SKETCHBOOK/$name"
  [[ -d "$path" ]] || die "스케치를 찾을 수 없습니다: $path
     사용 가능: $(ls "$SKETCHBOOK" | grep -v '^libraries$' | tr '\n' ' ')"
  printf '%s' "$path"
}
