#include "kbd-shortcuts-inhibit.h"

#include "qf_log.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QGuiApplication>
#include <QPointer>
#include <QStringList>
#include <QWindow>
#include <qpa/qplatformnativeinterface.h>

#include <wayland-client.h>

#include "protocols/keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cstring>

namespace qf {
namespace kbd {
namespace {

struct SeatEntry
{
    uint32_t globalName = 0;
    wl_seat* seat = nullptr;
    uint32_t capabilities = 0;
};

constexpr int kMaxSeats = 4;

/* 全部为 Qt 主线程访问，无需加锁 */
wl_display*  s_display = nullptr;
wl_registry* s_registry = nullptr;
zwp_keyboard_shortcuts_inhibit_manager_v1* s_manager = nullptr;
zwp_keyboard_shortcuts_inhibitor_v1*       s_inhibitor = nullptr;
uint32_t     s_managerName = 0;
SeatEntry    s_seats[kMaxSeats];
int          s_seatCount = 0;
QPointer<QWindow> s_window;
bool         s_wantInhibit = false;

void tryCreateInhibitor();

wl_seat* seatWithKeyboard()
{
    for (int i = 0; i < s_seatCount; ++i) {
        if (s_seats[i].seat && (s_seats[i].capabilities & WL_SEAT_CAPABILITY_KEYBOARD))
            return s_seats[i].seat;
    }
    return nullptr;
}

/* ---- zwp_keyboard_shortcuts_inhibitor_v1 事件 ---- */
void onInhibitorActive(void*, zwp_keyboard_shortcuts_inhibitor_v1*)
{
    qf::log::info("input/kbd", "快捷键抑制已生效（合成器快捷键已停用）");
}

void onInhibitorInactive(void*, zwp_keyboard_shortcuts_inhibitor_v1*)
{
    qf::log::warn("input/kbd", "快捷键抑制被合成器收回（Alt+Tab/Super 等仍会被本地桌面接管）");
}

const zwp_keyboard_shortcuts_inhibitor_v1_listener kInhibitorListener = {
    &onInhibitorActive,
    &onInhibitorInactive
};

/* ---- wl_seat：只需要 capabilities 来判断哪个 seat 带键盘 ---- */
void onSeatCapabilities(void* data, wl_seat*, uint32_t capabilities)
{
    auto* entry = static_cast<SeatEntry*>(data);
    entry->capabilities = capabilities;
    tryCreateInhibitor();
}

void onSeatName(void*, wl_seat*, const char*)
{
}

const wl_seat_listener kSeatListener = {
    &onSeatCapabilities,
    &onSeatName
};

/* ---- wl_registry ---- */
void onRegistryGlobal(void*, wl_registry* registry, uint32_t name,
                      const char* interface, uint32_t version)
{
    if (std::strcmp(interface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) == 0) {
        if (!s_manager) {
            s_manager = static_cast<zwp_keyboard_shortcuts_inhibit_manager_v1*>(
                wl_registry_bind(registry, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1));
            s_managerName = name;
            qf::log::info("input/kbd", "合成器提供 zwp_keyboard_shortcuts_inhibit_v1");
        }
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0 && s_seatCount < kMaxSeats) {
        SeatEntry& entry = s_seats[s_seatCount++];
        entry.globalName = name;
        entry.seat = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, std::min<uint32_t>(version, 5u)));
        wl_seat_add_listener(entry.seat, &kSeatListener, &entry);
    }

    tryCreateInhibitor();
}

void onRegistryGlobalRemove(void*, wl_registry*, uint32_t name)
{
    /* 座位被移除时清掉存根，避免后续用悬空指针请求抑制 */
    for (int i = 0; i < s_seatCount; ++i) {
        if (s_seats[i].globalName != name)
            continue;
        wl_seat_destroy(s_seats[i].seat);
        s_seats[i] = s_seats[--s_seatCount];
        break;
    }

    if (s_manager && name == s_managerName) {
        if (s_inhibitor) {
            zwp_keyboard_shortcuts_inhibitor_v1_destroy(s_inhibitor);
            s_inhibitor = nullptr;
        }
        zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(s_manager);
        s_manager = nullptr;
        s_managerName = 0;
        qf::log::warn("input/kbd", "合成器撤回了 zwp_keyboard_shortcuts_inhibit_v1");
    }
}

const wl_registry_listener kRegistryListener = {
    &onRegistryGlobal,
    &onRegistryGlobalRemove
};

void* queryNativeResource(const char* resource, QWindow* window)
{
    auto* qpa = QGuiApplication::platformNativeInterface();
    if (!qpa)
        return nullptr;
    if (void* value = qpa->nativeResourceForIntegration(resource))
        return value;
    return window ? qpa->nativeResourceForWindow(resource, window) : nullptr;
}

/* 协议对象齐备且窗口要求抑制时，创建 inhibitor（否则等 registry/seat 事件到达后再调） */
void tryCreateInhibitor()
{
    if (!s_wantInhibit || s_inhibitor || !s_manager || !s_display)
        return;

    QWindow* window = s_window.data();
    if (!window)
        return;

    wl_seat* seat = seatWithKeyboard();
    if (!seat)
        return;

    auto* surface = static_cast<wl_surface*>(queryNativeResource("surface", window));
    if (!surface)
        surface = static_cast<wl_surface*>(queryNativeResource("wl_surface", window));
    if (!surface) {
        qf::log::warn("input/kbd", "拿不到 wl_surface，跳过快捷键抑制");
        return;
    }

    s_inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(s_manager, surface, seat);
    if (!s_inhibitor)
        return;

    zwp_keyboard_shortcuts_inhibitor_v1_add_listener(s_inhibitor, &kInhibitorListener, nullptr);
    wl_display_flush(s_display);
    qf::log::info("input/kbd", "已请求抑制合成器快捷键");
}

/* 惰性建立 registry 连接；事件交给 Qt 的事件循环派发，不做阻塞 roundtrip */
bool ensureBindings(QWindow* window)
{
    if (s_display && s_registry)
        return true;

    void* display = queryNativeResource("display", window);
    if (!display)
        display = queryNativeResource("wl_display", window);
    if (!display) {
        qf::log::warn("input/kbd", "拿不到 wl_display，跳过快捷键抑制");
        return false;
    }

    s_display = static_cast<wl_display*>(display);
    s_registry = wl_display_get_registry(s_display);
    if (!s_registry) {
        s_display = nullptr;
        return false;
    }

    wl_registry_add_listener(s_registry, &kRegistryListener, nullptr);
    wl_display_flush(s_display);
    return true;
}

/* GNOME 收到抑制请求且该应用未授权时，会弹"是否允许抑制快捷键"；
 * 授权按 app_id 记在 xdg-desktop-portal 的权限库里（表 gnome / 项 shortcuts-inhibitor），
 * 而 app_id 必须能被 Shell 解析成已安装的 .desktop 文件，否则授权根本不会生效。
 * VDI 客户端每次连接都弹框很打扰，这里在首次请求抑制前直接写入 GRANTED，
 * 等价于用户点过一次"允许"。想恢复系统默认的"首次询问"，删掉本函数调用即可。 */
void ensureShortcutsInhibitGranted()
{
    static bool s_attempted = false;
    if (s_attempted)
        return;
    s_attempted = true;

    const QString appId = QGuiApplication::desktopFileName();
    if (appId.isEmpty())
        return;

    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.impl.portal.PermissionStore"),
        QStringLiteral("/org/freedesktop/impl/portal/PermissionStore"),
        QStringLiteral("org.freedesktop.impl.portal.PermissionStore"),
        QStringLiteral("SetPermission"));
    message << QStringLiteral("gnome")
            << true
            << QStringLiteral("shortcuts-inhibitor")
            << (appId + QStringLiteral(".desktop"))
            << QStringList{QStringLiteral("GRANTED")};

    /* 同步调用，保证授权先于抑制请求落到权限库；非 GNOME 环境服务不存在，直接失败返回 */
    QDBusConnection::sessionBus().call(message, QDBus::Block, 2000);
}

}  // namespace

void setShortcutsInhibited(QWindow* window, bool inhibited)
{
    if (QGuiApplication::platformName() != QLatin1String("wayland"))
        return;  /* X11 走键盘抓取（QWindow::setKeyboardGrabEnabled） */

    if (!inhibited) {
        s_wantInhibit = false;
        s_window = nullptr;
        if (s_inhibitor) {
            zwp_keyboard_shortcuts_inhibitor_v1_destroy(s_inhibitor);
            s_inhibitor = nullptr;
            qf::log::info("input/kbd", "已解除快捷键抑制");
        }
        if (s_display)
            wl_display_flush(s_display);
        return;
    }

    if (!window)
        return;

    s_window = window;
    s_wantInhibit = true;
    ensureShortcutsInhibitGranted();
    if (ensureBindings(window))
        tryCreateInhibitor();
}

}  // namespace kbd
}  // namespace qf
