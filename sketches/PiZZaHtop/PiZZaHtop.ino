/*
 * PiZZaHtop — USB-CDC 콘솔에 셸을 붙이고 htop 스타일 실시간 모니터를 제공
 *
 * 보드   : Raspberry Pi Zero 2 W (BCM2710)
 * FQBN   : pizza:zephyr:pizza
 * 사용   : ./tools/console.sh  로 붙은 뒤 프롬프트에 `htop` 입력
 *
 *   pizza> htop        전체화면 실시간 모니터 (q 로 복귀)
 *   pizza> ps          스레드 목록 1회 출력
 *   pizza> mem         메모리 현황 1회 출력
 *   pizza> help        명령 목록
 *
 * ─── 계측 방식 (이 로더의 제약에서 나온 선택) ─────────────────────────
 * 스케치는 llext 모듈이라 로더가 export 한 심볼만 쓸 수 있습니다.
 * k_thread_foreach / k_thread_runtime_stats_get / mallinfo 전부 못 씁니다
 * (CONFIG_THREAD_MONITOR·THREAD_NAME·INIT_STACKS 미설정, 심볼 미export).
 * micros() 도 z_clock_hw_cycles_per_sec 미export 로 링크되지 않습니다.
 *
 *   시간   : k_uptime_ticks()  — CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000 → 100us
 *   CPU%   : 각 스레드가 '깨어 있던' 시간을 스스로 누적 → 창 대비 비율
 *   스택   : 생성 전에 직접 페인팅 → 낮은 주소부터 살아있는 구간 = 미사용
 *            (CONFIG_HW_STACK_PROTECTION 미설정이라 가드 페이지 없음 → 안전)
 *   힙     : 이분 탐색으로 최대 연속 블록 (할당 즉시 해제 = 비파괴적)
 *
 * 실측 결과 스택 최대 사용량이 544 B 였으므로 4096 은 과하지만, 검증된 값을
 * 유지합니다. htop 의 STACK 열에서 실시간으로 확인할 수 있습니다.
 *
 * ─── loop() 우선순위가 14(최하위)라서 지키는 규칙 ──────────────────────
 * 워커는 반드시 재웁니다. k_yield() 는 같거나 높은 우선순위에게만 양보하므로
 * 우선순위 2~12 워커가 yield 만 하면 우선순위 14 인 loop()(= 이 셸)이
 * 영원히 굶습니다. 주기를 놓쳤을 때도 k_msleep(1) 로 실제로 잡니다.
 *
 * Serial 출력은 loop() 에서만 합니다. 워커가 건드리면 화면이 깨집니다.
 * ──────────────────────────────────────────────────────────────────────
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#define NUM_WORKERS 5
#define STACK_SIZE  4096
#define US_PER_TICK (1000000 / CONFIG_SYS_CLOCK_TICKS_PER_SEC)   /* 100 */

/* ── SMP / 코어 식별 ────────────────────────────────────────────────
 * BCM2710A1 은 Cortex-A53 쿼드코어이고 보드 DTS 에도 cpu@0..cpu@3 가 있습니다.
 * 그런데 이 로더는 CONFIG_SMP 미정의 / CONFIG_MP_MAX_NUM_CPUS=1 로 빌드돼
 * 있어서 Zephyr 는 코어 하나만 씁니다. SMP 는 커널 빌드 옵션이라 스케치
 * (llext 모듈)에서는 켤 수 없습니다 — 로더를 다시 빌드해야 합니다.
 * (`smp` 명령이 현재 상태와 필요한 절차를 출력합니다)
 *
 * 코어 번호를 얻는 방법: arch_curr_cpu() / arch_num_cpus() / _kernel 은
 * 전부 미export 라 쓸 수 없습니다. 대신 MPIDR_EL1 을 인라인 어셈블리로
 * 직접 읽습니다 — CPU 시스템 레지스터라 커널 심볼이 전혀 필요 없고,
 * Zephyr 가 도는 EL1 에서 읽기가 허용됩니다.
 * Aff0(하위 8비트)가 BCM2710 클러스터 내 코어 번호(0..3)입니다.
 *
 * 그래서 이 코드는 지금(1코어)도 정확히 동작하고, SMP 로더로 교체하면
 * 코드 수정 없이 4코어가 그대로 보입니다.
 */
#define HW_CORES 4          /* DTS cpu@0..cpu@3 */

static inline uint32_t cpu_id(void)
{
  uint64_t mpidr;
  __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  return (uint32_t)(mpidr & 0xFFu);          /* Aff0 */
}

/* 코어별 누적 사용 시간. SMP 가 켜지면 여러 코어가 동시에 갱신하므로 원자적
 * 증가가 필요합니다.
 *
 * __atomic_fetch_add 는 쓸 수 없습니다 — GCC 가 libgcc 의 outline atomic
 * 헬퍼(__aarch64_ldadd4_relax)를 부르는데 llext 모듈에는 링크되지 않습니다.
 * 그래서 ARMv8-A 기본 LL/SC(LDXR/STXR)로 직접 구현합니다. LSE 확장도,
 * 외부 심볼도 필요 없습니다. */
static volatile uint32_t core_active_tk[HW_CORES];
static volatile uint32_t core_seen[HW_CORES];      /* 그 코어에서 돈 횟수 */

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

static inline void core_add(uint32_t cpu, uint32_t ticks)
{
  if (cpu >= HW_CORES) {
    return;
  }
  atomic_add_u32(&core_active_tk[cpu], ticks);
  atomic_add_u32(&core_seen[cpu], 1u);
}

#define HTOP_REFRESH_MS 1000
#define HEAP_EVERY_N    4      /* 힙 탐색은 매 프레임 말고 N 프레임마다 */

/* ── ANSI ──────────────────────────────────────────────────────────── */
#define ESC       "\033"
#define CUR_HOME  ESC "[H"
#define CLR_BELOW ESC "[J"
#define CLR_ALL   ESC "[2J" ESC "[H"
#define CUR_HIDE  ESC "[?25l"
#define CUR_SHOW  ESC "[?25h"
/* 대체 화면 버퍼 — 리눅스 htop 이 쓰는 것과 동일 (terminfo smcup/rmcup).
 * 진입 시 별도 화면으로 전환하고, 나갈 때 이전 셸 화면을 그대로 복원한다. */
#define ALT_ON    ESC "[?1049h"
#define ALT_OFF   ESC "[?1049l"
#define C_OFF     ESC "[0m"
#define C_BOLD    ESC "[1m"
#define C_DIM     ESC "[2m"
#define C_RED     ESC "[31m"
#define C_GRN     ESC "[32m"
#define C_YEL     ESC "[33m"
#define C_BLU     ESC "[34m"
#define C_CYN     ESC "[36m"
#define C_REV     ESC "[7m"

/* ── 워커 ──────────────────────────────────────────────────────────── */
struct worker_cfg {
  const char *name;
  int         priority;
  uint32_t    period_ms;
  uint32_t    work_iters;
};

static const struct worker_cfg CFG[NUM_WORKERS] = {
  { "control",   2, 100,  20000 },
  { "sensor",    4,  50,  30000 },
  { "logger",    6, 250,  50000 },
  { "blink",     8, 500,      0 },
  { "cruncher", 12,  20, 200000 },
};

/* 전부 32비트 — CONFIG_MP_MAX_NUM_CPUS=1 이라 정렬된 32비트 접근은 원자적.
 * 여기에 뮤텍스를 걸면 측정 대상인 스케줄링 타이밍을 교란하므로 안 씁니다. */
struct worker_stat {
  volatile uint32_t loops;
  volatile uint32_t active_tk;   /* 깨어 있던 누적 시간 → CPU% */
  volatile uint32_t jit_max_tk;
  volatile uint32_t jit_sum_tk;
  volatile uint32_t jit_n;
  volatile uint32_t last_seen_ms;
  volatile uint32_t started;
  volatile uint32_t last_cpu;    /* 마지막으로 실행된 코어 (MPIDR Aff0) */
};

static struct worker_stat stats[NUM_WORKERS];

static K_THREAD_STACK_ARRAY_DEFINE(worker_stacks, NUM_WORKERS, STACK_SIZE);
static struct k_thread worker_tcb[NUM_WORKERS];
static k_tid_t         worker_tid[NUM_WORKERS];

/* ── 스택 계측: 생성 전에 칠하고, 남은 페인트를 세면 미사용량 ────────── */
#define STACK_PAINT 0xAAAAAAAAu

static void paint_stack(int i)
{
  uint32_t *p     = (uint32_t *)worker_stacks[i];
  size_t    words = K_THREAD_STACK_SIZEOF(worker_stacks[i]) / sizeof(uint32_t);

  for (size_t w = 0; w < words; w++) {
    p[w] = STACK_PAINT;
  }
}

static uint32_t stack_unused(int i)
{
  const uint32_t *p     = (const uint32_t *)worker_stacks[i];
  size_t          words = K_THREAD_STACK_SIZEOF(worker_stacks[i]) / sizeof(uint32_t);
  size_t          w     = 0;

  while (w < words && p[w] == STACK_PAINT) {
    w++;
  }
  return (uint32_t)(w * sizeof(uint32_t));
}

/* ── 힙 계측: 최대 연속 블록 (총 여유량 아님 — 단편화 지표) ─────────── */
static size_t largest_block(void *(*alloc_fn)(size_t), void (*free_fn)(void *), size_t upper)
{
  size_t lo = 0, hi = upper;

  while (lo < hi) {
    size_t mid = lo + (hi - lo + 1) / 2;
    void  *p   = alloc_fn(mid);

    if (p != NULL) {
      free_fn(p);
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return lo;
}

static void *libc_alloc(size_t n) { return malloc(n); }
static void  libc_free(void *p)   { free(p); }
static void *kern_alloc(size_t n) { return k_malloc(n); }
static void  kern_free(void *p)   { k_free(p); }

/* ── 워커 본체 ─────────────────────────────────────────────────────── */
static void burn_cpu(uint32_t iters)
{
  volatile uint32_t sink = 0;
  for (uint32_t i = 0; i < iters; i++) {
    sink += i;
  }
  (void)sink;
}

static void worker_entry(void *p1, void *p2, void *p3)
{
  int idx = (int)(intptr_t)p1;
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  const struct worker_cfg *c = &CFG[idx];
  struct worker_stat      *s = &stats[idx];

  s->started = 1;

  /* 절대시각 기준 — 상대 sleep 은 작업시간이 누적돼 드리프트가 생깁니다 */
  int64_t next_ms = k_uptime_get();

  for (;;) {
    next_ms += c->period_ms;

    int64_t left = next_ms - k_uptime_get();
    if (left > 0) {
      k_msleep((int32_t)left);
    } else {
      next_ms = k_uptime_get();
      k_msleep(1);            /* k_yield() 를 쓰면 loop()(prio 14)가 굶습니다 */
    }

    int64_t woke_tk = k_uptime_ticks();
    int64_t plan_tk = (int64_t)next_ms * CONFIG_SYS_CLOCK_TICKS_PER_SEC / 1000;
    int64_t late_tk = woke_tk - plan_tk;

    if (late_tk > 0) {
      uint32_t l = (uint32_t)late_tk;
      if (l > s->jit_max_tk) {
        s->jit_max_tk = l;
      }
      s->jit_sum_tk += l;
    }
    s->jit_n += 1;

    if (idx == 3) {
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
    burn_cpu(c->work_iters);

    /* 깨어 있던 시간 = CPU 를 실제로 쥐고 있던 시간 (근사) */
    uint32_t active = (uint32_t)(k_uptime_ticks() - woke_tk);
    s->active_tk   += active;

    /* 어느 코어에서 돌았는지 기록. SMP 가 꺼져 있으면 항상 0 이 나오고,
     * SMP 로더로 바꾸면 커널이 분배한 대로 0..3 이 나옵니다. */
    uint32_t cpu = cpu_id();
    s->last_cpu  = cpu;
    core_add(cpu, active);

    s->loops       += 1;
    s->last_seen_ms = (uint32_t)k_uptime_get();
  }
}

/* ── 출력 헬퍼 ─────────────────────────────────────────────────────── */
static void pad(const char *s, int w)
{
  Serial.print(s);
  for (int i = (int)strlen(s); i < w; i++) {
    Serial.print(' ');
  }
}

static void rnum(uint32_t v, int w)
{
  char b[16];
  int  n = 0;

  if (v == 0) {
    b[n++] = '0';
  } else {
    char t[16];
    int  k = 0;
    while (v > 0 && k < (int)sizeof(t)) {
      t[k++] = (char)('0' + (v % 10));
      v /= 10;
    }
    while (k > 0) {
      b[n++] = t[--k];
    }
  }
  b[n] = '\0';
  for (int i = n; i < w; i++) {
    Serial.print(' ');
  }
  Serial.print(b);
}

/* 소수 1자리 (x10 스케일 정수를 받음) — 부동소수 없이 */
static void rdec1(uint32_t x10, int w)
{
  rnum(x10 / 10, w - 2);
  Serial.print('.');
  Serial.print(x10 % 10);
}

/* htop 스타일 막대: 사용률에 따라 초록/노랑/빨강 */
static void bar(uint32_t pct, int width)
{
  if (pct > 100) {
    pct = 100;
  }
  int fill = (int)((pct * width) / 100);

  Serial.print(C_DIM "[" C_OFF);
  Serial.print(pct >= 80 ? C_RED : (pct >= 50 ? C_YEL : C_GRN));
  for (int i = 0; i < fill; i++) {
    Serial.print('|');
  }
  Serial.print(C_OFF C_DIM);
  for (int i = fill; i < width; i++) {
    Serial.print(' ');
  }
  Serial.print("]" C_OFF);
}

/* ── 스냅샷 ────────────────────────────────────────────────────────── */
struct row {
  uint32_t    loops, hz_x10, cpu_x10, late_av_us, late_max_us;
  uint32_t    stk_size, stk_peak;
  uint32_t    cpu;              /* 마지막 실행 코어 */
  const char *state;
  bool        ok;
};

/* 코어별 사용률 (x10 스케일). 창 사이 증분으로 계산. */
static uint32_t prev_core_tk[HW_CORES];
static uint32_t core_pct_x10[HW_CORES];

struct heaps {
  uint32_t libc_arena, libc_free_big, libc_used;
  uint32_t kern_arena, kern_free_big, kern_used;
};

static uint32_t prev_loops[NUM_WORKERS];
static uint32_t prev_active[NUM_WORKERS];
static int64_t  prev_ms;
static struct heaps cached_heaps;
static bool     heaps_valid;

static void snapshot(struct row *r, struct heaps *h, uint32_t *win_ms, bool do_heaps)
{
  int64_t now = k_uptime_get();
  uint32_t w  = (uint32_t)(now - prev_ms);
  prev_ms     = now;
  *win_ms     = w;

  uint32_t win_tk = w * (CONFIG_SYS_CLOCK_TICKS_PER_SEC / 1000);

  for (int i = 0; i < NUM_WORKERS; i++) {
    struct worker_stat *s = &stats[i];

    uint32_t loops = s->loops;
    uint32_t act   = s->active_tk;

    r[i].loops   = loops;
    r[i].hz_x10  = w ? ((loops - prev_loops[i]) * 10000u) / w : 0u;
    r[i].cpu_x10 = win_tk ? ((act - prev_active[i]) * 1000u) / win_tk : 0u;
    prev_loops[i]  = loops;
    prev_active[i] = act;

    uint32_t jn = s->jit_n;
    r[i].late_av_us  = (jn ? (s->jit_sum_tk / jn) : 0u) * US_PER_TICK;
    r[i].late_max_us = s->jit_max_tk * US_PER_TICK;

    r[i].stk_size = (uint32_t)K_THREAD_STACK_SIZEOF(worker_stacks[i]);
    r[i].stk_peak = r[i].stk_size - stack_unused(i);
    r[i].cpu      = s->last_cpu;

    if (!s->started) {
      r[i].state = "NEW";  r[i].ok = false;
    } else if ((uint32_t)now - s->last_seen_ms > CFG[i].period_ms * 3u + 200u) {
      r[i].state = "STALL"; r[i].ok = false;
    } else {
      r[i].state = "OK";   r[i].ok = true;
    }
  }

  /* 코어별 사용률 — 각 코어 버킷의 창 증분 / 창 길이 */
  for (int c = 0; c < HW_CORES; c++) {
    uint32_t tk = core_active_tk[c];
    core_pct_x10[c] = win_tk ? ((tk - prev_core_tk[c]) * 1000u) / win_tk : 0u;
    if (core_pct_x10[c] > 1000u) {
      core_pct_x10[c] = 1000u;
    }
    prev_core_tk[c] = tk;
  }

  if (do_heaps || !heaps_valid) {
    cached_heaps.libc_arena    = CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE;
    cached_heaps.kern_arena    = CONFIG_HEAP_MEM_POOL_SIZE;
    cached_heaps.libc_free_big = (uint32_t)largest_block(libc_alloc, libc_free,
                                                         cached_heaps.libc_arena);
    cached_heaps.kern_free_big = (uint32_t)largest_block(kern_alloc, kern_free,
                                                         cached_heaps.kern_arena);
    cached_heaps.libc_used = cached_heaps.libc_arena - cached_heaps.libc_free_big;
    cached_heaps.kern_used = cached_heaps.kern_arena - cached_heaps.kern_free_big;
    heaps_valid = true;
  }
  *h = cached_heaps;
}

/* ── 렌더링 ────────────────────────────────────────────────────────── */
static void print_uptime(int64_t ms)
{
  uint32_t s = (uint32_t)(ms / 1000);
  rnum(s / 3600, 2);
  Serial.print(':');
  if ((s / 60) % 60 < 10) Serial.print('0');
  Serial.print((s / 60) % 60);
  Serial.print(':');
  if (s % 60 < 10) Serial.print('0');
  Serial.print(s % 60);
}

static void render_threads(const struct row *r)
{
  Serial.print(C_REV C_BOLD);
  Serial.print(" CPU PRIO  NAME       STATE   PERIOD   LOOPS      HZ   CPU%   LATEus  STACK   ");
  Serial.println(C_OFF);

  for (int i = 0; i < NUM_WORKERS; i++) {
    Serial.print(C_CYN);
    rnum(r[i].cpu, 4);                 /* 마지막 실행 코어 */
    Serial.print(C_OFF);
    rnum((uint32_t)CFG[i].priority, 5);
    Serial.print("  ");
    pad(CFG[i].name, 11);
    Serial.print(r[i].ok ? C_GRN : C_RED);
    pad(r[i].state, 8);
    Serial.print(C_OFF);
    rnum(CFG[i].period_ms, 5);
    Serial.print("ms");
    rnum(r[i].loops, 8);
    rdec1(r[i].hz_x10, 8);
    /* CPU% 가 높으면 노랑/빨강 */
    Serial.print(r[i].cpu_x10 >= 500 ? C_RED : (r[i].cpu_x10 >= 200 ? C_YEL : ""));
    rdec1(r[i].cpu_x10, 7);
    Serial.print(C_OFF);
    rnum(r[i].late_max_us, 9);
    rnum(r[i].stk_peak, 7);
    Serial.print('/');
    Serial.print(r[i].stk_size);
    Serial.println("   ");
  }
}

/* 코어별 막대 — SMP 가 꺼져 있으면 CPU0 만 활동하고 나머지는 idle 로 표시.
 * 이게 곧 "SMP 가 꺼져 있다"는 시각적 증거이기도 합니다. */
static void render_cores(void)
{
  for (int c = 0; c < HW_CORES; c++) {
    Serial.print("  CPU");
    Serial.print(c);
    Serial.print(' ');
    bar(core_pct_x10[c] / 10, 30);
    rdec1(core_pct_x10[c], 7);
    Serial.print("%  " C_DIM);

    if (c < CONFIG_MP_MAX_NUM_CPUS) {
      Serial.print(core_seen[c] ? "zephyr, active" : "zephyr, idle");
    } else {
      Serial.print("SMP off - kernel unused");
    }
    Serial.println(C_OFF "   ");
  }
}

static void render_htop(bool full_clear)
{
  static uint32_t frame;
  struct row      r[NUM_WORKERS];
  struct heaps    h;
  uint32_t        win;

  snapshot(r, &h, &win, (frame % HEAP_EVERY_N) == 0);
  frame++;

  Serial.print(full_clear ? CLR_ALL : CUR_HOME);

  /* 헤더 */
  Serial.print(C_BOLD C_CYN "  PiZZa htop " C_OFF C_DIM "— Raspberry Pi Zero 2 W (BCM2710)" C_OFF);
  Serial.print("   up ");
  print_uptime(k_uptime_get());
  Serial.println(C_DIM "    q=quit" C_OFF "   ");

  /* 집계 */
  uint32_t cpu_x10 = 0, stk_sz = 0, stk_pk = 0;
  for (int i = 0; i < NUM_WORKERS; i++) {
    cpu_x10 += r[i].cpu_x10;
    stk_sz  += r[i].stk_size;
    stk_pk  += r[i].stk_peak;
  }
  if (cpu_x10 > 1000) {
    cpu_x10 = 1000;
  }

  render_cores();

  Serial.print("  ALL ");
  bar(cpu_x10 / 10, 30);
  rdec1(cpu_x10, 7);
  Serial.print("%  " C_DIM "kernel ");
  Serial.print(CONFIG_MP_MAX_NUM_CPUS);
  Serial.print("/");
  Serial.print(HW_CORES);
  Serial.print(" cores, ");
  Serial.print(NUM_WORKERS);
  Serial.println(" threads" C_OFF "   ");

  Serial.print("  Stk ");
  bar(stk_pk * 100u / stk_sz, 34);
  rnum(stk_pk * 100u / stk_sz, 6);
  Serial.print("%  " C_DIM);
  Serial.print(stk_pk);
  Serial.print('/');
  Serial.print(stk_sz);
  Serial.println(" B" C_OFF "   ");

  Serial.print("  Mem ");
  bar(h.libc_used * 100u / h.libc_arena, 34);
  rnum(h.libc_used * 100u / h.libc_arena, 6);
  Serial.print("%  " C_DIM);
  Serial.print(h.libc_used);
  Serial.print('/');
  Serial.print(h.libc_arena);
  Serial.println(" B libc" C_OFF "   ");

  Serial.print("  Krn ");
  bar(h.kern_used * 100u / h.kern_arena, 34);
  rnum(h.kern_used * 100u / h.kern_arena, 6);
  Serial.print("%  " C_DIM);
  Serial.print(h.kern_used);
  Serial.print('/');
  Serial.print(h.kern_arena);
  Serial.println(" B k_malloc" C_OFF "   ");

  Serial.println("   ");
  render_threads(r);
  Serial.println("   ");
  Serial.print(C_DIM "  CPU% = 스레드가 깨어 있던 시간 비율. Mem = 최대 연속 블록 기준.");
  Serial.println(C_OFF "   ");
  Serial.print(CLR_BELOW);
}

static void render_ps(void)
{
  struct row   r[NUM_WORKERS];
  struct heaps h;
  uint32_t     win;

  snapshot(r, &h, &win, false);
  Serial.println();
  render_threads(r);
}

static void render_mem(void)
{
  struct row   r[NUM_WORKERS];
  struct heaps h;
  uint32_t     win;

  snapshot(r, &h, &win, true);

  Serial.println();
  Serial.println(C_BOLD "메모리 계층 — Pi Zero 2 W 는 물리 512 MB" C_OFF);
  Serial.println("  Zephyr RAM 영역 (DT zephyr,sram)   256 MB  @ 0x200000");
  Serial.println("  커널 VM 창    (KERNEL_VM_SIZE)      32 MB");
  Serial.println("  llext 힙      (LLEXT_HEAP_SIZE)      1 MB   스케치 적재용");

  Serial.print("  libc malloc arena                 ");
  rnum(h.libc_arena, 8);
  Serial.print(" B  사용 ");
  Serial.println(h.libc_used);

  Serial.print("  kernel k_malloc heap              ");
  rnum(h.kern_arena, 8);
  Serial.print(" B  사용 ");
  Serial.println(h.kern_used);

  uint32_t sz = 0, pk = 0;
  for (int i = 0; i < NUM_WORKERS; i++) {
    sz += r[i].stk_size;
    pk += r[i].stk_peak;
  }
  Serial.print("  워커 스레드 스택 합               ");
  rnum(sz, 8);
  Serial.print(" B  최대사용 ");
  Serial.println(pk);
  Serial.println(C_DIM "  (사용량은 최대 연속 블록 기준 역산 — 총 여유량과 다름)" C_OFF);
}

static void render_smp(void)
{
  Serial.println();
  Serial.println(C_BOLD "SMP 상태 — BCM2710A1 (Cortex-A53 quad-core)" C_OFF);
  Serial.println("---------------------------------------------------");

  Serial.print("  하드웨어 코어 (DTS cpu@0..3)   : ");
  Serial.println(HW_CORES);

  Serial.print("  커널이 쓰는 코어              : ");
  Serial.print(CONFIG_MP_MAX_NUM_CPUS);
  Serial.println(CONFIG_MP_MAX_NUM_CPUS < HW_CORES ? C_RED "   <-- SMP 꺼짐" C_OFF : "");

  Serial.print("  CONFIG_SMP                    : ");
#ifdef CONFIG_SMP
  Serial.println("y");
#else
  Serial.println(C_RED "미정의" C_OFF);
#endif

  Serial.print("  현재 셸이 도는 코어 (MPIDR)   : ");
  Serial.println(cpu_id());

  Serial.println();
  Serial.println("  코어별 워커 실행 횟수:");
  for (int c = 0; c < HW_CORES; c++) {
    Serial.print("    CPU");
    Serial.print(c);
    Serial.print(" : ");
    rnum(core_seen[c], 10);
    Serial.println(core_seen[c] ? "" : "   (사용 안 됨)");
  }

  if (CONFIG_MP_MAX_NUM_CPUS >= HW_CORES) {
    return;
  }

  Serial.println();
  Serial.println(C_YEL "  SMP 를 켜려면 로더를 다시 빌드해야 합니다." C_OFF);
  Serial.println("  스케치는 llext 모듈이라 커널 빌드 옵션을 바꿀 수 없습니다.");
  Serial.println("  절차는 호스트의 docs/smp.md 참고.");
}

static void render_help(void)
{
  Serial.println();
  Serial.println(C_BOLD "명령" C_OFF);
  Serial.println("  htop      실시간 모니터 (q 로 복귀)");
  Serial.println("  ps        스레드 목록 1회");
  Serial.println("  mem       메모리 현황 1회");
  Serial.println("  smp       SMP/코어 상태 진단");
  Serial.println("  reset     통계 초기화 (지연 최대값 등)");
  Serial.println("  clear     화면 지우기");
  Serial.println("  help      이 목록");
}

/* ── 셸 ────────────────────────────────────────────────────────────── */
static char line[48];
static int  line_len;
static bool in_htop;
static int64_t next_frame_ms;

static void prompt(void)
{
  Serial.print(C_BOLD C_GRN "pizza> " C_OFF);
}

static void reset_stats(void)
{
  for (int i = 0; i < NUM_WORKERS; i++) {
    stats[i].jit_max_tk = 0;
    stats[i].jit_sum_tk = 0;
    stats[i].jit_n      = 0;
  }
}

static void run_command(const char *cmd)
{
  if (cmd[0] == '\0') {
    return;
  }
  if (!strcmp(cmd, "htop") || !strcmp(cmd, "top")) {
    in_htop       = true;
    next_frame_ms = 0;                 /* 즉시 첫 프레임 */
    Serial.print(ALT_ON CUR_HIDE);     /* 대체 화면으로 전환 (htop 방식) */
    render_htop(true);
    return;
  }
  if (!strcmp(cmd, "ps"))    { render_ps();   return; }
  if (!strcmp(cmd, "smp"))   { render_smp();  return; }
  if (!strcmp(cmd, "mem"))   { render_mem();  return; }
  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) { render_help(); return; }
  if (!strcmp(cmd, "clear")) { Serial.print(CLR_ALL); return; }
  if (!strcmp(cmd, "reset")) { reset_stats(); Serial.println("통계 초기화"); return; }

  Serial.print(C_RED "알 수 없는 명령: " C_OFF);
  Serial.println(cmd);
  Serial.println("help 를 입력하세요");
}

static void serve_shell(void)
{
  while (Serial.available() > 0) {
    int c = Serial.read();

    if (in_htop) {
      if (c == 'q' || c == 'Q' || c == 0x03 /* Ctrl-C */ || c == 0x1b /* ESC */) {
        in_htop = false;
        /* 대체 화면에서 복귀 — 이전 셸 화면(입력 이력 포함)이 그대로 돌아온다 */
        Serial.print(CUR_SHOW ALT_OFF);
        prompt();
      }
      continue;                        /* htop 중에는 다른 키 무시 */
    }

    if (c == '\r' || c == '\n') {
      Serial.println();
      line[line_len] = '\0';
      run_command(line);
      line_len = 0;
      if (!in_htop) {
        prompt();
      }
    } else if (c == 0x7f || c == 0x08) {          /* backspace */
      if (line_len > 0) {
        line_len--;
        Serial.print("\b \b");
      }
    } else if (c >= 0x20 && c < 0x7f) {
      if (line_len < (int)sizeof(line) - 1) {
        line[line_len++] = (char)c;
        Serial.print((char)c);         /* tio 는 로컬 에코가 없으므로 직접 */
      }
    }
  }
}

/* ── Arduino 진입점 ─────────────────────────────────────────────────── */

static void banner(void)
{
  /* htop 도중 터미널이 죽었다 다시 붙는 경우를 대비해 항상 일반 버퍼로
   * 복귀시키고 커서를 되살린 뒤 시작한다 (이미 일반 버퍼면 무해) */
  Serial.print(ALT_OFF CUR_SHOW CLR_ALL);
  Serial.println(C_BOLD C_CYN "PiZZa 콘솔" C_OFF " — Raspberry Pi Zero 2 W (BCM2710)");
  Serial.print("워커 스레드 ");
  Serial.print(NUM_WORKERS);
  Serial.print("개 기동, loop() 우선순위 ");
  Serial.println(k_thread_priority_get(k_current_get()));
  Serial.println(C_DIM "`htop` 을 입력하세요. `help` 로 명령 목록." C_OFF);
  Serial.println();
  prompt();
}

void setup()
{
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
  }

  pinMode(LED_BUILTIN, OUTPUT);
  prev_ms = k_uptime_get();

  for (int i = 0; i < NUM_WORKERS; i++) {
    paint_stack(i);                    /* 반드시 생성 전에 */
    worker_tid[i] = k_thread_create(&worker_tcb[i],
                                    worker_stacks[i],
                                    K_THREAD_STACK_SIZEOF(worker_stacks[i]),
                                    worker_entry,
                                    (void *)(intptr_t)i, NULL, NULL,
                                    CFG[i].priority, 0, K_NO_WAIT);
  }

  /* 배너는 호스트가 붙어 있을 때만. 없으면 loop() 의 접속 감지가 띄운다.
   * — 호스트 없는 출력 금지 규칙 (아래 주석 참고) */
  if (Serial) {
    banner();
  }
}

void loop()
{
  /* ── 근본 규칙: 호스트(DTR)가 없으면 절대 출력하지 않는다 ────────────
   * Serial 과 업로드 채널은 '같은' CDC ACM 하나를 씁니다. write() 에는
   * DTR 게이트가 없어서(코어 USB.cpp 주석) 아무도 안 듣는데 출력하면
   * CDC TX 백로그가 쌓이고, 그 상태의 로더는 1200-bps 업로드 터치에
   * 응답하지 못합니다 → 업로드 영구 멈춤. ThreadMonitor 가 보드의
   * 업로드 경로를 죽이던 원인이 이것이었습니다.
   *
   * 그래서: 호스트가 떨어지면(tio 종료 등) htop 을 즉시 끄고 침묵하며,
   * 다시 붙으면 배너부터 새로 그립니다. */
  static bool was_connected = false;
  bool        connected     = (bool)Serial;   /* operator bool() == DTR */

  if (connected && !was_connected) {
    in_htop  = false;
    line_len = 0;
    banner();
  } else if (!connected && was_connected) {
    in_htop  = false;                  /* 무인 프레임 출력 방지 — 핵심 */
    line_len = 0;
  }
  was_connected = connected;

  if (connected) {
    serve_shell();

    if (in_htop) {
      int64_t now = k_uptime_get();
      if (now >= next_frame_ms) {
        next_frame_ms = now + HTOP_REFRESH_MS;
        render_htop(false);
      }
    }
  }

  k_msleep(20);                        /* 입력 반응성과 부하의 절충 */
}
