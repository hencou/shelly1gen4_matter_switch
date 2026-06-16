# shelly1gen4_matter_switch

**Custom Matter-over-Thread firmware** for the **Shelly 1 Gen4** (ESP32-C6) with **Lua scripting** for fully configurable endpoints.

## Features

- **Dynamic Matter endpoints** — no hard-coded endpoints. Configure via the web management dashboard
- **Lua 5.4 scripting** — write custom button/relay/sensor logic per endpoint slot (up to 8 slots)
- **Matter 1.5** compatible — works with Home Assistant, Google Home, Apple Home
- **Thread + WiFi coexistence** — Thread for Matter communication, WiFi for management/OTA
- **WiFi persistent mode** — keep WiFi active after reboot (coexistence with Thread)
- **Thread Border Router** — optional: route IPv6 between WiFi and Thread mesh
- **Smart boot** — auto-detects factory reset vs configured vs commissioned state
- **WiFi management dashboard** — configure scripts, WiFi, endpoints, backup/restore

## Endpoint types

Each script slot can be configured as one of these Matter endpoint types:

| Type | Matter device | Description |
|---|---|---|
| **OnOff Toggle + Dim + Color** | 0x0103 Light Switch (client) | Toggle, dim, color temp via bindings |
| **OnOff State-follow** | 0x0103 Light Switch (client) | On/Off follows switch position |
| **Temperature Sensor** | 0x0302 Temp. Sensor (server) | DS18B20 via 1-Wire |
| **Occupancy Sensor** | 0x0107 Occupancy Sensor (server) | Analog IN duty cycle |
| **Relay (OnOff Light)** | 0x0100 OnOff Light (server) | Physical relay on GPIO5 |

## Based on

- [esp-matter](https://github.com/espressif/esp-matter) with **Matter 1.5** support
- [connectedhomeip](https://github.com/project-chip/connectedhomeip) (as submodule within esp-matter)
- ESP-IDF v5.4.1
- Lua 5.4 (compiled as component)

## Target hardware

| Component | Details |
|---|---|
| Shelly 1 Gen4 (ESP32-C6, 8 MB flash) | Target hardware |
| Shelly Plus Add-on | DS18B20 (TX=GPIO9/RX=GPIO16) + TTP223 touch (GPIO18) + Analog IN (GPIO17) |
| Thread Border Router | Google TV Streamer 4K (or any Thread BR) |
| Matter controller | Home Assistant Matter Server, Google Home, Apple Home |
| Commissioning | HA Matter Server UI or `chip-tool` |

## Setup procedure

### 1. First flash

Flash via UART (see [INSTALL.md](INSTALL.md)). OTA from original Shelly firmware is not possible (signature verification).

### 2. Factory reset → WiFi setup mode

After flashing (or factory reset via the web interface), the module boots into **WiFi setup mode**:

1. WiFi enabled, Bluetooth disabled
2. STA connection attempt with compile-time credentials from `main/secrets.h`
3. **STA succeeds** → credentials saved to NVS, management dashboard on router IP
4. **STA fails** → fallback to AP mode: `shelly-cfg-XXXX` (open network, `http://192.168.4.1/`)

### 3. Configure endpoints via dashboard

Open the management dashboard in your browser and go to the **Scripts** tab:

1. Set a **name**, **endpoint type**, **trigger**, and **Lua script** for each slot you need
2. Click **Save** for each slot
3. Click **Reboot** on the Scripts page

See [SCRIPTS.md](SCRIPTS.md) for example scripts.

### 4. Commissioning

After reboot with configured endpoints, the module enters **BLE commissioning mode**:

1. Open Home Assistant → Settings → Devices & Services → Matter → "Add device"
2. Enter setup code: **34970112332** (default, configurable in `sdkconfig.defaults`)
3. HA Matter Server pairs via BLE, provisions Thread credentials
4. After ~30-60s the device appears in HA with the configured endpoints

### 5. Normal operation

After commissioning, Thread + Bluetooth are active for Matter communication. WiFi is off by default.

To access the management dashboard: **press any button 6× rapidly** (within 2.5 seconds). This disables Thread and starts WiFi in APSTA mode:
- AP always available: `shelly-cfg-XXXX` on `192.168.4.1`
- STA connects to your router (if credentials are saved)

<del>### WiFi persistent mode

Enable via **Hardware** tab → **WiFi persistent** toggle. When ON:
- WiFi stays active across reboots (coexistence with Thread via time-division multiplexing)
- No need to press 6× — management dashboard always reachable
- 6× press still works but won't disable Thread (both stay active)

### Thread Border Router (TBR)

Enable via **Hardware** tab → **Thread Border Router** toggle. When ON:
- Routes IPv6 packets between WiFi backbone and Thread mesh
- Requires WiFi persistent (auto-enabled when enabling TBR)
- Allows Thread devices to communicate with non-Thread networks
- Uses software coexistence (TDM) — single ESP32-C6 radio shared between WiFi and 802.15.4

> **Note:** TBR on a single-SoC ESP32-C6 is functional but has reduced throughput compared to a dual-SoC setup (e.g. ESP32-S3 + ESP32-H2). Suitable for home automation, not industrial use.</del>

### Backup & restore

Via the management dashboard → **Backup** tab:
- **Download Backup** — exports all settings as a JSON file: WiFi credentials + all 8 script slot configurations (name, type, trigger, period, Lua code)
- **Restore Backup** — upload a previously downloaded JSON backup to restore all settings. The device reboots automatically after restore.

### Factory reset

Via the web management dashboard → **Factory Reset** button. This wipes:
- All NVS data (WiFi credentials, script configurations, bench mode)
- All Matter fabrics and commissioning data (`chip_kvs` partition)

After factory reset the module reboots into WiFi setup mode (step 2).

## Pin mapping

**Onboard Shelly 1 Gen4:**

| GPIO | Function |
|---|---|
| **GPIO4** | PCB button (active-low) |
| **GPIO5** | Relay output (active-high) |
| **GPIO10** | Pushbutton input / SW terminal |
| **GPIO15** | Status LED (active-low) |

**Shelly Plus Add-on** (via J6 connector):

| GPIO | Function |
|---|---|
| **GPIO9** | 1-Wire TX — DS18B20 commands via ISO7221A isolator |
| **GPIO16** | 1-Wire RX — DS18B20 responses via isolator |
| **GPIO17** | Analog IN — occupancy sensor (e.g. HLK-LD2410S) |
| **GPIO18** | Digital IN — TTP223 capacitive touch / add-on switch |

**J6 connector pinout** (1.27 mm pitch, 7-pin header on back of PCB):

| Pin | Function | GPIO | Notes |
|---|---|---|---|
| 1 | ESP_DBG_UART | GPIO18 | not used for flashing |
| 2 | TXD | GPIO16 | Shelly TXD → CP2102 RXD |
| 3 | RXD | GPIO17 | Shelly RXD ← CP2102 TXD |
| 4 | 3.3V | — | power supply (no 5V!) |
| 5 | RESET | EN | not needed for manual flashing |
| 6 | GPIO0 (BOOT) | GPIO0 | low at power-up → flash mode |
| 7 | GND | — | pin closest to `J6` silkscreen |

## Lua scripting API

### Input functions

| Function | Returns | Description |
|---|---|---|
| `input.button_event()` | string or nil | Last button event (see events table below) |
| `input.button_id()` | integer | Input that triggered the event: `0`=SW, `1`=Digital IN, `2`=PCB button |
| `input.sw()` | boolean | Current state of SW input (GPIO10) |
| `input.digital()` | boolean | Current state of Digital IN (GPIO18) |
| `input.device_btn()` | boolean | Current state of PCB button (GPIO4) |
| `input.analog()` | integer | Analog IN duty cycle 0–100 % (GPIO17) |
| `input.temperature()` | number | DS18B20 temperature in °C |

### Button events

| Event string | Description |
|---|---|
| `"short_press"` | Short press (< 500ms) |
| `"long_press_start"` | Long press started |
| `"long_press_stop"` | Long press released |
| `"double_press"` | Double press |
| `"short_long_start"` | Short press followed by long press started |
| `"short_long_stop"` | Short-long press released |
| `"contact_closed"` | Button/switch contact closed (pressed) |
| `"contact_open"` | Button/switch contact opened (released) |

### Input IDs

| ID | Input | GPIO |
|---|---|---|
| `0` | SW (pushbutton terminal) | GPIO10 |
| `1` | Digital IN (add-on) | GPIO18 |
| `2` | PCB button (onboard) | GPIO4 |

### Output functions

| Function | Description |
|---|---|
| `output.relay(bool)` | Set relay on/off |
| `output.relay_set(bool)` | Alias for `output.relay()` |
| `output.relay_toggle()` | Toggle relay state |
| `output.relay_state()` | Returns current relay state (boolean) |

### Endpoint functions (client endpoints)

| Function | Description |
|---|---|
| `endpoint.command("toggle")` | Send OnOff Toggle to bound devices |
| `endpoint.command("on")` | Send OnOff On |
| `endpoint.command("off")` | Send OnOff Off |
| `endpoint.command("move_with_onoff", {up=bool, rate=N})` | Start dimming |
| `endpoint.command("stop")` | Stop dimming |
| `endpoint.command("color_temp_set", {mireds=N})` | Set color temperature |
| `endpoint.command("color_temp_move", {warmer=bool, rate=N})` | Start color temp change |
| `endpoint.command("color_temp_stop")` | Stop color temp change |

### Other functions

| Function | Description |
|---|---|
| `endpoint.set(attr, value)` | Set sensor attribute (for server endpoints) |
| `log(msg)` | Print to serial log |
| `timer.millis()` | Uptime in milliseconds |

## WiFi behavior

| State | WiFi | Thread/BLE | How to reach |
|---|---|---|---|
| **Factory reset** (no scripts) | ON — STA + AP | OFF | After flash or factory reset |
| **Configured** (scripts, no fabrics) | OFF | ON (BLE commissioning) | After configuring endpoints + reboot |
| **Commissioned** (normal) | OFF | ON (Thread active) | After commissioning |
| **6× press** (management) | ON — APSTA mode | Thread disabled | Press any button 6× rapidly |
| **WiFi persistent** (setting) | ON — APSTA mode | ON (coexistence) | Toggle in Hardware tab |
| **WiFi persistent + TBR** | ON — APSTA + TBR | ON (border router) | Toggle both in Hardware tab |

## Status LED

The onboard status LED (GPIO15) indicates the device state:

| Pattern | Description |
|---|---|
| **Fast blink** (5 Hz) | Boot / initialization in progress, or OTA update active |
| **Slow blink** (1 Hz) | Not commissioned — waiting for BLE pairing |
| **Heartbeat** (short flash every 2s) | Normal operation — commissioned and online |
| **Off** | LED disabled or no pattern set |

During boot the LED blinks fast. After initialization it switches to heartbeat (if commissioned) or slow blink (if not yet commissioned).

## BENCH_MODE

Controls GPIO10 polarity and sensor initialization. Configurable at runtime via the management dashboard.

| BENCH_MODE | GPIO10 | Sensors | Use case |
|---|---|---|---|
| **0** | Active-high (230V optocoupler) | Active | Production |
| **1** (default) | Active-low + pull-up | Skipped (UART0 stays active) | Development |

## Build + flash

- **[INSTALL.md](INSTALL.md)** — Linux/macOS/WSL2 command-line setup with esp-matter and ESP-IDF
- **[INSTALL_VSCODE_WINDOWS.md](INSTALL_VSCODE_WINDOWS.md)** — VS Code on Windows 11 + WSL2

## File structure

```
shelly1gen4_matter_switch/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── app_config.h        # pins, timings, BENCH_MODE
│   ├── app_main.cpp        # boot sequence, smart boot logic
│   ├── matter_device.cpp   # dynamic endpoint creation + command emit
│   ├── script_engine.c     # Lua 5.4 scripting engine
│   ├── button.c/.h         # button driver (3 inputs, debounce, gestures)
│   ├── relay.c/.h          # relay GPIO control
│   ├── sensors.c/.h        # DS18B20 + analog occupancy
│   ├── ota.c/.h            # WiFi runtime, management dashboard, OTA
│   ├── status_led.c/.h     # LED patterns
│   ├── secrets.h           # compile-time WiFi credentials (gitignored)
│   └── CHIPProjectConfig.h # vendor/product name overrides
├── components/lua/         # Lua 5.4 as ESP-IDF component
├── SCRIPTS.md              # example Lua scripts
├── INSTALL.md
└── INSTALL_VSCODE_WINDOWS.md
```

## Known limitations

- **Test vendor ID**: firmware uses vendor ID 0xFFF1. For Google/Apple Home publication a CSA vendor ID is required.
- **Test DAC**: for production, provision real Device Attestation Certificates in `chip_factory`. For local HA usage the test DAC works fine.
- **WiFi + Thread coexistence**: ESP32-C6 has one 2.4 GHz radio shared via TDM. When WiFi persistent mode is OFF, Thread is disabled when WiFi is activated via 6× press and resumes on reboot. When WiFi persistent mode is ON, both coexist via software coexistence.

## License

Espressif esp-matter and connectedhomeip: Apache 2.0. Lua: MIT. This custom code: MIT.
