# Installatie via Visual Studio Code op Windows 11 (WSL2) — `shelly1gen4_matter_switch`

Stap-voor-stap gids om dit Matter-over-Thread firmware-project te bouwen en te flashen vanuit **Visual Studio Code op Windows 11**, met de build in **WSL2 (Ubuntu 22.04)**.

> **Waarom WSL2 en niet native Windows?** ESP-Matter (Espressif's SDK) en connectedhomeip (Matter-stack) ondersteunen **alleen Linux en macOS**. Op Windows is `wsl --install` de officieel aanbevolen route — zo bevestigt Espressif's eigen documentatie. Native Windows-builds via `install.bat` bestaan niet.

## Architectuur

```
Windows 11
├── VS Code (Windows-kant, met Remote WSL extensie)
└── WSL2 (Ubuntu 22.04)
    ├── ESP-IDF (toolchain voor ESP32-C6)
    ├── ESP-Matter SDK (Matter-implementatie)
    └── USB via usbipd-win → /dev/ttyUSB0 of /dev/ttyACM0
```

Voor de `chip-tool` binding-commando's (na pairing) gebruik je je Home Assistant Matter Server container — die heeft `chip-tool` al ingebakken. WSL hoeft `chip-tool` dus niet zelf te bouwen.

---

## 0. Vereisten

- **OS**: Windows 11 (21H2 of nieuwer)
- **Hardware**: minimaal 16 GB RAM, 40 GB vrij op de Windows C:\\-schijf
- **BIOS**: Virtualisatie (Intel VT-x of AMD-V) ingeschakeld
- **Software**: VS Code op Windows (niet in WSL)
- **Stabiele internetverbinding** voor downloads (~5 GB aan repos + tools)

---

## 1. WSL2 + Ubuntu 22.04 installeren

PowerShell openen als **Administrator** → uitvoeren:

```powershell
wsl --install -d Ubuntu-22.04
```

Dit activeert WSL-features, downloadt de WSL2-kernel, en installeert Ubuntu 22.04.

⚠️ **Herstart Windows** na de installatie. De Windows-features zijn pas actief na een reboot.

Na de reboot start Ubuntu automatisch. Kies een **UNIX-gebruikersnaam** (klein, geen spaties) en een **wachtwoord** (sudo-wachtwoord, niet je Windows-login).

### Kernel-versie controleren

In Ubuntu:
```bash
uname -a
```

Versie moet **5.10.60.1 of hoger** zijn. Niet? In PowerShell:
```powershell
wsl --upgrade
```

### Troubleshooting WSL-installatie

- **Fout `0x80370102`** → Virtualisatie is uit in BIOS. Reboot, ga naar BIOS (Del/F2 tijdens boot), schakel **Intel VT-x** of **AMD-V** in, save & reboot.
- **WSL Stopped** na een Windows-reboot → normaal. Open Ubuntu via start-menu, of run `wsl` in PowerShell om te starten.

---

## 2. usbipd-win installeren (USB-doorgifte naar WSL2)

WSL2 ziet standaard geen USB-poorten. `usbipd-win` is de officiële brug.

PowerShell als Administrator:
```powershell
winget install usbipd
```

Herstart de PowerShell-terminal na install zodat `usbipd` op PATH staat.

Het daadwerkelijk koppelen van de UART-adapter aan WSL doe je pas later in stap 8.

---

## 3. Linux dependencies installeren

In Ubuntu (start-menu → Ubuntu 22.04 LTS, of `wsl` in PowerShell):

```bash
sudo apt update && sudo apt upgrade -y

sudo apt install -y git wget curl flex bison gperf \
    python3 python3-pip python3-venv python3-setuptools \
    cmake ninja-build ccache libffi-dev libssl-dev \
    dfu-util libusb-1.0-0 libglib2.0-dev
```

Voeg jezelf toe aan de `dialout`-groep voor toegang tot `/dev/ttyUSB*`:
```bash
sudo usermod -aG dialout $USER
```
Sluit en heropen de Ubuntu-terminal (of `exit` + `wsl`) zodat de groepswijziging actief wordt.

---

## 4. ESP-IDF installeren

```bash
mkdir -p ~/esp
cd ~/esp
git clone --depth 1 https://github.com/espressif/esp-idf.git

cd ~/esp/esp-idf
./install.sh esp32c6

source ./export.sh
idf.py --version
```

Verwachte output:
```
ESP-IDF v5.x.x
```

Duur: 10-15 minuten.

---

## 5. ESP-Matter installeren

ESP-Matter heeft veel submodules via `connectedhomeip`. We gebruiken een **stable release-branch** (voorkomt de bekende `mobly` dependency-conflict op `main`).

```bash
cd ~/esp
source esp-idf/export.sh

git clone --depth 1 -b release/v1.4 https://github.com/espressif/esp-matter.git
cd esp-matter
git submodule update --init --depth 1
```

> Check [esp-matter releases](https://github.com/espressif/esp-matter/releases) voor de meest recente stabiele branch.

### Platform-specifieke submodules ophalen

```bash
cd ~/esp/esp-matter/connectedhomeip/connectedhomeip
./scripts/checkout_submodules.py --platform esp32 linux --shallow
cd ~/esp/esp-matter
```

### Installeren

```bash
./install.sh
```

⚠️ Duurt **15-45 minuten** afhankelijk van internetsnelheid en hardware.

### Als install.sh faalt met `mobly` / `ResolutionImpossible`

Bekende issue. Fix:
```bash
cd ~/esp/esp-matter/connectedhomeip/connectedhomeip
rm -rf .environment
cd ~/esp/esp-matter
./install.sh
```

Dit ruimt de halfgevulde Python-venv op en probeert opnieuw — werkt meestal direct.

---

## 6. Ccache + auto-environment activeren

Eerste Matter-build duurt > 1 uur. Met ccache zijn vervolg-builds **5-10× sneller**.

```bash
echo 'export IDF_CCACHE_ENABLE=1' >> ~/.bashrc
echo 'source ~/esp/esp-idf/export.sh' >> ~/.bashrc
echo 'source ~/esp/esp-matter/export.sh' >> ~/.bashrc
source ~/.bashrc
```

Nu zijn `idf.py` en alle ESP-Matter env-vars automatisch beschikbaar in elke nieuwe Ubuntu-terminal.

---

## 7. VS Code instellen

### 7a. Extensies installeren (Windows-kant)

In VS Code (`Ctrl+Shift+X`):
- **WSL** (`ms-vscode-remote.remote-wsl`) — van Microsoft
- **Espressif IDF** (`espressif.esp-idf-extension`) — van Espressif

### 7b. Project naar WSL kopiëren

Plaats de project-zip in Windows (bv. `Downloads`), dan in Ubuntu:

```bash
mkdir -p ~/projects
cd ~/projects
cp "/mnt/c/Users/<jouw-windows-user>/Downloads/shelly1gen4_matter_switch.zip" .
unzip shelly1gen4_matter_switch.zip
cd shelly1gen4_matter_switch
```

⚠️ Project moet in WSL-filesystem (`~/projects/...`) staan, **niet** in `/mnt/c/...`. Builds vanuit `/mnt/c/` zijn 5-10× trager door cross-FS overhead.

### 7c. Project openen in VS Code (WSL-modus)

Vanuit dezelfde Ubuntu-terminal in de project-folder:
```bash
code .
```

VS Code opent automatisch in **Remote-WSL modus**. Linksonder verschijnt de badge **`WSL: Ubuntu-22.04`** — dit bevestigt dat alles in Ubuntu draait.

> Dit `code .`-commando is **de eenvoudigste route**. Geen `WSL: Connect to WSL`-wizards, geen handmatige folder-koppelingen.

### 7d. Espressif IDF extensie in WSL-context activeren

`Ctrl+Shift+X` → zoek **"Espressif IDF"** → klik **"Install in WSL: Ubuntu-22.04"** als die knop verschijnt.

Het commando-palette (`F1`) toont daarna ESP-IDF-commando's zoals **"Open ESP-IDF Terminal"**, **"Build Project"**, etc.

> De **"Configure ESP-IDF extension"** wizard hoeft niet per se — omdat we ESP-IDF + tools al in `~/.bashrc` sourcen, vindt elke ESP-IDF terminal in VS Code automatisch het juiste pad.

Mocht de wizard wél nodig zijn (bv. config-validation faalt): kies **"Use existing setup"** met:
- ESP-IDF dir: `/home/<user>/esp/esp-idf`
- Tools dir: `/home/<user>/.espressif`
- Python venv: `/home/<user>/.espressif/python_env/idf5.2_py3.10_env/bin/python`

---

## 8. ESP32-C6 / Shelly aansluiten via UART

Open de Shelly 1 Gen4 behuizing en sluit een USB-UART adapter (CP2102) aan op de **J6-connector** (7-pin rij). Volledige procedure + canonical pinout staat in [`INSTALL.md` hoofdstuk 4](INSTALL.md#4-uart-flashen-via-de-j6-connector-op-de-achterkant) — hier alleen de samenvatting:

| CP2102 | Shelly J6-pin | Doel |
|---|---|---|
| 3.3V (**niet 5V**) | Pin 4 | Voeding |
| GND | Pin 7 (naast `J6` silkscreen) | Massa |
| RXD | Pin 2 (Shelly TXD) | UART data |
| TXD | Pin 3 (Shelly RXD) | UART data |
| GND (brug, voor flash mode) | Pin 6 (GPIO0 / BOOT) | Tijdens power-up |

⚠️ **Belangrijk**: Pin 4 is **alleen 3.3V** — nooit 5V. Pin-nummering start bij het pin het verst van het `J6`-label (pin 1 = ESP_DBG_UART) en eindigt bij `J6` (pin 7 = GND).

Steek de USB-adapter in een Windows USB-poort.

### 8a. Beschikbare USB-apparaten bekijken

PowerShell als **Administrator**:
```powershell
usbipd list
```

Voorbeeld output:
```
BUSID  VID:PID    DEVICE                                  STATE
3-2    10c4:ea60  Silicon Labs CP210x USB to UART Bridge  Not shared
```

### 8b. Devboard doorgeven aan WSL2

```powershell
# Eenmalig (admin-rechten vereist):
usbipd bind --busid 3-2

# Koppelen aan WSL (WSL moet draaien):
usbipd attach --wsl --busid 3-2
```

⚠️ Na elke `wsl --shutdown` of Windows-reboot moet `usbipd attach --wsl` opnieuw worden uitgevoerd.

### 8c. Verifieer in Ubuntu

```bash
lsusb
ls /dev/ttyUSB* /dev/ttyACM*
```

Verwacht: `/dev/ttyUSB0` zichtbaar.

| Chip / Board | USB-bridge | Poort in WSL |
|---|---|---|
| ESP32-C3 / C6 / H2 (native USB) | Ingebouwd JTAG | `/dev/ttyACM0` |
| ESP32 / ESP32-S3 (WROOM) | CP2102 / CH340 | `/dev/ttyUSB0` |
| ESP32-DevKitC | CP2104 | `/dev/ttyUSB0` |

Shelly 1 Gen4 met externe USB-UART adapter (CP2102): `/dev/ttyUSB0`.

---

## 9. Project bouwen en flashen

In VS Code (Remote-WSL actief), open een **ESP-IDF Terminal** via `F1` → "ESP-IDF: Open ESP-IDF Terminal".

```bash
# Target instellen (eenmalig per project)
idf.py set-target esp32c6

# Bouwen
idf.py build

# Flashen (eerste keer: brug Pin 6 (GPIO0) ↔ Pin 7 (GND) tijdens power-up voor flash mode)
idf.py -p /dev/ttyUSB0 flash

# Monitoren
idf.py -p /dev/ttyUSB0 monitor

# Of alles in één commando:
idf.py -p /dev/ttyUSB0 flash monitor
```

`Ctrl+]` om de monitor te verlaten.

**Eerste build duurt 25-45 min**. Vervolg-builds met ccache: 1-3 min.

Of via VS Code-knoppen (na correct ingestelde extensie):
- `F1` → "ESP-IDF: Build Project" (`Ctrl+E B`)
- `F1` → "ESP-IDF: Flash Device" (`Ctrl+E F`)
- `F1` → "ESP-IDF: Monitor Device" (`Ctrl+E M`)
- `F1` → "ESP-IDF: Build, Flash and Start a Monitor" (`Ctrl+E D`)

---

## 10. Setup Pin Code / QR Code uitlezen

In de serial monitor zie je bij eerste boot:
```
CHIP:SVR: SetupQRCode: [MT:U9VJ142C00KA0648G00]
CHIP:SVR: Copy/paste the below URL in a browser to see the QR Code:
CHIP:SVR: https://project-chip.github.io/connectedhomeip/qrcode.html?data=MT%3AU9VJ142C00KA0648G00
CHIP:SVR: Manual pairing code: [34970112332]
```

Open de URL → toon QR-code aan je HA Matter Server UI (Settings → Devices & Services → Add → Matter → scan code).

Standaard test-passcode: `20202021` (gedefinieerd in `sdkconfig.defaults`).

---

## 11. Binding-setup via chip-tool (op je HA-host)

Jouw `chip-tool` draait in de Home Assistant Matter Server container — niet in WSL. Voor binding-write:

Via HA Advanced SSH addon:
```bash
docker exec -it addon_core_matter_server bash
# Binnen container:
chip-tool binding write binding \
  '[{"fabricIndex":1,"node":<bulb-node-id>,"endpoint":1,"cluster":6},
    {"fabricIndex":1,"node":<bulb-node-id>,"endpoint":1,"cluster":8}]' \
  <switch-node-id> 1
```

Vervang `<bulb-node-id>` en `<switch-node-id>` met de node-IDs die HA toont voor je KAJPLATS en Shelly.

Cluster 6 = OnOff, cluster 8 = LevelControl.

---

## 12. OTA — firmware-updates

De firmware ondersteunt twee OTA-paden:

### WiFi-OTA (primair, kant-en-klaar)
1. `idf.py build` levert nieuwe `build/shelly1gen4_matter_switch.bin`
2. Kopieer dit bestand naar HA's `/config/www/`
3. Klik **10× snel** op je System 55 drukker op de Shelly
4. Eerste keer: telefoon → SoftAP `shelly-ota-XXXXXX` → `http://192.168.4.1/` → SSID/pass/URL invullen
5. Volgende keren: 10× klikken volstaat — creds zijn opgeslagen

### Matter OTA Provider (optioneel)
De Matter OTA Requestor is in de firmware geactiveerd (`esp_matter_ota_requestor_init()` in `matter_start()`). Om dit te gebruiken moet je een externe `chip-ota-provider-app` draaien (zie connectedhomeip examples). HA biedt vandaag geen ingebouwde provider; WiFi-OTA blijft daarom de praktische default.

---

## 13. Dagelijkse workflow

Na eenmalige installatie:

```bash
# 1. Ubuntu starten (start-menu → Ubuntu 22.04 LTS)
# 2. Project openen
cd ~/projects/shelly1gen4_matter_switch
code .

# 3. ESP32 doorgeven (PowerShell admin)
usbipd attach --wsl --busid <jouw-busid>

# 4. In VS Code ESP-IDF Terminal:
idf.py -p /dev/ttyUSB0 flash monitor
```

---

## 14. Veelvoorkomende problemen

| Probleem | Oorzaak | Fix |
|---|---|---|
| `wsl: command not found` / WSL niet geïnstalleerd | WSL feature uit | `wsl --install -d Ubuntu-22.04` als admin, reboot |
| WSL fout `0x80370102` | Virtualisatie uit in BIOS | BIOS → enable VT-x / AMD-V |
| `WSL State: Stopped` | Normaal na reboot | `wsl` of start-menu Ubuntu |
| `install.sh` faalt op `mobly` / `ResolutionImpossible` | Pip-dependency conflict in pigweed | `rm -rf connectedhomeip/connectedhomeip/.environment && ./install.sh` |
| `idf.py: command not found` | Env niet gesourced | `source ~/esp/esp-idf/export.sh` (of voeg toe aan `~/.bashrc`) |
| `permission denied` op `/dev/ttyUSB0` | User niet in dialout-groep | `sudo usermod -aG dialout $USER` + heropen terminal |
| USB-poort niet zichtbaar in WSL | `usbipd attach` na reboot vergeten | PowerShell admin: `usbipd attach --wsl --busid <ID>` |
| ESP-IDF extensie zegt "config not valid" | Auto-detect faalt | Skip wizard, voeg `source ~/esp/esp-idf/export.sh` toe aan `~/.bashrc` en reload VS Code |
| Build crasht met `out-of-memory` | WSL2 default-RAM te laag | Maak `%USERPROFILE%\.wslconfig`: `[wsl2]` + `memory=8GB` + `processors=4`. Dan `wsl --shutdown` en herstart |
| Trage build (>1 uur 2e keer) | Ccache niet aan | `export IDF_CCACHE_ENABLE=1` permanent in `~/.bashrc` |
| Build vanuit `/mnt/c/` extreem traag | Cross-FS overhead | Project verplaatsen naar `~/projects/` |

---

## 15. Bekende build-issues met esp-matter `release/v1.4`

Deze project-templates zijn al gefixt; staan hier ter referentie voor wie eigen Matter-projecten optuigt op v1.4.

### 15.1 `static_assert: Wi-Fi network endpoint id and Thread network endpoint id should not be the same`

**Oorzaak:** zowel `CONFIG_ENABLE_WIFI_STATION=y` als `CONFIG_OPENTHREAD_ENABLED=y` actief → Matter probeert twee Network Commissioning Cluster-instanties op endpoint 0.

**Fix** (in `sdkconfig.defaults`):
```
CONFIG_ENABLE_MATTER_OVER_THREAD=y
CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING=y
CONFIG_ENABLE_WIFI_STATION=n
CONFIG_ENABLE_WIFI_AP=n
```

Master toggle `CONFIG_ENABLE_MATTER_OVER_THREAD=y` gate't alle WiFi-Matter-paden uit.

### 15.2 `fatal error: esp_matter.h: No such file or directory`

**Oorzaak:** main-component mist `esp_matter` in `PRIV_REQUIRES`.

**Fix** (`main/CMakeLists.txt`):
```cmake
PRIV_REQUIRES
    ...
    chip
    esp_matter
```

### 15.3 `'ChipDevicePlatformEvent' does not name a type` + `static_cast` van `ConnectivityManager*` → `ConnectivityManagerImpl*` faalt

**Oorzaak:** platform target defines missen. esp-matter's GN-build zet deze normaal door naar CMake, maar bij v1.4 release-branch met Thread-only configs lekt dat soms.

**Fix** (root `CMakeLists.txt`, na `project()`):
```cmake
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
```

En in `main/CMakeLists.txt`:
```cmake
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
```

`CHIP_HAVE_CONFIG_H` triggert de juiste platform-config selectie in connectedhomeip headers. Patroon overgenomen uit `$ESP_MATTER_PATH/examples/light_switch/CMakeLists.txt`.

### 15.4 Partition table `does not fit in configured flash size 2MB`

**Oorzaak:** Shelly 1 Gen4 heeft 8 MB flash, ESP-IDF default is 2 MB.

**Fix** (`sdkconfig.defaults`):
```
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="8MB"
```

### 15.5 API drift v1.4 (codeniveau)

Code geschreven tegen oudere Matter-API faalt op v1.4 met:

| Oud | Nieuw in v1.4 |
|---|---|
| `EMBER_MULTICAST_BINDING` | `MATTER_MULTICAST_BINDING` |
| `EMBER_UNICAST_BINDING` | `MATTER_UNICAST_BINDING` |
| `ESP_MATTER_NONE_FEATURE_MAP` | `ESP_MATTER_NONE_FEATURE_ID` |
| `chip::BindingManager::Params` | `chip::BindingManagerInitParams` (top-level, met extra `mStorage` veld) |
| `cmd.optionsMask = 0` (int → BitMask) | Laat weg — default-init is al lege BitMask |
| `InvokeCommandRequest(... , void(*)(void*, ...), void(*)(void*, CHIP_ERROR))` | Typed lambdas: `[](const ConcreteCommandPath&, const StatusIB&, const auto& response){}` voor success, `[](CHIP_ERROR){}` voor error |

Plus `#include <app/clusters/bindings/BindingManager.h>` (BindingManager class verhuisd weg uit `binding-table.h`).

---

## 16. Volgende stappen

Zie:
- [`README.md`](README.md) — architectuur, Matter device-types, binding-uitleg
- [`INSTALL.md`](INSTALL.md) — CLI fallback workflow (puur Linux/macOS, geen VS Code)
- Espressif's [ESP-Matter Programming Guide](https://docs.espressif.com/projects/esp-matter/en/latest/) voor diepere SDK-info
