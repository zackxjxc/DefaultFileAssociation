"""Create a reproducible Windows release archive from CI build outputs."""
import argparse
import hashlib
import json
import re
import subprocess
import zipfile
from pathlib import Path


VERSION_RE = re.compile(r"^v\d+\.\d+\.\d+$")


def git_revision(root: Path) -> str:
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()


def release_notes(root: Path, version: str) -> str:
    dedicated = root / "docs" / "releases" / f"{version}.md"
    if dedicated.is_file():
        return dedicated.read_text(encoding="utf-8").strip() + "\n"
    changelog = root / "CHANGELOG.md"
    if changelog.is_file():
        lines = changelog.read_text(encoding="utf-8").splitlines()
        heading = re.compile(rf"^##\s+\[?{re.escape(version)}\]?\s*$", re.I)
        selected = []
        found = False
        for line in lines:
            if heading.match(line):
                found = True
                selected.append(line)
                continue
            if found and line.startswith("## "):
                break
            if found:
                selected.append(line)
        if selected:
            return "\n".join(selected).strip() + "\n"
    return f"Release {version}. See the repository history for the detailed changes.\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="Release tag, for example v0.1.0")
    parser.add_argument("--x86", type=Path, required=True, help="x86 Release executable")
    parser.add_argument("--x64", type=Path, required=True, help="x64 Release executable")
    parser.add_argument("--output-dir", type=Path, default=Path("out/packages"))
    parser.add_argument("--release-notes-output", type=Path)
    options = parser.parse_args()
    if not VERSION_RE.fullmatch(options.version):
        raise SystemExit("--version must use the vMAJOR.MINOR.PATCH form")

    root = Path(__file__).resolve().parents[2]
    x86 = options.x86.resolve()
    x64 = options.x64.resolve()
    required = {
        "DefaultFileAssociation.exe": x86,
        "DefaultFileAssociation-x64.exe": x64,
        "DfaAssociationHost-x86.dll": x86.parent / "DfaAssociationHost-x86.dll",
        "DfaAssociationManagedHost-x86.dll": x86.parent / "DfaAssociationManagedHost-x86.dll",
        "DfaAssociationHost-x64.dll": x64.parent / "DfaAssociationHost-x64.dll",
        "DfaAssociationManagedHost-x64.dll": x64.parent / "DfaAssociationManagedHost-x64.dll",
        "README.md": root / "README.md",
        "README.zh-CN.md": root / "README.zh-CN.md",
        "LICENSE": root / "LICENSE",
        "NOTICE": root / "NOTICE",
        "CHANGELOG.md": root / "CHANGELOG.md",
        "docs/msbuild-vm-test-package.md": root / "docs/msbuild-vm-test-package.md",
        "test-msbuild-package.ps1": root / "test_case/test-msbuild-package.ps1",
        "test-msbuild-menu.bat": root / "test_case/test-msbuild-menu.bat",
    }
    optional = {
        "012.pdf": root / "test_case/012.pdf",
        "SamplePdfReader.exe": root / "test_case/SamplePdfReader.exe",
        "libsamplepdfreader.dll": root / "test_case/libsamplepdfreader.dll",
    }
    for name, path in required.items():
        if not path.is_file():
            raise FileNotFoundError(f"Required release input is missing: {path}")

    notes = release_notes(root, options.version)
    if options.release_notes_output:
        options.release_notes_output.parent.mkdir(parents=True, exist_ok=True)
        options.release_notes_output.write_text(notes, encoding="utf-8", newline="\n")

    payloads = {name: path.read_bytes() for name, path in required.items()}
    optional_missing = []
    for name, path in optional.items():
        if path.is_file():
            payloads[name] = path.read_bytes()
        else:
            optional_missing.append(name)

    payloads["RELEASE_NOTES.md"] = notes.encode("utf-8")
    legacy_marker = bytes.fromhex("646f63626f78").decode("ascii")
    if any(legacy_marker in name.lower() for name in payloads):
        raise RuntimeError("Release payload contains a forbidden legacy sample name")

    manifest = {
        "version": options.version,
        "commit": git_revision(root),
        "backend": "Windows per-user association tool; selectable version-gated PDF backends",
        "optional_sample_files_missing": optional_missing,
        "files": {
            name: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            for name, data in payloads.items()
        },
    }
    payloads["manifest.json"] = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")

    output_dir = options.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    archive_path = output_dir / f"DefaultFileAssociation-{options.version}-windows.zip"
    if archive_path.exists():
        archive_path.unlink()
    with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in payloads.items():
            archive.writestr(name, data)
    with zipfile.ZipFile(archive_path) as archive:
        if archive.testzip() is not None:
            raise RuntimeError("The release archive failed its integrity check")
        for name, data in payloads.items():
            if archive.read(name) != data:
                raise RuntimeError(f"Archive round-trip mismatch: {name}")
    print(json.dumps({
        "archive": str(archive_path),
        "sha256": hashlib.sha256(archive_path.read_bytes()).hexdigest(),
        "version": options.version,
        "commit": manifest["commit"],
        "files": len(payloads),
        "optional_sample_files_missing": optional_missing,
    }, ensure_ascii=False))


if __name__ == "__main__":
    main()
