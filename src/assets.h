// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "libnks/wire.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace libnks {

using Sha256 = std::array<std::byte, 32>;

enum class ArtworkRole : std::uint8_t { product_thumbnail, instrument_banner };
struct ArtworkKey {
    ArtworkRole role{ ArtworkRole::product_thumbnail };
    std::string identity;
    auto operator<=>(const ArtworkKey &) const = default;
};
// these are your keys, so don't put device asset handles in there or encode
// the role in a string prefix
using ArtworkCatalog = std::map<ArtworkKey, std::vector<std::byte>>;

[[nodiscard]] Sha256 sha256(std::span<const std::byte> bytes) noexcept;

enum class KksAssetAcquireError : std::uint8_t {
    none,
    checksum_mismatch,
    insufficient_space,
};

struct KksAssetAcquireResult {
    KksAssetAcquireError error{ KksAssetAcquireError::none };
    std::uint64_t device_asset_id{};
    bool reused{};
    // send evictions before the upload, even if acquire reports
    // insufficient_space: we may have evicted dormant entries before running
    // out of candidates
    std::vector<KksRoutedFrame> outgoing;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == KksAssetAcquireError::none;
    }
};

// handshake assets start dormant and acquire() reuses them by checksum,
// dropping the last reference only makes an asset evictable, it stays there
class KksAssetDeviceModel {
  public:
    KksAssetDeviceModel() = default;
    explicit KksAssetDeviceModel(const KksDeviceHandshakeReply &handshake);

    void reset(const KksDeviceHandshakeReply &handshake);
    [[nodiscard]] KksAssetAcquireResult acquire(
            std::span<const std::byte> payload);
    [[nodiscard]] KksAssetAcquireResult acquire(
            const Sha256 &checksum, std::span<const std::byte> payload);
    [[nodiscard]] bool release(std::uint64_t device_asset_id) noexcept;

    [[nodiscard]] std::optional<std::uint64_t> find(
            const Sha256 &checksum) const noexcept;
    [[nodiscard]] std::uint64_t free_space() const noexcept {
        return available_bytes;
    }
    [[nodiscard]] std::uint64_t next_device_asset_id() const noexcept {
        return next_asset_id;
    }

  private:
    struct Entry {
        std::uint64_t id{};
        std::uint64_t size{};
        Sha256 checksum{};
        std::uint64_t references{};
        bool dormant{};
    };

    void remove_from_dormant(std::uint64_t id);

    std::uint64_t available_bytes{};
    std::uint64_t next_asset_id{};
    std::map<Sha256, Entry> entries;
    std::map<std::uint64_t, Sha256> ids;
    std::deque<std::uint64_t> dormant_lru;
};

} // namespace libnks
