// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "libnks/midi.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#elif defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <mmsystem.h>
#endif

#include <libusb.h>

namespace libnks {

struct MidiDevice::Impl {
    std::atomic<int> error{};
#if defined(__APPLE__)
    MIDIClientRef client{};
    MIDIPortRef input{};
    MIDIPortRef output{};
    MIDIEndpointRef source{};
    MIDIEndpointRef destination{};
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::vector<std::uint8_t>> received;
#elif defined(__linux__)
    int fd{ -1 };
#elif defined(_WIN32)
    HMIDIIN input{};
    HMIDIOUT output{};
    std::atomic<bool> closing{};
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::vector<std::uint8_t>> received;
    std::array<std::uint8_t, 1024> sysex_buffer{};
    MIDIHDR sysex_header{};
#endif
};

#if defined(__APPLE__)
namespace {
std::string endpoint_id(MIDIEndpointRef endpoint) {
    SInt32 unique{};
    (void)MIDIObjectGetIntegerProperty(
            endpoint, kMIDIPropertyUniqueID, &unique);
    return "coremidi:" + std::to_string(unique) + ":" +
           std::to_string(endpoint);
}

std::string endpoint_name(MIDIEndpointRef endpoint) {
    CFStringRef value{};
    if (MIDIObjectGetStringProperty(
                endpoint, kMIDIPropertyDisplayName, &value) != noErr ||
            !value)
        return {};
    char bytes[512]{};
    const auto ok = CFStringGetCString(
            value, bytes, sizeof(bytes), kCFStringEncodingUTF8);
    CFRelease(value);
    return ok ? std::string(bytes) : std::string{};
}

void midi_read(const MIDIPacketList *packets, void *context, void *) {
    auto *impl = static_cast<MidiDevice::Impl *>(context);
    const auto *packet = &packets->packet[0];
    {
        std::lock_guard lock(impl->mutex);
        for (UInt32 i = 0; i < packets->numPackets; ++i) {
            impl->received.emplace_back(
                    packet->data, packet->data + packet->length);
            packet = MIDIPacketNext(packet);
        }
    }
    impl->ready.notify_all();
}
} // namespace
#endif

#if defined(_WIN32)
namespace {
void CALLBACK midi_in(HMIDIIN input, UINT message, DWORD_PTR context,
        DWORD_PTR parameter1, DWORD_PTR) {
    auto *impl = reinterpret_cast<MidiDevice::Impl *>(context);
    if (!impl)
        return;
    if (message == MIM_DATA) {
        const auto packed = static_cast<DWORD>(parameter1);
        const auto status = static_cast<std::uint8_t>(packed & 0xff);
        const auto length = status >= 0xc0 && status < 0xe0 ? 2U : 3U;
        std::vector<std::uint8_t> bytes;
        bytes.reserve(length);
        for (unsigned i = 0; i < length; ++i)
            bytes.push_back(
                    static_cast<std::uint8_t>((packed >> (8 * i)) & 0xff));
        {
            std::lock_guard lock(impl->mutex);
            impl->received.push_back(std::move(bytes));
        }
        impl->ready.notify_all();
    } else if (message == MIM_LONGDATA) {
        auto *header = reinterpret_cast<MIDIHDR *>(parameter1);
        if (header && header->dwBytesRecorded) {
            std::vector<std::uint8_t> bytes(
                    reinterpret_cast<std::uint8_t *>(header->lpData),
                    reinterpret_cast<std::uint8_t *>(header->lpData) +
                            header->dwBytesRecorded);
            {
                std::lock_guard lock(impl->mutex);
                impl->received.push_back(std::move(bytes));
            }
            impl->ready.notify_all();
        }
        if (header && input && !impl->closing.load()) {
            header->dwBytesRecorded = 0;
            (void)midiInAddBuffer(input, header, sizeof(*header));
        }
    }
}

template <typename Caps, typename Getter>
std::string winmm_name(UINT index, Getter getter) {
    Caps caps{};
    if (getter(index, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
        return {};
#if defined(UNICODE)
    char bytes[512]{};
    const auto count = WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, bytes,
            sizeof(bytes), nullptr, nullptr);
    return count > 0 ? std::string(bytes) : std::string{};
#else
    return caps.szPname;
#endif
}
} // namespace
#endif

MidiDevice::MidiDevice() : impl(std::make_unique<Impl>()) {}
MidiDevice::~MidiDevice() { close(); }
MidiDevice::MidiDevice(MidiDevice &&) noexcept = default;
MidiDevice &MidiDevice::operator=(MidiDevice &&other) noexcept {
    if (this != &other) {
        close();
        impl = std::move(other.impl);
    }
    return *this;
}

bool MidiDevice::available() noexcept {
#if defined(__APPLE__) || defined(__linux__) || defined(_WIN32)
    return true;
#else
    return false;
#endif
}

std::vector<MidiPortInfo> MidiDevice::enumerate() {
    std::vector<MidiPortInfo> result;
#if defined(__APPLE__)
    MIDIClientRef enumeration_client{};
    if (MIDIClientCreate(CFSTR("libnks MIDI enumeration"), nullptr, nullptr,
                &enumeration_client) != noErr)
        return result;
    const auto append_endpoint = [&result](MIDIEndpointRef endpoint) {
        const auto name = endpoint_name(endpoint);
        if (!name.empty() &&
                std::ranges::none_of(result, [&](const auto &port) {
                    return port.id == endpoint_id(endpoint);
                }))
            result.push_back({ endpoint_id(endpoint), name });
    };
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i) {
        append_endpoint(MIDIGetSource(i));
    }
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i) {
        const auto endpoint = MIDIGetDestination(i);
        // keep the source's ID for this duplex pair, the destination one can't
        // be opened on its own
        if (std::ranges::none_of(result, [&](const auto &port) {
                return port.name == endpoint_name(endpoint);
            }))
            append_endpoint(endpoint);
    }
    std::ranges::sort(result, {}, &MidiPortInfo::name);
    (void)MIDIClientDispose(enumeration_client);
#elif defined(__linux__)
    std::error_code error;
    const std::filesystem::path directory{ "/dev/snd" };
    for (std::filesystem::directory_iterator it(directory, error), end;
            !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (name.starts_with("midiC"))
            result.push_back({ it->path().string(), name });
    }
    std::ranges::sort(result, {}, &MidiPortInfo::id);
#elif defined(_WIN32)
    const auto append = [&result](std::string name) {
        if (!name.empty() &&
                std::ranges::none_of(result,
                        [&](const auto &port) { return port.name == name; }))
            result.push_back({ name, name });
    };
    for (UINT i = 0; i < midiInGetNumDevs(); ++i)
        append(winmm_name<MIDIINCAPS>(i, midiInGetDevCaps));
    for (UINT i = 0; i < midiOutGetNumDevs(); ++i)
        append(winmm_name<MIDIOUTCAPS>(i, midiOutGetDevCaps));
    std::ranges::sort(result, {}, &MidiPortInfo::name);
#endif
    return result;
}

bool MidiDevice::open(std::string_view id_or_name) {
    return open_mode(id_or_name, false);
}

bool MidiDevice::open_input(std::string_view id_or_name) {
    return open_mode(id_or_name, true);
}

bool MidiDevice::open_mode(std::string_view id_or_name, bool input_only) {
    if (!impl)
        impl = std::make_unique<Impl>();
    close();
#if defined(__APPLE__)
    impl->error = MIDIClientCreate(
            CFSTR("libnks GenericDAW"), nullptr, nullptr, &impl->client);
    if (impl->error != noErr)
        return false;
    impl->error = MIDIInputPortCreate(impl->client, CFSTR("libnks input"),
            midi_read, impl.get(), &impl->input);
    if (impl->error == noErr && !input_only)
        impl->error = MIDIOutputPortCreate(
                impl->client, CFSTR("libnks output"), &impl->output);
    for (ItemCount i = 0; impl->error == noErr && i < MIDIGetNumberOfSources();
            ++i) {
        const auto endpoint = MIDIGetSource(i);
        const auto name = endpoint_name(endpoint);
        if (endpoint_id(endpoint) == id_or_name || name == id_or_name ||
                (!id_or_name.starts_with("coremidi:") &&
                        name.find(id_or_name) != std::string::npos)) {
            impl->source = endpoint;
            break;
        }
    }
    for (ItemCount i = 0;
            impl->error == noErr && i < MIDIGetNumberOfDestinations(); ++i) {
        const auto endpoint = MIDIGetDestination(i);
        const auto name = endpoint_name(endpoint);
        if (endpoint_id(endpoint) == id_or_name || name == id_or_name ||
                (impl->source && name == endpoint_name(impl->source)) ||
                (!id_or_name.starts_with("coremidi:") &&
                        name.find(id_or_name) != std::string::npos)) {
            impl->destination = endpoint;
            break;
        }
    }
    if (impl->error != noErr || !impl->source ||
            (!input_only && !impl->destination)) {
        if (impl->error == noErr)
            impl->error = kMIDIObjectNotFound;
        close();
        return false;
    }
    impl->error = MIDIPortConnectSource(impl->input, impl->source, nullptr);
    if (impl->error != noErr) {
        close();
        return false;
    }
    return true;
#elif defined(__linux__)
    impl->fd = ::open(std::string(id_or_name).c_str(),
            (input_only ? O_RDONLY : O_RDWR) | O_NONBLOCK);
    if (impl->fd < 0) {
        impl->error = errno;
        return false;
    }
    return true;
#elif defined(_WIN32)
    impl->closing = false;
    UINT input_id = UINT_MAX;
    UINT output_id = UINT_MAX;
    for (UINT i = 0; i < midiInGetNumDevs(); ++i) {
        const auto name = winmm_name<MIDIINCAPS>(i, midiInGetDevCaps);
        if (name == id_or_name || name.find(id_or_name) != std::string::npos) {
            input_id = i;
            break;
        }
    }
    for (UINT i = 0; i < midiOutGetNumDevs(); ++i) {
        const auto name = winmm_name<MIDIOUTCAPS>(i, midiOutGetDevCaps);
        if (name == id_or_name || name.find(id_or_name) != std::string::npos) {
            output_id = i;
            break;
        }
    }
    if (input_id == UINT_MAX || (!input_only && output_id == UINT_MAX)) {
        impl->error = MMSYSERR_BADDEVICEID;
        return false;
    }
    impl->error = midiInOpen(&impl->input, input_id,
            reinterpret_cast<DWORD_PTR>(midi_in),
            reinterpret_cast<DWORD_PTR>(impl.get()), CALLBACK_FUNCTION);
    if (impl->error == MMSYSERR_NOERROR && !input_only)
        impl->error =
                midiOutOpen(&impl->output, output_id, 0, 0, CALLBACK_NULL);
    if (impl->error != MMSYSERR_NOERROR) {
        close();
        return false;
    }
    impl->sysex_header.lpData =
            reinterpret_cast<LPSTR>(impl->sysex_buffer.data());
    impl->sysex_header.dwBufferLength = impl->sysex_buffer.size();
    impl->error = midiInPrepareHeader(
            impl->input, &impl->sysex_header, sizeof(impl->sysex_header));
    if (impl->error == MMSYSERR_NOERROR)
        impl->error = midiInAddBuffer(
                impl->input, &impl->sysex_header, sizeof(impl->sysex_header));
    if (impl->error == MMSYSERR_NOERROR)
        impl->error = midiInStart(impl->input);
    if (impl->error != MMSYSERR_NOERROR) {
        close();
        return false;
    }
    return true;
#else
    (void)id_or_name;
    (void)input_only;
    impl->error = -1;
    return false;
#endif
}

void MidiDevice::close() noexcept {
    if (!impl)
        return;
#if defined(__APPLE__)
    if (impl->input && impl->source)
        (void)MIDIPortDisconnectSource(impl->input, impl->source);
    if (impl->input)
        (void)MIDIPortDispose(impl->input);
    if (impl->output)
        (void)MIDIPortDispose(impl->output);
    if (impl->client)
        (void)MIDIClientDispose(impl->client);
    impl->client = 0;
    impl->input = 0;
    impl->output = 0;
    impl->source = 0;
    impl->destination = 0;
    std::lock_guard lock(impl->mutex);
    impl->received.clear();
#elif defined(__linux__)
    if (impl->fd >= 0)
        (void)::close(impl->fd);
    impl->fd = -1;
#elif defined(_WIN32)
    impl->closing = true;
    if (impl->input) {
        (void)midiInStop(impl->input);
        (void)midiInReset(impl->input);
        if (impl->sysex_header.dwFlags & MHDR_PREPARED)
            (void)midiInUnprepareHeader(impl->input, &impl->sysex_header,
                    sizeof(impl->sysex_header));
        (void)midiInClose(impl->input);
    }
    if (impl->output) {
        (void)midiOutReset(impl->output);
        (void)midiOutClose(impl->output);
    }
    impl->input = nullptr;
    impl->output = nullptr;
    impl->sysex_header = {};
    std::lock_guard lock(impl->mutex);
    impl->received.clear();
#endif
}

bool MidiDevice::connected() const noexcept {
    if (!impl)
        return false;
#if defined(__APPLE__)
    return impl->client && impl->source;
#elif defined(__linux__)
    return impl->fd >= 0;
#elif defined(_WIN32)
    return impl->input != nullptr;
#else
    return false;
#endif
}

bool MidiDevice::send(std::span<const std::uint8_t> message) {
    if (!connected() || message.empty())
        return false;
#if defined(__APPLE__)
    if (!impl->output || !impl->destination)
        return false;
    std::vector<std::uint8_t> storage(
            sizeof(MIDIPacketList) + message.size() + 256);
    auto *list = reinterpret_cast<MIDIPacketList *>(storage.data());
    auto *packet = MIDIPacketListInit(list);
    if (!MIDIPacketListAdd(list, storage.size(), packet, 0, message.size(),
                message.data())) {
        impl->error = kMIDIMessageSendErr;
        return false;
    }
    impl->error = MIDISend(impl->output, impl->destination, list);
    return impl->error == noErr;
#elif defined(__linux__)
    std::size_t offset{};
    while (offset < message.size()) {
        const auto count = ::write(
                impl->fd, message.data() + offset, message.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        impl->error = errno;
        return false;
    }
    return true;
#elif defined(_WIN32)
    if (!impl->output)
        return false;
    if (message.front() != 0xf0 && message.size() <= 3) {
        DWORD packed{};
        for (std::size_t i = 0; i < message.size(); ++i)
            packed |= static_cast<DWORD>(message[i]) << (8 * i);
        impl->error = midiOutShortMsg(impl->output, packed);
        return impl->error == MMSYSERR_NOERROR;
    }
    std::vector<std::uint8_t> bytes(message.begin(), message.end());
    MIDIHDR header{};
    header.lpData = reinterpret_cast<LPSTR>(bytes.data());
    header.dwBufferLength = static_cast<DWORD>(bytes.size());
    impl->error = midiOutPrepareHeader(impl->output, &header, sizeof(header));
    if (impl->error != MMSYSERR_NOERROR)
        return false;
    impl->error = midiOutLongMsg(impl->output, &header, sizeof(header));
    if (impl->error == MMSYSERR_NOERROR) {
        while ((header.dwFlags & MHDR_DONE) == 0)
            Sleep(1);
    }
    const auto send_error = impl->error.load();
    const auto unprepare =
            midiOutUnprepareHeader(impl->output, &header, sizeof(header));
    if (send_error == MMSYSERR_NOERROR)
        impl->error = unprepare;
    return impl->error == MMSYSERR_NOERROR;
#else
    return false;
#endif
}

bool MidiDevice::receive(
        std::vector<std::uint8_t> &message, std::chrono::milliseconds timeout) {
#if defined(__APPLE__)
    std::unique_lock lock(impl->mutex);
    if (!impl->ready.wait_for(
                lock, timeout, [&] { return !impl->received.empty(); }))
        return false;
    message = std::move(impl->received.front());
    impl->received.pop_front();
    return true;
#elif defined(__linux__)
    pollfd descriptor{ impl->fd, POLLIN, 0 };
    const auto ready =
            ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
    if (ready <= 0) {
        if (ready < 0)
            impl->error = errno;
        return false;
    }
    std::uint8_t bytes[1024];
    const auto count = ::read(impl->fd, bytes, sizeof(bytes));
    if (count <= 0) {
        impl->error = errno;
        return false;
    }
    message.assign(bytes, bytes + count);
    return true;
#elif defined(_WIN32)
    std::unique_lock lock(impl->mutex);
    if (!impl->ready.wait_for(
                lock, timeout, [&] { return !impl->received.empty(); }))
        return false;
    message = std::move(impl->received.front());
    impl->received.pop_front();
    return true;
#else
    (void)message;
    (void)timeout;
    return false;
#endif
}

int MidiDevice::last_error() const noexcept {
    return impl ? impl->error.load() : 0;
}

struct UsbMidiDevice::Impl {
    std::atomic<int> error{};
    libusb_context *context{};
    libusb_device_handle *handle{};
    int interface_number{ -1 };
    std::uint8_t input_endpoint{};
    std::uint8_t output_endpoint{};
    std::uint8_t cable{ 1 };
};

UsbMidiDevice::UsbMidiDevice() : impl(std::make_unique<Impl>()) {
    impl->error = libusb_init(&impl->context);
}

UsbMidiDevice::~UsbMidiDevice() {
    close();
    if (impl->context)
        libusb_exit(impl->context);
}
UsbMidiDevice::UsbMidiDevice(UsbMidiDevice &&) noexcept = default;
UsbMidiDevice &UsbMidiDevice::operator=(UsbMidiDevice &&) noexcept = default;

bool UsbMidiDevice::available() noexcept { return true; }

bool UsbMidiDevice::open(
        const KksUsbDeviceLocation &location, std::uint8_t cable) {
    close();
    if (!impl->context || cable > 15) {
        impl->error = LIBUSB_ERROR_INVALID_PARAM;
        return false;
    }
    libusb_device **devices{};
    const auto count = libusb_get_device_list(impl->context, &devices);
    if (count < 0) {
        impl->error = static_cast<int>(count);
        return false;
    }
    libusb_device *selected{};
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[i], &descriptor) == 0 &&
                descriptor.idVendor == location.vendor_id &&
                descriptor.idProduct == location.product_id &&
                libusb_get_bus_number(devices[i]) == location.bus &&
                libusb_get_device_address(devices[i]) == location.address) {
            selected = devices[i];
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
    int alternate_setting{};
    if (impl->error == 0) {
        for (std::uint8_t i = 0;
                i < config->bNumInterfaces && impl->interface_number < 0; ++i) {
            const auto &interface = config->interface[i];
            for (int a = 0; a < interface.num_altsetting; ++a) {
                const auto &alternate = interface.altsetting[a];
                if (alternate.bInterfaceClass != LIBUSB_CLASS_AUDIO ||
                        alternate.bInterfaceSubClass != 3)
                    continue;
                std::uint8_t input{};
                std::uint8_t output{};
                for (std::uint8_t e = 0; e < alternate.bNumEndpoints; ++e) {
                    const auto endpoint =
                            alternate.endpoint[e].bEndpointAddress;
                    if (endpoint & LIBUSB_ENDPOINT_IN)
                        input = endpoint;
                    else
                        output = endpoint;
                }
                if (input && output) {
                    impl->interface_number = alternate.bInterfaceNumber;
                    alternate_setting = alternate.bAlternateSetting;
                    impl->input_endpoint = input;
                    impl->output_endpoint = output;
                    break;
                }
            }
        }
    }
    if (impl->interface_number < 0) {
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
    impl->error = libusb_claim_interface(impl->handle, impl->interface_number);
    if (impl->error == 0 && alternate_setting)
        impl->error = libusb_set_interface_alt_setting(
                impl->handle, impl->interface_number, alternate_setting);
    if (impl->error != 0) {
        close();
        return false;
    }
    impl->cable = cable;
    return true;
}

void UsbMidiDevice::close() noexcept {
    if (impl->handle) {
        if (impl->interface_number >= 0)
            (void)libusb_release_interface(
                    impl->handle, impl->interface_number);
        libusb_close(impl->handle);
    }
    impl->handle = nullptr;
    impl->interface_number = -1;
    impl->input_endpoint = 0;
    impl->output_endpoint = 0;
}

bool UsbMidiDevice::connected() const noexcept {
    return impl->handle != nullptr;
}

bool UsbMidiDevice::send(std::span<const std::uint8_t> message) {
    if (!connected() || message.empty())
        return false;
    std::vector<std::uint8_t> packets;
    if (message.front() == 0xf0) {
        for (std::size_t offset = 0; offset < message.size();) {
            const auto remaining = message.size() - offset;
            const auto count = std::min<std::size_t>(remaining, 3);
            const auto cin = remaining > 3 ? 0x04 : 0x04 + count;
            packets.push_back(
                    static_cast<std::uint8_t>((impl->cable << 4) | cin));
            for (std::size_t i = 0; i < 3; ++i)
                packets.push_back(i < count ? message[offset + i] : 0);
            offset += count;
        }
    } else if (message.size() == 3 && (message.front() & 0xf0) == 0xb0) {
        packets = { static_cast<std::uint8_t>((impl->cable << 4) | 0x0b),
            message[0], message[1], message[2] };
    } else {
        impl->error = LIBUSB_ERROR_INVALID_PARAM;
        return false;
    }
    std::size_t offset{};
    while (offset < packets.size()) {
        int transferred{};
        impl->error = libusb_bulk_transfer(impl->handle, impl->output_endpoint,
                packets.data() + offset,
                static_cast<int>(packets.size() - offset), &transferred, 2'000);
        if (transferred > 0)
            offset += static_cast<std::size_t>(transferred);
        if (impl->error == 0)
            continue;
        if (impl->error == LIBUSB_ERROR_TIMEOUT && transferred > 0)
            continue;
        return false;
    }
    return true;
}

bool UsbMidiDevice::receive(
        std::vector<std::uint8_t> &message, std::chrono::milliseconds timeout) {
    if (!connected())
        return false;
    std::array<std::uint8_t, 1024> packets{};
    int transferred{};
    impl->error = libusb_bulk_transfer(impl->handle, impl->input_endpoint,
            packets.data(), packets.size(), &transferred,
            static_cast<unsigned>(std::max<std::int64_t>(1, timeout.count())));
    if (impl->error != 0)
        return false;
    message.clear();
    for (int offset = 0; offset + 3 < transferred; offset += 4) {
        if ((packets[offset] >> 4) != impl->cable)
            continue;
        const auto cin = packets[offset] & 0x0f;
        const auto length = cin == 0x05                                   ? 1
                            : cin == 0x06                                 ? 2
                            : (cin == 0x04 || cin == 0x07 || cin == 0x0b) ? 3
                                                                          : 0;
        message.insert(message.end(), packets.begin() + offset + 1,
                packets.begin() + offset + 1 + length);
    }
    return !message.empty();
}

int UsbMidiDevice::last_error() const noexcept { return impl->error.load(); }

} // namespace libnks
