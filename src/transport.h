// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "libnks/wire.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace libnks {

inline constexpr std::uint16_t kks_usb_vendor_id = 0x17cc;
inline constexpr std::array<std::uint16_t, 4> kks_usb_product_ids{
    0x2410, // Kontrol S32
    0x2100, // Komplete Kontrol S49 MK3
    0x2110, // Komplete Kontrol S61 MK3
    0x2120, // Komplete Kontrol S88 MK3
};
inline constexpr std::uint8_t kks_usb_interface_subclass = 0x0d;
inline constexpr std::uint8_t kks_usb_in_endpoint = 0x83;
inline constexpr std::uint8_t kks_usb_heartbeat_endpoint = 0x03;
inline constexpr std::uint8_t kks_usb_primary_endpoint = 0x04;
inline constexpr std::size_t kks_usb_read_size = 1024;

struct KksUsbEndpointDescriptor {
    std::uint8_t address{};
    std::uint8_t attributes{};
    std::uint16_t maximum_packet_size{};
};

[[nodiscard]] bool is_supported_kks_usb_device(
        std::uint16_t vendor_id, std::uint16_t product_id) noexcept;
[[nodiscard]] bool is_kks_usb_interface(std::uint8_t interface_subclass,
        std::span<const KksUsbEndpointDescriptor> endpoints) noexcept;
[[nodiscard]] constexpr std::uint8_t kks_usb_endpoint(
        KksUsbChannel channel) noexcept {
    return static_cast<std::uint8_t>(channel);
}

struct KksUsbDeviceLocation {
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint16_t device_version{};
    std::uint8_t bus{};
    std::uint8_t address{};
    friend bool operator==(const KksUsbDeviceLocation &,
            const KksUsbDeviceLocation &) = default;
};

enum class KksUsbHotplugEventKind : std::uint8_t { connected, disconnected };
struct KksUsbHotplugEvent {
    KksUsbHotplugEventKind kind{ KksUsbHotplugEventKind::connected };
    KksUsbDeviceLocation device;
};

class KksUsbDeviceMonitor {
  public:
    using Enumerator = std::function<std::vector<KksUsbDeviceLocation>()>;
    explicit KksUsbDeviceMonitor(Enumerator enumerate = {});
    [[nodiscard]] std::vector<KksUsbHotplugEvent> scan();
    [[nodiscard]] const std::vector<KksUsbDeviceLocation> &
    devices() const noexcept {
        return device_list;
    }
    void clear() noexcept {
        device_list.clear();
        initialized = false;
    }

  private:
    Enumerator enumerate;
    std::vector<KksUsbDeviceLocation> device_list;
    bool initialized{};
};

class KksUsbTransport {
  public:
    virtual ~KksUsbTransport() = default;
    [[nodiscard]] virtual bool open(const KksUsbDeviceLocation &location) = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool connected() const noexcept = 0;
    [[nodiscard]] virtual int last_error() const noexcept = 0;
    [[nodiscard]] virtual std::string_view error_stage() const noexcept {
        return "USB open/claim";
    }
    [[nodiscard]] virtual std::string_view error_detail() const noexcept {
        return {};
    }
    // a timeout isn't a USB failure, keep the default if you can't time out
    [[nodiscard]] virtual bool receive_timed_out() const noexcept {
        return false;
    }
    [[nodiscard]] virtual bool send(const KksRoutedFrame &frame) = 0;
    // wait for accepted writes; synchronous transports have already sent them
    [[nodiscard]] virtual bool flush() { return true; }
    [[nodiscard]] virtual std::optional<std::vector<std::byte>> receive_frame(
            unsigned timeout_ms) = 0;
};

// endpoint 0x04 takes whole size-prefixed frames glued into one bulk write
class KksPrimaryWriteBatch {
  public:
    [[nodiscard]] bool append(const KksRoutedFrame &frame);
    [[nodiscard]] bool empty() const noexcept { return bytes.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return bytes.size(); }
    [[nodiscard]] std::vector<std::byte> take() noexcept;
    void clear() noexcept { bytes.clear(); }

  private:
    std::vector<std::byte> bytes;
};

// available() is false when we're built without libusb
class KksLibusbDevice final : public KksUsbTransport {
  public:
    KksLibusbDevice();
    ~KksLibusbDevice();
    KksLibusbDevice(const KksLibusbDevice &) = delete;
    KksLibusbDevice &operator=(const KksLibusbDevice &) = delete;
    KksLibusbDevice(KksLibusbDevice &&) = delete;
    KksLibusbDevice &operator=(KksLibusbDevice &&) = delete;

    [[nodiscard]] static bool available() noexcept;
    [[nodiscard]] static std::vector<KksUsbDeviceLocation> enumerate();
    [[nodiscard]] bool open(const KksUsbDeviceLocation &location) override;
    void close() noexcept override;
    [[nodiscard]] bool connected() const noexcept override;
    [[nodiscard]] int last_error() const noexcept override;
    [[nodiscard]] std::string_view error_stage() const noexcept override;
    [[nodiscard]] std::string_view error_detail() const noexcept override;
    [[nodiscard]] bool receive_timed_out() const noexcept override;
    [[nodiscard]] bool send(const KksRoutedFrame &frame) override;
    [[nodiscard]] bool flush() override;
    [[nodiscard]] std::optional<std::vector<std::byte>> receive_frame(
            unsigned timeout_ms = 3'600'000) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// return whole ODR frames only
class OdrFrameTransport {
  public:
    virtual ~OdrFrameTransport() = default;
    OdrFrameTransport() = default;
    OdrFrameTransport(const OdrFrameTransport &) = delete;
    OdrFrameTransport &operator=(const OdrFrameTransport &) = delete;

    [[nodiscard]] virtual bool connect(std::string_view endpoint) = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool connected() const noexcept = 0;
    [[nodiscard]] virtual int last_error() const noexcept = 0;
    [[nodiscard]] virtual bool send_frame(std::span<const std::byte> frame) = 0;
    [[nodiscard]] virtual std::optional<std::vector<std::byte>>
    receive_frame() = 0;
};

// reads can split or merge frames, so we put them back together here
class OdrUnixSocket final : public OdrFrameTransport {
  public:
    OdrUnixSocket() = default;
    ~OdrUnixSocket() override;
    OdrUnixSocket(const OdrUnixSocket &) = delete;
    OdrUnixSocket &operator=(const OdrUnixSocket &) = delete;
    OdrUnixSocket(OdrUnixSocket &&other) noexcept;
    OdrUnixSocket &operator=(OdrUnixSocket &&other) noexcept;

    [[nodiscard]] bool connect(std::string_view endpoint) override;
    void close() noexcept override;
    [[nodiscard]] bool connected() const noexcept override {
        return descriptor != invalid_handle;
    }
    [[nodiscard]] std::intptr_t native_handle() const noexcept {
        return descriptor;
    }
    [[nodiscard]] int last_error() const noexcept override {
        return socket_error;
    }

    [[nodiscard]] bool send_frame(std::span<const std::byte> frame) override;
    [[nodiscard]] std::optional<std::vector<std::byte>>
    receive_frame() override;

  private:
    [[nodiscard]] bool receive_exact(std::span<std::byte> destination);

    static constexpr std::intptr_t invalid_handle = -1;
    std::intptr_t descriptor{ invalid_handle };
    int socket_error{};
};

} // namespace libnks
