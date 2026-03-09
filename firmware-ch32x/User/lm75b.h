#ifndef LM75B_H
#define LM75B_H

#include "debug.h"

#ifdef __cplusplus
extern "C" {
#endif

// I2C address (A0-A2 pins determine lower 3 bits, typically 0x48-0x4F)
#define LM75B_I2C_ADDR 0x48

// Registers
#define LM75B_REG_TEMP 0x00  // Temperature register (read-only)
#define LM75B_REG_CONF 0x01  // Configuration register
#define LM75B_REG_THYST 0x02 // Hysteresis register
#define LM75B_REG_TOS 0x03   // Overtemperature shutdown threshold

// API
int16_t lm75b_read_temp_raw(void);
float lm75b_read_temp_c(void);

#ifdef __cplusplus
}
#endif

#endif
