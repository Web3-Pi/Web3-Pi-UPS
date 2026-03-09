#ifndef MP2762A_H
#define MP2762A_H

#include "debug.h"

#ifdef __cplusplus
extern "C" {
#endif

// I2C address
#define MP2762A_I2C_ADDR 0x5C

// Register addresses (per datasheet)
#define MP2762A_REG_INPUT_ILIM 0x00  // Input current limit 1
#define MP2762A_REG_INPUT_VLIM 0x01  // Input voltage limit
#define MP2762A_REG_CHG_CURR 0x02    // Charge current setting
#define MP2762A_REG_PRETERM 0x03     // Pre-charge and termination current
#define MP2762A_REG_VBAT_REG 0x04    // Battery-full voltage and recharge threshold
#define MP2762A_REG_CONFIG0 0x08     // Configuration register 0
#define MP2762A_REG_CONFIG1 0x09     // Configuration register 1
#define MP2762A_REG_ADC_CFG 0x0B    // ADC configuration register
#define MP2762A_REG_STATUS 0x13      // Status register
#define MP2762A_REG_FAULT 0x14       // Fault register
#define MP2762A_REG_ADC_VBAT_L 0x16  // Battery voltage ADC LSB
#define MP2762A_REG_ADC_VBAT_H 0x17  // Battery voltage ADC MSB
#define MP2762A_REG_ADC_VSYS_L 0x18  // System voltage ADC LSB
#define MP2762A_REG_ADC_VSYS_H 0x19  // System voltage ADC MSB
#define MP2762A_REG_ADC_ICHG_L 0x1A  // Charge current ADC LSB
#define MP2762A_REG_ADC_ICHG_H 0x1B  // Charge current ADC MSB
#define MP2762A_REG_ADC_VIN_L 0x1C   // Input voltage ADC LSB
#define MP2762A_REG_ADC_VIN_H 0x1D   // Input voltage ADC MSB
#define MP2762A_REG_ADC_IIN_L 0x1E   // Input current ADC LSB
#define MP2762A_REG_ADC_IIN_H 0x1F   // Input current ADC MSB

// REG08H Configuration Register 0 bits
#define MP2762A_REG_RST     0x80  // Bit 7: Register reset
#define MP2762A_WTD_RST     0x40  // Bit 6: Watchdog reset
#define MP2762A_OTG_EN      0x20  // Bit 5: OTG enable
#define MP2762A_CHG_EN      0x10  // Bit 4: Charge enable
#define MP2762A_SUSP_EN     0x08  // Bit 3: Suspend enable
#define MP2762A_NTC_GCOMP   0x04  // Bit 2: NTC/OTG pin select
#define MP2762A_BATTFET_EN  0x02  // Bit 1: Battery FET enable

// REG13H Status register bits
#define MP2762A_BATT_UVLO   0x80  // Bit 7: Battery UVLO
#define MP2762A_VSYS_UV     0x40  // Bit 6: System undervoltage
#define MP2762A_PPM_STAT    0x10  // Bit 4: Power path management status
#define MP2762A_CHG_STAT_MASK 0x0C // Bits 3:2: Charge status
#define MP2762A_ACOK        0x02  // Bit 1: Power good (VIN OK)
#define MP2762A_VSYS_STAT   0x01  // Bit 0: VSYS regulation status

// Charge status values (bits 3:2 of STATUS)
#define MP2762A_CHG_NOT_CHARGING 0x00  // 00: Not charging
#define MP2762A_CHG_TRICKLE      0x04  // 01: Trickle/Pre-charge
#define MP2762A_CHG_FAST         0x08  // 10: Fast charge (CC or CV)
#define MP2762A_CHG_DONE         0x0C  // 11: Charge termination

// REG14H Fault register bits
#define MP2762A_WATCHDOG_FAULT  0x80  // Bit 7: Watchdog fault
#define MP2762A_OTG_FAULT       0x40  // Bit 6: OTG fault
#define MP2762A_CHG_FAULT_MASK  0x30  // Bits 5:4: Charge fault
#define MP2762A_BATT_FAULT      0x08  // Bit 3: Battery OVP
#define MP2762A_NTC_FAULT_MASK  0x07  // Bits 2:0: NTC fault

typedef enum {
  CHG_STATE_NOT_CHARGING = 0,
  CHG_STATE_TRICKLE,
  CHG_STATE_FAST,
  CHG_STATE_DONE
} mp2762a_chg_state_t;

typedef struct {
  mp2762a_chg_state_t chg_state;
  uint8_t power_good;
  uint16_t vin_mv;  // Input voltage in mV
  uint16_t iin_ma;  // Input current in mA
  uint16_t vbat_mv; // Battery voltage in mV
  uint16_t ichg_ma; // Charge current in mA
  uint8_t fault;    // Fault flags
} mp2762a_data_t;

// API
void mp2762a_init(void);
uint8_t mp2762a_read_reg(uint8_t reg);
void mp2762a_write_reg(uint8_t reg, uint8_t value);
uint16_t mp2762a_read_reg16(uint8_t reg);

mp2762a_chg_state_t mp2762a_get_charge_state(void);
uint8_t mp2762a_is_power_good(void);
uint16_t mp2762a_get_input_voltage_mv(void);
uint16_t mp2762a_get_input_current_ma(void);
uint16_t mp2762a_get_battery_voltage_mv(void);
uint16_t mp2762a_get_charge_current_ma(void);
uint8_t mp2762a_get_fault(void);

// Battery/charger recovery functions
uint8_t mp2762a_is_battery_uvlo(void);    // Returns 1 if BATT_UVLO is set
void mp2762a_restart_charging(void);       // Clear faults and toggle CHG_EN to restart charging

// Factory reset - resets all MP2762A registers to IC power-on defaults
void mp2762a_factory_reset(void);

// Watchdog kick + config refresh (call every 2s)
void mp2762a_kick_watchdog(void);

// Battery presence detection (BATTFET-toggle probe when VIN present)
void mp2762a_poll_battery(void);          // Call every 500ms - probes BATT_UVLO with BATTFET toggle
uint8_t mp2762a_is_battery_present(void); // Returns cached debounced state (no I2C)
uint8_t mp2762a_battery_inserted(void);   // Returns 1 once on insertion edge, clears flag

void mp2762a_read_all(mp2762a_data_t *data);

#ifdef __cplusplus
}
#endif

#endif
