#include "D031ServoBus.h"

#include <driver/gpio.h>
#include <esp_rom_gpio.h>       // esp_rom_gpio_connect_out/in_signal
#include <soc/gpio_struct.h>    // extern gpio_dev_t GPIO; — GPIO.pin[n].pad_driver
#include <soc/uart_periph.h>    // uart_periph_signal[], UART_PERIPH_SIGNAL()

namespace {
constexpr uint8_t kCmdPing = 0x01;
constexpr uint8_t kCmdRead = 0x02;
constexpr uint8_t kCmdWrite = 0x03;
constexpr uint8_t kRegTorqueEnable = 0x28;
constexpr uint8_t kRegTargetAngle = 0x2A;
constexpr uint8_t kRegCurrentAngle = 0x38;
constexpr uint32_t kDefaultResponseTimeoutMs = 20;
// 必须与 otto_ble.cpp 里 HardwareSerial ServoBusSerial(N) 的 N 一致
constexpr int kServoBusUartNr = 2;
}  // namespace

bool D031ServoBus::begin(HardwareSerial& serial, int ioPin, uint32_t baud) {
  serial_ = &serial;
  ioPin_ = ioPin;
  baud_ = baud;
  const gpio_num_t pin = static_cast<gpio_num_t>(ioPin_);

  // Step 1: 正常 UART 初始化（推挽 TX，GPIO Matrix 连接建立）
  serial_->begin(baud_, SERIAL_8N1, ioPin_, ioPin_);

  // Step 2: 显式重连 GPIO Matrix（防止某些 arduino-esp32 版本在 begin 后路由丢失）
  // 只修改矩阵路由，不触碰 pad drive 模式
  esp_rom_gpio_connect_out_signal(static_cast<uint32_t>(ioPin_),
      static_cast<uint32_t>(UART_PERIPH_SIGNAL(kServoBusUartNr, SOC_UART_TX_PIN_IDX)),
      false, false);
  esp_rom_gpio_connect_in_signal(static_cast<uint32_t>(ioPin_),
      static_cast<uint32_t>(UART_PERIPH_SIGNAL(kServoBusUartNr, SOC_UART_RX_PIN_IDX)),
      false);

  // Step 3: 设置开漏模式（直接写寄存器 pad_driver 位，无任何副作用）
  // gpio_set_direction() 在某些版本会意外断开 GPIO Matrix，故不用。
  // pad_driver=1：TX=0 拉低，TX=1 释放为高阻 → 外部 1kΩ 上拉恢复高电平
  //              → 从机可拉低总线发应答（FF F5 ...）
  GPIO.pin[ioPin_].pad_driver = 1;

  // Step 4: 使能内部上拉（外部必须有 1kΩ~4.7kΩ 上拉才能可靠通信）
  gpio_pullup_en(pin);

  delay(2);
  flushRx();
  return true;
}

bool D031ServoBus::ping(uint8_t id, uint8_t* status) {
  StatusFrame frame;
  if (!sendCommand(id, kCmdPing, nullptr, 0, true, &frame)) {
    return false;
  }
  if (status != nullptr) {
    *status = frame.status;
  }
  return true;
}

bool D031ServoBus::enableTorque(uint8_t id, bool enable, uint8_t* status) {
  uint8_t params[2] = {kRegTorqueEnable, static_cast<uint8_t>(enable ? 1 : 0)};
  const bool expectResponse = (id != kBroadcastId);
  StatusFrame frame;
  if (!sendCommand(id, kCmdWrite, params, sizeof(params), expectResponse, expectResponse ? &frame : nullptr)) {
    return false;
  }
  if (expectResponse && status != nullptr) {
    *status = frame.status;
  }
  return true;
}

bool D031ServoBus::writeTarget(uint8_t id, int16_t target, uint8_t speed, uint8_t* status) {
  uint8_t params[4] = {
      kRegTargetAngle,
      static_cast<uint8_t>((target >> 8) & 0xFF),
      static_cast<uint8_t>(target & 0xFF),
      speed,
  };
  const bool expectResponse = (id != kBroadcastId);
  StatusFrame frame;
  if (!sendCommand(id, kCmdWrite, params, sizeof(params), expectResponse, expectResponse ? &frame : nullptr)) {
    return false;
  }
  if (expectResponse && status != nullptr) {
    *status = frame.status;
  }
  return true;
}

bool D031ServoBus::readPosition(uint8_t id, int16_t& position, uint8_t* status) {
  uint8_t params[2] = {kRegCurrentAngle, 0x02};
  StatusFrame frame;
  if (!sendCommand(id, kCmdRead, params, sizeof(params), true, &frame)) {
    return false;
  }
  if (frame.paramLen < 2) {
    return false;
  }
  position = static_cast<int16_t>((static_cast<uint16_t>(frame.params[0]) << 8) | frame.params[1]);
  if (status != nullptr) {
    *status = frame.status;
  }
  return true;
}

uint8_t D031ServoBus::calcChecksum(uint8_t id, uint8_t len, uint8_t cmdOrStatus, const uint8_t* params, size_t paramLen) {
  uint8_t sum = static_cast<uint8_t>(id + len + cmdOrStatus);
  for (size_t i = 0; i < paramLen; ++i) {
    sum = static_cast<uint8_t>(sum + params[i]);
  }
  return static_cast<uint8_t>((~sum) & 0xFF);
}

bool D031ServoBus::sendCommand(
    uint8_t id,
    uint8_t cmd,
    const uint8_t* params,
    size_t paramLen,
    bool expectResponse,
    StatusFrame* outFrame) {
  if (serial_ == nullptr) {
    return false;
  }

  const uint8_t len = static_cast<uint8_t>(paramLen + 2);  // CMD + PARAM + CHECKSUM
  uint8_t frame[32] = {};
  size_t idx = 0;
  frame[idx++] = 0xFF;
  frame[idx++] = 0xFF;
  frame[idx++] = id;
  frame[idx++] = len;
  frame[idx++] = cmd;
  for (size_t i = 0; i < paramLen; ++i) {
    frame[idx++] = params[i];
  }
  frame[idx++] = calcChecksum(id, len, cmd, params, paramLen);

  flushRx();
  serial_->write(frame, idx);
  serial_->flush();
  // 发完后留一点 turnaround，再读 FF F5（从机约 75us 后起发）
  if (expectResponse) {
    delayMicroseconds(200);
  }

  if (!expectResponse) {
    return true;
  }
  if (outFrame == nullptr) {
    return false;
  }

  return readStatusFrame(id, *outFrame, kDefaultResponseTimeoutMs);
}

bool D031ServoBus::readStatusFrame(uint8_t expectedId, StatusFrame& outFrame, uint32_t timeoutMs) {
  uint8_t b = 0;
  uint8_t prev = 0;
  const uint32_t deadline = millis() + timeoutMs;

  while (millis() <= deadline) {
    const uint32_t remaining = deadline - millis();
    if (remaining == 0) {
      break;
    }
    if (!readByteWithTimeout(b, remaining)) {
      continue;
    }
    if (prev == 0xFF && b == 0xF5) {
      uint8_t id = 0;
      uint8_t len = 0;
      const uint32_t frameDeadline = millis() + timeoutMs;
      auto remainingForFrame = [&]() -> uint32_t {
        const uint32_t now = millis();
        return (now >= frameDeadline) ? 0 : (frameDeadline - now);
      };
      if (!readByteWithTimeout(id, remainingForFrame()) || !readByteWithTimeout(len, remainingForFrame())) {
        return false;
      }
      if (len < 2) {
        return false;
      }

      uint8_t payload[24] = {};
      if (len > sizeof(payload)) {
        return false;
      }
      for (uint8_t i = 0; i < len; ++i) {
        if (!readByteWithTimeout(payload[i], remainingForFrame())) {
          return false;
        }
      }

      const uint8_t status = payload[0];
      const size_t paramLen = static_cast<size_t>(len - 2);
      const uint8_t* params = &payload[1];
      const uint8_t rxChecksum = payload[len - 1];
      const uint8_t calc = calcChecksum(id, len, status, params, paramLen);
      if (rxChecksum != calc) {
        return false;
      }
      if (id != expectedId) {
        return false;
      }

      outFrame.id = id;
      outFrame.status = status;
      outFrame.paramLen = paramLen;
      for (size_t i = 0; i < paramLen && i < sizeof(outFrame.params); ++i) {
        outFrame.params[i] = params[i];
      }
      return true;
    }
    prev = b;
  }
  return false;
}

bool D031ServoBus::readByteWithTimeout(uint8_t& out, uint32_t timeoutMs) {
  if (serial_ == nullptr || timeoutMs == 0) {
    return false;
  }
  const uint32_t start = millis();
  while ((millis() - start) <= timeoutMs) {
    if (serial_->available() > 0) {
      out = static_cast<uint8_t>(serial_->read());
      return true;
    }
    yield();
  }
  return false;
}

void D031ServoBus::flushRx() {
  if (serial_ == nullptr) {
    return;
  }
  while (serial_->available() > 0) {
    serial_->read();
  }
}
