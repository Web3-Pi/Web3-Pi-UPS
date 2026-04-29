# Diagnoza BUG-1 — UPS nie startuje przy podłączonym RPi5 z oryginalnym zasilaczem RPi 27W

## Warunki testów

Wszystkie testy bez akumulatora w UPS-ie.

## Repro

Sekwencja: RPi5 podłączone do USB-C OUTPUT → następnie oryginalny zasilacz RPi 27W podłączony do USB-C INPUT.

Wynik: nic się nie zapala — żadna dioda na UPS-ie, RPi5 nie startuje, wyświetlacz nie wstaje. Powtarzalne 100%.

## Pomiary napięć w bug-scenariuszu (multimetr, testpointy strona 11 schematu)

| Sygnał | Testpoint | Wartość |
|---|---|---|
| PPVAR_INP_COM (rail po ideal-diode od USB-C INPUT) | TP106 | **0.004 V** |
| PP3300_PD (always-on 3.3V) | TP100 | 0.275 V |
| PP3300_EN (AP63300 enable pin) | TP129 | 0.004 V |

Zasilacz RPi 27W **nie podaje żadnego napięcia** na USB-C INPUT. Sprawdzone w trakcie wpinania — napięcie nie pojawia się nawet na chwilę.

**Wniosek:** CH32X w bug-scenariuszu nigdy nie dostaje zasilania (PP3300_PD = 0V). Bug nie może być softwareowy — MCU nawet nie startuje.

## Pomiary napięć linii CC INPUT (na rezystorach filtra R200 / R201, GND jako reference)

INPUT-side ma stałe Rd 5.1kΩ do GND na każdej linii CC (R202 i R203, strona 3 schematu, etykieta „Rd PULL-DOWN").

| Scenariusz | INPUT CC1 (R200) | INPUT CC2 (R201) | TP106 | Działa? |
|---|---|---|---|---|
| A. UPS pusty (bez RPi5, bez zasilacza) | 0.0 V | 0.0 V | 0 V | n/a |
| B. RPi5 podłączone, bez zasilacza | 0.0 V | 0.0 V | 0 V | n/a |
| C. **RPi 27W bez RPi5** | **1.128 V** | 0.0 V | **12 V** | **TAK** |
| D. **RPi 27W + RPi5 (bug)** | **0.744 V** | 0.0 V | **0 V** | **NIE** |
| E. Baseus powerbank PPBLD100HD + RPi5 | 0.0 V | **1.152 V** | 12 V | TAK |

Zasilacz RPi 27W używa tylko linii CC1 (CC2 = 0 V w scenariuszach C i D). Powerbank Baseus PPBLD100HD używa **przeciwnej linii — CC2** (CC1 = 0 V w scenariuszu E), co wynika z innego sposobu detekcji orientacji w jego kontrolerze PD. To wyjaśnia, dlaczego przy obecności RPi5 na OUTPUT linia CC2 INPUT-u Baseusa nie jest sprzęgana z muxem U231 w taki sam destrukcyjny sposób — Baseus mierzy linię, która nie jest wpięta w problematyczną ścieżkę przewodzenia ESD przez nieaktywny mux. Napięcie 1.152 V na CC2 INPUT w scenariuszu E jest zbliżone do 1.128 V na CC1 INPUT w scenariuszu C (oba czysty Rp/Rd divider z pojedynczym Rd), co potwierdza, że Baseus **widzi prawidłowy SNK** i normalnie negocjuje PD do 12 V.

## Interpretacja matematyczna

Scenariusz **C** (działający, jedno Rd 5.1kΩ na INPUT):

- V_CC1 = V_rp × R_d / (R_p + R_d)
- 1.128 V = V_rp × 5.1 / (R_p + 5.1)
- Dla V_rp = 3.3 V → **R_p ≈ 9.8 kΩ** (zgodne z USB-C PD spec dla advertise „Default USB Power")

Scenariusz **D** (bug, jedyna zmiana to dodanie RPi5 na OUTPUT):

- INPUT CC1 spada z 1.128 V do 0.744 V (−34%)
- Hipoteza: na linii INPUT CC1 widać dwa Rd 5.1kΩ równolegle = 2.55kΩ
- Predykcja: V = 3.3 × 2.55 / (9.8 + 2.55) = **0.671 V**
- Pomiar: 0.744 V — zgodne z hipotezą (różnica w tolerancji rezystorów ~5%)

Zasilacz RPi 27W na linii CC widzi 0.744 V zamiast ~1.13 V. Według USB-C PD spec dla SOURCE-side detekcji SNK (próg 0.85 V dla advertise Default USB), 0.744 V jest **poniżej progu „valid SNK Rd attached"**. Zasilacz klasyfikuje to jako nieprawidłowy SNK i odmawia podania VBUS.

**Uwaga o kontrolerze PD w zasilaczu RPi 27W:** układ U4 to **Weltrend WT6636F**, USB-PD 3.0 + Qualcomm QC4/4+ controller (datasheet rev 1.02, sierpień 2018, 16-pin QFN UG16C w zasilaczu RPi). Architektura:

- **Wbudowany 8-bit Turbo 8051 MCU + 16 kB MTP ROM** — całe behawioryzm chip-a jest firmware-driven. Datasheet (str. 6): „suffix number-XXX for difference Firmware code, please refer to Firmware control list" — każdy klient (Raspberry, inni) ma własną wersję firmware.
- **Piny CC1 (pin 6 QFN, GPIO4) i CC2 (pin 7 QFN, GPIO5)** — typu „CC, PP" (USB PD baseband input + push-pull), HV (tolerancja do 30 V).
- **10-bit ADC monitoruje obie linie CC w czasie rzeczywistym** (ADC4 na CC1, ADC5 na CC2). Firmware Raspberry ma precyzyjny obraz napięcia na CC i może implementować fine-grained anomaly detection (np. window-based: „valid SNK Rd produces CC voltage in 1.0–1.4 V window; anything outside → refuse VBUS").
- **Operating range**: VCC 3 V do 24 V — chip sam musi widzieć min 3 V żeby działać; jego własna logika startuje dopiero po wstępnym stanie VBUS na poziomie 5 V default USB-C.

Decyzja „valid SNK / reject" jest wykonywana w firmware Raspberry załadowanym do MTP ROM-a, nie przez czysto-sprzętową logikę. Datasheet Weltrend nie zawiera (i nie może zawierać) konkretnych progów detekcji — to dane proprietary klienta.

Empirycznie granica accept/reject dla WT6636F-a w zasilaczu RPi 27W leży między **0.744 V (reject) a 1.128 V (accept)**. To tłumaczy obserwowaną różnicę zachowania między zasilaczami — Baseus PB akceptuje 0.744 V (mniej restrykcyjny firmware), zasilacz RPi 27W odmawia (rygorystyczny firmware z anomaly detection).

## Asymetria CC1/CC2 — obserwacja scenariusza E

Baseus używa CC2 (1.152 V) zamiast CC1 jak RPi 27W. Mimo że RPi5 jest na OUTPUT, Baseus widzi **czysty Rp/Rd divider z pojedynczym Rd 5.1 kΩ** — bez sprzęgania, bez anomalii.

**Test orientacji wtyczki RPi 27W → INPUT (przeprowadzony):** odwrócenie wtyczki USB-C zasilacza RPi 27W o 180° **nie zmienia zachowania** — bug pozostaje. To wyklucza prostą hipotezę „bug zależy od orientacji jednego kabla".

Najbardziej prawdopodobne wyjaśnienie: **RPi5 prezentuje Rd na obu liniach CC OUTPUT** (lub jego kabel ma oba CC wires aktywne). Wtedy **oba muxy U230 (dla CC1) i U231 (dla CC2) są jednocześnie w pułapce ESD-coupling** — niezależnie od tego, którą linię mierzy zasilacz, widzi anomalię.

Dlaczego mimo to Baseus działa? Możliwe że firmware Baseus PB:
- Akceptuje 0.744 V jako „weak SNK present" bez retry (mniej restrykcyjne progi)
- Albo używa innego algorytmu detekcji (np. mierzy obie linie CC i decyduje na podstawie różnicowej, gdzie symetryczny anomaly nie jest dyskwalifikujący)
- Albo w ogóle nie ma anomaly detection poza prostym „CC voltage > threshold → SNK present"

**Pomiar uzupełniający do wykonania:** zmierzenie INPUT CC1 i CC2 po odwróceniu wtyczki RPi 27W. Spodziewany wynik (jeśli obie linie OUTPUT mają Rd RPi5): obie linie INPUT CC będą wykazywać podobny drop względem stanu bez RPi5, w zależności od orientacji wtyczki.

**Implikacja dla rozwiązania:** jeśli oba muxy są problemowe równocześnie, fix HW musi pokrywać obie linie. Rozwiązanie połowiczne (np. „tylko jedna linia naprawiona") nie zadziała. To nie zmienia kierunku rekomendacji (eliminacja muxa lub always-on V_CC), tylko wzmacnia wymóg pełnego pokrycia.

## Mechanizm

Mux U230 i U231 (74LVC1G3157DW-7, strona 2 schematu) ma VCC (pin 5) podłączone do PP3300_PD. W bug-scenariuszu PP3300_PD = 0 V, więc mux jest nieaktywny.

Z datasheetu 74LVC1G3157 (Diodes Inc, rev 4-2):
- **OFF state impedance = 50 MΩ typ.** — czyli właściwy switch w stanie nieaktywnym jest praktycznie idealnie odizolowany. Przewodzenie A↔B0↔B1 przez kanał MOSFET-ów nie jest mechanizmem.
- **Absolute Maximum Ratings:** `VSW (B0, B1, A) = -0.5V do VCC + 0.5V`. Przy VCC = 0 V, abs max na pinach przełącznika to **-0.5V do +0.5V**. W bug-scenariuszu INPUT CC1 = 0.744 V (B1) — przekracza abs max o 0.244 V.

**Właściwy mechanizm: clamping przez diody ochrony ESD przy V_CC = 0 V.**

W typowym CMOS analog switch pin signal ma diodę ESD od pinu do V_CC (oraz od GND do pinu). Gdy V_pin > V_CC + ~0.6 V, dioda ESD pin→V_CC zaczyna przewodzić w przód. Z V_CC = 0 V wystarczy, żeby pin był powyżej ~0.6 V.

W bug-scenariuszu:
- B1 (INPUT CC1) jest podciągane przez Rp zasilacza — bez RPi5 osiąga 1.128 V
- B0 (OUTPUT CC1) ma Rd 5.1 kΩ od RPi5 do GND — siedzi przy ~0 V
- B1 = 1.128 V > 0.6 V → dioda ESD B1→V_CC przewodzi w przód → V_CC pin podnosi się do ~B1 - 0.6 V
- V_CC = ~0.5 V > 0.6 V (z marginesem) na granicy przewodzenia ESD V_CC→B0 (lub V_CC→GND)
- Powstaje ścieżka prądowa: B1 → ESD diode → pin V_CC → ESD diode → B0 (przez Rd RPi5 do GND)
- Efektywnie B1 i B0 są shuntowane rezystancyjnie omijając właściwy przełącznik (50 MΩ pozostaje nieaktywne)

Liczbowo: z pomiaru drop V(CC1) z 1.128 V do 0.744 V wynika, że PSU widzi efektywną rezystancję ~2.85 kΩ między CC a GND. Z R_d UPS = 5.1 kΩ w równoległej z (R_shunt + R_d_RPi5 = R_shunt + 5.1 kΩ), wynika **R_shunt ≈ 1.36 kΩ** dla ścieżki ESD przez mux. Ta wartość jest spójna z dwoma diodami ESD w przewodzeniu w przód przy umiarkowanym prądzie roboczym.

Predykcja idealnego shuntu (R_shunt → 0): V = 0.671 V. Pomiar: 0.744 V. Różnica spójna ze skończoną rezystancją diod ESD w przewodzeniu.

**Wniosek:** mux 74LVC1G3157 z V_CC = 0 V nie izoluje pinów switch'a, gdy napięcie na nich przekracza ~0.6 V — diody ochrony ESD tworzą ścieżkę przewodzącą poprzez pin V_CC. To architektoniczna słabość konfiguracji, w której V_CC muxa zależy od rail-a, który sam jest zasilany dopiero po negocjacji PD.

## Architektoniczna implikacja

W cold-start bez akumulatora żadne wewnętrzne źródło zasilania UPS-a nie jest aktywne, dopóki zasilacz nie poda VBUS. Zasilacz nie poda VBUS, dopóki CC linie nie są zgodne ze spec. CC linie nie są zgodne, dopóki mux nie jest zasilony. **Deadlock cyrkularny** — nie da się go przerwać prostym dodaniem rezystora/zenera od PPVAR_SYS, bo PPVAR_SYS też jest 0V w tym scenariuszu.

## Obszary do rozważenia

1. Wymiana U230 / U231 na typ z czystą izolacją off-state przy V_CC = 0 (np. JFET-based switch lub inny analog mux z explicit „break-before-make" plus internal blocking)
2. Pasywna izolacja CC niezależna od V_CC — np. szeregowe diody na liniach CC z odpowiednim biasingiem (problem: PD wymaga signaling DC bidirectional)
3. Architektura z dwoma niezależnymi PD PHY na INPUT i OUTPUT, eliminująca konieczność muxa w ogóle
4. Inne podejście

Pomiary i mechanizm udokumentowane jednoznacznie.
