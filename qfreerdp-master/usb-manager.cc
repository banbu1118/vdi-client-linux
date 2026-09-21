#include "usb-manager.h"
#include "qf_log.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>

#include <algorithm>
#include <cstring>

namespace
{

/* rdpdr 在 Linux 下把哪些挂载点当盘送进虚拟机（见 freerdp-3.28.0/channels/rdpdr/
 * client/rdpdr_main.c 的 is_redirectable_mount），这里的判定必须与之保持一致，
 * 否则会出现"UI 说已重定向、VM 里却没盘符"。
 *
 * 与 USB 列表相关的只有"U盘/移动硬盘"这一支：块设备 + 挂在用户存储根下 + 可读。
 * NAS（网络文件系统）和 home 目录虽然也会被重定向，但它们不是 USB 设备，
 * 在下面 usbAddressForBlockDevice() 那一步就会被排除。 */
bool isUserStoragePath(const QString& path)
{
	static const QStringList roots = { QStringLiteral("/media"), QStringLiteral("/run/media"),
		                               QStringLiteral("/mnt") };

	for (const auto& root : roots)
	{
		if (path == root || path.startsWith(root + QLatin1Char('/')))
			return true;
	}
	return false;
}

bool isRedirectedUsbMount(const QString& source, const QString& mountPoint)
{
	if (!source.startsWith(QStringLiteral("/dev/")))
		return false;
	/* cliprdr 等 FUSE 通道的临时挂载，源是 /dev/fuse 而不是分区 */
	if (source == QStringLiteral("/dev/fuse"))
		return false;
	/* snap / squashfs 等只读镜像，不是用户可用的磁盘 */
	if (source.startsWith(QStringLiteral("/dev/loop")))
		return false;
	if (!isUserStoragePath(mountPoint))
		return false;
	/* 用户读不到的挂载点，rdpdr 那边也会跳过（access(R_OK)） */
	return QFileInfo(mountPoint).isReadable();
}

/* 显式模式（/drive:NAME,path）下判断挂载点是否落在被重定向的路径内 */
bool pathIsRedirected(const QStringList& paths, const QString& mountPoint)
{
	for (const auto& raw : paths)
	{
		QString p = raw;
		while (p.length() > 1 && p.endsWith(QLatin1Char('/')))
			p.chop(1);
		if (p.isEmpty())
			continue;
		if (mountPoint == p || mountPoint.startsWith(p + QLatin1Char('/')))
			return true;
	}
	return false;
}

bool readUintFile(const QString& path, uint* out)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return false;
	bool ok = false;
	const uint value = QString::fromLatin1(f.readAll()).trimmed().toUInt(&ok);
	if (!ok)
		return false;
	*out = value;
	return true;
}

/* 把块设备节点（/dev/sdb1）映射回 USB 设备的 (busnum, devnum)：
 * /sys/class/block/sdb1 是软链接，逐级向上找到带 idVendor 的 USB 设备目录即可。
 * 非 USB 设备（dm-*、nvme 等）回溯不到 idVendor，直接判定失败。 */
bool usbAddressForBlockDevice(const QString& source, uint8_t* bus, uint8_t* addr)
{
	if (!source.startsWith(QStringLiteral("/dev/")))
		return false;

	const QFileInfo link(QStringLiteral("/sys/class/block/%1").arg(source.mid(5)));
	if (!link.exists())
		return false;

	const QString real = link.symLinkTarget();
	if (real.isEmpty())
		return false;

	QDir dir(real);
	while (dir.path() != QStringLiteral("/") && !QFile::exists(dir.filePath(QStringLiteral("idVendor"))))
	{
		if (!dir.cdUp())
			return false;
	}
	if (!QFile::exists(dir.filePath(QStringLiteral("idVendor"))))
		return false;

	uint busnum = 0;
	uint devnum = 0;
	if (!readUintFile(dir.filePath(QStringLiteral("busnum")), &busnum) ||
	    !readUintFile(dir.filePath(QStringLiteral("devnum")), &devnum))
		return false;

	*bus = static_cast<uint8_t>(busnum);
	*addr = static_cast<uint8_t>(devnum);
	return true;
}

} // namespace

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

bool USBManager::inspectDevice(const libusb_device_descriptor& desc,
                               libusb_device* dev, DeviceInfo& info) const
{
	// Always skip USB hubs
	if (desc.bDeviceClass == 0x09)
		return false;

	// Skip wireless/Bluetooth controllers
	if (desc.bDeviceClass == 0xE0)
		return false;

	libusb_config_descriptor* config = nullptr;
	if (libusb_get_active_config_descriptor(dev, &config) != 0 || !config)
	{
		// 读不到接口描述符（权限/后端限制）时保守保留，且不参与置灰判断
		info.hasNonStorageInterface = true;
		return true;
	}

	// 按接口类别判定：仅当所有接口都是 HID(0x03)/Audio(0x01) 时才隐藏。
	// 只要出现其它类别（含厂商自定义 0xFF、Mass Storage 0x08）就保留，
	// 这样带 HID 子接口的加密狗能出现在列表里。
	bool allHidOrAudio = config->bNumInterfaces > 0;
	for (int i = 0; i < static_cast<int>(config->bNumInterfaces); i++)
	{
		const auto* iface = &config->interface[i];
		if (iface->num_altsetting <= 0)
		{
			allHidOrAudio = false;
			info.hasNonStorageInterface = true;
			continue;
		}

		const uint8_t cls = iface->altsetting[0].bInterfaceClass;
		if (cls != 0x03 && cls != 0x01)
			allHidOrAudio = false;
		if (cls == 0x08)
			info.hasStorageInterface = true;
		else
			info.hasNonStorageInterface = true;
	}

	libusb_free_config_descriptor(config);
	return !allHidOrAudio;
}

void USBManager::refreshMountPoints()
{
	for (auto& d : m_devices)
		d.mountPoints.clear();

	if (!m_diskWildcard && m_diskPaths.isEmpty())
		return;

	QFile f(QStringLiteral("/proc/mounts"));
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
	{
		qf::log::warn("usb/mount", "/proc/mounts open failed");
		return;
	}

	while (!f.atEnd())
	{
		const QByteArray line = f.readLine();
		const QList<QByteArray> fields = line.trimmed().split(' ');
		if (fields.size() < 2)
			continue;

		const QString source = QString::fromLocal8Bit(fields[0]);
		// /proc/mounts 用八进制转义空格等特殊字符
		QString mountPoint = QString::fromLocal8Bit(fields[1]);
		mountPoint.replace(QStringLiteral("\\040"), QStringLiteral(" "));
		mountPoint.replace(QStringLiteral("\\011"), QStringLiteral("\t"));
		mountPoint.replace(QStringLiteral("\\012"), QStringLiteral("\n"));
		mountPoint.replace(QStringLiteral("\\134"), QStringLiteral("\\"));

		/* 与 rdpdr 的 is_redirectable_mount 保持一致：U盘/移动硬盘 = 块设备
		 * 且挂在用户存储根下（/media、/run/media、/mnt）且当前用户读得到，
		 * 否则会出现"UI 说已重定向、VM 里却没盘符"。 */
		const bool redirected = m_diskWildcard ? isRedirectedUsbMount(source, mountPoint)
		                                       : pathIsRedirected(m_diskPaths, mountPoint);
		if (!redirected)
			continue;

		uint8_t bus = 0;
		uint8_t addr = 0;
		if (!usbAddressForBlockDevice(source, &bus, &addr))
			continue;

		for (auto& d : m_devices)
		{
			if (d.bus != bus || d.addr != addr)
				continue;
			if (!d.mountPoints.empty())
				d.mountPoints += ", ";
			d.mountPoints += mountPoint.toStdString();
			break;
		}
	}
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

		DeviceInfo info;
		info.vid = desc.idVendor;
		info.pid = desc.idProduct;
		info.bus = libusb_get_bus_number(dev);
		info.addr = libusb_get_device_address(dev);

		if (!inspectDevice(desc, dev, info))
			continue;

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

		m_devices.push_back(std::move(info));
	}

	libusb_free_device_list(list, 1);

	// addr: 是动态的：重插或换 USB 口后 bus/addr 会变，
	// 因此按 addr 匹配的选中项若已不在本机就丢弃，避免把失效参数写进通道。
	if (selectionNeedsAddrLocked())
	{
		m_selection.removeIf([this](const SelectionKey& k) {
			for (const auto& d : m_devices)
			{
				if (d.bus == k.bus && d.addr == k.addr)
					return false;
			}
			return true;
		});
	}

	refreshMountPoints();

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
// Selection
// ====================================================================

bool USBManager::selectionNeedsAddrLocked() const
{
	// 选中集合里出现同 VID:PID 多支时，id: 无法区分，必须整次连接改用 addr:
	for (const auto& k : m_selection)
	{
		int n = 0;
		for (const auto& d : m_devices)
		{
			if (d.vid == k.vid && d.pid == k.pid && ++n > 1)
				return true;
		}
	}
	return false;
}

bool USBManager::selectionNeedsAddr() const
{
	QMutexLocker lock(&m_mutex);
	return selectionNeedsAddrLocked();
}

bool USBManager::isSelectedLocked(const DeviceInfo& d) const
{
	const bool byAddr = selectionNeedsAddrLocked();
	for (const auto& k : m_selection)
	{
		if (byAddr)
		{
			if (k.bus == d.bus && k.addr == d.addr)
				return true;
		}
		else if (k.vid == d.vid && k.pid == d.pid)
			return true;
	}
	return false;
}

std::vector<USBManager::SelectedDevice> USBManager::selectedDevices() const
{
	QMutexLocker lock(&m_mutex);

	const bool byAddr = selectionNeedsAddrLocked();
	std::vector<SelectedDevice> out;

	for (const auto& k : m_selection)
	{
		for (const auto& d : m_devices)
		{
			const bool hit = byAddr ? (d.bus == k.bus && d.addr == k.addr)
			                        : (d.vid == k.vid && d.pid == k.pid);
			if (!hit)
				continue;

			const bool dup = std::any_of(
			    out.begin(), out.end(), [&](const SelectedDevice& s) {
				    return s.bus == d.bus && s.addr == d.addr;
			    });
			if (!dup)
			{
				SelectedDevice sel;
				sel.vid = d.vid;
				sel.pid = d.pid;
				sel.bus = d.bus;
				sel.addr = d.addr;
				out.push_back(sel);
			}
			break;
		}
	}

	return out;
}

void USBManager::setDeviceSelected(int index, bool selected)
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return;

	const DeviceInfo d = m_devices[index];
	const bool byAddr = selectionNeedsAddrLocked();

	// 同一 VID:PID 只可能选一支；未区分时按 id 匹配，区分时按 addr 匹配
	m_selection.removeIf([&](const SelectionKey& k) {
		if (k.vid != d.vid || k.pid != d.pid)
			return false;
		return byAddr ? (k.bus == d.bus && k.addr == d.addr) : true;
	});

	if (selected)
	{
		SelectionKey key;
		key.vid = d.vid;
		key.pid = d.pid;
		key.bus = d.bus;
		key.addr = d.addr;
		m_selection.append(key);
	}
}

void USBManager::clearSelection()
{
	QMutexLocker lock(&m_mutex);
	m_selection.clear();
	for (auto& d : m_devices)
	{
		d.state = DeviceInfo::Idle;
		d.error.clear();
	}
	emit deviceListChanged();
}

void USBManager::applySelection()
{
	// Selection is already stored in m_selection.
	// Signal C++ side to trigger a reconnect so my_pre_connect
	// picks up the new device selection.
	emit reconnectRequested();
}

int USBManager::selectedCount() const
{
	QMutexLocker lock(&m_mutex);
	int n = 0;
	for (const auto& d : m_devices)
	{
		if (isSelectedLocked(d))
			n++;
	}
	return n;
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
	return isSelectedLocked(m_devices[index]);
}

bool USBManager::isDiskRedirected(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return false;

	const auto& d = m_devices[index];
	// 没有挂载点 → 未被磁盘重定向（MTP/PTP、加密狗、U 盾等继续走 USB 透传）
	if (d.mountPoints.empty())
		return false;
	// 复合设备不整体置灰：除存储接口外还有其它接口，保留 USB 勾选项
	if (d.hasNonStorageInterface)
		return false;
	return true;
}

QString USBManager::deviceMountPoints(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return {};
	return QString::fromStdString(m_devices[index].mountPoints);
}

bool USBManager::isStorageComposite(int index) const
{
	QMutexLocker lock(&m_mutex);
	if (index < 0 || index >= static_cast<int>(m_devices.size()))
		return false;

	const auto& d = m_devices[index];
	return d.hasStorageInterface && d.hasNonStorageInterface;
}

void USBManager::refreshDiskState()
{
	QMutexLocker lock(&m_mutex);
	refreshMountPoints();
}

void USBManager::setDiskRedirectState(bool wildcard, const QStringList& paths)
{
	{
		QMutexLocker lock(&m_mutex);
		m_diskWildcard = wildcard;
		m_diskPaths = paths;
		refreshMountPoints();
	}
	// 该函数在 RDP 线程的 PostConnect 中调用，跨线程 emit 会排队投递到 QML 线程
	emit deviceListChanged();
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
