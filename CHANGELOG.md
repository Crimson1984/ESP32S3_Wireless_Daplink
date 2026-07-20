# Changelog

All notable changes to Wireless DATLINK are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
The project is currently in laboratory validation and does not yet guarantee
backward compatibility between protocol revisions.

## [Unreleased]

### Added

- Planned IDE integration path for using the existing CLI as a VSCode/CCS
  external downloader and for adding a future GDB server or CMSIS-DAP v2
  debugging backend.

### Validation pending

- Flash the latest Probe firmware containing the reset-run change and confirm
  that a freshly programmed target starts without pressing the LaunchPad reset
  button.
- Complete 20 consecutive full-application program/readback/reset cycles.
- Complete the remaining negative tests for unpowered, protected, malformed,
  interrupted and timeout cases.
- Implement and validate IDE debugging features such as single-step,
  breakpoints, watchpoints and GDB Remote Serial Protocol.

## [0.1.0] - 2026-07-20

### Added

- ESP-IDF 6.0 project with separate compile-time `gateway` and `probe` roles,
  independent build directories and a PowerShell build/flash/monitor entrypoint.
- TinyUSB CDC PC interface using COBS framing, explicit little-endian fields
  and CRC32C validation.
- Encrypted fixed-peer ESP-NOW transport with sessions, application ACK bitmap,
  retransmission, heartbeat and stale-session rejection.
- Complete-image staging on both ESP32-S3 nodes with manifest, segment CRC32C
  and image SHA-256 verification before MSPM0 Flash erase begins.
- PC CLI support for TI `.out`, ELF, Intel HEX and raw BIN images. BIN images
  require an explicit base address.
- Read-only `target-info`, non-destructive `loader-test`, target reset, abort,
  progress and full MAIN Flash backup commands.
- Two-pass 128 KiB MAIN Flash backup with Probe/host SHA-256 comparison,
  contiguous-offset validation and byte-for-byte pass comparison.
- GPIO SWD PHY with VTref gating, nRESET control, ACK/parity handling, line
  reset and validated 100/250/500/1000 kHz operation.
- ARM ADIv5 SW-DP, MEM-AP, posted-read, core register, halt/run and reset layers.
- Exact fail-closed MSPM0G3507 identification using DPIDR, AP IDR, CPUID and TI
  Factory Region values.
- TI DriverLib-based SRAM Flash Loader with `PROBE`, sector erase, 64-bit ECC
  program and CRC32 commands.
- Local erase/program/readback/reset state machine restricted to
  `0x00000000..0x0001FFFF` MAIN Flash.
- Hardware wiring, protocol, architecture, safety and validation documentation.

### Fixed

- Moved full-size USB protocol buffers off the USB task stack to prevent an
  8 KiB stack overflow.
- Corrected TinyUSB CDC write handling: the queue API returns a byte count, not
  an `esp_err_t`, and successful short responses now flush correctly.
- Cleared stale Windows USB CDC input and randomized request IDs to avoid
  accepting a response retained across port reopen.
- Made `build.ps1` invoke the configured ESP-IDF Python environment directly,
  avoiding conflicts with a project virtual environment missing `click`.
- Corrected SWCLK phase, SWDIO sampling, WAIT/FAULT data handling and the extra
  post-transaction clock that could be interpreted as a false start bit.
- Corrected MSPM0 connect-under-reset ordering so DP power-up occurs while
  nRESET is asserted and AP validation occurs after reset release.
- Added safe SWD speed promotion from 100 kHz to 1 MHz with 500/250/100 kHz
  recovery paths.
- Returned deterministic target-info and Loader diagnostics instead of allowing
  reliable transport retries to repeat a failed target operation.
- Accepted exact duplicate backup frames that can occur if Windows receives a
  USB frame before a flush reports failure.
- Updated `cortexm_system_reset()` to resume the core after `SYSRESETREQ`,
  verify that `DHCSR.S_HALT` is clear, and use a bounded GPIO6 nRESET pulse as
  a fallback.

### Security and safety

- PMK/LMK and fixed peer MAC addresses are generated into ignored local files;
  real keys are not committed.
- The Probe refuses programming when VTref or any allowlisted target identity
  value is invalid.
- BCR, BSL configuration and NONMAIN writes are rejected.
- No mass erase, factory reset or automatic security unlock operation is
  implemented.
- The LaunchPad is independently powered; the Probe only senses VTref.
- Local virtual environments, Python package metadata, backups, build outputs,
  secrets and debug traces are excluded by `.gitignore`.

### Hardware validation

- Verified the fixed Gateway/Probe ESP-NOW link and repeated 1 MHz target
  identification on the official LP-MSPM0G3507.
- Verified non-destructive SRAM Loader upload, readback, execution and return.
- Captured two identical 128 KiB MAIN Flash backups with SHA-256:
  `0d1be9fd350cc02a766ef4b4572b396327d6537e58182e88ed4aa2d2b85b9cdb`.
- Erased, programmed and read back sector 127
  (`0x0001FC00..0x0001FFFF`), then restored it from the backup.
- Confirmed the restored 128 KiB image is byte-for-byte identical to the
  original backup.
- Built and wirelessly programmed TI SDK `gpio_toggle_output`; observed the
  expected alternating RGB LED pattern.
- Built and wirelessly programmed TI SDK `systick_periodic_timer`; programming,
  readback and reset phases completed successfully.

### Known limitations

- Only MSPM0G3507 MAIN Flash is supported.
- The system is currently a programmer, not a selectable VSCode/CCS debugger.
- Dynamic pairing, signed images, target power control and multi-target support
  are not implemented.
- Only sectors covered by the incoming image are erased; stale data outside the
  image remains unchanged by design.
