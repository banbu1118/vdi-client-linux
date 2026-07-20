#!/usr/bin/env bash
# ===========================================================================
# VDI Client DEB 包构建脚本
# 用法: sudo ./build-deb.sh [版本号]
# 默认版本: 1.0.0
# ===========================================================================
set -euo pipefail

VERSION="${1:-1.5.0}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BASE_DIR="$(realpath "$SCRIPT_DIR/..")"
PARENT_DIR="$(realpath "$BASE_DIR/..")"
FREERDP_DIR="$(realpath "$PARENT_DIR/freerdp-3.28.0/install")"
QF_DIR="$(realpath "$PARENT_DIR/qfreerdp-master")"
BUILD_DIR="$BASE_DIR/build"

PKG_NAME="VDIClient"
PKG_DIR="/tmp/${PKG_NAME}_${VERSION}_amd64"
ARCH="amd64"

echo "=== 构建 VDI Client DEB 包 v${VERSION} ==="

# ---- 0. 清理 ----
rm -rf "$PKG_DIR"

# ---- 1. 创建目录结构 ----
mkdir -p "$PKG_DIR/DEBIAN"
mkdir -p "$PKG_DIR/usr/bin"
mkdir -p "$PKG_DIR/usr/lib/vdi-client"
mkdir -p "$PKG_DIR/usr/share/vdi-client/MyTestApp"
mkdir -p "$PKG_DIR/usr/share/icons/hicolor/256x256/apps"
mkdir -p "$PKG_DIR/usr/share/applications"
mkdir -p "$PKG_DIR/etc/udev/rules.d"

# ---- 2. control 文件 ----
cat > "$PKG_DIR/DEBIAN/control" << EOF
Package: ${PKG_NAME}
Version: ${VERSION}
Section: net
Priority: optional
Architecture: ${ARCH}
Depends: qml6-module-qtquick-controls,
         libspdlog1.15,
         libusb-1.0-0,
         libpulse0,
         libasound2t64,
         libgl1,
         libgles2,
         libva2,
         libvdpau1,
         libfuse3-4,
         libavcodec61,
         libavutil59,
         libswscale8,
         libswresample5
Maintainer: VDI Client Team <vdi@example.com>
Description: VDI Remote Desktop Client
 A Qt6/QML based remote desktop client using FreeRDP 3.28,
 part of the VDI Client suite. Provides RDP connectivity
 with USB redirection, clipboard, audio, and camera support.
EOF

# ---- 3. postinst 脚本（安装后配置） ----
cat > "$PKG_DIR/DEBIAN/postinst" << 'POSTINST'
#!/bin/sh
set -e

case "$1" in
    configure)
        # USB 权限：确保 libusb 可以访问 USB 设备
        if [ ! -f /etc/udev/rules.d/99-vdi-client-usb.rules ]; then
            echo 'SUBSYSTEM=="usb", GROUP="plugdev", MODE="0660"' > /etc/udev/rules.d/99-vdi-client-usb.rules
        fi

        # V4L 摄像头权限：确保 /dev/video* 可访问（摄像头重定向）
        # 使用 MODE="0666" 使所有本地用户可直接访问摄像头，无需额外加入 video 组
        if [ ! -f /etc/udev/rules.d/99-vdi-client-camera.rules ]; then
            echo 'SUBSYSTEM=="video4linux", MODE="0666"' > /etc/udev/rules.d/99-vdi-client-camera.rules
        fi

        # 重载 udev 规则
        udevadm control --reload-rules 2>/dev/null || true
        udevadm trigger 2>/dev/null || true
        ;;
esac
exit 0
POSTINST
chmod 755 "$PKG_DIR/DEBIAN/postinst"

# ---- 4. prerm 脚本（卸载前清理） ----
cat > "$PKG_DIR/DEBIAN/prerm" << 'PRERM'
#!/bin/sh
set -e

case "$1" in
    remove|purge)
        rm -f /etc/udev/rules.d/99-vdi-client-usb.rules
        rm -f /etc/udev/rules.d/99-vdi-client-camera.rules
        udevadm control --reload-rules 2>/dev/null || true
        ;;
esac
exit 0
PRERM
chmod 755 "$PKG_DIR/DEBIAN/prerm"

# ---- 5. 复制可执行文件 ----
echo "[1/4] 复制可执行文件..."
cp -a "$BUILD_DIR/VDIClient" "$PKG_DIR/usr/bin/vdi-client"
cp -a "$QF_DIR/build/qf-client" "$PKG_DIR/usr/bin/qf-client"

# ---- 6. 复制 QML 运行时文件 ----
echo "[2/4] 复制 QML 运行时文件..."
cp -a "$QF_DIR/build/MyTestApp/" "$PKG_DIR/usr/share/vdi-client/MyTestApp/"

# ---- 7. 复制 FreeRDP 自编译库 ----
echo "[3/4] 复制 FreeRDP 库..."
for lib in libfreerdp3.so.3.28.0 libfreerdp-client3.so.3.28.0 libwinpr3.so.3.28.0; do
    cp -a "$FREERDP_DIR/lib/$lib" "$PKG_DIR/usr/lib/vdi-client/$lib"
done
ln -sf libfreerdp3.so.3.28.0          "$PKG_DIR/usr/lib/vdi-client/libfreerdp3.so.3"
ln -sf libfreerdp-client3.so.3.28.0   "$PKG_DIR/usr/lib/vdi-client/libfreerdp-client3.so.3"
ln -sf libwinpr3.so.3.28.0            "$PKG_DIR/usr/lib/vdi-client/libwinpr3.so.3"
ln -sf libfreerdp3.so.3               "$PKG_DIR/usr/lib/vdi-client/libfreerdp3.so"
ln -sf libfreerdp-client3.so.3        "$PKG_DIR/usr/lib/vdi-client/libfreerdp-client3.so"
ln -sf libwinpr3.so.3                 "$PKG_DIR/usr/lib/vdi-client/libwinpr3.so"

# ---- 8. 图标 ----
echo "[4/4] 复制图标..."
if [ -f "$BASE_DIR/resources/logo.png" ]; then
    cp -a "$BASE_DIR/resources/logo.png" "$PKG_DIR/usr/share/icons/hicolor/256x256/apps/vdi-client.png"
    cp -a "$BASE_DIR/resources/logo.png" "$PKG_DIR/usr/share/vdi-client/logo.png"
fi

# ---- 10. desktop 文件（应用菜单入口） ----
cat > "$PKG_DIR/usr/share/applications/vdi-client.desktop" << DESKTOP
[Desktop Entry]
Name=VDI Client
Comment=VDI Remote Desktop Client
Exec=/usr/bin/vdi-client
Icon=vdi-client
Terminal=false
Type=Application
Categories=Network;RemoteAccess;
DESKTOP

# ---- 9. 修改 RUNPATH 并构建 .deb ----
echo ""
echo "=== 修改库搜索路径 ==="
# qf-client 的 RUNPATH 指向开发路径，打包后需要改为 \$ORIGIN/../lib
# 但 deb 安装后库在 /usr/lib/vdi-client/，可执行文件在 /usr/bin/
# 所以需要设置 LD_LIBRARY_PATH 或修改 RUNPATH
# 方案：用 chrpath 修改 qf-client 的 RUNPATH
if command -v chrpath &>/dev/null; then
    chrpath -r '$ORIGIN/../lib/vdi-client' "$PKG_DIR/usr/bin/qf-client" 2>/dev/null || true
    chrpath -r '$ORIGIN/../lib/vdi-client' "$PKG_DIR/usr/bin/vdi-client" 2>/dev/null || true
fi

# 创建启动器包装脚本（设置 LD_LIBRARY_PATH）
mv "$PKG_DIR/usr/bin/vdi-client" "$PKG_DIR/usr/bin/vdi-client.bin"
cat > "$PKG_DIR/usr/bin/vdi-client" << WRAPPER
#!/bin/sh
export LD_LIBRARY_PATH="/usr/lib/vdi-client\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
exec /usr/bin/vdi-client.bin "\$@"
WRAPPER
chmod +x "$PKG_DIR/usr/bin/vdi-client"

# 同样的包装脚本给 qf-client
mv "$PKG_DIR/usr/bin/qf-client" "$PKG_DIR/usr/bin/qf-client.bin"
cat > "$PKG_DIR/usr/bin/qf-client" << WRAPPER
#!/bin/sh
export LD_LIBRARY_PATH="/usr/lib/vdi-client\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
exec /usr/bin/qf-client.bin "\$@"
WRAPPER
chmod +x "$PKG_DIR/usr/bin/qf-client"

# ---- 10. 构建 .deb ----
echo ""
echo "=== 构建 DEB 包 ==="
dpkg-deb --build "$PKG_DIR" "$BASE_DIR/${PKG_NAME}_${VERSION}_${ARCH}.deb"

echo ""
echo "完成: $BASE_DIR/${PKG_NAME}_${VERSION}_${ARCH}.deb"
ls -lh "$BASE_DIR/${PKG_NAME}_${VERSION}_${ARCH}.deb"

echo ""
echo "=== 安装方法 ==="
echo "  sudo dpkg -i ${PKG_NAME}_${VERSION}_${ARCH}.deb"
echo "  sudo apt install -f  # 补齐依赖"
echo ""
echo "=== 卸载方法 ==="
echo "  sudo dpkg -r ${PKG_NAME}"
