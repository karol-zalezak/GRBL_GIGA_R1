# Plán ďalšieho vývoja

Stav k 12. 10. 2026: hotový a na doske overený port Grbl 1.1h (M7), displej
s dotykovým ovládaním (M4) — stránky DRO, JOG, OVR, I/O, `$$`, RGB dióda,
CoreXY, 4 osi XYZA, 8 vstupov, 8 výstupov. Podrobnosti a merania sú v README.

**Stav k 13. 10. 2026 — časti 1 a 2 sú vo firmvéri hotové** a otestované na
doske bez stroja (`tests/test_io.py`, `test_axes.py`, `test_touch.py`, vstupy
simulované háčikmi `$TIN` a `$TSW`, ktoré idú tou istou cestou ako optočlen):
kanály IN1–IN20 / OUT1–OUT12 s funkciami `$60`–`$93`, koncáky ± (jeden alebo
dva vstupy na os, výjazd z koncáka, `[MSG:Limit X+]`, homing na strane podľa
`$23`, `ALARM:20`), alarmy serv s impulzom resetu, centrálne STOP na EXTI4
(`ALARM:18`, ~11 µs), brzda Z v oboch režimoch (`$59`), čakanie na otáčky a
porucha meniča (`ALARM:14`, `ALARM:19`), klieština s blokovaním, `M6` ako pauza,
oneskorenie kroku po zmene smeru (`$56`), displej (I/O stránka, E-STOP, 87
nastavení). Popis v README („Inputs and outputs“).

**Stav k 13. 10. 2026 — časť 4 (USB kľúč) je hotová a otestovaná so skutočným
kľúčom** (8 GB, GPT + FAT32 aj exFAT): USB host beží na M4 (blok OTG_HS, Grbl na M7 ho nevidí),
súbor prúdi cez kruhový buffer 8 kB v SRAM4 do hlavnej slučky Grbl. Kontrola
`$C` pred prvým spustením (počet riadkov, rozsah X/Y/Z), opakované spustenie
bez kontroly, `M0`/`M1` pauza, `M6`, STOP bez straty polohy, chyba → zastavenie,
riadky z PC počas behu odmietnuté (`error:48`), priebeh v statuse (`|SD:`), na
displeji záložka FILE. Overené testami `tests/test_job.py` a `test_touch.py`
priamo na kľúči (testovací „RAM súbor“ z doby bez kľúča je odstránený). Krátke
segmenty z kľúča: 1417 riadkov/s (z PC 1282). Popis v
README („Programs from a USB stick“). Knižnica `Arduino_USBHostMbed5` je
prevzatá do `src/m4/usbhost/` (na M4 by inak siahla na USB-C port Grbl).

Doplnené po teste s kľúčom: vlastný **FatFs R0.15** (`src/m4/fatfs/`) — mbed
FatFs nepozná exFAT ani GPT a testovací kľúč mal GPT; **zápis z PC na kľúč v
GIGA** cez USB-C (`$FW`, `$FD`, `tools/usbfiles.py` s `put`/`rm`/`sync`,
overenie CRC-32, dátum súboru z PC zostáva); **formátovanie** (`$FF=ERASE`,
tlačidlo FORMAT na displeji podržať 3 s; FAT32 do 32 GB, nad tým exFAT).
Čítanie z kľúča 511 kB/s, zápis z PC 133 kB/s, `demo_complex.nc` z kľúča 148,9 s.
Vytiahnutie kľúča počas behu: zastaví hneď (Hold do 50 ms, poloha zachovaná, bez
alarmu). Kľúč naformátovaný na GIGA (FAT32 aj exFAT, MBR ako z Windows) Windows
otvorí, súbory zhodné, `chkdsk` bez chýb; súbory s diakritikou z PC GIGA zobrazí
a spustí. Kľúč s NTFS: displej ho ohlási, FORMAT (podržať 3 s) z neho
spraví FAT32 za 6 s.

**Stav k 14. 10. 2026 — vstup napájania driverov (DRV PWR, D53) je vo firmvéri
hotový** a otestovaný bez shieldu (`$TIN=22`, `tests/test_io.py` skupina
`drvpwr`). Keď GIGA beží z USB a drivery stratia napájanie, Grbl to zistí do
2 ms: zabrzdí Z, vypne enable, zastaví pohyb a prejde do `ALARM:22` (poloha
stratená) — počas pohybu, v pokoji, pri homingu aj v inom alarme. `$H` a `$X`
odmietne (`error:50`), kým napájanie nie je späť aspoň `$95` ms; potom
`[MSG:Driver power on]`. Sledovanie zapína `$94` (predvolene vypnuté — holá doska
číta na D53 „bez napájania“). Nastavenia z verzie 21 sa pri aktualizácii prevezmú.
Popis v časti 2 a v README („Driver power“), obvod na shielde v časti 5.

**Stav k 14. 10. 2026 — ručné koliesko (MPG) je vo firmvéri hotové** a otestované
bez pendantu (`tests/test_mpg.py`, koliesko simulované háčikom `$TMPW` cez
skutočné hrany, ktoré počíta TIM8). Pendant EHDW-BG6L z dávkovača: koliesko A/B na
D54/D59 (TIM8 v režime enkodéra), výber osi X/Y/Z/4 a kroku ×1/×10/×100, LED,
všetko cez konektor kamery J3 displeja. Os nasleduje rýchlosť kolieska a zastaví
sa do ~100 ms po ňom; rýchlejšie točenie, než os stihne, sa zahodí. Nastavenia
`$96`–`$98`, zobrazenie na stránke JOG. Zapojenie pendantu a požiadavky na
shield v časti 5 („Ručné koliesko“); rozloženie na shielde zatiaľ nie je.

**Ešte otvorené:**

- **Watchdog (IWDG)** — zámerne odložený. Po zapnutí sa nedá vypnúť; treba
  overiť, ako sa správa pri nahrávaní cez DFU (1200 bd reset → bootloader) a
  pri mazaní sektora nastavení vo flash (blokuje 1–2 s), a nastaviť ho až na
  doske so shieldom.
- **Kalibrácia 0–10 V** (časť 3) — po osadení izolačného zosilňovača
  (napätie pri `$30` a posun nuly), čísla nastavení podľa meraní.
- Test na stroji: skutočné spínače, relé, brzda (časy `$48`–`$50`), reakčný
  čas STOP osciloskopom.

Ďalej plánované:

1. [Vstupy a výstupy](#1-vstupy-a-výstupy) — univerzálne kanály IN1–IN20 / OUT1–OUT12 s funkciami z nastavení, brzda, klieština, signály meniča
2. [Centrálne STOP](#2-centrálne-stop) — signál z bezpečnostného relé, NC/NO nastaviteľné; napájanie driverov
3. [Výstup 0–10 V](#3-výstup-010-v-pre-menič) — DAC a op-amp s galvanickým oddelením
4. [Spúšťanie programov z USB kľúča](#4-spúšťanie-programov-z-usb-kľúča-usb-a) — beh bez PC
5. [Shield pre GIGA](#5-shield-pre-giga-hardvér) — 24 V periférie, zadanie pre návrh dosky

**Odporúčané poradie:**

1. **Tabuľka pinov** — **schválená** (13. 10. 2026): [PINOUT.md](PINOUT.md), návrh v2,
   bloky bez krížení. Z nej vychádza firmvér (`cpu_map.h`) aj doska.
2. **Centrálne STOP** (časť 2) — bezpečnosť, malá úloha, dá sa vyskúšať hneď s jedným
   optočlenom.
3. **Vstupy a výstupy** (časť 1) — firmvér, dá sa otestovať na GIGA aj bez shieldu.
4. **0–10 V** (časť 3) — vo firmvéri takmer hotové (DAC), hlavná práca je na shielde.
5. **USB kľúč** (časť 4) — hotové a otestované s kľúčmi (FAT32, exFAT, GPT, vytiahnutie, Windows).
6. **Shield** (časť 5) — návrh paralelne, výroba JLCPCB ako pri UNO shielde.

**Nastavenia pri aktualizácii firmvéru:** nový firmvér má iný formát uložených
nastavení. Grbl to pri štarte zistí a všetky nastavenia vráti na predvolené
hodnoty — vrátane offsetov G54–G59. Prevod starých hodnôt do nového formátu
nerobím: stroj ešte nie je uvedený do prevádzky a nastavovanie bude riešiť
vlastná aplikácia. Stačí si pred aktualizáciou uložiť výpis `$$` a `$#`.
Výnimka: verzia 22 (napájanie driverov) len pridáva `$94` a `$95` na koniec,
takže nastavenia z verzie 21 sa prevezmú bez zmeny.

---

## 1. Vstupy a výstupy

### Univerzálne kanály, funkcie z nastavení

Shield má **20 univerzálnych vstupov IN1–IN20** a **12 univerzálnych
výstupov OUT1–OUT12**. Kanál je pevne spojený s pinom GIGA
([PINOUT.md](PINOUT.md)), ale **ktorú funkciu má, určí nastavenie**: pre
každú funkciu jedno nastavenie s číslom kanála (0 = funkcia nepoužitá).

**Pevné ostávajú** len piny, ktoré to potrebujú: kroky, smery a enable
(diferenčné budiče, port J), centrálne STOP (vlastné prerušenie — chyba v
nastaveniach ho nesmie vypnúť ani presunúť), napájanie driverov (DRV PWR, D53,
vlastný optočlen — viď časť 2), brzda Z (výkonový výstup 5 A), PWM a 0–10 V.

**Funkcie vstupov:**

| Nastavenie | Funkcia | Predvolený kanál |
|---|---|---|
| `$60`–`$67` | koncák X−, X+, Y−, Y+, Z−, Z+, A−, A+ | IN1–IN8 |
| `$68`–`$71` | alarm serva X, Y, Z, A | IN9–IN12 |
| `$72` | sonda | IN13 |
| `$73`–`$76` | tlačidlá RESET, HOLD, START, dvere | IN14–IN17 |
| `$77` | vreteno na otáčkach (relé meniča) | IN18 |
| `$78` | porucha vretena (relé meniča) | IN19 |
| `$79` | NC/NO — maska pre IN1–IN20, STOP (bit 20) a DRV PWR (bit 21) | — |

IN20 je predvolene voľný.

**Funkcie výstupov:**

| Nastavenie | Funkcia | Predvolený kanál |
|---|---|---|
| `$80`, `$81` | vreteno chod (menič FWD), vreteno CCW (REV) | OUT1, OUT2 |
| `$82`, `$83` | chladenie `M8`, hmla `M7` | OUT3, OUT4 |
| `$84`–`$89` | AUX0–AUX5 (`M62`–`M65 P0`–`P5`, AUX0 = odsávanie) | OUT5–OUT8, OUT12, nepriradený |
| `$90` | otvorenie klieštiny | OUT9 |
| `$91` | reset alarmu serv (impulz) | OUT10 |
| `$92` | reset poruchy meniča (impulz) | OUT11 |
| `$93` | polarita — maska pre OUT1–OUT12 | — |

**Pravidlá priradenia:**

- **Staré nastavenia:** `$5`, `$6`, `$44`, `$45` a `$47` sa rušia. Nahrádzajú ich
  priradenie a masky `$79` a `$93`. `$46` (AUX so vretenom) ostáva.
- **Výstupy:** dve funkcie na jednom výstupe sa odmietnu (`error`).
- **Vstupy:** jeden vstup môže mať viac funkcií len pre koncáky tej istej osi (viď
  nižšie). Ostatné kombinácie sa odmietnu.
- **Kedy sa dá meniť:** len v Idle alebo v alarme. Pri zmene sa dotknuté výstupy vypnú.
- **Voľný vstup** bez funkcie sa aj tak zobrazuje na displeji a v `$I` (neskôr napr. pre
  `M66`).
- **Rýchlosť:** priradenie sa prepočíta na tabuľku port + maska pri štarte a pri zmene
  nastavenia. Snímanie 10 kHz ani sonda, ktorú krokovač číta pri každom kroku, sa
  nespomalia.
- **Displej:** I/O stránka ukáže kanály s menami funkcií. V editore `$$` sa kanál zadá
  číslom a NC/NO prepínačmi (21 prepínačov — editor pre masky treba rozšíriť).

**Ďalšie nové nastavenia** (všetky časy univerzálne nastaviteľné):

| Nastavenie | Význam | Jednotka |
|---|---|---|
| `$48` | brzda: enable → odbrzdenie (t_on) | ms |
| `$49` | brzda: odbrzdenie → prvý krok (čas mechanického odbrzdenia) | ms |
| `$50` | brzda: zabrzdenie → enable vypnutý (t_off) | ms |
| `$51` | dobeh vretena — klieština sa smie otvoriť až po ňom | ms |
| `$52` | vreteno na otáčkach: timeout | ms |
| `$53` | vreteno na otáčkach: reakcia pri strate (0 ignorovať, 1 hold, 2 alarm) | — |
| `$54` | pevné oneskorenie po `M3`/`M4`, ak nie je priradený vstup otáčok | ms |
| `$55` | dĺžka impulzu resetu (serva aj menič) | ms |
| `$56` | oneskorenie kroku po zmene smeru | µs |
| `$57` | centrálne STOP zapnuté | 0/1 |
| `$58` | `M6`: 0 = chyba (Grbl), 1 = pauza na ručnú výmenu nástroja | 0/1 |
| `$59` | brzda: 0 = ovláda ju driver (výstup BRAKE sa nepoužíva), 1 = výstup BRAKE | 0/1 |
| `$94` | sledovanie napájania driverov (DRV PWR, D53) zapnuté | 0/1 |
| `$95` | napájanie driverov musí byť späť aspoň tak dlho, kým sa povolí `$H`/`$X` | ms |

### Koncové spínače ± (predvolene IN1–IN8)

- **Jeden alebo dva vstupy na os:** nastaví sa pre každú os zvlášť priradením.

  | Priradenie | Význam |
  |---|---|
  | X− a X+ na rôznych kanáloch | dva vstupy: firmvér vie, ktorý koniec zopol |
  | X+ na tom istom kanáli ako X− | jeden vstup, oba spínače v sérii (klasický Grbl) |
  | X+ = 0 | os má spínač len na strane − |

- **NC/NO:** pre každý kanál zvlášť (`$79`).
- **Hard limit:** ktorýkoľvek spínač → `ALARM:1` ako dnes. Pri dvoch vstupoch sa ohlási
  aj **ktorý** (`[MSG:Limit X+]`, displej, `$I`). Status `Pn:` ostáva po osiach (X Y Z
  A).
- **Výjazd z koncáka:** tento port vyhodnocuje hard limit podľa úrovne. S jedným vstupom
  na os po `ALARM:1` a UNLOCK hneď nasleduje ďalší alarm a z koncáka sa dá vyjsť len s
  vypnutým `$21`. S dvoma vstupmi firmvér dovolí jog len smerom **od** zopnutého
  spínača a hard limity ostanú zapnuté.
- **Homing:** použije spínač na strane podľa `$23`. Pri dvoch vstupoch je zopnutie
  opačného spínača počas homingu okamžitý alarm — odhalí zlé zapojenie alebo zlý smer.
- **Slave A (gantry):** A− / A+ sú spínače druhého motora portálu (vyrovnávanie pri
  homingu ako dnes).

### Servo enable a brzda osi Z (pevný výstup BRAKE)

Elektromagnetická brzda 24 V, bez prúdu zabrzdená. Má **vlastný výkonový
MOSFET do 5 A na pevnom pine** (D16), mimo univerzálnych výstupov. Výstup
zapnutý = cievka pod prúdom = **odbrzdené**. Riadi sa automaticky podľa servo
enable, s nastaviteľným oneskorením pri zapínaní aj pri vypínaní:

- **Zapnutie:** enable ON → čakať **t_on** (`$48`, servo nabehne a drží) → odbrzdiť →
  čakať čas mechanického odbrzdenia (`$49`) → až potom prvý krok.
- **Vypnutie:** zabrzdiť → čakať **t_off** (`$50`, brzda dosadne) → enable OFF. Os ani na
  okamih nevisí bez držania.
- **Časy:** údaje brzdy nie sú známe, preto sú všetky tri univerzálne nastaviteľné v ms
  (predvolene napr. 200 / 100 / 200 ms, kým sa nezmerajú).
- **Okamžite zabrzdiť:** pri alarme, resete, centrálnom STOP (priamo v jeho prerušení)
  a pri štarte firmvéru.
- **Napájanie cievky** cez povoľovaciu dráhu bezpečnostného relé, aby pri STOP brzda
  zabrzdila aj hardvérovo.
- **Kedy je enable zapnutý:** riadi sa `$1`. Pri `$1=255` je enable trvalo zapnutý
  (okrem alarmu), takže brzda je odbrzdená celý čas a t_on sa uplatní len po alarme
  alebo resete.
- **Homing:** brzda je počas homingu odbrzdená ako pri bežnom pohybe.
- **Drivery to vedia aj samy:** Delta ASDA-A2 má výstup BRKR, Nanotec C5 má na
  výstupe 1 špeciálnu funkciu „brzda“. Ak sa použije brzda drivera, náš výstup BRAKE
  ostane nevyužitý.
- **Nastavenia:** `$48`–`$50` (ms).
- **Displej:** stav „Z BRAKE“ (zabrzdené/odbrzdené) na I/O stránke a pri osi Z v DRO.

### Klieština — pneumatický ventil 24 V (predvolene OUT9)

- **Kedy sa smie otvoriť:**
  - vreteno stojí: výstup vretena vypnutý aspoň po nastavený čas dobehu a vstup
    „vreteno na otáčkach“ (ak je priradený) nehlási chod;
  - stroj je v Idle alebo v Hold (nie počas pohybu);
  - nie je aktívne centrálne STOP.
- **Ovládanie:** tlačidlo na displeji (podržať) a neskôr M-kód pre makro výmeny nástroja.
- **Blokovanie vretena:** `M3`/`M4` sa odmietne, kým je klieština otvorená.
- **Fail-safe:** ventil bez prúdu = klieština zovretá (pružina). Napájanie ventilu cez
  bezpečnostné relé.
- **Nastavenia:** čas dobehu vretena `$51` (ms).

### Signály meniča: otáčky, porucha, reset (predvolene IN18, IN19, OUT11)

Menič má reléový výstup „otáčky dosiahnuté“, reléový výstup poruchy a
digitálny vstup na reset poruchy.

- **Vreteno na otáčkach (IN18):**
  - po `M3`/`M4` a po každej zmene S za chodu Grbl počká (ako pri `G4`), kým relé
    nepotvrdí otáčky, a až potom pokračuje rezaním;
  - **timeout** (`$52`, ms): ak otáčky neprídu, `ALARM:14` „vreteno nedosiahlo otáčky“
    (rovnaké číslo ako v grblHAL) a program sa zastaví;
  - **strata signálu počas rezania** (preťaženie, šmyk remeňa): reakcia podľa `$53` —
    0 ignorovať, 1 feed hold, 2 alarm. Počas rozbehu po zmene S sa nesleduje;
  - **bez priradeného vstupu:** pevné oneskorenie po `M3`/`M4` (`$54`, ms);
  - **laserový režim** (`$32=1`): nečaká sa.
- **Porucha vretena (IN19):** relé poruchy meniča zopne → riadené zastavenie pohybu (ako
  feed hold, poloha sa nestratí), vypnutie vretena a `ALARM:19` „porucha vretena“. Kým
  porucha trvá, RESET aj UNLOCK sa odmietnu.
- **Reset poruchy meniča (OUT11):** impulz dĺžky `$55` (ms) pri RESET alebo UNLOCK,
  keď je porucha aktívna, alebo tlačidlom na displeji.
- **Displej:** panel vretena ukáže „rozbieha sa…“, „na otáčkach“, „otáčky nedosiahnuté“
  alebo „porucha meniča“.
- **Nastavenia:** `$52`–`$55`.

### Reset alarmu serv (predvolene OUT10)

Impulz dĺžky `$55` (ms) na vstupy ARST všetkých serv (Delta ASDA-A2: 24 V
vstup) pri RESET alebo UNLOCK po `ALARM:17`, prípadne tlačidlom na displeji.
Nanotec C5 taký vstup nemá (viď časť 5).

### Oneskorenie kroku po zmene smeru (`$56`)

Nanotec C5 potrebuje medzi zmenou signálu smeru a ďalším krokom **35 µs**
(manuál C5, režim clock/direction). Grbl dnes dá krok hneď po zmene smeru.
Pri obratoch, kde os prechádza nulou rýchlosti (oblúky), je perióda kroku aj
tak dlhá, ale v ostrom rohu môže os zmeniť smer pri nenulovej rýchlosti a
prvý krok by prišiel skôr.

- **Riešenie:** ak sa v prerušení krokovača zmení smer, krok sa nevydá hneď, ale až po
  `$56` µs (časovač konca impulzu TIM6 ho odloží). Pri ostatných krokoch sa nič nemení.
- **Predvolene** 0 (ako dnes), pre C5 nastaviť 35–40, pre Deltu podľa jej manuálu (rádovo
  jednotky µs).
- **Test:** pulzy merané na doske — po každej zmene smeru odstup ≥ `$56`, inak strop
  200 kHz a 0 zlúčených impulzov bez zmeny.

### Zmeny vo firmvéri a testy

- **Firmvér:**
  - `cpu_map.h`: tabuľka kanálov IN1–IN20, OUT1–OUT12 a pevných pinov;
  - vrstva priradenia funkcia ↔ kanál (vstupy aj výstupy), NC/NO po kanáloch;
  - limits.c (± spínače, jeden/dva vstupy na os, homing, výjazd), sonda a tlačidlá cez
    priradenie;
  - `outputs.[ch]` (12 kanálov, AUX0–AUX5, klieština s blokovaním, impulzy resetov);
  - brzda: pevný výstup, enable s oneskoreniami;
  - krokovač: oneskorenie kroku po zmene smeru (`$56`);
  - vreteno: čakanie na otáčky, sledovanie, porucha, `ALARM:14` a `ALARM:19`;
  - parser (`P0`–`P5`, blokovanie `M3`/`M4` pri otvorenej klieštine);
  - nastavenia (nová verzia, `$48`–`$58` a `$60`–`$93`);
  - zdieľaná pamäť (vstupy na 32 bitov, nová verzia bloku), `$I`;
  - displej (I/O stránka s menami funkcií, tlačidlá klieštiny a resetov, stav brzdy a
    vretena, `$$` editor pre priradenie a masky);
  - README (tabuľka zapojenia podľa PINOUT.md).
- **Testy:**
  - rozšíriť `test_axes.py` a `test_touch.py`;
  - priradenie: presun funkcie na iný kanál, odmietnutie konfliktu, jeden vstup na os;
  - vstupy trikom s inverziou (ako `ALARM:17`) a káblikom na GND;
  - brzdu a enable cez `$I` s časovými značkami;
  - vreteno: s inverziou otáčky hneď → pokračuje, bez nej timeout `ALARM:14`, strata za
    chodu → hold/alarm, porucha → `ALARM:19` a impulz resetu;
  - blokovanie klieštiny: `M3` pri otvorenej klieštine → chyba, otvorenie pri bežiacom
    vretene → odmietnuté.

---

## 2. Centrálne STOP

**Rozdelenie úloh:**

- **Bezpečnostné relé** (dnes Phoenix PSR-SCP-24UC/ESL4, do budúcna hocijaké): stráži
  kanály núdzového tlačidla a pri stlačení **odpojí napájanie** serv, meniča, brzdy a
  ventilov. Robí to nezávisle od firmvéru — to je skutočná bezpečnostná funkcia.
- **GIGA:** signalizačný kontakt relé vedie cez optočlen na pevný vstup STOP. Grbl podľa
  neho **okamžite zastaví generovanie krokov**, aby drivery bežiace ešte z nabitých
  kondenzátorov nedostali ďalší krok, a prejde do alarmu, aby po návrate napájania stroj
  sám nepokračoval.

**NC/NO nastaviteľné** (bit STOP v `$79`), aby sa dalo použiť akékoľvek relé:

| Kontakt relé | Nastavenie |
|---|---|
| zatvorený pri STOP (napr. 41/42 na PSR-…/ESL4) | STOP = kontakt zopnutý |
| zatvorený pri OK (napr. povoľovacia dráha 33/34) | STOP = kontakt rozopnutý — fail-safe aj pre prerušený vodič |

**Prečo nestačí tlačidlo RESET:** Grbl ho berie ako obyčajný soft reset. Po
resete je stroj v Idle — aj keď je STOP stále stlačený — a prijal by ďalší
G-kód. Centrálne STOP potrebuje vlastný vstup, ktorý reaguje okamžite a drží
alarm, kým signál nie je späť.

**Pin:** D49 (PE4), prerušenie EXTI4 s vlastným vektorom, ktoré nič iné
nepoužíva — [PINOUT.md](PINOUT.md).

**Ako rýchlo:**

- **Prerušenie na hrane:** EXTI s **NVIC prioritou 0** — rovnakou ako časovač konca
  impulzu, nad krokovačom aj snímaním vstupov. Kritické sekcie mbed (USB) maskujú len
  prioritu 3 a nižšiu (BASEPRI, viď README), takže reakciu nič nezdrží.
- **Snímanie 10 kHz:** ten istý vstup sa kontroluje aj tam (TIM17). Zachytí stav pri
  zapnutí a prípadne ušlú hranu.
- **Bez filtra zákmitov smerom k STOP:** reaguje sa na prvú hranu. Návrat do OK sa
  akceptuje až po ustálení (napr. 100 ms).
- **Cieľ:** posledný krokový impulz do 50 µs od zmeny signálu.

**Čo urobí obsluha prerušenia:**

1. Zastaví generovanie krokov (TIM7/TIM6) a nastaví krokové piny do kľudu.
2. Zabrzdí os Z (výstup BRAKE vypne), vypne servo enable, výstupy vretena, PWM, DAC,
   klieštinu, chladenie a AUX — priamym zápisom do GPIO, žiadne volania mbed.
3. Nastaví alarm „E-stop“ a reset. Hlavná slučka vyprázdni plánovač, ohlási `ALARM:18`
   (nový kód), `[MSG:E-stop]` a v statuse `Pn:E`. Poloha sa považuje za stratenú, lebo
   motory boli bez napájania.

**Znovuspustenie:**

- **Po odistení tlačidla** a štarte bezpečnostného relé sa signál vráti, ale Grbl
  ostane v alarme. Treba RESET (Ctrl-X, tlačidlo na displeji alebo vstup RESET) a potom
  HOME, prípadne UNLOCK. Homing je odporúčaný, lebo motory boli bez napájania.
- **Kým je STOP aktívne**, RESET aj UNLOCK sa odmietnu a stroj ostane v alarme.
- **Pri zapnutí so STOP** začne Grbl rovno v alarme „E-stop“.

**Displej a hlásenia:**

- **Displej:** cez celú plochu červený pruh „E-STOP“ s návodom (odistiť tlačidlo,
  zopnúť bezpečnostné relé, potom RESET a HOME), RGB dióda bliká načerveno rýchlo. Na
  I/O stránke indikátor „SAFETY OK / E-STOP“.
- **Hlásenia na USB:** `ALARM:18`, `[MSG:E-stop]`, `Pn:E`, v `$I` stav vstupu, počet
  zásahov a posledný reakčný čas.

**Nastavenia:** zapnutie (predvolene vypnuté, kým nie je relé zapojené) a
NC/NO (bit v `$79`).

**Watchdog (IWDG) na M7:** ak firmvér zamrzne, procesor sa resetuje a
výstupy prejdú do kľudu. Dnes watchdog nie je; doplniť spolu so STOP.

**Testy:**

- **Bez hardvéru:** inverzia vstupu (ako pri `ALARM:17`) → `ALARM:18`. RESET a UNLOCK sa
  odmietnu, kým STOP trvá.
- **Reakčný čas:** výstup GIGA prepojený káblikom na vstup STOP. Firmvér zaznamená
  takty DWT pri hrane a pri poslednom krokovom impulze (diagnostika v `$I`). Opakovať
  počas pohybu pri 200 kHz na 4 osiach, počas streamovania z USB a počas homingu.
  Cieľ < 50 µs, žiadny impulz po zastavení.
- **Brzda:** výstup BRAKE sa vypne v tom istom prerušení; zmerať oneskorenie.
- **Znovuspustenie:** návrat signálu sám nič nerozbehne.
- **Pri vypnutom STOP:** všetky doterajšie testy prechádzajú bez zmeny.

### Napájanie driverov (DRV PWR, D53) — doplnené 14. 10. 2026

**Problém:** GIGA napájaná z USB beží ďalej, aj keď drivery stratia napájanie.
Počítadlo krokov potom hlási polohu, ktorú motory už nedržia — po zhomovaní a
vypnutí driverov sa poloha stratí a Grbl sa o tom nedozvie.

**Riešenie:** samostatný vstup **DRV PWR na D53** (PG7, pevný pin) s vlastným
optočlenom na shielde, ktorý sníma napätie driverov (pin v nule = napätie je).
Číta ho sken vstupov 10 kHz.

- **Strata napájania** (chýba 2 ms, `DRVPWR_LOSS_SCANS`): hneď zabrzdí Z a vypne
  enable — drivery os už nedržia a brzda odbrzdená výstupom BRAKE by nechala Z
  spadnúť. Potom `mc_reset()` (vyprázdni plánovač, vypne vreteno, chladenie, zruší
  program z kľúča) a **`ALARM:22`**. Platí počas pohybu, v pokoji (motor bez
  napájania sa dá pootočiť rukou), pri homingu (nie `ALARM:6`) aj v inom alarme —
  `ALARM:22` sa vtedy ohlási tiež, lebo poloha zachovaná po predošlom alarme už
  neplatí.
- **Kým napájanie chýba** a ešte `$95` ms po jeho návrate (čas štartu driverov,
  predvolene 1000 ms), `$H` aj `$X` sa odmietnu s **`error:50`**. Potom
  `[MSG:Driver power on]` a stačí HOME alebo UNLOCK. Ak by sa stroj dostal z alarmu
  inou cestou, kým napájanie chýba, hneď sa doňho vráti.
- **Pri zapnutí bez napájania driverov** začne Grbl v `ALARM:22`.
- **Nie je to kritický alarm:** reset netreba.
- **Nastavenia:** `$94` sledovanie zapnuté (predvolene 0 — holá doska číta na D53
  „bez napájania“), `$95` čas v ms, bit 21 v `$79` vstup invertuje (napr. kontakt
  „servo ready“, ktorý je pri pripravenosti rozopnutý).
- **Hlásenia:** `ALARM:22`, `error:50`, `[MSG:Driver power on]`, v `$I`
  `drv=<stav>/<počet strát>` (0 nesledované, 1 bez napájania, 2 späť, ale `$95` ešte
  neuplynul, 3 OK).
- **Displej:** na I/O stránke pri DRIVES indikátor PWR (červený / oranžový / zelený,
  tmavý keď sa nesleduje), na riadku správ „Drives have no power – switch them on“,
  UNLOCK a HOME sú sivé, kým sa napájanie nevráti.
- **Testy bez shieldu** (`$TIN=22`, `test_io.py` skupina `drvpwr`): strata počas
  pohybu (ani krok po nej, enable a brzda vypnuté), `$X`/`$H` → `error:50`, návrat
  a čakanie `$95`, strata v pokoji s drivermi zapnutými (`$1=255`), v alarme (bez
  resetu), počas homingu, zapnutie `$94` bez napájania, inverzia `$79`, `$94=0`;
  štart dosky bez napájania → Alarm, `drv=1`. Všetko prechádza.

---

## 3. Výstup 0–10 V pre menič

**Dnes:** rýchlosť vretena ide ako PWM na D6 a súčasne 0–3,3 V z DAC na A12
(0 V pri S0, 3,3 V pri `$30`).

**Rozhodnuté: DAC a op-amp s galvanickým oddelením.**

```
DAC A12 (0–3,3 V) → delič na 0–2 V → izolačný zosilňovač → op-amp ×5 (×2,5) → 0–10 V (0–5 V)
        logická strana             │ izolácia │          poľná strana 24 V
```

- **Izolačný zosilňovač** so vstupom 0–2 V (napr. AMC1311 alebo ACPL-C87). Napája sa z
  oboch strán: logická strana z 3,3/5 V, poľná strana z 5 V stabilizátora z poľných
  24 V. Ďalší izolovaný menič netreba.
- **Op-amp na poľnej strane**, napájaný z 24 V, zosilní na 0–10 V. Prepojka 0–5 / 0–10 V.
- **Zem výstupu = 24V0 poľa** — tá istá, voči ktorej spínajú výstupy pre menič (FWD, REV,
  reset), takže menič má všetky riadiace signály voči jednej zemi a nič ho nespája s
  logikou ani s PC.
- **Firmvér:** DAC už dnes sleduje S. Pribudne kalibrácia (napätie pri `$30` a posun
  nuly), aby sa vyrovnali tolerancie zosilňovačov.
- **A13 (DAC1)** ostáva v rezerve pre druhý kanál rovnakým zapojením.
- **Bezpečnosť:** analógová hodnota nie je bezpečnostný signál. Vreteno spúšťa a zastavuje
  výstup FWD/REV a napájanie meniča odpojí bezpečnostné relé.
- **Testy:** napätie zmerať multimetrom pri S0, S`$30`/2, S`$30`; M5, reset a STOP → 0 V;
  linearita v 10 bodoch po kalibrácii.

---

## 4. Spúšťanie programov z USB kľúča (USB-A)

**Cieľ:** stroj beží bez PC. Na displeji sa vyberie súbor `.nc` z USB kľúča,
spustí sa a sleduje priebeh. PC cez USB-C ostáva voliteľné — UGS môže dej
sledovať (stavové hlásenia, `!`, `~`, override), ale netreba ho na spustenie.

**Hardvér a knižnice:** port USB-A na GIGA je USB host. Jeho napájanie sa
zapína pinom PA_15 (vo variante „USB HOST ENABLE“, D92). Kľúč cez knižnicu
`Arduino_USBHostMbed5` (USBHostMSD) a súborový systém FAT z mbed. Kľúč musí byť
naformátovaný ako **FAT32** (exFAT mbed nepodporuje); overiť dlhé názvy súborov.

**Krok 0 — overovací pokus (najprv, samostatne):** na ktorom jadre pobeží USB host.

- **Preferované M4:** Grbl na M7 ostane nedotknutý; M4 aj tak robí UI a výber súboru.
  Overiť, že knižnica funguje pri preklade pre M4.
- **Záložný variant M7:** prerušenia USB hosta musia ostať pod časovačmi Grbl (dnešné
  stráženie priorít NVIC to robí) a znova zmerať krokovanie (200 kHz, 0 zlúčených impulzov).
- **Výstup pokusu:** výpis súborov z kľúča na displej a rýchlosť čítania.

**Architektúra:**

```
USB kľúč → (M4) čítanie súboru → kruhový buffer riadkov v SRAM4 → (M7) hlavná slučka Grbl
```

- **Zdieľaná pamäť:** v SRAM4 je od 0x3800C000 po 0x3800FBFF ešte asi 15 kB voľných,
  buffer riadkov 4–8 kB sa zmestí.
- **Hlavná slučka:** počas úlohy berie riadky zo „súborového zdroja“ namiesto USB,
  rovnakou cestou ako riadky z PC (parser, plánovač). Tok riadi samotný plánovač —
  ďalší riadok sa vezme, až keď je miesto.
- **Odpovede:** `ok`/`error` z riadkov súboru nejdú na USB.

**Displej — nová stránka FILE** (šiesta záložka; šírka na 6 záložiek stačí):

- **Výber súboru:** zoznam súborov (názov, veľkosť), listovanie, výber.
- **Detail súboru:** počet riadkov, veľkosť, rozsah X/Y/Z (min/max z predbežného
  prechodu súborom), prípadne odhad času.
- **START** (podržať 1 s).
- **Počas behu:** priebeh (riadok/celkom, %, uplynulý a zostávajúci čas) aj na stránke DRO.
  PAUSE = HOLD, pokračovanie = START. STOP = feed hold, počkať na `Hold:0`, potom reset,
  takže sa nestratí poloha (reset počas pohybu by dal `ALARM:3`).

**Pravidlá (rozhodnuté):**

- **Kontrola pred prvým spustením:** súbor najprv prejde režimom `$C` (bez pohybu).
  Opätovné spustenie toho istého súboru (rovnaký názov, veľkosť a dátum) sa už
  nekontroluje.
- **Chyba v súbore → alarm:** úloha sa zastaví, displej ukáže číslo riadka a text chyby.
- **Spustenie:** úloha sa spustí len v stave Idle a keď PC práve neposiela program.
- **PC počas úlohy:** riadky G-kódu z PC sa odmietnu s chybou. Real-time príkazy a `?`
  fungujú ďalej.
- **Pauza programom:** `M0`/`M1` = pauza, pokračuje START. `M2`/`M30` = koniec úlohy.
- **`M6` (výmena nástroja) — rozhodnuté: pauza.** Grbl ho nepodporuje (`error:20`).
  S `$58=1` sa `M6` správa ako pauza na ručnú výmenu: vreteno sa zastaví, displej ukáže
  číslo nástroja, výmena prebehne tlačidlom klieštiny na displeji a pokračuje sa START.
- **Prerušenie:** vytiahnutie kľúča počas behu → feed hold a správa. Alarm, reset alebo
  centrálne STOP → úloha zrušená, displej ukáže, na ktorom riadku.
- **Neskôr:** pokračovanie od riadka N po prerušení (riskantné — modálny stav, poloha).

**Testy:**

- **Porovnanie s PC:** `examples/demo_complex.nc` z kľúča musí dať rovnaký čas (z PC
  152,9 s), rovnakú koncovú polohu a 2595 spracovaných riadkov.
- **Kontrola a opakovanie:** prvé spustenie prejde `$C`, druhé nie; zmenený súbor znova
  áno.
- **Chybový súbor:** zastaví sa na správnom riadku s alarmom.
- **Pauza a koniec:** M0, HOLD/START, STOP bez straty polohy.
- **Ručne:** vytiahnutie kľúča počas behu; súbor s 100 000+ riadkami (rýchlosť čítania
  musí stačiť na ~1300 riadkov/s krátkych segmentov).

---

## 5. Shield pre GIGA (hardvér)

Zadanie pre veľkú dosku periférií. Šablóna návrhu: UNO servo shield v
`D:\ClaudeCode\Grbl_Uno_R4_Wifi\hardware\Grbl_Servo_Shield` — generovaný
skriptami (KiCad), výroba a osadenie JLCPCB. Vzor architektúry: kontrolér
CNCdrive AXBB-E (izolované a neizolované I/O).

### Rozloženie bez krížení

Každý blok shieldu je pripojený na jeden úsek konektorov GIGA v poradí svoriek
([PINOUT.md](PINOUT.md)):

- **Vstupy** — vnútorný stĺpec konektora 2×18 (IN1–IN16) a spodok vonkajšieho stĺpca
  (IN17–IN20, STOP, DRV PWR) → blok optočleniek → svorky vstupov.
- **Servá** — horná časť vonkajšieho stĺpca 2×18 (kroky, smery, enable) → AM26C31 →
  konektory serv na okraji dosky.
- **Výstupy** — horný konektor zľava doprava OUT1…OUT12 a BRAKE → budiče → svorky
  výstupov pozdĺž horného okraja.
- **0–10 V** — A12 na analógovom konektore → izolačný zosilňovač → svorka pre menič.

### Dve napäťové domény (ako AXBB-E)

| Doména | Čo do nej patrí | Zem |
|---|---|---|
| **Logická, neizolovaná** | GIGA, STEP/DIR/ENABLE (diferenčné budiče), vstupná strana izolačného zosilňovača, USB k PC | GND logiky (spoločná s USB a PC) |
| **Poľná 24 V, izolovaná** | vstupy IN1–IN20, STOP a DRV PWR, výstupy OUT1–OUT12, BRAKE, 0–10 V, ENABLE 24 V | 24V0, oddelená optočlenmi a izolačným zosilňovačom |

- **Prečo:** rušenie a prepätia z 24 V strany (cievky, menič, dlhé káble) sa nedostanú do
  GIGA ani cez USB do PC.
- **Napájanie (rozhodnuté):** jeden zdroj 24 V. Logika 5 V z **izolovaného** DC/DC
  24 → 5 V, aspoň 2 A (GIGA, displej, USB kľúč). Overiť v schéme GIGA, či napájať cez pin
  5V (s ideálnou diódou ako na UNO shielde) alebo cez VIN.
- **Vstup 24 V:** ochrana proti prepólovaniu, poistka (PTC), TVS.

### Rozhranie serv (Nanotec C5-01 alebo Delta ASDA-A2)

Jeden konektor na os:

| Signál | Riešenie | Poznámka |
|---|---|---|
| STEP±, DIR± | AM26C31, diferenčne 5 V | obe drivery majú diferenčné vstupy |
| ENABLE± | AM26C31, diferenčne 5 V | Nanotec C5 |
| ENABLE 24 V | izolovaný výstup NPN ako OUT | Delta ASDA-A2 (SON je 24 V vstup) |
| alarm serva | vstup IN9–IN12 priamo v konektore serva | Delta ALRM; C5 viď nižšie |
| reset alarmu | výstup OUT10 (spoločný pre všetky osi) | Delta ARST je 24 V vstup; C5 nemá |
| +24 V, 0 V | pre spoločné svorky I/O drivera | |

**Nanotec C5 podľa manuálu** (C5 USB Technical Manual V1.2.0, 2015):

- **Clock, Direction, Enable:** každý ±, prepínateľné 5 V / 24 V a single-ended /
  diferenčne (spolu pre všetky tri). Clock max. 1 MHz, impulz min. 200 ns. Pri Enable v
  log. 0 sa motor nerozbehne.
- **Smer pred krokom:** po zmene smeru treba 35 µs pred ďalším krokom → `$56` (časť 1).
- **Ďalšie vstupy:** 3 × 24 V single-ended (predvolene koncáky a referenčný spínač drivera
  — ich funkciu treba v C5 vypnúť, ak sa nepoužijú).
- **Výstupy:** 2 × open drain, 24 V / 100 mA. Jediná špeciálna funkcia je **brzda**.
  **Výstup alarmu nemá** — chybu ukazuje len LED.
- **Reset chyby** je len príkazom cez USB alebo z programu NanoJ (riadiace slovo, bit 7),
  **nie digitálnym vstupom**.
- **Dôsledok:** alarm a reset C5 by sa dali doplniť len programom NanoJ v driveri (napr.
  chyba → výstup 2, vstup 3 → reset chyby). Bez neho Grbl o chybe C5 nevie, kým
  neprestane pohyb — Delta ASDA-A2 tu má výhodu (ALRM aj ARST). Novšie verzie firmvéru
  C5 môžu byť ďalej — overiť pri kúpe.

### Vstupy IN1–IN20 a STOP

- **Optočleny so striedavým vstupom (AC input), 4-kanálové** (napr. LTV-844 alebo
  TLP290-4), päť štvoríc. STOP má vlastný jednokanálový optočlen.
- **Všetky snímače NPN:** COM každej štvorice na +24 V **prepojkou na doske**
  (predvolene NPN), nie svorkou — menej svoriek. Prepojením na 0 V by štvorica prijímala
  PNP, keby to niekedy bolo treba.
- **Štvorice podľa predvolených funkcií:**

  | Štvorica | Predvolené funkcie |
  |---|---|
  | IN1–IN4 | koncáky X−, X+, Y−, Y+ |
  | IN5–IN8 | koncáky Z−, Z+, A−, A+ |
  | IN9–IN12 | alarmy serv X, Y, Z, A (svorky v konektoroch serv) |
  | IN13–IN16 | sonda, RESET, HOLD, START |
  | IN17–IN20 | dvere, vreteno na otáčkach, porucha vretena, voľný |

- **Predradný odpor 4,7 kΩ (1206):** pracuje od 12 do 30 V, pri 24 V asi 5 mA a 0,11 W.
- **Strana GIGA:** tranzistor optočlena ťahá pin s pull-upom na 3,3 V k zemi, RC filter.
  LED pri každom vstupe.
- **Rýchlosť:** fototranzistorové optočleny reagujú za desiatky µs — na koncáky, alarmy,
  tlačidlá, STOP aj sondu to stačí.
- **Relé meniča** (otáčky, porucha) sú bezpotenciálové kontakty: jedna strana na 0 V,
  druhá na vstup (NPN).
- **NC spínače** pre koncáky a STOP odporúčané (prerušený vodič spustí ochranu), NO tiež
  funguje — nastaví sa v `$79`.

### Vstup napájania driverov DRV PWR (D53)

- **Účel:** GIGA sa dozvie, že drivery stratili napájanie (časť 2).
- **Vlastný jednokanálový optočlen a vlastná dvojica svoriek + / −** — napätie
  driverov môže mať inú hodnotu aj inú zem ako 24 V shieldu, preto nie spoločný COM
  so štvoricami.
- **Rozsah 12–60 V DC** (Nanotec C5 a krokové drivery na 24–48 V): LED optočlena
  napájať zdrojom prúdu ~2 mA (napr. CL2N3 alebo tranzistorový zdroj prúdu), aby
  stratový výkon nerástol s napätím; ochranná dióda proti prepólovaniu, TVS.
  Alternatíva: predradný odpor a zenerka, prepojkou voliteľné 24 / 48 V.
- **Servá napájané 230 V (Delta ASDA-A2):** 230 V na shield nevodiť. Na vstup
  pripojiť výstup drivera „servo ready“ (SRDY, 24 V NPN) alebo kontakt pomocného
  relé, ktoré drží napätie driverov; polaritu doladí bit 21 v `$79`.
- **Strana GIGA:** tranzistor optočlena ťahá D53 (pull-up na 3,3 V) k zemi, RC filter
  ako pri ostatných vstupoch, LED „DRV PWR“.

### Výstupy OUT1–OUT12

- **Izolované tranzistorové výstupy spínané proti 0 V (NPN / open drain), 500 mA, do
  24 V** — ako izolované výstupy AXBB-E (optočlen + N-MOSFET).
- **Zapojenie s ohľadom na štart:** GIGA → odpor → LED optočlena (4-kanálový s
  jednosmerným vstupom, napr. LTV-847). Na poľnej strane tranzistor optočlena **privádza**
  napätie na gate MOSFETu alebo vstup budiča, ktorý má pull-down. Bez signálu z GIGA je
  výstup vypnutý.
- **Budič:** diskrétne N-MOSFETy, alebo 8-kanálový DMOS budič s integrovanými nulovými
  diódami (napr. TBD62083, 500 mA, 50 V). ULN2803 nie — Darlington má pri 500 mA úbytok
  okolo 1 V a prehrieva sa.
- **Nulové diódy** (v budiči alebo dióda z každého výstupu na svorku „+24 V“ skupiny):
  relé a ventil klieštiny bez externých diód.
- **Ochrana a indikácia:** poistka alebo PTC na skupinu, LED na každom výstupe.
- **Menič:** výstupy NPN idú priamo na vstupy meniča (FWD, REV, reset poruchy), menič v
  režime NPN (sink).
- **Pull-down na strane GIGA:** LED optočlena je zhasnutá, kým sú piny po resete vo
  vysokej impedancii.

### Výstup BRAKE

- **Izolované budenie + výkonový N-MOSFET ≥ 5 A** (logic-level, nízky RDS(on)), spína
  proti 0 V.
- **Nulová dióda** na doske; voliteľne dióda so zenerkou pre rýchlejšie zabrzdenie
  (obyčajná dióda zabrzdenie spomaľuje).
- **Samostatná svorka a istenie;** napájanie cievky z povoľovacej dráhy bezpečnostného
  relé.

### 0–10 V

Reťazec z časti 3: delič → izolačný zosilňovač → op-amp napájaný z 24 V →
svorka 0–10 V a 24V0 pre menič, prepojka 0–5 / 0–10 V.

### Mechanika (zmerané v CAD súboroch Arduina 14. 10. 2026)

Zdroj: Altium dosky (`PCB.PcbDoc`) a STEP modely GIGA R1 (ABX00063) a Display Shieldu
(ASX00039), načítané importom Altium v KiCade. Všetky čísla sú v
[`hardware/Grbl_Giga_Shield/scripts/mech.py`](hardware/Grbl_Giga_Shield/scripts/mech.py),
simulované rozloženie v
[`hardware/Grbl_Giga_Shield/docs/layout_sim.svg`](hardware/Grbl_Giga_Shield/docs/layout_sim.svg).

- **Zostava spredu dozadu:** displej → GIGA → shield. Display Shield (106 × 80 mm, rohy
  R6,5, otvory M3 5 mm od okrajov) je zo spodnej strany GIGA na jej konektoroch J5/J6
  (nie HD). Shield ide zhora na konektory Mega (vrátane 2×18).
- **Poloha GIGA na displeji** (pohľad spredu): USB okraj 0,16 mm od pravého okraja
  displeja, horný okraj GIGA (strana JDIGITAL) 10,73 mm pod horným okrajom displeja, dolný
  15,93 mm nad dolným.
- **Shield:** výška 80 mm ako displej, pravý okraj zarovno s displejom (USB-C, USB-A s
  kľúčom), presah doľava. Koncept: 210 × 80 mm (104 mm vľavo od displeja).
- **Výšky (STEP):** kolíky displej → GIGA siahajú 17,9 mm nad zadnú stranu displeja, takže
  GIGA je asi 12,6 mm od displeja (v dokumentácii nie je — **zmerať**). Na GIGA: USB-A
  7,8 mm (a 3,9 mm pod doskou — preto výrez v displeji), ICSP 2×3 8,6 mm, SWD 7,8 mm,
  konektory Mega 8,5 mm. Shield na štandardných kolíkoch (plast 2,5 mm) leží 11 mm nad
  GIGA — nad ICSP ostane 2,4 mm, nad USB-A 3,2 mm: tam na spodnej strane shieldu žiadne
  vývody THT (alebo skrátené).
- **Stĺpiky:** M3 v rohoch displeja, displej ↔ shield ≈ 25 mm. Všetky tri dosky tak
  držia spolu aj mimo konektorov.
- **Kryté tlačidlá:** RESET a BOOT na GIGA sú pod shieldom → tlačidlo RESET na shielde
  (pin RESET konektora JANALOG).
- **Konektory:** odnímateľné svorkovnice Phoenix 3,81 / 5,08 mm s popisom; pri výstupoch
  +24 V a 0 V skupiny. Výstupy nad GIGA majú svorky s káblom zhora (vertikálne), aby
  24 V nemuselo krížiť logické rady konektorov GIGA.

### Rozšírenie cez konektor kamery J3 na displeji

Na displeji je pri hornom okraji (v strede šírky) konektor kamery **J3** — dutinková
lišta Samtec HLE 2×10, 3,7 mm vysoká, otvorená k zadnej strane. Bez kamery sú jeho
piny voľné: **14 GPIO D54–D67** (PI5, PH8, PA6, PJ9, PI7, PI6, PI4, PH14, PG11, PH11, PH10,
PH9, PA1, PD4), +3,3 V a GND. Firmvér ani displej ich nepoužívajú.

- **Prepojenie:** 2×10 kolíková lišta s dlhými kolíkmi zo shieldu (≈ 24 mm pod shieldom),
  prechádza 5 mm od horného okraja GIGA. Kontakty sú 3,2 a 5,8 mm od horného okraja
  displeja, stĺpce x 41,6–64,4 mm.
- **Piny 17/18 sú SCL1/SDA1 (I2C4 = Wire1)** — zbernica dotyku a RGB diódy displeja.
  Na shielde **nepripájať**.
- **Použitie:** ručné koliesko (MPG) — viď nasledujúcu časť. Voľné ostávajú D64–D67.

### Ručné koliesko (MPG) — doplnené 14. 10. 2026

**Firmvér je hotový** (`src/mpg.c`, README „Handwheel pendant“): koliesko A/B počíta
TIM8 v hardvéri (D54 = PI5 = TIM8_CH1, D59 = PI6 = TIM8_CH2), každých 20 ms sa otočené
zárezy zmenia na krátky jog po zvolenej osi rýchlosťou kolieska, os sa zastaví do
~100 ms po ňom, prepnutie osi alebo OFF zastaví hneď, softvérové limity zastavia na
limite. `$96` 0 vypnuté / 1 zapnuté / 2 opačný smer, `$97` krok ×1 (0,001 mm),
`$98` najvyšší posuv (3000 mm/min). LED svieti = koliesko hýbe osou, bliká = os
zvolená, ale stroj teraz nemôže, nesvieti = OFF.

**Pendant EHDW-BG6L-IM** (zo stroja ASA-10-326 Dispenser, schéma P009): napájanie
24 V, všetky signály typu NPN proti 0 V — prepínače spínajú vodiče na COM = 0 V,
koliesko PA/PB išlo na rýchly čítač PLC (X000/X001), LED− na výstup PLC (Y215).
Kábel MPG-C01, konektor DB15 (vidlica na kábli pendantu):

| DB15 | Vodič | Signál | Na shielde | Pin GIGA |
|---|---|---|---|---|
| 11 | BN (+ WHBU) | +24 V (a LED+) | +24 V poľa (cez poistku) | — |
| 10, 12 | WHYE, RD | 0 V, COM prepínačov | 24V0 | — |
| 8 | YE | PA (koliesko A) | rýchly izolovaný vstup | D54 |
| 9 | PK | PB (koliesko B) | rýchly izolovaný vstup | D59 |
| 1 | GN | os X | izolovaný vstup | D55 |
| 2 | BU | os Y | izolovaný vstup | D56 |
| 3 | VT | os Z | izolovaný vstup | D57 |
| 4 | GY | os U (4) = A | izolovaný vstup | D58 |
| 13, 14 | WHGN, GNBK | os 5, 6 | nezapájať (4 osi) | — |
| 5 | GYBN | ×1 | izolovaný vstup | D60 |
| 6 | WHRD | ×10 | izolovaný vstup | D61 |
| 7 | RDBK | ×100 | izolovaný vstup | D62 |
| 15 | GYBK | LED− | izolovaný výstup NPN (~10 mA) | D63 |
| plášť | GNYE | PE | tienenie na PE | — |

**Požiadavky na shield (rozloženie zatiaľ nerobiť):**

- **Konektor DB15 F s rovnakým zapojením** ako na pôvodnom stroji, aby sa pendant
  zapojil priamo svojím káblom. Plášť konektora na PE.
- **Napájanie pendantu z poľných 24 V** cez vlastnú vratnú poistku (pendant má regulátor
  24 → 5 V, odber desiatky mA).
- **Prepínače (7 vstupov):** rovnaké izolované vstupy NPN ako IN1–IN20 (optočlen,
  4,7 kΩ, COM na +24 V), strana GIGA pull-up a RC filter.
- **Koliesko PA/PB (2 vstupy):** rýchlejšie optočleny (napr. TLP2361, 6N137) alebo bežné
  s menším odporom — pri 100 zárezoch a rýchlom točení je to do ~2 kHz, filter TIM8
  ~4 µs. Výstupy kolieska môžu byť otvorený kolektor alebo push-pull 24 V — optočlen
  s LED proti +24 V zvládne oba.
- **LED (1 výstup):** izolovaný výstup NPN ako OUT1–OUT12, LED+ je v pendante na +24 V.
- **Prepojenie s GIGA:** cez J3 displeja (dlhé kolíky, viď vyššie), 10 signálov + GND
  logiky. Optočleny sú na shielde, k J3 idú už logické signály 3,3 V.

### Obmedzenia pre tabuľku pinov (zapracované v [PINOUT.md](PINOUT.md))

- **Nepoužiť:** SDA1/SCL1 pri AREF (Wire1 — zbernica dotyku a RGB diódy displeja),
  D70/D71 (dotyk displeja), D74 (podsvietenie), D68–D75 (displej), A8–A11 (len
  analógové). PA15 (napájanie USB-A) nie je vyvedený. D54–D67 (kamera) sú voľné cez J3
  displeja — viď vyššie.
- **Krokové piny** na jednom GPIO porte (port J: D25–D37).
- **D7 (PB4)** nie ako výstup (pull-up od JTAG po resete).
- **STOP** na pine s linkou EXTI, ktorú nič iné nepoužíva (D49 = EXTI4).
- **DRV PWR** na D53 (PG7) — pokračovanie bloku vstupov za IN20, číta ho sken.

---

## Rozhodnutia (12. 10. 2026)

| Téma | Rozhodnutie |
|---|---|
| Osi | 4 osi XYZA, os B nie |
| Vstupy a výstupy | univerzálne kanály, funkcie z nastavení; COLLET OK, TOOL BTN, TOOL SETTER zatiaľ nie |
| Tabuľka pinov | bloky bez krížení (návrh v2) |
| Koncáky | NC aj NO nastaviteľné; jeden alebo dva vstupy na os nastaviteľné |
| Brzda Z | elektromagnetická 24 V, vlastný MOSFET do 5 A, pevný výstup; časy v nastaveniach |
| Klieština | pneumatický ventil 24 V |
| Menič | relé „otáčky dosiahnuté“, relé poruchy, vstup na reset poruchy |
| Čísla nastavení | `$60`–`$93`; staré `$5`, `$6`, `$44`, `$45`, `$47` sa rušia (nastavovanie vlastnou aplikáciou, nie senderom) |
| Centrálne STOP | nie viazané na jedno relé — NC/NO nastaviteľné |
| 0–10 V | DAC a op-amp s galvanickým oddelením |
| Staré nastavenia | prevod nerobiť, pred aktualizáciou zálohovať `$$` a `$#` |
| USB kľúč | chyba v súbore → alarm; kontrola `$C` pred prvým spustením, opakované spustenie bez kontroly |
| Drivery | Nanotec C5-01 alebo Delta ASDA-A2 |
| Snímače | všetky NPN |
| Napájanie shieldu | jeden 24 V zdroj, izolovaný DC/DC pre logiku |
| Charge pump, rozširujúci konektor | nie |
| Tabuľka pinov v2 | schválená (13. 10. 2026) |
| Časy brzdy, dobeh vretena | univerzálne nastaviteľné v ms (`$48`–`$51`) |
| `M6` pri behu z kľúča | pauza na ručnú výmenu nástroja (`$58`) |
| Napájanie driverov (14. 10. 2026) | vlastný vstup DRV PWR na D53 s optočlenom; strata → `ALARM:22`, `$H`/`$X` až po návrate (`$95`); sledovanie zapína `$94` |

## Otvorené otázky

Žiadne. Rozhodnuté 13. 10. 2026: Nanotec C5 zatiaľ bez NanoJ (bez alarmu a resetu
drivera); firmvér pripraví obe možnosti brzdy (výstup BRAKE aj brzda ovládaná
driverom) — voľba nastavením `$59`.
