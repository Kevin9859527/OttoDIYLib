#ifndef D031ServoBus_h
#define D031ServoBus_h

#include <Arduino.h>

class D031ServoBus {
 public:
  static constexpr uint8_t kBroadcastId = 0xFE;

  bool begin(HardwareSerial& serial, int ioPin, uint32_t baud);

  bool ping(uint8_t id, uint8_t* status = nullptr);
  bool enableTorque(uint8_t id, bool enable, uint8_t* status = nullptr);
  bool writeTarget(uint8_t id, int16_t target, uint8_t speed, uint8_t* status = nullptr);
  bool readPosition(uint8_t id, int16_t& position, uint8_t* status = nullptr);

 private:
  struct StatusFrame {
    uint8_t id = 0;
    uint8_t status = 0;
    uint8_t params[16] = {};
    size_t paramLen = 0;
  };

  HardwareSerial* serial_ = nullptr;
  uint32_t baud_ = 115200;
  int ioPin_ = -1;

  static uint8_t calcChecksum(uint8_t id, uint8_t len, uint8_t cmdOrStatus, const uint8_t* params, size_t paramLen);
  bool sendCommand(uint8_t id, uint8_t cmd, const uint8_t* params, size_t paramLen, bool expectResponse, StatusFrame* outFrame);
  bool readStatusFrame(uint8_t expectedId, StatusFrame& outFrame, uint32_t timeoutMs);
  bool readByteWithTimeout(uint8_t& out, uint32_t timeoutMs);
  void flushRx();
};

#endif
