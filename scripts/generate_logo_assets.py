#!/usr/bin/env python3
"""
Generate GBA boot-logo assets and expected VRAM checks.

Sources:
  - GBATEK (cartridge header logo block 0x004..0x09F, 156 bytes):
    https://mgba-emu.github.io/gbatek/#gbacartridgeheader
  - Reverse-engineered BIOS intro data flow (logo blob at 0x332C, length 0x370):
    https://moonbase.lgbt/blog/adventures-in-the-gba-bios/
"""

from __future__ import annotations

import argparse
import csv
import hashlib
from pathlib import Path


BIOS_SIZE = 0x4000


ASSETS = [
    # name, offset, size, category
    ("bios_intro_logo_huff_lz_blob", 0x332C, 0x0370, "compressed"),
    ("nintendo_logo_palette", 0x3D14, 0x0010, "uncompressed"),
    ("nintendo_logo_small", 0x3D24, 0x0020, "uncompressed"),
    ("logo_tiles", 0x3D44, 0x0200, "uncompressed"),
    ("nintendo_logo_map", 0x3F44, 0x0038, "uncompressed"),
]


# Expected VRAM placements used by the runtime checker.
# NOTE:
# - `requires_rom=1` means the checker should skip that row when no ROM is loaded.
# - The old "logo_bg_src_chunk_* <= BIOS[0x00A0..]" assumption was incorrect:
#   BIOS[0x00A0..] is code, not static logo pixel data.
# - The large intro logo is produced via runtime decode (Huffman -> LZ77 -> BitUnPack)
#   from BIOS blob 0x332C..0x369B, so static byte-for-byte VRAM checks for those
#   intermediate chunks are intentionally omitted here.
EXPECTED_VRAM = [
    # region_name, asset_name, vram_offset, requires_rom
    # Bottom-logo OBJ tiles are cartridge-header dependent in practice on real
    # no-cart boot paths; skip this check when no ROM is loaded.
    ("logo_obj_tiles", "logo_tiles", 0x017400, True),
    ("logo_obj_map", "nintendo_logo_map", 0x017600, False),
]


def sha1_hex(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bios", required=True, help="Path to gba_bios.bin (16KB)")
    parser.add_argument(
        "--out-dir",
        default="analysis/logo_assets",
        help="Output directory for extracted assets and manifests",
    )
    args = parser.parse_args()

    bios_path = Path(args.bios)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    bios = bios_path.read_bytes()
    if len(bios) != BIOS_SIZE:
        raise SystemExit(f"Expected 0x{BIOS_SIZE:X}-byte BIOS, got 0x{len(bios):X} bytes")

    manifest_path = out_dir / "manifest.csv"
    with manifest_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["name", "category", "offset_hex", "size_hex", "file", "sha1"])
        for name, off, size, category in ASSETS:
            blob = bios[off : off + size]
            file_name = f"{name}.bin"
            (out_dir / file_name).write_bytes(blob)
            w.writerow([name, category, f"0x{off:04X}", f"0x{size:04X}", file_name, sha1_hex(blob)])

    expected_path = out_dir / "expected_vram.csv"
    with expected_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["region_name", "asset_file", "vram_offset_hex", "size_hex", "requires_rom"])
        for region_name, asset_name, vram_off, requires_rom in EXPECTED_VRAM:
            matching = [a for a in ASSETS if a[0] == asset_name]
            if not matching:
                raise SystemExit(f"Asset '{asset_name}' not found in ASSETS")
            _, _, size, _ = matching[0]
            w.writerow([
                region_name,
                f"{asset_name}.bin",
                f"0x{vram_off:05X}",
                f"0x{size:04X}",
                "1" if requires_rom else "0",
            ])

    summary_path = out_dir / "README.txt"
    summary_path.write_text(
        "\n".join(
            [
                "GBA BIOS logo assets generated.",
                f"BIOS: {bios_path}",
                f"Manifest: {manifest_path}",
                f"Expected VRAM checks: {expected_path}",
                "",
                "Notes:",
                "- 'bios_intro_logo_huff_lz_blob.bin' is the BIOS intro logo source blob at 0x332C (0x370 bytes).",
                "- BIOS[0x00A0..] is code, not raw logo gfx.",
                "- Cartridge header logo data (0x08000004..0x0800009F) is not BIOS-resident and is not extracted here.",
                "- expected_vram.csv includes 'requires_rom': rows with value 1 are skipped in BIOS-only mode.",
            ]
        )
        + "\n",
        encoding="utf-8",
    )

    print(f"Wrote logo assets to: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
