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
`IMAGE_END`, `PROGRAM_START`, `PROGRAM_ABORT`, `GET_PROGRESS`, `TARGET_RESET`
and `TARGET_READ_INFO`.

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
