# PWM 舵机改为单线 UART 总线舵机方案

本文档只写迁移思路和方案，不直接修改当前固件代码。单线 UART 舵机协议参考：

`/Volumes/External/Servo/ServoD031platformio-v01/UART_BUS_PROTOCOL.md`

当前工程中，高层动作逻辑仍然是 Otto 原库的正弦步态，底层输出目前是 `ESP32Servo` PWM。迁移目标是：**尽量保留 `Otto.cpp` 的动作函数，只替换舵机输出层**。

## 1. 当前 PWM 舵机控制链路

当前命令链路大致如下：

```text
UART/BLE 命令
  -> src/otto_ble.cpp
  -> moveRobot()
  -> Otto::walk() / Otto::turn() / Otto::_moveServos()
  -> Oscillator::SetPosition() / Oscillator::refresh()
  -> Oscillator::write()
  -> ESP32Servo::write()
  -> GPIO6/7/8/9 PWM 输出
```

关键文件：

| 文件 | 当前作用 |
| --- | --- |
| `src/otto_ble.cpp` | 接收 UART/BLE 命令，调用 Otto 高层动作 |
| `src/Otto.cpp` / `src/Otto.h` | 步态、手势、动作编排 |
| `src/Oscillator.cpp` / `src/Oscillator.h` | 正弦振荡器，把目标角度写到 PWM 舵机 |

真正需要替换的是这一层：

```text
Oscillator::write(position)
  -> _servo.write(_pos + _trim)
```

也就是说，不建议先改 `walk()`、`turn()`、`playGesture()` 这些动作函数，而是把 `Oscillator` 的“输出角度到舵机”接口换成 UART 总线舵机驱动。

## 2. 单线 UART 舵机协议要点

附件协议里的核心点：

| 项目 | 协议值 |
| --- | --- |
| 物理层 | 单线半双工 UART |
| 波特率 | 默认 `115200` |
| 数据格式 | 8N1 |
| 总线形式 | 一主多从 |
| 单播 ID | `1~252` |
| 广播 ID | `254` / `0xFE` |
| 下发帧头 | `FF FF` |
| 应答帧头 | `FF F5` |
| 目标角度寄存器 | `0x2A~0x2B`，Int16，大端，范围 `-700~700` |
| 最大速度寄存器 | `0x2C`，Uint8，`0~100` |
| 扭矩开关寄存器 | `0x28`，`0=卸载`，`1=使能` |

常用指令：

| CMD | 名称 | 用途 |
| --- | --- | --- |
| `0x01` | PING | 查询舵机在线 |
| `0x02` | READ | 读寄存器 |
| `0x03` | WRITE | 写寄存器，立即执行 |
| `0x83` | SYNC WRITE | 一帧同时写多个舵机，广播无应答 |

## 3. 推荐硬件连接

当前命令 UART 已占用：

| 功能 | GPIO |
| --- | --- |
| 上位机命令 UART TX | `GPIO10` |
| 上位机命令 UART RX | `GPIO11` |

建议保留这组作为上位机命令通道，另开一个 UART 总线给舵机：

| 功能 | 建议 |
| --- | --- |
| 单线舵机数据线 | 从原 PWM 舵机脚中选一个，例如 `GPIO6` |
| 舵机 ID | 左腿 `1`、右腿 `2`、左脚 `3`、右脚 `4` |
| 舵机供电 | 独立大电流电源，主控与舵机共地 |
| 总线上拉 | 按协议建议 `1kΩ~4.7kΩ` |

迁移后，原来的 `GPIO6/7/8/9` 不再分别输出 PWM；只需要一根 UART 总线数据线即可。剩余 GPIO7/8/9 可留给后续用途。

需要实机确认 ESP32-S3 单线半双工 UART 的实现方式：

1. 优先用 ESP-IDF UART 半双工/RS485 模式，并评估是否支持 TX/RX 同脚或外部单线收发电路。
2. 若 Arduino `HardwareSerial` 不能稳定做同脚半双工，可用 ESP-IDF `uart_driver_install()`、`uart_set_pin()`、`uart_set_mode()` 等底层 API。
3. 若同脚模式不可用，可用 TX/RX 两脚通过外部电路合成单线，或用带方向控制的半双工收发器。

## 4. 软件架构推荐

### 4.1 新增舵机总线驱动层

建议新增两个文件：

```text
src/D031ServoBus.h
src/D031ServoBus.cpp
```

职责：

| 函数 | 作用 |
| --- | --- |
| `begin(uart, pin, baud)` | 初始化单线 UART 总线 |
| `ping(id)` | 查询舵机是否在线 |
| `writeRegister(id, addr, data, len)` | 写寄存器 |
| `readRegister(id, addr, len, out)` | 读寄存器 |
| `enableTorque(id, enable)` | 写 `0x28` 扭矩开关 |
| `writeTarget(id, target, speed)` | 单个舵机写目标角度 `0x2A` 和速度 `0x2C` |
| `syncWriteTargets(ids, targets, speed)` | 用 `SYNC WRITE(0x83)` 同步写 4 个目标 |

驱动层内部负责：

- 组帧：`FF FF ID LEN CMD PARAM CHECKSUM`
- 校验：`CHECKSUM = ~(ID + LEN + CMD + PARAM...) & 0xFF`
- 半双工收发切换
- 单播等待应答
- 广播不等待应答
- 超时和重试

### 4.2 保留 Otto 高层动作

推荐保留：

```text
Otto::walk()
Otto::turn()
Otto::_moveServos()
Otto::oscillateServos()
Otto::playGesture()
```

这些函数仍然继续生成 `0~180` 度的“逻辑角度”。

替换：

```text
Oscillator::attach()
Oscillator::detach()
Oscillator::write()
```

从 PWM 输出改成：

```text
逻辑角度 0~180
  -> 中位换算为 -700~700
  -> D031ServoBus::writeTarget()
  -> UART 总线帧
```

## 5. 角度换算方案

当前 Otto 内部用的是传统舵机角度：

```text
0~180 度，90 为中位
```

D031 单线舵机协议使用：

```text
-700~+700，0 为机械零位
```

建议第一版使用线性换算：

```text
uartTarget = (pwmAngle - 90) * 700 / 90
```

示例：

| Otto 角度 | UART 舵机目标值 |
| --- | --- |
| `0` | `-700` |
| `45` | `-350` |
| `90` | `0` |
| `135` | `350` |
| `180` | `700` |

实际写入时需要加方向和 trim：

```text
logicalAngle = position + trim
centered = logicalAngle - 90
target = centered * 700 / 90
if reversed:
    target = -target
target = constrain(target, -700, 700)
```

建议保留当前 `SetTrim()` 语义：trim 仍然以“度”为单位，先加到 `0~180` 逻辑角度上，再换算为 `-700~700`。

## 6. 舵机 ID 映射

建议把原来的四个 PWM pin 映射改为四个 UART 舵机 ID：

| Otto 下标 | 原含义 | 建议 ID |
| --- | --- | --- |
| `servo[0]` | 左腿 `YL` | `1` |
| `servo[1]` | 右腿 `YR` | `2` |
| `servo[2]` | 左脚 `RL` | `3` |
| `servo[3]` | 右脚 `RR` | `4` |

迁移后，`Otto::init()` 的四个参数不再代表 GPIO，而可以改为舵机 ID：

```cpp
Otto.init(1, 2, 3, 4, false, PIN_BUZZER_DISABLED);
```

为了减少改动，也可以继续沿用变量名 `servo_pins[4]`，但实际含义改成 `servo_ids[4]`。更清晰的做法是重命名为：

```cpp
int servo_ids[4];
```

## 7. 两种实施路线

### 路线 A：最小改动，直接改 `Oscillator`

做法：

1. `Oscillator.h` 移除或条件屏蔽 `Servo _servo`。
2. `Oscillator::attach(int pin, bool rev)` 中的 `pin` 改成 `servoId`。
3. `Oscillator::write(int position)` 不再 `_servo.write()`，而是调用 UART 总线驱动。
4. `detach()` 改为扭矩关闭或空操作。

优点：

- 改动文件少。
- `Otto.cpp` 基本不用动。
- 适合快速跑通。

缺点：

- `attach(pin)` 名字和语义不再准确。
- `Oscillator` 与具体 D031 协议耦合。

### 路线 B：推荐方案，抽象舵机输出层

新增一个轻量输出接口，例如：

```cpp
class ServoOutput {
 public:
  void begin();
  void attach(uint8_t channel, uint8_t id, bool reversed);
  void detach(uint8_t channel);
  void writeAngle(uint8_t channel, int angleDeg, int trimDeg);
  void syncFlush();
};
```

然后 `Oscillator` 只负责算角度，输出交给 `ServoOutput`。

优点：

- 以后可以在 PWM、UART 舵机之间切换。
- 后续接其它总线舵机更容易。
- 便于单独测试协议层。

缺点：

- 改动比路线 A 多。
- 需要重新梳理 `Oscillator` 与输出层之间的关系。

建议：第一阶段用路线 A 快速验证，第二阶段再整理成路线 B。

## 8. `walk()` 的 UART 控制策略

当前 PWM 版本的 `walk()` 依赖 `Oscillator::refresh()`：

```text
每约 30ms 采样一次 sin 曲线
  -> 计算 4 路舵机角度
  -> 立即写 PWM
```

这个模式适合普通 PWM 舵机，因为 PWM 舵机只是持续接收目标脉宽。但 D031 UART 舵机内部有 PID、最大速度和软启动逻辑，如果主控每 30ms 不断改目标角度，可能导致下位机的轨迹规划反复被打断，表现为：

- 速度忽快忽慢；
- 动作不够平滑；
- 舵机一直追逐变化目标，内部软启动难以完整发挥；
- 总线写入频率高，占用 UART 带宽。

所以 UART 方案下，不建议把 `Oscillator::refresh()` 直接改成“每 30ms 发一帧 UART 目标角度”。

### 8.1 低频 sin 采样方案

如果仍想保留 Otto 原有 sin 曲线外形，可以把原来的高频采样改成低频采样：

```text
原 PWM：T / 30ms，约 50 帧（T=1500ms）
UART：每周期 8~16 帧
```

推荐初始值：

| 周期 T | 推荐帧数 | 单帧间隔 |
| --- | --- | --- |
| `800ms` | `8` | `100ms` |
| `1000ms` | `8~10` | `100~125ms` |
| `1500ms` | `12` | `125ms` |
| `2000ms` | `16` | `125ms` |

计算方式可以是：

```text
frameDt = 100~150ms
frameCount = constrain(T / frameDt, 8, 16)
```

每一帧仍然按 sin 算 4 路目标，但不是 30ms 一次，而是约 100~150ms 一次：

```text
phase = 2π * frameIndex / frameCount
pos = A * sin(phase + phase0) + O + 90
```

然后用一次 `SYNC WRITE` 同步写 4 路舵机。

### 8.2 关键帧步态方案

也可以完全不使用 sin，而是直接设计一组目标姿态：

```text
重心右移 -> 左腿前摆 -> 左脚落地 -> 重心左移 -> 右腿前摆 -> 右脚落地
```

这种做法更像“有限状态机步态”：

```text
每一帧 = 4 个舵机目标角度 + 速度 + 持续时间
```

优点：

- 更适合 UART 智能舵机；
- 可以显式设计重心转移；
- 更容易做出“正常迈步”的感觉；
- 不需要维持连续 30ms 目标流。

缺点：

- 需要实机调角度；
- 帧太少会像折线动作；
- 帧间速度和切换时机要调好，否则会顿挫。

建议第一版 walk 可以从 6~8 个关键帧开始，后续再根据实机稳定性增加到 8~12 帧。

### 8.3 切帧策略：固定节拍，不等到位

UART 舵机关键帧控制最大的问题是“什么时候切下一帧”。

不能简单等舵机完全到目标再切，因为这样会造成：

```text
到目标 -> 停住等待 -> 下一帧
```

走路时这种停顿很明显，尤其脚部和重心切换会变僵硬。

推荐策略：

```text
固定时间节拍切帧
不等待完全到位
让下一帧目标提前接管
```

也就是：

```text
发 frame 0
等待 frameDt
发 frame 1
等待 frameDt
发 frame 2
...
```

关键是速度不要太快，不能让舵机明显早于 `frameDt` 到达目标后停住。更合理的是：

```text
预计到达时间 = frameDt * arrivalRatio
arrivalRatio = 1.2~1.5
```

例如：

```text
frameDt = 150ms
预计到达时间 = 180~225ms
```

这样每 150ms 发下一帧时，舵机通常还在运动中，目标点始终“跑在前面”，轨迹会更连续。

### 8.4 是否读取当前位置来切帧

协议支持读取当前角度：

```text
READ 0x38~0x39
```

但第一版不建议依赖“读到位后切帧”，原因：

- 读取 4 路角度会占用总线；
- 若某一路卡住，会拖慢整体步态；
- 舵机位置到位不代表脚已经稳定落地；
- 等到完全到位容易造成停顿。

更推荐把位置反馈作为调试手段：

```text
每帧后低频读取当前角度
打印误差
用于调整 frameDt、speed、关键帧幅度
```

而不是作为第一版 walk 的主控制闭环。

## 9. 同步写策略

无论采用低频 sin 采样还是关键帧步态，都应优先用 **广播 `SYNC WRITE(0x83)`** 同步写 4 路目标。

原因：

1. 4 路舵机应尽量同时收到目标；
2. 广播 `SYNC WRITE` 无应答，减少阻塞；
3. 比逐个单播更适合步态相位控制。

推荐每一帧下发：

```text
ID1: targetAngle + speed
ID2: targetAngle + speed
ID3: targetAngle + speed
ID4: targetAngle + speed
```

即一帧：

```text
FF FF FE LEN 83 2A 03
  ID1 angle_H angle_L speed
  ID2 angle_H angle_L speed
  ID3 angle_H angle_L speed
  ID4 angle_H angle_L speed
CS
```

`0x2A` 是目标角度起始寄存器，`03` 表示每个舵机写 3 字节：目标角度 2 字节 + 最大速度 1 字节。

实现位置建议：

- 不要在每个 `Oscillator::write()` 里立即发 UART；
- 应在更高层收集 4 路目标后统一 `syncWriteTargets()`；
- 对 walk 来说，最好在新的 `walk` 调度函数里按帧统一发送。

## 10. 速度控制策略

D031 协议有最大速度寄存器：

```text
0x2C 最大速度，0~100%
```

第一版建议：

- `G`、`S`、`home()`：速度可以用 `50~80`，避免冲击；
- `walk()` 低频采样/关键帧：速度用 `80~100` 起步，再按实机调整；
- 不要让舵机明显早于下一帧到达，否则会停顿；
- 也不要太慢，否则下一帧来时仍落后很多，动作幅度会变小。

更合理的速度估算：

```text
delta = abs(nextTarget - currentTarget)
targetArrivalTime = frameDt * 1.2~1.5
speed = f(delta / targetArrivalTime)
```

由于 `0x2C` 是百分比速度，不是精确物理速度，第一版可以先用固定速度调通，再通过实测建立经验映射。

对于不同动作：

| 动作类型 | 推荐策略 |
| --- | --- |
| `G` 直控 | 一次 `SYNC WRITE`，速度中等 |
| `S` / `home()` | 一次 `SYNC WRITE` 回 0，速度中等，保持扭矩 |
| `M walk` | 固定节拍切帧，不等到位 |
| 手势动作 | 可先沿用关键帧/目标姿态，逐步替换 `_moveServos()` |

## 11. attach / detach / home 的语义变化

PWM 里：

```text
attach = 开始输出 PWM
detach = 停止 PWM，舵机可能卸力
```

UART 舵机里建议改成：

| 当前函数 | UART 舵机建议语义 |
| --- | --- |
| `attach()` | 记录舵机 ID，必要时 PING，写 `0x28=1` 使能扭矩 |
| `detach()` | 可写 `0x28=0` 卸载，或第一版先空操作 |
| `home()` | 写 4 路目标角度 `0`，可保持扭矩 |

是否在 `home()` 后卸载扭矩要实机决定：

- 若卸载，机器人省电但可能站不住。
- 若保持扭矩，站立更稳但舵机持续发热耗电。

建议第一版：`home()` 后**不要自动卸载扭矩**，避免机器人倒下。另加一个专门命令或内部函数控制卸载。

## 12. 命令 UART 与舵机 UART 的关系

当前 `GPIO10/11` 是上位机命令 UART，协议是 ASCII：

```text
M 1 1500 10
G 90 90 90 90
S
```

单线舵机 UART 是二进制帧：

```text
FF FF ID LEN CMD PARAM CHECKSUM
```

两者不要混在同一个 UART 上。建议：

| 通道 | 用途 |
| --- | --- |
| UART1 `GPIO10/11` | 上位机命令与 ACK |
| 另一个 UART 或 ESP-IDF UART | D031 舵机总线 |

这样外部调试、BLE/UART 命令协议都可以保留不变。

## 13. 推荐迁移步骤

### 第一步：单独验证 D031 总线

先不要接 Otto 动作，只写最小测试：

1. 初始化舵机 UART 总线。
2. PING ID 1~4。
3. 广播使能扭矩：写 `0x28=1`。
4. 单个舵机写目标角度：

```text
ID1 -> 0
ID1 -> 300
ID1 -> -300
ID1 -> 0
```

5. 读取当前角度 `0x38`，确认反馈范围和方向。

### 第二步：接入 `G` 命令

先让现有命令：

```text
G 90 90 90 90
G 110 90 90 90
```

通过 UART 舵机移动。

这是最容易验证的入口，因为 `G` 直接调用 `_moveServos()`，不涉及复杂步态。

### 第三步：接入 `home()`

确认：

```text
S
M 0 1000 15
```

能让 4 路舵机回到机械零位。

### 第四步：接入 `M` 动作

测试慢速小幅动作：

```text
M 1 1500 10
M 2 1500 10
M 3 1500 10
M 4 1500 10
```

重点观察：

- 四路动作是否同步。
- 是否有明显延迟。
- 是否丢帧或抖动。
- 角度方向是否需要反相。

### 第五步：改为 `SYNC WRITE`

若单播写动作不同步，再把步态刷新改为：

```text
计算 4 路目标
  -> 一帧 SYNC WRITE
```

这一步是步态效果的关键。

## 14. 推荐新增/修改文件

建议新增：

```text
src/D031ServoBus.h
src/D031ServoBus.cpp
```

建议修改：

```text
src/Oscillator.h
src/Oscillator.cpp
src/Otto.h
src/Otto.cpp
src/otto_ble.cpp
```

修改重点：

| 文件 | 改动 |
| --- | --- |
| `D031ServoBus.*` | 实现 D031 二进制协议 |
| `Oscillator.*` | 把 PWM 写角度改成 UART 写目标 |
| `Otto.*` | `servo_pins` 改语义为 `servo_ids`，必要时改同步写入口 |
| `otto_ble.cpp` | PWM GPIO 定义改为舵机 ID 定义，新增舵机总线 UART pin |

## 15. 需要重点验证的问题

| 问题 | 验证方式 |
| --- | --- |
| ESP32-S3 单线半双工是否稳定 | 示波器看 TX/RX 切换和应答帧 |
| 舵机 ID 是否正确 | PING ID 1~4 |
| 角度方向是否一致 | 单独给每路发送 `+300/-300` |
| 中位是否正确 | `G 90 90 90 90` 是否站正 |
| 同步性是否足够 | `M 1 1500 10` 观察四路动作相位 |
| ACK 是否影响主协议 | 确保舵机总线二进制帧不进入命令 UART |
| 动作结束是否站得住 | 决定 `home()` 后是否卸载扭矩 |

## 16. 风险点

1. **半双工时序风险**：协议要求主机发完后 `20~50us` 切接收，ESP32 Arduino 封装未必精确，需要必要时使用 ESP-IDF UART API。
2. **动作阻塞风险**：当前 Otto 动作本身是阻塞式；如果每个角度都等待单播应答，会进一步变慢。
3. **同步性风险**：四路舵机逐个单播可能导致步态相位不一致，应优先考虑 `SYNC WRITE`。
4. **角度标定风险**：PWM 的 `90°` 不一定等于 UART 舵机的 `0`，需要 trim 和方向表。
5. **扭矩/供电风险**：UART 舵机保持力矩时电流更集中，必须独立供电并共地。

## 17. 推荐最终结构

理想完成后的结构：

```text
上位机 / BLE
  -> ASCII 命令协议不变
  -> otto_ble.cpp
  -> Otto 高层动作不变
  -> Oscillator 只负责生成角度
  -> D031ServoBus 负责角度换算、组帧、半双工 UART
  -> 单线 UART 总线
  -> ID1/2/3/4 舵机
```

这样改完后，外部控制命令仍然可以继续使用：

```text
S
G 90 90 90 90
M 1 1500 10
H 1
```

只是底层从 `GPIO6/7/8/9 PWM` 变为 `单线 UART 总线 + 舵机 ID 1/2/3/4`。
