#!/usr/bin/env bash
#
# get-image.sh — PiZZa 부트 메뉴 SD 이미지 다운로드
#
#   ./tools/get-image.sh          # images/ 에 없으면 받는다
#   ./tools/get-image.sh --force  # 다시 받는다
#
# 이 이미지 한 장에 RetroPiZZa / Arduino / MicroPython / PiZZa Shell 이
# 모두 들어 있습니다. Arduino 전용 이미지는 따로 없습니다.
#
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

mkdir -p "$IMAGES_DIR"
DEST="$IMAGES_DIR/$IMAGE_NAME"

if [[ -f "$DEST" && "${1:-}" != "--force" ]]; then
  ok "이미 존재: $DEST"
  ls -lh "$DEST" | awk '{printf "     크기: %s\n", $5}'
  info "다시 받으려면: $0 --force"
  exit 0
fi

info "보드   : $BOARD_LABEL"
info "릴리스 : $IMAGE_TAG"
info "URL    : $IMAGE_URL"
echo

curl -fL --progress-bar -o "$DEST.part" "$IMAGE_URL" || die "다운로드 실패"
mv "$DEST.part" "$DEST"

echo
ok "다운로드 완료: $DEST"
ls -lh "$DEST" | awk '{printf "     크기: %s\n", $5}'

if xz -t "$DEST" 2>/dev/null; then
  ok "아카이브 무결성 확인 (xz -t)"
else
  warn "xz 무결성 검사 실패 — 파일이 손상됐을 수 있습니다"
fi

cat <<EOF

${C_YEL}다음 단계 — SD 카드 쓰기 (호스트에서 딱 한 번)${C_OFF}

  ${C_DIM}권장:${C_OFF} Raspberry Pi Imager
     Choose OS → Use custom → 위 .img.xz 선택 → 대상 SD 선택 → Write
     (.img.xz 를 직접 읽으므로 압축 해제 불필요)

  ${C_DIM}CLI:${C_OFF} ./tools/flash-sdcard.sh
     ${C_RED}대상 디스크 전체를 덮어씁니다. 실행 전 diskutil list 로 반드시 확인하세요.${C_OFF}

카드를 넣고 전원을 켜면 부트 메뉴가 뜹니다. ${C_GRN}Arduino${C_OFF} 를 선택하세요.
선택은 기억되므로 이후에는 바로 로더로 부팅합니다.
메뉴로 되돌아가려면 전원 인가 시 GPIO 17 과 GND 를 단락시키세요.
EOF
