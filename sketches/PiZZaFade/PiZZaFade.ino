/*
 * PiZZaFade — 하드웨어 PWM 페이드
 *
 * 보드   : Raspberry Pi Zero 2 W (BCM2710)
 * FQBN   : pizza:zephyr:pizza
 *
 * BCM283x 의 PWM 블록이 40핀 헤더에서 닿는 핀은 딱 두 개입니다:
 *   D15 = BCM GPIO 12 (헤더 32번 핀)
 *   D16 = BCM GPIO 13 (헤더 33번 핀)
 * 그 외 핀에서 analogWrite() 를 호출하면 PWM 이 아니라 half-scale 기준의
 * 단순 digital HIGH/LOW 로 폴백합니다. 그래서 온보드 ACT LED 는 페이드 불가.
 *
 * 배선: D15(헤더 32번) ─ 220Ω ─ LED(+) … LED(−) ─ GND(헤더 34번)
 */

const int PWM_PIN = 15;  // BCM GPIO 12 / 헤더 32번 핀

int brightness = 0;
int step = 5;

void setup() {
  pinMode(PWM_PIN, OUTPUT);
}

void loop() {
  analogWrite(PWM_PIN, brightness);

  brightness += step;
  if (brightness <= 0 || brightness >= 255) {
    step = -step;
  }

  delay(20);
}
