# Web3 Pi UPS — Quick Start

A compact DC UPS for the **Raspberry Pi 5**: powers the Pi over USB‑C, charges
and switches to a battery seamlessly on power loss, and reports its status to
the Pi over the **same cable** plus a small OLED + 2 buttons.

---

## ⚠️ Read this first

- **This is a prototype.** Always operate it **under supervision**. Do not leave it unattended, especially under havy load.
- **The battery MUST have a built‑in BMS / protection.** This board relies on
  the battery pack's own protection (over‑charge, over‑discharge, over‑current,
  short). All Sony NP‑F packs we know of have it — but **verify** before use,
  especially for third‑party/clone packs. Never use a raw, unprotected cell.
- **Li‑ion safety:** use the correct battery type, don't short the terminals,
  don't charge a damaged/swollen pack.

---

## TL;DR — get running in 3 steps

1. **Insert a battery** (Sony NP‑F, see below).
2. **Connect input power** — a USB‑C PD charger **or** a DC barrel supply. The
   OLED lights up and the battery starts charging.
3. **Connect the Raspberry Pi 5** to the UPS **`OUT`** USB‑C port (one cable =
   power **and** data) — the Pi boots.

On input‑power loss the UPS keeps the Pi running from the battery automatically.

---

## Connections — where is what

| Port | Function |
|------|----------|
| **`OUT`** (USB‑C) | **Output to the Raspberry Pi 5** — 5 V power **+ USB data** on one cable. |
| **Input** (USB‑C PD) | External power in. Voltage is auto‑negotiated (see below). |
| **Input** (DC barrel jack) | Alternative external power in, **12–20 V DC**. |

> If both a USB‑C source and the barrel jack are connected, the higher voltage
> is used (an ideal‑diode OR combines them).

---

## Batteries

- Use **Sony NP‑F (L‑series)** packs: **NP‑F550 / NP‑F770 / NP‑F970** (2S
  Li‑ion, 7.2 V nominal). Bigger number = more capacity / longer runtime.
- **The pack MUST have an internal BMS.** All genuine NP‑F packs do; the board
  has **no per‑cell protection of its own**, so the pack's BMS is the safety
  layer. (The external protection circuit was intentionally removed because
  NP‑F packs only expose `BAT+ / BAT−` and already protect themselves.)
- **Hot‑swap:** while running on **external power**, you can swap the battery
  without powering down the Pi.
- Charging targets ~8.1 V (gentle full); charge current is modest by design.

---

## Power & USB‑C PD

**Input (PD is automatic — no configuration):**
- Plug in any USB‑C PD charger. The UPS reads **all PD profiles the charger
  offers** and **auto‑selects the best one** (it prefers efficient mid
  voltages, up to **20 V**). You don't set anything.
- Or use a **12–20 V DC barrel** supply.

**Output to the Pi (USB‑C PD source):**
- The UPS is a **USB‑C PD source** and advertises **four output PD profiles**. The
  Pi 5 (or any PD sink) picks one, and the on‑board buck‑boost converter is set to
  that rail:

  | Output PD profile | Notes |
  |---|---|
  | **5.1 V / 5 A** | **Default** — the Raspberry Pi 5's full power (needs an e‑marked cable for 5 A). |
  | 9 V / 3 A | For other USB‑C PD sinks. |
  | 12 V / 2.25 A | For other USB‑C PD sinks. |
  | 15 V / 1.8 A | For other USB‑C PD sinks. |

- A **Raspberry Pi 5 uses the 5.1 V / 5 A profile** (negotiated automatically); the
  higher‑voltage profiles are there for general USB‑C PD devices.

**How much input power do you need?**
- To deliver the Pi's **full 5.1 V / 5 A (~25–27 W)** *and* charge the battery at
  the same time, the input source should provide **at least ~36 W** — converter
  losses, battery charging and overhead all add up. A **45 W or 65 W** USB‑C PD
  charger gives comfortable headroom. A weaker source still works, but output
  and/or charging will be reduced.

**Cables — e‑marker for 5 A:**
- Drawing **5 A** over USB‑C requires an **e‑marked cable** (it carries a chip
  that authorises 5 A). The official Raspberry Pi 27 W supply uses 5 V/5 A and
  ships with such a cable.

---

## OLED display & navigation

A small **OLED** plus **two buttons (LEFT / RIGHT)** show live status.

**Navigation:**
- Short‑press a button on the **Home** screen to enter the screen cycle;
  **RIGHT** = next, **LEFT** = previous.
- After **20 s** of no button activity it returns to **Home**.
- (Long‑press LEFT opens the system menu, when the optional LTE‑M module is
  fitted.)

**Screens:**

| Screen | Shows |
|--------|-------|
| **Home** | Battery icon, **SOC %**, charge state, and Vbat / Vin / Vout. |
| **INPUT** | `VIN` (input voltage), the negotiated **input PD** contract (V / A / W) or `N/A`, and the source type. |
| **OUTPUT** | Output **voltage**, the **output PD** contract to the Pi (V / A), and the current **limit**. |
| **BATTERY** | Battery **voltage + SOC %**, **Mode**, and charge current (`Ich`). |
| **SYSTEM** | **Uptime**, the two board **temperatures**, and **fault** flags. |

**Mode / charge‑state labels:**
`DSC` = discharging (on battery) · `CHG` = charging · `FUL` = full ·
`PRE` = pre‑charge · `IDL` = on mains, not charging.

---

## Raspberry Pi integration

- The **`OUT`** USB‑C cable carries **both power and USB data** to the Pi 5 —
  one cable, no extra wiring.
- The UPS appears on the Pi as a **USB serial port** (identifies itself as
  `Web3_Pi / Web3_Pi_UPS`), so the Pi can read status and send commands.
- **Web3 Pi vOS has native support** — it talks to the UPS over that serial
  port out of the box (telemetry, safe shutdown on low battery, etc.).

---

## Quick facts

- 🔌 **Two inputs:** USB‑C PD (auto‑negotiated) **or** 12–20 V DC barrel.
- 🔋 **Battery:** Sony NP‑F (2S Li‑ion); **must have its own BMS**; **hot‑swappable** on external power.
- ⚡ **Output (USB‑C PD source):** four profiles — **5.1 V / 5 A** (default, Pi 5), 9 V / 3 A, 12 V / 2.25 A, 15 V / 1.8 A.
- 🤖 **PD input is automatic:** it queries the charger and picks the best profile (≤ 20 V) — no setup.
- 🧮 **Headroom:** budget **~36 W+** input for full output **plus** charging.
- 🪢 **e‑marked cable** needed for 5 A.
- 🖥️ **One cable to the Pi** = power + data; shows up as a **serial port**; **vOS native support**.
- 📺 **OLED + 2 buttons:** Home + INPUT / OUTPUT / BATTERY / SYSTEM screens; auto‑returns to Home after 20 s.
- ⚙️ **Seamless switchover:** keeps the Pi running on battery when input power is lost.
- 🧪 **Prototype:** use under supervision, at your own risk.

---

## Notes & limitations

- **Don't feed power into the `OUT` port.** The output is a power *source*;
  connecting another charger to it is an unsupported, potentially damaging
  configuration.
- SOC (%) is derived from battery voltage; while charging it may read slightly
  optimistically and settle once charging stops.
- This document describes the current prototype firmware; details may change.
