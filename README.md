# Geiger ESP32 – PlatformIO projekt

Firmware nahrádza pôvodné LCD a tlačidlá webovou stránkou obsluhovanou cez `ESPAsyncWebServer`. Stránka zobrazuje okamžité CPM, µSv/h, priemer od vynulovania, celkový počet impulzov a počet/CPM za posledný interval. Hodnoty sa obnovujú raz za sekundu a navyše po každom započítanom impulze. Kalibračný faktor, dĺžku intervalu a aktívnu hranu impulzu možno meniť cez stránku; nastavenia sa ukladajú v NVS. Nulovanie začne nové meranie.

## Meranie a CSV

Tlačidlo **Spustiť meranie** začne nové meranie. CSV dostane jeden dátový riadok až po každom nastavenom intervale; počet impulzov, CPM a dávkový príkon sa vypočítajú z impulzov zachytených počas tohto intervalu. Po zastavení zhasne LED a meranie možno znovu spustiť; do toho istého CSV sa vloží prázdny riadok, nový dátum/čas v jednej bunke a hlavička. **Uložiť do LittleFS** dokončí CSV. Uložené súbory sa zobrazia v zozname a dajú sa stiahnuť alebo zmazať jednotlivo či hromadne. Označené súbory možno stiahnuť spolu v ZIP archíve. Tlačidlo **Stiahnuť posledný CSV** stiahne práve rozpracovaný záznam aj pred uložením, inak najnovší uložený súbor. Názov má formát `YYYY-MM-DD_HH-MM-SS.csv` a dátum/čas prevezme automaticky z prehliadača na zariadení používateľa pri spustení merania. Súbor používa UTF-8 s BOM, aby sa diakritika správne zobrazila aj v tabuľkových editoroch. Pri kolízii názvu sa pridá prípona, napríklad `_01`. Stĺpce CSV sú `čas (h:m:s)`, `C/interval`, `CPM` a `Dávkový príkon (µSv/h)`. Čas v stĺpci je uplynutý čas od spustenia príslušného merania, pretože ESP32 nemá samostatne nastavené hodiny.

V nastaveniach možno zvoliť oddeľovač stĺpcov `;` (predvolený) alebo `,` a desatinný oddeľovač `.` (predvolený) alebo `,`. Zvolená dvojica sa uloží v ESP32 a začne sa používať pri nasledujúcom meraní. Oddeľovač stĺpcov a desatinný oddeľovač musia byť rozdielne.

Tlačidlo **Formátovať LittleFS** vymaže všetky uložené súbory aj rozpracovaný záznam. Web pred formátovaním zobrazí potvrdzovaciu otázku.

LittleFS sa pripája bez automatického formátovania, aby sa pri chybe pripojenia nevymazali existujúce súbory. Ak pripojenie zlyhá (napríklad chyba `-84`), použi na stránke **Formátovať LittleFS**; tlačidlo zostáva dostupné aj v tomto stave. Formátovanie odstráni všetky súbory. Ak obnova zlyhá, skontroluj partition scheme dosky v PlatformIO.

## Zapojenie

- Výstup impulzov Geigerovho modulu: GPIO 13 (`GM_PIN` v `src/main.cpp`).
- LED indikácia impulzov: GPIO 2 (`LED_PIN`).
- Spoločná zem medzi modulom a ESP32.
- Výstup impulzov musí byť kompatibilný s 3,3 V ESP32. Ak modul dáva vyššie napätie, použite vhodné prispôsobenie úrovne.

GPIO 13 (D13) je vstup impulzov. V nastavení impulzov zvoľte `FALLING` pre zostupnú hranu (pôvodné správanie) alebo `RISING` pre vzostupnú hranu. Ak sa impulzy nezapočítavajú, skontrolujte označenie GPIO na doske, spoločnú zem a úroveň signálu.

## Zostavenie a nahratie

1. Nainštalujte PlatformIO IDE (napríklad rozšírenie PlatformIO pre VS Code).
2. Otvorte priečinok `ESP32_Geiger_Web` ako PlatformIO projekt.
3. Pripojte dosku ESP32 Dev Module cez USB. Projekt používa `board = esp32dev`; pre inú dosku zmeňte hodnotu `board` v `platformio.ini`.
4. Zostavte a nahrajte projekt cez **Build** a **Upload**. PlatformIO stiahne knižnice z `lib_deps` automaticky.
5. Voliteľne otvorte Serial Monitor na 115200 baud; vypíše adresu webového panela.

Z príkazového riadka v priečinku projektu:

```sh
pio run
pio run --target upload
pio device monitor
```

## Webový panel

ESP32 vytvorí vlastný Wi‑Fi prístupový bod, preto nie je potrebný router ani nastavenie Wi‑Fi v kóde:

- Sieť: `Geiger-ESP32`
- Heslo: `geiger1234`
- Adresa: `http://192.168.4.1`

Zmeňte prihlasovacie údaje `AP_SSID` a `AP_PASSWORD` v `src/main.cpp` pred nasadením.
