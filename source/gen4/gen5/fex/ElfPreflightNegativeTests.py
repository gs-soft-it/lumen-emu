#!/usr/bin/env python3
# Lumen modifications, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
"""Run malformed-container gates using private local copies of a user executable."""
import argparse
import json
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("preflight", type=Path)
    parser.add_argument("executable", type=Path)
    parser.add_argument("evidence_directory", type=Path)
    args = parser.parse_args()
    original = args.executable.read_bytes()
    data = bytearray(original)
    is_self = original[:4] in (bytes.fromhex("4f153d1d"), bytes.fromhex("5414f5ee"))
    self_segments = []
    elf_offset = 0
    if is_self:
        segment_count = struct.unpack_from("<H", data, 24)[0]
        elf_offset = 32 + segment_count * 32
        self_segments = [struct.unpack_from("<QQQQ", data, 32 + i * 32)
                         for i in range(segment_count)]
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data, elf_offset)
    phdrs = [struct.unpack_from("<IIQQQQQQ", data, elf_offset + header[5] + i * header[9])
             for i in range(header[10])]

    def file_position(offset):
        if not is_self:
            return offset
        for segment in self_segments:
            if not segment[0] & 0x800:
                continue
            ph = phdrs[(segment[0] >> 20) & 0xFFF]
            if ph[2] <= offset < ph[2] + ph[5]:
                return segment[1] + offset - ph[2]
        raise ValueError("source metadata lacks a stored SELF mapping")

    fixtures = [("truncated-container", original[:24], "truncated executable container")]
    invalid_machine = bytearray(original)
    struct.pack_into("<H", invalid_machine, elf_offset + 18, 0)
    fixtures.append(("invalid-machine", invalid_machine, "loader rejected SELF/ELF header"))
    invalid_entry = bytearray(original)
    struct.pack_into("<Q", invalid_entry, elf_offset + 24, (1 << 64) - 1)
    fixtures.append(("entry-outside-executable", invalid_entry, "entry is outside executable load segments"))
    invalid_headers = bytearray(original)
    struct.pack_into("<H", invalid_headers, elf_offset + 56, 65535)
    fixtures.append(("invalid-program-header-count", invalid_headers, "invalid program-header shape"))

    dynamic = next(ph for ph in phdrs if ph[0] == 2)
    tags = {}
    for offset in range(0, dynamic[5], 16):
        tag, value = struct.unpack_from("<qQ", original, file_position(dynamic[2] + offset))
        if tag == 0:
            break
        tags[tag] = value
    if 7 in tags and 8 in tags and tags[8] >= 24:
        address = tags[7]
        backing = next(ph for ph in phdrs if ph[0] == 1 and ph[3] <= address < ph[3] + ph[5])
        relocation_position = file_position(backing[2] + address - backing[3])
        invalid_relocation = bytearray(original)
        struct.pack_into("<I", invalid_relocation, relocation_position + 8, 0x7FFF)
        fixtures.append(("unsupported-relocation", invalid_relocation, "unsupported relocation type"))

    args.evidence_directory.mkdir(parents=True, exist_ok=True)
    results = []
    for name, content, expected in fixtures:
        fixture = args.evidence_directory / f"{name}.bin"
        fixture.write_bytes(content)
        completed = subprocess.run([str(args.preflight.resolve()), str(fixture.resolve())],
                                   capture_output=True, text=True, timeout=30)
        output = completed.stdout + completed.stderr
        (args.evidence_directory / f"{name}.log").write_text(output)
        passed = completed.returncode != 0 and expected in output
        results.append({"case": name, "passed": passed, "exit_code": completed.returncode,
                        "expected_rejection": expected, "guest_execution": False})
        print(f"PS5_PREFLIGHT_NEGATIVE case={name} passed={int(passed)} exit={completed.returncode}")
    (args.evidence_directory / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    return 0 if all(result["passed"] for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
