// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "libnks/daw.h"

#include <algorithm>
#include <array>

namespace libnks {
namespace {
constexpr std::array<std::uint8_t, 10> sysex_header{ 0xf0, 0x00, 0x21, 0x09,
    0x00, 0x00, 0x44, 0x43, 0x01, 0x00 };

constexpr bool valid_input_cc(std::uint8_t controller) noexcept {
    if ((controller >= 0x50 && controller <= 0x5f) ||
            (controller >= 0x70 && controller <= 0x77))
        return true;
    switch (controller) {
        case 0x01:
        case 0x02:
        case 0x06:
        case 0x08:
        case 0x10:
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17:
        case 0x18:
        case 0x20:
        case 0x21:
        case 0x22:
        case 0x23:
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x36:
        case 0x66:
        case 0x67:
        case 0x68:
        case 0x69:
            return true;
        default:
            return false;
    }
}
} // namespace

std::vector<std::uint8_t> DawMidiCodec::control_change(
        std::uint8_t controller, std::uint8_t value) {
    return { 0xbf, static_cast<std::uint8_t>(controller & 0x7f),
        static_cast<std::uint8_t>(value & 0x7f) };
}

std::vector<std::uint8_t> DawMidiCodec::sysex(
        std::uint8_t state_id, std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> result(sysex_header.begin(), sysex_header.end());
    result.push_back(static_cast<std::uint8_t>(state_id & 0x7f));
    for (const auto byte : payload)
        result.push_back(static_cast<std::uint8_t>(byte & 0x7f));
    result.push_back(0xf7);
    return result;
}

std::vector<std::uint8_t> DawMidiCodec::sysex(DawState state,
        std::uint8_t value, std::uint8_t index,
        std::span<const std::uint8_t> info) {
    std::vector<std::uint8_t> indexed;
    indexed.reserve(info.size() + 2);
    indexed.push_back(value & 0x7f);
    indexed.push_back(index & 0x7f);
    indexed.insert(indexed.end(), info.begin(), info.end());
    return sysex(static_cast<std::uint8_t>(state), indexed);
}

std::vector<std::uint8_t> DawMidiCodec::daw_identity(
        std::uint8_t major, std::uint8_t minor, std::string_view daw_name) {
    std::vector<std::uint8_t> payload{ static_cast<std::uint8_t>(major & 0x7f),
        static_cast<std::uint8_t>(minor & 0x7f) };
    payload.reserve(daw_name.size() + 2);
    for (const unsigned char c : daw_name)
        payload.push_back(c < 0x80 ? c : static_cast<std::uint8_t>('?'));
    return sysex(identity, payload);
}

std::optional<DawMidiEvent> DawMidiCodec::decode(
        std::span<const std::uint8_t> message) {
    if (message.size() == 3 && message[0] == 0xbf && message[1] < 0x80 &&
            message[2] < 0x80 && valid_input_cc(message[1]))
        return DawMidiCc{ message[1], message[2] };
    if (message.size() < sysex_header.size() + 2 || message.back() != 0xf7 ||
            !std::equal(
                    sysex_header.begin(), sysex_header.end(), message.begin()))
        return std::nullopt;
    DawMidiSysEx event;
    event.state_id = message[sysex_header.size()];
    event.payload.assign(
            message.begin() + sysex_header.size() + 1, message.end() - 1);
    if (event.state_id != identity && event.payload.size() >= 2) {
        event.value = event.payload[0];
        event.index = event.payload[1];
        event.payload.erase(event.payload.begin(), event.payload.begin() + 2);
    }
    return event;
}

int DawMidiCodec::relative_delta(std::uint8_t value) noexcept {
    value &= 0x7f;
    return value < 0x40    ? static_cast<int>(value)
           : value == 0x40 ? 0
                           : static_cast<int>(value) - 0x80;
}

std::vector<DawMidiEvent> DawMidiStreamDecoder::push(
        std::span<const std::uint8_t> bytes) {
    for (const auto byte : bytes)
        if (byte < 0xf8)
            pending.push_back(byte); // real-time is transparent
    std::vector<DawMidiEvent> events;
    std::size_t consumed{};
    while (consumed < pending.size()) {
        const auto byte = pending[consumed];
        if (byte == 0xf0) {
            const auto end = std::find(pending.begin() + consumed,
                    pending.end(), std::uint8_t{ 0xf7 });
            if (end == pending.end())
                break;
            const auto length = static_cast<std::size_t>(
                                        end - (pending.begin() + consumed)) +
                                1;
            if (auto event = DawMidiCodec::decode(
                        std::span(pending).subspan(consumed, length)))
                events.push_back(std::move(*event));
            consumed += length;
            running_status = 0;
            continue;
        }
        if (byte & 0x80) {
            if (byte != 0xbf) {
                ++consumed;
                running_status = 0;
                continue;
            }
            if (pending.size() - consumed < 3)
                break;
            if (auto event = DawMidiCodec::decode(
                        std::span(pending).subspan(consumed, 3)))
                events.push_back(std::move(*event));
            running_status = byte;
            consumed += 3;
            continue;
        }
        if (running_status == 0xbf) {
            if (pending.size() - consumed < 2)
                break;
            const std::array message{ running_status, pending[consumed],
                pending[consumed + 1] };
            if (auto event = DawMidiCodec::decode(message))
                events.push_back(std::move(*event));
            consumed += 2;
            continue;
        }
        ++consumed;
    }
    pending.erase(pending.begin(), pending.begin() + consumed);
    return events;
}

void DawMidiStreamDecoder::reset() noexcept {
    pending.clear();
    running_status = 0;
}

std::vector<std::uint8_t> DawSession::begin() {
    session_state = DawSessionState::hello_sent;
    negotiated.reset();
    decoder.reset();
    return DawMidiCodec::control_change(
            DawMidiCodec::hello, static_cast<std::uint8_t>(requested));
}

std::optional<DawMidiEvent> DawSession::receive(
        std::span<const std::uint8_t> message) {
    auto event = DawMidiCodec::decode(message);
    if (!event)
        return std::nullopt;
    apply(*event);
    return event;
}

void DawSession::apply(const DawMidiEvent &event) {
    if (const auto *cc = std::get_if<DawMidiCc>(&event)) {
        if (cc->controller == DawMidiCodec::hello && cc->value >= 1 &&
                cc->value <= 4) {
            negotiated = static_cast<DawProtocolVersion>(cc->value);
            session_state = DawSessionState::ready;
        } else if (cc->controller == DawMidiCodec::goodbye) {
            negotiated.reset();
            session_state = DawSessionState::disconnected;
        }
    }
}

std::vector<DawMidiEvent> DawSession::receive_stream(
        std::span<const std::uint8_t> bytes) {
    auto events = decoder.push(bytes);
    for (const auto &event : events)
        apply(event);
    return events;
}

std::vector<std::uint8_t> DawSession::end() {
    session_state = DawSessionState::disconnected;
    negotiated.reset();
    decoder.reset();
    return DawMidiCodec::control_change(DawMidiCodec::goodbye, 0);
}

std::vector<std::uint8_t> DawSession::identity_message(std::uint8_t major,
        std::uint8_t minor, std::string_view daw_name) const {
    return DawMidiCodec::daw_identity(major, minor, daw_name);
}

std::vector<std::uint8_t> DawSession::control(
        DawControl controller, std::uint8_t value) const {
    return DawMidiCodec::control_change(controller, value);
}

std::vector<std::uint8_t> DawSession::indexed_control(
        std::uint8_t base, std::uint8_t index, std::uint8_t value) const {
    if (index >= 8)
        return {};
    return DawMidiCodec::control_change(
            static_cast<std::uint8_t>(base + index), value);
}

std::vector<std::uint8_t> DawSession::track_volume(
        std::uint8_t track, std::uint8_t value) const {
    return indexed_control(
            static_cast<std::uint8_t>(DawControl::track_volume_0), track,
            value);
}

std::vector<std::uint8_t> DawSession::track_pan(
        std::uint8_t track, std::uint8_t value) const {
    return indexed_control(
            static_cast<std::uint8_t>(DawControl::track_pan_0), track, value);
}

std::vector<std::uint8_t> DawSession::parameter_value(
        std::uint8_t parameter, std::uint8_t value) const {
    return indexed_control(
            static_cast<std::uint8_t>(DawControl::parameter_value_0), parameter,
            value);
}

std::vector<std::uint8_t> DawSession::state_message(DawState state,
        std::uint8_t value, std::uint8_t index,
        std::span<const std::uint8_t> info) const {
    return DawMidiCodec::sysex(state, value, index, info);
}

std::vector<std::uint8_t> DawSession::text_state(DawState state,
        std::uint8_t value, std::uint8_t index, std::string_view text) const {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size());
    for (const unsigned char c : text)
        bytes.push_back(c < 0x80 ? c : static_cast<std::uint8_t>('?'));
    return state_message(state, value, index, bytes);
}

std::vector<std::uint8_t> DawSession::tempo_message(double bpm) const {
    if (!(bpm > 0.0))
        bpm = 120.0;
    const auto ns_per_minute = static_cast<std::uint64_t>(6.0e9 / bpm);
    std::array<std::uint8_t, 5> encoded{};
    for (std::size_t i = 0; i < encoded.size(); ++i)
        encoded[i] =
                static_cast<std::uint8_t>((ns_per_minute >> (7 * i)) & 0x7f);
    return state_message(DawState::tempo, 0, 0, encoded);
}

} // namespace libnks
