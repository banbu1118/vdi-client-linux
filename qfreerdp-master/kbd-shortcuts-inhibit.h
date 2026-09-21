#pragma once

class QWindow;

namespace qf {
namespace kbd {

/* Wayland 合成器快捷键抑制（zwp_keyboard_shortcuts_inhibit_v1）。
 *
 * 作用：窗口获得键盘焦点期间，请求合成器不再处理它自己的快捷键，这样被
 * GNOME/KDE 抢走的组合键（Super+A、Alt+Tab、Alt+F4 等）才能送到远端。
 * 非 Wayland 平台、或合成器未提供该协议时，函数为 no-op。
 *
 * 说明：抑制只在窗口处于聚焦状态时生效，失焦后合成器自动恢复自己的快捷键；
 * 用户也可通过系统提供的机制临时收回抑制，此时会收到 inactive 事件。 */
void setShortcutsInhibited(QWindow* window, bool inhibited);

}  // namespace kbd
}  // namespace qf
