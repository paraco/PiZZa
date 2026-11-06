/*
 * PiZZaThreadMonitor — Zephyr 스레드 5개 + 주기적 콘솔 모니터
 *
 * 보드   : Raspberry Pi Zero 2 W (BCM2710)
 * FQBN   : pizza:zephyr:pizza
 * 출력   : USB CDC (`Serial`) — ./tools/console.sh
 *
 * ─── 이 로더에서 확인한 제약 ────────────────────────────────────────────
 * 스케치는 llext 모듈이라 로더가 export 한 커널 심볼만 쓸 수 있습니다.
 *
 *   쓸 수 있음 : k_thread_create, k_msleep, k_uptime_get, k_uptime_ticks,
 *                k_yield, k_thread_priority_get, k_current_get,
 *                k_mutex_*, k_sem_*
 *   못 씀      : k_thread_foreach           (CONFIG_THREAD_MONITOR 미설정)
 *                k_thread_name_set          (CONFIG_THREAD_NAME 미설정)
 *                k_thread_stack_space_get   (CONFIG_INIT_STACKS 미설정)
 *                k_thread_runtime_stats_get (심볼 미export)
 *                mallinfo(), sys_heap_runtime_stats_get()  (미export)
 *                micros()  ← z_clock_hw_cycles_per_sec 미export 로 링크 실패
 *
 * 그래서 시간원은 k_uptime_ticks() 를 씁니다.
 * CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000 → 1 tick = 100us 해상도.
 *
 * ─── 메모리 모니터링 방식 ──────────────────────────────────────────────
 * 스택 : CONFIG_INIT_STACKS 가 꺼져 있어 커널이 스택을 칠해주지 않습니다.
 *        대신 스택 버퍼가 이 모듈 소유이고 CONFIG_HW_STACK_PROTECTION 도
 *        꺼져 있어(가드 페이지 없음) 스레드 생성 '전에' 직접 페인팅할 수
 *        있습니다. aarch64 스택은 위에서 아래로 자라므로, 낮은 주소부터
 *        페인트가 살아있는 구간을 세면 그게 미사용량 = high-water mark.
 *
 * 힙   : mallinfo() 가 없어 사용량을 직접 물어볼 수 없습니다. 대신 이분 탐색으로
 *        '지금 할당 가능한 최대 연속 블록'을 잽니다. 할당 즉시 해제하므로
 *        비파괴적이고, 총 여유량이 아니라 '연속성'을 보여줘서 단편화 관찰에
 *        오히려 더 유용합니다.
 *
 * 락을 쓰지 않는 이유: CONFIG_MP_MAX_NUM_CPUS=1 (단일 코어) + 통계값 전부
 * 32비트 정렬. 단일 코어에서 정렬된 32비트 접근은 원자적이라 값이 찢어지지
 * 않습니다. 뮤텍스를 걸면 측정 대상인 스케줄링 타이밍을 교란하므로 뺐습니다.
 *
 * Serial 출력은 loop() 한 곳에서만 합니다. 워커가 Serial 을 건드리면 출력이
 * 서로 끼어들어 깨집니다.
 * ──────────────────────────────────────────────────────────────────────
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#define NUM_WORKERS 5
#define STACK_SIZE  4096

/* ── 링크 속도 적응 ──────────────────────────────────────────────────
 * `Serial` 은 USB CDC ACM 입니다. 실제 전송은 USB 벌크라서 보율이 처리량을
 * 결정하지 않습니다 — 하지만 호스트가 설정한 line coding 값은 장치가 읽을 수
 * 있고(CONFIG_UART_LINE_CTRL=y), 터미널 프로그램이 그 값으로 동작하므로
 * '호스트가 기대하는 속도'로 취급해 출력량을 맞춰줍니다.
 *
 * 주의: 업로더가 쓰는 1200-bps 는 데이터 속도가 아니라 "업로드 모드로
 * 들어가라"는 노크 신호입니다. 그래서 아래에서 1200 은 링크 속도 후보에서
 * 제외합니다. 바꿀 수 있는 값도 아니고, 바꾸면 로더가 못 알아듣습니다.
 */
#define BAUD_DEFAULT        115200u   /* line coding 을 못 읽을 때 가정값 */
#define BAUD_TOUCH          1200u     /* 업로드 노크 — 링크 속도가 아님 */
#define REPORT_MS_BASE      2000u     /* 기본 리포트 주기 */
#define REPORT_MS_MAX       30000u    /* 아주 느린 링크에서의 상한 */
#define LINK_BUDGET_PCT     30u       /* 링크 대역의 몇 %까지 리포트에 쓸지 */

/* 출력 상세도 — 링크가 감당 못 하면 자동으로 낮춥니다. */
enum verbosity { V_MINIMAL = 0, V_COMPACT = 1, V_FULL = 2 };

/* 각 상세도의 대략적인 출력 바이트 수 (실측 기준 보수적으로 잡음) */
static const uint32_t TIER_BYTES[3] = { 120u, 460u, 1150u };
static const char    *TIER_NAME[3]  = { "minimal", "compact", "full" };

static const struct device *cdc_dev = DEVICE_DT_GET(DT_NODELABEL(board_cdc_acm_uart));

static uint32_t link_baud     = BAUD_DEFAULT;
static bool     baud_detected = false;
static uint32_t report_ms     = REPORT_MS_BASE;
static enum verbosity tier    = V_FULL;

#define US_PER_TICK (1000000 / CONFIG_SYS_CLOCK_TICKS_PER_SEC)  /* = 100 */

/* ── 워커 정의 ───────────────────────────────────────────────────────
 * Zephyr 선점 우선순위는 0 이 가장 높고 숫자가 커질수록 낮습니다.
 * 이 로더는 CONFIG_NUM_PREEMPT_PRIORITIES=15 → 0..14.
 * CONFIG_TIMESLICE_SIZE=20ms 라 같은 우선순위끼리는 20ms 라운드로빈.
 *
 * work_iters 는 sleep 이 아니라 실제로 CPU 를 태우는 고정 반복 횟수입니다.
 * 고정량이므로, 리포트의 '작업max' 가 늘어났다면 그건 일이 많아진 게 아니라
 * 그 스레드가 선점당해 밀린 것 — 우선순위 효과가 그대로 드러납니다.
 */
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
  { "blink",     8, 500,      0 },  // 온보드 ACT LED — 눈으로 보이는 심박
  { "cruncher", 12,  20, 200000 },  // 최저 우선순위 + 최대 작업량
};

/* ── 스레드가 기록하는 통계 (전부 32비트 = 단일 코어에서 원자적) ── */
struct worker_stat {
  volatile uint32_t loops;          // 총 반복 횟수
  volatile uint32_t jit_max_tk;     // 최대 깨어남 지연 (tick)
  volatile uint32_t jit_sum_tk;     // 지연 합
  volatile uint32_t jit_n;          // 합에 더해진 횟수
  volatile uint32_t work_max_tk;    // 고정 작업이 실제로 걸린 최대 시간
  volatile uint32_t last_seen_ms;   // 마지막으로 돈 시각 → 정지 감지
  volatile uint32_t started;        // 진입 확인
};

static struct worker_stat stats[NUM_WORKERS];

static K_THREAD_STACK_ARRAY_DEFINE(worker_stacks, NUM_WORKERS, STACK_SIZE);
static struct k_thread worker_tcb[NUM_WORKERS];
static k_tid_t         worker_tid[NUM_WORKERS];

static uint32_t prev_loops[NUM_WORKERS];   // 실측 주파수 계산용
static int64_t  prev_report_ms;

/* ── 스택 사용량 계측 ────────────────────────────────────────────────
 * 스레드 생성 전에 스택 전체를 이 값으로 칠해두고, 나중에 낮은 주소부터
 * 아직 칠해진 채로 남아있는 구간을 세면 그게 미사용량입니다.
 * (aarch64 스택은 높은 주소 → 낮은 주소로 자랍니다)
 */
#define STACK_PAINT 0xAAAAAAAAu

static void paint_stack(int i)
{
  uint32_t *p     = (uint32_t *)worker_stacks[i];
  size_t    words = K_THREAD_STACK_SIZEOF(worker_stacks[i]) / sizeof(uint32_t);

  for (size_t w = 0; w < words; w++) {
    p[w] = STACK_PAINT;
  }
}

static size_t stack_unused(int i)
{
  const uint32_t *p     = (const uint32_t *)worker_stacks[i];
  size_t          words = K_THREAD_STACK_SIZEOF(worker_stacks[i]) / sizeof(uint32_t);
  size_t          w     = 0;

  while (w < words && p[w] == STACK_PAINT) {
    w++;
  }
  return w * sizeof(uint32_t);
}

/* ── 힙 여유량 계측 ──────────────────────────────────────────────────
 * mallinfo() 가 없으므로 '지금 할당 가능한 최대 연속 블록'을 이분 탐색으로
 * 잽니다. 매 시도마다 즉시 free 하므로 힙 상태를 바꾸지 않습니다.
 * 총 여유량이 아니라 최대 연속 블록이라는 점에 유의 — 이 값이 총량보다
 * 훨씬 작아지면 단편화가 진행된 것입니다.
 */
static size_t largest_block(void *(*alloc_fn)(size_t), void (*free_fn)(void *),
                            size_t upper)
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

static void *libc_alloc(size_t n)  { return malloc(n); }
static void  libc_free(void *p)    { free(p); }
static void *kern_alloc(size_t n)  { return k_malloc(n); }
static void  kern_free(void *p)    { k_free(p); }

/* ── 링크 속도 감지 + 출력량 적응 ───────────────────────────────────── */

/* 호스트가 설정한 CDC line coding 을 읽는다. 못 읽으면 0. */
static uint32_t read_host_baud(void)
{
  uint32_t b = 0;

  if (!device_is_ready(cdc_dev)) {
    return 0;
  }
  if (uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_BAUD_RATE, &b) != 0) {
    return 0;
  }
  /* 1200 은 업로드 노크라 링크 속도가 아니고, 말도 안 되는 값은 무시 */
  if (b == BAUD_TOUCH || b < 300u || b > 12000000u) {
    return 0;
  }
  return b;
}

/*
 * 감지한 보율에 맞춰 (상세도, 주기) 를 고른다.
 *
 *   8N1 기준 1바이트 = 10비트 → 초당 바이트 = baud / 10
 *   그 중 LINK_BUDGET_PCT % 만 리포트에 쓴다 (나머지는 콘솔 응답성용)
 *
 * 예산 안에 들어가는 가장 상세한 티어를 고르고, 최소 티어조차 안 들어가면
 * 들어갈 때까지 주기를 늘린다.
 */
static void adapt_to_link(void)
{
  uint32_t detected = read_host_baud();

  if (detected != 0) {
    link_baud     = detected;
    baud_detected = true;
  } else {
    baud_detected = false;      /* 값을 못 읽으면 직전 값을 유지 */
  }

  uint32_t bytes_per_s = link_baud / 10u;
  report_ms = REPORT_MS_BASE;

  for (;;) {
    uint32_t allowed = (uint32_t)((uint64_t)bytes_per_s * report_ms / 1000u
                                  * LINK_BUDGET_PCT / 100u);
    int t;
    for (t = V_FULL; t >= V_MINIMAL; t--) {
      if (TIER_BYTES[t] <= allowed) {
        break;
      }
    }

    if (t >= V_MINIMAL) {
      tier = (enum verbosity)t;
      return;
    }

    /* 최소 티어도 안 들어간다 — 주기를 늘려 예산을 키운다 */
    if (report_ms >= REPORT_MS_MAX) {
      tier      = V_MINIMAL;
      report_ms = REPORT_MS_MAX;
      return;
    }
    report_ms = (report_ms * 2u > REPORT_MS_MAX) ? REPORT_MS_MAX : report_ms * 2u;
  }
}

/* 고정 반복 횟수만큼 CPU 를 태운다 (sleep 아님). */
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

  /* 절대시각 기준으로 다음 깨어날 시점을 잡습니다.
   * k_msleep(period) 를 그냥 쓰면 작업 시간이 매 주기 누적돼 드리프트가 생깁니다. */
  int64_t next_ms = k_uptime_get();

  for (;;) {
    next_ms += c->period_ms;

    int64_t left = next_ms - k_uptime_get();
    if (left > 0) {
      k_msleep((int32_t)left);
    } else {
      next_ms = k_uptime_get();   /* 이미 늦었다 — 따라잡지 말고 기준 리셋 */
      k_yield();
    }

    /* 계획한 시각 대비 실제로 깨어난 시각의 차이 = 스케줄링 지연 */
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

    int64_t w0 = k_uptime_ticks();
    burn_cpu(c->work_iters);
    uint32_t took = (uint32_t)(k_uptime_ticks() - w0);
    if (took > s->work_max_tk) {
      s->work_max_tk = took;
    }

    s->loops       += 1;
    s->last_seen_ms = (uint32_t)k_uptime_get();
  }
}

/* ── 리포트 ─────────────────────────────────────────────────────────── */

static void pad(const char *s, int width)
{
  Serial.print(s);
  for (int i = (int)strlen(s); i < width; i++) {
    Serial.print(' ');
  }
}

static void rpad_num(uint32_t v, int width)
{
  char buf[16];
  int n = 0;
  if (v == 0) {
    buf[n++] = '0';
  } else {
    char tmp[16];
    int t = 0;
    while (v > 0 && t < (int)sizeof(tmp)) {
      tmp[t++] = '0' + (v % 10);
      v /= 10;
    }
    while (t > 0) {
      buf[n++] = tmp[--t];
    }
  }
  buf[n] = '\0';
  for (int i = n; i < width; i++) {
    Serial.print(' ');
  }
  Serial.print(buf);
}

/* 한 스레드의 파생값 스냅샷 — 세 가지 상세도가 같은 값을 공유합니다. */
struct row {
  uint32_t    loops;
  uint32_t    hz_x10;
  uint32_t    late_av_us;
  uint32_t    late_max_us;
  uint32_t    work_max_us;
  uint32_t    stk_size;
  uint32_t    stk_peak;
  const char *state;
  bool        ok;
};

/* 힙 스냅샷 */
struct heaps {
  uint32_t libc_arena, libc_big, libc_used_pct;
  uint32_t kern_arena, kern_big, kern_used_pct;
};

static void collect(struct row *r, struct heaps *h, uint32_t window_ms, int64_t now)
{
  for (int i = 0; i < NUM_WORKERS; i++) {
    const struct worker_cfg *c = &CFG[i];
    struct worker_stat      *s = &stats[i];

    uint32_t loops = s->loops;                 /* 스냅샷 (32비트 원자적) */
    uint32_t delta = loops - prev_loops[i];
    prev_loops[i]  = loops;

    r[i].loops = loops;
    /* 창 안에서 실제로 몇 Hz 로 돌았나 — 소수 1자리, 부동소수 없이 */
    r[i].hz_x10 = window_ms ? (delta * 10000u) / window_ms : 0u;

    uint32_t jn     = s->jit_n;
    r[i].late_av_us = (jn ? (s->jit_sum_tk / jn) : 0u) * US_PER_TICK;
    r[i].late_max_us = s->jit_max_tk * US_PER_TICK;
    r[i].work_max_us = s->work_max_tk * US_PER_TICK;

    r[i].stk_size = (uint32_t)K_THREAD_STACK_SIZEOF(worker_stacks[i]);
    r[i].stk_peak = r[i].stk_size - (uint32_t)stack_unused(i);

    if (!s->started) {
      r[i].state = "NOTSTARTED";
      r[i].ok    = false;
    } else if ((uint32_t)now - s->last_seen_ms > c->period_ms * 3u + 200u) {
      r[i].state = "STALLED";
      r[i].ok    = false;
    } else {
      r[i].state = "OK";
      r[i].ok    = true;
    }
  }

  h->libc_arena = CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE;
  h->kern_arena = CONFIG_HEAP_MEM_POOL_SIZE;
  h->libc_big   = (uint32_t)largest_block(libc_alloc, libc_free, h->libc_arena);
  h->kern_big   = (uint32_t)largest_block(kern_alloc, kern_free, h->kern_arena);
  h->libc_used_pct = 100u - h->libc_big * 100u / h->libc_arena;
  h->kern_used_pct = 100u - h->kern_big * 100u / h->kern_arena;
}

/* ── 상세도별 렌더러 ─────────────────────────────────────────────────── */

static void render_full(const struct row *r, const struct heaps *h)
{
  Serial.println();
  Serial.println("[scheduling]");
  Serial.println("name        prio  period    loops     Hz  late_avg  late_max  work_max  state");
  Serial.println("-------------------------------------------------------------------------------");

  for (int i = 0; i < NUM_WORKERS; i++) {
    pad(CFG[i].name, 12);
    rpad_num((uint32_t)CFG[i].priority, 4);
    rpad_num(CFG[i].period_ms, 8);
    Serial.print("ms");
    rpad_num(r[i].loops, 9);
    rpad_num(r[i].hz_x10 / 10, 5);
    Serial.print('.');
    Serial.print(r[i].hz_x10 % 10);
    rpad_num(r[i].late_av_us, 8);
    Serial.print("us");
    rpad_num(r[i].late_max_us, 6);
    Serial.print("us");
    rpad_num(r[i].work_max_us, 6);
    Serial.print("us  ");
    Serial.println(r[i].state);
  }

  Serial.println();
  Serial.println("[memory - thread stacks]");
  Serial.println("name         size   peak   free   peak%");
  Serial.println("---------------------------------------");

  uint32_t tot = 0, peak_tot = 0;

  for (int i = 0; i < NUM_WORKERS; i++) {
    tot      += r[i].stk_size;
    peak_tot += r[i].stk_peak;

    pad(CFG[i].name, 12);
    rpad_num(r[i].stk_size, 5);
    rpad_num(r[i].stk_peak, 7);
    rpad_num(r[i].stk_size - r[i].stk_peak, 7);
    rpad_num(r[i].stk_peak * 100u / r[i].stk_size, 7);
    Serial.println("%");
  }

  Serial.print("total        ");
  rpad_num(tot, 4);
  rpad_num(peak_tot, 7);
  rpad_num(tot - peak_tot, 7);
  rpad_num(peak_tot * 100u / tot, 7);
  Serial.println("%");

  Serial.println();
  Serial.println("[memory - heaps]  (largest contiguous block, not total free)");
  Serial.println("heap          arena  largest   used%");
  Serial.println("-------------------------------------");

  Serial.print("libc malloc ");
  rpad_num(h->libc_arena, 7);
  rpad_num(h->libc_big, 8);
  rpad_num(h->libc_used_pct, 7);
  Serial.println("%");

  Serial.print("k_malloc    ");
  rpad_num(h->kern_arena, 7);
  rpad_num(h->kern_big, 8);
  rpad_num(h->kern_used_pct, 7);
  Serial.println("%");
}

static void render_compact(const struct row *r, const struct heaps *h)
{
  for (int i = 0; i < NUM_WORKERS; i++) {
    pad(CFG[i].name, 10);
    rpad_num(r[i].hz_x10 / 10, 4);
    Serial.print('.');
    Serial.print(r[i].hz_x10 % 10);
    Serial.print("Hz late");
    rpad_num(r[i].late_max_us, 7);
    Serial.print("us stk");
    rpad_num(r[i].stk_peak * 100u / r[i].stk_size, 4);
    Serial.print("% ");
    Serial.println(r[i].state);
  }
  Serial.print("heap libc ");
  Serial.print(h->libc_used_pct);
  Serial.print("%  kern ");
  Serial.print(h->kern_used_pct);
  Serial.println("%");
}

static void render_minimal(const struct row *r, const struct heaps *h)
{
  uint32_t ok = 0, peak_tot = 0, tot = 0, late_worst = 0;

  for (int i = 0; i < NUM_WORKERS; i++) {
    if (r[i].ok) {
      ok++;
    }
    tot      += r[i].stk_size;
    peak_tot += r[i].stk_peak;
    if (r[i].late_max_us > late_worst) {
      late_worst = r[i].late_max_us;
    }
  }

  Serial.print("thr ");
  Serial.print(ok);
  Serial.print('/');
  Serial.print(NUM_WORKERS);
  Serial.print(" late");
  Serial.print(late_worst);
  Serial.print("us stk");
  Serial.print(peak_tot * 100u / tot);
  Serial.print("% heap");
  Serial.print(h->libc_used_pct);
  Serial.print('/');
  Serial.print(h->kern_used_pct);
  Serial.println('%');
}

/* ── 리포트 진입점 ──────────────────────────────────────────────────── */

static void report(void)
{
  int64_t  now       = k_uptime_get();
  uint32_t window_ms = (uint32_t)(now - prev_report_ms);
  prev_report_ms     = now;

  struct row   rows[NUM_WORKERS];
  struct heaps hp;

  collect(rows, &hp, window_ms, now);

  Serial.println();
  Serial.print("=== thread monitor === up ");
  Serial.print((uint32_t)(now / 1000));
  Serial.print("s  link ");
  Serial.print(link_baud);
  Serial.print(baud_detected ? "bps(host)" : "bps(assumed)");
  Serial.print("  view ");
  Serial.print(TIER_NAME[tier]);
  Serial.print("  every ");
  Serial.print(report_ms);
  Serial.println("ms");

  switch (tier) {
  case V_FULL:    render_full(rows, &hp);    break;
  case V_COMPACT: render_compact(rows, &hp); break;
  default:        render_minimal(rows, &hp); break;
  }
}

/* ── Arduino 진입점 ─────────────────────────────────────────────────── */

void setup()
{
  /* USB CDC 라 이 값이 실제 전송률을 정하지는 않습니다 — 실제 속도는 USB 벌크.
   * 다만 API 상 인자가 필수이고, 호스트가 line coding 을 안 밀어줄 때의
   * 기본값 역할을 합니다. */
  Serial.begin(BAUD_DEFAULT);
  while (!Serial && millis() < 3000) {
  }

  adapt_to_link();

  pinMode(LED_BUILTIN, OUTPUT);

  Serial.println();
  Serial.println("==================================================");
  Serial.println(" PiZZa - Zephyr 5 threads + monitor");
  Serial.println("==================================================");
  Serial.print("CPUs             : ");
  Serial.println(CONFIG_MP_MAX_NUM_CPUS);
  Serial.print("preempt prio     : 0..");
  Serial.println(CONFIG_NUM_PREEMPT_PRIORITIES - 1);
  Serial.print("timeslice        : ");
  Serial.print(CONFIG_TIMESLICE_SIZE);
  Serial.println("ms");
  Serial.print("tick resolution  : ");
  Serial.print(US_PER_TICK);
  Serial.println("us");
  Serial.print("loop() priority  : ");
  Serial.println(k_thread_priority_get(k_current_get()));
  Serial.println("--------------------------------------------------");

  prev_report_ms = k_uptime_get();

  for (int i = 0; i < NUM_WORKERS; i++) {
    /* 반드시 k_thread_create 전에! 생성 후에 칠하면 커널이 잡아둔
     * 초기 스택 프레임을 뭉개서 스레드가 시작하자마자 죽습니다. */
    paint_stack(i);

    worker_tid[i] = k_thread_create(&worker_tcb[i],
                                    worker_stacks[i],
                                    K_THREAD_STACK_SIZEOF(worker_stacks[i]),
                                    worker_entry,
                                    (void *)(intptr_t)i, NULL, NULL,
                                    CFG[i].priority,
                                    0,            /* 옵션 없음 */
                                    K_NO_WAIT);   /* 즉시 시작 */

    Serial.print("spawn ");
    pad(CFG[i].name, 10);
    Serial.print(" prio ");
    Serial.print(CFG[i].priority);
    Serial.println(worker_tid[i] ? "  OK" : "  FAILED");
  }
}

void loop()
{
  /* 매 주기 호스트의 line coding 을 다시 읽습니다. 터미널을 115200 에서
   * 9600 으로 바꿔 다시 붙어도 다음 리포트부터 알아서 상세도를 낮춥니다. */
  adapt_to_link();

  k_msleep((int32_t)report_ms);

  /* ── 호스트(DTR)가 없으면 절대 출력하지 않는다 ──────────────────────
   * Serial 과 업로드 채널은 '같은' CDC ACM 하나를 씁니다. write() 에는
   * DTR 게이트가 없어서(코어 USB.cpp 주석 참고) 아무도 안 듣는데 계속
   * 출력하면 CDC TX 경로에 백로그가 쌓이고, 그 상태에서는 로더가
   * 1200-bps 업로드 터치에 응답하지 못합니다 → 업로드가 영영 멈춤.
   * 이 게이트가 이 스케치가 보드의 업로드 경로를 죽이던 근본 원인의
   * 수정입니다. */
  if (Serial) {
    report();
  }
}
