"""Authored by Codex. List fixed paths in every ISO edition without elevation.

Uses a project-local portable wimlib tool; never executes extracted ISO files.
"""
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid


def main():
    root = Path(__file__).resolve().parents[2]
    metadata = json.loads((root / "research/evidence/iso-framework-wimgapi-no-privilege.json").read_text(encoding="utf-8"))
    mount = json.loads((root / "research/evidence/iso-mount-state.json").read_text(encoding="utf-8"))
    tool = root / "research/tools/wimlib-1.14.5/bin/wimlib-imagex.exe"
    wim = mount["DevicePath"] + r"\sources\install.wim"
    paths = [
        "/Windows/Microsoft.NET/Framework64/v4.0.30319/MSBuild.exe",
        "/Windows/Microsoft.NET/Framework/v4.0.30319/MSBuild.exe",
        "/Windows/Microsoft.NET/Framework64/v4.0.30319/Microsoft.Build.Tasks.v4.0.dll",
        "/Windows/Microsoft.NET/Framework/v4.0.30319/Microsoft.Build.Tasks.v4.0.dll",
        "/Windows/Microsoft.NET/Framework64/v4.0.30319/Microsoft.Build.Utilities.v4.0.dll",
        "/Windows/Microsoft.NET/Framework64/v4.0.30319/MSBuild.exe.config",
        "/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe",
        "/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe.config",
    ]
    output = root / "research/iso-inspection" / (datetime.now().strftime("%Y%m%d-%H%M%S") + "-wimlib-" + uuid.uuid4().hex[:8])
    output.mkdir(parents=True)
    report = {"iso": mount["ImagePath"], "wim": wim, "read_only_wim": True,
              "tool": str(tool), "image_count": metadata["image_count"], "images": [], "extracted": []}
    report_path = root / "research/evidence/iso-framework-inspection.json"
    def invoke(arguments, log):
        result = subprocess.run([str(tool), *arguments], capture_output=True, timeout=90,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        log.write_bytes(result.stdout + result.stderr)
        return result.returncode, (result.stdout + result.stderr).decode("utf-8", errors="replace")
    def save():
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    for image in metadata["images"]:
        entry = {k: image[k] for k in ("index", "name", "edition", "version")}
        entry["files"] = []
        report["images"].append(entry)
        for number, path in enumerate(paths):
            code, text = invoke(["dir", wim, str(image["index"]), "--path=" + path, "--detailed"],
                                output / f"index-{image['index']}-file-{number}.log")
            expected = path.replace("/", "\\")
            # Windows names are case-insensitive; WIM preserves original case.
            present = code == 0 and ('"' + expected + '"').casefold() in text.casefold()
            digest = re.search(r"(?m)^Hash\s*=\s*0x([a-fA-F0-9]{40})", text)
            entry["files"].append({"image_path": path, "present": present, "exit_code": code,
                                   "wim_sha1": digest.group(1).lower() if digest else None})
            save()
        print("Image", image["index"], image["name"], "present:",
              sum(f["present"] for f in entry["files"]), "/", len(paths), flush=True)
    extract_paths = paths + ["/Windows/System32/windows.storage.dll", "/Windows/System32/drivers/UCPD.sys"]
    extracted_directory = output / "extracted-index-1"
    extracted_directory.mkdir()
    code, text = invoke(["extract", wim, "1", *extract_paths, "--dest-dir=" + str(extracted_directory),
                         "--preserve-dir-structure", "--no-acls", "--no-attributes"], output / "extract-index-1.log")
    report["extraction_exit_code"] = code
    for path in extract_paths:
        file = extracted_directory / path.lstrip("/")
        item = {"image_path": path, "path": str(file), "extracted": file.is_file()}
        if file.is_file():
            data = file.read_bytes()
            item.update(bytes=len(data), sha1=hashlib.sha1(data).hexdigest(), sha256=hashlib.sha256(data).hexdigest())
            host = Path("C:/") / path.lstrip("/")
            if host.is_file():
                item["host_sha256"] = hashlib.sha256(host.read_bytes()).hexdigest()
                item["matches_current_host"] = item["host_sha256"] == item["sha256"]
        report["extracted"].append(item)
    report["all_requested_framework_files_present"] = all(f["present"] for i in report["images"] for f in i["files"])
    report["complete"] = True
    save()
    print("All editions contain requested Framework files:", report["all_requested_framework_files_present"], flush=True)
    print("Extract exit:", code, "Report:", report_path, flush=True)
    if not report["all_requested_framework_files_present"] or code != 0:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
