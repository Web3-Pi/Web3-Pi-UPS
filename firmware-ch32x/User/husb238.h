#ifndef HUSB238_H
#define HUSB238_H

#include "debug.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HUSB238 — Hynetek USB Type-C Power Delivery SINK controller.
 *
 * On Web3 Pi UPS HW rev.3 the HUSB238 owns the INPUT-side PD negotiation
 * autonomously: after POR it applies Rd on CC, reads the source caps and
 * (per its VSET/ISET strap resistors + internal fuse) requests a contract
 * with NO MCU involvement. The CH32X is a pure passive READER over I2C —
 * it polls PD_STATUS0 to learn the negotiated voltage/current. The native
 * CH32X USB-PD SINK role used on earlier revisions is no longer involved
 * on the input path.
 *
 * Datasheet: Web3-Pi-UPS/docs/tmp/datasheet/HUSB238/
 *   - HUSB238-datasheet.pdf            (Rev 2.0, theory of operation)
 *   - HUSB238-register-information.pdf  (REV 1.1, authoritative register map)
 *
 * I2C: 7-bit address 0x08 (fixed, not strappable). i2c_lib does the <<1.
 * No INT pin and no flags register exist -> polling only. All registers
 * are single-byte.
 *
 * Powering: HUSB238 VIN is derived from the USB-C VBUS input ONLY (a barrel
 * jack, if present, ORs in later via an ideal diode and never reaches this
 * chip). When USB-C is unplugged the HUSB238 is unpowered and does not ACK;
 * the bit-bang master (which releases SDA to a pull-up and never checks ACK)
 * then reads 0xFF from every register — or 0x00 if something clamps the bus.
 * husb238_read_all() treats BOTH as "detached": `attached` demands the
 * ATTACH bit AND a valid voltage code (1..6), which neither 0xFF (code 0xF)
 * nor 0x00 (code 0) satisfies. So the HUSB238's own attach state is the
 * reliable, USB-C-specific presence signal — no powered() gate needed, and
 * crucially NOT the input-voltage ADC, which is OR'd with the barrel jack.
 */

/* I2C 7-bit slave address (fixed). */
#define HUSB238_I2C_ADDR        0x08

/* ---- Registers (0x00..0x09) -------------------------------------------- */
#define HUSB238_REG_PD_STATUS0  0x00  /* R  negotiated contract: V[7:4] I[3:0] */
#define HUSB238_REG_PD_STATUS1  0x01  /* R  attach / CC dir / PD response / 5V */
#define HUSB238_REG_SRC_PDO_5V  0x02  /* R  source-cap: detect[7] current[3:0] */
#define HUSB238_REG_SRC_PDO_9V  0x03
#define HUSB238_REG_SRC_PDO_12V 0x04
#define HUSB238_REG_SRC_PDO_15V 0x05
#define HUSB238_REG_SRC_PDO_18V 0x06
#define HUSB238_REG_SRC_PDO_20V 0x07
#define HUSB238_REG_SRC_PDO     0x08  /* RW select PDO to request: SEL[7:4]    */
#define HUSB238_REG_GO_COMMAND  0x09  /* RW command: FUNC[4:0]                  */

/* ---- PD_STATUS0 (0x00) decode ----------------------------------------- */
#define HUSB238_S0_VOLT_MASK    0xF0  /* [7:4] negotiated voltage code */
#define HUSB238_S0_VOLT_SHIFT   4
#define HUSB238_S0_CURR_MASK    0x0F  /* [3:0] negotiated current code (LUT) */

/* PD_SRC_VOLTAGE codes (PD_STATUS0[7:4]). */
#define HUSB238_V_UNATTACHED    0x0
#define HUSB238_V_5V            0x1
#define HUSB238_V_9V            0x2
#define HUSB238_V_12V           0x3
#define HUSB238_V_15V           0x4
#define HUSB238_V_18V           0x5
#define HUSB238_V_20V           0x6

/* ---- PD_STATUS1 (0x01) fields ----------------------------------------- */
#define HUSB238_S1_CC_DIR_MASK  0x80  /* [7] 0=CC1/unattached, 1=CC2 */
#define HUSB238_S1_ATTACH_MASK  0x40  /* [6] 1=attached (not unattached) */
#define HUSB238_S1_RESP_MASK    0x38  /* [5:3] PD response code */
#define HUSB238_S1_RESP_SHIFT   3
#define HUSB238_S1_5V_VOLT_MASK 0x04  /* [2] 1=5V contract */
#define HUSB238_S1_5V_CURR_MASK 0x03  /* [1:0] 5V contract current */

/* PD_RESPONSE codes (PD_STATUS1[5:3]). */
#define HUSB238_RESP_NONE       0x0
#define HUSB238_RESP_SUCCESS    0x1
#define HUSB238_RESP_INVALID    0x3  /* invalid command or argument */
#define HUSB238_RESP_UNSUPPORTED 0x4 /* command not supported */
#define HUSB238_RESP_FAIL_CRC   0x5  /* transaction fail, no GoodCRC */

/* ---- SRC_PDO_xV (0x02..0x07) fields ----------------------------------- */
#define HUSB238_PDO_DETECT_MASK 0x80  /* [7] source offers this voltage */
#define HUSB238_PDO_CURR_MASK   0x0F  /* [3:0] advertised current; [6:4] reserved */

/* ---- SRC_PDO (0x08) select codes (write code<<4 into [7:4]) ------------ */
/* NOTE the gap between 12V (0x3) and 15V (0x8) — confirmed by datasheet. */
#define HUSB238_SEL_NONE        0x0
#define HUSB238_SEL_5V          0x1
#define HUSB238_SEL_9V          0x2
#define HUSB238_SEL_12V         0x3
#define HUSB238_SEL_15V         0x8
#define HUSB238_SEL_18V         0x9
#define HUSB238_SEL_20V         0xA

/* ---- GO_COMMAND (0x09) function codes (FUNC[4:0]) --------------------- */
#define HUSB238_CMD_REQUEST_PDO 0x01  /* request PDO selected in SRC_PDO */
#define HUSB238_CMD_GET_SRC_CAP 0x04  /* send Get_SRC_Cap */
#define HUSB238_CMD_HARD_RESET  0x10  /* send hard reset */

/* Cached negotiated-contract snapshot, refreshed by husb238_read_all(). */
typedef struct {
    uint8_t  attached;       /* 1 if PD_STATUS1 ATTACH set */
    uint8_t  v_code;         /* PD_STATUS0[7:4] nibble (HUSB238_V_*) */
    uint8_t  i_code;         /* PD_STATUS0[3:0] nibble (LUT index)   */
    uint8_t  last_response;  /* PD_STATUS1[5:3] (HUSB238_RESP_*)  */
    uint16_t voltage_mV;     /* decoded negotiated voltage, 0 if unattached */
    uint16_t current_mA;     /* decoded negotiated current via LUT */
} husb238_data_t;

/* --- API (Phase 1: passive read of the negotiated contract) ------------ */
void     husb238_init(void);
uint8_t  husb238_read_reg(uint8_t reg);
void     husb238_write_reg(uint8_t reg, uint8_t value);
void     husb238_read_all(husb238_data_t *out);

/* Pure decoders. Current decode MUST use the table — the 16 steps are
 * non-linear (e.g. code 0xD = 4.0 A, not 3.75 A), so no formula works. */
uint16_t husb238_decode_voltage_mV(uint8_t v_code);
uint16_t husb238_decode_current_mA(uint8_t i_code);

/* --- Phase 2/3: source-cap enumeration + active profile selection ------ */
/* The 6 fixed source-cap slots, by index: 0=5V 1=9V 2=12V 3=15V 4=18V 5=20V. */
#define HUSB238_PDO_COUNT 6
extern const uint16_t HUSB238_PDO_MV[HUSB238_PDO_COUNT];

typedef struct {
    uint8_t  detected_mask;                 /* bit i set if slot i is offered */
    uint16_t current_mA[HUSB238_PDO_COUNT]; /* advertised current per slot    */
} husb238_src_caps_t;

/* Read SRC_PDO_5V..20V (0x02..0x07) into `out`. The chip auto-populates
 * these after POR; if detected_mask==0 the source caps aren't ready yet
 * (caller may issue GET_SRC_CAP via husb238_write_reg and retry). */
void husb238_read_src_caps(husb238_src_caps_t *out);

/* ---- PDO selection policy (owner-defined, 2026-06-21) ----------------- *
 * The downstream TPS55289 buck-boost almost always outputs 5 V to the Pi.
 * Per its datasheet (Fig 6-1, VOUT=5V) efficiency rises as Vin falls, so a
 * mid voltage (15/12 V) balances converter efficiency against input-side
 * I^2R (higher V = fewer amps). Policy:
 *   - need >= 9 V for correct operation (floor); 5 V only as a last resort
 *   - cap at 20 V; never request > 5 A (PD fixed PDOs are <= 5 A anyway)
 *   - 45 W is plenty, 60 W is great, > 60 W pointless -> "satisfice": take
 *     the most-preferred voltage that already gives >= 45 W, don't chase more
 *   - preference order: 15 > 12 > 18 > 20 > 9  (fewer amps + efficiency)
 *   - weak source (< 45 W everywhere): take best power, but prefer the
 *     efficient voltage among PDOs within ~10 % of the best.            */
#define HUSB238_POLICY_V_FLOOR_MV   9000
#define HUSB238_POLICY_V_CAP_MV     20000
#define HUSB238_POLICY_I_CAP_MA     5000
#define HUSB238_POLICY_P_ENOUGH_MW  45000
#define HUSB238_POLICY_BAND_NUM     9     /* tie-band lower bound = 90 % of best */
#define HUSB238_POLICY_BAND_DEN     10

/* Pick the PDO to request per the policy above. Returns an HUSB238_SEL_*
 * code, or HUSB238_SEL_NONE when nothing usable is offered (e.g. caps not
 * yet latched, or a garbled read) so the caller can retry. */
uint8_t husb238_pick_best_pdo(const husb238_src_caps_t *caps);

/* Map an HUSB238_SEL_* code back to its voltage in mV (0 if unknown). */
uint16_t husb238_sel_to_mV(uint8_t sel_code);

/* Request `sel_code` over I2C (write SRC_PDO[7:4] then GO_COMMAND=request).
 * I2C selection overrides the VSET/ISET straps (datasheet p.9). The new
 * contract appears asynchronously in PD_STATUS0/1 — poll husb238_read_all. */
void husb238_select_pdo(uint8_t sel_code);

#ifdef __cplusplus
}
#endif

#endif /* HUSB238_H */
