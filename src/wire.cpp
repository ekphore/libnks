// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "libnks/wire.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <type_traits>

namespace libnks {
namespace {

using Bytes = std::vector<std::byte>;

void byte(Bytes &out, std::uint8_t value) {
    out.push_back(static_cast<std::byte>(value));
}

template <class Integer>
void big_endian(Bytes &out, Integer value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    auto bits = static_cast<Unsigned>(value);
    for (std::size_t i = sizeof(Integer); i != 0; --i) {
        byte(out, static_cast<std::uint8_t>(bits >> ((i - 1) * 8)));
    }
}

void size_header(Bytes &out, std::size_t size, std::uint8_t fix_base,
        std::uint8_t tag16, std::uint8_t tag32, std::size_t fix_limit) {
    if (size <= fix_limit) {
        byte(out, static_cast<std::uint8_t>(fix_base | size));
    } else if (size <= std::numeric_limits<std::uint16_t>::max()) {
        byte(out, tag16);
        big_endian(out, static_cast<std::uint16_t>(size));
    } else {
        byte(out, tag32);
        big_endian(out, static_cast<std::uint32_t>(size));
    }
}

void pack(Bytes &out, const MsgpackValue &input);

void pack_string(Bytes &out, std::string_view value) {
    if (value.size() <= 31)
        byte(out, static_cast<std::uint8_t>(0xa0 | value.size()));
    else if (value.size() <= 0xff) {
        byte(out, 0xd9);
        byte(out, value.size());
    } else if (value.size() <= 0xffff) {
        byte(out, 0xda);
        big_endian(out, static_cast<std::uint16_t>(value.size()));
    } else {
        byte(out, 0xdb);
        big_endian(out, static_cast<std::uint32_t>(value.size()));
    }
    out.insert(out.end(), reinterpret_cast<const std::byte *>(value.data()),
            reinterpret_cast<const std::byte *>(value.data() + value.size()));
}

void pack(Bytes &out, const MsgpackValue &input) {
    std::visit(
            [&](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                    byte(out, 0xc0);
                else if constexpr (std::is_same_v<T, bool>)
                    byte(out, value ? 0xc3 : 0xc2);
                else if constexpr (std::is_same_v<T, std::uint64_t>) {
                    if (value <= 0x7f)
                        byte(out, value);
                    else if (value <= 0xff) {
                        byte(out, 0xcc);
                        byte(out, value);
                    } else if (value <= 0xffff) {
                        byte(out, 0xcd);
                        big_endian(out, static_cast<std::uint16_t>(value));
                    } else if (value <= 0xffffffffu) {
                        byte(out, 0xce);
                        big_endian(out, static_cast<std::uint32_t>(value));
                    } else {
                        byte(out, 0xcf);
                        big_endian(out, value);
                    }
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    if (value >= 0)
                        pack(out, MsgpackValue(
                                          static_cast<std::uint64_t>(value)));
                    else if (value >= -32)
                        byte(out, static_cast<std::uint8_t>(value));
                    else if (value >= std::numeric_limits<std::int8_t>::min()) {
                        byte(out, 0xd0);
                        byte(out, static_cast<std::uint8_t>(value));
                    } else if (value >=
                               std::numeric_limits<std::int16_t>::min()) {
                        byte(out, 0xd1);
                        big_endian(out, static_cast<std::int16_t>(value));
                    } else if (value >=
                               std::numeric_limits<std::int32_t>::min()) {
                        byte(out, 0xd2);
                        big_endian(out, static_cast<std::int32_t>(value));
                    } else {
                        byte(out, 0xd3);
                        big_endian(out, value);
                    }
                } else if constexpr (std::is_same_v<T, float>) {
                    byte(out, 0xca);
                    big_endian(out, std::bit_cast<std::uint32_t>(value));
                } else if constexpr (std::is_same_v<T, double>) {
                    byte(out, 0xcb);
                    big_endian(out, std::bit_cast<std::uint64_t>(value));
                } else if constexpr (std::is_same_v<T, std::string>)
                    pack_string(out, value);
                else if constexpr (std::is_same_v<T, MsgpackValue::Binary>) {
                    if (value.size() <= 0xff) {
                        byte(out, 0xc4);
                        byte(out, value.size());
                    } else if (value.size() <= 0xffff) {
                        byte(out, 0xc5);
                        big_endian(
                                out, static_cast<std::uint16_t>(value.size()));
                    } else {
                        byte(out, 0xc6);
                        big_endian(
                                out, static_cast<std::uint32_t>(value.size()));
                    }
                    out.insert(out.end(), value.begin(), value.end());
                } else if constexpr (std::is_same_v<T, MsgpackValue::Array>) {
                    size_header(out, value.size(), 0x90, 0xdc, 0xdd, 15);
                    for (const auto &item : value)
                        pack(out, item);
                } else if constexpr (std::is_same_v<T, MsgpackValue::Map>) {
                    size_header(out, value.size(), 0x80, 0xde, 0xdf, 15);
                    for (const auto &[key, item] : value) {
                        pack(out, key);
                        pack(out, item);
                    }
                }
            },
            input.value);
}

class Reader {
  public:
    explicit Reader(std::span<const std::byte> bytes) : bytes(bytes) {}
    [[nodiscard]] bool done() const { return offset == bytes.size(); }

    std::optional<MsgpackValue> value() {
        const auto tag = u8();
        if (!tag)
            return std::nullopt;
        if (*tag <= 0x7f)
            return MsgpackValue(std::uint64_t(*tag));
        if (*tag >= 0xe0)
            return MsgpackValue(std::int64_t(static_cast<std::int8_t>(*tag)));
        if ((*tag & 0xe0) == 0xa0)
            return string(*tag & 0x1f);
        if ((*tag & 0xf0) == 0x90)
            return array(*tag & 0x0f);
        if ((*tag & 0xf0) == 0x80)
            return map(*tag & 0x0f);
        switch (*tag) {
            case 0xc0:
                return MsgpackValue{};
            case 0xc2:
                return MsgpackValue(false);
            case 0xc3:
                return MsgpackValue(true);
            case 0xc4: {
                auto n = integer<std::uint8_t>();
                return n ? binary(*n) : std::nullopt;
            }
            case 0xc5: {
                auto n = integer<std::uint16_t>();
                return n ? binary(*n) : std::nullopt;
            }
            case 0xc6: {
                auto n = integer<std::uint32_t>();
                return n ? binary(*n) : std::nullopt;
            }
            case 0xca: {
                auto bits = integer<std::uint32_t>();
                return bits ? std::optional(
                                      MsgpackValue(std::bit_cast<float>(*bits)))
                            : std::nullopt;
            }
            case 0xcb: {
                auto bits = integer<std::uint64_t>();
                return bits ? std::optional(MsgpackValue(
                                      std::bit_cast<double>(*bits)))
                            : std::nullopt;
            }
            case 0xcc:
                return unsigned_value<std::uint8_t>();
            case 0xcd:
                return unsigned_value<std::uint16_t>();
            case 0xce:
                return unsigned_value<std::uint32_t>();
            case 0xcf:
                return unsigned_value<std::uint64_t>();
            case 0xd0:
                return signed_value<std::int8_t>();
            case 0xd1:
                return signed_value<std::int16_t>();
            case 0xd2:
                return signed_value<std::int32_t>();
            case 0xd3:
                return signed_value<std::int64_t>();
            case 0xd9: {
                auto n = integer<std::uint8_t>();
                return n ? string(*n) : std::nullopt;
            }
            case 0xda: {
                auto n = integer<std::uint16_t>();
                return n ? string(*n) : std::nullopt;
            }
            case 0xdb: {
                auto n = integer<std::uint32_t>();
                return n ? string(*n) : std::nullopt;
            }
            case 0xdc: {
                auto n = integer<std::uint16_t>();
                return n ? array(*n) : std::nullopt;
            }
            case 0xdd: {
                auto n = integer<std::uint32_t>();
                return n ? array(*n) : std::nullopt;
            }
            case 0xde: {
                auto n = integer<std::uint16_t>();
                return n ? map(*n) : std::nullopt;
            }
            case 0xdf: {
                auto n = integer<std::uint32_t>();
                return n ? map(*n) : std::nullopt;
            }
            default:
                return std::nullopt;
        }
    }

  private:
    std::optional<std::uint8_t> u8() {
        if (offset == bytes.size())
            return std::nullopt;
        return std::to_integer<std::uint8_t>(bytes[offset++]);
    }
    template <class T>
    std::optional<T> integer() {
        using U = std::make_unsigned_t<T>;
        if (bytes.size() - offset < sizeof(T))
            return std::nullopt;
        U result{};
        for (std::size_t i = 0; i < sizeof(T); ++i)
            result = static_cast<U>(
                    (result << 8) |
                    std::to_integer<std::uint8_t>(bytes[offset++]));
        return static_cast<T>(result);
    }
    template <class T>
    std::optional<MsgpackValue> unsigned_value() {
        auto n = integer<T>();
        return n ? std::optional(MsgpackValue(std::uint64_t(*n)))
                 : std::nullopt;
    }
    template <class T>
    std::optional<MsgpackValue> signed_value() {
        auto n = integer<T>();
        return n ? std::optional(MsgpackValue(std::int64_t(*n))) : std::nullopt;
    }
    std::optional<MsgpackValue> string(std::size_t size) {
        if (bytes.size() - offset < size)
            return std::nullopt;
        std::string text(size, '\0');
        std::memcpy(text.data(), bytes.data() + offset, size);
        offset += size;
        return MsgpackValue(std::move(text));
    }
    std::optional<MsgpackValue> binary(std::size_t size) {
        if (bytes.size() - offset < size)
            return std::nullopt;
        MsgpackValue::Binary data(
                bytes.begin() + offset, bytes.begin() + offset + size);
        offset += size;
        return MsgpackValue(std::move(data));
    }
    std::optional<MsgpackValue> array(std::size_t size) {
        // check before reserve(), otherwise a tiny broken frame can ask us to
        // allocate gigabytes
        if (size > bytes.size() - offset)
            return std::nullopt;
        MsgpackValue::Array result;
        result.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            auto item = value();
            if (!item)
                return std::nullopt;
            result.push_back(std::move(*item));
        }
        return MsgpackValue(std::move(result));
    }
    std::optional<MsgpackValue> map(std::size_t size) {
        // at least one byte for the key and one for the value
        if (size > (bytes.size() - offset) / 2)
            return std::nullopt;
        MsgpackValue::Map result;
        result.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            auto key = value();
            auto item = value();
            if (!key || !item)
                return std::nullopt;
            result.emplace_back(std::move(*key), std::move(*item));
        }
        return MsgpackValue(std::move(result));
    }
    std::span<const std::byte> bytes;
    std::size_t offset{};
};

std::uint32_t payload_size(std::span<const std::byte> bytes) {
    return std::to_integer<std::uint8_t>(bytes[0]) |
           (std::uint32_t(std::to_integer<std::uint8_t>(bytes[1])) << 8) |
           (std::uint32_t(std::to_integer<std::uint8_t>(bytes[2])) << 16) |
           (std::uint32_t(std::to_integer<std::uint8_t>(bytes[3])) << 24);
}

const MsgpackValue *map_value(
        const MsgpackValue::Map &map, std::string_view key) {
    for (const auto &[candidate, value] : map) {
        const auto *text = std::get_if<std::string>(&candidate.value);
        if (text && *text == key)
            return &value;
    }
    return nullptr;
}

MsgpackValue nullable_string(const std::optional<std::string> &value) {
    return value ? MsgpackValue(*value) : MsgpackValue{};
}

MsgpackValue empty_style(std::string_view name) {
    // marker variants still need a one-entry map with a nil payload on the wire
    return MsgpackValue(
            MsgpackValue::Map{ { MsgpackValue(name), MsgpackValue{} } });
}

MsgpackValue continuous_parameter_style(
        const OdrContinuousParameterStyle &style) {
    return std::visit(
            [](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, OdrKnobParameterStyle>)
                    return empty_style("knob_parameter_style");
                else
                    return empty_style("value_parameter_style");
            },
            style);
}

MsgpackValue toggle_parameter_style(const OdrToggleParameterStyle &style) {
    return std::visit(
            [](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, OdrValueParameterStyle>)
                    return empty_style("value_parameter_style");
                else if constexpr (std::is_same_v<T, OdrPowerParameterStyle>)
                    return empty_style("power_parameter_style");
                else
                    return empty_style("tempo_sync_parameter_style");
            },
            style);
}

MsgpackValue parameter_style_info(const OdrParameterStyleInfo &info) {
    return MsgpackValue(MsgpackValue::Map{
            { "image", nullable_string(info.image) }, { "name", info.name } });
}

template <typename Style>
MsgpackValue pictorial_parameter_style(
        std::string_view name, const Style &style) {
    MsgpackValue::Array values;
    values.reserve(style.values.size());
    for (const auto &value : style.values)
        values.push_back(parameter_style_info(value));
    return MsgpackValue(MsgpackValue::Map{ { MsgpackValue(name),
            MsgpackValue(
                    MsgpackValue::Map{ { "values", std::move(values) } }) } });
}

MsgpackValue discrete_parameter_style(const OdrDiscreteParameterStyle &style) {
    return std::visit(
            [](const auto &value) -> MsgpackValue {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, OdrValueParameterStyle>) {
                    return empty_style("value_parameter_style");
                } else if constexpr (std::is_same_v<T, OdrMenuParameterStyle>) {
                    MsgpackValue::Array values;
                    values.reserve(value.values.size());
                    for (const auto &item : value.values)
                        values.emplace_back(item);
                    return MsgpackValue(
                            MsgpackValue::Map{ { "menu_parameter_style",
                                    MsgpackValue(MsgpackValue::Map{ { "values",
                                            std::move(values) } }) } });
                } else if constexpr (std::is_same_v<T,
                                             OdrWaveformParameterStyle>) {
                    return pictorial_parameter_style(
                            "waveform_parameter_style", value);
                } else {
                    return pictorial_parameter_style(
                            "filtertype_parameter_style", value);
                }
            },
            style);
}

MsgpackValue parameter_value(const OdrParameterValue &input) {
    return std::visit(
            [](const auto &value) -> MsgpackValue {
                using T = std::decay_t<decltype(value)>;
                std::string_view kind;
                MsgpackValue::Map fields;
                if constexpr (std::is_same_v<T, ContinuousParameterValue>) {
                    kind = "continuous_parameter";
                    fields = { { "value", MsgpackValue(value.value) },
                        { "display_value",
                                MsgpackValue(value.display_value) } };
                } else if constexpr (std::is_same_v<T, ToggleParameterValue>) {
                    kind = "toggle_parameter";
                    fields = { { "value", MsgpackValue(value.value) },
                        { "display_value",
                                nullable_string(value.display_value) } };
                } else if constexpr (std::is_same_v<T,
                                             DiscreteParameterValue>) {
                    kind = "discrete_parameter";
                    fields = { { "index", MsgpackValue(
                                                  std::uint64_t(value.index)) },
                        { "display_value",
                                nullable_string(value.display_value) } };
                } else if constexpr (std::is_same_v<T,
                                             ContinuousBipolarParameterValue>) {
                    kind = "continuous_bipolar_parameter";
                    fields = { { "value", MsgpackValue(value.value) },
                        { "display_value",
                                MsgpackValue(value.display_value) } };
                } else {
                    kind = "discrete_bipolar_parameter";
                    fields = { { "index", MsgpackValue(
                                                  std::uint64_t(value.index)) },
                        { "display_value",
                                nullable_string(value.display_value) } };
                }
                return MsgpackValue(MsgpackValue::Map{ { MsgpackValue(kind),
                        MsgpackValue(std::move(fields)) } });
            },
            input);
}

MsgpackValue parameter(const OdrParameter &input) {
    return std::visit(
            [](const auto &value) -> MsgpackValue {
                using T = std::decay_t<decltype(value)>;
                std::string_view kind;
                OdrParameterValue raw_value;
                MsgpackValue style;
                std::optional<std::uint32_t> num_values;
                if constexpr (std::is_same_v<T, OdrContinuousParameter>) {
                    kind = "continuous_parameter";
                    raw_value = value.parameter_value;
                    style = continuous_parameter_style(value.parameter_style);
                } else if constexpr (std::is_same_v<T, OdrToggleParameter>) {
                    kind = "toggle_parameter";
                    raw_value = value.parameter_value;
                    style = toggle_parameter_style(value.parameter_style);
                } else if constexpr (std::is_same_v<T, OdrDiscreteParameter>) {
                    kind = "discrete_parameter";
                    raw_value = value.parameter_value;
                    style = discrete_parameter_style(value.parameter_style);
                    num_values = value.value_count;
                } else if constexpr (std::is_same_v<T,
                                             OdrContinuousBipolarParameter>) {
                    kind = "continuous_bipolar_parameter";
                    raw_value = value.parameter_value;
                    style = continuous_parameter_style(value.parameter_style);
                } else {
                    kind = "discrete_bipolar_parameter";
                    raw_value = value.parameter_value;
                    style = empty_style("value_parameter_style");
                    num_values = value.value_count;
                }
                auto encoded_value = parameter_value(raw_value);
                auto &value_variant =
                        std::get<MsgpackValue::Map>(encoded_value.value);
                MsgpackValue::Map fields{
                    { "parameter_value", std::move(value_variant[0].second) },
                    { "parameter_style", std::move(style) }
                };
                if (num_values) {
                    MsgpackValue encoded_count;
                    if (*num_values != 0)
                        encoded_count = std::uint64_t(*num_values);
                    fields.emplace_back("parameter_metadata",
                            MsgpackValue(MsgpackValue::Map{ { "num_values",
                                    std::move(encoded_count) } }));
                }
                return MsgpackValue(MsgpackValue::Map{ { MsgpackValue(kind),
                        MsgpackValue(std::move(fields)) } });
            },
            input);
}

MsgpackValue rgb_color(const std::optional<OdrRgbColor> &color) {
    if (!color)
        return MsgpackValue{};
    return MsgpackValue(MsgpackValue::Map{ { "r", std::uint64_t(color->r) },
            { "g", std::uint64_t(color->g) }, { "b", std::uint64_t(color->b) },
            { "is_rgb565", color->is_rgb565 } });
}

MsgpackValue layout_section(const DeviceLayoutSection &section) {
    MsgpackValue::Array parameters;
    parameters.reserve(section.parameters.size());
    for (const auto &placement : section.parameters) {
        if (!placement)
            parameters.emplace_back();
        else
            parameters.emplace_back(
                    MsgpackValue::Map{ { "id", std::uint64_t(placement->id) },
                            { "name", placement->name } });
    }
    return MsgpackValue(MsgpackValue::Map{ { "section_name", section.name },
            { "parameters", std::move(parameters) } });
}

MsgpackValue layout_sections(const std::vector<DeviceLayoutSection> &sections) {
    MsgpackValue::Array values;
    values.reserve(sections.size());
    for (const auto &section : sections)
        values.push_back(layout_section(section));
    return MsgpackValue(std::move(values));
}

MsgpackValue nks2_layout(const std::optional<DeviceParameterLayout> &layout) {
    if (!layout)
        return MsgpackValue{};
    MsgpackValue::Array groups;
    groups.reserve(layout->groups.size());
    for (const auto &group : layout->groups) {
        groups.emplace_back(MsgpackValue::Map{ { "group_name", group.name },
                { "sections", layout_sections(group.sections) } });
    }
    return MsgpackValue(MsgpackValue::Map{
            { "performance_mode", layout_sections(layout->sections) },
            { "groups", std::move(groups) } });
}

MsgpackValue nks1_layout(
        const std::optional<std::vector<OdrNks1ParameterPage>> &layout) {
    if (!layout)
        return MsgpackValue{};
    MsgpackValue::Array pages;
    pages.reserve(layout->size());
    for (const auto &page : *layout)
        pages.push_back(layout_sections(page));
    return MsgpackValue(std::move(pages));
}

MsgpackValue view_address(const OdrParameterPageViewAddress &address) {
    return std::visit(
            [](const auto &value) -> MsgpackValue {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, OdrNks1ViewAddress>) {
                    return MsgpackValue(
                            MsgpackValue::Map{ { "nks1_view_address",
                                    std::uint64_t(value.page) } });
                } else {
                    MsgpackValue group = value.group_index
                                                 ? MsgpackValue(std::uint64_t(
                                                           *value.group_index))
                                                 : MsgpackValue{};
                    return MsgpackValue(MsgpackValue::Map{ { "nks2_view_address",
                            MsgpackValue(MsgpackValue::Map{
                                    { "group_index", std::move(group) },
                                    { "section_index",
                                            std::uint64_t(
                                                    value.section_index) },
                                    { "parameter_index",
                                            std::uint64_t(
                                                    value.parameter_index) } }) } });
                }
            },
            address);
}

void compact_map_keys(MsgpackValue &value, const OdrSymbolRegistry &registry) {
    if (auto *array = std::get_if<MsgpackValue::Array>(&value.value)) {
        for (auto &item : *array)
            compact_map_keys(item, registry);
        return;
    }
    auto *map = std::get_if<MsgpackValue::Map>(&value.value);
    if (!map)
        return;
    for (auto &[key, item] : *map) {
        if (const auto *text = std::get_if<std::string>(&key.value)) {
            if (const auto index = registry.resolve(*text))
                key = MsgpackValue(std::uint64_t(*index));
        }
        compact_map_keys(item, registry);
    }
}

std::optional<MsgpackValue> host_owned_instance_state_value(
        const OdrHostOwnedInstanceState &state,
        const OdrSymbolRegistry *registry) {
    constexpr std::array transport_names{ std::string_view("stopped"),
        std::string_view("paused"), std::string_view("playing") };
    constexpr std::array record_names{ std::string_view("off"),
        std::string_view("armed"), std::string_view("recording") };
    constexpr std::array control_names{ std::string_view("disabled"),
        std::string_view("off"), std::string_view("on") };
    constexpr std::array pad_names{ std::string_view("pad_mode"),
        std::string_view("keyboard_mode") };
    const auto symbol = [registry](std::string_view name) {
        if (registry) {
            if (const auto index = registry->resolve(name))
                return MsgpackValue(std::uint64_t(*index));
        }
        return MsgpackValue(name);
    };
    const auto optional_string = [](const std::optional<std::string> &value) {
        return value ? MsgpackValue(*value) : MsgpackValue{};
    };
    const auto optional_u32 = [](const std::optional<std::uint32_t> &value) {
        return value ? MsgpackValue(std::uint64_t(*value)) : MsgpackValue{};
    };
    const auto optional_float = [](const std::optional<float> &value) {
        return value ? MsgpackValue(*value) : MsgpackValue{};
    };
    const auto control = [&](const std::optional<OdrControlState> &value) {
        if (!value)
            return MsgpackValue{};
        const auto index = static_cast<std::size_t>(*value);
        return index < control_names.size() ? symbol(control_names[index])
                                            : MsgpackValue{};
    };

    MsgpackValue transport;
    if (state.transport_section_state) {
        const auto &value = *state.transport_section_state;
        const auto transport_index = static_cast<std::size_t>(value.transport);
        const auto record_index = static_cast<std::size_t>(value.recording);
        if (transport_index >= transport_names.size() ||
                record_index >= record_names.size())
            return std::nullopt;
        transport = MsgpackValue(MsgpackValue::Map{
                { "transport", symbol(transport_names[transport_index]) },
                { "recording", symbol(record_names[record_index]) },
                { "metro", control(value.metro) },
                { "loop", control(value.loop) },
                { "tap_tempo", control(value.tap_tempo) },
                { "count_in", control(value.count_in) } });
    }
    MsgpackValue undo;
    if (state.undo_state)
        undo = MsgpackValue(
                MsgpackValue::Map{ { "can_undo", state.undo_state->can_undo },
                        { "can_redo", state.undo_state->can_redo } });
    MsgpackValue quantize;
    if (state.quantize_state)
        quantize = MsgpackValue(MsgpackValue::Map{
                { "can_quantize", state.quantize_state->can_quantize },
                { "can_quantize_half",
                        state.quantize_state->can_quantize_half } });
    MsgpackValue pad_mode;
    if (state.host_pad_mode) {
        const auto index = static_cast<std::size_t>(*state.host_pad_mode);
        if (index >= pad_names.size())
            return std::nullopt;
        pad_mode = symbol(pad_names[index]);
    }
    MsgpackValue plugin_identifier;
    if (state.plugin_identifier)
        plugin_identifier = MsgpackValue(
                MsgpackValue::Binary(state.plugin_identifier->begin(),
                        state.plugin_identifier->end()));

    MsgpackValue model(MsgpackValue::Map{
            { "transport_section_state", std::move(transport) },
            { "undo_state", std::move(undo) },
            { "current_preset_name",
                    optional_string(state.current_preset_name) },
            { "current_track_name", optional_string(state.current_track_name) },
            { "current_track_number",
                    optional_u32(state.current_track_number) },
            { "status_message", optional_string(state.status_message) },
            { "tempo", optional_float(state.tempo) },
            { "dawcontrol_plugin_identifier",
                    optional_string(state.dawcontrol_plugin_identifier) },
            { "master_volume", optional_float(state.master_volume_dbfs) },
            { "active_keyswitch", optional_string(state.active_keyswitch) },
            { "smartplay_enabled", state.smartplay_enabled },
            { "browser_enabled", state.browser_enabled },
            { "quantize_state", std::move(quantize) },
            { "host_pad_mode", std::move(pad_mode) },
            { "selected_sound_id", optional_u32(state.selected_sound_id) },
            { "plugin_identifier", std::move(plugin_identifier) } });
    if (registry)
        compact_map_keys(model, *registry);
    return model;
}

} // namespace

const std::vector<std::string> &kks_protocol_symbol_registry() {
    static const std::vector<std::string> symbols{
        "pedal_sustain_connected",
        "pedal_expression_connected",
        "pedal_assign1_connected",
        "pedal_assign2_connected",
        "root_key",
        "browser_toggle_filter_favorite_presets",
        "browser_toggle_filter_user_presets",
        "browser_toggle_favorite",
        "browser_quick_browse",
        "browser_toggle_speak_preset",
        "browser_toggle_prehear",
        "browser_load",
        "browser_load_previous_sound",
        "browser_load_next_sound",
        "browser_reset_filters",
        "browser_navigate_brand",
        "browser_navigate_filetype",
        "browser_navigate_product",
        "browser_navigate_bank",
        "browser_navigate_subbank",
        "browser_navigate_type",
        "browser_navigate_subtype",
        "browser_navigate_character",
        "browser_navigate_sound",
        "plugin_parameter_value_set",
        "plugin_parameter_value_change",
        "plugin_parameter_erp_touched",
        "plugin_parameter_set_page_index",
        "daw_set_tempo",
        "daw_tap_tempo",
        "daw_play",
        "daw_restart",
        "daw_stop",
        "daw_record",
        "daw_metro",
        "daw_loop",
        "daw_count_in",
        "daw_undo",
        "daw_redo",
        "daw_quantize",
        "daw_quantize_half",
        "daw_set_host_pad_mode",
        "daw_toggle_node_collapsed",
        "daw_select_track",
        "daw_navigate_track",
        "daw_navigate_track_depth",
        "daw_mute_track",
        "daw_solo_track",
        "daw_track_volume_change",
        "daw_track_pan_change",
        "daw_select_host_plugin",
        "daw_toggle_plugin_container",
        "daw_octave_change",
        "plugin_chain_navigate",
        "plugin_chain_entry_move",
        "plugin_chain_entry_bypass",
        "plugin_chain_entry_remove",
        "master_volume_change",
        "smartplay_arp_engine_change",
        "smartplay_arp_engine_trigger",
        "smartplay_scale_engine_change",
        "smartplay_device_owned_viewstate_changed",
        "forward",
        "backward",
        "pad_midi_event",
        "pad_base_note",
        "name",
        "dimmed",
        "thumbnail_image",
        "accessible_name",
        "items",
        "selected_index",
        "selected_indexes",
        "icon",
        "asset_image",
        "is_user_saved",
        "is_favorite",
        "identifier",
        "prehear_enabled",
        "quickbrowse_enabled",
        "speak_preset_enabled",
        "reset_filters_enabled",
        "keyboard",
        "multi",
        "track_data",
        "device_viewstate",
        "focus",
        "track_control_mode",
        "style",
        "daw_tempo",
        "focused_track",
        "track_infos",
        "selected_track_parameters",
        "current_parameter_page",
        "number_of_parameter_pages",
        "plugin_preset_name",
        "plugin_track_name",
        "arm",
        "automation",
        "color",
        "meter",
        "mute",
        "mute_by_solo",
        "pan",
        "pan_label",
        "selected",
        "short_name",
        "solo",
        "type",
        "volume",
        "volume_label",
        "value",
        "display_value",
        "display_type",
        "section_name",
        "view_address",
        "force_host_viewstate",
        "nks1_layout",
        "nks2_layout",
        "background",
        "parameters",
        "plugin_color",
        "nks_control_color",
        "r",
        "g",
        "b",
        "is_rgb565",
        "plugin_data",
        "device_owned_viewstate",
        "host_owned_viewstate",
        "id",
        "nks1_view_address",
        "nks2_view_address",
        "group_index",
        "section_index",
        "parameter_index",
        "performance_mode",
        "groups",
        "group_name",
        "sections",
        "continuous_parameter",
        "toggle_parameter",
        "discrete_parameter",
        "continuous_bipolar_parameter",
        "discrete_bipolar_parameter",
        "parameter_value",
        "parameter_style",
        "parameter_metadata",
        "knob_parameter_style",
        "value_parameter_style",
        "power_parameter_style",
        "tempo_sync_parameter_style",
        "menu_parameter_style",
        "waveform_parameter_style",
        "filtertype_parameter_style",
        "menu",
        "index",
        "num_values",
        "image",
        "default_filter_type",
        "hi_pass",
        "resonant_hi_pass",
        "lo_pass",
        "resonant_lo_pass",
        "band_pass",
        "all_pass",
        "bell",
        "prop_q",
        "band_shelf",
        "analog_lo_shelf",
        "analog_hi_shelf",
        "baxandall_lo_shelf",
        "baxandall_hi_shelf",
        "comb",
        "band_reject",
        "default_waveform",
        "sine",
        "square",
        "square_2",
        "square_3",
        "square_4",
        "triangle",
        "triangle_2",
        "triangle_3",
        "triangle_4",
        "saw",
        "saw_2",
        "saw_3",
        "saw_4",
        "noise",
        "pwm",
        "wavetable",
        "wavetable_2",
        "wavetable_3",
        "wavetable_4",
        "complex",
        "plugins",
        "current_plugin_index",
        "can_bypass",
        "bypass_enabled",
        "can_remove",
        "can_move_forward",
        "can_move_backward",
        "supported_nks_version",
        "empty",
        "children",
        "is_collapsed",
        "selected_plugin_id",
        "selection",
        "label",
        "enabled",
        "min",
        "max",
        "bank",
        "root_note",
        "scale",
        "key_mapping",
        "position",
        "harmonizer",
        "chord_set",
        "user_chord",
        "engine_model",
        "user_chords_enabled",
        "rate",
        "sequence",
        "swing",
        "octaves",
        "dynamic",
        "gate",
        "retrigger",
        "repeat",
        "offset",
        "inversion",
        "min_key",
        "max_key",
        "hold",
        "softbutton_shortcuts",
        "arpeggiator",
        "note_repeat",
        "mode_interactable",
        "arpeggiator_engine",
        "scale_engine",
        "page",
        "engine_state",
        "browser",
        "mixer",
        "parameter",
        "smartplay",
        "transport_section_state",
        "undo_state",
        "current_preset_name",
        "current_track_name",
        "current_track_number",
        "status_message",
        "tempo",
        "dawcontrol_plugin_identifier",
        "master_volume",
        "active_keyswitch",
        "smartplay_enabled",
        "browser_enabled",
        "quantize_state",
        "host_pad_mode",
        "selected_sound_id",
        "plugin_identifier",
        "tree_level_str",
        "has_lightguide_focus",
        "keybed",
        "client_midi_addressing",
        "restart_renderer",
        "reboot",
        "set_log_level_default",
        "set_log_level_trace",
        "request_application_log",
        "request_system_log",
        "request_crashdumps",
        "self_test",
        "enable_serial_console",
        "reboot_for_dfu",
        "horizontal",
        "vertical",
        "track_color",
        "accessibility_set_status",
        "assets_add",
        "assets_remove",
        "assets_reset",
        "client_focus_acquire",
        "client_focus_release",
        "client_instance_add",
        "client_instance_browser_set_data_model",
        "client_instance_browser_set_filter",
        "client_instance_browser_set_filter_selection",
        "client_instance_browser_set_filters",
        "client_instance_browser_set_settings",
        "client_instance_browser_set_sound_item",
        "client_instance_browser_set_sound_selection",
        "client_instance_browser_set_sounds",
        "client_instance_lightguide_set",
        "client_instance_mixer_set_meters",
        "client_instance_mixer_set_track_data",
        "client_instance_mixer_update_property",
        "client_instance_mixer_update_track_property",
        "client_instance_parameter_page_model_set",
        "client_instance_parameter_page_model_set_device_owned_viewstate",
        "client_instance_parameter_page_model_set_host_owned_viewstate",
        "client_instance_parameter_page_model_set_plugin_data",
        "client_instance_parameter_page_model_update_parameter_value",
        "client_instance_parameter_page_model_update_property",
        "client_instance_plugin_chain_model_set",
        "client_instance_plugin_chain_model_set_current_index",
        "client_instance_remove",
        "client_instance_set_host_owned_instance_client_state",
        "client_instance_smartplay_set_data",
        "client_instance_smartplay_set_device_owned_viewstate",
        "client_notification",
        "client_page_set",
        "global_notification",
        "goodbye",
        "handshake",
        "heartbeat",
        "host_set_project_tree",
        "host_set_selected_track",
        "host_set_track_orientation",
        "host_set_track_plugin_chain",
        "host_set_track_plugin_chain_selected_plugin",
        "midi_template_transfer_response",
        "settings_get",
        "settings_set",
        "system_command",
        "host_action",
        "accessibility_event",
        "agent_heartbeat_request_handshake",
        "client_instance_set_device_owned_instance_client_state",
        "crashdump",
        "device_properties",
        "device_setting",
        "data_tracking_event",
        "data_tracking_active",
        "handshake_reply",
        "midi_template_transfer_event",
        "midi_plugin_action",
        "plugin_action",
        "set_active_instance_client",
        "set_focus",
        "system_command_reply_with_log",
        "device_property_changed",
        "host",
        "genericdaw",
        "mackiecontrol",
        "settings",
        "standalone_midi",
        "daw",
        "play_assist",
        "notification",
        "instance",
        "instance_00",
        "instance_01",
        "instance_02",
        "instance_03",
        "instance_04",
        "instance_05",
        "instance_06",
        "instance_07",
        "instance_08",
        "instance_09",
        "instance_10",
        "instance_11",
        "instance_12",
        "instance_13",
        "instance_14",
        "instance_15",
        "instance_16",
        "instance_17",
        "instance_18",
        "instance_19",
        "instance_20",
        "instance_21",
        "instance_22",
        "instance_23",
        "instance_24",
        "instance_25",
        "instance_26",
        "instance_27",
        "instance_28",
        "instance_29",
        "active_instance_client_changed",
        "device_connected",
        "device_disconnected",
        "device_owned_instance_client_state_changed",
        "firmware_update_available",
        "focus_changed",
        "add_asset",
        "client_browser_set_data_model",
        "client_browser_set_filter",
        "client_browser_set_filter_selection",
        "client_browser_set_filters",
        "client_browser_set_settings",
        "client_browser_set_sound_item",
        "client_browser_set_sound_selection",
        "client_browser_set_sounds",
        "client_host_set_project_tree",
        "client_host_set_selected_track",
        "client_host_set_track_orientation",
        "client_host_set_track_plugin_chain",
        "client_host_set_track_plugin_chain_selected_plugin",
        "client_lightguide_set_leds",
        "client_metro_flash",
        "client_mixer_set_meters",
        "client_mixer_set_track_data",
        "client_mixer_set_trackinfo",
        "client_parameter_page_set_data",
        "client_parameter_page_set_device_owned_viewstate",
        "client_parameter_page_set_host_owned_viewstate",
        "client_parameter_page_set_parameter_value",
        "client_parameter_page_set_plugin_data",
        "client_plugin_chain_set_data",
        "client_plugin_chain_set_index",
        "client_release_focus",
        "client_request_focus",
        "client_set_host_owned_instance_client_state",
        "client_set_page",
        "client_smartplay_set_data",
        "client_smartplay_set_device_owned_viewstate",
        "host_goodbye",
        "run_self_test",
        "send_client_notification",
        "send_global_notification",
        "set_device_settings",
        "subscribe_device_properties",
        "unsubscribe_device_properties",
        "connect_device",
        "disconnect_device",
        "get_device_properties",
        "get_device_settings",
        "get_devices",
        "host_hello",
        "instance_heartbeat",
        "instance_sync",
    };
    return symbols;
}

std::vector<std::byte> encode_msgpack_value(const MsgpackValue &value) {
    Bytes bytes;
    pack(bytes, value);
    return bytes;
}

std::optional<MsgpackValue> decode_msgpack_value(
        std::span<const std::byte> bytes) {
    Reader reader(bytes);
    auto value = reader.value();
    if (!value || !reader.done())
        return std::nullopt;
    return value;
}

OdrPluginChainData make_sampler_plugin_chain(std::string instrument_name) {
    OdrPluginChainElement instrument;
    instrument.name = std::move(instrument_name);
    instrument.can_remove = true;
    OdrPluginChainElement empty_slot;
    empty_slot.name = "Empty";
    return { { std::move(instrument), std::move(empty_slot) }, 0 };
}

OdrHostOwnedInstanceState make_sampler_host_owned_instance_state(
        std::string preset_name, std::string dawcontrol_plugin_identifier,
        float tempo) {
    OdrHostOwnedInstanceState state;
    state.current_preset_name = std::move(preset_name);
    state.tempo = tempo;
    state.dawcontrol_plugin_identifier =
            std::move(dawcontrol_plugin_identifier);
    state.smartplay_enabled = false;
    state.browser_enabled = true;
    return state;
}

OdrParameter make_odr_parameter(const ParameterAttributes &input,
        float normalized_value, std::optional<std::string> display_value,
        const OdrParameterDisplayResolver &resolve_display_value) {
    const auto attributes = normalize(input);
    const auto value_count =
            attributes.values.empty()
                    ? attributes.value_count
                    : static_cast<std::uint32_t>(attributes.values.size());
    const auto index = [&] {
        const auto count = static_cast<std::int32_t>(value_count);
        const auto scaled = static_cast<std::int32_t>(
                static_cast<float>(count) * normalized_value);
        return static_cast<std::uint32_t>(std::min(scaled, count - 1));
    };
    const auto resolved_display = [&] {
        if (display_value)
            return *display_value;
        if (resolve_display_value)
            return resolve_display_value(normalized_value);
        return std::to_string(normalized_value);
    };
    const auto discrete_display = [&] {
        const auto selected = index();
        if (display_value)
            return *display_value;
        if (!attributes.values.empty())
            return attributes.values[selected];
        if (value_count > 0)
            return std::to_string(selected + 1);
        return resolved_display();
    };
    const auto style_values = [&] {
        if (!attributes.values.empty())
            return attributes.values;
        std::vector<std::string> result;
        result.reserve(value_count);
        for (std::uint32_t value = 0; value < value_count; ++value) {
            const auto normalized = static_cast<float>(value) /
                                    static_cast<float>(value_count - 1);
            result.push_back(resolve_display_value
                                     ? resolve_display_value(normalized)
                                     : std::to_string(normalized));
        }
        return result;
    };
    struct Mapping {
        std::string_view token;
        std::string_view display;
    };
    const auto style_info = [](std::string token, bool waveform) {
        static constexpr std::array waveform_mappings{
            Mapping{ "default_waveform", "Default" }, Mapping{ "sine", "Sine" },
            Mapping{ "square", "Square" }, Mapping{ "square2", "Square2" },
            Mapping{ "square3", "Square3" }, Mapping{ "square4", "Square4" },
            Mapping{ "triangle", "Triangle" },
            Mapping{ "triangle2", "Triangle2" },
            Mapping{ "triangle3", "Triangle3" },
            Mapping{ "triangle4", "Triangle4" }, Mapping{ "saw", "Saw" },
            Mapping{ "saw2", "Saw2" }, Mapping{ "saw3", "Saw3" },
            Mapping{ "saw4", "Saw4" }, Mapping{ "noise", "Noise" },
            Mapping{ "pwm", "PWM" }, Mapping{ "wavetable", "Wavetable" },
            Mapping{ "wavetable2", "Wavetable2" },
            Mapping{ "wavetable3", "Wavetable3" },
            Mapping{ "wavetable4", "Wavetable4" },
            Mapping{ "complex", "Complex" }
        };
        static constexpr std::array filter_mappings{
            Mapping{ "default_filter_type", "Default" },
            Mapping{ "hi_pass", "High Pass" },
            Mapping{ "resonant_hi_pass", "Resonant High Pass" },
            Mapping{ "lo_pass", "Low Pass" },
            Mapping{ "resonant_lo_pass", "Resonant Low Pass" },
            Mapping{ "band_pass", "Band Pass" },
            Mapping{ "all_pass", "All Pass" }, Mapping{ "bell", "Bell" },
            Mapping{ "prop_q", "Prop Q" },
            Mapping{ "band_shelf", "Band Shelf" },
            Mapping{ "analog_lo_shelf", "Analog Low Shelf" },
            Mapping{ "analog_hi_shelf", "Analog High Shelf" },
            Mapping{ "baxandall_lo_shelf", "Baxandall Low Shelf" },
            Mapping{ "baxandall_hi_shelf", "Baxandall High Shelf" },
            Mapping{ "comb", "Comb" }, Mapping{ "band_reject", "Band Reject" }
        };
        const auto find_mapping =
                [&token](const auto &mappings) -> std::optional<Mapping> {
            const auto found =
                    std::ranges::find(mappings, token, &Mapping::token);
            if (found == mappings.end())
                return std::nullopt;
            return *found;
        };
        const auto found = waveform ? find_mapping(waveform_mappings)
                                    : find_mapping(filter_mappings);
        if (!found)
            return OdrParameterStyleInfo{ std::nullopt, std::move(token) };
        return OdrParameterStyleInfo{ std::string(found->token),
            std::string(found->display) };
    };

    switch (static_cast<ParameterKind>(attributes.type)) {
        case ParameterKind::continuous:
            return OdrContinuousParameter{
                { normalized_value, resolved_display() },
                attributes.display_style == 2
                        ? OdrContinuousParameterStyle(OdrValueParameterStyle{})
                        : OdrContinuousParameterStyle(OdrKnobParameterStyle{})
            };
        case ParameterKind::continuous_bipolar:
            return OdrContinuousBipolarParameter{
                { normalized_value, resolved_display() },
                attributes.display_style == 2
                        ? OdrContinuousParameterStyle(OdrValueParameterStyle{})
                        : OdrContinuousParameterStyle(OdrKnobParameterStyle{})
            };
        case ParameterKind::toggle: {
            std::string text;
            if (attributes.values.size() == 2)
                text = attributes.values[normalized_value >= 0.5f ? 1 : 0];
            else if (display_value || resolve_display_value)
                text = resolved_display();
            OdrToggleParameterStyle style{ OdrValueParameterStyle{} };
            if (attributes.display_style == 3)
                style = OdrPowerParameterStyle{};
            else if (attributes.display_style == 4)
                style = OdrTempoSyncParameterStyle{};
            return OdrToggleParameter{
                { normalized_value >= 0.5f, std::move(text) }, std::move(style)
            };
        }
        case ParameterKind::discrete: {
            OdrDiscreteParameterStyle style{ OdrValueParameterStyle{} };
            if (attributes.display_style == 5) {
                style = OdrMenuParameterStyle{ style_values() };
            } else if (attributes.display_style == 6) {
                std::vector<OdrParameterStyleInfo> values;
                for (auto value : style_values())
                    values.push_back(style_info(std::move(value), true));
                style = OdrWaveformParameterStyle{ std::move(values) };
            } else if (attributes.display_style == 7) {
                std::vector<OdrParameterStyleInfo> values;
                for (auto value : style_values())
                    values.push_back(style_info(std::move(value), false));
                style = OdrFilterTypeParameterStyle{ std::move(values) };
            }
            return OdrDiscreteParameter{ { index(), discrete_display() },
                std::move(style), value_count };
        }
        case ParameterKind::discrete_bipolar:
            return OdrDiscreteBipolarParameter{ { index(), discrete_display() },
                {}, value_count };
    }
    return OdrContinuousParameter{ { normalized_value, resolved_display() } };
}

std::optional<OdrParameterPageData> make_odr_parameter_page(
        const PluginInfo &plugin, std::span<const Page> pages) {
    OdrParameterPageData result;
    result.plugin_data.name = plugin.name;
    result.device_owned_viewstate.view_address =
            OdrNks2ViewAddress{ std::nullopt, 0, 0 };

    DeviceParameterLayout layout;
    layout.sections.reserve(pages.size());
    std::vector<std::uint32_t> ids;
    for (const auto &page : pages) {
        DeviceLayoutSection section;
        section.name = page.name;
        section.parameters.reserve(page.parameters.size());
        for (const auto &parameter : page.parameters) {
            // runtime controls are looser than exported metadata, eg two
            // discrete values or a null zero count, don't use that validator
            if (parameter.attributes.type > 4 ||
                    parameter.attributes.display_style > 7 ||
                    !std::isfinite(parameter.normalized_value) ||
                    parameter.normalized_value < 0 ||
                    parameter.normalized_value > 1)
                return std::nullopt;
            if (parameter.id > std::numeric_limits<std::uint32_t>::max())
                return std::nullopt;
            const auto id = static_cast<std::uint32_t>(parameter.id);
            if (std::find(ids.begin(), ids.end(), id) != ids.end())
                return std::nullopt;
            ids.push_back(id);
            section.parameters.push_back(
                    DeviceParameterPlacement{ id, parameter.name });
            result.plugin_data.parameters.emplace_back(
                    id, make_odr_parameter(parameter.attributes,
                                static_cast<float>(parameter.normalized_value),
                                parameter.value));
        }
        layout.sections.push_back(std::move(section));
    }
    result.host_owned_viewstate.nks2_layout = std::move(layout);
    return result;
}

std::optional<KksParameterPageData> make_kks_parameter_page(
        const PluginInfo &plugin, std::span<const Page> pages) {
    auto odr = make_odr_parameter_page(plugin, pages);
    if (!odr)
        return std::nullopt;
    KksParameterPageData result;
    result.plugin_data.name = std::move(odr->plugin_data.name);
    result.plugin_data.parameters = std::move(odr->plugin_data.parameters);
    result.plugin_data.plugin_color = std::move(odr->plugin_data.plugin_color);
    result.plugin_data.nks_control_color =
            std::move(odr->plugin_data.nks_control_color);
    result.device_owned_viewstate = std::move(odr->device_owned_viewstate);
    result.host_owned_viewstate = std::move(odr->host_owned_viewstate);
    return result;
}

std::string odr_ipc_endpoint(std::string_view shared_ni_documents_dir) {
    std::string result(shared_ni_documents_dir);
    if (!result.empty() && result.back() != '/')
        result.push_back('/');
    result.append(odr_ipc_endpoint_leaf);
    return result;
}

std::optional<std::uint16_t> OdrSymbolRegistry::resolve(
        std::string_view symbol_name) const {
    const auto found = std::ranges::find(symbol_names, symbol_name);
    const auto index = static_cast<std::size_t>(found - symbol_names.begin());
    if (found == symbol_names.end() || index >= 0xffff)
        return std::nullopt;
    return static_cast<std::uint16_t>(index);
}

std::optional<std::string_view> OdrSymbolRegistry::symbol(
        std::uint16_t index) const {
    if (index == 0xffff || index >= symbol_names.size())
        return std::nullopt;
    return symbol_names[index];
}

std::optional<OdrProtocolVersion> parse_odr_protocol_version(
        std::string_view text) {
    std::size_t position = 0;
    const auto component = [&]() -> std::optional<std::uint8_t> {
        if (position == text.size() || text[position] < '0' ||
                text[position] > '9')
            return std::nullopt;
        std::uint32_t value = 0;
        do {
            value = value * 10 + static_cast<unsigned>(text[position] - '0');
            if (value > 255)
                return std::nullopt;
            ++position;
        } while (position < text.size() && text[position] >= '0' &&
                 text[position] <= '9');
        return static_cast<std::uint8_t>(value);
    };
    const auto major = component();
    if (!major || position == text.size() || text[position++] != '.')
        return std::nullopt;
    const auto minor = component();
    if (!minor || position == text.size() || text[position++] != '.')
        return std::nullopt;
    const auto patch = component();
    if (!patch)
        return std::nullopt;
    OdrProtocolVersion result{ *major, *minor, *patch };
    if (position == text.size())
        return result;
    if (text[position++] != '-')
        return std::nullopt;
    const auto equal_ci = [](std::string_view left, std::string_view right) {
        if (left.size() != right.size())
            return false;
        for (std::size_t i = 0; i < left.size(); ++i) {
            auto character = left[i];
            if (character >= 'A' && character <= 'Z')
                character += 'a' - 'A';
            if (character != right[i])
                return false;
        }
        return true;
    };
    const auto label_end = text.find('.', position);
    const auto label = text.substr(position, label_end == std::string_view::npos
                                                     ? text.size() - position
                                                     : label_end - position);
    if (equal_ci(label, "alpha"))
        result.prerelease = OdrPrerelease::alpha;
    else if (equal_ci(label, "beta"))
        result.prerelease = OdrPrerelease::beta;
    else if (equal_ci(label, "rc"))
        result.prerelease = OdrPrerelease::release_candidate;
    else
        return std::nullopt;
    if (label_end == std::string_view::npos)
        return result;
    position = label_end + 1;
    const auto number = component();
    if (!number || position != text.size())
        return std::nullopt;
    result.prerelease_number = *number;
    return result;
}

bool supports_odr_protocol_2_1(const OdrProtocolVersion &version) noexcept {
    if (version.major != 2)
        return version.major > 2;
    if (version.minor != 1)
        return version.minor > 1;
    if (version.patch != 0)
        return version.patch > 0;
    return version.prerelease == OdrPrerelease::release;
}

namespace {

std::string protocol_version_text(const OdrProtocolVersion &version) {
    std::string text = std::to_string(version.major) + "." +
                       std::to_string(version.minor) + "." +
                       std::to_string(version.patch);
    switch (version.prerelease) {
        case OdrPrerelease::alpha:
            text += "-alpha";
            break;
        case OdrPrerelease::beta:
            text += "-beta";
            break;
        case OdrPrerelease::release_candidate:
            text += "-rc";
            break;
        case OdrPrerelease::release:
            return text;
    }
    if (version.prerelease_number != 0)
        text += "." + std::to_string(version.prerelease_number);
    return text;
}

MsgpackValue kks_protocol_symbol(
        std::string_view symbol, const OdrSymbolRegistry *registry) {
    if (registry) {
        if (const auto index = registry->resolve(symbol))
            return MsgpackValue(std::uint64_t(*index));
    }
    return MsgpackValue(symbol);
}

MsgpackValue kks_client_symbol(
        const RpcMethodId &client, const OdrSymbolRegistry *registry) {
    if (const auto *name = std::get_if<std::string>(&client))
        return kks_protocol_symbol(*name, registry);
    return MsgpackValue(std::uint64_t(std::get<std::uint16_t>(client)));
}

RpcMethodId kks_method(
        std::string_view method, const OdrSymbolRegistry *registry) {
    if (registry) {
        if (const auto index = registry->resolve(method))
            return *index;
    }
    return std::string(method);
}

MsgpackValue kks_parameter_map(
        std::vector<std::pair<std::uint32_t, OdrParameter>> parameters) {
    std::ranges::stable_sort(
            parameters, {}, &std::pair<std::uint32_t, OdrParameter>::first);
    MsgpackValue::Map values;
    values.reserve(parameters.size());
    for (const auto &[id, value] : parameters)
        values.emplace_back(std::uint64_t(id), parameter(value));
    return MsgpackValue(std::move(values));
}

MsgpackValue kks_plugin_data(const KksParameterPagePluginData &data) {
    MsgpackValue background;
    if (data.background_asset_id)
        background = *data.background_asset_id;
    return MsgpackValue(MsgpackValue::Map{ { "name", data.name },
            { "background", std::move(background) },
            { "parameters", kks_parameter_map(data.parameters) },
            { "plugin_color", rgb_color(data.plugin_color) },
            { "nks_control_color", rgb_color(data.nks_control_color) } });
}

MsgpackValue kks_host_viewstate(const OdrParameterPageHostViewState &state) {
    return MsgpackValue(MsgpackValue::Map{
            { "nks1_layout", nks1_layout(state.nks1_layout) },
            { "nks2_layout", nks2_layout(state.nks2_layout) } });
}

MsgpackValue kks_parameter_page_device_viewstate(
        const OdrParameterPageDeviceViewState &state) {
    return MsgpackValue(MsgpackValue::Map{
            { "view_address", view_address(state.view_address) },
            { "force_host_viewstate", state.force_host_viewstate } });
}

MsgpackValue kks_device_owned_instance_state(
        const OdrDeviceOwnedInstanceState &state,
        const OdrSymbolRegistry *registry) {
    constexpr std::array pages{ std::string_view("browser"),
        std::string_view("mixer"), std::string_view("parameter"),
        std::string_view("smartplay") };
    const auto index = static_cast<std::size_t>(state.page);
    MsgpackValue page = index < pages.size()
                                ? kks_protocol_symbol(pages[index], registry)
                                : MsgpackValue{};
    MsgpackValue result(MsgpackValue::Map{ { "page", std::move(page) },
            { "has_lightguide_focus", state.has_lightguide_focus } });
    if (registry)
        compact_map_keys(result, *registry);
    return result;
}

MsgpackValue lightguide_value(const LightguideState &state) {
    return std::visit(
            [](const auto &value) -> MsgpackValue {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, DeviceOwnedLightguide>) {
                    return MsgpackValue(
                            MsgpackValue::Map{ { "keybed", MsgpackValue{} } });
                } else {
                    MsgpackValue::Array leds;
                    leds.reserve(value.size());
                    for (const auto &led : value) {
                        leds.emplace_back(MsgpackValue::Array{
                                MsgpackValue(std::int64_t(led.midi_note)),
                                MsgpackValue(std::uint64_t(led.value)) });
                    }
                    return MsgpackValue(
                            MsgpackValue::Map{ { "client_midi_addressing",
                                    MsgpackValue(std::move(leds)) } });
                }
            },
            state);
}

std::optional<MsgpackValue> plugin_chain_value(
        const OdrPluginChainData &data, const OdrSymbolRegistry *registry) {
    constexpr std::array type_names{ std::string_view("fx"),
        std::string_view("instrument") };
    constexpr std::array version_names{ std::string_view("non_nks"),
        std::string_view("nks1"), std::string_view("nks2") };
    MsgpackValue::Array plugins;
    plugins.reserve(data.plugins.size());
    for (const auto &plugin : data.plugins) {
        MsgpackValue type;
        if (plugin.type) {
            const auto index = static_cast<std::size_t>(*plugin.type);
            if (index >= type_names.size())
                return std::nullopt;
            type = kks_protocol_symbol(type_names[index], registry);
        }
        MsgpackValue version;
        if (plugin.supported_nks_version) {
            const auto index =
                    static_cast<std::size_t>(*plugin.supported_nks_version);
            if (index >= version_names.size())
                return std::nullopt;
            version = kks_protocol_symbol(version_names[index], registry);
        }
        plugins.emplace_back(MsgpackValue::Map{ { "name", plugin.name },
                { "identifier", MsgpackValue::Binary(plugin.identifier.begin(),
                                        plugin.identifier.end()) },
                { "can_bypass", plugin.can_bypass },
                { "bypass_enabled", plugin.bypass_enabled },
                { "can_remove", plugin.can_remove },
                { "can_move_forward", plugin.can_move_forward },
                { "can_move_backward", plugin.can_move_backward },
                { "type", std::move(type) },
                { "supported_nks_version", std::move(version) } });
    }
    MsgpackValue model(MsgpackValue::Map{ { "plugins", std::move(plugins) },
            { "current_plugin_index",
                    std::int64_t(data.current_plugin_index) } });
    if (registry)
        compact_map_keys(model, *registry);
    return model;
}

std::optional<MsgpackValue> mixer_track_type_value(
        KksMixerTrackType type, const OdrSymbolRegistry *registry) {
    constexpr std::array names{ std::string_view("generic_track"),
        std::string_view("audio"), std::string_view("midi"),
        std::string_view("group"), std::string_view("returnBus"),
        std::string_view("master"), std::string_view("empty") };
    const auto index = static_cast<std::size_t>(type);
    if (index >= names.size())
        return std::nullopt;
    return kks_protocol_symbol(names[index], registry);
}

std::optional<MsgpackValue> mixer_parameter_info_value(
        const KksMixerParameterInfo &parameter,
        const OdrSymbolRegistry *registry) {
    constexpr std::array display_names{ std::string_view("unipolar_knob"),
        std::string_view("bipolar_knob"), std::string_view("switch_button"),
        std::string_view("text") };
    const auto index = static_cast<std::size_t>(parameter.display_type);
    if (index >= display_names.size())
        return std::nullopt;
    return MsgpackValue(MsgpackValue::Map{ { "name", parameter.name },
            { "value", parameter.value },
            { "display_value", parameter.display_value },
            { "display_type",
                    kks_protocol_symbol(display_names[index], registry) },
            { "section_name", parameter.section_name } });
}

std::optional<MsgpackValue> mixer_track_info_value(
        const KksMixerTrackInfo &track, const OdrSymbolRegistry *registry) {
    auto type = mixer_track_type_value(track.type, registry);
    if (!type)
        return std::nullopt;
    MsgpackValue::Array meter{ MsgpackValue(track.meter[0]),
        MsgpackValue(track.meter[1]) };
    return MsgpackValue(MsgpackValue::Map{ { "arm", track.arm },
            { "automation", track.automation }, { "color", track.color },
            { "meter", std::move(meter) }, { "mute", track.mute },
            { "mute_by_solo", track.mute_by_solo }, { "name", track.name },
            { "pan", track.pan }, { "pan_label", track.pan_label },
            { "selected", track.selected }, { "short_name", track.short_name },
            { "solo", track.solo }, { "type", std::move(*type) },
            { "volume", track.volume },
            { "volume_label", track.volume_label } });
}

std::optional<MsgpackValue> mixer_track_data_value(
        const KksMixerModelTrackData &data, const OdrSymbolRegistry *registry) {
    MsgpackValue::Array tracks;
    tracks.reserve(data.track_infos.size());
    for (const auto &track : data.track_infos) {
        auto encoded = mixer_track_info_value(track, registry);
        if (!encoded)
            return std::nullopt;
        tracks.push_back(std::move(*encoded));
    }
    MsgpackValue::Array parameters;
    parameters.reserve(data.selected_track_parameters.size());
    for (const auto &parameter : data.selected_track_parameters) {
        auto encoded = mixer_parameter_info_value(parameter, registry);
        if (!encoded)
            return std::nullopt;
        parameters.push_back(std::move(*encoded));
    }
    MsgpackValue model(MsgpackValue::Map{
            { "focused_track", std::int64_t(data.focused_track) },
            { "track_infos", std::move(tracks) },
            { "selected_track_parameters", std::move(parameters) },
            { "current_parameter_page",
                    std::int64_t(data.current_parameter_page) },
            { "number_of_parameter_pages",
                    std::int64_t(data.number_of_parameter_pages) },
            { "plugin_preset_name", data.plugin_preset_name },
            { "plugin_track_name", data.plugin_track_name } });
    if (registry)
        compact_map_keys(model, *registry);
    return model;
}

std::optional<MsgpackValue> kks_browser_sound_item_value(
        const KksBrowserSoundItem &item, const OdrSymbolRegistry *registry) {
    MsgpackValue icon;
    if (item.icon) {
        if (*item.icon != OdrSoundIcon::keyboard)
            return std::nullopt;
        icon = kks_protocol_symbol("keyboard", registry);
    }
    MsgpackValue asset;
    if (item.asset_image)
        asset = *item.asset_image;
    MsgpackValue result(MsgpackValue::Map{ { "name", item.name },
            { "icon", std::move(icon) }, { "asset_image", std::move(asset) },
            { "is_user_saved", item.is_user_saved },
            { "is_favorite", item.is_favorite },
            { "identifier", item.identifier } });
    if (registry)
        compact_map_keys(result, *registry);
    return result;
}

std::optional<MsgpackValue> kks_browser_sounds_value(
        const KksBrowserSoundResults &sounds,
        const OdrSymbolRegistry *registry) {
    MsgpackValue::Array items;
    items.reserve(sounds.items.size());
    for (const auto &item : sounds.items) {
        auto encoded = kks_browser_sound_item_value(item, registry);
        if (!encoded)
            return std::nullopt;
        items.push_back(std::move(*encoded));
    }
    MsgpackValue selected;
    if (sounds.selected_index)
        selected = std::int64_t(*sounds.selected_index);
    MsgpackValue result(MsgpackValue::Map{ { "items", std::move(items) },
            { "selected_index", std::move(selected) } });
    if (registry)
        compact_map_keys(result, *registry);
    return result;
}

MsgpackValue kks_browser_named_item_value(const OdrBrowserNamedItem &item) {
    return MsgpackValue(MsgpackValue::Map{ { "name", item.name } });
}

MsgpackValue kks_browser_product_item_value(const KksBrowserProductItem &item) {
    MsgpackValue thumbnail;
    if (item.thumbnail_image)
        thumbnail = *item.thumbnail_image;
    MsgpackValue accessible;
    if (item.accessible_name)
        accessible = *item.accessible_name;
    return MsgpackValue(MsgpackValue::Map{ { "name", item.name },
            { "thumbnail_image", std::move(thumbnail) },
            { "accessible_name", std::move(accessible) } });
}

template <typename Filter, typename Encoder>
MsgpackValue kks_browser_single_filter_value(
        const Filter &filter, Encoder &&encoder) {
    MsgpackValue::Array items;
    items.reserve(filter.items.size());
    for (const auto &item : filter.items)
        items.push_back(encoder(item));
    MsgpackValue selected;
    if (filter.selected_index)
        selected = std::int64_t(*filter.selected_index);
    return MsgpackValue(MsgpackValue::Map{ { "items", std::move(items) },
            { "selected_index", std::move(selected) } });
}

MsgpackValue kks_browser_multi_filter_value(
        const OdrBrowserMultiSelection &filter) {
    MsgpackValue::Array items;
    items.reserve(filter.items.size());
    for (const auto &item : filter.items) {
        items.emplace_back(MsgpackValue::Map{
                { "name", item.name }, { "selected", item.selected } });
    }
    MsgpackValue::Array selected;
    selected.reserve(filter.selected_indexes.size());
    for (const auto index : filter.selected_indexes)
        selected.emplace_back(std::int64_t(index));
    return MsgpackValue(MsgpackValue::Map{ { "items", std::move(items) },
            { "selected_indexes", std::move(selected) } });
}

MsgpackValue kks_browser_carousel_value(const OdrBrowserCarousel &filter) {
    MsgpackValue::Array items;
    items.reserve(filter.items.size());
    for (const auto &item : filter.items)
        items.push_back(kks_browser_named_item_value(item));
    return MsgpackValue(MsgpackValue::Map{ { "items", std::move(items) },
            { "selected_index", std::int64_t(filter.selected_index) } });
}

MsgpackValue kks_browser_filters_value(
        const KksBrowserFilters &filters, const OdrSymbolRegistry *registry) {
    const auto named = [](const OdrBrowserNamedItem &item) {
        return kks_browser_named_item_value(item);
    };
    const auto product = [](const KksBrowserProductItem &item) {
        return kks_browser_product_item_value(item);
    };
    MsgpackValue result(MsgpackValue::Map{
            { "brands",
                    kks_browser_single_filter_value(filters.brands, named) },
            { "banks", kks_browser_single_filter_value(filters.banks, named) },
            { "subbanks",
                    kks_browser_single_filter_value(filters.subbanks, named) },
            { "types", kks_browser_multi_filter_value(filters.types) },
            { "subtypes", kks_browser_multi_filter_value(filters.subtypes) },
            { "characters",
                    kks_browser_multi_filter_value(filters.characters) },
            { "products", kks_browser_single_filter_value(
                                  filters.products, product) },
            { "file_types", kks_browser_carousel_value(filters.file_types) },
            { "user_saved", filters.user_saved },
            { "favorites", filters.favorites } });
    if (registry)
        compact_map_keys(result, *registry);
    return result;
}

std::optional<MsgpackValue> kks_browser_filter_value(
        OdrBrowserFilterProperty property, const KksBrowserFilter &filter,
        const OdrSymbolRegistry *registry) {
    MsgpackValue result;
    switch (property) {
        case OdrBrowserFilterProperty::brands:
            if (const auto *value = std::get_if<OdrBrowserBrandFilter>(&filter))
                result = kks_browser_single_filter_value(
                        *value, kks_browser_named_item_value);
            else
                return std::nullopt;
            break;
        case OdrBrowserFilterProperty::banks:
        case OdrBrowserFilterProperty::subbanks:
            if (const auto *value = std::get_if<OdrBrowserBankFilter>(&filter))
                result = kks_browser_single_filter_value(
                        *value, kks_browser_named_item_value);
            else
                return std::nullopt;
            break;
        case OdrBrowserFilterProperty::types:
        case OdrBrowserFilterProperty::subtypes:
        case OdrBrowserFilterProperty::characters:
            if (const auto *value =
                            std::get_if<OdrBrowserMultiSelection>(&filter))
                result = kks_browser_multi_filter_value(*value);
            else
                return std::nullopt;
            break;
        case OdrBrowserFilterProperty::products:
            if (const auto *value =
                            std::get_if<KksBrowserProductFilter>(&filter))
                result = kks_browser_single_filter_value(
                        *value, kks_browser_product_item_value);
            else
                return std::nullopt;
            break;
        case OdrBrowserFilterProperty::file_types:
            if (const auto *value = std::get_if<OdrBrowserCarousel>(&filter))
                result = kks_browser_carousel_value(*value);
            else
                return std::nullopt;
            break;
        case OdrBrowserFilterProperty::user_saved:
        case OdrBrowserFilterProperty::favorites:
            if (const auto *value = std::get_if<bool>(&filter))
                result = *value;
            else
                return std::nullopt;
            break;
    }
    if (registry)
        compact_map_keys(result, *registry);
    return result;
}

std::optional<MsgpackValue> kks_browser_selection_value(
        OdrBrowserFilterProperty property,
        const KksBrowserFilterSelection &selection) {
    switch (property) {
        case OdrBrowserFilterProperty::brands:
        case OdrBrowserFilterProperty::banks:
        case OdrBrowserFilterProperty::subbanks:
        case OdrBrowserFilterProperty::products:
            if (const auto *value = std::get_if<std::optional<std::int32_t>>(
                        &selection)) {
                return *value ? MsgpackValue(std::int64_t(**value))
                              : MsgpackValue{};
            }
            return std::nullopt;
        case OdrBrowserFilterProperty::types:
        case OdrBrowserFilterProperty::subtypes:
        case OdrBrowserFilterProperty::characters:
            if (const auto *value = std::get_if<std::vector<std::int32_t>>(
                        &selection)) {
                MsgpackValue::Array indexes;
                indexes.reserve(value->size());
                for (const auto index : *value)
                    indexes.emplace_back(std::int64_t(index));
                return MsgpackValue(std::move(indexes));
            }
            return std::nullopt;
        case OdrBrowserFilterProperty::file_types:
            if (const auto *value = std::get_if<std::int32_t>(&selection))
                return MsgpackValue(std::int64_t(*value));
            return std::nullopt;
        case OdrBrowserFilterProperty::user_saved:
        case OdrBrowserFilterProperty::favorites:
            if (const auto *value = std::get_if<bool>(&selection))
                return MsgpackValue(*value);
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string_view> kks_browser_property_name(
        OdrBrowserFilterProperty property) {
    constexpr std::array names{ std::string_view("brands"),
        std::string_view("banks"), std::string_view("subbanks"),
        std::string_view("types"), std::string_view("subtypes"),
        std::string_view("characters"), std::string_view("products"),
        std::string_view("file_types"), std::string_view("user_saved"),
        std::string_view("favorites") };
    const auto index = static_cast<std::size_t>(property);
    if (index >= names.size())
        return std::nullopt;
    return names[index];
}

MsgpackValue kks_smartplay_choice_value(const KksSmartplayChoice &choice) {
    MsgpackValue::Array items;
    items.reserve(choice.items.size());
    for (const auto &item : choice.items)
        items.emplace_back(item);
    return MsgpackValue(MsgpackValue::Map{ { "items", std::move(items) },
            { "selection", std::uint64_t(choice.selection) } });
}

template <typename T>
MsgpackValue kks_smartplay_range_value(const KksSmartplayRange<T> &range) {
    const auto encode = [](T value) -> MsgpackValue {
        if constexpr (std::is_same_v<T, float>)
            return MsgpackValue(value);
        else
            return MsgpackValue(std::uint64_t(value));
    };
    return MsgpackValue(MsgpackValue::Map{ { "min", encode(range.min) },
            { "max", encode(range.max) }, { "value", encode(range.value) } });
}

MsgpackValue kks_smartplay_shortcuts_value(
        const std::array<KksSmartplaySoftbuttonShortcut, 4> &shortcuts) {
    MsgpackValue::Array values;
    values.reserve(shortcuts.size());
    for (const auto &shortcut : shortcuts) {
        values.emplace_back(MsgpackValue::Map{
                { "label", shortcut.label }, { "enabled", shortcut.enabled } });
    }
    return MsgpackValue(std::move(values));
}

MsgpackValue kks_smartplay_arp_mode_value(const KksSmartplayArpMode &mode) {
    MsgpackValue swing;
    if (mode.swing)
        swing = kks_smartplay_range_value(*mode.swing);
    return MsgpackValue(MsgpackValue::Map{
            { "type", kks_smartplay_choice_value(mode.type) },
            { "rate", kks_smartplay_choice_value(mode.rate) },
            { "sequence", kks_smartplay_choice_value(mode.sequence) },
            { "swing", std::move(swing) },
            { "octaves", kks_smartplay_choice_value(mode.octaves) },
            { "dynamic", kks_smartplay_range_value(mode.dynamic) },
            { "gate", kks_smartplay_range_value(mode.gate) },
            { "retrigger", kks_smartplay_choice_value(mode.retrigger) },
            { "repeat", kks_smartplay_choice_value(mode.repeat) },
            { "offset", kks_smartplay_choice_value(mode.offset) },
            { "inversion", kks_smartplay_choice_value(mode.inversion) },
            { "min_key", kks_smartplay_range_value(mode.min_key) },
            { "max_key", kks_smartplay_range_value(mode.max_key) },
            { "hold", mode.hold },
            { "softbutton_shortcuts", kks_smartplay_shortcuts_value(
                                              mode.softbutton_shortcuts) } });
}

MsgpackValue kks_smartplay_note_repeat_value(
        const KksSmartplayNoteRepeatMode &mode) {
    MsgpackValue swing;
    if (mode.swing)
        swing = kks_smartplay_range_value(*mode.swing);
    return MsgpackValue(MsgpackValue::Map{
            { "rate", kks_smartplay_choice_value(mode.rate) },
            { "swing", std::move(swing) },
            { "gate", kks_smartplay_range_value(mode.gate) },
            { "hold", mode.hold },
            { "softbutton_shortcuts", kks_smartplay_shortcuts_value(
                                              mode.softbutton_shortcuts) } });
}

MsgpackValue kks_smartplay_arp_engine_value(
        const KksSmartplayArpEngine &engine) {
    MsgpackValue mode = std::visit(
            [](const auto &value) -> MsgpackValue {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, KksSmartplayArpMode>) {
                    return MsgpackValue(MsgpackValue::Map{ { "arpeggiator",
                            kks_smartplay_arp_mode_value(value) } });
                } else {
                    return MsgpackValue(MsgpackValue::Map{ { "note_repeat",
                            kks_smartplay_note_repeat_value(value) } });
                }
            },
            engine.engine_model);
    return MsgpackValue(MsgpackValue::Map{ { "enabled", engine.enabled },
            { "engine_model", std::move(mode) },
            { "mode_interactable", engine.mode_interactable } });
}

MsgpackValue kks_smartplay_scale_model_value(
        const KksSmartplayScaleModel &scale) {
    return MsgpackValue(MsgpackValue::Map{
            { "bank", kks_smartplay_choice_value(scale.bank) },
            { "type", kks_smartplay_choice_value(scale.type) } });
}

MsgpackValue kks_smartplay_scale_engine_value(
        const KksSmartplayScaleEngine &engine) {
    MsgpackValue mode = std::visit(
            [](const auto &value) -> MsgpackValue {
                using T = std::decay_t<decltype(value)>;
                MsgpackValue fields;
                std::string_view name;
                if constexpr (std::is_same_v<T, KksSmartplayScaleMode>) {
                    name = "scale";
                    fields = MsgpackValue(MsgpackValue::Map{
                            { "root_note", kks_smartplay_choice_value(
                                                   value.root_note) },
                            { "scale", kks_smartplay_scale_model_value(
                                               value.scale) },
                            { "key_mapping", kks_smartplay_choice_value(
                                                     value.key_mapping) } });
                } else if constexpr (std::is_same_v<T,
                                             KksSmartplayHarmonizerMode>) {
                    name = "harmonizer";
                    fields = MsgpackValue(MsgpackValue::Map{
                            { "root_note", kks_smartplay_choice_value(
                                                   value.root_note) },
                            { "scale", kks_smartplay_scale_model_value(
                                               value.scale) },
                            { "key_mapping", kks_smartplay_choice_value(
                                                     value.key_mapping) },
                            { "type", kks_smartplay_choice_value(value.type) },
                            { "position", kks_smartplay_choice_value(
                                                  value.position) } });
                } else if constexpr (std::is_same_v<T,
                                             KksSmartplayChordSetMode>) {
                    name = "chord_set";
                    fields = MsgpackValue(MsgpackValue::Map{
                            { "root_note", kks_smartplay_choice_value(
                                                   value.root_note) },
                            { "type", kks_smartplay_choice_value(value.type) },
                            { "position", kks_smartplay_choice_value(
                                                  value.position) } });
                } else {
                    name = "user_chord";
                    fields = MsgpackValue(MsgpackValue::Map{
                            { "root_note", kks_smartplay_choice_value(
                                                   value.root_note) },
                            { "type", kks_smartplay_choice_value(value.type) },
                            { "position", kks_smartplay_choice_value(
                                                  value.position) } });
                }
                return MsgpackValue(MsgpackValue::Map{
                        { MsgpackValue(name), std::move(fields) } });
            },
            engine.engine_model);
    return MsgpackValue(MsgpackValue::Map{ { "enabled", engine.enabled },
            { "engine_model", std::move(mode) },
            { "user_chords_enabled", engine.user_chords_enabled } });
}

MsgpackValue kks_smartplay_engine_state_value(
        const KksSmartplayEngineState &state,
        const OdrSymbolRegistry *registry) {
    MsgpackValue value(MsgpackValue::Map{
            { "arpeggiator_engine",
                    kks_smartplay_arp_engine_value(state.arpeggiator_engine) },
            { "scale_engine",
                    kks_smartplay_scale_engine_value(state.scale_engine) } });
    if (registry)
        compact_map_keys(value, *registry);
    return value;
}

bool kks_smartplay_choice_valid(const KksSmartplayChoice &choice) {
    return std::ranges::all_of(choice.items,
            [](const std::string &item) { return item.size() <= 16; });
}

bool kks_smartplay_shortcuts_valid(
        const std::array<KksSmartplaySoftbuttonShortcut, 4> &shortcuts) {
    return std::ranges::all_of(
            shortcuts, [](const KksSmartplaySoftbuttonShortcut &shortcut) {
                return shortcut.label.size() <= 16;
            });
}

bool kks_smartplay_state_valid(const KksSmartplayEngineState &state) {
    const bool arp = std::visit(
            [](const auto &mode) {
                using T = std::decay_t<decltype(mode)>;
                if constexpr (std::is_same_v<T, KksSmartplayArpMode>) {
                    return kks_smartplay_choice_valid(mode.type) &&
                           kks_smartplay_choice_valid(mode.rate) &&
                           kks_smartplay_choice_valid(mode.sequence) &&
                           kks_smartplay_choice_valid(mode.octaves) &&
                           kks_smartplay_choice_valid(mode.retrigger) &&
                           kks_smartplay_choice_valid(mode.repeat) &&
                           kks_smartplay_choice_valid(mode.offset) &&
                           kks_smartplay_choice_valid(mode.inversion) &&
                           kks_smartplay_shortcuts_valid(
                                   mode.softbutton_shortcuts);
                } else {
                    return kks_smartplay_choice_valid(mode.rate) &&
                           kks_smartplay_shortcuts_valid(
                                   mode.softbutton_shortcuts);
                }
            },
            state.arpeggiator_engine.engine_model);
    const bool scale = std::visit(
            [](const auto &mode) {
                using T = std::decay_t<decltype(mode)>;
                if constexpr (std::is_same_v<T, KksSmartplayScaleMode>) {
                    return kks_smartplay_choice_valid(mode.root_note) &&
                           kks_smartplay_choice_valid(mode.scale.bank) &&
                           kks_smartplay_choice_valid(mode.scale.type) &&
                           kks_smartplay_choice_valid(mode.key_mapping);
                } else if constexpr (std::is_same_v<T,
                                             KksSmartplayHarmonizerMode>) {
                    return kks_smartplay_choice_valid(mode.root_note) &&
                           kks_smartplay_choice_valid(mode.type) &&
                           kks_smartplay_choice_valid(mode.position) &&
                           kks_smartplay_choice_valid(mode.scale.bank) &&
                           kks_smartplay_choice_valid(mode.scale.type) &&
                           kks_smartplay_choice_valid(mode.key_mapping);
                } else {
                    return kks_smartplay_choice_valid(mode.root_note) &&
                           kks_smartplay_choice_valid(mode.type) &&
                           kks_smartplay_choice_valid(mode.position);
                }
            },
            state.scale_engine.engine_model);
    return arp && scale;
}

MsgpackValue kks_project_track_value(const KksProjectTrack &track) {
    MsgpackValue::Array children;
    children.reserve(track.children.size());
    for (const auto &child : track.children)
        children.push_back(kks_project_track_value(child));
    return MsgpackValue(MsgpackValue::Map{ { "id", std::uint64_t(track.id) },
            { "name", track.name },
            { "track_color", rgb_color(track.track_color) },
            { "children", std::move(children) },
            { "is_collapsed", track.is_collapsed } });
}

std::optional<MsgpackValue> kks_track_plugin_chain_element_value(
        const KksTrackPluginChainElement &element) {
    MsgpackValue::Map fields{
        { "identifier", MsgpackValue::Binary(element.identifier.begin(),
                                element.identifier.end()) },
        { "name", element.name }, { "can_bypass", element.can_bypass },
        { "bypass_enabled", element.bypass_enabled },
        { "can_remove", element.can_remove },
        { "can_move_forward", element.can_move_forward },
        { "can_move_backward", element.can_move_backward }
    };
    std::string_view type;
    switch (element.type) {
        case KksTrackPluginChainElementType::plugin:
            type = "plugin";
            break;
        case KksTrackPluginChainElementType::container: {
            type = "container";
            MsgpackValue::Array children;
            children.reserve(element.children.size());
            for (const auto &child : element.children) {
                auto value = kks_track_plugin_chain_element_value(child);
                if (!value)
                    return std::nullopt;
                children.push_back(std::move(*value));
            }
            fields.emplace_back("children", MsgpackValue(std::move(children)));
            fields.emplace_back("is_collapsed", element.is_collapsed);
            break;
        }
        default:
            return std::nullopt;
    }
    return MsgpackValue(MsgpackValue::Map{
            { "type", type }, { "value", MsgpackValue(std::move(fields)) } });
}

std::optional<MsgpackValue> kks_track_plugin_chain_value(
        const KksTrackPluginChainData &data,
        const OdrSymbolRegistry *registry) {
    MsgpackValue::Array plugins;
    plugins.reserve(data.plugins.size());
    for (const auto &element : data.plugins) {
        auto value = kks_track_plugin_chain_element_value(element);
        if (!value)
            return std::nullopt;
        plugins.push_back(std::move(*value));
    }
    MsgpackValue selected;
    if (data.selected_plugin_id) {
        selected = MsgpackValue::Binary(data.selected_plugin_id->begin(),
                data.selected_plugin_id->end());
    }
    return MsgpackValue(
            MsgpackValue::Map{ { kks_protocol_symbol("plugins", registry),
                                       MsgpackValue(std::move(plugins)) },
                    { kks_protocol_symbol("selected_plugin_id", registry),
                            std::move(selected) } });
}

KksRoutedFrame kks_client_model_notification(std::string_view method,
        const RpcMethodId &client, MsgpackValue value,
        const OdrSymbolRegistry *registry, bool compact = true) {
    if (registry && compact)
        compact_map_keys(value, *registry);
    const std::array parameters{ kks_client_symbol(client, registry),
        std::move(value) };
    return { KksUsbChannel::primary,
        encode_rpc_notification(kks_method(method, registry), parameters) };
}

} // namespace

KksRoutedFrame encode_kks_host_handshake(const KksHostHandshake &handshake) {
    MsgpackValue::Array symbols;
    symbols.reserve(handshake.symbol_registry.size());
    for (const auto &symbol : handshake.symbol_registry)
        symbols.emplace_back(symbol);
    MsgpackValue::Map fields{
        { "host_protocol_version",
                protocol_version_text(handshake.host_protocol_version) },
        { "symbol_registry", MsgpackValue(std::move(symbols)) },
        { "agent_version", handshake.agent_version },
        { "utc_clock", handshake.utc_clock },
        { "disable_heartbeat_validation",
                handshake.disable_heartbeat_validation },
    };
    const std::array parameters{ MsgpackValue(std::move(fields)) };
    return { KksUsbChannel::primary,
        encode_rpc_notification(std::string("handshake"), parameters) };
}

KksRoutedFrame encode_kks_heartbeat() {
    return { KksUsbChannel::heartbeat,
        encode_rpc_notification(std::string("heartbeat"), {}) };
}

KksRoutedFrame encode_kks_goodbye() {
    return { KksUsbChannel::primary,
        encode_rpc_notification(std::string("goodbye"), {}) };
}

KksRoutedFrame encode_kks_asset_add(std::uint64_t device_asset_id,
        const std::array<std::byte, 32> &checksum,
        std::span<const std::byte> payload) {
    const std::array parameters{
        MsgpackValue(device_asset_id),
        MsgpackValue(MsgpackValue::Binary(checksum.begin(), checksum.end())),
        MsgpackValue(MsgpackValue::Binary(payload.begin(), payload.end())),
    };
    return { KksUsbChannel::primary,
        encode_rpc_notification(std::string("assets_add"), parameters) };
}

KksRoutedFrame encode_kks_asset_remove(std::uint64_t device_asset_id) {
    const std::array parameters{ MsgpackValue(device_asset_id) };
    return { KksUsbChannel::primary,
        encode_rpc_notification(std::string("assets_remove"), parameters) };
}

KksRoutedFrame encode_kks_assets_reset() {
    return { KksUsbChannel::primary,
        encode_rpc_notification(std::string("assets_reset"), {}) };
}

std::optional<KksRoutedFrame> encode_kks_client_instance_add(
        const RpcMethodId &client, const OdrInstanceInfo &info,
        const OdrSymbolRegistry *registry) {
    constexpr std::array type_names{ std::string_view("standalone"),
        std::string_view("vst2"), std::string_view("vst3"),
        std::string_view("audio_unit"), std::string_view("aax") };
    const auto type_index = static_cast<std::size_t>(info.type);
    if (type_index >= type_names.size())
        return std::nullopt;

    MsgpackValue model(MsgpackValue::Map{ { "name", info.name },
            { "version", info.version },
            { "type", kks_protocol_symbol(type_names[type_index], registry) },
            { "is_host", info.is_host } });
    if (registry)
        compact_map_keys(model, *registry);

    auto client_value = kks_client_symbol(client, registry);
    const auto method = kks_method("client_instance_add", registry);
    const std::array parameters{ std::move(client_value), std::move(model) };
    return KksRoutedFrame{ KksUsbChannel::primary,
        encode_rpc_notification(method, parameters) };
}

KksRoutedFrame encode_kks_client_instance_remove(
        const RpcMethodId &client, const OdrSymbolRegistry *registry) {
    auto client_value = kks_client_symbol(client, registry);
    const auto method = kks_method("client_instance_remove", registry);
    const std::array parameters{ std::move(client_value) };
    return { KksUsbChannel::primary,
        encode_rpc_notification(method, parameters) };
}

namespace {
KksRoutedFrame encode_kks_client_focus(std::string_view method_name,
        const RpcMethodId &client, const OdrSymbolRegistry *registry) {
    const std::array parameters{ kks_client_symbol(client, registry) };
    return { KksUsbChannel::primary,
        encode_rpc_notification(
                kks_method(method_name, registry), parameters) };
}
} // namespace

KksRoutedFrame encode_kks_client_focus_acquire(
        const RpcMethodId &client, const OdrSymbolRegistry *registry) {
    return encode_kks_client_focus("client_focus_acquire", client, registry);
}

KksRoutedFrame encode_kks_client_focus_release(
        const RpcMethodId &client, const OdrSymbolRegistry *registry) {
    return encode_kks_client_focus("client_focus_release", client, registry);
}

KksRoutedFrame encode_kks_client_page_set(const RpcMethodId &client,
        OdrInstancePage page, const OdrSymbolRegistry *registry) {
    constexpr std::array pages{ std::string_view("browser"),
        std::string_view("mixer"), std::string_view("parameter"),
        std::string_view("smartplay") };
    const auto index = static_cast<std::size_t>(page);
    MsgpackValue value = index < pages.size()
                                 ? kks_protocol_symbol(pages[index], registry)
                                 : MsgpackValue{};
    const std::array parameters{ kks_client_symbol(client, registry),
        std::move(value) };
    return { KksUsbChannel::primary,
        encode_rpc_notification(
                kks_method("client_page_set", registry), parameters) };
}

std::optional<KksRoutedFrame> encode_kks_client_instance_set_host_owned_state(
        const RpcMethodId &client, const OdrHostOwnedInstanceState &state,
        const OdrSymbolRegistry *registry) {
    auto model = host_owned_instance_state_value(state, registry);
    if (!model)
        return std::nullopt;

    auto client_value = kks_client_symbol(client, registry);
    constexpr std::string_view method_name =
            "client_instance_set_host_owned_instance_client_state";
    const auto method = kks_method(method_name, registry);
    const std::array parameters{ std::move(client_value), std::move(*model) };
    return KksRoutedFrame{ KksUsbChannel::primary,
        encode_rpc_notification(method, parameters) };
}

KksRoutedFrame encode_kks_client_instance_set_device_owned_state(
        const RpcMethodId &client, const OdrDeviceOwnedInstanceState &state,
        const OdrSymbolRegistry *registry) {
    return kks_client_model_notification(
            "client_instance_set_device_owned_instance_client_state", client,
            kks_device_owned_instance_state(state, registry), registry);
}

KksRoutedFrame encode_kks_parameter_page_set(const RpcMethodId &client,
        const KksParameterPageData &data, const OdrSymbolRegistry *registry) {
    MsgpackValue model(MsgpackValue::Map{
            { "plugin_data", kks_plugin_data(data.plugin_data) },
            { "device_owned_viewstate", kks_parameter_page_device_viewstate(
                                                data.device_owned_viewstate) },
            { "host_owned_viewstate",
                    kks_host_viewstate(data.host_owned_viewstate) } });
    if (registry)
        compact_map_keys(model, *registry);
    // parameter maps keep literal keys even when the outer model uses symbols,
    // so rebuild this map after compacting the model
    auto &plugin = std::get<MsgpackValue::Map>(
            std::get<MsgpackValue::Map>(model.value)[0].second.value);
    plugin[2].second = kks_parameter_map(data.plugin_data.parameters);
    return kks_client_model_notification(
            "client_instance_parameter_page_model_set", client,
            std::move(model), registry, false);
}

KksRoutedFrame encode_kks_parameter_page_set_plugin_data(
        const RpcMethodId &client, const KksParameterPagePluginData &data,
        const OdrSymbolRegistry *registry) {
    auto model = kks_plugin_data(data);
    if (registry)
        compact_map_keys(model, *registry);
    std::get<MsgpackValue::Map>(model.value)[2].second =
            kks_parameter_map(data.parameters);
    return kks_client_model_notification(
            "client_instance_parameter_page_model_set_plugin_data", client,
            std::move(model), registry, false);
}

KksRoutedFrame encode_kks_parameter_page_set_host_owned_viewstate(
        const RpcMethodId &client, const OdrParameterPageHostViewState &state,
        const OdrSymbolRegistry *registry) {
    return kks_client_model_notification(
            "client_instance_parameter_page_model_set_host_owned_viewstate",
            client, kks_host_viewstate(state), registry);
}

KksRoutedFrame encode_kks_parameter_page_set_device_owned_viewstate(
        const RpcMethodId &client, const OdrParameterPageDeviceViewState &state,
        const OdrSymbolRegistry *registry) {
    return kks_client_model_notification(
            "client_instance_parameter_page_model_set_device_owned_viewstate",
            client, kks_parameter_page_device_viewstate(state), registry);
}

std::optional<KksRoutedFrame> encode_kks_parameter_page_update_property(
        const RpcMethodId &client, KksParameterPagePluginDataProperty property,
        const KksParameterPagePluginDataPropertyValue &value,
        const OdrSymbolRegistry *registry) {
    constexpr std::array property_names{ std::string_view("name"),
        std::string_view("background"), std::string_view("parameters"),
        std::string_view("plugin_color"),
        std::string_view("nks_control_color") };
    const auto index = static_cast<std::size_t>(property);
    if (index >= property_names.size())
        return std::nullopt;

    MsgpackValue encoded;
    switch (property) {
        case KksParameterPagePluginDataProperty::name:
            if (const auto *input = std::get_if<std::string>(&value))
                encoded = *input;
            else
                return std::nullopt;
            break;
        case KksParameterPagePluginDataProperty::background:
            if (const auto *input =
                            std::get_if<std::optional<std::uint64_t>>(&value)) {
                if (*input)
                    encoded = **input;
            } else
                return std::nullopt;
            break;
        case KksParameterPagePluginDataProperty::parameters:
            if (const auto *input = std::get_if<
                        std::vector<std::pair<std::uint32_t, OdrParameter>>>(
                        &value))
                encoded = kks_parameter_map(*input);
            else
                return std::nullopt;
            break;
        case KksParameterPagePluginDataProperty::plugin_color:
        case KksParameterPagePluginDataProperty::nks_control_color:
            if (const auto *input =
                            std::get_if<std::optional<OdrRgbColor>>(&value))
                encoded = rgb_color(*input);
            else
                return std::nullopt;
            break;
    }
    if (registry && property != KksParameterPagePluginDataProperty::parameters)
        compact_map_keys(encoded, *registry);
    const std::array parameters{ kks_client_symbol(client, registry),
        kks_protocol_symbol(property_names[index], registry),
        std::move(encoded) };
    constexpr std::string_view method_name =
            "client_instance_parameter_page_model_update_property";
    return KksRoutedFrame{ KksUsbChannel::primary,
        encode_rpc_notification(
                kks_method(method_name, registry), parameters) };
}

KksRoutedFrame encode_kks_parameter_page_update_parameter_value(
        const RpcMethodId &client, std::uint32_t parameter_id,
        const OdrParameterValue &value, const OdrSymbolRegistry *registry) {
    auto encoded = parameter_value(value);
    if (registry)
        compact_map_keys(encoded, *registry);
    const std::array parameters{ kks_client_symbol(client, registry),
        MsgpackValue(std::uint64_t(parameter_id)), std::move(encoded) };
    constexpr std::string_view method_name =
            "client_instance_parameter_page_model_update_parameter_value";
    return { KksUsbChannel::primary,
        encode_rpc_notification(
                kks_method(method_name, registry), parameters) };
}

KksRoutedFrame encode_kks_client_instance_set_lightguide(
        const RpcMethodId &client, const LightguideState &state,
        const OdrSymbolRegistry *registry) {
    return kks_client_model_notification("client_instance_lightguide_set",
            client, lightguide_value(state), registry);
}

std::optional<KksRoutedFrame> encode_kks_client_instance_set_plugin_chain(
        const RpcMethodId &client, const OdrPluginChainData &data,
        const OdrSymbolRegistry *registry) {
    auto model = plugin_chain_value(data, registry);
    if (!model)
        return std::nullopt;
    return kks_client_model_notification(
            "client_instance_plugin_chain_model_set", client, std::move(*model),
            registry);
}

KksRoutedFrame encode_kks_client_instance_set_plugin_chain_index(
        const RpcMethodId &client, std::int32_t current_plugin_index,
        const OdrSymbolRegistry *registry) {
    return kks_client_model_notification(
            "client_instance_plugin_chain_model_set_current_index", client,
            MsgpackValue(std::int64_t(current_plugin_index)), registry);
}

std::optional<KksRoutedFrame> encode_kks_mixer_set_track_data(
        const RpcMethodId &client, const KksMixerModelTrackData &data,
        const OdrSymbolRegistry *registry) {
    auto model = mixer_track_data_value(data, registry);
    if (!model)
        return std::nullopt;
    return kks_client_model_notification("client_instance_mixer_set_track_data",
            client, std::move(*model), registry);
}

std::optional<KksRoutedFrame> encode_kks_mixer_update_track_property(
        const RpcMethodId &client, std::int32_t track_index,
        KksMixerTrackProperty property, const KksMixerTrackPropertyValue &value,
        const OdrSymbolRegistry *registry) {
    constexpr std::array property_names{ std::string_view("arm"),
        std::string_view("automation"), std::string_view("color"),
        std::string_view("meter"), std::string_view("mute"),
        std::string_view("mute_by_solo"), std::string_view("name"),
        std::string_view("pan"), std::string_view("pan_label"),
        std::string_view("selected"), std::string_view("short_name"),
        std::string_view("solo"), std::string_view("type"),
        std::string_view("volume"), std::string_view("volume_label") };
    const auto index = static_cast<std::size_t>(property);
    if (index >= property_names.size())
        return std::nullopt;

    MsgpackValue encoded;
    switch (property) {
        case KksMixerTrackProperty::arm:
        case KksMixerTrackProperty::automation:
        case KksMixerTrackProperty::mute:
        case KksMixerTrackProperty::mute_by_solo:
        case KksMixerTrackProperty::selected:
        case KksMixerTrackProperty::solo:
            if (const auto *input = std::get_if<bool>(&value))
                encoded = *input;
            else
                return std::nullopt;
            break;
        case KksMixerTrackProperty::color:
        case KksMixerTrackProperty::name:
        case KksMixerTrackProperty::pan_label:
        case KksMixerTrackProperty::short_name:
        case KksMixerTrackProperty::volume_label:
            if (const auto *input = std::get_if<std::string>(&value))
                encoded = *input;
            else
                return std::nullopt;
            break;
        case KksMixerTrackProperty::meter:
            if (const auto *input = std::get_if<std::array<float, 2>>(&value)) {
                encoded = MsgpackValue(MsgpackValue::Array{
                        MsgpackValue((*input)[0]), MsgpackValue((*input)[1]) });
            } else
                return std::nullopt;
            break;
        case KksMixerTrackProperty::pan:
        case KksMixerTrackProperty::volume:
            if (const auto *input = std::get_if<float>(&value))
                encoded = *input;
            else
                return std::nullopt;
            break;
        case KksMixerTrackProperty::type:
            if (const auto *input = std::get_if<KksMixerTrackType>(&value)) {
                auto type = mixer_track_type_value(*input, registry);
                if (!type)
                    return std::nullopt;
                encoded = std::move(*type);
            } else
                return std::nullopt;
            break;
    }
    const std::array parameters{ kks_client_symbol(client, registry),
        MsgpackValue(std::int64_t(track_index)),
        kks_protocol_symbol(property_names[index], registry),
        std::move(encoded) };
    constexpr std::string_view method_name =
            "client_instance_mixer_update_track_property";
    return KksRoutedFrame{ KksUsbChannel::primary,
        encode_rpc_notification(
                kks_method(method_name, registry), parameters) };
}

std::optional<KksRoutedFrame> encode_kks_mixer_update_property(
        const RpcMethodId &client, KksMixerProperty property,
        const KksMixerPropertyValue &value, const OdrSymbolRegistry *registry) {
    constexpr std::array names{ std::string_view("focused_track"),
        std::string_view("track_infos"),
        std::string_view("selected_track_parameters"),
        std::string_view("current_parameter_page"),
        std::string_view("number_of_parameter_pages"),
        std::string_view("plugin_preset_name"),
        std::string_view("plugin_track_name") };
    const auto index = static_cast<std::size_t>(property);
    if (index >= names.size())
        return std::nullopt;
    MsgpackValue encoded;
    switch (property) {
        case KksMixerProperty::focused_track:
        case KksMixerProperty::current_parameter_page:
        case KksMixerProperty::number_of_parameter_pages:
            if (const auto *input = std::get_if<std::int32_t>(&value))
                encoded = std::int64_t(*input);
            else
                return std::nullopt;
            break;
        case KksMixerProperty::track_infos: {
            const auto *input =
                    std::get_if<std::vector<KksMixerTrackInfo>>(&value);
            if (!input)
                return std::nullopt;
            MsgpackValue::Array tracks;
            tracks.reserve(input->size());
            for (const auto &track : *input) {
                auto item = mixer_track_info_value(track, registry);
                if (!item)
                    return std::nullopt;
                tracks.push_back(std::move(*item));
            }
            encoded = MsgpackValue(std::move(tracks));
            break;
        }
        case KksMixerProperty::selected_track_parameters: {
            const auto *input =
                    std::get_if<std::array<KksMixerParameterInfo, 8>>(&value);
            if (!input)
                return std::nullopt;
            MsgpackValue::Array parameters;
            parameters.reserve(input->size());
            for (const auto &parameter : *input) {
                auto item = mixer_parameter_info_value(parameter, registry);
                if (!item)
                    return std::nullopt;
                parameters.push_back(std::move(*item));
            }
            encoded = MsgpackValue(std::move(parameters));
            break;
        }
        case KksMixerProperty::plugin_preset_name:
        case KksMixerProperty::plugin_track_name:
            if (const auto *input = std::get_if<std::string>(&value))
                encoded = *input;
            else
                return std::nullopt;
            break;
    }
    const std::array parameters{ kks_client_symbol(client, registry),
        kks_protocol_symbol(names[index], registry), std::move(encoded) };
    return KksRoutedFrame{ KksUsbChannel::primary,
        encode_rpc_notification(
                kks_method("client_instance_mixer_update_property", registry),
                parameters) };
}

KksRoutedFrame encode_kks_mixer_set_meters(const RpcMethodId &client,
        std::span<const std::array<float, 2>> meters,
        const OdrSymbolRegistry *registry) {
    MsgpackValue::Array encoded;
    encoded.reserve(meters.size());
    for (const auto &meter : meters) {
        encoded.emplace_back(MsgpackValue::Array{
                MsgpackValue(meter[0]), MsgpackValue(meter[1]) });
    }
    return kks_client_model_notification("client_instance_mixer_set_meters",
            client, MsgpackValue(std::move(encoded)), registry);
}

std::optional<KksRoutedFrame> encode_kks_browser_set_data(
        const RpcMethodId &client, const KksBrowserData &data,
        const OdrSymbolRegistry *registry) {
    auto sounds = kks_browser_sounds_value(data.sounds, registry);
    if (!sounds)
        return std::nullopt;
    MsgpackValue model(MsgpackValue::Map{
            { "filters", kks_browser_filters_value(data.filters, registry) },
            { "sounds", std::move(*sounds) } });
    return kks_client_model_notification(
            "client_instance_browser_set_data_model", client, std::move(model),
            registry);
}

KksRoutedFrame encode_kks_browser_set_settings(const RpcMethodId &client,
        const OdrBrowserSettings &settings, const OdrSymbolRegistry *registry) {
    MsgpackValue model(MsgpackValue::Map{
            { "prehear_enabled", settings.prehear_enabled },
            { "quickbrowse_enabled", settings.quickbrowse_enabled },
            { "speak_preset_enabled", settings.speak_preset_enabled },
            { "reset_filters_enabled", settings.reset_filters_enabled } });
    return kks_client_model_notification("client_instance_browser_set_settings",
            client, std::move(model), registry);
}

KksRoutedFrame encode_kks_browser_set_sound_selection(const RpcMethodId &client,
        std::optional<std::int32_t> selected_index,
        const OdrSymbolRegistry *registry) {
    MsgpackValue selection;
    if (selected_index)
        selection = std::int64_t(*selected_index);
    return kks_client_model_notification(
            "client_instance_browser_set_sound_selection", client,
            std::move(selection), registry);
}

KksRoutedFrame encode_kks_browser_set_filters(const RpcMethodId &client,
        const KksBrowserFilters &filters, const OdrSymbolRegistry *registry) {
    return kks_client_model_notification("client_instance_browser_set_filters",
            client, kks_browser_filters_value(filters, registry), registry);
}

std::optional<KksRoutedFrame> encode_kks_browser_set_filter(
        const RpcMethodId &client, OdrBrowserFilterProperty property,
        const KksBrowserFilter &filter, const OdrSymbolRegistry *registry) {
    const auto property_name = kks_browser_property_name(property);
    auto value = kks_browser_filter_value(property, filter, registry);
    if (!property_name || !value)
        return std::nullopt;
    const std::array parameters{ kks_client_symbol(client, registry),
        kks_protocol_symbol(*property_name, registry), std::move(*value) };
    constexpr std::string_view method_name =
            "client_instance_browser_set_filter";
    return KksRoutedFrame{ KksUsbChannel::primary,
        encode_rpc_notification(
                kks_method(method_name, registry), parameters) };
}

std::optional<KksRoutedFrame> encode_kks_browser_set_filter_selection(
        const RpcMethodId &client, OdrBrowserFilterProperty property,
        const KksBrowserFilterSelection &selection,
        const OdrSymbolRegistry *registry) {
    const auto property_name = kks_browser_property_name(property);
    auto value = kks_browser_selection_value(property, selection);
    if (!property_name || !value)
        return std::nullopt;
    const std::array parameters{ kks_client_symbol(client, registry),
        kks_protocol_symbol(*property_name, registry), std::move(*value) };
    constexpr std::string_view method_name =
            "client_instance_browser_set_filter_selection";
    return KksRoutedFrame{ KksUsbChannel::primary,
        encode_rpc_notification(
                kks_method(method_name, registry), parameters) };
}

std::optional<KksRoutedFrame> encode_kks_browser_set_sounds(
        const RpcMethodId &client, const KksBrowserSoundResults &sounds,
        const OdrSymbolRegistry *registry) {
    auto value = kks_browser_sounds_value(sounds, registry);
    if (!value)
        return std::nullopt;
    return kks_client_model_notification("client_instance_browser_set_sounds",
            client, std::move(*value), registry);
}

std::optional<KksRoutedFrame> encode_kks_browser_set_sound_item(
        const RpcMethodId &client, std::int32_t index,
        const KksBrowserSoundItem &item, const OdrSymbolRegistry *registry) {
    auto value = kks_browser_sound_item_value(item, registry);
    if (!value)
        return std::nullopt;
    const std::array parameters{ kks_client_symbol(client, registry),
        MsgpackValue(std::int64_t(index)), std::move(*value) };
    constexpr std::string_view method_name =
            "client_instance_browser_set_sound_item";
    return KksRoutedFrame{ KksUsbChannel::primary,
        encode_rpc_notification(
                kks_method(method_name, registry), parameters) };
}

std::optional<KksRoutedFrame> encode_kks_smartplay_set_data(
        const RpcMethodId &client, const KksSmartplayEngineState &state,
        const OdrSymbolRegistry *registry) {
    if (!kks_smartplay_state_valid(state))
        return std::nullopt;
    return kks_client_model_notification("client_instance_smartplay_set_data",
            client, kks_smartplay_engine_state_value(state, registry),
            registry);
}

KksRoutedFrame encode_kks_smartplay_set_device_owned_viewstate(
        const RpcMethodId &client, const KksSmartplayDeviceViewState &state,
        const OdrSymbolRegistry *registry) {
    constexpr std::array focus_names{ std::string_view("arpeggiator"),
        std::string_view("scale") };
    const auto index = static_cast<std::size_t>(state.focus);
    MsgpackValue focus =
            index < focus_names.size()
                    ? kks_protocol_symbol(focus_names[index], registry)
                    : MsgpackValue{};
    MsgpackValue model(MsgpackValue::Map{ { "focus", std::move(focus) },
            { "page", std::uint64_t(state.page) } });
    if (registry)
        compact_map_keys(model, *registry);
    return kks_client_model_notification(
            "client_instance_smartplay_set_device_owned_viewstate", client,
            std::move(model), registry);
}

KksRoutedFrame encode_kks_host_set_project_tree(const RpcMethodId &client,
        const KksProjectTree &tree, const OdrSymbolRegistry *registry) {
    return kks_client_model_notification("host_set_project_tree", client,
            kks_project_track_value(tree), registry);
}

KksRoutedFrame encode_kks_host_set_selected_track(const RpcMethodId &client,
        std::uint32_t selected_track, const OdrSymbolRegistry *registry) {
    return kks_client_model_notification("host_set_selected_track", client,
            MsgpackValue(std::uint64_t(selected_track)), registry);
}

std::optional<KksRoutedFrame> encode_kks_host_set_track_orientation(
        const RpcMethodId &client,
        std::optional<KksTrackOrientation> orientation,
        const OdrSymbolRegistry *registry) {
    MsgpackValue value;
    if (orientation) {
        constexpr std::array names{ std::string_view("horizontal"),
            std::string_view("vertical") };
        const auto index = static_cast<std::size_t>(*orientation);
        if (index >= names.size())
            return std::nullopt;
        value = kks_protocol_symbol(names[index], registry);
    }
    return kks_client_model_notification(
            "host_set_track_orientation", client, std::move(value), registry);
}

std::optional<KksRoutedFrame> encode_kks_host_set_track_plugin_chain(
        const RpcMethodId &client, const KksTrackPluginChainData &data,
        const OdrSymbolRegistry *registry) {
    auto value = kks_track_plugin_chain_value(data, registry);
    if (!value)
        return std::nullopt;
    const std::array parameters{ kks_client_symbol(client, registry),
        std::move(*value) };
    constexpr std::string_view method_name = "host_set_track_plugin_chain";
    return KksRoutedFrame{ KksUsbChannel::primary,
        encode_rpc_notification(
                kks_method(method_name, registry), parameters) };
}

KksRoutedFrame encode_kks_host_set_track_plugin_chain_selected_plugin(
        const RpcMethodId &client,
        const std::optional<std::array<std::byte, 16>> &selected_plugin_id,
        const OdrSymbolRegistry *registry) {
    MsgpackValue value;
    if (selected_plugin_id) {
        value = MsgpackValue::Binary(
                selected_plugin_id->begin(), selected_plugin_id->end());
    }
    return kks_client_model_notification(
            "host_set_track_plugin_chain_selected_plugin", client,
            std::move(value), registry);
}

std::optional<KksDeviceHandshakeReply> decode_kks_handshake_reply(
        const MsgpackValue &value) {
    const auto *fields = std::get_if<MsgpackValue::Map>(&value.value);
    if (!fields)
        return std::nullopt;
    const auto *protocol = map_value(*fields, "device_protocol_version");
    const auto *space = map_value(*fields, "total_asset_space");
    const auto *assets = map_value(*fields, "assets");
    const auto *os = map_value(*fields, "os_version");
    const auto *firmware = map_value(*fields, "firmware_version");
    const auto *renderer = map_value(*fields, "renderer_version");
    const auto *model = map_value(*fields, "hw_model");
    if (!protocol || !space || !assets || !os || !firmware || !renderer ||
            !model)
        return std::nullopt;
    const auto *protocol_text = std::get_if<std::string>(&protocol->value);
    const auto *total_space = std::get_if<std::uint64_t>(&space->value);
    const auto *asset_values = std::get_if<MsgpackValue::Array>(&assets->value);
    const auto *os_text = std::get_if<std::string>(&os->value);
    const auto *firmware_text = std::get_if<std::string>(&firmware->value);
    const auto *renderer_text = std::get_if<std::string>(&renderer->value);
    const auto *model_text = std::get_if<std::string>(&model->value);
    if (!protocol_text || !total_space || !asset_values || !os_text ||
            !firmware_text || !renderer_text || !model_text)
        return std::nullopt;
    auto parsed = parse_odr_protocol_version(*protocol_text);
    if (!parsed)
        return std::nullopt;
    std::vector<KksAssetRecord> records;
    records.reserve(asset_values->size());
    for (const auto &asset : *asset_values) {
        const auto *asset_fields = std::get_if<MsgpackValue::Map>(&asset.value);
        if (!asset_fields)
            return std::nullopt;
        const auto *size_value = map_value(*asset_fields, "size");
        const auto *checksum_value = map_value(*asset_fields, "checksum");
        if (!size_value || !checksum_value)
            return std::nullopt;
        const auto *size = std::get_if<std::uint64_t>(&size_value->value);
        if (!size)
            return std::nullopt;
        KksAssetRecord record;
        record.device_asset_id = records.size();
        record.size = *size;
        if (const auto *bytes = std::get_if<MsgpackValue::Binary>(
                    &checksum_value->value)) {
            if (bytes->size() != record.checksum.size())
                return std::nullopt;
            std::ranges::copy(*bytes, record.checksum.begin());
        } else if (const auto *text =
                           std::get_if<std::string>(&checksum_value->value)) {
            if (text->size() != record.checksum.size())
                return std::nullopt;
            std::ranges::transform(
                    *text, record.checksum.begin(), [](char byte) {
                        return std::byte(static_cast<unsigned char>(byte));
                    });
        } else {
            return std::nullopt;
        }
        records.push_back(record);
    }
    return KksDeviceHandshakeReply{ *parsed, *total_space, std::move(records),
        *os_text, *firmware_text, *renderer_text, *model_text };
}

std::optional<std::uint64_t> find_kks_asset_id(
        const KksDeviceHandshakeReply &reply,
        const std::array<std::byte, 32> &checksum) noexcept {
    const auto found = std::ranges::find(
            reply.assets, checksum, &KksAssetRecord::checksum);
    if (found == reply.assets.end())
        return std::nullopt;
    return found->device_asset_id;
}

std::uint64_t next_kks_asset_id(const KksDeviceHandshakeReply &reply) noexcept {
    if (reply.assets.empty())
        return 0;
    return std::ranges::max(reply.assets, {}, &KksAssetRecord::device_asset_id)
                   .device_asset_id +
           1;
}

namespace {

constexpr std::array kks_inbound_methods{
    std::string_view("device_property_changed"),
    std::string_view("midi_plugin_action"),
    std::string_view("plugin_action"),
    std::string_view("set_active_instance_client"),
    std::string_view("set_focus"),
    std::string_view("device_properties"),
    std::string_view("device_setting"),
    std::string_view("crashdump"),
    std::string_view("system_command_reply_with_log"),
    std::string_view("agent_heartbeat_request_handshake"),
    std::string_view("client_instance_set_device_owned_instance_client_state"),
    std::string_view("data_tracking_event"),
    std::string_view("data_tracking_active"),
    std::string_view("accessibility_event"),
    std::string_view("midi_template_transfer_event"),
    std::string_view("handshake_reply"),
};

constexpr std::array<std::uint8_t, kks_inbound_methods.size()>
        kks_inbound_arities{
            2,
            2,
            2,
            1,
            1,
            1,
            1,
            1,
            2,
            0,
            2,
            1,
            1,
            1,
            1,
            1,
        };

std::optional<KksDeviceMethod> kks_method(std::string_view name) {
    const auto found = std::ranges::find(kks_inbound_methods, name);
    if (found == kks_inbound_methods.end())
        return std::nullopt;
    return static_cast<KksDeviceMethod>(found - kks_inbound_methods.begin());
}

} // namespace

std::string_view kks_device_method_name(KksDeviceMethod method) noexcept {
    const auto index = static_cast<std::size_t>(method);
    return index < kks_inbound_methods.size() ? kks_inbound_methods[index]
                                              : std::string_view{};
}

std::span<const std::string_view> kks_device_method_names() noexcept {
    return kks_inbound_methods;
}

std::optional<KksDeviceMessage> decode_kks_device_message(
        std::span<const std::byte> frame, const OdrSymbolRegistry *registry) {
    RpcMethodId wire_method;
    MsgpackValue::Array parameters;
    std::optional<std::uint32_t> request_id;
    if (auto notification = decode_rpc_notification(frame)) {
        wire_method = std::move(notification->method);
        parameters = std::move(notification->parameters);
    } else if (auto request = decode_rpc_request(frame)) {
        wire_method = std::move(request->method);
        parameters = std::move(request->parameters);
        request_id = request->id;
    } else {
        return std::nullopt;
    }

    std::string_view name;
    if (const auto *literal = std::get_if<std::string>(&wire_method)) {
        name = *literal;
    } else {
        if (!registry)
            return std::nullopt;
        const auto resolved =
                registry->symbol(std::get<std::uint16_t>(wire_method));
        if (!resolved)
            return std::nullopt;
        name = *resolved;
    }
    const auto method = kks_method(name);
    if (!method)
        return std::nullopt;
    const auto index = static_cast<std::size_t>(*method);
    if (parameters.size() != kks_inbound_arities[index])
        return std::nullopt;
    return KksDeviceMessage{ *method, std::move(parameters), request_id };
}

std::optional<KksDecodedDevicePayload> decode_kks_device_payload(
        const KksDeviceMessage &message, const OdrSymbolRegistry *registry) {
    const auto symbol =
            [](const MsgpackValue &value) -> std::optional<RpcMethodId> {
        if (const auto *text = std::get_if<std::string>(&value.value))
            return RpcMethodId(*text);
        if (const auto *index = std::get_if<std::uint64_t>(&value.value);
                index && *index <= std::numeric_limits<std::uint16_t>::max())
            return RpcMethodId(static_cast<std::uint16_t>(*index));
        return std::nullopt;
    };
    const auto symbol_name = [registry](const MsgpackValue &value)
            -> std::optional<std::string_view> {
        if (const auto *text = std::get_if<std::string>(&value.value))
            return *text;
        if (!registry)
            return std::nullopt;
        if (const auto *index = std::get_if<std::uint64_t>(&value.value);
                index && *index <= std::numeric_limits<std::uint16_t>::max())
            return registry->symbol(static_cast<std::uint16_t>(*index));
        return std::nullopt;
    };
    const auto registry_map_value =
            [&symbol_name](const MsgpackValue::Map &fields,
                    std::string_view wanted) -> const MsgpackValue * {
        for (const auto &[key, value] : fields) {
            const auto name = symbol_name(key);
            if (name && *name == wanted)
                return &value;
        }
        return nullptr;
    };
    std::function<MsgpackValue(const MsgpackValue &)> expand_map_keys;
    expand_map_keys = [registry, &expand_map_keys](
                              const MsgpackValue &value) -> MsgpackValue {
        if (const auto *array =
                        std::get_if<MsgpackValue::Array>(&value.value)) {
            MsgpackValue::Array expanded;
            expanded.reserve(array->size());
            for (const auto &item : *array)
                expanded.push_back(expand_map_keys(item));
            return MsgpackValue(std::move(expanded));
        }
        if (const auto *map = std::get_if<MsgpackValue::Map>(&value.value)) {
            MsgpackValue::Map expanded;
            expanded.reserve(map->size());
            for (const auto &[raw_key, raw_value] : *map) {
                MsgpackValue key = raw_key;
                if (registry) {
                    if (const auto *index =
                                    std::get_if<std::uint64_t>(&raw_key.value);
                            index &&
                            *index <=
                                    std::numeric_limits<std::uint16_t>::max()) {
                        if (const auto name = registry->symbol(
                                    static_cast<std::uint16_t>(*index)))
                            key = MsgpackValue(*name);
                    }
                }
                expanded.emplace_back(
                        std::move(key), expand_map_keys(raw_value));
            }
            return MsgpackValue(std::move(expanded));
        }
        return value;
    };
    const auto expand_action_symbol_values = [registry](MsgpackValue &action) {
        if (!registry)
            return;
        auto *variant = std::get_if<MsgpackValue::Map>(&action.value);
        if (!variant || variant->size() != 1)
            return;
        const auto *kind = std::get_if<std::string>(&(*variant)[0].first.value);
        if (!kind)
            return;
        auto resolve = [registry](MsgpackValue &value) {
            const auto *index = std::get_if<std::uint64_t>(&value.value);
            if (!index || *index > std::numeric_limits<std::uint16_t>::max())
                return;
            if (const auto name = registry->symbol(
                        static_cast<std::uint16_t>(*index)))
                value = MsgpackValue(*name);
        };
        auto *tuple =
                std::get_if<MsgpackValue::Array>(&(*variant)[0].second.value);
        if (*kind == "daw_set_host_pad_mode" || *kind == "daw_navigate_track" ||
                *kind == "daw_navigate_track_depth" ||
                *kind == "smartplay_arp_engine_change" ||
                *kind == "smartplay_arp_engine_trigger" ||
                *kind == "smartplay_scale_engine_change") {
            if (tuple && !tuple->empty())
                resolve((*tuple)[0]);
            return;
        }
        if (*kind != "smartplay_device_owned_viewstate_changed" || !tuple ||
                tuple->size() != 1)
            return;
        auto *fields = std::get_if<MsgpackValue::Map>(&(*tuple)[0].value);
        if (!fields)
            return;
        for (auto &[field, value] : *fields) {
            const auto *name = std::get_if<std::string>(&field.value);
            if (name && *name == "focus")
                resolve(value);
        }
    };
    const auto unsigned_byte =
            [](const MsgpackValue &value) -> std::optional<std::uint8_t> {
        const auto *integer = std::get_if<std::uint64_t>(&value.value);
        if (!integer || *integer > std::numeric_limits<std::uint8_t>::max())
            return std::nullopt;
        return static_cast<std::uint8_t>(*integer);
    };
    const auto unsigned_word =
            [](const MsgpackValue &value) -> std::optional<std::uint32_t> {
        const auto *integer = std::get_if<std::uint64_t>(&value.value);
        if (!integer || *integer > std::numeric_limits<std::uint32_t>::max())
            return std::nullopt;
        return static_cast<std::uint32_t>(*integer);
    };
    const auto float_number =
            [](const MsgpackValue &value) -> std::optional<float> {
        if (const auto *number = std::get_if<float>(&value.value))
            return *number;
        if (const auto *number = std::get_if<double>(&value.value))
            return static_cast<float>(*number);
        if (const auto *number = std::get_if<std::int64_t>(&value.value))
            return static_cast<float>(*number);
        if (const auto *number = std::get_if<std::uint64_t>(&value.value))
            return static_cast<float>(*number);
        return std::nullopt;
    };
    const auto property_value = [](const MsgpackValue &value)
            -> std::optional<KksDevicePropertyValue> {
        if (std::holds_alternative<std::monostate>(value.value))
            return KksDevicePropertyValue(std::monostate{});
        if (const auto *boolean = std::get_if<bool>(&value.value))
            return KksDevicePropertyValue(*boolean);
        if (const auto *number = std::get_if<float>(&value.value))
            return KksDevicePropertyValue(*number);
        if (const auto *integer = std::get_if<std::int64_t>(&value.value);
                integer &&
                *integer >= std::numeric_limits<std::int32_t>::min() &&
                *integer <= std::numeric_limits<std::int32_t>::max())
            return KksDevicePropertyValue(static_cast<std::int32_t>(*integer));
        if (const auto *integer = std::get_if<std::uint64_t>(&value.value);
                integer &&
                *integer <=
                        std::uint64_t(std::numeric_limits<std::int32_t>::max()))
            return KksDevicePropertyValue(static_cast<std::int32_t>(*integer));
        if (const auto *text = std::get_if<std::string>(&value.value))
            return KksDevicePropertyValue(*text);
        return std::nullopt;
    };
    std::function<std::optional<MsgpackValue>(const MsgpackValue &)>
            tracking_value;
    tracking_value =
            [&tracking_value](
                    const MsgpackValue &value) -> std::optional<MsgpackValue> {
        if (std::holds_alternative<std::monostate>(value.value) ||
                std::holds_alternative<std::int64_t>(value.value) ||
                std::holds_alternative<std::uint64_t>(value.value) ||
                std::holds_alternative<double>(value.value) ||
                std::holds_alternative<std::string>(value.value))
            return value;
        if (const auto *number = std::get_if<float>(&value.value))
            return MsgpackValue(static_cast<double>(*number));
        if (const auto *values =
                        std::get_if<MsgpackValue::Array>(&value.value)) {
            MsgpackValue::Array result;
            result.reserve(values->size());
            for (const auto &item : *values) {
                auto decoded = tracking_value(item);
                if (!decoded)
                    return std::nullopt;
                result.push_back(std::move(*decoded));
            }
            return MsgpackValue(std::move(result));
        }
        if (const auto *fields = std::get_if<MsgpackValue::Map>(&value.value)) {
            MsgpackValue::Map result;
            result.reserve(fields->size());
            for (const auto &[key, raw] : *fields) {
                const auto *text = std::get_if<std::string>(&key.value);
                auto decoded = tracking_value(raw);
                if (!text || !decoded)
                    return std::nullopt;
                result.emplace_back(*text, std::move(*decoded));
            }
            return MsgpackValue(std::move(result));
        }
        return std::nullopt;
    };

    const auto &p = message.parameters;
    switch (message.method) {
        case KksDeviceMethod::midi_plugin_action:
            if (auto client = symbol(p[0]))
                return KksMidiPluginAction{ std::move(*client),
                    expand_map_keys(p[1]) };
            return std::nullopt;
        case KksDeviceMethod::plugin_action:
            if (auto client = symbol(p[0])) {
                auto action = expand_map_keys(p[1]);
                expand_action_symbol_values(action);
                return KksPluginAction{ std::move(*client), std::move(action) };
            }
            return std::nullopt;
        case KksDeviceMethod::system_command_reply_with_log: {
            auto command = symbol(p[0]);
            const auto *log = std::get_if<std::string>(&p[1].value);
            if (!command || !log)
                return std::nullopt;
            return KksSystemCommandReply{ std::move(*command), *log };
        }
        case KksDeviceMethod::agent_heartbeat_request_handshake:
            return KksHeartbeatHandshakeRequest{};
        case KksDeviceMethod::data_tracking_event: {
            const auto *fields = std::get_if<MsgpackValue::Map>(&p[0].value);
            if (!fields)
                return std::nullopt;
            std::optional<std::string> event_name;
            std::optional<MsgpackValue> event_value;
            std::optional<std::vector<std::pair<std::string, MsgpackValue>>>
                    extras;
            for (const auto &[key, raw] : *fields) {
                const auto name = symbol_name(key);
                if (!name)
                    continue;
                if (*name == "name" && !event_name) {
                    const auto *text = std::get_if<std::string>(&raw.value);
                    if (!text)
                        return std::nullopt;
                    event_name = *text;
                } else if (*name == "value" && !event_value) {
                    event_value = tracking_value(raw);
                    if (!event_value)
                        return std::nullopt;
                } else if (*name == "extra_values" && !extras) {
                    const auto *extra_fields =
                            std::get_if<MsgpackValue::Map>(&raw.value);
                    if (!extra_fields)
                        return std::nullopt;
                    extras.emplace();
                    extras->reserve(extra_fields->size());
                    for (const auto &[extra_key, extra_raw] : *extra_fields) {
                        const auto *text =
                                std::get_if<std::string>(&extra_key.value);
                        auto decoded = tracking_value(extra_raw);
                        if (!text || !decoded)
                            return std::nullopt;
                        extras->emplace_back(*text, std::move(*decoded));
                    }
                }
            }
            if (!event_name || !event_value || !extras)
                return std::nullopt;
            return KksDataTrackingEvent{ std::move(*event_name),
                std::move(*event_value), std::move(*extras) };
        }
        case KksDeviceMethod::crashdump: {
            const auto *fields = std::get_if<MsgpackValue::Map>(&p[0].value);
            if (!fields)
                return std::nullopt;
            const auto *metadata_value =
                    registry_map_value(*fields, "metadata");
            const auto *filename_value =
                    registry_map_value(*fields, "filename");
            const auto *minidump_value =
                    registry_map_value(*fields, "minidump");
            if (!metadata_value || !filename_value || !minidump_value)
                return std::nullopt;

            const auto *metadata_fields =
                    std::get_if<MsgpackValue::Map>(&metadata_value->value);
            const auto *filename =
                    std::get_if<std::string>(&filename_value->value);
            if (!metadata_fields || !filename)
                return std::nullopt;

            std::map<std::string, std::string> metadata;
            for (const auto &[key, value] : *metadata_fields) {
                const auto *key_text = std::get_if<std::string>(&key.value);
                const auto *value_text = std::get_if<std::string>(&value.value);
                if (!key_text || !value_text)
                    return std::nullopt;
                // a duplicate metadata key replaces its earlier value
                metadata[*key_text] = *value_text;
            }

            std::vector<std::byte> minidump;
            if (const auto *bytes = std::get_if<MsgpackValue::Binary>(
                        &minidump_value->value)) {
                minidump = *bytes;
            } else if (const auto *bytes = std::get_if<std::string>(
                               &minidump_value->value)) {
                minidump.reserve(bytes->size());
                std::ranges::transform(
                        *bytes, std::back_inserter(minidump), [](char byte) {
                            return std::byte(static_cast<unsigned char>(byte));
                        });
            } else {
                return std::nullopt;
            }
            return KksCrashdump{ std::move(metadata), *filename,
                std::move(minidump) };
        }
        case KksDeviceMethod::midi_template_transfer_event:
        case KksDeviceMethod::device_setting: {
            constexpr std::array port_names{ std::string_view("usb_priority"),
                std::string_view("dual") };
            constexpr std::array velocity_names{ std::string_view("soft3"),
                std::string_view("soft2"), std::string_view("soft1"),
                std::string_view("linear"), std::string_view("hard1"),
                std::string_view("hard2"), std::string_view("hard3"),
                std::string_view("fixed") };
            constexpr std::array aftertouch_names{ std::string_view("off"),
                std::string_view("channel_pressure"),
                std::string_view("poly_key_pressure") };
            constexpr std::array transfer_command_names{
                std::string_view("import_request"),
                std::string_view("export_request"),
                std::string_view("import_response"),
                std::string_view("export_response"), std::string_view("abort")
            };

            const bool is_transfer =
                    message.method ==
                    KksDeviceMethod::midi_template_transfer_event;
            const auto *fields = std::get_if<MsgpackValue::Map>(&p[0].value);
            if (!fields)
                return std::nullopt;
            const MsgpackValue::Array *template_values = nullptr;
            std::optional<KksMidiTemplateTransferCommand> transfer_command;
            KksDeviceSettings decoded_settings;
            if (is_transfer) {
                const auto *command = registry_map_value(*fields, "command");
                const auto *data = registry_map_value(*fields, "data");
                if (!command || !data)
                    return std::nullopt;
                const auto command_name = symbol_name(*command);
                template_values =
                        std::get_if<MsgpackValue::Array>(&data->value);
                if (!command_name || !template_values)
                    return std::nullopt;
                const auto command_it = std::ranges::find(
                        transfer_command_names, *command_name);
                if (command_it == transfer_command_names.end())
                    return std::nullopt;
                transfer_command = static_cast<KksMidiTemplateTransferCommand>(
                        command_it - transfer_command_names.begin());
            } else {
                const auto *led = registry_map_value(*fields, "led_brightness");
                const auto *display =
                        registry_map_value(*fields, "display_brightness");
                const auto *lightguide =
                        registry_map_value(*fields, "lightguide_enabled");
                const auto *color =
                        registry_map_value(*fields, "global_led_color");
                const auto *midi2 =
                        registry_map_value(*fields, "usb_midi_2_enabled");
                const auto *tracking = registry_map_value(
                        *fields, "usage_data_tracking_enabled");
                const auto *port =
                        registry_map_value(*fields, "midi_output_port");
                const auto *velocity =
                        registry_map_value(*fields, "velocity_curve");
                const auto *aftertouch =
                        registry_map_value(*fields, "aftertouch_mode");
                const auto *aftertouch_config =
                        registry_map_value(*fields, "aftertouch_configuration");
                const auto *active_template =
                        registry_map_value(*fields, "active_midi_template");
                const auto *templates =
                        registry_map_value(*fields, "midi_templates");
                const auto *speak =
                        registry_map_value(*fields, "speak_browser_preset");
                const auto *experimental = registry_map_value(
                        *fields, "experimental_features_enabled");
                if (!led || !display || !lightguide || !color || !midi2 ||
                        !tracking || !port || !velocity || !aftertouch ||
                        !aftertouch_config || !active_template || !templates ||
                        !speak || !experimental)
                    return std::nullopt;

                const auto led_value = float_number(*led);
                const auto display_value = float_number(*display);
                const auto *lightguide_value =
                        std::get_if<bool>(&lightguide->value);
                auto color_value = symbol(*color);
                const auto *midi2_value = std::get_if<bool>(&midi2->value);
                const auto *tracking_value_setting =
                        std::get_if<bool>(&tracking->value);
                const auto port_name = symbol_name(*port);
                const auto velocity_name = symbol_name(*velocity);
                const auto aftertouch_name = symbol_name(*aftertouch);
                const auto *active_template_value =
                        std::get_if<std::string>(&active_template->value);
                template_values =
                        std::get_if<MsgpackValue::Array>(&templates->value);
                const auto *speak_value = std::get_if<bool>(&speak->value);
                const auto *experimental_value =
                        std::get_if<bool>(&experimental->value);
                if (!led_value || !display_value || !lightguide_value ||
                        !color_value || !midi2_value ||
                        !tracking_value_setting || !port_name ||
                        !velocity_name || !aftertouch_name ||
                        !active_template_value || !template_values ||
                        !speak_value || !experimental_value)
                    return std::nullopt;

                const auto port_it = std::ranges::find(port_names, *port_name);
                const auto velocity_it =
                        std::ranges::find(velocity_names, *velocity_name);
                const auto aftertouch_it =
                        std::ranges::find(aftertouch_names, *aftertouch_name);
                if (port_it == port_names.end() ||
                        velocity_it == velocity_names.end() ||
                        aftertouch_it == aftertouch_names.end())
                    return std::nullopt;

                const auto *config = std::get_if<MsgpackValue::Map>(
                        &aftertouch_config->value);
                if (!config)
                    return std::nullopt;
                const auto *curve = registry_map_value(*config, "curve");
                const auto *sensitivity =
                        registry_map_value(*config, "sensitivity");
                const auto *threshold =
                        registry_map_value(*config, "threshold");
                const auto *delay = registry_map_value(*config, "delay");
                if (!curve || !sensitivity || !threshold || !delay)
                    return std::nullopt;
                const auto curve_value = float_number(*curve);
                const auto sensitivity_value = float_number(*sensitivity);
                const auto threshold_value = float_number(*threshold);
                std::optional<std::int64_t> delay_nanoseconds;
                if (const auto *value =
                                std::get_if<std::int64_t>(&delay->value)) {
                    delay_nanoseconds = *value;
                } else if (const auto *value =
                                   std::get_if<std::uint64_t>(&delay->value);
                        value && *value <= std::uint64_t(std::numeric_limits<
                                                   std::int64_t>::max())) {
                    delay_nanoseconds = static_cast<std::int64_t>(*value);
                }
                if (!curve_value || !sensitivity_value || !threshold_value ||
                        !delay_nanoseconds)
                    return std::nullopt;

                decoded_settings.led_brightness = *led_value;
                decoded_settings.display_brightness = *display_value;
                decoded_settings.lightguide_enabled = *lightguide_value;
                decoded_settings.global_led_color = std::move(*color_value);
                decoded_settings.usb_midi_2_enabled = *midi2_value;
                decoded_settings.usage_data_tracking_enabled =
                        *tracking_value_setting;
                decoded_settings.midi_output_port =
                        static_cast<KksGlobalMidiPortPreference>(
                                port_it - port_names.begin());
                decoded_settings.velocity_curve = static_cast<KksVelocityCurve>(
                        velocity_it - velocity_names.begin());
                decoded_settings.aftertouch_mode =
                        static_cast<KksAftertouchMode>(
                                aftertouch_it - aftertouch_names.begin());
                decoded_settings.aftertouch_configuration =
                        KksAftertouchParameters{ *curve_value,
                            *sensitivity_value, *threshold_value,
                            *delay_nanoseconds / 1'000'000 };
                decoded_settings.active_midi_template = *active_template_value;
                decoded_settings.speak_browser_preset = *speak_value;
                decoded_settings.experimental_features_enabled =
                        *experimental_value;
            }

            const auto signed_byte = [](const MsgpackValue &value)
                    -> std::optional<std::int8_t> {
                if (const auto *integer =
                                std::get_if<std::int64_t>(&value.value);
                        integer &&
                        *integer >= std::numeric_limits<std::int8_t>::min() &&
                        *integer <= std::numeric_limits<std::int8_t>::max())
                    return static_cast<std::int8_t>(*integer);
                if (const auto *integer =
                                std::get_if<std::uint64_t>(&value.value);
                        integer &&
                        *integer <= std::uint64_t(std::numeric_limits<
                                            std::int8_t>::max()))
                    return static_cast<std::int8_t>(*integer);
                return std::nullopt;
            };
            const auto signed_short = [](const MsgpackValue &value)
                    -> std::optional<std::int16_t> {
                if (const auto *integer =
                                std::get_if<std::int64_t>(&value.value);
                        integer &&
                        *integer >= std::numeric_limits<std::int16_t>::min() &&
                        *integer <= std::numeric_limits<std::int16_t>::max())
                    return static_cast<std::int16_t>(*integer);
                if (const auto *integer =
                                std::get_if<std::uint64_t>(&value.value);
                        integer &&
                        *integer <= std::uint64_t(std::numeric_limits<
                                            std::int16_t>::max()))
                    return static_cast<std::int16_t>(*integer);
                return std::nullopt;
            };
            const auto enum_index =
                    [&symbol_name](const MsgpackValue &value,
                            std::span<const std::string_view> names)
                    -> std::optional<std::size_t> {
                const auto name = symbol_name(value);
                if (!name)
                    return std::nullopt;
                const auto found = std::ranges::find(names, *name);
                if (found == names.end())
                    return std::nullopt;
                return static_cast<std::size_t>(found - names.begin());
            };
            const auto tagged = [](const MsgpackValue &value)
                    -> std::optional<
                            std::pair<std::string_view, const MsgpackValue *>> {
                const auto *fields =
                        std::get_if<MsgpackValue::Map>(&value.value);
                if (!fields || fields->size() != 1)
                    return std::nullopt;
                const auto *tag =
                        std::get_if<std::string>(&fields->front().first.value);
                if (!tag)
                    return std::nullopt;
                return std::pair<std::string_view, const MsgpackValue *>(
                        *tag, &fields->front().second);
            };
            const auto fields_of =
                    [](const MsgpackValue &value) -> const MsgpackValue::Map * {
                return std::get_if<MsgpackValue::Map>(&value.value);
            };

            const auto decode_cc = [&](const MsgpackValue &value)
                    -> std::optional<KksMidiCcAssignment> {
                const auto *fields = fields_of(value);
                if (!fields)
                    return std::nullopt;
                const auto *channel =
                        registry_map_value(*fields, "midi_channel");
                const auto *number =
                        registry_map_value(*fields, "midi_cc_number");
                if (!channel || !number)
                    return std::nullopt;
                const auto decoded_channel = unsigned_byte(*channel);
                const auto decoded_number = unsigned_byte(*number);
                if (!decoded_channel || !decoded_number)
                    return std::nullopt;
                return KksMidiCcAssignment{ *decoded_channel, *decoded_number };
            };
            const auto decode_channel = [&](const MsgpackValue &value)
                    -> std::optional<std::uint8_t> {
                const auto *fields = fields_of(value);
                if (!fields)
                    return std::nullopt;
                const auto *channel =
                        registry_map_value(*fields, "midi_channel");
                return channel ? unsigned_byte(*channel) : std::nullopt;
            };
            const auto decode_note = [&](const MsgpackValue &value)
                    -> std::optional<KksMidiNoteOnAssignment> {
                const auto *fields = fields_of(value);
                if (!fields)
                    return std::nullopt;
                const auto *channel =
                        registry_map_value(*fields, "midi_channel");
                const auto *pitch = registry_map_value(*fields, "pitch");
                const auto *velocity_value =
                        registry_map_value(*fields, "velocity");
                if (!channel || !pitch || !velocity_value)
                    return std::nullopt;
                const auto decoded_channel = unsigned_byte(*channel);
                const auto decoded_pitch = unsigned_byte(*pitch);
                const auto decoded_velocity = unsigned_byte(*velocity_value);
                if (!decoded_channel || !decoded_pitch || !decoded_velocity)
                    return std::nullopt;
                return KksMidiNoteOnAssignment{ *decoded_channel,
                    *decoded_pitch, *decoded_velocity };
            };
            const auto decode_encoder_assignment =
                    [&](const MsgpackValue &value)
                    -> std::optional<KksMidiEncoderAssignment> {
                const auto entry = tagged(value);
                if (!entry)
                    return std::nullopt;
                if (entry->first == "cc") {
                    auto decoded = decode_cc(*entry->second);
                    if (decoded)
                        return KksMidiEncoderAssignment(std::move(*decoded));
                } else if (entry->first == "program_change") {
                    auto decoded = decode_channel(*entry->second);
                    if (decoded)
                        return KksMidiEncoderAssignment(
                                KksMidiProgramChangeAssignment{ *decoded });
                }
                return std::nullopt;
            };
            const auto decode_button_assignment = [&](const MsgpackValue &value)
                    -> std::optional<KksMidiButtonAssignment> {
                const auto entry = tagged(value);
                if (!entry)
                    return std::nullopt;
                if (entry->first == "cc") {
                    auto decoded = decode_cc(*entry->second);
                    if (decoded)
                        return KksMidiButtonAssignment(std::move(*decoded));
                } else if (entry->first == "program_change") {
                    auto decoded = decode_channel(*entry->second);
                    if (decoded)
                        return KksMidiButtonAssignment(
                                KksMidiProgramChangeAssignment{ *decoded });
                } else if (entry->first == "note_on") {
                    auto decoded = decode_note(*entry->second);
                    if (decoded)
                        return KksMidiButtonAssignment(std::move(*decoded));
                }
                return std::nullopt;
            };
            const auto decode_continuous_assignment =
                    [&](const MsgpackValue &value)
                    -> std::optional<KksMidiContinuousAssignment> {
                const auto entry = tagged(value);
                if (!entry)
                    return std::nullopt;
                if (entry->first == "cc") {
                    auto decoded = decode_cc(*entry->second);
                    if (decoded)
                        return KksMidiContinuousAssignment(std::move(*decoded));
                } else if (entry->first == "pitch_bend") {
                    auto decoded = decode_channel(*entry->second);
                    if (decoded)
                        return KksMidiContinuousAssignment(
                                KksMidiPitchBendAssignment{ *decoded });
                } else if (entry->first == "program_change") {
                    auto decoded = decode_channel(*entry->second);
                    if (decoded)
                        return KksMidiContinuousAssignment(
                                KksMidiProgramChangeAssignment{ *decoded });
                }
                return std::nullopt;
            };

            const auto two_bytes = [&](const MsgpackValue &value,
                                           std::string_view first,
                                           std::string_view second)
                    -> std::optional<std::pair<std::uint8_t, std::uint8_t>> {
                const auto *fields = fields_of(value);
                if (!fields)
                    return std::nullopt;
                const auto *first_value = registry_map_value(*fields, first);
                const auto *second_value = registry_map_value(*fields, second);
                if (!first_value || !second_value)
                    return std::nullopt;
                const auto a = unsigned_byte(*first_value);
                const auto b = unsigned_byte(*second_value);
                if (!a || !b)
                    return std::nullopt;
                return std::pair(*a, *b);
            };
            const auto decode_encoder_behaviour = [&](const MsgpackValue &value)
                    -> std::optional<KksMidiEncoderBehaviour> {
                const auto entry = tagged(value);
                if (!entry)
                    return std::nullopt;
                if (entry->first == "absolute") {
                    const auto range = two_bytes(*entry->second, "min", "max");
                    if (range)
                        return KksMidiEncoderBehaviour(KksMidiEncoderAbsolute{
                                range->first, range->second });
                } else if (entry->first == "relative") {
                    if (fields_of(*entry->second))
                        return KksMidiEncoderBehaviour(
                                KksMidiEncoderRelative{});
                } else if (entry->first == "relative_offset") {
                    const auto *fields = fields_of(*entry->second);
                    const auto *offset =
                            fields ? registry_map_value(*fields, "offset")
                                   : nullptr;
                    const auto decoded =
                            offset ? signed_byte(*offset) : std::nullopt;
                    if (decoded)
                        return KksMidiEncoderBehaviour(
                                KksMidiEncoderRelativeOffset{ *decoded });
                }
                return std::nullopt;
            };
            const auto decode_button_behaviour = [&](const MsgpackValue &value)
                    -> std::optional<KksMidiButtonBehaviour> {
                const auto entry = tagged(value);
                if (!entry)
                    return std::nullopt;
                if (entry->first == "toggle" || entry->first == "gate") {
                    const auto states = two_bytes(*entry->second, "on", "off");
                    if (!states)
                        return std::nullopt;
                    if (entry->first == "toggle")
                        return KksMidiButtonBehaviour(KksMidiButtonToggle{
                                states->first, states->second });
                    return KksMidiButtonBehaviour(
                            KksMidiButtonGate{ states->first, states->second });
                }
                if (entry->first == "trigger") {
                    if (fields_of(*entry->second))
                        return KksMidiButtonBehaviour(KksMidiButtonTrigger{});
                    return std::nullopt;
                }
                if (entry->first != "increment")
                    return std::nullopt;
                const auto *fields = fields_of(*entry->second);
                if (!fields)
                    return std::nullopt;
                const auto *min = registry_map_value(*fields, "min");
                const auto *max = registry_map_value(*fields, "max");
                const auto *step = registry_map_value(*fields, "step");
                const auto *wrap = registry_map_value(*fields, "wrap");
                if (!min || !max || !step || !wrap)
                    return std::nullopt;
                const auto decoded_min = unsigned_byte(*min);
                const auto decoded_max = unsigned_byte(*max);
                const auto decoded_step = signed_byte(*step);
                const auto *decoded_wrap = std::get_if<bool>(&wrap->value);
                if (!decoded_min || !decoded_max || !decoded_step ||
                        !decoded_wrap)
                    return std::nullopt;
                return KksMidiButtonBehaviour(
                        KksMidiButtonIncrement{ *decoded_min, *decoded_max,
                                *decoded_step, *decoded_wrap });
            };

            const auto decode_encoder_mapping = [&](const MsgpackValue &value)
                    -> std::optional<KksMidiEncoderMapping> {
                const auto *fields = fields_of(value);
                if (!fields)
                    return std::nullopt;
                const auto *assignment =
                        registry_map_value(*fields, "assignment");
                const auto *behaviour =
                        registry_map_value(*fields, "behaviour");
                if (!assignment || !behaviour)
                    return std::nullopt;
                auto decoded_assignment =
                        decode_encoder_assignment(*assignment);
                auto decoded_behaviour = decode_encoder_behaviour(*behaviour);
                if (!decoded_assignment || !decoded_behaviour)
                    return std::nullopt;
                return KksMidiEncoderMapping{ std::move(*decoded_assignment),
                    std::move(*decoded_behaviour) };
            };
            const auto decode_button_mapping = [&](const MsgpackValue &value)
                    -> std::optional<KksMidiButtonMapping> {
                const auto *fields = fields_of(value);
                if (!fields)
                    return std::nullopt;
                const auto *assignment =
                        registry_map_value(*fields, "assignment");
                const auto *behaviour =
                        registry_map_value(*fields, "behaviour");
                if (!assignment || !behaviour)
                    return std::nullopt;
                auto decoded_assignment = decode_button_assignment(*assignment);
                auto decoded_behaviour = decode_button_behaviour(*behaviour);
                if (!decoded_assignment || !decoded_behaviour)
                    return std::nullopt;
                return KksMidiButtonMapping{ std::move(*decoded_assignment),
                    std::move(*decoded_behaviour) };
            };
            const auto decode_continuous_mapping =
                    [&](const MsgpackValue &value)
                    -> std::optional<KksMidiContinuousMapping> {
                const auto *fields = fields_of(value);
                if (!fields)
                    return std::nullopt;
                const auto *assignment =
                        registry_map_value(*fields, "assignment");
                const auto *behaviour =
                        registry_map_value(*fields, "behaviour");
                if (!assignment || !behaviour)
                    return std::nullopt;
                auto decoded_assignment =
                        decode_continuous_assignment(*assignment);
                const auto range_variant = tagged(*behaviour);
                if (!decoded_assignment || !range_variant ||
                        range_variant->first != "range")
                    return std::nullopt;
                const auto *range_fields = fields_of(*range_variant->second);
                const auto *min =
                        range_fields ? registry_map_value(*range_fields, "min")
                                     : nullptr;
                const auto *max =
                        range_fields ? registry_map_value(*range_fields, "max")
                                     : nullptr;
                const auto decoded_min =
                        min ? signed_short(*min) : std::nullopt;
                const auto decoded_max =
                        max ? signed_short(*max) : std::nullopt;
                if (!decoded_min || !decoded_max)
                    return std::nullopt;
                return KksMidiContinuousMapping{ std::move(*decoded_assignment),
                    KksMidiContinuousRange{ *decoded_min, *decoded_max } };
            };

            constexpr std::array display_mode_names{ std::string_view(
                                                             "unipolar_int"),
                std::string_view("bipolar_int"),
                std::string_view("unipolar_percent"),
                std::string_view("bipolar_percent"),
                std::string_view("bipolar_percent_50"),
                std::string_view("gm_volume_db") };
            const auto decode_knob = [&](const MsgpackValue &value)
                    -> std::optional<KksMidiKnobEntry> {
                const auto *fields = fields_of(value);
                if (!fields)
                    return std::nullopt;
                const auto *mapping = registry_map_value(*fields, "mapping");
                const auto *label = registry_map_value(*fields, "label");
                const auto *custom =
                        registry_map_value(*fields, "custom_label");
                const auto *display_mode =
                        registry_map_value(*fields, "display_mode");
                if (!mapping || !label || !custom || !display_mode)
                    return std::nullopt;
                auto decoded_mapping = decode_encoder_mapping(*mapping);
                const auto *decoded_label =
                        std::get_if<std::string>(&label->value);
                const auto *decoded_custom = std::get_if<bool>(&custom->value);
                const auto decoded_display = enum_index(*display_mode,
                        std::span<const std::string_view>(display_mode_names));
                if (!decoded_mapping || !decoded_label || !decoded_custom ||
                        !decoded_display)
                    return std::nullopt;
                return KksMidiKnobEntry{ std::move(*decoded_mapping),
                    *decoded_label, *decoded_custom,
                    static_cast<KksMidiEncoderDisplayMode>(*decoded_display) };
            };
            const auto decode_button = [&](const MsgpackValue &value)
                    -> std::optional<KksMidiButtonEntry> {
                const auto *fields = fields_of(value);
                if (!fields)
                    return std::nullopt;
                const auto *mapping = registry_map_value(*fields, "mapping");
                const auto *color = registry_map_value(*fields, "color");
                const auto *label = registry_map_value(*fields, "label");
                const auto *custom =
                        registry_map_value(*fields, "custom_label");
                if (!mapping || !color || !label || !custom)
                    return std::nullopt;
                auto decoded_mapping = decode_button_mapping(*mapping);
                auto decoded_color = symbol(*color);
                const auto *decoded_label =
                        std::get_if<std::string>(&label->value);
                const auto *decoded_custom = std::get_if<bool>(&custom->value);
                if (!decoded_mapping || !decoded_color || !decoded_label ||
                        !decoded_custom)
                    return std::nullopt;
                return KksMidiButtonEntry{ std::move(*decoded_mapping),
                    std::move(*decoded_color), *decoded_label,
                    *decoded_custom };
            };
            const auto decode_page = [&](const MsgpackValue &value)
                    -> std::optional<KksMidiMappingPage> {
                const auto *fields = fields_of(value);
                if (!fields)
                    return std::nullopt;
                const auto *knobs = registry_map_value(*fields, "knobs");
                const auto *buttons = registry_map_value(*fields, "buttons");
                const auto *knob_values =
                        knobs ? std::get_if<MsgpackValue::Array>(&knobs->value)
                              : nullptr;
                if (!knob_values || knob_values->size() != 8 || !buttons)
                    return std::nullopt;
                KksMidiMappingPage page;
                for (std::size_t index = 0; index < page.knobs.size();
                        ++index) {
                    if (std::holds_alternative<std::monostate>(
                                (*knob_values)[index].value))
                        continue;
                    auto decoded = decode_knob((*knob_values)[index]);
                    if (!decoded)
                        return std::nullopt;
                    page.knobs[index] = std::move(*decoded);
                }
                if (std::holds_alternative<std::monostate>(buttons->value))
                    return page;
                const auto *button_values =
                        std::get_if<MsgpackValue::Array>(&buttons->value);
                if (!button_values || button_values->size() != 8)
                    return std::nullopt;
                page.buttons.emplace();
                for (std::size_t index = 0; index < page.buttons->size();
                        ++index) {
                    if (std::holds_alternative<std::monostate>(
                                (*button_values)[index].value))
                        continue;
                    auto decoded = decode_button((*button_values)[index]);
                    if (!decoded)
                        return std::nullopt;
                    (*page.buttons)[index] = std::move(*decoded);
                }
                return page;
            };

            constexpr std::array keyzone_priority_names{
                std::string_view("plugin"), std::string_view("keyzone")
            };
            const auto decode_keyzone = [&](const MsgpackValue &value)
                    -> std::optional<KksMidiKeyzoneAssignment> {
                const auto *fields = fields_of(value);
                if (!fields)
                    return std::nullopt;
                const auto *upper = registry_map_value(*fields, "upper_key");
                const auto *channel =
                        registry_map_value(*fields, "midi_channel");
                const auto *color = registry_map_value(*fields, "color");
                const auto *transpose =
                        registry_map_value(*fields, "semitones_transpose");
                const auto *curve_value =
                        registry_map_value(*fields, "velocity_curve");
                const auto *disable = registry_map_value(*fields, "disable");
                const auto *priority = registry_map_value(*fields, "priority");
                if (!upper || !channel || !color || !transpose ||
                        !curve_value || !disable || !priority)
                    return std::nullopt;
                const auto decoded_upper = unsigned_byte(*upper);
                const auto decoded_channel = unsigned_byte(*channel);
                auto decoded_color = symbol(*color);
                const auto decoded_transpose = signed_byte(*transpose);
                const auto *decoded_disable =
                        std::get_if<bool>(&disable->value);
                const auto decoded_priority =
                        enum_index(*priority, std::span<const std::string_view>(
                                                      keyzone_priority_names));
                std::optional<KksVelocityCurve> decoded_curve;
                if (!std::holds_alternative<std::monostate>(
                            curve_value->value)) {
                    const auto index = enum_index(*curve_value,
                            std::span<const std::string_view>(velocity_names));
                    if (!index)
                        return std::nullopt;
                    decoded_curve = static_cast<KksVelocityCurve>(*index);
                }
                if (!decoded_upper || !decoded_channel || !decoded_color ||
                        !decoded_transpose || !decoded_disable ||
                        !decoded_priority)
                    return std::nullopt;
                return KksMidiKeyzoneAssignment{ *decoded_upper,
                    *decoded_channel, std::move(*decoded_color),
                    *decoded_transpose, decoded_curve, *decoded_disable,
                    static_cast<KksMidiKeyzonePriority>(*decoded_priority) };
            };

            const auto decode_pedal = [&](const MsgpackValue &value)
                    -> std::optional<KksMidiPedalAssignment> {
                const auto entry = tagged(value);
                if (!entry)
                    return std::nullopt;
                const auto *fields = fields_of(*entry->second);
                if (!fields)
                    return std::nullopt;
                const auto *configuration =
                        registry_map_value(*fields, "configuration");
                const auto *config_fields =
                        configuration ? fields_of(*configuration) : nullptr;
                if (!configuration || !config_fields)
                    return std::nullopt;
                if (entry->first == "pedal_continuous_configuration") {
                    const auto *swap =
                            registry_map_value(*config_fields, "swap_tip_ring");
                    const auto *invert =
                            registry_map_value(*config_fields, "invert");
                    const auto *mapping =
                            registry_map_value(*fields, "mapping");
                    const auto *decoded_swap =
                            swap ? std::get_if<bool>(&swap->value) : nullptr;
                    const auto *decoded_invert =
                            invert ? std::get_if<bool>(&invert->value)
                                   : nullptr;
                    auto decoded_mapping =
                            mapping ? decode_continuous_mapping(*mapping)
                                    : std::nullopt;
                    if (!decoded_swap || !decoded_invert || !decoded_mapping)
                        return std::nullopt;
                    return KksMidiPedalAssignment(
                            KksMidiPedalContinuousAssignment{
                                    { *decoded_swap, *decoded_invert },
                                    std::move(*decoded_mapping) });
                }
                if (entry->first != "pedal_switch_configuration")
                    return std::nullopt;
                const auto *invert =
                        registry_map_value(*config_fields, "invert");
                const auto *tip = registry_map_value(*fields, "mapping_tip");
                const auto *ring = registry_map_value(*fields, "mapping_ring");
                const auto *decoded_invert =
                        invert ? std::get_if<bool>(&invert->value) : nullptr;
                auto decoded_tip =
                        tip ? decode_button_mapping(*tip) : std::nullopt;
                auto decoded_ring =
                        ring ? decode_button_mapping(*ring) : std::nullopt;
                if (!decoded_invert || !decoded_tip || !decoded_ring)
                    return std::nullopt;
                return KksMidiPedalAssignment(KksMidiPedalSwitchAssignment{
                        { *decoded_invert }, std::move(*decoded_tip),
                        std::move(*decoded_ring) });
            };

            const auto decode_continuous_map =
                    [&](const MsgpackValue::Map &values)
                    -> std::optional<std::vector<std::pair<RpcMethodId,
                            std::optional<KksMidiContinuousMapping>>>> {
                std::vector<std::pair<RpcMethodId,
                        std::optional<KksMidiContinuousMapping>>>
                        result;
                result.reserve(values.size());
                for (const auto &[key, raw] : values) {
                    auto decoded_key = symbol(key);
                    if (!decoded_key)
                        return std::nullopt;
                    std::optional<KksMidiContinuousMapping> decoded_value;
                    if (!std::holds_alternative<std::monostate>(raw.value)) {
                        decoded_value = decode_continuous_mapping(raw);
                        if (!decoded_value)
                            return std::nullopt;
                    }
                    result.emplace_back(
                            std::move(*decoded_key), std::move(decoded_value));
                }
                return result;
            };
            const auto decode_pedal_map = [&](const MsgpackValue::Map &values)
                    -> std::optional<std::vector<std::pair<RpcMethodId,
                            std::optional<KksMidiPedalAssignment>>>> {
                std::vector<std::pair<RpcMethodId,
                        std::optional<KksMidiPedalAssignment>>>
                        result;
                result.reserve(values.size());
                for (const auto &[key, raw] : values) {
                    auto decoded_key = symbol(key);
                    if (!decoded_key)
                        return std::nullopt;
                    std::optional<KksMidiPedalAssignment> decoded_value;
                    if (!std::holds_alternative<std::monostate>(raw.value)) {
                        decoded_value = decode_pedal(raw);
                        if (!decoded_value)
                            return std::nullopt;
                    }
                    result.emplace_back(
                            std::move(*decoded_key), std::move(decoded_value));
                }
                return result;
            };

            constexpr std::array midi_port_names{ std::string_view(
                                                          "usb_priority"),
                std::string_view("dual"), std::string_view("din_priority") };
            std::vector<KksMidiTemplate> decoded_templates;
            decoded_templates.reserve(template_values->size());
            for (const auto &raw_template : *template_values) {
                const auto *template_fields =
                        std::get_if<MsgpackValue::Map>(&raw_template.value);
                if (!template_fields)
                    return std::nullopt;
                const auto *template_name =
                        registry_map_value(*template_fields, "template_name");
                const auto *pages =
                        registry_map_value(*template_fields, "pages");
                const auto *wheels = registry_map_value(
                        *template_fields, "wheel_assignments");
                const auto *touchstrip = registry_map_value(
                        *template_fields, "touchstrip_assignments");
                const auto *keyboard = registry_map_value(
                        *template_fields, "keyboard_assignments");
                const auto *pedals = registry_map_value(
                        *template_fields, "pedal_assignments");
                const auto *template_color =
                        registry_map_value(*template_fields, "color");
                const auto *image =
                        registry_map_value(*template_fields, "image_data");
                const auto *hide = registry_map_value(
                        *template_fields, "hide_template_name");
                const auto *preference = registry_map_value(
                        *template_fields, "midi_output_preference");
                if (!template_name || !pages || !wheels || !touchstrip ||
                        !keyboard || !pedals || !template_color || !image ||
                        !hide || !preference)
                    return std::nullopt;
                const auto *name =
                        std::get_if<std::string>(&template_name->value);
                const auto *page_values =
                        std::get_if<MsgpackValue::Array>(&pages->value);
                const auto *wheel_values =
                        std::get_if<MsgpackValue::Map>(&wheels->value);
                const auto *touchstrip_values =
                        std::get_if<MsgpackValue::Map>(&touchstrip->value);
                const auto *keyboard_values =
                        std::get_if<MsgpackValue::Array>(&keyboard->value);
                const auto *pedal_values =
                        std::get_if<MsgpackValue::Map>(&pedals->value);
                auto color_symbol = symbol(*template_color);
                const auto *image_value =
                        std::get_if<std::string>(&image->value);
                const auto *hide_value = std::get_if<bool>(&hide->value);
                const bool valid_preference =
                        std::holds_alternative<std::monostate>(
                                preference->value) ||
                        symbol(*preference).has_value();
                if (!name || !page_values || page_values->size() > 16 ||
                        !wheel_values || !touchstrip_values ||
                        !keyboard_values || keyboard_values->size() > 16 ||
                        !pedal_values || !color_symbol || !image_value ||
                        !hide_value || !valid_preference)
                    return std::nullopt;
                KksMidiTemplate decoded_template;
                decoded_template.template_name = *name;
                decoded_template.pages.reserve(page_values->size());
                for (const auto &raw_page : *page_values) {
                    auto decoded_page = decode_page(raw_page);
                    if (!decoded_page)
                        return std::nullopt;
                    decoded_template.pages.push_back(std::move(*decoded_page));
                }
                auto decoded_wheels = decode_continuous_map(*wheel_values);
                auto decoded_touchstrip =
                        decode_continuous_map(*touchstrip_values);
                if (!decoded_wheels || !decoded_touchstrip)
                    return std::nullopt;
                decoded_template.wheel_assignments = std::move(*decoded_wheels);
                decoded_template.touchstrip_assignments =
                        std::move(*decoded_touchstrip);
                decoded_template.keyboard_assignments.reserve(
                        keyboard_values->size());
                for (const auto &raw_keyzone : *keyboard_values) {
                    auto decoded_keyzone = decode_keyzone(raw_keyzone);
                    if (!decoded_keyzone)
                        return std::nullopt;
                    decoded_template.keyboard_assignments.push_back(
                            std::move(*decoded_keyzone));
                }
                auto decoded_pedals = decode_pedal_map(*pedal_values);
                if (!decoded_pedals)
                    return std::nullopt;
                decoded_template.pedal_assignments = std::move(*decoded_pedals);
                decoded_template.color = std::move(*color_symbol);
                decoded_template.image_data = *image_value;
                decoded_template.hide_template_name = *hide_value;
                if (!std::holds_alternative<std::monostate>(
                            preference->value)) {
                    const auto decoded_preference = enum_index(*preference,
                            std::span<const std::string_view>(midi_port_names));
                    if (!decoded_preference)
                        return std::nullopt;
                    decoded_template.midi_output_preference =
                            static_cast<KksMidiPortPreference>(
                                    *decoded_preference);
                }
                decoded_templates.push_back(std::move(decoded_template));
            }

            if (is_transfer) {
                return KksMidiTemplateTransferEvent{ *transfer_command,
                    std::move(decoded_templates) };
            }
            decoded_settings.midi_templates = std::move(decoded_templates);
            return decoded_settings;
        }
        case KksDeviceMethod::accessibility_event: {
            constexpr std::array command_names{
                std::string_view("unknown"),
                std::string_view("connect"),
                std::string_view("disconnect"),
                std::string_view("toggle_training_mode"),
                std::string_view("create_zone"),
                std::string_view("set_control_data"),
                std::string_view("set_control_state"),
                std::string_view("clear_control_data"),
                std::string_view("switch_layer"),
                std::string_view("trigger_speech"),
            };
            constexpr std::array control_state_names{
                std::string_view("NO_STATE"),
                std::string_view("OFF"),
                std::string_view("ON"),
                std::string_view("DISABLED"),
                std::string_view("FOCUSED"),
                std::string_view("INACTIVE"),
            };

            const auto *event_fields =
                    std::get_if<MsgpackValue::Map>(&p[0].value);
            if (!event_fields)
                return std::nullopt;
            const auto *command_value =
                    registry_map_value(*event_fields, "command");
            const auto *data_value = registry_map_value(*event_fields, "data");
            if (!command_value || !data_value)
                return std::nullopt;
            const auto command_name = symbol_name(*command_value);
            if (!command_name)
                return std::nullopt;
            const auto command_it =
                    std::ranges::find(command_names, *command_name);
            if (command_it == command_names.end())
                return std::nullopt;
            const auto command = static_cast<KksAccessibilityCommand>(
                    command_it - command_names.begin());

            const auto *variant =
                    std::get_if<MsgpackValue::Map>(&data_value->value);
            if (!variant || variant->size() != 1)
                return std::nullopt;
            const auto *tag =
                    std::get_if<std::string>(&variant->front().first.value);
            if (!tag)
                return std::nullopt;
            const auto &model = variant->front().second;
            KksAccessibilityData data;
            if (*tag == "no_data") {
                if (!std::holds_alternative<std::monostate>(model.value))
                    return std::nullopt;
                data = KksAccessibilityNoData{};
            } else {
                const auto *fields =
                        std::get_if<MsgpackValue::Map>(&model.value);
                if (!fields)
                    return std::nullopt;
                if (*tag == "zone_data") {
                    const auto *zone = registry_map_value(*fields, "zone_id");
                    const auto *name = registry_map_value(*fields, "name");
                    const auto *description =
                            registry_map_value(*fields, "description");
                    if (!zone || !name || !description)
                        return std::nullopt;
                    const auto zone_id = unsigned_byte(*zone);
                    const auto *name_text =
                            std::get_if<std::string>(&name->value);
                    const auto *description_text =
                            std::get_if<std::string>(&description->value);
                    if (!zone_id || !name_text || !description_text)
                        return std::nullopt;
                    data = KksAccessibilityZoneData{ *zone_id, *name_text,
                        *description_text };
                } else if (*tag == "control_data") {
                    const auto *zone = registry_map_value(*fields, "zone_id");
                    const auto *control =
                            registry_map_value(*fields, "control_id");
                    const auto *layer = registry_map_value(*fields, "layer");
                    const auto *row = registry_map_value(*fields, "row");
                    const auto *level = registry_map_value(*fields, "level");
                    const auto *text = registry_map_value(*fields, "text");
                    if (!zone || !control || !layer || !row || !level || !text)
                        return std::nullopt;
                    const auto zone_id = unsigned_byte(*zone);
                    const auto control_id = unsigned_word(*control);
                    const auto layer_id = unsigned_byte(*layer);
                    const auto row_id = unsigned_byte(*row);
                    const auto level_id = unsigned_byte(*level);
                    const auto *label = std::get_if<std::string>(&text->value);
                    if (!zone_id || !control_id || !layer_id || !row_id ||
                            !level_id || !label)
                        return std::nullopt;
                    data = KksAccessibilityControlData{ *zone_id, *control_id,
                        *layer_id, *row_id, *level_id, *label };
                } else if (*tag == "control_state_data") {
                    const auto *zone = registry_map_value(*fields, "zone_id");
                    const auto *control =
                            registry_map_value(*fields, "control_id");
                    const auto *layer = registry_map_value(*fields, "layer");
                    const auto *state =
                            registry_map_value(*fields, "control_state");
                    if (!zone || !control || !layer || !state)
                        return std::nullopt;
                    const auto zone_id = unsigned_byte(*zone);
                    const auto control_id = unsigned_word(*control);
                    const auto layer_id = unsigned_byte(*layer);
                    const auto state_name = symbol_name(*state);
                    if (!zone_id || !control_id || !layer_id || !state_name)
                        return std::nullopt;
                    const auto state_it =
                            std::ranges::find(control_state_names, *state_name);
                    if (state_it == control_state_names.end())
                        return std::nullopt;
                    data = KksAccessibilityControlStateData{ *zone_id,
                        *control_id, *layer_id,
                        static_cast<KksAccessibilityControlState>(
                                state_it - control_state_names.begin()) };
                } else if (*tag == "layer_data") {
                    const auto *layer = registry_map_value(*fields, "layer");
                    if (!layer)
                        return std::nullopt;
                    const auto layer_id = unsigned_byte(*layer);
                    if (!layer_id)
                        return std::nullopt;
                    data = KksAccessibilityLayerData{ *layer_id };
                } else if (*tag == "control") {
                    const auto *zone = registry_map_value(*fields, "zone_id");
                    const auto *control =
                            registry_map_value(*fields, "control_id");
                    if (!zone || !control)
                        return std::nullopt;
                    const auto zone_id = unsigned_byte(*zone);
                    const auto control_id = unsigned_word(*control);
                    if (!zone_id || !control_id)
                        return std::nullopt;
                    data = KksAccessibilityControl{ *zone_id, *control_id };
                } else {
                    return std::nullopt;
                }
            }
            return KksAccessibilityEvent{ command, std::move(data) };
        }
        case KksDeviceMethod::handshake_reply:
            if (auto reply = decode_kks_handshake_reply(p[0]))
                return std::move(*reply);
            return std::nullopt;
        case KksDeviceMethod::device_property_changed: {
            auto name = symbol(p[0]);
            auto value = property_value(p[1]);
            if (!name || !value)
                return std::nullopt;
            return KksDevicePropertyChanged{ std::move(*name),
                std::move(*value) };
        }
        case KksDeviceMethod::device_properties: {
            const auto *fields = std::get_if<MsgpackValue::Map>(&p[0].value);
            if (!fields)
                return std::nullopt;
            KksDeviceProperties result;
            result.properties.reserve(fields->size());
            for (const auto &[key, raw] : *fields) {
                auto name = symbol(key);
                auto value = property_value(raw);
                if (!name || !value)
                    return std::nullopt;
                result.properties.emplace_back(
                        std::move(*name), std::move(*value));
            }
            return result;
        }
        case KksDeviceMethod::set_active_instance_client:
            if (std::holds_alternative<std::monostate>(p[0].value))
                return KksActiveInstanceClient{ std::nullopt };
            if (auto client = symbol(p[0]))
                return KksActiveInstanceClient{ std::move(*client) };
            return std::nullopt;
        case KksDeviceMethod::set_focus:
            if (auto client = symbol(p[0]))
                return KksFocus{ std::move(*client) };
            return std::nullopt;
        case KksDeviceMethod::data_tracking_active:
            if (const auto *active = std::get_if<bool>(&p[0].value))
                return KksDataTrackingActive{ *active };
            return std::nullopt;
        case KksDeviceMethod::
                client_instance_set_device_owned_instance_client_state: {
            auto client = symbol(p[0]);
            const auto *fields = std::get_if<MsgpackValue::Map>(&p[1].value);
            if (!client || !fields)
                return std::nullopt;
            OdrDeviceOwnedInstanceState state;
            for (const auto &[key, raw] : *fields) {
                const auto name = symbol_name(key);
                if (!name)
                    continue;
                if (*name == "page") {
                    const auto page = symbol_name(raw);
                    if (!page)
                        return std::nullopt;
                    if (*page == "browser")
                        state.page = OdrInstancePage::browser;
                    else if (*page == "mixer")
                        state.page = OdrInstancePage::mixer;
                    else if (*page == "parameter")
                        state.page = OdrInstancePage::parameter;
                    else if (*page == "smartplay")
                        state.page = OdrInstancePage::smartplay;
                    else
                        return std::nullopt;
                } else if (*name == "has_lightguide_focus") {
                    const auto *focus = std::get_if<bool>(&raw.value);
                    if (!focus)
                        return std::nullopt;
                    state.has_lightguide_focus = *focus;
                }
            }
            return KksDeviceOwnedInstanceClientState{ std::move(*client),
                state };
        }
        default:
            return std::nullopt;
    }
}

std::vector<std::byte> encode_rpc_notification(
        const RpcMethodId &method, std::span<const MsgpackValue> parameters) {
    Bytes payload;
    byte(payload, 0x93);
    byte(payload, 0x02);
    if (const auto *name = std::get_if<std::string>(&method))
        pack_string(payload, *name);
    else
        pack(payload,
                MsgpackValue(std::uint64_t(std::get<std::uint16_t>(method))));
    size_header(payload, parameters.size(), 0x90, 0xdc, 0xdd, 15);
    for (const auto &parameter : parameters)
        pack(payload, parameter);

    Bytes frame;
    frame.reserve(payload.size() + 4);
    const auto size = static_cast<std::uint32_t>(payload.size());
    byte(frame, size);
    byte(frame, size >> 8);
    byte(frame, size >> 16);
    byte(frame, size >> 24);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<std::byte> encode_rpc_request(std::uint32_t id,
        const RpcMethodId &method, std::span<const MsgpackValue> parameters) {
    Bytes payload;
    byte(payload, 0x94);
    byte(payload, 0x00);
    pack(payload, MsgpackValue(std::uint64_t(id)));
    if (const auto *name = std::get_if<std::string>(&method))
        pack_string(payload, *name);
    else
        pack(payload,
                MsgpackValue(std::uint64_t(std::get<std::uint16_t>(method))));
    size_header(payload, parameters.size(), 0x90, 0xdc, 0xdd, 15);
    for (const auto &parameter : parameters)
        pack(payload, parameter);
    Bytes frame;
    const auto size = static_cast<std::uint32_t>(payload.size());
    frame.reserve(size + 4);
    byte(frame, size);
    byte(frame, size >> 8);
    byte(frame, size >> 16);
    byte(frame, size >> 24);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<std::byte> encode_rpc_success_response(std::uint32_t id) {
    return encode_rpc_response(
            id, MsgpackValue{}, MsgpackValue(MsgpackValue::Array{}));
}

std::vector<std::byte> encode_rpc_response(std::uint32_t id,
        const MsgpackValue &error, const MsgpackValue &result) {
    Bytes payload{ std::byte{ 0x94 }, std::byte{ 0x01 } };
    pack(payload, MsgpackValue(std::uint64_t(id)));
    pack(payload, error);
    pack(payload, result);
    Bytes frame;
    const auto size = static_cast<std::uint32_t>(payload.size());
    frame.reserve(size + 4);
    byte(frame, size);
    byte(frame, size >> 8);
    byte(frame, size >> 16);
    byte(frame, size >> 24);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<std::byte> encode_odr_instance_hello(std::uint32_t request_id,
        std::string_view identity, const OdrInstanceInfo &client_info) {
    constexpr std::array type_names{ std::string_view("standalone"),
        std::string_view("vst2"), std::string_view("vst3"),
        std::string_view("audio_unit"), std::string_view("aax") };
    const auto type_index = static_cast<std::size_t>(client_info.type);
    if (type_index >= type_names.size())
        return {};
    MsgpackValue::Map info{ { "name", client_info.name },
        { "version", client_info.version }, { "type", type_names[type_index] },
        { "is_host", client_info.is_host } };
    MsgpackValue::Map handshake{ { "client_info",
                                         MsgpackValue(std::move(info)) },
        { "ipc_protocol_version", MsgpackValue(odr_ipc_protocol_version) } };
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(std::move(handshake)) };
    return encode_rpc_request(
            request_id, std::string("instance_hello"), parameters);
}

std::optional<RpcNotification> decode_rpc_notification(
        std::span<const std::byte> frame) {
    if (frame.size() < 4 || payload_size(frame) != frame.size() - 4)
        return std::nullopt;
    Reader reader(frame.subspan(4));
    auto root = reader.value();
    if (!root || !reader.done())
        return std::nullopt;
    const auto *array = std::get_if<MsgpackValue::Array>(&root->value);
    if (!array || array->size() != 3)
        return std::nullopt;
    const auto *type = std::get_if<std::uint64_t>(&(*array)[0].value);
    const auto *params = std::get_if<MsgpackValue::Array>(&(*array)[2].value);
    if (!type || *type != 2 || !params)
        return std::nullopt;
    RpcMethodId method;
    if (const auto *name = std::get_if<std::string>(&(*array)[1].value))
        method = *name;
    else if (const auto *index = std::get_if<std::uint64_t>(&(*array)[1].value);
            index && *index <= std::numeric_limits<std::uint16_t>::max())
        method = static_cast<std::uint16_t>(*index);
    else
        return std::nullopt;
    return RpcNotification{ std::move(method), *params };
}

std::optional<RpcRequest> decode_rpc_request(std::span<const std::byte> frame) {
    if (frame.size() < 4 || payload_size(frame) != frame.size() - 4)
        return std::nullopt;
    Reader reader(frame.subspan(4));
    auto root = reader.value();
    if (!root || !reader.done())
        return std::nullopt;
    const auto *array = std::get_if<MsgpackValue::Array>(&root->value);
    if (!array || array->size() != 4)
        return std::nullopt;
    const auto *type = std::get_if<std::uint64_t>(&(*array)[0].value);
    const auto *id = std::get_if<std::uint64_t>(&(*array)[1].value);
    const auto *params = std::get_if<MsgpackValue::Array>(&(*array)[3].value);
    if (!type || *type != 0 || !id || *id > 0xffffffffu || !params)
        return std::nullopt;
    RpcMethodId method;
    if (const auto *name = std::get_if<std::string>(&(*array)[2].value))
        method = *name;
    else if (const auto *index = std::get_if<std::uint64_t>(&(*array)[2].value);
            index && *index <= 0xffff)
        method = static_cast<std::uint16_t>(*index);
    else
        return std::nullopt;
    return RpcRequest{ static_cast<std::uint32_t>(*id), std::move(method),
        *params };
}

std::optional<RpcResponse> decode_rpc_response(
        std::span<const std::byte> frame) {
    if (frame.size() < 4 || payload_size(frame) != frame.size() - 4)
        return std::nullopt;
    Reader reader(frame.subspan(4));
    auto root = reader.value();
    if (!root || !reader.done())
        return std::nullopt;
    const auto *array = std::get_if<MsgpackValue::Array>(&root->value);
    if (!array || array->size() != 4)
        return std::nullopt;
    const auto *type = std::get_if<std::uint64_t>(&(*array)[0].value);
    const auto *id = std::get_if<std::uint64_t>(&(*array)[1].value);
    if (!type || *type != 1 || !id || *id > 0xffffffffu)
        return std::nullopt;
    return RpcResponse{ static_cast<std::uint32_t>(*id), (*array)[2],
        (*array)[3] };
}

std::optional<OdrHandshakeReply> decode_odr_handshake_reply(
        const RpcResponse &response) {
    if (!std::holds_alternative<std::monostate>(response.error.value))
        return std::nullopt;
    const auto *fields = std::get_if<MsgpackValue::Map>(&response.result.value);
    if (!fields)
        return std::nullopt;
    const auto *agent = map_value(*fields, "agent_version");
    const auto *protocol = map_value(*fields, "ipc_protocol_version");
    const auto *devices = map_value(*fields, "available_devices");
    const auto *assets = map_value(*fields, "registered_assets");
    const auto *registry = map_value(*fields, "symbol_registry");
    if (!agent || !protocol || !devices || !assets || !registry)
        return std::nullopt;
    const auto *agent_text = std::get_if<std::string>(&agent->value);
    const auto *protocol_text = std::get_if<std::string>(&protocol->value);
    const auto *device_values =
            std::get_if<MsgpackValue::Array>(&devices->value);
    const auto *asset_values = std::get_if<MsgpackValue::Array>(&assets->value);
    const auto *registry_values =
            std::get_if<MsgpackValue::Array>(&registry->value);
    if (!agent_text || !protocol_text || !device_values || !asset_values ||
            !registry_values)
        return std::nullopt;
    const auto parsed_protocol = parse_odr_protocol_version(*protocol_text);
    if (!parsed_protocol)
        return std::nullopt;
    OdrHandshakeReply reply{ *agent_text, *protocol_text, *parsed_protocol,
        supports_odr_protocol_2_1(*parsed_protocol), {}, {}, {} };
    reply.available_devices.reserve(device_values->size());
    for (const auto &value : *device_values) {
        auto device = decode_odr_usb_device_info(value);
        if (!device)
            return std::nullopt;
        reply.available_devices.push_back(std::move(*device));
    }
    reply.registered_assets.reserve(asset_values->size());
    for (const auto &value : *asset_values) {
        const auto *bytes = std::get_if<MsgpackValue::Binary>(&value.value);
        if (!bytes || bytes->size() != 32)
            return std::nullopt;
        std::array<std::byte, 32> asset{};
        std::ranges::copy(*bytes, asset.begin());
        reply.registered_assets.push_back(asset);
    }
    reply.symbol_registry.reserve(registry_values->size());
    for (const auto &value : *registry_values) {
        const auto *text = std::get_if<std::string>(&value.value);
        if (!text)
            return std::nullopt;
        reply.symbol_registry.push_back(*text);
    }
    return reply;
}

std::optional<OdrUsbDeviceInfo> decode_odr_usb_device_info(
        const MsgpackValue &value) {
    const auto *fields = std::get_if<MsgpackValue::Map>(&value.value);
    if (!fields || fields->size() != 6)
        return std::nullopt;
    const auto *manufacturer = map_value(*fields, "manufacturer");
    const auto *product = map_value(*fields, "product");
    const auto *serial = map_value(*fields, "serialnumber");
    const auto *vendor = map_value(*fields, "vendorID");
    const auto *product_id = map_value(*fields, "productID");
    const auto *version = map_value(*fields, "versionNumber");
    if (!manufacturer || !product || !serial || !vendor || !product_id ||
            !version)
        return std::nullopt;
    const auto *manufacturer_text =
            std::get_if<std::string>(&manufacturer->value);
    const auto *product_text = std::get_if<std::string>(&product->value);
    const auto *serial_text = std::get_if<std::string>(&serial->value);
    const auto *vendor_value = std::get_if<std::uint64_t>(&vendor->value);
    const auto *product_value = std::get_if<std::uint64_t>(&product_id->value);
    const auto *version_value = std::get_if<std::uint64_t>(&version->value);
    if (!manufacturer_text || !product_text || !serial_text || !vendor_value ||
            !product_value || !version_value || *vendor_value > 0xffff ||
            *product_value > 0xffff || *version_value > 0xffff)
        return std::nullopt;
    return OdrUsbDeviceInfo{ static_cast<std::uint16_t>(*vendor_value),
        static_cast<std::uint16_t>(*product_value),
        static_cast<std::uint16_t>(*version_value), *manufacturer_text,
        *product_text, *serial_text };
}

void RpcFrameDecoder::append(std::span<const std::byte> bytes) {
    if (!invalid_frame)
        buffered.insert(buffered.end(), bytes.begin(), bytes.end());
}

std::optional<std::vector<std::byte>> RpcFrameDecoder::next() {
    if (invalid_frame || buffered.size() < 4)
        return std::nullopt;
    const auto size = payload_size(buffered);
    if (size > max_payload_size) {
        invalid_frame = true;
        buffered.clear();
        return std::nullopt;
    }
    if (buffered.size() < std::size_t(size) + 4)
        return std::nullopt;
    std::vector<std::byte> result(
            buffered.begin(), buffered.begin() + size + 4);
    buffered.erase(buffered.begin(), buffered.begin() + size + 4);
    return result;
}

void RpcFrameDecoder::clear() noexcept {
    buffered.clear();
    invalid_frame = false;
}

std::vector<std::byte> OdrClientMessages::instance_goodbye() const {
    const std::array parameters{ MsgpackValue(identity) };
    return notification("instance_goodbye", parameters);
}

std::vector<std::byte> OdrClientMessages::request(std::uint32_t request_id,
        std::string_view method,
        std::span<const MsgpackValue> parameters) const {
    return encode_rpc_request(request_id, method_id(method), parameters);
}

std::vector<std::byte> OdrClientMessages::subscribe_device_property(
        std::string_view device_identifier, std::string_view property) const {
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(device_identifier), protocol_symbol(property) };
    return notification("subscribe_device_properties", parameters);
}

RpcMethodId OdrClientMessages::method_id(std::string_view method) const {
    if (registry) {
        if (const auto index = registry->resolve(method))
            return *index;
    }
    return std::string(method);
}

MsgpackValue OdrClientMessages::protocol_symbol(std::string_view symbol) const {
    if (registry) {
        if (const auto index = registry->resolve(symbol))
            return MsgpackValue(std::uint64_t(*index));
    }
    return MsgpackValue(symbol);
}

std::vector<std::byte> OdrClientMessages::notification(std::string_view method,
        std::span<const MsgpackValue> parameters) const {
    if (!registry)
        return encode_rpc_notification(method_id(method), parameters);
    MsgpackValue::Array compact(parameters.begin(), parameters.end());
    for (auto &parameter : compact)
        compact_map_keys(parameter, *registry);
    return encode_rpc_notification(method_id(method), compact);
}

std::vector<std::byte> OdrClientMessages::identity_instance_message(
        std::string_view method, std::string_view instance_id) const {
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(instance_id) };
    return notification(method, parameters);
}

std::vector<std::byte> OdrClientMessages::request_focus(
        std::string_view instance_id) const {
    return identity_instance_message("client_request_focus", instance_id);
}

std::vector<std::byte> OdrClientMessages::release_focus(
        std::string_view instance_id) const {
    return identity_instance_message("client_release_focus", instance_id);
}

std::vector<std::byte> OdrClientMessages::set_page(
        std::string_view instance_id, std::uint8_t raw_page) const {
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(instance_id), MsgpackValue(std::uint64_t(raw_page)) };
    return notification("client_set_page", parameters);
}

std::vector<std::byte> OdrClientMessages::set_parameter_value(
        std::string_view instance_id, std::uint32_t parameter_id,
        const OdrParameterValue &input) const {
    MsgpackValue encoded = parameter_value(input);
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(instance_id), MsgpackValue(std::uint64_t(parameter_id)),
        std::move(encoded) };
    return notification(
            "client_parameter_page_set_parameter_value", parameters);
}

std::vector<std::byte> OdrClientMessages::set_parameter_page_data(
        std::string_view instance_id, const OdrParameterPageData &data) const {
    MsgpackValue background;
    if (data.plugin_data.background) {
        background = MsgpackValue(
                MsgpackValue::Binary(data.plugin_data.background->begin(),
                        data.plugin_data.background->end()));
    }
    auto parameters = data.plugin_data.parameters;
    std::ranges::stable_sort(
            parameters, {}, &std::pair<std::uint32_t, OdrParameter>::first);
    MsgpackValue::Map parameter_map;
    parameter_map.reserve(parameters.size());
    for (const auto &[id, value] : parameters) {
        parameter_map.emplace_back(std::uint64_t(id), parameter(value));
    }
    MsgpackValue plugin_data(MsgpackValue::Map{
            { "name", data.plugin_data.name },
            { "background", std::move(background) },
            { "parameters", std::move(parameter_map) },
            { "plugin_color", rgb_color(data.plugin_data.plugin_color) },
            { "nks_control_color",
                    rgb_color(data.plugin_data.nks_control_color) } });
    MsgpackValue device_state(MsgpackValue::Map{
            { "view_address",
                    view_address(data.device_owned_viewstate.view_address) },
            { "force_host_viewstate",
                    data.device_owned_viewstate.force_host_viewstate } });
    MsgpackValue host_state(MsgpackValue::Map{
            { "nks1_layout",
                    nks1_layout(data.host_owned_viewstate.nks1_layout) },
            { "nks2_layout",
                    nks2_layout(data.host_owned_viewstate.nks2_layout) } });
    MsgpackValue model(
            MsgpackValue::Map{ { "plugin_data", std::move(plugin_data) },
                    { "device_owned_viewstate", std::move(device_state) },
                    { "host_owned_viewstate", std::move(host_state) } });
    const std::array parameters_out{ MsgpackValue(identity),
        MsgpackValue(instance_id), std::move(model) };
    return notification("client_parameter_page_set_data", parameters_out);
}

std::vector<std::byte> OdrClientMessages::set_plugin_chain_data(
        std::string_view instance_id, const OdrPluginChainData &data) const {
    const auto symbols = registry ? &*registry : nullptr;
    auto model = plugin_chain_value(data, symbols);
    if (!model)
        return {};
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(instance_id), std::move(*model) };
    return notification("client_plugin_chain_set_data", parameters);
}

std::vector<std::byte> OdrClientMessages::set_lightguide(
        std::string_view instance_id, const LightguideState &state) const {
    MsgpackValue encoded = lightguide_value(state);
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(instance_id), std::move(encoded) };
    return notification("client_lightguide_set_leds", parameters);
}

std::vector<std::byte> OdrClientMessages::set_host_owned_instance_state(
        std::string_view instance_id,
        const OdrHostOwnedInstanceState &state) const {
    const auto symbols = registry ? &*registry : nullptr;
    auto model = host_owned_instance_state_value(state, symbols);
    if (!model)
        return {};
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(instance_id), std::move(*model) };
    return notification(
            "client_set_host_owned_instance_client_state", parameters);
}

std::vector<std::byte> OdrClientMessages::set_browser_sound_item(
        std::string_view instance_id, std::int32_t index,
        const OdrBrowserSoundItem &item) const {
    MsgpackValue icon;
    if (item.icon) {
        if (*item.icon != OdrSoundIcon::keyboard)
            return {};
        icon = protocol_symbol("keyboard");
    }
    MsgpackValue asset;
    if (item.asset_image)
        asset = MsgpackValue(MsgpackValue::Binary(
                item.asset_image->begin(), item.asset_image->end()));
    MsgpackValue model(
            MsgpackValue::Map{ { protocol_symbol("name"), item.name },
                    { protocol_symbol("icon"), std::move(icon) },
                    { protocol_symbol("asset_image"), std::move(asset) },
                    { protocol_symbol("is_user_saved"), item.is_user_saved },
                    { protocol_symbol("is_favorite"), item.is_favorite },
                    { protocol_symbol("identifier"), item.identifier } });
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(instance_id), MsgpackValue(std::int64_t(index)),
        std::move(model) };
    return notification("client_browser_set_sound_item", parameters);
}

std::vector<std::byte> OdrClientMessages::set_browser_sound_selection(
        std::string_view instance_id, std::optional<std::int32_t> index) const {
    MsgpackValue selection;
    if (index)
        selection = MsgpackValue(std::int64_t(*index));
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(instance_id), std::move(selection) };
    return notification("client_browser_set_sound_selection", parameters);
}

MsgpackValue OdrClientMessages::browser_sounds_value(
        const OdrBrowserSoundResults &sounds) const {
    MsgpackValue::Array items;
    items.reserve(sounds.items.size());
    for (const auto &item : sounds.items) {
        MsgpackValue icon;
        if (item.icon) {
            if (*item.icon != OdrSoundIcon::keyboard)
                return MsgpackValue{};
            icon = protocol_symbol("keyboard");
        }
        MsgpackValue asset;
        if (item.asset_image)
            asset = MsgpackValue(MsgpackValue::Binary(
                    item.asset_image->begin(), item.asset_image->end()));
        items.emplace_back(MsgpackValue::Map{
                { protocol_symbol("name"), item.name },
                { protocol_symbol("icon"), std::move(icon) },
                { protocol_symbol("asset_image"), std::move(asset) },
                { protocol_symbol("is_user_saved"), item.is_user_saved },
                { protocol_symbol("is_favorite"), item.is_favorite },
                { protocol_symbol("identifier"), item.identifier } });
    }
    MsgpackValue selected;
    if (sounds.selected_index)
        selected = MsgpackValue(std::int64_t(*sounds.selected_index));
    return MsgpackValue(MsgpackValue::Map{
            { protocol_symbol("items"), std::move(items) },
            { protocol_symbol("selected_index"), std::move(selected) } });
}

std::vector<std::byte> OdrClientMessages::set_browser_sounds(
        std::string_view instance_id,
        const OdrBrowserSoundResults &sounds) const {
    MsgpackValue model = browser_sounds_value(sounds);
    if (!std::holds_alternative<MsgpackValue::Map>(model.value))
        return {};
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(instance_id), std::move(model) };
    return notification("client_browser_set_sounds", parameters);
}

std::vector<std::byte> OdrClientMessages::set_browser_settings(
        std::string_view instance_id,
        const OdrBrowserSettings &settings) const {
    // literal keys, notification() does the registry lookup
    MsgpackValue model(MsgpackValue::Map{
            { "prehear_enabled", settings.prehear_enabled },
            { "quickbrowse_enabled", settings.quickbrowse_enabled },
            { "speak_preset_enabled", settings.speak_preset_enabled },
            { "reset_filters_enabled", settings.reset_filters_enabled } });
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(instance_id), std::move(model) };
    return notification("client_browser_set_settings", parameters);
}

MsgpackValue OdrClientMessages::browser_filters_value(
        const OdrBrowserFilters &filters) const {
    const auto named_item = [&](const OdrBrowserNamedItem &item) {
        return MsgpackValue(
                MsgpackValue::Map{ { protocol_symbol("name"), item.name } });
    };
    const auto product_item = [&](const OdrBrowserProductItem &item) {
        MsgpackValue thumbnail;
        if (item.thumbnail_image)
            thumbnail = MsgpackValue(
                    MsgpackValue::Binary(item.thumbnail_image->begin(),
                            item.thumbnail_image->end()));
        MsgpackValue accessible;
        if (item.accessible_name)
            accessible = MsgpackValue(*item.accessible_name);
        return MsgpackValue(MsgpackValue::Map{
                { protocol_symbol("name"), item.name },
                { protocol_symbol("thumbnail_image"), std::move(thumbnail) },
                { protocol_symbol("accessible_name"),
                        std::move(accessible) } });
    };
    const auto single = [&](const auto &filter) {
        MsgpackValue::Array items;
        items.reserve(filter.items.size());
        for (const auto &item : filter.items) {
            using Item = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, OdrBrowserNamedItem>)
                items.push_back(named_item(item));
            else
                items.push_back(product_item(item));
        }
        MsgpackValue selected;
        if (filter.selected_index)
            selected = MsgpackValue(std::int64_t(*filter.selected_index));
        return MsgpackValue(MsgpackValue::Map{
                { protocol_symbol("items"), std::move(items) },
                { protocol_symbol("selected_index"), std::move(selected) } });
    };
    const auto multi = [&](const OdrBrowserMultiSelection &filter) {
        MsgpackValue::Array items;
        items.reserve(filter.items.size());
        for (const auto &item : filter.items)
            items.emplace_back(
                    MsgpackValue::Map{ { protocol_symbol("name"), item.name },
                            { protocol_symbol("selected"), item.selected } });
        MsgpackValue::Array selected;
        selected.reserve(filter.selected_indexes.size());
        for (const auto index : filter.selected_indexes)
            selected.emplace_back(std::int64_t(index));
        return MsgpackValue(MsgpackValue::Map{
                { protocol_symbol("items"), std::move(items) },
                { protocol_symbol("selected_indexes"), std::move(selected) } });
    };
    MsgpackValue::Array filetype_items;
    filetype_items.reserve(filters.file_types.items.size());
    for (const auto &item : filters.file_types.items)
        filetype_items.push_back(named_item(item));
    MsgpackValue carousel(MsgpackValue::Map{
            { protocol_symbol("items"), std::move(filetype_items) },
            { protocol_symbol("selected_index"),
                    MsgpackValue(std::int64_t(
                            filters.file_types.selected_index)) } });
    return MsgpackValue(MsgpackValue::Map{
            { protocol_symbol("brands"), single(filters.brands) },
            { protocol_symbol("banks"), single(filters.banks) },
            { protocol_symbol("subbanks"), single(filters.subbanks) },
            { protocol_symbol("types"), multi(filters.types) },
            { protocol_symbol("subtypes"), multi(filters.subtypes) },
            { protocol_symbol("characters"), multi(filters.characters) },
            { protocol_symbol("products"), single(filters.products) },
            { protocol_symbol("file_types"), std::move(carousel) },
            { protocol_symbol("user_saved"), filters.user_saved },
            { protocol_symbol("favorites"), filters.favorites } });
}

std::vector<std::byte> OdrClientMessages::set_browser_filters(
        std::string_view instance_id, const OdrBrowserFilters &filters) const {
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(instance_id), browser_filters_value(filters) };
    return notification("client_browser_set_filters", parameters);
}

std::vector<std::byte> OdrClientMessages::set_browser_filter(
        std::string_view instance_id, OdrBrowserFilterProperty property,
        const OdrBrowserFilter &filter) const {
    static constexpr std::array properties{ "brands", "banks", "subbanks",
        "types", "subtypes", "characters", "products", "file_types",
        "user_saved", "favorites" };
    OdrBrowserFilters holder;
    MsgpackValue value = std::visit(
            [&](const auto &item) -> MsgpackValue {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, OdrBrowserBrandFilter>) {
                    holder.brands.items = item.items;
                    holder.brands.selected_index = item.selected_index;
                    return std::get<MsgpackValue::Map>(
                            browser_filters_value(holder).value)[0]
                            .second;
                } else if constexpr (std::is_same_v<T, OdrBrowserBankFilter>) {
                    holder.banks.items = item.items;
                    holder.banks.selected_index = item.selected_index;
                    return std::get<MsgpackValue::Map>(
                            browser_filters_value(holder).value)[1]
                            .second;
                } else if constexpr (std::is_same_v<T,
                                             OdrBrowserMultiSelection>) {
                    holder.types = item;
                    return std::get<MsgpackValue::Map>(
                            browser_filters_value(holder).value)[3]
                            .second;
                } else if constexpr (std::is_same_v<T,
                                             OdrBrowserProductFilter>) {
                    holder.products.items = item.items;
                    holder.products.selected_index = item.selected_index;
                    return std::get<MsgpackValue::Map>(
                            browser_filters_value(holder).value)[6]
                            .second;
                } else if constexpr (std::is_same_v<T, OdrBrowserCarousel>) {
                    holder.file_types = item;
                    return std::get<MsgpackValue::Map>(
                            browser_filters_value(holder).value)[7]
                            .second;
                } else {
                    return MsgpackValue(item);
                }
            },
            filter);
    const auto index = static_cast<std::size_t>(property);
    if (index >= properties.size())
        return {};
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(instance_id), protocol_symbol(properties[index]),
        std::move(value) };
    return notification("client_browser_set_filter", parameters);
}

std::vector<std::byte> OdrClientMessages::set_browser_filter_selection(
        std::string_view instance_id, OdrBrowserFilterProperty property,
        const OdrBrowserFilterSelection &selection) const {
    static constexpr std::array properties{ "brands", "banks", "subbanks",
        "types", "subtypes", "characters", "products", "file_types",
        "user_saved", "favorites" };
    MsgpackValue value = std::visit(
            [](const auto &item) -> MsgpackValue {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, std::vector<std::int32_t>>) {
                    MsgpackValue::Array indexes;
                    indexes.reserve(item.size());
                    for (const auto index : item)
                        indexes.emplace_back(std::int64_t(index));
                    return MsgpackValue(std::move(indexes));
                } else if constexpr (std::is_same_v<T,
                                             std::optional<std::int32_t>>) {
                    return item ? MsgpackValue(std::int64_t(*item))
                                : MsgpackValue(nullptr);
                } else if constexpr (std::is_same_v<T, std::int32_t>) {
                    return MsgpackValue(std::int64_t(item));
                } else {
                    return MsgpackValue(item);
                }
            },
            selection);
    const auto index = static_cast<std::size_t>(property);
    if (index >= properties.size())
        return {};
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(instance_id), protocol_symbol(properties[index]),
        std::move(value) };
    return notification("client_browser_set_filter_selection", parameters);
}

std::vector<std::byte> OdrClientMessages::set_browser_data_model(
        std::string_view instance_id, const OdrBrowserData &data) const {
    MsgpackValue sounds = browser_sounds_value(data.sounds);
    if (!std::holds_alternative<MsgpackValue::Map>(sounds.value))
        return {};
    MsgpackValue model(MsgpackValue::Map{
            { protocol_symbol("filters"), browser_filters_value(data.filters) },
            { protocol_symbol("sounds"), std::move(sounds) } });
    const std::array parameters{ MsgpackValue(identity),
        MsgpackValue(instance_id), std::move(model) };
    return notification("client_browser_set_data_model", parameters);
}

std::vector<std::byte> OdrClientMessages::upload_asset(
        const std::array<std::byte, 32> &handle,
        std::span<const std::byte> payload) const {
    // asset uploads have no client identity in their argument tuple
    const std::array parameters{ MsgpackValue(MsgpackValue::Binary(
                                         handle.begin(), handle.end())),
        MsgpackValue(MsgpackValue::Binary(payload.begin(), payload.end())) };
    return notification("add_asset", parameters);
}

} // namespace libnks
