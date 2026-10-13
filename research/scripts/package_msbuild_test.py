"""Package the built EXEs, supplied reader samples, and portable VM test instructions."""
import hashlib
import json
import subprocess
import zipfile
from datetime import datetime
from pathlib import Path

root = Path(__file__).resolve().parents[2]
stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
output = root / "out" / "packages" / ("DefaultFileAssociation-MSBuild-" + stamp + ".zip")
sources = {
    "DefaultFileAssociation.exe": root / "out/build/windows-x86-v143/Release/DefaultFileAssociation.exe",
    "DefaultFileAssociation-x64.exe": root / "out/build/windows-x64-v143/Release/DefaultFileAssociation.exe",
    "DocBox.exe": root / "test_case/DocBox.exe",
    "libdocbox.dll": root / "test_case/libdocbox.dll",
    "012.pdf": root / "test_case/012.pdf",
    "test-msbuild-package.ps1": root / "test_case/test-msbuild-package.ps1",
    "test-msbuild-menu.bat": root / "test_case/test-msbuild-menu.bat",
    "README.md": root / "docs/msbuild-vm-test-package.md",
}
payloads = {name: path.read_bytes() for name, path in sources.items()}
commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
manifest = {
    "commit": commit, "prog_id": "docboxtest.pdf", "icon_index": 1,
    "backend": "Windows Framework64 MSBuild; task embedded in EXE",
    "files": {name: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
              for name, data in payloads.items()},
}
output.parent.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
    for name, data in payloads.items():
        archive.writestr(name, data)
    archive.writestr("manifest.json", json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
with zipfile.ZipFile(output) as archive:
    assert archive.testzip() is None
    for name, data in payloads.items():
        assert archive.read(name) == data, name
print(json.dumps({"package": str(output), "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                  "commit": commit, "files": len(payloads) + 1}, ensure_ascii=False))
