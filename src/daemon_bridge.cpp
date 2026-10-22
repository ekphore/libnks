// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "daemon_files.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <libnks/daemon.h>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace libnks::daemon {
namespace fs = std::filesystem;
using V = libnks::MsgpackValue;
namespace {
std::string utf8(const fs::path &path) {
    const auto s = path.u8string();
    return { reinterpret_cast<const char *>(s.data()), s.size() };
}
std::uint64_t now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
}
std::string token() {
    std::random_device random;
    std::ostringstream out;
    for (int i = 0; i < 4; ++i)
        out << std::hex << std::setw(8) << std::setfill('0')
            << std::uint32_t(random());
    return out.str();
}
bool valid_id(std::string_view s) {
    return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool regular(const fs::path &p) {
    std::error_code ec;
    return fs::symlink_status(p, ec).type() == fs::file_type::regular;
}
bool fresh(const fs::path &p) {
    if (!regular(p))
        return false;
    std::error_code ec;
    const auto time = fs::last_write_time(p, ec);
    return !ec &&
           fs::file_time_type::clock::now() - time < std::chrono::seconds(5);
}
std::optional<V> read(const fs::path &p) {
    if (!regular(p))
        return {};
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec || size > 8 * 1024 * 1024)
        return {};
    std::vector<std::byte> bytes(size);
    std::ifstream in(p, std::ios::binary);
    if (!in.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(size)))
        return {};
    return libnks::decode_msgpack_value(bytes);
}
void write(const fs::path &p, const V &value) {
    const auto bytes = libnks::encode_msgpack_value(value);
    if (bytes.size() > 8 * 1024 * 1024)
        throw std::runtime_error("NKS snapshot exceeds the IPC size limit");
    const auto temporary = p.parent_path() / (token() + ".tmp");
#ifndef _WIN32
    const int parent = ::open(
            p.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (parent < 0)
        throw std::runtime_error(
                "Could not safely open libnks daemon IPC directory");
    struct CloseDirectory {
        int fd;
        ~CloseDirectory() { ::close(fd); }
    } directory{ parent };
#endif
    try {
#ifdef _WIN32
        {
            std::ofstream out(temporary, std::ios::binary);
            out.write(reinterpret_cast<const char *>(bytes.data()),
                    std::streamsize(bytes.size()));
            out.close();
            if (!out)
                throw std::runtime_error("Could not write libnks daemon IPC");
        }
        if (!MoveFileExW(temporary.c_str(), p.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not publish libnks daemon IPC");
#else
        const int fd = ::openat(parent, temporary.filename().c_str(),
                O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
        if (fd < 0)
            throw std::runtime_error("Could not create libnks daemon IPC");
        bool ok = files::owner_like_directory(fd, parent);
        std::size_t offset{};
        while (ok && offset < bytes.size()) {
            const auto count =
                    ::write(fd, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0) {
                ok = false;
                break;
            }
            offset += std::size_t(count);
        }
        if (::close(fd) != 0)
            ok = false;
        if (!ok)
            throw std::runtime_error("Could not write libnks daemon IPC");
        if (::renameat(parent, temporary.filename().c_str(), parent,
                    p.filename().c_str()) != 0)
            throw std::runtime_error("Could not publish libnks daemon IPC");
#endif
    } catch (...) {
#ifdef _WIN32
        std::error_code ec;
        fs::remove(temporary, ec);
#else
        ::unlinkat(parent, temporary.filename().c_str(), 0);
#endif
        throw;
    }
}
const V::Array &array(const V &v) { return std::get<V::Array>(v.value); }
std::string string(const V &v) { return std::get<std::string>(v.value); }
std::uint64_t integer(const V &v) {
    if (const auto *n = std::get_if<std::uint64_t>(&v.value))
        return *n;
    const auto n = std::get<std::int64_t>(v.value);
    if (n < 0)
        throw std::runtime_error("negative integer");
    return n;
}
double number(const V &v) {
    if (const auto *n = std::get_if<double>(&v.value))
        return *n;
    if (const auto *n = std::get_if<float>(&v.value))
        return *n;
    if (const auto *n = std::get_if<std::int64_t>(&v.value))
        return double(*n);
    return double(std::get<std::uint64_t>(v.value));
}
bool trace_enabled() {
    const auto *trace = std::getenv("LIBNKS_TRACE");
    return trace && std::string_view(trace) == "1";
}
} // namespace

fs::path runtime_directory() {
    if (const auto *p = std::getenv("LIBNKS_RUNTIME_DIR"); p && *p)
        return fs::u8path(p);
#ifdef _WIN32
    if (const auto *p = std::getenv("LOCALAPPDATA"); p && *p)
        return fs::u8path(p) / "libnks" / "nksd";
#elif defined(__APPLE__)
    if (const auto user = files::sudo_user())
        return user->home / "Library" / "Application Support" / "libnks" /
               "nksd";
    if (const auto *p = std::getenv("HOME"); p && *p)
        return fs::u8path(p) / "Library" / "Application Support" / "libnks" /
               "nksd";
#else
    if (const auto *p = std::getenv("XDG_RUNTIME_DIR"); p && *p)
        return fs::u8path(p) / "libnks" / "nksd";
    if (const auto *p = std::getenv("XDG_CONFIG_HOME"); p && *p)
        return fs::u8path(p) / "libnks" / "nksd";
    if (const auto user = files::sudo_user())
        return user->home / ".config" / "libnks" / "nksd";
    if (const auto *p = std::getenv("HOME"); p && *p)
        return fs::u8path(p) / ".config" / "libnks" / "nksd";
#endif
    throw std::runtime_error("No user runtime directory available for nksd");
}

V encode(const State &s) {
    V::Array pages, sounds, short_names;
    for (const auto &[name, label] : s.product_short_names)
        short_names.emplace_back(V::Array{ name, label });
    for (const auto &page : s.pages) {
        V::Array parameters;
        for (const auto &p : page.parameters) {
            V::Array values;
            for (const auto &text : p.attributes.values)
                values.emplace_back(text);
            parameters.emplace_back(V::Array{ p.id, p.name, p.value,
                    p.normalized_value, std::uint64_t(p.attributes.type),
                    std::uint64_t(p.attributes.display_style),
                    std::uint64_t(p.attributes.value_count),
                    std::move(values) });
        }
        pages.emplace_back(V::Array{ page.name, std::move(parameters) });
    }
    for (const auto &sound : s.sounds)
        sounds.emplace_back(V::Array{ utf8(sound.path), sound.name,
                sound.library, sound.favorite, sound.preset });
    V leds;
    if (s.lightguide) {
        V::Array values;
        for (auto color : *s.lightguide)
            values.emplace_back(std::uint64_t(color));
        leds = std::move(values);
    }
    V::Array fields{ std::uint64_t(1), s.name, s.preset, s.identity, s.status,
        std::move(pages), std::move(sounds), std::int64_t(s.selected), s.focus,
        s.favorites_only, s.product, std::move(leds), s.instrument_loaded,
        std::move(short_names),
        V::Array{ s.instance_info.name, s.instance_info.version,
                std::uint64_t(s.instance_info.type),
                s.instance_info.is_host } };
    if (!s.artwork_identity.empty())
        fields.emplace_back(s.artwork_identity);
    return fields;
}

std::optional<State> decode(const V &v) {
    try {
        const auto &fields = array(v);
        if ((fields.size() < 9 || fields.size() > 16) ||
                integer(fields[0]) != 1)
            return {};
        State s;
        s.name = string(fields[1]);
        s.preset = string(fields[2]);
        s.identity = string(fields[3]);
        s.status = string(fields[4]);
        if (fields.size() >= 16)
            s.artwork_identity = string(fields[15]);
        if (fields.size() >= 15) {
            const auto &info = array(fields[14]);
            if (info.size() != 4 ||
                    integer(info[2]) > std::uint64_t(OdrInstanceType::aax))
                return {};
            s.instance_info = { string(info[0]), string(info[1]),
                OdrInstanceType(integer(info[2])),
                std::get<bool>(info[3].value) };
        }
        if (fields.size() >= 14) {
            if (array(fields[13]).size() > 50000)
                return {};
            for (const auto &entry : array(fields[13])) {
                const auto &pair = array(entry);
                if (pair.size() != 2)
                    return {};
                s.product_short_names.emplace(string(pair[0]), string(pair[1]));
            }
        }
        std::set<std::uint64_t> ids;
        if (array(fields[5]).size() > 256 || array(fields[6]).size() > 50000)
            return {};
        for (const auto &item : array(fields[5])) {
            const auto &p = array(item);
            if (p.size() != 2)
                return {};
            libnks::Page page;
            page.name = string(p[0]);
            if (array(p[1]).size() > 48)
                return {};
            for (const auto &entry : array(p[1])) {
                const auto &f = array(entry);
                if (f.size() != 8)
                    return {};
                const auto id = integer(f[0]);
                if (id > UINT32_MAX || !ids.insert(id).second)
                    return {};
                libnks::Parameter param;
                param.id = std::uint32_t(id);
                param.name = string(f[1]);
                param.value = string(f[2]);
                param.normalized_value = number(f[3]);
                if (!std::isfinite(param.normalized_value) ||
                        param.normalized_value < 0 ||
                        param.normalized_value > 1 || integer(f[4]) > 255 ||
                        integer(f[5]) > 255 || integer(f[6]) > UINT32_MAX)
                    return {};
                param.attributes.type = std::uint8_t(integer(f[4]));
                param.attributes.display_style = std::uint8_t(integer(f[5]));
                param.attributes.value_count = std::uint32_t(integer(f[6]));
                for (const auto &text : array(f[7]))
                    param.attributes.values.push_back(string(text));
                page.parameters.push_back(std::move(param));
            }
            s.pages.push_back(std::move(page));
        }
        for (const auto &item : array(fields[6])) {
            const auto &f = array(item);
            if (f.size() != 5)
                return {};
            Sound sound{ fs::u8path(string(f[0])), string(f[1]), string(f[2]),
                std::get<bool>(f[3].value), std::get<bool>(f[4].value) };
            if (!sound.path.is_absolute())
                return {};
            s.sounds.push_back(std::move(sound));
        }
        const double selected = number(fields[7]);
        if (!std::isfinite(selected) || selected < -1 ||
                selected >= double(s.sounds.size()) ||
                std::floor(selected) != selected)
            return {};
        s.selected = int(selected);
        s.focus = integer(fields[8]);
        if (fields.size() >= 10)
            s.favorites_only = std::get<bool>(fields[9].value);
        if (fields.size() >= 11)
            s.product = string(fields[10]);
        if (fields.size() >= 12 &&
                !std::holds_alternative<std::monostate>(fields[11].value)) {
            const auto &leds = array(fields[11]);
            if (leds.size() != 128)
                return {};
            s.lightguide.emplace();
            for (std::size_t i = 0; i < 128; ++i) {
                const auto color = integer(leds[i]);
                if (color > 255)
                    return {};
                (*s.lightguide)[i] = std::uint8_t(color);
            }
        }
        s.instrument_loaded = fields.size() >= 13
                                      ? std::get<bool>(fields[12].value)
                                      : !s.preset.empty();
        return s;
    } catch (const std::exception &) { return {}; }
}

std::uint64_t generation(const State &s) {
    // don't hash values or selection, otherwise a late action could hit a new
    // instrument that happens to reuse the same automation IDs
    auto schema = s;
    schema.focus = 0;
    schema.status.clear();
    schema.selected = -1;
    schema.lightguide.reset();
    for (auto &page : schema.pages)
        for (auto &p : page.parameters) {
            p.value.clear();
            p.normalized_value = 0;
        }
    for (auto &sound : schema.sounds)
        sound.favorite = false;
    std::uint64_t h = 14695981039346656037ULL;
    auto wire = encode(schema);
    // v1 senders hash only these fields, before lightguide was added
    std::get<V::Array>(wire.value).resize(11);
    for (const auto byte : libnks::encode_msgpack_value(wire)) {
        h ^= std::to_integer<unsigned char>(byte);
        h *= 1099511628211ULL;
    }
    return h;
}
namespace {
std::uint64_t command_generation(const State &state, const V &action) {
    const auto decoded =
            libnks::decode_kks_plugin_action({ std::string("libnks"), action });
    if (decoded && std::holds_alternative<libnks::OdrBrowserNavigationAction>(
                           *decoded)) {
        // a relative turn has to survive the filter change from the previous
        // one. hash the catalog directly, serializing thousands of sounds on
        // every tick is way too slow
        struct CatalogStamp {
            std::vector<Sound> sounds;
            std::uint64_t hash{};
        };
        thread_local CatalogStamp cached;
        if (cached.hash && cached.sounds.size() == state.sounds.size() &&
                std::equal(cached.sounds.begin(), cached.sounds.end(),
                        state.sounds.begin(),
                        [](const Sound &a, const Sound &b) {
                            return a.path == b.path && a.library == b.library;
                        }))
            return cached.hash;
        std::uint64_t h = 14695981039346656037ULL;
        const auto append = [&](std::string_view text) {
            for (const unsigned char byte : text) {
                h ^= byte;
                h *= 1099511628211ULL;
            }
            h ^= 0xff;
            h *= 1099511628211ULL;
        };
        for (const auto &sound : state.sounds) {
            append(utf8(sound.path));
            append(sound.library);
        }
        cached.sounds = state.sounds;
        cached.hash = h;
        return h;
    }
    if (decoded &&
            std::holds_alternative<libnks::OdrParameterAction>(*decoded)) {
        // encoders only care about the schema, no need to copy the catalog
        State parameters;
        parameters.name = state.name;
        parameters.preset = state.preset;
        parameters.identity = state.identity;
        parameters.pages = state.pages;
        return generation(parameters);
    }
    return generation(state);
}
} // namespace
libnks::KksBrowserData browser_model(const State &s) {
    libnks::KksBrowserData result;
    result.filters.favorites = s.favorites_only;
    std::set<std::string> products;
    for (const auto &sound : s.sounds)
        if (!sound.library.empty())
            products.insert(sound.library);
    for (const auto &product : products) {
        if (product == s.product)
            result.filters.products.selected_index =
                    int(result.filters.products.items.size());
        const auto label = s.product_short_names.find(product);
        result.filters.products.items.push_back(
                { label == s.product_short_names.end() ? product
                                                       : label->second,
                        {}, product });
    }
    for (std::size_t i = 0; i < s.sounds.size(); ++i) {
        const auto &sound = s.sounds[i];
        if (s.favorites_only && !sound.favorite)
            continue;
        if (!s.product.empty() && sound.library != s.product)
            continue;
        if (int(i) == s.selected)
            result.sounds.selected_index = int(result.sounds.items.size());
        result.sounds.items.push_back(
                { sound.name, libnks::OdrSoundIcon::keyboard, {}, sound.preset,
                        sound.favorite, std::int64_t(i) });
    }
    return result;
}

std::optional<fs::path> apply(State &s, const V &action,
        const std::function<bool(std::uint32_t, double)> &setter) {
    const auto decoded =
            libnks::decode_kks_plugin_action({ std::string("libnks"), action });
    if (!decoded)
        return {};
    return apply(s, *decoded, setter);
}

std::optional<fs::path> apply(State &s, const OdrDecodedPluginAction &action,
        const std::function<bool(std::uint32_t, double)> &setter) {
    if (const auto *trigger =
                    std::get_if<libnks::OdrBrowserTriggerAction>(&action)) {
        if (trigger->kind ==
                libnks::OdrBrowserTriggerKind::toggle_filter_favorite_presets)
            s.favorites_only = !s.favorites_only;
        if (trigger->kind == libnks::OdrBrowserTriggerKind::reset_filters) {
            s.favorites_only = false;
            s.product.clear();
        }
        if (s.favorites_only &&
                (s.selected < 0 || std::size_t(s.selected) >= s.sounds.size() ||
                        !s.sounds[s.selected].favorite)) {
            s.selected = -1;
            for (std::size_t i = 0; i < s.sounds.size(); ++i)
                if (s.sounds[i].favorite) {
                    s.selected = int(i);
                    break;
                }
        }
        return {};
    }
    if (const auto *parameter =
                    std::get_if<libnks::OdrParameterAction>(&action)) {
        std::uint32_t id{};
        double value{};
        bool relative{};
        if (const auto *set = std::get_if<libnks::OdrParameterValueSetAction>(
                    parameter)) {
            id = set->parameter_id;
            value = set->value;
        } else if (const auto *change =
                           std::get_if<libnks::OdrParameterValueChangeAction>(
                                   parameter)) {
            id = change->parameter_id;
            value = change->increment;
            relative = true;
        } else
            return {};
        if (!std::isfinite(value))
            return {};
        for (auto &page : s.pages)
            for (auto &p : page.parameters)
                if (p.id == id) {
                    const auto normalized = std::clamp(
                            value + (relative ? p.normalized_value : 0), 0.0,
                            1.0);
                    const auto started = std::chrono::steady_clock::now();
                    const bool accepted = setter && setter(id, normalized);
                    if (trace_enabled())
                        std::fprintf(stderr,
                                "libnks encoder id=%u relative=%d input=%.9g "
                                "before=%.9g requested=%.9g accepted=%d "
                                "apply-ms=%.3f\n",
                                id, relative, value, p.normalized_value,
                                normalized, accepted,
                                std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() -
                                        started)
                                        .count());
                    if (accepted)
                        p.normalized_value = normalized;
                    return {};
                }
    }
    if (s.sounds.empty())
        return {};
    const auto navigate = [&](std::int64_t increment) {
        std::vector<int> visible;
        for (std::size_t i = 0; i < s.sounds.size(); ++i)
            if ((!s.favorites_only || s.sounds[i].favorite) &&
                    (s.product.empty() || s.sounds[i].library == s.product))
                visible.push_back(int(i));
        if (visible.empty()) {
            s.selected = -1;
            return;
        }
        const auto it = std::find(visible.begin(), visible.end(), s.selected);
        const auto index =
                it == visible.end() ? -1 : std::distance(visible.begin(), it);
        s.selected =
                visible[std::size_t(std::clamp(std::int64_t(index) + increment,
                        std::int64_t(0), std::int64_t(visible.size() - 1)))];
    };
    if (const auto *navigation =
                    std::get_if<libnks::OdrBrowserNavigationAction>(&action)) {
        if (navigation->kind == libnks::OdrBrowserNavigationKind::sound)
            navigate(navigation->increment);
        if (navigation->kind == libnks::OdrBrowserNavigationKind::product) {
            // only the names, building the sound list here would copy the
            // whole library on the first turn away from "all products"
            std::set<std::string> unique;
            for (const auto &sound : s.sounds)
                if (!sound.library.empty())
                    unique.insert(sound.library);
            const std::vector<std::string> products(
                    unique.begin(), unique.end());
            const auto selected =
                    std::find(products.begin(), products.end(), s.product);
            const auto prior =
                    selected == products.end()
                            ? -1
                            : std::distance(products.begin(), selected);
            const auto index = std::clamp(
                    std::int64_t(prior) + navigation->increment,
                    std::int64_t(-1), std::int64_t(products.size()) - 1);
            s.product =
                    index < 0 ? std::string{} : products[std::size_t(index)];
            s.selected = -1;
            navigate(1);
        }
    }
    if (const auto *sound =
                    std::get_if<libnks::OdrBrowserSoundAction>(&action)) {
        if (sound->kind == libnks::OdrBrowserSoundActionKind::previous ||
                sound->kind == libnks::OdrBrowserSoundActionKind::next) {
            navigate(sound->kind == libnks::OdrBrowserSoundActionKind::previous
                             ? -1
                             : 1);
            return s.selected < 0
                           ? std::nullopt
                           : std::optional<fs::path>(s.sounds[s.selected].path);
        }
        if (sound->value < 0 || std::uint64_t(sound->value) >= s.sounds.size())
            return {};
        s.selected = int(sound->value);
        if (sound->kind == libnks::OdrBrowserSoundActionKind::load)
            return s.sounds[s.selected].path;
        if (sound->kind == libnks::OdrBrowserSoundActionKind::toggle_favorite)
            s.sounds[s.selected].favorite = !s.sounds[s.selected].favorite;
    }
    return {};
}

struct Client::Impl {
    std::string id = token();
    fs::path root, dir;
    std::uint64_t focus = now();
    std::vector<std::byte> last;
    std::shared_ptr<const ArtworkData> artwork;
    std::shared_ptr<const ArtworkCatalog> assets;
    std::atomic<bool> permission_warning{};
    std::jthread alive_thread;
    explicit Impl(fs::path p) : root(std::move(p)), dir(root / id) {}
};
Client::Client(fs::path root) : impl(std::make_unique<Impl>(std::move(root))) {}
Client::~Client() {
    impl->alive_thread.request_stop();
    if (impl->alive_thread.joinable())
        impl->alive_thread.join();
    // only remove this instance's known files; the runtime directory is shared
    std::error_code ec;
    if (!valid_id(impl->dir.filename().string()) ||
            impl->dir.parent_path() != impl->root)
        return;
    if (fs::symlink_status(impl->dir, ec).type() != fs::file_type::directory)
        return;
    for (const auto &item : fs::directory_iterator(impl->dir, ec)) {
        const auto name = item.path().filename().string();
        const bool mailbox = name.size() == 36 &&
                             valid_id(name.substr(0, 32)) &&
                             (name.ends_with(".cmd") || name.ends_with(".tmp"));
        if ((name == "state.msg" || name == "artwork.msg" ||
                    name == "alive.msg" || mailbox) &&
                regular(item.path()))
            fs::remove(item.path(), ec);
    }
    fs::remove(impl->dir, ec);
}
bool Client::available() const {
    std::error_code error;
    (void)fs::symlink_status(impl->root / "host.msg", error);
    if (error == std::errc::permission_denied &&
            !impl->permission_warning.exchange(true))
        std::fprintf(stderr,
                "libnks: cannot access nksd runtime '%s'. Restart the rebuilt nksd "
                "with sudo to repair a root-owned runtime directory.\n",
                impl->root.string().c_str());
    return fresh(impl->root / "host.msg");
}
const std::string &Client::id() const { return impl->id; }
void Client::focus() { impl->focus = now(); }
void Client::register_instance() {
    fs::create_directory(impl->dir);
    if (fs::symlink_status(impl->dir).type() != fs::file_type::directory)
        throw std::runtime_error(
                "NKS instance directory is not a real directory");
#ifdef _WIN32
    fs::permissions(impl->dir, fs::perms::owner_all, fs::perm_options::replace);
#else
    files::runtime_permissions(impl->dir);
#endif
    if (!impl->alive_thread.joinable()) {
        write(impl->dir / "alive.msg", V(std::uint64_t(1)));
        impl->alive_thread = std::jthread([p = impl.get()](
                                                  std::stop_token stop) {
            std::mutex mutex;
            std::condition_variable_any wake;
            while (!stop.stop_requested()) {
                try {
                    if (fresh(p->root / "host.msg"))
                        write(p->dir / "alive.msg", V(std::uint64_t(1)));
                } catch (...) {}
                std::unique_lock lock(mutex);
                wake.wait_for(lock, stop, std::chrono::seconds(1),
                        [] { return false; });
            }
        });
    }
}
void Client::poll(State &s, const std::function<void(const V &)> &action) {
    if (!available())
        return;
    register_instance();
    s.focus = impl->focus;
    std::error_code ec;
    std::vector<fs::path> commands;
    for (const auto &item : fs::directory_iterator(impl->dir, ec)) {
        const auto name = item.path().filename().string();
        if (name.size() == 36 && name.ends_with(".cmd") &&
                valid_id(name.substr(0, 32)) && regular(item.path()))
            commands.push_back(item.path());
    }
    std::sort(commands.begin(), commands.end());
    for (const auto &path : commands) {
        const auto message = read(path);
        if (!fs::remove(path, ec))
            continue;
        if (!message)
            continue;
        const V *payload{};
        try {
            const auto &f = array(*message);
            if (f.size() != 3)
                continue;
            const auto version = integer(f[0]);
            // v1 envelopes still need the original full-schema check
            if (version != 1 && version != 2)
                continue;
            const auto expected =
                    version == 1 ? generation(s) : command_generation(s, f[2]);
            if (integer(f[1]) != expected) {
                if (trace_enabled())
                    std::fprintf(stderr,
                            "libnks rejected stale action generation=%llu "
                            "expected=%llu\n",
                            static_cast<unsigned long long>(integer(f[1])),
                            static_cast<unsigned long long>(expected));
                continue;
            }
            payload = &f[2];
        } catch (const std::exception &) { continue; }
        const auto *map = std::get_if<V::Map>(&payload->value);
        const auto *name =
                map && map->size() == 1
                        ? std::get_if<std::string>(&(*map)[0].first.value)
                        : nullptr;
        if (trace_enabled())
            std::fprintf(stderr, "libnks [%s] %s\n", impl->id.c_str(),
                    name ? name->c_str() : "invalid action");
        if (name && (*name == "nks_focus" || *name == "ekphore_focus"))
            focus(); // legacy diagnostic envelope
        else
            action(*payload);
    }
    publish(s);
}
void Client::publish(State &s) {
    if (!available())
        return;
    register_instance();
    s.focus = impl->focus;
    if (s.assets && s.assets != impl->assets) {
        auto images = std::make_shared<ArtworkData>();
        for (const auto &[key, bytes] : *s.assets) {
            const auto name =
                    (key.role == ArtworkRole::instrument_banner ? "banner:"
                                                                : "product:") +
                    key.identity;
            images->emplace(name, bytes);
        }
        s.artwork = std::move(images);
        impl->assets = s.assets;
    } else if (s.assets && impl->artwork)
        s.artwork = impl->artwork;
    if (!s.assets)
        impl->assets.reset();
    if (s.artwork != impl->artwork) {
        V::Array images;
        if (s.assets)
            for (const auto &[key, bytes] : *s.assets)
                images.emplace_back(V::Array{
                        std::uint64_t(key.role), key.identity, V(bytes) });
        else if (s.artwork)
            for (const auto &[name, bytes] : *s.artwork)
                images.emplace_back(V::Array{ name, V(bytes) });
        write(impl->dir / "artwork.msg", std::move(images));
        impl->artwork = s.artwork;
    }
    auto encoded = encode(s);
    auto bytes = libnks::encode_msgpack_value(encoded);
    if (bytes != impl->last) {
        write(impl->dir / "state.msg", encoded);
        impl->last = std::move(bytes);
    }
}
void Client::restore_browser(State &s) {
    const auto encoded = libnks::decode_msgpack_value(impl->last);
    const auto prior = encoded ? decode(*encoded) : std::nullopt;
    if (!prior)
        return;
    s.sounds = prior->sounds;
    s.selected = prior->selected;
    s.product = prior->product;
    s.favorites_only = prior->favorites_only;
    s.product_short_names = prior->product_short_names;
}

std::vector<Instance> instances(const fs::path &root) {
    struct Cached {
        fs::file_time_type modified;
        State state;
        fs::file_time_type artwork_time{};
    };
    thread_local fs::path cached_root;
    thread_local std::map<std::string, Cached> cache;
    if (cached_root != root) {
        cache.clear();
        cached_root = root;
    }
    std::set<std::string> alive;
    std::vector<Instance> result;
    std::error_code ec;
    for (const auto &item : fs::directory_iterator(root, ec)) {
        const auto id = item.path().filename().string();
        if (!valid_id(id) ||
                item.symlink_status().type() != fs::file_type::directory ||
                !fresh(item.path() / "alive.msg"))
            continue;
        alive.insert(id);
        const auto modified =
                fs::last_write_time(item.path() / "state.msg", ec);
        if (ec)
            continue;
        auto previous = cache.find(id);
        if (previous == cache.end() || previous->second.modified != modified) {
            const auto value = read(item.path() / "state.msg");
            if (!value)
                continue;
            auto state = decode(*value);
            if (!state)
                continue;
            if (previous != cache.end()) {
                state->artwork = previous->second.state.artwork;
                state->assets = previous->second.state.assets;
                previous->second.modified = modified;
                previous->second.state = std::move(*state);
            } else
                previous =
                        cache.emplace(id, Cached{ modified, std::move(*state) })
                                .first;
        }
        auto &cached = previous->second;
        const auto artwork_time =
                fs::last_write_time(item.path() / "artwork.msg", ec);
        if (!ec && artwork_time != cached.artwork_time) {
            const auto images = read(item.path() / "artwork.msg");
            try {
                if (images) {
                    auto artwork = std::make_shared<ArtworkData>();
                    auto typed = std::make_shared<ArtworkCatalog>();
                    const auto &entries = array(*images);
                    if (entries.size() > 50000)
                        continue;
                    for (const auto &entry : entries) {
                        const auto &pair = array(entry);
                        if (pair.size() == 2)
                            artwork->emplace(string(pair[0]),
                                    std::get<V::Binary>(pair[1].value));
                        else if (pair.size() == 3 &&
                                 integer(pair[0]) <=
                                         std::uint64_t(ArtworkRole::
                                                         instrument_banner)) {
                            ArtworkKey key{ ArtworkRole(integer(pair[0])),
                                string(pair[1]) };
                            const auto &bytes =
                                    std::get<V::Binary>(pair[2].value);
                            artwork->emplace(
                                    (key.role == ArtworkRole::instrument_banner
                                                    ? "banner:"
                                                    : "product:") +
                                            key.identity,
                                    bytes);
                            typed->emplace(std::move(key), bytes);
                        } else
                            throw std::runtime_error("Invalid artwork entry");
                    }
                    if (!cached.state.artwork ||
                            *cached.state.artwork != *artwork)
                        cached.state.artwork = std::move(artwork);
                    if (typed->empty())
                        cached.state.assets.reset();
                    else if (!cached.state.assets ||
                             *cached.state.assets != *typed)
                        cached.state.assets = std::move(typed);
                }
            } catch (const std::exception &) {} // retain last valid assets
            cached.artwork_time = artwork_time;
        }
        result.push_back({ id, cached.state });
    }
    std::erase_if(cache,
            [&](const auto &item) { return !alive.contains(item.first); });
    std::sort(result.begin(), result.end(),
            [](const auto &a, const auto &b) { return a.id < b.id; });
    return result;
}
void send_action(
        const fs::path &root, const Instance &instance, const V &action) {
    if (!valid_id(instance.id) || !fresh(root / instance.id / "alive.msg"))
        throw std::runtime_error("NKS instance is no longer connected");
    // commands are sorted by filename, so prefix the token with the timestamp
    std::ostringstream prefix;
    prefix << std::hex << std::setw(16) << std::setfill('0') << now();
    write(root / instance.id / (prefix.str() + token().substr(16) + ".cmd"),
            V::Array{ std::uint64_t(2),
                    command_generation(instance.state, action), action });
}

// keep the host's filesystem heartbeat out of the public API
void host_heartbeat(const fs::path &root) {
    write(root / "host.msg", V(std::uint64_t(1)));
}
} // namespace libnks::daemon
