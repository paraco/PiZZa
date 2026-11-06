/*
 * PiZZaHelloSerial — USB-CDC 시리얼 왕복 확인
 *
 * 보드   : Raspberry Pi Zero 2 W (BCM2710)
 * FQBN   : pizza:zephyr:pizza
 *
 * Serial  = USB CDC (업로드에 쓰는 usbmodem… 포트와 동일)
 * Serial1 = mini-UART, GPIO14(TX)/GPIO15(RX) @ 115200 — 로더 부팅 로그 포트
 *
 * 확인 방법:  tools/console.sh  (또는 tio /dev/cu.usbmodem* -b 115200)
 * 주의    :  Arduino IDE 가 실행 중이면 Serial Monitor 가 포트를 선점합니다.
 *            IDE 를 완전히 종료(⌘Q)한 뒤 콘솔을 여세요.
 */

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
    // 호스트가 CDC 포트를 열 때까지 최대 3초 대기
  }
  Serial.println();
  Serial.println("=== PiZZa Hello Serial ===");
  Serial.println("board : Raspberry Pi Zero 2 W (BCM2710)");
  Serial.println("보내는 문자를 그대로 되돌려줍니다. 입력해 보세요.");

  pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    Serial.write(c);
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }

  static unsigned long last = 0;
  if (millis() - last >= 5000) {
    last = millis();
    Serial.print("uptime: ");
    Serial.print(millis() / 1000);
    Serial.println(" s");
  }
}
