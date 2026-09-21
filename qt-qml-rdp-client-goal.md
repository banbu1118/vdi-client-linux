# Qt6 + QML 最小化 FreeRDP 客户端目标

本文档记录基于 libfreerdp 构建最小化跨平台 RDP 客户端的工作计划与实际状态。

> 目录：`/home/kk/vdi-client-linux-tmp-20260916/vdi-client-linux/`
> - `qfreerdp-master/` — qf-client (QML + FreeRDP)
> - `vdi-client-linux/` — VDIClient (Qt Widgets 登录/VM 管理)
> - `freerdp-3.28.0/` — FreeRDP 源码及编译产物
>
> 构建配置参考：FreeRDP 3.28.0 Release 模式（GCC 14 + `-O3 -DNDEBUG` + LTO），CPU 转存帧缓冲 + GL 纹理上传，无 DMA-BUF/VAAPI，音频输入/输出均使用 FreeRDP 库自带的 PulseAudio 后端（通过 PipeWire），X11/Wayland 客户端关闭。

## 架构概览

整套 VDI 客户端分为两个独立进程：

```
VDIClient (QWidgets)              qf-client (QML + FreeRDP)
┌──────────────────────┐          ┌─────────────────────────┐
│ LoginWindow          │  fork    │ QGuiApplication + QML   │
│ - 登录/Token 管理    │────────→ │ - RdpViewItem 渲染      │
│ - VM 列表/状态监控   │  启动    │ - FreeRDP 连接线程      │
│ - 启动 qf-client     │          │ - 剪贴板/USB/音频通道   │
│ - 心跳检测           │          │ - 动态分辨率 (disp DVC) │
└──────────────────────┘          └─────────────────────────┘
```

| 项目 | 技术栈 | 入口 | 构建产物 |
|------|--------|------|----------|
| VDIClient | Qt6 Widgets + Network | `main.cpp` → `LoginWindow` | `build/bin/VDIClient` |
| qf-client | Qt6 QML/Quick + FreeRDP 3.28 | `main()` → `mini-qf-client.cc` + `main.qml` | `build/qf-client` |

---

## 编译配置摘要

### FreeRDP 3.28.0 当前编译配置

**基础环境**：GCC 14 (`-O3 -DNDEBUG` + LTO 链接时优化 + PIC)，Release 模式，C11 标准，构建为共享库（不构建客户端/服务器可执行文件）。

| 分类 | 开启 (ON) | 关闭 (OFF) |
|------|-----------|------------|
| **编解码** | FFmpeg H.264 (v61.19.101), DSP FFmpeg, SWScale (v8.3.100), NSCodec (内置) | JPEG, OpenH264, GFX AV1, AOM, Opus, SoXR, LAME, AAC 系列, GSM, **VAAPI** |
| **音频** | PulseAudio (主用, 通过 PipeWire), ALSA (编译但 qf-client 不直接使用) | OSS |
| **通道** | cliprdr, rdpsnd, rdpdr, drdynvc, rdpGFX, disp, rdpecam, urbdrc, audin, RAIL, rdpei (多触点), ainput, video, encomsp, echo, remdesk, location, geometry | TSMF (多媒体), SSH Agent, RDP2TCP, GFX 重定向, RDPEAR, RDPEWA |
| **客户端通道** | drive, serial, parallel, smartcard (总开关 ON) | printer, rdpecam_client_stub, rdpemsc_client, telemetry_client |
| **客户端** | client_common, client_channels | X11, Wayland, SDL 客户端, 可执行二进制 |
| **其他** | FUSE3 (v3.17.2), OpenSSL (v3.5.6), PKCS11, UNICODE_BUILTIN, VERBOSE_WINPR_ASSERT, SIMD, AVX2, SMARTCARD_EMULATE | Kerberos, CUPS, AAD, cJSON, uriparser, LibreSSL, MbedTLS |

> **VAAPI 当前关闭** (`WITH_VAAPI=OFF`)：因部分机型上 VAAPI 硬件解码偶发 SIGSEGV (exit code 11)，暂时回退到 CPU 软件解码。`libva-dev` 已安装，需要时可重新开启。
>
> **PRINTER_CLIENT 关闭**：`CHANNEL_PRINTER_CLIENT=OFF`，qf-client 不使用打印机重定向。
>
> **JSON 支持缺失**：cJSON/jansson/json-c 均未检测到，`WITH_WINPR_JSON=OFF`。

### VDIClient (Qt Widgets)

| 分类 | 说明 |
|------|------|
| 框架 | Qt 6 Core + Widgets + Network |
| C++ 标准 | C++17 |
| 编译 | gcc/g++ |

### qf-client (QML + FreeRDP)

| 分类 | 说明 |
|------|------|
| 框架 | Qt 6 Core + Gui + Qml + Quick + DBus（另用 `Qt6::GuiPrivate` 取 Wayland 原生句柄） |
| 构建依赖 | `qt6-base-private-dev`（私有 QPA 头 `qpa/qplatformnativeinterface.h`）、`libwayland-dev` + `wayland-scanner`（生成快捷键抑制协议代码）、`libqt6dbus6` |
| C++ 标准 | C++20 (qf-client 源码) / C11 (cliprdr 等 C 源码) |
| 编译器 | clang / clang++ (通过 Debian clang 19 验证) |
| 日志 | spdlog + fmt |
| FreeRDP | 自编译 3.28.0 (GCC 14)，通过 `CMAKE_PREFIX_PATH` 引用到 `freerdp-3.28.0/install/` |
| 渲染 | GLESv2 + QSGRenderNode，CPU staging buffer + `glTexSubImage2D` 全帧上传 |

---

## 功能对照表

| 功能模块 | mstsc (Windows 原生) | 官方 FreeRDP | 你的 Qt + FreeRDP | 差距 | 功能实现原理 |
| --- | --- | --- | --- | --- | --- |
| RDP 基础连接 | ✅ | ✅ | ✅ | libfreerdp 库完整集成 | `freerdp_new`+`freerdp_context_new`+`freerdp_connect`；PreConnect/PostConnect 回调配置参数；失败自动重试 3 次（间隔 500ms）；三态状态机支持重连 |
| TLS/NLA/CredSSP 认证 | ✅ | ✅ 完整支持 | ⚠️ NLA 可用，证书跳过验证 | 缺少完整证书管理；`WITH_KRB5=OFF` 无 Kerberos | FreeRDP 内置 NLA (CredSSP)；`IgnoreCertificate=TRUE` 跳过证书校验 |
| Microsoft Entra SSO | ✅ | ✅ | ❌ | 未实现；`WITH_AAD=OFF` | — |
| VDIClient 登录/VM 管理 | ❌ (mstsc 无独立登录器) | ❌ | ✅ **QWidgets 登录界面 + VM 列表管理** | 自研 VDI 管理客户端 | `LoginWindow` 管理：服务器健康检查、Token 认证、虚拟机列表/状态查询、开机/关机/重启/还原、心跳保活、多语言 |
| 用户名密码登录 | ✅ | ✅ | ✅ ✅ **双重** | qf-client 和 VDIClient 各有一套凭据管理 | qf-client: `FreeRDP_Username`/`FreeRDP_Password`；VDIClient: REST API `/api/v1/auth/login` 获取 Token，记住密码 / 自动登录 |
| Drive 重定向 | ✅ | ✅ `/drive` `/drives` | ✅ | 与 Windows 版对齐：home + NAS + 可移动介质自动重定向，**不含系统分区**；CLI/.rdp 文件配置，未配置时自动添加默认 HOME 驱动；重连时通过 `g_saved_drive_args` 恢复 | `freerdp_client_add_device_channel(settings, 3, {"drive", "HOME", home})` 走显式路径；`/drives` 走 rdpdr 通配枚举（`is_redirectable_mount()` + 1000ms 挂载点热插拔）。详见「磁盘与 USB 重定向」章节 |
| USB 重定向 | ❌ (mstsc 原生不支持) | ✅ `/usb` | ✅ | URBDRC 通道 + libusb 枚举，由 CLI `/usb:` 或 .rdp 文件 `usbdevicestoredirect` 显式启用；默认禁用。**已被磁盘重定向的设备置灰互斥** | 标志持久化 + 首次连接时检测 + 重连时恢复；Linux 下用 libusb `LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED/LEFT` 事件；多设备合并且 `id:`/`addr:` 二选一。详见「磁盘与 USB 重定向」章节 |
| 摄像头重定向 | ✅ (Win10+) | ✅ `/camera` | ✅ | **硬编码启用**，默认重定向所有 V4L2 摄像头设备 (`device:*`)，无运行时 UI 控制。媒体类型上报已改造：**枚举全部输入格式 + 按 (width,height,fps) 去重 + 压缩格式优先**，不再只报单一未压缩格式。详见「摄像头重定向（rdpecam）」章节 | `freerdp_client_add_dynamic_channel(settings, 2, {RDPECAM_DVC_CHANNEL_NAME, "device:*"})` |
| 多显示器 | ✅ `/multimon`（最多 16 屏） | ✅ `/multimon` | ⚠️ 基础单屏分辨率变化 | 缺少多 Monitor Layout 支持 | disp DVC `SendMonitorLayout` 发送单个 primary 布局 |
| 选择显示器 | ✅ | ✅ | ❌ | 未实现 | — |
| 多屏切换 | ✅ | ✅ | ❌ | 未实现 | — |
| 动态分辨率 | ❌（mstsc 不支持） | ✅ | ✅ **300ms 消抖 + 4px 对齐 + 全帧上传 + glViewport 显式设置** | 窗口调整后 RDP 分辨率自动同步 | disp DVC `SendMonitorLayout` → GFX_RESET；300ms QTimer 消抖；4px 对齐请求；全帧 `glTexSubImage2D` 上传；`RenderNode::render()` 中 `glViewport(0,0,vpW,vpH)` 覆盖 Qt 场景图缓存的旧视口 |
| 字体平滑 (ClearType) | ✅ | ✅ | ✅ | 显式启用 | `FreeRDP_AllowFontSmoothing=TRUE`；PerformanceFlags 发送 PERF_ENABLE_FONT_SMOOTHING 位 |
| RD Gateway (TSG) | ✅ | ✅ `/g:` | ✅ | PAA token 认证，网关隧道通过 TSG 协议建立；代码层仅调低 WLog 级别，依赖 FreeRDP CLI 解析 | `GatewayHostname`/`GatewayPort`/`GatewayAccessToken`/`GatewayCredentialsSource=5` |
| Restricted Admin 模式 | ✅ | ✅ `/restricted-admin` | ❌ | 未实现 | — |
| Remote Credential Guard | ✅ | ✅（3.9+） | ❌ | 未实现 | — |
| Pass-the-Hash 登录 | ❌ | ✅ `/pth:hash` | ❌ | 未实现 | — |
| TLS 版本/密码配置 | ✅ | ✅ | ❌ | 未实现 | — |
| 证书管理 UI | ✅ | ✅ `/cert:tofu|name|fingerprint` | ⚠️ 仅跳过验证（IgnoreCertificate=TRUE） | 缺少 UI 化证书管理 | 当前仅支持 `/cert:ignore` |
| RemoteFX (GFX H.264) | ✅（Win7~10） | ✅ | ✅ GFX pipeline 硬编码开启（H.264/AVC444） | 使用 FFmpeg 软件解码（VAAPI 关闭） | `SupportGraphicsPipeline=TRUE`, `GfxH264=TRUE`, `GfxAVC444=TRUE`, `GfxAVC444v2=TRUE`；`WITH_GFX_H264=ON` 编译；`WITH_OPENH264=OFF` |
| AVC444/H.264 解码 | ✅（Win10+） | ✅ | ✅ AVC420+AVC444+AVC444v2 全开 | **VAAPI 关闭** → CPU 软件解码，性能较 VAAPI 低 | `FreeRDP_GfxH264=TRUE` 启用 AVC420；`FreeRDP_GfxAVC444=TRUE` 启用 AVC444；`FreeRDP_GfxAVC444v2=TRUE` 启用 AVC444v2 |
| GFX AV1 编解码 | ✅（Win11+） | ✅ `/gfx:av1` | ❌ | 未实现；`WITH_GFX_AV1=OFF` | — |
| 窗口缩放（Smart Sizing） | ✅ | ✅ | ✅ | 一致（等比缩放） | Qt 场景图自动缩放 GL 纹理四边形以填充窗口 |
| 自适应缩放 | ✅ | ✅ | ✅ | 一致 | `scale_frame()` 将服务器分辨率的脏矩形等比映射到窗口坐标 |
| 全屏模式 | ✅（`/f` 参数） | 完整支持 | ✅ | 默认全屏 + 切换按钮退出/进入 | QML `Window.FullScreen` 初始状态；`toggleDisplayMode()` |
| 窗口最小尺寸 | ✅ | ❌ | ✅ | minimumWidth=650 / minimumHeight=550 | QML `Window` 元素设置最小尺寸约束 |
| 无边框全屏 | ❌ | ✅ | ❌ | 已切回系统原生标题栏（用户要求） | 使用系统原生窗口装饰 |
| 悬浮工具栏（连接栏） | ✅（顶部固定连接栏） | ✅ | ✅ | 5px 顶部居中热区（380px 宽，比工具栏 340px 略宽作缓冲）+ **悬停 1s 延时显示** + 3s 自动隐藏 + 图钉固定 + CAD/USB/全屏/最小化按钮 | QML `MouseArea` 检测进入热区 → `showDelayTimer`(1000ms) 到点后校验鼠标仍在热区内才显示 → 离开热区启动 `hideDelayTimer`(3000ms) 隐藏；热区 `z` 高于工具栏且不消费事件，鼠标事件透传给 RDP。1s 延时用于降低鼠标路过屏幕顶部时的误触发 |
| 多窗口模式 | ✅（多个 mstsc） | ✅ | ❌ | 未实现 | — |
| RemoteApp | ✅ | ✅ | ❌ | 未实现；`CHANNEL_RAIL=ON` 已编译但未使用 | — |
| RemoteApp 无缝窗口 | ✅ | ✅ | ❌ | 未实现 | — |
| 远程协助 / 会话影子 | ✅ | ✅ | ❌ | 未实现 | — |
| 鼠标输入 | ✅ | ✅ | ✅ | 坐标缩放映射到 RDP 分辨率；左/中/右键分别映射到 `PTR_FLAGS_BUTTON1/3/2`（**中键是 `BUTTON3`**），拖拽中的 move 事件用 `QMouseEvent::buttons()` 带出按住的键 | `mouseEventScaleSend()` → `freerdp_input_send_mouse_event()`；`mouseButtonToPtrFlags()` / `mouseButtonsToPtrFlags()` |
| 相对鼠标模式 | ✅ | ✅ | ❌ | 未实现 | — |
| 鼠标捕获 | ✅ | ✅ | ❌ | 未实现 | — |
| 鼠标侧键 (XButton1/2) | ✅ | ✅ | ❌ | 未实现 | — |
| RDP Pointer（光标通道） | ✅ | ✅ | ✅ | 完整实现：Pointer 六回调 + XOR/AND 解码 + unordered_map 缓存 + SYSPTR_NULL 自动隐藏 + 3px 移动阈值恢复 + 2000ms 防抖 | `graphics_register_pointer` 六回调；`freerdp_image_copy_from_pointer_data()` 解码 → QCursor |
| 键盘扫描码 | ✅ | ✅ | ✅ | 覆盖主键盘、小键盘（按 keysym 判定，与 NumLock 无关）、PrintScreen / Pause / Menu、F1-F12、方向键与小键盘整组 | `qf::to_freerdp_key_code()` 查找表 → `freerdp_input_send_keyboard_event_ex()`；Pause 因是特殊按键序列，走 `freerdp_input_send_keyboard_pause_event()` |
| Unicode 输入 | ✅ | ✅ | ✅ | 扫描码回退到 Unicode | 键码映射失败时回退到 `freerdp_input_send_unicode_keyboard_event()` |
| **组合快捷键**（Super 组合、Alt+Tab 等） | ✅（专用键盘钩子逐条拦截） | ✅（Wayland 下用合成器快捷键抑制） | ✅ 修饰键重建 + 合成器快捷键抑制 | **无需白名单**：Wayland 下抑制生效后合成器把整窗口快捷键让给客户端，Super+R/E/D/A/I/M/X/S/V/G/P/W/T/B/U、Super+数字/Home/`.`、Alt+Tab、Alt+Shift+Tab、Ctrl+Esc、Ctrl+Shift+Esc 全部直达远端；对比 Windows 版需在 `isInterceptedWinCombo()` 中逐条登记 | 见下方「组合快捷键实现」章节 |
| 输入法 IME | ✅ | ⚠️ | ❌ | 中文输入可能有问题 | 未实现 IME 专用通道 |
| 触屏 / 多点触控 | ✅ | ✅ | ❌ | 未实现；`CHANNEL_RDPEI=ON` 已编译但未使用 | — |
| 触笔（Pen）输入 | ✅ | ✅ | ❌ | 未实现 | — |
| 粘贴文本 | ✅ | ✅ | ✅ **由 `/clipboard` 参数控制，默认禁用** | 一致 | cliprdr 通道；由 CLI 参数 `/clipboard` 控制（默认禁用） |
| 剪贴板图片 | ✅（PNG/DIB/DIBV5） | ✅ | ✅ **由 `/clipboard` 参数控制，默认禁用** | 一致 | cliprdr 通道：PNG/DIB/DIBV5 → `QImage::fromData()`/`imageFromDib()` → `QClipboard::setImage()` |
| 剪贴板文件 | ✅ | ✅ | ✅ **由 `/clipboard` 参数控制，默认禁用** | 一致 | `FileGroupDescriptorW` 解析 → 独立工作线程 64KB 分块下载；由 `/clipboard` 控制；`cliprdr_file_context_new` + `clipboard_entry` worker |
| 麦克风输入（音频输入） | ✅ | ✅ | ✅ | audin DVC + FreeRDP 库自带 PulseAudio 后端 → PipeWire；**硬编码启用**，无运行时 UI 控制 | `freerdp_client_add_dynamic_channel(settings, 2, {AUDIN_CHANNEL_NAME, "sys:pulse"})`；addin provider 为简单链式转发，无自编译后端 |
| RDPSND 声音输出 | ✅ | ✅ | ✅ | FreeRDP 库自带 PulseAudio 后端 → PipeWire | FreeRDP 编译时 `WITH_PULSE=ON`，rdpsnd 走库内 pulse 后端（`libfreerdp-client3.so` 中的 `rdpsnd_pulse.c`）；PCM 8/16 位音频播放；`pulse->stream=(nil)` 警告偶发时检查 PipeWire 是否运行 |
| 动态虚拟通道 DVC | ✅ | ✅ | ✅ | rdpsnd/GFX/disp/cliprdr/urbdrc/ecam/audin DVC 均已使用 | 通过 `freerdp_client_add_dynamic_channel` 注册；`ChannelConnected` 回调保存上下文 |
| 多触点（Multi-Touch） | ✅ | ✅ | ❌ | 未实现；`CHANNEL_RDPEI=ON` 已编译但未使用 | — |
| 触笔（Pen）输入 | ✅ | ✅ | ❌ | 未实现 | — |
| 多媒体重定向 (TSMF) | ⚠️ | ✅ | ❌ | 未实现；`CHANNEL_TSMF=OFF` | — |
| 图形通道 | GDI/RDP | GDI/OpenGL | ✅ **CPU 转存帧缓冲 + GL 纹理上传 + QSGRenderNode** | GFX 解码到 GDI primary buffer → `copyFrameData()` 拷贝脏矩形到 staging buffer → 渲染线程 `glTexSubImage2D` 上传 → QSGRenderNode 全屏四边形 | **架构变更：从 DMA-BUF 零拷贝回退到 CPU staging buffer**（使用 `std::vector<uint8_t>` + `glTexSubImage2D`），消除了 EGL/GBM 依赖，降低偶发崩溃概率 |
| RemoteFX Codec | ✅（Win7~10） | ✅ | ✅ GFX H.264 硬编码开启 | 使用 FFmpeg H.264（`WITH_OPENH264=OFF`）| `WITH_GFX_H264=ON` 编译 |
| Bitmap Cache | ✅ | ✅ | ✅ FreeRDP 默认开启 | BitmapCacheV3 已启用 | 默认值 |
| 网络自动优化 | ✅ | ✅ | ✅ | NetworkAutoDetect=TRUE | 带宽估算 |
| 数据压缩 | ✅ | ✅ `/compression` | ✅ | CompressionEnabled=TRUE | MPPC/MPPC-8k 无损压缩 |
| Bandwidth Auto Detect | ✅ | ✅ | ✅ | NetworkAutoDetect=TRUE | 标准带宽自适应 |
| UDP Transport | ✅（RDP 10 UDP 优先） | ✅ | ✅ | SupportMultitransport=TRUE | UDP 优先，失败回退 TCP |
| RDPEUDP2 | ✅ | ✅ | ✅ | SupportMultitransport=TRUE 时自动协商 | — |
| TCP fallback | ✅ | ✅ | ✅ | 默认 | TCP；`TcpConnectTimeout=5s` |
| Smartcard Login | ✅ | ✅ | ❌ | 未实现；`CHANNEL_SMARTCARD=ON` 但 `SMARTCARD_PCSC=ON` 仅总开关 | — |
| Kerberos SSO | ✅ | ✅ | ❌ | 不可用；`WITH_KRB5=OFF` | — |
| Windows Hello | ✅（Win10+） | ⚠️ | ❌ | 未实现 | — |
| GPO 策略兼容 | ✅ | ✅ | 依赖协议 | 未验证 | — |
| 多会话管理 | ❌ 客户端职责 | ❌ 客户端职责 | ✅ **VDIClient VM 列表管理** | 完整的 VDI 虚拟机列表管理 | VDIClient 通过 REST API 管理多个 VM 的开机/关机/重启/恢复/连接 |
| 断线重连 | ✅（自动重连） | ⚠️ | ✅ | 三态状态机 + 3 次重试 + 重连后清理上下文 | `WaitForMultipleObjects` 100ms 轮询 → `freerdp_shall_disconnect_context`/`g_reconnectRequested`（USB 切换触发）→ `freerdp_disconnect`+`freerdp_connect` 最多 3 次 |
| 自动恢复 Session | ⚠️ | ⚠️ | ✅ | `freerdp_disconnect` → `freerdp_connect` → my_pre_connect 清理旧设备/通道 → my_post_connect 重新初始化 GDI/指针回调 | 重连时 channel contexts 已释放：`my_pre_connect()` 中 `freerdp_device_collection_free()`+`freerdp_dynamic_channel_collection_free()` 清理旧设备；`g_dispContext/g_gfxContext/g_clipboard_client_context` 置空防止残指针回调 |
| 联网认证—Token 过期处理 | ✅ | ✅ | ✅ **VDIClient 处理** | Token 过期自动返回登录页、终止 RDP 进程 | VDIClient `isTokenExpired()` 检测 HTTP 401 → `handleTokenExpired()`: 清 Token、终止 qf-client、切回登录页 |
| VDI—快照管理 | ❌ (mstsc 无此功能) | ❌ | ✅ | 通过 REST API 查询/恢复 Milestone 快照 | VDIClient `fetchVmSnapshot()` → `onVmSnapshotReply()` 显示/隐藏还原按钮 |
| VDI—心跳保活 | ❌ | ❌ | ✅ | 每 15 秒发送心跳维持会话 | VDIClient `startHeartbeat()` → QTimer 15s → `/api/v1/users/heartbeat` POST |
| VDI—修改密码 | ❌ | ❌ | ✅ | 通过服务器 API 修改密码 | VDIClient `onChangePasswordClicked()` → 密码对话框 → `PUT /api/v1/users/password` |
| VDI—记住密码 / 自动登录 | ❌ | ❌ | ✅ | QSettings 持久化 + 启动自动登录 | VDIClient `saveSettings()`/`loadSettings()` 使用 QSettings |
| VDI—多语言 | ❌ | ❌ | ✅ | 中/英/日/繁四语言 | VDIClient `initTranslations()` 内嵌字典 + `updateLanguage()` 即时切换 |
| /admin 管理连接 | ✅ | ✅ `/admin` | ❌ | 未实现 | — |
| 网络连接类型选择 | ✅ | ✅ `/network:...` | ❌ | 未实现 | — |
| 代理支持 | ✅ | ✅ `/proxy:...` | ❌ | 未实现 | — |
| Hyper-V 控制台 | ✅ | ✅ `/vmconnect` | ❌ | 未实现 | — |
| RDP2TCP 隧道 | ❌ | ✅ `/rdp2tcp` | ❌ | 未实现；`CHANNEL_RDP2TCP=OFF` | — |
| 录音/回放 | ❌ | ✅ `/dump` | ❌ | 未实现 | — |
| 键盘布局/语言配置 | ✅ | ✅ `/kbd:...` | ❌ | 未实现 | — |
| Alternate Shell | ✅ | ✅ `/shell:...` | ❌ | 未实现 | — |
| 公共模式（Public Mode） | ✅ `/public` | ✅ | ❌ | 未实现 | — |
| 日志系统 | ✅ | CLI 日志 | ✅ **spdlog/fmt + VDIClient 日志** | qf-client: spdlog 5 级日志；VDIClient: `qInfo()/qWarning()` 日志 | 两份日志独立输出 |
| 参数配置文件 / CLI | ✅（.rdp 文件） | cmd 参数 | ✅ | `freerdp_client_settings_parse_command_line()` 解析 | 在 `my_pre_connect()` 中执行一次（初始连接）；`g_cli_argc/g_cli_argv` 全局变量保存；重连时跳过解析但恢复 `/drive:` `/usb:` 配置 |
| 交替 Shell / 启动程序 | ✅ | ✅ | ❌ | 未实现 | — |
| Public Mode (公共模式) | ✅ | ✅ | ❌ | 未实现 | — |
| KDC Proxy | ✅ | ✅ `/kdcproxy` | ❌ | 未实现 | — |
| 插件体系 | ❌ | channels | ❌ | 未实现 | — |
| 证书管理 | ✅ | 完整 | ⚠️ 仅跳过验证 | 需要 UI 化 | `FreeRDP_IgnoreCertificate=TRUE` |
| 多语言 | ✅ | 部分 | ✅ **VDIClient 支持 4 语言** | qf-client 使用了 `qsTr()` 但未配置多语言翻译文件 | QML `qsTr()` 占位；VDIClient 独立 QWidgets 多语言系统 |

---

## 组合快捷键实现

### 问题背景

Wayland 下 `Super` 组合键与 `Alt+Tab` 在**合成器层**就被 GNOME/KDE 消费，按键根本到不了客户端；而 X11 可用的 `XGrabKeyboard` 在 Wayland 没有等价接口。此外合成器吞掉 `Super` 的按下事件后，远端会收到"缺了 Win 的其他键"，导致 `Win+R` 之类组合失效。

两级处理：P0 不依赖合成器，靠按键事件自带的修饰键信息重建；P1 通过 Wayland 协议请合成器把整窗口快捷键让给客户端。

### P0 — 修饰键重建（`rdp-view-item.h`）

以 `QKeyEvent::modifiers()` 为准，与远端修饰键状态对齐。这是**不依赖任何平台协议**的兜底，对 X11/Wayland 都有效。

- 状态：`m_remoteModDown[ModCount]`（Shift / Ctrl / Alt / Meta 四个索引）记录**远端**当前按下的修饰键
- 普通键到达时调用 `syncRemoteModifiers()`：
  - `modifiers()` 里有、远端却没按下 → 补发按下（合成 `LSHIFT` / `LCONTROL` / `LMENU` / `LWIN`）
  - `modifiers()` 里没有、远端却按着 → 补发抬起（抬起事件被吞时救回来）
- 修饰键自身的物理事件仍按原路径转发，靠 `m_remoteModDown` 去重避免重复发送
- 窗口失焦时 `releaseAllRemoteModifiers()`，避免修饰键在远端卡住

效果：即使合成器吞掉 `Super` 的按下事件，`Win+R/E/D/A` 依然成立。

### P1 — 合成器快捷键抑制（`kbd-shortcuts-inhibit.{h,cc}` + `protocols/`）

通过 `zwp_keyboard_shortcuts_inhibit_v1` 请合成器在前台窗口聚焦期间不处理它自己的快捷键。实现移植自 FreeRDP `uwac` 的 `UwacSeatInhibitShortcuts`。

| 环节 | 做法 |
|---|---|
| 协议代码 | 协议 XML 取自 uwac，放在 `qfreerdp-master/protocols/`，由 `wayland-scanner` 生成 `client-header` + `private-code`，随 `qt_add_executable` 一起编译 |
| 取原生句柄 | 经 Qt 私有头 `qpa/qplatformnativeinterface.h`（CMake 目标 `Qt6::GuiPrivate`，Debian 包 `qt6-base-private-dev`），用 `nativeResourceForWindow("surface")` / `nativeResourceForIntegration("display")` 拿 `wl_surface` / `wl_display` |
| 绑定全局 | 监听 `wl_registry`，拿到 `zwp_keyboard_shortcuts_inhibit_manager_v1` 与 `wl_seat`；惰性取 `wl_display`，不做阻塞 roundtrip |
| 抑制时机 | `RdpViewItem::updateShortcutInhibit()` 在「已连接 + 窗口 active + 未被暂停」时请求抑制；窗口失焦自动解除 |
| 主动让出 | 按**右 Ctrl** 置 `m_inhibitSuspended` 并解除抑制，便于切回本地桌面；重新聚焦后自动复位 |
| 降级 | 非 Wayland 会话、或合成器未提供该协议时函数为 no-op，不影响其它功能 |

> 抑制生效后，`Super+R/E/D/A/I/M/X/S/V/G/P/W/T/B/U`、`Super+数字/Home/.`、`Alt+Tab`、`Alt+Shift+Tab`、`Ctrl+Esc`、`Ctrl+Shift+Esc` 都无需逐条登记 —— 合成器把整个窗口的快捷键都让出来了。这是与 Windows 版 `isInterceptedWinCombo()` 白名单机制最大的区别。

### 消除"是否允许抑制快捷键"授权弹窗

GNOME 对"窗口不是 window-backed"的应用会弹授权对话框，且**每次连接都弹**。授权按 app_id 记在 xdg-desktop-portal 的权限库里（表 `gnome` / 项 `shortcuts-inhibitor` / 值 `GRANTED`），前提是 app_id 能被 Shell 解析成一个已安装的 `.desktop` 文件：

| 坑 | 现象 |
|---|---|
| `Exec` 指向的程序不存在 | `Gio.DesktopAppInfo.new()` 返回 `NULL`，整个 desktop 文件被丢弃（`desktop-file-validate` 却会通过，非常隐蔽） |
| `NoDisplay=true` | `g_app_info_should_show()` 返回 false，应用不进 app system |
| app_id 与 `.desktop` 文件名不一致 | 授权记录对不上，仍然弹窗 |

对应处理：

1. `QGuiApplication::setDesktopFileName("qf-client")`（不带 `.desktop` 后缀，D-Bus 写入时再拼回）
2. 随包安装 `qf-client.desktop`：无 `NoDisplay`、`Exec` 指向真实存在的 `qf-client`、带 `StartupWMClass=qf-client`
3. 连接时再经 D-Bus 直接向 `org.freedesktop.impl.portal.PermissionStore` 写入 `GRANTED` 兜底，保证首次连接也不弹窗（非 GNOME 环境服务不存在，直接失败返回）

### 键码映射补充

- **小键盘按 keysym 判定**，不再依赖 NumLock：NumLock 关闭时 Qt 会把小键盘数字上报成方向键（keysym 变成 `KP_Left` 之类），只看 `key()` 会当成普通方向键、发出带扩展位的扫描码，远端永远只当方向键用；而本地与远端的 NumLock 各自独立翻转、相位差固定，按 NumLock 也对不上。现对齐 FreeRDP 官方 X11 客户端（`xf_keyboard.c`）做法，无条件映射到小键盘扫描码，由**远端** NumLock 决定输出数字还是方向键
- 补 `PrintScreen` → `RDP_SCANCODE_PRINTSCREEN`、`Menu` → `MAKE_RDP_SCANCODE(0x5D, TRUE)`（FreeRDP 3.28 未提供该常量）
- `Pause` 在 RDP 里是一串按键序列而非单个扫描码，走 FreeRDP 专用接口 `freerdp_input_send_keyboard_pause_event()`，仅在未按 Ctrl 时适用（`Ctrl+Pause` 即 Break，仍走普通扩展键路径）

### 与 Windows 版的差异

| 项 | Windows 版 | Linux 版 |
|---|---|---|
| 拦截机制 | `WH_KEYBOARD_LL` 低级钩子 + 逐条白名单 | 合成器快捷键抑制，整窗口让键、无需登记 |
| `Win+L` | 内核层截获，锁**本地** | 抑制生效后转发给远端（锁 VM 内的 Windows 会话） |
| `Ctrl+Alt+Del` | 用户态收不到，靠工具栏 CAD 按钮 | 同上，工具栏 CAD 按钮 |
| `Win+Tab` / `Win+方向键` | DWM 层处理，钩子收不到 | 抑制后可转发，比 Windows 版更完整 |
| `Ctrl+Alt+Enter`（本地全屏切换） | 钩子本地拦截 | 无此功能（窗口恒为全屏），由右 Ctrl 承担"切回本地" |

### 已知限制

- `Ctrl+Space` 可能仍被本机输入法（IBus/Fcitx）消费：输入法在 Wayland 下经 input-method 协议单独取键，快捷键抑制协议管不到
- 左右修饰键未区分（`Shift`/`Ctrl`/`Alt`/`Meta` 一律映射左侧），`AltGr` 场景可能不准

---

## 磁盘与 USB 重定向

策略与 Windows 版一致：**磁盘重定向为主、USB 透传为备选**，且两者互斥。

### 磁盘重定向（rdpdr）

VM 内的磁盘来自 FreeRDP `rdpdr` 通道。启用方式有三：CLI `/drives`（通配）、CLI `/drive:NAME,path`（显式）、`.rdp` 文件的 `drivestoredirect:s:...`。

`/drives` 走通配路径，由 rdpdr 自己枚举本机挂载点并做热插拔轮询：

| 环节 | 位置 | 说明 |
|---|---|---|
| 通配开启 | `freerdp_client_load_addins()` | `FreeRDP_RedirectDrives=TRUE` 时注册设备 `{"drive","media","*"}` |
| 通配解析 | `rdpdr_main.c` `rdpdr_add_devices()` | `Path=="*"` → `first_hotplug()`；`Path=="DynamicDrives"` 只起热插拔线程 |
| 纳入判定 | `is_redirectable_mount()` | 见下表 |
| 来源枚举 | `handle_platform_mounts_linux()` | `$HOME` + `/proc/mounts` + gvfs 子挂载 |
| 热插拔 | `drive_hotplug_thread_func()` | 每 1000ms 取一次快照，新增/移除自动同步到 VM |
| 盘名生成 | `hotplug_drive_name()` | 普通挂载点取路径末段；gvfs 目录名取首个 `key=value` 的 value |

**纳入判定**（`is_redirectable_mount()`，等价于 Windows 版的"驱动类型白名单"）：

| 类别 | 判定规则 | 举例 |
|---|---|---|
| 固定盘 → home | 单独取 `$HOME`。它通常是根文件系统上的普通目录，`/proc/mounts` 里看不到，必须单独加 | `/home/kk` → 盘名 `kk` |
| 移动介质 | 挂载源是块设备（`/dev/*`，排除 `/dev/fuse`、`/dev/loop*`）且落在 `/media`、`/run/media`、`/mnt` 下 | `/media/kk/D-LIVE 13_6` → 盘名 `D-LIVE 13_6` |
| 网络存储 NAS | fstype ∈ nfs/nfs2/nfs3/nfs4、cifs、smbfs、sshfs、fuse.sshfs、davfs、fuse.davfs、9p | `/mnt/nas` |
| gvfs 子挂载 | `/run/user/<uid>/gvfs/` 下**真实存在**的挂载点（GNOME 文件管理器访问 `smb://` 时挂到这里） | `.../smb-share:server=nas,share=data` → 盘名 `nas` |

以上一律附加**可读性过滤** `access(path, R_OK)`：`/boot/efi` 之类 root 专属挂载点即使命中规则也跳过，否则 VM 里只会多出一个打不开的盘符。

> **不重定向系统分区**。把 `/`、`/boot`、`/usr` 整个搬进虚拟机既危险也没有意义，因此判定是"白名单式收纳"而非"黑名单式排除"。
>
> **gvfs 根目录本身不是盘**。`/run/user/<uid>/gvfs` 是 `gvfsd-fuse` 提供的 FUSE 挂载点，平时是空目录且权限 `dr-x------`。旧实现按挂载点前缀白名单（`automountLocations[]`）匹配时，它恰好命中第一条，于是 VM 里凭空多出一个既看不到文件、也拷不进去的 "gvfs" 盘符；而反过来挂在 `/` 的内置盘因不在白名单而永远进不去。现在只取它下面真实存在的子挂载。

热插拔的增删两侧都已对齐：`hotplug_delete_foreach()` 改为**精确路径匹配**（顺带修掉上游 `strstr` 的子串误判，避免 `/media/a` 被 `/media/ab` 误保留）。

### USB 透传（urbdrc）

CLI `/usb:` 或 `.rdp` 的 `usbdevicestoredirect` 启用，默认关闭。

- **枚举与设备映射**：`USBManager` 用 libusb 列出设备；`usbAddressForBlockDevice()` 经 `/sys/class/block/<dev>` 软链接逐级向上找带 `idVendor` 的目录，读出 `(busnum, devnum)`，用于把某个挂载点关联回具体 USB 设备（对应 Windows 版 SetupAPI → `CM_Get_Parent()`）。`dm-*`、`nvme` 等非 USB 块设备回溯不到 `idVendor`，直接判定失败
- **热插拔**：Linux 下 `libusb_hotplug_register_callback()` 的 `ARRIVED | LEFT` 事件可用；Windows 版 libusb 返回 `LIBUSB_ERROR_NOT_SUPPORTED`，只能退回 `WM_DEVICECHANGE` + 400ms 去抖
- **选择语法**：`/usb:[dbg,][id:<vid>:<pid>#...,][addr:<bus>:<addr>#...,][auto]`。两条硬约束 —— `id:` 与 `addr:` 二选一；多设备必须写进**同一条**参数、用 `#` 分隔
- **写入通道**：必须先 `freerdp_client_del_dynamic_channel()` 再 `freerdp_client_add_dynamic_channel()`。上游对已存在的通道直接 `return TRUE` 且不追加参数，否则重连时新的设备选择会静默失效

### 两种重定向的互斥

已被磁盘重定向进 VM 的 U 盘，若再勾选 USB 透传，会与 VM 内的磁盘驱动争抢同一设备，因此：

- `USBManager::isRedirectedUsbMount()` 与 rdpdr 的 `is_redirectable_mount()` **共用同一套判定规则**（块设备 + 用户存储根 + 可读），否则会出现"UI 说已重定向、VM 里却没盘符"
- 命中的设备在 USB 弹窗中**置灰不可勾选**，并标注 `已磁盘重定向：<挂载点>`；含存储接口的复合设备额外提示 `⚠ 含存储接口 · 已挂载`
- 无盘符的设备（加密狗、手机、串口适配器等）不受影响，仍可走 USB 透传

### 与 Windows 版的差异

| 项 | Windows 版 | Linux 版 |
|---|---|---|
| 枚举单位 | 盘符（`GetLogicalDrives()` + `DRIVE_FIXED/REMOVABLE/CDROM/REMOTE`） | 挂载点（`/proc/mounts` + `$HOME` + gvfs 子目录） |
| 盘符→设备映射 | SetupAPI → `CM_Get_Parent()` 取 VID/PID | sysfs `/sys/class/block/<dev>` 回溯到带 `idVendor` 的目录 |
| 系统盘 | 含 `C:\`（不做收敛） | 不重定向 `/`、`/boot`、`/usr` |
| 设备热插拔 | `WM_DEVICECHANGE` + 400ms 去抖 | libusb `ARRIVED`/`LEFT` 事件；磁盘侧另有 rdpdr 1000ms 挂载点轮询 |
| 前置依赖 | — | `settings.c` 的 `freerdp_addin_argv_new()` 需跳过 `nullptr` 参数：通配 `/drives` 的 `argv[2]` 为 `nullptr`（表示 automount），原实现在此处崩溃 |

### 本机实算验证（模拟 FreeRDP 判定逻辑）

```
[HOME]   /home/kk                           -> 盘名 kk
[可移动] /media/kk/D-LIVE 13_6              -> 盘名 D-LIVE 13_6
[gvfs]   /run/user/1000/gvfs/smb-share:server=debian-2.local,share=smbshare
                                            -> 盘名 debian-2.local
共 3 个
```

`/`、`/boot/efi`、gvfs 空根目录、所有 `/dev/fuse` 的 cliprdr 临时挂载、tmpfs 全部被排除。

### 待真机验证

1. VM 内出现 `kk`(home) / `D-LIVE 13_6`(U盘) / NAS 共享盘
2. **不再出现** `gvfs` 空盘符，也没有 `root` / 整个根分区
3. 插拔 U 盘、打开/关闭 GNOME smb 共享，VM 内盘符 1 秒内自动增减
4. USB 弹窗里已挂载的 U 盘仍置灰，无盘符设备（加密狗、手机）仍可勾选走 USB 透传

---

## 摄像头重定向（rdpecam）

### 链路总览

```
V4L2 采集 (camera_v4l.c)  →  freerdp_video_sample_convert  →  rdpecam DVC  →  VM 内虚拟摄像头
   MJPG/YUY2 原始帧            MJPG/YUY2 → H264 (FFmpeg)         动态通道      SampleRequest 拉取模型
```

启用方式是**硬编码启用**，默认重定向全部 V4L2 设备（无运行时 UI 开关）：

```c
freerdp_client_add_dynamic_channel(settings, 2, {RDPECAM_DVC_CHANNEL_NAME, "device:*"});
```

协议为**服务端拉取模型**：服务端（VM 内摄像头驱动）依次发 `MediaTypeListRequest` → `CurrentMediaTypeRequest` → `StartStreamsRequest` → 持续 `SampleRequest`；客户端只做应答，不主动推流。

### 关键约束：Format 字段被单向改写

客户端采集侧的格式（MJPG / YUY2 / NV12 …）对服务端**不可见**。`ecam_dev_process_media_type_list_request()` 在回包前会把每个条目的 `Format` 统一改写为 `streamOutputFormat()`（本构型恒为 H264），并置 `CAM_MEDIA_TYPE_DESCRIPTION_FLAG_DecodingRequired`：

| 字段 | 上报前（客户端内部） | 上报后（服务端看到） |
|---|---|---|
| `Format` | 真实采集格式，如 `MJPG(2)` | `H264(1)` |
| `Flags` | `0` | `DecodingRequired` |
| `Width` / `Height` / `FrameRate*` | 原样 | 原样 |

服务端拿到的就是「H264 + 各种分辨率/帧率」，回传时也只有这些字段。因此**服务端选定某个条目后，只能靠 `(width,height,fps)` 反查**它原本的采集格式。

### 多格式上报（本次改造）

#### 上游行为与问题

上游 `cam_v4l_get_media_type_descriptions()` 遍历候选表 `getSupportedFormats()`（`available[]` 的 7×7 两两配对、按 `freerdp_video_conversion_supported()` 过滤，共 38 项；外层是输出格式、内层是输入格式），**命中第一个设备支持的输入格式就 `break`**，只上报该格式的尺寸；`mediaTypes[0]` 直接被当作默认值。两个后果：

- 候选表里 MJPG 排在第 5 位、YUY2 在它之前（且函数头注释声称的顺序 `H264, MJPG, I420, …` 与实际数组不符），于是内置/外接摄像头**都只上报未压缩 YUY2**
- 外接摄像头的 YUYV 最大支持到 1920x1080，`mediaTypes[0]` 因此是 `YUYV 1920x1080@10` 并成为默认值；而该模式实测只有 3~4 fps —— 这正是「外接摄像头画面卡顿」的根因

#### 改造内容

| 位置 | 改动 |
|---|---|
| `camera_device_main.c` `available[]` | 顺序修正为 `H264, MJPG, YUY2, NV12, I420, RGB24, RGB32`，与函数头注释声明的偏好顺序一致（**MJPG 提到 YUY2 之前是本改造的关键**） |
| `camera_v4l.c` 枚举循环 | 去掉 `break`，遍历所有设备支持的输入格式；同一输入格式只取候选表里**首次出现**的配对（外层是输出格式且 H264 是 `available[0]`，首次配对即 →H264），用 `seenFormat[]` 跳过后续重复配对 |
| `camera_v4l.c` 新增 `cam_v4l_media_type_duplicate()` | 按 `(width,height,fps)` 去重：MJPG 与 YUYV 共有的尺寸只保留先出现的（MJPG 在候选表里靠前，故保留 MJPG 那条） |
| 返回值 | 仍为候选表里首个命中的索引（`firstMatchedIndex`），供上层推导默认采集格式 |

#### 选定后的格式回映射

去重后，清单里同一 `(width,height)` 仍可能有多条（如 `1920x1080@30` 来自 MJPG、`1920x1080@10` 来自 YUYV），而服务端回传的 `Format` 已被改写成 H264，无法区分。因此在 `CameraDeviceStream` 中新增一份**采集格式快照**：

```c
/* 上报给服务端的媒体类型清单快照，Format 字段保留采集侧(输入)格式 */
CAM_MEDIA_TYPE_DESCRIPTION reportedMediaTypes[ECAM_MAX_MEDIA_TYPE_DESCRIPTORS]; /* 256 */
size_t nReportedMediaTypes;
```

`ecam_dev_process_media_type_list_request()` 负责填入快照；`StartStreamsRequest` 到达时由 `ecam_dev_apply_reported_media_type()` 还原：

1. 先按 `(width,height,fps)` 精确匹配快照
2. 匹配不到则退化为**同尺寸**匹配（取首条）
3. 都失败则保持原有采集格式不变
4. 命中后经 `ecam_dev_format_info()` 取出候选表里的 `(inputFormat, outputFormat)` 配对覆盖 `stream->formats` —— 这一步决定了 HAL 实际使用哪个 V4L2 像素格式

> `stream->formats` 也参与可丢帧判定：`mediaSupportDrops()` 仅对 `H264` 返回 FALSE。因此采集格式为 MJPG/YUY2 时，上一帧还没被服务端取走就允许被新帧覆盖，而 H264 采集会给服务端施加背压。

### 踩坑：`currMediaType` 必须与上报列表一致

`CurrentMediaTypeRequest` 的应答是把 `stream->currMediaType` **原样**发回服务端，而 `currMediaType` 的初始值取自 `mediaTypes[0]`。上游把它放在「改写 Format」循环**之后**赋值，因此带的是 `H264 + DecodingRequired`，与服务端手里的列表一致。

改造中一度把它提前到改写之前，于是服务端收到 `Format=MJPG(2)`、`Flags=0` —— 一个它列表里根本不存在的条目，匹配失败，**VM 内摄像头直接打不开**（现象：日志中 `MediaTypeListRequest` 之后再无任何请求，`StartStreams` 从未发生）。

修正后的顺序固定为：

```
存采集格式快照 → 由 mediaTypes[0] 推导 stream->formats
              → 改写 Format/Flags → 最后存 currMediaType
```

### 实测数据

摄像头均挂在 USB 2.0 Bus 上。同一份采集代码在不同强制模式下的真实出帧率：

| 强制模式 | 标称 | 实测 | 码流 |
|---|---|---|---|
| YUYV 1920x1080 | 10 fps | **3~4 fps** | 15 MB/s |
| YUYV 1280x720 | 15 fps | 10~11 fps | 18 MB/s |
| YUYV 640x360 | 30 fps | 30 fps | — |
| MJPG 1920x1080 | 30 fps | **30 fps** | 3.4 MB/s |

USB 2.0 等时带宽上限约 15~18 MB/s：未压缩 YUYV 1080p 无论标称多少都跑不满，而 MJPG 压缩后 1080p30 只需 3.4 MB/s。

改造后上报清单（首项即默认值）：

| 设备 | 上报清单 |
|---|---|
| 外接 `/dev/video4`（7 条） | `MJPG 1920x1080@30`(默认) · `MJPG 1280x720@30` · `MJPG 960x544@30` · `MJPG 800x480@30` · `MJPG 640x360@30` · `YUYV 1920x1080@10` · `YUYV 1280x720@15` |
| 内置 `/dev/video0`（3 条） | `MJPG 1280x720@30`(默认) · `MJPG 640x360@30` · `MJPG 640x480@30` |

对比改造前：外接 5 条（全是 YUYV，默认 `YUYV 1920x1080@10`，实跑 3~4 fps）→ 7 条，默认改为 `MJPG 1920x1080@30`，实跑 30 fps。

> MJPG 与 YUYV 共有的 960x544 / 800x480 / 640x360@30 只保留 MJPG 那条（MJPG 在 `available[]` 中靠前，先入列）。这也解释了 7 条 = 5 条 MJPG 独立尺寸 + 2 条 YUYV 独有的高分辨率。

### 参考：上游 Android HAL 的做法

`camera_android.c` 的 `cam_android_get_media_type_descriptions()` 是另一种取舍：只上报 `NV12` 一种格式，但会**过滤掉大于 1080p 的分辨率**（注释写明是为避免打满 RDP 链路），并给所有尺寸统一 30 fps。Linux V4L HAL 不做分辨率过滤，因为要尊重设备真实能力，帧率也从 `VIDIOC_ENUM_FRAMEINTERVALS` 实际读出（查询失败时才退回 30/1）。

### 排查线索

rdpecam 相关日志域名：`com.freerdp.channels.rdpecam-device.client`（设备/流/媒体类型协议层）、`rdpecam-v4l.client`（V4L2 采集层）、`rdpecam-uvch264.client`（UVC H264 直通）。默认被 `WLog_SetLogLevel(WLog_GetRoot(), WLOG_WARN)` 压到 WARN，排查时可在 `mini-qf-client.cc` 中临时把对应域名调到 `WLOG_INFO` / `WLOG_DEBUG`。

改动文件（均在 `freerdp-3.28.0/`，随 FreeRDP 一起重编译）：

- `channels/rdpecam/client/v4l/camera_v4l.c`
- `channels/rdpecam/client/camera_device_main.c`
- `channels/rdpecam/client/camera.h`

---

## 性能优化记录

### 已实施优化

| 优化 | 问题 | 解决方式 | 效果 |
|------|------|---------|------|
| **DMA-BUF → CPU Staging Buffer 迁移** | DMA-BUF + EGL/GBM 路径偶发 SIGSEGV (exit 11)，部分 GPU 驱动兼容性差 | 改用 `std::vector<uint8_t>` 作为 CPU 侧 staging buffer → `glTexSubImage2D` 上传到 GL 纹理。移除了 EGL Image / GBM BO / DMA-BUF FD 等复杂依赖 | ✅ 消除 EGL/GBM 相关崩溃 |
| **glViewport 显式设置** | 窗口缩放后 Qt 场景图缓存了旧的 glViewport，新区域黑屏 | `render()` 中 `glViewport(0,0,vpW,vpH)` 覆盖场景图缓存 | ✅ 黑屏消除 |
| **全帧上传替代脏矩形** | DMA-BUF 移除后，使用 dirty rect 上传时 stride 不匹配导致纹理错位 | 改用全帧 `glTexSubImage2D` 上传，避免 `GL_UNPACK_ROW_LENGTH` 跨 stride 复杂度 | ✅ 纹理正确 |
| **Fence wait 移到 FreeRDP 线程** | EGL fence 在 GUI 线程阻塞 16ms | 移到主循环 `freerdp_check_event_handles` 之前（DMA-BUF 时期）；**当前 CPU staging buffer 无需 fence** | ✅ 无需 fence 同步 |
| **FrameAcknowledge 调优** | 服务端等待确认产生 100ms+ 编码间隙 | 从 2 调到 8（后改回 2） | 测试证实 8 时 FPS 提升 |
| **剪贴板改为 CLI 控制** | 硬编码启用，无法通过 CLI 关闭 | CLI 解析前先设为 FALSE，由 `/clipboard` 决定；检测子选项自动启用 | ✅ 默认禁用，`/clipboard` 启用 |
| **重连时清理通道上下文** | `freerdp_disconnect` 释放 channel contexts 后指针悬空 | `my_pre_connect()` 中清理旧设备集合；`g_dispContext/g_gfxContext/g_clipboard_client_context` 置空 | ✅ 消除重连时残指针回调 |
| **悬浮工具栏延时显示** | 鼠标只要碰到屏幕顶部 5px 热区工具栏就立刻弹出，容易误触发 | 引入 `showDelayTimer`(1000ms)：进入热区先启动延时，到点后校验鼠标仍在热区内才显示；离开热区取消延时并启动 3s 隐藏定时器 | ✅ 误触发大幅降低 |
| **组合快捷键（修饰键重建 + 合成器快捷键抑制）** | Wayland 下 GNOME/KDE 在合成器层吃掉 Super 组合键与 Alt+Tab，`Win+R/E/D/A` 等无法进虚拟机 | P0 按 `QKeyEvent::modifiers()` 重建远端修饰键状态；P1 经 `zwp_keyboard_shortcuts_inhibit_v1` 让合成器把整窗口快捷键让给客户端，并用 D-Bus 预写授权消除弹窗 | ✅ `Win+R/E/D/A` 等实测可用，无需逐条白名单 |
| **VDIClient 启动路径泛化** | 硬编码了 `window-resize/` 中间路径 | 改为基于 `applicationDirPath()` 向上查找 `bin/` 目录 | ✅ 支持不同部署目录结构 |
| **初始分辨率使用屏幕尺寸** | QML 窗口还未 mapped 时 item size 很小，导致 RDP session 初始分辨率过小 | `start_rdp_connection()` 中用 `screen->availableGeometry()` 而非 item size | ✅ 初始 RDP session 为合理大分辨率 |
| **分辨率 4px 对齐** | RDP 服务器将请求分辨率对齐到 4 的倍数，不匹配导致黑边 | 请求前 `(w+3)&~3u` 预对齐 | ✅ 请求分辨率与服务器返回一致 |
| **片段着色器 Alpha 强制不透明** | GDI buffer 使用 BGRX32 格式（Alpha 为未初始化的填充字节），但 GL 纹理按 RGBA 上传，着色器直接使用随机 Alpha 值导致 Win7/Win10 拖动窗口时局部出现半透明方块 | 片段着色器中 `fragColor.a = 1.0` 强制 Alpha 不透明 | ✅ Win7 半透明方块消除 |
| **Staging Buffer Alpha 初始化** | `resizeStagingBuffer()` 中 `std::vector::resize()` 分配新内存但不初始化，Win10 GFX 管道增量更新导致未更新区域保留垃圾 Alpha 值 | 缓冲区分配后遍历所有 Alpha 字节（第 4 字节）设置为 `0xFF` | ✅ Win10 半透明方块消除 |
| **磁盘重定向改按「挂载源 + fstype + 挂载点」判定** | 旧实现按挂载点前缀白名单匹配：`/run/user/<uid>/gvfs` 空目录恰好命中第一条，VM 里凭空多出一个看不到文件、也拷不进去的 `gvfs` 盘符；而挂在 `/` 的内置盘因不在白名单永远进不去 | rdpdr 的 `is_redirectable_mount()` 改为白名单式收纳：`$HOME` + NAS（按 fstype）+ `/media`、`/run/media`、`/mnt` 下的块设备；统一附加 `access(R_OK)` 可读性过滤；gvfs 只取其下真实存在的子挂载。系统分区不再重定向 | ✅ VM 内只出现 home / U盘 / NAS 盘符，空的 `gvfs` 盘与 `root` 盘消失 |
| **挂载点热插拔增删与盘名生成** | 挂载点增减后 VM 内盘符不自动同步；gvfs 目录名形如 `smb-share:server=nas,share=data`，直接取末段作盘名不可读 | `handle_platform_mounts_linux()` 每次快照枚举三类来源；新增 `hotplug_drive_name()`，对 gvfs 目录名取首个 `key=value` 的 value 作盘名；`hotplug_delete_foreach()` 改为精确路径匹配 | ✅ 插拔 1 秒内自动增减，NAS 盘名可读；顺带修掉上游 `strstr` 子串误判 |
| **USB 与磁盘重定向互斥置灰** | 已通过磁盘重定向进 VM 的 U 盘若再走 USB 透传，会与 VM 内的磁盘驱动争抢同一设备 | `USBManager::isRedirectedUsbMount()` 与 rdpdr 共用同一套判定规则；QML 列表置灰不可勾选并标注 `已磁盘重定向：<挂载点>`，复合设备额外提示 | ✅ 两种重定向不冲突，UI 与 VM 实际盘符一致 |
| **USB 设备选择语法与通道写入修正** | 多设备需分别写 `/usb:` 参数；与磁盘重定向同时开启时新选择在重连后静默失效 | 按 urbdrc 语法合并为单条参数（`id:`/`addr:` 二选一、多设备 `#` 分隔）；写入前先 `freerdp_client_del_dynamic_channel()` 再 add（上游对已存在通道直接 `return TRUE` 且不追加参数） | ✅ 多设备一次透传，重连后选择仍生效 |
| **`settings.c` 支持 `nullptr` 参数（通配磁盘前置依赖）** | `freerdp_addin_argv_new()` 遇到 `argv[2] == nullptr` 时崩溃，导致 `/drives` 通配路径无法启用 | `freerdp_addin_argv_new()` 跳过 `nullptr`（`argv[2]==nullptr` 在 rdpdr 语义中表示 automount） | ✅ `/drives` 通配可正常开启 |
| **摄像头媒体类型多格式上报** | 上游 `cam_v4l_get_media_type_descriptions()` 命中第一个设备支持的输入格式就 `break`，且候选表里 MJPG 排在 YUY2 之后 → 只上报未压缩 YUY2；外接摄像头的默认值落在 `YUYV 1920x1080@10`，实测仅 3~4 fps（USB 2.0 等时带宽上限 15~18 MB/s），表现为画面卡顿 | `available[]` 把 MJPG 提到 YUY2 之前；枚举改为遍历全部设备支持的输入格式；新增 `cam_v4l_media_type_duplicate()` 按 `(width,height,fps)` 去重；`CameraDeviceStream` 增加上报快照，`StartStreamsRequest` 时按 `(width,height,fps)` 反查回真实采集格式 | ✅ 外接上报 5→7 条、内置 3 条，默认值变为 `MJPG 1920x1080@30`，实测 30 fps（3.4 MB/s） |
| **鼠标中键映射修复** | `mousePressEvent` / `mouseReleaseEvent` 用二元判断（`LeftButton` → `BUTTON1`，其余 → `BUTTON2`），`Qt::MiddleButton` 落进 else 分支被当作**右键**发给服务端 → 3D 设计软件里按住滚轮拖拽（平移/旋转视图）不生效，收到的是右键拖拽行为。另外 `mouseMoveEvent` 只发 `PTR_FLAGS_MOVE`，拖拽过程中不带按键状态 | 新增 `mouseButtonToPtrFlags()`（单键三分支映射到 `BUTTON1/3/2`）与 `mouseButtonsToPtrFlags()`（掩码按位合并）；press/release 改用前者；move/hover 改为 `PTR_FLAGS_MOVE \| <按住的键>`（move 事件里 `event->button()` 恒为 `NoButton`，必须用 `buttons()`；`QHoverEvent` 无 `buttons()`，用 `QGuiApplication::mouseButtons()`） | ✅ 中键拖拽在 VM 内正常触发 |

### 架构变更记录

| 时间 | 变更 | 原因 |
|------|------|------|
| 初始 | DMA-BUF 零拷贝 + EGL Image + GBM | 追求性能最优 |
| k1 | DMA-BUF + VAAPI 硬件解码 | 降低 CPU 负载 |
| k2 | 完整功能版 | 窗口缩放 glViewport 修复 |
| k3 | 移除 VAAPI | 部分 GPU 驱动不兼容 |
| k4 | 移除 DMA-BUF | EGL/GBM 偶发崩溃 |
| k5 | 窗口花屏优化 | 纹理上传错位修复 |
| k6 | 音频后端重构 | rdpsnd（播放）和 audin（麦克风）从 qf-client 内置后端改为 FreeRDP 库自带 PulseAudio 后端（通过 PipeWire）。移除 `rdpsnd-src/`（~2500 行）和 `audin-src/`（~500 行）自编译源码。 |
| k7 | 渲染透明度修复 | 修复 Win7/Win10 拖动窗口时局部出现半透明方块的问题（片段着色器强制 `fragColor.a=1.0` + staging buffer Alpha 初始化） |
| k8 | 组合快捷键：修饰键重建 + 合成器快捷键抑制 | Wayland 下合成器抢占 Super 组合键与 Alt+Tab；改用 `zwp_keyboard_shortcuts_inhibit_v1` 整窗口让键，替代"逐条白名单"方案。附：工具栏改为悬停 1s 延时显示，降低误触发；键码表补 PrintScreen/Pause/Menu，小键盘改按 keysym 判定以摆脱 NumLock 状态依赖 |
| k9 | 磁盘/USB 重定向与 Windows 版对齐 | 旧磁盘重定向按挂载点前缀白名单匹配，导致 VM 里出现空的 `gvfs` 盘符、而内置盘永远进不去。改为 home + NAS + 可移动介质的白名单式收纳，不再重定向系统分区；`USBManager` 与 rdpdr 共用判定规则并对已重定向设备置灰互斥；USB 选择改单条 `#` 分隔参数 + 通道先删后加；`settings.c` 补 `nullptr` 参数跳过以支持 `/drives` 通配 |
| k10 | 摄像头媒体类型多格式上报 + 采集格式回映射 | 外接 USB 摄像头被协商成未压缩 YUYV 1920x1080，实测仅 3~4 fps 导致画面卡顿。改为枚举全部输入格式、按 (width,height,fps) 去重、压缩格式优先，默认值变为 MJPG；新增上报清单快照，`StartStreamsRequest` 时按 (width,height,fps) 还原真实采集格式。附：`currMediaType` 必须取改写后的条目，否则 `CurrentMediaTypeResponse` 与服务端持有的列表不一致会导致摄像头打不开 |
| k11 | 鼠标中键映射修复 | `Qt::MiddleButton` 此前被误映射为右键（发出 `PTR_FLAGS_BUTTON2`），3D 设计软件里按住滚轮拖拽无法触发平移/旋转。改为左/中/右三分支映射到 `PTR_FLAGS_BUTTON1/3/2`；拖拽中的 move 事件改由 `event->buttons()` 带出按住的键，不再只发 `PTR_FLAGS_MOVE` |
| 当前 | CPU staging buffer + GL 纹理 + 全帧上传 | 稳定优先 |

### 已知瓶颈

| 瓶颈 | 说明 | 上限 |
|------|------|------|
| 服务端 H.264 编码速率 | 服务端发送 GFX 瓦片的速率 | ~50-60fps |
| invokeMethod 跨线程开销 | `QMetaObject::invokeMethod(QueuedConnection)` 投递延迟 | ~7ms/帧 |
| 场景图 vsync 同步 | Qt 场景图以显示器刷新率为节拍 | 60fps |
| **CPU 帧拷贝** | `copyFrameData()` memcpy 脏矩形 + `glTexSubImage2D` 全帧上传 | 较 DMA-BUF 零拷贝多 5-15% CPU 开销 |
| **软件解码** | VAAPI 关闭后 H.264 解码用 FFmpeg 软件实现 | 解码耗时从 ~2ms 升至 ~33ms |

### 性能测试数据

| 指标 | DMA-BUF + VAAPI 时期 | 当前 (CPU staging + 软件解码) |
|------|---------------------|----------------------------|
| H.264 解码耗时 | ~2ms (硬件) | ~33ms (软件) |
| 显示 FPS | 57fps | ~25fps |
| 丢帧率 >33ms | 1.7% | 高负载时上升 |
| CPU 使用率 | 低 (GPU 解码) | 中高 |
| 崩溃率 (SIGSEGV) | 偶发 (EGL/GBM) | 已验证多轮无崩溃 |

### 编译配置详情

FreeRDP 3.28.0 的完整 CMake 配置见 `freerdp-3.28.0/build/CMakeCache.txt`。选编编译选项：

```bash
cd /home/kk/vdi-client-linux-tmp-20260916/vdi-client-linux/freerdp-3.28.0

cmake -S . -B build \
  -DCMAKE_INSTALL_PREFIX="$PWD/install" \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DWITH_ALSA=ON \
  -DWITH_PULSE=ON \
  -DWITH_FFMPEG=ON \
  -DWITH_SWSCALE=ON \
  -DWITH_VAAPI=OFF \
  -DWITH_VAAPI_H264_ENCODING=OFF \
  -DWITH_X11=OFF \
  -DWITH_WAYLAND=OFF \
  -DWITH_SERVER=OFF \
  -DWITH_CLIENT=OFF \
  -DWITH_CCACHE=OFF \
  -DWITH_SAMPLE=OFF \
  -DWITH_KRB5=OFF \
  -DWITH_CUPS=OFF \
  -DWITH_FUSE=ON \
  -DWITH_OSS=OFF \
  -DWITH_PCSC=OFF \
  -DWITH_UNICODE_BUILTIN=ON \
  -DWITH_VERBOSE_WINPR_ASSERT=ON \
  -DCHANNEL_RDPECAM=ON \
  -DCHANNEL_RDPECAM_CLIENT=ON \
  -DCHANNEL_URBDRC=ON \
  -DCHANNEL_AINPUT=ON \
  -DCHANNEL_VIDEO=ON
```

## 参考
- [FreeRDP GitHub](https://github.com/FreeRDP/FreeRDP)
- [Microsoft Compare Remote Desktop clients](https://learn.microsoft.com/en-us/previous-versions/remote-desktop-client/compare-remote-desktop-clients?pivots=remote-pc)
