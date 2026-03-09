// MP2762A Charger helper (I2C)
// Minimal class wrapper around example helpers to work with TwoWire on RP2040

#pragma once

#include <Arduino.h>
#include <Wire.h>

class MP2762A {
public:
  static constexpr uint8_t DefaultAddress = 0x5C;

  MP2762A() : _wire(nullptr), _addr(DefaultAddress) {}

  // Initialize with I2C bus and optional address. Returns true if device ACKs.
  bool begin(TwoWire &wire, uint8_t address = DefaultAddress) {
    _wire = &wire;
    _addr = address;
    return i2cIsDevicePresent(wire, address);
  }

  // Reset all registers to defaults
  void registerReset() {
    uint8_t status = readRegister(MP2762A_CONFIG_0);
    status |= (1u << 7); // REG_RST
    writeRegister(MP2762A_CONFIG_0, status);
  }

  // Reset safety timer (write 0 then 1 to REG08H, bit[4])
  void resetSafetyTimer() {
    uint8_t status = readRegister(MP2762A_CONFIG_0);
    status &= ~(1u << 4);
    writeRegister(MP2762A_CONFIG_0, status);

    status |= (1u << 4);
    writeRegister(MP2762A_CONFIG_0, status);
  }

  // Returns: 0b00 - Not charging, 01 - trickle/precharge, 10 - fast charge, 11 - charge termination
  uint8_t getChargeStatus() {
    uint8_t status = readRegister(MP2762A_STATUS);
    status >>= 2; // bits[3:2]
    status &= 0b11;
    return status;
  }

  // Battery voltage in millivolts. Register is 16-bit with data in bits[15:6], LSB=12.5mV
  uint16_t getBatteryVoltageMv() {
    uint16_t raw = readRegister16(MP2762A_BATTERY_VOLTAGE);
    raw >>= 6; // bits 15:6 carry the value
    float mv = convertBitsToDoubler(raw, 12.5f);
    return static_cast<uint16_t>(mv + 0.5f);
  }

  // Set precharge current (mA)
  void setPrechargeCurrentMa(uint16_t currentLevelMa) {
    uint8_t newIPre = 0b0011; // 180mA default
    if (currentLevelMa > 180) {
      uint8_t steps = (uint8_t)((currentLevelMa - 240) / 60); // (480-240)/60 = 4
      newIPre = static_cast<uint8_t>(0b0101 + steps);
    }

    uint8_t reg = readRegister(MP2762A_PRECHARGE_CURRENT);
    reg &= ~(0b1111 << 4);   // clear bits 7..4
    reg |= (uint8_t)(newIPre << 4);
    writeRegister(MP2762A_PRECHARGE_CURRENT, reg);
  }

  // Set fast charge voltage (mV) using PRECHARGE_THRESHOLD registers/options
  void setFastChargeVoltageMv(uint16_t mVoltLevel) {
    uint8_t option = 1;          // option 2 (per example/comment)
    uint8_t newVbattPre = 0b01;  // 6.8V default

    if (mVoltLevel <= 5800) { option = 0; newVbattPre = 0b00; } // 5.8V
    else if (mVoltLevel <= 6000) { option = 0; newVbattPre = 0b01; } // 6.0V
    else if (mVoltLevel <= 6200) { option = 0; newVbattPre = 0b10; } // 6.2V
    else if (mVoltLevel <= 6400) { option = 0; newVbattPre = 0b11; } // 6.4V
    else if (mVoltLevel <= 6600) { option = 1; newVbattPre = 0b00; } // 6.6V
    else if (mVoltLevel <= 6800) { option = 1; newVbattPre = 0b01; } // 6.8V
    else if (mVoltLevel <= 7200) { option = 1; newVbattPre = 0b11; } // 7.2V
    else if (mVoltLevel <= 7400) { option = 1; newVbattPre = 0b10; } // 7.4V

    uint8_t reg = readRegister(MP2762A_PRECHARGE_THRESHOLD);
    reg &= ~(0b11 << 4);
    reg |= (uint8_t)(newVbattPre << 4);
    writeRegister(MP2762A_PRECHARGE_THRESHOLD, reg);

    reg = readRegister(MP2762A_PRECHARGE_THRESHOLD_OPTION);
    reg &= ~(1u << 3);
    reg |= (uint8_t)(option << 3);
    writeRegister(MP2762A_PRECHARGE_THRESHOLD_OPTION, reg);
  }

  // Low-level helpers
  static bool i2cIsDevicePresent(TwoWire &wire, uint8_t address) {
    wire.beginTransmission(address);
    return (wire.endTransmission() == 0);
  }

  uint8_t readRegister(uint8_t reg) {
    if (!_wire) return 0;
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    if (_wire->endTransmission(false) != 0) {
      Serial.println(F("MP2762 error: did not ACK (write addr)"));
    }

    _wire->requestFrom(_addr, (uint8_t)1);
    if (_wire->available()) {
      return _wire->read();
    }
    Serial.println(F("MP2762 error: no response"));
    return 0;
  }

  uint16_t readRegister16(uint8_t reg) {
    if (!_wire) return 0;
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    if (_wire->endTransmission(false) != 0) {
      Serial.println(F("MP2762 error: did not ACK (write addr16)"));
      return 0;
    }

    _wire->requestFrom(_addr, (uint8_t)2);
    if (_wire->available() >= 2) {
      uint8_t lsb = _wire->read();
      uint8_t msb = _wire->read();
      return (uint16_t)msb << 8 | lsb; // little endian
    }
    Serial.println(F("MP2762 error: no response 16"));
    return 0;
  }

  bool writeRegister(uint8_t reg, uint8_t value) {
    if (!_wire) return false;
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->write(value);
    if (_wire->endTransmission() != 0) {
      Serial.println(F("MP2762 error: did not ACK write"));
      return false;
    }
    return true;
  }

private:
  // Given a bit field and a starting bit value, sum as powers of two
  static float convertBitsToDoubler(uint16_t bitField, float startingBitValue) {
    float totalMv = 0;
    for (int i = 0; i < 16; ++i) {
      if (bitField & 0x0001) totalMv += startingBitValue;
      bitField >>= 1;
      startingBitValue *= 2.0f;
    }
    return totalMv;
  }

  // Register map
  static constexpr uint8_t MP2762A_PRECHARGE_CURRENT          = 0x03;
  static constexpr uint8_t MP2762A_PRECHARGE_THRESHOLD        = 0x07;
  static constexpr uint8_t MP2762A_CONFIG_0                   = 0x08;
  static constexpr uint8_t MP2762A_CONFIG_1                   = 0x09;
  static constexpr uint8_t MP2762A_STATUS                     = 0x13;
  static constexpr uint8_t MP2762A_FAULT_REGISTER             = 0x14;
  static constexpr uint8_t MP2762A_BATTERY_VOLTAGE            = 0x16;
  static constexpr uint8_t MP2762A_CHARGE_CURRENT             = 0x1A;
  static constexpr uint8_t MP2762A_INPUT_VOLTAGE              = 0x1C;
  static constexpr uint8_t MP2762A_INPUT_CURRENT              = 0x1E;
  static constexpr uint8_t MP2762A_PRECHARGE_THRESHOLD_OPTION = 0x30;

  TwoWire *_wire;
  uint8_t _addr;
};
