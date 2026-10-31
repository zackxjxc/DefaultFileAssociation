using System;
using System.Collections;
using System.ComponentModel;
using System.Configuration.Install;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

internal static class DfaNativeHost
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibrary(string path);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool FreeLibrary(IntPtr module);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate int RunDelegate();

    public static void Run()
    {
        string host = Process.GetCurrentProcess().MainModule.FileName;
        if (!String.Equals(Path.GetFileName(host), "InstallUtil.exe", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Unexpected association host");
        string mode = Environment.GetEnvironmentVariable("DFA_HOST_PROBE_MODE");
        string report = Environment.GetEnvironmentVariable("DFA_HOST_PROBE_REPORT");
        string nativeDll = Environment.GetEnvironmentVariable("DFA_HOST_PROBE_DLL");
        if ((mode != "preflight" && mode != "read" && mode != "write") ||
            String.IsNullOrEmpty(report) || !Path.IsPathRooted(report) ||
            String.IsNullOrEmpty(nativeDll) || !Path.IsPathRooted(nativeDll) || !File.Exists(nativeDll))
            throw new InvalidOperationException("Missing explicit association host configuration");
        IntPtr module = LoadLibrary(nativeDll);
        if (module == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        try
        {
            IntPtr address = GetProcAddress(module, "DfaAssociationHostRun");
            if (address == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            int result = ((RunDelegate)Marshal.GetDelegateForFunctionPointer(address, typeof(RunDelegate)))();
            if (result < 0) Marshal.ThrowExceptionForHR(result);
        }
        finally
        {
            FreeLibrary(module);
        }
    }
}

[RunInstaller(true), ComVisible(false)]
public sealed class DfaAssociationInstaller : Installer
{
    public override void Install(IDictionary state) { DfaNativeHost.Run(); }
    public override void Uninstall(IDictionary state) { DfaNativeHost.Run(); }
}
