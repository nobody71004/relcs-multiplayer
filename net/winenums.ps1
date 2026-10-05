param([int]$ProcId = 0)
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class WE {
  public delegate bool P(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(P cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
"@
$results = New-Object System.Collections.Generic.List[string]
$cb = [WE+P]{
  param($h, $l)
  $pid2 = 0
  [WE]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
  if ($pid2 -eq $ProcId) {
    $c = New-Object Text.StringBuilder 256
    $t = New-Object Text.StringBuilder 256
    [WE]::GetClassName($h, $c, 256) | Out-Null
    [WE]::GetWindowText($h, $t, 256) | Out-Null
    $v = [WE]::IsWindowVisible($h)
    $script:results.Add("hwnd=$h class=$c title=$t visible=$v")
  }
  return $true
}
[WE]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($results.Count -eq 0) { Write-Output "no windows for pid $ProcId" }
else { $results | ForEach-Object { Write-Output $_ } }
