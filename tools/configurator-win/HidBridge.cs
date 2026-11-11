using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace MineKey;

/// <summary>一个 HID 设备（对应某个 top-level collection 的接口路径）。</summary>
public sealed class HidDevice : IDisposable
{
    public string Path { get; }
    public ushort VendorId { get; }
    public ushort ProductId { get; }
    public ushort UsagePage { get; }
    public ushort Usage { get; }
    public string ProductName { get; }

    public int InputReportLength { get; private set; } = 9;    // ID + 8（键盘/状态最大负载）
    public int FeatureReportLength { get; private set; } = 64; // ID + 63（固件 Feature 负载 63）

    FileStream? _stream;
    CancellationTokenSource? _cts;
    Task? _readTask;

    /// <summary>收到输入报文：reportId + 数据（不含 reportId）。</summary>
    public event Action<byte, byte[]>? ReportReceived;
    /// <summary>设备被拔出或读线程异常退出。</summary>
    public event Action? Disconnected;

    HidDevice(string path, ushort vid, ushort pid, ushort usagePage, ushort usage, string name)
    {
        Path = path; VendorId = vid; ProductId = pid; UsagePage = usagePage; Usage = usage; ProductName = name;
    }

    /* ===================== 枚举 ===================== */

    public static List<HidDevice> Enumerate(ushort? wantUsagePage = null)
    {
        var list = new List<HidDevice>();
        HidD_GetHidGuid(out Guid hidGuid);
        IntPtr set = SetupDiGetClassDevs(ref hidGuid, null, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == IntPtr.Zero || set == new IntPtr(-1)) return list;

        try
        {
            var did = new SP_DEVICE_INTERFACE_DATA();
            did.cbSize = Marshal.SizeOf<SP_DEVICE_INTERFACE_DATA>();

            for (uint i = 0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref hidGuid, i, ref did); i++)
            {
                if (!SetupDiGetDeviceInterfaceDetail(set, ref did, IntPtr.Zero, 0, out uint need, IntPtr.Zero))
                {
                    /* 这次调用**注定**返回 false：它只是来问需要多少字节。
                       原来靠 Marshal.GetLastWin32Error() == 122 判断，但下面
                       的 DllImport 没写 SetLastError = true，那里拿到的永远
                       是 0，于是每个设备都被 continue 掉 —— 枚举结果恒为空，
                       配置器一个设备都连不上。改用 need 本身判断，不依赖错误码。 */
                    if (need == 0) continue;
                }
                IntPtr detail = Marshal.AllocHGlobal((int)need);
                try
                {
                    // cbSize：x64 = 8（含 4 字节对齐），x86 = 6
                    Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);
                    if (!SetupDiGetDeviceInterfaceDetail(set, ref did, detail, need, out _, IntPtr.Zero)) continue;
                    string? path = Marshal.PtrToStringUni(detail + 4);
                    if (string.IsNullOrEmpty(path)) continue;

                    var dev = Probe(path!);
                    if (dev == null) continue;
                    if (wantUsagePage is ushort up && dev.UsagePage != up) continue;
                    list.Add(dev);
                }
                finally { Marshal.FreeHGlobal(detail); }
            }
        }
        finally { SetupDiDestroyDeviceInfoList(set); }

        return list;
    }

    /// <summary>打开设备拿属性；不是 HID 或打不开就返回 null。</summary>
    static HidDevice? Probe(string path)
    {
        IntPtr h = CreateFile(path, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (h == new IntPtr(-1)) return null;

        try
        {
            var attr = new HIDD_ATTRIBUTES { Size = Marshal.SizeOf<HIDD_ATTRIBUTES>() };
            if (!HidD_GetAttributes(h, ref attr)) return null;

            if (!HidD_GetPreparsedData(h, out IntPtr pp)) return null;
            var caps = new HIDP_CAPS();
            try
            {
                if (HidP_GetCaps(pp, ref caps) != HIDP_STATUS_SUCCESS) return null;
            }
            finally { HidD_FreePreparsedData(pp); }

            var sb = new StringBuilder(256);
            string name = HidD_GetProductString(h, sb, sb.Capacity * 2) ? sb.ToString() : "";

            var d = new HidDevice(path, attr.VendorID, attr.ProductID, caps.UsagePage, caps.Usage, name)
            {
                InputReportLength = caps.InputReportByteLength,
                FeatureReportLength = caps.FeatureReportByteLength
            };
            return d;
        }
        finally { CloseHandle(h); }
    }

    /* ===================== 打开 / 关闭 ===================== */

    public void Open()
    {
        _stream = new FileStream(Path, FileMode.Open, FileAccess.ReadWrite,
            FileShare.ReadWrite, 0, FileOptions.Asynchronous);

        // 用 OpenExisting 句柄重新取一次 caps（Probe 时是临时打开的）
        if (!HidD_GetPreparsedData(_stream.SafeFileHandle.DangerousGetHandle(), out IntPtr pp))
            throw new InvalidOperationException("无法读取 HID 能力描述");
        try
        {
            var caps = new HIDP_CAPS();
            if (HidP_GetCaps(pp, ref caps) == HIDP_STATUS_SUCCESS)
            {
                InputReportLength = caps.InputReportByteLength;
                FeatureReportLength = caps.FeatureReportByteLength;
            }
        }
        finally { HidD_FreePreparsedData(pp); }

        _cts = new CancellationTokenSource();
        _readTask = Task.Run(() => ReadLoop(_cts.Token));
    }

    void ReadLoop(CancellationToken ct)
    {
        var buf = new byte[Math.Max(InputReportLength, 1)];
        try
        {
            while (!ct.IsCancellationRequested && _stream != null)
            {
                int n = _stream.ReadAsync(buf, 0, buf.Length, ct).GetAwaiter().GetResult();
                if (n <= 0) break;
                byte id = buf[0];
                var payload = new byte[n - 1];
                Array.Copy(buf, 1, payload, 0, n - 1);
                ReportReceived?.Invoke(id, payload);
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception) { /* 拔出、句柄失效等都走这里 */ }
        finally
        {
            if (!ct.IsCancellationRequested) Disconnected?.Invoke();
        }
    }

    /* ===================== Feature 报告 ===================== */

    /// <summary>写 + 读一次 Feature 报告。data 是 63 字节负载（不含 reportId）。
    /// 缓冲长度必须等于固件报告描述符算出的 FeatureReportByteLength（= ID + 63 = 64），
    /// HidD_SetFeature/GetFeature 对长度不符的调用会直接失败。</summary>
    public byte[] FeatureExchange(byte reportId, byte[] data)
    {
        if (_stream == null) throw new InvalidOperationException("设备未打开");
        IntPtr h = _stream.SafeFileHandle.DangerousGetHandle();

        int len = FeatureReportLength > 0 ? FeatureReportLength : 64;
        if (data.Length + 1 > len)
            throw new ArgumentOutOfRangeException(nameof(data),
                $"负载 {data.Length} 字节超过固件 Feature 上限 {len - 1} 字节");

        var w = new byte[len];
        w[0] = reportId;
        Array.Copy(data, 0, w, 1, data.Length);

        if (!HidD_SetFeature(h, w, len))
            throw new InvalidOperationException("HID SetFeature 失败，Win32=" + Marshal.GetLastWin32Error());

        var r = new byte[len];
        r[0] = reportId;
        if (!HidD_GetFeature(h, r, len))
            throw new InvalidOperationException("HID GetFeature 失败，Win32=" + Marshal.GetLastWin32Error());

        return r;
    }

    /// <summary>清空输入缓冲（读超时最多等 timeoutMs）。</summary>
    public void Drain(int timeoutMs = 40)
    {
        // 简单策略：暂停读线程不可能，这里只是等待一会儿让积压报文被消费
        Thread.Sleep(timeoutMs);
    }

    public void Dispose()
    {
        try { _cts?.Cancel(); } catch { }
        try { _stream?.Dispose(); } catch { }
        _stream = null;
    }

    /* ===================== P/Invoke ===================== */

    const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
    const uint FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2;
    const uint OPEN_EXISTING = 3;
    const int DIGCF_PRESENT = 0x02, DIGCF_DEVICEINTERFACE = 0x10;
    const int ERROR_INSUFFICIENT_BUFFER = 122;
    const int HIDP_STATUS_SUCCESS = 0x00110000;

    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVICE_INTERFACE_DATA
    {
        public int cbSize;
        public Guid InterfaceClassGuid;
        public int Flags;
        public IntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct HIDD_ATTRIBUTES
    {
        public int Size;
        public ushort VendorID;
        public ushort ProductID;
        public ushort VersionNumber;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct HIDP_CAPS
    {
        public ushort Usage;
        public ushort UsagePage;
        public ushort InputReportByteLength;
        public ushort OutputReportByteLength;
        public ushort FeatureReportByteLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)] public ushort[] Reserved;
        public ushort NumberLinkCollectionNodes;
        public ushort NumberInputButtonCaps;
        public ushort NumberInputValueCaps;
        public ushort NumberInputDataIndices;
        public ushort NumberOutputButtonCaps;
        public ushort NumberOutputValueCaps;
        public ushort NumberOutputDataIndices;
        public ushort NumberFeatureButtonCaps;
        public ushort NumberFeatureValueCaps;
        public ushort NumberFeatureDataIndices;
    }

    [DllImport("hid.dll")] static extern void HidD_GetHidGuid(out Guid guid);
    [DllImport("hid.dll")] static extern bool HidD_GetAttributes(IntPtr h, ref HIDD_ATTRIBUTES attr);
    [DllImport("hid.dll")] static extern bool HidD_GetPreparsedData(IntPtr h, out IntPtr data);
    [DllImport("hid.dll")] static extern bool HidD_FreePreparsedData(IntPtr data);
    [DllImport("hid.dll")] static extern int HidP_GetCaps(IntPtr data, ref HIDP_CAPS caps);
    [DllImport("hid.dll", CharSet = CharSet.Unicode)]
    static extern bool HidD_GetProductString(IntPtr h, StringBuilder buf, int len);
    [DllImport("hid.dll")] static extern bool HidD_GetFeature(IntPtr h, byte[] buf, int len);
    [DllImport("hid.dll")] static extern bool HidD_SetFeature(IntPtr h, byte[] buf, int len);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr SetupDiGetClassDevs(ref Guid guid, string? enumerator, IntPtr hwndParent, int flags);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr devInfo, ref Guid guid, uint index, ref SP_DEVICE_INTERFACE_DATA data);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr set, ref SP_DEVICE_INTERFACE_DATA data,
        IntPtr detail, uint detailSize, out uint required, IntPtr devInfo);
    [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFile(string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr templ);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);
}
