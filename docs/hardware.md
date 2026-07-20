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

## Captured target identity

The first-version exact allowlist was captured from the project LaunchPad only
after an independent XDS110 read succeeded:

```text
DPIDR              0x6BA02477
AP IDR             0x84770001
CPUID              0x410CC601
Factory DEVICEID   0x2BB8802F
Factory USERID     0x80C7AE2D
Factory SRAMFLASH  0x00200080
```

`0x00200080` identifies 32 KiB SRAM and 128 KiB MAIN Flash.  Probe programming
fails closed if any allowlisted value differs.  `target-info` performs a system
reset before disconnecting after a successful read-only identity/SRAM test, so
the target is not left halted.

Every connection is first established at 100 kHz under reset.  After DP power
up and reset release, the Probe reconnects at 1000, 500 or 250 kHz in descending
order and uses the fastest rate whose complete DP/AP initialization succeeds.
If all promotion attempts fail it performs a fresh 100 kHz initialization rather
than reusing a possibly desynchronized link.

## Current validation boundary

### Verified hardware milestones (2026-07-20)

The fixed Gateway/Probe pair and the official LP-MSPM0G3507 passed the
following real-hardware checks:

- repeated target identification and SRAM tests at 1000 kHz;
- non-destructive SRAM Loader upload, readback, `PROBE` execution and reset;
- two complete 128 KiB MAIN Flash reads with identical Probe/host SHA-256;
- sector 127 (`0x0001FC00..0x0001FFFF`) erase, 1024-byte 64-bit/ECC program,
  MEM-AP readback verification and system reset;
- restoration of sector 127 from the pre-test backup;
- two complete post-restore reads, byte-for-byte identical to the original
  128 KiB backup.

The original and restored full-image SHA-256 is:

```text
0d1be9fd350cc02a766ef4b4572b396327d6537e58182e88ed4aa2d2b85b9cdb
```

The programmed test-pattern SHA-256 was
`785b0751fc2c53dc14a4ce3d800e69ef9ce1009eb327ccf458afe09c242c26c9`.
The restored all-`FF` sector SHA-256 was
`5f4ecdb7b71c3e403983fe405cddcdc2f2576b655fdb3e80d94a6f7c32e58bc2`.
No mass erase, factory reset, security unlock, BCR/BSL configuration or
NONMAIN operation was issued.

Local builds and protocol tests do not prove electrical timing. Before claiming
hardware acceptance, perform the documented 1000 SWD connects, multi-address
SRAM tests, 20 full program/readback/reset cycles and the required negative
tests (unpowered target, wrong SHA, range error, WAIT/FAULT, timeout and protected
target).
