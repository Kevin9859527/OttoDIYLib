# UART 通信协议说明

本文档记录当前 ESP32-S3 BLE 固件中 UART1 命令接口的通信参数、收发格式、返回码和命令编号对应关系。实现位置：`src/otto_ble.cpp`。

## 1. UART 接口参数

| 项目 | 当前值 |
| --- | --- |
| UART 控制器 | UART1 / `HardwareSerial CmdSerial(1)` |
| 波特率 | `115200` |
| 数据位 | `8` |
| 校验位 | `None` |
| 停止位 | `1` |
| ESP32-S3 TX | `GPIO10` |
| ESP32-S3 RX | `GPIO11` |

接线方向：

| ESP32-S3 | 外部 USB-TTL / 主控 |
| --- | --- |
| `GPIO10` TX | 对方 RX |
| `GPIO11` RX | 对方 TX |
| GND | GND |

## 2. 启动输出

开机后，固件会同时从 USB CDC Serial 和 UART1 TX 输出初始化日志。

典型输出：

```text
UART1 init: OK
UART1 pins: TX=GPIO10 RX=GPIO11 BAUD=115200
STARTUP-CODE:XXXXXXXX
Otto ESP32-S3 ready
  Log: USB Serial @ 115200
  Cmd: UART1 GPIO10=TX GPIO11=RX @ 115200
  Matrix: disabled (TFT eye GPIO3/4/5 planned)
  Buzzer: disabled
```

说明：

- `STARTUP-CODE:XXXXXXXX` 为每次启动生成的 8 位十六进制启动码，用于确认设备发生过重启。
- 启动日志不是命令响应，不需要上位机回复。

## 3. 命令帧格式

UART 命令为 ASCII 文本格式：

```text
<命令字母> [参数1] [参数2] ...
```

规则：

- 命令字母不区分大小写，例如 `S` 和 `s` 等价。
- 参数之间使用空格分隔。
- 推荐每条命令以 `\r`、`\n` 或 `\r\n` 结束。
- 当前固件也兼容不带换行的串口工具：收到最后一个字节后超过约 `40ms` 没有新字节，会自动提交当前缓冲区作为一条命令。
- 单条命令缓冲区大小为 `128` 字节，超过后会清空当前接收缓冲。

示例：

```text
S
G 90 90 90 90
M 1 1000 15
```

## 4. 通用返回

正常命令会通过 UART1 TX 输出 ACK。当前 ACK 同时也会输出到 USB CDC Serial 和 BLE notify。

| 返回 | 含义 | 发送时机 |
| --- | --- | --- |
| `&&A%%` | 已收到并开始处理命令 | 命令分发后立即发送 |
| `&&F%%` | 命令处理完成 | 动作或处理流程完成后发送 |

典型流程：

```text
上位机 -> S
设备   -> &&A%%
设备   -> &&F%%
```

`M` 运动命令为两段式：

```text
上位机 -> M 1 1000 15
设备   -> &&A%%
设备   -> 执行动作
设备   -> &&F%%
```

注意：

- 当前错误提示，例如 `M: invalid move id`、`G: invalid servo arguments`，主要打印到 USB CDC Serial 日志，不作为 UART 协议错误码返回。
- 未知命令当前会进入 `receiveStop()`，效果等同于 `S`：回中位并返回 `&&A%%` / `&&F%%`。

## 5. 硬件与舵机通道

当前 4 路 PWM 舵机 GPIO：

| 参数名 | 舵机 | GPIO |
| --- | --- | --- |
| `YL` | 左腿 | `GPIO6` |
| `YR` | 右腿 | `GPIO7` |
| `RL` | 左脚 | `GPIO8` |
| `RR` | 右脚 | `GPIO9` |

TFT 单眼预留：

| 功能 | GPIO |
| --- | --- |
| MOSI | `GPIO3` |
| DC | `GPIO4` |
| CS | `GPIO5` |

## 6. 命令总表

| 命令 | 格式 | 功能 | UART 返回 |
| --- | --- | --- | --- |
| `S` | `S` | 停止当前动作，回中位，清除运动编号 | `&&A%%`、`&&F%%` |
| `L` | `L <binary>` | 点阵/眼睛命令占位；当前点阵已屏蔽，TFT 未实现 | `&&A%%`、`&&F%%` |
| `T` | `T <freq> <duration>` | 蜂鸣器命令占位；当前蜂鸣器已屏蔽 | `&&A%%`、`&&F%%` |
| `M` | `M <moveId> [T] [size]` | 执行运动动作 | 先 `&&A%%`，动作完成后 `&&F%%` |
| `H` | `H <gestureId>` | 执行组合手势 | `&&A%%`、`&&F%%` |
| `K` | `K <songId>` | 播放预设声音序列；蜂鸣器屏蔽时主要保留时序 | `&&A%%`、`&&F%%` |
| `C` | `C <trimYL> <trimYR> <trimRL> <trimRR>` | 设置四路舵机 trim，当前仅 RAM 生效 | `&&A%%`、`&&F%%` |
| `G` | `G <YL> <YR> <RL> <RR>` | 直接设置四路舵机角度 | `&&A%%`、`&&F%%` |

## 7. `S` 停止命令

格式：

```text
S
```

效果：

- 发送 `&&A%%`
- 执行 `Otto.home()`
- `moveId = 0`
- 发送 `&&F%%`

## 8. `G` 舵机直控命令

格式：

```text
G <YL> <YR> <RL> <RR>
```

参数：

| 参数 | 含义 | 建议范围 |
| --- | --- | --- |
| `YL` | 左腿角度 | `0` 到 `180`，中位 `90` |
| `YR` | 右腿角度 | `0` 到 `180`，中位 `90` |
| `RL` | 左脚角度 | `0` 到 `180`，中位 `90` |
| `RR` | 右脚角度 | `0` 到 `180`，中位 `90` |

示例：

```text
G 90 90 90 90
G 110 90 90 90
G 90 90 90 70
```

说明：

- 执行动作为 `_moveServos(200, servoPos)`，会在约 `200ms` 内移动到目标角度。
- 收到 `G` 后内部会设置 `moveId = 30`，避免 `loop()` 继续执行预设运动。

## 9. `C` trim 校准命令

格式：

```text
C <trimYL> <trimYR> <trimRL> <trimRR>
```

参数：

| 参数 | 含义 |
| --- | --- |
| `trimYL` | 左腿 trim |
| `trimYR` | 右腿 trim |
| `trimRL` | 左脚 trim |
| `trimRR` | 右脚 trim |

示例：

```text
C 0 0 0 0
C 5 -3 0 2
```

说明：

- 当前通过 `Otto.setTrims()` 设置，**仅 RAM 生效**。
- 重启后会丢失，后续若接入 Preferences / NVS 可改为持久化。

## 10. `M` 运动命令

格式：

```text
M <moveId> [T] [size]
```

参数：

| 参数 | 含义 | 默认值 |
| --- | --- | --- |
| `moveId` | 运动编号 | 无效时置 `0` |
| `T` | 动作周期，单位 ms | `1000` |
| `size` | 动作幅度参数 | `15` |

当前实现为单次执行：收到 `M` 后先设置运动参数，`loop()` 中执行一次 `moveRobot(moveId)`，动作完成后发送 `&&F%%`，然后 `Otto.home()` 并清 `moveId = 0`。

### `M` 编号表

| `moveId` | 调用 | 含义 |
| --- | --- | --- |
| `0` | `Otto.home()` | 回中位 |
| `1` | `Otto.walk(1, T, 1)` | 向前走一步 |
| `2` | `Otto.walk(1, T, -1)` | 向后走一步 |
| `3` | `Otto.turn(1, T, 1)` | 左转一步 |
| `4` | `Otto.turn(1, T, -1)` | 右转一步 |
| `5` | `Otto.updown(1, T, size)` | 上下摆动 |
| `6` | `Otto.moonwalker(1, T, size, 1)` | moonwalker 左 |
| `7` | `Otto.moonwalker(1, T, size, -1)` | moonwalker 右 |
| `8` | `Otto.swing(1, T, size)` | swing 摇摆 |
| `9` | `Otto.crusaito(1, T, size, 1)` | crusaito 方向 1 |
| `10` | `Otto.crusaito(1, T, size, -1)` | crusaito 方向 -1 |
| `11` | `Otto.jump(1, T)` | jump 跳跃 |
| `12` | `Otto.flapping(1, T, size, 1)` | flapping 方向 1 |
| `13` | `Otto.flapping(1, T, size, -1)` | flapping 方向 -1 |
| `14` | `Otto.tiptoeSwing(1, T, size)` | 踮脚摇摆 |
| `15` | `Otto.bend(1, T, 1)` | bend 方向 1 |
| `16` | `Otto.bend(1, T, -1)` | bend 方向 -1 |
| `17` | `Otto.shakeLeg(1, T, 1)` | shakeLeg 方向 1 |
| `18` | `Otto.shakeLeg(1, T, -1)` | shakeLeg 方向 -1 |
| `19` | `Otto.jitter(1, T, size)` | jitter 抖动 |
| `20` | `Otto.ascendingTurn(1, T, size)` | ascendingTurn 上升转身 |

示例：

```text
M 0 1000 15
M 1 1500 10
M 2 1500 10
M 3 1500 10
M 11 2000 15
```

## 11. `H` 手势命令

格式：

```text
H <gestureId>
```

流程：

- 发送 `&&A%%`
- 先执行 `Otto.home()`
- 调用对应 `Otto.playGesture(...)`
- 发送 `&&F%%`

### `H` 编号表

| `gestureId` | 调用 | 含义 |
| --- | --- | --- |
| `1` | `OttoHappy` | 开心 |
| `2` | `OttoSuperHappy` | 超级开心 |
| `3` | `OttoSad` | 伤心 |
| `4` | `OttoSleeping` | 睡觉 |
| `5` | `OttoFart` | 放屁/搞怪 |
| `6` | `OttoConfused` | 困惑 |
| `7` | `OttoLove` | 爱心 |
| `8` | `OttoAngry` | 生气 |
| `9` | `OttoFretful` | 烦躁 |
| `10` | `OttoMagic` | 魔法动画 |
| `11` | `OttoWave` | 挥手/波浪动画 |
| `12` | `OttoVictory` | 胜利 |
| `13` | `OttoFail` | 失败 |

示例：

```text
H 1
H 4
H 12
```

## 12. `K` 声音命令

格式：

```text
K <songId>
```

说明：

- 当前 `PIN_BUZZER_DISABLED = -1`，蜂鸣器硬件已屏蔽。
- `K` 命令仍会走完整 ACK / Final 流程，`Otto.sing(...)` 内部的时序仍可能产生延时。

### `K` 编号表

| `songId` | 调用 | 含义 |
| --- | --- | --- |
| `1` | `S_connection` | 连接音 |
| `2` | `S_disconnection` | 断开音 |
| `3` | `S_surprise` | 惊讶 |
| `4` | `S_OhOoh` | OhOoh |
| `5` | `S_OhOoh2` | OhOoh2 |
| `6` | `S_cuddly` | 可爱音效 |
| `7` | `S_sleeping` | 睡觉音效 |
| `8` | `S_happy` | 开心 |
| `9` | `S_superHappy` | 超级开心 |
| `10` | `S_happy_short` | 短开心 |
| `11` | `S_sad` | 伤心 |
| `12` | `S_confused` | 困惑 |
| `13` | `S_fart1` | 放屁音效 1 |
| `14` | `S_fart2` | 放屁音效 2 |
| `15` | `S_fart3` | 放屁音效 3 |
| `16` | `S_mode1` | 模式音 1 |
| `17` | `S_mode2` | 模式音 2 |
| `18` | `S_mode3` | 模式音 3 |
| `19` | `S_buttonPushed` | 按键音 |

示例：

```text
K 1
K 8
```

## 13. `L` 点阵 / 眼睛命令

格式：

```text
L <binary>
```

当前状态：

- 原 MAX7219 点阵已通过 `OTTO_DISABLE_MATRIX` 屏蔽。
- TFT 单眼尚未接入驱动。
- 收到 `L` 后会先 `Otto.home()`，然后返回 `&&A%%` / `&&F%%`。
- USB Serial 会打印 `L: matrix disabled; TFT eyes not implemented yet`，但这不是 UART 协议返回。

## 14. `T` 蜂鸣器命令

格式：

```text
T <freq> <duration>
```

当前状态：

- 蜂鸣器已屏蔽，`PIN_BUZZER_DISABLED = -1`。
- 收到 `T` 后会先 `Otto.home()`，然后返回 `&&A%%` / `&&F%%`。
- USB Serial 会打印 `T: buzzer disabled; use Xiaozhi speaker later`，但这不是 UART 协议返回。

## 15. 推荐测试命令

先测通信：

```text
S
```

预期：

```text
&&A%%
&&F%%
```

再测四路舵机：

```text
G 90 90 90 90
G 110 90 90 90
G 90 110 90 90
G 90 90 110 90
G 90 90 90 110
G 90 90 90 90
```

再测低风险动作：

```text
M 0 1000 15
M 1 1500 10
M 2 1500 10
M 3 1500 10
M 4 1500 10
```

需要急停时发送：

```text
S
```

## 16. 当前限制

- UART 当前主要返回 `&&A%%` 和 `&&F%%`，没有独立错误码。
- 参数错误信息目前主要在 USB CDC Serial 日志中，不保证通过 UART 返回。
- `L`、`T` 命令保留协议入口，但对应硬件当前已屏蔽或未实现。
- `C` 校准值当前不持久化，重启后丢失。
- `M` 动作执行期间为阻塞式动作流程，`&&F%%` 需要等动作完成后才返回。
