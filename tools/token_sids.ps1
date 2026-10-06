# Dumps the user, group and restricted SID lists of Chrome's child processes.
#
# A restricted token has to satisfy two access checks at once: the ordinary one against its
# enabled groups, and a second one against TokenRestrictedSids. A DACL that grants every
# ordinary group can still deny a restricted token, so the restricted list is the only way
# to know which ACE the shim DLL actually needs.
param([string]$Chrome = 'C:\Program Files\Google\Chrome\Application\chrome.exe')

$src = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class Tok {
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr proc, uint access, out IntPtr token);
  [DllImport("advapi32.dll", SetLastError=true)] static extern bool GetTokenInformation(IntPtr token, int cls, IntPtr info, int len, out int ret);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern bool ConvertSidToStringSidW(IntPtr sid, out IntPtr str);
  [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr h);

  const int TokenUser = 1, TokenGroups = 2, TokenRestrictedSids = 11, TokenIntegrityLevel = 25;
  const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000, TOKEN_QUERY = 0x0008;

  static string SidOf(IntPtr p) {
    IntPtr s;
    if (ConvertSidToStringSidW(p, out s)) { string r = Marshal.PtrToStringUni(s); LocalFree(s); return r; }
    return "?";
  }

  public static string Describe(int pid) {
    var sb = new StringBuilder();
    IntPtr proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
    if (proc == IntPtr.Zero) return "pid=" + pid + " OpenProcess err=" + Marshal.GetLastWin32Error();
    IntPtr tok;
    if (!OpenProcessToken(proc, TOKEN_QUERY, out tok)) {
      int e = Marshal.GetLastWin32Error(); CloseHandle(proc);
      return "pid=" + pid + " OpenProcessToken err=" + e;
    }

    int len = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, IntPtr.Zero, 0, out len);
    if (len > 0) {
      IntPtr buf = Marshal.AllocHGlobal(len);
      int got;
      if (GetTokenInformation(tok, TokenIntegrityLevel, buf, len, out got)) {
        IntPtr sid = Marshal.ReadIntPtr(buf);
        byte count = Marshal.ReadByte(sid, 1);
        int rid = Marshal.ReadInt32(sid, 8 + (count - 1) * 4);
        sb.Append("il_rid=" + rid + " ");
      }
      Marshal.FreeHGlobal(buf);
    }

    int[] classes = { TokenUser, TokenGroups, TokenRestrictedSids };
    string[] names = { "user", "groups", "restricted" };
    for (int c = 0; c < classes.Length; c++) {
      len = 0;
      GetTokenInformation(tok, classes[c], IntPtr.Zero, 0, out len);
      if (len <= 0) continue;
      IntPtr buf = Marshal.AllocHGlobal(len);
      int got;
      if (GetTokenInformation(tok, classes[c], buf, len, out got)) {
        if (classes[c] == TokenUser) {
          sb.Append(names[c] + "=[" + SidOf(Marshal.ReadIntPtr(buf)) + "] ");
        } else {
          int count = Marshal.ReadInt32(buf);
          sb.Append(names[c] + "(" + count + ")=[");
          for (int i = 0; i < count; i++) sb.Append(SidOf(Marshal.ReadIntPtr(buf, 8 + i * 16)) + " ");
          sb.Append("] ");
        }
      }
      Marshal.FreeHGlobal(buf);
    }
    CloseHandle(tok); CloseHandle(proc);
    return "pid=" + pid + " " + sb;
  }
}
'@

Add-Type -TypeDefinition $src -Language CSharp

Get-Process chrome -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
Start-Process -FilePath $Chrome -ArgumentList '--user-data-dir=E:\projects\unknowbrowser\.tmp\plain-profile --no-first-run --no-default-browser-check about:blank' -WindowStyle Hidden
Start-Sleep -Seconds 8

$procs = Get-CimInstance Win32_Process -Filter "Name='chrome.exe'"
foreach ($p in $procs) {
  $type = 'browser'
  if ($p.CommandLine -match '--type=([a-z-]+)') { $type = $Matches[1] }
  Write-Output ("[{0,-18}] {1}" -f $type, [Tok]::Describe($p.ProcessId))
}

Get-Process chrome -ErrorAction SilentlyContinue | Stop-Process -Force
