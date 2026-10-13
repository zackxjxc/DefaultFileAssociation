"""Authored by Codex. Export static UCPD policy data from an owned copy only."""
import ida
import hashlib
import json
from pathlib import Path

import ida_auto
import ida_bytes
import ida_hexrays
import ida_idaapi
import ida_name
import ida_nalt
import idautils


def wide_string(ea):
    data = bytearray()
    for offset in range(0, 4096, 2):
        word = ida_bytes.get_bytes(ea + offset, 2)
        if not word or word == b"\0\0":
            break
        data.extend(word)
    return data.decode("utf-16-le", errors="replace")


def main():
    root = Path(__file__).resolve().parents[2]
    binary = root / "research/binaries/UCPD-probe.sys"
    output = root / "research/evidence/ucpd-policy-detail"
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    if digest.lower() != "1b9b4998342d51e9f350e5f87c52183ea096f8e6d226bada879493a67cf4ddc7":
        raise RuntimeError("Input does not match the analyzed driver")
    output.mkdir(parents=True, exist_ok=True)
    if ida.open_database(str(binary), True) != 0:
        raise RuntimeError("Cannot open project-owned database")
    try:
        ida_auto.auto_wait()
        ida_hexrays.init_hexrays_plugin()
        report = {"sha256": digest, "arrays": [], "context_references": [], "globals": []}
        for name, width in [("qword_1400259A0", 8), ("dword_1400259A8", 4)]:
            ea = ida_name.get_name_ea(ida_idaapi.BADADDR, name)
            value = ida_bytes.get_qword(ea) if width == 8 else ida_bytes.get_dword(ea)
            report["globals"].append({"name": name, "disk_value": value,
                "references": [hex(x.frm) for x in idautils.XrefsTo(ea)]})
        strings = idautils.Strings()
        strings.setup(strtypes=[ida_nalt.STRTYPE_C, ida_nalt.STRTYPE_C_16], minlen=4)
        report["msbuild_string_matches"] = [{"ea": hex(s.ea), "value": str(s)}
            for s in strings if "msbuild" in str(s).lower()]
        for name, count in [("off_140025450", 14), ("off_1400254C0", 4), ("off_140025400", 9)]:
            ea = ida_name.get_name_ea(ida_idaapi.BADADDR, name)
            if ea == ida_idaapi.BADADDR:
                raise RuntimeError("Missing data array: " + name)
            values = []
            for index in range(count):
                pointer = ida_bytes.get_qword(ea + 8 * index)
                values.append({"index": index, "pointer": hex(pointer), "value": wide_string(pointer)})
            references = [{"ea": hex(x.frm), "function": ida_name.get_name(x.frm)}
                          for x in idautils.XrefsTo(ea)]
            report["arrays"].append({"name": name, "values": values, "references": references})
        context = ida_name.get_name_ea(ida_idaapi.BADADDR, "Context")
        functions = set()
        for x in idautils.XrefsTo(context):
            import ida_funcs
            function = ida_funcs.get_func(x.frm)
            if function:
                functions.add(function.start_ea)
        for ea in sorted(functions):
            report["context_references"].append({"ea": hex(ea), "name": ida_name.get_name(ea)})
            try:
                code = str(ida_hexrays.decompile(ea))
                (output / f"{ea:x}.c").write_text(code + "\n", encoding="utf-8", newline="\n")
            except Exception as error:
                report["context_references"][-1]["error"] = str(error)
        (output / "policy-data.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(json.dumps(report["arrays"], indent=2))
        print("Context-referencing functions:", len(functions))
    finally:
        ida.close_database(True)


if __name__ == "__main__":
    main()
