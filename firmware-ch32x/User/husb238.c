#include "husb238.h"
#include "i2c_lib.h"

/* Negotiated/advertised current decode table (PD_STATUS0[3:0] and
 * SRC_PDO_xV[3:0] share it). Values in mA. The steps are NON-LINEAR —
 * note index 13 = 4000 mA (not 3750) — so this table is mandatory; any
 * arithmetic formula would be wrong. Source: HUSB238 register doc REV 1.1. */
static const uint16_t HUSB238_CURRENT_LUT_MA[16] = {
     500,  700, 1000, 1250, 1500, 1750, 2000, 2250,
    2500, 2750, 3000, 3250, 3500, 4000, 4500, 5000
};

uint8_t husb238_read_reg(uint8_t reg) {
    return I2C_read_reg(HUSB238_I2C_ADDR, reg);
}

void husb238_write_reg(uint8_t reg, uint8_t value) {
    I2C_write_reg(HUSB238_I2C_ADDR, reg, value);
}

uint16_t husb238_decode_voltage_mV(uint8_t v_code) {
    switch (v_code) {
    case HUSB238_V_5V:  return 5000;
    case HUSB238_V_9V:  return 9000;
    case HUSB238_V_12V: return 12000;
    case HUSB238_V_15V: return 15000;
    case HUSB238_V_18V: return 18000;
    case HUSB238_V_20V: return 20000;
    case HUSB238_V_UNATTACHED:
    default:            return 0;   /* unattached / reserved */
    }
}

uint16_t husb238_decode_current_mA(uint8_t i_code) {
    return HUSB238_CURRENT_LUT_MA[i_code & 0x0F];
}

/* Refresh the cached negotiated contract. When no USB-C source is present
 * the chip is unpowered and both registers read 0x00, which decodes to
 * attached=0 / 0 mV / 500 mA — callers treat attached==0 (or v_code==0) as
 * "no input contract", so the unpowered case needs no special handling. */
void husb238_read_all(husb238_data_t *out) {
    uint8_t s0 = husb238_read_reg(HUSB238_REG_PD_STATUS0);
    uint8_t s1 = husb238_read_reg(HUSB238_REG_PD_STATUS1);

    out->v_code        = (uint8_t)((s0 & HUSB238_S0_VOLT_MASK) >> HUSB238_S0_VOLT_SHIFT);
    out->i_code        = (uint8_t)(s0 & HUSB238_S0_CURR_MASK);
    /* "attached" requires the ATTACH bit AND a valid PD voltage code (1..6).
     * When the chip is unpowered (USB-C unplugged) the shared bit-bang bus
     * reads 0x00 (clamped low) or 0xFF (floated high); 0xFF would otherwise
     * set the ATTACH bit and look connected. Demanding a real voltage code
     * makes both bogus reads decode as detached. */
    out->attached      = ((s1 & HUSB238_S1_ATTACH_MASK) &&
                          out->v_code >= HUSB238_V_5V &&
                          out->v_code <= HUSB238_V_20V) ? 1 : 0;
    out->last_response = (uint8_t)((s1 & HUSB238_S1_RESP_MASK) >> HUSB238_S1_RESP_SHIFT);
    out->voltage_mV    = husb238_decode_voltage_mV(out->v_code);
    out->current_mA    = husb238_decode_current_mA(out->i_code);
    if (!out->attached) {        /* no contract -> report no V/I, not LUT[0] */
        out->voltage_mV = 0;
        out->current_mA = 0;
    }
}

/* Nothing to configure on the chip — it self-negotiates from its VSET/ISET
 * straps after POR. Prime the bus with one read so the first poll is warm. */
void husb238_init(void) {
    (void)husb238_read_reg(HUSB238_REG_PD_STATUS0);
}

/* ---- Phase 2/3: source caps + active selection ------------------------ */

const uint16_t HUSB238_PDO_MV[HUSB238_PDO_COUNT] =
    { 5000, 9000, 12000, 15000, 18000, 20000 };

static const uint8_t HUSB238_PDO_SEL[HUSB238_PDO_COUNT] = {
    HUSB238_SEL_5V, HUSB238_SEL_9V, HUSB238_SEL_12V,
    HUSB238_SEL_15V, HUSB238_SEL_18V, HUSB238_SEL_20V
};

static const uint8_t HUSB238_PDO_REG[HUSB238_PDO_COUNT] = {
    HUSB238_REG_SRC_PDO_5V,  HUSB238_REG_SRC_PDO_9V,  HUSB238_REG_SRC_PDO_12V,
    HUSB238_REG_SRC_PDO_15V, HUSB238_REG_SRC_PDO_18V, HUSB238_REG_SRC_PDO_20V
};

void husb238_read_src_caps(husb238_src_caps_t *out) {
    out->detected_mask = 0;
    for (uint8_t i = 0; i < HUSB238_PDO_COUNT; i++) {
        uint8_t b = husb238_read_reg(HUSB238_PDO_REG[i]);
        if (b & HUSB238_PDO_DETECT_MASK) out->detected_mask |= (uint8_t)(1u << i);
        out->current_mA[i] = husb238_decode_current_mA(b & HUSB238_PDO_CURR_MASK);
    }
}

/* Voltage preference order (most -> least preferred), per the policy in
 * husb238.h: 15 V sweet spot, then 12 V, then higher V only for more power,
 * 9 V floor last. 5 V is intentionally absent (below the operating floor). */
static const uint16_t HUSB238_PREF_MV[5] = { 15000, 12000, 18000, 20000, 9000 };

/* Caps index for a voltage in mV, or -1 if not one of the 6 fixed slots. */
static int husb238_idx_of_mV(uint16_t v_mV) {
    for (int i = 0; i < HUSB238_PDO_COUNT; i++)
        if (HUSB238_PDO_MV[i] == v_mV) return i;
    return -1;
}

/* Power (mW) a slot would deliver, current clamped to the 5 A policy cap.
 * Max = 20000 mV * 5000 mA / 1000 = 100000 mW, fits u32 with margin. */
static uint32_t husb238_slot_mW(const husb238_src_caps_t *caps, int idx) {
    uint16_t i_mA = caps->current_mA[idx];
    if (i_mA > HUSB238_POLICY_I_CAP_MA) i_mA = HUSB238_POLICY_I_CAP_MA;
    return (uint32_t)HUSB238_PDO_MV[idx] * i_mA / 1000u;
}

static int husb238_slot_in_range(int idx) {
    return HUSB238_PDO_MV[idx] >= HUSB238_POLICY_V_FLOOR_MV &&
           HUSB238_PDO_MV[idx] <= HUSB238_POLICY_V_CAP_MV;
}

uint8_t husb238_pick_best_pdo(const husb238_src_caps_t *caps) {
    /* Stage A — satisfice: the first preferred voltage (in [floor, cap])
     * that already delivers >= P_ENOUGH. Stops as soon as "enough" power is
     * reached at the most-preferred (efficient) voltage; never chases more. */
    for (int p = 0; p < 5; p++) {
        int i = husb238_idx_of_mV(HUSB238_PREF_MV[p]);
        if (i < 0 || !(caps->detected_mask & (1u << i))) continue;
        if (husb238_slot_mW(caps, i) >= HUSB238_POLICY_P_ENOUGH_MW)
            return HUSB238_PDO_SEL[i];
    }

    /* Stage B — weak source (< P_ENOUGH everywhere): take the best power
     * among in-range PDOs, but among those within the tie-band prefer the
     * more-efficient voltage. (RPi 27 W lands here -> 15 V.) */
    uint32_t best_p = 0;
    for (int i = 0; i < HUSB238_PDO_COUNT; i++) {
        if (!(caps->detected_mask & (1u << i)) || !husb238_slot_in_range(i)) continue;
        uint32_t pw = husb238_slot_mW(caps, i);
        if (pw > best_p) best_p = pw;
    }
    if (best_p > 0) {
        uint32_t band = best_p * HUSB238_POLICY_BAND_NUM / HUSB238_POLICY_BAND_DEN;
        for (int p = 0; p < 5; p++) {
            int i = husb238_idx_of_mV(HUSB238_PREF_MV[p]);
            if (i < 0 || !(caps->detected_mask & (1u << i)) || !husb238_slot_in_range(i)) continue;
            if (husb238_slot_mW(caps, i) >= band) return HUSB238_PDO_SEL[i];
        }
    }

    /* Stage C — nothing at/above the 9 V floor: degraded fallback to the
     * highest offered voltage with real current (typically 5 V) so power
     * still flows. All-zero currents (garbled read) -> NONE, so we retry. */
    uint8_t fb_sel = HUSB238_SEL_NONE;
    for (int i = 0; i < HUSB238_PDO_COUNT; i++)
        if ((caps->detected_mask & (1u << i)) && caps->current_mA[i] > 0)
            fb_sel = HUSB238_PDO_SEL[i];
    return fb_sel;
}

uint16_t husb238_sel_to_mV(uint8_t sel_code) {
    for (uint8_t i = 0; i < HUSB238_PDO_COUNT; i++)
        if (HUSB238_PDO_SEL[i] == sel_code) return HUSB238_PDO_MV[i];
    return 0;
}

void husb238_select_pdo(uint8_t sel_code) {
    /* SRC_PDO[3:0] are reserved (reset 0); writing the code shifted into
     * [7:4] with a zero low nibble is correct, no read-modify-write needed. */
    husb238_write_reg(HUSB238_REG_SRC_PDO, (uint8_t)(sel_code << 4));
    husb238_write_reg(HUSB238_REG_GO_COMMAND, HUSB238_CMD_REQUEST_PDO);
}
