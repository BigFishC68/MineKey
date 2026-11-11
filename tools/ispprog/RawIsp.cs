using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

/* CH55x BootLoader 底层驱动（走 CH375DLL 的 bulk 端点，自己实现协议）
 * 协议参考开源实现 ch55xtool（MarsTechHAN/ch552tool，GPL-3.0）：
 *   命令包 = cmd(1) + len(2, LE) + payload
 *   数据走 EP 0x02(OUT) / 0x82(IN)
 */
static class RawIsp
{
    const uint EP_OUT = 0x02, EP_IN = 0x82;
    const int PKT = 64;

    [DllImport("CH375DLL.dll")] static extern uint CH375OpenDevice(uint i);
    [DllImport("CH375DLL.dll")] static extern void CH375CloseDevice(uint i);
    [DllImport("CH375DLL.dll")] static extern void CH375SetTimeout(uint i, uint w, uint r);
    [DllImport("CH375DLL.dll")] static extern uint CH375WriteEndP(uint i, uint ep, byte[] buf, ref uint len);
    [DllImport("CH375DLL.dll")] static extern uint CH375ReadEndP(uint i, uint ep, byte[] buf, ref uint len);

    public const byte CMD_DETECT = 0xA1, CMD_END = 0xA2, CMD_SETKEY = 0xA3,
                      CMD_ERASE = 0xA4, CMD_WRITE = 0xA5, CMD_VERIFY = 0xA6,
                      CMD_RDCFG = 0xA7;

    public static bool Opened;
    public static uint Handle;

    public static string Open()
    {
        uint h = CH375OpenDevice(0);
        Handle = h;
        if (h == 0 || h == 0xFFFFFFFF) return $"CH375OpenDevice 失败 (0x{h:X})";
        CH375SetTimeout(0, 2000, 3000);
        Opened = true;
        return "";
    }

    /// <summary>调试：用不同首参/长度试发一包，打印返回码</summary>
    public static void WriteTest()
    {
        var pkt = new byte[64];
        byte[] cmd = { 0xA1, 0x11, 0x00 };
        Array.Copy(cmd, 0, pkt, 0, cmd.Length);
        System.Text.Encoding.ASCII.GetBytes("MCU ISP & WCH.CN").CopyTo(pkt, 3);

        foreach (var first in new uint[] { 0, Handle })
        foreach (var len in new uint[] { 3 + 15, 64 })
        {
            uint l = len;
            uint r = CH375WriteEndP(first, EP_OUT, pkt, ref l);
            Console.WriteLine($"    WriteEndP(first={first}, ep=0x{EP_OUT:X2}, len={len}) -> {r} (实际 {l})");
        }
        foreach (var first in new uint[] { 0, Handle })
        {
            var buf = new byte[64];
            uint l = 64;
            uint r = CH375ReadEndP(first, EP_IN, buf, ref l);
            Console.WriteLine($"    ReadEndP (first={first}, ep=0x{EP_IN:X2}) -> {r} len={l} data={BitConverter.ToString(buf, 0, 8)}");
        }
    }

    /// <summary>调试：发 Detect，然后多端点多次尝试读</summary>
    public static void DetectTest()
    {
        var pkt = new byte[64];
        pkt[0] = 0xA1; pkt[1] = 0x11; pkt[2] = 0x00;
        System.Text.Encoding.ASCII.GetBytes("MCU ISP & WCH.CN").CopyTo(pkt, 3);
        uint wl = 64;
        uint wr = CH375WriteEndP(0, EP_OUT, pkt, ref wl);
        Console.WriteLine($"    写 Detect(64B) -> {wr}");

        foreach (uint ep in new uint[] { 0x82, 0x81, 0x02 })
        {
            for (int i = 0; i < 3; i++)
            {
                System.Threading.Thread.Sleep(150);
                var buf = new byte[64];
                uint rl = 64;
                uint rr = CH375ReadEndP(0, ep, buf, ref rl);
                Console.WriteLine($"    读 EP 0x{ep:X2} 第{i+1}次 -> 返回{rr} len={rl} 前8字节={BitConverter.ToString(buf, 0, 8)}");
                if (rr != 0 && rl > 0) return;
            }
        }
    }

    public static void Close()
    {
        if (Opened) { CH375CloseDevice(0); Opened = false; }
    }

    /// <summary>发一条命令并取回复。返回 (回复状态字节, 回复负载)，失败返回 null。</summary>
    public static byte[]? Exec(byte cmd, byte[] payload, out int status)
    {
        status = -1;
        var pkt = new byte[3 + payload.Length];
        pkt[0] = cmd;
        pkt[1] = (byte)(payload.Length & 0xFF);
        pkt[2] = (byte)((payload.Length >> 8) & 0xFF);
        Array.Copy(payload, 0, pkt, 3, payload.Length);

        uint wl = (uint)pkt.Length;
        /* 注意：CH375 API 的返回值是 TRUE(1)=成功，FALSE(0)=失败 */
        uint wr = CH375WriteEndP(0, EP_OUT, pkt, ref wl);
        if (wr == 0) { status = -2; return null; }

        var buf = new byte[PKT + 8];
        uint rl = PKT;
        uint rr = CH375ReadEndP(0, EP_IN, buf, ref rl);
        if (rr == 0) { status = -3; return null; }
        if (rl < 4) { status = -4; return null; }
        if (buf[0] != cmd) { status = -5; return null; }

        status = buf[1];
        int plen = buf[2] | (buf[3] << 8);
        if (plen == 0) return Array.Empty<byte>();
        if (plen > rl - 4) plen = (int)rl - 4;
        var pl = new byte[plen];
        Array.Copy(buf, 4, pl, 0, plen);
        return pl;
    }

    /// <summary>识别芯片，返回 chip_id / chip_subid</summary>
    public static (byte id, byte sub) Detect(out int st)
    {
        var pl = new List<byte> { 0x42, 0x10 };
        pl.AddRange(System.Text.Encoding.ASCII.GetBytes("MCU ISP & WCH.CN"));
        var r = Exec(CMD_DETECT, pl.ToArray(), out st);
        if (r == null || r.Length < 2) return (0, 0);
        return (r[0], r[1]);
    }

    /// <summary>读配置（含 UID）</summary>
    public static byte[]? ReadConfig(byte fields, out int st)
        => Exec(CMD_RDCFG, new byte[] { fields, 0x00 }, out st);

    /// <summary>设置密钥</summary>
    public static bool SetKey(byte[] keyBase)
    {
        var r = Exec(CMD_SETKEY, keyBase, out int st);
        return r != null && r.Length > 0 && r[0] == 0;
    }

    public static bool EraseFlash(int pageCount)
    {
        var pl = new byte[4];
        pl[0] = (byte)(pageCount & 0xFF);
        pl[1] = (byte)((pageCount >> 8) & 0xFF);
        pl[2] = (byte)((pageCount >> 16) & 0xFF);
        pl[3] = (byte)((pageCount >> 24) & 0xFF);
        var r = Exec(CMD_ERASE, pl, out int st);
        return r != null && r.Length > 0 && r[0] == 0;
    }

    /// <summary>写入 / 校验数据块，data 会用 key 做异或</summary>
    public static bool FlashOps(byte cmd, byte[] key, byte[] data, int offset, int length)
    {
        const int MAXDATA = PKT - 3 - 5;           /* 56 */
        int curr = 0;
        var rnd = new Random(12345);
        var buff = new byte[MAXDATA + 8];

        while (curr < length)
        {
            int left = length - curr;
            int n = left > MAXDATA ? MAXDATA : left;
            Array.Clear(buff, 0, buff.Length);
            Array.Copy(data, offset + curr, buff, 0, n);
            if (n == left)
            {
                int rem = n & 0x07;
                if (rem != 0) n += 8 - rem;        /* 补齐到 8 的倍数 */
            }
            for (int i = 0; i < n; i++) buff[i] ^= key[i & 0x07];

            var pl = new byte[5 + n];
            int addr = curr;
            pl[0] = (byte)(addr & 0xFF);
            pl[1] = (byte)((addr >> 8) & 0xFF);
            pl[2] = (byte)((addr >> 16) & 0xFF);
            pl[3] = (byte)((addr >> 24) & 0xFF);
            pl[4] = (byte)rnd.Next(256);
            Array.Copy(buff, 0, pl, 5, n);

            var r = Exec(cmd, pl, out int st);
            if (r == null || r.Length == 0 || r[0] != 0) return false;
            curr += n;
        }
        return true;
    }

    public static void End(bool run)
    {
        Exec(CMD_END, new byte[] { (byte)(run ? 1 : 0) }, out _);
    }

    /// <summary>按芯片 UID 生成密钥（与 ch55xtool 相同）</summary>
    public static (byte[] key, byte[] keyBase) GenKey(byte chipId, byte uidChecksum)
    {
        var rnd = new Random();
        int len = 30 + rnd.Next(32);
        var kb = new byte[len];
        rnd.NextBytes(kb);
        var k = new byte[8];
        k[0] = (byte)(uidChecksum ^ kb[4 * (len / 7)]);
        k[1] = (byte)(uidChecksum ^ kb[len / 5]);
        k[2] = (byte)(uidChecksum ^ kb[len / 7]);
        k[3] = (byte)(uidChecksum ^ kb[6 * (len / 7)]);
        k[4] = (byte)(uidChecksum ^ kb[3 * (len / 7)]);
        k[5] = (byte)(uidChecksum ^ kb[3 * (len / 5)]);
        k[6] = (byte)(uidChecksum ^ kb[5 * (len / 7)]);
        k[7] = (byte)((chipId + k[0]) & 0xFF);
        return (k, kb);
    }
}
