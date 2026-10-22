// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <iostream>
#include <libnks/client.h>

libnks::ClientOptions sampler_options() {
    libnks::ClientOptions options;
    options.mode = libnks::ClientMode::standalone;
    options.instance_info = { "Example Sampler", "1.0.0",
        libnks::OdrInstanceType::standalone, false };
    return options;
}

libnks::ClientResult publish_sampler(libnks::Client &client) {
    libnks::ClientState state;
    state.name = "Example Piano";
    state.preset = "Concert";
    state.identity = "piano-generation-1";
    state.instrument_loaded = true;
    state.pages = { { "Main", { { 1, "Tone", "72%", 0.72, {} } } } };
    state.lightguide.emplace();
    for (unsigned note = 21; note <= 108; ++note)
        (*state.lightguide)[note] = libnks::mk3_led_value(4, false);
    return client.publish(state);
}

int main() {}
