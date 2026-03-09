#ifndef I2CLIB_H
#define I2CLIB_H

#ifdef __cplusplus
extern "C" {
#endif

void I2C_init(void);
void I2C_write_reg(UINT8 addr, UINT8 reg, UINT8 dat);
UINT8 I2C_read_reg(UINT8 addr, UINT8 reg);
void I2C_read_regs(UINT8 addr, UINT8 reg, UINT8 *buf, UINT8 len);
UINT16 I2C_read_reg16(UINT8 addr, UINT8 reg);

#ifdef __cplusplus
}
#endif

#endif
