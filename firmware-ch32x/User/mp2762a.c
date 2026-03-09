#include "mp2762a.h"
#include "i2c_lib.h"

// Expected CONFIG0 value (CHG_EN + NTC_GCOMP + BATTFET_EN)
#define MP2762A_CONFIG0_NORMAL (MP2762A_CHG_EN | MP2762A_NTC_GCOMP | MP2762A_BATTFET_EN)

// Battery presence debounce state
static uint8_t g_bat_present_state = 0;
static uint8_t g_bat_present_counter = 0;
static uint8_t g_bat_inserted_flag = 0;
#define BAT_DEBOUNCE_COUNT 3

// Charge current threshold for battery detection when in FAST charge state (mA).
// Without battery: charger charges BATT bus caps with ~10-15mA phantom current, cs stays FAST.
// With battery charging: ci >> 30mA. Full battery: cs transitions to DONE.
#define BAT_CHG_CURRENT_THRESHOLD_MA 30

// Stuck charger restart: when VIN present but no battery detected, the charger
// may be stuck in a non-functional state (e.g., FAST mode with VBAT > VBAT_REG
// from VSYS back-feed). Periodically restarting the charger allows it to properly
// detect a newly inserted battery.
static uint8_t g_stuck_counter = 0;
#define STUCK_RESTART_COUNT 6  // 6 * 500ms = 3s

// Initialize MP2762A for 2S Li-ion battery
void mp2762a_init(void) {
  // Read fault register to clear latched faults
  (void)mp2762a_read_reg(MP2762A_REG_FAULT);

  // REG00H: Input current limit - 2A
  // Bits 6:0 set current, 50mA per bit, 0mA offset
  // 2000mA / 50 = 40 = 0x28
  mp2762a_write_reg(MP2762A_REG_INPUT_ILIM, 0x28);

  // REG01H: Input voltage limit - 4.5V minimum
  // Bits 6:0, 100mV per bit, 0V offset (0V to 12.8V range)
  // 4500mV / 100 = 45 = 0x2D
  mp2762a_write_reg(MP2762A_REG_INPUT_VLIM, 0x2D);

  // REG02H: Charge current - 1A
  // Bits 6:0, 50mA per bit, 0mA offset
  // 1000mA / 50 = 20 = 0x14
  mp2762a_write_reg(MP2762A_REG_CHG_CURR, 0x14);

  // REG04H: Battery-full voltage - 8.1V for 2S (4.05V/cell for longer battery life)
  // Bits 6:1 set voltage, 25mV per bit, 7.425V offset
  // (8100 - 7425) / 25 = 27 = 0x1B, shift left by 1 = 0x36
  mp2762a_write_reg(MP2762A_REG_VBAT_REG, 0x36);

  // REG08H: Enable charger and battery FET
  // CHG_EN (bit 4) + NTC_GCOMP (bit 2) + BATTFET_EN (bit 1) = 0x16
  mp2762a_write_reg(MP2762A_REG_CONFIG0, MP2762A_CONFIG0_NORMAL);

  // REG09H: Termination enabled, watchdog DISABLED, safety timer DISABLED
  // EN_TERM (bit 6) = 1, WTD[1:0] (bits 7,5) = 00, EN_TMR (bit 3) = 0
  mp2762a_write_reg(MP2762A_REG_CONFIG1, 0x44);

  // REG0BH: Enable ADC in battery-only mode (bits 1:0 = 11)
  // Without this, ADC returns 0 when VIN is absent
  uint8_t adc_cfg = mp2762a_read_reg(MP2762A_REG_ADC_CFG);
  mp2762a_write_reg(MP2762A_REG_ADC_CFG, adc_cfg | 0x03);
}

// Reset all MP2762A registers to IC power-on defaults via REG_RST bit.
// After this call, mp2762a_init() must be called to re-apply project settings.
void mp2762a_factory_reset(void) {
  // Set REG_RST (bit 7) in CONFIG0 - resets all registers, bit auto-clears
  mp2762a_write_reg(MP2762A_REG_CONFIG0, MP2762A_REG_RST);
  Delay_Ms(10);

  // Clear any latched faults from the reset
  (void)mp2762a_read_reg(MP2762A_REG_FAULT);

  // Reset software state
  g_bat_present_state = 0;
  g_bat_present_counter = 0;
  g_bat_inserted_flag = 0;
  g_stuck_counter = 0;
}

uint8_t mp2762a_read_reg(uint8_t reg) { return I2C_read_reg(MP2762A_I2C_ADDR, reg); }

void mp2762a_write_reg(uint8_t reg, uint8_t value) { I2C_write_reg(MP2762A_I2C_ADDR, reg, value); }

uint16_t mp2762a_read_reg16(uint8_t reg) { return I2C_read_reg16(MP2762A_I2C_ADDR, reg); }

mp2762a_chg_state_t mp2762a_get_charge_state(void) {
  uint8_t status = mp2762a_read_reg(MP2762A_REG_STATUS);
  uint8_t chg_bits = status & MP2762A_CHG_STAT_MASK;

  switch (chg_bits) {
  case MP2762A_CHG_NOT_CHARGING:
    return CHG_STATE_NOT_CHARGING;
  case MP2762A_CHG_TRICKLE:
    return CHG_STATE_TRICKLE;
  case MP2762A_CHG_FAST:
    return CHG_STATE_FAST;
  case MP2762A_CHG_DONE:
    return CHG_STATE_DONE;
  default:
    return CHG_STATE_NOT_CHARGING;
  }
}

uint8_t mp2762a_is_power_good(void) {
  uint8_t status = mp2762a_read_reg(MP2762A_REG_STATUS);
  return (status & MP2762A_ACOK) ? 1 : 0;
}

// ADC readings - 10-bit values in bits 15:6 (shift right by 6)
// VIN: 25mV per LSB
uint16_t mp2762a_get_input_voltage_mv(void) {
  uint16_t raw = mp2762a_read_reg16(MP2762A_REG_ADC_VIN_L);
  return (raw >> 6) * 25;
}

// IIN: 6.25mA per LSB = 25/4 mA per LSB
uint16_t mp2762a_get_input_current_ma(void) {
  uint16_t raw = mp2762a_read_reg16(MP2762A_REG_ADC_IIN_L);
  return ((raw >> 6) * 25) / 4;
}

// VBAT: 12.5mV per LSB = 25/2 mV per LSB
uint16_t mp2762a_get_battery_voltage_mv(void) {
  uint16_t raw = mp2762a_read_reg16(MP2762A_REG_ADC_VBAT_L);
  return ((raw >> 6) * 25) / 2;
}

// ICHG: 12.5mA per LSB = 25/2 mA per LSB
uint16_t mp2762a_get_charge_current_ma(void) {
  uint16_t raw = mp2762a_read_reg16(MP2762A_REG_ADC_ICHG_L);
  return ((raw >> 6) * 25) / 2;
}

uint8_t mp2762a_get_fault(void) { return mp2762a_read_reg(MP2762A_REG_FAULT); }

// Check if BATT_UVLO (battery under-voltage lockout) is active
uint8_t mp2762a_is_battery_uvlo(void) {
  uint8_t status = mp2762a_read_reg(MP2762A_REG_STATUS);
  return (status & MP2762A_BATT_UVLO) ? 1 : 0;
}

// Battery presence detection using charge state + charge current.
//
// When VIN is present, BATT_UVLO is unreliable because BATTFET back-feeds VSYS
// to the BATT pin (capacitors on the BATT bus hold the voltage). Instead we use:
//   - cs=DONE(3)    → battery present (full, charger terminated properly)
//   - cs=TRICKLE(1) → battery present (deeply discharged)
//   - cs=FAST(2) + ci > threshold → battery present (actively charging)
//   - cs=FAST(2) + ci <= threshold → no battery (phantom current on caps)
//   - cs=NOT_CHARGING(0) → no battery (charger not running)
//
// When VIN is absent, BATT_UVLO is reliable (no back-feed from VSYS).
void mp2762a_poll_battery(void) {
  uint8_t status = mp2762a_read_reg(MP2762A_REG_STATUS);
  uint8_t raw_present;

  if (status & MP2762A_ACOK) {
    // VIN present - use charge state + current for detection
    uint8_t chg_bits = status & MP2762A_CHG_STAT_MASK;
    uint16_t ichg = mp2762a_get_charge_current_ma();

    if (chg_bits == MP2762A_CHG_DONE || chg_bits == MP2762A_CHG_TRICKLE) {
      // DONE = full battery, TRICKLE = deeply discharged battery
      raw_present = 1;
    } else if (ichg > BAT_CHG_CURRENT_THRESHOLD_MA) {
      // Significant charge current flowing = real battery
      raw_present = 1;
    } else {
      // FAST with phantom current on caps, or NOT_CHARGING = no battery
      raw_present = 0;
    }
  } else {
    // No VIN - BATT_UVLO is reliable directly
    raw_present = (status & MP2762A_BATT_UVLO) ? 0 : 1;
  }

  // When VIN present and no battery detected, periodically restart charger
  // to unstick its state machine (allows detection of newly inserted battery).
  if ((status & MP2762A_ACOK) && !raw_present) {
    g_stuck_counter++;
    if (g_stuck_counter >= STUCK_RESTART_COUNT) {
      g_stuck_counter = 0;
      mp2762a_restart_charging();
    }
  } else {
    g_stuck_counter = 0;
  }

  // Debounce: require BAT_DEBOUNCE_COUNT consecutive consistent readings
  if (raw_present == g_bat_present_state) {
    g_bat_present_counter = 0;
  } else {
    g_bat_present_counter++;
    if (g_bat_present_counter >= BAT_DEBOUNCE_COUNT) {
      uint8_t old_state = g_bat_present_state;
      g_bat_present_state = raw_present;
      g_bat_present_counter = 0;
      // Edge detection: absent -> present
      if (old_state == 0 && raw_present == 1) {
        g_bat_inserted_flag = 1;
      }
    }
  }
}

// Returns cached debounced battery presence state (no I2C traffic)
uint8_t mp2762a_is_battery_present(void) {
  return g_bat_present_state;
}

// Returns 1 once when battery insertion is detected, then clears the flag
uint8_t mp2762a_battery_inserted(void) {
  if (g_bat_inserted_flag) {
    g_bat_inserted_flag = 0;
    return 1;
  }
  return 0;
}

// Kick watchdog timer and re-assert full configuration.
// Call every 2s. Defends against silent register resets from watchdog expiry.
// WTD_RST bit auto-clears after write.
void mp2762a_kick_watchdog(void) {
  mp2762a_write_reg(MP2762A_REG_CONFIG0,
      MP2762A_WTD_RST | MP2762A_CONFIG0_NORMAL);
  mp2762a_write_reg(MP2762A_REG_CONFIG1, 0x44);
}

// Restart charging by clearing faults and toggling CHG_EN
// Call this when battery is inserted or when recovering from fault
void mp2762a_restart_charging(void) {
  // 1. Read fault register to clear latched faults
  (void)mp2762a_read_reg(MP2762A_REG_FAULT);

  // 2. Disable charger (CHG_EN = 0), keep BATTFET on
  mp2762a_write_reg(MP2762A_REG_CONFIG0, MP2762A_NTC_GCOMP | MP2762A_BATTFET_EN);

  // 3. Short delay for charger to reset (10ms)
  Delay_Ms(10);

  // 4. Re-enable charger with full config
  mp2762a_write_reg(MP2762A_REG_CONFIG0, MP2762A_CONFIG0_NORMAL);
}

void mp2762a_read_all(mp2762a_data_t *data) {
  uint8_t status = mp2762a_read_reg(MP2762A_REG_STATUS);
  uint8_t chg_bits = status & MP2762A_CHG_STAT_MASK;

  switch (chg_bits) {
  case MP2762A_CHG_NOT_CHARGING:
    data->chg_state = CHG_STATE_NOT_CHARGING;
    break;
  case MP2762A_CHG_TRICKLE:
    data->chg_state = CHG_STATE_TRICKLE;
    break;
  case MP2762A_CHG_FAST:
    data->chg_state = CHG_STATE_FAST;
    break;
  case MP2762A_CHG_DONE:
    data->chg_state = CHG_STATE_DONE;
    break;
  default:
    data->chg_state = CHG_STATE_NOT_CHARGING;
    break;
  }

  data->power_good = (status & MP2762A_ACOK) ? 1 : 0;
  data->vin_mv = mp2762a_get_input_voltage_mv();
  data->iin_ma = mp2762a_get_input_current_ma();
  data->vbat_mv = mp2762a_get_battery_voltage_mv();
  data->ichg_ma = mp2762a_get_charge_current_ma();
  data->fault = mp2762a_read_reg(MP2762A_REG_FAULT);
}
