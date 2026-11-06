/*
 * PiZZaThreadTest1 — llext 모듈에서 k_thread_create 가 되는지 확인하는 최소 시험
 *
 * 보드   : Raspberry Pi Zero 2 W (BCM2710)
 * FQBN   : pizza:zephyr:pizza
 *
 * ─── 왜 이 스케치가 필요한가 ───────────────────────────────────────────
 * PiZZaThreadMonitor(스레드 5개 + 스택계측 + 힙탐색 + 보율적응)를 올렸더니
 * 보드가 USB 열거만 되고 무응답 상태가 됐습니다. 요소가 너무 많아 어디서
 * 죽는지 알 수 없어서, 한 번에 하나씩만 바꿔가며 범인을 좁힙니다.
 *
 * 이번 단계에서 검증하는 것은 딱 하나:
 *   "llext 모듈 안에 정의한 K_THREAD_STACK_DEFINE 스택으로
 *    k_thread_create 가 정상 동작하는가"
 *
 * 그래서 스레드는 1개, 하는 일은 카운터 증가뿐, GPIO·힙·스택스캔 전부 없음.
 *
 * ─── 안전장치: 스폰을 늦춥니다 ─────────────────────────────────────────
 * setup() 에서 바로 스레드를 만들면, 죽는 순간 USB 가 같이 죽어서 업로드
 * 경로가 막히고 SD 카드를 빼야만 복구됩니다.
 *
 * 그래서 부팅 후 SPAWN_DELAY_S 초 동안은 아무것도 하지 않고 하트비트만
 * 찍습니다. 그 사이에 보드는 살아 있고 업로드도 받습니다. 이 스케치가
 * 보드를 죽이더라도, 다음 부팅 때 그 창 안에서 다른 스케치를 올리면
 * 카드를 뺄 필요가 없습니다.
 *
 *   ./tools/upload.sh <다른스케치>   ← 카운트다운 중에 실행
 * ──────────────────────────────────────────────────────────────────────
 */

#include <zephyr/kernel.h>

#define STACK_SIZE     8192   /* 1단계는 넉넉하게. 통과하면 4096 으로 낮춰 재시험 */
#define SPAWN_DELAY_S  15     /* 이 시간 동안은 안전 — 업로드 가능 */
#define WORKER_PERIOD  100

static K_THREAD_STACK_DEFINE(worker_stack, STACK_SIZE);
static struct k_thread worker_tcb;
static k_tid_t         worker_tid;

static volatile uint32_t worker_loops;
static volatile uint32_t worker_entered;

static void worker_entry(void *p1, void *p2, void *p3)
{
  ARG_UNUSED(p1);
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  worker_entered = 1;

  for (;;) {
    worker_loops++;
    k_msleep(WORKER_PERIOD);
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
  Serial.println(" thread test 1 - minimal k_thread_create");
  Serial.println("========================================");
  Serial.print("stack size   : ");
  Serial.println(STACK_SIZE);
  Serial.print("threads      : 1");
  Serial.println();
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

    /* 여기가 위험 구간. 이 다음 줄이 안 찍히면 k_thread_create 에서 죽은 것. */
    Serial.println(">>> k_thread_create 호출 직전");
    Serial.flush();

    worker_tid = k_thread_create(&worker_tcb,
                                 worker_stack,
                                 K_THREAD_STACK_SIZEOF(worker_stack),
                                 worker_entry,
                                 NULL, NULL, NULL,
                                 5,            /* 우선순위 */
                                 0,
                                 K_NO_WAIT);
    spawned = true;

    Serial.print(">>> k_thread_create 반환: ");
    Serial.println(worker_tid ? "tid 유효" : "NULL");
    Serial.flush();
    return;
  }

  /* 스폰 이후 — 스레드가 실제로 돌고 있는지 */
  Serial.print("t=");
  Serial.print(t);
  Serial.print("s  post-spawn  entered=");
  Serial.print(worker_entered);
  Serial.print("  loops=");
  Serial.print(worker_loops);
  Serial.print("  expect~");
  Serial.println((t - SPAWN_DELAY_S) * (1000 / WORKER_PERIOD));
}
