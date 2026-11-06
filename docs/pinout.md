# PiZZa 핀맵 — Raspberry Pi Zero 2 W (BCM2710)

출처: 코어 0.6.3 의 변형 오버레이
`~/Library/Arduino15/packages/pizza/hardware/zephyr/0.6.3/variants/rpi_zero_2w_bcm2710/rpi_zero_2w_bcm2710.overlay`
— 추측이 아니라 실제 디바이스트리에서 읽어온 값입니다.

## 디지털 핀 (Arduino D 번호 ↔ BCM GPIO ↔ 40핀 헤더)

| Arduino | BCM GPIO | 헤더 핀 | 기능 |
|--------:|---------:|--------:|------|
| D0  | GPIO 15 | 10 | UART1 RX (`Serial1`) |
| D1  | GPIO 14 |  8 | UART1 TX (`Serial1`) |
| D2  | GPIO 4  |  7 | |
| D3  | GPIO 17 | 11 | 부팅 시 GND 로 단락 → 부트 메뉴 강제 |
| D4  | GPIO 27 | 13 | |
| D5  | GPIO 22 | 15 | |
| D6  | GPIO 5  | 29 | |
| D7  | GPIO 6  | 31 | |
| D8  | GPIO 23 | 16 | |
| D9  | GPIO 24 | 18 | |
| D10 | GPIO 8  | 24 | SPI0 CE0 |
| D11 | GPIO 10 | 19 | SPI0 MOSI |
| D12 | GPIO 9  | 21 | SPI0 MISO |
| D13 | GPIO 11 | 23 | SPI0 SCLK |
| **D14** | **GPIO 29** | **없음** | **온보드 녹색 ACT LED = `LED_BUILTIN`** |
| D15 | GPIO 12 | 32 | **하드웨어 PWM 채널 0** |
| D16 | GPIO 13 | 33 | **하드웨어 PWM 채널 1** |

D10~D13 이 SPI0 에 정렬돼 있어 기존 SPI 스케치가 그대로 동작합니다.
D0/D1 은 Arduino Uno 관례(D0=RX, D1=TX)를 따릅니다.

## 버스

| 버스 | 객체 | 핀 |
|---|---|---|
| I²C | `Wire` | BSC1 — SDA = GPIO 2 (헤더 3), SCL = GPIO 3 (헤더 5) |
| SPI | `SPI` | SPI0 — MISO GPIO 9, MOSI GPIO 10, SCLK GPIO 11, CE0 GPIO 8 |
| I²S | `I2S` | PCM — BCK GPIO 18(12), LRCK GPIO 19(35), DIN GPIO 20(38), DOUT GPIO 21(40) |

## 시리얼

| 객체 | 실체 | 비고 |
|---|---|---|
| `Serial`  | USB CDC ACM | 업로드 채널과 동일한 `usbmodem…` 포트 |
| `Serial1` | mini-UART, GPIO14(TX)/GPIO15(RX) @ 115200 | Zephyr 콘솔 + 로그 백엔드. 로더 부팅 로그를 보는 용도 |

`Serial2` 이상은 UART 가 추가로 배선되기 전까지 바인딩되지 않습니다.

## PWM — 반드시 알아둘 제약

`analogWrite()` 가 **진짜 PWM 으로 동작하는 핀은 D15, D16 두 개뿐**입니다.
BCM283x PWM 블록이 40핀 헤더에서 닿는 핀이 그 둘밖에 없습니다
(I²S · 아날로그/HDMI 오디오 · UART 콘솔 · ACT LED 와 충돌하지 않는 유일한 쌍).

- 주파수는 490 Hz — Arduino Uno 의 `analogWrite` 주파수와 동일
- **그 외 모든 핀**에서 `analogWrite()` 는 PWM 이 아니라 half-scale 임계값
  기준의 단순 digital HIGH/LOW 로 폴백합니다
- 따라서 **온보드 ACT LED(`LED_BUILTIN`)는 페이드가 불가능**합니다.
  페이드를 보려면 D15 또는 D16 에 외부 LED 를 다세요 (`PiZZaFade` 스케치)

`analogWrite()` 는 최초 호출 시 `device_init()` 을 부르고 해당 채널의 핀만
PWM 으로 다시 mux 합니다 (`zephyr,deferred-init`). 그래서 `digitalWrite()` 로
같은 핀을 GPIO 로 썼다가 `analogWrite()` 로 돌아가도 정상 동작합니다.

## ADC — 없음

BCM2710 에는 온칩 ADC 가 없습니다. **`analogRead()` 는 사용할 수 없습니다.**
오버레이에 `adc-pin-gpios` / `io-channels` 노드 자체가 없습니다.

아날로그 입력이 필요하면 외부 ADC 를 다세요:
- I²C: ADS1115, ADS1015
- SPI: MCP3008, MCP3208

## LED_BUILTIN 이 GPIO 29 인 이유

Pi Zero 2 W 는 원본 Zero W(GPIO 47)가 아니라 **Pi 3 계열의 LED 위치인
GPIO 29** 를 씁니다. 오버레이 주석에 따르면 2026-06-06 에 실측으로 확인됐고,
GPIO 47 은 레지스터 상에서는 토글되지만 보드의 LED 를 켜지 못합니다.

`LED_BUILTIN` 매크로는 `digital-pin-gpios` 배열에서 `builtin-led-gpios` 와
일치하는 항목의 인덱스를 돌려주므로 **`LED_BUILTIN` == `14`** 입니다.

## 저장소

SD 카드는 `/SD:` 에 FAT 로 automount 됩니다 (`zephyr,fstab`).
업로드된 스케치는 `/SD:/sketch.llext` 로 기록되고 다음 부팅에 자동 로드됩니다.
