#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QMutex>
#include <atomic>
#include <cstdint>
#include <thread>
#include <memory>
#include <vector>
#include <libusb-1.0/libusb.h>

class USBManager : public QObject
{
	Q_OBJECT
public:
	explicit USBManager(QObject* parent = nullptr);
	~USBManager() override;

	// === QML-invokable interface ===
	Q_INVOKABLE void enumerate();
	Q_INVOKABLE int deviceCount() const;
	Q_INVOKABLE QString deviceLabel(int index) const;
	Q_INVOKABLE QString deviceVidPid(int index) const;
	Q_INVOKABLE int deviceState(int index) const;
	Q_INVOKABLE QString deviceError(int index) const;
	Q_INVOKABLE bool isDeviceSelected(int index) const;
	Q_INVOKABLE void setDeviceSelected(int index, bool selected);
	Q_INVOKABLE void clearSelection();
	Q_INVOKABLE int selectedCount() const;
	Q_INVOKABLE void applySelection();

	// === 磁盘重定向相关的只读状态（供 QML 置灰与标注） ===
	// 该设备已被磁盘重定向（按挂载点判定），不应再走 USB 透传
	Q_INVOKABLE bool isDiskRedirected(int index) const;
	// 已挂载点，逗号分隔（如 "/media/kk/U盘"）；无挂载返回空串
	Q_INVOKABLE QString deviceMountPoints(int index) const;
	// 除存储接口外还有其它接口（厂商自定义/HID/Audio 等），即复合设备
	Q_INVOKABLE bool isStorageComposite(int index) const;
	// 重新读取本机挂载表（挂载/卸载不会触发 USB 热插拔事件，需要单独刷新）
	Q_INVOKABLE void refreshDiskState();

	// === Internal API (called from C++ connection code) ===
	// 本次待透传的设备。同一 VID:PID 出现多支时，urdbrc 的 id: 无法区分，
	// 整次连接改按 bus/addr 匹配（见 selectionNeedsAddr()）。
	struct SelectedDevice
	{
		uint16_t vid = 0, pid = 0;
		uint8_t bus = 0, addr = 0;
	};
	std::vector<SelectedDevice> selectedDevices() const;
	bool selectionNeedsAddr() const;

	// 磁盘重定向状态：wildcard 为真表示所有落在自动挂载点下的卷都已被重定向；
	// 否则 paths 为显式重定向的路径集合
	void setDiskRedirectState(bool wildcard, const QStringList& paths);

	// Mark a device as redirected (by VID:PID)
	void markRedirected(uint16_t vid, uint16_t pid, bool success,
	                    const std::string& error = {});

signals:
	void deviceListChanged(); // QML re-builds its list model
	void reconnectRequested(); // C++ triggers reconnect

private slots:
	void onHotplugEvent();

private:
	struct DeviceInfo
	{
		uint16_t vid = 0, pid = 0;
		uint8_t bus = 0, addr = 0;
		std::string manufacturer;
		std::string product;
		std::string serial;

		// 接口描述符分析结果，用于过滤与"磁盘重定向置灰"判定
		bool hasStorageInterface = false;    // 含 Mass Storage(0x08)
		bool hasNonStorageInterface = false; // 含非存储接口
		std::string mountPoints;             // 已挂载点，逗号分隔（展示用）

		enum State : int { Idle = 0, Redirecting = 1, Redirected = 2, Failed = 3 };
		State state = Idle;
		std::string error;
	};

	// 选中项：总是记录 bus/addr，避免设备重插后按 id 误匹配
	struct SelectionKey
	{
		uint16_t vid = 0, pid = 0;
		uint8_t bus = 0, addr = 0;
	};

	static int LIBUSB_CALL hotplugCallback(libusb_context* ctx, libusb_device* dev,
	                                       libusb_hotplug_event event, void* userdata);
	void enumerateInternal();
	void startHotplugThread();
	void stopHotplugThread();
	// 返回是否应出现在列表里，并填写接口类别分析结果
	bool inspectDevice(const libusb_device_descriptor& desc, libusb_device* dev,
	                   DeviceInfo& info) const;
	// 重建"USB 设备 ↔ 本机挂载点"映射
	void refreshMountPoints();
	// 调用方需已持锁
	bool selectionNeedsAddrLocked() const;
	bool isSelectedLocked(const DeviceInfo& d) const;

	libusb_context* m_ctx = nullptr;
	std::vector<DeviceInfo> m_devices;
	QVector<SelectionKey> m_selection; // persists across enumerate()
	mutable QMutex m_mutex;
	libusb_hotplug_callback_handle m_hotplugHandle{};
	std::thread m_eventThread;
	std::atomic<bool> m_stop{ false };

	// 磁盘重定向状态（由 mini-qf-client 在 PreConnect 下发）
	bool m_diskWildcard = false;
	QStringList m_diskPaths;
};
