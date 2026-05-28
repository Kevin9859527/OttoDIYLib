# 路线 B：抽象舵机输出层迁移实施步骤

本文档用于指导工程师把当前 **PWM 舵机输出** 逐步迁移为 **单线 UART 总线舵机输出**。本次只写实施计划，不修改现有代码。

目标路线采用 `changprotocol.md` 中的 **路线 B：抽象舵机输出层**：

```text
Otto 高层动作
  -> Oscillator 负责角度计算
  -> ServoOutput 抽象输出层
  -> PWM 输出实现 或 D031 UART 总线输出实现
```

核心原则：

- **从第一步开始就用编译宏切换后端**，不要等最后才加宏。
- 默认后端保持 PWM，保证现有固件可编译、可回退。
- 优先验证单线 UART 舵机通信：初始化 UART、`PING`、扭矩使能、目标角度写入。
- 新增 UART 相关函数和文件时尽量独立，尽可能不改原来的 PWM 函数。
- 先验证单个 UART 舵机，再验证 4 个 UART 舵机，最后才接入 Otto 动作。
- `walk` 不照搬 30ms 高频 UART 刷新，后续改成低频 sin 采样或关键帧步态。

## 0. 迁移前确认

### 0.1 当前输入命令协议保持不变

上位机/小智/调试工具仍使用当前 ASCII 命令：

```text
S
G 90 90 90 90
M 1 1500 10
H 1
```

不要把上位机 UART 命令协议和 D031 舵机二进制总线协议混在一起。

| 通道 | 用途 |
| --- | --- |
| UART1 `GPIO10/11` | 上位机命令、ACK、启动日志 |
| 新增舵机 UART | D031 单线半双工总线 |

### 0.2 当前舵机通道定义

高层动作仍按 Otto 的 4 个通道理解：

| 通道 index | 含义 | 当前 PWM GPIO | UART 方案建议 ID |
| --- | --- | --- | --- |
| `0` | 左腿 `YL` | `GPIO6` | `1` |
| `1` | 右腿 `YR` | `GPIO7` | `2` |
| `2` | 左脚 `RL` | `GPIO8` | `3` |
| `3` | 右脚 `RR` | `GPIO9` | `4` |

迁移后，`Otto.init(...)` 的四个参数建议从 GPIO 语义改为“舵机通道 ID”。最终应避免继续命名为 `servo_pins`，但可以分阶段处理。

### 0.3 编译配置注意

当前 `otto_ble.ini` 中 `build_src_filter` 只编译：

```text
otto_ble.cpp
Otto.cpp
Oscillator.cpp
```

后续新增文件时必须同步加入：

```text
ServoOutput.cpp
PwmServoOutput.cpp
D031ServoBus.cpp
D031ServoOutput.cpp
```

否则代码文件存在但不会参与编译。

### 0.4 后端编译宏

从迁移第一步开始就引入后端选择宏：

```text
OTTO_SERVO_BACKEND_PWM
OTTO_SERVO_BACKEND_D031_UART
```

要求：

- 默认只启用 `OTTO_SERVO_BACKEND_PWM`。
- 两个宏不能同时启用。
- PWM 后端代码继续保留，并尽量不修改现有 PWM 函数。
- D031 UART 相关代码全部放在独立文件和 `#ifdef OTTO_SERVO_BACKEND_D031_UART` 路径下。
- 工程师每一步都要分别确认 PWM 后端仍可编译，D031 后端逐步可编译。

建议第一版 `otto_ble.ini` 默认：

```text
-DOTTO_SERVO_BACKEND_PWM
```

需要测试 UART 舵机时再切换为：

```text
-DOTTO_SERVO_BACKEND_D031_UART
```

## 1. 第一步：建立后端编译宏与默认 PWM 路径

### 1.1 目标

先把“后端选择”机制建好，后续所有 UART 改动都走独立宏路径。这样工程师可以大胆新增 D031 UART 文件，而不破坏当前 PWM 固件。

### 1.2 修改文件

```text
otto_ble.ini
src/otto_ble.cpp
src/Oscillator.h
src/Oscillator.cpp
src/Otto.h
src/Otto.cpp
```

### 1.3 宏定义

在 `otto_ble.ini` 的 `build_flags` 中默认启用：

```text
-DOTTO_SERVO_BACKEND_PWM
```

后续测试 UART 舵机时改为：

```text
-DOTTO_SERVO_BACKEND_D031_UART
```

建议在公共头文件或相关 `.cpp` 顶部加保护：

```cpp
#if defined(OTTO_SERVO_BACKEND_PWM) && defined(OTTO_SERVO_BACKEND_D031_UART)
#error "Only one servo backend can be enabled"
#endif

#if !defined(OTTO_SERVO_BACKEND_PWM) && !defined(OTTO_SERVO_BACKEND_D031_UART)
#define OTTO_SERVO_BACKEND_PWM
#endif
```

### 1.4 对 PWM 路径的要求

第一步不要重构 PWM 代码，只加宏保护：

```cpp
#ifdef OTTO_SERVO_BACKEND_PWM
// 现有 ESP32Servo / Servo 相关代码
#endif
```

要求：

- 当前 `Oscillator::attach()`、`detach()`、`write()` 在 PWM 宏下行为不变。
- 当前 `Otto::attachServos()`、`detachServos()` 行为不变。
- 当前 `otto_ble.cpp` 的四个 PWM GPIO 定义暂时保留。

### 1.5 验收标准

- 默认 `OTTO_SERVO_BACKEND_PWM` 编译通过。
- 不接舵机也能看到启动日志。
- 现有 PWM 测试命令仍可用：

```text
S
G 90 90 90 90
M 1 1500 10
```

## 2. 第二步：优先实现 D031 单舵机 UART 通讯验证

### 2.1 目标

在不接入 Otto、不改 PWM 动作函数的前提下，优先验证 D031 单线 UART 舵机基础通信是否成立：

```text
初始化 UART
PING 单个 ID
使能扭矩
写目标角度
读取当前角度
```

这是整个迁移的前置条件。如果单舵机 `PING` 和目标角度都不稳定，不应继续改 `Oscillator` 或 `Otto`。

### 2.2 新增文件

```text
src/D031ServoBus.h
src/D031ServoBus.cpp
```

这些文件只在 UART 后端启用：

```cpp
#ifdef OTTO_SERVO_BACKEND_D031_UART
// D031 UART 总线实现
#endif
```

### 2.3 必须实现的 UART 函数

先只实现协议基础函数，不接动作层：

| 函数 | 作用 |
| --- | --- |
| `begin(...)` | 初始化单线 UART |
| `checksum(...)` | 计算校验 |
| `writeFrame(...)` | 发送 `FF FF ...` 下发帧 |
| `readStatus(...)` | 读取 `FF F5 ...` 应答帧 |
| `ping(id)` | 验证舵机在线 |
| `writeRegister(id, addr, data, len)` | 写寄存器 |
| `readRegister(id, addr, len, out)` | 读寄存器 |
| `enableTorque(id, enable)` | 写 `0x28` 扭矩开关 |
| `writeTarget(id, target, speed)` | 写 `0x2A~0x2C` 目标角度和速度 |

### 2.4 临时测试入口

为了不影响原命令协议，可以先使用编译宏控制一个临时测试函数：

```cpp
#ifdef OTTO_SERVO_BACKEND_D031_UART
void testD031SingleServo();
#endif
```

测试函数只在启动时或收到某个临时调试命令时执行，建议先不要和 `M/G/H` 混在一起。

测试顺序：

```text
1. begin UART
2. ping(1)
3. enableTorque(1, true)
4. writeTarget(1, 0, 50)
5. writeTarget(1, 300, 50)
6. writeTarget(1, -300, 50)
7. writeTarget(1, 0, 50)
8. readRegister(1, 0x38, 2)
```

### 2.5 验收标准

- `OTTO_SERVO_BACKEND_PWM` 编译通过，原 PWM 行为不变。
- `OTTO_SERVO_BACKEND_D031_UART` 编译通过。
- 单个 D031 舵机 `PING ID=1` 有应答。
- `writeTarget(1, 300, 50)` 和 `writeTarget(1, -300, 50)` 能让舵机正反方向运动。
- 能读取当前角度 `0x38~0x39`。
- 总线错误和超时能从 USB/UART 日志看到。

## 3. 第三步：扩展 D031 四舵机基础函数，不接 Otto

### 3.1 目标

在单舵机通信稳定后，继续只在 UART 后端路径下新增 4 舵机相关函数，仍然不改原 PWM 函数，不改 `Oscillator::refresh()`。

### 3.2 新增/扩展文件

```text
src/D031ServoBus.h
src/D031ServoBus.cpp
```

### 3.3 必须新增的 UART 相关函数

| 函数 | 作用 |
| --- | --- |
| `pingAll(ids, count)` | 逐个 PING ID1~4 |
| `enableTorqueAll(ids, count, enable)` | 逐个或广播使能扭矩 |
| `writeTargetsSingle(ids, targets, speeds, count)` | 用单播逐个写 4 路目标，便于调试应答 |
| `syncWriteTargets(ids, targets, speeds, count)` | 用 `SYNC WRITE(0x83)` 一帧写 4 路目标 |
| `readPositions(ids, positions, count)` | 逐个读 `0x38~0x39` 当前角度 |

### 3.4 先验证单播，再验证同步写

测试顺序：

```text
1. pingAll(1,2,3,4)
2. enableTorqueAll(true)
3. 单播 ID1 -> +300 -> 0
4. 单播 ID2 -> +300 -> 0
5. 单播 ID3 -> +300 -> 0
6. 单播 ID4 -> +300 -> 0
7. syncWriteTargets 四路 -> 0
8. syncWriteTargets 四路 -> 小幅姿态
9. readPositions 四路
```

### 3.5 验收标准

- 4 个 ID 都能 PING。
- 单播写目标时能确认每个 ID 的方向和安装含义。
- `SYNC WRITE` 能让 4 路同时动作。
- `readPositions()` 能返回 4 路当前角度。
- PWM 后端仍不受影响。

## 4. 第四步：建立 `ServoOutput` 抽象接口

### 4.1 目标

在 D031 基础通信已验证后，再建立硬件无关的舵机输出接口。此时抽象接口的设计可以基于真实 UART 调试经验，而不是凭空设计。

### 4.2 建议新增文件

```text
src/ServoOutput.h
src/ServoOutput.cpp   （可选，如果全是接口可不需要）
```

### 4.3 接口职责

建议接口表达“4 个逻辑通道写角度”，不要表达“PWM pin”：

```cpp
class ServoOutput {
 public:
  virtual bool begin() = 0;
  virtual void attach(uint8_t channel, int physicalId, bool reversed) = 0;
  virtual void detach(uint8_t channel) = 0;
  virtual void writeAngle(uint8_t channel, int angleDeg, int trimDeg) = 0;
  virtual int getLastAngle(uint8_t channel) const = 0;
  virtual void flush() = 0;
};
```

说明：

| 方法 | 含义 |
| --- | --- |
| `begin()` | 初始化底层硬件 |
| `attach(channel, physicalId, reversed)` | 绑定 Otto 通道到物理对象，PWM 时是 GPIO，UART 时是舵机 ID |
| `detach(channel)` | 释放该通道，PWM 是 detach，UART 可空操作或扭矩关闭 |
| `writeAngle(channel, angleDeg, trimDeg)` | 写 `0~180` 逻辑角度 |
| `getLastAngle(channel)` | 返回最后输出角度 |
| `flush()` | PWM 可空操作，D031 UART 用于 `SYNC WRITE` |

### 4.4 宏要求

接口文件本身可以不带后端宏，但具体实现必须由宏隔离：

```text
PwmServoOutput -> OTTO_SERVO_BACKEND_PWM
D031ServoOutput -> OTTO_SERVO_BACKEND_D031_UART
```

### 4.5 验收标准

- 新增接口文件。
- 不接入 `Otto` / `Oscillator`。
- PWM 后端编译通过。
- D031 后端编译通过。

## 5. 第五步：实现两个输出适配器，但暂不接入 Otto

### 5.1 目标

新增 PWM 和 D031 两个 `ServoOutput` 实现，但先不改 `Oscillator` 和 `Otto`。这样可以继续保持原 PWM 函数稳定，同时让 UART 输出层具备独立测试能力。

### 5.2 建议新增文件

```text
src/PwmServoOutput.h
src/PwmServoOutput.cpp
src/D031ServoOutput.h
src/D031ServoOutput.cpp
```

### 5.3 `PwmServoOutput`

只在 PWM 宏下编译：

```cpp
#ifdef OTTO_SERVO_BACKEND_PWM
// ESP32Servo 封装
#endif
```

行为应尽量对齐当前 `Oscillator`：

| 当前行为 | 适配器行为 |
| --- | --- |
| `_servo.attach(pin)` | `pwmServos[channel].attach(pin)` |
| `_servo.detach()` | `pwmServos[channel].detach()` |
| `_servo.write(_pos + _trim)` | `writeAngle(channel, angleDeg, trimDeg)` |

### 5.4 `D031ServoOutput`

只在 UART 宏下编译：

```cpp
#ifdef OTTO_SERVO_BACKEND_D031_UART
// D031ServoBus 封装
#endif
```

职责：

- 把 `0~180` Otto 逻辑角度转换为 D031 `-700~700`。
- 维护 4 路 `pendingTargets`。
- `writeAngle()` 只更新 pending。
- `flush()` 用 `syncWriteTargets()` 一次发送 4 路目标。

角度换算：

```text
logicalAngle = constrain(angleDeg + trimDeg, 0, 180)
target = (logicalAngle - 90) * 700 / 90
if reversed:
    target = -target
```

### 5.5 验收标准

- PWM 宏下 `PwmServoOutput` 编译通过。
- D031 宏下 `D031ServoOutput` 编译通过。
- 两个适配器暂不接入现有动作，不影响原 PWM 行为。
- D031 适配器可以用临时测试函数直接写 4 路目标并 `flush()`。

## 6. 第六步：用宏接入 `Otto` / `Oscillator`，PWM 路径尽量不动

### 6.1 目标

在 D031 通信、D031 输出适配器都独立验证后，再接入 `Otto` 和 `Oscillator`。接入时必须通过编译宏分支实现，尽量不改原 PWM 函数体。

### 6.2 修改文件

```text
src/Oscillator.h
src/Oscillator.cpp
src/Otto.h
src/Otto.cpp
src/otto_ble.cpp
otto_ble.ini
```

### 6.3 接入原则

PWM 路径保持原函数：

```cpp
#ifdef OTTO_SERVO_BACKEND_PWM
// 当前 Servo _servo、attach(pin)、_servo.write(...) 逻辑尽量原样保留
#endif
```

D031 路径新增独立分支：

```cpp
#ifdef OTTO_SERVO_BACKEND_D031_UART
// 使用 ServoOutput / D031ServoOutput
#endif
```

不要把 `Oscillator::write()` 直接改成只有 UART 逻辑。应保留：

```text
PWM 宏：原 PWM 写法
D031 宏：输出到 D031ServoOutput pendingTargets
```

### 6.4 `Otto.init()` 参数含义

宏下语义不同：

| 宏 | `Otto.init(YL,YR,RL,RR,...)` 四个参数含义 |
| --- | --- |
| `OTTO_SERVO_BACKEND_PWM` | GPIO6/7/8/9 |
| `OTTO_SERVO_BACKEND_D031_UART` | 舵机 ID1/2/3/4 |

第一版可以保留 `servo_pins[4]` 变量名，但必须加注释说明在 D031 宏下它实际是 ID。后续稳定后再改名。

### 6.5 验收标准

- PWM 宏下：现有 PWM 舵机动作和之前一致。
- D031 宏下：`Otto.init(1,2,3,4,...)` 能绑定 4 个 UART 舵机 ID。
- 两个宏不能同时启用。
- 不启用宏时默认回到 PWM。

## 7. 第七步：先接 `G/S/home`，不接 `walk`

### 7.1 目标

先让低风险动作通过 D031 后端工作。不要在这一步处理 `walk()`，也不要让 `Oscillator::refresh()` 以 30ms 频率持续发 UART 帧。

### 7.2 接入范围

先接这些路径：

```text
S -> Otto.home()
G -> Otto._moveServos()
M 0 -> Otto.home()
```

暂不接或暂不验证：

```text
M 1 walk
M 2 backward
M 3/4 turn
H 手势
```

### 7.3 测试顺序

```text
S
G 90 90 90 90
G 110 90 90 90
G 70 90 90 90
G 90 110 90 90
G 90 90 110 90
G 90 90 90 110
G 90 90 90 90
M 0 1000 15
```

### 7.4 验收标准

- 四个舵机 ID 对应正确。
- `G 90 90 90 90` 能回机械零位。
- 单路 `110/70` 测试方向符合预期。
- `S` 和 `M 0` 可以回中位。
- UART 命令 ACK 不受 D031 总线影响。

## 8. 第八步：宏切换回归验证

### 8.1 目标

确认“直接用编译宏切换后端”的机制可靠。每次接入 D031 新能力后，都必须同时回归 PWM 和 D031 两个后端。

### 8.2 PWM 后端回归

启用：

```text
OTTO_SERVO_BACKEND_PWM
```

测试：

```text
pio run -e otto_ble
S
G 90 90 90 90
G 110 90 90 90
M 1 1500 10
```

要求：

- 编译通过。
- 原 PWM 舵机动作不变。
- `ESP32Servo` 依赖仍正常。

### 8.3 D031 后端回归

启用：

```text
OTTO_SERVO_BACKEND_D031_UART
```

测试：

```text
pio run -e otto_ble
启动 UART 总线
PING ID1~4
S
G 90 90 90 90
G 110 90 90 90
M 0 1000 15
```

要求：

- 编译通过。
- `PING ID1~4` 正常。
- `S/G/M0` 正常。
- 上位机命令 UART 的 `&&A%%` / `&&F%%` 不受 D031 总线影响。

### 8.4 验收标准

- 两个宏都能单独编译。
- 两个宏不能同时启用。
- PWM 后端不因为 D031 新代码而退化。
- D031 后端不依赖任何 PWM GPIO 输出。

## 9. 第九步：调整 `_moveServos()` 和手势动作

### 9.1 目标

`_moveServos()` 当前每 10ms 插值一次，适合 PWM，但对 UART 智能舵机可能过密。

第一版不要急着完全优化，只要避免明显高频打断。

### 9.2 推荐策略

把 `_moveServos(time, servo_target)` 从 10ms 插值改为较低频分段：

```text
frameDt = 80~150ms
frames = max(1, time / frameDt)
每帧计算中间角度
每帧 flush 一次 SYNC WRITE
```

如果动作本身是简单回中位，也可以直接：

```text
一次 SYNC WRITE 到目标
等待 time
```

### 9.3 验收标准

测试：

```text
S
H 1
H 3
H 4
```

观察：

- 是否明显顿挫。
- 是否动作过快或过慢。
- 是否需要保留部分插值。

## 10. 第十步：重新实现 UART 版 `walk`

### 10.1 目标

不要复用 `Oscillator::refresh()` 的 30ms 高频输出作为 UART walk 的最终方案。

walk 应采用以下二选一策略：

1. 低频 sin 采样；
2. 关键帧步态。

### 10.2 方案 A：低频 sin 采样 walk

适合先快速继承原 Otto 行为。

流程：

```text
frameDt = 100~150ms
frameCount = constrain(T / frameDt, 8, 16)
for frame in frameCount:
  phase = 2π * frame / frameCount
  计算 4 路 sin 目标角度
  D031ServoOutput 写入 4 路 pendingTargets
  flush() -> SYNC WRITE
  delay(frameDt)
```

注意：

- 固定时间切帧；
- 不读到位再切；
- 速度设置为让舵机略慢于 `frameDt` 到达。

### 10.3 方案 B：关键帧 walk

适合后续做更正常的步态。

示例结构：

```text
frame 0: 中位
frame 1: 重心右移，左脚变轻
frame 2: 左腿前摆，右腿后摆
frame 3: 左脚落地
frame 4: 重心左移，右脚变轻
frame 5: 右腿前摆，左腿后摆
frame 6: 右脚落地
frame 7: 回到连续周期起点
```

每帧内容：

```text
YL, YR, RL, RR, speed, frameDt
```

第一版可先用 6~8 帧，后续根据实机增加到 8~12 帧。

### 10.4 切帧原则

采用：

```text
固定节拍
不等到位
下一帧提前接管
```

不要采用：

```text
等待四个舵机全部到目标后再切下一帧
```

否则会出现到点停顿。

### 10.5 速度原则

```text
targetArrivalTime = frameDt * 1.2~1.5
```

让舵机在下一帧到来时还没完全停稳，避免停顿。

### 10.6 验收标准

测试：

```text
M 1 1500 10
M 2 1500 10
M 3 1500 10
M 4 1500 10
```

观察：

- 是否连续，无明显停顿。
- 四路是否同步。
- 是否站得住。
- 是否需要调整关键帧角度、frameDt、speed。

## 11. 第十一步：增加调试与反馈工具

### 11.1 目标

用反馈调参，但不要第一版就把反馈作为主控制闭环。

### 11.2 建议功能

新增调试命令或编译开关：

```text
读取 ID1~4 当前角度 0x38
打印当前目标、当前角度、误差
打印每帧发送时间
打印总线超时次数
```

### 11.3 验收标准

工程师能通过日志判断：

- 舵机是否追不上目标；
- 是否提前到位停顿；
- 哪一路方向或 trim 不对；
- 总线是否有丢包或超时。

## 12. 第十二步：保留双后端并清理宏边界

### 12.1 目标

D031 UART 方案稳定后，也不建议马上删除 PWM 路径。最终目标是长期保留两个后端，用编译宏切换：

```text
OTTO_SERVO_BACKEND_PWM
OTTO_SERVO_BACKEND_D031_UART
```

这样可以继续用 PWM 后端做回归测试，也方便硬件版本不同的机器人复用同一套高层动作代码。

### 12.2 清理内容

不是删除 PWM，而是清理宏边界：

```text
PWM 专用 include 只出现在 OTTO_SERVO_BACKEND_PWM 分支
D031 专用 include 只出现在 OTTO_SERVO_BACKEND_D031_UART 分支
PIN_LEFT_LEG/PIN_RIGHT_LEG 等命名只在 PWM 分支使用
D031 分支使用 SERVO_ID_LEFT_LEG 等命名
build_src_filter 包含两个后端文件，但文件内部用宏保护
```

### 12.3 验收标准

- `OTTO_SERVO_BACKEND_PWM` 下能继续链接 `ESP32Servo` 并正常运行。
- `OTTO_SERVO_BACKEND_D031_UART` 下不依赖 PWM GPIO 输出。
- 两个宏都能独立编译。
- 两个宏不能同时启用。
- 文档更新 `protocoluart.md`、`changprotocol.md` 和本文件。

## 13. 推荐实施顺序总览

| 阶段 | 目标 | 是否影响动作 |
| --- | --- | --- |
| 1 | 建立后端编译宏，默认 PWM | 否 |
| 2 | 优先验证 D031 单舵机 UART 通讯 | 否，不接 Otto |
| 3 | 扩展 D031 四舵机基础函数 | 否，不接 Otto |
| 4 | 新增 `ServoOutput` 接口 | 否 |
| 5 | 新增 `PwmServoOutput` / `D031ServoOutput` | 否，独立测试 |
| 6 | 用宏接入 `Otto` / `Oscillator` | 是，但 PWM 路径应保持不变 |
| 7 | D031 后端先接 `G/S/home` | 是，低风险动作 |
| 8 | 宏切换回归验证 | 否，验证 PWM 与 D031 双路径 |
| 9 | `_moveServos` 和手势适配 | 是，中风险 |
| 10 | UART 版 `walk` | 是，高风险，重点调试 |
| 11 | 增加反馈调试工具 | 否，辅助调参 |
| 12 | 保留双后端并清理宏边界 | 是，收尾 |

## 14. 每一步提交建议

建议每一步单独提交，便于回滚：

```text
1. add servo backend build macros
2. verify single d031 uart servo
3. add d031 multi-servo bus helpers
4. add servo output abstraction
5. add pwm and d031 output backends
6. route otto oscillator through backend macros
7. drive home and direct servo commands over d031
8. add backend macro regression checks
9. adapt move servos for uart backend
10. implement uart walk scheduler
11. add d031 debug telemetry
12. clean up backend macro boundaries
```

## 15. 当前不建议做的事

- 不建议一上来删除 `ESP32Servo`。
- 不建议不加编译宏就直接替换 PWM 代码。
- 不建议在 D031 单舵机通信未验证前改 `Oscillator` / `Otto`。
- 不建议直接把 `Oscillator::write()` 改成 UART 单播。
- 不建议 `walk()` 继续 30ms 一帧高频发送 UART 目标。
- 不建议等待所有舵机完全到位再切 walk 下一帧。
- 不建议把上位机命令 UART 和 D031 舵机二进制 UART 混用。

## 16. 第一位工程师应从哪里开始

第一位工程师只做前三件事：

1. 建立 `OTTO_SERVO_BACKEND_PWM` / `OTTO_SERVO_BACKEND_D031_UART` 编译宏，默认保持 PWM。
2. 新增 `D031ServoBus`，只做单舵机 UART 初始化、`PING`、扭矩使能、目标角度写入、当前角度读取。
3. 扩展 D031 四舵机基础函数：`pingAll()`、`syncWriteTargets()`、`readPositions()`，但暂不接入 `Otto` / `Oscillator`。

验收命令：

```text
PWM 宏：
pio run -e otto_ble
S / G 90 90 90 90 / M 1 1500 10 仍保持原行为

D031 宏：
pio run -e otto_ble
PING ID=1
ID1 -> 0
ID1 -> +300
ID1 -> -300
ID1 -> 0
READ ID1 current angle
```

第一位工程师完成后，第二位工程师再开始 `ServoOutput` 抽象层和后端适配器，不要一开始就改 `Oscillator::write()` 或 `Otto::walk()`。
