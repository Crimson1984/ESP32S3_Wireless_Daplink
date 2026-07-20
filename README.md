# Wireless DATLINK

Wireless DATLINK is a two-node ESP32-S3 programmer for the official
LP-MSPM0G3507 LaunchPad. The repository builds two compile-time roles from the
same source tree:

- `gateway`: PC-side node, downloaded through CH343 `COM8`; commands use the
  separate native USB OTG port as TinyUSB CDC.
- `probe`: target-side node, downloaded/logged through CH343 `COM7`; all SWD
  transactions and Flash operations execute locally.

The project uses ESP-IDF 6.0, CMake and Ninja directly. PlatformIO is not used.

## Safety boundary

The first version only accepts MSPM0G3507 MAIN Flash addresses
`0x00000000..0x0001FFFF`. It never intentionally writes BCR, BSL configuration
or NONMAIN, never requests mass erase/factory reset, and never automatically
unlocks a protected target. The full image must be present and SHA-256 verified
in the Probe's `target_image` partition before any target sector is erased.

The Probe does not power the LaunchPad. The LaunchPad must use its own USB
supply, share ground with the Probe, and provide VTref sense. If VTref is not
detected, SWCLK, SWDIO and nRESET remain high impedance.

See [hardware.md](docs/hardware.md) before attaching the SWD connector.

## Local environment

Required versions currently verified on this workstation:

```text
ESP-IDF       v6.0
CMake         4.0.3
Ninja         1.12.1
Xtensa GCC    15.2.0
MSPM0 SDK     D:\ti\mspm0_sdk_2_10_00_04
TI Arm Clang  D:\ti\ccs2050\ccs\tools\compiler\ti-cgt-armllvm_4.0.4.LTS
```

First-time ESP-IDF setup:

```powershell
$env:IDF_TOOLS_PATH = 'C:\Espressif'
Set-Location 'D:\Espressif\.espressif\v6.0\esp-idf'
.\install.ps1 esp32s3
. .\export.ps1
```

The first ESP-IDF build may initialize upstream submodules and managed
components. Do not launch multiple first builds against the same IDF directory.

## Keys and fixed peers

Only [secrets.example.json](secrets.example.json) is versioned. The first build
creates ignored `secrets.local.json` with random 16-byte PMK/LMK values and an
ignored generated header. Keep this file private and copy the same file when
building both nodes on another workstation.

Fixed identities:

| Role | Wi-Fi station MAC | Download/log port |
|---|---|---|
| Gateway | `14:c1:9f:cd:33:4c` | COM8 |
| Probe | `14:c1:9f:cc:80:5c` | COM7 |

Firmware startup fails closed when its compiled role does not match the local
station MAC.

## Build and download

```powershell
# Both roles
.\build.ps1 -Role All

# Reconfigure and rebuild one role
.\build.ps1 -Role Gateway -Clean
.\build.ps1 -Role Probe -Clean

# Download ESP32-S3 firmware (does not program MSPM0)
.\build.ps1 -Role Gateway -Flash -Port COM8
.\build.ps1 -Role Probe   -Flash -Port COM7

# Serial log
.\build.ps1 -Role Gateway -Monitor -Port COM8
.\build.ps1 -Role Probe   -Monitor -Port COM7
```

Outputs:

```text
build/gateway/datlink_gateway.bin
build/probe/datlink_probe.bin
tools/mspm0_loader/build/mspm0_loader.bin
```

`idf.py flash` consumes the generated flash arguments; `build.ps1` does not
hard-code partition offsets. In ESP-IDF 6 the QIO build still shows `--flash-mode
dio` in the esptool command because the bootloader image starts in DIO and then
switches the flash to QIO.

## PC command-line tool

Install in an isolated Python environment:

```powershell
python -m venv tools\.venv
.\tools\.venv\Scripts\Activate.ps1
python -m pip install -e .\tools\datlink_cli
datlink ports
```

Use the Gateway native OTG CDC COM port, not CH343 COM8:

```powershell
datlink --port COMx info
datlink --port COMx link
datlink --port COMx target-info
datlink --port COMx loader-test
datlink --port COMx backup backups\mspm0g3507-main.bin
datlink --port COMx program app.out
datlink --port COMx program app.elf
datlink --port COMx program app.hex
datlink --port COMx program app.bin --base 0x00000000
datlink --port COMx verify
datlink --port COMx reset
datlink --port COMx abort
```

ELF/TI `.out` must be ELF32 little-endian. The host selects loadable MAIN-Flash
segments; BIN requires an explicit base. Segment starts must be 64-bit aligned,
matching the MSPM0 Flash programming unit.

`backup` is read-only. It reads the complete 128 KiB MAIN Flash twice by
default, compares every byte and both SHA-256 digests, and only then finalizes
the requested output file. Existing files are not replaced unless `--force`
is supplied. The Probe issues a system reset after every pass.

## Verification

Pure software checks:

```powershell
python -m unittest discover -s tests -v
.\build.ps1 -Role All
.\tools\mspm0_loader\build_loader.ps1
```

The tests cover COBS, USB CRC32C, manifest generation, HEX/BIN parsing and fifty
128 KiB reference transfers with 5% loss plus duplicate/reorder injection.
They do not replace real ESP-NOW, USB, SWD timing or Flash endurance testing.

## Documentation

- [Architecture and module ownership](docs/architecture.md)
- [USB, ESP-NOW and image protocol](docs/protocol.md)
- [LaunchPad wiring and hardware bring-up](docs/hardware.md)
- [TI DriverLib third-party notice](THIRD_PARTY_NOTICES.md)
