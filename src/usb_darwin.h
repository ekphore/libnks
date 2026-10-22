// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "libnks/transport.h"
#include <IOKit/usb/IOUSBLib.h>
#include <array>
#include <atomic>
#include <string>
#include <string_view>

namespace libnks {
// bulk interface only, opening the whole device can detach or re-enumerate the
// CoreMIDI interfaces next to it
class DarwinUsbInterface {
  public:
    ~DarwinUsbInterface() { close(); }
    bool open(const KksUsbDeviceLocation &location);
    void close() noexcept;
    bool connected() const noexcept { return interface != nullptr; }
    int transfer(std::uint8_t endpoint, unsigned char *bytes, int length,
            int *transferred, unsigned timeout_ms);
    int error() const noexcept { return last_error; }
    std::string_view error_stage() const noexcept { return stage; }
    std::string_view error_detail() const noexcept;

  private:
    IOUSBInterfaceInterface300 **interface{};
    std::array<UInt8, 256> pipes{};
    int last_error{};
    std::string detail;
    std::atomic<std::uint64_t> transfer_failure{};
    std::string_view stage{ "USB interface enumeration" };
};
} // namespace libnks
