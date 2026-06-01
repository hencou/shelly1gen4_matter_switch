# Installatie- en build-gids — `shelly1gen4_matter_switch`

⚠️ Eerste build duurt **20–45 minuten** omdat connectedhomeip moet compileren. Daarna is incrementele build met ccache ~1-3 min.

> Voor Windows-gebruikers: gebruik [`INSTALL_VSCODE_WINDOWS.md`](INSTALL_VSCODE_WINDOWS.md) (WSL2 + VS Code is de officieel aanbevolen route). Native Windows wordt niet ondersteund door esp-matter.

## 1. ESP-IDF v5.2.2

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive -b v5.2.2 https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c6
. ./export.sh           # elke nieuwe shell-sessie
```

## 2. esp-matter clonen (eenmalig, ~3 GB met submodules)

```bash
cd ~/esp
# release/v1.4 is de bevestigd-werkende branch voor dit project
git clone --depth 1 -b release/v1.4 https://github.com/espressif/esp-matter.git
cd esp-matter
git submodule update --init --depth 1

# Platform-specifieke submodules (esp32 + linux voor chip-tool)
cd connectedhomeip/connectedhomeip
./scripts/checkout_submodules.py --platform esp32 linux --shallow
cd ~/esp/esp-matter

./install.sh
. ./export.sh           # zet $ESP_MATTER_PATH + voegt chip-tool aan PATH toe
```

⚠️ Als `install.sh` faalt met `mobly` / `ResolutionImpossible`:
```bash
rm -rf connectedhomeip/connectedhomeip/.environment
./install.sh
```

⚠️ De `connectedhomeip/` submodule alleen is ~2 GB. Zorg voor genoeg vrije ruimte (>5 GB voor build artifacts).

## 3. Project bouwen

```bash
# Ccache aanzetten voor snellere herhaalbuilds
export IDF_CCACHE_ENABLE=1

# Beide environments laden (elke shell-sessie)
. ~/esp/esp-idf/export.sh
. ~/esp/esp-matter/export.sh

cd ~/projects/shelly1gen4_matter_switch
idf.py set-target esp32c6
idf.py menuconfig     # pas GPIO's aan indien nodig
idf.py build          # eerste keer 20-45 min, daarna 1-3 min
```

Tip: zet beide `source ...export.sh` regels + `IDF_CCACHE_ENABLE=1` in je `~/.bashrc` zodat ze automatisch geladen worden in elke nieuwe terminal.

Voor bekende build-issues (CHIP_HAVE_CONFIG_H, MATTER_OVER_THREAD, API drift in v1.4 etc.), zie **sectie 15 in [`INSTALL_VSCODE_WINDOWS.md`](INSTALL_VSCODE_WINDOWS.md#15-bekende-build-issues-met-esp-matter-releasev14)**.

In `menuconfig`:
- **Shelly 1 Gen4 Matter Switch configuration** → GPIO's
- **Component config → CHIP Device Layer** → laat default (Thread enabled, BLE-pairing enabled)
- **Partition Table → Custom** → al goed via `sdkconfig.defaults`

Verwachte output: `build/shelly1gen4_matter_switch.bin` (~1.6-1.8 MB).

## 4. UART-flashen via de J6-connector op de achterkant

De Shelly 1 Gen4 heeft op de achterkant **7 gaatjes op een rij** — dit is de **J6-connector** (silkscreen-label `J6`) van het geïntegreerde ESP32-C6 module. Dit is hetzelfde rijtje pinnen waar een Shelly Plus Add-on op aansluit, maar het kan ook gebruikt worden om de ESP32-C6 te flashen.

OTA flashen vanuit de originele Shelly firmware werkt **niet** — Shelly verifieert OTA-images met een ECDSA-signature. De eerste flash moet dus via UART.

> **Bron**: deze sectie volgt de canonieke pinout uit
> [`automatous-io/shelly-1-gen4-matter-thread` FLASHING.md](https://github.com/automatous-io/shelly-1-gen4-matter-thread/blob/main/docs/FLASHING.md)
> — geverifieerd op hardware-revisie `v0.1.2` (op de PCB gedrukt).

### J6 Pinout

Nummering loopt **van het pin het verst van het J6-label (pin 1) naar het pin direct naast het label (pin 7 = GND)**.

| Pin | Functie | GPIO | Toelichting |
|---|---|---|---|
| 1 | **ESP_DBG_UART** | GPIO18 (Digital IN) | niet gebruikt voor flashen |
| 2 | **TXD** | GPIO16 (UART0 TX // 1-Wire RX) | Shelly TXD → CP2102 RXD |
| 3 | **RXD** | GPIO17 (UART0 RX // Analog IN) | Shelly RXD ← CP2102 TXD |
| 4 | **3.3V** | -- | voeding (alleen 3.3V — **géén 5V**) |
| 5 | **RESET** | EN | EN — niet nodig voor handmatig flashen |
| 6 | **GPIO0 (BOOT)** | GPIO0 | low bij power-up → flash mode |
| 7 | **GND** | -- | massa — pin direct naast het `J6` silkscreen |

⚠️ **Pin 7 = GND is je oriëntatie-anker**: zet een multimeter op continuïteit, zoek de pin die piept tegen de metalen shield van het ESP-module of een bekende GND-traceringspad — dat is pin 7. Tel vandaar terug naar pin 1 in tegengestelde richting.

### Wat je nodig hebt

- **CP2102 USB-UART adapter** met aparte 3.3V output (deze guide is op CP2102 getest). FTDI FT232RL / CH340G werken ook mits ze 3.3V kunnen leveren en op 3.3V logic-level draaien.
- 1.27 mm 7-pin naar 2.54 mm Dupont kabel of adapter-board (de J6-gaatjes zijn 1.27 mm pitch). Vaste-kern Cat 5e/6 draadjes passen in nood ook in de vierkante gaatjes, maar lastig vast te houden.
- Een korte jumper-wire of drukknop voor de **Pin 6 ↔ Pin 7 (GPIO0 ↔ GND)** brug tijdens flash-mode entry.
- Een Chromium-browser (Chrome / Edge) voor de ESPConnect web-flasher, óf `esptool.py` / `idf.py` via CLI.

### ⚠️ Veiligheid — eerst lezen

- **Nooit 230V op de Shelly aansluiten terwijl je flasht.** De programming-header is **niet** galvanisch geïsoleerd van het relais-circuit. Tegelijk USB-UART + mains = elektrocutie-risico voor jou, kapotte Shelly, kapotte USB-poort, kapotte laptop. Trek de Shelly volledig uit de wandcontactdoos / inbouwdoos vóórdat je iets aansluit.
- **Nooit 5V op pin 4.** De pin is direct gekoppeld aan de 3.3V rail van de ESP32-C6. 5V vernielt het module.
- **Bench-test note**: bij voeding via pin 4 (3.3V vanuit USB-UART) klikt het relais **niet** hoorbaar — de relais-spoel heeft 230V nodig. Firmware, commissioning, GPIO, LED en Matter-pairing werken wel volledig. Hoor je geen klik op een short-press: dat is normaal in bench mode, geen bug.

### Bedrading

USB-UART **nog niet in de PC** tijdens bekabelen. Sluit zo aan:

```
CP2102 pin  ←→  Shelly J6
──────────────────────────────────
GND         ─→  Pin 7  (GND, direct naast J6-label)
3.3V        ─→  Pin 4  (3.3V — NIET 5V)
RXD         ─→  Pin 2  (Shelly TXD)
TXD         ─→  Pin 3  (Shelly RXD)

Niet aansluiten op CP2102:  5V, DTR
Niet aansluiten op Shelly:  Pin 1 (DBG), Pin 5 (RESET)

Voor flash mode (extra):
GND         ─→  Pin 6  (GPIO0 BOOT, jumper of drukknop)
```

TX/RX kruisen is in de tabel hierboven al verwerkt — CP2102 **RXD** gaat naar Shelly **TXD** (pin 2), CP2102 **TXD** naar Shelly **RXD** (pin 3).

### Procedure — flash mode betreden

De Shelly boot in ROM-bootloader wanneer GPIO0 (pin 6) laag is bij power-up.

1. Bekabeling aansluiten (USB-UART nog niet in PC).
2. **Brug Pin 6 (GPIO0) en Pin 7 (GND)** — jumper of drukknop ingedrukt.
3. **USB-UART in PC pluggen** → Shelly krijgt 3.3V via pin 4, boot direct in flash mode.
4. Brug kan blijven zitten tijdens flashen, of na ~1 seconde loslaten — beide werken zodra de bootloader draait.
5. **Na succesvol flashen**: brug verwijderen vóór de volgende power-on, anders boot Shelly weer in flash mode in plaats van je firmware.

### WSL2-specifiek — USB-UART doorzetten

Vanuit PowerShell als admin:

```powershell
usbipd list                       # noteer BUSID van de CP2102
usbipd bind --busid <BUSID>       # eenmalig per adapter
usbipd attach --wsl --busid <BUSID>
```

Daarna verschijnt het device in WSL als `/dev/ttyUSB0` (of `/dev/ttyACM0` voor sommige chipsets — check met `ls /dev/tty*`).

## 5. Stock firmware backuppen + flashen

Twee routes — kies één.

> ⚠️ **Belangrijk over flashen via J6**: de J6-connector heeft **geen DTR/RTS bedrading**, dus esptool's auto-reset werkt niet. Bij `idf.py flash` (Route B) moet je **vóór elk commando** opnieuw flash mode betreden door Pin 6 ↔ Pin 7 te bruggen en de USB-UART te her-pluggen. Dat is omslachtig. **Route A (merged binary + ESPConnect) doet alles in één flash-actie en wordt aanbevolen.**

### Route A — Merged binary + ESPConnect (browser, één-shot flash)

Onze build produceert 4 aparte binaries die elk op een eigen offset moeten staan (bootloader 0x0, partition-table 0x8000, otadata 0xf000, app 0x20000). ESPConnect kan alleen één bestand op één offset schrijven, dus we mergen eerst.

**Stap 1 — merged bin bouwen (eenmalig per build):**

```bash
. ~/esp/esp-idf/export.sh
cd ~/projects/shelly1gen4_matter_switch
idf.py build      # zorgt dat alle 4 binaries up-to-date zijn

esptool.py --chip esp32c6 merge_bin \
    -o shelly1gen4_matter_switch_merged.bin \
    --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x0      build/bootloader/bootloader.bin \
    0x8000   build/partition_table/partition-table.bin \
    0xf000   build/ota_data_initial.bin \
    0x20000  build/shelly1gen4_matter_switch.bin

# Kopieer naar Windows-zijde voor ESPConnect (vervang <USERNAME>)
cp shelly1gen4_matter_switch_merged.bin /mnt/c/Users/<USERNAME>/Downloads/
```

Tip: zet die laatste drie commando's in een scriptje `tools/make-merged.sh` voor latere iteraties.

**Stap 2 — flashen via ESPConnect:**

1. **USB-UART uit** de PC, **Pin 6 ↔ Pin 7 (GPIO0 ↔ GND) bruggen**, USB-UART weer pluggen → Shelly in ROM bootloader. Brug mag nu blijven of weg, allebei werkt.
2. Open [ESPConnect](https://thelastoutpostworkshop.github.io/microcontroller_devkit/espconnect/) in Chrome / Edge.
3. **Connect** → kies CP2102 seriële poort. Baudrate **115200** (lager = stabieler bij grote write).
4. ESPConnect toont chip-info incl. **MAC-adres** — noteer voor backup-naming.
5. **Optioneel maar aanbevolen**: **Flash Tools → Download Flash Backup** → 8 MB stock-image (`shelly-1-gen4-stock-AABBCCDDEEFF.bin`). Duurt 5–10 min. Verifieer bestand ≈ 8 388 608 bytes. Bewaar veilig — enige weg terug naar factory.
6. **Flash Tools → Flash Firmware**:
   - Bestand: `shelly1gen4_matter_switch_merged.bin`
   - Offset: `0x0`
   - **Erase entire flash before writing**: aan
   - Klik **Flash**. ~1-2 min.
7. **Disconnect** in ESPConnect.
8. USB-UART uit, **brug Pin 6 ↔ Pin 7 verwijderen** (kritisch — anders boot Shelly weer in flash mode), USB-UART weer pluggen → Shelly draait nu jouw Matter firmware en opent BLE-pairing.

### Route B — esptool / `idf.py` (CLI, voor ontwikkelaars)

Vergt opnieuw flash mode betreden tussen **elke** esptool-call (erase, flash, etc.) omdat DTR/RTS niet bedraad is op J6.

```bash
# Backup stock (eenmalig — bewaar veilig!)
# Stap voor: brug Pin 6 ↔ Pin 7, USB her-pluggen
esptool.py --chip esp32c6 -p /dev/ttyUSB0 --baud 460800 \
    --before no_reset --after no_reset \
    read_flash 0x0 0x800000 shelly_stock_backup.bin

# Erase + flash Matter firmware
# Stap voor: brug Pin 6 ↔ Pin 7, USB her-pluggen
idf.py -p /dev/ttyUSB0 erase-flash

# Stap voor: brug Pin 6 ↔ Pin 7, USB her-pluggen
idf.py -p /dev/ttyUSB0 flash

# Monitor (brug eraf, USB her-pluggen)
idf.py -p /dev/ttyUSB0 monitor
```

Terugzetten naar stock kan altijd later met (ook met handmatige flash-mode entry):

```bash
esptool.py --chip esp32c6 -p /dev/ttyUSB0 write_flash 0x0 shelly_stock_backup.bin
```

Na flashen: **brug Pin 6 ↔ Pin 7 verwijderen**, daarna power-cycle. Anders boot Shelly weer in flash mode.

In de log zou je ongeveer dit moeten zien:
```
I (... ) matter_dev: EP1 = OnOff Light Switch (drukker)
I (... ) matter_dev: EP2 = Temperature Sensor
I (... ) matter_dev: EP3 = Occupancy Sensor (LD2410)
I (... ) matter_dev: EP4 = OnOff Light (relais)
I (... ) matter_dev: EP5 = OnOff Light Switch (state-follow)
I (... ) chip[DL]: Device Configuration:
I (... ) chip[DL]:   Setup Pin Code: 20202021
I (... ) chip[DL]:   Setup Discriminator: 3840 (0xF00)
I (... ) chip[DL]:   Manual Pairing Code: [...]
I (... ) chip[DL]:   QR Code: [...]
```

Noteer de **QR Code** of **Manual Pairing Code** — daarmee voeg je 'm toe aan HA.

## 6. Thread-netwerk voorbereiden

Je hebt al een Google TV Streamer 4K = werkende TBR (Google Home fabric).
Voor HA Matter Server moet HA óók aan datzelfde Thread-netwerk gekoppeld zijn:

**Optie A — Shared Thread Credentials**
HA Matter Server pakt automatisch Thread-credentials van Google TV via de Apple/Google/HA Thread Credentials API (Android 14+ / iOS 16.4+).

**Optie B — HA Connect ZBT-2 als secondary TBR**
Plug een ZBT-2 in de HA-host → HA wordt secondary TBR → joint hetzelfde Thread-mesh.

Check in HA → Settings → Devices & Services → Thread → je zou minstens één dataset moeten zien.

## 7. Pairing in HA Matter Server

1. HA → Settings → Devices & Services → "Add Integration" → Matter Server → "Add device".
2. Scan de QR-code uit de UART-log of voer de Manual Pairing Code in.
3. Wacht 30-90 s. Hij zou via BLE pairen, Thread-credentials krijgen, Thread joinen, en daarna verschijnen als "Shelly 1 Gen4 Matter Switch" met 4 entities (switch, temp, occupancy, relais).

Als pairing faalt:
- Check dat de Shelly minder dan 5 minuten geleden geboot is (BLE-pairing-window).
- Reset met **6× snel klikken** op de drukker → device wist Matter NVS en heropent het pairing-window.

## 8. Pair een KAJPLATS bulb in Matter-mode

KAJPLATS is dual-stack (Zigbee + Matter-over-Thread). Voor Matter:

1. Schroef de bulb in, lamp gaat aan.
2. Op de IKEA-doos staat een QR-code voor Matter setup, of gebruik default code op IKEA's website.
3. HA → Matter → Add device → scan QR.
4. Bulb joint Thread, verschijnt in HA als KAJPLATS light.

Noteer het Matter node-ID van de bulb (zichtbaar in HA device-info).

## 9. Binding opzetten met chip-tool

Vanaf je laptop:

```bash
. ~/esp/esp-matter/export.sh    # voegt chip-tool aan PATH toe

# Vervang met je eigen node-IDs (zie HA device-info)
SWITCH=0x0123456789ABCDEF       # Shelly node-ID
BULB=0x00112233AABBCCDD         # KAJPLATS node-ID

# Bind drukker (EP1) aan de bulb (EP1, OnOff + LevelControl)
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$BULB',"endpoint":1,"cluster":6},
    {"fabricIndex":1,"node":'$BULB',"endpoint":1,"cluster":8}]' \
  $SWITCH 1


```

Druk op de drukker → de KAJPLATS bulb zou direct moeten reageren, **zonder HA in het pad**.

### Optioneel: relais mee-schakelen via binding

Het lokale relais (EP4) is een apart server endpoint en reageert standaard alleen op commando's vanuit HA. Om het relais mee te laten schakelen bij een knopdruk, voeg EP4 toe aan de binding:

```bash
# Bind drukker (EP1) aan zowel de bulb ALS het lokale relais (EP4)
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$BULB',"endpoint":1,"cluster":6},
    {"fabricIndex":1,"node":'$BULB',"endpoint":1,"cluster":8},
    {"fabricIndex":1,"node":'$SWITCH',"endpoint":4,"cluster":6}]' \
  $SWITCH 1
```

### Optioneel: vaste schakelaar (EP5, state-follow)

EP5 is een tweede switch-endpoint dat On/Off stuurt op basis van de schakelaarstand (i.p.v. Toggle). Gebruik EP5 voor vaste of wissel-schakelaars:

```bash
# Bind vaste schakelaar (EP5) aan de bulb — lamp volgt schakelaarstand
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$BULB',"endpoint":1,"cluster":6}]' \
  $SWITCH 5

# Of bind EP5 aan het lokale relais (EP4)
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$SWITCH',"endpoint":4,"cluster":6}]' \
  $SWITCH 5
```

> EP1 (Toggle) en EP5 (State-follow) werken naast elkaar. Bind het endpoint dat past bij je schakelaartype.

## 10. OTA — firmware-updates zonder UART

1. Bouw nieuwe firmware: `idf.py build`. Plaats `build/shelly1gen4_matter_switch.bin` in HA `/config/www/`.
2. Klik **6× snel** op de System 55 drukker.
3. Eerste keer: telefoon → SoftAP `shelly-ota-XXXXXX` → `http://192.168.4.1/` → SSID + password + URL invullen.
4. Volgende keren: 6× klikken volstaat (config blijft bewaard in NVS).

## 11. Rollback naar stock Shelly firmware

```bash
esptool.py -p /dev/ttyUSB0 write_flash 0x0 shelly_stock_backup.bin
```

## 12. Troubleshooting

| Symptoom | Oorzaak / oplossing |
|---|---|
| Build faalt met "esp_matter.h not found" | `$ESP_MATTER_PATH` niet gezet. Run `. ~/esp/esp-matter/export.sh`. |
| Build OOM (out of memory) | connectedhomeip is fors. Min 8 GB RAM aanbevolen, of `idf.py -j2 build` voor sequentiëler. |
| Apparaat verschijnt niet in HA na flash | Check BLE-pairing window: log moet "BLE advertising started" tonen. Reboot apparaat om window opnieuw te openen. |
| Thread joinen lukt niet | Geen actieve Thread-credentials in fabric. Check HA → Thread → zie of dataset bestaat. |
| Binding command werkt niet | Check `chip-tool binding read binding $SWITCH 1` — zou je nieuwe entries moeten tonen. Bulb moet ACL hebben die switch toestaat te schrijven (HA Matter Server doet dit automatisch tijdens commissioning). |
| Lamp reageert traag (>1s) | Thread-mesh kan herstellen via reboot Google TV Streamer; check `chip-tool thread show-state $SWITCH 0`. |
| Crash bij boot na OTA | Bootloader-rollback aktief (zie sdkconfig). Hij valt automatisch terug naar vorige slot. Check je build. |
| Long-press dimmen werkt niet | Bind moet **cluster 8** (LevelControl) bevatten, niet alleen cluster 6. |
| OTA SoftAP verschijnt niet | Check dat 10× klikken binnen ~3s gedaan wordt. Anders houdt de drukker `BTN_EVT_LONG_PRESS` aan. Reset device en probeer opnieuw. |
| `esptool` timeout op `/dev/ttyUSB0` | Pin 6 (GPIO0) was niet laag bij power-on. Brug Pin 6 ↔ Pin 7 (GND) opnieuw aanleggen en USB-UART her-pluggen. Zie hoofdstuk 4. |
| Shelly boot na flash weer in flash mode | Brug Pin 6 ↔ Pin 7 zit er nog. Verwijder de jumper en power-cycle. |
| 5V per ongeluk op Pin 4 gezet | Module is hoogstwaarschijnlijk dood. Geen rescue mogelijk — alleen vervangen. Gebruik altijd 3.3V. |
