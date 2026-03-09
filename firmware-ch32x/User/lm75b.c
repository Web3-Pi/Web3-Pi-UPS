#include "lm75b.h"
#include "i2c_lib.h"

// Read raw temperature value (11-bit, 0.125C resolution)
int16_t lm75b_read_temp_raw(void) {
  UINT8 buf[2];
  I2C_read_regs(LM75B_I2C_ADDR, LM75B_REG_TEMP, buf, 2);
  // Temperature is in MSB:LSB format, 11-bit value in bits [15:5]
  int16_t raw = ((int16_t)buf[0] << 8) | buf[1];
  return raw >> 5; // Shift to get 11-bit signed value
}

// Read temperature in Celsius
float lm75b_read_temp_c(void) {
  int16_t raw = lm75b_read_temp_raw();
  return raw * 0.125f; // Each LSB = 0.125C
}
