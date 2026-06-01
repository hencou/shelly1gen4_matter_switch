# Installation and build guide — `shelly1gen4_matter_switch`

⚠️ First build takes **20–45 minutes** because connectedhomeip must compile. After that, incremental builds with ccache take ~1-3 min.

> For Windows users: use [`INSTALL_VSCODE_WINDOWS.md`](INSTALL_VSCODE_WINDOWS.md) (WSL2 + VS Code is the officially recommended route). Native Windows is not supported by esp-matter.

## 1. ESP-IDF v5.2.2

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive -b v5.2.2 https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c6
. ./export.sh           # every new shell session
```

## 2. Clone esp-matter (one-time, ~3 GB with submodules)

```bash
cd ~/esp
# release/v1.4 is the confirmed-working branch for this project
git clone --depth 1 -b release/v1.4 https://github.com/espressif/esp-matter.git
cd esp-matter
git submodule update --init --depth 1

# Platform-specific submodules (esp32 + linux for chip-tool)
cd connectedhomeip/connectedhomeip
./scripts/checkout_submodules.py --platform esp32 linux --shallow
cd ~/esp/esp-matter

./install.sh
. ./export.sh           # sets $ESP_MATTER_PATH + adds chip-tool to PATH
```

⚠️ If `install.sh` fails with `mobly` / `ResolutionImpossible`:
```bash
rm -rf connectedhomeip/connectedhomeip/.environment
./install.sh
```

⚠️ The `connectedhomeip/` submodule alone is ~2 GB. Make sure you have enough free space (>5 GB for build artifacts).

## 3. Build the project

```bash
# Enable ccache for faster rebuild cycles
export IDF_CCACHE_ENABLE=1

# Load both environments (every shell session)
. ~/esp/esp-idf/export.sh
. ~/esp/esp-matter/export.sh

cd ~/projects/shelly1gen4_matter_switch
idf.py set-target esp32c6
idf.py menuconfig     # adjust GPIOs if needed
idf.py build          # first time 20-45 min, then 1-3 min
```

Tip: add both `source ...export.sh` lines + `IDF_CCACHE_ENABLE=1` to your `~/.bashrc` so they load automatically in every new terminal.

For known build issues (CHIP_HAVE_CONFIG_H, MATTER_OVER_THREAD, API drift in v1.4 etc.), see **section 15 in [`INSTALL_VSCODE_WINDOWS.md`](INSTALL_VSCODE_WINDOWS.md#15-known-build-issues-with-esp-matter-releasev14)**.

In `menuconfig`:
- **Shelly 1 Gen4 Matter Switch configuration** → GPIOs
- **Component config → CHIP Device Layer** → leave defaults (Thread enabled, BLE pairing enabled)
- **Partition Table → Custom** → already set correctly via `sdkconfig.defaults`

Expected output: `build/shelly1gen4_matter_switch.bin` (~1.6-1.8 MB).

## 4. UART flashing via the J6 connector on the back

The Shelly 1 Gen4 has **7 holes in a row** on the back — this is the **J6 connector** (silkscreen label `J6`) of the integrated ESP32-C6 module. This is the same row of pins where a Shelly Plus Add-on connects, but it can also be used to flash the ESP32-C6.

OTA flashing from the original Shelly firmware does **not** work — Shelly verifies OTA images with an ECDSA signature. The first flash must be done via UART.

> **Source**: this section follows the canonical pinout from
> [`automatous-io/shelly-1-gen4-matter-thread` FLASHING.md](https://github.com/automatous-io/shelly-1-gen4-matter-thread/blob/main/docs/FLASHING.md)
> — verified on hardware revision `v0.1.2` (printed on the PCB).

### J6 Pinout

Numbering runs **from the pin farthest from the J6 label (pin 1) to the pin right next to the label (pin 7 = GND)**.

| Pin | Function | GPIO | Notes |
|---|---|---|---|
| 1 | **ESP_DBG_UART** | GPIO18 (Digital IN) | not used for flashing |
| 2 | **TXD** | GPIO16 (UART0 TX // 1-Wire RX) | Shelly TXD → CP2102 RXD |
| 3 | **RXD** | GPIO17 (UART0 RX // Analog IN) | Shelly RXD ← CP2102 TXD |
| 4 | **3.3V** | -- | power supply (3.3V only — **no 5V**) |
| 5 | **RESET** | EN | EN — not needed for manual flashing |
| 6 | **GPIO0 (BOOT)** | GPIO0 | low at power-up → flash mode |
| 7 | **GND** | -- | ground — pin right next to the `J6` silkscreen |

⚠️ **Pin 7 = GND is your orientation anchor**: use a multimeter on continuity, find the pin that beeps against the metal shield of the ESP module or a known GND trace pad — that is pin 7. Count back from there to pin 1 in the opposite direction.

### What you need

- **CP2102 USB-UART adapter** with separate 3.3V output (this guide was tested on CP2102). FTDI FT232RL / CH340G also work as long as they can supply 3.3V and operate at 3.3V logic level.
- 1.27 mm 7-pin to 2.54 mm Dupont cable or adapter board (the J6 holes are 1.27 mm pitch). Solid-core Cat 5e/6 wires also fit in the square holes in a pinch, but are hard to hold steady.
- A short jumper wire or pushbutton for the **Pin 6 ↔ Pin 7 (GPIO0 ↔ GND)** bridge during flash mode entry.
- A Chromium browser (Chrome / Edge) for the ESPConnect web flasher, or `esptool.py` / `idf.py` via CLI.

### ⚠️ Safety — read first

- **Never connect 230V to the Shelly while flashing.** The programming header is **not** galvanically isolated from the relay circuit. USB-UART + mains simultaneously = electrocution risk for you, broken Shelly, broken USB port, broken laptop. Completely disconnect the Shelly from the wall outlet / junction box before connecting anything.
- **Never apply 5V to pin 4.** The pin is directly connected to the 3.3V rail of the ESP32-C6. 5V will destroy the module.
- **Bench test note**: when powering via pin 4 (3.3V from USB-UART), the relay does **not** click audibly — the relay coil needs 230V. Firmware, commissioning, GPIO, LED, and Matter pairing all work fully. If you don't hear a click on a short press: that's normal in bench mode, not a bug.

### Wiring

USB-UART **not plugged into the PC yet** during wiring. Connect as follows:

```
CP2102 pin  ←→  Shelly J6
──────────────────────────────────
GND         →  Pin 7  (GND, right next to J6 label)
3.3V        →  Pin 4  (3.3V — NOT 5V)
RXD         →  Pin 2  (Shelly TXD)
TXD         →  Pin 3  (Shelly RXD)

Do not connect on CP2102:  5V, DTR
Do not connect on Shelly:  Pin 1 (DBG), Pin 5 (RESET)

For flash mode (extra):
GND         →  Pin 6  (GPIO0 BOOT, jumper or pushbutton)
```

TX/RX crossing is already handled in the table above — CP2102 **RXD** goes to Shelly **TXD** (pin 2), CP2102 **TXD** to Shelly **RXD** (pin 3).

### Procedure — entering flash mode

The Shelly boots into ROM bootloader when GPIO0 (pin 6) is low at power-up.

1. Connect wiring (USB-UART not plugged into PC yet).
2. **Bridge Pin 6 (GPIO0) and Pin 7 (GND)** — jumper or pushbutton held down.
3. **Plug USB-UART into PC** → Shelly gets 3.3V via pin 4, boots directly into flash mode.
4. Bridge can stay connected during flashing, or released after ~1 second — both work once the bootloader is running.
5. **After successful flashing**: remove bridge before the next power-on, otherwise Shelly boots into flash mode again instead of your firmware.

### WSL2-specific — forwarding USB-UART

From PowerShell as admin:

```powershell
usbipd list                       # note the BUSID of the CP2102
usbipd bind --busid <BUSID>       # one-time per adapter
usbipd attach --wsl --busid <BUSID>
```

Afterwards the device appears in WSL as `/dev/ttyUSB0` (or `/dev/ttyACM0` for some chipsets — check with `ls /dev/tty*`).

## 5. Back up stock firmware + flash

Two routes — pick one.

> ⚠️ **Important about flashing via J6**: the J6 connector has **no DTR/RTS wiring**, so esptool's auto-reset does not work. With `idf.py flash` (Route B) you must re-enter flash mode **before each command** by bridging Pin 6 ↔ Pin 7 and re-plugging the USB-UART. That's cumbersome. **Route A (merged binary + ESPConnect) does everything in a single flash action and is recommended.**

### Route A — Merged binary + ESPConnect (browser, one-shot flash)

Our build produces 4 separate binaries that each need to be at their own offset (bootloader 0x0, partition-table 0x8000, otadata 0xf000, app 0x20000). ESPConnect can only write one file at one offset, so we merge first.

**Step 1 — build merged bin (one-time per build):**

```bash
. ~/esp/esp-idf/export.sh
cd ~/projects/shelly1gen4_matter_switch
idf.py build      # ensures all 4 binaries are up to date

esptool.py --chip esp32c6 merge_bin \
    -o shelly1gen4_matter_switch_merged.bin \
    --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x0      build/bootloader/bootloader.bin \
    0x8000   build/partition_table/partition-table.bin \
    0xf000   build/ota_data_initial.bin \
    0x20000  build/shelly1gen4_matter_switch.bin

# Copy to Windows side for ESPConnect (replace <USERNAME>)
cp shelly1gen4_matter_switch_merged.bin /mnt/c/Users/<USERNAME>/Downloads/
```

Tip: put those last three commands in a script `tools/make-merged.sh` for later iterations.

**Step 2 — flash via ESPConnect:**

1. **Unplug USB-UART** from PC, **bridge Pin 6 ↔ Pin 7 (GPIO0 ↔ GND)**, plug USB-UART back in → Shelly in ROM bootloader. Bridge can stay or be removed, both work.
2. Open [ESPConnect](https://thelastoutpostworkshop.github.io/microcontroller_devkit/espconnect/) in Chrome / Edge.
3. **Connect** → select CP2102 serial port. Baud rate **115200** (lower = more stable for large writes).
4. ESPConnect shows chip info including **MAC address** — note for backup naming.
5. **Optional but recommended**: **Flash Tools → Download Flash Backup** → 8 MB stock image (`shelly-1-gen4-stock-AABBCCDDEEFF.bin`). Takes 5–10 min. Verify file ≈ 8,388,608 bytes. Keep safe — only way back to factory.
6. **Flash Tools → Flash Firmware**:
   - File: `shelly1gen4_matter_switch_merged.bin`
   - Offset: `0x0`
   - **Erase entire flash before writing**: on
   - Click **Flash**. ~1-2 min.
7. **Disconnect** in ESPConnect.
8. Unplug USB-UART, **remove Pin 6 ↔ Pin 7 bridge** (critical — otherwise Shelly boots into flash mode again), plug USB-UART back in → Shelly now runs your Matter firmware and opens BLE pairing.

### Route B — esptool / `idf.py` (CLI, for developers)

Requires re-entering flash mode between **every** esptool call (erase, flash, etc.) because DTR/RTS is not wired on J6.

```bash
# Back up stock (one-time — keep safe!)
# Prerequisite: bridge Pin 6 ↔ Pin 7, re-plug USB
esptool.py --chip esp32c6 -p /dev/ttyUSB0 --baud 460800 \
    --before no_reset --after no_reset \
    read_flash 0x0 0x800000 shelly_stock_backup.bin

# Erase + flash Matter firmware
# Prerequisite: bridge Pin 6 ↔ Pin 7, re-plug USB
idf.py -p /dev/ttyUSB0 erase-flash

# Prerequisite: bridge Pin 6 ↔ Pin 7, re-plug USB
idf.py -p /dev/ttyUSB0 flash

# Monitor (remove bridge, re-plug USB)
idf.py -p /dev/ttyUSB0 monitor
```

Restoring stock can always be done later (also with manual flash mode entry):

```bash
esptool.py --chip esp32c6 -p /dev/ttyUSB0 write_flash 0x0 shelly_stock_backup.bin
```

After flashing: **remove Pin 6 ↔ Pin 7 bridge**, then power-cycle. Otherwise Shelly boots into flash mode again.

In the log you should see something like:
```
I (... ) matter_dev: EP1 = OnOff Light Switch (pushbutton)
I (... ) matter_dev: EP2 = Temperature Sensor
I (... ) matter_dev: EP3 = Occupancy Sensor (LD2410)
I (... ) matter_dev: EP4 = OnOff Light (relay)
I (... ) matter_dev: EP5 = OnOff Light Switch (state-follow)
I (... ) chip[DL]: Device Configuration:
I (... ) chip[DL]:   Setup Pin Code: 20202021
I (... ) chip[DL]:   Setup Discriminator: 3840 (0xF00)
I (... ) chip[DL]:   Manual Pairing Code: [...]
I (... ) chip[DL]:   QR Code: [...]
```

Note the **QR Code** or **Manual Pairing Code** — use these to add the device in HA.

## 6. Prepare Thread network

You already have a Google TV Streamer 4K = working TBR (Google Home fabric).
For HA Matter Server, HA must also be connected to that same Thread network:

**Option A — Shared Thread Credentials**
HA Matter Server automatically picks up Thread credentials from Google TV via the Apple/Google/HA Thread Credentials API (Android 14+ / iOS 16.4+).

**Option B — HA Connect ZBT-2 as secondary TBR**
Plug a ZBT-2 into the HA host → HA becomes secondary TBR → joins the same Thread mesh.

Check in HA → Settings → Devices & Services → Thread → you should see at least one dataset.

## 7. Pairing in HA Matter Server

1. HA → Settings → Devices & Services → "Add Integration" → Matter Server → "Add device".
2. Scan the QR code from the UART log or enter the Manual Pairing Code.
3. Wait 30-90 s. It pairs via BLE, receives Thread credentials, joins Thread, and then appears as "Shelly 1 Gen4 Matter Switch" with 4 entities (switch, temp, occupancy, relay).

If pairing fails:
- Check that the Shelly booted less than 5 minutes ago (BLE pairing window).
- Reset with **6× rapid clicks** on the pushbutton → device wipes Matter NVS and reopens the pairing window.

## 8. Pair a KAJPLATS bulb in Matter mode

KAJPLATS is dual-stack (Zigbee + Matter-over-Thread). For Matter:

1. Screw in the bulb, lamp turns on.
2. On the IKEA box there is a QR code for Matter setup, or use the default code from IKEA's website.
3. HA → Matter → Add device → scan QR.
4. Bulb joins Thread, appears in HA as KAJPLATS light.

Note the Matter node-ID of the bulb (visible in HA device info).

## 9. Binding setup with chip-tool

From your laptop:

```bash
. ~/esp/esp-matter/export.sh    # adds chip-tool to PATH

# Replace with your own node-IDs (see HA device info)
SWITCH=0x0123456789ABCDEF       # Shelly node-ID
BULB=0x00112233AABBCCDD         # KAJPLATS node-ID

# Bind pushbutton (EP1) to the bulb (EP1, OnOff + LevelControl)
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$BULB',"endpoint":1,"cluster":6},
    {"fabricIndex":1,"node":'$BULB',"endpoint":1,"cluster":8}]' \
  $SWITCH 1


```

Press the pushbutton → the KAJPLATS bulb should respond directly, **without HA in the path**.

### Optional: relay co-switching via binding

The local relay (EP4) is a separate server endpoint and by default only responds to commands from HA. To make the relay switch along on a button press, add EP4 to the binding:

```bash
# Bind pushbutton (EP1) to both the bulb AND the local relay (EP4)
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$BULB',"endpoint":1,"cluster":6},
    {"fabricIndex":1,"node":'$BULB',"endpoint":1,"cluster":8},
    {"fabricIndex":1,"node":'$SWITCH',"endpoint":4,"cluster":6}]' \
  $SWITCH 1
```

### Optional: maintained switch (EP5, state-follow)

EP5 is a second switch endpoint that sends On/Off based on switch position (instead of Toggle). Use EP5 for maintained or toggle switches:

```bash
# Bind maintained switch (EP5) to the bulb — lamp follows switch position
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$BULB',"endpoint":1,"cluster":6}]' \
  $SWITCH 5

# Or bind EP5 to the local relay (EP4)
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":'$SWITCH',"endpoint":4,"cluster":6}]' \
  $SWITCH 5
```

> EP1 (Toggle) and EP5 (State-follow) work alongside each other. Bind the endpoint that matches your switch type.

## 10. OTA — firmware updates without UART

1. Build new firmware: `idf.py build`. Place `build/shelly1gen4_matter_switch.bin` in HA `/config/www/`.
2. Click **6× rapidly** on the System 55 pushbutton.
3. First time: phone → SoftAP `shelly-ota-XXXXXX` → `http://192.168.4.1/` → enter SSID + password + URL.
4. Subsequent times: 6× clicks suffice (config is saved in NVS).

## 11. Rollback to stock Shelly firmware

```bash
esptool.py -p /dev/ttyUSB0 write_flash 0x0 shelly_stock_backup.bin
```

## 12. Troubleshooting

| Symptom | Cause / solution |
|---|---|
| Build fails with "esp_matter.h not found" | `$ESP_MATTER_PATH` not set. Run `. ~/esp/esp-matter/export.sh`. |
| Build OOM (out of memory) | connectedhomeip is heavy. Min 8 GB RAM recommended, or `idf.py -j2 build` for more sequential builds. |
| Device does not appear in HA after flash | Check BLE pairing window: log should show "BLE advertising started". Reboot device to reopen window. |
| Thread join fails | No active Thread credentials in fabric. Check HA → Thread → see if dataset exists. |
| Binding command doesn't work | Check `chip-tool binding read binding $SWITCH 1` — should show your new entries. Bulb must have ACL allowing switch to write (HA Matter Server does this automatically during commissioning). |
| Lamp responds slowly (>1s) | Thread mesh can recover via rebooting Google TV Streamer; check `chip-tool thread show-state $SWITCH 0`. |
| Crash at boot after OTA | Bootloader rollback active (see sdkconfig). It automatically falls back to the previous slot. Check your build. |
| Long-press dimming doesn't work | Binding must include **cluster 8** (LevelControl), not just cluster 6. |
| OTA SoftAP doesn't appear | Check that 10× clicks are done within ~3s. Otherwise the pushbutton holds `BTN_EVT_LONG_PRESS`. Reset device and try again. |
| `esptool` timeout on `/dev/ttyUSB0` | Pin 6 (GPIO0) was not low at power-on. Bridge Pin 6 ↔ Pin 7 (GND) again and re-plug USB-UART. See chapter 4. |
| Shelly boots into flash mode again after flash | Pin 6 ↔ Pin 7 bridge is still connected. Remove the jumper and power-cycle. |
| Accidentally applied 5V to Pin 4 | Module is most likely dead. No rescue possible — replace only. Always use 3.3V. |
