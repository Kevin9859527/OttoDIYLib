# ESP32-S3 BLE PlatformIO 迁移说明

本文档记录本次将 OttoDIYLib 迁移到 PlatformIO + ESP32-S3R8 + BLE 透传的第一步改动。

## 编译入口

新增 PlatformIO 配置：

- `platformio.ini`：项目总入口，默认环境为 `otto_ble`。
- `otto_ble.ini`：ESP32-S3 BLE 固件环境配置。
- `src/otto_ble.cpp`：新的 ESP32-S3 BLE 主程序。
- `ignore/SerialCommand.cpp` / `ignore/SerialCommand.h`：旧串口命令解析实现，已移出 `src`，避免被误认为参与当前 BLE 固件编译。

编译命令：

```bash
pio run
```

当前已验证 `pio run` 编译通过。

## PlatformIO 环境

`otto_ble.ini` 使用：

- `platform = espressif32`
- `board = esp32-s3-devkitc-1`
- `framework = arduino`
- `board_build.mcu = esp32s3`
- `board_upload.flash_size = 8MB`
- `board_build.partitions = default_8MB.csv`
- `BOARD_HAS_PSRAM`、`board_build.psram_type = opi`、`board_build.arduino.memory_type = qio_opi`

说明：PlatformIO 的 `esp32-s3-devkitc-1` 板卡定义名称仍会显示为 DevKitC-1-N8，但配置已按 ESP32-S3、8MB Flash、OPI PSRAM 方向设置。若你的具体开发板 flash/psram 类型不同，后续需要按板卡丝印或模组型号微调。

## 预设 GPIO

| 功能 | GPIO |
| --- | --- |
| 左腿 / 右腿 / 左脚舵机 | GPIO7 / 8 / 9 |
| 右脚舵机（第 4 路，可改） | GPIO12 |
| TFT MOSI / DC / CS左 / CS右 | GPIO3 / 4 / 5 / 6（预留，未驱动） |
| 命令 UART TX / RX | GPIO10 / 11 |
| 点阵、蜂鸣器 | 已软件屏蔽 |

## 烧录与日志（问题 5）

当前 `otto_ble.ini` 配置：

| 项目 | 设置 |
| --- | --- |
| 日志口 | USB CDC `Serial`，`monitor_speed = 115200` |
| USB 枚举 | `ARDUINO_USB_CDC_ON_BOOT=1`、`ARDUINO_USB_MODE=1` |
| 烧录口 | 与日志相同，接开发板 USB 后由 esptool 写入 |
| 烧录速率 | `upload_speed = 921600` |
| 固件产物 | `.pio/build/otto_ble/firmware.bin`（及 bootloader/partitions） |

常用命令：

```bash
pio run -t upload    # 烧录
pio device monitor   # 看 USB 日志
```

无 BLE 时可用外接 USB-TTL 接 **GPIO10(TX)、GPIO11(RX)**，115200，发送与 App 相同命令（以 `\r` 结尾），ACK 会从该串口回显。

点阵源码已移至 `ignore/Otto_matrix.cpp`，编译宏 `OTTO_DISABLE_MATRIX`；蜂鸣器宏 `OTTO_DISABLE_BUZZER`。

## BLE 透传设计

原 `examples/Otto_APP/Otto_APP.ino` 使用 `SoftwareSerial + SerialCommand` 接收蓝牙串口模块命令。本次 ESP32-S3 版本改为板载 BLE：

- BLE 名称：`OttoDIY-BLE`
- Service UUID：`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- RX Characteristic：`6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
- TX Characteristic：`6E400003-B5A3-F393-E0A9-E50E24DCCA9E`

这是常见 Nordic UART Service 风格的 BLE 透传接口。手机端向 RX 写入命令，固件通过 TX notify 返回 ACK。

命令协议沿用原 App 固件：

- `S`：停止并回到 home
- `L <binary>`：显示自定义点阵图案
- `T <freq> <duration>`：蜂鸣器发声
- `M <moveId> [T] [size]`：运动命令
- `H <gestureId>`：手势
- `K <songId>`：声音
- `C <trimYL> <trimYR> <trimRL> <trimRR>`：舵机校准，当前仅 RAM 生效
- `G <YL> <YR> <RL> <RR>`：直接设置四个舵机角度

命令以 `\r` 或 `\n` 结束，参数使用空格分隔。

## 暂时注释/绕开的功能

为保证第一步稳定编译，做了以下取舍：

- `SerialCommand.cpp` 和 `SerialCommand.h` 已移动到 `ignore/`，ESP32-S3 BLE 主程序内部直接解析 BLE 收到的命令。
- 不使用 `SoftwareSerial`。
- `Otto.init(..., false, PIN_BUZZER)` 关闭 EEPROM 校准读取。
- `C` 校准命令只调用 `Otto.setTrims()`，不调用 `Otto.saveTrimsOnEEPROM()`，因此当前校准值只在 RAM 中生效，重启后丢失。

原库文件 `Otto.cpp`、`Oscillator.cpp`、`Otto_matrix.cpp` 仍参与编译，高层动作和 PWM 舵机控制逻辑继续复用。

`otto_ble.ini` 通过 `build_src_filter` 明确只编译：

- `src/otto_ble.cpp`
- `src/Otto.cpp`
- `src/Oscillator.cpp`
- `src/Otto_matrix.cpp`

## 当前验证结果

已执行：

```bash
pio run
```

结果：编译成功，生成 ESP32-S3 固件。

首次编译输出的资源占用约为：

- RAM：约 14%
- Flash：约 28%

## 后续建议

下一步建议按硬件实际连接顺序处理：

- 确认 ESP32-S3R8 开发板具体模组、Flash/PSRAM 类型和 GPIO 引脚可用性。
- 实机测试 BLE 扫描、连接、写入 `S\r`、`M 1 1000 15\r` 等命令。
- 检查 `ESP32Servo` 输出的 PWM 频率和舵机供电是否稳定。
- 用 ESP32 的 `Preferences` 或正确的 EEPROM 初始化流程恢复校准持久化。
- 若后续改 UART 单线舵机，建议替换 `Oscillator` 底层写舵机接口，而不是改高层动作函数。
