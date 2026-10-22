// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <libnks/libnks.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <span>
#include <vector>

// libnks doesn't do MIDI I/O, plug SendMidi into whatever your DAW already
// uses (or write a script if supported which might be much easier)
class ExampleDawSurface {
  public:
    using SendMidi = std::function<bool(std::span<const std::uint8_t> message)>;

    explicit ExampleDawSurface(SendMidi send) : send_(std::move(send)) {}

    bool start() { return send(session_.begin()); }

    bool midi_received(std::span<const std::uint8_t> bytes) {
        for (const auto &event : session_.receive_stream(bytes)) {
            const auto *cc = std::get_if<libnks::DawMidiCc>(&event);
            if (!cc)
                continue;
            if (cc->controller >= 0x50 && cc->controller <= 0x57) {
                const auto track = std::uint8_t(cc->controller - 0x50);
                volumes_[track] = std::clamp(
                        volumes_[track] +
                                libnks::DawMidiCodec::relative_delta(cc->value),
                        0, 127);
                // the keyboard wants the absolute value back
                if (!send(session_.track_volume(
                            track, std::uint8_t(volumes_[track]))))
                    return false;
            }
        }
        return true;
    }

    bool publish() {
        if (session_.state() != libnks::DawSessionState::ready)
            return false;
        return send(session_.identity_message(1, 0, "Example DAW")) &&
               send(session_.tempo_message(120.0)) &&
               send(session_.state_message(libnks::DawState::track_available,
                       std::uint8_t(libnks::DawTrackType::midi), 0)) &&
               send(session_.state_message(
                       libnks::DawState::track_selected, 1, 0)) &&
               send(session_.text_state(
                       libnks::DawState::track_name, 0, 0, "Piano")) &&
               send(session_.track_volume(0, std::uint8_t(volumes_[0])));
    }

    bool stop() { return send(session_.end()); }

  private:
    bool send(const std::vector<std::uint8_t> &message) {
        return !message.empty() && send_(message);
    }

    SendMidi send_;
    libnks::DawSession session_;
    std::array<int, 8> volumes_{ 96, 80, 80, 80, 80, 80, 80, 80 };
};

int main() {
    std::vector<std::vector<std::uint8_t>> sent;
    ExampleDawSurface surface([&](std::span<const std::uint8_t> message) {
        sent.emplace_back(message.begin(), message.end());
        return true;
    });
    if (!surface.start())
        return 1;

    // in a real DAW this comes from your MIDI input callback
    const auto hello =
            libnks::DawMidiCodec::control_change(libnks::DawMidiCodec::hello,
                    std::uint8_t(libnks::DawProtocolVersion::v3));
    if (!surface.midi_received(hello) || !surface.publish())
        return 1;
}
