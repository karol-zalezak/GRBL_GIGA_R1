# Tabuľka pinov — GIGA R1 + shield periférií (návrh v2)

Spoločný podklad pre firmvér (`src/cpu_map.h`) a návrh shieldu (časť 5 v
[PLAN.md](PLAN.md)). Grafická verzia: mapa pinov (artefakt, odkaz v
konverzácii). **Stav: schválené 13. 10. 2026**, firmvér ho používa. 14. 10. 2026
doplnený vstup napájania driverov **DRV PWR na D53**.

**Zásady návrhu:**

- **Bloky bez krížení:** každý funkčný blok shieldu je na jednom úseku konektorov GIGA,
  v poradí, v akom idú svorky:

  | Blok | Konektor GIGA | Piny |
  |---|---|---|
  | vstupy IN1–IN16 | 2×18, vnútorný (párny) stĺpec | D22–D52 |
  | vstupy IN17–IN20, STOP a DRV PWR | 2×18, vonkajší stĺpec dole | D43–D53 |
  | kroky, smery, enable | 2×18, vonkajší stĺpec hore | D25–D41 (port J, ako dnes) |
  | výstupy OUT1–OUT12 a BRAKE | horný konektor zľava doprava | D13–D8, D5–D2, D14–D16 |
  | 0–10 V | analógový konektor | A12 (DAC0) |

- **Univerzálne kanály:** vstupy **IN1–IN20** a výstupy **OUT1–OUT14** (OUT13/OUT14 od
  shieldu v4) sú univerzálne kanály. Kanál je pevne spojený s pinom, **funkciu mu
  priraďuje nastavenie** (`$60`–`$93`, viď PLAN.md). Uvedené funkcie sú predvolené.
- **Pevné piny (nemenia sa nastavením):** kroky, smery a enable, centrálne STOP,
  napájanie driverov (DRV PWR), brzda Z (výkonový výstup 5 A), 0–10 V a panel obsluhy;
  PWM vretena je od 27. 10. 2026 na OUT13 podľa nastavenia `$38`.
- **Oproti dnešnému firmvéru:** kroky, smery a enable ostávajú. Vstupy a výstupy sa
  presúvajú do blokov, takže dnešné zapojenie holej dosky podľa README prestane platiť
  (dnes nie je pripojený žiadny stroj).

---

## Pevné piny

| Signál | Funkcia | Pin | Port | Konektor | Oproti dnešku |
|---|---|---|---|---|---|
| STEP X | krok X | D25 | PJ0 | 2×18 | ako dnes |
| DIR X | smer X | D27 | PJ1 | 2×18 | ako dnes |
| STEP Y | krok Y | D29 | PJ2 | 2×18 | ako dnes |
| DIR Y | smer Y | D31 | PJ3 | 2×18 | ako dnes |
| STEP Z | krok Z | D33 | PJ4 | 2×18 | ako dnes |
| DIR Z | smer Z | D35 | PJ5 | 2×18 | ako dnes |
| STEP A | krok A | D37 | PJ6 | 2×18 | ako dnes |
| DIR A | smer A | D39 | PI14 | 2×18 | ako dnes |
| ENABLE | servo enable (všetky osi) | D41 | PK7 | 2×18 | ako dnes |
| STOP | signál z bezpečnostného relé, NC/NO nastaviteľné | D49 | PE4 | 2×18 | **zmena** (dnes dvere); prerušenie EXTI4 |
| DRV PWR | napájanie driverov je prítomné — vlastný optočlen, sledovanie `$94` | D53 | PG7 | 2×18 | **nový** (14. 10. 2026); číta ho sken 10 kHz |
| BRAKE | brzda osi Z — výkonový MOSFET do 5 A | D16 | PH13 | komunikačný | **nový** |
| PWM | rýchlosť vretena PWM (TIM4_CH2, 1 kHz) — od 27. 10. 2026 len s `$38` > 0, inak je D6 výstup OUT13 | D6 | PD13 | UNO | ako dnes |
| 0–10 V | DAC0 → izolačný zosilňovač → op-amp → menič | A12 | PA4 | analógový | ako dnes (DAC) |

## Vstupy IN1–IN20 (univerzálne)

Všetky cez 24 V optočlen (izolovaná poľná strana), v štvoriciach. Predvolene
NPN (COM štvorice na +24 V prepojkou na doske). Na strane GIGA pull-up, pin
aktívny v nule. NC/NO pre každý kanál nastavením.

| Kanál | Pin | Port | Štvorica | Predvolená funkcia | Oproti dnešku |
|---|---|---|---|---|---|
| IN1 | D22 | PJ12 | 1 | koncák X− | ako dnes (X limit) |
| IN2 | D24 | PG12 | 1 | koncák X+ | **zmena** (dnes Y limit) |
| IN3 | D26 | PJ14 | 1 | koncák Y− | **zmena** (dnes Z limit) |
| IN4 | D28 | PJ15 | 1 | koncák Y+ | **zmena** (dnes A limit) |
| IN5 | D30 | PK3 | 2 | koncák Z− | **zmena** (dnes alarm X) |
| IN6 | D32 | PK4 | 2 | koncák Z+ | **zmena** (dnes alarm Y) |
| IN7 | D34 | PK5 | 2 | koncák A− (pri slave: 2. spínač portálu) | **zmena** (dnes alarm Z) |
| IN8 | D36 | PK6 | 2 | koncák A+ | **zmena** (dnes alarm A) |
| IN9 | D38 | PJ7 | 3 | sonda | **zmena** (dnes OUT1) |
| IN10 | D40 | PE6 | 3 | dvere (DOOR) | **zmena** (dnes OUT2) |
| IN11 | D42 | PI15 | 3 | vreteno na otáčkach (relé meniča) | **zmena** (dnes OUT3) |
| IN12 | D44 | PG10 | 3 | porucha vretena (relé meniča) | **zmena** (dnes OUT4) |
| IN13 | D46 | PH15 | 4 | tlačidlo RESET | **zmena** (dnes OUT5) |
| IN14 | D48 | PK0 | 4 | tlačidlo HOLD | **zmena** (dnes OUT6) |
| IN15 | D50 | PI11 | 4 | tlačidlo START | **zmena** (dnes OUT7) |
| IN16 | D52 | PK2 | 4 | — (voľný) | **zmena** (dnes OUT8) |
| IN17 | D43 | PI10 | 5 | alarm serva X | **zmena** (dnes RESET) |
| IN18 | D45 | PI13 | 5 | alarm serva Y | **zmena** (dnes HOLD) |
| IN19 | D47 | PB2 | 5 | alarm serva Z | **zmena** (dnes START) |
| IN20 | D51 | PE5 | 5 | alarm serva A | **zmena** (dnes sonda) |

STOP (D49) je medzi nimi, ale má vlastný optočlen. Za IN20 nasleduje DRV PWR (D53),
tiež s vlastným optočlenom a vlastnou svorkou.

**Shield v4 (od 27. 10. 2026):** alarmy serv z konektorov DE-15 HD (osi X, Y, Z, A) idú
na **IN17–IN20**, svorky IN1–IN16 sú univerzálne (zmena kvôli rozloženiu dosky). Predvolené
funkcie v tabuľke vyššie sú od 27. 10. 2026 tie zo shieldu v4 (schválené používateľom);
platia pre nové nastavenia (`$RST=$`), aktualizácia firmvéru priradenie na doske nemení.

## Výstupy OUT1–OUT12 (univerzálne)

Izolované tranzistorové výstupy spínané proti 0 V (NPN / open drain), 500 mA,
do 24 V. Poradie na hornom konektore zľava doprava.

| Kanál | Pin | Port | Predvolená funkcia | Oproti dnešku |
|---|---|---|---|---|
| OUT1 | D13 | PH6 | vreteno chod (menič FWD) | **nový pin** (dnes D38) |
| OUT2 | D12 | PJ11 | vreteno CCW (menič REV) | **nový pin** (dnes D40) |
| OUT3 | D11 | PJ10 | chladenie `M8` | **nový pin** (dnes D42) |
| OUT4 | D10 | PK1 | hmla `M7` | **nový pin** (dnes D44) |
| OUT5 | D9 | PB9 | AUX0 (odsávanie) | **nový pin** (dnes D46) |
| OUT6 | D8 | PB8 | AUX1 | **nový pin** (dnes D48) |
| OUT7 | D5 | PA7 | AUX2 | **nový pin** (dnes D50) |
| OUT8 | D4 | PJ8 | AUX3 | **nový pin** (dnes D52) |
| OUT9 | D3 | PA2 | otvorenie klieštiny (pneumatický ventil 24 V) | **nový** |
| OUT10 | D2 | PA3 | reset alarmu serv (impulz) | **nový** |
| OUT11 | D14 | PG14 | reset poruchy meniča (impulz) | **nový** |
| OUT12 | D15 | PC7 | AUX4 | **nový** |

Medzi OUT6 a OUT7 sú na konektore D7 (rezerva, len vstup) a D6 (PWM). Za
OUT12 nasleduje BRAKE (D16).

**Shield v4 (od 27. 10. 2026)** má v každej skupine 7. kanál (voľný kanál optočlena a 7.
kanál TBD62003) a 9-pólové svorky. Vo firmvéri sú to bežné univerzálne výstupy (`$80`–`$93`
až po 14); predvolene AUX5 na OUT14, OUT13 voľný. PWM na OUT13 nastavením `$38`: 0 vypnuté
(predvolené, D6 je OUT13), 1 PWM, 2 PWM obrátené na pine (pre TTL vstup za výstupom, ktorý
spína proti 0 V). Kým je PWM zapnuté, OUT13 nemôže mať funkciu (`error:42`).

| Kanál | Pin | Port | Skupina / svorka | Poznámka |
|---|---|---|---|---|
| OUT13 | D6 | PD13 | OUT7–13 (J20) | pin PWM vretena / lasera (TIM4_CH2); bez PWM bežný výstup |
| OUT14 | A2 | PB0 | OUT1–6+14 (J19) | vie aj PWM (TIM3_CH3) |

Cez optočlen prejde PWM asi do 1 kHz. Výstup spína proti 0 V, takže vstup PWM s TTL
úrovňou potrebuje vlastný pull-up a signál je na ňom obrátený.

### Ktoré výstupy vedia hardvérové PWM

Podľa tabuľky alternatívnych funkcií STM32H747, len s časovačmi, ktoré firmvér nepoužíva
(TIM2/TIM5 = časovač mbed, TIM6/TIM7/TIM17 = kroky a sken vstupov, TIM8 = koliesko).
Kanály toho istého časovača majú spoločnú frekvenciu.

| Výstup | Pin | Port | Časovač |
|---|---|---|---|
| OUT1 | D13 | PH6 | TIM12_CH1 |
| OUT2 | D12 | PJ11 | TIM1_CH2 |
| OUT3 | D11 | PJ10 | TIM1_CH2N |
| OUT4 | D10 | PK1 | TIM1_CH1 |
| OUT5 | D9 | PB9 | TIM4_CH4 |
| OUT6 | D8 | PB8 | TIM4_CH3, TIM16_CH1 |
| OUT7 | D5 | PA7 | TIM3_CH2, TIM14_CH1 |
| OUT8 | D4 | PJ8 | TIM1_CH3N |
| OUT9 | D3 | PA2 | TIM15_CH1 |
| OUT10 | D2 | PA3 | TIM15_CH2 |
| OUT11 | D14 | PG14 | — (bez časovača) |
| OUT12 | D15 | PC7 | TIM3_CH2 |
| OUT13 | D6 | PD13 | TIM4_CH2 (dnešné PWM vretena) |
| OUT14 | A2 | PB0 | TIM3_CH3 |
| BRAKE | D16 | PH13 | — (len TIM8, ten má koliesko) |

## Rezerva — nepoužité

| Piny | Poznámka |
|---|---|
| D23 (PG13) | 2×18, napr. ďalší vstup (D53 je od 14. 10. 2026 DRV PWR) |
| D7 (PB4) | len ako vstup (po resete pull-up od JTAG) |
| A0–A7 | analógové vstupy 0–3,3 V; na shielde v4 od 27. 10. 2026 A0/A1 potenciometre, A2 OUT14, A4–A6 tlačidlá panela (viď nižšie), voľné A3 a A7 |
| A13 (PA5, DAC1) | druhý analógový výstup |
| D20–D21 | I2C (Wire) — D17–D19 od 22. 10. 2026 RS485 na shielde v4, viď nižšie |
| D0/D1 | Serial1 |
| CAN RX/TX (PB5/PB13), 6-pinový konektor SPI (D89–D91) | rezerva |
| D54–D67 | 14 GPIO kamery — bez kamery voľné, dostupné cez konektor J3 na displeji (2×10 pri hornom okraji) dlhými kolíkmi zo shieldu; v J3 sú aj SDA1/SCL1 (nepoužiť), +3,3 V a GND. Od 14. 10. 2026 ručné koliesko (MPG), viď nižšie; D64–D67 ostávajú voľné |

## Ručné koliesko (MPG) na pinoch kamery — od 14. 10. 2026

Vo firmvéri (`src/mpg.c`, `$96`–`$98`). Piny vedú cez konektor kamery J3 na
displeji. Vstupy sú aktívne v nule (kontakt pendantu alebo optočlen shieldu
ťahá pin s pull-upom k zemi), LED je výstup aktívny v jednotke.

| Signál | Pin | Port | J3 pin | Poznámka |
|---|---|---|---|---|
| koliesko A | D54 | PI5 | 15 | TIM8_CH1 (AF3) — pevne, počíta časovač |
| koliesko B | D59 | PI6 | 12 | TIM8_CH2 (AF3) — pevne |
| os X | D55 | PH8 | 16 | |
| os Y | D56 | PA6 | 13 | |
| os Z | D57 | PJ9 | 14 | |
| os 4 (= A) | D58 | PI7 | 11 | |
| ×1 | D60 | PI4 | 9 | |
| ×10 | D61 | PH14 | 10 | |
| ×100 | D62 | PG11 | 7 | |
| LED | D63 | PH11 | 8 | výstup |
| rezerva | D64–D67 | PH10, PH9, PA1, PD4 | 5, 6, 1/3, 2/4 | |

## RS485 k meniču (shield v4) — od 22. 10. 2026

Izolované RS485 (ADM2587E) na shielde v4 na riadenie meniča vretena cez Modbus RTU.
Vo firmvéri (`src/vfd.c`, `$33`–`$37`, viď README): Grbl je Modbus RTU master, D17
riadi softvérovo — 1 tesne pred prvým štart bitom, 0 v prerušení po poslednom stop
bite (TC); prijímač USART2 je počas vysielania vypnutý. Prvý profil: Delta (MS300,
C2000, VFD-E).

| Signál | Pin | Port | Poznámka |
|---|---|---|---|
| TX | D18 | PD5 | Serial2 (USART2_TX) |
| RX | D19 | PD6 | Serial2 (USART2_RX) |
| DE + ~RE | D17 | PI9 | 1 = vysielanie; pull-down 10k na shielde = príjem počas štartu |

D17 je RX linky Serial3, ktorú nepoužívame (jej TX D16 je BRAKE).

## Panel obsluhy (shield v4) — od 27. 10. 2026

Priamo na piny A, bez izolácie (krátke vodiče k panelu pri riadiacej skrini), konektory
JST XH. Vo firmvéri `src/panel.c`: tlačidlá `$39` (predvolene zapnuté), START a HOLD ako
tie funkcie na vstupoch, STOP = riadené zastavenie (feed hold, potom reset s polohou, bez
alarmu); potenciometre `$44` (A0), `$45` (A1): 0 nič, 1 korekcia posuvu, 2 otáčok,
3 rýchloposuvu, predvolene vypnuté (zvyčajne `$44=1`, `$45=2`).

| Signál | Pin | Port | Svorka | Zapojenie |
|---|---|---|---|---|
| potenciometer 1 | A0 | PC4 | J31 (XH 3) | +3V3 cez 100 Ω, bežec cez 1k, 100 nF na pine |
| potenciometer 2 | A1 | PC5 | J32 (XH 3) | ako potenciometer 1 |
| START | A4 | PC3 | J33 (XH 2) | tlačidlo k GND, pull-up 10k, 1k + 100 nF |
| HOLD | A5 | PC2 | J34 (XH 2) | ako START |
| STOP | A6 | PC0 | J35 (XH 2) | ako START; zastavenie programu, nie bezpečnostný STOP (D49) |

## Nepoužiť

| Piny | Dôvod |
|---|---|
| SDA1/SCL1 (pri AREF) | I2C Wire1 — zbernica dotyku (GT911, 0x5D) a RGB diódy (0x50) displeja |
| D68–D75 | konektor displeja; D70/D71 = dotyk (INT/RST), D74 = podsvietenie |
| D86–D88 | RGB LED na doske — Grbl ňou ukazuje príjem, vysielanie a alarm |
| A8–A11 | len analógové piny, bez GPIO |
| PA15 (D92) | napájanie portu USB-A — nie je vyvedený |

---

## Prečo práve takto (pre firmvér)

- **Kroky:** STEP X/Y/Z/A = PJ0/PJ2/PJ4/PJ6, smery X/Y/Z na tom istom porte. Firmvér
  zapisuje jedným zápisom na každý použitý port; port J tak dá kroky všetkých osí naraz.
- **STOP na D49 = PE4 / EXTI4:** linka 4 má vlastný vektor prerušenia (`EXTI4_IRQn`) a
  nič iné ju nepoužíva. Linkám 1, 3 a 8 som sa vyhol (INT dotyku PI1, „host wake“ BLE
  PG3 a WiFi PI8). STOP je pevný — chyba v nastaveniach ho nesmie vypnúť ani presunúť.
- **Snímanie vstupov:** kanály sú na portoch J, G, K, E, I, H, B. Sken 10 kHz číta každý
  port raz. Priradenie funkcií sa prepočíta na tabuľku port + maska pri štarte a pri
  zmene nastavenia, takže nič nespomalí — ani sondu, ktorú krokovač číta pri každom
  kroku.
- **Výstupy počas štartu:** po resete sú piny STM32 v analógovom režime (vysoká
  impedancia). Pull-down na shielde pred každým budičom zaručí, že sa počas štartu a
  nahrávania nič nezopne. Žiadny výstupný pin nie je pin JTAG/SWD ani BOOT.
- **D7 (PB4) ostáva rezerva:** po resete patrí PB4 rozhraniu JTAG (NJTRST) a má zapnutý
  pull-up — ako výstup by počas štartu zopol.
- **D14–D16** sú sériové piny (Serial3/Serial4), ktoré nepoužívame, takže slúžia ako GPIO.
  D10–D13 sú SPI na konektore UNO; SPI ostáva dostupné na 6-pinovom konektore.

## Požiadavky na shield (podľa skupín)

| Skupina | Riešenie na shielde |
|---|---|
| Vstupy IN1–IN20 | izolované: 4-kanálové optočleny so striedavým vstupom (napr. LTV-844, TLP290-4), päť štvoríc, COM každej štvorice prepojkou na doske (predvolene +24 V = NPN); predradný odpor 4,7 kΩ (12–30 V), RC filter, LED |
| STOP | samostatný optočlen, inak ako vstupy |
| DRV PWR | samostatný optočlen so vstupom pre napätie driverov 12–60 V DC (zdroj prúdu ~2 mA alebo predradný odpor a zenerka), vlastná dvojica svoriek + / −, oddelená od 24 V; pri servách napájaných 230 V ich výstup „servo ready“ namiesto napätia (230 V nie na shield) |
| STEP/DIR (8), ENABLE | AM26C31, diferenčne 5 V, pull-down na vstupoch; ENABLE aj ako 24 V NPN výstup (Delta ASDA-A2: SON je 24 V vstup) |
| OUT1–OUT12 | izolované tranzistorové výstupy NPN / open drain, 500 mA, do 24 V: optočlen + N-MOSFET alebo DMOS budič (napr. TBD62083) s nulovými diódami; bez signálu z GIGA vypnuté; istenie a LED |
| BRAKE | izolované budenie + výkonový N-MOSFET ≥ 5 A, nulová dióda (voliteľne so zenerkou pre rýchlejšie zabrzdenie), samostatná svorka a istenie |
| 0–10 V | DAC A12 → delič → izolačný zosilňovač (napr. AMC1311, ACPL-C87) → op-amp na poľnej strane napájaný z 24 V; prepojka 0–5 / 0–10 V |
| Napájanie | jeden zdroj 24 V: prepólovanie, PTC, TVS; logika 5 V ≥ 2 A z izolovaného DC/DC 24 → 5 V (GIGA, displej, USB kľúč) |

## Dopad na firmvér

- **`cpu_map.h`:** tabuľka kanálov `IN1`–`IN20` a `OUT1`–`OUT12` (pin pre každý kanál) a
  pevné piny: kroky, smery, enable, `ESTOP_PIN` (D49), `DRVPWR_PIN` (D53), `BRAKE_PIN`
  (D16), PWM a DAC.
- **Nastavenia:** priradenie funkcia → kanál `$60`–`$78` a `$80`–`$92`, masky NC/NO
  `$79` (vstupy, STOP a DRV PWR) a `$93` (výstupy), ďalšie časy a voľby `$48`–`$59`,
  napájanie driverov `$94`, `$95` — viď PLAN.md, časti 1 a 2. Shield v4: menič cez RS485
  `$33`–`$37`, PWM na OUT13 `$38`, panel obsluhy `$39`, `$44`, `$45`.
- **README:** tabuľka zapojenia sa prepíše podľa tejto tabuľky.
