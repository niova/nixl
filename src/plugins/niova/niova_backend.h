/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NIOVA Systems, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NIXL_SRC_PLUGINS_NIOVA_NIOVA_BACKEND_H
#define NIXL_SRC_PLUGINS_NIOVA_NIOVA_BACKEND_H

#include "backend/backend_engine.h"

#include <memory>
#include <mutex>
#include <string>

typedef void niova_block_client_t;

class nixlNiovaEngine : public nixlBackendEngine {
public:
    explicit nixlNiovaEngine(const nixlBackendInitParams *init_params);
    ~nixlNiovaEngine() override;

    bool
    supportsNotif() const override {
        return false;
    }

    bool
    supportsRemote() const override {
        return false;
    }

    bool
    supportsLocal() const override {
        return true;
    }

    nixl_mem_list_t
    getSupportedMems() const override {
        return {BLK_SEG, DRAM_SEG};
    }

    nixl_status_t
    connect(const std::string &remote_agent) override {
        (void)remote_agent;
        return NIXL_SUCCESS;
    }

    nixl_status_t
    disconnect(const std::string &remote_agent) override {
        (void)remote_agent;
        return NIXL_SUCCESS;
    }

    nixl_status_t
    loadLocalMD(nixlBackendMD *input, nixlBackendMD *&output) override {
        output = input;
        return NIXL_SUCCESS;
    }

    nixl_status_t
    unloadMD(nixlBackendMD *input) override {
        (void)input;
        return NIXL_SUCCESS;
    }

    nixl_status_t
    registerMem(const nixlBlobDesc &mem,
                const nixl_mem_t &nixl_mem,
                nixlBackendMD *&out) override;

    nixl_status_t
    deregisterMem(nixlBackendMD *meta) override;

    nixl_status_t
    prepXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &remote_agent,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *opt_args = nullptr) const override;

    nixl_status_t
    postXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &remote_agent,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *opt_args = nullptr) const override;

    nixl_status_t
    checkXfer(nixlBackendReqH *handle) const override;

    nixl_status_t
    releaseReqH(nixlBackendReqH *handle) const override;

private:
    struct Defaults {
        std::string target;
        std::string vdev;
        std::string client_uuid;
        unsigned queue_depth = 128;
    };

    Defaults defaults_;
};

#endif
