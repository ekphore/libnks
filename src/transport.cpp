// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "libnks/transport.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

#include <afunix.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace libnks {

bool KksPrimaryWriteBatch::append(const KksRoutedFrame &frame) {
    if (frame.channel != KksUsbChannel::primary)
        return false;
    bytes.insert(bytes.end(), frame.bytes.begin(), frame.bytes.end());
    return true;
}

std::vector<std::byte> KksPrimaryWriteBatch::take() noexcept {
    std::vector<std::byte> result;
    result.swap(bytes);
    return result;
}

bool is_supported_kks_usb_device(
        std::uint16_t vendor_id, std::uint16_t product_id) noexcept {
    return vendor_id == kks_usb_vendor_id &&
           std::ranges::find(kks_usb_product_ids, product_id) !=
                   kks_usb_product_ids.end();
}

bool is_kks_usb_interface(std::uint8_t interface_subclass,
        std::span<const KksUsbEndpointDescriptor> endpoints) noexcept {
    if (interface_subclass != kks_usb_interface_subclass)
        return false;
    constexpr std::uint8_t transfer_type_mask = 0x03;
    constexpr std::uint8_t bulk_transfer = 0x02;
    const auto has_bulk = [&](std::uint8_t address) {
        return std::ranges::any_of(endpoints, [&](const auto &endpoint) {
            return endpoint.address == address &&
                   (endpoint.attributes & transfer_type_mask) == bulk_transfer;
        });
    };
    return has_bulk(kks_usb_in_endpoint) &&
           has_bulk(kks_usb_heartbeat_endpoint) &&
           has_bulk(kks_usb_primary_endpoint);
}

namespace {
#if defined(_WIN32)
int read_socket_error() noexcept { return ::WSAGetLastError(); }
bool interrupted(int error) noexcept { return error == WSAEINTR; }
void close_socket(std::intptr_t handle) noexcept {
    (void)::closesocket(static_cast<SOCKET>(handle));
}
bool ensure_socket_runtime() noexcept {
    static const bool initialized = [] {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
}
constexpr int not_connected_error = WSAENOTCONN;
constexpr int broken_pipe_error = WSAECONNRESET;
constexpr int message_size_error = WSAEMSGSIZE;
#else
int read_socket_error() noexcept { return errno; }
bool interrupted(int error) noexcept { return error == EINTR; }
void close_socket(std::intptr_t handle) noexcept {
    (void)::close(static_cast<int>(handle));
}
bool ensure_socket_runtime() noexcept { return true; }
constexpr int not_connected_error = ENOTCONN;
constexpr int broken_pipe_error = EPIPE;
constexpr int message_size_error = EMSGSIZE;
#endif
} // namespace

OdrUnixSocket::~OdrUnixSocket() { close(); }

OdrUnixSocket::OdrUnixSocket(OdrUnixSocket &&other) noexcept
    : descriptor(other.descriptor), socket_error(other.socket_error) {
    other.descriptor = invalid_handle;
    other.socket_error = 0;
}

OdrUnixSocket &OdrUnixSocket::operator=(OdrUnixSocket &&other) noexcept {
    if (this == &other)
        return *this;
    close();
    descriptor = other.descriptor;
    socket_error = other.socket_error;
    other.descriptor = invalid_handle;
    other.socket_error = 0;
    return *this;
}

bool OdrUnixSocket::connect(std::string_view endpoint) {
    close();
    if (!ensure_socket_runtime()) {
        socket_error = read_socket_error();
        return false;
    }
    sockaddr_un address{};
    if (endpoint.empty() || endpoint.size() >= sizeof(address.sun_path)) {
#if defined(_WIN32)
        socket_error = WSAEINVAL;
#else
        socket_error = ENAMETOOLONG;
#endif
        return false;
    }
    address.sun_family = AF_UNIX;
#if defined(__APPLE__)
    address.sun_len = static_cast<std::uint8_t>(
            offsetof(sockaddr_un, sun_path) + endpoint.size() + 1);
#endif
    std::memcpy(address.sun_path, endpoint.data(), endpoint.size());
    address.sun_path[endpoint.size()] = '\0';
    const auto socket_handle = ::socket(AF_UNIX, SOCK_STREAM, 0);
#if defined(_WIN32)
    if (socket_handle == INVALID_SOCKET) {
#else
    if (socket_handle < 0) {
#endif
        socket_error = read_socket_error();
        return false;
    }
    descriptor = static_cast<std::intptr_t>(socket_handle);
#if defined(SO_NOSIGPIPE)
    const int enabled = 1;
    (void)::setsockopt(static_cast<int>(descriptor), SOL_SOCKET, SO_NOSIGPIPE,
            &enabled, sizeof(enabled));
#endif
#if defined(_WIN32)
    const auto length = static_cast<int>(
            offsetof(sockaddr_un, sun_path) + endpoint.size() + 1);
    const auto connect_result = ::connect(static_cast<SOCKET>(descriptor),
            reinterpret_cast<const sockaddr *>(&address), length);
#else
    const auto length = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + endpoint.size() + 1);
    const auto connect_result = ::connect(static_cast<int>(descriptor),
            reinterpret_cast<const sockaddr *>(&address), length);
#endif
    if (connect_result != 0) {
        socket_error = read_socket_error();
        close();
        return false;
    }
    socket_error = 0;
    return true;
}

void OdrUnixSocket::close() noexcept {
    if (connected()) {
        close_socket(descriptor);
        descriptor = invalid_handle;
    }
}

bool OdrUnixSocket::send_frame(std::span<const std::byte> frame) {
    if (!connected()) {
        socket_error = not_connected_error;
        return false;
    }
    std::size_t offset = 0;
    while (offset < frame.size()) {
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
#if defined(_WIN32)
        const auto sent = ::send(static_cast<SOCKET>(descriptor),
                reinterpret_cast<const char *>(frame.data() + offset),
                static_cast<int>(frame.size() - offset), flags);
#else
        const auto sent = ::send(static_cast<int>(descriptor),
                frame.data() + offset, frame.size() - offset, flags);
#endif
        const auto error = sent < 0 ? read_socket_error() : 0;
        if (sent < 0 && interrupted(error))
            continue;
        if (sent <= 0) {
            socket_error = sent < 0 ? error : broken_pipe_error;
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    socket_error = 0;
    return true;
}

bool OdrUnixSocket::receive_exact(std::span<std::byte> destination) {
    std::size_t offset = 0;
    while (offset < destination.size()) {
#if defined(_WIN32)
        const auto received = ::recv(static_cast<SOCKET>(descriptor),
                reinterpret_cast<char *>(destination.data() + offset),
                static_cast<int>(destination.size() - offset), 0);
#else
        const auto received = ::recv(static_cast<int>(descriptor),
                destination.data() + offset, destination.size() - offset, 0);
#endif
        const auto error = received < 0 ? read_socket_error() : 0;
        if (received < 0 && interrupted(error))
            continue;
        if (received <= 0) {
            socket_error = received < 0 ? error : 0;
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    return true;
}

std::optional<std::vector<std::byte>> OdrUnixSocket::receive_frame() {
    if (!connected()) {
        socket_error = not_connected_error;
        return std::nullopt;
    }
    std::array<std::byte, 4> header{};
    if (!receive_exact(header))
        return std::nullopt;
    const auto payload_size =
            std::to_integer<std::uint8_t>(header[0]) |
            (std::uint32_t(std::to_integer<std::uint8_t>(header[1])) << 8) |
            (std::uint32_t(std::to_integer<std::uint8_t>(header[2])) << 16) |
            (std::uint32_t(std::to_integer<std::uint8_t>(header[3])) << 24);
    if (payload_size > RpcFrameDecoder::max_payload_size) {
        socket_error = message_size_error;
        return std::nullopt;
    }
    std::vector<std::byte> frame(payload_size + 4);
    std::ranges::copy(header, frame.begin());
    if (!receive_exact(std::span(frame).subspan(4)))
        return std::nullopt;
    socket_error = 0;
    return frame;
}

} // namespace libnks
