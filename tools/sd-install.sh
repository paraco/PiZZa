#!/usr/bin/env bash
#
# sd-install.sh — 빌드한 스케치를 SD 카드에 직접 설치 (USB 업로드 우회)
#
#   ./tools/sd-install.sh PiZZaHelloSerial     # 카드에 설치
#   ./tools/sd-install.sh --restore            # 격리해 둔 스케치 목록/복원 안내
#   ./tools/sd-install.sh --clear              # 스케치 제거 (로더만 부팅)
#
# ─── 언제 쓰나 ────────────────────────────────────────────────────────
# 스케치가 보드를 죽이면 USB CDC 도 같이 죽어서 ./tools/upload.sh 가 통하지
# 않습니다. 그때 카드를 빼서 이 스크립트로 정상 스케치를 얹으면 복구됩니다.
#
# 로더는 부팅할 때 /SD:/sketch.llext 를 읽어 llext 모듈로 올립니다. cdc-upload
# 가 하는 일도 결국 그 파일을 쓰는 것이라, 파일을 직접 복사하는 것과 동등합니다.
#
# 전제: Pi 에서 SD 카드를 빼서 카드리더에 꽂아 /Volumes/PIZZA 로 마운트된 상태
#
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

SD_MOUNT="${PIZZA_SD_MOUNT:-/Volumes/PIZZA}"
TARGET="$SD_MOUNT/sketch.llext"

[[ -d "$SD_MOUNT" ]] || die "SD 카드가 마운트되지 않았습니다: $SD_MOUNT
     - Pi 의 전원(USB)을 뽑고 카드를 빼서 카드리더에 꽂으세요
     - 다른 경로면:  PIZZA_SD_MOUNT=/Volumes/XXX $0 ..."

[[ -f "$SD_MOUNT/arduino.bin" ]] || die "PiZZa 카드가 아닌 것 같습니다 (arduino.bin 없음): $SD_MOUNT"

finish() {
  # macOS 가 만든 AppleDouble 리소스 포크는 지웁니다 (로더가 읽을 필요 없는 쓰레기)
  dot_clean -m "$SD_MOUNT" 2>/dev/null || true
  rm -f "$SD_MOUNT"/._* 2>/dev/null || true
  sync
  echo
  info "카드 내용:"
  ls -la "$SD_MOUNT"/*.llext "$SD_MOUNT"/*.bad 2>/dev/null | sed 's/^/       /'
  echo
  ok "완료. 카드를 꺼내고 Pi 에 꽂으세요:"
  echo "       diskutil eject $SD_MOUNT"
}

case "${1:-}" in
  --clear)
    if [[ -f "$TARGET" ]]; then
      mv "$TARGET" "$SD_MOUNT/sketch.llext.removed"
      ok "제거: sketch.llext → sketch.llext.removed (지우지 않고 이름만 변경)"
    else
      warn "sketch.llext 가 이미 없습니다"
    fi
    info "로더가 스케치 없이 부팅해 업로드를 기다립니다"
    finish
    exit 0
    ;;

  --restore)
    info "격리/보관된 파일:"
    ls -la "$SD_MOUNT"/*.bad "$SD_MOUNT"/*.removed 2>/dev/null | sed 's/^/       /' || echo "       (없음)"
    echo
    info "되돌리려면:  mv $SD_MOUNT/<파일> $TARGET"
    exit 0
    ;;

  ""|-h|--help)
    echo "사용법: $0 <스케치명> | --clear | --restore"
    echo
    echo "빌드된 스케치:"
    for d in "$BUILD_DIR"/*/; do
      [[ -d "$d" ]] || continue
      n="$(basename "$d")"
      f="$d/$n.ino.elf"
      [[ -f "$f" ]] && printf "  %-24s %8d bytes\n" "$n" "$(stat -f%z "$f")"
    done
    exit 0
    ;;
esac

SKETCH_NAME="$1"
ELF="$BUILD_DIR/$SKETCH_NAME/$SKETCH_NAME.ino.elf"

[[ -f "$ELF" ]] || die "빌드 산출물이 없습니다: $ELF
     먼저 실행:  ./tools/build.sh $SKETCH_NAME"

# aarch64 재배치 ELF 가 맞는지 확인 — 엉뚱한 파일을 얹으면 보드가 부팅 중 죽습니다
if ! file "$ELF" | grep -q "ELF 64-bit.*relocatable.*aarch64"; then
  die "aarch64 재배치 ELF 가 아닙니다: $ELF
     $(file "$ELF")"
fi

# 기존 스케치는 지우지 않고 보관
if [[ -f "$TARGET" ]]; then
  mv "$TARGET" "$SD_MOUNT/sketch.llext.prev"
  info "이전 스케치 보관: sketch.llext → sketch.llext.prev"
fi

cp "$ELF" "$TARGET"

if cmp -s "$ELF" "$TARGET"; then
  ok "설치: $SKETCH_NAME ($(stat -f%z "$TARGET") bytes) → sketch.llext"
  ok "바이트 단위 일치 확인"
else
  die "복사 검증 실패 — 카드에 문제가 있을 수 있습니다"
fi

finish
