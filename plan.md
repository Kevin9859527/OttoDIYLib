# Otto ESP32-S3 BLE 功能接入计划

本文档用于跟踪当前 `otto_ble` PlatformIO 固件已经参与编译的功能、已经接入主程序的功能，以及后续需要逐步补齐的功能。

## 当前编译入口

当前主编译环境为 `otto_ble.ini` 中的 `[env:otto_ble]`。

当前参与编译的 `.cpp` 文件只有：

- `src/otto_ble.cpp`
- `src/Otto.cpp`
- `src/Oscillator.cpp`

编译宏：`OTTO_DISABLE_MATRIX`、`OTTO_DISABLE_BUZZER`

已移出编译（在 `ignore/`）：

- `ignore/Otto_matrix.cpp` / `ignore/Otto_matrix.h`
- `ignore/SerialCommand.cpp` / `ignore/SerialCommand.h`

## 当前 GPIO 分配

| 功能 | GPIO | 状态 |
| --- | --- | --- |
| 左腿舵机 | 7 | 已用 |
| 右腿舵机 | 8 | 已用 |
| 左脚舵机 | 9 | 已用 |
| 右脚舵机 | 12 | 已用（第 4 路，待确认） |
| TFT MOSI | 3 | 预留 INPUT |
| TFT DC | 4 | 预留 INPUT |
| TFT CS 左眼 | 5 | 预留 INPUT |
| TFT CS 右眼 | 6 | 预留 INPUT |
| 命令 UART TX | 10 | UART1 |
| 命令 UART RX | 11 | UART1 |
| USB 日志 | 内置 CDC | 115200 |
| 点阵 / 蜂鸣器 | — | 已屏蔽 |
| 其余 GPIO0–21 | — | INPUT 高阻 |

## examples 是否参与编译

当前 `examples/` 目录没有参与 `pio run` 编译。

原因：

- PlatformIO 默认编译 `src/` 下的源码。
- `otto_ble.ini` 使用 `build_src_filter` 明确只包含当前 BLE 固件需要的 4 个 `.cpp`。
- `examples/Otto_APP/Otto_APP.ino` 等示例仍作为参考代码保留，不作为固件入口。

因此暂时不需要把 `examples/` 移到 `src/`。后续如果某个示例中的功能要进入实际固件，应把对应逻辑整理进 `src/otto_ble.cpp` 或拆成新的 `src/*.cpp` / `src/*.h`。

## 已参与编译的 Otto 库能力

因为 `src/Otto.cpp` 整体参与编译，下列 Otto 类能力已经进入固件：

- 舵机初始化：`Otto.init()`
- 舵机挂载/释放：`attachServos()`、`detachServos()`
- 舵机校准值设置：`setTrims()`
- 舵机基础移动：`_moveServos()`、`_moveSingle()`
- 舵机正弦振荡动作执行：`oscillateServos()`、`_execute()`
- 回中/休息状态：`home()`、`getRestState()`、`setRestState()`
- 基础动作：`jump()`、`walk()`、`turn()`、`bend()`、`shakeLeg()`
- 舞蹈动作：`updown()`、`swing()`、`tiptoeSwing()`、`jitter()`、`ascendingTurn()`、`moonwalker()`、`crusaito()`、`flapping()`
- 点阵嘴型：`putMouth()`、`putAnimationMouth()`、`clearMouth()`、`setLed()`、`writeText()`
- 蜂鸣器声音：`_tone()`、`bendTones()`、`sing()`
- 组合表情/手势：`playGesture()`
- 点阵初始化和亮度：`initMATRIX()`、`matrixIntensity()`
- 舵机限速：`enableServoLimit()`、`disableServoLimit()`

因为 `src/Oscillator.cpp` 参与编译，下列底层 PWM 舵机能力已经进入固件：

- `ESP32Servo` 舵机 attach/detach
- 角度写入：`SetPosition()`
- 正弦曲线刷新：`refresh()`
- 舵机软件位置记录：`getPosition()`
- 舵机 trim 偏移
- 舵机命令速度限制

点阵底层已屏蔽（`OTTO_DISABLE_MATRIX`），`putMouth()` 等为空操作，待接 TFT 双眼。

## 当前主程序已实际接入的功能

`src/otto_ble.cpp` 当前已经实际调用或暴露给 BLE 命令的功能：

- BLE 透传服务启动，设备名为 `OttoDIY-BLE`
- UART1（GPIO10/11）与 BLE 相同命令协议
- BLE RX 写入命令，BLE TX notify 返回 ACK（UART 同步回显 ACK）
- `S`：停止，执行 `Otto.home()`
- `L <binary>`：显示自定义点阵图案
- `T <freq> <duration>`：蜂鸣器发声
- `M <moveId> [T] [size]`：执行运动
- `H <gestureId>`：执行组合手势
- `K <songId>`：播放预设声音
- `C <trimYL> <trimYR> <trimRL> <trimRR>`：设置舵机校准值，仅 RAM 生效
- `G <YL> <YR> <RL> <RR>`：直接设置四个舵机角度
- 启动时初始化 4 路 PWM 舵机
- 点阵、蜂鸣器已屏蔽；手势中的嘴型/音效调用保留时序但不驱动硬件
- 启动时启用舵机速度限制

## 当前可用的运动编号

`M <moveId> [T] [size]` 当前支持：

- `0`：home
- `1`：向前走
- `2`：向后走
- `3`：左转
- `4`：右转
- `5`：updown
- `6`：moonwalker 左
- `7`：moonwalker 右
- `8`：swing
- `9`：crusaito 左/前向参数
- `10`：crusaito 右/反向参数
- `11`：jump
- `12`：flapping 前
- `13`：flapping 后
- `14`：tiptoeSwing
- `15`：左 bend
- `16`：右 bend
- `17`：左/右腿 shakeLeg 参数 1
- `18`：左/右腿 shakeLeg 参数 -1
- `19`：jitter
- `20`：ascendingTurn

## 当前可用的手势编号

`H <gestureId>` 当前支持：

- `1`：OttoHappy
- `2`：OttoSuperHappy
- `3`：OttoSad
- `4`：OttoSleeping
- `5`：OttoFart
- `6`：OttoConfused
- `7`：OttoLove
- `8`：OttoAngry
- `9`：OttoFretful
- `10`：OttoMagic
- `11`：OttoWave
- `12`：OttoVictory
- `13`：OttoFail

## 当前可用的声音编号

`K <songId>` 当前支持：

- `1`：S_connection
- `2`：S_disconnection
- `3`：S_surprise
- `4`：S_OhOoh
- `5`：S_OhOoh2
- `6`：S_cuddly
- `7`：S_sleeping
- `8`：S_happy
- `9`：S_superHappy
- `10`：S_happy_short
- `11`：S_sad
- `12`：S_confused
- `13`：S_fart1
- `14`：S_fart2
- `15`：S_fart3
- `16`：S_mode1
- `17`：S_mode2
- `18`：S_mode3
- `19`：S_buttonPushed

## 等待后续实现或实机验证的功能

下列功能尚未完成，后续应逐步接入并验证：

- EEPROM/Preferences 持久化：当前 `C` 命令只设置 RAM trim，重启丢失。
- ESP32-S3R8 PSRAM 实际识别：当前配置已加入 PSRAM 相关项，但 PlatformIO 输出仍显示板卡定义为 `No PSRAM`，需要按实际开发板进一步校准。
- BLE / UART 命令兼容性测试。
- 舵机 GPIO7/8/9/12 方向与 trim 实机确认；若只需 3 路舵机请改接线定义。
- SPI TFT 双眼驱动接入（GPIO3/4/5/6）。
- 小智 AI 喇叭播放，替代蜂鸣器 `K`/`T` 命令语义。
- 按键功能：当前只配置了 `PIN_BUTTON`，尚未实现按钮模式切换。
- 装配/使能检测脚：当前只配置了 `PIN_ASSEMBLY`，尚未恢复原 App 固件中的装配等待逻辑。
- 传感器功能：避障超声波、光敏、电压/模拟输入等示例功能尚未接入 BLE 主程序。
- 歌曲舞蹈示例：`Otto_singleladies`、`Otto_smoothcriminal`、`Otto_happybirthday` 尚未接入主程序。
- 原 `SerialCommand` 串口命令路径：已移到 `ignore/`，当前不再作为 BLE 固件路径使用。
- 外接 UART BLE 模块路径：当前使用 ESP32-S3 板载 BLE，不支持旧 `SoftwareSerial` 路径。
- UART 单线舵机替换：当前仍是 PWM 舵机 + `ESP32Servo`，单线串口舵机需要重写底层舵机驱动层。
- 小智 AI 集成：尚未接入，需要先确定具体 SDK/工程、分区表、PSRAM、音频输入输出和网络依赖。

## 建议后续顺序

1. 确认 ESP32-S3R8 板卡型号、Flash/PSRAM 类型和可用 GPIO。
2. 烧录当前 BLE 固件，验证串口日志和 BLE 广播。
3. 用 BLE 调试工具发送 `S\r`、`M 1 1000 15\r`、`K 8\r`。
4. 单独验证 4 路舵机方向和 trim。
5. 单独验证点阵方向和嘴型显示。
6. 单独验证蜂鸣器。
7. 恢复 trim 持久化。
8. 加入按键/装配检测逻辑。
9. 按需要加入传感器示例功能。
10. 评估并接入小智 AI 或 UART 单线舵机。
