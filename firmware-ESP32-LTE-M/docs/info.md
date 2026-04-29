# LilyGo T-SIM7080G-S3 — Dev Board Reference

Płytka deweloperska użyta do prototypowania karty rozszerzeń LTE-M dla Web3 Pi UPS.
Po zakończeniu prototypowania nasz elektronik zaprojektuje dedykowaną płytkę M.2 2232 z analogicznymi peryferiami.

- Producent / link: [LilyGo T-SIM7080G-S3](https://lilygo.cc/en-us/products/t-sim7080-s3)
- Repo z przykładami: [Xinyuan-LilyGO/LilyGo-T-SIM7080G](https://github.com/Xinyuan-LilyGO/LilyGo-T-SIM7080G) (gitignored lokalnie w `LilyGo-T-SIM7080G/` jeśli sklonowane)
- Schemat: [T-SIM7080G_Schematic.pdf](T-SIM7080G_Schematic.pdf)
- Wymiary mechaniczne (DXF): [w upstream repo](https://github.com/Xinyuan-LilyGO/LilyGo-T-SIM7080G/blob/master/dwg/T-SIM7080G.DXF)

## Główne komponenty

| Element            | Model                                      | Uwagi                                                              |
| ------------------ | ------------------------------------------ | ------------------------------------------------------------------ |
| MCU                | ESP32-S3-WROOM-1                           | 16 MB Flash, **OPI PSRAM** → GPIO35–37 zajęte                      |
| Modem LTE-M/NB-IoT | SimCom SIM7080G                            | NB-IoT + Cat-M1, **brak 2G/3G/4G**                                 |
| PMU                | X-Powers AXP2101                           | I²C, zarządza wszystkimi szynami zasilania, ładowanie Li-Ion + solar |
| Karta SIM          | 1nce (M2M, NB-IoT/Cat-M)                   | Musi być włożona PRZED startem modemu, inaczej nie zostanie wykryta |

Datasheety w [`datasheets/`](datasheets/):
- ESP32-S3: [esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf](datasheets/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)
- SIM7080G — najczęściej używane:
  - [AT Command Manual V1.05](datasheets/SIM7070_SIM7080_SIM7090_AT_Command_Manual_V1.05.pdf) — kanoniczna referencja AT, używamy non-stop
  - [MQTT(S) Application Note V1.03](datasheets/SIM7070_SIM7080_SIM7090_MQTTS_Application_Note_V1.03.pdf)
  - [TCP/UDP(S) Application Note V1.03](datasheets/SIM7070_SIM7080_SIM7090_TCPUDPS_Application_Note_V1.03.pdf)
  - [SSL Application Note V1.00](datasheets/SIM7070_SIM7080_SIM7090_SSL_Application_Note_V1.00.pdf) — dla MQTTS i połączeń TLS
  - [SIM7080 Series Spec](datasheets/SIM7080_Series_SPEC_20200427.pdf) — przegląd, pinout, parametry RF
- Pozostałe (FOTA, GNSS, FS, HTTP, PING, low-power, LwM2M, CoAP, ThreadX, chińskie wersje, certyfikaty) są w upstream repo [Xinyuan-LilyGO/LilyGo-T-SIM7080G/datasheet/](https://github.com/Xinyuan-LilyGO/LilyGo-T-SIM7080G/tree/master/datasheet) — sklonuj lokalnie jeśli potrzebujesz offline:
  ```sh
  git clone https://github.com/Xinyuan-LilyGO/LilyGo-T-SIM7080G.git docs/LilyGo-T-SIM7080G
  ```

## Pinout ESP32-S3 ↔ SIM7080G (UART do modemu)

Z [examples/ATDebug/utilities.h](https://github.com/Xinyuan-LilyGO/LilyGo-T-SIM7080G/blob/master/examples/ATDebug/utilities.h) (upstream LilyGo repo):

| Sygnał | GPIO ESP32-S3 | Kierunek z punktu widzenia ESP32 |
| ------ | ------------- | -------------------------------- |
| MODEM_PWR (PWRKEY przez tranzystor) | 41 | OUT (pulse to power on/off)     |
| MODEM_DTR | 42         | OUT                              |
| MODEM_RI  | 3          | IN                               |
| MODEM_RXD | 4          | IN  (ESP32 RX ← Modem TX)        |
| MODEM_TXD | 5          | OUT (ESP32 TX → Modem RX)        |

UART do modemu: **115200 8N1** na `UART1` ESP-IDF.

> Sekwencja włączenia modemu (z przykładu): GPIO41 LOW → 100 ms → HIGH → 1000 ms → LOW. To wymusza
> "naciśnięcie" PWRKEY przez ~1 s. Wyłączenie wymaga ~6 s przyciśnięcia.

## I²C — PMU AXP2101

| Sygnał  | GPIO |
| ------- | ---- |
| I2C_SDA | 15   |
| I2C_SCL | 7    |
| PMU_IRQ | 6    |

Adres I²C: `AXP2101_SLAVE_ADDRESS` (0x34).

## SDMMC (slot karty SD na płytce)

| Sygnał    | GPIO |
| --------- | ---- |
| SDMMC_CMD | 39   |
| SDMMC_CLK | 38   |
| SDMMC_DAT | 40   |

## Domeny zasilania (AXP2101)

| Domena   | Co zasila                              | Napięcie domyślne | Notatki                                   |
| -------- | -------------------------------------- | ----------------- | ----------------------------------------- |
| DC1      | Rdzeń ESP32-S3                         | 3.3 V             | **NIE WYŁĄCZAĆ, nie zmieniać programowo** |
| DC3      | SIM7080G — główne zasilanie modemu     | 3.0 V (zakres 2.7–3.4 V) | Wyłączane podczas restartu/sleep    |
| BLDO1    | Konwersja poziomów ESP32↔modem         | 3.3 V             | **NIE WYŁĄCZAĆ — bez tego brak komunikacji UART z modemem** |
| BLDO2    | Antena GPS modemu                      | 3.3 V             | Włączyć tylko jeśli używamy GNSS          |
| ALDO3    | Karta SD                               | 3.3 V             |                                           |
| ALDO1/2/4 | Kamera (PIR/CAM, niewykorzystywane u nas) | -             | W naszym projekcie wyłączone              |
| DC5      | Wyjście użytkownika (regulowane)       | -                 | Max 1 A, regulowane                        |
| VSYS     | Szyna systemowa PMU (= napięcie wejściowe — USB-C lub bateria) |    |                                           |

> ⚠ SIM7080G **nie potrafi jednocześnie korzystać z sieci i GNSS** — trzeba przełączać się między trybami.

## Złącza i przyciski

- **USB-C** (obok SUSB) — programowanie ESP32-S3 + Serial monitor (USB-CDC).
- **Micro-USB** (SUSB) — używane TYLKO do aktualizacji firmware'u SIM7080G (bezpośrednie połączenie z modemem przez QDL).
- **BOOT** — wymuszenie trybu download na ESP32-S3 (trzymać przy podłączaniu USB).
- **RST** — reset ESP32-S3.
- **PWR (PWRKEY)** — włączanie/wyłączanie zasilania całej płytki (long-press 6 s = off, 128 ms = on).
- **SBOOT** — wymuszenie trybu upgrade na modemie (przed wpięciem Micro-USB).
- **Mechaniczny suwak** obok USB — odcina baterię.

## LED-y na płytce

| LED                    | Kolor    | Sterowanie                          | Czy możemy zmieniać z firmware'u? |
| ---------------------- | -------- | ----------------------------------- | --------------------------------- |
| **CHG LED**            | niebieski (przy USB-C) | AXP2101, rejestr `0x69` (I²C) | ✅ ON/OFF/Blink 1Hz/Blink 4Hz/Auto |
| **MODEM STATUS**       | czerwony (przy modemie) | Sprzętowy w SIM7080G              | ❌ niemożliwe                      |
| **MODEM NETWORK STATE** | czerwony (przy modemie) | Firmware SIM7080G                  | ⚠️ tylko ON/OFF komendą AT `AT+CNETLIGHT=0/1` |

**Brak "user LED" podpiętego do GPIO ESP32.** Jeśli potrzebujemy więcej kanałów wizualnego feedbacku poza CHG LED, podpinamy zewnętrzny LED do wolnego GPIO przez kabelek (na prototypie) — finalnie elektronik może to przewidzieć na płytce M.2.

### Wzorce sieciowego LED (modem firmware)

| Wzorzec migania        | Stan modemu                                        |
| ---------------------- | -------------------------------------------------- |
| 64 ms ON / 800 ms OFF  | Niezarejestrowany                                  |
| 64 ms ON / 3000 ms OFF | Zarejestrowany (PS domain)                        |
| 64 ms ON / 300 ms OFF  | Transmisja danych (PPP / wbudowany TCP/HTTP/MQTT) |
| OFF                    | Power off lub PSM sleep                            |

### CHG LED — tryby

Z `XPowersLib`:
- `XPOWERS_CHG_LED_OFF` — wyłączony
- `XPOWERS_CHG_LED_ON` — świeci
- `XPOWERS_CHG_LED_BLINK_1HZ` — miganie 1 Hz
- `XPOWERS_CHG_LED_BLINK_4HZ` — miganie 4 Hz
- `XPOWERS_CHG_LED_CTRL_CHG` — domyślny (zapala się przy ładowaniu)

Ustawienie czegokolwiek poza `CTRL_CHG` **przejmuje** LED i tracimy wtedy automatyczny wskaźnik ładowania. Jeśli używamy CHG LED jako statusu aplikacji, fakt ładowania trzeba pokazywać inaczej (np. odpytując PMU `getChargerStatus` i wstawiając stan ładowania do schematu migania własnoręcznie).

W ESP-IDF (bez Arduino) wystarczy jeden zapis I²C do AXP2101 reg `0x69`, bity sterujące CHGLED_CTRL — referencyjny kod jest w `XPowersLib/src/XPowersAXP2101.tpp` ([https://github.com/lewisxhe/XPowersLib](https://github.com/lewisxhe/XPowersLib)).

## GPIO — co wolno używać

- **GPIO35–37 zajęte** przez OPI PSRAM — nie ruszać.
- Bez kamery wszystkie pozostałe IO są wolne (poza pinami modemu/PMU/SDMMC powyżej).
- Wolne na komunikację z RP2040 (UART do UPS-a, nasz docelowy use-case): GPIO 18, 0 (BOOT — niewskazany), 33, 34, 43, 44, 45, 46, 47, 48 i ewentualnie wolne piny kamery, jeśli kamera nieużywana.

## Bateria / solar

- 18650 Li-Ion, 3.5–4.2 V, ładowanie do 1 A (regulowane przez PMU).
- Wejście solar: 4.4–6 V, max 500 mA.
- Domyślny PMU ma włączoną detekcję `TS pin`, którą **trzeba wyłączyć** (`PMU.disableTSPinMeasure()`),
  inaczej ładowanie nie ruszy (na płytce nie ma NTC).

## Toolchain — w naszym projekcie

- Używamy **ESP-IDF 6.0.0** (nie Arduino jak w oficjalnych przykładach LilyGo).
- Przykłady w `LilyGo-T-SIM7080G/examples/` są w Arduino + biblioteki TinyGSM / XPowersLib —
  traktujemy je jako **referencję do pinów i sekwencji startowej**, nie do bezpośredniego użycia.
- Plan na ESP-IDF:
  - PMU AXP2101 → `driver/i2c_master` + własny mini-driver lub port `XPowersLib` jako component
    (XPowersLib jest C++-owy i ma `#ifdef ARDUINO`, da się go skompilować poza Arduino, ale wymaga shimu).
  - Modem → komponent **`esp_modem`** z ESP-IDF (oficjalny, wspiera SIM7070/7080 jako wariant SIM7000,
    daje gotowe PPPoS przez netif → integracja z `lwip` i np. `esp-mqtt` "za darmo").
  - PPP/IP → przez `esp_netif` + `esp-mqtt` (TLS przez mbedTLS).
  - Arkiv — później, na bazie tego samego stacku TCP/IP po PPP.

## Ważne ostrzeżenia z oficjalnego README

1. **Nie wyłączać BLDO1** — to zasilanie level shiftera między ESP32-S3 a SIM7080G. Bez tego komunikacja UART nie działa.
2. **Nie modyfikować DC1** — to zasilanie rdzenia ESP32-S3.
3. SIM musi być **włożony przed startem modemu**.
4. Aktualizacja firmware modemu **niezalecana** (patrz osobna sekcja niżej + [docs/sim7080_update_firmware.md w upstream LilyGo repo](https://github.com/Xinyuan-LilyGO/LilyGo-T-SIM7080G/blob/master/docs/sim7080_update_firmware.md)).
5. Jeżeli ESP32-S3 nie wpada w tryb download: trzymać BOOT, podpiąć USB-C, zwolnić BOOT, upload.
6. Jeśli zasilanie z baterii nie startuje: sprawdzić mechaniczny suwak (ON) i przytrzymać PWR ~2 s.

## Konfiguracja sdkconfig — punkty do ustawienia

Na podstawie wymagań sprzętowych (z Arduino-owej konfiguracji LilyGo + ESP-IDF mappingu):

- `CONFIG_IDF_TARGET="esp32s3"`
- Flash: 16 MB, mode QIO @ 80 MHz
- PSRAM: **Octal (OPI)**, 80 MHz — `CONFIG_SPIRAM_MODE_OCT=y`
- CPU: 240 MHz
- USB CDC On Boot: ENABLED (do logów po USB-C)
- Partition table: custom dla 16 MB (np. 3 MB APP + reszta na SPIFFS/FATFS lub OTA)
- (later) komponent `esp_modem` z `idf_component.yml`
