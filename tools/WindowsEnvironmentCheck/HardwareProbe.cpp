#include "HardwareProbe.h"

#include <Windows.h>
#include <SetupAPI.h>
#include <devguid.h>
#include <dxgi.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace zengine::envcheck
{
namespace
{
template <typename T>
class ComPtr
{
public:
    ~ComPtr()
    {
        Reset();
    }

    T* Get() const
    {
        return pointer_;
    }

    T** Put()
    {
        Reset();
        return &pointer_;
    }

    T* operator->() const
    {
        return pointer_;
    }

    void Reset()
    {
        if (pointer_ != nullptr)
        {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

private:
    T* pointer_ = nullptr;
};

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    const int byteCount = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0)
    {
        return {};
    }

    std::string result(static_cast<std::size_t>(byteCount), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), byteCount, nullptr, nullptr);
    return result;
}

std::string WideToUtf8(const wchar_t* value)
{
    return value == nullptr ? std::string{} : WideToUtf8(std::wstring(value));
}

std::string TrimAscii(std::string value)
{
    const auto notSpace = [](unsigned char character)
    {
        return character != 0 && !std::isspace(character);
    };
    const auto first = std::find_if(value.begin(), value.end(), notSpace);
    const auto last = std::find_if(value.rbegin(), value.rend(), notSpace).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::wstring ReadDeviceProperty(HDEVINFO deviceSet, SP_DEVINFO_DATA& device, DWORD property)
{
    DWORD requiredBytes = 0;
    SetupDiGetDeviceRegistryPropertyW(deviceSet, &device, property, nullptr, nullptr, 0, &requiredBytes);
    if (requiredBytes == 0)
    {
        return {};
    }

    std::vector<BYTE> buffer(requiredBytes + sizeof(wchar_t), 0);
    if (SetupDiGetDeviceRegistryPropertyW(
            deviceSet, &device, property, nullptr, buffer.data(), requiredBytes, nullptr) == FALSE)
    {
        return {};
    }
    return reinterpret_cast<const wchar_t*>(buffer.data());
}

std::wstring ReadRegistryString(HKEY key, const wchar_t* valueName)
{
    DWORD type = 0;
    DWORD byteCount = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &byteCount) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || byteCount == 0)
    {
        return {};
    }

    std::vector<wchar_t> value(byteCount / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(
            key, valueName, nullptr, nullptr, reinterpret_cast<BYTE*>(value.data()), &byteCount) != ERROR_SUCCESS)
    {
        return {};
    }
    return value.data();
}

std::wstring ToUpper(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character)
    {
        return static_cast<wchar_t>(towupper(character));
    });
    return value;
}

struct DisplayDriverInfo
{
    std::wstring hardwareIds;
    std::string provider;
    std::string version;
    std::string date;
};

std::vector<DisplayDriverInfo> CollectDisplayDrivers()
{
    std::vector<DisplayDriverInfo> result;
    const HDEVINFO deviceSet = SetupDiGetClassDevsW(
        &GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (deviceSet == INVALID_HANDLE_VALUE)
    {
        return result;
    }

    for (DWORD index = 0;; ++index)
    {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (SetupDiEnumDeviceInfo(deviceSet, index, &device) == FALSE)
        {
            break;
        }

        DisplayDriverInfo driver;
        driver.hardwareIds = ToUpper(ReadDeviceProperty(deviceSet, device, SPDRP_HARDWAREID));
        const HKEY driverKey = SetupDiOpenDevRegKey(
            deviceSet, &device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (driverKey != INVALID_HANDLE_VALUE)
        {
            driver.provider = WideToUtf8(ReadRegistryString(driverKey, L"ProviderName"));
            driver.version = WideToUtf8(ReadRegistryString(driverKey, L"DriverVersion"));
            driver.date = WideToUtf8(ReadRegistryString(driverKey, L"DriverDate"));
            RegCloseKey(driverKey);
        }
        result.push_back(std::move(driver));
    }

    SetupDiDestroyDeviceInfoList(deviceSet);
    return result;
}

std::wstring FormatPciId(std::uint32_t vendorId, std::uint32_t deviceId)
{
    wchar_t value[32]{};
    swprintf_s(value, L"VEN_%04X&DEV_%04X", vendorId, deviceId);
    return value;
}

const DisplayDriverInfo* FindDisplayDriver(
    const std::vector<DisplayDriverInfo>& drivers,
    std::uint32_t vendorId,
    std::uint32_t deviceId)
{
    const std::wstring id = FormatPciId(vendorId, deviceId);
    const auto match = std::find_if(drivers.begin(), drivers.end(), [&id](const DisplayDriverInfo& driver)
    {
        return driver.hardwareIds.find(id) != std::wstring::npos;
    });
    return match == drivers.end() ? nullptr : &*match;
}

std::string DriveTypeName(UINT type)
{
    switch (type)
    {
    case DRIVE_REMOVABLE:
        return "Removable";
    case DRIVE_FIXED:
        return "Fixed";
    case DRIVE_REMOTE:
        return "Network";
    case DRIVE_CDROM:
        return "Optical";
    case DRIVE_RAMDISK:
        return "RAM disk";
    case DRIVE_NO_ROOT_DIR:
        return "No root";
    default:
        return "Unknown";
    }
}

std::string BusTypeName(STORAGE_BUS_TYPE type)
{
    switch (type)
    {
    case BusTypeScsi: return "SCSI";
    case BusTypeAtapi: return "ATAPI";
    case BusTypeAta: return "ATA";
    case BusType1394: return "IEEE 1394";
    case BusTypeSsa: return "SSA";
    case BusTypeFibre: return "Fibre Channel";
    case BusTypeUsb: return "USB";
    case BusTypeRAID: return "RAID";
    case BusTypeiScsi: return "iSCSI";
    case BusTypeSas: return "SAS";
    case BusTypeSata: return "SATA";
    case BusTypeSd: return "SD";
    case BusTypeMmc: return "MMC";
    case BusTypeVirtual: return "Virtual";
    case BusTypeFileBackedVirtual: return "File-backed virtual";
    case BusTypeSpaces: return "Storage Spaces";
    case BusTypeNvme: return "NVMe";
    case BusTypeSCM: return "SCM";
    case BusTypeUfs: return "UFS";
    default: return "Unknown";
    }
}

std::string DescriptorString(const std::vector<BYTE>& buffer, DWORD offset)
{
    if (offset == 0 || offset >= buffer.size())
    {
        return {};
    }

    const char* begin = reinterpret_cast<const char*>(buffer.data() + offset);
    const char* end = reinterpret_cast<const char*>(buffer.data() + buffer.size());
    const char* terminator = std::find(begin, end, '\0');
    return TrimAscii(std::string(begin, terminator));
}
} // namespace

void CollectGraphicsInformation(EnvironmentSnapshot& snapshot)
{
    const auto drivers = CollectDisplayDrivers();
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.Put()))))
    {
        return;
    }

    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumResult = factory->EnumAdapters1(adapterIndex, adapter.Put());
        if (enumResult == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        if (FAILED(enumResult))
        {
            continue;
        }

        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)))
        {
            continue;
        }

        GpuAdapterInfo gpu;
        gpu.name = WideToUtf8(description.Description);
        gpu.vendorId = description.VendorId;
        gpu.deviceId = description.DeviceId;
        gpu.subsystemId = description.SubSysId;
        gpu.revision = description.Revision;
        gpu.dedicatedVideoMemoryBytes = description.DedicatedVideoMemory;
        gpu.dedicatedSystemMemoryBytes = description.DedicatedSystemMemory;
        gpu.sharedSystemMemoryBytes = description.SharedSystemMemory;
        gpu.softwareAdapter = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

        if (const auto* driver = FindDisplayDriver(drivers, gpu.vendorId, gpu.deviceId))
        {
            gpu.driverProvider = driver->provider;
            gpu.driverVersion = driver->version;
            gpu.driverDate = driver->date;
        }

        for (UINT outputIndex = 0;; ++outputIndex)
        {
            ComPtr<IDXGIOutput> output;
            const HRESULT outputResult = adapter->EnumOutputs(outputIndex, output.Put());
            if (outputResult == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(outputResult))
            {
                continue;
            }
            ++gpu.outputCount;
        }
        snapshot.gpuAdapters.push_back(std::move(gpu));
    }

    for (DWORD index = 0;; ++index)
    {
        DISPLAY_DEVICEW display{};
        display.cb = sizeof(display);
        if (EnumDisplayDevicesW(nullptr, index, &display, 0) == FALSE)
        {
            break;
        }
        if ((display.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0)
        {
            continue;
        }

        DisplayInfo info;
        info.deviceName = WideToUtf8(display.DeviceName);
        info.attachedToDesktop = (display.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        info.primary = (display.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;

        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        if (EnumDisplayDevicesW(display.DeviceName, 0, &monitor, 0) != FALSE)
        {
            info.monitorName = WideToUtf8(monitor.DeviceString);
        }

        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(display.DeviceName, ENUM_CURRENT_SETTINGS, &mode) != FALSE)
        {
            info.width = mode.dmPelsWidth;
            info.height = mode.dmPelsHeight;
            info.refreshRate = mode.dmDisplayFrequency;
            info.bitsPerPixel = mode.dmBitsPerPel;
        }
        snapshot.displays.push_back(std::move(info));
    }
}

void CollectStorageInformation(EnvironmentSnapshot& snapshot)
{
    for (unsigned int index = 0; index < 32; ++index)
    {
        const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(index);
        const HANDLE disk = CreateFileW(
            path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (disk == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        PhysicalDiskInfo info;
        info.devicePath = WideToUtf8(path);

        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        STORAGE_DESCRIPTOR_HEADER header{};
        DWORD returnedBytes = 0;
        if (DeviceIoControl(
                disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &header, sizeof(header),
                &returnedBytes, nullptr) != FALSE && header.Size >= sizeof(STORAGE_DEVICE_DESCRIPTOR))
        {
            std::vector<BYTE> buffer(header.Size, 0);
            if (DeviceIoControl(
                    disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer.data(),
                    static_cast<DWORD>(buffer.size()), &returnedBytes, nullptr) != FALSE)
            {
                const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
                info.vendor = DescriptorString(buffer, descriptor->VendorIdOffset);
                info.model = DescriptorString(buffer, descriptor->ProductIdOffset);
                info.busType = BusTypeName(descriptor->BusType);
                info.removable = descriptor->RemovableMedia != FALSE;
            }
        }

        GET_LENGTH_INFORMATION length{};
        if (DeviceIoControl(
                disk, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length, sizeof(length),
                &returnedBytes, nullptr) != FALSE)
        {
            info.capacityBytes = static_cast<std::uint64_t>(length.Length.QuadPart);
        }
        else
        {
            std::array<BYTE, sizeof(DISK_GEOMETRY_EX) + 1024> geometryBuffer{};
            if (DeviceIoControl(
                    disk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, geometryBuffer.data(),
                    static_cast<DWORD>(geometryBuffer.size()), &returnedBytes, nullptr) != FALSE)
            {
                const auto* geometry = reinterpret_cast<const DISK_GEOMETRY_EX*>(geometryBuffer.data());
                info.capacityBytes = static_cast<std::uint64_t>(geometry->DiskSize.QuadPart);
            }
        }
        CloseHandle(disk);
        snapshot.physicalDisks.push_back(std::move(info));
    }

    DWORD characterCount = GetLogicalDriveStringsW(0, nullptr);
    if (characterCount == 0)
    {
        return;
    }

    std::vector<wchar_t> roots(characterCount + 1, L'\0');
    if (GetLogicalDriveStringsW(characterCount, roots.data()) == 0)
    {
        return;
    }

    for (const wchar_t* root = roots.data(); *root != L'\0'; root += wcslen(root) + 1)
    {
        VolumeInfo info;
        info.rootPath = WideToUtf8(root);
        info.driveType = DriveTypeName(GetDriveTypeW(root));

        wchar_t label[MAX_PATH + 1]{};
        wchar_t fileSystem[MAX_PATH + 1]{};
        if (GetVolumeInformationW(
                root, label, MAX_PATH, nullptr, nullptr, nullptr, fileSystem, MAX_PATH) != FALSE)
        {
            info.volumeLabel = WideToUtf8(label);
            info.fileSystem = WideToUtf8(fileSystem);
        }

        ULARGE_INTEGER available{};
        ULARGE_INTEGER total{};
        ULARGE_INTEGER free{};
        if (GetDiskFreeSpaceExW(root, &available, &total, &free) != FALSE)
        {
            info.availableBytes = available.QuadPart;
            info.totalBytes = total.QuadPart;
            info.freeBytes = free.QuadPart;
        }
        snapshot.volumes.push_back(std::move(info));
    }
}
} // namespace zengine::envcheck
