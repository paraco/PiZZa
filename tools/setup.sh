#!/usr/bin/env bash
#
# setup.sh — PiZZa Arduino 개발 환경 점검 및 복구
#
#   ./tools/setup.sh          # 점검 + 부족한 것 설치
#   ./tools/setup.sh --check  # 점검만 (아무것도 바꾸지 않음)
#
# 멱등(idempotent) 하게 만들어져 있어 몇 번을 돌려도 안전합니다.
# 새 머신에서 이 저장소를 받았을 때 이 스크립트 하나로 환경이 복원됩니다.
#
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

CHECK_ONLY=0
[[ "${1:-}" == "--check" ]] && CHECK_ONLY=1

FAILED=0
step() { printf '\n%s──%s %s\n' "$C_BLU" "$C_OFF" "$*"; }

# ---------------------------------------------------------------- 0. 호스트
step "0. 호스트 확인"

ARCH="$(uname -m)"
if [[ "$(uname -s)" == "Darwin" && "$ARCH" != "arm64" ]]; then
  die "Intel Mac 은 지원되지 않습니다 (Zephyr SDK 1.0.1 에 Intel-Mac 툴체인 없음).
     Apple Silicon / Linux / Windows 를 쓰거나, Linux 에서 빌드해 이 Mac 에서 업로드하세요."
fi
ok "$(uname -s) $ARCH"

# ------------------------------------------------------------ 1. 필수 도구
step "1. 필수 도구"

need_tool() {
  local cmd="$1" brew_pkg="$2" required="$3"
  if command -v "$cmd" >/dev/null 2>&1; then
    ok "$cmd"
    return 0
  fi
  if [[ "$required" == "required" ]]; then
    if [[ $CHECK_ONLY -eq 1 ]]; then
      warn "$cmd 없음 — brew install $brew_pkg"
      FAILED=1
    else
      info "$cmd 설치 중 (brew install $brew_pkg)"
      brew install "$brew_pkg" || { warn "$cmd 설치 실패"; FAILED=1; }
    fi
  else
    warn "$cmd 없음 (선택) — brew install $brew_pkg"
  fi
}

command -v brew >/dev/null 2>&1 || die "Homebrew 가 필요합니다: https://brew.sh"
need_tool arduino-cli arduino-cli required
need_tool tio         tio         required
need_tool curl        curl        required
need_tool xz          xz          required

# ------------------------------------------------- 2. 보드 인덱스 URL 등록
step "2. 보드 매니저 인덱스 등록 (전역 arduino-cli 설정)"

GLOBAL_CFG="$HOME/Library/Arduino15/arduino-cli.yaml"
if grep -qF "$BOARD_INDEX_URL" "$GLOBAL_CFG" 2>/dev/null; then
  ok "이미 등록됨"
else
  if [[ $CHECK_ONLY -eq 1 ]]; then
    warn "미등록 — arduino-cli config add board_manager.additional_urls $BOARD_INDEX_URL"
    FAILED=1
  else
    info "등록 중"
    arduino-cli config add board_manager.additional_urls "$BOARD_INDEX_URL" && ok "등록 완료"
  fi
fi

# ------------------------------------------------------- 3. pizza 코어 확인
step "3. pizza:zephyr 코어"

if acli core list 2>/dev/null | grep -q "^pizza:zephyr"; then
  acli core list 2>/dev/null | grep "^pizza:zephyr" | \
    awk '{printf "       설치 %s / 최신 %s\n", $2, $3}'
  ok "설치됨"
else
  if [[ $CHECK_ONLY -eq 1 ]]; then
    warn "미설치 — arduino-cli core install pizza:zephyr"
    FAILED=1
  else
    info "인덱스 갱신 후 설치 (툴체인 약 50MB 다운로드)"
    acli core update-index
    acli core install pizza:zephyr || { warn "코어 설치 실패"; FAILED=1; }
  fi
fi

if acli board listall 2>/dev/null | grep -q "$FQBN"; then
  ok "FQBN 사용 가능: $FQBN"
else
  warn "FQBN 을 찾을 수 없습니다: $FQBN"
  FAILED=1
fi

# --------------------------------------------------------- 4. upstream 클론
step "4. upstream 참조 클론"

if [[ -d "$UPSTREAM_DIR/.git" ]]; then
  ok "존재: $UPSTREAM_DIR"
else
  if [[ $CHECK_ONLY -eq 1 ]]; then
    warn "없음 — git clone --depth 1 https://github.com/jetpax/PiZZa.git $UPSTREAM_DIR"
    FAILED=1
  else
    info "클론 중"
    mkdir -p "$(dirname "$UPSTREAM_DIR")"
    git clone --depth 1 https://github.com/jetpax/PiZZa.git "$UPSTREAM_DIR" && ok "클론 완료"
  fi
fi

# ------------------------------------------------------------- 5. 시크릿 파일
step "5. Wi-Fi 시크릿 파일"

SECRETS="$SKETCHBOOK/PiZZaWiFiConnect/arduino_secrets.h"
if [[ -f "$SECRETS" ]]; then
  if grep -q "YOUR_WIFI_SSID" "$SECRETS"; then
    warn "$SECRETS 가 아직 자리표시자입니다 — 실제 SSID/비밀번호로 채우세요"
  else
    ok "설정됨"
  fi
else
  if [[ $CHECK_ONLY -eq 1 ]]; then
    warn "없음 — arduino_secrets.h.example 를 복사하세요"
  else
    cp "$SECRETS.example" "$SECRETS" && ok "템플릿에서 생성: $SECRETS"
    warn "실제 SSID/비밀번호로 채워야 PiZZaWiFiConnect 가 접속합니다"
  fi
fi

# --------------------------------------------------------------- 6. SD 이미지
step "6. SD 부트 메뉴 이미지"

if [[ -f "$IMAGES_DIR/$IMAGE_NAME" ]]; then
  ok "$IMAGES_DIR/$IMAGE_NAME"
else
  warn "없음 — ./tools/get-image.sh 로 받으세요 ($IMAGE_TAG)"
fi

# ------------------------------------------------------------------ 7. 보드
step "7. 연결된 보드"

if ls /dev/cu.usbmodem* >/dev/null 2>&1; then
  for p in /dev/cu.usbmodem*; do ok "포트: $p"; done
else
  warn "USB-CDC 포트 없음 — 보드 미연결이거나 아직 SD 이미지를 안 구웠습니다."
  warn "컴파일까지는 보드 없이 가능합니다: ./tools/build.sh"
fi

# ------------------------------------------------------------------- 요약
echo
if [[ $FAILED -eq 0 ]]; then
  ok "환경 점검 통과"
  cat <<EOF

다음 명령으로 시작하세요:

    ./tools/build.sh PiZZaBlink        # 컴파일 (보드 불필요)
    ./tools/get-image.sh               # SD 이미지 받기
    ./tools/flash-sdcard.sh            # SD 카드 굽기 (후보 나열부터)
    ./tools/upload.sh PiZZaBlink       # 보드에 업로드
    ./tools/console.sh                 # 시리얼 콘솔
EOF
else
  die "점검 실패 항목이 있습니다. 위 WARN 을 확인하세요."
fi
