#!/usr/bin/env bash
#
# upload.sh — 컴파일된 스케치를 USB-CDC 로 PiZZa 로더에 스트리밍
#
#   ./tools/upload.sh                      # PiZZaBlink 빌드 후 업로드
#   ./tools/upload.sh PiZZaHelloSerial
#   PIZZA_PORT=/dev/cu.usbmodem1234 ./tools/upload.sh PiZZaBlink
#   ./tools/upload.sh PiZZaBlink --no-build   # 기존 빌드 산출물 그대로 업로드
#
# 동작: cdc-upload 가 스케치를 로더에 전송 → 로더가 SD 에 기록 →
#       콜드 리셋(약 1초) → 재부팅 시 /SD:/sketch.llext 자동 로드
#
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

SKETCH_NAME="${1:-PiZZaBlink}"
shift || true

DO_BUILD=1
for arg in "$@"; do
  case "$arg" in
    --no-build) DO_BUILD=0 ;;
  esac
done

SKETCH_PATH="$(resolve_sketch "$SKETCH_NAME")"
OUT_DIR="$BUILD_DIR/$SKETCH_NAME"

check_ide_not_running
check_no_stale_upload
PORT="$(resolve_port)"

if [[ $DO_BUILD -eq 1 ]]; then
  "$PROJECT_ROOT/tools/build.sh" "$SKETCH_NAME"
  echo
else
  [[ -d "$OUT_DIR" ]] || die "빌드 산출물이 없습니다: $OUT_DIR  (--no-build 를 빼고 실행하세요)"
fi

info "스케치 : $SKETCH_NAME"
info "포트   : $PORT"
info "방식   : USB (CDC) — upload_method=usbcdc"
echo

# 응답 없이 매달리면 좀비가 되어 포트를 영구 점유하므로 감시자를 붙인다
watchdog_cdc_upload &
WATCHDOG_PID=$!
trap 'kill "$WATCHDOG_PID" 2>/dev/null || true' EXIT

set +e
acli upload \
  --fqbn "$FQBN" \
  --port "$PORT" \
  --input-dir "$OUT_DIR" \
  "$SKETCH_PATH"
RC=$?
set -e

kill "$WATCHDOG_PID" 2>/dev/null || true
trap - EXIT

echo
if [[ $RC -eq 0 ]]; then
  ok "업로드 완료. 보드가 콜드 리셋되며 약 1초 뒤 새 스케치가 자동 로드됩니다."
  info "직렬 출력 확인:  ./tools/console.sh"
else
  die "업로드 실패 (exit $RC)

     흔한 원인 순서대로:
     1. Arduino IDE 가 실행 중  → ⌘Q 로 완전 종료
     2. 다른 콘솔(tio/screen)이 포트 점유 → 종료 후 재시도
     3. 좀비 cdc-upload 잔존 → lsof /dev/cu.usbmodem* 확인
     4. 위를 다 해도 안 되면 USB 케이블 재연결 (전원 차단)
     5. 그래도 안 되면 SD 카드로 우회:  ./tools/sd-install.sh $SKETCH_NAME"
fi
