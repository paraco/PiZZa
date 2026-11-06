# SMP (멀티코어) — 현재 상태와 활성화 절차

## 결론 먼저

**스케치로는 SMP 를 켤 수 없습니다.** 스케치는 llext 모듈이고, SMP 는 커널
빌드 옵션이라 로더(`arduino.bin`)에 이미 구워져 있습니다. 켜려면 로더를
소스에서 다시 빌드해야 하고, **그러면 EDK ABI 가 바뀌어 모든 스케치와
Arduino 코어 패키지를 함께 다시 빌드해야 합니다.**

## 현재 상태 (코어 0.6.4 로더에서 실측)

| 항목 | 값 | 확인 방법 |
|---|---|---|
| 하드웨어 코어 | **4** (Cortex-A53) | DTS `cpu@0` ~ `cpu@3` |
| `CONFIG_SMP` | **미정의** | llext-edk `autoconf.h` |
| `CONFIG_MP_MAX_NUM_CPUS` | **1** | 〃 |
| `arch_curr_cpu` | 미export | `syms-dynamic.ld` |
| `arch_num_cpus` | 미export | 〃 |
| `k_thread_cpu_pin` / `k_thread_cpu_mask_*` | 미export | 〃 |
| `_kernel` | 미export | 〃 |

즉 Zephyr 는 4코어 중 **1개만** 씁니다. 나머지 3개는 스핀테이블에서 대기
상태로 남아 있습니다.

참고: upstream PiZZa README 는 SMP 를 "Spin-table + mailbox IPI, 커널 smp /
smp_stress 테스트 통과(dev 브랜치)" 로 기술합니다. 즉 **PiZZa 전체로는 SMP 가
동작하지만, Arduino 로더 빌드에서는 꺼져 있는** 상태입니다.

## 스케치에서 코어를 관찰하는 법 (지금 가능)

커널 심볼이 전부 막혀 있어도 **MPIDR_EL1 시스템 레지스터**는 읽을 수 있습니다.
Zephyr 는 EL1 에서 돌고, 이 레지스터 읽기는 EL1 에서 허용됩니다.

```c
static inline uint32_t cpu_id(void)
{
  uint64_t mpidr;
  __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  return (uint32_t)(mpidr & 0xFFu);   /* Aff0 = 클러스터 내 코어 번호 */
}
```

외부 심볼이 필요 없으므로 llext 에서 그대로 링크됩니다.
`PiZZaHtop` 이 이 방식으로 코어별 사용률을 표시합니다 — 지금은 CPU0 만
활동하고 CPU1~3 은 `SMP off` 로 나오며, SMP 로더로 바꾸면 **코드 수정 없이**
4코어가 그대로 보입니다.

### 코어별 카운터의 원자적 증가

`__atomic_fetch_add` 는 쓸 수 없습니다 — GCC 가 libgcc 의 outline atomic
헬퍼(`__aarch64_ldadd4_relax`)를 호출하는데 llext 모듈에는 링크되지 않아
`undefined reference` 가 납니다. ARMv8-A 기본 LL/SC 로 직접 구현하세요:

```c
static inline void atomic_add_u32(volatile uint32_t *p, uint32_t v)
{
  uint32_t val, status;
  __asm__ volatile(
    "1: ldxr  %w0, [%2]\n"
    "   add   %w0, %w0, %w3\n"
    "   stxr  %w1, %w0, [%2]\n"
    "   cbnz  %w1, 1b\n"
    : "=&r"(val), "=&r"(status)
    : "r"(p), "r"(v)
    : "memory");
}
```

## SMP 를 실제로 켜는 절차

### 필요한 것

- 디스크 10 GB 이상, `west update` 에 30~60분
- 호스트에 이미 있음: `west 1.5.0`, `~/zephyr-sdk-1.0.1`, cmake/ninja/dtc/gperf

### 1. Zephyr 워크스페이스 구성

```bash
west init -m https://github.com/jetpax/zephyr --mr dev ~/zephyrproject
cd ~/zephyrproject && west update && west blobs fetch hal_broadcom
```

### 2. 로더 소스 클론

```bash
git clone -b pizza https://github.com/jetpax/ArduinoCore-zephyr ~/github/SS/ArduinoCore-zephyr
```

upstream `apps/Arduino/build.sh` 가 이 경로(`$HOME/github/SS/ArduinoCore-zephyr`)를
기대합니다.

### 3. SMP 설정 추가

변형 Kconfig (`.../variants/rpi_zero_2w_bcm2710/rpi_zero_2w_bcm2710.conf` 에
해당하는 로더 쪽 소스) 에 추가:

```
CONFIG_SMP=y
CONFIG_MP_MAX_NUM_CPUS=4
CONFIG_SCHED_CPU_MASK=y          # k_thread_cpu_pin() 을 쓰려면
```

### 4. 빌드

```bash
~/Projects/PiZZa/upstream/PiZZa/apps/Arduino/build.sh
```

또는 직접:

```bash
cd ~/zephyrproject && source ~/.zephyr-venv/bin/activate
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export CROSS_COMPILE=$HOME/zephyr-sdk/aarch64-zephyr-elf/bin/aarch64-zephyr-elf-
west build -p always -b rpi_zero_2w -s ~/github/SS/ArduinoCore-zephyr/loader \
  -- -DTOOLCHAIN_HAS_GLIBCXX=ON
```

### 5. 카드에 설치

```bash
~/Projects/PiZZa/upstream/PiZZa/install-to-sdcard.sh --slot Arduino \
  ~/zephyrproject/build/zephyr/zephyr.bin
```

파일 복사라 권한이 필요 없습니다.

## ⚠ ABI 가 깨집니다 — 이게 진짜 비용입니다

`CONFIG_SMP=y` 는 커널 구조체 레이아웃을 바꿉니다 (`struct k_thread` 에 SMP
필드 추가, spinlock 에 CPU 필드 추가 등). 변형 `.conf` 에 같은 종류의 사고가
두 건 기록돼 있습니다:

- `CONFIG_POLL` 이 `struct k_sem` 을 24→40 바이트로 키움 → "sketches MUST be
  rebuilt against this EDK" + 캐시된 코어 아카이브도 무효화 필요
- `CONFIG_RING_BUFFER_LARGE` 가 `struct ring_buf` 레이아웃 변경 → "EDK ABI
  break forcing every sketch to be rebuilt"

따라서 SMP 로더를 만들면:

1. 새 llext-EDK 가 생성됨 (변형 디렉터리 통째로 교체)
2. **기존 스케치 전부 재빌드 필요** — 안 하면 구조체 크기 불일치로 메모리가
   깨지고, 증상이 엉뚱하게 나타납니다 (`.conf` 주석: `sys_dlist_remove -> FAR=0`)
3. Arduino 코어 캐시 삭제 필요: `rm -rf ~/Library/Caches/arduino/cores`
4. Boards Manager 패키지를 직접 만들어 배포하거나, 로컬 하드웨어 폴더로 설치

즉 "설정 한 줄 추가" 가 아니라 **툴체인 패키지를 직접 관리하는 작업**입니다.

## 대안 — SMP 없이 4코어를 쓰고 싶다면

Arduino 로더 대신 **PiZZa Shell** 로 부팅하면 upstream 의 SMP 지원 빌드를
그대로 쓸 수 있습니다 (`boot "PiZZa Shell"`). 다만 Arduino 스케치는 못 돌리고
Zephyr 셸 환경이 됩니다. 멀티코어 실험이 목적이라면 이쪽이 훨씬 빠릅니다.
