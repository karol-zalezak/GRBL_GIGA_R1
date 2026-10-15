# Grbl pre Arduino GIGA R1 WiFi

[English](README.md) · **Slovensky** · [Wiki](https://github.com/karol-zalezak/GRBL_GIGA_R1/wiki)

<p align="center">
  <img src="docs/photo_display.jpg" alt="GIGA Display Shield so stránkou DRO" width="49%">
  <img src="docs/photo_board.jpg" alt="GIGA R1 na zadnej strane Display Shieldu" width="49%">
</p>

Port [Grbl 1.1h](https://github.com/gnea/grbl) na **Arduino GIGA R1 WiFi**
(STM32H747, dve jadrá). Grbl beží na **Cortex-M7 s 480 MHz**; **GIGA Display
Shield** (800×480) obsluhuje **koprocesor Cortex-M4** ako plnohodnotný displej
stavu stroja — DRO, stav, posuvy, korekcie, všetky vstupy a výstupy — a ako
**dotykový panel**: pozastavenie, štart, reset, odblokovanie, ručný posuv,
homing, nulovanie, korekcie, chladenie, výstupy AUX, klieština, impulzy resetu
a všetky nastavenia `$`. RGB dióda shieldu ukazuje stav stroja.

Je to súrodenec autorových portov pre Arduino UNO R4 WiFi a UNO Q: jadro Grbl
nad hardvérovou vrstvou je ten istý kód, tu rozšírený na štyri osi.

Čo tento port pridáva ku Grbl 1.1h:

* **Štyri osi, X Y Z A.** Motor A je podľa jedného nastavenia samostatná
  rotačná os, samostatná lineárna os alebo **druhý motor osi X alebo Y**
  s automatickým vyrovnaním portálu pri homingu (`$42`).
* **Dvadsať vstupov a dvanásť výstupov, všetky univerzálne.** Každá funkcia je
  nastavenie, ktoré určuje jej kanál (`$60`–`$93`): koncové spínače na oboch
  koncoch každej osi (X−, X+, …), alarmy serv (`ALARM:17`), sonda, tlačidlá,
  dvere, relé meniča „otáčky dosiahnuté“ a „porucha“; vreteno FWD/REV,
  chladenie, šesť výstupov AUX (`M62`–`M65`, AUX0 ide s vretenom pre
  odsávanie), klieština, impulzy resetu serv a meniča. Každý vstup NC alebo
  NO, každý výstup sa dá invertovať.
* **Centrálne STOP** z bezpečnostného relé na vlastnom prerušení: po ňom ani
  jeden krok, brzda Z zabrzdí, všetky výstupy vypnuté, `ALARM:18` až do resetu.
* **Stráženie napájania driverov**: GIGA beží ďalej z USB, aj keď drivery
  stratia napájanie; snímací vstup (D53) to Grbl oznámi — kroky skončia,
  brzda zabrzdí, `ALARM:22` a `$H`/`$X` počkajú, kým drivery nie sú späť.
* **Ručné koliesko (MPG)**: koliesko počíta hardvérový časovač, prepínače osi
  a kroku x1/x10/x100, LED; os nasleduje koliesko a zastaví sa s ním.
* **Postupnosť enable driverov a brzdy Z** s nastaviteľnými časmi, výstupom
  BRAKE shieldu alebo samotným driverom.
* **Relé meniča „otáčky dosiahnuté“ a „porucha“**, **pneumatická klieština**
  s blokovaniami, **`M6` ako pauza** na ručnú výmenu nástroja a **oneskorenie
  kroku po zmene smeru** pre drivery ako Nanotec C5.
* **Programy z USB kľúča** v porte USB-A, vybrané a spustené na displeji —
  stroj beží bez PC. Súbor sa pred prvým spustením skontroluje (`$C`); STOP
  zachová polohu. FAT32 alebo exFAT, MBR alebo GPT; súbory idú na kľúč z PC
  cez USB-C (`tools/usbfiles.py`, aj synchronizácia priečinka) a GIGA vie kľúč
  naformátovať.
* **Otáčky vretena** ako PWM a súčasne ako analógová úroveň 0–3,3 V z DAC pre
  menič.
* **CoreXY ako nastavenie** (`$40`, `$41`), pričom F znamená rýchlosť po
  dráhe ako na karteziánskom stroji; overené impulz po impulze na oboch
  remeňových motoroch.
* **Displej** beží celý na M4: Grbl 25-krát za sekundu zapíše snímku svojho
  stavu do zdieľanej SRAM a na panel nikdy nečaká.
* **Dotykové tlačidlá** na šiestich stránkach (DRO, JOG, OVR, I/O, `$$`, FILE);
  `$$` vypíše každé nastavenie s vysvetlením a dá sa v ňom upraviť. Čokoľvek,
  čo hýbe strojom alebo mení offsety, treba podržať, plynulý posuv zastane,
  keď prst pustí, a príkaz z dotyku nikdy nerozdelí riadok, ktorý práve
  posiela PC.
* **Úvodná obrazovka** s logom Grbl, kým štartujú obe jadrá a USB kľúč.

**Stav (október 2026).** Firmvér je hotový a overený na GIGA R1 WiFi
s Display Shieldom na stole, bez stroja: vstupy, koncové spínače, STOP
a napájanie driverov cez testovacie háčiky, ktoré ovládajú samotné piny
(pozri [Overenie](#overenie)). Shield periférií — 24 V vstupy a výstupy cez
optočleny, 0–10 V pre menič, konektor ručného kolieska — je vo fáze konceptu
v [`hardware/`](hardware/Grbl_Giga_Shield/). Plán vývoja [PLAN.md](PLAN.md)
a tabuľka pinov [PINOUT.md](PINOUT.md) sú po slovensky;
[wiki](https://github.com/karol-zalezak/GRBL_GIGA_R1/wiki) má návody krok za
krokom po anglicky aj po slovensky.

---

## Rýchly štart

Priečinok sketchu sa musí volať `Grbl_Giga_R1`, rovnako ako súbor `.ino`
v ňom (Arduino to vyžaduje), kým repozitár sa volá `GRBL_GIGA_R1`:

```bash
git clone https://github.com/karol-zalezak/GRBL_GIGA_R1.git Grbl_Giga_R1
```

(alebo premenuj priečinok rozbaleného ZIP). Potom:

1. Nainštaluj jadro **Arduino Mbed OS GIGA Boards** (`arduino:mbed_giga`)
   cez Boards Manager v Arduino IDE, alebo:

   ```bash
   arduino-cli core install arduino:mbed_giga
   ```

   **Windows:** nahrávanie ide cez DFU bootloader dosky, ktorého USB
   rozhranie potrebuje ovládač WinUSB. Arduino IDE ho nainštaluje spolu
   s jadrom (pýta si práva správcu); ak používaš len `arduino-cli`, spusti raz
   v PowerShelli ako správca:

   ```bash
   pnputil /add-driver "$env:LOCALAPPDATA\Arduino15\packages\arduino\hardware\mbed_giga\4.6.0\drivers\giga.inf" /install
   ```

2. **Nahraj ten istý sketch dvakrát**, raz pre každé jadro, zakaždým
   s **Tools → Flash split = "1.5MB M7 + 0.5MB M4"**:

   | Tools → Target core | Čo sa nahrá do dosky |
   |---|---|
   | Main Core | Grbl (pohyb, USB, nastavenia) |
   | M4 Co-processor | displej stavu |

   Z príkazového riadka `./build.sh COM5` zostaví oba programy a oba nahrá
   (najprv M7). FQBN sú
   `arduino:mbed_giga:giga:target_core=cm7,split=75_25` a
   `arduino:mbed_giga:giga:target_core=cm4,split=75_25`.

   Prvý štart po prvom nahraní dosku raz sám resetuje: STM32H747 je z výroby
   nastavený tak, aby M4 štartovalo samo, a Grbl to vypne (rovnako ako
   knižnica RPC jadra), aby o štarte M4 rozhodovalo M7.

3. Pripoj ľubovoľný Grbl sender (UGS, bCNC, Candle, ioSender, LaserGRBL) na
   COM port dosky. Je to natívne USB, takže na prenosovej rýchlosti
   nezáleží. Doska sa hlási ako `Grbl 1.1h` a vypíše úvodný riadok vždy, keď
   sender otvorí port.

Bez Display Shieldu alebo bez programu pre M4 beží Grbl úplne rovnako; `$I`
a úvodný riadok hlásia `display=0`.

---

## Zapojenie

Tabuľka pinov je tá schválená pre shield periférií
([PINOUT.md](PINOUT.md), v2): každý blok leží na jednom úseku konektorov
v poradí svojich svoriek. Funkcie v zátvorkách sú predvolené — dvadsať
vstupov a dvanásť výstupov sú univerzálne kanály, ktorých funkcie sú
nastavenia (pozri nižšie).

Konektor 2×18 D22–D53:

| Nepárny stĺpec | Funkcia | | Párny stĺpec | Funkcia |
|---|---|---|---|---|
| D23 | rezerva | | D22 | **IN1** (koncák X−) |
| D25 | krok X (PJ0) | | D24 | **IN2** (koncák X+) |
| D27 | smer X (PJ1) | | D26 | **IN3** (koncák Y−) |
| D29 | krok Y (PJ2) | | D28 | **IN4** (koncák Y+) |
| D31 | smer Y (PJ3) | | D30 | **IN5** (koncák Z−) |
| D33 | krok Z (PJ4) | | D32 | **IN6** (koncák Z+) |
| D35 | smer Z (PJ5) | | D34 | **IN7** (koncák A−) |
| D37 | krok A (PJ6) | | D36 | **IN8** (koncák A+) |
| D39 | smer A (PI14) | | D38 | **IN9** (alarm serva X) |
| D41 | enable driverov | | D40 | **IN10** (alarm serva Y) |
| D43 | **IN17** (bezpečnostné dvere) | | D42 | **IN11** (alarm serva Z) |
| D45 | **IN18** (otáčky vretena dosiahnuté) | | D44 | **IN12** (alarm serva A) |
| D47 | **IN19** (porucha vretena) | | D46 | **IN13** (sonda) |
| D49 | **STOP** — bezpečnostné relé, EXTI4 (pevné) | | D48 | **IN14** (tlačidlo reset) |
| D51 | **IN20** (voľný) | | D50 | **IN15** (tlačidlo hold) |
| D53 | **DRV PWR** — napájanie driverov prítomné (pevné) | | D52 | **IN16** (tlačidlo štart) |

Horný konektor a analóg:

| Pin | Funkcia | | Pin | Funkcia |
|---|---|---|---|---|
| D13 | **OUT1** (vreteno FWD) | | D5 | **OUT7** (AUX2) |
| D12 | **OUT2** (vreteno REV) | | D4 | **OUT8** (AUX3) |
| D11 | **OUT3** (chladenie flood) | | D3 | **OUT9** (klieština) |
| D10 | **OUT4** (chladenie mist) | | D2 | **OUT10** (reset serv) |
| D9 | **OUT5** (AUX0, odsávanie) | | D14 | **OUT11** (reset meniča) |
| D8 | **OUT6** (AUX1) | | D15 | **OUT12** (AUX4) |
| D6 | PWM vretena (TIM4_CH2, 1 kHz, 60000 krokov) | | D16 | **BRAKE** — brzda Z (pevné) |
| A12 (DAC0) | otáčky vretena 0–3,3 V (0–10 V za shieldom) | | D7 | rezerva (len vstup: pull-up JTAG) |

**GIGA je 3,3 V doska a jej piny nie sú pre signály 5 V ani 24 V.** Shield
(PINOUT.md) dáva každý vstup za optočlen a každý výstup za optočlen
a tranzistor, na izolovanej 24 V strane. Na holej doske majú vstupy interné
pull-upy a sú **aktívne v nule** — spínač alebo výstup s otvoreným kolektorom
na GND — a výstupy dávajú 3,3 V.

Piny D68–D75 (konektor displeja), SDA1/SCL1 vedľa AREF (zbernica dotyku a LED
displeja), D0/D1 a D17–D21 zostávajú voľné. D54–D67 (piny kamery) sú tiež
voľné a na shield sa dostanú cez konektor kamery J3 na Display Shielde
(PLAN.md, časť 5).

Všetky štyri krokové piny sú na porte J, takže krok je jeden 32-bitový
zápis. Vstupy číta časovač a výstupy sa pri štarte prevedú na bity portov,
takže každý pin sa dá presunúť v `src/cpu_map.h`.

---

## Štvrtá os (`$42`, `$43`)

| `$42` | A je | Jednotky | `G20` mení A | Slová A |
|---|---|---|---|---|
| 0 (predvolené) | samostatná **rotačná** os | stupne | nie | povolené |
| 1 | samostatná **lineárna** os | mm | áno | povolené |
| 2 | **druhý motor X** | — | — | `error:20` |
| 3 | **druhý motor Y** | — | — | `error:20` |

Ako samostatná os má A vlastné `$103` (kroky/° alebo kroky/mm), `$113`
(max. rýchlosť), `$123` (zrýchlenie) a `$133` (dráha), zúčastňuje sa oblúkov
ako lineárna os (pohybuje sa úmerne pozdĺž `G2`/`G3`, ako os skrutkovice) a do
rýchlosti posuvu sa započítava ako ostatné osi (stupne, ako keby to boli mm).
`$43=1` ju zreferuje ako posledný cyklus `$H` (a `$HA` len ju); s `$43=0` sa
nereferuje a softvérové limity ju ignorujú.

Ako **podriadený (slave) motor** driver A opakuje každý krokový impulz svojej
hlavnej osi. Smer nasleduje hlavnú os, invertovaný vlastným bitom v `$3`
(bit 3) pre zrkadlovo namontovaný motor. Nastavenia A sa potom nepoužívajú
a A hlási 0. Pri homingu hlavnej osi sa oba motory **zastavia nezávisle**:
hlavný na svojom koncovom spínači, podriadený na **spínači A− / A+** (`$66`,
`$67`). Tým sa portál pri každom homingu vyrovná. Ak jeden spínač zopol
a druhý nie do 5 % dráhy druhej osi (2,5–25 mm), homing skončí `ALARM:10`,
aby sa rám neskrútil — kontrola dvojitej osi z Grbl 1.1h, tu ako nastavenie
za behu namiesto voľby pri kompilácii.

Zmena `$42` sa prijme len v pokoji alebo v alarme. Podriadený motor sa nedá
kombinovať s CoreXY (`$40=1`); každé z nastavení odmietne to druhé
s `error:5`.

---

## CoreXY (`$40`, `$41`)

`$40=1` prepne stroj na remeňovú kinematiku CoreXY za behu — bez
prekompilovania. Motor A z nákresov CoreXY je **driver X** (D25/D27), motor B
**driver Y** (D29/D31):

    A = X + Y        X = (A + B) / 2
    B = X - Y        Y = (A - B) / 2

takže pohyb v X otáča oba motory rovnakým smerom, pohyb v Y opačnými smermi
a uhlopriečka 45° otáča len jeden z nich, dvakrát ďalej. Z a A zostávajú
obyčajné osi. Ako správne nastaviť dva remeňové motory, s `G91 G0 X10`
a `G91 G0 Y10`:

| Čo sa deje | Oprava |
|---|---|
| príkaz X ide po Y (a Y po X) | invertuj jeden remeňový motor (`$3` bit 0 alebo 1) |
| X doprava, Y dozadu | vymeň dva motory (výstupy driverov X a Y) |
| oba dozadu | invertuj oba remeňové motory (`$3` bity 0 a 1) |

Oba remeňové motory potrebujú rovnaké `$100`/`$101`.

* **Rýchlosť posuvu.** F je rýchlosť nástroja po naprogramovanej dráhe, ako
  ju definuje RS274/NGC a ako na karteziánskom stroji. (CoreXY v pôvodnom
  Grbl plánuje v priestore motorov, kde je každá dráha XY presne √2-krát
  dlhšia, takže jeho `G1 X60 F3600` trvá 1,41 s namiesto 1,0 s.)
* **`$41` — čo obmedzujú `$110`/`$120`.** `$41=0` (predvolené): každý remeňový
  motor, takže na uhlopriečke, kde všetku prácu robí jeden motor, ide hlava
  rýchlosťou 1/√2 z `$110`. `$41=1`: osi X a Y hlavy, presne ako na
  karteziánskom stroji; motor sa potom na uhlopriečke otáča až 2 × `$110`,
  takže `$110` treba dimenzovať na to.
* **Homing** funguje na CoreXY ako na karteziánskom stroji, ibaže X a Y sa
  musia referovať v samostatných cykloch (rám CoreXY nevie dohnať obe osi
  naraz k ich spínačom). Predvolené cykly v `config.h` sú Z, potom X, potom Y;
  ak ich zmeníš tak, aby sa X a Y referovali spolu, `$40=1` sa odmietne.
* Polohy v `?`, `$#`, výsledok sondy a displej sú karteziánska poloha hlavy;
  `$I` hlási surový počet impulzov každého drivera (`motor=`), čím testy
  kontrolujú remene.

---

## Vstupy a výstupy

Shield periférií má **dvadsať vstupov IN1–IN20** a **dvanásť výstupov
OUT1–OUT12**. Žiaden nemá pevnú úlohu: každá funkcia je nastavenie s číslom
kanála, na ktorom je (0 = nepoužitá), takže zapojenie sa mení v `$$`, nie
v zdrojovom kóde. Pevné sú len krok/smer/enable, centrálne STOP (D49), vstup
napájania driverov (D53), brzda Z (D16), PWM (D6) a DAC (A12).

| Nastavenie | Funkcia vstupu | Predvolene | `Pn:` |
|---|---|---|---|
| `$60`–`$67` | koncové spínače X−, X+, Y−, Y+, Z−, Z+, A−, A+ | IN1–IN8 | `X` `Y` `Z` `A` |
| `$68`–`$71` | alarmy serv X, Y, Z, A | IN9–IN12 | `M` |
| `$72` | sonda (`G38.x`) | IN13 | `P` |
| `$73`–`$76` | tlačidlá reset, feed hold, cycle start, bezpečnostné dvere | IN14–IN17 | `R` `H` `S` `D` |
| `$77` | otáčky vretena dosiahnuté (relé meniča) | IN18 | |
| `$78` | porucha vretena (relé meniča) | IN19 | |
| `$79` | **maska NC**: bit n−1 = INn, bit 20 = STOP, bit 21 = DRV PWR | 0 (všetky NO) | |

| Nastavenie | Funkcia výstupu | Predvolene |
|---|---|---|
| `$80`, `$81` | vreteno **FWD** (`M3`), **REV** (`M4`) — nikdy obe naraz | OUT1, OUT2 |
| `$82`, `$83` | chladenie flood `M8`, mist `M7` | OUT3, OUT4 |
| `$84`–`$89` | AUX0–AUX5 (`M62`–`M65 P0`–`P5`) | OUT5–OUT8, OUT12, žiaden |
| `$90` | otvorenie klieštiny | OUT9 |
| `$91` | reset alarmu serv (impulz) | OUT10 |
| `$92` | reset poruchy meniča (impulz) | OUT11 |
| `$93` | **invertované výstupy**: bit n−1 = OUTn | 0 |

Vstup je **aktívny, keď jeho optočlen vedie** (stiahne pin, ktorý má pull-up,
na GND — na holej doske urobí to isté spínač na GND). Bit v `$79` spraví
kanál aktívnym, keď je optočlen *vypnutý*, pre rozpínacie kontakty, pri
ktorých potom vypne aj prerušený vodič. Dva koncové spínače jednej osi môžu
zdieľať kanál; akékoľvek iné dvojité použitie a dve funkcie na jednom
výstupe sa odmietnu s `error:42`. Voľný vstup sa aj tak zobrazuje na displeji
a v `$I`.

**Koncové spínače, jeden alebo dva na os.** X− a X+ na dvoch kanáloch: Grbl
vie, ktorý koniec bol zasiahnutý, a povie to (`ALARM:1` a za ním
`[MSG:Limit X-]`). X+ na tom istom kanáli ako X−: dva spínače v sérii ako
v pôvodnom Grbl (`[MSG:Limit X]`). X+ = 0: os má spínač len na mínusovom
konci. Hardvérový limit vypne, keď spínač **zopne**. Kým je jeden zopnutý —
po `ALARM:1`, resete a `$X` — os sa môže od neho vzďaľovať, a len
vzďaľovať: posuv do neho sa odmietne s `error:41`, akýkoľvek iný pohyb do
neho znova vyvolá `ALARM:1`. Os sa tak dá zo spínača vyviesť posuvom so
zapnutými hardvérovými limitmi. (S dvoma spínačmi v sérii Grbl nevie povedať
stranu a os sa môže pohnúť oboma smermi, kým je spínač zopnutý, ako
v pôvodnom Grbl.)

**Homing** používa spínač na tom konci, kam os posiela `$23` (bit nastavený
= mínus). `$H` sa odmietne s `error:45`, ak ten koniec nemá spínač. S dvoma
vstupmi na osi zopnutie spínača na **vzdialenom** konci počas homingu ho
zastaví s `ALARM:20` — zlé `$23` alebo prehodené vodiče. Podriadený motor A sa
referuje na spínačoch A− / A+ a vyrovná portál ako vyššie.

**Alarmy serv** (`$68`–`$71`). Driver v poruche okamžite zastaví stroj
v akomkoľvek stave okrem alarmu — aj v pokoji, lebo servo, ktoré spadne do
poruchy pri držaní polohy, ju stratilo rovnako — s `ALARM:17` („motor fault“,
číslo, ktoré používa grblHAL). Ako hardvérový limit potrebuje reset a ďalší
reset (alebo odblokovanie) dá driverom impulz `$55` ms na výstupe **reset
serv**. Vstupy sa odrušujú 2 ms, koncové spínače 0,3 ms, tlačidlá 10 ms.

**Výstupy.** `M2`/`M30` a reset vypnú všetky výstupy. `$46` určuje výstupy
AUX, ktoré sa zapnú vždy, keď beží vreteno (predvolene AUX0, odsávanie).
`$93` invertuje výstupy na pine; displej a M-kódy ďalej ukazujú logický stav.
Zmena priradenia funkciu najprv vypne.

### Centrálne STOP (`$57`, D49)

Bezpečnostnú funkciu robí bezpečnostné relé: odpojí napájanie driverov,
meniča, brzdy a ventilov, nezávisle od firmvéru. Jeho signálny kontakt na
**D49** spôsobí, že Grbl s ním hneď súhlasí — drivery môžu ešte chvíľu bežať
z kondenzátorov a nesmú dostať ďalší krok. D49 (PE4) má vlastné prerušenie
EXTI4 s prioritou NVIC 0, nad krokovaním, skenovaním vstupov a celým mbed;
sleduje ho aj skenovanie 10 kHz (STOP prítomné pri zapnutí alebo zmeškaná
hrana). Prerušenie:

1. zastaví krokové časovače a dá krokové piny do pokoja,
2. zabrzdí brzdu Z a vypne drivery,
3. vypne všetky výstupy — vreteno FWD/REV, PWM, DAC, chladenie, AUX,
   klieštinu — zápismi do registrov, bez volaní mbed,
4. potom odovzdá riadenie po grblovsky: `mc_reset()` a **`ALARM:18`**,
   `[MSG:E-stop]`, `Pn:E` v stave.

Rutina trvá asi **11 µs** po posledný výstup; prerušenie krokovania
kontroluje západku s maskovanými prerušeniami tesne pred zápisom kroku, takže
po vypnutí nepríde žiaden impulz. Grbl zostane v alarme: `$X` a `$H` sa
odmietajú s `error:40`, kým je STOP aktívne, a aj po jeho uvoľnení až do
resetu — reset potom nájde STOP uvoľnené (na 100 ms) a západku ukončí. Reset
s ešte aktívnym STOP ponechá `ALARM:18`. `$57=0` (predvolené, kým nie je relé
zapojené) vstup vypne; bit 20 v `$79` určuje, či je kontakt pri STOP zopnutý
alebo rozopnutý. `$I` ukazuje `estop=<stav>/<počet vypnutí>/<čas posledného
vypnutia v 0,1 µs>/<1 prerušenie, 2 skenovanie>`. Displej prekryje stránku
červeným pruhom **E-STOP** a RGB dióda bliká načerveno.

### Napájanie driverov (`$94`, `$95`, D53)

GIGA beží ďalej, kým ju napája USB, aj keď drivery stratili napájanie —
a jej počítadlo krokov potom tvrdí polohu, ktorú motory už nedržia. **D53**
sníma napájanie driverov cez vlastný optočlen (nízka úroveň = napájanie je;
snímací obvod shieldu v PLAN.md, časť 5) a zosúladí Grbl s drivermi:

* **napájanie preč na 2 ms** (`DRVPWR_LOSS_SCANS`): brzda Z hneď zabrzdí
  a enable spadne (drivery už os neudržia; brzda uvoľnená výstupom BRAKE by
  nechala Z spadnúť), potom `mc_reset()` a **`ALARM:22`** — v pohybe,
  v pokoji, pri homingu (nie `ALARM:6`), a hlási sa aj vtedy, keď je Grbl už
  v inom alarme, lebo poloha zachovaná cez ten alarm je teraz preč;
* **kým je vypnuté** a kým nie je späť aspoň **`$95` ms** (čas štartu
  driverov, predvolene 1000), `$H` a `$X` sa odmietajú s **`error:50`**;
  potom Grbl povie `[MSG:Driver power on]` a HOME alebo UNLOCK pokračuje.
  Čokoľvek, čo opustilo alarm, kým je napájanie vypnuté, skončí hneď späť
  v ňom.
* **pri zapnutí** bez napájania driverov Grbl štartuje v `ALARM:22`.

Nie je to kritický alarm: reset netreba. `$94=0` (predvolené, kým shield
napájanie nesníma) stráženie vypne — holá doska číta na D53 „bez napájania“.
Bit 21 v `$79` invertuje vstup, pre kontakt „ready“ drivera, ktorý je pri
pripravenosti rozopnutý. `$I` ukazuje `drv=<0 nestráži sa, 1 vypnuté, 2 späť,
ale $95 ešte neuplynulo, 3 zapnuté>/<výpadky od zapnutia>`; displej ukazuje
červené / jantárové / zelené **PWR** vedľa DRIVES na stránke I/O a v riadku
správ, čo treba urobiť.

### Ručné koliesko (`$96`–`$98`, D54–D63)

Pendant s ručným kolieskom, napríklad EHDW-BG6L: kvadratúrne koliesko
(100 dielikov na otáčku), prepínač osi OFF / X / Y / Z / 4 / 5 / 6, prepínač
kroku x1 / x10 / x100 — každá poloha je kontakt na 0 V pendantu — a LED.
Pripája sa na piny kamery, ktoré Display Shield prepúšťa cez svoj konektor J3
(PLAN.md, časť 5):

| Signál | Pin | | Signál | Pin |
|---|---|---|---|---|
| koliesko A | D54 (PI5, TIM8_CH1) | | krok x1 | D60 |
| koliesko B | D59 (PI6, TIM8_CH2) | | krok x10 | D61 |
| os X, Y, Z | D55, D56, D57 | | krok x100 | D62 |
| os 4 (= A) | D58 | | LED (výstup) | D63 |

**TIM8 počíta koliesko** hardvérovo (režim enkodéra, každá hrana oboch
vstupov cez filter ~4 µs), takže sa naň nemíňa žiadne prerušenie. Vstupy
prepínačov sú aktívne v nule s pull-upmi (stiahne ich kontakt pendantu alebo
optočlen shieldu) a odrušené 20 ms. Polohy 5 a 6 a OFF nevyberajú nič.

Každých 20 ms, v tom mieste hlavnej slučky, kde bežia príkazy dotykovej
obrazovky (žiaden napoly prijatý riadok z PC), sa otočené dieliky zmenia na
jeden posuv `$J=G21G91<os><dieliky × krok>F<posuv>` — s posuvom, ktorý ho
prejde za tých 20 ms, takže **os nasleduje rýchlosť kolieska**. Blok obsahuje
najviac 20 ms pohybu pri `$98`, vo fronte sú najviac 3 bloky a čakajú najviac
2 periódy: točenie rýchlejšie, než os stihne, sa **zahodí, nehromadí sa**,
a os zastaví do zhruba 100 ms po koliesku aj pri plnej rýchlosti. Koliesko
hýbe strojom len v stave Idle (nič naplánované) alebo počas vlastného
posuvu — nie počas programu, pozastavenia, alarmu, režimu kontroly ani počas
posuvu z PC alebo z dotykovej obrazovky. **Iná os alebo OFF jeho posuv hneď
zruší.** Na softvérovom limite os zastane na limite, namiesto aby sa posuv
odmietol. Pauza hlavnej slučky dlhšia ako 100 ms (pozastavenie, prestávka
`G4`) zabudne, čo sa medzitým otočilo.

| Nastavenie | Predvolene | |
|---|---|---|
| `$96` | 0 | 0 vypnuté, 1 zapnuté, 2 zapnuté s opačným smerom |
| `$97` | 0.001 | dráha jedného dielika pri x1 (mm, stupne pri rotačnej A); x10 a x100 sú 10× a 100× |
| `$98` | 3000 | najvyšší posuv, ktorým koliesko hýbe osou (mm/min), nikdy nad `$11x` osi |

**LED** svieti, kým koliesko hýbe vybranou osou, bliká, keď je os vybraná,
ale stroj sa nemôže posúvať, a nesvieti pri OFF. Stránka JOG na displeji
ukazuje os a krok (alebo „blocked“) vľavo od Y+ a `$I` má `mpg=<stav
0 vypnuté, 1 bez osi, 2 pripravené, 3 blokované>/<os 0, 1-4>/<krok>/<počítadlo
TIM8>/<vstupy prepínačov>/<LED>`.

### Enable driverov a brzda Z (`$1`, `$48`–`$50`, `$59`)

Brzda je uvoľnená, kým je jej cievka pod prúdom: výstup BRAKE **D16** (5 A
MOSFET na shielde). S `$59=1` ju Grbl riadi v postupnosti s enable driverov:

    drivery zap. --$48--> brzda uvoľnená --$49--> prvý krok
    koniec pohybu --$1--> brzda zabrzdí --$50--> drivery vyp.

Nič neblokuje: prerušenie krokovania štartuje hneď, ale nevydá krok, kým
brzda nemala svoj čas, a vypínanie sa časuje z hlavnej slučky (pôvodné Grbl
čakalo `$1` ms vo vnútri prerušenia krokovania). Alarm hneď zabrzdí a drivery
vypne o `$50` neskôr; STOP vypne oboje naraz. S `$1=255` zostanú drivery
zapnuté a brzda uvoľnená, takže časy platia len po alarme alebo resete.
S `$59=0` riadi brzdu driver sám (Delta BRKR, výstup 1 Nanotecu C5): BRAKE
zostane vypnutý, `$48`+`$49` ešte uplynú pred prvým krokom a drivery sa vypnú
bez čakania. Predvolene 200/100/200 ms, skôr dlhšie, kým sa brzda nezmeria.
`$I` ukazuje `en=` a `brake=`.

### Vreteno: otáčky dosiahnuté, porucha, klieština, M6

* **Otáčky dosiahnuté** (`$77`, `$52`). Po `M3`/`M4` a po každej zmene S Grbl
  čaká — ako `G4` — na relé meniča „frekvencia dosiahnutá“, najviac `$52` ms,
  potom `ALARM:14` a program sa zastaví (planner a parser sa resetujú, poloha
  zostane). Po zmene otáčok bežiaceho vretena sa relé 250 ms ignoruje, kým
  menič cez svoj vstupný filter nezaregistruje novú žiadanú hodnotu. Počas
  rezania sa strata relé rieši podľa `$53`: 0 ignorovať, 1 feed hold
  (pokračovať tlačidlom START, keď je späť), 2 alarm po riadenom zastavení.
  `$52=0` (predvolené): žiadne čakanie, namiesto neho pevné `$54`. Nie
  v laserovom režime.
* **Porucha meniča** (`$78`). FWD/REV hneď spadnú — aby reset meniča nemohol
  vreteno znova roztočiť —, pohyb zastane cez feed hold a keď stroj stojí,
  `ALARM:19` so zachovanou polohou. `$X` a `$H` sa odmietajú s `error:43`, kým
  vstup poruchy zostáva aktívny; reset alebo `$X` dá impulz na výstup **reset
  meniča** (`$92`, `$55` ms).
* **Klieština** (`$90`). Otvorí sa len pri vretene vypnutom aspoň `$51` ms
  (a rozopnutom relé otáčok), so strojom v pokoji alebo v dokončenom
  pozastavení a bez STOP — inak `error:44`. Kým je otvorená, `M3`/`M4` sa
  odmietajú s `error:39` a pozastavenie nepokračuje. Na displeji: COLLET na
  stránke I/O (podržať na otvorenie, ťuknúť na zatvorenie), funguje aj počas
  pauzy M6; z PC `$COLLET=1` / `$COLLET=0`.
* **`M6`** (`$58=1`, predvolené): dokončí pohyb, zastaví vreteno a pozastaví
  ako `M0` s `[MSG:Tool change T<n>, cycle start to resume]`. Nástroj vymeníš
  tlačidlom COLLET, potom START. `$58=0`: odmietne sa ako v pôvodnom Grbl.

### Oneskorenie kroku po zmene smeru (`$56`)

Niektoré drivery potrebujú čas medzi zmenou smeru a ďalším krokom — Nanotec
C5 35 µs. S `$56` > 0 sa prvý krok po zmene smeru zdrží, kým smer nestojí
tak dlho: oneskorovacia fáza časovača krokového impulzu zdvihne krokové piny
neskôr a tá jedna perióda krokovania sa natiahne, aby bolo miesto. Všetky
ostatné kroky sú nezmenené. `$I dir=` ukazuje, koľko krokov sa zdržalo,
a najkratší čas od zmeny smeru po krok od posledného resetu (35,4 µs namerané
s `$56=35`; 0 s `$56=0`).

### Testovacie háčiky

Na testy bez stroja (všetky používa `tests/test_io.py`):

* `$TIN=n,1` stiahne vstupný kanál n (21 = STOP, 22 = DRV PWR) do nuly —
  presne to, čo vodivý optočlen, takže vstup ide svojou normálnou cestou
  (odrušenie, prerušenie STOP) — a `$TIN=n,0` ho uvoľní; `$TIN=n,v,ms` to
  urobí o ms neskôr (Grbl počas pozastavenia alebo homingu nečíta riadky);
  `$TIN=0,0` uvoľní všetky.
* `$TSW=n,X+5` spraví, že kanál n je aktívny, kým je strojová poloha X na
  5 mm alebo vyššie (`X-5`: na alebo nižšie) — virtuálny koncový spínač,
  ktorý zopína a rozopína s pohybom, na homing. `$TSW=0` ich odstráni.
* `$TMPG=os,krok` prepne prepínače kolieska (os 0 = OFF, 1–4 = X Y Z 4; krok
  1, 10, 100, 0 = žiaden) stiahnutím tých pinov; `$TMPG=0,0` ich uvoľní.
  `$TMPW=n[,rýchlosť]` otočí koliesko o n dielikov (záporné: dozadu)
  rýchlosťou toľko dielikov za sekundu (predvolene 100) prepínaním pull-upov
  pinov A a B v kvadratúre zo skenovania vstupov — TIM8 počíta skutočné hrany.

* `$SPLASH[=ms]` znova ukáže úvodnú obrazovku, aspoň na ms (predvolene
  3000) — na snímky obrazovky.

Ani jeden neprežije vypnutie napájania; všetky zámerne prežijú reset.

**Otáčky** vretena sú PWM na D6 a zároveň analógová úroveň na A12: 0 V pri
S0 a 3,3 V pri `$30`. Laserový režim (`$32=1`) moduluje oboje po segmentoch
pohybu, ako pôvodné Grbl moduluje PWM pin.

## Programy z USB kľúča

Stroj beží bez PC: nahraj G-kód na kľúč — **FAT32 alebo exFAT** (aj FAT16),
s tabuľkou oddielov MBR alebo **GPT** — zastrč ho do portu **USB-A** na GIGA,
vyber súbor na stránke **FILE** displeja a podrž START. PC na USB-C zostáva
voliteľné — sender môže sledovať (stavové hlásenia nesú priebeh), pozastaviť
a pokračovať, ale nie je potrebný. Kľúč môže v GIGA zostať natrvalo: súbory
sa naň dostanú z PC cez USB-C (nižšie) a GIGA ho v prípade potreby
naformátuje.

**Ako to beží.** Port USB-A je USB host na **M4**, vo vlastnom vlákne
(`src/m4/usbstick.cpp`), takže Grbl na M7 nikdy nevidí prerušenie USB hosta.
M4 pripojí kľúč vlastným FatFs R0.15 (`src/m4/fatfs/` — vstavaný FatFs
v mbed nepozná ani exFAT, ani GPT, a kľúče prichádzajú s oboma), vypíše
G-kódové súbory koreňového priečinka (`.nc .ngc .gcode .gc .tap .txt .cnc`,
najviac 40, zoradené podľa mena; mená do 47 znakov, ako UTF-8 — displej
ukazuje písmená s diakritikou bez nej) a na požiadanie posiela súbor do 8 kB
kruhového buffera v zdieľanej SRAM4 (asi 510 kB/s z kľúča; program potrebuje
najviac 25 kB/s). Grbl berie riadky z buffera vo svojej hlavnej slučke tak
rýchlo, ako má planner miesto, a spracúva ich tou istou funkciou ako riadky
z PC (`src/job.c`):

1. **Štart** — len v stave Idle s prázdnym plannerom a bez STOP; z displeja
   navyše len potom, čo bolo PC sekundu ticho.
2. **Kontrolný prechod** — súbor, ktorý ešte nebežal (meno, veľkosť
   a dátum), najprv prejde režimom kontroly Grbl (`$C`: spracuje sa, nič sa
   nehýbe), ktorý zároveň spočíta jeho riadky a nájde rozsah X/Y/Z, v ktorom
   sa pohybuje (pracovné súradnice). Chyba ho tam ukončí s číslom riadka;
   stav parsera spred štartu — G91, G92, nástroj — sa vráti. Súbor, ktorý už
   nezmenený bežal, kontrolu preskočí.
3. **Beh** — od začiatku. `M0` a `M1` pozastavia (START pokračuje), `M6` je
   pauza na výmenu nástroja (`$58`), `M2`/`M30` program ukončia.
4. **STOP** (stránka FILE alebo `$FS`) pozastaví stroj, počká, kým stojí,
   a resetuje Grbl so strojom vyhláseným za zastavený: **poloha sa
   zachová**, žiaden `ALARM:3`. Chyba G-kódu počas behu ho zastaví rovnako
   a skončí v `ALARM:21`; reset, alarm alebo centrálne STOP ho ukončia hneď;
   vytiahnutý kľúč ho zastaví ako STOP, a to hneď — nie až po riadkoch už
   načítaných dopredu do buffera, čo by mohli byť sekundy pohybu.

Kým program beží, riadok z PC dostane odpoveď hneď: príkazy `$` len na
čítanie (`$$ $G $# $I $N $F`) a `$FS` fungujú, akýkoľvek iný riadok sa
odmietne s `error:48`. Stavové hlásenie nesie `|SD:<percentá>,<súbor>` (pole
grblHAL pre úlohy z SD karty) a Grbl hlási jednotlivé kroky v riadkoch
`[MSG:USB job ...]`.

**Na displeji** — šiesta záložka, FILE: zoznam s veľkosťou, predchádzajúci /
ďalší a pre vybraný súbor, čo zistil posledný beh (riadky, rozsah, výsledok);
START, podržať 1 s. Kým program beží, stránka ukazuje jeho meno, stav, pruh
priebehu, riadok, uplynulý a zostávajúci čas a STOP (podržať 0,5 s); HOLD
a START v pravom stĺpci pozastavia a pokračujú. Horná lišta ukazuje kľúč na
každej stránke (pod USB káblom): `No USB stick`, `Stick mounting`,
`Stick FAT32` / `Stick exFAT` (zelené), `Stick NTFS` / `Stick error`
(červené), `Formatting...` a počas behu programu `45%  L1234`; riadok správ
hovorí, ako skončil posledný program.

**Z PC:**

| Príkaz | |
|---|---|
| `$F` | stav kľúča, súborový systém, veľkosť a voľné miesto; jeho súbory (`[FILE:name\|SIZE:n\|DATE:2026-10-13T02:33:22]`); úloha (`[JOB:state\|name\|LINE:\|POS:\|ERR:\|ALARM:\|CHECKED:\|MS:\|RANGE:]`) |
| `$F=meno` | spustí súbor (ako je vypísaný; na veľkosti písmen nezáleží) |
| `$FS` | STOP |
| `$FR` | znova načíta priečinok |
| `$FT=meno` | test čítania: prečíta súbor cez buffer do konca, vypíše bajty, riadky, CRC-32 a rýchlosť |
| `$FW=meno`, `$FW+<hex>`…, `$FWE=YYYYMMDDhhmmss` | zapíše súbor na kľúč s daným dátumom (`$FWA` zápis zruší) |
| `$FD=meno` | zmaže súbor |
| `$FF=ERASE` | naformátuje kľúč: FAT32 do 32 GB, nad tým exFAT (`$FF=EXFAT`: exFAT bez ohľadu na veľkosť) |
| `$FX=lba` | diagnostika: jeden sektor kľúča v hex (`[SEC:…]`), pripojeného aj nepripojeného |

Čo nasleduje po `=` v príkaze `$F`, sa berie tak, ako prišlo — veľké a malé
písmená, medzery, zátvorky —, takže `$F=Front panel (v2).nc` funguje. Zápis,
mazanie a formátovanie len v stave Idle alebo Alarm a nie počas behu
programu.

Nové stavové kódy: `error:46` žiaden kľúč, `47` súbor sa nedá prečítať alebo
zapísať (riadok `[MSG:USB stick: …]` povie prečo), `48` beží program
z kľúča, `49` PC posiela (displej vtedy program nespustí). Nový alarm:
`ALARM:21` program zastavený chybou G-kódu.

**Súbory z PC na kľúč** — `tools/usbfiles.py` (Python 3 s pyserial),
s kľúčom v GIGA a bez sendera na porte:

```
python tools/usbfiles.py ls
python tools/usbfiles.py put part.nc "Front panel.nc"
python tools/usbfiles.py sync D:\CNC\programs
python tools/usbfiles.py rm old.nc
python tools/usbfiles.py format
```

`sync` skopíruje G-kódové súbory priečinka, ktoré sú nové alebo zmenené
(podľa veľkosti a dátumu: kópia si ponechá dátum súboru z PC) a s `--delete`
odstráni tie, ktoré priečinok už nemá; `--dry-run` to len ukáže. Každá kópia
sa na GIGA spätne prečíta a jej CRC-32 sa porovná so súborom v PC. Dáta idú
ako hex po sériovej linke rýchlosťou asi 135 kB/s — 2 MB súbor za 15 s.
Mená po ceste stratia diakritiku (`frézka.nc` → `frezka.nc`) a `!`, `~`, `?`
sa zmenia na `_` (sú to príkazy Grbl v reálnom čase). `format` si najprv
vypýta `ERASE`.

**FORMAT na displeji** — tlačidlo vedľa stavu kľúča na stránke FILE,
**podržať 3 s** (ťuknutie to len oznámi): FAT32 do 32 GB, nad tým exFAT,
jeden oddiel cez celý kľúč; 8 GB kľúč trvá 10 s (FAT32) alebo 5 s (exFAT).
Všetko na kľúči sa stratí. MBR je taký, aký zapisuje Windows — podpis disku,
oddiel od 1 MB — a len ten oddiel sa nechá na f_mkfs() z FatFs, ktorého
vlastný MBR podpis nemá (nad 128 GB FatFs vytvorí GPT).

**Testy:** `tests/test_job.py` a test stránky FILE v `test_touch.py`
potrebujú v GIGA kľúč s FAT32 alebo exFAT. Svoje súbory naň dajú ako
`zz_<meno>` a na konci ich zase zmažú; vlastné súbory kľúča zostanú, ako
boli.

Stack hosta je kópia tých častí
[Arduino_USBHostMbed5](https://github.com/arduino-libraries/Arduino_USBHostMbed5)
0.3.1, ktoré kľúč potrebuje, v `src/m4/usbhost/` (pozri jeho README):
knižnica vyberá port USB-A na GIGA len pri kompilácii pre M7 a na M4 by vzala
port USB-C, cez ktorý Grbl komunikuje s PC. Súborový systém je FatFs R0.15
od ChaN v `src/m4/fatfs/` (pozri jeho README). Nič netreba inštalovať.

---

## Displej a dotyková obrazovka

| DRO | JOG |
|---|---|
| ![Stránka DRO](docs/display.png) | ![Stránka JOG](docs/display_jog.png) |
| **OVR** | **I/O** |
| ![Stránka OVR](docs/display_override.png) | ![Stránka I/O](docs/display_io.png) |
| **`$$`** | **`$$`, úprava nastavenia** |
| ![Stránka $$](docs/display_settings.png) | ![Editor $$](docs/display_settings_edit.png) |
| **FILE** (programy z USB kľúča) | **FILE**, bežiaci program (pozastavený) |
| ![Stránka FILE](docs/display_file.png) | ![Stránka FILE, beh](docs/display_file_run.png) |
| **E-STOP** (po STOP, až do resetu) | **Úvodná obrazovka** (po zapnutí) |
| ![Pruh E-STOP](docs/display_estop.png) | ![Úvodná obrazovka](docs/splash_a.png) |

**Úvodná obrazovka.** Po zapnutí displej ukáže logo Grbl, „GIGA R1“, pruh
priebehu a jeden riadok stavu — „Starting Grbl on the M7 core...“, „Waiting
for the USB stick...“, „Ready“ — s dátumom firmvéru a verziou nastavení
v rohoch. Zostane, kým Grbl neodpovie a USB kľúč sa neustáli (najviac
o 2,5 s dlhšie), aspoň 2 s, a dotyky sa medzitým nevykonávajú. Tá istá
obrazovka bez pruhu hlási „Waiting for Grbl on the M7 core...“ vždy, keď M7
na 3 s stíchne. Logo je `src/m4/logo.h`, vytvorené z `docs/Grbl Logo.svg`
skriptom `tools/make_logo.py` (4-bitové pokrytie ako pri písmach, kreslí ho
`cv_bitmap4()`).

Vždy na obrazovke:

* **stav** stroja (Idle, Run, Hold, Jog, Homing, Alarm, Door, Check, Sleep)
  ako farebný štítok; aktívny **modálny stav** (režim pohybu, pracovný
  súradnicový systém, G90/G91, G20/G21, rovina, nástroj, M-kódy vretena
  a chladenia); USB kábel do PC (s číslom riadka posielaného programu, ktorý
  má slová N) a pod ním USB kľúč: žiaden, pripája sa, pripojený
  (FAT32/exFAT), nedá sa pripojiť (NTFS, chyba), formátuje sa, alebo priebeh
  programu, ktorý z neho beží;
* **HOLD**, **START**, **RESET** a **UNLOCK** v pravom stĺpci. START sa
  rozsvieti, keď je čo obnoviť; UNLOCK je aktívny len v alarme;
* riadok správ s textom **alarmu**, poslednou **chybou** (desať sekúnd),
  radou po dotyku, ktorý nič neurobil, alebo s konfiguráciou stroja;
* šesť záložiek stránok.

Stránky:

* **DRO** — pracovná poloha X, Y, Z, A veľkými číslicami, strojová poloha
  vedľa každej, v mm / palcoch / stupňoch podľa potreby (riadok podriadenej
  osi A to povie), tlačidlo **X=0 … A=0** pre každú os (`G10 L20 P0`) a posuv
  a otáčky s ich korekciami.
* **JOG** — krok 0,01 / 0,1 / 1 / 10 mm (0,001 – 1 in pri `$13=1`; stupne pre
  rotačnú A) alebo **CONT**, rýchlosť posuvu 5 / 10 / 25 / 50 / 100 % z `$11x`
  každej osi, tlačidlá posuvu X Y Z A, **HOME** (`$H`) a **ALL=0**, kompaktné
  DRO a os a krok kolieska, keď je `$96` zapnuté.
* **OVR** — posuv −10 / −1 / 100 % / +1 / +10 (tlačidlá ± sa pri podržaní
  opakujú), rýchloposuv 25 / 50 / 100 %, otáčky −10 / 100 % / +10 a **STOP**
  vretena (vo feed hold), prepínače **FLOOD** a **MIST**.
* **I/O** — IN1–IN20 a OUT1–OUT12 ako indikátory, každý s menom priradenej
  funkcie; STOP, drivery, brzda Z a klieština; **AUX0 – AUX5** ako tlačidlá
  zap./vyp., **COLLET** (podržať na otvorenie, ťuknúť na zatvorenie)
  a impulzy **SERVO** a **VFD reset**.
* **`$$`** — všetkých 92 nastavení, šesť na stránku, každé s číslom, menom,
  jednoriadkovou nápovedou a aktuálnou hodnotou (masky ako písmená osí alebo
  čísla kanálov, voľby menom). Ťuknutie otvorí editor s úplným vysvetlením,
  čo nastavenie robí, a s:
  * **klávesnicou** pre čísla a čísla kanálov (DEL, CLR; bez desatinnej bodky
    pre celočíselné nastavenia);
  * **prepínačmi** pre bitové masky — X Y Z A pre `$2 $3 $23`, AUX0–5 pre
    `$46`, IN1–IN20, STOP a PWR pre `$79`, OUT1–12 pre `$93`, MPos/Buffer pre
    `$10`;
  * **voľbami** pre nastavenia zap./vyp. a režimy (`$40` Cartesian/CoreXY,
    `$41`, `$42` rotary / linear / slave of X / slave of Y, …).

  SAVE pošle Grbl `$n=hodnota` — len v pokoji alebo v alarme — a zatvorí
  editor; hodnota, ktorú Grbl odmietne (napr. `$0=1`, pod minimom 3 µs),
  nechá nastavenie tak, editor otvorený a chybu v riadku správ. CANCEL alebo
  znova záložka `$$` sa vráti na zoznam.

**Ako sa z dotykov stanú príkazy.** HOLD, START, RESET, korekcie, chladenie
a zastavenie vretena sú vlastné bajty Grbl v reálnom čase; M4 ich dá do
kruhového buffera v zdieľanej pamäti a M7 ich vloží do tej istej cesty ako
bajty z PC, takže pôsobia hneď v každom stave, presne ako zo sendera.
Tlačidlá AUX prepínajú tiež hneď, ako `M64`/`M65`, a rovnako COLLET a impulzy
resetu (aj počas pauzy `M6`, keď Grbl nečíta riadky). Posuv, homing,
odblokovanie a nulovanie sú pod tým príkazy (`$J=…`, `$H`, `$X`,
`G10 L20 P0 …`); M7 ich vykoná len **medzi dvoma riadkami z PC** — nikdy,
keď prišla len časť riadka —, takže dotyková obrazovka nemôže rozdeliť
riadok, ktorý sender posiela. Ich `ok`/`error` ide na displej, nie na USB
port, kde by neočakávaná odpoveď pokazila senderovi počítanie riadkov. Príkaz
z dotyku, ktorý sa nemôže vykonať do 0,5 s (lebo Grbl je napr. zaneprázdnený
prestávkou), sa zahodí, nevykoná sa neskôr v nejakom nesúvisiacom okamihu.

**Bezpečnosť.** Dotyková obrazovka nie je núdzové zastavenie — núdzové
zastavenie je bezpečnostné relé, ktoré odpojí napájanie, a jeho signálny
kontakt na STOP (D49, `$57=1`); kým je STOP zablokované, displej ukazuje cez
stránku červený pruh E-STOP s tým, čo treba urobiť. V rámci toho:

* **Podržať na vykonanie**: HOME a ALL=0 treba podržať 1 s, X=0 … A=0 0,8 s,
  UNLOCK a výstupy AUX 0,5 s — počas držania sa plní pruh. Letmý dotyk prsta
  nerobí nič, len ukáže radu.
* Tlačidlá, ktoré v aktuálnom stave nedávajú zmysel, sú sivé a nerobia nič
  (posuv len v pokoji alebo počas posuvu, nulovanie len v pokoji, …).
* **Plynulý posuv** beží len, kým prst zostáva na tlačidle. Zdvihnutie alebo
  zosunutie prsta pošle Grbl zrušenie posuvu; nezávisle od toho M7 posuv
  zruší samo, ak M4 prestane prst potvrdzovať na 300 ms (bezpečnostný spínač
  proti zamrznutému displeju), a každý plynulý posuv je naplánovaný najviac
  na 30 s a zastaví sa pred softvérovými limitmi, keď sú zapnuté. Grbl
  zastaví ~90 ms po zrušení plus spomalenie — najprv dobehnú už pripravené
  krokové segmenty, ako pri každom feed hold.
* Dotyky sa ignorujú, kým displej nemá čerstvý stav z Grbl (3 s, čo mazanie
  flash na M7 nikdy nedosiahne; homing posiela stav ďalej).

**RGB dióda.** RGB dióda shieldu (IS31FL3197 na tej istej I2C zbernici)
svieti farbou stavového štítku: zelená Idle, modrá Run, jantárová Hold,
azúrová Jog, fialová Homing, oranžová Door, sivá Check; červená blikajúca
v Alarme; červená rýchlo blikajúca po STOP; nesvieti, kým displej nemá stav
z Grbl. `$I` hlási `rgb=1`, keď odpovedala.

**Dotykový radič.** Goodix GT911 shieldu na I2C4 (Wire1, 400 kHz), reset PI2,
prerušenie PI1, číta M4 každých 10 ms. `$I` hlási `touch=1/…`, keď pri štarte
odpovedal, potom počet stlačení a kam padlo posledné.
`$TOUCH=x,y[,ms[,delay]]` stlačí obrazovku v bode (x, y) na šírku na ms
milisekúnd — tá istá cesta kódu ako prst, používa ju `tests/test_touch.py`.

Všetko kreslí M4 zo snímky, ktorú M7 každých 40 ms zapíše do zdieľanej SRAM4.
Každý panel sa prekreslí, len keď sa jeho obsah zmenil, do buffera vo vlastnej
RAM M4, ktorý potom DMA2D skopíruje do framebuffera — takže nič nebliká
a stojaci stroj nič nestojí.

`$SCREEN` spôsobí, že Grbl pošle framebuffer cez USB; `tools/screenshot.py`
z neho spraví PNG. Tak vznikli obrázky v tomto README.

---

## Nastavenia

Všetky nastavenia pôvodného Grbl okrem `$5` a `$6` (inverzia koncových
spínačov a sondy — každý vstup tu má vlastný bit v `$79`), a navyše:

| Nastavenie | Predvolene | Význam |
|---|---|---|
| `$40` | 0 | kinematika: 0 karteziánska, 1 CoreXY |
| `$41` | 0 | limity CoreXY: 0 na motor, 1 na os hlavy |
| `$42` | 0 | os A: 0 rotačná, 1 lineárna, 2 druhý motor X, 3 druhý motor Y |
| `$43` | 0 | referovať samostatnú os A v `$H` |
| `$46` | 1 | výstupy AUX, ktoré idú s vretenom (maska AUX0=1 … AUX5=32) |
| `$48` `$49` `$50` | 200, 100, 200 | brzda Z: drivery zap. → uvoľnená, uvoľnená → prvý krok, zabrzdená → drivery vyp. (ms) |
| `$51` | 5000 | dobeh vretena, kým sa smie otvoriť klieština (ms) |
| `$52` | 0 | časový limit pre otáčky (ms); 0 = nečakať |
| `$53` | 2 | strata otáčok počas rezania: 0 ignorovať, 1 feed hold, 2 alarm |
| `$54` | 0 | pevné čakanie po `M3`/`M4` bez relé otáčok (ms) |
| `$55` | 500 | impulz resetu serv / meniča (ms) |
| `$56` | 0 | zmena smeru → ďalší krok (µs, do 1000) |
| `$57` | 0 | vstup centrálneho STOP sa používa |
| `$58` | 1 | `M6`: 0 chyba (ako pôvodné Grbl), 1 pauza na ručnú výmenu nástroja |
| `$59` | 1 | brzda Z: 0 ju riadi driver, 1 výstup BRAKE |
| `$60`–`$78` | 1 … 19 | vstupný kanál každej vstupnej funkcie (0 = nepoužitá) |
| `$79` | 0 | NC vstupy: bit n−1 = INn, bit 20 = STOP, bit 21 = DRV PWR |
| `$80`–`$92` | 1 2 3 4 5 6 7 8 12 0 9 10 11 | výstupný kanál každej výstupnej funkcie (0 = nepoužitá) |
| `$93` | 0 | invertované výstupy: bit n−1 = OUTn |
| `$94` | 0 | strážiť vstup napájania driverov (D53) |
| `$95` | 1000 | ako dlho musí byť napájanie driverov späť pred `$H`/`$X` (ms) |
| `$96` | 0 | ručné koliesko: 0 vyp., 1 zap., 2 zap. s opačným smerom |
| `$97` | 0.001 | krok kolieska pri x1 (mm alebo stupne) |
| `$98` | 3000 | najvyšší posuv kolieska (mm/min) |
| `$103` `$113` `$123` `$133` | 8.889, 3600, 360, 360 | os A: kroky, rýchlosť, zrýchlenie, dráha (stupne pri `$42=0`) |

Verzia nastavení sa zmenila s I/O kanálmi (21): prvý štart toho firmvéru
obnovil predvolené hodnoty. Verzie 22 (`$94`, `$95`) a 23 (`$96`–`$98`) len
pridávajú nastavenia a záznam verzie 21 alebo 22 sa prevezme tak, ako je
(nové nastavenia dostanú predvolené hodnoty). Staršie záznamy sa stále
nahradia predvolenými — pred aktualizáciou si ulož výpis `$$` a `$#`.

Nové stavové kódy: `error:39` klieština otvorená, `40` STOP aktívne, `41`
koncový spínač v tom smere zopnutý, `42` kanál už je použitý, `43` porucha
meniča aktívna, `44` blokovanie klieštiny, `45` žiaden koncový spínač na
konci homingu, `50` drivery bez napájania (alebo ešte nie `$95` späť). Nové
alarmy: `ALARM:14` vreteno nedosiahlo otáčky, `17` alarm serva, `18` STOP,
`19` porucha meniča, `20` homing narazil na vzdialený spínač, `22` strata
napájania driverov.

Nastavenia sú vo vnútornej flash STM32, v dvoch 128 kB sektoroch banky 2
tesne pod programom M4 (0x08140000 a 0x08160000). Každá zmena pripíše 1 kB
záznam s poradovým číslom a CRC; sektor pojme 124 záznamov a až keď je jeden
plný, druhý sa zmaže (sekunda či dve, raz za 124 zmien). Záznam je platný, až
keď je zapísaný celý, takže strata napájania v ktoromkoľvek okamihu nechá
posledné dobré nastavenia čitateľné. Nahrávanie firmvéru tieto sektory
nemení, takže nastavenia prežijú preprogramovanie.

---

## Ako to funguje na GIGA

**Dve jadrá, jeden sketch.** `Grbl_Giga_R1.ino` sa kompiluje raz pre každé
jadro. Jadro Grbl a HAL pre M7 sú ohraničené `CORE_CM7`, displej `CORE_CM4`;
čo build pre M4 skompiluje z Grbl, sa nikdy nevolá a linker to vyhodí. Jadrá
nezdieľajú nič okrem bloku v `src/grbl_shm.h` na 0x3800C000 v SRAM4: M7 ho
cez MPU mapuje bez cache a zapisuje pod počítadlom sekvencie, M4 ho skopíruje
a kópiu si ponechá, len ak bolo počítadlo párne a nezmenené. M4 zapisuje späť
heartbeat, kruhový buffer bajtov v reálnom čase z dotykovej obrazovky
a jednomiestnu schránku pre dotykové akcie, ktoré sú príkazmi (pozri
dotykovú obrazovku vyššie).

**Generovanie krokov.** mbed OS drží na každom jadre 32-bitový časovač pre
svoj mikrosekundový ticker (TIM5 na M7, TIM2 na M4), takže Grbl používa
16-bitové časovače: TIM7 pre prerušenie krokovania, TIM6 v režime jedného
impulzu na ukončenie každého krokového impulzu, TIM17 na skenovanie vstupov,
všetky na 240 MHz. Perióda kroku dlhšia ako 65536 tikov (pod 3,7 kHz) sa
v HAL rozdelí na rovnaké cykly časovača a prerušenie Grbl beží na poslednom
z nich. Časovač impulzu a prerušenie STOP bežia s prioritou NVIC 0,
krokovanie s 1, skenovanie vstupov s 2; všetko ostatné na M7, vrátane USB
stacku, sa pri štarte presunie pod ne. (Skenovanie bývalo nad krokovaním;
s dvadsiatimi kanálmi trvá 1,3 µs a keď prerušilo krokovanie medzi udalosťou
časovača a krokovým impulzom, pri 200 kHz sa impulzy zlučovali. STOP
skenovanie nepotrebuje a skenovanie sa oneskorí najviac o jedno prerušenie
krokovania.) Prerušenia bežia na vlastnom 8 kB zásobníku namiesto 1 kB
z mbed.

**USB.** USB CDC stack v mbed volá callback príjmu zo svojho prerušenia
s maskovanými prerušeniami; callback Grbl beží hneď po vlastnom callbacku
jadra a každý bajt odovzdá filtru príkazov v reálnom čase, takže `!`
a Ctrl-X pôsobia z kontextu prerušenia ako na AVR. Výstup ide do 8 kB
kruhového buffera, ktorý hlavná slučka odovzdáva endpointu po 63 bajtoch
neblokujúcim `send_nb()` — 63, aby každý paket ukončil USB prenos. Keď port
nikto nečíta, Grbl sa nezasekne: výstup sa zahadzuje, kým port nemá otvorený
žiaden terminál, a po 3 s zaseknutého endpointu.

**Kritické sekcie mbed.** mbed chráni svoje dáta pomocou
`core_util_critical_section_enter()`, ktoré predvolene maskuje *všetky*
prerušenia — a USB stack zariadenia spracúva každú udalosť USB vo vnútri
jednej z nich. To zdržiavalo prerušenie krokového impulzu až o 13 µs, dosť
na zlúčenie krokových impulzov nad 100 kHz (17 z 383 000 pri 100 kHz). Jadro
deklaruje tri háčiky za tým ako weak, takže tento port ich nahrádza
(`hal_h7.cpp`) verziou, ktorá maskuje podľa priority (BASEPRI): celé mbed
vrátane USB je zadržané presne ako predtým, zatiaľ čo tri časovače Grbl nad
ním bežia ďalej — stav mbed nikdy nemenia. Najhoršia latencia časovača
impulzu odvtedy: **1 µs**, a v žiadnom teste zlúčený impulz. Kým `hal_init()`
nepresunie prerušenia mbed pod časovače Grbl, háčiky ďalej maskujú všetko,
a prerušenia, ktoré nejaký ovládač neskôr zapne s prioritou 0, sa do 100 ms
presunú nižšie.

**Ďalšie dve pasce v mbed, každá za cenu jedného pádu**, dobré vedieť pre
každého, kto na tomto jadre zapisuje do flash: MPU v mbed robí celú oblasť
kódu len na čítanie, takže obyčajné `HAL_FLASH_Program()` skončí ako
MemManage fault; a volanie, ktoré tú ochranu zruší, je
`mbed_mpu_manager_lock_rom_write()` — „zamkni“ zapisovateľný stav —, zatiaľ
čo `unlock` ochranu vráti. Volanie v opačnom poradí podtečie počítadlo a mbed
sa zastaví s červeným vzorom 4 dlhé – 4 krátke.

**Čas.** `millis()` z mbed na tejto doske meraním bežal o 4 % pomalšie než
PC (pričom rýchlosti krokov merané podľa PC sú presné na 0,1 %, takže takt
jadra je správny); `hal_millis()` preto počíta takty CPU.

**Menšie zmeny oproti pôvodnému správaniu.** Stavové hlásenie vyžiadané
v tom istom USB pakete ako `!` alebo `~` hlási stav, ktorý ten príkaz
vytvoril (pôvodné Grbl ho vypíše pred ich spracovaním, čo fungovalo len
preto, že UART nechal hlavnú slučku bežať medzi dvoma znakmi). Na žiadosti
o stav sa odpovedá aj počas kritického alarmu („Reset to continue“), ako to
tam hovorí komentár v pôvodnom kóde. Číslo nástroja `T` sa zachová z jedného
bloku do ďalšieho (pôvodné Grbl ho nulovalo pri každom bloku bez slova T,
takže `$G` ukazovalo T0). `M5` uvoľní FWD aj REV. `$I` dostane odpoveď
v každom stave (len číta), takže jeho diagnostiku možno sledovať aj počas
pohybu.

**Homing udržiava spojenie.** Slučka homingu v pôvodnom Grbl nič neobsluhuje
(„no time to run protocol_execute_realtime()“). Na AVR prerušenie UART stále
prijalo `?` a Ctrl-X; tu USB výstup, displej a príkazy dotykovej obrazovky
žijú v hlavnej slučke, takže počas celého hľadania spínača — až 1,5 × `$13x`
pri `$25`, 36 s na os s predvolenými nastaveniami a bez pripojeného
spínača — PC nič nepočulo, displej sa vrátil na „waiting for Grbl“ a jeho
RESET sa nečítal. Slučka teraz pri každom prechode obsluhuje linku a displej
a odpovedá na `?` (stav `Home`); reset z PC, z dotykovej obrazovky alebo zo
vstupu reset ukončí homing s `ALARM:6`, ako v pôvodnom Grbl.

---

## Štruktúra

```
Grbl_Giga_R1.ino         jeden sketch, dve jadrá: Grbl na M7, displej na M4
build.sh                 zostaví a nahrá obe jadrá
PLAN.md                  plán vývoja, požiadavky na shield
PINOUT.md                tabuľka pinov shieldu periférií
src/
  grbl_main.c            main.c z pôvodného Grbl, rozdelený na grbl_init()/grbl_loop()
  config.h defaults.h    voľby pri kompilácii a profily strojov
  cpu_map.h              priradenie pinov
  kinematics.[ch]        karteziánska / CoreXY kinematika, režimy osi A
  io.[ch]                kanály IN1-IN20 / OUT1-OUT12, priradenie funkcií, $TIN/$TSW
  outputs.[ch]           výstupné funkcie, M62-M65, $46, klieština, impulzy resetu
  estop.[ch]             centrálne STOP (EXTI4)
  display.[ch]           strana displeja na M7: snímka stavu, príkazy z dotyku, $SCREEN, $TOUCH, $SPLASH
  jog.[ch]               príkazy posuvu: $J= zo sendera, z dotykovej obrazovky, z kolieska
  drvpwr.[ch]            vstup napájania driverov (D53), ALARM:22
  mpg.[ch]               ručné koliesko: koliesko na TIM8, prepínače, LED
  job.[ch]               programy z USB kľúča: kontrolný prechod, beh, STOP, príkazy $F
  grbl_job.h             zdieľaný blok pre USB kľúč: zoznam súborov, požiadavky, kruhový buffer riadkov
  grbl_shm.h             rozloženie zdieľanej pamäte medzi jadrami
  platform.h             náhrady za AVR
  planner.c gcode.c stepper.c limits.c ...   jadro Grbl, štyri osi
  hal/
    hal.h                rozhranie platformy, ktoré vidí jadro v C
    hal_h7.cpp           GPIO, TIM6/7/17, PWM na TIM4, DAC, oneskorenia, LED
    hal_serial.cpp       prenos cez USB CDC
    hal_nvs.cpp          záznamník nastavení vo vnútornej flash
    hal_shm.cpp          zdieľaná pamäť, štart M4
  m4/
    gui.cpp              displej stavu a dotykový panel (beží na M4)
    usbstick.[ch]        USB kľúč: vlákno hosta, zoznam, čítanie, zápis, formátovanie
    usbhost/             USB host stack (z Arduino_USBHostMbed5 0.3.1, build pre M4)
    fatfs/               FatFs R0.15 od ChaN: FAT a exFAT, MBR a GPT (build pre M4)
    touch.[ch]           dotykový radič GT911, RGB dióda
    canvas.[ch]          kreslenie: pomocný buffer + DMA2D, vyhladzovaný text
    fonts.h              generuje tools/gen_fonts.py (DejaVu Sans Mono)
    logo.h               generuje tools/make_logo.py (logo Grbl na úvodnej obrazovke)
tools/
  gen_fonts.py           vykreslí písma displeja
  make_logo.py           rastrovanie docs/Grbl Logo.svg pre úvodnú obrazovku
  screenshot.py          uloží displej ako PNG cez $SCREEN
  fastbuild.py           rýchly prírastkový build z podrobného logu arduino-cli (pomôcka pri vývoji)
  usbfiles.py            súbory z PC na USB kľúč v GIGA: ls, put, rm, sync, format
tests/
  grbl_link.py           spoločný pomocník pyserial pre skripty nižšie
  test_comm.py           linka: úvodný riadok, integrita, latencia, príkazy v reálnom čase, priepustnosť, dlhodobý test
  test_steprate.py       presnosť rýchlosti krokov, strop, počty impulzov, pokrytie skenovania
  scope_step.py          pin STEP na osciloskope Tektronix MDO3034: rýchlosť, šírka impulzu, rampa voči $0/$100/$110/$120
  scope_dir.py           STEP a DIR na osciloskope: príprava smeru a držanie pri rýchlych zmenách smeru, $56
  test_stream.py         posielanie G-kódu: rampa, feed hold, trvalá záťaž
  test_axes.py           režimy osi A, impulzy podriadeného motora, výstupy, M62-M65, alarm serva
  test_io.py             priradenie, koncáky +/-, homing, STOP, relé vretena, klieština, M6, brzda, $56, napájanie driverov
  test_job.py            programy z USB kľúča (jeden musí byť zastrčený): kontrolný prechod, M0, M6, chyby,
                         STOP, rýchlosť, mená s medzerami, spätné čítanie
  usbjob.py              dá testovacie súbory na kľúč ako zz_<meno> ($FW) a odstráni ich, vypíše ($F)
  test_corexy.py         CoreXY: impulzy remeňových motorov, uzavretie dráhy, posuv, $41, vylúčenia
  test_touch.py          každé dotykové tlačidlo a editor $$ cez $TOUCH, snímky obrazovky (--shots)
  test_mpg.py            koliesko: kroky, OFF/blokované, sledovanie kolieska, zahodený nadbytok, zmena osi, softvérové limity
  demo_run.py            pošle examples/demo_complex.nc a zmeria čas
examples/                ukážkový G-kód; usb/ - testovacie súbory pre kľúč
docs/*.png               snímky displeja ($SCREEN)
docs/scope/              grafy STEP a DIR z osciloskopu
hardware/Grbl_Giga_Shield/   mechanika shieldu z CAD súborov Arduina, koncept rozloženia
```

---

## Overenie

Merané na Arduino GIGA R1 WiFi s GIGA Display Shieldom, cez natívne USB
z PC s Windows 11, skriptmi v `tests/` (Python 3 + pyserial). Bez pripojených
motorov: strojová poloha v stavových hláseniach je vlastné počítadlo krokov
prerušenia krokovania, takže jej vzorkovanie voči hodinám PC meria rýchlosť
krokov, ktorú prerušenie vytvorilo; `$I` počíta prerušenia krokovania,
ukončenia impulzov, impulzy, ktoré dostal každý driver (so znamienkom podľa
smeru), a skenovania vstupov a spätne číta registre PWM a DAC. Čísla nižšie
sú z finálneho firmvéru vrátane dotykového panela a CoreXY.

**Presnosť rýchlosti krokov** (`test_steprate.py`), 100 krokov/mm, zadané
voči nameranému — všetky štyri úrovne AMASS a pod 3,7 kHz natiahnuté periódy
16-bitového časovača v HAL:

| Zadané kroky/s | 100 | 250 | 500 | 1000 | 2000 | 4000 | 8000 | 16000 | 4000 pri 400 krokoch/mm |
|---|---|---|---|---|---|---|---|---|---|
| Namerané | 100 | 250 | 500 | 1000 | 2000 | 4001 | 8000 | 16005 | 4000 |

**Strop rýchlosti krokov**, 250 krokov/mm, `$0=3`; posledný stĺpec sú
ukončenia impulzov voči prerušeniam krokovania pri tom istom pohybe (rovnaké
číslo znamená, že sa žiaden impulz nezlúčil s ďalším):

| Zadané | 1 os | X+Y+Z+A naraz | impulzy / tiky (4 osi) |
|---|---|---|---|
| 10000 | 10001 | 10002 | 37279 / 37279 |
| 50000 | 50014 | 50018 | 188425 / 188425 |
| 100000 | 100016 | 100026 | 383417 / 383417 |
| 150000 | 150008 | 149973 | 585126 / 585126 |
| 200000 | 199999 | 200059 | 793425 / 793425 |
| 250000 | 200067 (obmedzené) | 200053 (obmedzené) | 1008417 / 1008417 |
| 300000 | 200015 (obmedzené) | 200036 (obmedzené) | 1230126 / 1230126 |

Presné do **200 kHz na všetkých štyroch osiach naraz**, kde to
`MINIMUM_STEP_CYCLES` zámerne obmedzí; za celý beh 10,11 milióna prerušení
krokovania a rovnako veľa ukončení impulzov, **0 zlúčených impulzov**,
najhoršia latencia prerušenia krokovania **1 µs**. (Port pre UNO Q končí na
100 kHz, R4 na 84 kHz na jednej osi, AVR Grbl okolo 30 kHz.) Skenovanie
vstupov si drží svojich 10 kHz — 100 % pokrytie pri 200 kHz × 4 osi —, hoci
je teraz tesne pod prerušením krokovania; `$I isr=` hlási jeho najdlhší beh,
1,3 µs s dvadsiatimi kanálmi v pokoji.

**Na osciloskope** (`tests/scope_step.py`: Tektronix MDO3034, CH1 na STEP X;
celý pohyb zaznamenaný 10M bodmi, potom vlastné merania osciloskopu počas
ustálenej rýchlosti; grafy a snímky v `docs/scope/`):

| `$0` | `$100` | `$110` | zadané | očakávané | namerané (priemer ustálenej rýchlosti) | kroky | impulz | medzera |
|---|---|---|---|---|---|---|---|---|
| 10 µs | 250 | 500 | 2083,3 Hz | 2083,3 Hz | 2083,6 Hz, periódy 480 ± 1 µs | 12500 / 12500 | 10,38 µs | 469,6 µs |
| 10 µs | 1000 | 12000 | 200 kHz | 83,33 kHz (perióda ≥ `$0` + 2 µs) | 83,342 kHz | 100000 / 100000 | 10,37 µs | 1,64 µs |
| 3 µs | 1000 | 12000 | 200 kHz | 200 kHz | 200,020 kHz | 100000 / 100000 | 3,37 µs | 1,63 µs |
| 3 µs | 1000 | 15000 | 250 kHz | 200 kHz (`MINIMUM_STEP_CYCLES`) | 200,022 kHz | 100000 / 100000 | 3,37 µs | 1,63 µs |

* Rýchlosť je tá zadaná, alebo strop: 5 µs (200 kHz) a `$0` + 2 µs — pri
  predvolenom `$0=10` je strop 83,3 kHz. Všetky namerané rýchlosti sú
  o 0,010 % vyššie, kryštál GIGA voči kryštálu osciloskopu.
* Impulz je vždy `$0` + 0,37 µs: vstup do prerušenia, ktoré impulz ukončí,
  a zápisy do časovača a GPIO pred ním. Nikdy nie kratší ako `$0`.
* Zrýchlenie zmerané z rámp: 10,02 a 995 mm/s² pre `$120` 10 a 1000 (999,9
  s 2 ms segmentmi, nižšie). Na rampách môže jeden krok prísť skôr alebo
  neskôr tam, kde začína nový segment alebo kde AMASS mení úroveň (2, 4
  a 8 kHz) — až ±60 µs pri 2 kHz, ako v AVR Grbl.
* **Rampa je schodisko**, jeden schod na krokový segment — tak Grbl vytvára
  zrýchlenie: každý segment beží jednou rýchlosťou. 100 segmentov za sekundu
  (10 ms) v pôvodnom Grbl robilo schody po 10 kHz pri 1000 mm/s²
  a 1000 krokoch/mm; port teraz beží **500 za sekundu (2 ms) → schody po
  2 kHz** (`ACCELERATION_TICKS_PER_SECOND`, so `SEGMENT_BUFFER_SIZE` 50, aby
  buffer stále obsahoval ~100 ms pohybu):
  ![rampa 100 vs 500 segmentov](docs/scope/step_x_ramp_100_vs_500.png)
  Toto meranie odhalilo chybu, ktorú kratšie segmenty vyniesli na povrch. Pod
  asi 460 krokmi/s je perióda kroku dlhšia ako 16-bitový časovač a HAL ju
  počíta v niekoľkých cykloch časovača; zhruba raz za načítanie segmentu
  prišlo prerušenie časovača druhýkrát bez udalosti update (jeho príznak ešte
  nebol v časovači vymazaný, keď obsluha skončila) a započítané ako cyklus
  spôsobilo, že ďalší krok prišiel o jeden cyklus skôr — 100 krokov/s bežalo
  na **101,8**, 250 na 254,2. Obsluha teraz vstup bez príznaku update
  ignoruje (`$I spur=` ich počíta): **100,013 a 250,030 Hz**, každá perióda
  do 1 µs od 10 a 4 ms.
  Pri presne 200 kHz meria ustálená rýchlosť o 0,01–0,05 % menej
  (5000,5–5002,6 ns v 8 ms oknách): Grbl zaokrúhľuje periódu kroku každého
  segmentu nahor na celé 4,17 ns tiky časovača a s 2 ms segmentmi ich viac
  padne na 1201 než na 1200. Tam, kde sa segmenty stretávajú, je jedna
  perióda mimo až o ±125 ns a nasledujúca to vyrovná; kroky a geometria sú
  presné.
* Hrany 1,1 ns, 0,04 / 3,25 V; prekmit ±52 % je hlavne zemný vodič sondy pri
  takých hranách (pre shield by stačila pomalšia rýchlosť GPIO).
* **DIR voči STEP** (`tests/scope_dir.py`, CH2 na DIR X; cik-cak s 20 ostrými
  rohmi, kde X mení smer pri 50–100 kHz, `$0=3`, 1000 krokov/mm): s `$56=0`
  sa DIR zmení **203 ns** pred nábežnou hranou kroku (obe v tom istom
  prerušení, DIR prvé); s `$56=35` prvý krok po každej zmene príde
  **35,4–35,6 µs** po nej, ďalší o 6,5 µs neskôr (stále nad minimom 5 µs),
  potom ostatné podľa plánu. DIR sa nikdy nezmenil, kým bol impulz hore —
  vždy aspoň 6,6 µs po poslednej dobežnej hrane — a všetkých 40 000 krokov
  bolo na mieste v oboch prípadoch.

**Linka** (`test_comm.py`): odozva `?` **0,21 ms medián** (p95 0,33 ms,
max. 0,41 ms, 4818 hlásení/s); `!` ukáže `Hold` a `~` je späť v `Idle` do
**0,3 ms**. Riadky s komentárom s počítaním znakov: **9850 riadkov/s**
(128 kB/s) s oknom 127 bajtov, 9130 s 500 bajtmi. 30 s dlhodobý test
s dopytovaním každých 50 ms: **270 848 riadkov, 270 848 ok, 0 chýb,
0 poškodených rámcov** (skorší 60 s test: 535 012 riadkov, 0 chýb). `$$`
a `$I` 20-krát bajtovo zhodné; nastavenia prežijú softvérový reset.

**G-kód počas pohybu** (`test_stream.py`), 250 krokov/mm, 300 segmentov na
riadok tabuľky, každý riadok potvrdený a konečná poloha presná na krok:

| Segment | Posuv | kroky/s | riadky/s |
|---|---|---|---|
| 0,50 mm | 2400 | 10000 | 93 |
| 0,50 mm | 9600 | 40000 | 168 |
| 0,20 mm | 12000 | 50000 | 417 |
| 0,10 mm | 12000 | 50000 | 767 |
| 0,05 mm | 12000 | 50000 | 1282 |

Tieto čísla určuje planner, nie linka: každý segment treba rozbehnúť
a zabrzdiť. Feed hold 1,5 s po začiatku 400-riadkového posielania,
pokračovanie o 3 s neskôr: `Run → Hold:1 → Hold:0 → Run`, 400/400 ok,
konečná poloha presná. 120 s segmentov 0,15 mm pri F5400 s dopytovaním
každých 50 ms: **59 686 riadkov, 0 chýb**, 2 587 936 prerušení krokovania
= 2 587 936 ukončení impulzov (30 s s finálnym firmvérom: 14 953 riadkov,
0 chýb, žiaden zlúčený impulz).

**Ukážkový program** `examples/demo_complex.nc` (2595 riadkov: oblúky,
skrutkovice, 720-segmentová špirála, 1786-riadkový raster) poslaný ako zo
sendera: **152,9 s, 2595 ok** — rovnako ako na UNO Q, teda limity zrýchlenia
stroja, nie riadiacej jednotky.

**Štvrtá os, výstupy, alarmy** (`test_axes.py`), všetko prechádza:

* `$42=0`: `G0 A90` → A 90; `G20 G1 A45` → A 45 (stupne sa neprepočítavajú);
  celý kruh `G2` s `A0→180` končí presne na A 180,000.
* `$42=1`: `G20 G1 A1` → A 25,4 mm.
* `$42=2` / `3`: `G1 A10` odmietnuté s `error:20`; `G1 X10` (alebo `Y10`) pri
  100 krokoch/mm vydá presne **1000 impulzov na driver A**, zatiaľ čo A hlási
  0; pohyb Z nepridá žiaden.
* `M3` → OUT1 FWD + OUT5 (AUX0 ide s vretenom), `M4` → OUT2 REV s FWD
  vypnutým, `M5` všetko vypnuté; `M7`/`M8` → OUT4/OUT3; `M64 P1`, `M64 P3` →
  OUT6, OUT8; `M65 P1` + `M62 P2` po pohybe → OUT7 + OUT8; `M64 P4` → OUT12;
  `M64 P5` zapne AUX5 bez priradeného kanála; `M64 P6` odmietnuté; `$93=4095`
  invertuje piny bez zmeny logického stavu; `$84=11` odmietnuté (`error:42`,
  OUT11 je reset meniča), kým `$92=0`, potom AUX0 beží na OUT11; `M2` vypne
  všetko.
* `S12000` s `$30=24000`: PWM spätne prečítané z TIM4 50,0 %, DAC 2047;
  `S6000`: 25,0 %, 1023; `M5`: 0, 0.
* IN9 (alarm serva X) aktivovaný cez `$TIN=9,1`: **ALARM:17** hneď, `Pn:M`
  v stave; reset po ňom dá impulz `$55` na OUT10 (reset serv), po `$55`
  zmizne; uvoľnené a `$X`: Idle.

**I/O kanály, koncáky, STOP, vreteno, brzda** (`test_io.py`), všetko
prechádza:

* Priradenie: X+ na vstup Y− odmietnuté (`error:42`), X+ na vstup X− prijaté
  (spínače v sérii), `$60=21` a `$79` s bitom 22 odmietnuté, `$5` neexistuje;
  `$79=1` spraví, že nečinný IN1 číta aktívny.
* Koncáky, `$21=1`: X− zopne → `ALARM:1` `[MSG:Limit X-]`; po resete stále
  Alarm (`Check Limits`) a `Pn:X`; posuv ďalej do X− odmietnutý
  (`error:41`), posuv preč o 2 mm so zopnutým spínačom: Idle; `G1` späť doň:
  `ALARM:1`. Dva spínače na jednom vstupe: `[MSG:Limit X]` a os sa môže
  pohnúť oboma smermi. Len mínusový spínač: rovnaké pravidlá ako dva vstupy.
  Virtuálny spínač X+ na 3 mm, do ktorého narazí `G1 X10`: `[MSG:Limit X+]`.
* Homing na virtuálnych spínačoch 5 mm od začiatku (`$TSW`), `$23=0`: `$H`
  hotový za 6,4 s, strojová poloha −1, −1, −1 (odsun od spínača); spínač Z−
  umiestnený na plusovom konci zopne pri homingu smerom k Z+: `ALARM:20`;
  `$65=0` (bez Z+): `$H` odmietnutý s `error:45`.
* STOP počas pohybu so zapnutým vretenom a chladením: `ALARM:18`
  `[MSG:E-stop]`, `Pn:E`, ani jeden krok po vypnutí, vyvolané jeho
  prerušením v rutine trvajúcej **10,6 µs**; výstupy, enable a brzda vypnuté;
  `$X` odmietnuté (`error:40`), kým je aktívne, aj po uvoľnení až do resetu;
  potom `$X` ok. STOP aktívne už pri zapnutí `$57`: nájde ho skenovanie.
* Napájanie driverov, `$94=1`, `$95=500`, napájanie simulované cez `$TIN=22`:
  stratené počas pohybu → `ALARM:22`, ani jeden krok po ňom, enable a brzda
  vypnuté; `$X` a `$H` → `error:50`; späť → `$X` stále odmietnuté, kým
  neuplynie `$95`, potom `[MSG:Driver power on]` a `$X` ok; stratené v pokoji
  s drivermi držiacimi polohu (`$1=255`, brzda uvoľnená) → `ALARM:22`, brzda
  zabrzdená, drivery vypnuté; stratené znova v tom alarme → `ALARM:22` bez
  resetu; stratené počas homingu → `ALARM:22` (nie `ALARM:6`); `$94=1` bez
  napájania → `ALARM:22`; bit 21 v `$79` invertuje vstup; `$94=0` ho znova
  ignoruje. Doska naštartovaná s `$94=1` a ničím na D53: Alarm, `drv=1/1`,
  `$X` odmietnuté.

**Ručné koliesko** (`test_mpg.py`, 1000 krokov/mm, koliesko otáčané cez
`$TMPW` cez TIM8), všetko prechádza:

* x1, 100 dielikov: X +0,1000 mm; x10, −50: −0,5000; x100, 20: +2,0000;
  `$96=2`: +10 dielikov pri x10 → Y −0,1000; os 4 = rotačná A, x100,
  15 dielikov: +1,5000°; `$97=0.005`, x10, 7 dielikov: Z +0,3500.
* S `$96=0`, pri OFF alebo v režime kontroly sa nič nepohne (stav „blocked“,
  LED bliká).
* 200 dielikov rýchlosťou 100/s s x100: os ide 10,00 mm/s ako koliesko,
  dorazí všetkých 20,0000 mm, Idle 140 ms po zastavení kolieska (vrátane
  dopytovania stavu).
* 200 dielikov rýchlosťou 1000/s (100 mm/s) s `$98=1200` (20 mm/s): pohyb
  4,000 mm — len to, čo os stihla — a Idle 134 ms po koliesku.
* Z X na Y uprostred otáčania: X sa hneď zastaví, zvyšok otáčania hýbe Y; OFF
  hneď zastaví Y.
* Po `$H` a `$20=1` (dráha 10 mm): koliesko zastaví X na −0,0010 a −9,9990,
  na softvérových limitoch.
* Vreteno, `$52=1500`: bez relé → `ALARM:14` po 1,50 s s FWD/REV vypnutými;
  relé zopnuté → `M3` hotové hneď; `S800` počas behu počká na ustálenie
  250 ms (0,250 s); relé stratené počas rezania s `$53=1` → `Hold:0`, START
  po jeho návrate → Run; `$53=2` → alarm po riadenom zastavení; porucha
  meniča → `ALARM:19`, FWD/REV vypnuté, `$X` odmietnuté (`error:43`)
  s impulzom resetu meniča na OUT11, porucha preč → `$X` ok.
* Klieština, `$51=300`: otvorenie odmietnuté, kým vreteno beží a hneď po
  `M5`, prijaté po dobehu (OUT9); `M3` pri otvorenej → `error:39`; otvorenie
  počas pohybu → `error:44`. `T3 M6`: `[MSG:Tool change T3, cycle start to
  resume]`, `Hold:0`, START pokračuje, vreteno zostane vypnuté.
* Brzda, `$48/$49/$50` = 200/100/200, `$1=25`: drivery zapnuté v 1 ms, brzda
  uvoľnená v 201 ms, prvý krok v 302 ms; po pohybe brzda zabrzdená v 26 ms,
  drivery vypnuté v 225 ms. `$59=0`: BRAKE nikdy nezapnutý, prvý krok 301 ms
  po enable, drivery vypnuté v 26 ms.
* `$56`, 40 ostrých cik-cak rohov pri 30 m/min, 1000 krokov/mm, impulzy
  3 µs: bez neho krok nasleduje zmenu smeru v tom istom prerušení (0,0 µs);
  s `$56=35` sa zdržalo 39 krokov a najkratší čas od smeru po krok bol
  **35,4 µs**; 0 zlúčených impulzov a dráha presná na krok v oboch prípadoch.

**CoreXY** (`test_corexy.py`, 100 krokov/mm, `$40=1`), všetko prechádza —
impulzy počítané pre každý driver cez `$I motor=`:

| Pohyb | Driver X (A) | Driver Y (B) |
|---|---|---|
| `G1 X10` | +1000 | +1000 |
| `G1 Y10` | +1000 | −1000 |
| `G1 X0 Y0` z X10 Y10 | −2000 | 0 |
| `G1 X10 Y-10` z 0,0 | 0 | +2000 |
| dva celé kruhy (`G2`, `G3`) | spolu 0 | spolu 0 |
| `$J=G91 X5 Y5` | +1000 | 0 |

Stav po štvrtom pohybe hlási hlavu na X10 Y−10; pohyby Z a A vytvárajú len
impulzy Z a A. Časy pri `$110=6000` (100 mm/s) a `$120=1000`:

| Pohyb | Namerané | Očakávané | Pôvodné CoreXY |
|---|---|---|---|
| `G1 X60 F3600` | 1,078 s | 1,060 s | 1,457 s |
| 60 mm uhlopriečka pri F3600 | 1,109 s | 1,085 s | 1,474 s |
| `G0 X100 Y100`, `$41=0` (limity motorov) | 2,125 s | 2,100 s | |
| `G0 X100 Y100`, `$41=1` (limity hlavy) | 1,125 s | 1,100 s | |

(konštantných +20 ms je dopytovanie stavu z PC). `$40=1` s podriadenou A
a `$42=2` s CoreXY sa obe odmietnu s `error:5`.

**Dotyková obrazovka** (`test_touch.py`, cez `$TOUCH`), všetko prechádza:

* HOLD počas pohybu → `Hold:0`; START → `Run`; RESET počas pohybu →
  `ALARM:3`; UNLOCK krátko ťuknutý → stále Alarm, podržaný 0,8 s → Idle.
* Krokové posuvy X+, Y−, Z+, A− o 1 mm sa pohnú presne o 1,000; tri ťuknutia
  pri 0,01 mm o 0,030.
* Plynulý posuv pri 25 % (25 mm/s) podržaný 1 s: 27,8 mm, Idle 0,19 s po
  zdvihnutí prsta, potom stojí; pri 100 % (100 mm/s) podržaný 0,5 s: 60 mm.
* Y=0 krátko ťuknuté: bez zmeny; X=0 podržané: G54 X = 12,5; ALL=0 podržané:
  G54 = 12,5, −3, 4, 7.
* Posuv +10, −10 dvakrát, 100 %; +1 podržané 1 s → 105 % (automatické
  opakovanie); rýchloposuv 25 %; otáčky +10; FLOOD a MIST prepínajú OUT3
  a OUT4.
* AUX1 krátko ťuknuté: vypnuté; podržané: OUT6 zapnutý; podržané znova:
  vypnutý.
* COLLET krátko ťuknuté: zostane zatvorená; podržané: OUT9 zapnutý, `M3`
  odmietnuté; ťuknuté: zatvorená. V pauze `T5 M6` (stlačenia zaradené
  vopred, lebo Grbl vtedy nečíta riadky): podržaný COLLET ju otvorí, START sa
  odmietne, kým je otvorená, ťuknutie ju zatvorí, START pokračuje.
* Nulovanie podržané počas 2 s `G4` (Grbl hlási Idle, ale je v parseri) sa
  zahodí, nevykoná sa po prestávke; posuv v alarme nerobí nič.
* Ani jeden `ok` ani `error` za dotykovú akciu sa nedostal na USB port.
* Stránka `$$`: `$0` napísané 12 a uložené → `$0=12`; `$0=1` Grbl odmietol,
  `$0` nezmenené; `$110` napísané 12345, DEL, .5 → `$110=1234.5`; `$3`
  so zapnutými X a Z → `$3=5`; `$42` voľba „linear“ → `$42=1`; CANCEL nič
  nezmení; `$79` so zaškrtnutými IN1, STOP a PWR → `$79=3145729`; `$95`
  napísané 750 → `$95=750`; `$90=4` (OUT4 je mist) Grbl odmietol.
* Ovládač RGB diódy odpovedá na I2C (`rgb=1`).
* HOME so zapnutým homingom a bez pripojeného spínača: `?` počas celého
  hľadania odpovedá `Home` a RESET na obrazovke 4 s po začiatku ukončí
  homing s `ALARM:6`. (Pred opravou: žiadna odpoveď a dotykový RESET sa
  ignoroval, kým hľadanie neskončilo s `ALARM:9`.)

GT911 odpovedá na adrese 0x5D a hlási konfiguráciu 480 × 800 (na výšku),
ktorú M4 otáča rovnako ako obraz; prstom ťuknuté záložky sa trafia tam, kde
sa ich dotkne.

Chyba, ktorú ukázali len skutočné prsty: M7 zahadzuje dotykové príkazy staršie
ako 0,5 s a vek meria ako aktuálny čas M4 mínus časová značka požiadavky.
Skutočné stlačenie číta GT911 cez I2C pred tým, ako požiadavku označí, takže
značka mohla byť o milisekundu *neskoršia* ako čas M4, s ktorým ju M7
porovnávalo — záporný vek, ktorý porovnanie bez znamienka považovalo za
veľmi starý. Každý posuv, nulovanie a odblokovanie prstom sa zahodil
s `error:8`; simulované stlačenia preskakujú prenos I2C a prechádzali. Teraz
je vek so znamienkom a simulované stlačenia tiež čítajú panel. Reprodukované
na stole so zbernicou I2C vrátenou na pôvodných 100 kHz: pred opravou sa
pohlo 0 z 10 krokových posuvov, po nej 10 z 10.

**Nastavenia** prežijú opätovné nahratie programu M7 aj reset; úvodný riadok
hlási `nvs=1`.

**Displej:** 25 aktualizácií za sekundu z M4, zatiaľ čo M7 posiela kroky
plnou rýchlosťou; všetky čísla vyššie boli namerané so spusteným displejom.

**Programy z USB kľúča** (`test_job.py`, jeho súbory nahraté na kľúč), všetko
prechádza:

* `short.nc` (11 riadkov): kontrolný prechod, 11 riadkov, rozsah X 0..10
  Y 0..10; beh sa pozastaví na `M0`, START pokračuje, skončí na 0,0. Znova
  spustený: bez kontrolného prechodu; znova zapísaný: znova skontrolovaný.
* `error_l7.nc`: odmietnutý v kontrolnom prechode s `error:20` v riadku 7,
  nič sa nepohlo, `$G` ukazuje G91 nastavené pred štartom.
* `toolchange.nc`: `[MSG:Tool change T2, cycle start to resume]`, Hold, START
  pokračuje, hotovo.
* Počas behu: `G0 X1` a `$X` z PC odmietnuté (`error:48`), `$I` dostane
  odpoveď, stavové hlásenie nesie `|SD:77.1,short.nc`.
* STOP 3 s po štarte `demo_complex.nc`: Idle, žiaden `ALARM:3`, a nasledujúce
  `G91 G0 X1` sa pohne presne o 1 mm ďalej od miesta, kde stál. Namiesto toho
  Ctrl-X: `ALARM:3`, úloha prerušená.
* 6000 rastrových segmentov po 0,05 mm pri F12000 (raster z `test_stream.py`):
  **1415 riadkov/s** z buffera, oproti 1282 z PC.
* Celý `demo_complex.nc`: kontrolný prechod 2658 riadkov, rozsah X 0..98
  Y 0..98 Z 0,5..5; beh **148,6 s** (152,9 s pri posielaní z PC), hotovo na
  `M30`, končí na X0 Y0.
* Z dotykovej obrazovky (`test_touch.py`): FILE, súbor vybraný, START
  ťuknutý nerobí nič, podržaný 1 s ho spustí; `M0` pozastaví a START
  v pravom stĺpci pokračuje; STOP podržaný počas `demo_complex.nc` ho
  zastaví, Idle, žiaden `ALARM:3`.

**So skutočným kľúčom** (8 GB, tak ako prišiel: tabuľka oddielov GPT,
FAT32 — ktorý vlastný FatFs v mbed nevedel pripojiť):

* Pripojený, vypísaný; `$FT=big.nc` (1,96 MB, 120006 riadkov) prečítaný za
  3,8 s = **511 kB/s** (FAT32 aj exFAT rovnako), CRC-32 zhodné so súborom
  v PC.
* `usbfiles.py put big.nc`: **14,7 s = 133 kB/s** po sériovej linke, spätne
  prečítaný a overený; dátum súboru z PC zachovaný (`[FILE:…|DATE:…]`).
  `sync` priečinka `examples/usb/`: 3 aktuálne, 2 skopírované, potom
  5 aktuálnych; `rm`, prepísanie, meno s medzerami a malými písmenami
  (`zz test short.nc`) spustené cez `$F=`.
* `demo_complex.nc` spustený z kľúča: **148,9 s** (148,6 s zo súboru v RAM,
  152,9 s z PC), 2658 riadkov, hotovo.
* Formátovanie: `$FF=ERASE` vytvorí FAT32 za 10 s, `$FF=EXFAT` exFAT za 5 s,
  FORMAT na displeji (podržaný 3 s) FAT32 za 10 s, ťuknutie nerobí nič; po
  každom sa kľúč hneď znova pripojí a celý test vyššie prejde aj na exFAT.
* Odmietnuté, ako má byť: `$FW=drawing.dxf` (nie je meno G-kódu), `$FD=`
  súbor, ktorý neexistuje, `$FF=FOO`.
* **Vytiahnutý počas behu** (`big.nc` pri F3000, stav zaznamenávaný 10× za
  sekundu): správa o zastavení, Hold 50 ms po nej, Idle 160 ms po nej, asi
  tucet riadkov vykonaných po poslednom hlásení pred vytiahnutím; poloha
  zachovaná, žiaden alarm, `$F` hlási `aborted`, `ERR:47`. Po zastrčení sa
  kľúč znova pripojí. (Prvý pokus, pred opravou, bežal ďalej cez 8 kB už
  načítaných dopredu.)
* **Windows prečíta, čo GIGA zapíše** (druhý kľúč, 16 GB): naformátovaný na
  GIGA ako FAT32 aj ako exFAT, súbory nahraté — v PC dostane písmeno
  jednotky, súbory sú zhodné bajt po bajte, `chkdsk` nič nenájde. Opačne,
  súbory skopírované v Prieskumníkovi so slovenskými menami
  (`frézka ľavá strana.nc`, `Výmena nástroja (T2).NC`) sa vypíšu v UTF-8,
  na displeji sa zobrazia bez diakritiky a dajú sa z neho spustiť.
* 8 GB kľúč najprv na testovacom PC nedostal písmeno jednotky, nech bolo na
  ňom čokoľvek — ani Správa diskov ho nevedela naformátovať („systém
  nemôže nájsť zadaný súbor“); správca pripojení Windows nemal pre to
  zariadenie záznam. Keď ho diskpart (ako správca) naformátoval a pridelil
  mu písmeno, ten istý kľúč naformátovaný na GIGA (FAT32, FORMAT na
  displeji) sa hneď ukázal ako F: a súbor zapísaný z PC sa prečítal zhodne:
  problém bol v zázname toho PC o zariadení, nie vo formáte.

* **Kľúč s NTFS** (naformátovaný tak vo Windows): `$F` hlási `cannot mount,
  error 13, the stick is NTFS`, stránka FILE ho ukáže načerveno so
  zapnutým FORMAT; FORMAT podržaný 3 s z neho spravil FAT32 za 6 s, hneď
  pripravený, súbory nahraté a overené.

**Netestované na stroji** (žiaden nie je pripojený): koncové spínače, homing
a STOP boli vyskúšané cez testovacie háčiky, ktoré ovládajú samotné vstupné
piny, nie so skutočnými spínačmi, relé, brzdou či drivermi; vyrovnanie
portálu podriadenou osou A a homing CoreXY vôbec; sonda a tlačidlá len cez
svoje piny. Reakcia na STOP je vlastné meranie firmvéru — treba ju potvrdiť
osciloskopom na shielde.

---

## Licencia

**GNU GPL v3**, ako Grbl, z ktorého je tento port odvodený — pozri
[LICENSE](LICENSE) a v [COPYING](COPYING) copyright Grbl. Pribalený kód
tretích strán, všetko pod licenciami kompatibilnými s GPLv3:

* [FatFs](http://elm-chan.org/fsw/ff/) R0.15 od ChaN v `src/m4/fatfs/` —
  licencia ChaN v štýle BSD;
* USB host stack z
  [Arduino_USBHostMbed5](https://github.com/arduino-libraries/Arduino_USBHostMbed5)
  v `src/m4/usbhost/` — Apache 2.0 / MIT;
* písma displeja, vykreslené z DejaVu Sans Mono — licencia Bitstream Vera;
* logo Grbl (`docs/Grbl Logo.svg`) z projektu Grbl.
