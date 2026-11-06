/*
 * PiZZaThreadTest2 — 스레드 개수만 1 → 5 로 올린 시험
 *
 * 보드   : Raspberry Pi Zero 2 W (BCM2710)
 * FQBN   : pizza:zephyr:pizza
 *
 * ─── 1단계에서 확인된 것 ──────────────────────────────────────────────
 *   ✅ llext 모듈의 K_THREAD_STACK_DEFINE 스택으로 k_thread_create 정상 동작
 *   ✅ 스레드 진입, 주기 실행 모두 오차 없음 (loops == expect, 35초)
 *   📌 loop() 우선순위 = 14 (CONFIG_NUM_PREEMPT_PRIORITIES=15 → 최하위)
 *
 * ─── 이번 단계에서 바꾸는 것: 스레드 개수뿐 ────────────────────────────
 *   스레드 1 → 5
 *   스택   8192 유지  (4096 검증은 다음 단계)
 *   바쁜-대기 없음, digitalWrite 없음  (그것도 다음 단계)
 *
 * ─── loop() 가 최하위(14)라서 지켜야 하는 규칙 ─────────────────────────
 * 워커는 반드시 '재워야' 합니다. k_yield() 는 같거나 높은 우선순위에게만
 * 양보하므로, 우선순위 2~12 인 워커가 yield 만 해서는 우선순위 14 인 loop()
 * 에 영원히 차례가 오지 않습니다. 그래서 주기를 놓쳤을 때도 k_yield() 가
 * 아니라 k_msleep(1) 로 '실제로 자도록' 했습니다. (v1 의 결함)
 * ──────────────────────────────────────────────────────────────────────
 */

#include <zephyr/kernel.h>

#define NUM_WORKERS    5
#define STACK_SIZE     8192
#define SPAWN_DELAY_S  15     /* 안전창 — 이 동안 업로드 가능 */

struct worker_cfg {
  const char *name;
  int         priority;
  uint32_t    period_ms;
};

/* v1 과 같은 우선순위·주기 (전부 loop()=14 보다 높음) */
static const struct worker_cfg CFG[NUM_WORKERS] = {
  { "control",   2, 100 },
  { "sensor",    4,  50 },
  { "logger",    6, 250 },
  { "blink",     8, 500 },
  { "cruncher", 12,  20 },
};

static K_THREAD_STACK_ARRAY_DEFINE(worker_stacks, NUM_WORKERS, STACK_SIZE);
static struct k_thread worker_tcb[NUM_WORKERS];
static k_tid_t         worker_tid[NUM_WORKERS];

static volatile uint32_t loops[NUM_WORKERS];
static volatile uint32_t entered[NUM_WORKERS];
static volatile uint32_t overrun[NUM_WORKERS];   /* 주기를 놓친 횟수 */

static void worker_entry(void *p1, void *p2, void *p3)
{
  int idx = (int)(intptr_t)p1;
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  const struct worker_cfg *c = &CFG[idx];
  entered[idx] = 1;

  int64_t next_ms = k_uptime_get();

  for (;;) {
    next_ms += c->period_ms;

    int64_t left = next_ms - k_uptime_get();
    if (left > 0) {
      k_msleep((int32_t)left);
    } else {
      /* 주기를 놓쳤다. k_yield() 를 쓰면 loop()(prio 14)가 굶으므로
       * 반드시 '자야' 한다. 1ms 라도 자면 전 우선순위가 돌아간다. */
      overrun[idx]++;
      next_ms = k_uptime_get();
      k_msleep(1);
    }

    loops[idx]++;
  }
}

static bool spawned;

void setup()
{
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println(" thread test 2 - 5 threads, stack 8192");
  Serial.println("========================================");
  Serial.print("threads      : ");
  Serial.println(NUM_WORKERS);
  Serial.print("stack each   : ");
  Serial.print(STACK_SIZE);
  Serial.print("  total ");
  Serial.println(NUM_WORKERS * STACK_SIZE);
  Serial.print("loop() prio  : ");
  Serial.println(k_thread_priority_get(k_current_get()));
  Serial.print("spawn delay  : ");
  Serial.print(SPAWN_DELAY_S);
  Serial.println("s  <-- 이 동안 업로드 가능 (안전창)");
  Serial.println("----------------------------------------");
}

void loop()
{
  static uint32_t t = 0;

  k_msleep(1000);
  t++;

  if (!spawned) {
    if (t < SPAWN_DELAY_S) {
      Serial.print("t=");
      Serial.print(t);
      Serial.print("s  pre-spawn (안전창 ");
      Serial.print(SPAWN_DELAY_S - t);
      Serial.println("s 남음)");
      return;
    }

    /* 하나씩 만들면서 매번 찍습니다 — 몇 번째에서 죽는지 알기 위해 */
    for (int i = 0; i < NUM_WORKERS; i++) {
      Serial.print(">>> spawn ");
      Serial.print(i);
      Serial.print(" ");
      Serial.print(CFG[i].name);
      Serial.print(" prio ");
      Serial.print(CFG[i].priority);
      Serial.print(" ... ");
      Serial.flush();

      worker_tid[i] = k_thread_create(&worker_tcb[i],
                                      worker_stacks[i],
                                      K_THREAD_STACK_SIZEOF(worker_stacks[i]),
                                      worker_entry,
                                      (void *)(intptr_t)i, NULL, NULL,
                                      CFG[i].priority,
                                      0,
                                      K_NO_WAIT);

      Serial.println(worker_tid[i] ? "tid 유효" : "NULL");
      Serial.flush();
    }
    spawned = true;
    Serial.println(">>> 전부 생성 완료");
    Serial.flush();
    return;
  }

  Serial.print("t=");
  Serial.print(t);
  Serial.print("s ");
  for (int i = 0; i < NUM_WORKERS; i++) {
    Serial.print(" ");
    Serial.print(CFG[i].name[0]);   /* c s l b c */
    Serial.print(entered[i] ? '=' : '?');
    Serial.print(loops[i]);
    if (overrun[i]) {
      Serial.print("!");
      Serial.print(overrun[i]);
    }
  }
  Serial.println();
}
