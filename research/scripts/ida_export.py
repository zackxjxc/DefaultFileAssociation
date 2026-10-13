"""Read-only binary analysis using the existing IDA installation.

Run with research/tools/ida-env/Scripts/python.exe. IDA databases and exports
are written next to project-owned binary copies, never to Windows directories.
"""
import ida
import argparse
import hashlib
import json
import re
from pathlib import Path

import ida_auto
import ida_funcs
import ida_hexrays
import ida_ida
import ida_idaapi
import ida_lines
import ida_nalt
import ida_name
import ida_ua
import idautils
import idc


def function_record(ea):
    function = ida_funcs.get_func(ea)
    if function is None:
        return None
    return {"ea": hex(function.start_ea), "name": ida_funcs.get_func_name(function.start_ea)}


def references(ea):
    return [{"ea": hex(x.frm), "type": x.type, "iscode": bool(x.iscode),
             "function": function_record(x.frm)} for x in idautils.XrefsTo(ea, 0)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--functions", nargs="*", default=[])
    options = parser.parse_args()
    binary = options.binary.resolve()
    project = Path(__file__).resolve().parents[2]
    if not binary.is_relative_to(project / "research" / "binaries"):
        raise ValueError("Analyze project-owned binary copies only")
    options.output.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    status = ida.open_database(str(binary), True)
    if status != 0:
        raise RuntimeError(f"IDA open_database returned {status}")
    try:
        ida_auto.auto_wait()
        decompiler = ida_hexrays.init_hexrays_plugin()
        functions = [{"ea": hex(ea), "name": ida_funcs.get_func_name(ea),
                      "size": ida_funcs.get_func(ea).end_ea - ea}
                     for ea in idautils.Functions()]
        imports = []
        for module in range(ida_nalt.get_import_module_qty()):
            module_name = ida_nalt.get_import_module_name(module)
            def imported(ea, name, ordinal):
                imports.append({"module": module_name, "name": name,
                                "ordinal": ordinal, "ea": hex(ea), "references": references(ea)})
                return True
            ida_nalt.enum_import_names(module, imported)
        strings = idautils.Strings()
        strings.setup(strtypes=[ida_nalt.STRTYPE_C, ida_nalt.STRTYPE_C_16], minlen=4)
        pattern = re.compile(r"UserChoice|HashVersion|AppDefault|mshta|OriginalFile|StackTrace|RegCallback|Certificate|AllowList|DenyList|FeatureV|ProgId", re.I)
        selected_strings = []
        candidate_functions = set()
        for value in strings:
            rendered = str(value)
            if not pattern.search(rendered):
                continue
            refs = references(value.ea)
            for reference in refs:
                if reference["function"]:
                    candidate_functions.add(int(reference["function"]["ea"], 16))
            selected_strings.append({"ea": hex(value.ea), "value": rendered, "references": refs})
        for imported in imports:
            if imported["name"] and re.search(r"CmRegisterCallback|RtlWalkFrameChain", imported["name"]):
                for reference in imported["references"]:
                    if reference["function"]:
                        candidate_functions.add(int(reference["function"]["ea"], 16))
        if options.functions:
            candidate_functions = set()
            for token in options.functions:
                ea = int(token, 16) if token.startswith("0x") else ida_name.get_name_ea(ida_idaapi.BADADDR, token)
                function = ida_funcs.get_func(ea)
                if function is None:
                    raise ValueError(f"Function not found: {token}")
                candidate_functions.add(function.start_ea)
        dossiers = []
        for ea in sorted(candidate_functions):
            function = ida_funcs.get_func(ea)
            record = function_record(ea)
            record["references"] = references(ea)
            record["calls"] = []
            assembly = []
            for instruction in idautils.FuncItems(ea):
                assembly.append(f"{instruction:#x}: {ida_lines.tag_remove(idc.generate_disasm_line(instruction, 0) or '')}")
                if ida_ua.print_insn_mnem(instruction).startswith("call"):
                    targets = list(idautils.CodeRefsFrom(instruction, False))
                    if not targets:
                        targets = list(idautils.DataRefsFrom(instruction))
                    record["calls"].append({"ea": hex(instruction), "targets": [
                        {"ea": hex(target), "name": ida_name.get_name(target)} for target in targets]})
            basename = f"{ea:x}_{record['name']}"
            basename = re.sub(r"[^A-Za-z0-9_.-]", "_", basename)
            (options.output / f"{basename}.asm.txt").write_text("\n".join(assembly) + "\n", encoding="utf-8", newline="\n")
            if decompiler:
                try:
                    pseudocode = str(ida_hexrays.decompile(ea))
                    (options.output / f"{basename}.c").write_text(pseudocode + "\n", encoding="utf-8", newline="\n")
                    record["decompiled"] = True
                except Exception as error:
                    record["decompiled"] = False
                    record["decompile_error"] = str(error)
            dossiers.append(record)
        report = {"binary": str(binary), "sha256": digest, "imagebase": hex(ida_nalt.get_imagebase()),
                  "is_64bit": ida_ida.inf_is_64bit(), "decompiler_available": bool(decompiler),
                  "functions": functions, "imports": imports, "strings": selected_strings,
                  "dossiers": dossiers}
        (options.output / "inventory.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(f"Analyzed {binary.name}: {len(functions)} functions, {len(imports)} imports, {len(selected_strings)} relevant strings, {len(dossiers)} dossiers; decompiler={decompiler}")
        assert hashlib.sha256(binary.read_bytes()).hexdigest() == digest
    finally:
        ida.close_database(True)


if __name__ == "__main__":
    main()
