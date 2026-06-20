#!/usr/bin/env python3
"""Convert an Arduino Intel HEX firmware image into nRF52840 UF2.

The nice!nano UF2 bootloader identifies the nRF52840 family as 0xADA52840.
No external Python packages are required.
"""

import argparse
import struct
from pathlib import Path

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
NRF52840_FAMILY_ID = 0xADA52840
PAYLOAD_SIZE = 256


def read_hex(path: Path) -> dict[int, int]:
    """Return byte-addressed data from an Intel HEX file."""
    data: dict[int, int] = {}
    upper = 0
    for line_number, line in enumerate(path.read_text().splitlines(), start=1):
        if not line.startswith(":"):
            raise ValueError(f"line {line_number}: not an Intel HEX record")
        record = bytes.fromhex(line[1:])
        if len(record) < 5 or sum(record) & 0xFF:
            raise ValueError(f"line {line_number}: invalid Intel HEX checksum")
        length = record[0]
        address = (record[1] << 8) | record[2]
        record_type = record[3]
        payload = record[4:-1]
        if len(payload) != length:
            raise ValueError(f"line {line_number}: invalid record length")
        if record_type == 0x00:
            for offset, value in enumerate(payload):
                data[upper + address + offset] = value
        elif record_type == 0x04:
            if length != 2:
                raise ValueError(f"line {line_number}: invalid extended address")
            upper = ((payload[0] << 8) | payload[1]) << 16
        elif record_type == 0x01:
            break
    if not data:
        raise ValueError("the HEX file contains no firmware data")
    return data


def write_uf2(image: dict[int, int], destination: Path) -> None:
    first = min(image) & ~(PAYLOAD_SIZE - 1)
    last = max(image) & ~(PAYLOAD_SIZE - 1)
    addresses = list(range(first, last + PAYLOAD_SIZE, PAYLOAD_SIZE))
    with destination.open("wb") as output:
        for block_number, address in enumerate(addresses):
            payload = bytes(image.get(address + offset, 0xFF) for offset in range(PAYLOAD_SIZE))
            header = struct.pack(
                "<IIIIIIII",
                UF2_MAGIC_START0,
                UF2_MAGIC_START1,
                UF2_FLAG_FAMILY_ID_PRESENT,
                address,
                PAYLOAD_SIZE,
                block_number,
                len(addresses),
                NRF52840_FAMILY_ID,
            )
            output.write(header + payload + bytes(476 - PAYLOAD_SIZE) + struct.pack("<I", UF2_MAGIC_END))


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert an Intel HEX image to nice!nano nRF52840 UF2.")
    parser.add_argument("hex_file", type=Path, help="compiled Arduino .hex file")
    parser.add_argument("uf2_file", type=Path, help="destination .uf2 file")
    args = parser.parse_args()
    write_uf2(read_hex(args.hex_file), args.uf2_file)
    print(f"Wrote {args.uf2_file}")


if __name__ == "__main__":
    main()
