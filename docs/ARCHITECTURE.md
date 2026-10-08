# Ghost Browser — 架构设计

> 面向 Agent / 自动化工具的指纹浏览器。
> 核心命题：**指纹修改必须发生在 JS 层以下**，控制通道**不引入 CDP 特征**。

---

## 1. 为什么"注入脚本"必然失败

现有反检测方案（Playwright stealth、puppeteer-extra-plugin-stealth）都在 JS 层打补丁，因此存在**结构性不可修复**的检测面：

| 检测手法 | 为什么 JS 注入挡不住 |
|---|---|
| `Function.prototype.toString.call(navigator.__lookupGetter__('webdriver'))` | 被 `defineProperty` 替换的 getter 返回的是**用户 JS 函数**，`toString()` 会打印出源码，而不是 `[native code]` |
| `Object.getOwnPropertyDescriptor(Navigator.prototype, 'hardwareConcurrency')` | 注入会改变 getter/setter 的存在性与可配置性 |
| `Object.getOwnPropertyNames(window)` 差集 | 任何 `window.__xxx` 注入都会暴露 |
| iframe / Worker / 新 Realm 遍历 | 必须为每个 Realm 重复注入，漏一个就露馅；且 `about:blank` iframe 的注入时机难以保证 |
| 时序 / 堆形状 | 注入脚本会改变脚本编译顺序、`performance.now()` 分布、JS 堆对象数量 |
| `Error.prepareStackTrace` / stack 形状 | 注入框架会在调用栈里留下自己的帧 |
| `document.currentScript` / CSP 报告 | 部分注入方式会被 CSP 上报 |

**结论：JS 注入是"在被监视的房间里演戏"，原生层修改是"把房间本身改掉"。**

---

## 2. 五层架构

```
┌──────────────────────────────────────────────────────────────────┐
│ L5  行为引擎  ghostbehavior                                       │
│     贝塞尔鼠标 / digraph 打字节奏 / 滚动物理 / 阅读停顿            │
│     ↓ 全部通过 OS 输入合成下发（真实 trusted 事件）                │
├──────────────────────────────────────────────────────────────────┤
│ L4  验证码    ghostcaptcha                                        │
│     CF 静默通过 → cf_clearance 复用 → hCaptcha 音频(Whisper) → 兜底 │
├──────────────────────────────────────────────────────────────────┤
│ L3  控制面    ghostd  (Rust daemon, 零 CDP)                       │
│     OS 输入合成 · 无障碍树读取 · OS 级截屏 · profile SQLite 直读   │
├──────────────────────────────────────────────────────────────────┤
│ L2  指纹档案  ghostprof                                           │
│     相干设备档案生成 / 校验 / 种子派生（HMAC(seed, origin)）        │
├──────────────────────────────────────────────────────────────────┤
│ L1  原生 Shim ghost_shim  (C/C++, 跨平台)                         │
│     OS API Hook · ANGLE 导出 Hook · 二进制补丁 · 注入与传播        │
├──────────────────────────────────────────────────────────────────┤
│ L0  引擎      Chromium（预编译，按版本锁定 + 补丁器）              │
└──────────────────────────────────────────────────────────────────┘
```

---

## 3. L1 原生 Shim — 本项目的技术核心

### 3.1 关键约束（实测得出）

对官方 Chrome 154.0.8037.98 的实测：

```
chrome.dll      exports: nfunc=5  nnames=5   → 仅 ChromeMain 等 5 个
chrome_elf.dll  exports: nfunc=27            → 全是 *_ExportThunk 崩控钩子
PDB 附带:       无
```

**因此：靠符号名定位 `blink::CanvasRenderingContext2D::getImageData` 这类内部函数不可行**；只能字节特征码逆向，且每个 Chromium 版本重做，ROI 极差。

架构据此避开 Blink 内部符号，改走三条**可符号定位 / 稳定**的通道：

### 3.2 通道 A — OS API Hook（覆盖面最大）

Chromium 的 JS 可见指纹有相当一部分**直接源自操作系统 API**。在 JS 层以下拦截这些调用，页面完全无法区分：

| JS 可见属性 | Chromium 内部调用 | 拦截点 |
|---|---|---|
| `navigator.hardwareConcurrency` | `base::SysInfo::NumberOfProcessors()` | `GetActiveProcessorCount` / `GetSystemInfo` / `GetLogicalProcessorInformationEx` |
| `navigator.deviceMemory` | `base::SysInfo::AmountOfPhysicalMemoryMB()` | `GlobalMemoryStatusEx` |
| `screen.width/height/availWidth/availHeight` | `display::Screen::GetPrimaryDisplay()` | `EnumDisplayMonitors` / `GetMonitorInfoW` / `GetSystemMetrics` |
| `window.devicePixelRatio` | `display::Display::device_scale_factor()` | `GetDpiForMonitor` / `GetDpiForWindow` / `GetDeviceCaps` |
| `Intl.DateTimeFormat().resolvedOptions().timeZone` | `icu::TimeZone::createDefault()` | `GetDynamicTimeZoneInformation` / `GetTimeZoneInformation` |
| 字体列表 | DirectWrite | `dwrite.dll!DWriteCreateFactory` → 包装返回的 factory vtable |
| WebGL `UNMASKED_VENDOR/RENDERER` | ANGLE / DXGI | `dxgi.dll!CreateDXGIFactory1` → 包装 `IDXGIAdapter::GetDesc1` |

**Linux 对应**：`sched_getaffinity` / `sysconf(_SC_NPROCESSORS_ONLN)`、`sysinfo()`、`XineramaQueryScreens` / `XRRGetScreenResources`、`gdk_monitor_get_scale_factor`、`/etc/localtime`+`TZ`、`fontconfig`（`FcFontList`/`FcFontSort`）、`libGL`/EGL。
**macOS 对应**：`sysctlbyname("hw.ncpu"/"hw.memsize")`、`CGGetActiveDisplayList` / `CGDisplayBounds`、`CGDisplayPixelsWide`、`NSTimeZone`/`CFTimeZoneCopySystem`、CoreText `CTFontCollectionCreateFromAvailableFonts`、`CGLGetParameter`。

### 3.3 通道 B — WebGL 身份（原方案已作废，现行方案见 §11）

> **⚠️ 本节原方案已被实测推翻（2026-10-06）。** 原设计假设 `libGLESv2.dll` / `libEGL.dll`
> 是独立 DLL、导出完整 GL 入口，可以 hook `glGetString` 等。**现代 Chrome 上该假设不成立**，
> 详见 §11.1。以下保留原文仅供追溯，**不要照此实现**。

原方案（已作废，勿实现）：

- ~~`glGetString(GL_VENDOR/GL_RENDERER/GL_VERSION/GL_SHADING_LANGUAGE_VERSION)`~~
- ~~`glGetIntegerv(GL_MAX_*)`、`glGetFloatv`（WebGL 参数哈希）~~
- ~~`glReadPixels`（WebGL 指纹读取路径）~~
- ~~`eglGetProcAddress` → 包装以捕获扩展函数指针~~

**现行方案：改写 ANGLE 读取的 DXGI 适配器身份。** ANGLE 自己用
`IDXGIAdapter::GetDesc()` 的 `Description` 与 `DeviceId` 拼出 `GL_RENDERER`，而该 COM
vtable 位于 `dxgi.dll`，patch 它**不需要任何 `chrome.dll` 符号逆向**。完整说明见 **§11**。

### 3.4 通道 C — 二进制补丁

对剥离符号的二进制做**字符串常量与简单指令模式**补丁，不依赖符号表：

- `--enable-automation` 相关分支（`navigator.webdriver` 的**来源之一**）
- `AutomationControlled` Blink runtime feature 默认值（**另一个来源**；实测只要打开
  `--remote-debugging-pipe` 就会打开它，现行做法是用
  `--disable-blink-features=AutomationControlled` 抵消，见 §4.3）
- UA 产品串 / 品牌串常量
- 自动化提示条（infobar）资源
- 版本/品牌元数据

补丁器按**版本化签名库**工作：`patches/<engine>/<version>.json`，含字节模式、偏移、替换值、校验哈希。升级引擎只需重跑 `ghost_patcher --verify`。

### 3.5 注入与传播（跨平台）

| 平台 | 注入机制 | 子进程传播 |
|---|---|---|
| Windows | 启动器 `CreateProcessW(..., CREATE_SUSPENDED)` → `VirtualAllocEx`+`WriteProcessMemory`+`CreateRemoteThread(LoadLibraryW)` → `ResumeThread` | Shim 内 hook `CreateProcessW`/`CreateProcessAsUserW`：给子进程加 `CREATE_SUSPENDED` → 注入 → 恢复（原本就 suspended 的则交给 Chromium 恢复） |
| Linux | `LD_PRELOAD=<shim.so>`（launcher 设置） | **自动继承**，无需 hook |
| macOS | `DYLD_INSERT_LIBRARIES=<shim.dylib>` + 对 Chromium.app 做 ad-hoc 重签名并去除 library validation | **自动继承** |

配置通过 **环境变量 `GHOST_PROFILE_JSON`** 下发，天然被所有子进程继承，无需额外 IPC。

> **Windows 注入时序要点**：在子进程 **resume 之前**注入，可保证 DLL 先于 Chromium 沙箱初始化加载；Chromium 的 `chrome_elf` DLL blocklist 按**已知哈希**拦截，第三方未知 DLL 不在名单内，不触发。

### 3.6 已知缺口（必须诚实记录）

| 面 | 状态 | 说明 |
|---|---|---|
| Canvas `getImageData`/`toDataURL` 哈希 | ⚠️ **缺口** | 纯 Blink 内部 Skia 路径，无导出符号。短期靠字节特征码补丁；正解是源码级 patch（Track B） |
| Audio `OfflineAudioContext` 指纹 | ⚠️ **缺口** | 同上，Blink 内 DSP |
| WebGL 参数哈希 | ✅ 已覆盖 | 走 ANGLE 导出层 |
| 字体**度量**（非列表） | ⚠️ 部分 | 列表可 hook；文本测量宽度由 Blink/Skia 计算 |
| `hardwareConcurrency` 等 | ✅ 已覆盖 | OS API 层 |

**缺口收敛路线（Track B）**：建立 Chromium 源码 patch set（ungoogled-chromium 风格），在 `third_party/blink/renderer/core/` 层做源码级修改。本机 4 核/16GB 无法编译，需 CI/云构建。**Track A 与 Track B 的 patch 语义一一对应**，Track A 是 Track B 的验证器与过渡形态。

---

## 4. L3 控制面 — 管道而非端口

### 4.1 为什么 CDP 是最大检测面

页面侧可探测的 CDP 痕迹：
1. **`--remote-debugging-port` 端口可被页面扫描**（Cloudflare 会探测 localhost 常见调试端口）
2. `Runtime.enable` 对 V8 的副作用（序列化路径、console 上下文）
3. Puppeteer/Playwright 注入的 `__playwright__binding__` / utility script（本架构天然不存在）
4. `--enable-automation` → `navigator.webdriver`（本架构不设置该 flag）。**但这不是唯一来源**：实测只要打开 `--remote-debugging-pipe`，Blink 的 `AutomationControlled` 特性就被打开，页面读到的 `navigator.webdriver` 同样是 `true`（见 §4.3）

### 4.2 零 CDP 的四个替代能力

| 需要的能力 | CDP 做法 | 本架构做法 |
|---|---|---|
| **输入**（点击/打字/滚动） | `Input.dispatchMouseEvent` | **OS 输入合成**：Win `SendInput` / Linux XTEST+`uinput` / macOS `CGEventPost`。产生**真实 OS 输入事件**，`isTrusted=true`、真实硬件时间戳、真实 `pointerId`/`movementX` 序列 |
| **读取页面结构** | `Runtime.evaluate` / `DOM.getDocument` | **无障碍树**：Win UIA / Linux AT-SPI2 (D-Bus) / macOS AX API。拿到 role/name/value/bounds/actions，**页面 JS 无感知** |
| **截屏** | `Page.captureScreenshot` | OS 窗口捕获：Win `PrintWindow`/`BitBlt` / Linux `XGetImage` / macOS `CGWindowListCreateImage` |
| **Cookie/Storage** | `Network.getCookies` | 直接读 profile 目录 SQLite：`Cookies`、`Local Storage/leveldb`、`Session Storage`。这也是 `cf_clearance` 收割与复用的通道 |

**导航**：首次用命令行 URL 参数；后续用真实键盘输入到地址栏（`Ctrl+L` → 打字 → `Enter`），全程 OS 输入。

### 4.3 权衡

零 CDP 的代价是**实现量大**且无障碍树对复杂 SPA 有信息损失。实测下来，这个代价里有两项是**替代通道根本补不上的**：

1. **写隐藏表单字段。** 无障碍树能读，不能把任意值写进去。`g-recaptcha-response` 正是隐藏字段，第三方打码服务返回的 token 必须落到它里面，否则「解出来了」只是自说自话。
2. **读 `<audio>` 元素自己的 URL。** 挑战音频的地址只有页面自己知道，无障碍树只给 role/name/value。

因此 `control.mode = "cdp-pipe"` **自 0.11.0 起是默认档位**：用 `--remote-debugging-pipe`（匿名管道句柄，非 TCP 端口，页面无法做端口扫描，profile 里也不写 `DevToolsActivePort`）补上这两项，其余能力仍走原生通道。**`--no-cdp` 回到纯 `native` 档位**——此时上述两项确实不可用，这是取舍，不是缺陷。

打开该管道会连带打开 Blink 的 `AutomationControlled` 特性，把 `navigator.webdriver` 变成 `true`——恰好是这个项目最该避免的那件事。启动器在追加管道参数的同时**合并** `--disable-blink-features=AutomationControlled`（Chrome 只认一次该开关，所以要合并进调用方已有的那一个）。实测三种配置：管道开 → `true`；`--no-cdp` → `false`；管道开且手动加该开关 → `false`；修复后三者全部 `false`。

### 4.4 渲染进程被标记为 hidden 时，无障碍树与 CDP 输入会一起失效

在**无法投递输入**的会话里（断开的 RDP、无头会话，`GetForegroundWindow()` 返回 0），Chromium 把渲染进程标记为 hidden。这一个条件会造成两个看起来毫不相干的故障：

- **无障碍树不构建**：`tree` 只返回浏览器 chrome（实测 44 行、`primed no`），`find` 一个都匹配不到。
- **CDP `Input.*` 被静默丢弃**：原始回复是 `{}`（没有错误），但一个事件都没到渲染进程。

孤立验证（同一页面，**中间不发生任何导航**，只改一个变量）：发一次 `Emulation.setFocusEmulationEnabled {"enabled": true}`，`document.visibilityState` 由 `hidden` 变 `visible`，`ghost __uia` 由 44 行涨到 **71 行、`primed yes`**，树里出现 `document id=RootWebArea "Example Domain"` 与页面自己的 `link "Learn more"`；此时用 CDP 点击该链接真的导航了。

修法是 `repair_hidden_renderer()`：在会话启动时、**第一次 `tree` 之前**调用（树是渲染进程**得知自己可见**时构建的，不是客户端索取时才建），并且仅在 `GetForegroundWindow() == nullptr` **且**页面自报 `hidden` 时才发那一条命令。理由是：在真实用户正看着浏览器的机器上强制 focus emulation，会让页面永远收不到 blur，那是另一种异常。

---

## 5. L2 指纹档案 — 相干性优先

反检测的失败大多不是"某个值不对"，而是**值之间不自洽**。`ghostprof` 的职责是保证档案在构造上相干：

```
GPU 型号 ─┬─ WebGL UNMASKED_VENDOR/RENDERER
          ├─ WebGL 参数上限（显存推导）
          ├─ 驱动版本串
          └─ 平台（Win 上是 D3D11/ANGLE 后端，Mac 上是 Metal）

UA ───────┬─ userAgentData.brands / platform / platformVersion
          ├─ Sec-CH-UA-* 请求头
          ├─ navigator.platform / oscpu
          └─ 可用字体集合（Win 字体集 ≠ Mac 字体集）

屏幕 ─────┬─ screen.* / availWidth
          ├─ devicePixelRatio（与 GPU/OS 缩放档位匹配）
          └─ 窗口尺寸 ≤ availWidth

时区 ─────┬─ Intl 时区
          ├─ UTC 偏移与 DST 规则
          ├─ 语言/地区（zh-CN ↔ Asia/Shanghai）
          └─ 出口代理 IP 地理位置（geoip 校验）
```

**确定性噪声**：Canvas/Audio 噪声必须**同一 profile 稳定、不同 profile 不同**（真实设备就是稳定的）。种子派生：
```
noise_seed = HMAC-SHA256(profile_seed, origin + ":" + surface)[0..8]
```
——这样同一 profile 在不同站点得到不同噪声（防止跨站关联），但在同一站点内稳定（不会被"刷新后哈希变化"识破）。

---

## 6. L4 验证码

三级降级：

```
① 静默通过   —— 好指纹 + 真实浏览器 + 真实输入 → CF Managed Challenge / Turnstile 非交互自动放行
   ↓ 失败
② 本地求解   —— hCaptcha 音频挑战 → 下载音频 → 本地 Whisper 转写 → 填入
                 CF Turnstile 交互挑战 → OS 输入真实鼠标轨迹点击复选框
   ↓ 失败
③ 第三方兜底 —— 打码服务 API（2Captcha / CapSolver）拿 token → 注入页面
```

**关键约束：token 必须在真实浏览器会话内产生并消费。** 第三方返回的 token 若来自别的 IP/指纹，风控评分会归零。因此第③级只用于"取得答案"，提交动作仍由真实浏览器完成。

`cf_clearance` 按 **profile × 域名** 持久化到 profile 目录，新会话直接复用。

**落地情况（见 §14）**：识别层与第①级已完成。`ghost captcha` 能读出挑战的 provider、状态与 sitekey，并用一次可信点击清掉 Cloudflare Turnstile。第②级的入口已经可达——reCAPTCHA 的 `recaptcha-audio-button` 能被点开、状态转为 `audio`、`bft` token 已能读出——但音频采集、转写与填入都还没做。第③级未做。

---

## 7. L5 行为引擎

所有行为最终收敛为 OS 输入事件序列：

- **鼠标**：Fitts 定律 + 三次贝塞尔轨迹 + 过冲/修正（overshoot & correction）+ 微抖动；速度剖面符合人类最小 jerk 模型
- **打字**：digraph 延迟表（常见字母对更快）+ 错字-退格-重打 + 词间停顿
- **滚动**：非匀速 + 惯性衰减 + 回滚（人类回看）
- **节奏**：阅读停顿与内容长度相关；表单填写有思考间隔；不出现机器级的等间隔
- **会话**：标签页切换、失焦/聚焦、偶发空闲

---

## 8. 跨平台矩阵

| 组件 | Windows | Linux | macOS |
|---|---|---|---|
| 引擎 | ungoogled-chromium / Chromium 预编译 | 同左（deb/AppImage/tar） | 同左（.app/.dmg） |
| 注入 | CreateProcess(SUSPENDED)+注入 | `LD_PRELOAD` | `DYLD_INSERT_LIBRARIES` + 重签名 |
| Shim 构建 | MSVC x64 | clang/gcc x64 | clang universal |
| 输入 | `SendInput` | XTEST / `uinput` | `CGEventPost` |
| 无障碍 | UIA / IAccessible2 | AT-SPI2 (D-Bus) | AX API |
| 截屏 | `PrintWindow` / `BitBlt` | `XGetImage` | `CGWindowListCreateImage` |
| 字体 Hook | DirectWrite | fontconfig | CoreText |
| GPU Hook | DXGI 适配器 vtable（`dxgi.dll`） | 待定（Vulkan/GLX 适配器层） | 待定（IOKit/Metal） |

**daemon 与 profile 引擎用 Rust 实现**（本机 rustc 齐备），保证三平台单套代码；仅 Shim 与注入器分平台实现。

---

## 9. 合规边界

本项目的合法用途：隐私保护、广告验证、价格监测、公开数据采集、Web 应用 QA、自动化测试、无障碍自动化。
使用者须自行确保其使用场景符合目标站点的服务条款与适用法律（含 CFAA、GDPR、以及各平台反自动化条款）。项目不提供、也不应被用于绕过身份验证、批量虚假注册、欺诈或未授权访问。

---

## 10. Track A 垂直切片实测结论

本节全部结论来自 `E:\projects\unknowbrowser` 上的实机验证，不是推断。入口命令：
`python harness\run_detect.py` → **32 checks, 0 failed**。

### 10.1 已打通的能力

| 项 | 实现位置 | 实测 |
|---|---|---|
| 档案 → 引擎 API 欺骗 | `ghost_shim` 24 个 hook | 全部安装成功 `hooks installed=24 failed=0` |
| 启动器注入（suspend→inject→ready→resume） | `native/ghost_launch/src/main.cpp` | `shim ready (hooks=24)` |
| 子进程传播 | `hooks_proc.cpp` 的 `propagate` | browser + 全部 renderer 均 `shim=LOADED`（关沙箱时） |
| `hardwareConcurrency` | `hooks_sysinfo.cpp` | 4（profile 值） |
| `deviceMemory` | `hooks_sysinfo.cpp` | 8（profile 值） |
| `screen.*` / `devicePixelRatio` | `hooks_display.cpp` | 2560×1440 / 1.25，且 `width×DPR==3200` 物理像素自洽 |
| `Intl` 时区与偏移 | `hooks_time.cpp` | Europe/London，1 月 0 / 7 月 −60，`GMT`/`BST` |
| `navigator.language` | `--lang` | en-GB |
| `navigator.webdriver` / 自动化全局泄漏 | Chromium 原生 | false / 无 |
| CDP 端口 | 未开启 | 9222/9223/9229/9515 全 closed |
| 被 hook 的 getter 仍显示 `[native code]` | 原生 hook 的必然结果 | 通过 |

### 10.2 渲染进程注入的结构性障碍（最重要的负面结论）

**`LoadLibraryW` 在 Chromium 沙箱渲染进程内必然失败，返回 `STATUS_ACCESS_DENIED` (`0xC0000022`)。**

实测令牌（`tools/token_sids.ps1`）：

| 进程 | 完整性 RID | 受限 SID 列表 |
|---|---|---|
| browser | 8192 (Medium) | 空 |
| gpu-process | 4096 (Low) | 5 个（含 `S-1-5-32-545`、`S-1-1-0`、`S-1-5-12`） |
| **renderer** | **0 (Untrusted)** | **1 个：`S-1-0-0`（NULL SID / Nobody）** |

`S-1-0-0` 是 NULL SID，**任何常规 ACE 都无法授予它**，因此无论怎样配置 DLL 的 ACL，
受限令牌的第二道访问检查都不可能通过。

逐一排除的替代解释（全部证伪）：

1. **CIG / 签名策略** —— `target_signature_policy` 对全部子进程返回 `any`。
2. **AppContainer ACL** —— `C:\` 本身也没有 AppContainer ACE，渲染进程却能加载
   `C:\Program Files` 下的 `chrome.dll`；traverse 由 `SeChangeNotifyPrivilege` 绕过，
   只有最终对象 ACL 参与检查。且我们的 DLL 本就带 `ALL APPLICATION PACKAGES:(RX)`。
3. **强制完整性标签** —— 用 `SetNamedSecurityInfoW(..., LABEL_SECURITY_INFORMATION, ...)`
   把 DLL 分别设为 Untrusted 与 Low（需先 `icacls /grant "*<sid>:(F)"` 取得
   `WRITE_OWNER`，否则 `rc=5`），**两次都不改变结果**。
4. **NULL SID 授权** —— `icacls $dll /grant "*S-1-0-0:(RX)"` 成功，仍是 `0xC0000022`。
   （对照：`chrome.dll` 自身**没有** `S-1-0-0` ACE。）
5. **路径 / 目录 traverse** —— 把两个产物复制到 `C:\Users\Public\ghostbin` 再启动，
   仍全部 `0xC0000022`。
6. **进程缓解策略** —— `tools/mitigations.ps1` 遍历 16 项策略，Chrome 全部进程
   `OptionsMask=0x33333303`，**无任何策略被启用**（ACG、ProcessImageLoadPolicy 均排除）。

⇒ 这是**架构级**限制，不是配置问题。**Track A 无法覆盖沙箱渲染进程；
`--no-sandbox` 是唯一可用降级**（已验证关沙箱后渲染进程侧 hook 生效：
`deviceMemory` 由 16 变 8）。

**⇒ 这条结论正是 Track B（源码级 patch）的存在理由：Track B 不需要注入渲染进程。**

### 10.3 `navigator.hardwareConcurrency` 的真实来源（曾长期误判）

只 hook `GetActiveProcessorCount` / `GetSystemInfo` **无效**。Chromium 实际路径：

- `base/system/sys_info_win.cc:130` → `SysInfo::NumberOfProcessors()` → `win::OSInfo::GetInstance()->processors()`
- `base/win/windows_version.cc:68-75` → `GetSystemInfoStorage()` 内部调 `::GetNativeSystemInfo(&info)`
- `base/win/windows_version.cc:174` → `processors_ = static_cast<int>(system_info.dwNumberOfProcessors)`

⇒ 必须 hook **`GetNativeSystemInfo`**。补上后 `hardwareConcurrency` 立即由 8 变为 profile 的 4。

### 10.4 locale 必须靠 `--lang`，hook 不够

shim 已 hook `GetUserDefaultLocaleName` / `GetUserDefaultUILanguage` / `GetSystemDefaultUILanguage`，
但**引擎在 hook 生效之前就已解析并缓存 application locale**，因此
`navigator.language` 与 `Intl` 默认 locale 仍是宿主值（实测为 `zh-CN` / en-US 日期格式，
二者互相矛盾——一个很容易被检测的相干性破绽）。
修法：`ghost_launch --lang <tag>` 追加 `--lang=` 到引擎命令行；已由 `run_detect.py`
从 profile 自动下发。加后 `navigator.language` 与 `Intl` 同时变为 en-GB 且自洽。

### 10.5 Windows Defender 会查杀注入器

`ghost_launch.exe` 曾被隔离，威胁名 **`Behavior:Win32/DefenseEvasion.A!ml`**，
触发源是 `icacls /setintegritylevel` + 给 DLL 授予 NULL SID 这类「防御规避」行为
（而这些操作已被证明对结果毫无帮助）。还原 ACL 后重建即不再被查杀。
`Add-MpPreference -ExclusionPath` 需要管理员权限，当前进程无法设置。

**⇒ 产品化必须走代码签名 + 用户显式排除目录。**

### 10.6 仍未覆盖（Track A 的边界）

Canvas `getImageData`/`toDataURL`、`OfflineAudioContext`、字体**度量**仍是宿主真实值——
它们完全在 Blink/Skia 内部，且官方 Chrome 不附带 PDB（`tools/pe_exports.py` 实测
`chrome.dll` 仅导出 5 个符号，对 v8/blink/canvas/webgl 等关键词命中 0）。这些缺口只能由
Track B 的源码级 patch set（ungoogled-chromium 风格，需云 CI）填补。

> **更新（2026-10-06）：WebGL `UNMASKED_*` 已从本列表移除**，改由 §11 的 DXGI 通道覆盖。

---

## 11. WebGL 通道重做 — ANGLE 静态链接与 DXGI 适配器身份

### 11.1 原方案为何失效：`libGLESv2.dll` 不存在了

§3.3 的原设计假设 `libGLESv2.dll` / `libEGL.dll` 是独立 DLL。**实测推翻**：

- 枚举 `C:\Program Files\Google\Chrome\Application\154.0.8037.98\*.dll`：
  `chrome_elf.dll` 2,793,112、`chrome_wer.dll` 125,592、**`chrome.dll` 302,382,744**、
  `chromecompaneros.dll` 479,384、`d3dcompiler_47.dll` 4,753,864、`dxcompiler.dll` 25,862,296、
  `dxil.dll` 1,503,728、`eventlog_provider.dll` 16,536、
  `libLiteRtWebGpuAccelerator.dll` 9,659,032、`optimization_guide_internal.dll` 24,592,536、
  `vk_swiftshader.dll` 5,426,328、`vulkan-1.dll` 996,504。
- **`libGLESv2.dll` 在整个安装目录下 NOT FOUND。**
- 在 `chrome.dll` 内搜 ASCII：`libGLESv2`=**0**、`libEGL`=1、`ANGLE (`=6、`EGL_`=546、`GL_ANGLE`=202。
- 强制创建 WebGL 上下文后，GPU 进程加载了 `dxgi.dll` 与 `d3d11.dll`，**没有 `libGLESv2.dll`**，
  shim 日志中 `install_gl_hooks()` 从未产生任何一行。

**⇒ ANGLE 被静态链接进 `chrome.dll`（302 MB）。没有导出表可 hook，也没有 PDB。**

### 11.2 现行通道：patch DXGI 适配器 vtable

ANGLE 自己拼出 `GL_RENDERER`：

```
ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 (0x00002504) Direct3D11 vs_5_0 ps_5_0, D3D11)
                └── IDXGIAdapter::GetDesc().Description     └── DeviceId
```

`IDXGIAdapter` 是 COM 对象，vtable 在 `dxgi.dll` 中。patch 它不需要任何 `chrome.dll` 符号。

**实现（`native/ghost_shim/src/hooks_gpu.cpp`）**：

1. `hook_LdrLoadDll` 检测到 `dxgi.dll` 加载 → `install_dxgi_hooks()`。
   （shim attach 时 `dxgi.dll` 尚未加载，所以必须在模块加载时才装；装完立即
   `hook_engine_enable_all()`。）
2. hook `CreateDXGIFactory` / `CreateDXGIFactory1`（`HRESULT(WINAPI*)(const GUID&, void**)`）
   与 `CreateDXGIFactory2`（`HRESULT(WINAPI*)(UINT, const GUID&, void**)`），各配独立 trampoline。
3. `patch_factory(void** factory)`：改写 `IDXGIFactory::EnumAdapters1`（vtable slot 12）指向
   `hook_EnumAdapters1`。
4. `hook_EnumAdapters1` 拿到 `IDXGIAdapter1*` 后调用 `patch_adapter_vtable()`：
   改写 `GetDesc` / `GetDesc1` 两个 slot 指向我们的 detour。
5. `hook_GetDesc` / `hook_GetDesc1` 调真实实现后，用 `write_adapter_fields()` 覆写
   `Description` / `VendorId` / `DeviceId` / `DedicatedVideoMemory`。

**必须同时改 `Description` 与 `DeviceId`**：真实串里的 `(0x00001C82)` 就是 `DeviceId`，
只改描述会让字符串自相矛盾——这是检测器最容易抓的破绽。

**Profile 字段**（`harness/profiles/slice-test-001.json`）：
`gpu_adapter_description` / `gpu_adapter_vendor_id` / `gpu_adapter_device_id` / `gpu_adapter_video_memory`。

### 11.3 实测结果

shim 日志：
```
hook ok: dxgi.dll!CreateDXGIFactory  (target=00007FFC828DDF20 tramp=00007FFC85B309C0)
hook ok: dxgi.dll!CreateDXGIFactory1 (target=00007FFC828DE0C0 tramp=00007FFC85B30980)
hook ok: dxgi.dll!CreateDXGIFactory2 (target=00007FFC828DDFC0 tramp=00007FFC85B30940)
dxgi: factory hooks installed (f=1 f1=1 f2=1)
dxgi: adapter vtable patched (GetDesc=00007FFC3E768930 GetDesc1=00007FFC3E7688E0)
```

页面观测：
```
UNMASKED_RENDERER_WEBGL = ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 (0x00002504) Direct3D11 vs_5_0 ps_5_0, D3D11)
```
真实 GTX 1050 Ti 消失。**注意这个串是 ANGLE 自己拼的**——vendor 前缀、feature level、
`(0x…)` 后缀全部由真实 ANGLE 代码生成，我们只改了它的输入，所以格式上不可能露馅。

`python harness/run_detect.py --sandboxed --chrome-arg=--disable-gpu-sandbox` → **34 checks, 0 failed**。

### 11.4 为什么必须 `--disable-gpu-sandbox`（而不是 `--no-sandbox`）

实测发现 GPU 进程与渲染进程的创建路径不同：

| 子进程 | 创建 API | 令牌 | 可注入 |
|---|---|---|---|
| renderer / utility | `CreateProcessAsUserW` | Untrusted + 受限 SID `[S-1-0-0]` | ❌ `LoadLibraryW` → `0xC0000022` |
| **gpu-process** | **`CreateProcessW`** | Low，无受限令牌 | ✅ `injected+ready hooks=24` |

所以 `--disable-gpu-sandbox` 让 GPU 进程可注入，**而渲染进程沙箱保持开启**。
这比 `--no-sandbox` 好得多：WebGL 面被伪装，渲染进程仍受沙箱保护。

**被证伪的假设**：曾认为 GPU 进程是 Low 完整性、把 shim DLL 降到 Low 就能加载它。
实测 `icacls ghost_shim.dll /setintegritylevel L` 后**仍然全部 `0xC0000022`**，
且日志首次明确打印受限 SID `restricted_sids=1 [S-1-0-0]`。**完整性标签从来不是阻碍，
受限令牌才是。**（此实验还导致 Defender 查杀 `ghost_launch.exe`，见 §10.5。）

### 11.5 Track B 仍需做的事

DXGI 通道覆盖了 `UNMASKED_VENDOR_WEBGL` / `UNMASKED_RENDERER_WEBGL`，但**没有**覆盖：

- `glGetParameter` 返回的 `GL_MAX_*` 上限集合（应与伪装的 GPU 档位相干）
- WebGL 渲染结果的像素级差异（canvas 哈希）
- 驱动的扩展字符串集合

这些仍在 `chrome.dll` 内部，只能靠 Track B 源码 patch。**DXGI 通道的价值在于：它把
WebGL 从「必须重编引擎」降级为「Track A 即可」，大幅缩小了 Track B 的必要范围。**

## 12. L3 控制面已落地 — 命名管道 + UIA + SendInput

§4 设计的东西已经实现，作为 `ghost.exe` 的 `serve` 模式（不是独立的 Rust daemon）。
验收：`python tools/serve_check.py` → **19 checks, 0 failed**（另有 7 项因本会话没有前台
窗口而记为 not measurable，而不是 failed）。

### 12.1 为什么不写独立 daemon

产品的核心承诺是「一个自包含文件」。引入 Rust 工具链会同时破坏这个承诺、让 CI 多一套
工具链，而控制面本身几乎全是 Win32 C API（UIA 的 COM 接口、`SendInput`、`PrintWindow`）。
唯一需要 SQLite 的部分（cookie）放到了 Python 客户端，用标准库 `sqlite3`。

### 12.2 传输：命名管道，不是端口

`\\.\pipe\ghost-<profile-id>`，换行分隔 JSON，一请求一响应。**页面能扫端口，扫不到管道。**
服务端一次只服务一个连接：`DisconnectNamedPipe` 与下一次 `ConnectNamedPipe` 之间管道名会
短暂消失，正在处理中的请求返回 `ERROR_PIPE_BUSY`。两者都是正常状态，客户端必须重试
（`ghost_client.CONNECT_RETRY_SECONDS = 5.0`）——**连接重试窗口必须远短于请求超时**，
否则对已死服务的一次请求要挂满整个超时。

### 12.3 三个能力与实测

| 能力 | 实现 | 实测 |
|---|---|---|
| 输入 | `SendInput`，鼠标路径插值（smoothstep + 垂直弓形，非瞬移）、按下 35–90 ms、`KEYEVENTF_UNICODE` 逐 UTF-16 码元打字 | `trusted=117`、`untrusted=0`，页面确认 `clicked via trusted event` |
| 读取页面 | UIA `ControlViewWalker` 遍历，role/name/value/bounds | 按钮可按 role+name 定位，`bounds={'x': 11, 'y': 386, 'width': 71, 'height': 27}` |
| 截屏 | `PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)`，失败退回 `BitBlt` | 7,963,590 字节 BMP |
| 导航 | `Ctrl+L` → 打字 → `Enter`，轮询标题变化 | 标题变为 `ghost control plane check - Ghost Browser` |

### 12.4 两个必须记住的 Win32 语义

- **前台锁**：只 `AttachThreadInput` 不足以让窗口到前台。必须**先合成一次 ALT 按键**
  （`SendInput` 发 `VK_MENU` down/up）使自己成为「最后输入线程」，再
  `ShowWindow` + `BringWindowToTop` + `SetWindowPos(HWND_TOP, SWP_SHOWWINDOW)` +
  `SetForegroundWindow`，最后轮询 `GetForegroundWindow()` 确认。
- **UIA 的矩形是四元组 `[left, top, width, height]`**，不是 `[left, top, right, bottom]`。
  按后者解析会得到大量负 width。

### 12.5 品牌化与 infobar

`--disable-gpu-sandbox` 会让 Chromium 弹一条 "unsupported command-line flag" 的 infobar：
它既是肉眼可见的自动化痕迹，又偷走 56 px 视口高度。实测 **`--test-type` 让 infobar 节点数
从 3 变 0**，所以 `browse`/`serve` 在非 `--sandbox` 档默认加上它。

窗口品牌化由已经注入的 shim 完成（`native/ghost_shim/src/hooks_brand.cpp`）：hook
`SetWindowTextW`/`SetWindowTextA` 替换引擎产品名、`SetCurrentProcessExplicitAppUserModelID`
归组任务栏、`WM_SETICON` + `SetClassLongPtrW(GCLP_HICON/GCLP_HICONSM)` 贴图标。
**局限**：`chrome://version`、磁盘上的 `chrome.exe`、窗口类名 `Chrome_WidgetWin_1`
仍然是 Chrome——它们编译在二进制里。

### 12.6 cookie 的三重障碍（实测）

1. **运行时不可读。** Chrome 对 `Default/Network/Cookies` 不开放任何共享：
   `CreateFileW` 在 share = 0/1/3/7 下全部 `ERROR_SHARING_VIOLATION`(32)，而同目录其他文件
   正常打开 ⇒ 不是 ACL、不是沙箱。只能在浏览器退出后读。
2. **会话 cookie 不入库。** 没有 `max-age`/`expires` 的 cookie 只存在于内存。
3. **`value` 列永远是空串。** 真实值在 `encrypted_value`：`b"v10"` + AES-256-GCM
   (`nonce(12)` + ciphertext + `tag(16)`)。密钥在 `<data_dir>/Local State` 的
   `os_crypt.encrypted_key`（base64 的 `b"DPAPI"` + DPAPI 包裹的 32 字节密钥；无
   `app_bound_encrypted_key`，即不是 App-Bound Encryption）。**明文 = 32 字节域绑定 + 值本身**，
   所以 `ghost_check=ok` 解出来是 32 个未知字节后跟 `ok`。

`ghost_client` 用 `ctypes` 调 `crypt32`/`bcrypt` 完成解密，**不引入第三方依赖**。用
`bcrypt` 走 GCM 有三个坑，每个都表现为 `STATUS_ACCESS_VIOLATION` 而不是干净的错误返回：
不声明 `argtypes`；`BCryptDecrypt` 有 **10** 个参数（漏掉末尾 `dwFlags`）；GCM 下
**`pPaddingInfo` 也必须指向同一个 `BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO`**，
传 `NULL` 会直接崩。

### 12.7 仍未做

行为引擎（§7）、Linux/macOS（§8）还没有实现。验证码不是「未做」——见 §14，已做到三级。

### 12.8 DevTools 通道：默认开启，管道而非端口

零 CDP 的承诺实测下来有两处**替代通道补不上**：隐藏表单字段写不进去，`<audio>` 元素
自己的 URL 读不到。后者自 0.12.0 起也用上了（`captcha action=audio-url`，见 §14.7），
走的是同一个管道；前者正好是第三方打码服务返回 token 的落点，所以自 0.11.0 起
`ghost` 默认打开 `--remote-debugging-pipe`：**句柄由启动器创建后传给子进程**
（`--remote-debugging-io-pipes=<read>,<write>`，见 `native/ghost_cli/src/launch.cpp:60-97`
的注释——`chrome_main_delegate.cc:1221-1233` 在 `--remote-debugging-pipe` 存在时会跳过
描述符检查），所以：

- **没有 TCP 端口**可被页面扫描；
- **profile 里不写 `DevToolsActivePort`**（实测：整个 profile 目录递归查找无 `DevTools*` 文件）；
- 只用 `Runtime.evaluate` 与 `DOM.getDocument`，**两者都不需要先 `enable`**。

代价与修法：打开该管道会连带打开 Blink 的 `AutomationControlled` 特性，把
`navigator.webdriver` 变成 `true`。启动器把 `AutomationControlled` **合并进**调用方已有的
`--disable-blink-features=`（Chrome 只认一次该开关）。三种配置实测：管道开 → `true`、
`--no-cdp` → `false`、管道开 + 手动加该开关 → `false`；修复后全部 `false`。

`--no-cdp` 回到纯原生档位，上述两项能力随之不可用——这是取舍，不是缺陷。

## 13. 字体通道 — DirectWrite 是 Track A 唯一还剩的可达指纹面

### 13.1 为什么是字体，而不是 canvas / audio

canvas 的像素由渲染进程内的 Skia 光栅化，audio 的采样由 Blink 的 DSP 产生，**两者都不
跨任何 OS API**。Track A 的全部手段是 hook 系统调用与 COM vtable，所以这两个面在结构上
就够不到——不是工作量问题，是没有可拦截的调用。它们只能等 Track B（改 Chromium 源码）。

字体枚举不一样。Skia 在 Windows 上的字体后端走 **DirectWrite**，而 DirectWrite 是一个
真实的、可被 hook 的 OS 组件。而且本机字体列表本身就是**最响的指纹之一**：它直接读出
机器上装了什么软件。本机实测 **266 个字体族**——Windows 自带 + 整套 Office + 中文 IME
包 + 开发者的工具链（`Cascadia Code`、`Noto Sans SC`、`Ubuntu Mono`）。任意一个都能给
档案定期，组合起来能直接指认到人。

### 13.2 做法：照搬 DXGI 适配器的 vtable 先例

不改源码、不需要符号，和 §11 的 DXGI 通道同一套路：

1. `install_hook_export("dwrite.dll", "DWriteCreateFactory", ...)` 装导出钩子。
2. 在 detour 里调真函数拿到 `IDWriteFactory*`，把 **vtable 槽 3**
   （`GetSystemFontCollection`）改成我们的实现，原值存为 trampoline。
   **`factory_patched` 守卫是必需的**：工厂会被创建多次，第二次若不守卫就会把已经改写的
   槽当成真函数，无限递归。
3. 拿到 `IDWriteFontCollection*` 后改它的 vtable：槽 3 `GetFontFamilyCount`、
   槽 4 `GetFontFamily`、槽 5 `FindFamilyName`。（这些槽位自 Windows 7 起冻结。）
4. 先遍历真集合，对每个 family 取 `GetFamilyNames()`（优先 `FindLocaleName(L"en-us")`，
   否则 index 0），命中档案白名单的记下**真实下标**，得到 `visible_to_real` 映射。
5. `GetFontFamilyCount` 返回映射长度；`GetFontFamily(i)` 转发到
   `real_family(visible_to_real[i])`。映射是必需的——调用方会把 `FindFamilyName` 返回的
   下标原样交回给 `GetFontFamily`，两者必须自洽。

`VirtualProtect(vtable, sizeof(void*) * 8, PAGE_READWRITE, &old)` 改完再还原。

### 13.3 不能按请求字符串匹配

一个字体族有多个本地化名字。`MS Gothic` 同时也是 `ＭＳ ゴシック`，`SimSun` 同时也是
`宋体`。如果 `FindFamilyName` 拿请求的字符串去和白名单比对，合法别名会被误拒——而按
canonical 名比对又会漏掉别名进来的路径，两者矛盾。

正确做法是**先让真集合解析**：调 `real_find_family(self, name, &real_index, exists)`，
拿到真实下标后再判断它是否在 `visible_to_real` 里。命中就返回我们这边的下标，不命中就
返回 `*exists = FALSE; *index = UINT32_MAX; return S_OK;`——即「这台机器上没装这个字体」，
正是 `document.fonts.check()` 和 canvas `measureText` 期望看到的答案，两者因此不会互相
矛盾。**整个过程不需要任何字符串匹配。**

### 13.4 一个都不匹配时拒绝 patch

如果档案列出的族在本机一个都不存在，`visible_to_real` 为空，我们就**放弃 patch** 而不是
返回空集合。理由：一个相信自己没有任何字体的浏览器会把文字渲染得极其明显地崩坏，那是比
「不过滤」更糟的破绽。宁可退回真实列表，也不要制造一个一眼假的渲染结果。

同理，档案里没有 `fonts` 键就完全不装这个钩子，保持 pass-through。

### 13.5 实测

| 项 | 值 |
|---|---|
| 宿主字体族总数 | 266 |
| 生成档案里的白名单 | 89（`ghost profile new` 自动填充的标准 Windows 集合） |
| 钩子关闭时页面上可见 | `Cascadia Code`、`Noto Sans SC`、`Ubuntu Mono`、`Agency FB` 全部泄漏 |
| 钩子开启时页面上可见 | 只剩 `Arial`、`Segoe UI`、`Times New Roman`、`Bahnschrift` |

**A/B 对照**：`GHOST_HOOK_MASK=BF`（十六进制，除字体外全部启用）→
`[FAIL] the fonts that betray extra software are gone
actual=['Cascadia Code', 'Noto Sans SC', 'Ubuntu Mono', 'Agency FB']`，`19 checks, 1 failed`；
mask 恢复默认后 `19 checks, 0 failed`。**证明移除它们的就是字体钩子本身。**

> `GHOST_HOOK_MASK` 是十六进制。`wcstoul(..., 16)` 会把 `"0x3F"` 解析成 `0` 从而静默关掉
> **全部**钩子，所以现在解析前先跳过可选的 `0x`/`0X` 前缀。

### 13.6 局限

- **只能隐藏，不能凭空发明。** 可见集合 = 档案白名单 ∩ 本机真实存在的字体。档案里列一个
  本机没有的字体不会让它出现。
- **字体度量仍是宿主的。** 枚举被过滤了，但 Skia 拿到字体后算出的宽度、字距、行高还是
  真实值。本机装了 `Arial` 的话，`Arial` 的度量就是真的——这通常没问题，但如果档案声称的
  字体被换成了 fallback，度量会露馅。
- **仍是进程级的。** 钩子装在哪个进程就只影响哪个进程；渲染进程能否被注入仍然取决于
  §10.2 的沙箱问题。

---

## 14. L4 验证码通道 — 零 CDP 下怎么拿到题目

### 14.1 难点不是解题，是拿题

三级降级里，②③都要求先知道**这是什么挑战、sitekey 是什么、挑战 frame 的 token 是什么**。
在 CDP 时代这是 `Runtime.evaluate` 一行的事；零 CDP 之后它成了整件事的门槛。

答案在无障碍树里。两个性质让它在没有 CDP 的情况下也能工作：

1. **UIA 能看进跨域 iframe。** 挑战控件作为嵌套的 `document`/`group` 节点出现：hCaptcha 是
   `document '包含 hCaptcha 安全挑战复选框的小部件'`，reCAPTCHA 是 `group 'reCAPTCHA'`，
   Turnstile 是 `document '包含 Cloudflare 安全质询的小组件'`。
2. **document 节点的 `value` 就是该 frame 的 URL。** 于是 sitekey 不用猜、不用注入，直接
   从 URL 里解析：

   ```
   https://newassets.hcaptcha.com/captcha/v1/.../hcaptcha.html#frame=checkbox&...&sitekey=a5f74b19-...
   https://www.google.com/recaptcha/api2/anchor?ar=1&k=6Le-wvkSAAAAAPBMRTvw0Q4Muexq9bi0DJwx_mJ-&...
   https://www.google.com/recaptcha/api2/bframe?...&k=6Le-...&bft=0dAFcWeA40KW7...
   ```

   第三条里的 `bft` 就是图像挑战的 token —— 正是打码服务要的东西。**第③级因此也能在零 CDP
   下实现。**

### 14.2 实现

`native/ghost_cli/src/captcha.{h,cpp}` 是**纯函数**：树进，结论出。它只返回节点下标、不自己
点击 —— 输入投递留在 `daemon.cpp`，这样识别逻辑可以脱离浏览器测试。

判定顺序是有讲究的：

| 顺序 | 条件 | 状态 |
|---|---|---|
| 1 | 答案输入框存在（`audio-response`） | `kAudio` |
| 2 | 任一 frame URL 含 `frame=challenge` 或 `bframe` | `kVisual` |
| 3 | checkbox 存在 | `kCheckbox` |
| 4 | 都没有 | `kAbsent` |

**checkbox 必须最后判**：reCAPTCHA 点完之后 anchor 的 checkbox 还留在屏幕上，先判它就会把
已经升级成图像挑战的状态永远报成 `checkbox`。

控件一律按 **automation id** 匹配（`checkbox`、`recaptcha-anchor`、`recaptcha-audio-button`、
`recaptcha-verify-button`、`audio-response`、`menu-info`），不按可见文本 —— 本机标签全是中文，
按文本匹配会失败。

### 14.3 三个把它做错过的坑

1. **页面自己就住在厂商域名上。** hCaptcha 的 demo 页是 `accounts.hcaptcha.com`，reCAPTCHA 的
   是 `google.com/recaptcha/api2/demo`，所以「value 里含厂商域名」这个判据会命中**页面级
   document 自己**，在一个还没有 widget 的页面上报出 widget。修法是先按 `depth` 找出最浅的
   document 作为页面 URL，再在识别循环里跳过它。
2. **只看第一个 frame 不够。** reCAPTCHA 点击后 anchor frame 仍然开着、bframe 另开一个；
   hCaptcha 则把同一个 frame 从 `frame=checkbox` 换成 `frame=challenge`。只看「第一个命中厂商
   的 frame」会永远看不到挑战。现在先把**全部** frame URL 收集起来，再据整组判断。
3. **widget 是动画出现的。** 第一次读到的矩形在点击落下时已经过期一两帧，落进这个空档的点击
   等于没点。现在点击前先 `Sleep(800)` 重读一次坐标，若状态没变再点一次。

### 14.4 实测

`python tools/captcha_check.py` → **18 checks, 0 failed**：

| 挑战 | 结果 |
|---|---|
| Cloudflare Turnstile（嵌入式 widget） | **一次可信点击直接通过**（`state=solved`，widget 消失，页面标题变为 `NopeCHA - CAPTCHA Demo`） |
| Cloudflare 全页 interstitial（`请稍候…`） | 同样识别为 `turnstile`：挑战 frame 在 `challenges.cloudflare.com`，复选框名为 `请验证您是真人` 且**没有 automation id** —— 这正是 Turnstile 必须按名字匹配、并且它的 checkbox 必须最后判的原因 |
| hCaptcha | 读出 `site_key=a5f74b19-9e45-40e0-b45d-47ff91b7a6c2`，点击后 `state=visual`（图像挑战打开，无障碍菜单可用） |
| reCAPTCHA v2 | 读出 `site_key=6Le-wvkSAAAAAPBMRTvw0Q4Muexq9bi0DJwx_mJ-`，点击后 `state=visual`；音频按钮可达，点它后 `state=audio`，`bft` token 读出 |

### 14.5 还没做的

- **图像挑战不求解，而且原因是架构性的，不是偷懒。** 第三级的打码服务本来也能解图片格，但
  reCAPTCHA 一类的流程是：服务端解完返回一个 token，再由页面把它写进隐藏的
  `g-recaptcha-response` 表单域——**这一步需要 DOM 访问**。本架构能读页面、能操作控件的唯一通道
  是无障碍树，而隐藏控件不在树里，于是「拿得到 token，送不进去」。音频路线之所以是零 CDP 下唯一
  走得通的一条，正是因为它把答案变成了一次普通的键盘输入（见 §14.6）。所以这里不是「还没做」，
  是一条必须承认的墙。
- **hCaptcha 的音频路线不可达**，理由与实测见 §14.6 末——它的无障碍菜单按钮对 UIA 的 Invoke
  模式没有反应。
- **第③级已经做了**，见 §14.7。它只走音频，理由同上：只有音频的答案能靠打字送进页面。
- **`cf_clearance` 的持久化不用自己做。** 它就是一个普通 cookie，Chromium 本来就会把它写进
  profile 的 Cookies 库，所以同一个 profile 第二次访问就不再被挑战。这既是好事（通过一次
  就够），也是**验收测试必须每次用全新 profile** 的原因：复用 profile 的
  `tools/captcha_check.py` 会静默退化成「我们已经拿到 clearance」的测试，把读取路径的回归
  全部藏起来。实测：复用 profile 时 `nopecha.com/demo/cloudflare` 直接给 demo 页，树里一个
  widget 都没有，而全新 profile 三次里三次都在 3–5 秒内看到 Cloudflare 的全页 Turnstile
  挑战（`请稍候…` + `challenges.cloudflare.com` frame + `请验证您是真人` 复选框）。

### 14.6 第二级：环回采集 + 本地语音识别

这一级要回答的是「零 CDP 下怎么拿到音频挑战的题」。答案不是去下载音频 URL，而是**让操作系统
把浏览器真正播放过的样本交回来**：浏览器播放时样本必然经过 OS 音频栈，而 OS 会把渲染端点的
样本交给任何申请 loopback 的人。于是不需要注入、不需要读页面、不需要知道挑战的 URL 结构，
拿到的是机器真正播过的东西，而不是对「挑战打算播什么」的猜测。

实现分三块：

| 文件 | 职责 |
|---|---|
| `native\ghost_cli\src\audio_capture.cpp` | WASAPI 共享模式环回采集（`AUDCLNT_STREAMFLAGS_LOOPBACK`），报电平而不只报成功；另给出渲染端点表（含音量/静音）与**音频会话表** |
| `native\ghost_cli\src\speech.cpp` | SAPI 5 本地识别（`CLSID_SpInprocRecognizer` + 引擎 token），数字专用 SRGS 语法 |
| `native\ghost_cli\src\daemon.cpp` | 控制面的 `captcha` 命令新增 `action=solve-audio`：点开音频挑战 → 起采集线程 → 点重放 → 转写 → 回填 → 提交 |

三个实测结论：

1. **环回采集本身工作正常。** 播放 3 s 440 Hz 正弦时 `ghost __audio 6 out.wav` 报
   `远程音频 44100 Hz 2ch 32-bit 3.44s`、`peak 0.610321`、`rms 0.428018`。
2. **浏览器音频确实进得了环回。** 一个 `<audio autoplay loop>` 页面（加
   `--autoplay-policy=no-user-gesture-required`，因此**不需要任何输入合成**）在采集里给出
   `peak 0.610340`、`rms 0.427081`、`silent 0 frames`；同一时刻音频会话表里 Chrome 是
   `active peak 0.6000`，播放前则是 `inactive peak 0.0000`。
3. **数字专用语法是数量级的差别。** 12 个随机 5 位数字串（本机 TTS 合成）：听写语法
   2/12 全对、17/60 位（28.3%）、置信度约 0.02；数字语法 **10/12 全对、58/60 位（96.7%）、
   置信度 0.73–0.99**。前置 0/250/500 ms 静音结果完全相同，所以丢数字是声学模型错误，而不是
   起始点被截断。

`__audio` 与 `__speech` 是隐藏的排障命令（`ghost __audio list|sessions|<秒> [wav]`、
`ghost __speech list|<wav> [lang] [--digits]`）。它们报电平、报是谁在播、报识别置信度，因为
这一层真正有意思的失败不是「端点打不开」（那会直接返回错误），而是端点打开了却什么都没听到
——而「页面没播」和「根本没开流」在样本里长得一模一样。音频会话表就是为区分这两者加的，它
显示的就是 Windows 音量合成器画的那份数据。

**结论：音频挑战不自播。** reCAPTCHA 的音频帧里有它自己的播放控件，在按下之前音频元素只持有一个
开着却不渲染任何样本的会话——所以 `active peak 0.0000` 与「页面没播」是同一个现象，先前那些
`peak 0` 的采集并不是采集失败，而是**挑战在等人按下播放**。这个控件不能靠文本找：它的标签是本地化
句子（本机是「按“播放”可听语音内容」，英文是 “Press PLAY to listen”）。它靠结构找——在含 `bframe`
的挑战帧子树里，第一个 automation id **不以 `recaptcha-` 开头**的按钮就是它，因为 reCAPTCHA 自己
的控件（reload / image / liveness / help / verify）都带 `recaptcha-` 前缀且跨语言稳定，这也是本文件
其余控件一律按 id 匹配的同一个理由（`native\ghost_cli\src\captcha.cpp`）。补上这一次点击后，同一台
机器、同一个会话的采集立刻从 `peak 0` 变成 `peak 0.1767`；用 `keep` 参数留下的录音是
`44100 Hz 2ch 16-bit 7.98 s`、整段 `peak 0.405060`，逐秒电平里有 **7 秒有声**。

**本机的一个诚实限制：**

- **本机没有英文识别器。** `HKLM:\SOFTWARE\Microsoft\Speech\Recognizers\Tokens` 下只有
  `MS-2052-80-DESK`（zh-CN），`Speech_OneCore` 下只有 `MS-2052-110-WINMO-DNN`，安装英文识别器
  需要管理员权限（本机有 en-US *语音* `TTS_MS_EN-US_ZIRA_11.0`，却没有 en-US *识别器*——Windows
  按语言特性装识别器，这两者并不绑定）。因此英文的 reCAPTCHA 音频挑战在这台机器上**无法本地
  转写**；`ghost __speech` 对未知语言走诚实降级路径（`no recognizer for "en" is installed
  (installed: zh-CN)`），不会假装听懂。这不是第二级的设计缺陷，而是它的边界，也正是第三级存在的
  理由。

- **hCaptcha 的音频路线在当前实现下不可达，但「卡在哪」已按通道分开实测。** 挑战帧里确实有
  一个按钮，名字就是 `About hCaptcha & Accessibility Options`（**没有 automation id**，只能靠名字
  找），挑战图片自己的替代文本也在提示「请使用辅助功能菜单里的『检索 Cookie』或『文本验证』」。两条
  点击通道结果不同：
  - **UIA Invoke 模式（无前台会话的唯一通道）**：点它之后 **10 秒内树完全不变**（49 个节点，逐秒
    采样前后一致）。同一个 Invoke 模式点 hCaptcha 自己的复选框是有效的（状态从 `checkbox` 变
    `visual`），所以差别不在通道而在控件：复选框是真正的表单控件，那个菜单按钮是自定义元素，
    Chromium 给了它 UIA 按钮角色，但激活它并不触发页面的处理器。
  - **合成输入（SendInput，需要前台会话）**：2026-10-08 重测，点击约 1 秒后菜单打开（树 42→47
    个节点）。菜单里有四项——「可访问性：检索 Cookie」「向 hCaptcha 举报图片」「向 hCaptcha 报告
    错误」「关于 hCaptcha 的信息」——**没有任何一项是可解的挑战**：检索 Cookie 需要浏览器拿不到的
    辅助功能 cookie，这个构建里也没有文本挑战项。所以这一版卡住的原因是**菜单内容**，不是菜单
    打不开；未来的 hCaptcha 构建若在这个按钮后面放出文本挑战，前台会话就能点到它。
  reCAPTCHA 之所以能走通，正是因为它把音频做成了一个带 `recaptcha-audio-button` id 的真按钮。

**这一层可用的正对照。** 判断「环回→STT 这条路到底行不行」不能只看挑战：本机 TTS 合成中文数字
`三八五一二四` 播进一个 `<audio autoplay loop>` 页面，环回采到 `44100 Hz 2ch 32-bit 4.99s`、
`silent 0 frames`、`peak 0.955259`，`ghost __speech <wav> zh --digits` 转写出 `385124`、
**置信度 0.9366**。**所以整条通路是通的，reCAPTCHA 的失败纯粹是缺英文识别器。**

## 14.7 第三级：把采集到的音频交给第三方解题服务

第二级只被语言卡住，所以第三级不是「另一条采集路线」，而是**同一份录音的另一个去向**：本地识别
失败且配置了 key 时，把环回采到的音频交给 2captcha 一类的服务转写。**0.12.0 起还有更短的一条**：
既然服务要的是文件而不是声音，那就别录了——直接问页面要那段 clip 的地址（§14.8），这条路连声卡
都不需要。

上传的不是原始录音。渲染端点给的是 44.1 kHz 立体声，既超出语音识别所需，也大到不必上传，所以
`write_speech_wav`（`native\ghost_cli\src\audio_capture.cpp`）一遍完成混单声道与线性重采样，写出
16 kHz 单声道 16 位 PCM——实测一次 5 秒挑战的上传体是 **156,844 字节**。

| 文件 | 职责 |
|---|---|
| `native\ghost_cli\src\solve_api.cpp` | WinHTTP 客户端：`POST /in.php`（`method=audio`，体是 url 编码后的 base64）拿任务号，再每 5 秒 `GET /res.php` 轮询，`CAPCHA_NOT_READY` 视为继续等，最长 120 秒 |
| `native\ghost_cli\src\solve_api.h` | `resolve_solve_api`：URL 取 `GHOST_CAPTCHA_URL`（默认 `https://2captcha.com`），key 依次取 显式参数 → `GHOST_CAPTCHA_KEY` → profile 的 `captcha_api_key` |
| `native\ghost_cli\src\daemon.cpp` | `solve-audio`：配了 key 时先试 §14.8 的 URL 路线，否则本地识别；两条都拿不到数字且配了 key 时才把录音交给服务，并在回应里如实标注 `solved_by` 是 `local` 还是 `api` |

选 WinHTTP 而不是 WinINet，是因为进程可能处于受限令牌或模拟身份之下，而 WinINet 的按用户会话状态
正是那种会静默失败的东西。**没有内置 key，也不会有**：未配置时 `provider` 为空，第三级是「关闭」
而不是「坏掉」，`solve-audio` 就停在第二级的诚实拒绝上。

实测（`tools\solve_api_check.py`，用一个绑在 `127.0.0.1:0` 的本地替身服务当对手）：**12 checks,
0 failed**。替身服务收到的确实是 `method=audio`、带 key、带 `language=en`、`Content-Type:
application/x-www-form-urlencoded` 的表单；base64 体解开后 RIFF 头是 `16000 Hz 1ch 16-bit`；
第一次轮询拿到 `CAPCHA_NOT_READY` 后确实再轮了一次；最终 `solved_by='api'`、`heard='385124'`、
`typed=True`。**这条验收不碰真钱、不碰真服务，验的是这一级的协议与管线，而不是某个服务商的答案
质量。**

**这条验收是会被限流的，而且它如实报告这一点。** 同一个地址反复索要音频挑战之后，reCAPTCHA
先是停止提供音频路线，再往后连 widget 都不再下发（实测：连续四轮全部 `provider=none`、
`state=absent`）。这时脚本把结果记为 **not measurable 而不是 failed**——把挑战的拒绝算成这一级的
失败，和把一次没发生的运行算成通过一样不诚实。四轮之间的退避是刻意的：**拒绝是按频率形成的，
猛敲它正是造成它的原因。**

一个在验收里暴露出来的真 bug 值得记下来：API 分支覆盖了 `transcript.digits` 却忘了改
`transcript.ok`，于是后面 `if (!transcript.ok)` 的提前返回照常触发，服务给的答案**从未被输入页面**
（表现是 `heard` 有值而 `typed=None`）。**一个「成功」的分支必须把整份判定状态一起改掉，只改它自己
写的那几个字段，等于没改。**

## 14.8 `captcha action=audio-url`：直接读挑战自己的 clip 地址

服务要的是音频文件，而音频文件的地址页面自己最清楚。所以 0.12.0 加了一条独立动作：把挑战即将
播放的那段音频的地址读出来并下载，`solve-audio` 在配了 key 时先走它。

三个实测出来的细节决定了实现：

1. **clip 通常不在页面的 DOM 树里。** reCAPTCHA 的音频帧是独立 target：对 page session 调
   `DOM.getDocument {depth:-1, pierce:true}` 遍历整棵树，`AUDIO`/`SOURCE` 节点数是 **0**；
   `Target.getTargets` 里能看到那个 iframe，`Target.attachToTarget {targetId, flatten:true}`
   之后在返回的 sessionId 上再取一次树，才拿到 **1 个** AUDIO 节点。所以 `read_audio_url`
   先试页面树，再退到「逐个 attach 所有 iframe target」。
2. **过滤不能是白名单。** 第一版只 attach 地址里含 `recaptcha`/`hcaptcha`/
   `challenges.cloudflare.com` 的帧——这是闭集缺陷，我自己写的 stand-in 帧就是反例。现在按
   「先命名厂商帧、再其余帧」排序后逐个试，上限 12 个。**厂商名单不是这段代码有资格决定的。**
3. **CDP 给的是属性的原文，不是解析后的 URL。** 帧里写 `src="/clip.wav"`，树里就是
   `/clip.wav`；而 `document.querySelector('audio').src` 是绝对的。**document 节点自带
   `baseURL`**，所以可以在 C++ 里自己解析（`resolve_url`），**不需要跑任何页面脚本**。

**真机实测**：对 live reCAPTCHA 跑一次，`audio-url` 返回
`https://www.google.com/recaptcha/api2/payload?p=06AFcWeA...&k=6Le-wvkSAAAAAPBMRTvw0Q4Muexq9bi0DJwx_mJ-`、
`bytes=38463`，落盘文件头是 `49 44 33`（`ID3`）——**clip 是 MP3**。所以服务路线把它原样上传，
本机不需要解码器，可执行文件里也就没有链接任何解码器。

实测（`tools\audio_url_check.py`，本地两个源做 stand-in）：**13 checks, 0 failed**。`127.0.0.1`
的外层页面内嵌 `127.0.0.2` 的 iframe——两个 origin 不同 site，Chromium 真的给了独立渲染进程，
`Target.getTargets` 能证明这一点，所以 stand-in 覆盖的正是最难的那条路。四个会话分别验：无名帧
（回落路径，做全部断言）、命名成 `/recaptcha/bframe` 的帧（厂商路径）、没有 audio 元素的页面
（`found:false`）、`--no-cdp` 会话（`found:false` 且说明缺的是哪条通道）。下载到的字节数与
sha256 与源文件逐字节一致。

`blob:` 与 `data:` 地址是诚实的边界：它们住在渲染进程里，页面之外没有任何东西能取到，所以
`found` 为真而 `fetched` 为假，`detail` 说明原因，`solve-audio` 随之回落到环回采集。


