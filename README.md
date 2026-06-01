# shelly1gen4_matter_switch

**Custom Matter-over-Thread firmware** voor de **Shelly 1 Gen4** (ESP32-C6).

1. **Matter OnOff/Dimmer Light Switch** (Toggle, EP1) — bind-client, kort drukken = toggle, lang = dimmen
2. **Matter Temperature Sensor** (EP2) — DS18B20 via dual-pin 1-Wire: TX=GPIO9, RX=GPIO16 (Shelly Plus Add-on)
3. **Matter Occupancy Sensor** (EP3) — HLK-LD2410 op GPIO17 (Data ingang Addon)
4. **Matter OnOff Light** (Relais op GPIO5, EP4) — server endpoint, direct aanstuurbaar vanuit HA
5. **Matter OnOff Light Switch** (State-follow, EP5) — bind-client, On bij contact-sluiting, Off bij -opening

Alle 5 endpoints zijn altijd actief — geen compile-time keuze nodig. Universele firmware voor alle configuraties.
EP1 (Toggle) en EP5 (State-follow) bedienen dezelfde fysieke inputs — de gebruiker kiest via binding welk endpoint zijn lamp/relais aanstuurt:
- **Momentknop** (drukker) → bind EP1
- **Vaste/wissel-schakelaar** → bind EP5

Drukker→lamp en touch→lamp werken **standalone** na binding-setup: HA Matter Server, Google TV Streamer, mag offline — direct Thread-mesh-multicast naar de gebonden bulbs.

Het relais (EP4) is **niet meer hardcoded gekoppeld** aan de knopdruk. Via een Matter binding van EP1 naar EP4 kun je het relais optioneel mee laten schakelen. Zonder binding reageert het relais alleen op commando's vanuit HA.

## Gebaseerd op

- [esp-matter](https://github.com/espressif/esp-matter) `release/v1.4` (Espressif's officiële Matter SDK — bevestigd-werkende branch voor dit project)
- [connectedhomeip](https://github.com/project-chip/connectedhomeip) (als submodule binnen esp-matter)
- ESP-IDF v5.2.2 (vereist voor stabiele esp-matter v1.4)
- Reference: esp-matter `examples/light_switch` voor het Binding-pattern + CMakeLists.txt patroon (`CHIP_HAVE_CONFIG_H` + `-std=gnu++17`)

## Doel-hardware en netwerk

| Component | Status |
|---|---|
| Shelly 1 Gen4 (ESP32-C6, 8 MB flash) | Doel-hardware |
| Shelly Plus Add-on | DS18B20 (TX=GPIO9/RX=GPIO16) + TTP223 touch (GPIO18) + LD2410 radar (GPIO17) |
| Thread Border Router | **Google TV Streamer 4K** |
| Matter primary admin | **Home Assistant Matter Server** add-on |
| Matter-Thread bulb | **IKEA KAJPLATS** (Thread-mode via setup-code) |
| Commissioning tool | HA Matter Server UI of `chip-tool` commandline |
| Binding setup tool | `HA Python Matter Server` |

## Pin-mapping

**Onboard Shelly 1 Gen4** pinnen:

| GPIO | Functie | Bron |
|---|---|---|
| **GPIO4** | PCB-knop (active-low) | Shelly 1 Gen4 onboard |
| **GPIO5** | Relais-uitgang (active-high) | Shelly 1 Gen4 onboard |
| **GPIO10** | Drukker-input / SW-terminal | Shelly 1 Gen4 onboard — **externe pull op PCB, geen interne pull** |
| **GPIO15** | Status LED onboard (active-low) | Shelly 1 Gen4 onboard PCB-LED  |

**Shelly Plus Add-on** pinnen (via J6 connector):

De Add-on gebruikt een **ISO7221A galvanische isolator** die het 1-Wire protocol opsplitst in aparte TX en RX lijnen.

| GPIO | Functie | Status |
|---|---|---|
| **GPIO9** | 1-Wire TX (data out) — DS18B20 commando's via isolator | Altijd actief |
| **GPIO16** | 1-Wire RX (data in) — DS18B20 antwoorden via isolator | Altijd actief |
| **GPIO17** | HLK-LD2410 mmWave occupancy sensor (EP3 Occupancy) | Altijd actief |
| **GPIO18** | TTP223 capacitive touch button (stuurt via EP1) | Altijd actief |

Alle pinnen zijn aanpasbaar via `idf.py menuconfig` → **"Shelly 1 Gen4 Matter Switch configuration"**.

Status-LED-patronen (`status_led.c`):
- **Snelle knipper (5 Hz)** tijdens boot/initialisatie
- **Langzame knipper (1 Hz)** tijdens commissioning (BLE-pairing window)
- **Korte flits** bij elke short-press

## Matter device-structuur

| Endpoint | Device type | Server clusters | Client clusters | Doel |
|---|---|---|---|---|
| **EP 1** | 0x0103 OnOff Light Switch | Descriptor, Binding | OnOff, LevelControl | Momentknop → Toggle → lamp |
| **EP 2** | 0x0302 Temperature Sensor | TemperatureMeasurement | — | DS18B20 report |
| **EP 3** | 0x0107 Occupancy Sensor | OccupancySensing | — | LD2410 binary |
| **EP 4** | 0x0100 OnOff Light | OnOff | — | Relais GPIO5 — aanstuurbaar vanuit HA |
| **EP 5** | 0x0103 OnOff Light Switch | Descriptor, Binding | OnOff | Vaste schakelaar → On/Off → lamp |

EP1 en EP5 hebben elk een **Binding cluster** (server). De Binding-tabel wordt door HA Matter Server of `chip-tool` ingevuld met:
- Unicast-binding (1 specifiek bulb, identified by node-ID + endpoint)
- Multicast-binding (1 group-ID — alle bulbs in die group reageren tegelijk)

Bij elke knop-event stuurt `matter_device.cpp` commando's direct via `FindOrEstablishSession` + `InvokeCommandRequest` naar elke entry in de Binding-tabel.

**EP1 vs EP5:** Beide endpoints worden door dezelfde fysieke inputs aangestuurd. EP1 stuurt Toggle (voor momentknoppen), EP5 stuurt On/Off (voor vaste schakelaars die een stand vasthouden). Bind het endpoint dat past bij je schakelaartype.

**EP4 (relais)** is een server endpoint — HA ziet dit als een schakelaar die je direct kunt aan/uitzetten. Om het relais mee te laten schakelen met de drukker, maak een binding van EP1 of EP5 naar EP4 (zie binding-setup hieronder).

## Gebruikers-interactie

Alle 3 inputs doen **precies hetzelfde** — ze sturen via EP1 (Toggle) én EP5 (State-follow):

| GPIO | Input | Omschrijving |
|---|---|---|
| **GPIO10** | System 55 drukker | 230V impulsdrukker op de SW-terminal (via optocoupler) |
| **GPIO18** | TTP223 touch | Capacitieve touch-knop op Shelly Plus Add-on digital input |
| **GPIO4** | PCB-knop | Onboard knopje op de Shelly 1 Gen4 printplaat |

| Actie | Effect |
|---|---|
| Kort drukken (< 500 ms) | Matter `OnOff.Toggle` naar EP1 binding-entries |
| Contact sluiten | Matter `OnOff.On` naar EP5 binding-entries (state-follow) |
| Contact openen | Matter `OnOff.Off` naar EP5 binding-entries (state-follow) |
| Lang drukken (> 500 ms) | `LevelControl.Move` (up/down, alternerend) via EP1 |
| Loslaten | `LevelControl.Stop` via EP1 |
| 6× snel (< 2,5 s) | **Mode toggle** — in Matter-mode: reboot naar OTA-mode; in OTA-mode: factory reset (wipe nvs + chip_kvs) |

> ℹ️ Alle 3 inputs lopen via dezelfde callback in `app_main.cpp`. EP1 (Toggle) en EP5 (State-follow) hebben elk een eigen Binding-tabel. Bind EP1 voor momentknoppen, EP5 voor vaste schakelaars — of beide als je zowel toggle als state-follow wilt.

## Commissioning in Home Assistant Matter Server

1. Flash de firmware (zie INSTALL.md). Bij eerste boot adverteert het apparaat 5 minuten lang via BLE en SRP/DNS-SD.
2. Open Home Assistant → Settings → Devices & Services → Matter → "Add device".
3. Voer setup-code in: **20202021** (default test passcode, configureerbaar in `sdkconfig.defaults`).
4. HA Matter Server pairt via BLE, provisioneert Thread-credentials (vraag aan Google TV Streamer als TBR), het apparaat join het Thread-netwerk.
5. Na ~30-60 s verschijnt het apparaat in HA met 5 entities: switch toggle (EP1), switch state-follow (EP5), temperature sensor, occupancy sensor, relais (light).

⚠️ **Het Thread-netwerk moet al bestaan** — Google TV Streamer is je TBR. Als HA Matter Server zelf nog niet aan datzelfde Thread-netwerk gekoppeld is, gebruik dan HA Connect ZBT-2 of een vergelijkbare dongle als secondary TBR (delen automatisch het Thread-credentialset via de Thread-credentials API).

## Binding-setup met chip-tool

Op je laptop (chip-tool wordt meegeleverd door esp-matter, of bouw eigen):

```bash
# Step 0: stel je node-IDs vast
# Shelly switch node-ID (zie HA): bv. 0x0123456789ABCDEF
# Bulb (KAJPLATS) node-ID:        bv. 0x00112233AABBCCDD
SWITCH_NODE=0x0123456789ABCDEF
BULB_NODE=0x00112233AABBCCDD
SWITCH_EP=1   # drukker = EP1
BULB_EP=1     # KAJPLATS light = EP1

# Step 1: bind drukker (EP1) aan de bulb
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$BULB_NODE',"endpoint":'$BULB_EP',"cluster":6},
    {"fabricIndex":1,"node":'$BULB_NODE',"endpoint":'$BULB_EP',"cluster":8}]' \
  $SWITCH_NODE $SWITCH_EP

```

`cluster:6` = OnOff, `cluster:8` = LevelControl.

### Drukker → relais koppelen (optioneel)

Om het lokale relais mee te laten schakelen bij een knopdruk, voeg een binding toe van EP1 naar EP4 (het relais endpoint op dezelfde Shelly):

```bash
RELAY_EP=4    # relais = EP4

# Bind drukker (EP1) aan zowel de bulb ALS het lokale relais
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$BULB_NODE',"endpoint":'$BULB_EP',"cluster":6},
    {"fabricIndex":1,"node":'$BULB_NODE',"endpoint":'$BULB_EP',"cluster":8},
    {"fabricIndex":1,"node":'$SWITCH_NODE',"endpoint":'$RELAY_EP',"cluster":6}]' \
  $SWITCH_NODE $SWITCH_EP
```

Zonder deze binding reageert het relais alleen op commando's vanuit HA (of andere controllers).

### Vaste schakelaar → lamp/relais (EP5, state-follow)

Voor een vaste of wissel-schakelaar die een stand vasthoudt (maintained contact), bind EP5 in plaats van EP1. EP5 stuurt On bij sluiting en Off bij opening:

```bash
STATE_EP=5    # state-follow = EP5

# Bind vaste schakelaar (EP5) aan de bulb — lamp volgt schakelaarstand
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$BULB_NODE',"endpoint":'$BULB_EP',"cluster":6}]' \
  $SWITCH_NODE $STATE_EP

# Of bind EP5 aan het lokale relais — relais volgt schakelaarstand
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$SWITCH_NODE',"endpoint":4,"cluster":6}]' \
  $SWITCH_NODE $STATE_EP
```

> ℹ️ EP1 en EP5 kunnen naast elkaar bestaan. Je kunt EP1 binden aan een lamp (toggle) en EP5 aan het relais (state-follow), of andersom.

### Many-to-many: groups gebruiken

```bash
# Step A: maak een group op de switch + bulb (groupId 0x0001)
GROUP=0x0001

# Add group on bulb (server)
chip-tool groups add-group $GROUP "Woonkamer" $BULB_NODE 0
chip-tool groups write current-group $GROUP $BULB_NODE 0   # optioneel

# Add group on switch (client)
chip-tool groups add-group $GROUP "Woonkamer" $SWITCH_NODE 1

# Step B: bind switch EP1 aan de group (i.p.v. unicast naar bulb)
chip-tool binding write binding \
  '[{"fabricIndex":1,"group":'$GROUP',"cluster":6},
    {"fabricIndex":1,"group":'$GROUP',"cluster":8}]' \
  $SWITCH_NODE 1

# Step C: voeg meer bulbs toe aan dezelfde group → ze reageren allemaal samen
chip-tool groups add-group $GROUP "Woonkamer" $BULB2_NODE 0
chip-tool groups add-group $GROUP "Woonkamer" $BULB3_NODE 0
```

Eenmaal ingesteld: drukker → multicast `OnOff.Toggle` op group 0x0001 → alle bulbs in de group reageren. **Google TV Streamer / HA mogen offline** — pure Thread-mesh-multicast.

## OTA — WiFi-update zonder kabel

WiFi staat normaal **uit**. Trigger via **6× snelle clicks** (MODE_TOGGLE) op de drukker → reboot in OTA-mode → directe STA-fetch (met opgeslagen creds) of SoftAP `shelly-ota-XXXXXX` voor eerste provisioning. Nogmaals 6× klikken in OTA-mode → factory reset.

Naast deze WiFi-OTA is er **ook Matter OTA** mogelijk: `esp_matter_ota_requestor_init()` wordt aangeroepen in `matter_start()`, dus zodra HA Matter Server of Google TV Streamer een OTA-image aanbiedt via het Matter OTA Provider cluster (1.3+ standaard), kan dat ook via Thread. Voor de meeste gebruikers blijft WiFi-OTA praktischer (sneller, lokaal HTTP-bestand).

## Build + flash

- **[`INSTALL_VSCODE_WINDOWS.md`](INSTALL_VSCODE_WINDOWS.md)** — stap-voor-stap via **Visual Studio Code op Windows 11 + WSL2 (Ubuntu)**. chip-tool draait apart op je HA Linux-host. **Aanbevolen pad.** Bevat ook sectie 15 met bekende v1.4 build-issues + fixes.
- **[`INSTALL.md`](INSTALL.md)** — pure command-line workflow voor Linux/macOS / WSL2-zonder-VSCode.

Eerste build duurt ~20-45 min (connectedhomeip eenmalig compileren). Daarna met ccache ~1-3 min per incrementele build.

## Bestandsstructuur

```
shelly1gen4_matter_switch/
├── CMakeLists.txt
├── partitions.csv          # dual-OTA + chip_factory + chip_kvs partities
├── sdkconfig.defaults      # Matter + Thread + BLE + OTA defaults
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild
│   ├── app_config.h        # pins, timings, BENCH_MODE (productie=0, bench=1)
│   ├── app_main.cpp        # C++ entrypoint, hergebruikt C-modules
│   ├── CHIPProjectConfig.h # vendor/product-naam overrides (i.p.v. TEST_VENDOR/TEST_PRODUCT)
│   ├── matter_device.cpp   # 4 endpoints + Binding cluster + bound-command emit
│   ├── matter_device.h
│   ├── button.c/.h         # hergebruikt uit Zigbee project
│   ├── relay.c/.h          # idem
│   ├── sensors.c/.h        # idem
│   ├── ota.c/.h            # idem (WiFi STA + SoftAP)
│   ├── status_led.c/.h     # Shelly Add-on LED patronen (OFF/ON/SLOW/FAST/heartbeat)
│   └── idf_component.yml
├── README.md
├── INSTALL.md
└── INSTALL_VSCODE_WINDOWS.md
```

## BENCH_MODE — ontwikkeling vs. productie

`BENCH_MODE` in `app_config.h` bepaalt de GPIO10-polariteit en sensor-initialisatie.

Op de ESP32-C6 zijn **GPIO16 (U0TXD) en GPIO17 (U0RXD) de standaard UART0-pins** — dit is je seriële verbinding via PuTTY / minicom. In productie worden deze pins hergebruikt voor DS18B20 (1-Wire) resp. LD2410 (occupancy). Zodra `sensors_init()` ze herconfigureert, stopt de UART-output. In BENCH_MODE worden de sensor-tasks overgeslagen zodat serial debugging beschikbaar blijft.

| BENCH_MODE | GPIO10 (drukker) | Sensor-tasks (GPIO16/17) | Wanneer |
|---|---|---|---|
| **0** | Active-high — 230V optocoupler drijft pin | Gestart (temp + occ) | **Productie** (Shelly op 230V + Add-on) |
| **1** (default) | Active-low + interne pull-up | Overgeslagen (UART0 blijft actief) | **Bench** (USB-UART via J6, zonder 230V/Add-on) |

> ⚠️ Met `BENCH_MODE=1` in productie: GPIO10 detecteert geen drukker-pulsen (verkeerde polariteit) en sensor-data wordt niet gerapporteerd. Zet `BENCH_MODE=0` voor productie-firmware.

> ℹ️ De Shelly Plus Add-on en J6 (UART debug header) delen GPIO16/17 en kunnen niet tegelijk worden gebruikt.

Overriden bij compilatie: `idf.py build -DBENCH_MODE=0` (of pas `app_config.h` aan).

## Bekende limitaties / TODO

- **Color Control** niet geïmplementeerd in v1 (gebruiker-keuze). Later toe te voegen: `cluster::color_control::create(ep, &cfg, CLUSTER_FLAG_CLIENT)` op EP1 + extra commando-emit-helpers.
- **`chip-tool` binding-UI**: Home Assistant Matter integratie heeft binding-UI nog niet, zelf chip-tool commando's draaien is even nodig. Begin 2026 update verwacht.
- **Matter OTA Provider**: requestor zit erin, maar er is geen lokale provider in HA standaard — voor automatische OTA's via Thread heb je een Matter OTA Provider nodig (komt in HA Matter Server roadmap).
- **DCL (Distributed Compliance Ledger)**: deze firmware gebruikt test vendor ID 0xFFF1. Voor publicatie op Google/Apple Home moet een echte vendor ID aangevraagd worden via CSA membership.
- **DAC (Device Attestation Certificate)**: voor productie zou je echte DAC's moeten provisionen in `chip_factory` partition. Voor lokaal HA-gebruik werkt de test-DAC prima.

## Licentie

Espressif esp-matter en connectedhomeip vallen onder Apache 2.0. Deze custom code is MIT-gelicenseerd.
