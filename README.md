# esp-dongle

*ESP32-S3 USB stick that maps BLE remote buttons to USB keyboard/media keys; config tool ships on the stick's own USB drive.*

ESP32-S3 USB 스틱(`dongle-s3-0.96`)이 BLE 입력 장치(안드로이드TV 리모콘 등)를 받아 USB HID 입력으로 매핑.
스틱의 USB 드라이브(MSC)에 설정용 `DongleConfig.exe`가 들어 있고, 이 exe가 USB 시리얼(CDC)로 키맵을 설정.

```
[BLE 리모콘] --BLE HID--> [ESP32-S3] --USB composite 303A:4007--> PC
                                       ├─ CDC  설정 프로토콜 (아래 명령)
                                       ├─ MSC  FAT 1MB, DongleConfig.exe
                                       └─ HID  키보드(report 1) + 컨슈머/미디어키(report 2)
```

## 디렉터리
| 경로 | 내용 |
|---|---|
| `dev.ps1` | 빌드/플래시/테스트 진입점 (아래 표) |
| `firmware/main/main.c` | 앱 진입점: 저장된 BLE 장치 재연결, scan/pair/forget, K1 → 리포트를 `keymap`과 CDC로 전달 |
| `firmware/main/lcd.c` | 0.96" LCD 상태 화면 (연결 상태, 장치명) |
| `firmware/main/hid_parse.c` | 장치의 Report Map 해석 → 리포트에서 눌린 키(page:usage) 추출. 장치별 레이아웃 무관 |
| `firmware/main/keymap.c` | 키(page:usage) 매핑 + CDC 명령 처리. 둘 다 ESP 의존 없음 → `firmware/test/`에서 PC로 테스트 |
| `firmware/main/usb.c` | USB 디스크립터, HID 출력, CDC 입출력, MSC, `dfu` |
| `firmware/main/keymap_nvs.c` | 키맵 NVS 저장/로드 |
| `firmware/main/esp_hid_gap.*` | IDF 예제에서 복사한 BLE GAP 헬퍼 (스캔 HID 판별 `adv_is_hid()`만 수정) |
| `firmware/fatfs_image/` | USB 드라이브에 들어갈 파일 (빌드 시 exe가 생성됨) |
| `tool/DongleConfig.cs`, `tool/build.ps1` | 스틱에 실리는 설정 exe 소스/빌드 |
| `tool/keycodes.json` → `gen_keycodes.py` | 키 이름 단일 소스 → `firmware/main/keycodes.h`, `tool/KeyCodes.cs` 생성 (직접 수정 금지) |
| `tool/*.py`, `tool/clean-usb.ps1` | 개발 PC용 보조 스크립트 (아래) |

## Quick start
1. 설치: [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/windows-setup.html)
   (`git clone -b v6.1 --recursive https://github.com/espressif/esp-idf` 후 `install.ps1 esp32s3`),
   Python 3.11 + `pip install -r tool/requirements.txt`, (테스트용) LLVM clang.
   ESP-IDF가 `%USERPROFILE%\esp\esp-idf-v6.1`이 아니면 `$env:IDF_PATH` 설정.
2. 스틱을 꽂고 `.\dev.ps1 flash` (첫 플래시가 안 잡히면 K1/BOOT 누른 채 꽂기).
3. 리모콘을 페어링 모드로 두고 **K1을 짧게 누르기** (5초 스캔 → 가장 가까운 HID 장치와 페어링), 또는 USB 드라이브의 `DongleConfig.exe` → 장치 탭 → 스캔 → 페어링.
   페어링된 장치는 저장되어 이후엔 꽂기만 하면 자동 재연결 (끊겨도 계속 재시도). LCD에 상태/장치명 표시.
4. **매핑된 키만 USB로 나감** (매핑 안 된 키, 자이로/마우스 움직임은 전부 무시). exe → 키 매핑 탭 → 버튼 누르기 → 키 선택 → 추가 (자동 저장). 마우스 움직임은 학습에 안 잡힘.
5. 확인: `python tool\usb_monitor.py watch` (Windows가 실제로 받은 키).

> 빌드는 `dev.ps1 build`로. `idf.py build`만 쓰면 exe 없이(경고 표시) 빈 드라이브로 빌드됨.

## 보드 (dongle-s3-0.96)
- ESP32-S3 (QFN56, rev v0.2), **16MB flash**, **PSRAM 8MB 내장** (현재 미사용)
- 네이티브 USB: GPIO19(D-) / GPIO20(D+)
- 버튼 K1 = GPIO0 (BOOT) · UART0 헤더 GPIO43(TX)/GPIO44(RX) = 로그
- 0.96" ST7735S LCD: SCLK 10, MOSI 11, CS 12, DC 13, RST 14, 백라이트 상시 ON
- TF 슬롯: GPIO15~18, 47, 48
- 참고 자료: 판매처 제공 패키지(회로도 `ESP32-S3-0.96LCD.pdf`, Arduino/IDF 예제, 원래 펌웨어 `.bin`) — 원래 펌웨어로 복구할 때 필요

> ESP32-S3는 **BLE만** 지원(BT Classic 없음).

## 개발 환경
| 항목 | 위치 / 버전 |
|---|---|
| ESP-IDF | v6.1, 기본 위치 `%USERPROFILE%\esp\esp-idf-v6.1` (다르면 `IDF_PATH`), 툴체인 `%USERPROFILE%\.espressif` |
| USB 스택 | `espressif/esp_tinyusb` 3.x (컴포넌트 매니저 자동 다운로드) |
| BLE HID host | IDF `esp_hid` (Bluedroid), `examples/bluetooth/esp_hid_host` 기반 |
| 설정 툴 | C# WinForms + Windows 내장 `csc.exe` (.NET Framework 4.x) → 설치 없이 실행되는 ~10KB exe |
| PC 스크립트 | Python 3.11, `pip install -r tool/requirements.txt` (bleak, pyserial) |
| 호스트 테스트 | clang (`C:\Program Files\LLVM`) |

## dev.ps1 — 반복 작업 한 줄로
| 명령 | 하는 일 |
|---|---|
| `.\dev.ps1 build` | 키코드 생성 → exe 빌드 → 펌웨어 빌드 |
| `.\dev.ps1 flash` | build + 플래시. 포트 자동 탐색, 우리 펌웨어가 돌고 있으면 `dfu` 보내서 다운로드 모드로 |
| `.\dev.ps1 monitor` | 동글 CDC 터미널 (UART 어댑터 쓰면 `-Port COMx`) |
| `.\dev.ps1 all` | flash + monitor |
| `.\dev.ps1 test` | `keymap.c` 호스트 유닛 테스트 (ESP 없이) |
| `.\dev.ps1 ports` | 연결된 ESP32-S3 포트 / PID (1001 = JTAG·다운로드, 4007 = 동글 펌웨어) |
| `.\dev.ps1 erase-nvs` | BLE 본딩 + 저장된 키맵만 삭제 |
| `.\dev.ps1 storage` | FAT 파티션(exe)만 다시 쓰기 |
| `.\dev.ps1 coredump` | 플래시에 저장된 마지막 크래시 분석 |
| `.\dev.ps1 dist` | `dist\esp-dongle-*.bin` 단일 바이너리 (0x0에 기록, [esptool-js](https://espressif.github.io/esptool-js/) 가능) |
| `.\dev.ps1 menuconfig` | `idf.py menuconfig` |

`dfu`가 안 먹으면(하드웨어 미검증) K1(BOOT)을 누른 채 꽂으면 다운로드 모드.

## CDC 프로토콜 (줄 단위, DTR 켜야 응답 — USB CDC라 baud는 아무 값이나 무관)
| 명령 | 동작 |
|---|---|
| `help` / `ver` / `keys` | 명령 목록 / 펌웨어 버전 (`firmware/main/version.h`) / 키 이름 전체 |
| `list` | 현재 매핑 출력. 각 줄 형식: `map <i> <page:usage> <출력키> [입력키 이름]` |
| `map <page:usage\|키이름> <key>` | 리모콘 키(HID usage, hex) → 키. 예: `map c:224 AppHome` (뒤로→홈), `map 9:1 Enter` (OK 클릭→Enter), `map AppBack AppHome` |
| `unmap <i>` / `clear` / `save` | 삭제 / 전부 삭제 / NVS에 저장 (재부팅 후 유지) |
| `inject <rid> <hex>` | BLE 리포트를 받은 것처럼 처리 (리플레이/테스트용, 연결된 장치의 Report Map으로 해석) |
| `hidmap` | 연결된 장치의 Report Map 해석 결과: `field <map> rid <id> <page:usage> bit <b> size <s> count <c> array\|var [rel]` |
| `status` | `ok none\|connecting\|connected <addr> <name>` |
| `scan [초]` | `ok` 후 `dev <addr> <addr_type> <rssi> <name>` 줄들, 끝나면 `evt scan end <n>` (재연결 시도 중이면 최대 10초 늦게 시작) |
| `pair <addr> <addr_type>` / `forget` | 그 장치로 교체 / 장치 삭제. 둘 다 기존 본딩을 지움 (장치 1대만 유지) |
| `dfu` | 다운로드 모드로 재부팅 |
키가 눌리고 뗄 때마다 `key <page:usage> 1|0 [이름]` 출력 (마우스 움직임은 안 나옴) → 그 page:usage를 `map`에 쓰면 됨.
키 변화가 있었던 리포트는 원본도 `rpt <rid> <hex>`로 출력 (replay.py 녹화용). 응답은 `ok ...` / `err ...`.
장치 상태가 바뀌면 `evt none|connecting|connected <addr> <name>` (exe가 이걸로 화면 갱신).
매핑된 키만 USB로 나감 (패스스루 없음). 마우스 축과 벤더 page(ff00+)는 키가 아니라 무시.
동시 입력(코드) 지원. 저장 형식이 바뀌어 예전(rid+hex) 매핑은 무시됨.
키 이름: [tool/keycodes.json](tool/keycodes.json) (한영 `HanEng`, 한자 `Hanja`, 안드로이드TV `AppHome`/`AppBack`/`Menu*` 포함).
수정 후 `dev.ps1 build`가 `firmware/main/keycodes.h`, `tool/KeyCodes.cs`를 다시 생성.

## PC 스크립트 (tool/)
```powershell
# BLE: 장치/서비스/디스크립터/Report Map, 버튼 리포트 실시간 + 녹화
python tool\ble_inspect.py scan -t 8 -n remote
python tool\ble_inspect.py inspect AA:BB:CC:DD:EE:FF --pair --listen 30 --record remote.jsonl
python tool\ble_inspect.py selftest                 # Report Map 디코더 자체 테스트

# 녹화/리플레이: 리모콘 없이 매핑 테스트
python tool\replay.py record remote.jsonl          # 동글이 출력하는 rpt 줄 녹화
python tool\replay.py play remote.jsonl --speed 2  # inject로 재생
python tool\replay.py send "map 3 2302 AppHome"    # 명령 1회 (포트 지정: ... send "ver" --port COM7)

# USB 출력 확인: Windows가 실제로 받은 키 (Raw Input, 키보드도 동글 것만 구분)
python tool\usb_monitor.py list
python tool\usb_monitor.py watch

# 디스크립터 바꾼 뒤 장치가 이상하게 잡힐 때: 남은 303A 유령 장치 정리 (관리자)
.\tool\clean-usb.ps1            # 목록만
.\tool\clean-usb.ps1 -Remove
```
- Windows 설정에서 이미 페어링된 리모콘은 Windows가 HID 서비스를 점유해서 `ble_inspect`에 안 보임 → 설정에서 제거 후 `--pair`.
- USB 디스크립터를 바꾸면 `firmware/main/usb.c`의 `USB_BCD`도 올릴 것 (Windows 캐시 회피).

## 외부 툴 (필요할 때 설치)
- Wireshark + USBPcap: `winget install WiresharkFoundation.Wireshark` (설치 중 USBPcap 체크) — HID 패킷 확인
- Microsoft BTVS (Bluetooth Virtual Sniffer, Windows 11): PC 블루투스 HCI를 Wireshark로 — 페어링 디버깅
- nRF Connect (휴대폰): 리모콘 서비스 확인
- USB Device Tree Viewer (uwe-sieber.de): USB 디스크립터 GUI

## 상태 / 남은 일
- [x] 툴체인, 펌웨어 빌드, USB composite(HID+CDC+MSC), 키맵 + CDC 명령, 호스트 테스트, PC 스크립트
- [x] 하드웨어: USB 열거, MSC(J:에 exe), CDC, inject → Windows 키 입력 확인
- [x] 저장된 장치 1대 자동 재연결, scan/pair/forget, K1 페어링, LCD 상태, exe 장치/매핑 탭
- [ ] **실제 BLE 리모콘으로 미검증** (페어링/본딩/재연결), LCD 방향·오프셋 보정 (`lcd.c` 상단 상수)
- [x] G20BTS 리모콘: 페어링, 재플래시 후 자동 재연결, Report Map 해석 확인
- [ ] 주소가 바뀌는(RPA) 장치는 재연결 안 될 수 있음. 미디어키 출력은 한 번에 1개
- [ ] 원래 펌웨어 백업: `dist\original-flash-16MB.bin` (0x0에 기록하면 복구, gitignore라 이 PC에만 있음)
- [ ] 실제 제품화 시 자체 VID/PID 필요 (`303A:4007`은 개발용)
