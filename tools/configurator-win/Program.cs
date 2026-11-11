using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

namespace MineKey;

static class Program
{
    const string HostName = "keyforge.local";
    static WebView2? web;
    static Form? form;
    static HidDevice? current;
    static string? shotPath;
    static string? selftestPath;
    static string? extraQuery;
    static string? evalJs;
    static string? sizeArg;
    static bool demoMode;

    [STAThread]
    static void Main(string[] args)
    {
        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--shot" && i + 1 < args.Length) shotPath = args[++i];
            else if (args[i] == "--selftest" && i + 1 < args.Length) selftestPath = args[++i];
            else if (args[i] == "--demo") demoMode = true;
            else if (args[i] == "--query" && i + 1 < args.Length) extraQuery = args[++i];
            else if (args[i] == "--eval" && i + 1 < args.Length) evalJs = args[++i];
            else if (args[i] == "--size" && i + 1 < args.Length) sizeArg = args[++i];
        }

        ApplicationConfiguration.Initialize();

        form = new Form
        {
            Text = "MineKey",
            FormBorderStyle = FormBorderStyle.Sizable,   // 可拖拽缩放 / 可最大化
            MaximizeBox = true,
            MinimumSize = new Size(760, 620),
            ClientSize = new Size(900, 960),
            StartPosition = FormStartPosition.CenterScreen,
            BackColor = Color.FromArgb(0x2b, 0x2b, 0x2b),
            Icon = LoadIcon()
        };

        web = new WebView2
        {
            Dock = DockStyle.Fill,
            DefaultBackgroundColor = Color.FromArgb(0x2b, 0x2b, 0x2b)
        };
        form.Controls.Add(web);

        // 窗口尺寸：优先用上次退出时记住的，没有才按 DPI 换算设计尺寸
        _ = form.Handle;
        float s = form.DeviceDpi > 0 ? form.DeviceDpi / 96f : 1f;
        if (s <= 0) s = 1f;
        ApplySavedBounds(form, s);

        if (sizeArg != null)
        {
            var parts = sizeArg.ToLowerInvariant().Split('x');
            if (parts.Length == 2 && int.TryParse(parts[0], out int sw) && int.TryParse(parts[1], out int sh))
                form.ClientSize = new Size(sw, sh);
        }

        form.Shown += async (_, _) => await InitAsync();
        // FormClosing 里窗口还在，Bounds/WindowState 都是真实值；FormClosed 后 RestoreBounds 可能已失效
        form.FormClosing += (_, _) => SaveBounds(form);
        form.FormClosed += (_, _) => { current?.Dispose(); };
        Application.Run(form);
    }

    /* ---------------- 窗口尺寸记忆 ---------------- */
    /* 存 %LOCALAPPDATA%\MineKey\window.json，只用 ClientSize（不含边框），
       换显示器/改缩放后尺寸可能放不下，恢复时夹回工作区。 */
    static string StateFile => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "MineKey", "window.json");

    static void ApplySavedBounds(Form f, float scale)
    {
        var wa = Screen.FromControl(f).WorkingArea;

        if (shotPath == null && selftestPath == null)
        {
            try
            {
                if (File.Exists(StateFile))
                {
                    using var doc = JsonDocument.Parse(File.ReadAllText(StateFile));
                    var r = doc.RootElement;
                    int ver = r.TryGetProperty("v", out var ve) ? ve.GetInt32() : 1;
                    int w = r.TryGetProperty("w", out var we) ? we.GetInt32() : 0;
                    int h = r.TryGetProperty("h", out var he) ? he.GetInt32() : 0;
                    // 存的是 CSS 逻辑尺寸，恢复时按当前屏的缩放换算，换显示器后视觉大小才不会跳
                    if (ver == 2 && w >= 480 && h >= 360)
                    {
                        int dw = (int)Math.Round(w * scale);
                        int dh = (int)Math.Round(h * scale);
                        f.ClientSize = new Size(Math.Min(dw, wa.Width - 20), Math.Min(dh, wa.Height - 20));
                        if (r.TryGetProperty("max", out var me) && me.GetBoolean())
                            f.WindowState = FormWindowState.Maximized;
                        return;
                    }
                }
            }
            catch { /* 文件损坏就当没存过，别让记忆功能挡住启动 */ }
        }

        int wantW = (int)Math.Round(900 * scale);
        int wantH = (int)Math.Round(1000 * scale);
        f.ClientSize = new Size(Math.Min(wantW, wa.Width - 60), Math.Min(wantH, wa.Height - 60));
    }

    static void SaveBounds(Form f)
    {
        if (shotPath != null || selftestPath != null) return;   // 测试运行不污染用户设置
        try
        {
            bool max = f.WindowState == FormWindowState.Maximized;
            var b = max ? f.RestoreBounds : f.Bounds;           // 最大化时记还原后的尺寸
            int dw = f.Width - f.ClientSize.Width;
            int dh = f.Height - f.ClientSize.Height;
            float sc = f.DeviceDpi > 0 ? f.DeviceDpi / 96f : 1f;
            if (sc <= 0) sc = 1f;
            Directory.CreateDirectory(Path.GetDirectoryName(StateFile)!);
            // 存 CSS 逻辑尺寸（除以当前缩放），换到缩放不同的显示器上打开时大小才不会跳
            File.WriteAllText(StateFile, JsonSerializer.Serialize(new
            {
                v = 2,
                w = Math.Max(0, (int)Math.Round((b.Width - dw) / sc)),
                h = Math.Max(0, (int)Math.Round((b.Height - dh) / sc)),
                max
            }));
        }
        catch { }
    }

    /* CH552 出厂 BootLoader 的 VID/PID，Windows 不会给它装驱动，
       所以只能从注册表查（HID 枚举看不到它） */
    static bool BootloaderPresent()
    {
        try
        {
            using var k = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(
                @"SYSTEM\CurrentControlSet\Enum\USB\VID_4348&PID_55E0");
            if (k != null && k.SubKeyCount > 0) return true;
        }
        catch { }
        return false;
    }

    static Icon? LoadIcon()
    {
        try { return Icon.ExtractAssociatedIcon(Application.ExecutablePath); }
        catch { return null; }
    }

    static async Task InitAsync()
    {        string www = Path.Combine(AppContext.BaseDirectory, "www");
        string udf = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MineKey", "WebView2");
        Directory.CreateDirectory(udf);

        var env = await CoreWebView2Environment.CreateAsync(null, udf);
        await web!.EnsureCoreWebView2Async(env);

        var core = web.CoreWebView2;
        core.SetVirtualHostNameToFolderMapping(HostName, www, CoreWebView2HostResourceAccessKind.Allow);
        core.Settings.AreDevToolsEnabled = true;
        core.Settings.IsStatusBarEnabled = false;
        core.Settings.AreDefaultContextMenusEnabled = false;

        core.WebMessageReceived += OnWebMessage;

        if (evalJs != null)
        {
            core.NavigationCompleted += async (_, _) =>
            {
                await Task.Delay(600);
                try { await core.ExecuteScriptAsync(evalJs!); }
                catch (Exception ex) { Console.WriteLine("eval failed: " + ex.Message); }
            };
        }

        if (shotPath != null)
        {
            core.NavigationCompleted += async (_, _) =>
            {
                await Task.Delay(1400);
                try
                {
                    var r = await core.CallDevToolsProtocolMethodAsync("Page.captureScreenshot", "{\"format\":\"png\"}");
                    using var doc = JsonDocument.Parse(r);
                    var b64 = doc.RootElement.GetProperty("data").GetString() ?? "";
                    File.WriteAllBytes(shotPath!, Convert.FromBase64String(b64));
                }
                catch (Exception ex) { File.WriteAllText(shotPath + ".err", ex.ToString()); }
                form!.Close();
            };
        }

        if (selftestPath != null)
        {
            core.NavigationCompleted += async (_, _) =>
            {
                await Task.Delay(800);
                var sb = new System.Text.StringBuilder();
                try
                {
                    var ping = await core.ExecuteScriptAsync(
                        "navigator.hid ? 'webhid-available' : 'webhid-missing'");
                    sb.AppendLine("[env] " + ping);
                    sb.AppendLine("[metrics] " + await core.ExecuteScriptAsync(
                        "JSON.stringify({w:innerWidth,h:innerHeight,dpr:devicePixelRatio," +
                        "cols:getComputedStyle(document.querySelector('.layout')).gridTemplateColumns," +
                        "layoutW:document.querySelector('.layout').getBoundingClientRect().width," +
                        "font:getComputedStyle(document.body).fontFamily," +
                        "capFont:getComputedStyle(document.querySelector('.key .cap')).fontSize,"+
                        "scrollH:document.documentElement.scrollHeight,"+
                        "stageH:Math.round(document.querySelector('.stage').getBoundingClientRect().height),"+
                        "sideH:Math.round(document.querySelector('.side').getBoundingClientRect().height),"+
                        "keyVar:getComputedStyle(document.documentElement).getPropertyValue('--key').trim(),"+
                        "wrapW:Math.round(document.querySelector('.board-wrap').clientWidth),"+
                        "wrapH:Math.round(document.querySelector('.board-wrap').clientHeight),"+
                        "boardH:Math.round(document.querySelector('.board').getBoundingClientRect().height),"+
                        "paneSH:document.querySelector('.tabpane.active').scrollHeight,"+
                        "paneCH:document.querySelector('.tabpane.active').clientHeight,"+
                        "actType:document.querySelector('#actType').value,"+
                        "a0:JSON.stringify(S&&S.actions?S.actions[0]:null)})"));

                    // 点「连接设备」，验证原生通道的请求/应答链路
                    await core.ExecuteScriptAsync("document.getElementById('btnConnect').click()");
                    await Task.Delay(1500);

                    var logText = await core.ExecuteScriptAsync(
                        "document.getElementById('log').innerText");
                    sb.AppendLine("[connect-log] " + logText);
                }
                catch (Exception ex) { sb.AppendLine("[err] " + ex.Message); }

                File.WriteAllText(selftestPath!, sb.ToString());
                form!.Close();
            };
        }

        string q = (demoMode ? "?demo=1" : "?") + (extraQuery != null ? "&" + extraQuery.TrimStart('&', '?') : "");
        core.Navigate($"https://{HostName}/index.html{q}");
    }

    /* ================= 消息桥 ================= */

    static void OnWebMessage(object? sender, CoreWebView2WebMessageReceivedEventArgs e)
    {
        int id = 0;
        try
        {
            using var doc = JsonDocument.Parse(e.WebMessageAsJson);
            var root = doc.RootElement;
            if (root.TryGetProperty("id", out var idEl)) id = idEl.GetInt32();
            string cmd = root.GetProperty("cmd").GetString() ?? "";

            switch (cmd)
            {
                case "ping":
                    Reply(id, new { ok = true, pong = true, version = "1.0" });
                    break;

                case "list":
                {
                    var devs = HidDevice.Enumerate();
                    Reply(id, new
                    {
                        ok = true,
                        devices = devs.Select(d => new
                        {
                            vid = d.VendorId, pid = d.ProductId,
                            usagePage = d.UsagePage, usage = d.Usage,
                            name = d.ProductName, path = d.Path,
                            inputLen = d.InputReportLength, featureLen = d.FeatureReportLength
                        }).ToArray()
                    });
                    break;
                }

                case "open":
                {
                    int vid = root.GetProperty("vid").GetInt32();
                    int pid = root.TryGetProperty("pid", out var p) ? p.GetInt32() : -1;
                    int usagePage = root.TryGetProperty("usagePage", out var up) ? up.GetInt32() : -1;

                    current?.Dispose();
                    current = null;

                    var all = HidDevice.Enumerate();
                    /* 必须 VID+PID 都对上。原来还有一条「只按 UsagePage 匹配」的
                       兜底，会误抓到别的厂商自定义设备（比如触控板的 0xFF00
                       集合）—— 于是没插键盘也能"连上"，但一读就失败。 */
                    var hit = all.FirstOrDefault(d => d.VendorId == vid && d.ProductId == pid
                                                     && (usagePage < 0 || d.UsagePage == usagePage))
                              ?? all.FirstOrDefault(d => d.VendorId == vid && d.ProductId == pid);

                    if (hit == null)
                    {
                        string err = "未找到设备（VID:PID = " + vid.ToString("X4") + ":" + pid.ToString("X4") + "）";
                        if (BootloaderPresent())
                            err = "芯片停在 WCH BootLoader（4348:55E0）—— 固件还没烧进去。"
                                + "用 WCHISPTool 烧 firmware\\minekey.hex，烧完设备会变成 1209:0001";
                        Reply(id, new { ok = false, error = err });
                        break;
                    }

                    hit.Open();
                    hit.ReportReceived += OnReport;
                    hit.Disconnected += OnGone;
                    current = hit;

                    Reply(id, new
                    {
                        ok = true,
                        name = string.IsNullOrEmpty(hit.ProductName) ? "HID 设备" : hit.ProductName,
                        vid = hit.VendorId, pid = hit.ProductId,
                        usagePage = hit.UsagePage, usage = hit.Usage,
                        inputLen = hit.InputReportLength, featureLen = hit.FeatureReportLength
                    });
                    break;
                }

                case "feature":
                {
                    if (current == null) { Reply(id, new { ok = false, error = "设备未打开" }); break; }
                    int rid = root.GetProperty("reportId").GetInt32();
                    var data = root.GetProperty("data").EnumerateArray().Select(x => (byte)x.GetInt32()).ToArray();
                    var res = current.FeatureExchange((byte)rid, data);
                    Reply(id, new { ok = true, data = res.Select(b => (int)b).ToArray() });
                    break;
                }

                case "run":
                {
                    /* 「试跑」：配置器点了按钮就真的执行一次，不用装 host-agent、不用板子 */
                    string type = root.TryGetProperty("type", out var tEl) ? (tEl.GetString() ?? "") : "";
                    string target = root.TryGetProperty("target", out var gEl) ? (gEl.GetString() ?? "") : "";
                    if (string.IsNullOrWhiteSpace(target))
                    {
                        Reply(id, new { ok = false, error = "目标为空" });
                        break;
                    }
                    try
                    {
                        if (type == "cmd")
                        {
                            Process.Start(new ProcessStartInfo("cmd.exe", "/c " + target)
                            {
                                UseShellExecute = false,
                                CreateNoWindow = true,
                                WindowStyle = ProcessWindowStyle.Hidden
                            });
                        }
                        else
                        {
                            // url / app / folder 都交给 ShellExecute，和资源管理器双击一个效果
                            Process.Start(new ProcessStartInfo(target) { UseShellExecute = true });
                        }
                        Reply(id, new { ok = true });
                    }
                    catch (Exception ex)
                    {
                        Reply(id, new { ok = false, error = ex.Message });
                    }
                    break;
                }

                case "pick":
                {
                    /* 「浏览」：软件 → 文件对话框，文件夹 → 文件夹对话框。
                       返回绝对路径；用户取消时 canceled=true。 */
                    string kind = root.TryGetProperty("kind", out var kEl) ? (kEl.GetString() ?? "file") : "file";
                    string? picked = null;
                    if (kind == "folder")
                    {
                        /* .NET Core 之后 FolderBrowserDialog 走的是 Vista 风格的
                           IFileOpenDialog（FOS_PICKFOLDERS），不是老的树形框 */
                        using var dlg = new FolderBrowserDialog
                        {
                            Description = "选择要打开的文件夹",
                            UseDescriptionForTitle = true,
                            ShowNewFolderButton = true
                        };
                        if (dlg.ShowDialog(form) == DialogResult.OK) picked = dlg.SelectedPath;
                    }
                    else
                    {
                        using var dlg = new OpenFileDialog
                        {
                            Title = "选择程序",
                            Filter = "程序 (*.exe;*.bat;*.cmd;*.lnk)|*.exe;*.bat;*.cmd;*.lnk|所有文件 (*.*)|*.*",
                            CheckFileExists = true
                        };
                        if (dlg.ShowDialog(form) == DialogResult.OK) picked = dlg.FileName;
                    }
                    Reply(id, new { ok = true, path = picked, canceled = picked == null });
                    break;
                }

                case "save":
                {
                    /* 导出（json / h）：弹「另存为」，由用户决定目录和文件名 */
                    string name = root.TryGetProperty("name", out var nEl) ? (nEl.GetString() ?? "untitled.txt") : "untitled.txt";
                    string data = root.TryGetProperty("data", out var dEl) ? (dEl.GetString() ?? "") : "";
                    string kind = root.TryGetProperty("filter", out var fEl) ? (fEl.GetString() ?? "") : "";
                    string filter = kind switch
                    {
                        "json" => "JSON 文件 (*.json)|*.json|所有文件 (*.*)|*.*",
                        "h"    => "C 头文件 (*.h)|*.h|所有文件 (*.*)|*.*",
                        _      => "所有文件 (*.*)|*.*"
                    };
                    using var dlg = new SaveFileDialog
                    {
                        Title = "保存",
                        FileName = name,
                        Filter = filter,
                        DefaultExt = Path.GetExtension(name).TrimStart('.'),
                        AddExtension = true,
                        OverwritePrompt = true,
                        RestoreDirectory = true
                    };
                    if (dlg.ShowDialog(form) != DialogResult.OK)
                    {
                        Reply(id, new { ok = true, canceled = true });
                        break;
                    }
                    try
                    {
                        File.WriteAllText(dlg.FileName, data, new UTF8Encoding(false));
                        Reply(id, new { ok = true, path = dlg.FileName });
                    }
                    catch (Exception ex)
                    {
                        Reply(id, new { ok = false, error = "写入失败：" + ex.Message });
                    }
                    break;
                }

                case "writeActions":
                {
                    /* 自动同步：把动作表直接写进 tools/host-agent/actions.json，不用再点导出。
                       路径不写死——从 exe 所在目录往上找含 host-agent 的那一层。 */
                    string data = root.TryGetProperty("data", out var waEl) ? (waEl.GetString() ?? "") : "";

                    string? dir = null;
                    DirectoryInfo? probe = new DirectoryInfo(AppContext.BaseDirectory);
                    while (probe != null && dir == null)
                    {
                        string cand = Path.Combine(probe.FullName, "host-agent");
                        if (Directory.Exists(cand)) dir = cand;
                        probe = probe.Parent;
                    }
                    if (dir == null)
                    {
                        Reply(id, new { ok = false, error = "向上找不到 host-agent 目录（用的是网页版？那请手动导出）" });
                        break;
                    }
                    string file = Path.Combine(dir, "actions.json");
                    try
                    {
                        File.WriteAllText(file, data, new UTF8Encoding(false));
                        Reply(id, new { ok = true, path = file });
                    }
                    catch (Exception ex)
                    {
                        Reply(id, new { ok = false, error = "写入失败：" + ex.Message });
                    }
                    break;
                }

                case "close":
                    current?.Dispose();
                    current = null;
                    Reply(id, new { ok = true });
                    break;

                default:
                    Reply(id, new { ok = false, error = "未知命令 " + cmd });
                    break;
            }
        }
        catch (Exception ex)
        {
            Reply(id, new { ok = false, error = ex.Message });
        }
    }

    static void OnReport(byte reportId, byte[] data)
    {
        Post(new { evt = "report", id = (int)reportId, data = data.Select(b => (int)b).ToArray() });
    }

    static void OnGone() => Post(new { evt = "gone" });

    static void Reply(int id, object payload)
    {
        var node = JsonSerializer.SerializeToNode(payload)!.AsObject();
        node["id"] = id;
        Post(node);
    }

    static void Post(object payload)
    {
        if (web?.CoreWebView2 == null) return;
        string json = JsonSerializer.Serialize(payload);
        if (form != null && form.InvokeRequired)
        {
            try { form.BeginInvoke(new Action(() => web!.CoreWebView2.PostWebMessageAsJson(json))); } catch { }
        }
        else
        {
            try { web.CoreWebView2.PostWebMessageAsJson(json); } catch { }
        }
    }
}
