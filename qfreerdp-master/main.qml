import QtQuick
import QtQuick.Controls
import MyTestApp

Window {
    id: root
    visible: true
    title: qsTr("VDI Client")
    color: "black"
    minimumWidth: 650
    minimumHeight: 550

    // ========== 初始状态：全屏窗口 ==========
    visibility: Window.FullScreen

    // DEBUG: 追踪窗口宽高变化
    // onWidthChanged: console.log("[debug] root.width changed to", root.width)
    // onHeightChanged: console.log("[debug] root.height changed to", root.height)
    onVisibilityChanged: function(visibility) {
        // console.log("[debug] root.visibility changed to", root.visibility,
        //             "flags=", root.flags,
        //             "minW=", root.minimumWidth, "maxW=", root.maximumWidth,
        //             "minH=", root.minimumHeight, "maxH=", root.maximumHeight)

        // 从最大化(4)还原到普通窗口(2)时，Wayland 合成器可能锁定宽度
        if (visibility === Window.Windowed) {
            Qt.callLater(function() {
                root.width = root.width + 1
                Qt.callLater(function() {
                    root.width = root.width - 1
                })
            })
        }
    }

    property bool isFullscreen: true

    Component.onCompleted: {
        // 设置普通窗口默认尺寸（一次性赋值，非持久绑定，避免拖拽冲突）
        root.width = Screen.desktopAvailableWidth * 0.8
        root.height = Screen.desktopAvailableHeight * 0.8

        // 窗口创建后延迟到下一帧启动连接，确保 scene graph 已设置好 EGL surface
        // console.log("[qml] Component.onCompleted: window=" + root.width + "x" + root.height
        //             + " rdpItem=" + rdpItem.width + "x" + rdpItem.height
        //             + " screen=" + Screen.desktopAvailableWidth + "x" + Screen.desktopAvailableHeight)
        Qt.callLater(function() {
            // console.log("[qml] Qt.callLater: window=" + root.width + "x" + root.height
            //             + " rdpItem=" + rdpItem.width + "x" + rdpItem.height
            //             + " contentItem=" + root.contentItem.width + "x" + root.contentItem.height)
            rdpItem.startConnection()
        })
    }

    // DEBUG: 定期记录窗口尺寸（每秒）
    Timer {
        id: debugTimer
        interval: 1000
        repeat: true
        running: true
        onTriggered: { /* console.log suppressed */ }
    }

    // DEBUG: 窗口约束变化日志
    // onMinimumWidthChanged: console.log("[dbg/constraint] minimumWidth changed to", root.minimumWidth)
    // onMinimumHeightChanged: console.log("[dbg/constraint] minimumHeight changed to", root.minimumHeight)
    // onMaximumWidthChanged: console.log("[dbg/constraint] maximumWidth changed to", root.maximumWidth)
    // onMaximumHeightChanged: console.log("[dbg/constraint] maximumHeight changed to", root.maximumHeight)

    function toggleDisplayMode() {
        if (isFullscreen) {
            exitFullscreen()
        } else {
            enterFullscreen()
        }
    }

    function enterFullscreen() {
        isFullscreen = true
        root.showFullScreen()
    }

    function exitFullscreen() {
        isFullscreen = false
        // 直接进入普通大窗口，不经过最大化状态，完全避开 Wayland 宽度锁定 BUG
        root.showNormal()
        const w = Math.round(Screen.desktopAvailableWidth * 0.95)
        const h = Math.round(Screen.desktopAvailableHeight * 0.95)
        // console.log("[qml] exitFullscreen -> showNormal size =", w, "x", h)
        root.width = w
        root.height = h
    }

    // ========== RDP 渲染区域（始终填满窗口，与工具栏分离） ==========
    RdpViewItem {
        id: rdpItem
        objectName: "rdpViewItem"
        anchors.fill: parent
        focus: true
        fullscreen: root.isFullscreen
        onWidthChanged: {
            // console.log("[debug] rdpItem.width changed to", rdpItem.width)
            rdpItem.notifyWindowResized()
        }
        onHeightChanged: {
            // console.log("[debug] rdpItem.height changed to", rdpItem.height)
            rdpItem.notifyWindowResized()
        }
    }

    // ========== Overlay 悬浮工具栏（独立，不影响 RDP 渲染区域） ==========

    // 屏幕顶部 5px 热区 — 仅检测鼠标进入，不阻挡 RDP 交互
    // 宽度与工具栏对齐（居中），避免左上/右上角误触发
    MouseArea {
        id: topHotZone
        anchors.horizontalCenter: parent.horizontalCenter
        width: 380   // 比工具栏（340px）略宽，作为触发缓冲区
        anchors.top: parent.top
        height: 5
        hoverEnabled: true
        propagateComposedEvents: true
        z: toolbar.z + 1   // 确保热区永远能触发，即使工具栏已显示

        onContainsMouseChanged: {
            if (containsMouse) {
                hideDelayTimer.stop()
                toolbar.visible = true
                toolbar.y = toolbar.shownY
            } else {
                // 鼠标离开热区，启动隐藏定时器
                hideDelayTimer.restart()
            }
        }
        // 不消费鼠标事件，透传给 RDP
        onPressed: mouse.accepted = false
        onReleased: mouse.accepted = false
        onClicked: mouse.accepted = false
        onDoubleClicked: mouse.accepted = false
        onPressAndHold: mouse.accepted = false
        onWheel: wheel.accepted = false
    }

    // 悬浮工具栏 — 固定在屏幕顶部中央
    Rectangle {
        id: toolbar
        width: 340
        height: 36
        radius: 6
        anchors.horizontalCenter: parent.horizontalCenter
        y: hiddenY
        z: 100
        visible: false
        color: "#3d3d3d"
        opacity: visible ? 0.93 : 0

        property int hiddenY: -height - 4
        property int shownY: 6
        property bool pinned: false   // 固定显示模式标志

        Behavior on y {
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }

        // 工具栏内的鼠标检测（仅自动隐藏模式下生效）
        MouseArea {
            id: toolbarHover
            anchors.fill: parent
            hoverEnabled: true

            onContainsMouseChanged: {
                if (toolbar.pinned) return
                if (containsMouse) {
                    hideDelayTimer.stop()
                } else {
                    hideDelayTimer.restart()
                }
            }
        }

        // 工具栏标题
        Text {
            id: toolbarTitle
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: "VDI Client"
            color: "#d0d0d0"
            font.pixelSize: 12
        }

        // 右侧控制按钮
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            ToolButton {
                property bool pinState: toolbar.pinned
                highlighted: pinState
                onClicked: toolbar.pinned = !toolbar.pinned

                // 原生绘制的图钉图标
                Item {
                    anchors.centerIn: parent
                    width: 14; height: 16

                    // 钉子头部（圆形）
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        width: 8; height: 8; radius: 4
                        color: parent.parent.highlighted ? "#ffffff" : "#999999"
                    }
                    // 钉子身体（竖线）
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        anchors.topMargin: 4
                        width: 2.5; height: 10
                        radius: 1
                        color: parent.parent.highlighted ? "#ffffff" : "#999999"
                    }
                    // 钉子尖端（三角形）
                    Canvas {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        width: 6; height: 4
                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.fillStyle = parent.parent.highlighted ? "#ffffff" : "#999999"
                            ctx.beginPath()
                            ctx.moveTo(1.5, 0)
                            ctx.lineTo(4.5, 0)
                            ctx.lineTo(3, 4)
                            ctx.closePath()
                            ctx.fill()
                        }
                    }
                }
            }
            ToolButton { text: "CAD"; hint: "Ctrl+Alt+Del"; onClicked: rdpItem.sendCtrlAltDelete() }

            // USB 设备选择按钮
            ToolButton {
                id: usbBtn
                text: "USB"
                highlighted: usbManager.selectedCount() > 0
                onClicked: {
                    usbManager.enumerate()
                    usbWin.show()
                }
                // 小圆点指示选中状态
                Rectangle {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.topMargin: 2
                    anchors.rightMargin: 2
                    width: 6; height: 6; radius: 3
                    visible: usbManager.selectedCount() > 0
                    color: "#4CAF50"
                }
            }

            ToolButton { text: "–";  onClicked: root.visibility = Window.Minimized }
            ToolButton {
                onClicked: root.toggleDisplayMode()

                // 全屏切换图标
                Item {
                    anchors.centerIn: parent
                    width: 12; height: 10

                    // 退出全屏：两个偏移空心矩形（经典还原图标）
                    Loader {
                        anchors.fill: parent
                        active: root.isFullscreen
                        sourceComponent: Item {
                            anchors.fill: parent
                            Rectangle {
                                x: 0; y: 3
                                width: 9; height: 7
                                color: "transparent"
                                border.color: "#aaaaaa"
                                border.width: 1.2
                            }
                            Rectangle {
                                x: 3; y: 0
                                width: 9; height: 7
                                color: "#3d3d3d"
                                border.color: "#aaaaaa"
                                border.width: 1.2
                            }
                        }
                    }
                    // 进入全屏：单个空心矩形（经典最大化图标）
                    Loader {
                        anchors.fill: parent
                        active: !root.isFullscreen
                        sourceComponent: Rectangle {
                            anchors.centerIn: parent
                            width: 12; height: 10
                            color: "transparent"
                            border.color: "#aaaaaa"
                            border.width: 1.2
                        }
                    }
                }
            }
            ToolButton { text: "✕"; isClose: true; onClicked: Qt.quit() }
        }
    }

    // ========== USB 设备选择弹出菜单 ==========

    // USB 设备列表模型 (由 USBManager 动态填充)
    ListModel {
        id: usbListModel
        function refresh() {
            clear()
            var count = usbManager.deviceCount()
            for (var i = 0; i < count; i++) {
                append({
                    idx: i,
                    label: usbManager.deviceLabel(i),
                    checked: usbManager.isDeviceSelected(i),
                    stateVal: usbManager.deviceState(i),
                    errStr: usbManager.deviceError(i)
                })
            }
        }
    }

    // 监听设备列表变化 (热插拔/枚举完成后自动刷新)
    Connections {
        target: usbManager
        function onDeviceListChanged() {
            if (usbWin.visible)
                usbListModel.refresh()
        }
    }

    // USB 设备选择窗口 (独立窗口，不影响主窗口 RDP 渲染)
    Window {
        id: usbWin
        width: 420
        height: 420
        minimumWidth: 350
        minimumHeight: 250
        flags: Qt.Dialog | Qt.WindowCloseButtonHint
        title: "USB Devices"
        color: "#2d2d2d"
        modality: Qt.NonModal
        visible: false

        onVisibleChanged: {
            if (visible) usbListModel.refresh()
        }

        Column {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8

            // 标题行
            Row {
                width: parent.width
                spacing: 6
                Text {
                    text: "USB Devices"
                    color: "#e0e0e0"
                    font.pixelSize: 14
                    font.bold: true
                }
                Text {
                    text: usbManager.selectedCount() > 0 ? ("(" + usbManager.selectedCount() + " selected)") : ""
                    color: "#4CAF50"
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // 分隔线
            Rectangle {
                width: parent.width; height: 1; color: "#444"
            }

            // 设备列表
            ListView {
                id: usbListView
                width: parent.width
                height: Math.min(260, usbListModel.count * 42)
                model: usbListModel
                clip: true
                visible: usbListModel.count > 0

                delegate: Rectangle {
                    width: usbListView.width
                    height: 38
                    color: itemMouse.containsMouse ? "#3a3a3a" : "transparent"
                    radius: 4

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 18; height: 18; radius: 3
                            border.color: model.checked ? "#4CAF50" : "#888"
                            border.width: 1
                            color: model.checked ? "#4CAF50" : "transparent"

                            Text {
                                anchors.centerIn: parent
                                text: "✓"
                                color: "white"
                                font.pixelSize: 11
                                visible: model.checked
                            }

                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    var newVal = !model.checked
                                    usbManager.setDeviceSelected(model.idx, newVal)
                                    model.checked = newVal
                                }
                            }
                        }

                        Text {
                            text: model.label
                            color: "#e0e0e0"
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            width: 280
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: model.stateVal === 0 ? "" :
                                  model.stateVal === 1 ? "⟳" :
                                  model.stateVal === 2 ? "✓" : "✗"
                            color: model.stateVal === 2 ? "#4CAF50" :
                                   model.stateVal === 3 ? "#F44336" : "#999"
                            font.pixelSize: 13
                            visible: model.stateVal !== 0
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 18
                        color: "#e53935"
                        radius: 2
                        visible: itemMouse.containsMouse && model.errStr !== ""

                        Text {
                            anchors.fill: parent
                            anchors.leftMargin: 4
                            text: model.errStr
                            color: "white"
                            font.pixelSize: 10
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                        }
                    }

                    MouseArea {
                        id: itemMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            var newVal = !model.checked
                            usbManager.setDeviceSelected(model.idx, newVal)
                            model.checked = newVal
                        }
                    }
                }
            }

            // 空列表提示
            Text {
                width: parent.width
                height: 40
                text: "No USB devices found"
                color: "#888"
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                visible: usbListModel.count === 0
            }

            // 分隔线
            Rectangle {
                width: parent.width; height: 1; color: "#444"
                visible: usbListModel.count > 0
            }

            // 底部操作按钮
            Row {
                spacing: 12
                anchors.horizontalCenter: parent.horizontalCenter

                Rectangle {
                    width: 90; height: 28; radius: 4
                    color: usbManager.selectedCount() > 0 ? "#1a5fb4" : "#555"
                    Text {
                        anchors.centerIn: parent
                        text: "Confirm"
                        color: "white"
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            usbManager.applySelection()
                            usbWin.close()
                        }
                    }
                }

                Rectangle {
                    width: 90; height: 28; radius: 4
                    color: "#555"
                    Text {
                        anchors.centerIn: parent
                        text: "Refresh"
                        color: "white"
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: usbManager.enumerate()
                    }
                }

                Rectangle {
                    width: 60; height: 28; radius: 4
                    color: "#555"
                    Text {
                        anchors.centerIn: parent
                        text: "Cancel"
                        color: "#ccc"
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: usbWin.close()
                    }
                }
            }
        }
    }

    // ========== 延时隐藏定时器 ==========
    Timer {
        id: hideDelayTimer
        interval: 3000                         // 鼠标离开 3 秒后自动隐藏
        onTriggered: {
            if (toolbar.pinned) return
            toolbar.y = toolbar.hiddenY
            toolbar.visible = false
        }
    }

    // ========== 工具栏按钮组件 ==========
    component ToolButton: Rectangle {
        property string text: ""
        property string hint: ""
        property bool isClose: false
        property bool highlighted: false
        signal clicked()

        width: 32; height: 26; radius: 4
        color: highlighted ? "#1a5fb4" : (btnMouse.containsMouse ? (isClose ? "#e81123" : "#5a5a5a") : "transparent")

        Text {
            anchors.centerIn: parent
            text: parent.text
            color: "white"
            font.pixelSize: 13
        }
        MouseArea {
            id: btnMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: parent.clicked()
        }
    }
}
