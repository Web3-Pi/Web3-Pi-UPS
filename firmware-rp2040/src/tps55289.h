// Minimal TPS55289 Buck-Boost controller helper for RP2040 (Arduino)
// Based on the provided MicroPython reference. Some conversions need calibration.

#pragma once

#include <Arduino.h>
#include <Wire.h>

class TPS55289 {
public:
  static constexpr uint8_t DefaultAddress = 0x75;

  TPS55289() : _wire(nullptr), _addr(DefaultAddress), _intfbBits(0b11), _intfbVal(0.0564f) {}

  bool begin(TwoWire &wire, uint8_t address = DefaultAddress) {
    _wire = &wire;
    _addr = address;
    return ping();
  }

  bool ping() {
    if (!_wire) return false;
    _wire->beginTransmission(_addr);
    return (_wire->endTransmission() == 0);
  }

  // Voltage setup (internal feedback)
  // stepSize: 2.5, 5.0, 7.5, 10.0 (mV per LSB at internal divider)
  void setStepSize(float stepSize_mV) {
    if      (fabs(stepSize_mV - 2.5f)  < 0.01f) { _intfbBits = 0b00; _intfbVal = 0.2256f; }
    else if (fabs(stepSize_mV - 5.0f)  < 0.01f) { _intfbBits = 0b01; _intfbVal = 0.1128f; }
    else if (fabs(stepSize_mV - 7.5f)  < 0.01f) { _intfbBits = 0b10; _intfbVal = 0.0752f; }
    else /*10.0*/                               { _intfbBits = 0b11; _intfbVal = 0.0564f; }

    uint8_t vout_fs = readReg8(VOUT_FS_ADDR);
    vout_fs &= ~0b11;            // clear INTFB bits [1:0]
    vout_fs |= _intfbBits;       // set step size
    vout_fs &= ~(1u << 7);       // FB_MODE=0 -> internal feedback
    writeReg8(VOUT_FS_ADDR, vout_fs);
  }

  // Internal feedback mode (bit7=0)
  void setFeedbackInternal() {
    uint8_t vout_fs = readReg8(VOUT_FS_ADDR);
    vout_fs &= ~(1u << 7);
    writeReg8(VOUT_FS_ADDR, vout_fs);
  }

  // Set output voltage (V) using internal feedback mapping from the reference code
  // Valid approx range: 0.8V..22V (datasheet). Requires proper step size to be set.
  void setOutputVoltage(float volts) {
    setFeedbackInternal();
    // Default to 10mV step unless changed by caller
    setStepSize(10.0f);
    float VREF = volts * _intfbVal;          // as per reference
    int VREF_code = (int)(1.7715f * ((VREF * 1000.0f) - 45.0f)) + 1; // ref formula
    if (VREF_code < 0) VREF_code = 0;
    uint8_t lsb = (uint8_t)(VREF_code & 0xFF);
    uint8_t msb = (uint8_t)((VREF_code >> 8) & 0x07);
    writeReg8(REF_VOLTAGE_LSB_ADDR, lsb);
    writeReg8(REF_VOLTAGE_MSB_ADDR, msb);
  }

  // Enable/Disable output via MODE bit7 (OE)
  void enable(bool on = true) {
    uint8_t mode = readReg8(MODE_ADDR);
    if (on) mode |=  (1u << 7);
    else    mode &= ~(1u << 7);
    writeReg8(MODE_ADDR, mode);
  }

  // Optional operating modes
  void setHiccupMode(bool enableHiccup) {
    uint8_t mode = readReg8(MODE_ADDR);
    if (enableHiccup) mode |=  (1u << 5);
    else              mode &= ~(1u << 5);
    writeReg8(MODE_ADDR, mode);
  }

  void setFPWM(bool fpwm) {
    uint8_t mode = readReg8(MODE_ADDR);
    if (fpwm) mode |=  (1u << 1);
    else      mode &= ~(1u << 1);
    writeReg8(MODE_ADDR, mode);
  }

  void setVoutDischarge(bool enableDischarge) {
    uint8_t mode = readReg8(MODE_ADDR);
    if (enableDischarge) mode |=  (1u << 4);
    else                 mode &= ~(1u << 4);
    writeReg8(MODE_ADDR, mode);
  }

  // Current limit - enable bit and raw code writer (mapping requires datasheet)
  void enableCurrentLimit(bool enable) {
    uint8_t iout = readReg8(IOUT_LIMIT_ADDR);
    if (enable) iout |=  (1u << 7);
    else        iout &= ~(1u << 7);
    writeReg8(IOUT_LIMIT_ADDR, iout);
  }

  // Set the current-limit code (lower 7 bits). Caller must provide calibrated code.
  void setCurrentLimitCode(uint8_t code7bit) {
    uint8_t iout = readReg8(IOUT_LIMIT_ADDR);
    iout &= ~(0x7F);           // clear bits 6..0
    iout |= (code7bit & 0x7F);
    writeReg8(IOUT_LIMIT_ADDR, iout);
  }

  // Convenience: attempt rough mapping from Amps using shunt 10mΩ (example only)
  // NOTE: This is a placeholder; replace with proper mapping per datasheet.
  void setCurrentLimitAmps(float amps) {
    enableCurrentLimit(true);
    // Placeholder linear map -> user must adjust. Clamp 0..6.35A to 0..127
    if (amps < 0) amps = 0; if (amps > 6.35f) amps = 6.35f;
    uint8_t code = (uint8_t)roundf((amps / 6.35f) * 127.0f);
    setCurrentLimitCode(code);
  }

  // Undervoltage: provide a simple UVLO enable/disable placeholder (needs mapping)
  // Returns false as not implemented without datasheet register details
  bool setUVLOThresholdVolts(float /*volts*/) {
    // TODO: implement using proper register once mapping is provided
    return false;
  }

  // Read status register (0x07)
  uint8_t readStatus() { return readReg8(STATUS_ADDR); }

  // Low-level access
  void writeReg8(uint8_t reg, uint8_t value) {
    if (!_wire) return;
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->write(value);
    _wire->endTransmission();
  }

  uint8_t readReg8(uint8_t reg) {
    if (!_wire) return 0;
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    if (_wire->endTransmission(false) != 0) return 0;
    _wire->requestFrom(_addr, (uint8_t)1);
    if (_wire->available()) return _wire->read();
    return 0;
  }

private:
  // Registers
  static constexpr uint8_t REF_VOLTAGE_LSB_ADDR  = 0x00;
  static constexpr uint8_t REF_VOLTAGE_MSB_ADDR  = 0x01;
  static constexpr uint8_t IOUT_LIMIT_ADDR       = 0x02;
  static constexpr uint8_t VOUT_SR_ADDR          = 0x03;
  static constexpr uint8_t VOUT_FS_ADDR          = 0x04;
  static constexpr uint8_t CDC_ADDR              = 0x05;
  static constexpr uint8_t MODE_ADDR             = 0x06;
  static constexpr uint8_t STATUS_ADDR           = 0x07;

  TwoWire *_wire;
  uint8_t _addr;
  uint8_t _intfbBits;  // INTFB selection bits
  float   _intfbVal;   // scaling used by provided reference
};
