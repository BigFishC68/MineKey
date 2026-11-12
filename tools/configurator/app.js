/* =========================================================================
 * MineKey · 键盘配置器
 * CH552 + 11 键 + EC11 旋钮 · WebHID 客户端
 *
 * 协议：HID Feature Report 0x10（负载 63 字节，首字节为命令字；+报告 ID = 64 EP0 单包）
 * 详细说明见 docs/02-软件架构与协议.md
 * ========================================================================= */
'use strict';

/* ===================== 协议常量 ===================== */
const PROTO_VERSION = 1;
const VID = 0x1209;              // pid.codes 测试 VID（无 USB-IF 会员时的通用做法）
const PID = 0x0001;              // 待定，需与固件一致
const R_FEATURE = 0x10;
const R_KBD = 0x01, R_CONSUMER = 0x02, R_STATUS = 0x03;

const CMD = {
  INFO: 0x01, GET_KEYMAP: 0x02, SET_KEY: 0x03, SET_ALL_KEYS: 0x04,
  GET_ENC: 0x05, SET_ENC: 0x06, GET_LED: 0x07, SET_LED: 0x08,
  SAVE: 0x09, FACTORY_RESET: 0x0A, BOOTLOADER: 0x0B
};

/* ===================== 物理布局 =====================
 * 编号规则：**从左到右、从下到上**（和板子上丝印的 CN1..CN11 一致）
 *
 *   上排   CN9  CN10 CN11 [旋钮 SW1]
 *   中排   CN5  CN6  CN7  CN8
 *   下排   CN1  CN2  CN3  CN4
 *
 * `row` / `col` 是**电气**行列（原理图 R1/R2/R3 × C1..C4），只用于显示和排查；
 * `gx` / `gy` 是界面网格坐标，gy=0 是最上面一行（CSS grid-row 从 1 开始，所以 +1）。
 * 换配列只改这张表：id 必须连续 0..10。
 */
const LAYOUT = [
  { id: 0,  ref: 'CN1',  gx: 0, gy: 2, row: 2, col: 0 },
  { id: 1,  ref: 'CN2',  gx: 1, gy: 2, row: 2, col: 1 },
  { id: 2,  ref: 'CN3',  gx: 2, gy: 2, row: 2, col: 2 },
  { id: 3,  ref: 'CN4',  gx: 3, gy: 2, row: 2, col: 3 },
  { id: 4,  ref: 'CN5',  gx: 0, gy: 1, row: 1, col: 0 },
  { id: 5,  ref: 'CN6',  gx: 1, gy: 1, row: 1, col: 1 },
  { id: 6,  ref: 'CN7',  gx: 2, gy: 1, row: 1, col: 2 },
  { id: 7,  ref: 'CN8',  gx: 3, gy: 1, row: 1, col: 3 },
  { id: 8,  ref: 'CN9',  gx: 0, gy: 0, row: 0, col: 0 },
  { id: 9,  ref: 'CN10', gx: 1, gy: 0, row: 0, col: 1 },
  { id: 10, ref: 'CN11', gx: 2, gy: 0, row: 0, col: 2 }
];

/* 旋钮在右上角，电气上接 R1/C4（bit3） */
const KNOB = { id: 11, ref: 'SW1', gx: 3, gy: 0, row: 0, col: 3 };

const NKEYS = 11, NLEDS = 11;

/* 矩阵位序 → 键位 id（bit = row*4 + col，其中 bit3 是编码器按压）
 *
 * 2026-11-11 按实测接线修正：ROW1 是**上排**、ROW3 是**下排**，COL1..COL4 从左到右。
 * 所以下排那 4 个（CN1..CN4）在 bit8..bit11，中排（CN5..CN8）在 bit4..bit7，
 * 上排（CN9..CN11）在 bit0..bit2。
 * 实测佐证：修表前按物理 CN1 出的是 '4'，而 '4' 当时在 key7 = bit8 ✓
 */
const MATRIX_TO_KEY = [8, 9, 10, -1, 4, 5, 6, 7, 0, 1, 2, 3];

/* 灯珠串联顺序（2026-11-11 按实物串接标定，用户实测）
 *
 * 实物灯链：CN11 → CN10 → CN9 → CN5 → CN6 → CN7 → CN8 → CN4 → CN3 → CN2 → CN1
 * 和键位顺序完全不同，所以 per_key 必须按这张表换算。
 *
 * 本表 = 键 id → 链条序号；固件里的 LED_KEY 是它的逆。
 */
const LED_ORDER = [10, 9, 8, 7, 3, 4, 5, 6, 2, 1, 0];

/* 链条序号 → 灯珠位号（按 PCB 坐标配对得出，仅用于界面显示和排查） */
const LED_REF = ['U6', 'U12', 'U11', 'U9', 'U8', 'U7', 'U10', 'U2', 'U5', 'U4', 'U3'];

/* ===================== 功能码表 ===================== */
// 编码：code = (type << 8) | value
const T_NONE = 0x00, T_KEY = 0x01, T_MEDIA = 0x02, T_MOUSE = 0x03, T_ACTION = 0x04, T_COMBO = 0x05, T_HOST = 0x06, T_MOVE = 0x07;

const KEYS = [
  [0x00, '（无）'], [0x04, 'A'], [0x05, 'B'], [0x06, 'C'], [0x07, 'D'], [0x08, 'E'],
  [0x09, 'F'], [0x0A, 'G'], [0x0B, 'H'], [0x0C, 'I'], [0x0D, 'J'], [0x0E, 'K'],
  [0x0F, 'L'], [0x10, 'M'], [0x11, 'N'], [0x12, 'O'], [0x13, 'P'], [0x14, 'Q'],
  [0x15, 'R'], [0x16, 'S'], [0x17, 'T'], [0x18, 'U'], [0x19, 'V'], [0x1A, 'W'],
  [0x1B, 'X'], [0x1C, 'Y'], [0x1D, 'Z'],
  [0x1E, '1'], [0x1F, '2'], [0x20, '3'], [0x21, '4'], [0x22, '5'], [0x23, '6'],
  [0x24, '7'], [0x25, '8'], [0x26, '9'], [0x27, '0'],
  [0x28, 'Enter'], [0x29, 'Esc'], [0x2A, 'Backspace'], [0x2B, 'Tab'], [0x2C, 'Space'],
  [0x2D, '-'], [0x2E, '='], [0x2F, '['], [0x30, ']'], [0x31, '\\'],
  [0x33, ';'], [0x34, '\''], [0x35, '`'], [0x36, ','], [0x37, '.'], [0x38, '/'],
  [0x39, 'CapsLock'],
  [0x3A, 'F1'], [0x3B, 'F2'], [0x3C, 'F3'], [0x3D, 'F4'], [0x3E, 'F5'], [0x3F, 'F6'],
  [0x40, 'F7'], [0x41, 'F8'], [0x42, 'F9'], [0x43, 'F10'], [0x44, 'F11'], [0x45, 'F12'],
  [0x46, 'PrintScreen'], [0x47, 'ScrollLock'], [0x48, 'Pause'],
  [0x49, 'Insert'], [0x4A, 'Home'], [0x4B, 'PgUp'], [0x4C, 'Delete'], [0x4D, 'End'], [0x4E, 'PgDn'],
  [0x4F, '→ 右'], [0x50, '← 左'], [0x51, '↓ 下'], [0x52, '↑ 上'],
  /* 小键盘只留 NumLock 和四则运算符：Num 0-9 / Num Enter / Num . 在主键盘区已经有
     发出去完全一样的键，留着只会让人选错（0x58..0x63 已移除） */
  [0x53, 'NumLock'], [0x54, 'Num /'], [0x55, 'Num *'], [0x56, 'Num -'], [0x57, 'Num +'],
  /* 左右修饰键只保留一套（0xE4 右Ctrl / 0xE5 右Shift / 0xE6 右Alt / 0xE7 右Win 已移除） */
  [0xE0, 'Ctrl'], [0xE1, 'Shift'], [0xE2, 'Alt'], [0xE3, 'Win']
];

/* 多媒体码 = HID Consumer Page(0x0C) 的真实 usage。
   注意不能用自己的序号(1,2,3…)——固件是把这 16 位原样塞进报告里的，
   主机按 usage 查表，序号对不上就什么都不会发生（旋钮没反应就是这个原因）。 */
const MEDIA = [
  [0xE9, '音量 +'], [0xEA, '音量 −'], [0xE2, '静音'], [0xCD, '播放/暂停'],
  [0xB5, '下一曲'], [0xB6, '上一曲'], [0x6F, '亮度 +'], [0x70, '亮度 −'],
  [0x223, '浏览器主页'], [0x221, '浏览器搜索'], [0x192, '计算器'], [0xB7, '停止'],
  [0xB3, '快进'], [0xB4, '快退']
];

const MOUSE = [
  [1, '左键单击'], [2, '右键单击'], [3, '中键单击'],
  [4, '滚轮上滚'], [5, '滚轮下滚']
];

/* 光标相对移动：这里只列方向，速度由「灵敏度」滑杆调。
 * 低字节 = (档位 << 4) | 方向，像素 = 6 + 档位×2（6..36 px 每 20ms，300..1800 px/s）
 * 老的 0x0701..0x0704 就是档位 0，向后兼容。
 */
const MOVES = [[1, '光标 ↑'], [2, '光标 ↓'], [3, '光标 ←'], [4, '光标 →']];

const MOVE_PX_MIN = 6, MOVE_PX_INC = 2;
const movePx = lv => MOVE_PX_MIN + lv * MOVE_PX_INC;
const moveLv = code => (code >> 4) & 0x0F;
const moveLabel = lv => `${movePx(lv)} px/20ms · ${movePx(lv) * 50} px/s`;

/* 鼠标与光标合并成一组：类型下拉里只留一个"鼠标与光标"，
   值列表里按钮/滚轮（0x03xx）和相对移动（0x07xx）混排。
   T_POINTER 只是界面用的伪类型，不会写进功能码。 */
const T_POINTER = 8;
const POINTER_ITEMS = MOUSE.map(m => [(T_MOUSE << 8) | m[0], m[1]])
  .concat(MOVES.map(m => [(T_MOVE << 8) | m[0], m[1]]));

/* 只保留"能在键盘上按出来"的灯效动作。
   重新加载配置(5) / 恢复出厂设置(6) 已删除：这两个只能由配置器执行，
   一旦绑到按键上，误按一下就把 11 个键全清了。固件侧对应分支同步删除。 */
const ACTIONS = [
  [2, '亮度 +'], [3, '亮度 −'], [4, '下一个灯效']
];

const MODS = [['Ctrl', 0x01], ['Shift', 0x02], ['Alt', 0x04], ['Win', 0x08]];

/* 主机动作：键盘只发编号，真正执行在 tools/host-agent/agent.py
   编号 0..11 与 actions.json 里的键一一对应 */
const N_ACTIONS = 12;
const HOST_TYPES = [['url', '网页'], ['app', '软件'], ['folder', '文件夹'], ['cmd', '命令']];
const DEFAULT_ACTIONS = [
  { name: '打开 B 站', type: 'url', target: 'https://www.bilibili.com' },
  { name: 'VS Code', type: 'app', target: '' },
  { name: '下载文件夹', type: 'folder', target: '' },
  { name: '锁屏', type: 'cmd', target: '' },
  { name: '', type: 'url', target: '' },
  { name: '', type: 'url', target: '' },
  { name: '', type: 'url', target: '' },
  { name: '', type: 'url', target: '' },
  { name: '', type: 'url', target: '' },
  { name: '', type: 'url', target: '' },
  { name: '', type: 'url', target: '' },
  { name: '', type: 'url', target: '' }
];

/* ===================== 状态 ===================== */
const S = {
  online: false, demo: false,
  info: null,
  keys: new Array(NKEYS).fill(T_NONE),
  enc: { cw: T_NONE | 0, ccw: 0, push: 0, invert: 0 },
  led: {
    mode: 0, brightness: 40, speed: 50,
    r: 0xF7, g: 0x76, b: 0x34,
    perKey: Array.from({ length: NLEDS }, () => ({ r: 0xF7, g: 0x76, b: 0x34 }))
  },
  selected: 0,          // 0..10 = 键，11 = 旋钮（按下）
  selIsKnob: false,
  pressed: new Set(),
  testMode: false,
  knobAngle: 0,
  knobCount: 0,
  ledPhase: 0,
  logLines: 0,
  actions: [],
  actSel: 0,
  powerOver: false
};

/* ===================== 小工具 ===================== */
const $ = s => document.querySelector(s);
const $$ = s => Array.from(document.querySelectorAll(s));
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const hex = (n, w = 4) => '0x' + n.toString(16).toUpperCase().padStart(w, '0');

function nowStamp() {
  const d = new Date();
  return String(d.getHours()).padStart(2, '0') + ':' +
         String(d.getMinutes()).padStart(2, '0') + ':' +
         String(d.getSeconds()).padStart(2, '0') + '.' +
         String(d.getMilliseconds()).padStart(3, '0');
}

function log(msg, cls = '') {
  const box = $('#log');
  const line = document.createElement('div');
  line.innerHTML = `<span class="t">${nowStamp()}</span><span class="${cls}">${msg}</span>`;
  box.appendChild(line);
  if (box.children.length > 300) box.removeChild(box.firstChild);
  box.scrollTop = box.scrollHeight;
}

let toastTimer = null;
function toast(msg, isErr = false) {
  const t = $('#toast');
  t.textContent = msg;
  t.className = 'toast show' + (isErr ? ' err' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.className = 'toast'; }, 2200);
}

/* ===================== 功能码 <-> 文本 ===================== */
function codeType(code) {
  const hi = (code >> 8) & 0xFF;
  return (hi & 0xF0) === 0x50 ? T_COMBO : hi;   // 0x5M.. = 组合键（M 为修饰位）
}
function codeValue(code) { return code & 0xFF; }

function codeToLabel(code) {
  const t = codeType(code), v = codeValue(code);
  if (code === 0) return '未分配';
  if (t === T_KEY) {
    const f = KEYS.find(k => k[0] === v);
    return f ? f[1] : 'Key ' + hex(v, 2);
  }
  if (t === T_MEDIA) {
    const f = MEDIA.find(k => k[0] === v);
    return f ? f[1] : '媒体 ' + v;
  }
  if (t === T_MOUSE) {
    const f = MOUSE.find(k => k[0] === v);
    return f ? f[1] : '鼠标 ' + v;
  }
  if (t === T_MOVE) {
    const f = MOVES.find(k => k[0] === v);
    return f ? f[1] : '光标 ' + v;
  }
  if (t === T_ACTION) {
    const f = ACTIONS.find(k => k[0] === v);
    return f ? f[1] : '动作 ' + v;
  }
  if (t === T_HOST) {
    const a = S.actions[v];
    return a && a.name ? '→ ' + a.name : '动作 ' + v;
  }
  if (t === T_COMBO) {
    const mod = (code >> 8) & 0x0F;
    const mods = MODS.filter(m => mod & m[1]).map(m => m[0]).join('+');
    const kv = code & 0xFF;
    const f = KEYS.find(x => x[0] === kv);
    return (mods ? mods + '+' : '') + (f ? f[1] : hex(kv, 2));
  }
  return hex(code);
}

/* ===================== 设备层 ===================== */

/* 桌面版（WebView2 宿主）原生 HID 通道。
   WebView2 里 navigator.hid 存在，但宿主没有实现 HID 选择器，
   requestDevice() 的 Promise 永远不返回 —— 所以桌面版走 C# 侧的原生 HID。 */
const BRIDGE = (typeof window !== 'undefined' && window.chrome && window.chrome.webview)
  ? window.chrome.webview : null;

const bridge = {
  seq: 1,
  pending: new Map(),
  onReport: null,
  onGone: null,
  install() {
    if (!BRIDGE) return;
    BRIDGE.addEventListener('message', ev => {
      const m = ev.data;
      if (!m) return;
      if (m.evt === 'report') { if (this.onReport) this.onReport(m.id, m.data); return; }
      if (m.evt === 'gone') { if (this.onGone) this.onGone(); return; }
      const p = this.pending.get(m.id);
      if (!p) return;
      this.pending.delete(m.id);
      if (m.ok) p.resolve(m); else p.reject(new Error(m.error || '设备返回错误'));
    });
  },
  call(cmd, extra = {}, timeout = 3000) {
    const id = this.seq++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      BRIDGE.postMessage(Object.assign({ id, cmd }, extra));
      setTimeout(() => {
        if (this.pending.has(id)) {
          this.pending.delete(id);
          reject(new Error(`宿主无响应（${cmd}）`));
        }
      }, timeout);
    });
  }
};
bridge.install();

class Device {
  constructor() {
    this.demo = false;
    this.dev = null;
    this.infoCache = null;
    this.onReport = null;
    this.onGone = null;
    this._bound = this._handleReport.bind(this);
  }

  /* ---------- 真实设备 ---------- */
  async connectReal() {
    if (BRIDGE) return this._connectBridge();
    if (!('hid' in navigator)) {
      throw new Error('当前浏览器不支持 WebHID（请用 Chrome / Edge 89+）');
    }
    const filters = [
      { vendorId: VID, productId: PID },
      { usagePage: 0xFF00, usage: 0x0001 },
      { usagePage: 0xFF00 }
    ];
    log('请求 HID 设备…', 'tx');
    const [dev] = await navigator.hid.requestDevice({ filters });
    if (!dev) throw new Error('未选择设备');
    if (!dev.opened) await dev.open();
    this.dev = dev;
    this.demo = false;
    dev.addEventListener('inputreport', this._bound);
    navigator.hid.addEventListener('disconnect', ev => {
      if (ev.device === this.dev) this._gone();
    });
    log(`已打开：${dev.productName || 'HID 设备'} (${hex(dev.vendorId)}:${hex(dev.productId)})`, 'ok');
    return dev;
  }

  _gone() {
    this.dev = null;
    this.demo = false;
    if (this.onGone) this.onGone();
    log('设备已断开', 'warn');
  }

  /* ---------- 桌面版原生通道 ---------- */
  async _connectBridge() {
    log('正在查找 HID 设备…', 'tx');
    const r = await bridge.call('open', { vid: VID, pid: PID, usagePage: 0xFF00 }, 4000);
    this.demo = false;
    this.dev = {
      bridge: true,
      vendorId: r.vid,
      productId: r.pid,
      productName: r.name,
      usagePage: r.usagePage
    };
    bridge.onReport = (id, data) => {
      if (this.onReport) this.onReport(id, new Uint8Array(data));
    };
    bridge.onGone = () => this._gone();
    log(`已打开：${r.name} (${hex(r.vid)}:${hex(r.pid)}) · 原生 HID 通道`, 'ok');
    return this.dev;
  }

  _handleReport(e) {
    const d = new Uint8Array(e.data.buffer, e.data.byteOffset, e.data.byteLength);
    if (this.onReport) this.onReport(e.reportId, d);
  }

  async _feature(payload) {
    if (!this.dev) throw new Error('设备未打开');
    // 63 = Feature 负载长度：63 + 报告 ID = 64 = 固件 EP0 单包上限。
    // 固件报告描述符里 Report Count = 63，主机 HID 栈的 FeatureReportByteLength = 64。
    // 发送长度必须与之匹配，否则 WebHID / HidD_SetFeature 直接失败。
    const p = new Uint8Array(63);
    p.set(payload.slice(0, 63));

    if (this.dev.bridge) {
      const r = await bridge.call('feature', { reportId: R_FEATURE, data: Array.from(p) });
      let out = new Uint8Array(r.data);
      if (out.length === 64) out = out.subarray(1);   // 桌面版回 64 字节（ID + 63）
      return out;
    }

    await this.dev.sendFeatureReport(R_FEATURE, p);
    const raw = await this.dev.receiveFeatureReport(R_FEATURE);
    let out = new Uint8Array(raw.buffer, raw.byteOffset, raw.byteLength);
    if (out.length === 64) out = out.subarray(1);   // WebHID 回 64 字节，去掉 ID
    return out;
  }

  async _cmd(bytes) {
    const r = await this._feature(bytes);
    const echo = r[0], status = r[1];
    if (status !== 0) throw new Error(`设备返回错误 ${status}（命令 ${hex(echo & 0x7F, 2)}）`);
    return r;
  }

  /* ---------- 演示设备 ---------- */
  connectDemo() {
    this.demo = true;
    this.dev = null;
    log('进入演示模式：所有操作只在本地生效，不会写硬件', 'warn');
    return true;
  }

  disconnect() {
    if (this.dev && this.dev.bridge) {
      bridge.call('close').catch(() => {});
    } else if (this.dev) {
      try { this.dev.removeEventListener('inputreport', this._bound); this.dev.close(); } catch (_) {}
    }
    this.dev = null;
    this.demo = false;
  }

  /* ---------- 统一 API ---------- */
  async readAll() {
    if (this.demo) {
      return {
        info: {
          proto: PROTO_VERSION, fwMajor: 0, fwMinor: 9, rows: 3, cols: 4,
          nkeys: NKEYS, nleds: NLEDS, name: 'MineKey Demo (未连接硬件)', serial: 'DEMO-0000'
        },
        keys: S.keys.slice(),
        enc: { ...S.enc },
        led: JSON.parse(JSON.stringify(S.led))
      };
    }
    const i = await this._cmd([CMD.INFO]);
    const info = {
      proto: i[2], fwMajor: i[3], fwMinor: i[4],
      rows: i[5], cols: i[6], nkeys: i[7], nleds: i[8],
      name: String.fromCharCode.apply(null, Array.from(i.slice(10, 42)).filter(c => c >= 32 && c < 127)) || 'MineKey',
      serial: this.dev ? (this.dev.productName || '—') : '—'
    };
    const k = await this._cmd([CMD.GET_KEYMAP]);
    const keys = [];
    for (let n = 0; n < NKEYS; n++) keys.push(k[2 + n * 2] | (k[3 + n * 2] << 8));
    const e = await this._cmd([CMD.GET_ENC]);
    const enc = {
      cw: e[2] | (e[3] << 8), ccw: e[4] | (e[5] << 8),
      push: e[6] | (e[7] << 8), invert: e[8]
    };
    const l = await this._cmd([CMD.GET_LED]);
    const led = {
      mode: l[2], brightness: l[3], speed: l[4],
      r: l[5], g: l[6], b: l[7],
      perKey: []
    };
    for (let n = 0; n < NLEDS; n++) {
      const o = 8 + n * 3;
      led.perKey.push({ r: l[o], g: l[o + 1], b: l[o + 2] });
    }
    this.infoCache = info;
    return { info, keys, enc, led };
  }

  /* ---- 自动落盘 ----
     每次写配置都排队一次 SAVE，700ms 内多次改动只写一次（避免拖滑杆时狂刷 DataFlash）。
     以前只有「写入旋钮」和「保存」按钮会存盘，改键位 / 改灯只写 RAM，
     拔电就回默认值 —— 用户看到的现象是"设置记不住"。 */
  _scheduleSave() {
    if (this.demo) return;
    clearTimeout(this._saveTimer);
    this._saveTimer = setTimeout(async () => {
      try {
        await this.save();
        log('已自动保存到 DataFlash', 'ok');
      } catch (e) {
        log('自动保存失败：' + e.message, 'err');
      }
    }, 700);
  }

  async writeKey(idx, code) {
    if (this.demo) { S.keys[idx] = code; return; }
    await this._cmd([CMD.SET_KEY, idx, code & 0xFF, (code >> 8) & 0xFF]);
    this._scheduleSave();
  }

  async writeEnc(enc) {
    if (this.demo) { Object.assign(S.enc, enc); return; }
    await this._cmd([CMD.SET_ENC,
      enc.cw & 0xFF, (enc.cw >> 8) & 0xFF,
      enc.ccw & 0xFF, (enc.ccw >> 8) & 0xFF,
      enc.push & 0xFF, (enc.push >> 8) & 0xFF, enc.invert ? 1 : 0]);
    this._scheduleSave();
  }

  async writeLed(led) {
    if (this.demo) { S.led = JSON.parse(JSON.stringify(led)); return; }
    const p = [CMD.SET_LED, led.mode, led.brightness, led.speed, led.r, led.g, led.b];
    for (let n = 0; n < NLEDS; n++) {
      const c = led.perKey[n] || { r: 0, g: 0, b: 0 };
      p.push(c.r, c.g, c.b);
    }
    await this._cmd(p);
    this._scheduleSave();
  }

  async save() {
    if (this.demo) { log('（演示）配置已写入本地', 'ok'); return; }
    await this._cmd([CMD.SAVE]);
  }

  async factoryReset() {
    if (this.demo) { log('（演示）已恢复默认', 'warn'); return; }
    await this._cmd([CMD.FACTORY_RESET]);
  }
}

const dev = new Device();

/* ===================== 渲染：键盘 ===================== */
function buildBoard() {
  const board = $('#board');
  board.innerHTML = '';

  LAYOUT.forEach(k => {
    const el = document.createElement('div');
    el.className = 'key';
    el.dataset.id = k.id;
    el.style.gridColumn = k.gx + 1;
    el.style.gridRow = k.gy + 1;
    el.innerHTML = `<div class="led"></div>
                    <div class="cap" data-cap>—</div>
                    <div class="ref">${k.ref}</div>`;
    board.appendChild(el);
  });

  // 旋钮槽位
  const slot = document.createElement('div');
  slot.className = 'knob-slot';
  slot.style.gridColumn = KNOB.gx + 1;
  slot.style.gridRow = KNOB.gy + 1;
  slot.innerHTML = `<div class="knob" id="knobBtn" title="编码器按压 (SW1)">
                      <div class="knot"></div><div class="knob-mark"></div>
                    </div>`;
  board.appendChild(slot);

  board.addEventListener('mousedown', e => {
    const keyEl = e.target.closest('.key');
    const knobEl = e.target.closest('.knob');
    if (keyEl) {
      const id = +keyEl.dataset.id;
      selectKey(id, false);
      if (S.testMode) simPress(id, true);
    } else if (knobEl) {
      selectKey(11, true);
      if (S.testMode) simKnob('push');
    }
  });
  document.addEventListener('mouseup', () => {
    if (S.testMode && S.demo) {
      $$('.key.press').forEach(el => { el.classList.remove('press'); S.pressed.delete(+el.dataset.id); });
      $('#knobBtn')?.classList.remove('press');
    }
  });

  // 旋钮拖动 = 模拟旋转
  const knob = $('#knobBtn');
  let lastAngle = null;
  knob.addEventListener('mousedown', e => {
    e.preventDefault();
    lastAngle = null;
    const move = ev => {
      const r = knob.getBoundingClientRect();
      const cx = r.left + r.width / 2, cy = r.top + r.height / 2;
      const a = Math.atan2(ev.clientY - cy, ev.clientX - cx) * 180 / Math.PI;
      if (lastAngle !== null) {
        let d = a - lastAngle;
        if (d > 180) d -= 360;
        if (d < -180) d += 360;
        if (Math.abs(d) >= 12) {
          simKnob(d > 0 ? 'cw' : 'ccw');
          lastAngle = a;
        }
      } else lastAngle = a;
    };
    const up = () => {
      window.removeEventListener('mousemove', move);
      window.removeEventListener('mouseup', up);
    };
    window.addEventListener('mousemove', move);
    window.addEventListener('mouseup', up);
  });
}

function renderBoard() {
  LAYOUT.forEach(k => {
    const el = $(`.key[data-id="${k.id}"]`);
    if (!el) return;
    const code = S.keys[k.id];
    const t = codeType(code);
    el.dataset.type = t;
    el.querySelector('[data-cap]').textContent = code ? codeToLabel(code) : '未分配';
    el.classList.toggle('sel', !S.selIsKnob && S.selected === k.id);
    el.classList.toggle('press', S.pressed.has(k.id));

    // 灯效预览
    let c = S.led.perKey[LED_ORDER[k.id]] || { r: 0, g: 0, b: 0 };
    if (S.led.mode === 2 || S.led.mode === 5) {
      const h = ((LED_ORDER[k.id] * 23) + S.ledPhase * 60) % 360;
      c = hsv2rgb(h, 1, 1);
    }
    const on = S.led.mode !== 4 && S.led.brightness > 0;
    const mul = effectMul(k.id);
    const rgb = `rgb(${Math.round(c.r * mul)},${Math.round(c.g * mul)},${Math.round(c.b * mul)})`;
    const ledEl = el.querySelector('.led');
    ledEl.style.background = rgb;          /* 纯色灯条，不做渐变 */
    el.dataset.lit = (on && (c.r + c.g + c.b) > 12) ? '1' : '0';
  });

  const kn = $('#knobBtn');
  if (kn) {
    kn.classList.toggle('sel', S.selIsKnob);
    kn.style.transform = `rotate(${S.knobAngle}deg)`;
  }
  $('#knobCount').textContent = S.knobCount;
  $('#knobCW').textContent = codeToLabel(S.enc.cw);
  $('#knobCCW').textContent = codeToLabel(S.enc.ccw);
  $('#knobPush').textContent = codeToLabel(S.enc.push);
}

function effectMul(i) {
  const b = S.led.brightness / 100;
  const t = S.ledPhase;
  switch (S.led.mode) {
    case 0: return b;
    case 1: return b * (0.35 + 0.65 * (0.5 + 0.5 * Math.sin(t * 2 + i * 0.5)));
    case 2: return b;
    case 3: return b * (S.pressed.has(i) ? 1 : 0.25);
    case 5: return b * (0.35 + 0.65 * (0.5 + 0.5 * Math.sin(t * 2)));
    case 6: {
      const head = Math.floor(S.ledPhase * 1.6) % NLEDS;
      const d = (i - head + NLEDS) % NLEDS;
      return d === 0 ? b : d === NLEDS - 1 ? b * 0.33 : 0;
    }
    case 7: {
      const x = ((i * 41 + Math.floor(S.ledPhase * 7) * 17) & 0xFF);
      const v = ((x << 1) ^ (x >> 3)) & 0xFF;
      return v > 190 ? b : v > 140 ? b * 0.5 : 0;
    }
    default: return 0;
  }
}

/* ===================== 渲染：侧栏 ===================== */
function fillValueSelect(type, current) {
  const sel = $('#keyValue');
  sel.innerHTML = '';
  let list = [];
  if (type === T_KEY) list = KEYS.map(k => [k[0], k[1]]);
  else if (type === T_MEDIA) list = MEDIA;
  else if (type === T_POINTER) list = POINTER_ITEMS;   /* 值本身就是完整功能码 */
  else if (type === T_ACTION) list = ACTIONS;
  else if (type === T_COMBO) list = KEYS.filter(k => k[0] >= 0x04 && k[0] <= 0x63).map(k => [k[0], k[1]]);
  else if (type === T_HOST) {
    /* 编号用 0 基，和 actions.json 的键、代理启动日志、原始码 0x06xx 完全一致。
       以前这里显示 i+1，会让人把"第 5 项"当成编号 5，结果键发出去是 4 号槽位。 */
    list = S.actions.map((a, i) => [i, '[' + i + '] ' + (a.name || '（空）')]);
  }

  if (type === T_NONE) {
    sel.innerHTML = '<option value="0">—</option>';
    sel.disabled = true;
    return;
  }
  sel.disabled = false;
  list.forEach(([v, label]) => {
    const o = document.createElement('option');
    o.value = v;
    o.textContent = label;
    sel.appendChild(o);
  });
  /* 当前码不在列表里时补一条兜底项。不然 sel.value 会静默落到第一项，
     一按「写入」就把键悄悄改成 'A' —— 用了已移除的 Num9 / 右Ctrl 就会踩到 */
  if (current && !list.some(([v]) => v === current)) {
    const o = document.createElement('option');
    o.value = current;
    o.textContent = `0x${current.toString(16).toUpperCase().padStart(4, '0')}（已弃用 / 未列出）`;
    sel.appendChild(o);
  }
  sel.value = current;
  if (!sel.value && list.length) sel.value = list[0][0];
}

function syncComboUI(code) {
  const isCombo = codeType(code) === T_COMBO;
  $('#comboField').hidden = !isCombo;
  const mod = isCombo ? ((code >> 8) & 0x0F) : 0;
  $$('#comboMods input').forEach(i => { i.checked = !!(mod & +i.value); });
}

function renderSidebar() {
  const isKnob = S.selIsKnob;
  const code = isKnob ? S.enc.push : S.keys[S.selected];
  const item = isKnob ? KNOB : LAYOUT.find(k => k.id === S.selected);

  $('#selTitle').textContent = isKnob ? '旋钮 (EC11)' : `按键 ${item.id + 1}`;
  $('#selSub').textContent = isKnob
    ? `${item.ref} · 旋转 + 按压 · ${hex(code)}`
    : `${item.ref} · 矩阵 R${item.row + 1}C${item.col + 1} · ${hex(code)}`;

  /* 一个页签装两种编辑器：选键 = 单个功能码，选旋钮 = 三向 + 极性 */
  $('#keyEditor').hidden = isKnob;
  $('#knobEditor').hidden = !isKnob;

  if (isKnob) {
    $('#encCW').innerHTML = buildCodeOptions(S.enc.cw);
    $('#encCCW').innerHTML = buildCodeOptions(S.enc.ccw);
    $('#encPush').innerHTML = buildCodeOptions(S.enc.push);
    $('#encInvert').value = S.enc.invert;
  } else {
    const rawType = codeType(code);
    const isPointer = (rawType === T_MOUSE || rawType === T_MOVE);
    const isMove = (rawType === T_MOVE);
    $('#keyType').value = isPointer ? T_POINTER : rawType;
    /* 光标码要剥掉速度档位再去找选项，否则 0x0721 匹配不到「光标 ↑」 */
    const selVal = isPointer ? (isMove ? (code & 0xFF0F) : code) : codeValue(code);
    fillValueSelect(isPointer ? T_POINTER : rawType, selVal);
    $('#keyRaw').value = hex(code);
    syncComboUI(code);

    /* 只有光标类按键才显示灵敏度滑杆 */
    $('#moveSpeedField').hidden = !isMove;
    if (isMove) {
      const lv = moveLv(code);
      $('#moveSpeed').value = lv;
      $('#moveSpeedVal').textContent = moveLabel(lv);
    }
  }

  // 灯光页的全局参数
  $('#ledMode').value = S.led.mode;
  $('#ledBrightness').value = S.led.brightness;
  $('#ledBriVal').textContent = S.led.brightness + '%';
  $('#ledSpeed').value = S.led.speed;
  $('#ledSpdVal').textContent = S.led.speed + '%';
  const hexc = '#' + [S.led.r, S.led.g, S.led.b].map(v => v.toString(16).padStart(2, '0')).join('');
  $('#ledColor').style.background = hexc;
  $('#ledColorHex').value = hexc.toUpperCase();

  // 动作页：选中键若已绑定动作，自动跳到那个槽位
  const hs = hostSlotOfSelected();
  if (hs >= 0) S.actSel = hs;

  renderLedPane();
  renderActPane();
  updatePower();
}

function buildCodeOptions(current) {
  const groups = [
    ['禁用', [[0, '（无）']]],
    ['键盘', KEYS.filter(k => k[0] >= 0x04).map(k => [(T_KEY << 8) | k[0], k[1]])],
    ['多媒体', MEDIA.map(m => [(T_MEDIA << 8) | m[0], m[1]])],
    ['鼠标与光标', MOUSE.map(m => [(T_MOUSE << 8) | m[0], m[1]])
      .concat(MOVES.map(m => [(T_MOVE << 8) | m[0], m[1]]))],
    ['灯效设置', ACTIONS.map(a => [(T_ACTION << 8) | a[0], a[1]])]
  ];
  let html = '';
  groups.forEach(([name, list]) => {
    html += `<optgroup label="${name}">`;
    list.forEach(([v, l]) => {
      html += `<option value="${v}"${v === current ? ' selected' : ''}>${l}</option>`;
    });
    html += '</optgroup>';
  });
  return html;
}

/* 当前选中的键对应的灯珠序号；旋钮没有独立灯珠，返回 -1 */
function ledIndexOfSel() {
  if (S.selIsKnob) return -1;
  const k = LAYOUT.find(x => x.id === S.selected);
  return k ? LED_ORDER[k.id] : -1;
}

function rgbHex(c) {
  return '#' + [c.r, c.g, c.b].map(v => v.toString(16).padStart(2, '0')).join('');
}

/* 灯光页：和「按键」页一样，跟着左边选中的键走 */
function renderLedPane() {
  const idx = ledIndexOfSel();
  const item = S.selIsKnob ? KNOB : LAYOUT.find(k => k.id === S.selected);
  const name = $('#ledKeyName'), sub = $('#ledKeySub'), sw = $('#ledSw');

  if (!item) {
    name.textContent = '未选择';
    sub.textContent = '在左边点一个键';
    sw.style.background = '#555';
  } else if (idx < 0) {
    name.textContent = '旋钮（无灯珠）';
    sub.textContent = '11 颗灯珠对应 11 个按键';
    sw.style.background = '#555';
  } else {
    const c = S.led.perKey[idx] || { r: 0, g: 0, b: 0 };
    name.textContent = `按键 ${item.id + 1}`;
    sub.textContent = `${item.ref} · 灯珠 ${LED_REF[idx] || (idx + 1)} · ${rgbHex(c).toUpperCase()}`;
    sw.style.background = rgbHex(c);
    $('#ledKeyColor').style.background = rgbHex(c);
    $('#ledKeyHex').value = rgbHex(c).toUpperCase();
  }

  const dis = idx < 0;
  ['#ledKeyColor', '#ledKeyHex', '#btnKeyUseMaster', '#btnKeyClear']
    .forEach(id => { $(id).disabled = dis; });
}

function setSelLed(rgb) {
  const idx = ledIndexOfSel();
  if (idx < 0) return;
  S.led.perKey[idx] = rgb;
  renderLedPane();
  renderBoard();
  updatePower();
}

function hex2rgb(h) {
  return {
    r: parseInt(h.slice(1, 3), 16),
    g: parseInt(h.slice(3, 5), 16),
    b: parseInt(h.slice(5, 7), 16)
  };
}

/* ===================== 主机动作 ===================== */
function esc(s) {
  return String(s || '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

function actPlaceholder(t) {
  if (t === 'url') return 'https://…';
  if (t === 'app') return '点右侧「浏览」选程序';
  if (t === 'folder') return '点右侧「浏览」选文件夹';
  return '命令行';
}

const ACT_KEY = 'kf.actions.v3';   /* 升版：清掉旧版本被 bug 写坏的 type */
const HOST_TYPE_IDS = HOST_TYPES.map(t => t[0]);

function loadActions() {
  try {
    const raw = localStorage.getItem(ACT_KEY);
    if (raw) {
      const arr = JSON.parse(raw);
      if (Array.isArray(arr) && arr.length === N_ACTIONS) {
        // 校验：type 必须是已知类型，否则回落到 url
        return arr.map(a => ({
          name: String(a.name || ''),
          type: HOST_TYPE_IDS.includes(a.type) ? a.type : 'url',
          target: String(a.target || '')
        }));
      }
    }
  } catch (_) {}
  return DEFAULT_ACTIONS.map(a => ({ ...a }));
}

let actSyncTimer = null, actSyncLogged = false;

/* 打包成 actions.json 的结构（导出和自动同步共用同一份逻辑） */
function actionsPayload() {
  const obj = { vid: '0x1209', pid: '0x0001', actions: {} };
  S.actions.forEach((a, i) => {
    if (a.name || a.target) {
      obj.actions[i] = { name: a.name || ('动作 ' + i), type: a.type, target: a.target };
    }
  });
  return obj;
}

/* 桌面版：改完槽位自动写进 tools/host-agent/actions.json，不用再点导出。
   网页版没有本地写权限，走原来的「导出 actions.json」。 */
function syncActionsToAgent() {
  if (!BRIDGE) return;
  clearTimeout(actSyncTimer);
  actSyncTimer = setTimeout(async () => {
    try {
      const r = await bridge.call('writeActions',
        { data: JSON.stringify(actionsPayload(), null, 2) }, 4000);
      if (r && r.ok) {
        if (!actSyncLogged) {
          actSyncLogged = true;
          log('动作表已自动同步到 host-agent/actions.json', 'ok');
        }
      } else {
        log('动作表自动同步失败：' + ((r && r.error) || '未知原因'), 'warn');
      }
    } catch (e) { log('动作表自动同步失败：' + e.message, 'warn'); }
  }, 400);
}

function saveActions() {
  try { localStorage.setItem(ACT_KEY, JSON.stringify(S.actions)); } catch (_) {}
  syncActionsToAgent();
}

/* 当前选中的键绑定的动作槽位；不是主机动作则返回 -1 */
function hostSlotOfSelected() {
  const code = S.selIsKnob ? S.enc.push : S.keys[S.selected];
  return codeType(code) === T_HOST ? (code & 0xFF) : -1;
}

/* 动作页：同样跟着左边选中的键走 */
function renderActPane() {
  const item = S.selIsKnob ? KNOB : LAYOUT.find(k => k.id === S.selected);
  const code = S.selIsKnob ? S.enc.push : S.keys[S.selected];
  const slot = hostSlotOfSelected();

  if (!item) {
    $('#actTitle').textContent = '未选择';
    $('#actSub').textContent = '在左边点一个键';
  } else {
    $('#actTitle').textContent = S.selIsKnob ? '旋钮按下 (EC11)' : `按键 ${item.id + 1}`;
    $('#actSub').textContent = slot >= 0
      ? `${item.ref}. 已绑定动作 ${slot + 1}`
      : `${item.ref}. 当前：${codeToLabel(code)}`;
  }
  $('#actSw').style.background = slot >= 0 ? 'var(--gold)' : '#555';

  const sel = $('#actSlot');
  sel.innerHTML = S.actions.map((a, i) =>
    `<option value="${i}">${i + 1}. ${a.name || '（空）'}</option>`).join('');
  sel.value = String(clamp(S.actSel, 0, N_ACTIONS - 1));

  if (!$('#actType').options.length) {
    $('#actType').innerHTML = HOST_TYPES.map(t => `<option value="${t[0]}">${t[1]}</option>`).join('');
  }

  fillActFields();
  renderSlotMap();
}

function fillActFields() {
  const a = S.actions[S.actSel] || { name: '', type: 'url', target: '' };
  $('#actName').value = a.name || '';
  $('#actType').value = a.type || 'url';
  $('#actTarget').value = a.target || '';
  $('#actTarget').placeholder = actPlaceholder(a.type || 'url');
  const pb = $('#btnActPick');
  if (pb) {
    const need = a.type === 'app' || a.type === 'folder';
    pb.disabled = !need;
    pb.title = need ? '在文件管理器里选择' : '只有「软件」和「文件夹」需要选路径';
  }
}

function renderSlotMap() {
  const box = $('#slotMap');
  box.innerHTML = '';
  for (let i = 0; i < N_ACTIONS; i++) {
    const a = S.actions[i];
    const d = document.createElement('div');
    d.className = 'sm' + (!a.name && !a.target ? ' free' : '');
    d.textContent = `${i + 1}. ${a.name || '空'}`;
    d.title = `槽位 ${i + 1}\n名称：${a.name || '（未命名）'}\n类型：${a.type}\n目标：${a.target || '（未设置）'}`;
    d.addEventListener('click', () => { S.actSel = i; renderActPane(); });
    box.appendChild(d);
  }
}

/* 选路径：软件 → 文件对话框，文件夹 → 文件夹对话框。
   桌面版由 WebView2 宿主弹系统对话框；网页版拿不到本地路径，只能手输。 */
async function pickTarget(quiet) {
  const a = S.actions[S.actSel];
  if (!a) return;
  const kind = a.type === 'folder' ? 'folder' : a.type === 'app' ? 'file' : null;
  if (!kind) {
    if (!quiet) toast('只有「软件」和「文件夹」需要选路径', true);
    return;
  }
  if (!BRIDGE) {
    if (!quiet) {
      toast('网页版不能选路径，请用桌面版', true);
      log('网页版拿不到本地路径 —— 请用桌面版，或手动输入', 'warn');
    }
    return;
  }
  try {
    const r = await bridge.call('pick', { kind }, 600000);
    if (r && r.path) {
      a.target = r.path;
      saveActions(); fillActFields();
      log(`已选择路径：${r.path}`, 'ok');
    }
  } catch (e) {
    log('选择路径失败：' + e.message, 'err');
    toast(e.message, true);
  }
}

/* 试跑一个动作槽位。
   桌面版：交给宿主用 ShellExecute 真的打开（网页/软件/文件夹/命令都行）；
   网页版：只能开网页（浏览器沙箱里没法启动程序）。 */
async function runActionLocal(i) {
  const a = S.actions[i];
  if (!a) return;
  const label = a.name || ('动作 ' + i);

  if (!a.target) {
    log(`槽位 ${i}（${label}）还没填目标，先在「目标」里写上`, 'warn');
    toast('先填目标', true);
    return;
  }

  // 网页目标没写协议时补 https://，否则 ShellExecute 可能当成搜索词
  let target = String(a.target).trim();
  if (a.type === 'url' && !/^[a-z][a-z0-9+.-]*:\/\//i.test(target)) target = 'https://' + target;

  log(`试跑 [${i}] ${label} · ${a.type} · ${target}`, 'rx');

  if (BRIDGE) {
    try {
      await bridge.call('run', { type: a.type, target }, 5000);
      log(`已执行：${target}`, 'ok');
      toast('▶ ' + label);
    } catch (e) {
      log('执行失败：' + e.message, 'err');
      toast(e.message, true);
    }
    return;
  }

  if (a.type === 'url') {
    const ok = window.open(target, '_blank');
    if (!ok) { log('浏览器拦截了弹窗，允许后重试', 'warn'); toast('被浏览器拦截', true); }
    else toast('▶ ' + label);
    return;
  }
  log('网页版只能试跑「网页」类型；启动程序 / 开文件夹 / 跑命令请用桌面版或 host-agent', 'warn');
  toast('网页版不支持该类型', true);
}

/* ===================== 电流估算 ===================== */
const POWER_LIMIT = 400;   // mA，超过就不允许下发（固件侧同阈值）

function updatePower() {
  let mA = 11;  // CH552 @24MHz
  const b = S.led.brightness / 100;
  S.led.perKey.forEach(c => {
    const sum = (c.r + c.g + c.b) / 255;
    mA += (1 + sum * 19) * b;      // WS2812B 经验模型：静态 1mA + 每通道满值约 19mA
  });
  const total = Math.round(mA);
  const over = total > POWER_LIMIT;
  S.powerOver = over;

  $('#powerVal').textContent = total + ' mA';
  $('#powerBar').style.width = clamp(total / POWER_LIMIT * 100, 0, 100) + '%';
  $('#powerBar').style.background = over ? 'var(--redstone)'
    : total > POWER_LIMIT * 0.85 ? 'var(--gold)' : 'var(--grass)';

  const note = $('#powerNote');
  note.textContent = over
    ? `超出 ${POWER_LIMIT} mA 上限（多 ${total - POWER_LIMIT} mA），降亮度后才能下发`
    : `上限 ${POWER_LIMIT} mA · 余量 ${POWER_LIMIT - total} mA`;
  note.style.color = over ? '#8d2f2a' : '';

  const btn = $('#btnLedApply');
  btn.disabled = over;
  btn.textContent = over ? `超出 ${POWER_LIMIT} mA，无法下发` : '下发光效';
}

/* ===================== 交互 ===================== */
/* 选中一个键（或旋钮）。不再强制切页签 —— 三个页签共用这一份选择，
   在灯光页点左边的键，右侧就跟着换，这是统一交互的关键。 */
function selectKey(id, isKnob) {
  S.selected = id;
  S.selIsKnob = isKnob;
  renderBoard();
  renderSidebar();
}

function switchTab(name) {
  $$('.tab').forEach(t => t.classList.toggle('active', t.dataset.tab === name));
  $$('.tabpane').forEach(p => p.classList.toggle('active', p.dataset.pane === name));
}

function simPress(id, down) {
  if (down) {
    const code = S.keys[id];
    if (codeType(code) === T_HOST) runActionLocal(codeValue(code));
    S.pressed.add(id);
    renderBoard();
    setTimeout(() => {
      if (!S.testMode) return;
      S.pressed.delete(id);
      renderBoard();
    }, 130);
  }
}

function simKnob(dir) {
  S.knobCount += dir === 'cw' ? 1 : -1;
  S.knobAngle += dir === 'cw' ? 15 : -15;
  if (dir === 'push') {
    const k = $('#knobBtn');
    k.classList.add('press');
    setTimeout(() => k.classList.remove('press'), 130);
  }
  renderBoard();
}

/* ===================== 设备动作 ===================== */
async function doConnect() {
  try {
    await dev.connectReal();
    /* 先确认真能读到配置，再显示「已连接」。
       原来先置 online 再读，读失败也照样显示已连接，很容易误导。 */
    const ok = await loadAll();
    if (!ok) throw new Error('设备已打开但读取配置失败');
    S.online = true; S.demo = false;
    setStatus('online', '已连接');
  } catch (e) {
    try { dev.disconnect(); } catch (_) {}
    S.online = false; S.demo = false;
    setStatus('offline', '未连接');
    renderDevice();
    log('连接失败：' + e.message, 'err');
    toast(e.message, true);
  }
}

function doDemo() {
  dev.connectDemo();
  S.online = true; S.demo = true;
  setStatus('demo', '演示模式');
  loadAll();
}

async function loadAll() {
  try {
    const all = await dev.readAll();
    S.info = all.info;
    S.keys = all.keys;
    Object.assign(S.enc, all.enc);
    S.led = all.led;
    renderDevice();
    renderBoard();
    renderSidebar();
    renderLedPane();
    updatePower();
    log(`读取配置完成：${all.info.nkeys} 键 / ${all.info.nleds} 灯珠 / 协议 v${all.info.proto}`, 'ok');
    return true;
  } catch (e) {
    log('读取配置失败：' + e.message, 'err');
    toast(e.message, true);
    return false;
  }
}

/* 状态与按钮联动：连接成功后同一个按钮变成「断开」，省掉一个重复按钮 */
function setStatus(state, text) {
  $('#statusChip').dataset.state = state;
  $('#statusText').textContent = text;
  const b = $('#btnConnect');
  b.textContent = state === 'offline' ? '连接设备' : '断开';
  b.classList.toggle('btn-primary', state === 'offline');
  $('#btnDemo').hidden = state !== 'offline';   /* 已连接时藏起来，别和状态条重复 */
}

function doDisconnect() {
  dev.disconnect();
  S.online = false; S.demo = false; S.pressed.clear();
  S.info = null;                 /* 清掉设备面板，否则断开后仍显示固件版本等信息 */
  setStatus('offline', '未连接');
  renderDevice();
  renderBoard();
  log('已断开', 'warn');
}

async function toggleConnect() {
  if (S.online) doDisconnect();
  else await doConnect();
}

function renderDevice() {
  const i = S.info || {};
  $('#dvName').textContent = i.name || '—';
  $('#dvVid').textContent = S.demo ? 'DEMO' : `${hex(VID)}:${hex(PID)}`;
  $('#dvFw').textContent = i.fwMajor !== undefined ? `v${i.fwMajor}.${i.fwMinor}` : '—';
  $('#dvProto').textContent = i.proto !== undefined ? `v${i.proto}` : '—';
  $('#dvMatrix').textContent = i.rows ? `${i.rows} × ${i.cols}` : '3 × 4';
  $('#dvLed').textContent = i.nleds ? `${i.nleds} 颗 WS2812` : '11 颗 WS2812';
}

/* ===================== 事件绑定 ===================== */
function bind() {
  $('#btnConnect').addEventListener('click', toggleConnect);
  $('#btnDemo').addEventListener('click', doDemo);

  $$('.tab').forEach(t => t.addEventListener('click', () => switchTab(t.dataset.tab)));

  $('#testMode').addEventListener('change', e => {
    S.testMode = e.target.checked;
    log(S.testMode ? '进入按键测试模式' : '退出按键测试模式');
  });

  // 功能类型切换
  $('#keyType').addEventListener('change', e => {
    const t = +e.target.value;
    fillValueSelect(t, 0);
    $('#comboField').hidden = t !== T_COMBO;
    if (t === T_NONE) $('#keyRaw').value = '0x0000';
  });

  /* 光标灵敏度：拖动时只更新文字，松手（change）才写进设备，避免一路拖一路存 */
  $('#moveSpeed').addEventListener('input', e => {
    $('#moveSpeedVal').textContent = moveLabel(+e.target.value);
  });
  $('#moveSpeed').addEventListener('change', async e => {
    const lv = +e.target.value;
    const cur = S.keys[S.selected];
    if (codeType(cur) !== T_MOVE) return;
    await writeCurrent((cur & 0xFF0F) | (lv << 4));   /* 保留方向，只换档位 */
  });

  // 写入此键
  $('#btnAssign').addEventListener('click', async () => {
    const t = +$('#keyType').value;
    const v = +$('#keyValue').value || 0;
    let code;
    if (t === T_NONE) code = 0;
    else if (t === T_COMBO) {
      let mod = 0;
      $$('#comboMods input').forEach(i => { if (i.checked) mod |= +i.value; });
      code = (0x5000 | (mod << 8) | (v & 0xFF)) & 0xFFFF;
    } else if (t === T_HOST) {
      code = (0x0600 | (v & 0xFF)) & 0xFFFF;
    } else if (t === T_POINTER) {
      code = v & 0xFFFF;                    /* 值列表里已经是完整功能码（0x03xx / 0x07xx） */
      /* 换方向时把原来的灵敏度档位带上，否则一改方向速度就被打回默认 */
      if ((code & 0xFF00) === (T_MOVE << 8)) {
        const cur = S.keys[S.selected] || 0;
        if (codeType(cur) === T_MOVE) code = (code & 0xFF0F) | (cur & 0x00F0);
      }
    } else code = ((t << 8) | (v & 0xFF)) & 0xFFFF;
    await writeCurrent(code);
  });

  $('#btnApplyRaw').addEventListener('click', async () => {
    const v = parseCode($('#keyRaw').value);
    if (v === null) { toast('格式应为 0x0000', true); return; }
    await writeCurrent(v);
  });

  $('#btnClearKey').addEventListener('click', () => writeCurrent(0));

  // 旋钮写入
  $('#btnEncApply').addEventListener('click', async () => {
    const enc = {
      cw: +$('#encCW').value, ccw: +$('#encCCW').value,
      push: +$('#encPush').value, invert: +$('#encInvert').value
    };
    if (!S.online) { toast('设备未连接，先连上再写入', true); return; }
    try {
      await dev.writeEnc(enc);
      /* 落盘由 writeEnc/writeKey 内部的 _scheduleSave 统一处理 */
      Object.assign(S.enc, enc);
      renderBoard(); renderSidebar();
      log(`旋钮已写入并保存：CW=${codeToLabel(enc.cw)} CCW=${codeToLabel(enc.ccw)} `
        + `按下=${codeToLabel(enc.push)}`, 'tx');
      toast('旋钮设置已写入');
    } catch (e) { log('写入失败：' + e.message, 'err'); toast(e.message, true); }
  });

  /* 旋钮三个下拉框：一改就同步进 S.enc。
     否则任何一次 renderSidebar 重绘都会按 S.enc 重建 <select>，把刚选的值冲掉。 */
  ['#encCW', '#encCCW', '#encPush'].forEach(sel => {
    const el = $(sel);
    if (!el) return;
    el.addEventListener('change', () => {
      S.enc.cw = +$('#encCW').value;
      S.enc.ccw = +$('#encCCW').value;
      S.enc.push = +$('#encPush').value;
      const sub = $('#selSub');
      if (sub) sub.textContent = `${KNOB.ref} · 旋转 + 按压 · ${hex(S.enc.push)}`;
    });
  });

  // 主机动作
  $('#btnActExport').addEventListener('click', async () => {
    const obj = actionsPayload();
    const path = await saveText('actions.json', JSON.stringify(obj, null, 2), 'json');
    if (!path) return;
    log(`已导出：${path}`, 'ok');
    log('放到 tools/host-agent/ 下，运行 python agent.py 生效', 'rx');
    toast('actions.json 已保存');
  });
  $('#btnActTest').addEventListener('click', () => runActionLocal(S.actSel));
  $('#btnActPick').addEventListener('click', () => pickTarget(false));

  // 动作：槽位 / 字段编辑（都作用于"当前选中键"所关联的槽位）
  $('#actSlot').addEventListener('change', e => {
    S.actSel = +e.target.value;
    fillActFields();
  });
  $('#actName').addEventListener('input', e => {
    S.actions[S.actSel].name = e.target.value;
    saveActions(); renderActPane(); renderBoard();
  });
  $('#actType').addEventListener('change', e => {
    const a = S.actions[S.actSel];
    if (!a || a.type === e.target.value) return;
    a.type = e.target.value;
    a.target = '';
    saveActions(); renderActPane();
    // 软件 / 文件夹 直接弹选择框，省一次手动输入
    if (a.type === 'app' || a.type === 'folder') pickTarget(true);
  });
  $('#actTarget').addEventListener('input', e => {
    S.actions[S.actSel].target = e.target.value;
    saveActions();
  });

  // 灯光
  $('#ledBrightness').addEventListener('input', e => {
    S.led.brightness = +e.target.value;
    $('#ledBriVal').textContent = S.led.brightness + '%';
    renderBoard(); updatePower();
  });
  $('#ledSpeed').addEventListener('input', e => {
    S.led.speed = +e.target.value;
    $('#ledSpdVal').textContent = S.led.speed + '%';
  });
  $('#ledMode').addEventListener('change', e => {
    S.led.mode = +e.target.value;
    renderBoard(); updatePower();
  });
  // 主色：点色块开调色板，手输 hex 直接生效
  $('#ledColor').addEventListener('click', () => openColorPicker('master', $('#ledColor'), { r: S.led.r, g: S.led.g, b: S.led.b }));
  $('#ledColorHex').addEventListener('change', e => {
    const v = String(e.target.value).trim();
    if (!/^#?[0-9a-fA-F]{6}$/.test(v)) { toast('格式应为 #RRGGBB', true); renderSidebar(); return; }
    const c = hex2rgb(v.startsWith('#') ? v : '#' + v);
    S.led.r = c.r; S.led.g = c.g; S.led.b = c.b;
    renderSidebar(); renderBoard(); updatePower();
  });

  // 选中键的颜色
  $('#ledKeyColor').addEventListener('click', () => {
    const idx = ledIndexOfSel();
    if (idx < 0) return;
    const c = S.led.perKey[idx] || { r: 0, g: 0, b: 0 };
    openColorPicker('key', $('#ledKeyColor'), c);
  });
  $('#ledKeyHex').addEventListener('change', e => {
    const v = String(e.target.value).trim();
    if (!/^#[0-9a-fA-F]{6}$/.test(v)) { toast('格式应为 #RRGGBB', true); renderLedPane(); return; }
    setSelLed(hex2rgb(v));
  });
  // 调色板
  [['#cpH', () => cpH = +$('#cpH').value],
   ['#cpS', () => cpS = +$('#cpS').value],
   ['#cpV', () => cpV = +$('#cpV').value]].forEach(([sel, set]) => {
    $(sel).addEventListener('input', () => { set(); syncCpPreview(); });
  });
  $('#cpHex').addEventListener('change', e => {
    const v = String(e.target.value).trim();
    if (!/^#?[0-9a-fA-F]{6}$/.test(v)) { syncCpPreview(); return; }
    setCpHex(v.startsWith('#') ? v : '#' + v);
  });
  $('#cpPresets').addEventListener('click', e => {
    const el = e.target.closest('i[data-c]');
    if (el) setCpHex(el.dataset.c);
  });
  $('#cpOk').addEventListener('click', applyColorPicker);
  $('#cpCancel').addEventListener('click', closeColorPicker);
  document.addEventListener('mousedown', e => {
    if ($('#cpPop').hidden) return;
    if (e.target.closest('#cpPop') || e.target.closest('.swatch')) return;
    closeColorPicker();
  });

  $('#btnKeyUseMaster').addEventListener('click', () => setSelLed({ r: S.led.r, g: S.led.g, b: S.led.b }));
  $('#btnKeyClear').addEventListener('click', () => setSelLed({ r: 0, g: 0, b: 0 }));

  $('#btnFillAll').addEventListener('click', () => {
    S.led.perKey = S.led.perKey.map(() => ({ r: S.led.r, g: S.led.g, b: S.led.b }));
    renderLedPane(); renderBoard(); updatePower();
  });
  $('#btnRainbowFill').addEventListener('click', () => {
    S.led.perKey = S.led.perKey.map((_, i) => hsv2rgb(i / NLEDS * 360, 1, 1));
    renderLedPane(); renderBoard(); updatePower();
  });
  $('#btnLedApply').addEventListener('click', async () => {
    if (S.powerOver) {
      toast(`超出 ${POWER_LIMIT} mA，先降低亮度`, true);
      log(`拒绝下发：估算 ${$('#powerVal').textContent} 超过上限 ${POWER_LIMIT} mA`, 'err');
      return;
    }
    try {
      await dev.writeLed(S.led);
      log('灯效已下发', 'tx');
      toast('灯效已下发');
    } catch (e) { log('下发失败：' + e.message, 'err'); toast(e.message, true); }
  });

  // 设备操作
  $('#btnSave').addEventListener('click', async () => {
    try { await dev.save(); toast('已保存到设备'); log('配置已写入 DataFlash', 'ok'); }
    catch (e) { toast(e.message, true); }
  });
  $('#btnReset').addEventListener('click', async () => {
    if (!confirm('恢复出厂设置？当前配置将被覆盖。')) return;
    try { await dev.factoryReset(); await loadAll(); toast('已恢复默认'); }
    catch (e) { toast(e.message, true); }
  });

  // 导入导出
  $('#btnExport').addEventListener('click', async () => {
    const data = {
      version: 1, created: new Date().toISOString(),
      layout: 'ch552-11key-ec11',
      keys: S.keys.map(hex),
      enc: { cw: hex(S.enc.cw), ccw: hex(S.enc.ccw), push: hex(S.enc.push), invert: S.enc.invert },
      led: {
        mode: S.led.mode, brightness: S.led.brightness, speed: S.led.speed,
        color: '#' + [S.led.r, S.led.g, S.led.b].map(v => v.toString(16).padStart(2, '0')).join(''),
        perKey: S.led.perKey.map(c => '#' + [c.r, c.g, c.b].map(v => v.toString(16).padStart(2, '0')).join(''))
      }
    };
    const path = await saveText('keyforge-profile.json', JSON.stringify(data, null, 2), 'json');
    if (!path) return;
    log(`配置已导出：${path}`, 'ok');
    toast('配置已保存');
  });

  $('#btnExportC').addEventListener('click', async () => {
    let s = '/* 由 MineKey 配置器生成 —— 粘贴到 firmware/src/config.h */\n';
    s += '#define DEFAULT_KEYMAP { \\\n  ';
    s += S.keys.map(c => hex(c)).join(', ') + ' \\\n}\n\n';
    s += '#define DEFAULT_ENC { \\\n  ';
    s += `${hex(S.enc.cw)}, ${hex(S.enc.ccw)}, ${hex(S.enc.push)}, ${S.enc.invert} \\\n}\n\n`;
    s += '#define DEFAULT_LED_MODE       ' + S.led.mode + '\n';
    s += '#define DEFAULT_LED_BRIGHTNESS ' + S.led.brightness + '\n';
    s += '#define DEFAULT_LED_RGB        { ' +
         [S.led.r, S.led.g, S.led.b].map(v => '0x' + v.toString(16).padStart(2, '0').toUpperCase()).join(', ') + ' }\n';
    const path = await saveText('keymap_default.h', s, 'h');
    if (!path) return;
    log(`已导出固件默认表：${path}`, 'ok');
    toast('keymap_default.h 已保存');
  });

  $('#fileImport').addEventListener('change', async e => {
    const f = e.target.files[0];
    if (!f) return;
    try {
      const txt = await f.text();
      applyProfile(JSON.parse(txt));
      toast('配置已导入');
    } catch (err) { toast('文件解析失败', true); log('导入失败：' + err.message, 'err'); }
    e.target.value = '';
  });

  $('#btnClearLog').addEventListener('click', () => { $('#log').innerHTML = ''; });
}

async function writeCurrent(code) {
  const idx = S.selected;
  try {
    if (S.selIsKnob) {
      S.enc.push = code;
      await dev.writeEnc(S.enc);
      /* 落盘由 writeEnc/writeKey 内部的 _scheduleSave 统一处理 */
    } else {
      await dev.writeKey(idx, code);
      S.keys[idx] = code;
    }
    log(`写入 ${S.selIsKnob ? '旋钮按下' : 'K' + (idx + 1)} = ${hex(code)} (${codeToLabel(code)})`, 'tx');
    renderBoard(); renderSidebar();
    toast('已写入');
  } catch (e) {
    log('写入失败：' + e.message, 'err');
    toast(e.message, true);
  }
}

function parseCode(str) {
  const s = String(str).trim();
  if (!/^(0x)?[0-9a-fA-F]{1,4}$/.test(s)) return null;
  return parseInt(s, 16);
}

function applyProfile(p) {
  if (p.keys) S.keys = p.keys.map(v => typeof v === 'string' ? parseCode(v) : v);
  if (p.enc) {
    S.enc.cw = parseCode(p.enc.cw); S.enc.ccw = parseCode(p.enc.ccw);
    S.enc.push = parseCode(p.enc.push); S.enc.invert = p.enc.invert | 0;
  }
  if (p.led) {
    S.led.mode = p.led.mode; S.led.brightness = p.led.brightness; S.led.speed = p.led.speed;
    const c = hex2rgb(p.led.color);
    S.led.r = c.r; S.led.g = c.g; S.led.b = c.b;
    if (p.led.perKey) S.led.perKey = p.led.perKey.map(h => hex2rgb(h));
  }
  renderBoard(); renderSidebar(); updatePower();
}

function download(name, text) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 2000);
}

/* 保存文本：桌面版弹「另存为」自己挑目录，网页版退回浏览器下载。
   返回实际路径（网页版返回文件名）；用户取消则返回 null。 */
async function saveText(name, text, filter) {
  if (BRIDGE) {
    try {
      const r = await bridge.call('save', { name, data: text, filter: filter || '' }, 600000);
      if (!r || r.canceled) { log('已取消保存', 'warn'); return null; }
      return r.path || name;
    } catch (e) {
      log('保存失败：' + e.message, 'err');
      toast(e.message, true);
      return null;
    }
  }
  download(name, text);
  return name;
}

/* ===================== 调色板 ===================== */
/* WebView2 不实现 <input type=color> 的系统对话框（点了没任何反应），
   所以自己画一个 HSV 面板。定位用 offsetTop（相对 .tabpane），
   天然不受 #app 的 CSS zoom 影响，不用做坐标换算。 */
let cpTarget = null;            // 'master' | 'key'
let cpH = 20, cpS = 80, cpV = 97;

const CP_PRESETS = [
  '#f77634', '#e5484d', '#f5a623', '#f5d90a', '#46a758',
  '#12a594', '#33d6c0', '#3e63dd', '#8e4ec6', '#c084fc',
  '#ffffff', '#c6c6c6', '#8b8b8b', '#5a5a5a', '#2b2b2b'
];

function rgb2hsv(r, g, b) {
  r /= 255; g /= 255; b /= 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b);
  const d = max - min;
  let h = 0;
  if (d > 0) {
    if (max === r) h = ((g - b) / d) % 6;
    else if (max === g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h *= 60;
    if (h < 0) h += 360;
  }
  /* 不取整：s/v 取整会让 #F77634 回算成 #F77534，色值必须原样往返 */
  return {
    h: h,
    s: max > 0 ? (d / max) * 100 : 0,
    v: max * 100
  };
}

function cpColor() { return hsv2rgb(cpH, cpS / 100, cpV / 100); }

function syncCpPreview() {
  const hx = rgbHex(cpColor()).toUpperCase();
  $('#cpPrev').style.background = hx;
  $('#cpHex').value = hx;
}

function openColorPicker(target, anchor, rgb) {
  const hsv = rgb2hsv(rgb.r, rgb.g, rgb.b);
  cpTarget = target;
  cpH = hsv.h; cpS = hsv.s; cpV = hsv.v;

  $('#cpH').value = Math.round(cpH);
  $('#cpS').value = Math.round(cpS);
  $('#cpV').value = Math.round(cpV);
  if (!$('#cpPresets').children.length) {
    $('#cpPresets').innerHTML = CP_PRESETS
      .map(c => `<i data-c="${c}" style="background:${c}"></i>`).join('');
  }
  syncCpPreview();

  const pop = $('#cpPop');
  pop.hidden = false;
  pop.style.top = '0px';                     // 先归零才能量到真实高度
  const hgt = pop.offsetHeight;
  const below = anchor.offsetTop + anchor.offsetHeight + 6;
  const pane = anchor.closest('.tabpane');
  // 下方放不下就翻到色块上面
  pop.style.top = (below + hgt > pane.clientHeight - 6)
    ? Math.max(6, anchor.offsetTop - hgt - 6) + 'px'
    : below + 'px';
}

function closeColorPicker() {
  $('#cpPop').hidden = true;
  cpTarget = null;
}

function applyColorPicker() {
  const c = cpColor();
  if (cpTarget === 'master') {
    S.led.r = c.r; S.led.g = c.g; S.led.b = c.b;
    $('#ledColorHex').value = rgbHex(c).toUpperCase();
  } else if (cpTarget === 'key') {
    const idx = ledIndexOfSel();
    if (idx >= 0) {
      S.led.perKey[idx] = { r: c.r, g: c.g, b: c.b };
      $('#ledKeyHex').value = rgbHex(c).toUpperCase();
    }
  }
  closeColorPicker();
  renderSidebar(); renderBoard(); updatePower();
}

function setCpHex(hexStr) {
  const c = hex2rgb(hexStr);
  const hsv = rgb2hsv(c.r, c.g, c.b);
  cpH = hsv.h; cpS = hsv.s; cpV = hsv.v;
  $('#cpH').value = Math.round(cpH);
  $('#cpS').value = Math.round(cpS);
  $('#cpV').value = Math.round(cpV);
  syncCpPreview();
}

function hsv2rgb(h, s, v) {
  const c = v * s, x = c * (1 - Math.abs((h / 60) % 2 - 1)), m = v - c;
  let r = 0, g = 0, b = 0;
  if (h < 60) { r = c; g = x; }
  else if (h < 120) { r = x; g = c; }
  else if (h < 180) { g = c; b = x; }
  else if (h < 240) { g = x; b = c; }
  else if (h < 300) { r = x; b = c; }
  else { r = c; b = x; }
  return { r: Math.round((r + m) * 255), g: Math.round((g + m) * 255), b: Math.round((b + m) * 255) };
}

/* ===================== 动画 ===================== */
function tick() {
  S.ledPhase += 0.02 * (0.2 + S.led.speed / 100 * 1.8);
  if (S.led.mode !== 0 && S.led.mode !== 3 && S.led.mode !== 4) renderBoard();
  requestAnimationFrame(tick);
}

/* ===================== 设备上报 ===================== */
dev.onReport = (id, data) => {
  if (id === R_STATUS) {
    const m0 = data[0], m1 = data[1], encDelta = (data[2] << 24) >> 24;
    const pressed = new Set();
    for (let bit = 0; bit < 12; bit++) {
      const v = bit < 8 ? (m0 >> bit) & 1 : (m1 >> (bit - 8)) & 1;
      if (!v) continue;
      if (bit === 3) S.knobPressed = true;          // 矩阵 (R1,C4) = 编码器按压
      else pressed.add(MATRIX_TO_KEY[bit]);
    }
    const changed = pressed.size !== S.pressed.size ||
      Array.from(pressed).some(i => !S.pressed.has(i));
    S.pressed = pressed;
    if (encDelta) { S.knobCount += encDelta; S.knobAngle += encDelta * 15; }
    const kn = $('#knobBtn');
    if (kn) kn.classList.toggle('press', !!S.knobPressed);
    if (changed || encDelta) renderBoard();
  } else if (id === 5) {
    /* 固件上报的主机动作编号 */
    runActionLocal(data[0] & 0x3F);
  } else if (id === R_KBD) {
    // 键盘报文：可用于在界面上回显真实上报内容
    const mod = data[0];
    const keys = Array.from(data.slice(2, 8)).filter(v => v);
    log(`KBD mod=${hex(mod, 2)} keys=[${keys.map(k => hex(k, 2)).join(' ')}]`, 'rx');
  }
};

dev.onGone = () => {
  S.online = false; S.demo = false;
  setStatus('offline', '未连接');
  renderBoard();
};

/* ===================== 布局适配 =====================
 * 窗口可自由缩放：整页用 zoom 等比缩放，再把 #app 撑满窗口，避免大片黑边；
 * 键位按下算出来的方格尺寸排版，窗口大小变了也不会挤压 / 变形。
 */
const DESIGN_W = 980, DESIGN_H = 920;   /* 设计基准：内容刚好放下、不留空 */

function fitApp() {
  const app = document.getElementById('app');
  if (!app) return;
  const w = window.innerWidth, h = window.innerHeight;
  let z = Math.min(w / DESIGN_W, h / DESIGN_H);
  z = clamp(Math.round(z * 100) / 100, 0.45, 2.0);
  app.style.zoom = z;
  app.style.width = Math.floor(w / z) + 'px';
  app.style.height = Math.floor(h / z) + 'px';
  fitBoard();
}

/* 键位网格：4 列 × 3 行，格子必须方正（宽高都取画布可用空间的最小值），
   画布本身是弹性的，窗口放大时画布变大、键跟着变大，不会留大片空白。 */
function fitBoard() {
  const wrap = document.querySelector('.board-wrap');
  if (!wrap) return;

  const BOX_PAD = 32;   // .board-wrap 内边距 16×2
  const GAPS    = 20;   // 3 行之间 2 个 10px 间隙

  const maxByW = (wrap.clientWidth - BOX_PAD - 30) / 4;
  const maxByH = (wrap.clientHeight - BOX_PAD - GAPS) / 3;
  const cell = clamp(Math.floor(Math.min(maxByW, maxByH)), 38, 152);

  document.documentElement.style.setProperty('--key', cell + 'px');
}

/* ===================== 启动 ===================== */
(function init() {
  buildBoard();
  bind();
  fitApp();
  window.addEventListener('resize', fitApp);
  if (window.ResizeObserver) {
    new ResizeObserver(fitBoard).observe(document.querySelector('.board-wrap'));
  }
  S.actions = loadActions();
  renderBoard();
  renderSidebar();
  renderLedPane();
  renderDevice();
  updatePower();
  tick();

  // 默认给一套能用的配置，方便第一次打开就有东西看
  S.keys = [
    (T_KEY << 8) | 0x29, (T_KEY << 8) | 0x2C, (T_MEDIA << 8) | 0xE2,
    (T_KEY << 8) | 0x1E, (T_KEY << 8) | 0x1F, (T_KEY << 8) | 0x20, (T_KEY << 8) | 0x21,
    (T_KEY << 8) | 0x22, (T_KEY << 8) | 0x23, (T_KEY << 8) | 0x24, (T_KEY << 8) | 0x25
  ];
  S.enc = {
    cw: (T_MEDIA << 8) | 0xE9, ccw: (T_MEDIA << 8) | 0xEA,
    push: (T_MEDIA << 8) | 0xCD, invert: 0
  };
  renderBoard(); renderSidebar();

  log('配置器已就绪', 'ok');
  if (BRIDGE) {
    log('桌面版：原生 HID 通道就绪。点「连接设备」直接打开键盘，或点「演示模式」预览界面。', 'ok');
    document.body.dataset.shell = 'desktop';
  } else if (!('hid' in navigator)) {
    log('当前浏览器不支持 WebHID —— 请用 Chrome / Edge 89+ 打开，或先点「演示模式」', 'warn');
  } else {
    log('WebHID 可用。点「连接设备」选择键盘，或点「演示模式」预览界面。');
  }

  /* 便于演示 / 截图：index.html?tab=led&demo=1 */
  const q = new URLSearchParams(location.search);
  if (q.get('tab')) switchTab(q.get('tab'));
  if (q.get('demo') === '1' && $('#btnDemo') && !$('#btnDemo').disabled) doDemo();
})();
