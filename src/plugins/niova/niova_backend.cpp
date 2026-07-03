/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NIOVA Systems, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "niova_backend.h"

#include "common/nixl_log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <sys/uio.h>
#include <unordered_map>
#include <uuid/uuid.h>
#include <vector>

namespace {

constexpr size_t NIOVA_VDEV_NAME_MAX_LEN = 128;
constexpr unsigned NIOVA_BLOCK_FLAGS_USE_EXTRA_OPTS = 1U << 31;

extern "C" {

struct niova_block_client_opts {
    uuid_t client_uuid;
    uuid_t vdev_uuid;
    char vdev_name[NIOVA_VDEV_NAME_MAX_LEN];
    uuid_t target_uuid;
    char net_target_addr[16]; /* INET_ADDRSTRLEN */
    unsigned int net_target_port;
    unsigned int queue_depth;
    unsigned int flags;
    bool nbco_concurrent_trace_emit;
};

int
NiovaBlockClientReadv(niova_block_client_t *nbc,
                      uint64_t start_blk,
                      struct iovec *iovs,
                      size_t niovs,
                      void (*cb)(void *, ssize_t),
                      void *arg);

int
NiovaBlockClientWritev(niova_block_client_t *nbc,
                       uint64_t start_blk,
                       struct iovec *iovs,
                       size_t niovs,
                       void (*cb)(void *, ssize_t),
                       void *arg);

int
NiovaBlockClientDestroy(niova_block_client_t *nbc);

int
NiovaBlockClientNew(niova_block_client_t **ret_nbc,
                    const struct niova_block_client_opts *opts);

int
niova_block_client_sector_size(const niova_block_client_t *nbc);

ssize_t
niova_block_client_vdev_size(const niova_block_client_t *nbc);

int
niova_block_client_parse_target_opt_string(const char *opt_str,
                                           struct niova_block_client_opts *nbco);
}

struct NiovaClient {
    niova_block_client_t *client = nullptr;
    int sector_size = 0;
    ssize_t vdev_size = 0;

    ~NiovaClient() {
        if (client) {
            NiovaBlockClientDestroy(client);
        }
    }
};

class NiovaMD : public nixlBackendMD {
public:
    nixl_mem_t mem_type;
    uint64_t dev_id;
    uintptr_t addr;
    size_t len;
    std::shared_ptr<NiovaClient> client;

    NiovaMD(const nixlBlobDesc &mem, nixl_mem_t type)
        : nixlBackendMD(true),
          mem_type(type),
          dev_id(mem.devId),
          addr(mem.addr),
          len(mem.len) {}
};

struct NiovaXferDesc {
    std::shared_ptr<NiovaClient> client;
    uint64_t start_blk = 0;
    struct iovec iov = {};
};

class NiovaReq : public nixlBackendReqH {
public:
    explicit NiovaReq(nixl_xfer_op_t op_in) : op(op_in) {}

    nixl_xfer_op_t op;
    std::vector<NiovaXferDesc> descs;
    std::atomic<unsigned> pending{0};
    std::atomic<bool> posted{false};
    mutable std::mutex mutex;
    nixl_status_t status = NIXL_ERR_NOT_POSTED;
};

std::string
trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::unordered_map<std::string, std::string>
parseParams(const std::string &input) {
    std::unordered_map<std::string, std::string> out;
    std::string normalized = input;
    std::replace(normalized.begin(), normalized.end(), ';', ',');

    std::stringstream ss(normalized);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (item.empty()) {
            continue;
        }

        size_t eq = item.find('=');
        if (eq == std::string::npos) {
            out["vdev"] = item;
            continue;
        }

        std::string key = trim(item.substr(0, eq));
        std::string val = trim(item.substr(eq + 1));
        if (!key.empty()) {
            out[key] = val;
        }
    }

    return out;
}

std::string
paramOrDefault(const std::unordered_map<std::string, std::string> &params,
               const char *key,
               const std::string &fallback) {
    auto it = params.find(key);
    return it == params.end() ? fallback : it->second;
}

bool
isValidVdevName(const std::string &name) {
    if (name.empty() || name.size() >= NIOVA_VDEV_NAME_MAX_LEN) {
        return false;
    }
    for (unsigned char c : name) {
        if (!std::isalnum(c)) {
            return false;
        }
    }
    return true;
}

nixl_status_t
setVdev(struct niova_block_client_opts &opts, const std::string &vdev) {
    if (vdev.empty()) {
        NIXL_ERROR << "NIOVA: missing vdev parameter for BLK_SEG registration";
        return NIXL_ERR_INVALID_PARAM;
    }

    if (uuid_parse(vdev.c_str(), opts.vdev_uuid) == 0) {
        opts.vdev_name[0] = '\0';
        return NIXL_SUCCESS;
    }

    uuid_clear(opts.vdev_uuid);
    if (!isValidVdevName(vdev)) {
        NIXL_ERROR << "NIOVA: vdev must be a UUID or alphanumeric name";
        return NIXL_ERR_INVALID_PARAM;
    }

    std::strncpy(opts.vdev_name, vdev.c_str(), sizeof(opts.vdev_name) - 1);
    opts.vdev_name[sizeof(opts.vdev_name) - 1] = '\0';
    return NIXL_SUCCESS;
}

void
niovaXferCb(void *arg, ssize_t rc) {
    auto *req = static_cast<NiovaReq *>(arg);
    bool last = req->pending.fetch_sub(1, std::memory_order_acq_rel) == 1;

    if (rc < 0) {
        std::lock_guard<std::mutex> lock(req->mutex);
        req->status = NIXL_ERR_BACKEND;
    }

    if (last) {
        std::lock_guard<std::mutex> lock(req->mutex);
        if (req->status == NIXL_IN_PROG) {
            req->status = NIXL_SUCCESS;
        }
    }
}

nixl_status_t
validateRequest(const nixl_xfer_op_t &op,
                const nixl_meta_dlist_t &local,
                const nixl_meta_dlist_t &remote) {
    if ((op != NIXL_READ) && (op != NIXL_WRITE)) {
        return NIXL_ERR_INVALID_PARAM;
    }
    if (local.getType() != DRAM_SEG) {
        NIXL_ERROR << "NIOVA: local memory type must be DRAM_SEG";
        return NIXL_ERR_INVALID_PARAM;
    }
    if (remote.getType() != BLK_SEG) {
        NIXL_ERROR << "NIOVA: remote memory type must be BLK_SEG";
        return NIXL_ERR_INVALID_PARAM;
    }
    if (local.descCount() != remote.descCount()) {
        NIXL_ERROR << "NIOVA: local and remote descriptor counts differ";
        return NIXL_ERR_INVALID_PARAM;
    }
    return NIXL_SUCCESS;
}

} // namespace

nixlNiovaEngine::nixlNiovaEngine(const nixlBackendInitParams *init_params)
    : nixlBackendEngine(init_params) {
    if (init_params && init_params->customParams) {
        const nixl_b_params_t *params = init_params->customParams;
        if (params->count("target")) {
            defaults_.target = params->at("target");
        }
        if (params->count("vdev")) {
            defaults_.vdev = params->at("vdev");
        }
        if (params->count("client_uuid")) {
            defaults_.client_uuid = params->at("client_uuid");
        }
        if (params->count("queue_depth")) {
            try {
                defaults_.queue_depth =
                    static_cast<unsigned>(std::stoul(params->at("queue_depth")));
            }
            catch (const std::exception &) {
                NIXL_ERROR << "NIOVA: invalid queue_depth backend parameter";
                initErr = true;
            }
        }
    }
}

nixlNiovaEngine::~nixlNiovaEngine() = default;

nixl_status_t
nixlNiovaEngine::registerMem(const nixlBlobDesc &mem,
                             const nixl_mem_t &nixl_mem,
                             nixlBackendMD *&out) {
    out = nullptr;
    if ((nixl_mem != DRAM_SEG) && (nixl_mem != BLK_SEG)) {
        return NIXL_ERR_NOT_SUPPORTED;
    }

    auto md = std::make_unique<NiovaMD>(mem, nixl_mem);
    if (nixl_mem == DRAM_SEG) {
        out = md.release();
        return NIXL_SUCCESS;
    }

    auto params = parseParams(mem.metaInfo);
    const std::string target = paramOrDefault(params, "target", defaults_.target);
    const std::string vdev = paramOrDefault(params, "vdev", defaults_.vdev);
    const std::string client_uuid =
        paramOrDefault(params, "client_uuid", defaults_.client_uuid);
    unsigned queue_depth = defaults_.queue_depth;
    if (auto it = params.find("queue_depth"); it != params.end()) {
        try {
            queue_depth = static_cast<unsigned>(std::stoul(it->second));
        }
        catch (const std::exception &) {
            NIXL_ERROR << "NIOVA: invalid queue_depth metadata parameter";
            return NIXL_ERR_INVALID_PARAM;
        }
    }

    if (target.empty()) {
        NIXL_ERROR << "NIOVA: missing target parameter for BLK_SEG registration";
        return NIXL_ERR_INVALID_PARAM;
    }

    struct niova_block_client_opts opts = {};
    int rc = niova_block_client_parse_target_opt_string(target.c_str(), &opts);
    if (rc) {
        NIXL_ERROR << "NIOVA: failed to parse target parameter";
        return NIXL_ERR_INVALID_PARAM;
    }

    if (setVdev(opts, vdev) != NIXL_SUCCESS) {
        return NIXL_ERR_INVALID_PARAM;
    }

    if (!client_uuid.empty()) {
        if (uuid_parse(client_uuid.c_str(), opts.client_uuid) != 0) {
            NIXL_ERROR << "NIOVA: invalid client_uuid parameter";
            return NIXL_ERR_INVALID_PARAM;
        }
    } else {
        uuid_generate(opts.client_uuid);
    }

    opts.queue_depth = queue_depth;
    opts.nbco_concurrent_trace_emit = true;
    opts.flags |= NIOVA_BLOCK_FLAGS_USE_EXTRA_OPTS;

    auto client = std::make_shared<NiovaClient>();
    rc = NiovaBlockClientNew(&client->client, &opts);
    if (rc) {
        NIXL_ERROR << "NIOVA: NiovaBlockClientNew failed with rc=" << rc;
        return NIXL_ERR_BACKEND;
    }

    client->sector_size = niova_block_client_sector_size(client->client);
    client->vdev_size = niova_block_client_vdev_size(client->client);
    if (client->sector_size <= 0 || client->vdev_size < 0) {
        NIXL_ERROR << "NIOVA: failed to query vdev geometry";
        return NIXL_ERR_BACKEND;
    }

    md->client = std::move(client);
    out = md.release();
    return NIXL_SUCCESS;
}

nixl_status_t
nixlNiovaEngine::deregisterMem(nixlBackendMD *meta) {
    delete static_cast<NiovaMD *>(meta);
    return NIXL_SUCCESS;
}

nixl_status_t
nixlNiovaEngine::prepXfer(const nixl_xfer_op_t &operation,
                          const nixl_meta_dlist_t &local,
                          const nixl_meta_dlist_t &remote,
                          const std::string &remote_agent,
                          nixlBackendReqH *&handle,
                          const nixl_opt_b_args_t *opt_args) const {
    (void)remote_agent;
    (void)opt_args;
    handle = nullptr;

    nixl_status_t valid = validateRequest(operation, local, remote);
    if (valid != NIXL_SUCCESS) {
        return valid;
    }

    auto req = std::make_unique<NiovaReq>(operation);
    req->descs.reserve(local.descCount());
    if (local.descCount() == 0) {
        return NIXL_ERR_INVALID_PARAM;
    }

    for (int i = 0; i < local.descCount(); i++) {
        auto *blk_md = static_cast<NiovaMD *>(remote[i].metadataP);
        if (!blk_md || blk_md->mem_type != BLK_SEG || !blk_md->client) {
            NIXL_ERROR << "NIOVA: missing BLK_SEG metadata";
            return NIXL_ERR_INVALID_PARAM;
        }
        if (local[i].len != remote[i].len) {
            NIXL_ERROR << "NIOVA: local and remote descriptor lengths differ";
            return NIXL_ERR_INVALID_PARAM;
        }

        const int sector_size = blk_md->client->sector_size;
        if ((remote[i].addr % sector_size) || (remote[i].len % sector_size)) {
            NIXL_ERROR << "NIOVA: BLK_SEG offset and length must be sector aligned";
            return NIXL_ERR_INVALID_PARAM;
        }
        if ((remote[i].addr + remote[i].len) > static_cast<uintptr_t>(blk_md->client->vdev_size)) {
            NIXL_ERROR << "NIOVA: transfer exceeds vdev size";
            return NIXL_ERR_INVALID_PARAM;
        }

        NiovaXferDesc desc;
        desc.client = blk_md->client;
        desc.start_blk = remote[i].addr / sector_size;
        desc.iov.iov_base = reinterpret_cast<void *>(local[i].addr);
        desc.iov.iov_len = local[i].len;
        req->descs.push_back(desc);
    }

    handle = req.release();
    return NIXL_SUCCESS;
}

nixl_status_t
nixlNiovaEngine::postXfer(const nixl_xfer_op_t &operation,
                          const nixl_meta_dlist_t &local,
                          const nixl_meta_dlist_t &remote,
                          const std::string &remote_agent,
                          nixlBackendReqH *&handle,
                          const nixl_opt_b_args_t *opt_args) const {
    (void)operation;
    (void)local;
    (void)remote;
    (void)remote_agent;
    (void)opt_args;

    auto *req = static_cast<NiovaReq *>(handle);
    if (!req || req->posted.exchange(true, std::memory_order_acq_rel)) {
        return NIXL_ERR_REPOST_ACTIVE;
    }

    req->pending.store(req->descs.size(), std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(req->mutex);
        req->status = NIXL_IN_PROG;
    }

    for (auto &desc : req->descs) {
        int rc = 0;
        if (req->op == NIXL_READ) {
            rc = NiovaBlockClientReadv(desc.client->client,
                                       desc.start_blk,
                                       &desc.iov,
                                       1,
                                       niovaXferCb,
                                       req);
        } else {
            rc = NiovaBlockClientWritev(desc.client->client,
                                        desc.start_blk,
                                        &desc.iov,
                                        1,
                                        niovaXferCb,
                                        req);
        }
        if (rc) {
            niovaXferCb(req, rc);
        }
    }

    return NIXL_IN_PROG;
}

nixl_status_t
nixlNiovaEngine::checkXfer(nixlBackendReqH *handle) const {
    auto *req = static_cast<NiovaReq *>(handle);
    if (!req) {
        return NIXL_ERR_INVALID_PARAM;
    }

    std::lock_guard<std::mutex> lock(req->mutex);
    nixl_status_t status = req->status;
    if (status != NIXL_IN_PROG && req->pending.load(std::memory_order_acquire) == 0) {
        req->posted.store(false, std::memory_order_release);
    }
    return status;
}

nixl_status_t
nixlNiovaEngine::releaseReqH(nixlBackendReqH *handle) const {
    auto *req = static_cast<NiovaReq *>(handle);
    if (!req) {
        return NIXL_SUCCESS;
    }
    if (req->pending.load(std::memory_order_acquire) != 0) {
        return NIXL_ERR_NOT_ALLOWED;
    }

    delete req;
    return NIXL_SUCCESS;
}
