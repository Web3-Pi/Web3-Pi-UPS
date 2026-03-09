#ifndef TPS55289_H
#define TPS55289_H

#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
//  Adres I2C (zależny od pinu MODE)
// -----------------------------------------------------------------------------
#define TPS55289_I2C_ADDR   0x75

// -----------------------------------------------------------------------------
//  Rejestry
// -----------------------------------------------------------------------------
#define REG_REF_LSB         0x00
#define REG_REF_MSB         0x01
#define REG_IOUT_LIMIT      0x02
#define REG_VOUT_SR         0x03
#define REG_VOUT_FS         0x04
#define REG_CDC             0x05
#define REG_MODE            0x06
#define REG_STATUS          0x07

// -----------------------------------------------------------------------------
//  BITY
// -----------------------------------------------------------------------------
#define MODE_OE_BIT     (1 << 7)

// -----------------------------------------------------------------------------
//  PARAMETRY TPS55289
// -----------------------------------------------------------------------------
#define LSB_REF_VOLTAGE   (0.0005645)   // 0.5645 mV
#define REF_MIN_V         (0.045)
#define REF_MAX_V         (1.129)

#define RSENSE_OHMS       (0.010)       // 10mΩ
#define IOUT_LSB_PER_AMP  (20)          // 1A => 20 LSB

// -----------------------------------------------------------------------------
//  CDC Compensation levels (voltage rise at 50mV V_ISP-V_ISN)
// -----------------------------------------------------------------------------
#define CDC_COMP_0V0  0  // 000b = 0.0V (disabled)
#define CDC_COMP_0V1  1  // 001b = 0.1V
#define CDC_COMP_0V2  2  // 010b = 0.2V
#define CDC_COMP_0V3  3  // 011b = 0.3V
#define CDC_COMP_0V4  4  // 100b = 0.4V
#define CDC_COMP_0V5  5  // 101b = 0.5V
#define CDC_COMP_0V6  6  // 110b = 0.6V
#define CDC_COMP_0V7  7  // 111b = 0.7V (max)

// -----------------------------------------------------------------------------
//  API
// -----------------------------------------------------------------------------

void tps55289_init(void);
void tps55289_enable_output(UINT8 enable);
UINT8 tps55289_set_current_limit(float amps);
UINT8 tps55289_set_voltage(float vout);
void tps55289_set_cdc_compensation(UINT8 level);

// Get cached set values (in 0.1V and 0.1A units)
int16_t tps55289_get_voltage_set_v10(void);
int16_t tps55289_get_current_set_a10(void);

// Read back configured values from I2C
float tps55289_read_voltage(void);
float tps55289_read_current_limit(void);

extern void I2C_write_reg(UINT8 addr, UINT8 reg, UINT8 dat);

#ifdef __cplusplus
}
#endif

#endif