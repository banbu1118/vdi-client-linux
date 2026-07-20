#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QPair>
#include <QMutex>
#include <QSet>
#include <atomic>
#include <thread>
#include <memory>
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

	// === Internal API (called from C++ connection code) ===
	std::vector<std::pair<uint16_t, uint16_t>> selectedDeviceIds() const;

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
		uint16_t vid, pid;
		uint8_t bus, addr;
		std::string manufacturer;
		std::string product;
		std::string serial;
		bool selected = false;

		enum State : int { Idle = 0, Redirecting = 1, Redirected = 2, Failed = 3 };
		State state = Idle;
		std::string error;
	};

	static int LIBUSB_CALL hotplugCallback(libusb_context* ctx, libusb_device* dev,
	                                       libusb_hotplug_event event, void* userdata);
	void enumerateInternal();
	void startHotplugThread();
	void stopHotplugThread();
	bool shouldShowDevice(const libusb_device_descriptor& desc,
	                      libusb_device* dev) const;

	libusb_context* m_ctx = nullptr;
	std::vector<DeviceInfo> m_devices;
	QSet<QPair<uint16_t, uint16_t>> m_selectedIds; // persists across enumerate()
	mutable QMutex m_mutex;
	libusb_hotplug_callback_handle m_hotplugHandle{};
	std::thread m_eventThread;
	std::atomic<bool> m_stop{ false };
};
