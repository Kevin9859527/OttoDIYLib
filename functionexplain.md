# H 命令与 playGesture 功能说明

本文档解释当前 BLE 固件中 `H` 命令、`receiveGesture()`、`Otto.playGesture()` 之间的关系，以及 `playGesture()` 内部各小模块分别控制什么硬件。

## H 命令调用链

BLE 收到命令后，`src/otto_ble.cpp` 中的 `handleCommand()` 会按命令首字母分发。

当收到：

```text
H <gestureId>
```

会进入 `receiveGesture()`。

`receiveGesture()` 的流程是：

1. `sendAck()`：返回开始执行 ACK。
2. `Otto.home()`：先让机器人回到中位姿态。
3. 解析 `gestureId`。
4. 根据编号调用 `Otto.playGesture(...)`。
5. `sendFinalAck()`：返回完成 ACK。

当前 `H` 命令编号对应关系：

| H 编号 | 调用的 gesture | 含义 |
| --- | --- | --- |
| `H 1` | `OttoHappy` | 开心 |
| `H 2` | `OttoSuperHappy` | 超级开心 |
| `H 3` | `OttoSad` | 伤心 |
| `H 4` | `OttoSleeping` | 睡觉 |
| `H 5` | `OttoFart` | 放屁/搞怪 |
| `H 6` | `OttoConfused` | 困惑 |
| `H 7` | `OttoLove` | 爱心 |
| `H 8` | `OttoAngry` | 生气 |
| `H 9` | `OttoFretful` | 烦躁 |
| `H 10` | `OttoMagic` | 魔法动画 |
| `H 11` | `OttoWave` | 挥手/波浪动画 |
| `H 12` | `OttoVictory` | 胜利 |
| `H 13` | `OttoFail` | 失败 |

## 当前 GPIO 对应关系

`playGesture()` 内的小模块最终会控制以下 IO：

| 模块类型 | 主要函数 | 控制对象 | 当前 GPIO |
| --- | --- | --- | --- |
| 舵机动作 | `_moveServos()`、`home()`、`swing()`、`tiptoeSwing()`、`crusaito()` 等 | 4 路 PWM 舵机 | 左腿 GPIO4、右腿 GPIO5、左脚 GPIO6、右脚 GPIO7 |
| 点阵表情 | `putMouth()`、`putAnimationMouth()`、`clearMouth()` | MAX7219 8x8 点阵 | DIN GPIO11、CS GPIO12、CLK GPIO13 |
| 蜂鸣器声音 | `_tone()`、`bendTones()`、`sing()` | 蜂鸣器 | GPIO16 |
| BLE ACK | `sendAck()`、`sendFinalAck()` | BLE notify，另有 USB 串口日志 | 不占用普通 GPIO |
| 延时/状态 | `delay()`、变量判断 | 只影响流程时序 | 不直接控制 IO |

注意：`playGesture()` 里的舵机动作仍是 PWM 舵机开环控制。代码只发送角度命令，不读取舵机真实位置反馈。

## 小模块分类

### 表情控制模块

这些函数控制 MAX7219 点阵，使用 GPIO11、GPIO12、GPIO13：

| 函数 | 作用 |
| --- | --- |
| `putMouth(mouth)` | 显示一个预设嘴型，比如 `smile`、`happyOpen`、`sad`、`heart`、`angry` |
| `putAnimationMouth(anim, index)` | 显示动画表中的某一帧，比如 `dreamMouth`、`adivinawi`、`wave` |
| `clearMouth()` | 清空点阵 |

常见嘴型：

| 嘴型 | 含义 |
| --- | --- |
| `smile` | 微笑 |
| `happyOpen` | 开心张嘴 |
| `happyClosed` | 开心闭嘴 |
| `sad` / `sadOpen` / `sadClosed` | 伤心 |
| `heart` | 爱心 |
| `angry` | 生气 |
| `confused` | 困惑 |
| `lineMouth` | 横线嘴 |
| `tongueOut` | 吐舌 |
| `xMouth` | X 嘴，失败/错误 |
| `smallSurprise` / `bigSurprise` | 惊讶 |

### 动作控制模块

这些函数控制 4 个舵机，使用 GPIO4、GPIO5、GPIO6、GPIO7：

| 函数 | 作用 |
| --- | --- |
| `home()` | 四个舵机回到 90 度中位，并 detach 舵机 |
| `_moveServos(time, positions)` | 在指定时间内把 4 个舵机移动到目标角度 |
| `swing(steps, T, h)` | 左右摆动 |
| `tiptoeSwing(steps, T, h)` | 踮脚左右摆动 |
| `crusaito(steps, T, h, dir)` | 交叉舞步 |
| `detachServos()` | 释放舵机控制信号 |

`gesturePOSITION[4]` 的顺序是：

| 数组下标 | 舵机 | 当前 GPIO |
| --- | --- | --- |
| `gesturePOSITION[0]` | 左腿 `LeftLeg` | GPIO4 |
| `gesturePOSITION[1]` | 右腿 `RightLeg` | GPIO5 |
| `gesturePOSITION[2]` | 左脚 `LeftFoot` | GPIO6 |
| `gesturePOSITION[3]` | 右脚 `RightFoot` | GPIO7 |

### 声音控制模块

这些函数控制蜂鸣器，使用 GPIO16：

| 函数 | 作用 |
| --- | --- |
| `_tone(freq, duration, silent)` | 播放一个固定频率的短音 |
| `bendTones(initFreq, finalFreq, prop, duration, silent)` | 从一个频率滑到另一个频率，形成滑音 |
| `sing(songName)` | 播放预设音效，比如开心、困惑、放屁、连接音等 |

## 各手势内部模块拆解

### `H 1` / `OttoHappy`

表情控制：

- `putMouth(smile)`
- `putMouth(happyOpen)`

动作控制：

- `swing(1, 800, 20)`
- `home()`

声音控制：

- `_tone(note_E5, 50, 30)`
- `sing(S_happy_short)` 两次

IO 使用：

- 点阵 GPIO11/12/13
- 舵机 GPIO4/5/6/7
- 蜂鸣器 GPIO16

### `H 2` / `OttoSuperHappy`

表情控制：

- `putMouth(happyOpen)`
- `putMouth(happyClosed)`

动作控制：

- `tiptoeSwing(1, 500, 20)` 两次
- `home()`

声音控制：

- `sing(S_happy)`
- `sing(S_superHappy)`

IO 使用：

- 点阵 GPIO11/12/13
- 舵机 GPIO4/5/6/7
- 蜂鸣器 GPIO16

### `H 3` / `OttoSad`

表情控制：

- `putMouth(sad)`
- `putMouth(sadClosed)`
- `putMouth(sadOpen)`
- 最后 `putMouth(happyOpen)`

动作控制：

- `_moveServos(700, {110, 70, 20, 160})`
- `home()`

声音控制：

- 多段 `bendTones()`，频率逐步下降，模拟伤心下滑音

IO 使用：

- 点阵 GPIO11/12/13
- 舵机 GPIO4/5/6/7
- 蜂鸣器 GPIO16

### `H 4` / `OttoSleeping`

表情控制：

- `putAnimationMouth(dreamMouth, 0/1/2)` 循环播放梦境动画
- `putMouth(lineMouth)`
- 最后 `putMouth(happyOpen)`

动作控制：

- `_moveServos(700, {100, 80, 60, 120})`
- `home()`

声音控制：

- 多段 `bendTones()`，模拟睡觉/呼吸音
- `sing(S_cuddly)`

IO 使用：

- 点阵 GPIO11/12/13
- 舵机 GPIO4/5/6/7
- 蜂鸣器 GPIO16

### `H 5` / `OttoFart`

表情控制：

- `putMouth(lineMouth)`
- `putMouth(tongueOut)`
- 最后 `putMouth(happyOpen)`

动作控制：

- `_moveServos(500, {90, 90, 145, 122})`
- `_moveServos(500, {90, 90, 80, 122})`
- `_moveServos(500, {90, 90, 145, 80})`
- `home()`

声音控制：

- `sing(S_fart1)`
- `sing(S_fart2)`
- `sing(S_fart3)`

IO 使用：

- 点阵 GPIO11/12/13
- 舵机 GPIO4/5/6/7
- 蜂鸣器 GPIO16

### `H 6` / `OttoConfused`

表情控制：

- `putMouth(confused)`
- 最后 `putMouth(happyOpen)`

动作控制：

- `_moveServos(300, {110, 70, 90, 90})`
- `home()`

声音控制：

- `sing(S_confused)`

IO 使用：

- 点阵 GPIO11/12/13
- 舵机 GPIO4/5/6/7
- 蜂鸣器 GPIO16

### `H 7` / `OttoLove`

表情控制：

- `putMouth(heart)`
- 最后 `putMouth(happyOpen)`

动作控制：

- `crusaito(2, 1500, 15, 1)`
- `home()`

声音控制：

- `sing(S_cuddly)`
- `sing(S_happy_short)`

IO 使用：

- 点阵 GPIO11/12/13
- 舵机 GPIO4/5/6/7
- 蜂鸣器 GPIO16

### `H 8` / `OttoAngry`

表情控制：

- `putMouth(angry)`
- 最后 `putMouth(happyOpen)`

动作控制：

- `_moveServos(300, {90, 90, 70, 110})`
- `_moveServos(200, {110, 110, 90, 90})`
- `_moveServos(200, {70, 70, 90, 90})`
- `home()`

声音控制：

- `_tone(note_A5, 100, 30)`
- 多段 `bendTones()`，模拟生气音效

IO 使用：

- 点阵 GPIO11/12/13
- 舵机 GPIO4/5/6/7
- 蜂鸣器 GPIO16

### `H 9` / `OttoFretful`

表情控制：

- `putMouth(angry)`
- `putMouth(lineMouth)`
- 最后 `putMouth(happyOpen)`

动作控制：

- 循环 4 次 `_moveServos(100, {90, 90, 90, 110})`
- 每次后 `home()`

声音控制：

- 多段 `bendTones()`，模拟烦躁音效

IO 使用：

- 点阵 GPIO11/12/13
- 舵机 GPIO4/5/6/7
- 蜂鸣器 GPIO16

### `H 10` / `OttoMagic`

表情控制：

- `putAnimationMouth(adivinawi, index)` 循环播放魔法动画
- `clearMouth()`
- 最后 `putMouth(happyOpen)`

动作控制：

- 无明显舵机动作

声音控制：

- 多段 `bendTones()`，频率上升和下降，配合动画

IO 使用：

- 点阵 GPIO11/12/13
- 蜂鸣器 GPIO16
- 不主动控制舵机 GPIO4/5/6/7

### `H 11` / `OttoWave`

表情控制：

- `putAnimationMouth(wave, index)` 循环播放波浪动画
- `clearMouth()`
- 最后 `putMouth(happyOpen)`

动作控制：

- 无明显舵机动作

声音控制：

- 多段 `bendTones()`，配合波浪动画

IO 使用：

- 点阵 GPIO11/12/13
- 蜂鸣器 GPIO16
- 不主动控制舵机 GPIO4/5/6/7

### `H 12` / `OttoVictory`

表情控制：

- `putMouth(smallSurprise)`
- `putMouth(bigSurprise)`
- `putMouth(happyOpen)`
- `putMouth(happyClosed)`
- `clearMouth()`

动作控制：

- 两段循环 `_moveServos(10, pos)`，逐步把脚部舵机从中位抬起再放回
- `tiptoeSwing(1, 500, 20)` 两次
- `home()`

声音控制：

- 循环 `_tone()`，频率逐步上升
- `sing(S_superHappy)`

IO 使用：

- 点阵 GPIO11/12/13
- 舵机 GPIO4/5/6/7
- 蜂鸣器 GPIO16

### `H 13` / `OttoFail`

表情控制：

- `putMouth(sadOpen)`
- `putMouth(sadClosed)`
- `putMouth(confused)`
- `putMouth(xMouth)`
- `clearMouth()`
- 最后 `putMouth(happyOpen)`

动作控制：

- `_moveServos(300, {90, 90, 70, 35})`
- `_moveServos(300, {90, 90, 55, 35})`
- `_moveServos(300, {90, 90, 42, 35})`
- `_moveServos(300, {90, 90, 34, 35})`
- `detachServos()`
- `home()`

声音控制：

- `_tone(900, 200, 1)`
- `_tone(600, 200, 1)`
- `_tone(300, 200, 1)`
- `_tone(150, 2200, 1)`

IO 使用：

- 点阵 GPIO11/12/13
- 舵机 GPIO4/5/6/7
- 蜂鸣器 GPIO16

## 按功能看哪些手势用了哪些硬件

| 手势 | 点阵表情 | 舵机动作 | 蜂鸣器声音 |
| --- | --- | --- | --- |
| `OttoHappy` | 有 | 有 | 有 |
| `OttoSuperHappy` | 有 | 有 | 有 |
| `OttoSad` | 有 | 有 | 有 |
| `OttoSleeping` | 有 | 有 | 有 |
| `OttoFart` | 有 | 有 | 有 |
| `OttoConfused` | 有 | 有 | 有 |
| `OttoLove` | 有 | 有 | 有 |
| `OttoAngry` | 有 | 有 | 有 |
| `OttoFretful` | 有 | 有 | 有 |
| `OttoMagic` | 有 | 无明显舵机动作 | 有 |
| `OttoWave` | 有 | 无明显舵机动作 | 有 |
| `OttoVictory` | 有 | 有 | 有 |
| `OttoFail` | 有 | 有 | 有 |

## 后续改 UART 单线舵机时的影响

如果后续把 PWM 舵机替换成 UART 单线舵机，`playGesture()` 的表情和声音部分不需要重写，仍然对应：

- 点阵：GPIO11/12/13
- 蜂鸣器：GPIO16

需要替换的是动作控制链路：

- `_moveServos()`
- `home()`
- `swing()` / `tiptoeSwing()` / `crusaito()` 等动作函数
- 底层 `Oscillator` 对 `ESP32Servo` 的调用

也就是说，手势的“剧情编排”可以保留，但舵机输出层要从 PWM 角度写入改成 UART 舵机 ID + 目标位置命令。
