#!/usr/bin/env bash
#
# flash-sdcard.sh — upstream flash-sdcard.sh 얇은 래퍼
#
#   ./tools/flash-sdcard.sh              # 후보 디스크만 나열, 쓰지 않음
#   ./tools/flash-sdcard.sh /dev/disk6   # 지정 디스크에 쓰기
#
# 하는 일은 images/ 의 이미지를 찾아 upstream 스크립트에 넘기는 것뿐입니다.
# 실제 안전장치는 upstream 이 갖고 있으며, 모두 통과해야 씁니다:
#   - 파티션이 아닌 '전체 디스크' 일 것
#   - external / removable 일 것
#   - 현재 / 를 받치는 디스크가 아닐 것
#   - 크기 상한(기본 128GB) 이하일 것  ← 외장 백업 드라이브를 걸러냅니다
#   - 디스크 식별자를 직접 다시 타이핑할 것
# 쓴 뒤에는 같은 바이트 수를 되읽어 SHA-256 을 비교하고 eject 합니다.
#
# GUI 를 선호하면 Raspberry Pi Imager 를 쓰세요 (.img.xz 직접 지원).
#
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

IMAGE="$IMAGES_DIR/$IMAGE_NAME"

[[ -f "$IMAGE" ]] || die "이미지가 없습니다: $IMAGE
     먼저 실행:  ./tools/get-image.sh"

[[ -x "$UPSTREAM_DIR/flash-sdcard.sh" ]] || die "upstream 클론이 없습니다: $UPSTREAM_DIR
     먼저 실행:  ./tools/setup.sh"

info "이미지 : $IMAGE"
info "보드   : $BOARD_LABEL"

if [[ $# -eq 0 ]]; then
  warn "대상 디스크 미지정 — upstream 이 후보만 나열하고 아무것도 쓰지 않습니다."
fi
echo

exec "$UPSTREAM_DIR/flash-sdcard.sh" "$IMAGE" "$@"
