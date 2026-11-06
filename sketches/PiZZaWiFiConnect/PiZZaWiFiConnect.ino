/*
 * PiZZaWiFiConnect — CYW43439 Wi-Fi 접속 확인
 *
 * 보드   : Raspberry Pi Zero 2 W (BCM2710)
 * FQBN   : pizza:zephyr:pizza
 *
 * 준비   : arduino_secrets.h.example 를 arduino_secrets.h 로 복사하고
 *          SSID / 비밀번호를 채워 넣으세요.
 *
 * 참고: 이 코어의 WiFi API 는 Arduino WiFi 라이브러리의 부분 구현입니다.
 *       - scanNetworks() 는 발견한 AP '개수'만 돌려주며, 인덱스로 목록을
 *         꺼내는 SSID(i)/RSSI(i) 오버로드는 없습니다.
 *       - SSID()/RSSI() 는 '현재 접속된' 네트워크의 정보를 반환합니다.
 *       AP 목록 전체를 보려면 PiZZa Shell 의 `wifi scan` 을 쓰세요.
 */

#include <ZephyrClient.h>
#include <WiFi.h>

#include "arduino_secrets.h"

const char ssid[] = SECRET_SSID;
const char pass[] = SECRET_PASS;

void printStatus() {
  Serial.print("SSID   : ");
  Serial.println(WiFi.SSID());

  Serial.print("RSSI   : ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");

  Serial.print("IP     : ");
  Serial.println(WiFi.localIP());

  Serial.print("firmware: ");
  Serial.println(WiFi.firmwareVersion());
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
  }

  Serial.println();
  Serial.println("=== PiZZa WiFi Connect ===");

  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Wi-Fi 모듈과 통신할 수 없습니다 (brcmfmac 펌웨어 확인).");
    while (true) {
      delay(1000);
    }
  }

  int8_t found = WiFi.scanNetworks();
  Serial.print("스캔된 AP 수: ");
  Serial.println(found);

  int status = WL_IDLE_STATUS;
  for (int attempt = 1; attempt <= 10 && status != WL_CONNECTED; attempt++) {
    Serial.print("접속 시도 ");
    Serial.print(attempt);
    Serial.print("/10 → ");
    Serial.println(ssid);

    status = WiFi.begin(ssid, pass, ENC_TYPE_WPA2);
    delay(3000);
    status = WiFi.status();
  }

  if (status != WL_CONNECTED) {
    Serial.println("접속 실패. SSID/비밀번호와 2.4GHz 대역 여부를 확인하세요.");
    return;
  }

  Serial.println("접속 성공");
  printStatus();
}

void loop() {
  static unsigned long last = 0;
  if (millis() - last >= 10000) {
    last = millis();
    Serial.print("status = ");
    Serial.print(WiFi.status());
    Serial.print(WiFi.status() == WL_CONNECTED ? "  (WL_CONNECTED)" : "");
    Serial.println();
  }
}
