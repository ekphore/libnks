// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libnks/transport.h"

namespace libnks {

struct MidiPortInfo {
    std::string id;
    std::string name;
};

// on macOS an ID/name picks a CoreMIDI endpoint pair, on linux it's a raw MIDI
// path like /dev/snd/midiC1D0
class MidiDevice {
  public:
    struct Impl;
    MidiDevice();
    ~MidiDevice();
    MidiDevice(MidiDevice &&) noexcept;
    MidiDevice &operator=(MidiDevice &&) noexcept;
    MidiDevice(const MidiDevice &) = delete;
    MidiDevice &operator=(const MidiDevice &) = delete;

    [[nodiscard]] static bool available() noexcept;
    [[nodiscard]] static std::vector<MidiPortInfo> enumerate();
    [[nodiscard]] bool open(std::string_view id_or_name);
    // input-only ports don't need a matching destination
    [[nodiscard]] bool open_input(std::string_view id_or_name);
    void close() noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool send(std::span<const std::uint8_t> message);
    [[nodiscard]] bool receive(std::vector<std::uint8_t> &message,
            std::chrono::milliseconds timeout);
    [[nodiscard]] int last_error() const noexcept;

  private:
    bool open_mode(std::string_view id_or_name, bool input_only);
    std::unique_ptr<Impl> impl;
};

// raw USB-MIDI for when the OS MIDI service isn't there, eg a privileged
// macOS probe. MK3 DAW is on cable 1
class UsbMidiDevice {
  public:
    struct Impl;
    UsbMidiDevice();
    ~UsbMidiDevice();
    UsbMidiDevice(UsbMidiDevice &&) noexcept;
    UsbMidiDevice &operator=(UsbMidiDevice &&) noexcept;
    UsbMidiDevice(const UsbMidiDevice &) = delete;
    UsbMidiDevice &operator=(const UsbMidiDevice &) = delete;

    [[nodiscard]] static bool available() noexcept;
    [[nodiscard]] bool open(
            const KksUsbDeviceLocation &location, std::uint8_t cable = 1);
    void close() noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool send(std::span<const std::uint8_t> message);
    [[nodiscard]] bool receive(std::vector<std::uint8_t> &message,
            std::chrono::milliseconds timeout);
    [[nodiscard]] int last_error() const noexcept;

  private:
    std::unique_ptr<Impl> impl;
};

} // namespace libnks
