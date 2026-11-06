# PiZZa Arduino 개발 환경

Raspberry Pi Zero 2 W 에서 **Zephyr RTOS 위의 Arduino 스케치**를 개발하는
로컬 작업 환경입니다. 상위 프로젝트는 [jetpax/PiZZa](https://github.com/jetpax/PiZZa).

| 항목 | 값 |
|---|---|
| 보드 | Raspberry Pi Zero 2 W (BCM2710, Cortex-A53) |
| FQBN | `pizza:zephyr:pizza` |
| 코어 | `pizza:zephyr` 0.6.3 |
| 툴체인 | `aarch64-zephyr-elf` 1.0.1 (Zephyr SDK) |
| 업로더 | `cdc-upload` 1.0.0 — USB CDC 스트리밍 |
| SD 이미지 | `pizza-menu-rpi_zero_2w-v0.7.1.img.xz` |

원본 Zero W(BCM2835)를 쓰려면 FQBN 이 `pizza:zephyr:pizza_zero_w` 입니다.
`tools/common.sh` 의 `FQBN` / `IMAGE_NAME` 두 줄만 바꾸면 됩니다.

## 빠른 시작

```bash
./tools/setup.sh          # 환경 점검 + 부족분 설치 (멱등)
./tools/build.sh          # PiZZaBlink 컴파일 — 보드 없어도 됨
./tools/get-image.sh      # SD 부트 메뉴 이미지 받기
./tools/flash-sdcard.sh   # SD 카드 굽기 (인자 없으면 후보만 나열)
./tools/upload.sh         # 보드에 업로드
./tools/console.sh        # 시리얼 콘솔
```

## 동작 방식

스케치는 **재배치 가능한 ELF** 로 컴파일되어, SD 카드에서 돌고 있는 Zephyr
**로더**에 USB CDC 로 스트리밍됩니다. 로더가 이를 `/SD:/sketch.llext` 로 쓰고
콜드 리셋(약 1초)하면, 재부팅 시 `llext` 모듈로 자동 로드됩니다.

즉 **최초 SD 굽기 한 번 이후로는 카드를 뺄 일이 없습니다.** 업로드는 버튼
하나입니다.

## 폴더 구조

```
PiZZa/
├── README.md              이 문서
├── arduino-cli.yaml       프로젝트 전용 CLI 설정
├── sketches/              스케치북 (arduino-cli 의 directories.user)
│   ├── PiZZaBlink/          온보드 ACT LED 점멸 — 첫 검증용
│   ├── PiZZaHelloSerial/    USB-CDC 시리얼 에코 + uptime
│   ├── PiZZaFade/           D15 하드웨어 PWM 페이드
│   ├── PiZZaWiFiConnect/    CYW43439 Wi-Fi 접속
│   ├── PiZZaThreadMonitor/  Zephyr 스레드 5개 + 스케줄링·메모리 주기 리포트
│   └── PiZZaHtop/           콘솔 셸 + htop 실시간 모니터 (코어별 CPU 포함)
├── tools/                 실행 스크립트 (아래 참고)
├── docs/
│   ├── pinout.md            D0~D16 ↔ BCM GPIO ↔ 헤더 핀, 버스, PWM/ADC 제약
│   ├── smp.md               SMP 현재 상태(꺼짐)와 활성화 절차·ABI 비용
│   └── troubleshooting.md   증상별 해결
├── images/                SD 이미지 (.gitignore)
├── build/                 컴파일 산출물 (.gitignore)
└── upstream/PiZZa/        jetpax/PiZZa 참조 클론 (.gitignore, setup.sh 가 복원)
```

### 설정 분리 방식

이미 설치된 `~/Library/Arduino15` 의 코어·툴체인(약 200 MB)은 **그대로 공유**하고,
`arduino-cli.yaml` 로 **스케치북 경로만** 이 프로젝트로 분리했습니다.
재다운로드가 없고, 기존 esp32 / teensy / rp2040 환경도 건드리지 않습니다.

모든 스크립트가 `--config-file ./arduino-cli.yaml` 을 자동으로 붙이므로,
직접 `arduino-cli` 를 부를 때는 이렇게 하세요:

```bash
arduino-cli --config-file ./arduino-cli.yaml compile --fqbn pizza:zephyr:pizza sketches/PiZZaBlink
```

## 스크립트

| 스크립트 | 하는 일 |
|---|---|
| `setup.sh` | 호스트·도구·인덱스·코어·클론·시크릿·이미지·포트를 점검하고 부족분 설치. `--check` 는 점검만 |
| `build.sh` | 스케치 컴파일 → `build/<이름>/<이름>.ino.elf`. `--clean` 지원 |
| `upload.sh` | 빌드 후 USB-CDC 업로드. 포트 자동 탐지, `--no-build` 지원 |
| `console.sh` | `tio` 시리얼 콘솔 (없으면 `screen`). `--uart <포트>` 로 mini-UART 부팅 로그 |
| `get-image.sh` | 릴리스 SD 이미지 다운로드 + `xz -t` 무결성 검사 |
| `flash-sdcard.sh` | upstream `flash-sdcard.sh` 얇은 래퍼 — 안전장치는 upstream 소유 |
| `sd-install.sh` | **USB 우회 탈출구** — 카드리더로 `sketch.llext` 직접 교체. 스케치가 보드를 죽여 업로드가 막혔을 때 |

공통 변수(FQBN, 보드명, 이미지 태그, 보드 인덱스 URL)와 포트 탐지·IDE 감지
헬퍼는 `tools/common.sh` 한 곳에 있습니다.

환경변수:
- `PIZZA_PORT` — 포트를 직접 지정 (USB-CDC 장치가 여럿일 때)

## SD 카드 굽기

**이 작업은 대상 디스크를 통째로 덮어씁니다.**

가장 안전한 방법은 [Raspberry Pi Imager](https://www.raspberrypi.com/software/)
입니다 — *Choose OS → Use custom* 에서 `images/*.img.xz` 를 그대로 고르면 됩니다
(압축 해제 불필요).

CLI 를 쓰려면 `./tools/flash-sdcard.sh` 를 인자 없이 먼저 돌려 후보 디스크를
확인하고, 그 다음 식별자를 넘기세요. upstream 스크립트가 다음을 **모두**
통과해야만 씁니다: 파티션 아닌 전체 디스크 / external / 루트 디스크 아님 /
크기 상한(기본 128 GB) 이하 / 식별자 재입력. 쓴 뒤 되읽어 SHA-256 을 비교하고
eject 합니다.

굽고 나면 카드를 넣고 전원을 켜세요. 부트 메뉴가 뜨는데 **기본값이 RetroPiZZa
이고 타임아웃이 5초**입니다 — 5초 안에 **Arduino** 를 고르지 않으면 RetroPiZZa 로
넘어갑니다.

놓쳤어도 전원을 뽑을 필요 없습니다. `./tools/console.sh` 로 붙어 `boot Arduino`
를 치면 고정되고 즉시 재부팅합니다. 제대로 전환됐는지는 USB 열거 이름으로 확인:

```bash
ioreg -p IOUSB -w0 | grep -iE "PiZZa|BCM2710"
```

`PiZZa (Raspberry Pi Zero 2 W)` 로 나오면 Arduino 로더입니다. 잘못된 상태의
증상과 해독법은 [docs/troubleshooting.md](docs/troubleshooting.md) 첫 항목에
정리해 뒀습니다.

한 번 고른 항목은 기억되므로 이후에는 바로 로더로 부팅합니다.
메뉴로 돌아가려면 전원 인가 시 **GPIO 17(헤더 11번) 과 GND 를 단락**하세요
(`config.txt` 의 `gpio=17=ip,pu` 로 풀업돼 있습니다).

## 알아둘 하드웨어 제약

- **`analogRead()` 없음** — BCM2710 에 온칩 ADC 자체가 없습니다.
  외부 ADS1115(I²C) 또는 MCP3008(SPI)을 쓰세요.
- **하드웨어 PWM 은 D15(GPIO 12, 헤더 32) / D16(GPIO 13, 헤더 33) 뿐** —
  그 외 핀의 `analogWrite()` 는 half-scale 임계 HIGH/LOW 로 폴백합니다.
- **`LED_BUILTIN` 은 페이드 불가** — ACT LED(GPIO 29)가 PWM 블록에 없습니다.
- **Intel Mac 미지원** — Zephyr SDK 1.0.1 에 Intel-Mac 툴체인이 없습니다.
- **업로드 전 Arduino IDE 를 ⌘Q 로 종료** — IDE 의 Serial Monitor 가 포트를
  선점하며 죽여도 되살아납니다.

전체 핀맵은 [docs/pinout.md](docs/pinout.md), 증상별 해결은
[docs/troubleshooting.md](docs/troubleshooting.md).

## Wi-Fi 스케치 준비

```bash
cp sketches/PiZZaWiFiConnect/arduino_secrets.h.example \
   sketches/PiZZaWiFiConnect/arduino_secrets.h
# SECRET_SSID / SECRET_PASS 를 실제 값으로 채우기
```

`arduino_secrets.h` 는 `.gitignore` 에 있어 커밋되지 않습니다.
CYW43439 는 **2.4 GHz 전용**입니다.

## 범위 밖 (필요해지면 추가)

이 환경은 **Arduino 스케치 개발 전용**입니다. PiZZa 자체(로더, PizzaShell,
RetroPiZZa)를 소스에서 빌드하려면 별도로 Zephyr west 워크스페이스가 필요합니다:

```bash
west init -m https://github.com/jetpax/zephyr --mr dev zephyrproject
cd zephyrproject && west update && west blobs fetch hal_broadcom
```

디스크 10 GB 이상, `west update` 에 30~60분이 걸립니다.
호스트에는 이미 `west 1.5.0` 과 `~/zephyr-sdk-1.0.1` 이 있습니다.

## 참고

- [jetpax/PiZZa](https://github.com/jetpax/PiZZa) — 상위 프로젝트
- [apps/Arduino/README.md](upstream/PiZZa/apps/Arduino/README.md) — 원본 Arduino 문서 (클론 후)
- [jetpax/ArduinoCore-zephyr `pizza` 브랜치](https://github.com/jetpax/ArduinoCore-zephyr/tree/pizza) — 코어 소스
- 보드 인덱스 URL:
  `https://raw.githubusercontent.com/jetpax/PiZZa/main/apps/Arduino/package_pizza_index.json`
