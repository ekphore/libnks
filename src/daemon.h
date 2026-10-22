// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <libnks/assets.h>
#include <libnks/session.h>
#include <map>
#include <memory>
#include <stop_token>

namespace libnks::daemon {
// our own IPC, this doesn't talk to NI Hardware Connection Service
struct Sound {
    std::filesystem::path path;
    std::string name, library;
    bool favorite{}, preset{};
};
using ArtworkData = std::map<std::string, std::vector<std::byte>>;
struct State {
    std::string name{ "NKS" }, preset, identity, status;
    std::vector<libnks::Page> pages;
    std::vector<Sound> sounds;
    int selected{ -1 };
    std::uint64_t focus{};
    bool favorites_only{};
    std::string
            product; // empty selects all products; names come from the catalog
    // device LED bytes (palette << 2 | pressed << 1), in note order 0..127;
    // there's no lightguide while browsing without an instrument
    std::optional<std::array<std::uint8_t, 128>> lightguide;
    bool instrument_loaded{};
    std::map<std::string, std::string> product_short_names;
    std::shared_ptr<const ArtworkData>
            artwork; // product names; banner:<identity path>
    OdrInstanceInfo instance_info{ "libnks", "0.1.0", OdrInstanceType::vst3,
        false };
    std::shared_ptr<const ArtworkCatalog>
            assets;               // preferred typed artwork input
    std::string artwork_identity; // explicit instrument banner identity
};
std::filesystem::path runtime_directory();
std::uint64_t generation(const State &state);
libnks::MsgpackValue encode(const State &state);
std::optional<State> decode(const libnks::MsgpackValue &value);
libnks::KksBrowserData browser_model(const State &state);
// only returns paths from the catalog we sent
std::optional<std::filesystem::path> apply(State &state,
        const libnks::MsgpackValue &action,
        const std::function<bool(std::uint32_t, double)> &set_parameter = {});
std::optional<std::filesystem::path> apply(State &state,
        const OdrDecodedPluginAction &action,
        const std::function<bool(std::uint32_t, double)> &set_parameter = {});

class Client {
  public:
    explicit Client(std::filesystem::path root = runtime_directory());
    virtual ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    bool available() const;
    // NOT on the audio thread. actions for another instrument or catalog get
    // dropped, relative navigation only checks the catalog so it still works
    // right after a filter change
    void poll(State &state,
            const std::function<void(const libnks::MsgpackValue &)> &action);
    void publish(State &state); // e.g. loading status before a long preparation
    void restore_browser(State &state); // retain catalog/filter/selection
                                        // across standalone handoff
    void focus();
    const std::string &id() const;

  private:
    void register_instance();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct Instance {
    std::string id;
    State state;
};
std::vector<Instance> instances(const std::filesystem::path &root);
void send_action(const std::filesystem::path &root, const Instance &instance,
        const libnks::MsgpackValue &action);
// only one process gets the runtime directory lock. virtual mode doesn't touch
// USB, returns nonzero on failure
int run_host(std::stop_token stop, const std::filesystem::path &root,
        bool virtual_device, std::size_t device_index = 0,
        const std::function<std::unique_ptr<libnks::KksUsbTransport>()>
                &transport = {},
        const std::function<std::vector<libnks::KksUsbDeviceLocation>()>
                &enumerate = {});
// tries nksd first and opens USB itself if it isn't there. plug-ins have to
// use Client, otherwise they'd all fight over the same USB interface
int run_standalone(std::stop_token stop, const std::filesystem::path &root,
        const std::function<std::unique_ptr<KksUsbTransport>()> &transport = {},
        const std::function<std::vector<KksUsbDeviceLocation>()>
                &enumerate = {});
} // namespace libnks::daemon
