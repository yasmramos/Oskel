# Checks the UCRT and kernel32 exports the own-linker runtime imports. The
# Windows loader reports a missing one as STATUS_ENTRYPOINT_NOT_FOUND
# (0xc0000139) without naming it in the process output.
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class OskelNative {
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  public static extern IntPtr LoadLibraryW(string name);
  [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
  public static extern IntPtr GetProcAddress(IntPtr module, string name);
}
"@
$ucrt = [OskelNative]::LoadLibraryW("ucrtbase.dll")
if ($ucrt -eq [IntPtr]::Zero) {
  Write-Output "cannot load ucrtbase.dll"
  exit 1
}
foreach ($name in @('_write','exit','_setmode','malloc','time','_open','_read','_close','_lseeki64')) {
  $address = [OskelNative]::GetProcAddress($ucrt, $name)
  Write-Output ("ucrtbase {0}: {1}" -f $name, $address)
}
$kernel = [OskelNative]::LoadLibraryW("kernel32.dll")
if ($kernel -eq [IntPtr]::Zero) {
  Write-Output "cannot load kernel32.dll"
  exit 1
}
foreach ($name in @('GetTickCount64','ExitProcess')) {
  $address = [OskelNative]::GetProcAddress($kernel, $name)
  Write-Output ("kernel32 {0}: {1}" -f $name, $address)
}
