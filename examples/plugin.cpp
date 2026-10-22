// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <iostream>
#include <libnks/client.h>

libnks::ClientResult publish_plugin(
        libnks::Client &client, libnks::ClientState &state) {
    state.name = "Example Synth";
    state.preset = "Init Patch";
    state.identity = "example-instrument-generation-1";
    state.instrument_loaded = true;
    state.pages = { { "Oscillator",
            { { 10, "Shape", "25%", 0.25, { 0, 0, 0, {} } },
                    { 11, "Enabled", "On", 1.0,
                            { 2, 3, 0, { "Off", "On" } } } } } };
    return client.publish(state);
}

libnks::ClientResult service_plugin(
        libnks::Client &client, libnks::ClientState &state) {
    return client.poll(state, [&](const libnks::ClientAction &action) {
        const auto load = libnks::apply_action(
                state, action, [](std::uint32_t id, double normalized) {
                    std::cout << "Parameter " << id << " = " << normalized
                              << '\n';
                    return true;
                });
        if (load)
            std::cout << "Load catalog instrument " << *load << '\n';
    });
}

int main() {}
