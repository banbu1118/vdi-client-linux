#include "usb-manager.h"
#include "qf_log.h"

#include <cstring>
#include <QMetaObject>

USBManager::USBManager(QObject* parent)
	: QObject(parent)
{
	// Initialize libusb
	int rc = libusb_init(&m_ctx);
	if (rc != LIBUSB_SUCCESS)
	{
		qf::log::error("usb/init", "libusb_init failed: {}", libusb_error_name(rc));
		m_ctx = nullptr;
		return;
	}

#if LIBUSB_API_VERSION >= 0x01000102
	libusb_set_option(m_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
#else
	libusb_set_debug(m_ctx, LIBUSB_LOG_LEVEL_WARNING);
#endif

	qf::log::info("usb/init", "libusb initialized");
	startHotplugThread();
}

USBManager::~USBManager()
{
	stopHotplugThread();
	if (m_ctx)
	{
		libusb_exit(m_ctx);
		m_ctx = nullptr;
	}
}

// ====================================================================
// Hotplug
// ====================================================================

int LIBUSB_CALL USBManager::hotplugCallback(libusb_context* /*ctx*/,
                                            libusb_device* /*dev*/,
                                            libusb_hotplug_event /*event*/,
                                            void* userdata)
{
	auto* self = static_cast<USBManager*>(userdata);
	// Queue a re-enumerate on the Qt main thread
	QMetaObject::invokeMethod(self, "onHotplugEvent", Qt::QueuedConnection);
	return 0; // keep callback registered
}

void USBManager::startHotplugThread()
{
	if (!m_ctx)
		return;

	// Register hotplug callback for device arrival + removal
	int rc = libusb_hotplug_register_callback(
		m_ctx,
		static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
		                                   LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
		LIBUSB_HOTPLUG_NO_FLAGS,
		LIBUSB_HOTPLUG_MATCH_ANY, // vid
		LIBUSB_HOTPLUG_MATCH_ANY, // pid
		LIBUSB_HOTPLUG_MATCH_ANY, // dev_class
		USBManager::hotplugCallback,
		this,
		&m_hotplugHandle);

	if (rc != LIBUSB_SUCCESS)
	{
		qf::log::warn("usb/hotplug", "hotplug registration failed: {}",
		              libusb_error_name(rc));
		return;
	}

	qf::log::info("usb/hotplug", "hotplug callback registered");

	// Start a dedicated event thread so hotplug callbacks actually fire
	m_stop = false;
	m_eventThread = std::thread([this]() {
		while (!m_stop.load(std::memory_order_relaxed))
		{
			// libusb_handle_events_completed blocks until an event occurs,
			// then returns 0. It returns 1 when the context is about to be
			// destroyed.
			struct timeval tv = { 1, 0 }; // 1 second timeout
			int rc = libusb_handle_events_timeout_completed(m_ctx, &tv, nullptr);
			if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED)
			{
				// Context destroyed or other fatal error
				break;
			}
		}
	});
}

void USBManager::stopHotplugThread()
{
	m_stop.store(true, std::memory_order_relaxed);
	if (m_eventThread.joinable())
		m_eventThread.join();

	if (m_hotplugHandle && m_ctx)
	{
		libusb_hotplug_deregister_callback(m_ctx, m_hotplugHandle);
		m_hotplugHandle = {};
	}
}

void USBManager::onHotplugEvent()
{
	qf::log::info("usb/hotplug", "device change detected, re-enumerating");
	enumerate();
}

// ====================================================================
// Enumeration
// ====================================================================

bool USBManager::shouldShowDevice(const libusb_device_descriptor& desc,
                                  libusb_device* dev) const
{
	// Always skip USB hubs
	if (desc.bDeviceClass == 0x09)
		return false;

	// Skip wireless/Bluetooth controllers
	if (desc.bDeviceClass == 0xE0)
		return false;

	// Skip audio devices (headsets, speakers, microphones)
	if (desc.bDeviceClass == 0x01)
		return false;

	// Check interface descriptors for HID (keyboard, mouse, touchpad)
	// and Audio (headsets, speakers)
	libusb_config_descriptor* config = nullptr;
	if (libusb_get_active_config_descriptor(dev, &config) == 0 && config)
	{
		for (int i = 0; i < static_cast<int>(config->bNumInterfaces); i++)
		{
			const auto* iface = &config->interface[i];
			for (int j = 0; j < iface->num_altsetting; j++)
			{
				if (iface->altsetting[j].bInterfaceClass == 0x03 ||
				    iface->altsetting[j].bInterfaceClass == 0x01)
				{
					// HID (keyboard/mouse/touchpad) or Audio (headset/speaker)
					libusb_free_config_descriptor(config);
					return false;
				}
			}
		}
		libusb_free_config_descriptor(config);
	}

	return true;
}

void USBManager::enumerateInternal()
{
	if (!m_ctx)
		return;

	m_devices.clear();

	libusb_device** list = nullptr;
	ssize_t count = libusb_get_device_list(m_ctx, &list);
	if (count < 0)
		return;

	for (ssize_t i = 0; i < count; i++)
	{
		libusb_device* dev = list[i];
		libusb_device_descriptor desc;

		if (libusb_get_device_descriptor(dev, &desc) != 0)
			continue;

		if (!shouldShowDevice(desc, dev))
			continue;

		DeviceInfo info;
		info.vid = desc.idVendor;
		info.pid = desc.idProduct;
		info.bus = libusb_get_bus_number(dev);
		info.addr = libusb_get_device_address(dev);

		// Try to get string descriptors
		libusb_device_handle* handle = nullptr;
		if (libusb_open(dev, &handle) == 0)
		{
			char buf[256] = {};

			if (desc.iManufacturer)
			{
				int len = libusb_get_string_descriptor_ascii(
					handle, desc.iManufacturer,
					reinterpret_cast<unsigned char*>(buf), sizeof(buf));
				if (len > 0)
					info.manufacturer.assign(buf, static_cast<size_t>(len));
			}

			if (desc.iProduct)
			{
				int len = libusb_get_string_descriptor_ascii(
					handle, desc.iProduct,
					reinterpret_cast<unsigned char*>(buf), sizeof(buf));
				if (len > 0)
					info.product.assign(buf, static_cast<size_t>(len));
			}

			if (desc.iSerialNumber)
			{
				int len = libusb_get_string_descriptor_ascii(
					handle, desc.iSerialNumber,
					reinterpret_cast<unsigned char*>(buf), sizeof(buf));
				if (len > 0)
					info.serial.assign(buf, static_cast<size_t>(len));
			}

			libusb_close(handle);
		}

		// Restore selection state if this device was previously selected
		auto key = qMakePair(info.vid, info.pid);
		if (m_selectedIds.contains(key))
		{
			info.selected = true;
		}

		m_devices.push_back(std::move(info));
	}

	libusb_free_device_list(list, 1);

	qf::log::info("usb/enum", "found {} USB device(s) after filtering",
	              m_devices.size());
}

void USBManager::enumerate()
{
	{
		QMutexLocker lock(&m_mutex);
		enumerateInternal();
	}
	emit deviceListChanged();
}

// ====================================================================
// QML accessors
// ====================================================================

int USBManager::deviceCount() const
{
	QMutexLocker lock(&m_mutex);
	return static_cast<int>(m_devices.size());
}

QString USBManager::deviceLabel(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return {};

	const auto& d = m_devices[index];
	if (!d.manufacturer.empty() && !d.product.empty())
		return QString::fromStdString(d.manufacturer + " " + d.product);
	if (!d.product.empty())
		return QString::fromStdString(d.product);
	return QString::fromStdString(d.manufacturer);
}

QString USBManager::deviceVidPid(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return {};

	const auto& d = m_devices[index];
	return QString("%1:%2")
		.arg(d.vid, 4, 16, QLatin1Char('0'))
		.arg(d.pid, 4, 16, QLatin1Char('0'));
}

int USBManager::deviceState(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return DeviceInfo::Idle;
	return static_cast<int>(m_devices[index].state);
}

QString USBManager::deviceError(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return {};
	return QString::fromStdString(m_devices[index].error);
}

bool USBManager::isDeviceSelected(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return false;
	return m_devices[index].selected;
}

void USBManager::setDeviceSelected(int index, bool selected)
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return;

	auto& d = m_devices[index];
	d.selected = selected;
	auto key = qMakePair(d.vid, d.pid);

	if (selected)
		m_selectedIds.insert(key);
	else
		m_selectedIds.remove(key);
}

void USBManager::clearSelection()
{
	QMutexLocker lock(&m_mutex);
	m_selectedIds.clear();
	for (auto& d : m_devices)
	{
		d.selected = false;
		d.state = DeviceInfo::Idle;
		d.error.clear();
	}
	emit deviceListChanged();
}

void USBManager::applySelection()
{
	// Selection is already stored in m_selectedIds.
	// Signal C++ side to trigger a reconnect so my_pre_connect
	// picks up the new device selection.
	emit reconnectRequested();
}

int USBManager::selectedCount() const
{
	QMutexLocker lock(&m_mutex);
	return static_cast<int>(m_selectedIds.size());
}

std::vector<std::pair<uint16_t, uint16_t>> USBManager::selectedDeviceIds() const
{
	QMutexLocker lock(&m_mutex);
	std::vector<std::pair<uint16_t, uint16_t>> ids;
	ids.reserve(m_selectedIds.size());
	for (const auto& key : m_selectedIds)
		ids.emplace_back(key.first, key.second);
	return ids;
}

void USBManager::markRedirected(uint16_t vid, uint16_t pid, bool success,
                                const std::string& error)
{
	{
		QMutexLocker lock(&m_mutex);
		for (auto& d : m_devices)
		{
			if (d.vid == vid && d.pid == pid)
			{
				d.state = success ? DeviceInfo::Redirected : DeviceInfo::Failed;
				d.error = error;
				break;
			}
		}
	}
	emit deviceListChanged();
}
