# 2026-11-21: 리모콘이 오래 연결된 뒤 끊기면 재연결이 안 됨

## 증상
- 보드: ESP32-S3-Zero. 펌웨어 0.1.0, 커밋 19f5141 (USB로 로그 출력 추가된 빌드)
- 리모콘: G20BTS (`f4:4e:fd:13:07:f8`, public 주소)
- 며칠 연결돼 있다가 끊기면, 리모콘 버튼으로 깨워도 다시 연결되지 않음
- USB를 다시 꽂으면(재부팅) 다시 연결됨

## 고장 상태에서 모은 증거 (보드는 재부팅하지 않음)
캡처 시점 보드 가동 시간은 약 17,200초(4.8시간)입니다. 이번 재부팅 뒤 5시간이 안 돼서 이미 이 상태에 들어갔다는 뜻입니다.

1. `status` → `ok connecting f4:4e:fd:13:07:f8 G20BTS`. 저장된 장치와 키 매핑 14개(`list`)는 그대로입니다. [stuck-info.log](2026-11-21-stuck-info.log)
2. 재연결 시도마다 같은 로그가 1초 간격으로 반복됩니다:
   ```
   W BT_L2CAP: is_aux auto-set to true (BLE 5.0 API used)
   W BT_HCI: opcode=0x2043, status= 0c: Cmd Disallowed
   W BT_APPL: gattc_conn_cb: if=3 st=0 id=3 rsn=0x100
   E BLE_HIDH: OPEN failed: 0x85
   ```
   - 연결 명령이 HCI `0x2043` (LE Extended Create Connection, BLE 5.0)으로 나갑니다.
   - 컨트롤러가 이를 `0x0C Command Disallowed`로 **즉시** 거부합니다(약 10ms). 시간 초과가 아닙니다.
3. 같은 상태에서 `scan 5`(레거시 스캔 명령)는 정상 동작합니다. 리모콘이 광고 중으로 잡힙니다(`dev f4:4e:fd:13:07:f8 0 -68 G20BTS`). [stuck-scan.log](2026-11-21-stuck-scan.log)
   - 리모콘은 깨어 있고 연결을 받을 준비가 돼 있습니다.
   - 레거시 명령은 통과하고 확장(BLE 5.0) 명령만 거부됩니다.

## 소스에서 확인한 동작 (ESP-IDF v6.1 Bluedroid)
- `stack/l2cap/l2c_ble.c` (약 1041행): BLE 4.2와 5.0이 둘 다 켜진 빌드에서는 `btm_ble_inter_get()`이 참이면 `is_aux = true`가 됩니다. 그러면 연결 명령이 레거시 `0x200D` 대신 확장 `0x2043`으로 나갑니다.
- `stack/btm/btm_ble_5_gap.c` `BTM_ExtBleCallbackTrigger()`: BLE 5.0 GAP 이벤트가 **성공**하면 무엇이든 `btm_ble_inter_set(true)`를 호출합니다.
  - 중앙 장치(이 동글)에서 상대가 일으킬 수 있는 대표적인 경우는 `btm_ble_update_phy_evt()`입니다. 리모콘이 요청한 PHY 변경(예: 2M PHY)이 완료되면 `BTM_BLE_5_GAP_PHY_UPDATE_COMPLETE_EVT`가 발생합니다.
- 이 플래그를 `false`로 되돌리는 코드는 어디에도 없습니다. 그래서 재부팅 전까지 계속 유지됩니다.
- 이 펌웨어는 레거시 명령을 씁니다. 스캔(`esp_ble_gap_start_scanning`)과 부팅 직후 첫 연결(`0x200D`)이 그렇습니다.
  - 블루투스 스펙상 컨트롤러는 레거시 명령과 확장 명령을 섞어 쓰면 `Command Disallowed`로 거부할 수 있습니다(Core spec Vol 4 Part E 3.1.1).
  - 관찰된 결과와 맞습니다.
- `esp_hid_gap.c`는 처리하지 않는 GAP 이벤트를 VERBOSE 레벨로만 로그합니다. 그래서 PHY 변경 이벤트가 언제 왔는지는 로그에 남지 않았습니다.

## 결론 (추정)
1. 연결돼 있는 동안 리모콘이 PHY 변경을 요청합니다.
2. BLE 5.0 플래그가 켜집니다.
3. 연결이 끊기면 재연결이 확장 연결 명령으로 바뀌고, 컨트롤러가 거부합니다.
4. 재부팅할 때까지 영구히 실패합니다.

아직 확인하지 않은 것: PHY 변경이 연결 직후에 오는지, 한참 뒤에 오는지.

## 재현 결과 (같은 날, 수정 전 펌웨어) → 1분 만에 재현됨
[repro.log](2026-11-21-repro.log)
1. 재부팅 직후 리모콘이 잠들어 있는 동안에는 연결 시도가 레거시 명령으로 나갑니다. `is_aux` 경고 없이 10초 시간 초과(`rsn=0x100`)로만 실패합니다.
2. 59.9초: 리모콘을 깨우자 연결 성공(`AUTH SUCCESS` → `connected`), 키 입력도 정상입니다.
3. 120.3초: 배터리를 빼서 연결이 끊깁니다(`rsn 0x8`, supervision timeout). 이때 `ESP_HIDH: Already Connected` 오류도 한 번 찍힙니다.
4. 121.3초부터: `is_aux auto-set to true (BLE 5.0 API used)` → `0x2043 Cmd Disallowed`가 반복되고, 재연결은 영구 실패합니다.

→ BLE 5.0 플래그는 **연결 직후**(약 1분 안)에 켜집니다. "이틀, 삼일"은 플래그가 켜지는 데 걸린 시간이 아니라 첫 연결 끊김까지 걸린 시간이었습니다. 따라서 **첫 연결 → 한 번 끊김**만으로 항상 재현됩니다.

## 수정본 검증 (BLE 4.2 전용) → 재연결됨
[fixed.log](2026-11-21-fixed.log)
- 57.9초: 연결
- 116.1초: 배터리를 빼서 끊김(`rsn 0x8`)
- 120.8초: 리모콘을 깨우자 바로 재연결(`OPEN` → `connected`)
- 로그 전체에 `is_aux`나 `Cmd Disallowed`가 한 번도 나오지 않음

## 함께 발견한 크래시: OPEN 이벤트의 NULL 포인터
[coredump.txt](2026-11-21-coredump.txt). 플래시에 남아 있던 크래시 기록을, 같은 소스(19f5141)로 다시 빌드한 ELF로 해석했습니다(SHA 검사만 우회).
- `esp_hidh_events` 작업, `LoadProhibited`, `excvaddr 0x0`, 위치는 `main.c` `hidh_callback`의 OPEN 처리 부분입니다.
- 원인: esp_hidh가 장치를 이미 해제한 뒤 OPEN 이벤트가 오면 `esp_hidh_dev_bda_get()`이 NULL을 돌려주는데, 이를 확인하지 않고 읽었습니다.
- 가동 4.8시간이던 "원인 모를 재부팅"이 이것이었을 가능성이 있습니다. 크래시 시각은 기록에 없습니다.
- 수정:
  - OPEN 처리에서 NULL을 확인합니다.
  - `ble_task`에서 연결 직후 장치가 이미 해제됐으면 연결된 것으로 치지 않습니다. 그대로 두면 연결된 상태로 남아 재시도를 안 하게 됩니다.

## S3-Zero에서 `dfu`가 USB에서 사라짐 → 해결
- 증상: `dfu`를 보내면 보드가 USB에서 사라지고 다운로드 포트(1001)로 다시 나타나지 않았습니다. Windows에는 열거 실패 장치도, 이벤트 로그도 남지 않았습니다(전기적으로 연결이 안 된 상태).
- 단계를 바꾼 `dfu`를 실제 보드에서 시험했습니다:

  | 방식 | 결과 |
  |---|---|
  | 기존 순서 | 실패 |
  | 1: 로그를 끈 뒤 기존 순서 | 실패 → 로그 미러링은 원인이 아님 |
  | 2: 1 + GPIO19/20 조작 없음 | **성공**. 다운로드 포트가 바로 나타나고, esptool 리셋 뒤 앱으로 복귀 |

- 원인: 연결 끊김을 알리려고 D+/D- 핀을 LOW로 구동했다가 `gpio_reset_pin()`으로 되돌리던 단계입니다. `gpio_reset_pin()`은 풀업도 켭니다.
- 수정: 핀은 건드리지 않고, USB 패드만 100ms 끕니다.
- 검증: S3-Zero에서 `dev.ps1 flash`가 `dfu` → 플래시 → 앱 복귀까지 사람 손 없이 2회 연속 성공했고, 페어링 정보도 유지됐습니다.
- LCD 스틱에서는 바뀐 `dfu`를 아직 시험하지 않았습니다.
- 참고: BOOT를 누른 채 꽂아 들어간 다운로드 모드에서는 esptool 리셋 뒤에도 다운로드 모드에 남아(`boot:0x0`) 다시 꽂아야 했습니다. `dfu`로 들어간 경우에는 리셋으로 앱에 돌아옵니다.

## 빨리 재현하는 방법 (수정 전 펌웨어)
1. 재부팅한 뒤 리모콘을 연결합니다.
2. 리모콘 배터리를 뺐다 끼워서 연결을 끊습니다.
3. `0x2043 Cmd Disallowed`가 나오면, 플래그가 연결 초반에 이미 켜진다는 뜻입니다(바로 재현 가능).
   나오지 않으면 PHY 변경이 나중에 오는 것이므로, 오래 기다려야 재현됩니다.

## 수정안
`sdkconfig.defaults`에서 `CONFIG_BT_BLE_50_FEATURES_SUPPORTED`를 끕니다(BLE 4.2만 사용).
- `l2c_ble.c`가 항상 `is_aux = false`가 되어 연결 명령이 레거시 `0x200D`로만 나갑니다.
- `btm_ble_5_gap.c` 자체가 빌드에서 빠집니다.
