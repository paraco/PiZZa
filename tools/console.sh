#!/usr/bin/env bash
#
# console.sh — PiZZa USB-CDC 시리얼 콘솔
#
#   ./tools/console.sh                        # 포트 자동 탐지, 115200 (기본)
#   PIZZA_BAUD=9600 ./tools/console.sh        # 보율 변경
#   PIZZA_PORT=/dev/cu.usbmodem1234 ./tools/console.sh
#   ./tools/console.sh --uart /dev/cu.usbserial-XXXX   # mini-UART(부트 로그)
#
# USB CDC 에서 보율은 실제 처리량을 바꾸지 않지만(USB 벌크), CDC line coding
# 으로 장치에 전달됩니다. PiZZaThreadMonitor 는 그 값을 읽어 리포트 상세도를
# 자동으로 낮추거나 올립니다.
#
# PiZZaHtop 스케치를 올리면 이 콘솔이 대화형 셸이 됩니다:
#     pizza> htop     실시간 모니터 (q 로 복귀)
#     pizza> ps / mem / help
# 반드시 tio(또는 screen)로 붙어야 합니다 — Arduino IDE 의 Serial Monitor 는
# ANSI 이스케이프를 해석하지 못하고 키 입력도 제대로 전달하지 못합니다.
#
# 종료: tio 는 Ctrl-t q,  screen 은 Ctrl-a k
#
# 주의: Arduino IDE 가 실행 중이면 Serial Monitor 가 포트를 선점합니다.
#       프로세스를 죽여도 재생성되므로 IDE 를 ⌘Q 로 완전히 종료하세요.
#
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

PORT=""
if [[ "${1:-}" == "--uart" ]]; then
  PORT="${2:-}"
  [[ -n "$PORT" ]] || die "--uart 뒤에 포트 경로가 필요합니다"
  info "mini-UART 모드 (GPIO14 TX / GPIO15 RX / GND) — 로더 부팅 로그"
else
  check_ide_not_running
  PORT="$(resolve_port)"
fi

info "포트 : $PORT @ ${BAUD} 8N1"

if command -v tio >/dev/null 2>&1; then
  info "종료 : Ctrl-t 다음 q"
  echo
  exec tio "$PORT" -b "$BAUD"
elif command -v screen >/dev/null 2>&1; then
  warn "tio 가 없어 screen 으로 대체합니다 (brew install tio 권장)"
  info "종료 : Ctrl-a 다음 k"
  echo
  exec screen "$PORT" "$BAUD"
else
  die "tio 또는 screen 이 필요합니다.  brew install tio"
fi
