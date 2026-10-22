// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "libnks/transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

#include <libusb.h>
#if defined(__APPLE__)
#include "usb_darwin.h"
#endif

namespace libnks {

KksUsbDeviceMonitor::KksUsbDeviceMonitor(Enumerator enumerate)
    : enumerate(std::move(enumerate)) {
    if (!this->enumerate)
        this->enumerate = [] { return KksLibusbDevice::enumerate(); };
}

std::vector<KksUsbHotplugEvent> KksUsbDeviceMonitor::scan() {
    auto current = enumerate();
    const auto less = [](const KksUsbDeviceLocation &left,
                              const KksUsbDeviceLocation &right) {
        return std::tie(left.bus, left.address, left.vendor_id, left.product_id,
                       left.device_version) <
               std::tie(right.bus, right.address, right.vendor_id,
                       right.product_id, right.device_version);
    };
    std::ranges::sort(current, less);
    current.erase(std::unique(current.begin(), current.end()), current.end());
    std::vector<KksUsbHotplugEvent> events;
    if (initialized) {
        for (const auto &previous : device_list) {
            if (!std::ranges::binary_search(current, previous, less))
                events.push_back(
                        { KksUsbHotplugEventKind::disconnected, previous });
        }
    }
    for (const auto &device : current) {
        if (!initialized ||
                !std::ranges::binary_search(device_list, device, less))
            events.push_back({ KksUsbHotplugEventKind::connected, device });
    }
    device_list = std::move(current);
    initialized = true;
    return events;
}

struct KksLibusbDevice::Impl {
    libusb_context *context{};
    libusb_device_handle *handle{};
#if defined(__APPLE__)
    DarwinUsbInterface native;
#endif
    int transfer(std::uint8_t endpoint, unsigned char *bytes, int length,
            int *transferred, unsigned timeout_ms) {
#if defined(__APPLE__)
        return native.transfer(
                endpoint, bytes, length, transferred, timeout_ms);
#else
        return libusb_bulk_transfer(
                handle, endpoint, bytes, length, transferred, timeout_ms);
#endif
    }
    int interface_number{ -1 };
    RpcFrameDecoder frames;
    std::mutex primary_mutex;
    std::condition_variable primary_ready;
    std::condition_variable primary_drained;
    KksPrimaryWriteBatch primary_batch;
    std::jthread primary_worker;
    bool primary_active{};
    bool primary_failed{};
    std::atomic<int> primary_error{};
    std::atomic<int> error{};
};

namespace {
template <class Transfer>
bool bulk_write_all(Transfer transfer, std::uint8_t endpoint,
        std::span<const std::byte> bytes, std::atomic<int> &error) {
    std::size_t offset{};
    while (offset < bytes.size()) {
        int transferred{};
        const auto result = transfer(endpoint,
                reinterpret_cast<unsigned char *>(
                        const_cast<std::byte *>(bytes.data() + offset)),
                static_cast<int>(bytes.size() - offset), &transferred, 10'000);
        error.store(result, std::memory_order_relaxed);
        if (transferred > 0)
            offset += static_cast<std::size_t>(transferred);
        if (result == 0)
            continue;
        if (result == LIBUSB_ERROR_TIMEOUT && transferred > 0)
            continue;
        return false;
    }
    return true;
}
} // namespace

KksLibusbDevice::KksLibusbDevice() : impl(std::make_unique<Impl>()) {
    impl->error = libusb_init(&impl->context);
}

KksLibusbDevice::~KksLibusbDevice() {
    close();
    if (impl && impl->context)
        libusb_exit(impl->context);
}

bool KksLibusbDevice::available() noexcept { return true; }

std::vector<KksUsbDeviceLocation> KksLibusbDevice::enumerate() {
    std::vector<KksUsbDeviceLocation> result;
    libusb_context *context{};
    if (libusb_init(&context) != 0)
        return result;
    libusb_device **devices{};
    const auto count = libusb_get_device_list(context, &devices);
    if (count >= 0) {
        for (ssize_t index = 0; index < count; ++index) {
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(devices[index], &descriptor) !=
                            0 ||
                    !is_supported_kks_usb_device(
                            descriptor.idVendor, descriptor.idProduct))
                continue;
            result.push_back({ descriptor.idVendor, descriptor.idProduct,
                    descriptor.bcdDevice, libusb_get_bus_number(devices[index]),
                    libusb_get_device_address(devices[index]) });
        }
    }
    if (devices)
        libusb_free_device_list(devices, 1);
    libusb_exit(context);
    return result;
}

bool KksLibusbDevice::open(const KksUsbDeviceLocation &location) {
    close();
    if (!impl->context)
        return false;
    if (!is_supported_kks_usb_device(location.vendor_id, location.product_id)) {
        impl->error = LIBUSB_ERROR_INVALID_PARAM;
        return false;
    }
#if defined(__APPLE__)
    if (!impl->native.open(location)) {
        impl->error = impl->native.error();
        return false;
    }
#else
    libusb_device **devices{};
    const auto count = libusb_get_device_list(impl->context, &devices);
    if (count < 0) {
        impl->error = static_cast<int>(count);
        return false;
    }
    libusb_device *selected{};
    for (ssize_t index = 0; index < count; ++index) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[index], &descriptor) == 0 &&
                descriptor.idVendor == location.vendor_id &&
                descriptor.idProduct == location.product_id &&
                libusb_get_bus_number(devices[index]) == location.bus &&
                libusb_get_device_address(devices[index]) == location.address) {
            selected = devices[index];
            break;
        }
    }
    if (!selected) {
        libusb_free_device_list(devices, 1);
        impl->error = LIBUSB_ERROR_NO_DEVICE;
        return false;
    }
    libusb_config_descriptor *config{};
    impl->error = libusb_get_active_config_descriptor(selected, &config);
    int interface_number = -1;
    int alternate_setting = 0;
    if (impl->error == 0) {
        for (std::uint8_t i = 0;
                i < config->bNumInterfaces && interface_number < 0; ++i) {
            const auto &interface = config->interface[i];
            for (int a = 0; a < interface.num_altsetting; ++a) {
                const auto &alternate = interface.altsetting[a];
                std::vector<KksUsbEndpointDescriptor> endpoints;
                endpoints.reserve(alternate.bNumEndpoints);
                for (std::uint8_t e = 0; e < alternate.bNumEndpoints; ++e)
                    endpoints.push_back(
                            { alternate.endpoint[e].bEndpointAddress,
                                    alternate.endpoint[e].bmAttributes,
                                    alternate.endpoint[e].wMaxPacketSize });
                if (is_kks_usb_interface(
                            alternate.bInterfaceSubClass, endpoints)) {
                    interface_number = alternate.bInterfaceNumber;
                    alternate_setting = alternate.bAlternateSetting;
                    break;
                }
            }
        }
    }
    if (interface_number < 0) {
        if (config)
            libusb_free_config_descriptor(config);
        libusb_free_device_list(devices, 1);
        if (impl->error == 0)
            impl->error = LIBUSB_ERROR_NOT_FOUND;
        return false;
    }
    impl->error = libusb_open(selected, &impl->handle);
    libusb_free_config_descriptor(config);
    libusb_free_device_list(devices, 1);
    if (impl->error != 0)
        return false;
    (void)libusb_set_auto_detach_kernel_driver(impl->handle, 1);
    impl->error = libusb_claim_interface(impl->handle, interface_number);
    if (impl->error == 0 && alternate_setting != 0)
        impl->error = libusb_set_interface_alt_setting(
                impl->handle, interface_number, alternate_setting);
    if (impl->error != 0) {
        libusb_close(impl->handle);
        impl->handle = nullptr;
        return false;
    }
    impl->interface_number = interface_number;
#endif
    impl->error = 0;
    impl->frames.clear();
    impl->primary_failed = false;
    impl->primary_error.store(0);
    impl->primary_active = false;
    impl->primary_batch.clear();
    impl->primary_worker = std::jthread([impl = this->impl.get()](
                                                std::stop_token stop) {
        using namespace std::chrono_literals;
        std::unique_lock lock(impl->primary_mutex);
        while (!stop.stop_requested()) {
            impl->primary_ready.wait(lock, [&] {
                return stop.stop_requested() || !impl->primary_batch.empty();
            });
            if (stop.stop_requested())
                break;
            const auto deadline = std::chrono::steady_clock::now() + 1ms;
            impl->primary_ready.wait_until(
                    lock, deadline, [&] { return stop.stop_requested(); });
            if (stop.stop_requested())
                break;
            do {
                auto bytes = impl->primary_batch.take();
                impl->primary_active = true;
                lock.unlock();
                const auto succeeded = bulk_write_all(
                        [impl](auto... args) {
                            return impl->transfer(args...);
                        },
                        kks_usb_primary_endpoint, bytes, impl->primary_error);
                lock.lock();
                if (!succeeded) {
                    impl->primary_failed = true;
                    impl->primary_batch.clear();
                }
            } while (!stop.stop_requested() && !impl->primary_failed &&
                     !impl->primary_batch.empty());
            impl->primary_active = false;
            impl->primary_drained.notify_all();
            if (impl->primary_failed)
                break;
        }
        impl->primary_batch.clear();
        impl->primary_active = false;
        impl->primary_drained.notify_all();
    });
    return true;
}

void KksLibusbDevice::close() noexcept {
    if (connected()) {
        try {
            (void)flush();
        } catch (...) {}
        impl->primary_worker.request_stop();
        impl->primary_ready.notify_all();
        if (impl->primary_worker.joinable())
            impl->primary_worker.join();
#if defined(__APPLE__)
        impl->native.close();
#else
        if (impl->interface_number >= 0)
            (void)libusb_release_interface(
                    impl->handle, impl->interface_number);
        libusb_close(impl->handle);
        impl->handle = nullptr;
#endif
        impl->interface_number = -1;
        impl->frames.clear();
    }
}

bool KksLibusbDevice::connected() const noexcept {
#if defined(__APPLE__)
    return impl && impl->native.connected();
#else
    return impl && impl->handle;
#endif
}

int KksLibusbDevice::last_error() const noexcept { return impl->error.load(); }

std::string_view KksLibusbDevice::error_stage() const noexcept {
#if defined(__APPLE__)
    return impl->native.error_stage();
#else
    return "USB open/claim";
#endif
}

std::string_view KksLibusbDevice::error_detail() const noexcept {
#if defined(__APPLE__)
    return impl->native.error_detail();
#else
    return {};
#endif
}

bool KksLibusbDevice::receive_timed_out() const noexcept {
    return impl->error.load() == LIBUSB_ERROR_TIMEOUT;
}

bool KksLibusbDevice::send(const KksRoutedFrame &frame) {
    if (!connected()) {
        impl->error = LIBUSB_ERROR_NO_DEVICE;
        return false;
    }
    if (frame.channel == KksUsbChannel::primary) {
        std::lock_guard lock(impl->primary_mutex);
        if (impl->primary_failed) {
            impl->error.store(impl->primary_error.load());
            return false;
        }
        const auto was_empty = impl->primary_batch.empty();
        if (!impl->primary_batch.append(frame))
            return false;
        if (was_empty)
            impl->primary_ready.notify_one();
        return true;
    }
    return bulk_write_all(
            [this](auto... args) { return impl->transfer(args...); },
            kks_usb_endpoint(frame.channel), frame.bytes, impl->error);
}

bool KksLibusbDevice::flush() {
    if (!connected()) {
        impl->error.store(LIBUSB_ERROR_NO_DEVICE);
        return false;
    }
    std::unique_lock lock(impl->primary_mutex);
    impl->primary_drained.wait(lock, [&] {
        return impl->primary_failed ||
               (impl->primary_batch.empty() && !impl->primary_active);
    });
    if (impl->primary_failed)
        impl->error.store(impl->primary_error.load());
    return !impl->primary_failed;
}

std::optional<std::vector<std::byte>> KksLibusbDevice::receive_frame(
        unsigned timeout_ms) {
    if (!connected()) {
        impl->error = LIBUSB_ERROR_NO_DEVICE;
        return std::nullopt;
    }
    if (auto frame = impl->frames.next())
        return frame;
    std::array<std::byte, kks_usb_read_size> chunk{};
    while (true) {
        int transferred{};
        impl->error = impl->transfer(kks_usb_in_endpoint,
                reinterpret_cast<unsigned char *>(chunk.data()),
                static_cast<int>(chunk.size()), &transferred, timeout_ms);
        if (impl->error != 0)
            return std::nullopt;
        impl->frames.append(
                std::span(chunk).first(static_cast<std::size_t>(transferred)));
        if (auto frame = impl->frames.next())
            return frame;
        if (impl->frames.failed()) {
            impl->error = LIBUSB_ERROR_OVERFLOW;
            return std::nullopt;
        }
    }
}

} // namespace libnks
