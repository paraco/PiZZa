# PiZZa 문제 해결

## 업로드 / 시리얼

### 【근본 원인】`1200-bps touch` 에서 영구 멈춤 — 케이블을 다시 꽂아도 반복

**증상** — IDE 를 껐고, 좀비 프로세스도 없고, 케이블을 다시 꽂았는데도
업로드가 매번 `cdc-upload: 1200-bps touch on ...` 에서 멈춘다.

**근본 원인 — 지금 보드에서 도는 스케치가 "아무도 안 듣는데" 계속 출력하고
있기 때문입니다.** 연쇄는 이렇습니다:

1. `Serial` 과 업로드 채널은 **같은 CDC ACM 하나**를 씁니다
   (변형 overlay: "carries both the sketch's serial I/O and the upload channel")
2. 코어의 `write()` 에는 DTR 게이트가 없습니다 (`USB.cpp` 주석: 호스트가 안
   들으면 "TX FIFO buffers, eventual NAK" — 즉 드라이버에 쌓입니다)
3. 터미널을 안 붙인 채 스케치가 계속 `Serial.print` 하면 CDC TX 경로에
   백로그가 쌓입니다
4. 그 상태의 로더는 1200-bps 터치에 응답하지 못합니다 → 업로드 영구 멈춤
5. **케이블을 다시 꽂아도 소용없습니다** — 재부팅한 스케치가 몇 초 만에
   다시 백로그를 만들기 때문. 그래서 "매번 반복"됩니다
6. 이때 포트를 여는 프로세스(cdc-upload, tio, 진단 스크립트)는 커널
   드라이버 안에서 `U` 상태로 매달려 kill -9 도 안 듣습니다 — 이 좀비들은
   원인이 아니라 **증상**입니다

실측 대조 (이 프로젝트에서 실제 발생):

| 보드의 스케치 | 무인 출력량 | 업로드 |
|---|---|---|
| HelloSerial | ~20 B / 5초 | ✅ 항상 성공 |
| ThreadTest1 | ~40 B / 1초 | ✅ 항상 성공 |
| ThreadMonitor (수정 전) | **~1150 B / 2초** | ❌ 항상 멈춤 |

**해결 — 스케치 규칙: 호스트(DTR) 없으면 출력 금지.**

```c
if (Serial) {          // operator bool() == DTR
  report();            // 출력은 전부 이 안에서만
}
```

htop 처럼 연속 화면을 그리는 스케치는 **호스트가 떨어지는 순간**(tio 종료)
그리기를 멈추는 전환 감지까지 필요합니다 — PiZZaHtop 의 `was_connected`
패턴 참고. 수정된 ThreadMonitor / Htop 은 이 규칙이 적용돼 있습니다.

**당장 복구 (보드에 수정 전 스케치가 도는 경우)** — USB 로는 못 들어가므로
SD 카드로 우회합니다:

1. 케이블 뽑기 (매달린 프로세스도 이때 전부 정리됨)
2. 카드를 카드리더에 꽂고: `./tools/sd-install.sh <수정된 스케치>`
3. 카드를 Pi 에 꽂고 전원 인가

**검증** — 수정된 스케치가 돌고 있으면, 터미널을 안 붙인 채로도
`./tools/upload.sh` 가 정상 동작해야 합니다. 그게 이 수정의 판정 기준입니다.

### 업로드가 `device: ...PZUPELF...[7D...` 같은 쓰레기 문자열로 실패

**증상** — 전송까지는 되는데 응답 단계에서 죽습니다:

```
cdc-upload: sent 20176 bytes; waiting for device...
cdc-upload: device: PPZUPELF[7DZPZUPELF[7DUPZUPELF[7D ... HHPE@PZUPELF[18D
Failed uploading: uploading error: exit status 1
```

**원인 — 보드가 Arduino 로더가 아니라 Zephyr 셸로 부팅했습니다.**
`[7D` `[18D` `[J` 는 ANSI 커서 이동 이스케이프입니다. 업로더가 보낸 매직
문자열 `PZUPELF` 를 셸이 **명령어 입력으로 받아** 한 글자씩 에코하며 프롬프트를
다시 그리고 있는 겁니다. 포트에 직접 붙어 보면 정체가 드러납니다:

```
HPE@HPE@PZUPELFPZUPELF: command not found
uart:~$
```

Arduino IDE 나 케이블 문제가 **아닙니다**. 부트 메뉴에서 5초 안에 Arduino 를
고르지 못해 기본값(RetroPiZZa)으로 부팅한 것입니다.

**해결** — 셸이 살아 있으므로 전원을 뽑을 필요 없이 그 자리에서 전환합니다:

```
uart:~$ boot list          ← `*` 가 현재 항목
* RetroPiZZa               retropizza.bin
  Arduino                  arduino.bin
  MicroPython              micropython.bin
  PiZZa Shell              pizzashell.bin

uart:~$ boot Arduino       ← 고정 + 즉시 재부팅
```

`./tools/console.sh` 로 붙어서 치면 됩니다. 재부팅에 5초쯤 걸리고 포트가
잠깐 사라졌다 돌아옵니다. 제대로 전환됐는지는 USB 열거 이름으로 확인합니다:

```bash
ioreg -p IOUSB -w0 | grep -iE "PiZZa|BCM2710"
```

| 열거 이름 | 상태 |
|---|---|
| `PiZZa (Raspberry Pi Zero 2 W)` | ✅ Arduino 로더 — 업로드 가능 |
| `BCM2710 Boot` | ❌ 부팅 가능한 SD 를 못 찾아 USB 부트 모드로 폴백 |
| (CDC 포트는 있는데 셸 프롬프트) | ❌ RetroPiZZa / PiZZa Shell 로 부팅됨 |

성공하면 응답이 이렇게 짧습니다:

```
cdc-upload: device: OK
```

### 스케치를 올린 뒤 보드가 무응답 — USB 열거만 되고 아무 반응 없음

**증상**
- `ioreg -p IOUSB -w0 | grep PiZZa` 에는 `PiZZa (Raspberry Pi Zero 2 W)` 로 잡힘
- 그런데 1200-bps 노크에 반응 없음, 콘솔도 무응답
- 결국 USB 에서 장치가 사라지기도 함

**원인** — 방금 올린 **스케치가 보드를 죽이고 있습니다.**
로더는 USB 를 먼저 올린 뒤에 `/SD:/sketch.llext` 를 로드합니다. 그래서
스케치가 죽으면 *열거는 정상인데(제품명까지 맞음) 아무도 USB 를 서비스하지
않는* 상태가 됩니다. 파일이 깨진 게 아니라 내용이 문제입니다.

**파일이 깨진 건지 스케치가 나쁜 건지 구분하는 법** — 카드를 빼서:

```bash
aarch64-zephyr-elf-readelf -h /Volumes/PIZZA/sketch.llext
```

`Start of section headers` + (`Number of section headers` × 64) 가 파일 크기와
같으면 **파일은 멀쩡**합니다. 그러면 전송 사고가 아니라 스케치 자체가 원인입니다.
어떤 스케치인지는 심볼로 확인합니다:

```bash
aarch64-zephyr-elf-readelf -s /Volumes/PIZZA/sketch.llext | grep OBJECT
```

**복구** — USB 업로드는 이미 막혔으므로 SD 카드로 우회합니다:

1. USB 케이블을 뽑고 (전원 차단) 카드를 빼서 카드리더에 꽂기
2. 정상 스케치를 직접 얹기:
   ```bash
   ./tools/build.sh PiZZaHelloSerial
   ./tools/sd-install.sh PiZZaHelloSerial
   ```
   또는 스케치 없이 로더만 띄우려면 `./tools/sd-install.sh --clear`
3. 카드를 Pi 에 꽂고 전원 인가 → 콘솔로 확인

`sd-install.sh` 는 기존 스케치를 지우지 않고 `sketch.llext.prev` 로 보관하며,
aarch64 재배치 ELF 인지 검사하고 복사 후 바이트 비교까지 합니다.

**예방 — 위험한 스케치는 스폰을 늦추세요.** `setup()` 에서 바로 스레드를
만들면 죽는 순간 업로드 경로가 같이 막혀 매번 카드를 빼야 합니다.
`PiZZaThreadTest1` 처럼 부팅 후 10~15초는 하트비트만 찍는 **안전창**을 두면,
보드가 죽더라도 다음 부팅의 그 창 안에서 다른 스케치를 올려 복구할 수 있습니다.

### `Serial port busy` 가 IDE 를 껐는데도 계속 난다 — cdc-upload 좀비

**증상** — IDE 를 종료했는데도 업로드마다 `touch open failed: Serial port busy`.
`lsof` 에는 아무것도 안 잡히는데도 계속 실패합니다.

**확인** — 멈춰버린 업로더 프로세스를 찾습니다:

```bash
ps -o pid,etime,stat,command -p $(pgrep -d, -f cdc-upload)
```

`STAT` 열이 **`U`** (uninterruptible sleep) 면 이 경우입니다:

```
  PID ELAPSED STAT COMMAND
25459   18:01 U    .../cdc-upload /dev/cu.usbmodem2101 ...
26425   06:43 U    .../cdc-upload /dev/cu.usbmodem2101 ...
```

**원인** — 여러 업로더가 동시에 같은 포트를 잡으면 macOS USB CDC 드라이버가
엉킵니다. 전형적인 경로:

1. Arduino IDE 의 Serial Monitor 가 포트를 붙잡고 있는 상태에서
2. `cdc-upload` 가 1200-bps 터치를 걸어 보드가 리셋되고 CDC 장치가 사라짐
3. IDE 가 그 순간 포트를 다시 열려고 달려듦
4. 드라이버가 교착 → `cdc-upload` 가 `U` 상태로 영구 대기
5. 그 프로세스가 포트를 놓지 않으니 이후 모든 업로드가 `Serial port busy`

IDE 에서 업로드를 시도했다가 실패한 뒤 터미널에서 또 시도하면 이 상태에
빠지기 쉽습니다.

**해결 — `kill -9` 는 듣지 않습니다.** `U` 상태는 커널 드라이버 안에서
멈춘 것이라 시그널로 깨울 수 없습니다. 순서대로:

1. Arduino IDE 를 ⌘Q 로 완전히 종료
2. **USB 케이블을 뽑고 3초 뒤 다시 꽂는다** ← 이 단계는 건너뛸 수 없습니다.
   장치가 사라지면 드라이버가 대기를 풀고 좀비들이 정리됩니다
3. `ls /dev/cu.usbmodem*` 로 포트가 새로 잡혔는지 확인
4. 업로드 재시도

**예방** — IDE 와 `./tools/upload.sh` 를 번갈아 쓰지 마세요. 한쪽만 쓰거나,
터미널을 쓸 거면 IDE 를 아예 종료한 상태로 두는 편이 안전합니다.

### `Serial port busy` — 업로드 실패, 또는 콘솔에 아무것도 안 나옴

**Arduino IDE 를 완전히 종료(⌘Q)하세요.**

IDE 의 Serial Monitor 하위 프로세스가 포트가 나타나는 순간 자동으로 붙잡고,
프로세스를 죽여도 **다시 살아납니다**. IDE 를 종료해야만 `arduino-cli`,
`tio`, `screen` 이 포트를 쓸 수 있습니다.

`tools/upload.sh` 와 `tools/console.sh` 는 IDE 프로세스를 감지해 경고합니다.

### USB-CDC 포트(`/dev/cu.usbmodem*`)가 안 보임

순서대로 확인:

1. **안쪽 USB 포트**(`USB` 라벨)에 연결했는지 — 바깥쪽 `PWR` 이 아님
2. **데이터 케이블**인지 — 충전 전용 케이블은 열거되지 않음
3. 전원 인가 후 **약 6초** 대기 (Zephyr 부팅 + USB 열거)
4. SD 카드가 들어 있고 부트 메뉴에서 **Arduino** 를 선택했는지

여전히 안 보이면 mini-UART 로 부팅 로그를 보세요:

```bash
./tools/console.sh --uart /dev/cu.usbserial-XXXX
```

USB-TTL 어댑터를 GPIO 14(TX) / GPIO 15(RX) / GND 에 물리고 115200 8N1.

### 포트가 여러 개라 스크립트가 멈춤

```bash
PIZZA_PORT=/dev/cu.usbmodem1234 ./tools/upload.sh PiZZaBlink
```

## 부팅

### 부트 메뉴가 Arduino 가 아니라 RetroPiZZa 로 넘어감

이미지의 `menu.txt` 기본값이 RetroPiZZa 이고 **타임아웃이 5초**입니다:

```
timeout = 5
default = RetroPiZZa

RetroPiZZa = retropizza.bin
Arduino = arduino.bin
MicroPython = micropython.bin
PiZZa Shell = pizzashell.bin
```

5초 안에 **Arduino** 를 고르세요. 놓쳤으면 PiZZa Shell 로 들어가 `boot Arduino`
를 치면 고정되고 재부팅됩니다. 한 번 고르면 기억됩니다.

### 부트 메뉴로 되돌아가고 싶음

전원 인가 시 **GPIO 17(헤더 11번, = Arduino D3)** 과 **GND** 를 단락시키세요.
한 번 고른 항목은 기억되므로, 이후에는 바로 Arduino 로더로 부팅합니다.

### `Starting kernel...` 에서 멈춤

SD 의 `config.txt` 안 `kernel_address=0x200000` 이 이미지의 링크 주소와
맞는지 확인하세요. 정품 릴리스 이미지를 그대로 구웠다면 발생하지 않습니다.

### 로더만 업데이트하고 싶음 (재이미징 없이)

```bash
./upstream/PiZZa/install-to-sdcard.sh --slot Arduino <loader zephyr.bin>
```

파일 복사라서 권한이 필요 없습니다.

## 빌드

### `라이브러리 WiFi가 zephyr_main, zephyr_contrib 아키텍처에서 실행되며 ... 호환되지 않을 수 있습니다`

**무시해도 되는 경고입니다.** WiFi 라이브러리의 `library.properties` 에 적힌
`architectures` 값이 이 보드의 아키텍처 문자열(`zephyr`)과 문자 그대로
일치하지 않아 arduino-cli 가 내는 경고입니다. 링크와 동작은 정상이며
빌드는 성공합니다.

### `EXTRA[@]: unbound variable` 류의 셸 오류

macOS 기본 `bash` 는 3.2 라 `set -u` 상태에서 빈 배열 확장이 오류입니다.
프로젝트 스크립트는 `${arr[@]+"${arr[@]}"}` 형태로 처리해 두었습니다.
직접 스크립트를 추가할 때 같은 패턴을 쓰세요.

### 코어를 최신으로 올리고 싶음

```bash
arduino-cli core update-index
arduino-cli core upgrade pizza:zephyr
```

보드 인덱스 URL 이 전역 설정에 등록돼 있어야 동작합니다
(`./tools/setup.sh` 가 처리). URL 은 v0.6.0 에서 바뀌었으니, 예전
`/dev/os/Arduino/...` 를 넣어뒀다면 교체하세요.

## Wi-Fi

### 접속 실패

- **2.4 GHz 대역**인지 확인 — CYW43439 는 5 GHz 를 지원하지 않습니다
- `arduino_secrets.h` 의 SSID/비밀번호 확인 (자리표시자가 그대로인지)
- AP 목록 전체를 보려면 부트 메뉴에서 PiZZa Shell 로 들어가 `wifi scan`

### AP 목록을 스케치에서 못 가져옴

이 코어의 `WiFi` API 는 부분 구현입니다. `scanNetworks()` 는 발견 **개수**만
돌려주고, 인덱스로 목록을 꺼내는 `SSID(i)` / `RSSI(i)` 오버로드가 없습니다.
`SSID()` / `RSSI()` 는 **현재 접속된** 네트워크 정보입니다.
목록이 필요하면 PiZZa Shell 의 `wifi scan` 을 쓰세요.

## 하드웨어 제약 (버그가 아님)

| 증상 | 원인 |
|---|---|
| `analogRead()` 없음 | BCM2710 에 온칩 ADC 자체가 없음. 외부 ADS1115/MCP3008 사용 |
| `LED_BUILTIN` 이 페이드 안 됨 | ACT LED(GPIO 29)가 PWM 블록에 연결돼 있지 않음. D15/D16 사용 |
| D15/D16 외 핀에서 `analogWrite()` 가 계단식 | PWM 폴백 — half-scale 임계 HIGH/LOW |

자세한 내용은 [pinout.md](pinout.md).

## 지원되지 않는 호스트

**Intel Mac 은 지원되지 않습니다.** upstream Zephyr SDK 1.0.1 에 Intel-Mac
툴체인이 없습니다. Apple Silicon / Linux / Windows 를 쓰거나, Linux 에서
빌드해 Intel Mac 에서 업로드만 하세요.

## PiZZa Shell 유용한 명령

부트 메뉴에서 **PiZZa Shell** 선택 후:

```
kernel uptime                               시스템 가동 시간
hwinfo devid                                보드 시리얼
sensor get vc-thermal                       다이 온도
wifi scan                                   주변 AP 목록
wifi connect -s <ssid> -p <password> -k 1   WPA2-PSK 접속
boot list                                   부트 메뉴 항목 목록
boot Arduino                                Arduino 로 고정하고 재부팅
```
