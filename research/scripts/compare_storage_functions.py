"""Compare x64 PE function bodies after masking address-dependent operands.

This is static analysis only. Equal normalized bytes do not establish that
referenced functions or data have identical behavior. Requires capstone.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_CALL, CS_GRP_JUMP
from capstone.x86 import X86_OP_MEM, X86_REG_RIP


class Image:
    def __init__(self, path):
        self.path = path
        self.data = path.read_bytes()
        pe = struct.unpack_from("<I", self.data, 0x3c)[0]
        if self.data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("Invalid PE signature")
        machine, count = struct.unpack_from("<HH", self.data, pe + 4)
        optional_size = struct.unpack_from("<H", self.data, pe + 20)[0]
        optional = pe + 24
        if machine != 0x8664 or struct.unpack_from("<H", self.data, optional)[0] != 0x20b:
            raise ValueError("An x64 PE image is required")
        self.sections = []
        for index in range(count):
            section = optional + optional_size + index * 40
            size, rva, raw_size, offset = struct.unpack_from("<IIII", self.data, section + 8)
            self.sections.append((rva, size, offset, raw_size))
        exception_rva, exception_size = struct.unpack_from("<II", self.data, optional + 112 + 3 * 8)
        table = self.read(exception_rva, exception_size)
        self.functions = {start: end for start, end, _ in struct.iter_unpack("<III", table)}

    def read(self, rva, length):
        for start, size, offset, raw_size in self.sections:
            if start <= rva and rva + length <= start + min(size, raw_size):
                return self.data[offset + rva - start:offset + rva - start + length]
        raise ValueError(f"RVA range is not backed by file data: {rva:#x}, {length}")

    def function(self, rva):
        if rva not in self.functions:
            raise ValueError(f"No exact runtime-function entry at {rva:#x}")
        return self.read(rva, self.functions[rva] - rva)


def describe(image, rva, output):
    body = image.function(rva)
    engine = Cs(CS_ARCH_X86, CS_MODE_64)
    engine.detail = True
    normalized = bytearray()
    assembly = []
    references = []
    decoded = 0
    for instruction in engine.disasm(body, rva):
        code = bytearray(instruction.bytes)
        if any(op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP
               for op in instruction.operands):
            target = instruction.address + instruction.size + instruction.disp
            references.append({"at": hex(instruction.address), "kind": "rip-relative", "target": hex(target)})
            code[instruction.disp_offset:instruction.disp_offset + instruction.disp_size] = bytes(instruction.disp_size)
        if instruction.group(CS_GRP_CALL) or instruction.group(CS_GRP_JUMP):
            if instruction.imm_size:
                target = instruction.operands[0].imm
                if not rva <= target < rva + len(body):
                    references.append({"at": hex(instruction.address), "kind": "external-branch", "target": hex(target)})
                    code[instruction.imm_offset:instruction.imm_offset + instruction.imm_size] = bytes(instruction.imm_size)
        normalized.extend(code)
        decoded += instruction.size
        assembly.append(f"{instruction.address:#x}: {instruction.bytes.hex()} {instruction.mnemonic} {instruction.op_str}")
    if decoded != len(body):
        raise ValueError("The function body was not fully decoded")
    output.write_text("\n".join(assembly) + "\n", encoding="utf-8", newline="\n")
    return {"rva": hex(rva), "size": len(body),
            "body_sha256": hashlib.sha256(body).hexdigest(),
            "normalized_sha256": hashlib.sha256(normalized).hexdigest(),
            "references": references}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("old", type=Path)
    parser.add_argument("new", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--function", action="append", required=True, help="OLD_RVA:NEW_RVA")
    options = parser.parse_args()
    options.output.mkdir(parents=True, exist_ok=True)
    old, new = Image(options.old), Image(options.new)
    report = {"old_sha256": hashlib.sha256(old.data).hexdigest(),
              "new_sha256": hashlib.sha256(new.data).hexdigest(), "functions": []}
    for pair in options.function:
        old_rva, new_rva = (int(value, 0) for value in pair.split(":"))
        before = describe(old, old_rva, options.output / f"old-{old_rva:x}.asm.txt")
        after = describe(new, new_rva, options.output / f"new-{new_rva:x}.asm.txt")
        report["functions"].append({"old": before, "new": after,
                                    "normalized_equal": before["normalized_sha256"] == after["normalized_sha256"]})
    (options.output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"functions": [{"old": item["old"]["rva"], "new": item["new"]["rva"],
                                     "old_size": item["old"]["size"], "new_size": item["new"]["size"],
                                     "normalized_equal": item["normalized_equal"]}
                                    for item in report["functions"]]}, indent=2))


if __name__ == "__main__":
    main()
