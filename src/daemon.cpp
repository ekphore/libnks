// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "daemon_files.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <iostream>
#include <libnks/daemon.h>
#include <mutex>
#include <set>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace libnks::daemon {
void host_heartbeat(const std::filesystem::path &root);
int run_standalone(std::stop_token stop, const std::filesystem::path &root,
        const std::function<std::unique_ptr<KksUsbTransport>()> &transport,
        const std::function<std::vector<KksUsbDeviceLocation>()> &enumerate) {
    Client client(root);
    if (client.available() || stop.stop_requested())
        return 0;
    return run_host(stop, root, false, 0, transport, enumerate);
}
namespace {
bool plugin_ready(const State &s) {
    return s.instrument_loaded && s.status != "Busy... Loading";
}
std::string banner_key(const State &state) {
    if (!state.artwork_identity.empty())
        return "banner:" + state.artwork_identity;
    // older clients put the banner identity before the last colon
    const auto separator = state.identity.rfind(':');
    return separator == std::string::npos
                   ? std::string{}
                   : "banner:" + state.identity.substr(0, separator);
}
libnks::OdrHostOwnedInstanceState host_state(const State &s) {
    auto state =
            libnks::make_sampler_host_owned_instance_state(s.preset, "NIKB00");
    if (!plugin_ready(s))
        state.current_preset_name.reset();
    if (!s.status.empty())
        state.status_message = s.status;
    return state;
}
std::optional<libnks::KksParameterPageData> parameter_page(const State &s) {
    if (!plugin_ready(s))
        return libnks::KksParameterPageData{};
    return libnks::make_kks_parameter_page(
            { s.name, s.instance_info.name, s.preset }, s.pages);
}
bool same_pages(const State &a, const State &b, bool values) {
    if (plugin_ready(a) != plugin_ready(b))
        return false;
    if (a.identity != b.identity || a.artwork_identity != b.artwork_identity ||
            a.name != b.name || a.preset != b.preset ||
            a.pages.size() != b.pages.size())
        return false;
    for (std::size_t i = 0; i < a.pages.size(); ++i) {
        const auto &x = a.pages[i];
        const auto &y = b.pages[i];
        if (x.name != y.name || x.parameters.size() != y.parameters.size())
            return false;
        for (std::size_t j = 0; j < x.parameters.size(); ++j) {
            const auto &p = x.parameters[j];
            const auto &q = y.parameters[j];
            if (p.id != q.id || p.name != q.name ||
                    p.attributes.type != q.attributes.type ||
                    p.attributes.display_style != q.attributes.display_style ||
                    p.attributes.value_count != q.attributes.value_count ||
                    p.attributes.values != q.attributes.values)
                return false;
            if (values && (p.value != q.value ||
                                  p.normalized_value != q.normalized_value))
                return false;
        }
    }
    return true;
}
bool same_catalog(const State &a, const State &b) {
    if (a.product_short_names != b.product_short_names ||
            a.sounds.size() != b.sounds.size())
        return false;
    for (std::size_t i = 0; i < a.sounds.size(); ++i) {
        const auto &p = a.sounds[i];
        const auto &q = b.sounds[i];
        if (p.path != q.path || p.name != q.name || p.library != q.library ||
                p.favorite != q.favorite || p.preset != q.preset)
            return false;
    }
    return true;
}
bool same_browser(const State &a, const State &b) {
    return a.product == b.product && a.favorites_only == b.favorites_only &&
           same_catalog(a, b);
}
bool same_state(const State &a, const State &b) {
    return a.instrument_loaded == b.instrument_loaded && a.status == b.status &&
           a.selected == b.selected && a.focus == b.focus &&
           a.lightguide == b.lightguide && a.artwork == b.artwork &&
           same_pages(a, b, true) && same_browser(a, b);
}
bool same_browser_artwork(const State &a, const State &b) {
    const ArtworkData empty;
    const auto &x = a.artwork ? *a.artwork : empty;
    const auto &y = b.artwork ? *b.artwork : empty;
    const auto next = [](auto it, const auto &end) {
        while (it != end && it->first.starts_with("banner:"))
            ++it;
        return it;
    };
    auto i = next(x.begin(), x.end()), j = next(y.begin(), y.end());
    while (i != x.end() && j != y.end()) {
        if (*i != *j)
            return false;
        i = next(++i, x.end());
        j = next(++j, y.end());
    }
    return i == x.end() && j == y.end();
}
std::string utc_clock() {
    const auto time = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char text[32]{};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S+0000", &utc);
    return text;
}
// the kernel releases this lock after a crash, so a stale PID can't keep us out
struct HostLock {
#ifdef _WIN32
    HANDLE handle{ INVALID_HANDLE_VALUE };
    explicit HostLock(const std::filesystem::path &p) {
        handle = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    explicit operator bool() const { return handle != INVALID_HANDLE_VALUE; }
    ~HostLock() {
        if (*this)
            CloseHandle(handle);
    }
#else
    int fd{ -1 };
    explicit HostLock(const std::filesystem::path &p) {
        fd = ::open(p.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
        if (fd >= 0 && !files::owner_like_directory(fd, p.parent_path())) {
            ::close(fd);
            fd = -1;
        }
        if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            fd = -1;
        }
    }
    explicit operator bool() const { return fd >= 0; }
    ~HostLock() {
        if (fd >= 0)
            ::close(fd);
    }
#endif
};
struct Artwork {
    libnks::KksAssetDeviceModel assets;
    std::map<std::string, std::uint64_t> ids;
    void reset(const libnks::KksDeviceHandshakeReply &reply) {
        assets.reset(reply);
        ids.clear();
    }
    void retain(const std::vector<Instance> &clients) {
        std::set<std::string> needed;
        for (const auto &client : clients)
            if (client.state.artwork) {
                const auto banner = plugin_ready(client.state)
                                            ? banner_key(client.state)
                                            : std::string{};
                for (const auto &[name, bytes] : *client.state.artwork) {
                    if (name.starts_with("banner:") && name != banner)
                        continue;
                    const auto checksum = libnks::sha256(bytes);
                    needed.emplace(
                            reinterpret_cast<const char *>(checksum.data()),
                            checksum.size());
                }
            }
        // another client might still use a shared image, so only release it
        // once nobody needs it or it loses its device ID
        for (auto it = ids.begin(); it != ids.end();) {
            if (!needed.contains(it->first)) {
                (void)assets.release(it->second);
                it = ids.erase(it);
            } else
                ++it;
        }
    }
    bool attach_one(libnks::KksHostConnection &connection, const State &state,
            const std::string &key, std::optional<std::uint64_t> &destination) {
        if (!state.artwork)
            return true;
        const auto png = state.artwork->find(key);
        if (png == state.artwork->end() || png->second.empty())
            return true;
        const auto checksum = libnks::sha256(png->second);
        const std::string hash(reinterpret_cast<const char *>(checksum.data()),
                checksum.size());
        auto id = ids.find(hash);
        if (id == ids.end()) {
            const auto acquired = assets.acquire(png->second);
            for (const auto &frame : acquired.outgoing)
                if (!connection.send(frame))
                    return false;
            if (acquired)
                id = ids.emplace(hash, acquired.device_asset_id).first;
        }
        if (id != ids.end())
            destination = id->second;
        return true;
    }
    bool attach_banner(libnks::KksHostConnection &connection,
            const State &state, libnks::KksParameterPagePluginData &model) {
        const auto key = banner_key(state);
        if (!plugin_ready(state) || key.empty())
            return true;
        return attach_one(connection, state, key, model.background_asset_id);
    }
    bool attach(libnks::KksHostConnection &connection, const State &state,
            libnks::KksBrowserData &model) {
        for (auto &product : model.filters.products.items)
            if (!attach_one(connection, state,
                        (state.assets ? "product:" : "") +
                                product.accessible_name.value_or(product.name),
                        product.thumbnail_image))
                return false;
        return true;
    }
};
libnks::LightguideState lightguide(const State &state) {
    if (!state.lightguide)
        return libnks::DeviceOwnedLightguide{};
    std::vector<libnks::LightguideLed> leds;
    leds.reserve(128);
    for (std::size_t note = 0; note < 128; ++note)
        leds.push_back({ std::int8_t(note), (*state.lightguide)[note] });
    return leds;
}
bool publish(libnks::KksHostConnection &connection, const Instance &item,
        bool added, Artwork &artwork, bool select_page = true,
        const State *previous = nullptr, bool force_browser = false) {
    auto instance = connection.instance(item.id);
    if (added && !instance.add(item.state.instance_info))
        return false;
    auto page = parameter_page(item.state);
    if (!page) {
        std::cerr << "NKS model rejected before USB: " << item.state.name
                  << '\n';
        return false;
    }
    if (!artwork.attach_banner(connection, item.state, page->plugin_data))
        return false;
    const auto sent = [&](bool ok, const char *operation) {
        if (!ok)
            std::cerr << "NKS operation failed: " << operation << " for "
                      << item.state.name
                      << " stage=" << int(connection.last_error())
                      << " USB=" << connection.system_error() << '\n';
        return ok;
    };
    const bool browser_changed =
            force_browser || !previous || !same_browser(item.state, *previous);
    libnks::KksBrowserData browser;
    if (browser_changed) {
        browser = browser_model(item.state);
        if (!artwork.attach(connection, item.state, browser))
            return false;
    }
    // plug-in data has to go before the host layout
    if (!sent(instance.set_parameter_page_plugin_data(page->plugin_data),
                "parameter data") ||
            !sent(instance.set_parameter_page_host_viewstate(
                          page->host_owned_viewstate),
                    "parameter layout") ||
            !sent(instance.set_plugin_chain(
                          plugin_ready(item.state)
                                  ? libnks::make_sampler_plugin_chain(
                                            item.state.name)
                                  : libnks::OdrPluginChainData{ { libnks::OdrPluginChainElement{
                                                                        "Empty" } },
                                            0 }),
                    "plugin chain") ||
            (browser_changed &&
                    !sent(instance.set_browser(browser), "browser")) ||
            (!browser_changed && item.state.selected != previous->selected &&
                    !sent(instance.set_browser_sound_selection(
                                  browser_model(item.state)
                                          .sounds.selected_index),
                            "browser selection")) ||
            !sent(instance.set_browser_settings({ false, false, false, true }),
                    "browser settings") ||
            !sent(instance.set_lightguide(lightguide(item.state)),
                    "light guide"))
        return false;
    auto state = host_state(item.state);
    std::cerr << "NKS publishing " << item.id << ": " << item.state.name << ", "
              << item.state.pages.size() << " parameter pages, "
              << page->plugin_data.parameters.size() << " controls, "
              << item.state.sounds.size() << " browser items\n";
    return instance.set_host_owned_state(state) &&
           (!(select_page || !plugin_ready(item.state)) ||
                   instance.set_page(
                           plugin_ready(item.state)
                                   ? libnks::OdrInstancePage::parameter
                                   : libnks::OdrInstancePage::browser)) &&
           instance.flush();
}

bool update(libnks::KksHostConnection &connection, const Instance &item,
        const State &old, Artwork &artwork) {
    if (item.state.artwork != old.artwork ||
            !same_pages(item.state, old, false))
        return publish(connection, item, false, artwork,
                item.state.identity != old.identity, &old,
                !same_browser_artwork(item.state, old));
    auto instance = connection.instance(item.id);
    const auto sent = [&](bool ok, const char *operation) {
        if (!ok)
            std::cerr << "NKS operation failed: " << operation << " for "
                      << item.state.name
                      << " stage=" << int(connection.last_error())
                      << " USB=" << connection.system_error() << '\n';
        return ok;
    };
    if (item.state.lightguide != old.lightguide &&
            !instance.set_lightguide(lightguide(item.state)))
        return false;
    const auto page = parameter_page(item.state);
    if (!page)
        return false;
    for (std::size_t i = 0; i < page->plugin_data.parameters.size(); ++i) {
        const auto &[id, value] = page->plugin_data.parameters[i];
        // only send the value back on an encoder tick, resending the whole
        // page resets it
        const auto newer = std::visit(
                [](const auto &p) -> libnks::OdrParameterValue {
                    return p.parameter_value;
                },
                value);
        // the variants have no operator==, compare the sampler values instead
        const libnks::Parameter *next{};
        const libnks::Parameter *prior{};
        for (const auto &p : item.state.pages)
            for (const auto &v : p.parameters)
                if (v.id == id)
                    next = &v;
        for (const auto &p : old.pages)
            for (const auto &v : p.parameters)
                if (v.id == id)
                    prior = &v;
        if (next && prior &&
                (next->normalized_value != prior->normalized_value ||
                        next->value != prior->value) &&
                !sent(instance.update_parameter_value(id, newer),
                        "parameter feedback"))
            return false;
    }
    if (!same_browser(item.state, old)) {
        auto model = browser_model(item.state);
        if (same_catalog(item.state, old)) {
            // resending the carousel resets navigation on the keyboard, so
            // only send the filters and sounds
            if (!sent(instance.set_browser_filter_selection(
                              libnks::OdrBrowserFilterProperty::products,
                              libnks::KksBrowserFilterSelection{
                                      model.filters.products.selected_index }),
                        "product selection") ||
                    !sent(instance.set_browser_filter_selection(
                                  libnks::OdrBrowserFilterProperty::favorites,
                                  libnks::KksBrowserFilterSelection{
                                          item.state.favorites_only }),
                            "favorites selection") ||
                    !sent(instance.set_browser_sounds(model.sounds),
                            "filtered sounds"))
                return false;
        } else if (!sent(artwork.attach(connection, item.state, model),
                           "browser artwork") ||
                   !sent(instance.set_browser(model), "browser refresh"))
            return false;
    } else if (item.state.selected != old.selected &&
               !instance.set_browser_sound_selection(
                       browser_model(item.state).sounds.selected_index))
        return false;
    if (item.state.status != old.status) {
        auto state = host_state(item.state);
        if (!instance.set_host_owned_state(state))
            return false;
    }
    return sent(instance.flush(), "feedback flush");
}
} // namespace
int run_host(std::stop_token stop, const std::filesystem::path &root,
        bool virtual_device, std::size_t device_index,
        const std::function<std::unique_ptr<libnks::KksUsbTransport>()>
                &transport,
        const std::function<std::vector<libnks::KksUsbDeviceLocation>()>
                &enumerate) {
    namespace fs = std::filesystem;
    try {
        if (!virtual_device && !transport &&
                !libnks::KksLibusbDevice::available())
            throw std::runtime_error("This build has no libusb hardware "
                                     "backend; use --virtual for simulation");
        fs::create_directories(root);
        if (fs::symlink_status(root).type() != fs::file_type::directory)
            throw std::runtime_error("NKS runtime must be a real directory");
#ifndef _WIN32
        files::runtime_permissions(root);
#endif
        HostLock lock(root / "host.lock");
        if (!lock) {
            std::cerr << "An nksd owner is already running.\n";
            return 2;
        }
        // USB can block on a large library transfer, so keep the IPC heartbeat
        // on its own thread while we're loading a sound
        std::jthread keepalive([&](std::stop_token exit) {
            std::mutex mutex;
            std::condition_variable_any wake;
            while (!exit.stop_requested()) {
                try {
                    host_heartbeat(root);
                } catch (const std::exception &e) {
                    std::cerr << "NKS heartbeat: " << e.what() << '\n';
                }
                std::unique_lock guard(mutex);
                wake.wait_for(guard, exit, std::chrono::seconds(1),
                        [] { return false; });
            }
        });
        std::unique_ptr<libnks::KksHostConnection> connection;
        auto next_connect = std::chrono::steady_clock::time_point{};
        auto heartbeat = std::chrono::steady_clock::now();
        std::map<std::string, State> published;
        Artwork artwork;
        std::string focused;
        std::set<std::string> known_clients;
        const auto started = std::chrono::steady_clock::now();
        bool registration_reported = false;
        std::cout << "nksd "
                  << (virtual_device ? "virtual device" : "hardware owner")
                  << " ready: " << root << std::endl;
        while (!stop.stop_requested()) {
            const auto time = std::chrono::steady_clock::now();
            if (!virtual_device && !connection && time >= next_connect) {
                next_connect = time + std::chrono::seconds(2);
                const auto devices =
                        enumerate ? enumerate()
                                  : libnks::KksLibusbDevice::enumerate();
                if (device_index < devices.size()) {
                    libnks::KksHostHandshake hello;
                    hello.utc_clock = utc_clock();
                    auto candidate =
                            transport
                                    ? std::make_unique<
                                              libnks::KksHostConnection>(
                                              hello, transport())
                                    : std::make_unique<
                                              libnks::KksHostConnection>(hello);
                    if (candidate->connect(devices[device_index])) {
                        connection = std::move(candidate);
                        published.clear();
                        focused.clear();
                        heartbeat = time;
                    } else {
                        const auto error = candidate->system_error();
                        std::cerr
                                << "NKS keyboard connection failed during "
                                << (candidate->last_error() ==
                                                           libnks::KksHostConnectionError::
                                                                   open
                                                   ? candidate
                                                             ->transport_error_stage()
                                                   : "handshake send")
                                << " (USB error " << error << "). ";
                        if (const auto detail =
                                        candidate->transport_error_detail();
                                !detail.empty())
                            std::cerr << detail << ". ";
                        if (error == -6)
                            std::cerr << "The USB interface is busy; another "
                                         "application may own it. ";
                        else if (error == -3)
                            std::cerr << "USB access was denied by the "
                                         "operating system. ";
                        else if (error == -4)
                            std::cerr << "The keyboard disconnected. ";
#if defined(__APPLE__)
                        if (error == -6 || error == -3)
                            std::cerr << "If NI Hardware Connection Service is "
                                         "running, temporarily quit/disable it "
                                         "before using nksd; it can "
                                         "exclusively own the keyboard. nksd "
                                         "will not stop it automatically. ";
#elif defined(_WIN32)
                        if (error == -6 || error == -3)
                            std::cerr
                                    << "If NI Hardware Connection Service is "
                                       "running, temporarily stop/disable it in "
                                       "Windows Services before using nksd; it can "
                                       "exclusively own the keyboard. nksd will "
                                       "not stop it automatically. ";
#endif
                        std::cerr << "Retrying.\n";
                    }
                }
            }
            const auto clients = instances(root);
            std::set<std::string> current_clients;
            for (const auto &client : clients) {
                current_clients.insert(client.id);
                if (!known_clients.contains(client.id))
                    std::cerr << "nksd client connected: "
                              << client.state.instance_info.name << " ("
                              << client.id << "), " << client.state.pages.size()
                              << " parameter pages, "
                              << client.state.sounds.size()
                              << " browser items\n";
            }
            known_clients = std::move(current_clients);
            if (!clients.empty())
                registration_reported = true;
            if (!registration_reported &&
                    time - started > std::chrono::seconds(5)) {
                std::cerr
                        << "nksd: no sampler/plug-in clients registered yet. Open "
                           "the rebuilt plug-in and ensure it uses runtime "
                        << root << ".\n";
                registration_reported = true;
            }
            if (connection) {
                // drain a few inputs before the catalog, otherwise fast encoder
                // turns wait behind it. NOT timeout=0, libusb treats that as
                // wait forever
                const auto input_started = std::chrono::steady_clock::now();
                for (unsigned input = 0; connection && input < 32; ++input) {
                    const auto received = connection->receive(input ? 1 : 5);
                    if (!received &&
                            connection->last_error() !=
                                    libnks::KksHostConnectionError::timeout &&
                            connection->last_error() !=
                                    libnks::KksHostConnectionError::protocol) {
                        std::cerr << "NKS receive failed: stage="
                                  << int(connection->last_error())
                                  << " USB=" << connection->system_error();
                        if (const auto detail =
                                        connection->transport_error_detail();
                                !detail.empty())
                            std::cerr << " " << detail;
                        std::cerr << ". Reconnecting.\n";
                        connection.reset();
                        published.clear();
                        focused.clear();
                        continue;
                    }
                    if (received) {
                        if (const auto *reply = std::get_if<
                                    libnks::KksDeviceHandshakeReply>(
                                    &received->payload)) {
                            std::cerr << "NKS keyboard handshake complete\n";
                            artwork.reset(*reply);
                            published.clear();
                            focused.clear();
                        }
                        if (const auto *action =
                                        std::get_if<libnks::KksPluginAction>(
                                                &received->payload)) {
                            if (const auto *id = std::get_if<std::string>(
                                        &action->client)) {
                                const auto item = std::find_if(clients.begin(),
                                        clients.end(), [&](const auto &c) {
                                            return c.id == *id;
                                        });
                                if (item != clients.end()) {
                                    // page changes come from the keyboard,
                                    // don't treat them as automation or a load
                                    const auto decoded =
                                            libnks::decode_kks_plugin_action(
                                                    *action);
                                    const auto *p =
                                            decoded ? std::get_if<
                                                              libnks::OdrParameterAction>(
                                                              &*decoded)
                                                    : nullptr;
                                    const auto *address =
                                            p ? std::get_if<
                                                        libnks::OdrParameterPageAddressAction>(
                                                        p)
                                              : nullptr;
                                    if (address && plugin_ready(item->state)) {
                                        const auto page =
                                                libnks::make_kks_parameter_page(
                                                        { item->state.name,
                                                                item->state
                                                                        .instance_info
                                                                        .name,
                                                                item->state
                                                                        .preset },
                                                        item->state.pages);
                                        if (page)
                                            (void)connection->instance(*id)
                                                    .set_parameter_page_device_viewstate(
                                                            { address->address });
                                    } else
                                        send_action(
                                                root, *item, action->action);
                                }
                            }
                        }
                    }
                    if (!received ||
                            std::chrono::steady_clock::now() - input_started >=
                                    std::chrono::milliseconds(5))
                        break;
                }
                if (!connection)
                    continue;
                if (connection->session().state() ==
                        libnks::KksHostSessionState::ready) {
                    const bool assets_changed =
                            clients.size() != published.size() ||
                            std::any_of(clients.begin(), clients.end(),
                                    [&](const auto &client) {
                                        const auto old =
                                                published.find(client.id);
                                        return old == published.end() ||
                                               old->second.artwork !=
                                                       client.state.artwork ||
                                               old->second.identity !=
                                                       client.state.identity ||
                                               plugin_ready(old->second) !=
                                                       plugin_ready(
                                                               client.state);
                                    });
                    if (assets_changed)
                        artwork.retain(clients);
                    for (auto old = published.begin();
                            old != published.end();) {
                        if (std::none_of(clients.begin(), clients.end(),
                                    [&](const auto &c) {
                                        return c.id == old->first;
                                    })) {
                            (void)connection->instance(old->first).remove();
                            old = published.erase(old);
                        } else
                            ++old;
                    }
                    for (const auto &client : clients) {
                        const auto old = published.find(client.id);
                        const bool added = old == published.end();
                        if (added || !same_state(old->second, client.state)) {
                            if (!(added ? publish(*connection, client, true,
                                                  artwork)
                                        : update(*connection, client,
                                                  old->second, artwork))) {
                                std::cerr
                                        << "NKS publication failed for "
                                        << client.state.name << ": stage="
                                        << int(connection->last_error())
                                        << " USB=" << connection->system_error()
                                        << '\n';
                                connection.reset();
                                break;
                            }
                            published[client.id] = client.state;
                        }
                    }
                    if (connection && !clients.empty()) {
                        const auto selected = std::max_element(clients.begin(),
                                clients.end(),
                                [](const auto &a, const auto &b) {
                                    return a.state.focus < b.state.focus;
                                });
                        if (focused != selected->id) {
                            focused = selected->id;
                            (void)connection->instance(focused).acquire_focus();
                        }
                    }
                    if (connection &&
                            time - heartbeat >= std::chrono::seconds(8)) {
                        if (!connection->heartbeat())
                            connection.reset();
                        heartbeat = time;
                    }
                    if (connection && !connection->flush())
                        connection.reset();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (connection)
            connection->shutdown();
        keepalive.request_stop();
        keepalive.join();
        std::error_code ec;
        fs::remove(root / "host.msg", ec);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "nksd: " << e.what() << '\n';
        return 1;
    }
}
} // namespace libnks::daemon
