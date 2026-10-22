// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "libnks/assets.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace libnks {
namespace {

constexpr std::array<std::uint32_t, 64> sha256_round_constants{
    0x428a2f98,
    0x71374491,
    0xb5c0fbcf,
    0xe9b5dba5,
    0x3956c25b,
    0x59f111f1,
    0x923f82a4,
    0xab1c5ed5,
    0xd807aa98,
    0x12835b01,
    0x243185be,
    0x550c7dc3,
    0x72be5d74,
    0x80deb1fe,
    0x9bdc06a7,
    0xc19bf174,
    0xe49b69c1,
    0xefbe4786,
    0x0fc19dc6,
    0x240ca1cc,
    0x2de92c6f,
    0x4a7484aa,
    0x5cb0a9dc,
    0x76f988da,
    0x983e5152,
    0xa831c66d,
    0xb00327c8,
    0xbf597fc7,
    0xc6e00bf3,
    0xd5a79147,
    0x06ca6351,
    0x14292967,
    0x27b70a85,
    0x2e1b2138,
    0x4d2c6dfc,
    0x53380d13,
    0x650a7354,
    0x766a0abb,
    0x81c2c92e,
    0x92722c85,
    0xa2bfe8a1,
    0xa81a664b,
    0xc24b8b70,
    0xc76c51a3,
    0xd192e819,
    0xd6990624,
    0xf40e3585,
    0x106aa070,
    0x19a4c116,
    0x1e376c08,
    0x2748774c,
    0x34b0bcb5,
    0x391c0cb3,
    0x4ed8aa4a,
    0x5b9cca4f,
    0x682e6ff3,
    0x748f82ee,
    0x78a5636f,
    0x84c87814,
    0x8cc70208,
    0x90befffa,
    0xa4506ceb,
    0xbef9a3f7,
    0xc67178f2,
};

void sha256_block(
        std::array<std::uint32_t, 8> &state, const std::byte *block) noexcept {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i) {
        words[i] = (std::to_integer<std::uint32_t>(block[i * 4]) << 24) |
                   (std::to_integer<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (std::to_integer<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   std::to_integer<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
        const auto s0 = std::rotr(words[i - 15], 7) ^
                        std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
        const auto s1 = std::rotr(words[i - 2], 17) ^
                        std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = state;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choose = (e & f) ^ (~e & g);
        const auto t1 = h + s1 + choose + sha256_round_constants[i] + words[i];
        const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // namespace

Sha256 sha256(std::span<const std::byte> bytes) noexcept {
    const auto original_size = static_cast<std::uint64_t>(bytes.size());
    std::array<std::uint32_t, 8> state{
        0x6a09e667,
        0xbb67ae85,
        0x3c6ef372,
        0xa54ff53a,
        0x510e527f,
        0x9b05688c,
        0x1f83d9ab,
        0x5be0cd19,
    };
    while (bytes.size() >= 64) {
        sha256_block(state, bytes.data());
        bytes = bytes.subspan(64);
    }

    std::array<std::byte, 128> tail{};
    std::ranges::copy(bytes, tail.begin());
    tail[bytes.size()] = std::byte{ 0x80 };
    const auto padded_size = bytes.size() < 56 ? 64u : 128u;
    const auto bit_length = original_size * 8;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[padded_size - 1 - i] = std::byte((bit_length >> (i * 8)) & 0xff);
    }
    sha256_block(state, tail.data());
    if (padded_size == 128)
        sha256_block(state, tail.data() + 64);

    Sha256 result{};
    for (std::size_t i = 0; i < state.size(); ++i) {
        result[i * 4] = std::byte((state[i] >> 24) & 0xff);
        result[i * 4 + 1] = std::byte((state[i] >> 16) & 0xff);
        result[i * 4 + 2] = std::byte((state[i] >> 8) & 0xff);
        result[i * 4 + 3] = std::byte(state[i] & 0xff);
    }
    return result;
}

KksAssetDeviceModel::KksAssetDeviceModel(
        const KksDeviceHandshakeReply &handshake) {
    reset(handshake);
}

void KksAssetDeviceModel::reset(const KksDeviceHandshakeReply &handshake) {
    available_bytes = handshake.total_asset_space;
    next_asset_id = next_kks_asset_id(handshake);
    entries.clear();
    ids.clear();
    dormant_lru.clear();
    for (const auto &asset : handshake.assets) {
        Entry entry{ asset.device_asset_id, asset.size, asset.checksum, 0,
            true };
        entries.insert_or_assign(asset.checksum, entry);
        ids.insert_or_assign(asset.device_asset_id, asset.checksum);
        dormant_lru.push_back(asset.device_asset_id);
    }
}

KksAssetAcquireResult KksAssetDeviceModel::acquire(
        std::span<const std::byte> payload) {
    return acquire(sha256(payload), payload);
}

KksAssetAcquireResult KksAssetDeviceModel::acquire(
        const Sha256 &checksum, std::span<const std::byte> payload) {
    if (sha256(payload) != checksum)
        return { KksAssetAcquireError::checksum_mismatch, 0, false, {} };

    if (auto found = entries.find(checksum); found != entries.end()) {
        auto &entry = found->second;
        if (entry.dormant) {
            remove_from_dormant(entry.id);
            entry.dormant = false;
        }
        ++entry.references;
        return { KksAssetAcquireError::none, entry.id, true, {} };
    }

    KksAssetAcquireResult result;
    const auto size = static_cast<std::uint64_t>(payload.size());
    while (available_bytes < size && !dormant_lru.empty()) {
        const auto id = dormant_lru.front();
        dormant_lru.pop_front();
        const auto id_entry = ids.find(id);
        if (id_entry == ids.end())
            continue;
        const auto entry = entries.find(id_entry->second);
        if (entry == entries.end() || !entry->second.dormant)
            continue;
        available_bytes += entry->second.size;
        result.outgoing.push_back(encode_kks_asset_remove(id));
        entries.erase(entry);
        ids.erase(id_entry);
    }
    if (available_bytes < size) {
        result.error = KksAssetAcquireError::insufficient_space;
        return result;
    }

    const auto id = next_asset_id++;
    available_bytes -= size;
    entries.emplace(checksum, Entry{ id, size, checksum, 1, false });
    ids.emplace(id, checksum);
    result.device_asset_id = id;
    result.outgoing.push_back(encode_kks_asset_add(id, checksum, payload));
    return result;
}

bool KksAssetDeviceModel::release(std::uint64_t device_asset_id) noexcept {
    const auto id = ids.find(device_asset_id);
    if (id == ids.end())
        return false;
    const auto found = entries.find(id->second);
    if (found == entries.end() || found->second.references == 0)
        return false;
    auto &entry = found->second;
    if (--entry.references == 0) {
        entry.dormant = true;
        dormant_lru.push_back(entry.id);
    }
    return true;
}

std::optional<std::uint64_t> KksAssetDeviceModel::find(
        const Sha256 &checksum) const noexcept {
    const auto found = entries.find(checksum);
    if (found == entries.end())
        return std::nullopt;
    return found->second.id;
}

void KksAssetDeviceModel::remove_from_dormant(std::uint64_t id) {
    const auto found = std::ranges::find(dormant_lru, id);
    if (found != dormant_lru.end())
        dormant_lru.erase(found);
}

} // namespace libnks
