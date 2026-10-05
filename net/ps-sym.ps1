param(
  [string]$Exe = "C:\Users\mattb\Downloads\_relcs-src\bin\win-amd64-librw_d3d9-oal\Release\reLCS.exe",
  [string]$PdbDir = "C:\Users\mattb\Downloads\_relcs-src\bin\win-amd64-librw_d3d9-oal\Release",
  [string[]]$Rvas = @("0x22200","0x44dc2","0x159d11","0x15a507","0x11b913","0x11d81f","0x211139","0x213007","0x2b95f9")
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Dbg {
  [DllImport("dbghelp.dll", SetLastError=true, CharSet=CharSet.Ansi)]
  public static extern bool SymInitialize(IntPtr h, string path, bool invade);
  [DllImport("dbghelp.dll", SetLastError=true, CharSet=CharSet.Ansi)]
  public static extern ulong SymLoadModuleEx(IntPtr h, IntPtr hFile, string image, string mod, ulong baseAddr, uint size, IntPtr data, uint flags);
  [DllImport("dbghelp.dll", SetLastError=true)]
  public static extern bool SymFromAddr(IntPtr h, ulong addr, out ulong displacement, IntPtr symInfo);
  [DllImport("dbghelp.dll", SetLastError=true)]
  public static extern bool SymGetLineFromAddr64(IntPtr h, ulong addr, out uint displacement, IntPtr lineInfo);
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Ansi)]
  public struct IMAGEHLP_LINE64 {
    public uint SizeOfStruct; public IntPtr Key; public uint LineNumber;
    public IntPtr FileName; public ulong Address;
  }
}
"@

$base = [UInt64]0x140000000
$proc = [IntPtr](-1)  # pseudo process handle
$ok = [Dbg]::SymInitialize($proc, $PdbDir, $false)
if (-not $ok) { "SymInitialize failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"; exit 1 }
$loaded = [Dbg]::SymLoadModuleEx($proc, [IntPtr]::Zero, $Exe, $null, $base, 0, [IntPtr]::Zero, 0)
if ($loaded -eq 0) { "SymLoadModuleEx failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"; exit 1 }

foreach ($r in $Rvas) {
  $addr = $base + [Convert]::ToUInt64($r, 16)
  # SYMBOL_INFO: SizeOfStruct=84 (offset of Name), name buffer 256 extra
  $buf = [Runtime.InteropServices.Marshal]::AllocHGlobal(84 + 256)
  for ($i = 0; $i -lt (84 + 256); $i++) { [Runtime.InteropServices.Marshal]::WriteByte($buf, $i, 0) }
  [Runtime.InteropServices.Marshal]::WriteInt32($buf, 0, 84)     # SizeOfStruct
  [Runtime.InteropServices.Marshal]::WriteInt32($buf, 80, 256)   # MaxNameLen
  $disp = [UInt64]0
  $res = [Dbg]::SymFromAddr($proc, $addr, [ref]$disp, $buf)
  if ($res) {
    $namePtr = [IntPtr]::Add($buf, 84)
    $name = [Runtime.InteropServices.Marshal]::PtrToStringAnsi($namePtr)
    "{0} -> {1} +0x{2:x}" -f $r, $name, $disp
  } else {
    "{0} -> <no symbol> err={1}" -f $r, [Runtime.InteropServices.Marshal]::GetLastWin32Error()
  }
  # line info
  $lineBuf = [Runtime.InteropServices.Marshal]::AllocHGlobal([Runtime.InteropServices.Marshal]::SizeOf([type][Dbg+IMAGEHLP_LINE64]))
  [Runtime.InteropServices.Marshal]::WriteInt32($lineBuf, 0, [Runtime.InteropServices.Marshal]::SizeOf([type][Dbg+IMAGEHLP_LINE64]))
  $ldisp = [UInt32]0
  if ([Dbg]::SymGetLineFromAddr64($proc, $addr, [ref]$ldisp, $lineBuf)) {
    $st = [Runtime.InteropServices.Marshal]::PtrToStructure($lineBuf, [type][Dbg+IMAGEHLP_LINE64])
    $fname = [Runtime.InteropServices.Marshal]::PtrToStringAnsi($st.FileName)
    "       at {0}:{1}" -f $fname, $st.LineNumber
  }
  [Runtime.InteropServices.Marshal]::FreeHGlobal($buf)
  [Runtime.InteropServices.Marshal]::FreeHGlobal($lineBuf)
}
