#!/usr/bin/env bash
#
# build-smp-loader.sh — SMP(4코어) 로더를 빌드하고 툴체인/카드에 반영
#
#   ./tools/build-smp-loader.sh            # 빌드만 (설치 안 함)
#   ./tools/build-smp-loader.sh --install  # 빌드 + Arduino 패키지에 EDK 설치
#                                          #   + 캐시 삭제 + 스케치 전체 재빌드
#
# ─── 왜 이 스크립트가 필요한가 ────────────────────────────────────────
# SMP 는 커널 빌드 옵션이라 스케치(llext 모듈)에서 켤 수 없습니다. 로더를
# 다시 빌드해야 하고, 그러면 llext-EDK ABI 가 바뀌어 다음이 전부 따라옵니다:
#
#   1. 새 EDK 를 Arduino 코어 패키지의 variants/ 에 덮어쓰기
#   2. 캐시된 코어 아카이브 삭제 (안 하면 옛 구조체 크기가 남아 메모리 파손)
#   3. 모든 스케치 재빌드
#   4. 새 로더(zephyr.bin)를 SD 카드의 Arduino 슬롯에 설치
#
# 순서를 틀리면 증상이 엉뚱하게 나타납니다 (변형 .conf 주석의
# `sys_dlist_remove -> FAR=0` 사례 참고). 그래서 스크립트로 고정합니다.
#
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

ZEPHYR_WS="${PIZZA_ZEPHYR_WS:-$HOME/zephyrproject}"
ACZ="${PIZZA_ACZ:-$HOME/github/SS/ArduinoCore-zephyr}"
VENV="${PIZZA_ZEPHYR_VENV:-$HOME/.zephyr-venv}"
SDK="${PIZZA_ZEPHYR_SDK:-$HOME/zephyr-sdk-1.0.1}"
VARIANT="rpi_zero_2w_bcm2710"
CORE_PKG="$HOME/Library/Arduino15/packages/pizza/hardware/zephyr/0.6.4"
ARDUINO_CACHE="$HOME/Library/Caches/arduino"

DO_INSTALL=0
[[ "${1:-}" == "--install" ]] && DO_INSTALL=1

step() { printf '\n%s──%s %s\n' "$C_BLU" "$C_OFF" "$*"; }

# ── 0. 전제 조건 ─────────────────────────────────────────────────────
step "0. 전제 조건 확인"

[[ -d "$ZEPHYR_WS/zephyr" ]] || die "Zephyr 워크스페이스가 없습니다: $ZEPHYR_WS/zephyr
     west init -m https://github.com/jetpax/zephyr --mr dev $ZEPHYR_WS
     cd $ZEPHYR_WS && west update && west blobs fetch hal_broadcom"

[[ -d "$ACZ" ]] || die "ArduinoCore-zephyr 가 없습니다: $ACZ
     git clone -b pizza https://github.com/jetpax/ArduinoCore-zephyr $ACZ"

CROSS="$SDK/gnu/aarch64-zephyr-elf/bin/aarch64-zephyr-elf-"
[[ -x "${CROSS}gcc" ]] || die "aarch64 툴체인이 없습니다: ${CROSS}gcc"

[[ -f "$VENV/bin/activate" ]] || die "Zephyr venv 가 없습니다: $VENV"

ok "워크스페이스 $ZEPHYR_WS"
ok "로더 소스   $ACZ ($(git -C "$ACZ" rev-parse --short HEAD))"
ok "툴체인      ${CROSS}gcc"

# SMP 설정이 실제로 들어 있는지 — 이게 이 스크립트의 존재 이유
CONF="$ACZ/variants/$VARIANT/$VARIANT.conf"
if grep -q "^CONFIG_SMP=y" "$CONF"; then
  CPUS="$(grep '^CONFIG_MP_MAX_NUM_CPUS=' "$CONF" | cut -d= -f2)"
  ok "SMP 설정 확인: CONFIG_SMP=y, ${CPUS}코어"
else
  die "$CONF 에 CONFIG_SMP=y 가 없습니다. SMP 설정을 먼저 넣으세요."
fi

# ── 1. 로더 + llext-EDK 빌드 ─────────────────────────────────────────
step "1. 로더 + llext-EDK 빌드 (pristine)"

# dist-build.sh 는 ZEPHYR_BASE 를 zephyr-dev 로 가정하지만 이 매니페스트는
# manifest.path 를 zephyr 로 잡습니다. 명시적으로 덮어씁니다.
export ZEPHYR_BASE="$ZEPHYR_WS/zephyr"
export EXTRA_ZEPHYR_MODULES="$ZEPHYR_WS/modules/hal/broadcom;$ACZ"
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export CROSS_COMPILE="$CROSS"

# shellcheck disable=SC1091
source "$VENV/bin/activate"

BUILD_DIR="$ACZ/build/$VARIANT"

info "west build -t llext-edk  (수 분 소요)"
west build -p always -b "rpi_zero_2w/bcm2710" -d "$BUILD_DIR" -s "$ACZ/loader" \
  -t llext-edk -- -DTOOLCHAIN_HAS_GLIBCXX=ON

LOADER_BIN="$BUILD_DIR/zephyr/zephyr.bin"
[[ -f "$LOADER_BIN" ]] || die "로더 바이너리가 생성되지 않았습니다: $LOADER_BIN"
ok "로더: $LOADER_BIN ($(stat -f%z "$LOADER_BIN") bytes)"

# 빌드된 설정에 SMP 가 실제로 반영됐는지 확인 — Kconfig 의존성 때문에
# 요청한 옵션이 조용히 무시될 수 있습니다.
BUILT_CONF="$BUILD_DIR/zephyr/.config"
for opt in CONFIG_SMP=y CONFIG_MP_MAX_NUM_CPUS=4 CONFIG_PM_CPU_OPS_SPINTABLE=y CONFIG_SCHED_CPU_MASK=y; do
  if grep -q "^$opt\$" "$BUILT_CONF"; then
    ok "  $opt"
  else
    warn "  $opt  ← 반영 안 됨! $BUILT_CONF 확인 필요"
  fi
done

if [[ $DO_INSTALL -eq 0 ]]; then
  echo
  ok "빌드 완료. 설치하려면:  $0 --install"
  exit 0
fi

# ── 2. EDK 를 변형 디렉터리로 추출 ───────────────────────────────────
step "2. llext-EDK 추출 → $ACZ/variants/$VARIANT"

VARIANT_DIR="$ACZ/variants/$VARIANT"
( cd "$BUILD_DIR" && rm -rf llext-edk && tar xf zephyr/llext-edk.tar.Z )
rsync -a --delete "$BUILD_DIR/llext-edk" "$VARIANT_DIR/"

# EDK 매크로 헤더의 인라인 주석 제거 (upstream dist-build.sh 와 동일 처리)
perl -i -pe 's/\s*\/\*.*?\*\///gs unless /^\s*#\s*(if|else|elif|endif)/ || (/^\s*\/\*/ && !/\\$/)' \
  $(find "$VARIANT_DIR/llext-edk/include/" -type f)

EDK_ARM64="$VARIANT_DIR/llext-edk/include/zephyr/include/zephyr/arch/arm64"
[[ -d "$EDK_ARM64" ]] && cp "$ZEPHYR_BASE/include/zephyr/arch/arm64/macro.inc" "$EDK_ARM64/macro.inc"
ok "EDK 추출 완료"

# ── 2b. 심볼 테이블 + Arduino 플래그 파일 생성 ───────────────────────
# upstream dist-build.sh 의 [5/6] 단계. 이걸 빠뜨리면 변형 디렉터리에
# cflags.txt / includes.txt / syms-*.ld 가 없어서 컴파일이
# "cannot specify '-o' with '-c' ... with multiple files" 로 죽습니다.
step "2b. syms-*.ld + Arduino 플래그 파일 생성"

( cd "$ACZ" && \
  extra/gen_provides.py "$BUILD_DIR/zephyr/zephyr.elf" -T > "$VARIANT_DIR/tls-syms.S" && \
  extra/gen_provides.py "$BUILD_DIR/zephyr/zephyr.elf" -L > "$VARIANT_DIR/syms-dynamic.ld" && \
  extra/gen_provides.py "$BUILD_DIR/zephyr/zephyr.elf" -LF \
    "+kheap_llext_heap" \
    "+kheap__system_heap" \
    "*sketch_base_addr=_sketch_start" \
    "*sketch_max_size=_sketch_max_size" \
    "*loader_max_size=_loader_max_size" \
    "malloc=__wrap_malloc" \
    "free=__wrap_free" \
    "realloc=__wrap_realloc" \
    "calloc=__wrap_calloc" \
    "random=__wrap_random" > "$VARIANT_DIR/syms-static.ld" && \
  cmake -P extra/gen_arduino_files.cmake "$VARIANT" ) \
  || die "gen_provides / gen_arduino_files 실패"

for f in tls-syms.S syms-dynamic.ld syms-static.ld cflags.txt cxxflags.txt includes.txt machine_flags.txt; do
  if [[ -s "$VARIANT_DIR/$f" ]]; then
    printf "  %s✓%s %-20s %s bytes\n" "$C_GRN" "$C_OFF" "$f" "$(stat -f%z "$VARIANT_DIR/$f")"
  else
    die "$f 가 생성되지 않았습니다 — 이 파일 없이는 스케치가 컴파일되지 않습니다"
  fi
done

# SMP 심볼이 실제로 export 됐는지 확인
for s in k_thread_cpu_pin k_thread_cpu_mask_enable; do
  grep -qw "$s" "$VARIANT_DIR/syms-dynamic.ld" \
    && ok "  export: $s" \
    || warn "  export 누락: $s (스케치에서 쓸 수 없습니다)"
done

# 새 EDK 가 SMP 를 반영하는지 — 스케치가 보게 될 값
NEW_AUTOCONF="$VARIANT_DIR/llext-edk/include/zephyr/include/generated/zephyr/autoconf.h"
grep -E "define CONFIG_(SMP|MP_MAX_NUM_CPUS) " "$NEW_AUTOCONF" | sed 's/^/       /'

# ── 3. Arduino 코어 패키지에 설치 ────────────────────────────────────
step "3. Arduino 코어 패키지 갱신"

[[ -d "$CORE_PKG" ]] || die "코어 패키지를 찾을 수 없습니다: $CORE_PKG"

BACKUP="$CORE_PKG/variants/$VARIANT.pre-smp"
if [[ ! -d "$BACKUP" ]]; then
  cp -R "$CORE_PKG/variants/$VARIANT" "$BACKUP"
  ok "원본 변형 백업: $BACKUP"
else
  info "백업이 이미 있습니다: $BACKUP (덮어쓰지 않음)"
fi

rsync -a --delete --exclude="*.orig" "$VARIANT_DIR/" "$CORE_PKG/variants/$VARIANT/"
ok "변형 디렉터리 갱신 (llext-edk, .conf, .overlay, syms-*.ld)"

# ── 4. 캐시 무효화 ───────────────────────────────────────────────────
step "4. Arduino 빌드 캐시 무효화"

# 이걸 건너뛰면 옛 EDK 로 컴파일된 core.a 가 재사용돼 구조체 크기가
# 어긋납니다 — 변형 .conf 의 CONFIG_POLL 사고와 같은 함정입니다.
rm -rf "$ARDUINO_CACHE/cores" "$ARDUINO_CACHE/sketches"
rm -rf "$PROJECT_ROOT/build"
ok "코어 아카이브 + 스케치 캐시 + 로컬 build/ 삭제"

# ── 5. 스케치 전체 재빌드 ────────────────────────────────────────────
step "5. 스케치 전체 재빌드 (새 EDK 기준)"

FAILED_SKETCHES=()
for d in "$SKETCHBOOK"/*/; do
  n="$(basename "$d")"
  [[ "$n" == "libraries" ]] && continue
  [[ -f "$d/$n.ino" ]] || continue
  if "$PROJECT_ROOT/tools/build.sh" "$n" >/tmp/pizza-smp-$n.log 2>&1; then
    printf "  %s✓%s %-22s %s bytes\n" "$C_GRN" "$C_OFF" "$n" \
      "$(stat -f%z "$PROJECT_ROOT/build/$n/$n.ino.elf" 2>/dev/null || echo '?')"
  else
    printf "  %s✗%s %-22s (로그: /tmp/pizza-smp-%s.log)\n" "$C_RED" "$C_OFF" "$n" "$n"
    FAILED_SKETCHES+=("$n")
  fi
done

# ── 6. 안내 ──────────────────────────────────────────────────────────
step "6. 다음 단계 — 로더를 카드에 설치"

cat <<EOF

새 로더는 아직 카드에 없습니다. USB 케이블을 뽑고 SD 카드를 카드리더에 꽂은 뒤:

    $UPSTREAM_DIR/install-to-sdcard.sh --slot Arduino \\
        $LOADER_BIN

그 다음 스케치도 새 것으로 교체 (구 EDK 로 빌드된 스케치는 ABI 불일치):

    ./tools/sd-install.sh PiZZaHtop

카드를 Pi 에 꽂고 전원을 넣은 뒤 확인:

    ./tools/console.sh
    pizza> smp          <- CONFIG_SMP=y, 4 코어로 나와야 정상
    pizza> htop         <- CPU0..CPU3 가 모두 활동해야 정상
EOF

if [[ ${#FAILED_SKETCHES[@]} -gt 0 ]]; then
  echo
  die "재빌드 실패: ${FAILED_SKETCHES[*]}
     새 EDK 와 맞지 않는 코드가 있습니다. 위 로그를 확인하세요."
fi

echo
ok "SMP 로더 빌드 + 툴체인 반영 완료"
