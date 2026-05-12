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

  // REG00H: Input current limit 1 (LIM1, sustained "lower" rail) - 5A
  // Bits 6:0 set current, 50mA per bit, 0mA offset.
  // 5000mA / 50 = 100 = 0x64.
  // Sized so the DC-IN path doesn't bottleneck a 5V/5A USB-C output
  // (PDO #1). With 12V VIN that's up to 60W upstream, comfortably above
  // the ~30W needed for full PDO #1 (sink + chain efficiency).
  mp2762a_write_reg(MP2762A_REG_INPUT_ILIM, 0x64);

  // REG0FH: Input current limit 2 (LIM2, "higher" burst rail) - 5A
  // Per datasheet, the chip uses BOTH LIM1 and LIM2 in a 1600 us cycle:
  // ~700 us at LIM2, then ~900 us at LIM1. Default LIM2 = 1.5A. If we
  // only set LIM1 (REG00H) and leave LIM2 at default, the chip caps the
  // sustained input draw near 1.5A and the rest is supplemented from the
  // battery -> battery slowly drains even though VIN is plenty. Match
  // LIM2 to LIM1 (5A) so both halves of the cycle allow full input draw.
  // Bits 6:0 same scale as LIM1: 50mA/LSB. 5000/50 = 100 = 0x64.
  mp2762a_write_reg(MP2762A_REG_INPUT_ILIM2, 0x64);

  // REG01H: Input voltage limit - 4.5V minimum
  // Bits 6:0, 100mV per bit, 0V offset (0V to 12.8V range)
  // 4500mV / 100 = 45 = 0x2D
  mp2762a_write_reg(MP2762A_REG_INPUT_VLIM, 0x2D);

  // REG02H: Charge current - 500mA
  // Bits 6:0, 50mA per bit, 0mA offset
  // 500mA / 50 = 10 = 0x0A
  mp2762a_write_reg(MP2762A_REG_CHG_CURR, 0x0A);

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

  // REG0BH is Configuration Register 3, NOT an "ADC config" register —
  // the older comment here was wrong. The MP2762A ADC has no software
  // enable; it runs whenever the chip is operational. Bits 1:0 here are
  // PROCHOT/PSYS_CFG; setting them to 11 enables both PROCHOT and PSYS
  // open-drain outputs. Kept for board parity with previous behavior;
  // PROCHOT thresholds default to 11.9A which is far above anything we
  // can pull from this PCB so the assertion never fires in practice.
  uint8_t cfg3 = mp2762a_read_reg(MP2762A_REG_ADC_CFG);
  mp2762a_write_reg(MP2762A_REG_ADC_CFG, cfg3 | 0x03);
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

/* ADC readings — all MP2762A ADC results are 10-bit values stored in bits
 * 15:6 of a two-byte little-endian register pair (LSB at the lower address,
 * MSB at the higher one). The bottom 6 bits of the LSB byte are reserved.
 *
 * Read each 16-bit ADC pair as TWO independent single-byte transactions
 * rather than one I2C_read_reg16. The bit-banged software I²C in i2c_lib.c
 * does not consistently latch the chip's internal address auto-increment
 * across a repeated start, so the MSB read silently aliases back to the
 * LSB byte for some registers (notably ADC_ICHG, which is why CI was
 * stuck at 0 on the OLED). Two single-byte reads always re-issue the
 * register address and avoid the issue. ~2x the I²C traffic, but ADC
 * polling is only every 2 s. */
static uint16_t mp2762a_read_adc16(uint8_t reg_lsb) {
    uint8_t lsb = mp2762a_read_reg(reg_lsb);
    uint8_t msb = mp2762a_read_reg((uint8_t)(reg_lsb + 1));
    return ((uint16_t)msb << 8) | lsb;
}

// VIN: 25mV per LSB
uint16_t mp2762a_get_input_voltage_mv(void) {
  uint16_t raw = mp2762a_read_adc16(MP2762A_REG_ADC_VIN_L);
  return (raw >> 6) * 25;
}

// IIN: 6.25mA per LSB = 25/4 mA per LSB
uint16_t mp2762a_get_input_current_ma(void) {
  uint16_t raw = mp2762a_read_adc16(MP2762A_REG_ADC_IIN_L);
  return ((raw >> 6) * 25) / 4;
}

// VBAT: 12.5mV per LSB = 25/2 mV per LSB
uint16_t mp2762a_get_battery_voltage_mv(void) {
  uint16_t raw = mp2762a_read_adc16(MP2762A_REG_ADC_VBAT_L);
  return ((raw >> 6) * 25) / 2;
}

// ICHG: 12.5mA per LSB = 25/2 mA per LSB
uint16_t mp2762a_get_charge_current_ma(void) {
  uint16_t raw = mp2762a_read_adc16(MP2762A_REG_ADC_ICHG_L);
  return ((raw >> 6) * 25) / 2;
}

// VSYS: 12.5mV per LSB = 25/2 mV per LSB (same scale as VBAT in the datasheet).
// Used to diagnose TPS55289 UVLO trips: TPS VIN comes from this rail and
// will reset the converter if VSYS dips below the chip's UVLO threshold
// (~3 V) under load step transients — typical when running with no battery
// to cushion VSYS.
uint16_t mp2762a_get_system_voltage_mv(void) {
  uint16_t raw = mp2762a_read_adc16(MP2762A_REG_ADC_VSYS_L);
  return ((raw >> 6) * 25) / 2;
}

// Junction temperature (per datasheet REG24~25H):
//   T(°C) = (903 - TJ) / 2.578   where TJ = raw_reg16 >> 6  (10-bit ADC)
// Returns deci-Celsius (0.1 °C units) so it composes cleanly with the
// LM75B reading (Board_Temp_c10) which is also deci-Celsius.
//   T_dC = T(°C) × 10 = (903 - TJ) × 10 / 2.578
// Fixed-point: multiply by 10000 then divide by 2578 → tenths of a degree
// with one extra digit of integer-math precision before truncation.
int16_t mp2762a_get_junction_temp_c10(void) {
  uint16_t raw = mp2762a_read_adc16(MP2762A_REG_ADC_TJUNC_L);
  int32_t tj = (int32_t)(raw >> 6);
  // (903 - tj) can be negative for very hot dies (TJ > 903 LSB ⇒ ~0 °C
  // and rising). At our normal operating range it's positive.
  int32_t dC = ((903 - tj) * 10000) / 2578;
  if (dC > 32767) dC = 32767;
  if (dC < -32768) dC = -32768;
  return (int16_t)dC;
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

// Periodic config refresh.
//
// Call every 2 s. The I²C watchdog itself is disabled in CONFIG1 (WTD[1:0]
// = 00 in 0x44), so we don't need to assert WTD_RST here — and writing
// WTD_RST appears to be the trigger that wipes some R/W settings back to
// their OTP defaults (observed via system.log dumps: r02 returned to 0x14
// = 1A despite mp2762a_init writing 0x0A = 500 mA). Drop WTD_RST and
// blindly rewrite every register we care about so the CHG configuration
// stays where we put it.
void mp2762a_kick_watchdog(void) {
  // CONFIG0 / CONFIG1 — full normal config, no WTD_RST.
  mp2762a_write_reg(MP2762A_REG_CONFIG0, MP2762A_CONFIG0_NORMAL);
  mp2762a_write_reg(MP2762A_REG_CONFIG1, 0x44);
  // Charge-current setting (500 mA). Was being silently reset to OTP
  // default 1 A — visible directly when the system.log dump showed
  // r02=0x14 instead of our intended 0x0A.
  mp2762a_write_reg(MP2762A_REG_CHG_CURR,    0x0A);
  // Input current limits 1 + 2 (5 A each). REG0FH OTP default is 1.5 A
  // and the chip apparently re-asserts it under some circumstances; LIM1
  // we keep on 5 A so DC IN can fully feed VBUS_OUT plus charging.
  mp2762a_write_reg(MP2762A_REG_INPUT_ILIM,  0x64);
  mp2762a_write_reg(MP2762A_REG_INPUT_ILIM2, 0x64);
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
  // MP2762A is powered exclusively from PPVAR_INP_SYS (gated by PA6 driving
  // Q222). When PA6=0 the chip has no VCC, can't ACK I²C, and the bit-banged
  // master happily reads 0x00 from every register. We must NOT use those
  // zeros — they look like "0 V, 0 mA, TJ=350.3 °C" downstream. The PA1
  // hysteresis loop in TIM1_UP_IRQHandler is the source of truth for "is
  // mains present"; ACOK is unreliable when the chip is dead.
  if (!mp2762a_powered()) {
    data->chg_state  = CHG_STATE_NOT_CHARGING;
    data->power_good = 0;
    data->vin_mv     = 0;
    data->iin_ma     = 0;
    data->vbat_mv    = 0;       // on-battery VBAT is sourced from PA5 ADC in main.c
    data->ichg_ma    = 0;
    data->vsys_mv    = 0;
    data->tjunc_c10  = MP2762A_TJ_NA;
    data->fault      = 0;
    return;
  }

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
  data->vsys_mv = mp2762a_get_system_voltage_mv();
  data->tjunc_c10 = mp2762a_get_junction_temp_c10();
  data->fault = mp2762a_read_reg(MP2762A_REG_FAULT);
}
