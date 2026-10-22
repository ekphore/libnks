// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace libnks {

struct DawMidiCc {
    std::uint8_t controller{};
    std::uint8_t value{};
};
struct DawMidiSysEx {
    std::uint8_t state_id{};
    std::uint8_t value{};
    std::uint8_t index{};
    std::vector<std::uint8_t> payload;
};
using DawMidiEvent = std::variant<DawMidiCc, DawMidiSysEx>;

enum class DawProtocolVersion : std::uint8_t { v1 = 1, v2 = 2, v3 = 3, v4 = 4 };

enum class DawControl : std::uint8_t {
    hello = 0x01,
    goodbye = 0x02,
    shift = 0x04,
    mode_select = 0x05,
    use_sysex_parameter_updates = 0x06,
    controller_command = 0x08,
    play = 0x10,
    restart = 0x11,
    record = 0x12,
    count_in = 0x13,
    stop = 0x14,
    clear = 0x15,
    loop = 0x16,
    metronome = 0x17,
    tap_tempo = 0x18,
    undo = 0x20,
    redo = 0x21,
    quantize = 0x22,
    automation = 0x23,
    navigate_tracks = 0x30,
    navigate_banks = 0x31,
    navigate_clips = 0x32,
    navigate_transport = 0x34,
    navigate_loop = 0x35,
    navigate_presets = 0x36,
    track_volume_0 = 0x50,
    track_pan_0 = 0x58,
    play_selected_clip = 0x60,
    stop_clip = 0x61,
    play_scene = 0x62,
    record_session = 0x63,
    selected_track_volume = 0x64,
    selected_track_pan = 0x65,
    selected_track_mute = 0x66,
    selected_track_solo = 0x67,
    selected_track_available = 0x68,
    selected_track_muted_by_solo = 0x69,
    parameter_value_0 = 0x70
};

enum class DawState : std::uint8_t {
    surface_configuration = 0x03,
    identity = 0x07,
    tempo = 0x19,
    track_available = 0x40,
    track_instance = 0x41,
    track_selected = 0x42,
    track_mute = 0x43,
    track_solo = 0x44,
    track_record_arm = 0x45,
    track_volume_text = 0x46,
    track_pan_text = 0x47,
    track_name = 0x48,
    track_level = 0x49,
    track_muted_by_solo = 0x4a,
    track_color = 0x4b,
    selected_plugin = 0x70,
    plugin_chain = 0x71,
    parameter_name = 0x72,
    parameter_value_text = 0x73,
    parameter_page = 0x74,
    parameter_page_name = 0x75,
    selected_preset = 0x76
};

enum class DawTrackType : std::uint8_t {
    empty = 0,
    generic = 1,
    midi = 2,
    audio = 3,
    group = 4,
    return_bus = 5,
    master = 6
};

// GenericDAW/NIHIA travels over MIDI; MK3 display and plug-in models use
// KKS/ODR over USB, so don't feed those packets to this codec
class DawMidiCodec {
  public:
    static constexpr std::uint8_t hello = 0x01;
    static constexpr std::uint8_t goodbye = 0x02;
    static constexpr std::uint8_t identity = 0x07;

    [[nodiscard]] static std::vector<std::uint8_t> control_change(
            std::uint8_t controller, std::uint8_t value);
    [[nodiscard]] static std::vector<std::uint8_t> control_change(
            DawControl controller, std::uint8_t value) {
        return control_change(static_cast<std::uint8_t>(controller), value);
    }
    [[nodiscard]] static std::vector<std::uint8_t> sysex(
            std::uint8_t state_id, std::span<const std::uint8_t> payload = {});
    [[nodiscard]] static std::vector<std::uint8_t> sysex(DawState state,
            std::uint8_t value, std::uint8_t index,
            std::span<const std::uint8_t> info = {});
    [[nodiscard]] static std::vector<std::uint8_t> daw_identity(
            std::uint8_t major, std::uint8_t minor, std::string_view daw_name);
    // only supported channel-16 CCs become controller events
    [[nodiscard]] static std::optional<DawMidiEvent> decode(
            std::span<const std::uint8_t> message);
    // GenericDAW encoders send 7-bit two's complement deltas
    [[nodiscard]] static int relative_delta(std::uint8_t value) noexcept;
};

// a MIDI read can split SysEx or hold several CCs, so we keep the leftover
// bytes and running status around for the next chunk
class DawMidiStreamDecoder {
  public:
    [[nodiscard]] std::vector<DawMidiEvent> push(
            std::span<const std::uint8_t> bytes);
    void reset() noexcept;

  private:
    std::vector<std::uint8_t> pending;
    std::uint8_t running_status{};
};

enum class DawSessionState : std::uint8_t { disconnected, hello_sent, ready };

// no MIDI I/O here, only the handshake and building messages
class DawSession {
  public:
    explicit DawSession(DawProtocolVersion requested = DawProtocolVersion::v3)
        : requested(requested) {}

    [[nodiscard]] std::vector<std::uint8_t> begin();
    [[nodiscard]] std::optional<DawMidiEvent> receive(
            std::span<const std::uint8_t> message);
    [[nodiscard]] std::vector<DawMidiEvent> receive_stream(
            std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::vector<std::uint8_t> end();
    [[nodiscard]] std::vector<std::uint8_t> identity_message(std::uint8_t major,
            std::uint8_t minor, std::string_view daw_name) const;
    [[nodiscard]] std::vector<std::uint8_t> control(
            DawControl controller, std::uint8_t value) const;
    [[nodiscard]] std::vector<std::uint8_t> track_volume(
            std::uint8_t track, std::uint8_t value) const;
    [[nodiscard]] std::vector<std::uint8_t> track_pan(
            std::uint8_t track, std::uint8_t value) const;
    [[nodiscard]] std::vector<std::uint8_t> parameter_value(
            std::uint8_t parameter, std::uint8_t value) const;
    [[nodiscard]] std::vector<std::uint8_t> state_message(DawState state,
            std::uint8_t value, std::uint8_t index,
            std::span<const std::uint8_t> info = {}) const;
    [[nodiscard]] std::vector<std::uint8_t> text_state(DawState state,
            std::uint8_t value, std::uint8_t index,
            std::string_view text) const;
    [[nodiscard]] std::vector<std::uint8_t> tempo_message(double bpm) const;
    [[nodiscard]] DawSessionState state() const noexcept {
        return session_state;
    }
    [[nodiscard]] std::optional<DawProtocolVersion>
    negotiated_version() const noexcept {
        return negotiated;
    }

  private:
    DawProtocolVersion requested;
    DawSessionState session_state{ DawSessionState::disconnected };
    std::optional<DawProtocolVersion> negotiated;
    DawMidiStreamDecoder decoder;

    void apply(const DawMidiEvent &event);
    [[nodiscard]] std::vector<std::uint8_t> indexed_control(
            std::uint8_t base, std::uint8_t index, std::uint8_t value) const;
};

} // namespace libnks
