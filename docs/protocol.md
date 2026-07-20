# Protocol

All multibyte values are unsigned little-endian unless explicitly stated. Wire
records are encoded field by field; C structure layout is never the protocol.
Protocol version is currently `1`.

## PC USB CDC

The raw record is COBS encoded and terminated by `00`:

```text
version:u8 type:u8 flags:u16 request_id:u32 payload_length:u32
payload[payload_length] crc32c:u32
```

CRC32C covers the raw header and payload. Maximum payload is 4096 bytes. A
response payload begins with signed `status:i32`. An event additionally contains
`event_type:u8` after status.

Commands are `GET_INFO`, `GET_LINK_STATUS`, `IMAGE_BEGIN`, `IMAGE_DATA`,
`IMAGE_END`, `PROGRAM_START`, `PROGRAM_ABORT`, `GET_PROGRESS`, `TARGET_RESET`,
`TARGET_READ_INFO`, `LOADER_TEST` and `TARGET_BACKUP_START`.

`TARGET_READ_INFO` completes asynchronously with a `TARGET_INFO` event.  Its
payload starts with a signed little-endian `datlink_status_t`.  On success the
status is zero and is followed by the 28-byte target information record.  On
failure it is followed by four `u32` diagnostic fields: stage, DPIDR, AP IDR
and SWD clock in kHz.  Probe-side VTref and SWD failures are therefore reported
explicitly and the reliable request is acknowledged rather than retried until
timeout.

`LOADER_TEST` is a non-destructive preflight operation.  It connects and
identifies the target, performs the SRAM write/read test, uploads the SRAM
Flash Loader, executes only its `PROBE` mailbox command, and then issues a
system reset so the existing target application resumes.  It never invokes a
Flash erase or program command.  Its asynchronous result uses the same status,
target-information, and failure-diagnostic layout as `TARGET_READ_INFO`.

`TARGET_BACKUP_START` carries a non-zero `operation_id:u32` and starts a
read-only 128 KiB MAIN Flash stream. Data events are ordered records:

```text
operation_id:u32 offset:u32 length:u16 data[length]
```

`length` is 1..180 bytes. The final 60-byte result is:

```text
operation_id:u32 status:i32 total_length:u32 sha256:bytes[32]
stage:u32 dpidr:u32 ap_idr:u32 swd_clock_khz:u32
```

The Probe keeps one SWD connection for a complete pass, hashes bytes before
queueing them to the reliable radio transport, then system-resets and
disconnects the target before emitting the result. The PC validates contiguous
offsets, the exact 128 KiB length, the Probe digest and its own digest. The CLI
requires at least two identical complete passes before finalizing a backup
file. No Flash Loader erase/program command is used.

## ESP-NOW reliable frame

```text
magic:u16 version:u8 type:u8 flags:u16
session_id:u32 sequence:u32 ack_base:u32 ack_bitmap:u32
payload_length:u16 header_crc16:u16
payload[0..192] crc32c:u32
```

- One ESP-NOW physical send is in flight.
- Up to four reliable application frames can await application ACK.
- Initial RTO is 50 ms; maximum attempts are eight.
- Heartbeat is 500 ms and link timeout is 3000 ms.
- `ack_base` acknowledges every sequence at or below the base.
- Bit zero in `ack_bitmap` acknowledges `ack_base + 1`.
- The Probe advances ACK only after its application handler has persisted image
  data to Flash.
- ESP-NOW's send callback only releases the physical sender; it is not an
  application ACK.

PMK and LMK are both 16 bytes. Only the compiled peer MAC is registered and
receive callbacks reject all other source MACs.

## Image manifest

```text
format_version:u16 segment_count:u16 target:u32 operation_id:u32 total_length:u32
image_sha256:bytes[32]
repeated segment_count times:
    address:u32 length:u32 data_offset:u32 crc32c:u32
```

Constraints:

- target is `0x3507`;
- one to eight non-overlapping segments;
- total image no larger than 128 KiB;
- address range only `0x00000000..0x0001FFFF`;
- segment starts are eight-byte aligned;
- image data-offset ranges do not overlap and fully cover `total_length`;
- segment CRC32C and concatenated-image SHA-256 must match.

The host concatenates segments in ascending address order. Both ESP nodes stage
and hash the full image. MSPM0 erase starts only after the Probe reports a valid
complete image.

## Progress and target information

Progress is exactly 20 bytes:

```text
status:i32 phase:u8 reserved:bytes[3] completed:u32 total:u32 detail:u32
```

Target information is exactly seven `u32` fields: DPIDR, AP IDR, CPUID, Factory
DEVICEID, Factory USERID, Factory SRAMFLASH and active SWD clock in kHz.
