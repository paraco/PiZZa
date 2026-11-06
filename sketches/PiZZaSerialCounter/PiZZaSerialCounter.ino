/*
 * PiZZaSerialCounter — 1초마다 카운터를 시리얼로 출력
 *
 * 보드   : Raspberry Pi Zero 2 W (BCM2710)
 * FQBN   : pizza:zephyr:pizza
 *
 * 원본에서 고친 것 3가지는 아래 주석 참고.
 */

void setup() {
  // (1) begin() 은 보드레이트 인자가 필수입니다.
  //     이 코어의 SerialUSB_::begin 오버로드는 begin(unsigned long) 과
  //     begin(unsigned long, uint16_t) 둘뿐이라 무인자 호출은 컴파일 에러입니다.
  //     USB CDC 라 값 자체는 무시되지만 문법상 있어야 합니다.
  Serial.begin(115200);

  // (3) USB CDC 는 호스트가 포트를 열기 전에 출력하면 그대로 버려집니다.
  //     이 가드가 없으면 초반 몇 줄이 콘솔에 안 보입니다.
  //     보드 단독 동작(호스트 미연결)을 막지 않도록 타임아웃을 둡니다.
  while (!Serial && millis() < 3000) {
  }
}

void loop() {
  static int cnt = 0;

  // (2) Arduino 의 println 은 printf 가 아닙니다. 이 코어의 Print 클래스에는
  //     printf 가 없고, println(const char*, int) 오버로드도 없어서
  //     println("Test = %d\n", cnt++) 는 컴파일되지 않습니다.
  //     또 println 은 개행을 스스로 붙이므로 "\n" 을 넣으면 빈 줄이 하나 더 생깁니다.
  Serial.print("Test = ");
  Serial.println(cnt++);

  delay(1000);
}

/*
 * printf 형식이 꼭 필요하다면 snprintf 로 버퍼에 찍어 넘기세요:
 *
 *   char buf[32];
 *   snprintf(buf, sizeof(buf), "Test = %d", cnt++);
 *   Serial.println(buf);
 */
