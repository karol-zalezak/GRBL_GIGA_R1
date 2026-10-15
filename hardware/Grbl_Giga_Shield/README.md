# Grbl GIGA Shield (návrh)

Doska periférií pre Grbl na Arduino GIGA R1 WiFi s GIGA Display Shieldom —
zadanie v [PLAN.md](../../PLAN.md), časť 5; piny v [PINOUT.md](../../PINOUT.md).

**Stav (14. 10. 2026):** koncept rozloženia, bez schémy.

| Súbor | Čo je v ňom |
|---|---|
| `scripts/mech.py` | geometria zostavy displej – GIGA – shield: obrysy, otvory, konektory, výšky. Čísla sú z Altium a STEP súborov Arduina (ASX00039, ABX00063). |
| `scripts/layout_sim.py` | simulované rozloženie blokov súčiastok → `docs/layout_sim.svg` |
| `docs/layout_sim.svg`, `.png` | pohľad spredu (od displeja) a bočný rez |

```bash
python hardware/Grbl_Giga_Shield/scripts/layout_sim.py
```

**Súradnice v `mech.py`:** pohľad spredu, začiatok v ľavom hornom rohu displeja, x
doprava, y nadol, mm. Pre DPS shieldu (strana súčiastok, pohľad zozadu) sa x zrkadlí:
`x_pcb = 106 − x`.

**Treba zmerať na skutočnej zostave:** medzeru medzi zadnou stranou displeja a
spodnou stranou GIGA (zo STEP vychádza ≈ 12,6 mm). Od nej závisí dĺžka stĺpikov M3
(≈ 25 mm) a dĺžka kolíkov do konektora kamery J3 (≈ 24 mm pod shieldom).
