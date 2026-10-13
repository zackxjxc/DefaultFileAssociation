"""Authored by Codex. Reuse the tested hash probe in a local managed test DLL."""
from pathlib import Path
import subprocess


def main():
    root = Path(__file__).resolve().parents[2]
    project = (root / "research/scripts/msbuild-latest-probe.proj").read_text(encoding="utf-8")
    source = project.split("<![CDATA[", 1)[1].split("]]>", 1)[0].strip() + "\n"
    source = source.replace("using Microsoft.Build.Framework;\n", "")
    source = source.replace("public class LatestAssociationProbe : Microsoft.Build.Utilities.Task", "public class LatestAssociationProbe")
    source = source.replace("[Required] public string ReportFile", "public string ReportFile")
    source = source.replace("public override bool Execute()", "public bool Execute()")
    source = source.replace("Log.LogMessage(MessageImportance.High, json);", "Console.WriteLine(json);")
    source = source.replace('Log.LogError("Latest association probe failed at " + stage);',
                            'Console.Error.WriteLine("Latest association probe failed at " + stage);')
    path = root / "research/host-probes/LatestAssociationProbe.cs"
    path.write_text("// Authored by Codex. Generated from the tested MSBuild probe.\n" + source,
                    encoding="utf-8", newline="\n")
    folder = root / "research/host-probes/bin"
    folder.mkdir(parents=True, exist_ok=True)
    compiler = Path(r"C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe")
    dll = folder / "ManagedHostProbe.dll"
    command = [str(compiler), "/nologo", "/target:library", "/platform:x64", "/optimize+",
               "/reference:System.Web.Extensions.dll", "/reference:System.Configuration.Install.dll",
               "/out:" + str(dll), str(path), str(root / "research/host-probes/ManagedHostAdapters.cs")]
    result = subprocess.run(command, capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
    (root / "research/evidence/managed-host-probe-build.log").write_bytes(result.stdout + result.stderr)
    print("Compiler exit:", result.returncode, "DLL:", dll)
    if result.returncode:
        print((result.stdout + result.stderr).decode("utf-8", errors="replace"))
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
