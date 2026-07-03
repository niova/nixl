/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NIOVA Systems, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "backend/backend_plugin.h"
#include "niova_backend.h"

using niova_plugin_t = nixlBackendPluginCreator<nixlNiovaEngine>;

[[nodiscard]] nixl_b_params_t
get_niova_backend_options() {
    nixl_b_params_t params;
    params["target"] = "";
    params["vdev"] = "";
    params["client_uuid"] = "";
    params["queue_depth"] = "128";
    return params;
}

#ifdef STATIC_PLUGIN_NIOVA
nixlBackendPlugin *
createStaticNiovaPlugin() {
    return niova_plugin_t::create(NIXL_PLUGIN_API_VERSION,
                                  "NIOVA",
                                  "0.1.0",
                                  get_niova_backend_options(),
                                  {BLK_SEG, DRAM_SEG});
}
#else
extern "C" NIXL_PLUGIN_EXPORT nixlBackendPlugin *
nixl_plugin_init() {
    return niova_plugin_t::create(NIXL_PLUGIN_API_VERSION,
                                  "NIOVA",
                                  "0.1.0",
                                  get_niova_backend_options(),
                                  {BLK_SEG, DRAM_SEG});
}

extern "C" NIXL_PLUGIN_EXPORT void
nixl_plugin_fini() {}
#endif
