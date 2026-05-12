# CH32X telemetry fix — battery mode (no mains) reports garbage

**Status**: hardware fix applied (R433 = 100 Ω), firmware change pending.
**Affected file**: `Web3-Pi-UPS/firmware-ch32x/User/main.c` (+ `mp2762a.c`, `ADC_Function_Init`).
**Target**: CH32X035 firmware. RP2040 firmware unchanged.

---

## 1. Observed bug

When external power (USB-C **or** barrel jack) is disconnected and the UPS runs from battery, the `power.status` frame emitted by CH32X over WUPS contains nonsensical data. Captured frames from a live unit:

### Mains ON (frame seq=74, sane)

```
payload @ off+10:  01 02 a8 2f 88 13 88 13 dc 20 00 00 9b 01 d0 20 32 01 00 00
parsed:
  version=1  charge_state=2 (DONE)
  vbus_in_mV   = 12200
  vbus_out_mV  = 5000
  ibus_out_mA  = 5000
  vbat_mV      = 8412     ← 2S Li-Ion fully charged ✓
  ibat_mA      = 0
  temp_dC      = 411      ← 41.1°C ✓
  pd_contract  = 8400 mV @ 306 mA  (aliased to VSYS/IIN per main.c:509-515)
  faults       = 0
```

### Mains OFF (frame seq=75, broken)

```
payload:           01 00 00 00 88 13 88 13 00 00 00 00 ae 0d 00 00 00 00 00 00
parsed:
  version=1  charge_state=0 (NOT_CHARGING)
  vbus_in_mV   = 0        ← correct (mains gone)
  vbus_out_mV  = 5000     ← TPS55289 OK, alive on battery
  ibus_out_mA  = 5000     ← suspicious (separate latent bug, see §7)
  vbat_mV      = 0        ✗ battery is physically connected and full (~8.4 V)
  ibat_mA      = 0
  temp_dC      = 3502     ✗ 350.2°C — IC fire?
  pd_contract  = 0 mV @ 0 mA
  faults       = 0
```

The frame CRC is valid; the bytes in the payload are exactly what CH32X put on the wire. The bug is in the **source data**, not transport.

The RP2040 OLED, fed from the same `power.status` stream, mirrors the issue: shows "no battery" + a residual voltage reading.

---

## 2. Hardware context — why the chip is dead

```
USB-C (PPVAR_VBUS_INP)  ┐
                        ├─[ideal-diode OR]─→ PPVAR_INP_COM ─┬─→ ADC: R205(27.4k)/R206(5.1k) → PA1 (DC_INP_ADC_SRC)
Barrel  (PPVAR_DC_INP)  ┘                                   │
                                                            └─→ Q222 P-MOS (gate driven by Q201, controlled by PA6 = DC_INP_EN_SRC)
                                                                       │
                                                                       ↓
                                                                PPVAR_INP_SYS  (10–20 V, only present when mains AND Q222 ON)
                                                                       │
                                                                       ↓
                                                                  MP2762A — IN pin (powers the entire chip including its internal LDO at VCC pin)
```

Key fact: **MP2762A is exclusively powered from `PPVAR_INP_SYS`**. When mains is disconnected, or when Q222 is opened by `DC_INP_EN_SRC = 0`, the chip has no Vcc and is completely dead — its I²C pins do not respond, its ACOK output is undefined, its internal ADC isn't running. The chip cannot fall back to battery-powered operation in this hardware configuration.

When the chip is dead, the bit-banged I²C in `i2c_lib.c::I2C_read_reg()` blindly clocks 8 bits without checking ACK. The dead chip's I/O pins, with their substrate body diodes shunting current from the bus pull-ups to GND, clamp SDA low enough that the master reads **0x00 from every register**. The firmware happily treats those zeros as valid measurements.

The temperature `350.2°C` is the smoking gun. In `mp2762a_get_junction_temp_c10()`:

```c
int32_t dC = ((903 - tj) * 10000) / 2578;   // tj = raw_reg16 >> 6
```

With `raw = 0` → `tj = 0` → `dC = 903 × 10000 / 2578 = 3503` → 350.3°C. Matches exactly what we see.

---

## 3. Why earlier hypotheses are wrong

Two dead ends, documented so we don't go back:

- **"REG 0x0B ADC_START needs re-arming after VIN cycle"** — irrelevant. Chip has no Vcc, can't accept I²C writes anyway. The mp2762a.c:71-77 comment claiming `REG0BH is NOT an ADC config register` is also misleading per datasheet but doesn't matter for this bug.
- **"Use ACOK pin to detect mains state"** — ACOK is an open-drain output of MP2762A. When the chip is dead, the pin's state is whatever R311 (10k pull-up to PP3300_PD) decides, which is "high" = "no input" — coincidentally correct, but it's not a real signal, just the absence of one. Not a reliable source-of-truth.

The **clean** signal is `DC_INP_EN_SRC` (PA6 output) state. The firmware controls this pin via hysteresis on PA1 in `TIM1_UP_IRQHandler` (main.c:1053-1087). When `PA6 = 1`, MP2762A has power; when `PA6 = 0`, it doesn't. Read back the GPIO output register and you know with certainty whether the chip is alive.

---

## 4. Hardware change — already done

The CH32X has a dedicated VBAT ADC pin (PA5 = ADC_Channel_5 = PD_SRC_VBAT) that reads `PPVAR_VBAT+` through a divider:

```
PPVAR_VBAT+ ─── R430 (100k) ─┬─── R431 (47k) ─── GND
                              │
                              ├─── C430 (100 nF) ─── GND     (filter)
                              │
                              ├─── R433 (DNP, now 100R) ─── PA5 (CH32X)
                              │
                              └─── R432 (0R)             ─── VBAT_RP_ADC (RP2040)
```

**Until this fix R433 was DNP**, so PA5 floated and the firmware (correctly) didn't sample it.

**Now**: R433 = 100 Ω soldered. PA5 reads `0.3197 × VBAT` (47/(100+47)).

Schematic reference values:

| VBAT     | PA5 voltage |
|----------|-------------|
| 8.4 V    | 2.686 V     |
| 7.2 V    | 2.302 V     |
| 6.0 V    | 1.918 V     |

With CH32X ADC Vref ≈ Vdd ≈ 3.3 V, 12-bit, the formula:

```
VBAT_mV = ADC_count × (3300 / 4096) × ((100 + 47) / 47)
        = ADC_count × 3300 × 147 / (4096 × 47)
        ≈ ADC_count × 10322 / 4096
```

Check at VBAT=8.4 V: ADC ≈ 2.686/3.3 × 4096 ≈ 3333. `3333 × 10322 / 4096 ≈ 8398 mV` ≈ 8.4 V ✓.

R432 stays at 0 Ω (RP2040 side untouched — its calibration and OLED display remain valid).

---

## 5. Required firmware changes

All in `Web3-Pi-UPS/firmware-ch32x/`.

### 5.1 Enable PA5 as analog input — `User/main.c` `ADC_Function_Init()` (~ line 1163)

Current code at line 1177-1180:

```c
//PA5: input floating (battery ADC divider - not used, MP2762A provides VBAT)
GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5;
GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
GPIO_Init(GPIOA, &GPIO_InitStructure);
```

Change to analog mode:

```c
//PA5: PD_SRC_VBAT — battery voltage ADC via R430/R431 divider (R433=100R since 2026-05-11)
GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5;
GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
GPIO_Init(GPIOA, &GPIO_InitStructure);
```

Note: the existing init only configures ADC for `ADC_Channel_1` (line 1195). `Get_ADC_Val(uint8_t ch)` at line 1142 calls `ADC_RegularChannelConfig(ADC1, ch, ...)` dynamically each call, so it can sample `ADC_Channel_5` on demand without additional init. Verify this works (the channel switch + EOC poll path is shared). If `Get_ADC_Val()` returns 0 or garbage for ch=5, add an explicit `ADC_RegularChannelConfig(ADC1, ADC_Channel_5, 1, ADC_SampleTime_11Cycles)` call somewhere reasonable.

### 5.2 Sample PA5 periodically and cache — `User/main.c` (TIM1 IRQ or main loop)

Option A (preferred — main loop, just before `wups_send_power_status`):

```c
// Add near other static globals (~line 81 area)
volatile u16    Vbat_ADC_Val = 0;
static UINT16   Vbat_Voltage_mV = 0;

// In the main loop's 1 Hz block (around line 1027 where DC_Inp_Voltage_mV is computed):
Vbat_ADC_Val    = Get_ADC_Val(ADC_Channel_5);
Vbat_Voltage_mV = (UINT16)((UINT32)Vbat_ADC_Val * 10322u / 4096u);
DC_Inp_Voltage_mV = (UINT16)((UINT32)DC_Inp_ADC_Val * 21029u / 4096u);
wups_send_power_status(WUPS_ADDR_RP2040, WUPS_FLAG_EVENT, Wups_Tx_Seq++);
```

Option B (TIM1 IRQ — only if you want faster updates than 1 Hz). The IRQ already does one `Get_ADC_Val` per tick for PA1; adding a second call costs ~1.4 µs and is fine. But the WUPS frame only emits at 1 Hz so there's no consumer for higher-rate samples — Option A is cleaner.

### 5.3 Detect "MP2762A powered" cleanly — new helper

In `User/main.c` near other static helpers:

```c
// True iff PA6 (DC_INP_EN_SRC) is currently driving Q222 ON, which is the
// only condition under which MP2762A has VCC. When this returns false, do
// not touch the I²C bus to address 0x5C — every read will return 0x00 from
// the dead chip's clamped pins and pollute Chg_Data with bogus values.
static inline UINT8 mp2762a_powered(void) {
    return (GPIO_ReadOutputDataBit(GPIOA, GPIO_Pin_6) == Bit_SET) ? 1 : 0;
}
```

### 5.4 Skip MP2762A reads when dead — `User/mp2762a.c` `mp2762a_read_all()` (line 324)

Add early-out at the top. Either: a) export `mp2762a_powered()` to mp2762a.c via a header, or b) take a "powered" hint as a parameter, or c) do the GPIO read inline. Option (a) is cleanest:

```c
void mp2762a_read_all(mp2762a_data_t *data) {
    extern UINT8 mp2762a_powered(void);   // or move helper to a shared header

    if (!mp2762a_powered()) {
        data->chg_state  = CHG_STATE_NOT_CHARGING;  // semantically "discharging" — see §6
        data->power_good = 0;
        data->vin_mv     = 0;
        data->iin_ma     = 0;
        data->vbat_mv    = 0;       // signal: not measured here (PA5 path owns this on battery)
        data->ichg_ma    = 0;
        data->vsys_mv    = 0;
        data->tjunc_c10  = INT16_MIN;  // sentinel "n/a" — see §5.6
        data->fault      = 0;
        return;
    }

    // ... rest of existing function unchanged
}
```

### 5.5 Swap data sources in `wups_send_power_status` — `User/main.c` line 489

Current (lines 493-507):

```c
s.vbus_in_mV  = (uint16_t)Chg_Data.vin_mv;   // ← MP2762A: zeros when on battery
// ...
s.vbat_mV     = (uint16_t)Chg_Data.vbat_mv;  // ← same problem
// ...
{
    int16_t lm = Board_Temp_c10;
    int16_t tj = Chg_Data.tjunc_c10;
    s.temp_dC = (tj > lm) ? tj : lm;          // ← 350.3°C from raw=0
}
```

After (replace exactly these lines):

```c
// vbus_in: always from our own PA1 ADC. Works in every state — mains
// present, mains absent, Q222 cut, MP2762A dead. The MP2762A's VIN
// reading is redundant and unavailable on battery; we ignore it.
s.vbus_in_mV  = DC_Inp_Voltage_mV;
// ...
// vbat: always from our own PA5 ADC. Available whether MP2762A is alive
// or not; with mains on and no battery present, MP2762A's BATTFET leakage
// can leave residual voltage on PPVAR_VBAT+, but that's a separate
// edge-case handled below (faults flag).
s.vbat_mV     = Vbat_Voltage_mV;
// ...
{
    int16_t lm = Board_Temp_c10;
    int16_t tj = Chg_Data.tjunc_c10;
    // Skip MP2762A's TJ when the chip is unpowered — it's INT16_MIN
    // (sentinel set in mp2762a_read_all). Use LM75B alone in that case.
    s.temp_dC = (tj == INT16_MIN) ? lm : ((tj > lm) ? tj : lm);
}
```

### 5.6 `INT16_MIN` sentinel

If `INT16_MIN` is not already imported, `#include <stdint.h>` and use `INT16_MIN` or define `MP2762A_TJ_NA (-32768)`. Don't pick `0` as the sentinel because 0 °C / 0.0 dC is a legitimate reading on a cold device.

### 5.7 `charge_state` and `ibat_mA` on battery

After §5.4, when on battery `Chg_Data.chg_state = CHG_STATE_NOT_CHARGING (0)` and `ibat_ma = 0`. The downstream `wups_power_status_v1_t::charge_state` semantics in `common/protocol.h` say `0=idle 1=charging 2=charged 3=fault`. There's no "discharging" code today. Two options:

- **Now**: leave `0` (NOT_CHARGING) as the on-battery indicator. The panel can derive "on battery" from `vbus_in_mV < 4500 && vbat_mV > 2500` (it already does — see `panel/api/lib/wupsproto.ts::powerToSnapshotPart`).
- **Later (protocol v2)**: add a dedicated state, e.g. `4 = DISCHARGING`. Out of scope for this hotfix.

Battery discharge current measurement is **physically unavailable** with current hardware — MP2762A is dead, there's no shunt on the BATT→VSYS path readable by CH32X. `ibat_mA = 0` is the best we can do until a hardware addition (shunt + amp into a free CH32X ADC channel).

### 5.8 Move `mp2762a_powered()` helper to a shared header (optional but tidy)

Put the inline in `mp2762a.h` (or a new small `power_state.h`) so both `main.c` and `mp2762a.c` can use it without `extern` games.

---

## 6. Verification plan

After flashing, **before merging**:

1. **Sanity check PA5 reading via serial debug**. Add a temporary printf next to the Vbat sampling:

   ```c
   printf("VBAT: raw=%u mv=%u\r\n", Vbat_ADC_Val, Vbat_Voltage_mV);
   ```

   With mains on **and** battery connected (fully charged): expect `raw ≈ 3300–3340` and `mv ≈ 8300–8400`. Measure with a multimeter across the battery and confirm ±2%.

2. **Without battery, mains on**: PA5 will read whatever BATTFET leakage holds on the BATT cap bank (C322–C326 = ~50 µF + C320 1 µF + C321 1 µF on `PPVAR_VBAT+`). This may settle anywhere from `0 V` to `VSYS − Vf(body diode)` depending on the chip's BATTFET state. **Don't use this run for calibration**. Use it just to verify the reading isn't pinned at 4096 (would indicate ADC misconfig).

3. **Battery only (mains pulled)**: PA5 should read true VBAT continuously. Multimeter cross-check expected ±2%. If gain or offset is off by more than ~5%, the math constant `10322` needs adjustment per actual measured Vdd of CH32X (the ADC ratiometric to Vdd, not to a 3.3 V reference — if the 3V3 rail is actually 3.28 V or 3.32 V under the load, scale changes proportionally).

4. **Capture a `power.status` frame in each state** via the WUPS MQTT debug tool (`Web3-Pi-UPS-Panel/tools/mqtt-debug/index.html`). Expected fields after fix:

   | state             | vbus_in | vbat       | temp                 | charge_state |
   |-------------------|---------|------------|----------------------|--------------|
   | mains, charging   | ~12000  | 6000–8400  | ~max(LM75B, MP2762A) | 1 (FAST)     |
   | mains, full       | ~12000  | ~8400      | same                 | 2 (DONE)     |
   | battery only      | 0       | true VBAT  | LM75B only           | 0 (NOT_CHG)  |
   | no input, no bat  | 0       | ~0         | LM75B only           | 0            |

   Specifically: **`temp_dC` MUST NEVER be 3502 again**. If it is, §5.5 or §5.4 didn't take.

5. **OLED check on RP2040 side**: pull mains, confirm display shows real VBAT (~8 V) and not "no battery". RP2040 firmware is **not** changed in this fix; it derives its UI state from the same `power.status` frame, so once CH32X reports honest data the OLED follows.

6. **No-regression check with mains on**: charging state, ACOK, temperature, etc. should look exactly like before this change. The §5.5 swap only changes which source feeds `vbus_in_mV` and `vbat_mV`; for mains-on this is a wash (PA1 and MP2762A agree on VIN within ADC tolerance, PA5 and MP2762A agree on VBAT within ADC tolerance).

---

## 7. Out of scope (separate commits)

These items are tangentially related, do **not** fix as part of this hotfix:

- **`ibus_out_mA = 5000` always**: `Tps_Iread_a10` is the TPS55289 current-limit register, not a measurement. The field name in the frame implies measurement; consumers (panel, OLED) treat it as such. Either change the source, rename the field, or document explicitly. Latent UX bug, not data-corruption.
- **`pd_contract_mV/_mA` aliased to VSYS/IIN**: documented hack in `wups_send_power_status` lines 509-525. Becomes a real protocol bug if anything starts reading these fields literally. Resolve in protocol v2.
- **Misleading comment in `mp2762a.c:71-77`** about REG 0x0B not being ADC config — datasheet says otherwise. Doesn't impact this fix (chip is dead anyway, can't write any register).
- **v3 dead code**: per the user, the v3 hardware moves all SINK-side PD work to a dedicated PD-Trigger IC. The CH32X firmware still contains dual-role state machine, `PD_Switch_To_Sink`, `PD_Role_Manager_Tick`, and references to `PB11 = PDC_CC_DET` / `PB3 = PDC_CC_SEL` that are NC on v3. Removing this code frees flash (62 KB target is tight) and eliminates any risk of stray GPIO activity on reused pins. Audit + clean as a separate pass.
- **Discharging state in protocol** (see §5.7): add `wups_power_status_v1_t::charge_state = 4 (DISCHARGING)` and bump struct version. Coordinate with RP2040 OLED and panel decoders.

---

## 8. Files touched by this fix

```
Web3-Pi-UPS/firmware-ch32x/User/main.c        (ADC_Function_Init, main loop, wups_send_power_status)
Web3-Pi-UPS/firmware-ch32x/User/mp2762a.c     (mp2762a_read_all early-out)
Web3-Pi-UPS/firmware-ch32x/User/mp2762a.h     (optional: move mp2762a_powered() helper here)
```

No changes to:
- `Web3-Pi-UPS/common/protocol.h` (protocol schema unchanged)
- `Web3-Pi-UPS/firmware-rp2040/**` (RP2040 reads the same frame, will see correct data automatically)
- `Web3-Pi-UPS-Panel/**` (backend/parser unchanged)
