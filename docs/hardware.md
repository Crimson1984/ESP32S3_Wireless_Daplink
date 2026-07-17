# Hardware and bring-up

## Probe GPIO

| ESP32-S3 | Direction | Function | Hardware note |
|---|---|---|---|
| GPIO4 | output | SWCLK | 47 ohm series resistor |
| GPIO5 | bidirectional | SWDIO | 47 ohm series resistor |
| GPIO6 | open drain | nRESET | about 100 ohm series resistor |
| GPIO1 / ADC1_CH0 | input only | VTref sense | high-impedance divider, never direct overvoltage |
| GND | — | common reference | mandatory |

GPIO19/20 are reserved for native USB OTG, GPIO43/44 for CH343 UART,
GPIO35/36/37 for PSRAM. GPIO0/3/45/46 must not be repurposed for ordinary
control. GPIO48 WS2812 is intentionally unused in version 1.

## LP-MSPM0G3507 jumper and J103 wiring

Use TI SLAU873D as the authoritative board document.

1. Power the LaunchPad from its own USB connector.
2. Keep the J101 GND, 5 V and 3V3 power jumpers installed.
3. Remove J101 `11:12` (NRST), `13:14` (SWDIO), and `15:16` (SWCLK).
4. Connect the Probe to J103:

| J103 | Signal | Probe |
|---|---|---|
| pin 1 | VTref / 3V3 sense | GPIO1 divider input |
| pin 2 | SWDIO | GPIO5 through 47 ohm |
| pin 4 | SWCLK | GPIO4 through 47 ohm |
| pin 10 | nRESET | GPIO6 through about 100 ohm |
| pin 3, 5 or 9 | GND | ESP32-S3 GND |

Do not connect the XDS110 and external Probe drivers to the same SWD signals at
the same time. Restore the three J101 debug jumpers when returning control to
the onboard XDS110.

## Safe validation order

1. With J101 restored, use XDS110/CCS only for a known-good baseline read.
2. Disconnect/stop XDS110, remove the three SWD jumpers and wire J103.
3. Confirm LaunchPad supply and common ground before enabling the Probe.
4. Validate VTref, DPIDR, CPUID, Factory Region and SRAM patterns.
5. Validate 100, 250, 500 and 1000 kHz SWD reads without Flash commands.
6. Test one intentionally disposable 1 KiB MAIN sector.
7. Compare readback byte-for-byte against XDS110/CCS before full-image tests.

No Flash operation should be attempted if target identity, VTref, reset control,
J101 isolation or SWD signal integrity is uncertain.

## Current validation boundary

Local builds and protocol tests do not prove electrical timing. Before claiming
hardware acceptance, perform the documented 1000 SWD connects, multi-address
SRAM tests, 20 full program/readback/reset cycles and the required negative
tests (unpowered target, wrong SHA, range error, WAIT/FAULT, timeout and protected
target).
