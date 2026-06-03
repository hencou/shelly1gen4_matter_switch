# shelly1gen4_matter_switch

**Custom Matter-over-Thread firmware** for the **Shelly 1 Gen4** (ESP32-C6).

1. **Matter OnOff/Dimmer Light Switch** (Toggle, EP1) — bind-client, short press = toggle, long = dim
2. **Matter OnOff Light Switch** (State-follow, EP2) — bind-client, On on contact close, Off on contact open
3. **Matter Temperature Sensor** (EP3) — DS18B20 via dual-pin 1-Wire: TX=GPIO9, RX=GPIO16 (Shelly Plus Add-on)
4. **Matter Occupancy Sensor** (EP4) — Analog IN (GPIO17) via Add-on PWM duty cycle (e.g. HLK-LD2410S)
5. **Matter OnOff Light** (Relay on GPIO5, EP5) — server endpoint, directly controllable from HA

All 5 endpoints are always active — no compile-time choice needed. Universal firmware for all configurations.
EP1 (Toggle) and EP2 (State-follow) drive the same physical inputs — the user chooses via binding which endpoint controls their light/relay:
- **Momentary pushbutton** → bind EP1
- **Maintained/toggle switch** → bind EP2

Pushbutton→light and touch→light work **standalone** after binding setup: HA Matter Server, Google TV Streamer, may be offline — direct Thread-mesh-multicast to the bound bulbs.

The relay (EP5) is **no longer hardcoded** to the button press. Via a Matter binding from EP1/EP2 to EP5 you can optionally make the relay switch along. Without binding the relay only responds to commands from HA.

## Based on

- [esp-matter](https://github.com/espressif/esp-matter) `release/v1.4` (Espressif's official Matter SDK — confirmed-working branch for this project)
- [connectedhomeip](https://github.com/project-chip/connectedhomeip) (as submodule within esp-matter)
- ESP-IDF v5.2.2 (required for stable esp-matter v1.4)
- Reference: esp-matter `examples/light_switch` for the Binding pattern + CMakeLists.txt pattern (`CHIP_HAVE_CONFIG_H` + `-std=gnu++17`)

## Target hardware and network

| Component | Status |
|---|---|
| Shelly 1 Gen4 (ESP32-C6, 8 MB flash) | Target hardware |
| Shelly Plus Add-on | DS18B20 (TX=GPIO9/RX=GPIO16) + TTP223 touch (GPIO18) + Analog IN as PWM duty cycle (GPIO17). Please be aware addon can deliver max 10mA so choose HLK-LD2410S to use as less current as possible |
| Thread Border Router | **Google TV Streamer 4K** |
| Matter primary admin | **Home Assistant Matter Server** add-on |
| Matter-Thread bulb | **IKEA KAJPLATS** (Matter mode) |
| Commissioning tool | HA Matter Server UI or `chip-tool` command line |
| Binding setup tool | `HA Python Matter Server` |

## Pin mapping

**Onboard Shelly 1 Gen4** pins:

| GPIO | Function | Source |
|---|---|---|
| **GPIO4** | PCB button (active-low) | Shelly 1 Gen4 onboard |
| **GPIO5** | Relay output (active-high) | Shelly 1 Gen4 onboard |
| **GPIO10** | Pushbutton input / SW terminal | Shelly 1 Gen4 onboard — **external pull on PCB, no internal pull** |
| **GPIO15** | Status LED onboard (active-low) | Shelly 1 Gen4 onboard PCB LED |

**Shelly Plus Add-on** pins (via J6 connector):

The Add-on uses an **ISO7221A galvanic isolator** that splits the 1-Wire protocol into separate TX and RX lines.

| GPIO | Function | Status |
|---|---|---|
| **GPIO9** | 1-Wire TX (data out) — DS18B20 commands via isolator | Always active |
| **GPIO16** | 1-Wire RX (data in) — DS18B20 responses via isolator | Always active |
| **GPIO17** | Analog IN — HLK-LD2410**S** mmWave sensor (EP3 Occupancy) | Always active |
| **GPIO18** | Digital IN - TTP223 capacitive touch button (drives via EP1) | Always active |

All pins are configurable via `idf.py menuconfig` → **"Shelly 1 Gen4 Matter Switch configuration"**.

**J6 connector pinout** (1.27 mm pitch, 7-pin header on the back of the PCB):

Numbering runs from the pin farthest from the J6 label (pin 1) to the pin right next to the label (pin 7 = GND).

| Pin | Function | GPIO | Notes |
|---|---|---|---|
| 1 | **ESP_DBG_UART** | GPIO18 (Digital IN) | not used for flashing |
| 2 | **TXD** | GPIO16 (UART0 TX // 1-Wire RX) | Shelly TXD → CP2102 RXD |
| 3 | **RXD** | GPIO17 (UART0 RX // Analog IN) | Shelly RXD ← CP2102 TXD |
| 4 | **3.3V** | -- | power supply (3.3V only — **no 5V**) |
| 5 | **RESET** | EN | EN — not needed for manual flashing |
| 6 | **GPIO0 (BOOT)** | GPIO0 | low at power-up → flash mode |
| 7 | **GND** | -- | ground — pin right next to the `J6` silkscreen |

> ⚠️ **Pin 7 = GND is your orientation anchor**: use a multimeter on continuity, find the pin that beeps against the metal shield of the ESP module — that is pin 7. GPIO9 (1-Wire TX) is **not** on J6 but is internally routed on the PCB.

Status LED patterns (`status_led.c`):
- **Fast blink (5 Hz)** during boot/initialization
- **Slow blink (1 Hz)** during commissioning (BLE pairing window)
- **Short flash** on each short-press

## Matter device structure

| Endpoint | Device type | Server clusters | Client clusters | Purpose |
|---|---|---|---|---|
| **EP 1** | 0x0103 OnOff Light Switch | Descriptor, Binding | OnOff, LevelControl | Pushbutton → Toggle → light |
| **EP 2** | 0x0103 OnOff Light Switch | Descriptor, Binding | OnOff | Maintained switch → On/Off → light |
| **EP 3** | 0x0302 Temperature Sensor | TemperatureMeasurement | — | DS18B20 report |
| **EP 4** | 0x0107 Occupancy Sensor | OccupancySensing | — | Analog IN duty ≥ 25 % (≈ 2.5 V) |
| **EP 5** | 0x0100 OnOff Light | OnOff | — | Relay GPIO5 — controllable from HA |

EP1 and EP2 each have a **Binding cluster** (server). The Binding table is populated by HA Matter Server or `chip-tool` with:
- Unicast binding (1 specific bulb, identified by node-ID + endpoint)
- Multicast binding (1 group-ID — all bulbs in that group respond simultaneously)

On each button event, `matter_device.cpp` sends commands directly via `FindOrEstablishSession` + `InvokeCommandRequest` to each entry in the Binding table.

**EP1 vs EP2:** Both endpoints are driven by the same physical inputs. EP1 sends Toggle (for momentary pushbuttons), EP2 sends On/Off (for maintained switches that hold a position). Bind the endpoint that matches your switch type.

**EP5 (relay)** is a server endpoint — HA sees this as a switch you can directly turn on/off. To make the relay switch along with the pushbutton, create a binding from EP1 or EP2 to EP5 (see binding setup below).

## User interaction

All 3 inputs behave **identically** — they send via EP1 (Toggle) and EP2 (State-follow):

| GPIO | Input | Description |
|---|---|---|
| **GPIO10** | System 55 pushbutton | 230V momentary pushbutton on the SW terminal (via optocoupler) |
| **GPIO18** | TTP223 touch | Capacitive touch button on Shelly Plus Add-on digital input |
| **GPIO4** | PCB button | Onboard button on the Shelly 1 Gen4 circuit board |

| Action | Effect |
|---|---|
| Short press (< 500 ms) | Matter `OnOff.Toggle` to EP1 binding entries |
| Contact close | Matter `OnOff.On` to EP2 binding entries (state-follow) |
| Contact open | Matter `OnOff.Off` to EP2 binding entries (state-follow) |
| Long press (> 500 ms) | `LevelControl.Move` (up/down, alternating) via EP1 |
| Release | `LevelControl.Stop` via EP1 |
| 6× rapid (< 2.5 s) | **Mode toggle** — in Matter mode: reboot to OTA mode; in OTA mode: factory reset (wipe nvs + chip_kvs) |

> ℹ️ All 3 inputs use the same callback in `app_main.cpp`. EP1 (Toggle) and EP2 (State-follow) each have their own Binding table. Bind EP1 for momentary pushbuttons, EP2 for maintained switches — or both if you want both toggle and state-follow.

## Commissioning in Home Assistant Matter Server

1. Flash the firmware (see INSTALL.md). On first boot, the device advertises for 5 minutes via BLE and SRP/DNS-SD.
2. Open Home Assistant → Settings → Devices & Services → Matter → "Add device".
3. Enter setup code: **20202021** (default test passcode, configurable in `sdkconfig.defaults`).
4. HA Matter Server pairs via BLE, provisions Thread credentials (requests from Google TV Streamer as TBR), the device joins the Thread network.
5. After ~30-60 s the device appears in HA with 5 entities: switch toggle (EP1), switch state-follow (EP2), temperature sensor (EP3), occupancy sensor (EP4), relay (EP5).

⚠️ **The Thread network must already exist** — Google TV Streamer is your TBR. If HA Matter Server is not yet connected to that same Thread network, use an HA Connect ZBT-2 or similar dongle as secondary TBR (they automatically share the Thread credential set via the Thread Credentials API).

## Binding setup with chip-tool

On your laptop (chip-tool is included with esp-matter, or build your own):

```bash
# Step 0: determine your node IDs
# Shelly switch node-ID (see HA): e.g. 0x0123456789ABCDEF
# Bulb (KAJPLATS) node-ID:        e.g. 0x00112233AABBCCDD
SWITCH_NODE=0x0123456789ABCDEF
BULB_NODE=0x00112233AABBCCDD
SWITCH_EP=1   # pushbutton = EP1
BULB_EP=1     # KAJPLATS light = EP1

# Step 1: bind pushbutton (EP1) to the bulb
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$BULB_NODE',"endpoint":'$BULB_EP',"cluster":6},
    {"fabricIndex":1,"node":'$BULB_NODE',"endpoint":'$BULB_EP',"cluster":8}]' \
  $SWITCH_NODE $SWITCH_EP

```

`cluster:6` = OnOff, `cluster:8` = LevelControl.

### Pushbutton → relay coupling (optional)

To make the local relay switch along on a button press, add a binding from EP1 to EP5 (the relay endpoint on the same Shelly):

```bash
RELAY_EP=5    # relay = EP5

# Bind pushbutton (EP1) to both the bulb AND the local relay
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$BULB_NODE',"endpoint":'$BULB_EP',"cluster":6},
    {"fabricIndex":1,"node":'$BULB_NODE',"endpoint":'$BULB_EP',"cluster":8},
    {"fabricIndex":1,"node":'$SWITCH_NODE',"endpoint":'$RELAY_EP',"cluster":6}]' \
  $SWITCH_NODE $SWITCH_EP
```

Without this binding the relay only responds to commands from HA (or other controllers).

### Maintained switch → light/relay (EP2, state-follow)

For a maintained or toggle switch that holds a position, bind EP2 instead of EP1. EP2 sends On on close and Off on open:

```bash
STATE_EP=2    # state-follow = EP2

# Bind maintained switch (EP2) to the bulb — light follows switch position
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$BULB_NODE',"endpoint":'$BULB_EP',"cluster":6}]' \
  $SWITCH_NODE $STATE_EP

# Or bind EP2 to the local relay — relay follows switch position
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$SWITCH_NODE',"endpoint":5,"cluster":6}]' \
  $SWITCH_NODE $STATE_EP
```

> ℹ️ EP1 and EP2 can coexist. You can bind EP1 to a light (toggle) and EP2 to the relay (state-follow), or vice versa.

### Many-to-many: using groups

```bash
# Step A: create a group on the switch + bulb (groupId 0x0001)
GROUP=0x0001

# Add group on bulb (server)
chip-tool groups add-group $GROUP "Living room" $BULB_NODE 0
chip-tool groups write current-group $GROUP $BULB_NODE 0   # optional

# Add group on switch (client)
chip-tool groups add-group $GROUP "Living room" $SWITCH_NODE 1

# Step B: bind switch EP1 to the group (instead of unicast to bulb)
chip-tool binding write binding \
  '[{"fabricIndex":1,"group":'$GROUP',"cluster":6},
    {"fabricIndex":1,"group":'$GROUP',"cluster":8}]' \
  $SWITCH_NODE 1

# Step C: add more bulbs to the same group → they all respond together
chip-tool groups add-group $GROUP "Living room" $BULB2_NODE 0
chip-tool groups add-group $GROUP "Living room" $BULB3_NODE 0
```

Once configured: pushbutton → multicast `OnOff.Toggle` on group 0x0001 → all bulbs in the group respond. **Google TV Streamer / HA may be offline** — pure Thread mesh multicast.

## OTA — WiFi update without cable

WiFi is normally **off**. To enter OTA mode:

| Method | How | When to use |
|---|---|---|
| **6× rapid clicks** | Press any button 6 times within 2.5 s | Universal — works anytime |

After entering OTA mode the device reboots into a dedicated WiFi state: direct STA fetch (with stored credentials) or SoftAP `shelly-ota-XXXXXX` for first provisioning. **10 minute timeout** — if no upload occurs, the device reboots back to Matter mode automatically. Another 6× clicks in OTA mode → factory reset.

In addition to this WiFi OTA, **Matter OTA** is also possible: `esp_matter_ota_requestor_init()` is called in `matter_start()`, so when HA Matter Server or Google TV Streamer offers an OTA image via the Matter OTA Provider cluster (1.3+ standard), that can also work via Thread. For most users WiFi OTA remains more practical (faster, local HTTP file).

## Build + flash

- **[`INSTALL_VSCODE_WINDOWS.md`](INSTALL_VSCODE_WINDOWS.md)** — step-by-step via **Visual Studio Code on Windows 11 + WSL2 (Ubuntu)**. chip-tool runs separately on your HA Linux host. **Recommended path.** Also contains section 15 with known v1.4 build issues + fixes.
- **[`INSTALL.md`](INSTALL.md)** — pure command-line workflow for Linux/macOS / WSL2 without VS Code.

First build takes ~20-45 min (connectedhomeip one-time compilation). After that with ccache ~1-3 min per incremental build.

## File structure

```
shelly1gen4_matter_switch/
├── CMakeLists.txt
├── partitions.csv          # dual-OTA + chip_factory + chip_kvs partitions
├── sdkconfig.defaults      # Matter + Thread + BLE + OTA defaults
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild
│   ├── app_config.h        # pins, timings, BENCH_MODE (production=0, bench=1)
│   ├── app_main.cpp        # C++ entrypoint, reuses C modules
│   ├── CHIPProjectConfig.h # vendor/product name overrides (instead of TEST_VENDOR/TEST_PRODUCT)
│   ├── matter_device.cpp   # 6 endpoints + Binding cluster + bound-command emit
│   ├── matter_device.h
│   ├── button.c/.h         # reused from Zigbee project
│   ├── relay.c/.h          # idem
│   ├── sensors.c/.h        # idem
│   ├── ota.c/.h            # idem (WiFi STA + SoftAP)
│   ├── status_led.c/.h     # Shelly Add-on LED patterns (OFF/ON/SLOW/FAST/heartbeat)
│   └── idf_component.yml
├── README.md
├── INSTALL.md
└── INSTALL_VSCODE_WINDOWS.md
```

## BENCH_MODE — development vs. production

`BENCH_MODE` in `app_config.h` controls the GPIO10 polarity and sensor initialization.

On the ESP32-C6, **GPIO16 (U0TXD) and GPIO17 (U0RXD) are the default UART0 pins** — this is your serial connection via PuTTY / minicom. In production these pins are repurposed for DS18B20 (1-Wire RX) and Analog IN (PWM duty cycle for occupancy) respectively. Once `sensors_init()` reconfigures them, UART output stops. In BENCH_MODE the sensor tasks are skipped so serial debugging remains available.

| BENCH_MODE | GPIO10 (pushbutton) | Sensor tasks (GPIO16/17) | When |
|---|---|---|---|
| **0** | Active-high — 230V optocoupler drives pin | Started (temp + occ) | **Production** (Shelly on 230V + Add-on) |
| **1** (default) | Active-low + internal pull-up | Skipped (UART0 stays active) | **Bench** (USB-UART via J6, without 230V/Add-on) |

> ⚠️ With `BENCH_MODE=1` in production: GPIO10 does not detect pushbutton pulses (wrong polarity) and sensor data is not reported. Set `BENCH_MODE=0` for production firmware.

> ℹ️ The Shelly Plus Add-on and J6 (UART debug header) share GPIO16/17 and cannot be used simultaneously.

Override at compile time: `idf.py build -DBENCH_MODE=0` (or edit `app_config.h`).

## Known limitations / TODO

- **Color Control** not implemented in v1 (user choice). Can be added later: `cluster::color_control::create(ep, &cfg, CLUSTER_FLAG_CLIENT)` on EP1 + additional command emit helpers.
- **`chip-tool` binding UI**: Home Assistant Matter integration does not yet have a binding UI, running chip-tool commands manually is needed for now. Update expected in early 2026.
- **Matter OTA Provider**: requestor is included, but there is no local provider in HA by default — for automatic OTA updates via Thread you need a Matter OTA Provider (on HA Matter Server roadmap).
- **DCL (Distributed Compliance Ledger)**: this firmware uses test vendor ID 0xFFF1. For publication on Google/Apple Home a real vendor ID must be requested via CSA membership.
- **DAC (Device Attestation Certificate)**: for production you should provision real DACs in the `chip_factory` partition. For local HA usage the test DAC works fine.

## License

Espressif esp-matter and connectedhomeip fall under Apache 2.0. This custom code is MIT licensed.
