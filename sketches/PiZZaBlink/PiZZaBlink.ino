/*
 * PiZZaBlink — 온보드 녹색 ACT LED 점멸
 *
 * 보드   : Raspberry Pi Zero 2 W (BCM2710)
 * FQBN   : pizza:zephyr:pizza
 *
 * LED_BUILTIN 은 Zero 2 W 변형(variant)에서 D14 = BCM GPIO 29 로 해석됩니다.
 * (원본 Zero W 는 BCM GPIO 47). 외부 부품이 필요 없어 툴체인·업로드 경로를
 * 검증하는 첫 스케치로 적합합니다.
 *
 * 주의: ACT LED 는 PWM 블록에 연결돼 있지 않아 analogWrite() 로 페이드가
 *       되지 않습니다. 페이드는 D15/D16 을 쓰는 PiZZaFade 를 참고하세요.
 */

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(100);
  digitalWrite(LED_BUILTIN, LOW);
  delay(100);
}
