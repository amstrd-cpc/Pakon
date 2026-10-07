# pakon_boot_reference.ps1 — FROZEN REFERENCE FIXTURE (2026-10-07)
#
# The exact procedure that successfully booted the lab unit from cold
# 0f05:f235 (REV AA07) to warm 0f05:f135 (REV 0002). Its full transcript
# and per-step evidence markers are documented in docs/BOOT_CHAIN.md.
#
# DO NOT EDIT THE LOGIC. This file is the specification the C++ loader
# port (docs/LOADER_DESIGN.md) must reproduce request-for-request. If a
# change is ever required, that is a design change: update docs, get
# explicit approval, and re-validate on hardware before freezing again.
#
# Properties (by construction):
#   - host-side gates before ANY USB operation (MD5 of both artifacts,
#     stage-1 shape 360/4476 + terminator, Pakon7 shape
#     728/10326 = 709/10128 + 19/198, max addr 0x478F, addr < 0xC000);
#   - gates abort (print + return) — never retry;
#   - exactly one gated 0xA9 read per run;
#   - every write is volatile (FX2 RAM + halt/run registers only) —
#     unplug/replug restores the cold state;
#   - no EEPROM/I2C, no bootloaders, no type-byte-0 frames, no bulk.
#
# Usage (cold device attached, from the repo root):
#   powershell -ExecutionPolicy Bypass -File .\tools\pakon_boot_reference.ps1

$src = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class PakonBoot
{
    const uint DIGCF_PRESENT = 0x2;
    const uint DIGCF_DEVICEINTERFACE = 0x10;
    static readonly Guid GuidWinUsb = new Guid("0e9e6f29-e70a-4582-8d02-bde3ad701252");

    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVICE_INTERFACE_DATA
    {
        public int cbSize;
        public Guid InterfaceClassGuid;
        public int Flags;
        public IntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WINUSB_SETUP_PACKET
    {
        public byte RequestType;
        public byte Request;
        public ushort Value;
        public ushort Index;
        public ushort Length;
    }

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr SetupDiGetClassDevs(ref Guid ClassGuid, IntPtr Enumerator, IntPtr hwndParent, uint Flags);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiEnumDeviceInterfaces(IntPtr DeviceInfoSet, IntPtr DeviceInfoData, ref Guid InterfaceClassGuid, uint MemberIndex, ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr DeviceInfoSet, ref SP_DEVICE_INTERFACE_DATA DeviceInterfaceData, IntPtr DeviceInterfaceDetailData, uint DeviceInterfaceDetailDataSize, out uint RequiredSize, IntPtr DeviceInfoData);

    [DllImport("setupapi.dll")]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFile(string lpFileName, uint dwDesiredAccess, uint dwShareMode, IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr hObject);

    [DllImport("winusb.dll", SetLastError = true, ExactSpelling = true)]
    static extern bool WinUsb_Initialize(IntPtr DeviceHandle, out IntPtr InterfaceHandle);

    [DllImport("winusb.dll", SetLastError = true, ExactSpelling = true)]
    static extern bool WinUsb_ControlTransfer(IntPtr InterfaceHandle, WINUSB_SETUP_PACKET SetupPacket, byte[] Buffer, uint BufferLength, out uint LengthTransferred, IntPtr Overlapped);

    [DllImport("winusb.dll", SetLastError = true, ExactSpelling = true)]
    static extern bool WinUsb_Free(IntPtr InterfaceHandle);

    static string FindPath()
    {
        Guid g = GuidWinUsb;
        IntPtr set = SetupDiGetClassDevs(ref g, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == (IntPtr)(-1)) return null;
        string found = null;
        for (uint i = 0; ; i++)
        {
            var did = new SP_DEVICE_INTERFACE_DATA();
            did.cbSize = Marshal.SizeOf(typeof(SP_DEVICE_INTERFACE_DATA));
            if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref g, i, ref did)) break;
            uint need;
            SetupDiGetDeviceInterfaceDetail(set, ref did, IntPtr.Zero, 0, out need, IntPtr.Zero);
            if (need == 0) continue;
            IntPtr p = Marshal.AllocHGlobal((int)need);
            Marshal.WriteInt32(p, IntPtr.Size == 8 ? 8 : 6);
            string path = null;
            if (SetupDiGetDeviceInterfaceDetail(set, ref did, p, need, out need, IntPtr.Zero))
                path = Marshal.PtrToStringUni(new IntPtr(p.ToInt64() + 4));
            Marshal.FreeHGlobal(p);
            if (path != null && path.IndexOf("VID_0F05&PID_F235", StringComparison.OrdinalIgnoreCase) >= 0)
            {
                found = path;
                break;
            }
        }
        SetupDiDestroyDeviceInfoList(set);
        return found;
    }

    static string Err()
    {
        int e = Marshal.GetLastWin32Error();
        return "FAIL win32=" + e + " (" + new System.ComponentModel.Win32Exception(e).Message + ")";
    }

    static bool VendorOut(IntPtr w, byte bReq, ushort wValue, byte[] data, uint len, out string err)
    {
        var p = new WINUSB_SETUP_PACKET { RequestType = 0x40, Request = bReq, Value = wValue, Index = 0, Length = (ushort)len };
        uint n;
        bool ok = WinUsb_ControlTransfer(w, p, data, len, out n, IntPtr.Zero);
        err = ok ? null : Err();
        return ok;
    }

    static bool CpuCtl(IntPtr w, byte val, out string err)
    {
        if (!VendorOut(w, 0xA0, 0x7F92, new byte[] { val }, 1, out err)) return false;
        return VendorOut(w, 0xA0, 0xE600, new byte[] { val }, 1, out err);
    }

    static bool SendPhase(IntPtr w, byte[] r, byte bReq, bool highPhase, out int sent, out string err)
    {
        sent = 0;
        err = null;
        int n = 0;
        while (n * 0x16 + 0x16 <= r.Length && r[n * 0x16 + 4] == 0)
        {
            int o = n * 0x16;
            ushort addr = (ushort)(r[o + 2] | (r[o + 3] << 8));
            bool isHigh = addr > 0x1B3F;
            if (isHigh == highPhase)
            {
                byte ln = r[o];
                byte[] data = new byte[ln];
                Array.Copy(r, o + 5, data, 0, ln);
                if (!VendorOut(w, bReq, addr, data, (uint)ln, out err)) return false;
                sent++;
            }
            n++;
        }
        return true;
    }

    public static string Run(byte[] r, byte[] f)
    {
        StringBuilder sb = new StringBuilder();
        string path = FindPath();
        if (path == null)
            return "STEP0  no WinUSB interface for VID_0F05&PID_F235 found";
        sb.AppendLine("STEP0  path = " + path);
        IntPtr h = CreateFile(path, 0xC0000000u, 3u, IntPtr.Zero, 3u, 0x40000000u, IntPtr.Zero);
        if (h == (IntPtr)(-1))
            return sb.ToString() + "STEP0  CreateFile FAILED " + Err();
        IntPtr w;
        if (!WinUsb_Initialize(h, out w))
        {
            string rr = sb.ToString() + "STEP0  WinUsb_Initialize FAILED " + Err();
            CloseHandle(h);
            return rr;
        }
        sb.AppendLine("STEP0  opened OK (R/W, share=3, OPEN_EXISTING, FILE_FLAG_OVERLAPPED)");

        bool alive = true;
        string err;

        alive = CpuCtl(w, 1, out err);
        sb.AppendLine(alive ? "STEP1  CPUCS halt pair (7F92 then E600, data 01) -> OK"
                            : "STEP1  CPUCS halt pair -> " + err);

        if (alive)
        {
            alive = CpuCtl(w, 1, out err);
            sb.AppendLine(alive ? "STEP2  in-downloader re-halt pair -> OK" : "STEP2  re-halt pair -> " + err);
        }

        if (alive)
        {
            int i = 0;
            while (alive && i < 360 && r[i * 0x16 + 4] == 0)
            {
                int o = i * 0x16;
                ushort addr = (ushort)(r[o + 2] | (r[o + 3] << 8));
                byte ln = r[o];
                byte[] data = new byte[ln];
                Array.Copy(r, o + 5, data, 0, ln);
                alive = VendorOut(w, 0xA0, addr, data, (uint)ln, out err);
                if (!alive)
                    sb.AppendLine("STEP3  FAIL at record " + i + " (addr 0x" + addr.ToString("X4") + ", len " + ln + ") -> " + err);
                i++;
            }
            if (alive && i == 360)
                sb.AppendLine("STEP3  360 x 0xA0 stage-1 write -> OK (4476 bytes)");
            else if (alive)
            {
                sb.AppendLine("STEP3  early terminator at record " + i + " -> aborted");
                alive = false;
            }
        }

        if (alive)
        {
            alive = CpuCtl(w, 0, out err);
            sb.AppendLine(alive ? "STEP4  CPUCS run pair (data 00) -> OK" : "STEP4  CPUCS run pair -> " + err);
        }

        if (alive)
        {
            alive = VendorOut(w, 0xA4, 0x00A1, null, 0, out err);
            sb.AppendLine(alive ? "STEP5  40 A4 00A1 0 0 -> OK" : "STEP5  40 A4 00A1 0 0 -> " + err);
        }

        if (alive)
        {
            var p6 = new WINUSB_SETUP_PACKET { RequestType = 0xC0, Request = 0xA9, Value = 0, Index = 0, Length = 8 };
            byte[] buf = new byte[8];
            uint n6;
            bool ok6 = WinUsb_ControlTransfer(w, p6, buf, 8, out n6, IntPtr.Zero);
            string hx = ok6 ? BitConverter.ToString(buf, 0, (int)n6) : "(no data)";
            bool good = ok6 && n6 == 8 && buf[0] == 0xC0 && buf[1] == 0x05 && buf[2] == 0x0F
                        && buf[3] == 0x35 && buf[4] == 0xF2 && buf[5] == 0x07 && buf[6] == 0xAA;
            sb.AppendLine(good
                ? "STEP6  0xA9 verified: " + hx + "  (tag C0, 0F05:F235 REV AA07 -> F235_AA07 -> Pakon7.hex)"
                : (ok6 ? "STEP6  0xA9 UNEXPECTED: " + hx + " -> gate closed"
                       : "STEP6  0xA9 -> " + Err() + " -> gate closed"));
            if (!good)
            {
                sb.AppendLine("STEP7-10 skipped: 0xA9 gate not passed - no file download, no retry");
                alive = false;
            }
        }

        if (alive)
        {
            int sent;
            alive = SendPhase(w, f, 0xA3, true, out sent, out err);
            sb.AppendLine(alive ? "STEP7  phase1: " + sent + " x 0xA3 (addr > 0x1B3F, CPU running) -> OK"
                                : "STEP7  phase1 FAIL after " + sent + " records -> " + err);
        }

        if (alive)
        {
            alive = CpuCtl(w, 1, out err);
            sb.AppendLine(alive ? "STEP8  CPUCS halt pair before phase2 -> OK" : "STEP8  halt pair -> " + err);
        }

        if (alive)
        {
            int sent;
            alive = SendPhase(w, f, 0xA0, false, out sent, out err);
            sb.AppendLine(alive ? "STEP9  phase2: " + sent + " x 0xA0 (addr <= 0x1B3F, halted) -> OK"
                                : "STEP9  phase2 FAIL after " + sent + " records -> " + err);
        }

        if (alive)
        {
            alive = CpuCtl(w, 1, out err);
            sb.AppendLine(alive ? "STEP10 final halt pair -> OK" : "STEP10 final halt -> " + err);
        }

        if (alive)
        {
            alive = CpuCtl(w, 0, out err);
            sb.AppendLine(alive ? "STEP11 final run pair -> OK (Pakon7 executing; renumeration expected now)"
                                : "STEP11 final run -> " + err);
        }

        if (!alive)
            sb.AppendLine("BOOT SEQUENCE STOPPED - see failing step above; no retry");

        WinUsb_Free(w);
        CloseHandle(h);
        return sb.ToString().TrimEnd();
    }
}
'@

& {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $u = 'https://raw.githubusercontent.com/plonsker/pakon-scanning-software/master/Pakon%20Update/fx35install/program%20files/Pakon/F-135/F135Driver/'
    $t1 = Join-Path $env:TEMP ('ldr_' + [guid]::NewGuid().ToString('N') + '.sys')
    $t2 = Join-Path $env:TEMP ('pak7_' + [guid]::NewGuid().ToString('N') + '.hex')
    try {
        (New-Object System.Net.WebClient).DownloadFile($u + 'F235Ldr.sys', $t1)
        (New-Object System.Net.WebClient).DownloadFile($u + 'Pakon7.hex', $t2)
    } catch { "STEP-A  FETCH FAILED: $($_.Exception.Message)"; return }

    $h1 = (Get-FileHash -Algorithm MD5 -Path $t1).Hash.ToLower()
    $h2 = (Get-FileHash -Algorithm MD5 -Path $t2).Hash.ToLower()
    $ldr = [IO.File]::ReadAllBytes($t1); Remove-Item $t1 -EA SilentlyContinue
    $hexText = [IO.File]::ReadAllText($t2); Remove-Item $t2 -EA SilentlyContinue
    if ($h1 -ne '6be781bad6fb9cf7ade4095baed336e0') { "STEP-A  F235Ldr.sys MD5 MISMATCH: $h1"; return }
    if ($h2 -ne '07f5001a951c4be179008d948be82b69') { "STEP-A  Pakon7.hex MD5 MISMATCH: $h2"; return }

    # --- stage-1 record buffer: F235Ldr .data, VA 0x11700..0x135EF, terminator 0x135F0 ---
    [byte[]]$s1 = $ldr[0x1700..0x35EF]
    [byte[]]$s1t = $ldr[0x35F0..0x3605]
    $pay = 0
    for ($i = 0; $i -lt 360; $i++) {
        $o = $i * 22; $ln = $s1[$o]
        if ($s1[$o+1] -ne 0 -or $s1[$o+4] -ne 0 -or $ln -lt 1 -or $ln -gt 16) { "STEP-A  stage1 record $i invalid"; return }
        $a = ([int]$s1[$o+2]) + (([int]$s1[$o+3]) * 256)
        if ($a -gt 0x1B3F) { "STEP-A  stage1 record $i addr > 0x1B3F"; return }
        $pay += $ln
    }
    if ($pay -ne 4476 -or $s1t[0] -ne 0 -or $s1t[4] -eq 0) { "STEP-A  stage1 sanity failed"; return }

    # --- Pakon7.hex -> OEM record buffer (stride 0x16: len,0,addrLE,flag,data) ---
    $fb = New-Object System.Collections.Generic.List[byte]
    $base = 0; $fpay = 0; $n = 0; $hi = 0; $hipay = 0; $lo = 0; $lop = 0; $maxA = 0
    foreach ($line in ($hexText -split "`r?`n")) {
        $line = $line.Trim()
        if (-not $line) { continue }
        if ($line[0] -ne ':') { "STEP-A  Pakon7: line does not start with ':'"; return }
        $nb = New-Object byte[] ([int](($line.Length - 1) / 2))
        for ($k = 0; $k -lt $nb.Length; $k++) { $nb[$k] = [Convert]::ToByte($line.Substring(1 + $k*2, 2), 16) }
        $cnt = $nb[0]; $la = (([int]$nb[1]) * 256) + ([int]$nb[2]); $typ = $nb[3]
        if ($typ -eq 0) {
            if ($cnt -lt 1 -or $cnt -gt 16) { "STEP-A  HEX record len $cnt out of range"; return }
            $a = $base + $la
            if ($a -ge 0xC000) { "STEP-A  unsafe addr 0x$([Convert]::ToString($a,16)) (>= 0xC000 incl. CPUCS)"; return }
            $alo = ([int]$a) -band 0xFF
            $ahi = ([int]$a) -shr 8
            $fb.Add([byte]$cnt); $fb.Add([byte]0); $fb.Add([byte]$alo); $fb.Add([byte]$ahi); $fb.Add([byte]0)
            for ($k = 0; $k -lt $cnt; $k++) { $fb.Add($nb[4 + $k]) }
            while (($fb.Count % 22) -ne 0) { $fb.Add([byte]0) }
            $fpay += $cnt; $n++
            if ($a -gt $maxA) { $maxA = $a }
            if ($a -gt 0x1B3F) { $hi++; $hipay += $cnt } else { $lo++; $lop += $cnt }
        }
        elseif ($typ -eq 1) { }
        else { "STEP-A  unexpected HEX record type $typ"; return }
    }
    $termStart = $fb.Count
    for ($k = 0; $k -lt 22; $k++) { $fb.Add([byte]0) }
    $fb[$termStart + 4] = 1
    if ($n -ne 728 -or $fpay -ne 10326 -or $hi -ne 709 -or $hipay -ne 10128 -or $lo -ne 19 -or $lop -ne 198 -or $maxA -ne 0x478F) {
        "STEP-A  Pakon7 shape mismatch: n=$n pay=$fpay hi=$hi/$hipay lo=$lo/$lop max=0x$([Convert]::ToString($maxA,16))"; return }
    [byte[]]$fileRecs = $fb.ToArray()
    "STEP-A  images OK: stage1 360 rec/4476 B; Pakon7 728 rec/10326 B (phase1 709/10128, phase2 19/198, max 0x478F); terminators verified"

    if (-not ('PakonBoot' -as [type])) { Add-Type -TypeDefinition $src }
    [PakonBoot]::Run($s1, $fileRecs)

    "--- post-boot enumeration poll (up to 15 s) ---"
    $f135 = $null
    $seen = @()
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 500
        $seen = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                  Where-Object { $_.InstanceId -like 'USB\VID_0F05*' -or $_.InstanceId -like 'USB\VID_04B4*' })
        $f135 = @($seen | Where-Object { $_.InstanceId -like '*PID_F135*' })
        if ($f135.Count -gt 0) { break }
    }
    if ($seen.Count -eq 0) { "post-boot: NO 0F05/04B4 USB device present (device did not re-enumerate yet)" }
    foreach ($d in $seen) {
        $svc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Service' -EA SilentlyContinue).Data
        "post-boot: {0}  status={1} class={2} service={3}" -f $d.InstanceId, $d.Status, $d.Class, $svc
    }
    if ($f135.Count -eq 0) {
        "post-boot: no 0F05:F135 appeared within 15 s - paste everything anyway; DO NOT replug-and-rerun yet"
    }
}
