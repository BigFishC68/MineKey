
$src = @"
using System;
using System.Runtime.InteropServices;

public static class Hid {
    [StructLayout(LayoutKind.Sequential)]
    public struct IFD { public int cbSize; public Guid g; public int Flags; public IntPtr R; }
    [StructLayout(LayoutKind.Sequential)]
    public struct ATTR { public int Size; public ushort VID; public ushort PID; public ushort Ver; }
    [StructLayout(LayoutKind.Sequential)]
    public struct CAPS {
        public ushort Usage; public ushort UsagePage;
        public ushort InLen; public ushort OutLen; public ushort FeatLen;
        public ushort R0,R1,R2,R3,R4,R5,R6,R7,R8,R9,R10,R11,R12,R13,R14,R15,R16;
        public ushort NLink; public ushort NInBtn; public ushort NInVal; public ushort NInIdx;
        public ushort NOutBtn; public ushort NOutVal; public ushort NOutIdx;
        public ushort NFeatBtn; public ushort NFeatVal; public ushort NFeatIdx;
    }
    [DllImport("hid.dll")] public static extern void HidD_GetHidGuid(out Guid g);
    [DllImport("hid.dll")] public static extern bool HidD_GetPreparsedData(IntPtr h, out IntPtr p);
    [DllImport("hid.dll")] public static extern bool HidD_FreePreparsedData(IntPtr p);
    [DllImport("hid.dll")] public static extern int HidP_GetCaps(IntPtr p, ref CAPS c);
    [DllImport("hid.dll")] public static extern bool HidD_GetFeature(IntPtr h, byte[] b, int n);
    [DllImport("hid.dll")] public static extern bool HidD_SetFeature(IntPtr h, byte[] b, int n);
    [DllImport("setupapi.dll", CharSet=CharSet.Auto)] public static extern IntPtr SetupDiGetClassDevs(ref Guid g, IntPtr e, IntPtr h, int f);
    [DllImport("setupapi.dll")] public static extern bool SetupDiEnumDeviceInterfaces(IntPtr d, IntPtr i, ref Guid g, int n, ref IFD data);
    [DllImport("setupapi.dll", CharSet=CharSet.Auto)] public static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr d, ref IFD data, IntPtr det, int sz, out int need, IntPtr info);
    [DllImport("setupapi.dll")] public static extern bool SetupDiDestroyDeviceInfoList(IntPtr d);
    [DllImport("kernel32.dll", CharSet=CharSet.Auto, SetLastError=true)] public static extern IntPtr CreateFile(string n, uint a, uint s, IntPtr sec, uint disp, uint fl, IntPtr t);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
"@
Add-Type -TypeDefinition $src

$g = New-Object Guid
[Hid]::HidD_GetHidGuid([ref]$g)
$set = [Hid]::SetupDiGetClassDevs([ref]$g, [IntPtr]::Zero, [IntPtr]::Zero, 0x12)
$i = 0
$hit = 0
while ($true) {
    $ifd = New-Object Hid+IFD
    $ifd.cbSize = 32
    if (-not [Hid]::SetupDiEnumDeviceInterfaces($set, [IntPtr]::Zero, [ref]$g, $i, [ref]$ifd)) { break }
    $i++
    $need = 0
    [Hid]::SetupDiGetDeviceInterfaceDetail($set, [ref]$ifd, [IntPtr]::Zero, 0, [ref]$need, [IntPtr]::Zero) | Out-Null
    $buf = [Runtime.InteropServices.Marshal]::AllocHGlobal($need)
    [Runtime.InteropServices.Marshal]::WriteInt32($buf, 8)
    $okd = [Hid]::SetupDiGetDeviceInterfaceDetail($set, [ref]$ifd, $buf, $need, [ref]$need, [IntPtr]::Zero)
    if (-not $okd) { [Runtime.InteropServices.Marshal]::FreeHGlobal($buf); continue }
    $path = [Runtime.InteropServices.Marshal]::PtrToStringAuto([IntPtr]::Add($buf, 4))
    [Runtime.InteropServices.Marshal]::FreeHGlobal($buf)
    if ($path -notmatch 'vid_1209.*pid_0001.*col04') { continue }
    $hit++

    $h = [Hid]::CreateFile($path, [uint32]3221225472, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($h -eq [IntPtr]::Zero -or $h -eq [IntPtr]::new(-1)) { Write-Host "打不开 COL04, err=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"; continue }

    $pp = [IntPtr]::Zero
    $caps = New-Object Hid+CAPS
    [Hid]::HidD_GetPreparsedData($h, [ref]$pp) | Out-Null
    [Hid]::HidP_GetCaps($pp, [ref]$caps) | Out-Null
    [Hid]::HidD_FreePreparsedData($pp) | Out-Null
    Write-Host ("COL04: UsagePage=0x{0:X4} In={1} Feat={2}" -f $caps.UsagePage, $caps.InLen, $caps.FeatLen)

    $len = $caps.FeatLen
    if ($len -le 0) { $len = 65 }

    $s1 = New-Object byte[] $len
    $s1[0] = 0x10; $s1[1] = 0x01
    $ok1 = [Hid]::HidD_SetFeature($h, $s1, $len)
    Write-Host ("SET 0x10 -> " + $(if($ok1){"OK"}else{"FAIL err=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error()}))
    Start-Sleep -Milliseconds 80

    $r1 = New-Object byte[] $len
    $r1[0] = 0x10
    $ok2 = [Hid]::HidD_GetFeature($h, $r1, $len)
    Write-Host ("GET 0x10 -> " + $(if($ok2){"OK"}else{"FAIL err=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error()}))
    if ($ok2) {
        $n = [Math]::Min(14, $len-1)
        $hex = (0..$n | ForEach-Object { "{0:X2}" -f $r1[$_] }) -join ' '
        Write-Host ("   bytes: " + $hex)
    }
    [Hid]::CloseHandle($h) | Out-Null
}
[Hid]::SetupDiDestroyDeviceInfoList($set) | Out-Null
Write-Host ("matched COL04 count = " + $hit)
