# Dumps every non-zero process mitigation policy for Chrome's child processes.
#
# GetProcessMitigationPolicy returns a 4-byte bitfield per policy. A policy such as
# ProcessImageLoadPolicy or ProcessDynamicCodePolicy (ACG) blocks LoadLibraryW of a new
# image outright, which surfaces as STATUS_ACCESS_DENIED -- the exact status the shim has
# been seeing for every sandboxed child.
param([string]$Chrome = 'C:\Program Files\Google\Chrome\Application\chrome.exe')

$src = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class Mit {
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint a, bool i, int pid);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetProcessMitigationPolicy(IntPtr p, int policy, IntPtr buf, IntPtr len);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

  static readonly string[] Names = {
    "DEP", "ASLR", "DynamicCode(ACG)", "StrictHandle", "SyscallDisable", "OptionsMask",
    "ExtPointDisable", "CFG", "Signature", "FontDisable", "ImageLoad", "SyscallFilter",
    "PayloadRestriction", "ChildProcess", "SideChannel", "ShadowStack"
  };

  public static string Dump(int pid) {
    IntPtr p = OpenProcess(0x1000 /*QUERY_LIMITED_INFORMATION*/, false, pid);
    if (p == IntPtr.Zero) return "pid=" + pid + " open err=" + Marshal.GetLastWin32Error();
    var sb = new StringBuilder("pid=" + pid + " ");
    for (int i = 0; i < Names.Length; i++) {
      IntPtr buf = Marshal.AllocHGlobal(16);
      for (int k = 0; k < 16; k++) Marshal.WriteByte(buf, k, 0);
      bool ok = GetProcessMitigationPolicy(p, i, buf, (IntPtr)16);
      if (ok) {
        uint v = (uint)Marshal.ReadInt32(buf);
        if (v != 0) sb.Append(Names[i] + "=0x" + v.ToString("X") + " ");
      }
      Marshal.FreeHGlobal(buf);
    }
    CloseHandle(p);
    return sb.ToString();
  }
}
'@

Add-Type -TypeDefinition $src -Language CSharp

Get-Process chrome -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
$profileDir = 'E:\projects\unknowbrowser\.tmp\mit-profile'
Start-Process -FilePath $Chrome -ArgumentList "--user-data-dir=$profileDir --no-first-run --no-default-browser-check about:blank" -WindowStyle Hidden
Start-Sleep -Seconds 9

foreach ($p in Get-CimInstance Win32_Process -Filter "Name='chrome.exe'") {
  $type = 'browser'
  if ($p.CommandLine -match '--type=([a-z-]+)') { $type = $Matches[1] }
  Write-Output ("[{0,-18}] {1}" -f $type, [Mit]::Dump($p.ProcessId))
}

Get-Process chrome -ErrorAction SilentlyContinue | Stop-Process -Force
