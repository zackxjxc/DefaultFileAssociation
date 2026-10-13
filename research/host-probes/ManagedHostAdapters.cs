// Authored by Codex. No service installation or COM registration is performed.
using System;
using System.Collections;
using System.ComponentModel;
using System.Configuration.Install;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

public static class ProbeHostEntry
{
    public static void Run(string expectedHost)
    {
        string host = Process.GetCurrentProcess().MainModule.FileName;
        if (!String.Equals(Path.GetFileName(host), expectedHost, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Unexpected experiment host");
        string mode = Environment.GetEnvironmentVariable("DFA_HOST_PROBE_MODE");
        string report = Environment.GetEnvironmentVariable("DFA_HOST_PROBE_REPORT");
        if ((mode != "read" && mode != "write") || String.IsNullOrEmpty(report) ||
            !Path.IsPathRooted(report) || Path.GetFileName(Path.GetDirectoryName(report)) != "host-probes")
            throw new InvalidOperationException("Missing explicit experiment configuration");
        var probe = new LatestAssociationProbe { AttemptSet = mode == "write", ReportFile = report };
        if (!probe.Execute()) throw new InvalidOperationException("PDF probe failed; inspect the report");
    }
}

[RunInstaller(true), ComVisible(false)]
public class PdfAssociationInstallerProbe : Installer
{
    public override void Install(IDictionary state) { ProbeHostEntry.Run("InstallUtil.exe"); }
    public override void Commit(IDictionary state) { }
    public override void Rollback(IDictionary state) { }
    public override void Uninstall(IDictionary state) { ProbeHostEntry.Run("InstallUtil.exe"); }
}

[ComVisible(true), Guid("2FDE0A75-778F-4D31-B95C-0DBEC4D5B2A3")]
public class PdfAssociationRegistrationProbe
{
    [ComUnregisterFunction]
    public static void Unregister(Type type) { ProbeHostEntry.Run("RegAsm.exe"); }
}
