// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "usb_darwin.h"

#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include <libusb.h>
#include <vector>

namespace libnks {
namespace {
int usb_error(IOReturn result) {
    switch (result) {
        case kIOReturnSuccess:
            return 0;
        case kIOReturnNotPrivileged:
            return LIBUSB_ERROR_ACCESS;
        case kIOReturnExclusiveAccess:
            return LIBUSB_ERROR_BUSY;
        case kIOReturnNoDevice:
        case kIOReturnNotAttached:
            return LIBUSB_ERROR_NO_DEVICE;
        case kIOReturnTimeout:
        case kIOUSBTransactionTimeout:
            return LIBUSB_ERROR_TIMEOUT;
        case kIOUSBPipeStalled:
            return LIBUSB_ERROR_PIPE;
        case kIOReturnBadArgument:
            return LIBUSB_ERROR_INVALID_PARAM;
        case kIOReturnNoMemory:
            return LIBUSB_ERROR_NO_MEM;
        default:
            return LIBUSB_ERROR_IO;
    }
}
template <class Interface>
Interface **query(
        io_service_t service, CFUUIDRef kind, CFUUIDRef iid, IOReturn &result) {
    IOCFPlugInInterface **plugin{};
    SInt32 score{};
    result = IOCreatePlugInInterfaceForService(
            service, kind, kIOCFPlugInInterfaceID, &plugin, &score);
    if (result != kIOReturnSuccess || !plugin)
        return nullptr;
    Interface **object{};
    const auto queried = (*plugin)->QueryInterface(plugin,
            CFUUIDGetUUIDBytes(iid), reinterpret_cast<LPVOID *>(&object));
    (*plugin)->Release(plugin);
    if (queried != S_OK || !object)
        result = kIOReturnUnsupported;
    return object;
}
} // namespace

bool DarwinUsbInterface::open(const KksUsbDeviceLocation &location) {
    close();
    detail.clear();
    transfer_failure = 0;
    stage = "USB interface enumeration";
    last_error = LIBUSB_ERROR_NO_DEVICE;
    io_iterator_t devices{};
    auto result = IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching(kIOUSBDeviceClassName), &devices);
    if (result != kIOReturnSuccess) {
        last_error = usb_error(result);
        return false;
    }
    io_service_t service{};
    while ((service = IOIteratorNext(devices))) {
        auto **device = query<IOUSBDeviceInterface>(service,
                kIOUSBDeviceUserClientTypeID, kIOUSBDeviceInterfaceID, result);
        IOObjectRelease(service);
        if (!device)
            continue;
        UInt16 vendor{}, product{};
        UInt32 loc{};
        USBDeviceAddress address{};
        const bool match =
                (*device)->GetDeviceVendor(device, &vendor) ==
                        kIOReturnSuccess &&
                (*device)->GetDeviceProduct(device, &product) ==
                        kIOReturnSuccess &&
                (*device)->GetLocationID(device, &loc) == kIOReturnSuccess &&
                (*device)->GetDeviceAddress(device, &address) ==
                        kIOReturnSuccess &&
                vendor == location.vendor_id &&
                product == location.product_id && (loc >> 24) == location.bus &&
                address == location.address;
        if (!match) {
            (*device)->Release(device);
            continue;
        }
        // only open the 0x0d subclass, grabbing the whole device would claim
        // interfaces we don't use
        IOUSBFindInterfaceRequest request{ kIOUSBFindInterfaceDontCare, 0x0d,
            kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
        io_iterator_t interfaces{};
        result = (*device)->CreateInterfaceIterator(
                device, &request, &interfaces);
        (*device)->Release(device);
        last_error = usb_error(result);
        if (result != kIOReturnSuccess)
            break;
        last_error = LIBUSB_ERROR_NOT_FOUND;
        io_service_t interface_service{};
        while ((interface_service = IOIteratorNext(interfaces))) {
            stage = "USB interface query";
            auto **candidate = query<IOUSBInterfaceInterface300>(
                    interface_service, kIOUSBInterfaceUserClientTypeID,
                    kIOUSBInterfaceInterfaceID300, result);
            if (!candidate) {
                IOObjectRelease(interface_service);
                last_error = usb_error(result);
                continue;
            }
            stage = "USB interface open";
            result = (*candidate)->USBInterfaceOpen(candidate);
            if (result != kIOReturnSuccess) {
                last_error = usb_error(result);
                char native_error[32]{};
                std::snprintf(native_error, sizeof native_error, "IOKit=0x%08x",
                        unsigned(result));
                detail = native_error;
                if (result == kIOReturnExclusiveAccess) {
                    const auto owner = IORegistryEntrySearchCFProperty(
                            interface_service, kIOServicePlane,
                            CFSTR("UsbExclusiveOwner"), kCFAllocatorDefault,
                            kIORegistryIterateRecursively |
                                    kIORegistryIterateParents);
                    if (owner) {
                        char name[512]{};
                        if (CFGetTypeID(owner) == CFStringGetTypeID() &&
                                CFStringGetCString(
                                        static_cast<CFStringRef>(owner), name,
                                        sizeof name, kCFStringEncodingUTF8))
                            detail += std::string("; owner: ") + name;
                        CFRelease(owner);
                    }
                }
                IOObjectRelease(interface_service);
                (*candidate)->Release(candidate);
                continue;
            }
            IOObjectRelease(interface_service);
            stage = "USB bulk endpoint discovery";
            UInt8 count{}, subclass{};
            std::vector<KksUsbEndpointDescriptor> endpoints;
            std::array<UInt8, 256> pipes{};
            result = (*candidate)->GetInterfaceSubClass(candidate, &subclass);
            if (result == kIOReturnSuccess)
                result = (*candidate)->GetNumEndpoints(candidate, &count);
            for (unsigned p = 1; result == kIOReturnSuccess && p <= count;
                    ++p) {
                UInt8 direction{}, number{}, type{}, interval{};
                UInt16 packet_size{};
                result = (*candidate)
                                 ->GetPipeProperties(candidate, UInt8(p),
                                         &direction, &number, &type,
                                         &packet_size, &interval);
                if (result != kIOReturnSuccess)
                    break;
                const auto endpoint =
                        std::uint8_t(number | (direction == kUSBIn ? 0x80 : 0));
                endpoints.push_back({ endpoint, type, packet_size });
                pipes[endpoint] = UInt8(p);
            }
            if (result == kIOReturnSuccess &&
                    is_kks_usb_interface(subclass, endpoints)) {
                interface = candidate;
                this->pipes = pipes;
                last_error = 0;
                detail.clear();
                stage = "USB interface ready";
                break;
            }
            last_error = result == kIOReturnSuccess ? LIBUSB_ERROR_NOT_FOUND
                                                    : usb_error(result);
            (*candidate)->USBInterfaceClose(candidate);
            (*candidate)->Release(candidate);
        }
        IOObjectRelease(interfaces);
        break;
    }
    IOObjectRelease(devices);
    return connected();
}

void DarwinUsbInterface::close() noexcept {
    if (interface) {
        (*interface)->USBInterfaceClose(interface);
        (*interface)->Release(interface);
        interface = nullptr;
    }
    this->pipes.fill(0);
}

int DarwinUsbInterface::transfer(std::uint8_t endpoint, unsigned char *bytes,
        int length, int *transferred, unsigned timeout_ms) {
    *transferred = 0;
    if (!interface)
        return LIBUSB_ERROR_NO_DEVICE;
    if (length < 0 || !this->pipes[endpoint])
        return LIBUSB_ERROR_INVALID_PARAM;
    UInt32 size = UInt32(length);
    const bool input = (endpoint & 0x80) != 0;
    const auto result =
            input ? (*interface)
                            ->ReadPipeTO(interface, this->pipes[endpoint],
                                    bytes, &size, timeout_ms, timeout_ms)
                  : (*interface)
                            ->WritePipeTO(interface, this->pipes[endpoint],
                                    bytes, size, timeout_ms, timeout_ms);
    // WritePipeTO doesn't tell us how much it wrote, so no partial retry
    if (result == kIOReturnSuccess)
        *transferred = input ? int(size) : length;
    const auto error = usb_error(result);
    if (error != 0 && error != LIBUSB_ERROR_TIMEOUT)
        transfer_failure.store(
                (std::uint64_t(std::uint32_t(result)) << 8) | endpoint);
    return error;
}
std::string_view DarwinUsbInterface::error_detail() const noexcept {
    if (const auto failure = transfer_failure.load()) {
        thread_local char text[80];
        std::snprintf(text, sizeof text, "IOKit=0x%08x endpoint=0x%02x",
                unsigned(failure >> 8), unsigned(failure & 255));
        return text;
    }
    return detail;
}
} // namespace libnks
