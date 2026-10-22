// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "libnks/nks.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace libnks {

// macOS autostart wants the app name on top of its path
inline constexpr std::string_view odr_agent_application_name =
        "NIHardwareConnectionService.app";
inline constexpr std::string_view odr_agent_relative_path =
        "Hardware/Hardware Connection Service/NIHardwareConnectionService.app";
inline constexpr std::string_view odr_ipc_endpoint_leaf =
        "com.native-instruments.odr_agent.kks";
inline constexpr std::string_view odr_ipc_protocol_version = "2.2.0";

[[nodiscard]] std::string odr_ipc_endpoint(
        std::string_view shared_ni_documents_dir);

// keep map insertion order: field order is visible in the encoded MessagePack
struct MsgpackValue {
    using Binary = std::vector<std::byte>;
    using Array = std::vector<MsgpackValue>;
    using Map = std::vector<std::pair<MsgpackValue, MsgpackValue>>;
    using Storage = std::variant<std::monostate, bool, std::int64_t,
            std::uint64_t, float, double, std::string, Binary, Array, Map>;

    Storage value;

    MsgpackValue() = default;
    MsgpackValue(std::nullptr_t) : value(std::monostate{}) {}
    MsgpackValue(bool input) : value(input) {}
    MsgpackValue(std::int64_t input) : value(input) {}
    MsgpackValue(std::uint64_t input) : value(input) {}
    MsgpackValue(float input) : value(input) {}
    MsgpackValue(double input) : value(input) {}
    MsgpackValue(std::string input) : value(std::move(input)) {}
    MsgpackValue(std::string_view input) : value(std::string(input)) {}
    MsgpackValue(const char *input) : value(std::string(input)) {}
    MsgpackValue(Binary input) : value(std::move(input)) {}
    MsgpackValue(Array input) : value(std::move(input)) {}
    MsgpackValue(Map input) : value(std::move(input)) {}
};

using RpcMethodId = std::variant<std::string, std::uint16_t>;

struct OdrKnobParameterStyle {};
struct OdrValueParameterStyle {};
struct OdrPowerParameterStyle {};
struct OdrTempoSyncParameterStyle {};
struct OdrMenuParameterStyle {
    std::vector<std::string> values;
};
struct OdrParameterStyleInfo {
    // ODR enum symbol like "sine" or "hi_pass", unknown NKS tokens stay nil
    // like kontakt does
    std::optional<std::string> image;
    std::string name;
};
struct OdrWaveformParameterStyle {
    std::vector<OdrParameterStyleInfo> values;
};
struct OdrFilterTypeParameterStyle {
    std::vector<OdrParameterStyleInfo> values;
};
using OdrContinuousParameterStyle =
        std::variant<OdrKnobParameterStyle, OdrValueParameterStyle>;
using OdrToggleParameterStyle = std::variant<OdrValueParameterStyle,
        OdrPowerParameterStyle, OdrTempoSyncParameterStyle>;
using OdrDiscreteParameterStyle =
        std::variant<OdrValueParameterStyle, OdrMenuParameterStyle,
                OdrWaveformParameterStyle, OdrFilterTypeParameterStyle>;

struct ContinuousParameterValue {
    float value{};
    std::string display_value;
};
struct ToggleParameterValue {
    bool value{};
    std::optional<std::string> display_value;
};
struct DiscreteParameterValue {
    std::uint32_t index{};
    std::optional<std::string> display_value;
};
struct ContinuousBipolarParameterValue {
    float value{};
    std::string display_value;
};
struct DiscreteBipolarParameterValue {
    std::uint32_t index{};
    std::optional<std::string> display_value;
};
using OdrParameterValue = std::variant<ContinuousParameterValue,
        ToggleParameterValue, DiscreteParameterValue,
        ContinuousBipolarParameterValue, DiscreteBipolarParameterValue>;

struct OdrContinuousParameter {
    ContinuousParameterValue parameter_value;
    OdrContinuousParameterStyle parameter_style{ OdrKnobParameterStyle{} };
};
struct OdrToggleParameter {
    ToggleParameterValue parameter_value;
    OdrToggleParameterStyle parameter_style{ OdrValueParameterStyle{} };
};
struct OdrDiscreteParameter {
    DiscreteParameterValue parameter_value;
    OdrDiscreteParameterStyle parameter_style{ OdrValueParameterStyle{} };
    std::uint32_t value_count{};
};
struct OdrContinuousBipolarParameter {
    ContinuousBipolarParameterValue parameter_value;
    OdrContinuousParameterStyle parameter_style{ OdrKnobParameterStyle{} };
};
struct OdrDiscreteBipolarParameter {
    DiscreteBipolarParameterValue parameter_value;
    OdrValueParameterStyle parameter_style{};
    std::uint32_t value_count{};
};
using OdrParameter = std::variant<OdrContinuousParameter, OdrToggleParameter,
        OdrDiscreteParameter, OdrContinuousBipolarParameter,
        OdrDiscreteBipolarParameter>;

// use the resolver when metadata gives a value count but no labels
using OdrParameterDisplayResolver = std::function<std::string(float)>;
[[nodiscard]] OdrParameter make_odr_parameter(
        const ParameterAttributes &attributes, float normalized_value,
        std::optional<std::string> display_value = std::nullopt,
        const OdrParameterDisplayResolver &resolve_display_value = {});

struct OdrNks1ViewAddress {
    std::uint8_t page{};
};
struct OdrNks2ViewAddress {
    std::optional<std::uint8_t> group_index;
    std::uint8_t section_index{};
    std::uint8_t parameter_index{};
};
using OdrParameterPageViewAddress =
        std::variant<OdrNks1ViewAddress, OdrNks2ViewAddress>;

struct OdrRgbColor {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
    bool is_rgb565{};
};
struct OdrParameterPagePluginData {
    std::string name;
    std::optional<std::array<std::byte, 32>> background;
    std::vector<std::pair<std::uint32_t, OdrParameter>> parameters;
    std::optional<OdrRgbColor> plugin_color;
    std::optional<OdrRgbColor> nks_control_color;
};
using OdrNks1ParameterPage = std::vector<DeviceLayoutSection>;
struct OdrParameterPageHostViewState {
    std::optional<std::vector<OdrNks1ParameterPage>> nks1_layout;
    std::optional<DeviceParameterLayout> nks2_layout;
};
struct OdrParameterPageDeviceViewState {
    OdrParameterPageViewAddress view_address;
    bool force_host_viewstate{};
};
struct OdrParameterPageData {
    OdrParameterPagePluginData plugin_data;
    OdrParameterPageDeviceViewState device_owned_viewstate;
    OdrParameterPageHostViewState host_owned_viewstate;
};

enum class OdrPluginChainElementType : std::uint8_t { fx, instrument };
enum class OdrSupportedNksVersion : std::uint8_t { non_nks, nks1, nks2 };
struct OdrPluginChainElement {
    std::string name;
    std::array<std::byte, 16> identifier{};
    bool can_bypass{};
    bool bypass_enabled{};
    bool can_remove{};
    bool can_move_forward{};
    bool can_move_backward{};
    std::optional<OdrPluginChainElementType> type;
    std::optional<OdrSupportedNksVersion> supported_nks_version;
};
struct OdrPluginChainData {
    std::vector<OdrPluginChainElement> plugins;
    std::int32_t current_plugin_index{ -1 };
};

// kontakt shows one loaded slot as the active entry with ID 0 plus an empty
// rack slot after it
[[nodiscard]] OdrPluginChainData make_sampler_plugin_chain(
        std::string instrument_name);

enum class KksMixerTrackType : std::uint8_t {
    generic_track,
    audio,
    midi,
    group,
    return_bus,
    master,
    empty,
};

enum class KksMixerParameterDisplayType : std::uint8_t {
    unipolar_knob,
    bipolar_knob,
    switch_button,
    text,
};

struct KksMixerParameterInfo {
    std::string name;
    float value{};
    std::string display_value;
    KksMixerParameterDisplayType display_type{
        KksMixerParameterDisplayType::unipolar_knob
    };
    std::string section_name;
};

struct KksMixerTrackInfo {
    bool arm{};
    bool automation{};
    std::string color;
    std::array<float, 2> meter{};
    bool mute{};
    bool mute_by_solo{};
    std::string name;
    float pan{};
    std::string pan_label;
    bool selected{};
    std::string short_name;
    bool solo{};
    KksMixerTrackType type{ KksMixerTrackType::generic_track };
    float volume{};
    std::string volume_label;
};

struct KksMixerModelTrackData {
    std::int32_t focused_track{ -1 };
    std::vector<KksMixerTrackInfo> track_infos;
    std::array<KksMixerParameterInfo, 8> selected_track_parameters;
    std::int32_t current_parameter_page{};
    std::int32_t number_of_parameter_pages{};
    std::string plugin_preset_name;
    std::string plugin_track_name;
};

enum class KksMixerTrackProperty : std::uint8_t {
    arm,
    automation,
    color,
    meter,
    mute,
    mute_by_solo,
    name,
    pan,
    pan_label,
    selected,
    short_name,
    solo,
    type,
    volume,
    volume_label,
};

using KksMixerTrackPropertyValue = std::variant<bool, std::string,
        std::array<float, 2>, float, KksMixerTrackType>;

enum class KksMixerProperty : std::uint8_t {
    focused_track,
    track_infos,
    selected_track_parameters,
    current_parameter_page,
    number_of_parameter_pages,
    plugin_preset_name,
    plugin_track_name,
};
using KksMixerPropertyValue =
        std::variant<std::int32_t, std::vector<KksMixerTrackInfo>,
                std::array<KksMixerParameterInfo, 8>, std::string>;

struct KksSmartplayChoice {
    std::vector<std::string> items;
    std::uint32_t selection{};
};
template <typename T>
struct KksSmartplayRange {
    T min{};
    T max{};
    T value{};
};
struct KksSmartplaySoftbuttonShortcut {
    std::string label;
    bool enabled{};
};
struct KksSmartplayArpMode {
    KksSmartplayChoice type;
    KksSmartplayChoice rate;
    KksSmartplayChoice sequence;
    std::optional<KksSmartplayRange<float>> swing;
    KksSmartplayChoice octaves;
    KksSmartplayRange<float> dynamic;
    KksSmartplayRange<float> gate;
    KksSmartplayChoice retrigger;
    KksSmartplayChoice repeat;
    KksSmartplayChoice offset;
    KksSmartplayChoice inversion;
    KksSmartplayRange<std::uint8_t> min_key;
    KksSmartplayRange<std::uint8_t> max_key;
    bool hold{};
    std::array<KksSmartplaySoftbuttonShortcut, 4> softbutton_shortcuts;
};
struct KksSmartplayNoteRepeatMode {
    KksSmartplayChoice rate;
    std::optional<KksSmartplayRange<float>> swing;
    KksSmartplayRange<float> gate;
    bool hold{};
    std::array<KksSmartplaySoftbuttonShortcut, 4> softbutton_shortcuts;
};
using KksSmartplayArpEngineMode =
        std::variant<KksSmartplayArpMode, KksSmartplayNoteRepeatMode>;
struct KksSmartplayArpEngine {
    bool enabled{};
    KksSmartplayArpEngineMode engine_model;
    bool mode_interactable{};
};
struct KksSmartplayScaleModel {
    KksSmartplayChoice bank;
    KksSmartplayChoice type;
};
struct KksSmartplayScaleMode {
    KksSmartplayChoice root_note;
    KksSmartplayScaleModel scale;
    KksSmartplayChoice key_mapping;
};
struct KksSmartplayHarmonizerMode {
    KksSmartplayChoice root_note;
    KksSmartplayScaleModel scale;
    KksSmartplayChoice key_mapping;
    KksSmartplayChoice type;
    KksSmartplayChoice position;
};
struct KksSmartplayChordSetMode {
    KksSmartplayChoice root_note;
    KksSmartplayChoice type;
    KksSmartplayChoice position;
};
struct KksSmartplayUserChordMode {
    KksSmartplayChoice root_note;
    KksSmartplayChoice type;
    KksSmartplayChoice position;
};
using KksSmartplayScaleEngineMode =
        std::variant<KksSmartplayScaleMode, KksSmartplayHarmonizerMode,
                KksSmartplayChordSetMode, KksSmartplayUserChordMode>;
struct KksSmartplayScaleEngine {
    bool enabled{};
    KksSmartplayScaleEngineMode engine_model;
    bool user_chords_enabled{};
};
struct KksSmartplayEngineState {
    KksSmartplayArpEngine arpeggiator_engine;
    KksSmartplayScaleEngine scale_engine;
};

struct KksProjectTrack {
    std::uint32_t id{};
    std::string name;
    std::optional<OdrRgbColor> track_color;
    std::vector<KksProjectTrack> children;
    bool is_collapsed{};
};
using KksProjectTree = KksProjectTrack;
enum class KksTrackOrientation : std::uint8_t { horizontal, vertical };

enum class KksTrackPluginChainElementType : std::uint8_t { plugin, container };
struct KksTrackPluginChainElement {
    KksTrackPluginChainElementType type{
        KksTrackPluginChainElementType::plugin
    };
    std::array<std::byte, 16> identifier{};
    std::string name;
    bool can_bypass{};
    bool bypass_enabled{};
    bool can_remove{};
    bool can_move_forward{};
    bool can_move_backward{};
    std::vector<KksTrackPluginChainElement> children;
    bool is_collapsed{};
};
struct KksTrackPluginChainData {
    std::vector<KksTrackPluginChainElement> plugins;
    std::optional<std::array<std::byte, 16>> selected_plugin_id;
};

enum class OdrTransportState : std::uint8_t { stopped, paused, playing };
enum class OdrRecordState : std::uint8_t { off, armed, recording };
enum class OdrControlState : std::uint8_t { disabled, off, on };
enum class OdrInstancePage : std::uint8_t {
    browser,
    mixer,
    parameter,
    smartplay
};
enum class OdrHostPadModeState : std::uint8_t { pad_mode, keyboard_mode };
struct OdrTransportSectionState {
    OdrTransportState transport{ OdrTransportState::stopped };
    OdrRecordState recording{ OdrRecordState::off };
    std::optional<OdrControlState> metro;
    std::optional<OdrControlState> loop;
    std::optional<OdrControlState> tap_tempo;
    std::optional<OdrControlState> count_in;
};
struct OdrUndoState {
    bool can_undo{};
    bool can_redo{};
};
struct OdrQuantizeState {
    bool can_quantize{};
    bool can_quantize_half{};
};
struct OdrHostOwnedInstanceState {
    std::optional<OdrTransportSectionState> transport_section_state;
    std::optional<OdrUndoState> undo_state;
    std::optional<std::string> current_preset_name;
    std::optional<std::string> current_track_name;
    std::optional<std::uint32_t> current_track_number;
    std::optional<std::string> status_message;
    std::optional<float> tempo;
    std::optional<std::string> dawcontrol_plugin_identifier;
    std::optional<float> master_volume_dbfs;
    std::optional<std::string> active_keyswitch;
    bool smartplay_enabled{};
    bool browser_enabled{};
    std::optional<OdrQuantizeState> quantize_state;
    std::optional<OdrHostPadModeState> host_pad_mode;
    std::optional<std::uint32_t> selected_sound_id;
    std::optional<std::array<std::byte, 16>> plugin_identifier;
};
// leave track and status empty, an absent optional and an empty string aren't
// the same on the wire
[[nodiscard]] OdrHostOwnedInstanceState make_sampler_host_owned_instance_state(
        std::string preset_name, std::string dawcontrol_plugin_identifier,
        float tempo = 120.0F);
struct OdrDeviceOwnedInstanceState {
    OdrInstancePage page{ OdrInstancePage::browser };
    bool has_lightguide_focus{};
};
enum class OdrSmartplayViewFocus : std::uint8_t { arpeggiator, scale };
struct KksSmartplayDeviceViewState {
    OdrSmartplayViewFocus focus{ OdrSmartplayViewFocus::arpeggiator };
    std::uint8_t page{};
};

enum class OdrSoundIcon : std::uint8_t { keyboard };
struct OdrBrowserSoundItem {
    std::string name;
    std::optional<OdrSoundIcon> icon;
    std::optional<std::array<std::byte, 32>> asset_image;
    bool is_user_saved{};
    bool is_favorite{};
    std::int64_t identifier{};
};
struct OdrBrowserSoundResults {
    std::vector<OdrBrowserSoundItem> items;
    std::optional<std::int32_t> selected_index;
};
struct OdrBrowserSettings {
    bool prehear_enabled{};
    bool quickbrowse_enabled{};
    bool speak_preset_enabled{};
    bool reset_filters_enabled{};
};
struct OdrBrowserNamedItem {
    std::string name;
};
struct OdrBrowserTagItem {
    std::string name;
    bool selected{};
};
struct OdrBrowserProductItem {
    std::string name;
    std::optional<std::array<std::byte, 32>> thumbnail_image;
    std::optional<std::string> accessible_name;
};
template <typename Item>
struct OdrBrowserSingleSelection {
    std::vector<Item> items;
    std::optional<std::int32_t> selected_index;
};
struct OdrBrowserMultiSelection {
    std::vector<OdrBrowserTagItem> items;
    std::vector<std::int32_t> selected_indexes;
};
struct OdrBrowserCarousel {
    std::vector<OdrBrowserNamedItem> items;
    std::int32_t selected_index{};
};
struct OdrBrowserFilters {
    OdrBrowserSingleSelection<OdrBrowserNamedItem> brands;
    OdrBrowserSingleSelection<OdrBrowserNamedItem> banks;
    OdrBrowserSingleSelection<OdrBrowserNamedItem> subbanks;
    OdrBrowserMultiSelection types;
    OdrBrowserMultiSelection subtypes;
    OdrBrowserMultiSelection characters;
    OdrBrowserSingleSelection<OdrBrowserProductItem> products;
    OdrBrowserCarousel file_types;
    bool user_saved{};
    bool favorites{};
};
struct OdrBrowserData {
    OdrBrowserFilters filters;
    OdrBrowserSoundResults sounds;
};
enum class OdrBrowserFilterProperty : std::uint8_t {
    brands,
    banks,
    subbanks,
    types,
    subtypes,
    characters,
    products,
    file_types,
    user_saved,
    favorites
};
struct OdrBrowserBrandFilter {
    std::vector<OdrBrowserNamedItem> items;
    std::optional<std::int32_t> selected_index;
};
struct OdrBrowserBankFilter {
    std::vector<OdrBrowserNamedItem> items;
    std::optional<std::int32_t> selected_index;
};
struct OdrBrowserProductFilter {
    std::vector<OdrBrowserProductItem> items;
    std::optional<std::int32_t> selected_index;
};
using OdrBrowserFilter = std::variant<OdrBrowserBrandFilter,
        OdrBrowserBankFilter, OdrBrowserMultiSelection, OdrBrowserProductFilter,
        OdrBrowserCarousel, bool>;
using OdrBrowserFilterSelection = std::variant<std::vector<std::int32_t>,
        std::optional<std::int32_t>, std::int32_t, bool>;

// same browser model HCS sends to the keyboard, but file_asset is already the
// device asset ID here, NOT the 32 byte ODR handle, so don't mix them up
struct KksBrowserSoundItem {
    std::string name;
    std::optional<OdrSoundIcon> icon;
    std::optional<std::uint64_t> asset_image;
    bool is_user_saved{};
    bool is_favorite{};
    std::int64_t identifier{};
};
struct KksBrowserSoundResults {
    std::vector<KksBrowserSoundItem> items;
    std::optional<std::int32_t> selected_index;
};
struct KksBrowserProductItem {
    std::string name;
    std::optional<std::uint64_t> thumbnail_image;
    std::optional<std::string> accessible_name;
};
struct KksBrowserProductFilter {
    std::vector<KksBrowserProductItem> items;
    std::optional<std::int32_t> selected_index;
};
struct KksBrowserFilters {
    OdrBrowserSingleSelection<OdrBrowserNamedItem> brands;
    OdrBrowserSingleSelection<OdrBrowserNamedItem> banks;
    OdrBrowserSingleSelection<OdrBrowserNamedItem> subbanks;
    OdrBrowserMultiSelection types;
    OdrBrowserMultiSelection subtypes;
    OdrBrowserMultiSelection characters;
    KksBrowserProductFilter products;
    OdrBrowserCarousel file_types;
    bool user_saved{};
    bool favorites{};
};
struct KksBrowserData {
    KksBrowserFilters filters;
    KksBrowserSoundResults sounds;
};
using KksBrowserFilter = std::variant<OdrBrowserBrandFilter,
        OdrBrowserBankFilter, OdrBrowserMultiSelection, KksBrowserProductFilter,
        OdrBrowserCarousel, bool>;
using KksBrowserFilterSelection = OdrBrowserFilterSelection;

struct RpcNotification {
    RpcMethodId method;
    MsgpackValue::Array parameters;
};
struct RpcRequest {
    std::uint32_t id{};
    RpcMethodId method;
    MsgpackValue::Array parameters;
};
struct RpcResponse {
    std::uint32_t id{};
    MsgpackValue error;
    MsgpackValue result;
};

// raw MessagePack for NKS metadata, no RPC size prefix
[[nodiscard]] std::vector<std::byte> encode_msgpack_value(
        const MsgpackValue &value);
[[nodiscard]] std::optional<MsgpackValue> decode_msgpack_value(
        std::span<const std::byte> bytes);

enum class OdrInstanceType : std::uint8_t {
    standalone,
    vst2,
    vst3,
    audio_unit,
    aax
};
struct OdrInstanceInfo {
    std::string name;
    std::string version;
    OdrInstanceType type{ OdrInstanceType::standalone };
    bool is_host{};
};

struct OdrUsbDeviceInfo {
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint16_t version_number{};
    std::string manufacturer;
    std::string product;
    std::string serial_number;
};

enum class OdrPrerelease : std::uint8_t {
    alpha,
    beta,
    release_candidate,
    release
};
struct OdrProtocolVersion {
    std::uint8_t major{};
    std::uint8_t minor{};
    std::uint8_t patch{};
    OdrPrerelease prerelease{ OdrPrerelease::release };
    std::uint8_t prerelease_number{};
};

[[nodiscard]] std::optional<OdrProtocolVersion> parse_odr_protocol_version(
        std::string_view text);
[[nodiscard]] bool supports_odr_protocol_2_1(
        const OdrProtocolVersion &version) noexcept;

struct OdrHandshakeReply {
    std::string agent_version;
    std::string ipc_protocol_version;
    OdrProtocolVersion negotiated_protocol;
    bool supports_protocol_2_1{};
    std::vector<OdrUsbDeviceInfo> available_devices;
    std::vector<std::array<std::byte, 32>> registered_assets;
    std::vector<std::string> symbol_registry;
};

// USB endpoint addresses for the keyboard side, not the kontakt to agent side
enum class KksUsbChannel : std::uint8_t {
    heartbeat = 0x03,
    primary = 0x04,
};

struct KksRoutedFrame {
    KksUsbChannel channel{ KksUsbChannel::primary };
    std::vector<std::byte> bytes;
};

// don't reorder these, compact method/client/enum/model values are indices into
// this table
[[nodiscard]] const std::vector<std::string> &kks_protocol_symbol_registry();

struct KksHostHandshake {
    OdrProtocolVersion host_protocol_version{ 2, 5, 0 };
    std::vector<std::string> symbol_registry{ kks_protocol_symbol_registry() };
    std::string agent_version{ "2.1.5 (R14)" };
    std::string utc_clock;
    bool disable_heartbeat_validation{};
};

struct KksAssetRecord {
    // never on the wire, it's just the position in handshake_reply.assets
    std::uint64_t device_asset_id{};
    std::uint64_t size{};
    std::array<std::byte, 32> checksum{};
};

// file_asset is the u64 device asset ID here, not the 32 byte ODR handle
// OdrParameterPagePluginData uses
struct KksParameterPagePluginData {
    std::string name;
    std::optional<std::uint64_t> background_asset_id;
    std::vector<std::pair<std::uint32_t, OdrParameter>> parameters;
    std::optional<OdrRgbColor> plugin_color;
    std::optional<OdrRgbColor> nks_control_color;
};

struct KksParameterPageData {
    KksParameterPagePluginData plugin_data;
    OdrParameterPageDeviceViewState device_owned_viewstate;
    OdrParameterPageHostViewState host_owned_viewstate;
};

// fails on duplicate IDs or ones that don't fit on the wire instead of
// truncating them
[[nodiscard]] std::optional<OdrParameterPageData> make_odr_parameter_page(
        const PluginInfo &plugin, std::span<const Page> pages);
[[nodiscard]] std::optional<KksParameterPageData> make_kks_parameter_page(
        const PluginInfo &plugin, std::span<const Page> pages);

enum class KksParameterPagePluginDataProperty : std::uint8_t {
    name,
    background,
    parameters,
    plugin_color,
    nks_control_color,
};

using KksParameterPagePluginDataPropertyValue =
        std::variant<std::string, std::optional<std::uint64_t>,
                std::vector<std::pair<std::uint32_t, OdrParameter>>,
                std::optional<OdrRgbColor>>;

struct KksDeviceHandshakeReply {
    OdrProtocolVersion device_protocol_version;
    std::uint64_t total_asset_space{};
    std::vector<KksAssetRecord> assets;
    std::string os_version;
    std::string firmware_version;
    std::string renderer_version;
    std::string hardware_model;
};

// a new ID is one past the largest handshake one, or zero if there's none
[[nodiscard]] std::optional<std::uint64_t> find_kks_asset_id(
        const KksDeviceHandshakeReply &reply,
        const std::array<std::byte, 32> &checksum) noexcept;
[[nodiscard]] std::uint64_t next_kks_asset_id(
        const KksDeviceHandshakeReply &reply) noexcept;

enum class KksDeviceMethod : std::uint8_t {
    device_property_changed,
    midi_plugin_action,
    plugin_action,
    set_active_instance_client,
    set_focus,
    device_properties,
    device_setting,
    crashdump,
    system_command_reply_with_log,
    agent_heartbeat_request_handshake,
    client_instance_set_device_owned_instance_client_state,
    data_tracking_event,
    data_tracking_active,
    accessibility_event,
    midi_template_transfer_event,
    handshake_reply,
};

class OdrSymbolRegistry;

struct KksDeviceMessage {
    KksDeviceMethod method{};
    MsgpackValue::Array parameters;
    // only set for requests, those want an empty success reply after dispatch
    std::optional<std::uint32_t> request_id;
};

// symbols can be a string or a registry index depending on whether we've
// negotiated the registry yet
using KksDevicePropertyValue =
        std::variant<std::monostate, bool, float, std::int32_t, std::string>;
struct KksDevicePropertyChanged {
    RpcMethodId property;
    KksDevicePropertyValue value;
};
struct KksDeviceProperties {
    std::vector<std::pair<RpcMethodId, KksDevicePropertyValue>> properties;
};
struct KksActiveInstanceClient {
    std::optional<RpcMethodId> client;
};
struct KksFocus {
    RpcMethodId client;
};
struct KksDataTrackingActive {
    bool active{};
};
struct KksMidiPluginAction {
    RpcMethodId client;
    MsgpackValue action;
};
struct KksPluginAction {
    RpcMethodId client;
    MsgpackValue action;
};
struct KksSystemCommandReply {
    RpcMethodId command;
    std::string log;
};
struct KksHeartbeatHandshakeRequest {};
struct KksDataTrackingEvent {
    std::string name;
    MsgpackValue value;
    std::vector<std::pair<std::string, MsgpackValue>> extra_values;
};
enum class KksGlobalMidiPortPreference : std::uint8_t {
    usb_priority,
    dual,
};
enum class KksVelocityCurve : std::uint8_t {
    soft3,
    soft2,
    soft1,
    linear,
    hard1,
    hard2,
    hard3,
    fixed,
};
enum class KksAftertouchMode : std::uint8_t {
    off,
    channel_pressure,
    poly_key_pressure,
};
struct KksAftertouchParameters {
    float curve{};
    float sensitivity{};
    float threshold{};
    std::int64_t delay_milliseconds{};
};
struct KksMidiCcAssignment {
    std::uint8_t midi_channel{};
    std::uint8_t midi_cc_number{};
};
struct KksMidiPitchBendAssignment {
    std::uint8_t midi_channel{};
};
struct KksMidiProgramChangeAssignment {
    std::uint8_t midi_channel{};
};
struct KksMidiNoteOnAssignment {
    std::uint8_t midi_channel{};
    std::uint8_t pitch{};
    std::uint8_t velocity{};
};
using KksMidiEncoderAssignment =
        std::variant<KksMidiCcAssignment, KksMidiProgramChangeAssignment>;
using KksMidiButtonAssignment = std::variant<KksMidiCcAssignment,
        KksMidiProgramChangeAssignment, KksMidiNoteOnAssignment>;
using KksMidiContinuousAssignment = std::variant<KksMidiCcAssignment,
        KksMidiPitchBendAssignment, KksMidiProgramChangeAssignment>;
struct KksMidiEncoderAbsolute {
    std::uint8_t min{};
    std::uint8_t max{};
};
struct KksMidiEncoderRelative {};
struct KksMidiEncoderRelativeOffset {
    std::int8_t offset{};
};
using KksMidiEncoderBehaviour = std::variant<KksMidiEncoderAbsolute,
        KksMidiEncoderRelative, KksMidiEncoderRelativeOffset>;
enum class KksMidiEncoderDisplayMode : std::uint8_t {
    unipolar_int,
    bipolar_int,
    unipolar_percent,
    bipolar_percent,
    bipolar_percent_50,
    gm_volume_db,
};
struct KksMidiEncoderMapping {
    KksMidiEncoderAssignment assignment;
    KksMidiEncoderBehaviour behaviour;
};
struct KksMidiKnobEntry {
    KksMidiEncoderMapping mapping;
    std::string label;
    bool custom_label{};
    KksMidiEncoderDisplayMode display_mode{
        KksMidiEncoderDisplayMode::unipolar_int
    };
};
struct KksMidiButtonToggle {
    std::uint8_t on{};
    std::uint8_t off{};
};
struct KksMidiButtonGate {
    std::uint8_t on{};
    std::uint8_t off{};
};
struct KksMidiButtonTrigger {};
struct KksMidiButtonIncrement {
    std::uint8_t min{};
    std::uint8_t max{};
    std::int8_t step{};
    bool wrap{};
};
using KksMidiButtonBehaviour = std::variant<KksMidiButtonToggle,
        KksMidiButtonGate, KksMidiButtonTrigger, KksMidiButtonIncrement>;
struct KksMidiButtonMapping {
    KksMidiButtonAssignment assignment;
    KksMidiButtonBehaviour behaviour;
};
struct KksMidiButtonEntry {
    KksMidiButtonMapping mapping;
    RpcMethodId color;
    std::string label;
    bool custom_label{};
};
struct KksMidiMappingPage {
    std::array<std::optional<KksMidiKnobEntry>, 8> knobs;
    std::optional<std::array<std::optional<KksMidiButtonEntry>, 8>> buttons;
};
struct KksMidiContinuousRange {
    std::int16_t min{};
    std::int16_t max{};
};
struct KksMidiContinuousMapping {
    KksMidiContinuousAssignment assignment;
    KksMidiContinuousRange behaviour;
};
enum class KksMidiKeyzonePriority : std::uint8_t { plugin, keyzone };
struct KksMidiKeyzoneAssignment {
    std::uint8_t upper_key{};
    std::uint8_t midi_channel{};
    RpcMethodId color;
    std::int8_t semitones_transpose{};
    std::optional<KksVelocityCurve> velocity_curve;
    bool disable{};
    KksMidiKeyzonePriority priority{ KksMidiKeyzonePriority::plugin };
};
struct KksMidiPedalContinuousConfiguration {
    bool swap_tip_ring{};
    bool invert{};
};
struct KksMidiPedalContinuousAssignment {
    KksMidiPedalContinuousConfiguration configuration;
    KksMidiContinuousMapping mapping;
};
struct KksMidiPedalSwitchConfiguration {
    bool invert{};
};
struct KksMidiPedalSwitchAssignment {
    KksMidiPedalSwitchConfiguration configuration;
    KksMidiButtonMapping mapping_tip;
    KksMidiButtonMapping mapping_ring;
};
using KksMidiPedalAssignment = std::variant<KksMidiPedalContinuousAssignment,
        KksMidiPedalSwitchAssignment>;
enum class KksMidiPortPreference : std::uint8_t {
    usb_priority,
    dual,
    din_priority,
};
struct KksMidiTemplate {
    std::string template_name;
    std::vector<KksMidiMappingPage> pages;
    std::vector<std::pair<RpcMethodId, std::optional<KksMidiContinuousMapping>>>
            wheel_assignments;
    std::vector<std::pair<RpcMethodId, std::optional<KksMidiContinuousMapping>>>
            touchstrip_assignments;
    std::vector<KksMidiKeyzoneAssignment> keyboard_assignments;
    std::vector<std::pair<RpcMethodId, std::optional<KksMidiPedalAssignment>>>
            pedal_assignments;
    RpcMethodId color;
    std::string image_data;
    bool hide_template_name{};
    std::optional<KksMidiPortPreference> midi_output_preference;
};
enum class KksMidiTemplateTransferCommand : std::uint8_t {
    import_request,
    export_request,
    import_response,
    export_response,
    abort,
};
struct KksMidiTemplateTransferEvent {
    KksMidiTemplateTransferCommand command{
        KksMidiTemplateTransferCommand::abort
    };
    std::vector<KksMidiTemplate> data;
};
struct KksCrashdump {
    std::map<std::string, std::string> metadata;
    std::string filename;
    std::vector<std::byte> minidump;
};
struct KksDeviceSettings {
    float led_brightness{};
    float display_brightness{};
    bool lightguide_enabled{};
    RpcMethodId global_led_color;
    bool usb_midi_2_enabled{};
    bool usage_data_tracking_enabled{};
    KksGlobalMidiPortPreference midi_output_port{
        KksGlobalMidiPortPreference::usb_priority
    };
    KksVelocityCurve velocity_curve{ KksVelocityCurve::linear };
    KksAftertouchMode aftertouch_mode{ KksAftertouchMode::off };
    KksAftertouchParameters aftertouch_configuration;
    std::string active_midi_template;
    std::vector<KksMidiTemplate> midi_templates;
    bool speak_browser_preset{};
    bool experimental_features_enabled{};
};
enum class KksAccessibilityCommand : std::uint8_t {
    unknown,
    connect,
    disconnect,
    toggle_training_mode,
    create_zone,
    set_control_data,
    set_control_state,
    clear_control_data,
    switch_layer,
    trigger_speech,
};
enum class KksAccessibilityControlState : std::uint8_t {
    no_state,
    off,
    on,
    disabled,
    focused,
    inactive,
};
struct KksAccessibilityNoData {};
struct KksAccessibilityZoneData {
    std::uint8_t zone_id{};
    std::string name;
    std::string description;
};
struct KksAccessibilityControlData {
    std::uint8_t zone_id{};
    std::uint32_t control_id{};
    std::uint8_t layer{};
    std::uint8_t row{};
    std::uint8_t level{};
    std::string text;
};
struct KksAccessibilityControlStateData {
    std::uint8_t zone_id{};
    std::uint32_t control_id{};
    std::uint8_t layer{};
    KksAccessibilityControlState control_state{
        KksAccessibilityControlState::no_state
    };
};
struct KksAccessibilityLayerData {
    std::uint8_t layer{};
};
struct KksAccessibilityControl {
    std::uint8_t zone_id{};
    std::uint32_t control_id{};
};
using KksAccessibilityData =
        std::variant<KksAccessibilityNoData, KksAccessibilityZoneData,
                KksAccessibilityControlData, KksAccessibilityControlStateData,
                KksAccessibilityLayerData, KksAccessibilityControl>;
struct KksAccessibilityEvent {
    KksAccessibilityCommand command{ KksAccessibilityCommand::unknown };
    KksAccessibilityData data;
};
struct KksDeviceOwnedInstanceClientState {
    RpcMethodId client;
    OdrDeviceOwnedInstanceState state;
};
using KksDecodedDevicePayload = std::variant<KksDevicePropertyChanged,
        KksDeviceProperties, KksActiveInstanceClient, KksFocus,
        KksDataTrackingActive, KksMidiPluginAction, KksPluginAction,
        KksSystemCommandReply, KksHeartbeatHandshakeRequest,
        KksDataTrackingEvent, KksDeviceSettings, KksCrashdump,
        KksMidiTemplateTransferEvent, KksAccessibilityEvent,
        KksDeviceOwnedInstanceClientState, KksDeviceHandshakeReply>;

[[nodiscard]] std::string_view kks_device_method_name(
        KksDeviceMethod method) noexcept;
[[nodiscard]] std::span<const std::string_view>
kks_device_method_names() noexcept;
[[nodiscard]] std::optional<KksDeviceMessage> decode_kks_device_message(
        std::span<const std::byte> frame,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksDecodedDevicePayload> decode_kks_device_payload(
        const KksDeviceMessage &message,
        const OdrSymbolRegistry *registry = nullptr);

[[nodiscard]] KksRoutedFrame encode_kks_host_handshake(
        const KksHostHandshake &handshake);
[[nodiscard]] KksRoutedFrame encode_kks_heartbeat();
[[nodiscard]] KksRoutedFrame encode_kks_goodbye();
[[nodiscard]] KksRoutedFrame encode_kks_asset_add(std::uint64_t device_asset_id,
        const std::array<std::byte, 32> &checksum,
        std::span<const std::byte> payload);
[[nodiscard]] KksRoutedFrame encode_kks_asset_remove(
        std::uint64_t device_asset_id);
[[nodiscard]] KksRoutedFrame encode_kks_assets_reset();
[[nodiscard]] std::optional<KksRoutedFrame> encode_kks_client_instance_add(
        const RpcMethodId &client, const OdrInstanceInfo &info,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_client_instance_remove(
        const RpcMethodId &client, const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_client_focus_acquire(
        const RpcMethodId &client, const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_client_focus_release(
        const RpcMethodId &client, const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_client_page_set(
        const RpcMethodId &client, OdrInstancePage page,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksRoutedFrame>
encode_kks_client_instance_set_host_owned_state(const RpcMethodId &client,
        const OdrHostOwnedInstanceState &state,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_client_instance_set_device_owned_state(
        const RpcMethodId &client, const OdrDeviceOwnedInstanceState &state,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_parameter_page_set(
        const RpcMethodId &client, const KksParameterPageData &data,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_parameter_page_set_plugin_data(
        const RpcMethodId &client, const KksParameterPagePluginData &data,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame
encode_kks_parameter_page_set_device_owned_viewstate(const RpcMethodId &client,
        const OdrParameterPageDeviceViewState &state,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_parameter_page_set_host_owned_viewstate(
        const RpcMethodId &client, const OdrParameterPageHostViewState &state,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksRoutedFrame>
encode_kks_parameter_page_update_property(const RpcMethodId &client,
        KksParameterPagePluginDataProperty property,
        const KksParameterPagePluginDataPropertyValue &value,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_parameter_page_update_parameter_value(
        const RpcMethodId &client, std::uint32_t parameter_id,
        const OdrParameterValue &value,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_client_instance_set_lightguide(
        const RpcMethodId &client, const LightguideState &state,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksRoutedFrame>
encode_kks_client_instance_set_plugin_chain(const RpcMethodId &client,
        const OdrPluginChainData &data,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_client_instance_set_plugin_chain_index(
        const RpcMethodId &client, std::int32_t current_plugin_index,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksRoutedFrame> encode_kks_mixer_set_track_data(
        const RpcMethodId &client, const KksMixerModelTrackData &data,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksRoutedFrame>
encode_kks_mixer_update_track_property(const RpcMethodId &client,
        std::int32_t track_index, KksMixerTrackProperty property,
        const KksMixerTrackPropertyValue &value,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksRoutedFrame> encode_kks_mixer_update_property(
        const RpcMethodId &client, KksMixerProperty property,
        const KksMixerPropertyValue &value,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_mixer_set_meters(
        const RpcMethodId &client, std::span<const std::array<float, 2>> meters,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksRoutedFrame> encode_kks_browser_set_data(
        const RpcMethodId &client, const KksBrowserData &data,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_browser_set_settings(
        const RpcMethodId &client, const OdrBrowserSettings &settings,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_browser_set_sound_selection(
        const RpcMethodId &client, std::optional<std::int32_t> selected_index,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_browser_set_filters(
        const RpcMethodId &client, const KksBrowserFilters &filters,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksRoutedFrame> encode_kks_browser_set_filter(
        const RpcMethodId &client, OdrBrowserFilterProperty property,
        const KksBrowserFilter &filter,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksRoutedFrame>
encode_kks_browser_set_filter_selection(const RpcMethodId &client,
        OdrBrowserFilterProperty property,
        const KksBrowserFilterSelection &selection,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksRoutedFrame> encode_kks_browser_set_sounds(
        const RpcMethodId &client, const KksBrowserSoundResults &sounds,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksRoutedFrame> encode_kks_browser_set_sound_item(
        const RpcMethodId &client, std::int32_t index,
        const KksBrowserSoundItem &item,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksRoutedFrame> encode_kks_smartplay_set_data(
        const RpcMethodId &client, const KksSmartplayEngineState &state,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_smartplay_set_device_owned_viewstate(
        const RpcMethodId &client, const KksSmartplayDeviceViewState &state,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_host_set_project_tree(
        const RpcMethodId &client, const KksProjectTree &tree,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame encode_kks_host_set_selected_track(
        const RpcMethodId &client, std::uint32_t selected_track,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksRoutedFrame>
encode_kks_host_set_track_orientation(const RpcMethodId &client,
        std::optional<KksTrackOrientation> orientation,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksRoutedFrame>
encode_kks_host_set_track_plugin_chain(const RpcMethodId &client,
        const KksTrackPluginChainData &data,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] KksRoutedFrame
encode_kks_host_set_track_plugin_chain_selected_plugin(
        const RpcMethodId &client,
        const std::optional<std::array<std::byte, 16>> &selected_plugin_id,
        const OdrSymbolRegistry *registry = nullptr);
[[nodiscard]] std::optional<KksDeviceHandshakeReply> decode_kks_handshake_reply(
        const MsgpackValue &value);

class OdrSymbolRegistry {
  public:
    OdrSymbolRegistry() = default;
    explicit OdrSymbolRegistry(std::vector<std::string> symbols)
        : symbol_names(std::move(symbols)) {}

    [[nodiscard]] std::optional<std::uint16_t> resolve(
            std::string_view symbol) const;
    [[nodiscard]] std::optional<std::string_view> symbol(
            std::uint16_t index) const;
    [[nodiscard]] const std::vector<std::string> &symbols() const noexcept {
        return symbol_names;
    }

  private:
    std::vector<std::string> symbol_names;
};

// little endian uint32 length, then the [2, method, parameters] notification
[[nodiscard]] std::vector<std::byte> encode_rpc_notification(
        const RpcMethodId &method, std::span<const MsgpackValue> parameters);
[[nodiscard]] std::vector<std::byte> encode_rpc_request(std::uint32_t id,
        const RpcMethodId &method, std::span<const MsgpackValue> parameters);
[[nodiscard]] std::vector<std::byte> encode_rpc_success_response(
        std::uint32_t id);
[[nodiscard]] std::vector<std::byte> encode_rpc_response(std::uint32_t id,
        const MsgpackValue &error, const MsgpackValue &result);
[[nodiscard]] std::vector<std::byte> encode_odr_instance_hello(
        std::uint32_t request_id, std::string_view identity,
        const OdrInstanceInfo &client_info);

// one whole frame, trailing bytes or anything that isn't a notification fails
[[nodiscard]] std::optional<RpcNotification> decode_rpc_notification(
        std::span<const std::byte> frame);
[[nodiscard]] std::optional<RpcRequest> decode_rpc_request(
        std::span<const std::byte> frame);
[[nodiscard]] std::optional<RpcResponse> decode_rpc_response(
        std::span<const std::byte> frame);
[[nodiscard]] std::optional<OdrHandshakeReply> decode_odr_handshake_reply(
        const RpcResponse &response);
[[nodiscard]] std::optional<OdrUsbDeviceInfo> decode_odr_usb_device_info(
        const MsgpackValue &value);

class RpcFrameDecoder {
  public:
    static constexpr std::size_t max_payload_size = 1024 * 1024;

    void append(std::span<const std::byte> bytes);
    [[nodiscard]] std::optional<std::vector<std::byte>> next();
    [[nodiscard]] bool failed() const noexcept { return invalid_frame; }
    void clear() noexcept;

  private:
    std::vector<std::byte> buffered;
    bool invalid_frame{};
};

// one connection can drive several instances, so the instance ID comes with
// each call and isn't the client identity
class OdrClientMessages {
  public:
    explicit OdrClientMessages(std::string client_identity)
        : identity(std::move(client_identity)) {}

    void install_symbol_registry(OdrSymbolRegistry registry) {
        this->registry = std::move(registry);
    }
    void clear_symbol_registry() noexcept { registry.reset(); }
    [[nodiscard]] const std::optional<OdrSymbolRegistry> &
    symbol_registry() const noexcept {
        return registry;
    }

    [[nodiscard]] const std::string &client_identity() const noexcept {
        return identity;
    }
    [[nodiscard]] std::vector<std::byte> request(std::uint32_t request_id,
            std::string_view method,
            std::span<const MsgpackValue> parameters) const;
    [[nodiscard]] std::vector<std::byte> instance_goodbye() const;
    [[nodiscard]] std::vector<std::byte> subscribe_device_property(
            std::string_view device_identifier,
            std::string_view property) const;
    [[nodiscard]] std::vector<std::byte> request_focus(
            std::string_view instance_id) const;
    [[nodiscard]] std::vector<std::byte> release_focus(
            std::string_view instance_id) const;
    [[nodiscard]] std::vector<std::byte> set_page(
            std::string_view instance_id, std::uint8_t raw_page) const;
    [[nodiscard]] std::vector<std::byte> set_parameter_value(
            std::string_view instance_id, std::uint32_t parameter_id,
            const OdrParameterValue &value) const;
    [[nodiscard]] std::vector<std::byte> set_parameter_page_data(
            std::string_view instance_id,
            const OdrParameterPageData &data) const;
    [[nodiscard]] std::vector<std::byte> set_plugin_chain_data(
            std::string_view instance_id, const OdrPluginChainData &data) const;
    [[nodiscard]] std::vector<std::byte> set_lightguide(
            std::string_view instance_id, const LightguideState &state) const;
    [[nodiscard]] std::vector<std::byte> set_host_owned_instance_state(
            std::string_view instance_id,
            const OdrHostOwnedInstanceState &state) const;
    [[nodiscard]] std::vector<std::byte> set_browser_sound_item(
            std::string_view instance_id, std::int32_t index,
            const OdrBrowserSoundItem &item) const;
    [[nodiscard]] std::vector<std::byte> set_browser_sound_selection(
            std::string_view instance_id,
            std::optional<std::int32_t> index) const;
    [[nodiscard]] std::vector<std::byte> set_browser_sounds(
            std::string_view instance_id,
            const OdrBrowserSoundResults &sounds) const;
    [[nodiscard]] std::vector<std::byte> set_browser_settings(
            std::string_view instance_id,
            const OdrBrowserSettings &settings) const;
    [[nodiscard]] std::vector<std::byte> set_browser_filters(
            std::string_view instance_id,
            const OdrBrowserFilters &filters) const;
    [[nodiscard]] std::vector<std::byte> set_browser_filter(
            std::string_view instance_id, OdrBrowserFilterProperty property,
            const OdrBrowserFilter &filter) const;
    [[nodiscard]] std::vector<std::byte> set_browser_filter_selection(
            std::string_view instance_id, OdrBrowserFilterProperty property,
            const OdrBrowserFilterSelection &selection) const;
    [[nodiscard]] std::vector<std::byte> set_browser_data_model(
            std::string_view instance_id, const OdrBrowserData &data) const;
    [[nodiscard]] std::vector<std::byte> upload_asset(
            const std::array<std::byte, 32> &handle,
            std::span<const std::byte> payload) const;

  private:
    [[nodiscard]] RpcMethodId method_id(std::string_view method) const;
    [[nodiscard]] MsgpackValue protocol_symbol(std::string_view symbol) const;
    [[nodiscard]] std::vector<std::byte> notification(std::string_view method,
            std::span<const MsgpackValue> parameters) const;
    [[nodiscard]] std::vector<std::byte> identity_instance_message(
            std::string_view method, std::string_view instance_id) const;
    [[nodiscard]] MsgpackValue browser_filters_value(
            const OdrBrowserFilters &filters) const;
    [[nodiscard]] MsgpackValue browser_sounds_value(
            const OdrBrowserSoundResults &sounds) const;
    std::string identity;
    std::optional<OdrSymbolRegistry> registry;
};

} // namespace libnks
