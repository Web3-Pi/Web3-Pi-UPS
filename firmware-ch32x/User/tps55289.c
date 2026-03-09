#include <math.h>
#include "debug.h"

#include "i2c_lib.h"
#include "tps55289.h"

// Cached set values (in 0.1V and 0.1A units)
static int16_t g_voltage_set_v10 = 0;
static int16_t g_current_set_a10 = 0;

// Tabela INTFB → współczynnik VREF/VOUT
static const float INTFB_OPTIONS[4] =
{
    0.2256f,   // 00 -> max ~5 V
    0.1128f,   // 01 -> max ~10 V
    0.0752f,   // 10 -> max ~15 V
    0.0564f    // 11 -> max ~20 V
};

// -----------------------------------------------------------------------------
//  Funkcje pomocnicze
// -----------------------------------------------------------------------------
void tps55289_write_byte(UINT8 reg, UINT8 value)
{
    I2C_write_reg(TPS55289_I2C_ADDR, reg, value);
}

void write_word_lsb_first(UINT8 reg_lsb, UINT16 value)
{
    UINT8 lsb = value & 0xFF;
    UINT8 msb = (value >> 8) & 0x07;   // 11-bit

    I2C_write_reg(TPS55289_I2C_ADDR, reg_lsb, lsb);
    I2C_write_reg(TPS55289_I2C_ADDR, reg_lsb + 1, msb);
}
// -----------------------------------------------------------------------------
//  Inicjalizacja
// -----------------------------------------------------------------------------
void tps55289_init(void)
{
    tps55289_write_byte(REG_VOUT_SR, 0x01);
    tps55289_set_cdc_compensation(CDC_COMP_0V7);  // Enable max cable droop compensation
    tps55289_enable_output(0);
}
// -----------------------------------------------------------------------------
//  CDC Cable Voltage Droop Compensation (0-7)
//  Preserves SC_MASK, OCP_MASK, OVP_MASK bits (7:5), CDC_OPTION=0 (internal)
// -----------------------------------------------------------------------------
void tps55289_set_cdc_compensation(UINT8 level)
{
    if (level > 7) level = 7;
    // Bits 7:5 = 111 (enable SC/OCP/OVP indication)
    // Bit 4 = 0 (reserved)
    // Bit 3 = 0 (CDC_OPTION: internal compensation)
    // Bits 2:0 = CDC level
    UINT8 reg_val = 0xE0 | (level & 0x07);
    tps55289_write_byte(REG_CDC, reg_val);
}
// -----------------------------------------------------------------------------
//  Włączanie/wyłączanie wyjścia
// -----------------------------------------------------------------------------
void tps55289_enable_output(UINT8 enable)
{
    if (enable)
    {
        tps55289_write_byte(REG_MODE, MODE_OE_BIT);
	}
    else
    {
        tps55289_write_byte(REG_MODE, 0x00);
	}
}
// -----------------------------------------------------------------------------
//  Ustawianie limitu prądowego 0–6.35 A (TPS55289 max 8A)
// -----------------------------------------------------------------------------
UINT8 tps55289_set_current_limit(float amps)
{
    if (amps < 0.0f || amps > 6.35f)
        return -1;

    //int reg_val = (int)roundf(amps * IOUT_LSB_PER_AMP); // 0..100
    int reg_val = (int) (amps * IOUT_LSB_PER_AMP); // 0..100

    if (reg_val > 0x7F)
        reg_val = 0x7F;

    reg_val |= 0x80;    // enable bit

    tps55289_write_byte(REG_IOUT_LIMIT, (UINT8)reg_val);
    g_current_set_a10 = (int16_t)(amps * 10.0f);
    return 0;
}
// -----------------------------------------------------------------------------
//  Ustawianie napięcia wyjściowego 3.3–20 V
// -----------------------------------------------------------------------------
UINT8 tps55289_set_voltage(float vout)
{
    if (vout < 3.3f || vout > 20.0f)
        return -1;

    UINT8 intfb_bits;

/*
    if (vout <= 5.0f)
        intfb_bits = 0;
    else if (vout <= 10.0f)
        intfb_bits = 1;
    else if (vout <= 15.0f)
        intfb_bits = 2;
    else
        intfb_bits = 3;
*/
//    float ratio = INTFB_OPTIONS[intfb_bits];

	intfb_bits = 0x03;
	float ratio = 0.0564f;
    float vref = vout * ratio;

    if (vref < REF_MIN_V || vref > REF_MAX_V)
        return -1;

    float reg_float = (vref - REF_MIN_V) / LSB_REF_VOLTAGE;
    //int reg = (int)roundf(reg_float);
    reg_float += 0.5f;		//round
    int reg = (int) (reg_float);

    if (reg < 0) reg = 0;
    if (reg > 0x7FF) reg = 0x7FF;

    UINT8 vout_fs_val = intfb_bits & 0x03;

    tps55289_write_byte(REG_VOUT_FS, vout_fs_val);
	write_word_lsb_first(REG_REF_LSB, (UINT16)reg);
	g_voltage_set_v10 = (int16_t)(vout * 10.0f);
	return 0;
}
// -----------------------------------------------------------------------------
//  Get cached set values (in 0.1V and 0.1A units)
// -----------------------------------------------------------------------------
int16_t tps55289_get_voltage_set_v10(void) { return g_voltage_set_v10; }
int16_t tps55289_get_current_set_a10(void) { return g_current_set_a10; }
// -----------------------------------------------------------------------------
//  Read back configured voltage from I2C registers
// -----------------------------------------------------------------------------
float tps55289_read_voltage(void)
{
    UINT8 lsb = I2C_read_reg(TPS55289_I2C_ADDR, REG_REF_LSB);
    UINT8 msb = I2C_read_reg(TPS55289_I2C_ADDR, REG_REF_MSB);
    UINT8 vout_fs = I2C_read_reg(TPS55289_I2C_ADDR, REG_VOUT_FS);

    UINT16 reg = ((UINT16)(msb & 0x07) << 8) | lsb;
    UINT8 intfb_bits = vout_fs & 0x03;

    float ratio = INTFB_OPTIONS[intfb_bits];
    float vref = REF_MIN_V + (reg * LSB_REF_VOLTAGE);
    float vout = vref / ratio;

    return vout;
}
// -----------------------------------------------------------------------------
//  Read back configured current limit from I2C register
// -----------------------------------------------------------------------------
float tps55289_read_current_limit(void)
{
    UINT8 reg = I2C_read_reg(TPS55289_I2C_ADDR, REG_IOUT_LIMIT);
    UINT8 val = reg & 0x7F; // Lower 7 bits are the value
    float amps = (float)val / IOUT_LSB_PER_AMP;
    return amps;
}
// -----------------------------------------------------------------------------
