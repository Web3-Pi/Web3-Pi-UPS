#include "debug.h"
#include "i2c_lib.h"

// Forward declarations
void I2C_SDA_HIGH(void);
void I2C_SDA_LOW(void);
void I2C_SCL_HIGH(void);
void I2C_SCL_LOW(void);

//------------------------------------------------------------------------------
void I2C_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    UINT8 i;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
//PC18 PC19
//port I2C - input, release SCL/SDA
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_18|GPIO_Pin_19;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    // Bus recovery: 9 clock pulses to release any stuck slave
    I2C_SDA_HIGH();
    for (i = 0; i < 9; i++) {
        I2C_SCL_HIGH();
        I2C_SCL_LOW();
    }
    // Generate STOP condition to reset bus state
    I2C_SDA_LOW();
    Delay_Us(5);
    I2C_SCL_HIGH();
    Delay_Us(5);
    I2C_SDA_HIGH();
    Delay_Us(10);
}
//------------------------------------------------------------------------------
void I2C_SDA_HIGH(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
//port I2C - input, release SDA
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBPD, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_19;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
}
//------------------------------------------------------------------------------
void I2C_SDA_LOW(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
//port I2C - output, pull SDA low
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBPD, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_19;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
    GPIO_WriteBit(GPIOC, GPIO_Pin_19, 0);
}
//------------------------------------------------------------------------------
void I2C_SCL_HIGH(void)
{
	Delay_Us(10);
    GPIO_InitTypeDef GPIO_InitStructure = {0};
//port I2C - input, release SCL
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBPD, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_18;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
}
//------------------------------------------------------------------------------
void I2C_SCL_LOW(void)
{
	Delay_Us(10);
    GPIO_InitTypeDef GPIO_InitStructure = {0};
//port I2C - output, pull SCL low
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBPD, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_18;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
    GPIO_WriteBit(GPIOC, GPIO_Pin_18, 0);
}
//------------------------------------------------------------------------------
void I2C_CLOCKOUT(void)
{
	I2C_SCL_HIGH();
	I2C_SCL_LOW();
}
//------------------------------------------------------------------------------
void I2C_write(UINT8 data)
{
  UINT8 i;
  for(i=8; i; i--, data<<=1)
  {
	  // transmit 8 bits, MSB first
    (data & 0x80) ? (I2C_SDA_HIGH()) : (I2C_SDA_LOW());  // SDA HIGH if bit is 1
    I2C_CLOCKOUT();                         // clock out -> slave reads the bit
  }
  I2C_SDA_HIGH();                           // release SDA for ACK bit of slave
  I2C_CLOCKOUT();                           // 9th clock pulse is for the ignored ACK bit
}
//------------------------------------------------------------------------------
void I2C_start(UINT8 addr)
{
  // Works for both initial start and repeated start
  I2C_SDA_HIGH();                           // Release SDA first
  I2C_SCL_HIGH();                           // Then release SCL (idle state)
  Delay_Us(5);
  I2C_SDA_LOW();                            // Start condition: SDA falls while SCL is HIGH
  Delay_Us(5);
  I2C_SCL_LOW();                            // Then pull SCL LOW
  I2C_write(addr);                          // send slave address
}
//------------------------------------------------------------------------------
void I2C_stop(void)
{
  I2C_SDA_LOW();                            // prepare SDA for LOW to HIGH transition
  I2C_SCL_HIGH();                           // stop condition: SCL goes HIGH first
  I2C_SDA_HIGH();                           // stop condition: SDA goes HIGH second
}
//------------------------------------------------------------------------------
void I2C_write_reg(UINT8 addr, UINT8 reg, UINT8 dat)
{
  I2C_start(addr << 1);
  I2C_write(reg);
  I2C_write(dat);
  I2C_stop();
}
//------------------------------------------------------------------------------
UINT8 I2C_SDA_READ(void)
{
  return GPIO_ReadInputDataBit(GPIOC, GPIO_Pin_19);
}
//------------------------------------------------------------------------------
UINT8 I2C_read(UINT8 ack)
{
  UINT8 i, data = 0;
  I2C_SDA_HIGH();
  for (i = 8; i; i--) {
    data <<= 1;
    I2C_SCL_HIGH();
    Delay_Us(5);
    if (I2C_SDA_READ())
      data |= 1;
    I2C_SCL_LOW();
  }
  if (ack)
    I2C_SDA_LOW();
  else
    I2C_SDA_HIGH();
  I2C_CLOCKOUT();
  I2C_SDA_HIGH();
  return data;
}
//------------------------------------------------------------------------------
UINT8 I2C_read_reg(UINT8 addr, UINT8 reg)
{
  UINT8 data;
  I2C_start(addr << 1);
  I2C_write(reg);
  Delay_Us(50);
  I2C_start((addr << 1) | 1);
  data = I2C_read(0);
  I2C_stop();
  return data;
}
//------------------------------------------------------------------------------
void I2C_read_regs(UINT8 addr, UINT8 reg, UINT8 *buf, UINT8 len)
{
  UINT8 i;
  I2C_start(addr << 1);
  I2C_write(reg);
  Delay_Us(50);
  I2C_start((addr << 1) | 1);
  for (i = 0; i < len; i++) {
    buf[i] = I2C_read(i < (len - 1) ? 1 : 0);
  }
  I2C_stop();
}
//------------------------------------------------------------------------------
UINT16 I2C_read_reg16(UINT8 addr, UINT8 reg)
{
  UINT8 lsb, msb;
  I2C_start(addr << 1);
  I2C_write(reg);
  Delay_Us(50);
  I2C_start((addr << 1) | 1);
  lsb = I2C_read(1);
  msb = I2C_read(0);
  I2C_stop();
  return ((UINT16)msb << 8) | lsb;
}
//------------------------------------------------------------------------------

