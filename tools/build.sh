#!/usr/bin/env bash
#
# build.sh — PiZZa 스케치 컴파일
#
#   ./tools/build.sh                 # 기본 스케치(PiZZaBlink) 컴파일
#   ./tools/build.sh PiZZaHelloSerial
#   ./tools/build.sh PiZZaBlink --clean
#
# 산출물: build/<스케치명>/<스케치명>.ino.elf
#
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

SKETCH_NAME="${1:-PiZZaBlink}"
shift || true

SKETCH_PATH="$(resolve_sketch "$SKETCH_NAME")"
OUT_DIR="$BUILD_DIR/$SKETCH_NAME"

EXTRA=()
for arg in "$@"; do
  case "$arg" in
    --clean) info "빌드 캐시 삭제: $OUT_DIR"; rm -rf "$OUT_DIR" ;;
    *)       EXTRA+=("$arg") ;;
  esac
done

mkdir -p "$OUT_DIR"

info "스케치 : $SKETCH_NAME"
info "보드   : $BOARD_LABEL"
info "FQBN   : $FQBN"
info "출력   : $OUT_DIR"
echo

# macOS 기본 bash 3.2 에서는 set -u + 빈 배열 확장이 오류라서 ${a[@]+"${a[@]}"} 형태 사용
acli compile \
  --fqbn "$FQBN" \
  --build-path "$OUT_DIR" \
  ${EXTRA[@]+"${EXTRA[@]}"} \
  "$SKETCH_PATH"

ELF="$OUT_DIR/${SKETCH_NAME}.ino.elf"
if [[ -f "$ELF" ]]; then
  echo
  ok "빌드 성공: $ELF"
  ls -lh "$ELF" | awk '{printf "       크기: %s\n", $5}'
else
  echo
  warn "예상 경로에 ELF 가 없습니다: $ELF"
  find "$OUT_DIR" -maxdepth 1 -name '*.elf' -exec ls -lh {} \;
fi
