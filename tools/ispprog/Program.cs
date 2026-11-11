using System;
using System.IO;
using CHISP.Wrapper;
using System.Runtime.InteropServices;

/* MineKey CH552 烧录工具
 * 用途：绕开 WCHISPTool 的“写配置位”步骤，只写用户代码区。
 * 用法：
 *   dotnet run -c Release -- <hex路径> [noprotect|protect|remove]
 */
class Program
{
    /* v4.0 的 CHISPDeviceInfo 可能比封装里的更大，用原始缓冲区避免越界写崩 */
    [DllImport("CH375DLL.dll", CallingConvention = CallingConvention.Winapi)]
    static extern uint CH375OpenDevice(uint index);
    [DllImport("CH375DLL.dll", CallingConvention = CallingConvention.Winapi)]
    static extern uint CH375CloseDevice(uint index);
    [DllImport("CH375DLL.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Winapi)]
    static extern uint CH375GetDeviceName(uint index, byte[] name);

    [DllImport("WCH55xISPDLL.dll", EntryPoint = "WCH55x_EnumDevices", CallingConvention = CallingConvention.Winapi)]
    static extern uint RawEnumDevices(IntPtr buf, byte maxCnt,
        out byte chipSeries, out byte chipType, out int preBTV);

    static string[] argv2 = Array.Empty<string>();

    static void Main(string[] argv)
    {
        argv2 = argv;
        if (argv.Length > 1 && argv[1] == "raw") { RawDetect(); return; }
        if (argv.Length > 1 && argv[1] == "wtest") { RawWriteTest(); return; }
        if (argv.Length > 1 && argv[1] == "dtest") { RawDetectTest(); return; }

        string hexPath = argv.Length > 0 ? argv[0] : @"D:\Codes\Keyboard\firmware\minekey.hex";
        string mode = argv.Length > 1 ? argv[1] : "noprotect";

        var opt = new CHISPWrapper.CHISPOption();
        CHISPWrapper.InitISPOptionStruct(ref opt);
        opt.IspInterface = 0;        // USB
        opt.IspMcuSeries = 0;        // CH55X
        opt.IspMcuType = 0x52;       // CH552
        opt.IsBootLoader = 1;        // 从 BOOT 启动
        opt.IsMcuResetAfterIsp = 1;  // 下载后复位运行
        opt.IsEnableResetPin = 1;
        opt.IsEnableLongRest = 1;
        opt.IsEraseAllCFlash = 1;
        opt.IsCodeProtect = 1;       // 默认：与工具一致

        if (mode == "noprotect")
        {
            // 关键：不写保护位。芯片当前已是保护态，写它必然失败并导致整个下载中止。
            opt.IsCodeProtect = 0;
        }

        /* 先直接问底层驱动 */
        for (uint i = 0; i < 4; i++)
        {
            var nm = new byte[64];
            uint gn = CH375GetDeviceName(i, nm);
            uint h = CH375OpenDevice(i);
            string name = "";
            int z = Array.IndexOf(nm, (byte)0);
            if (z > 0) name = System.Text.Encoding.ASCII.GetString(nm, 0, z);
            Console.WriteLine($"    CH375 dev{i}: GetName={gn} name='{name}' Open={h}");
            if (h != 0) CH375CloseDevice(i);
        }

        int ok = CHISPWrapper.SetISPOption(in opt);
        Console.WriteLine($"[1] SetIspOption -> {ok} (mode={mode})");
        if (ok == 0) { Console.WriteLine("    设置下载参数失败，终止"); return; }

        int waitSec = 0;
        if (argv.Length > 2) int.TryParse(argv[2], out waitSec);

        const int DevInfoSize = 2048;      /* 单台设备信息给足 */
        IntPtr raw = Marshal.AllocHGlobal(DevInfoSize * 16);
        uint n = 0;
        byte series = 0, chipType = 0;
        int preBtv = 0;
        var t0 = DateTime.Now;
        try
        {
            while (true)
            {
                for (int k = 0; k < DevInfoSize * 16; k++) Marshal.WriteByte(raw, k, 0);
                n = RawEnumDevices(raw, 16, out series, out chipType, out preBtv);
                Console.Out.Flush();
                if (n > 0) break;
                if ((DateTime.Now - t0).TotalSeconds >= waitSec) break;
                System.Threading.Thread.Sleep(200);
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine("EnumDevices 异常: " + ex.Message);
            return;
        }
        Console.WriteLine($"[2] EnumDevices -> {n} 台  (等待 {(int)(DateTime.Now - t0).TotalSeconds}s)  series={series} type=0x{chipType:X2} preBTV230={preBtv}");
        for (uint i = 0; i < n && i < 16; i++)
        {
            int off = (int)i * DevInfoSize;
            uint idx = (uint)Marshal.ReadInt32(raw, off);
            byte mcu = Marshal.ReadByte(raw, off + 4);
            int cold = Marshal.ReadInt32(raw, off + 8 + 16 + 32);   /* 粗略位置，仅作参考 */
            Console.WriteLine($"    dev{i}: index={idx} mcu=0x{mcu:X2} (cold≈{cold})");
        }
        if (n == 0) { Console.WriteLine("    没有找到设备，请让板子处于 BootLoader（上电后约 2 秒内）"); return; }

        if (mode == "remove")
        {
            int rp = CHISPWrapper.RemoveProtect(0);
            Console.WriteLine($"[3] RemoveProtect -> {rp}");
            CHISPWrapper.ISPEnd(0, 0);
            return;
        }

        byte[] data = File.ReadAllBytes(hexPath);
        Console.WriteLine($"[3] 载入 {hexPath}  {data.Length} 字节（HEX 文本）");

        int rc = CHISPWrapper.FlashProgram(0, data, (uint)data.Length, 0, null, 0, 0);
        Console.WriteLine($"[4] FlashProgramB -> {rc}");

        if (rc != 0)
        {
            int vc = CHISPWrapper.FlashVerify(0, data, (uint)data.Length, 0, null, 0, 0);
            Console.WriteLine($"[5] FlashVerifyB  -> {vc}");
        }

        CHISPWrapper.ISPEnd(0, 0);
        Console.WriteLine(rc != 0 ? "结果：成功" : "结果：失败");
    }

    static void RawDetectTest()
    {
        string err = RawIsp.Open();
        Console.WriteLine(err == "" ? "[1] Open OK handle=" + RawIsp.Handle : "[1] " + err);
        if (err != "") return;
        RawIsp.DetectTest();
        RawIsp.Close();
    }

    static void RawWriteTest()
    {
        string err = RawIsp.Open();
        Console.WriteLine(err == "" ? "[1] Open OK, handle=" + RawIsp.Handle : "[1] " + err);
        if (err != "") return;
        RawIsp.WriteTest();
        RawIsp.Close();
    }

    /* ===== 自研底层通道：识别 + 读配置 ===== */
    static void RawDetect()
    {
        int waitS = 90;
        if (argv2.Length > 2) int.TryParse(argv2[2], out waitS);
        string err = RawIsp.Open();
        var t0 = DateTime.Now;
        while (err != "" && (DateTime.Now - t0).TotalSeconds < waitS)
        {
            System.Threading.Thread.Sleep(200);
            err = RawIsp.Open();
        }
        Console.WriteLine(err == "" ? $"[1] CH375OpenDevice(0) -> OK (等待 {(int)(DateTime.Now-t0).TotalSeconds}s)" : "[1] " + err);
        if (err != "") return;

        try
        {
            var (id, sub) = RawIsp.Detect(out int st);
            Console.WriteLine($"[2] Detect -> id=0x{id:X2} sub=0x{sub:X2} (status={st}, 0=OK)");
            if (id == 0) { Console.WriteLine("    没有识别到芯片（可能在跑用户程序，不在 BootLoader）"); return; }

            var cfg = RawIsp.ReadConfig(0x1F, out int st2);
            Console.WriteLine($"[3] ReadConfig -> status={st2}, 长度={cfg?.Length ?? -1}");
            if (cfg == null) return;

            int p = 0;
            int fields = cfg[p] | (cfg[p + 1] << 8); p += 2;
            Console.WriteLine($"    字段掩码 = 0x{fields:X4}");
            if ((fields & 0x01) != 0) { Console.WriteLine($"    CFG1 = 0x{cfg[p] | (cfg[p+1]<<8) | (cfg[p+2]<<16) | (cfg[p+3]<<24):X8}"); p += 4; }
            if ((fields & 0x02) != 0) { Console.WriteLine($"    CFG2 = 0x{cfg[p] | (cfg[p+1]<<8) | (cfg[p+2]<<16) | (cfg[p+3]<<24):X8}"); p += 4; }
            if ((fields & 0x04) != 0) { Console.WriteLine($"    CFG3 = 0x{cfg[p] | (cfg[p+1]<<8) | (cfg[p+2]<<16) | (cfg[p+3]<<24):X8}"); p += 4; }
            if ((fields & 0x08) != 0) { Console.WriteLine($"    BOOTVER = {cfg[p]:X2}.{cfg[p+1]:X2}.{cfg[p+2]:X2}"); p += 4; }
            if ((fields & 0x10) != 0)
            {
                var uid = new byte[8];
                Array.Copy(cfg, p, uid, 0, 8); p += 8;
                Console.WriteLine("    UID = " + BitConverter.ToString(uid));
            }
            Console.WriteLine("[4] 通信成功 —— 可以进入写入阶段");
        }
        finally { RawIsp.Close(); }
    }
}
