# qfreerdp

A minimal Qt 6/QML + FreeRDP 3 remote desktop client, part of the VDI Client suite.

## Features

- RDP connection via FreeRDP 3.28 (libfreerdp)
- GLESv2 rendering via QSGRenderNode (CPU staging buffer → glTexSubImage2D)
- Full remote desktop display in Qt Quick window
- Dynamic resolution (disp DVC): window resize triggers RDP resolution change (300ms debounce, 4px aligned)
- Mouse, keyboard, and wheel event forwarding with coordinate mapping
- Full RDP pointer (cursor) support with auto-hide and restore
- Two-way clipboard: text / image (PNG/DIB) / files (via CLIPRDR channel, CLI-controlled)
- Drive redirection: `/drive:HOME,<path>`
- USB device redirection: `/usb:device:*` (URBDRC + libusb)
- Camera redirection: `/camera` (RDPECAM + V4L2)
- Microphone input: `/mic` (audin + PulseAudio via PipeWire)
- Audio output: PulseAudio via PipeWire (rdpsnd, FreeRDP library backend)
- RD Gateway support: `/g:host:port` with PAA token authentication
- Multiple monitor layouts: disp DVC dynamic resolution
- Auto-reconnect on disconnect (3 retries)
- Reconnect on USB device selection change
- Clipboard controlled by CLI (`/clipboard`), disabled by default

## Architecture

qf-client is the RDP rendering engine launched by [VDIClient](https://github.com/your-org/VDIClient):

```
VDIClient (Qt Widgets)          qf-client (QML + FreeRDP)
┌──────────────────────┐        ┌─────────────────────────┐
│ LoginWindow          │ fork   │ QGuiApplication + QML   │
│ - 登录/Token 管理    │──────→ │ - RdpViewItem 渲染      │
│ - VM 列表/状态监控   │ 启动   │ - FreeRDP 连接线程      │
│ - 启动 qf-client     │        │ - 剪贴板/USB/音频       │
│ - 心跳保活           │        │ - 动态分辨率 (disp DVC) │
└──────────────────────┘        └─────────────────────────┘
```

## Dependencies

- CMake 3.16+
- Clang/Clang++ (for qf-client)
- Qt 6: `Core`, `Gui`, `Qml`, `Quick` (dev packages)
- FreeRDP 3.28 (self-compiled from source, see `../freerdp-3.28.0/`)
- spdlog + fmt for structured logging
- GLESv2 for rendering
- libusb-1.0 for USB redirection (optional)
- libv4l for camera redirection (optional)
- PulseAudio for all audio (mic input + speaker output, via PipeWire)

The `cliprdr` client add-in implementation is not installed as a public development
interface, so a local copy in [ref-tmp](ref-tmp) is used for the clipboard channel.

## Build

```bash
cd /home/kk/vdi-client/VDIClient/qfreerdp-master

# Configure (uses clang, finds FreeRDP from ../freerdp-3.28.0/install/)
cmake -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -B build .

# Build
cmake --build build -j$(nproc)
```

The executable is generated at:

```bash
./build/qf-client
```

## Run (standalone)

```bash
./build/qf-client /v:192.168.1.90 /u:administrator /p:123456 /cert:ignore \
  /drive:HOME,/home/kk /clipboard /usb:device:* -a
```

Parameters use FreeRDP standard CLI syntax. See [.codex/安装教程.md](.codex/安装教程.md) for full reference.

## Related projects

- [VDIClient](https://github.com/your-org/VDIClient) — Qt Widgets login/VM manager that launches qf-client
- [FreeRDP](https://github.com/FreeRDP/FreeRDP) — the RDP library this client is built on

## Documentation

- [功能对照表](.codex/qt-qml-rdp-client-goal.md) — Detailed feature comparison with mstsc
- [安装教程](.codex/安装教程.md) — Full build and installation guide (Chinese)
