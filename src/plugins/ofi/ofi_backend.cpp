/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 Intel Corporation. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ofi_backend.h"
#include "common/nixl_log.h"
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_endpoint.h>

nixlOFI_Engine::nixlOFI_Engine(const nixlBackendInitParams* init_params) :
    nixlBackendEngine(init_params),
    fabric(nullptr),
    domain(nullptr),
    ep(nullptr),
    cq(nullptr),
    eq(nullptr),
    pep(nullptr),
    fi(nullptr),
    cached_provider_info(nullptr),
    av(nullptr),
    is_connectionless(false),
    eq_thread_stop(false),
    eq_timeout_ms(100),
    hmem_ze_supported(false),
    hmem_cuda_supported(false),
    hmem_synapseai_supported(false)
{
    local_agent_name = init_params->localAgent;
    struct fi_info *hints = nullptr;
    struct fi_info *info = nullptr;
    struct fi_info *current_info = nullptr;
    int ret = 0;

    // set default provider and get user preference
    provider_name = "verbs;ofi_rxm";
    auto it = init_params->customParams->find("provider");
    if (it != init_params->customParams->end()) {
        provider_name = it->second;
    }
    
    NIXL_DEBUG << "OFI plugin using provider: " << provider_name;

    it = init_params->customParams->find("eq_timeout_ms");
    if (it != init_params->customParams->end()) {
        try {
            long timeout = std::stol(it->second);
            if (timeout >= 0 && timeout <= 60000) {  // 0-60 seconds max
                eq_timeout_ms = timeout;
            } else {
                NIXL_WARN << "eq_timeout_ms out of range [0-60000]: " << timeout << ", using default 100ms";
            }
        } catch (const std::exception& e) {
            NIXL_WARN << "Invalid eq_timeout_ms parameter: " << it->second << ", using default 100ms";
        }
    }

    hints = fi_allocinfo();
    if (!hints) {
        initErr = true;
        NIXL_ERROR << "fi_allocinfo failed";
        return;
    }

    // RMA - remote memory ops without CPU involvement$
    // HMEM - heterogenous memory support$
    hints->caps = FI_MSG | FI_RMA | FI_READ | FI_WRITE | FI_HMEM;

    // context pointer and cq data required for completion tracking
    hints->mode = FI_CONTEXT | FI_RX_CQ_DATA;

    // RDM works with both connection-oriented (verbs) and connectionless (shm) providers
    hints->ep_attr->type = FI_EP_RDM;

    // RoCEv2 and performance optimizations for verbs provider
    if (provider_name == "verbs") {
        auto roce_it = init_params->customParams->find("roce_version");
        if (roce_it != init_params->customParams->end() && roce_it->second == "2") {
            NIXL_DEBUG << "Enabling RoCEv2 optimizations";
        }
        
        // set inline threshold for small messages
        auto inline_it = init_params->customParams->find("inline_threshold");
        if (inline_it != init_params->customParams->end()) {
            try {
                size_t inline_thresh = std::stoull(inline_it->second);
                hints->tx_attr->inject_size = inline_thresh;
                NIXL_DEBUG << "Set inline threshold to " << inline_thresh << " bytes";
            } catch (const std::exception& e) {
                NIXL_WARN << "Invalid inline_threshold: " << inline_it->second;
            }
        }
        
        // set TX queue size for performance
        auto tx_size_it = init_params->customParams->find("tx_queue_size");
        if (tx_size_it != init_params->customParams->end()) {
            try {
                size_t tx_size = std::stoull(tx_size_it->second);
                hints->tx_attr->size = tx_size;
                NIXL_DEBUG << "Set TX queue size to " << tx_size;
            } catch (const std::exception& e) {
                NIXL_WARN << "Invalid tx_queue_size: " << tx_size_it->second;
            }
        }
    }

    // connection management capability if not shm provider
    if (provider_name != "shm") {
        hints->caps |= FI_MSG | FI_RMA;
    }

    ret = fi_getinfo(FI_VERSION(1, 0), nullptr, nullptr, 0, hints, &info);
    if (ret) {
        NIXL_ERROR << "fi_getinfo failed: " << fi_strerror(-ret);
        goto cleanup_getinfo;
    }

    // find the desired libfabrics provider
    current_info = info;
    while (current_info) {
        if (current_info->fabric_attr->prov_name) {
            std::string available_provider = current_info->fabric_attr->prov_name;
            NIXL_DEBUG << "Available provider: " << available_provider;
            
            // match exact provider name or provider name as substring for composite providers
            if (available_provider == provider_name || 
                available_provider.find(provider_name) != std::string::npos) {
                fi = current_info;
                NIXL_DEBUG << "Selected provider: " << available_provider;
                break;
            }
        }
        current_info = current_info->next;
    }

    if (!fi) {
        NIXL_ERROR << "Provider " << provider_name << " not found. Available providers:";
        current_info = info;
        while (current_info) {
            if (current_info->fabric_attr->prov_name) {
                NIXL_ERROR << "  - " << current_info->fabric_attr->prov_name;
            }
            current_info = current_info->next;
        }
        goto cleanup_getinfo;
    }

    // connectionless provider?
    is_connectionless = isConnectionlessProvider();

    // detect HMEM capabilities for this provider
    detectHmemCapabilities(fi, provider_name, hmem_cuda_supported,
                           hmem_ze_supported, hmem_synapseai_supported);

    ret = fi_fabric(fi->fabric_attr, &fabric, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_fabric failed: " << fi_strerror(-ret);
        goto cleanup_getinfo;
    }

    ret = fi_domain(fabric, fi, &domain, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_domain failed: " << fi_strerror(-ret);
        goto cleanup_fabric;
    }

    if (setupEndpoint(!is_connectionless) != NIXL_SUCCESS) {
        goto cleanup_domain;
    }

    if (ret != NIXL_SUCCESS) {
        goto cleanup_domain;
    }

    // get local address
    if (getEndpointAddress(ep, local_addr) != NIXL_SUCCESS) {
        goto cleanup_ep;
    }

    // cache provider use in connect()
    cached_provider_info = fi_dupinfo(fi);

    fi_freeinfo(hints);
    fi_freeinfo(info);

    // start event loop thread for connection-oriented providers
    if (!is_connectionless) {
        eq_thread = std::thread(&nixlOFI_Engine::eq_event_loop, this);
    }
    return;

cleanup_ep:
    if (pep) fi_close(&pep->fid);
    if (ep) fi_close(&ep->fid);
    if (av) fi_close(&av->fid);
cleanup_domain:
    if (domain) fi_close(&domain->fid);
cleanup_fabric:
    if (fabric) fi_close(&fabric->fid);
cleanup_getinfo:
    fi_freeinfo(hints);
    if (info) fi_freeinfo(info);
    initErr = true;
}

nixlOFI_Engine::~nixlOFI_Engine() {
    if (!is_connectionless) {
        eq_thread_stop = true;
        if (eq_thread.joinable()) {
            // wake up the EQ thread to ensure it exits
            if (eq) {
                uint32_t event;
                fi_eq_read(eq, &event, nullptr, 0, 0);
            }
            eq_thread.join();
        }
    }

    // close connected endpoints
    for (auto const& [key, val] : connected_eps) {
        fi_close(&val->fid);
    }

    if (pep) fi_close(&pep->fid);
    if (ep) fi_close(&ep->fid);
    if (cq) fi_close(&cq->fid);
    if (eq) fi_close(&eq->fid);
    if (av) fi_close(&av->fid);
    if (domain) fi_close(&domain->fid);
    if (fabric) fi_close(&fabric->fid);
    if (fi) fi_freeinfo(fi);
    if (cached_provider_info) fi_freeinfo(cached_provider_info);
}

bool nixlOFI_Engine::supportsNotif() const {
    return false;
}

bool nixlOFI_Engine::supportsRemote() const {
    return true;
}

bool nixlOFI_Engine::supportsLocal() const {
    return false;
}

bool nixlOFI_Engine::supportsProgTh() const {
    return true;
}

nixl_mem_list_t nixlOFI_Engine::getSupportedMems() const {
    nixl_mem_list_t mems;
    mems.push_back(DRAM_SEG);
    if (hmem_cuda_supported) {
        mems.push_back(VRAM_SEG);
    }
    if (hmem_synapseai_supported) {
        mems.push_back(VRAM_SEG);
    }
    if (hmem_ze_supported) {
        mems.push_back(VRAM_SEG);
    }
    return mems;
}

nixl_status_t nixlOFI_Engine::connect(const std::string &remote_agent) {
    std::lock_guard<std::mutex> lock(ep_lock);

    if (is_connectionless) {
        // for connectionless providers like shm: insert remote address into av
        if (shm_addrs.count(remote_agent)) {
            NIXL_DEBUG << "Already have address mapping for " << remote_agent;
            return NIXL_SUCCESS;
        }

        auto remote_addr_it = remote_addrs.find(remote_agent);
        if (remote_addr_it == remote_addrs.end()) {
            NIXL_ERROR << "Remote address for " << remote_agent << " not found.";
            return NIXL_ERR_NOT_FOUND;
        }

        fi_addr_t addr;
        int ret = fi_av_insert(av, remote_addr_it->second.data(), 1, &addr, 0, nullptr);
        if (ret != 1) {
            NIXL_ERROR << "fi_av_insert failed: " << fi_strerror(-ret);
            return NIXL_ERR_BACKEND;
        }

        shm_addrs[remote_agent] = addr;
        NIXL_DEBUG << "OFI backend: Added address mapping for " << remote_agent;
        return NIXL_SUCCESS;
    }

    // connection-oriented logic
    if (connected_eps.count(remote_agent)) {
        NIXL_DEBUG << "Already connected to " << remote_agent;
        return NIXL_SUCCESS;
    }

    auto remote_addr_it = remote_addrs.find(remote_agent);
    if (remote_addr_it == remote_addrs.end()) {
        NIXL_ERROR << "Remote address for " << remote_agent << " not found.";
        return NIXL_ERR_NOT_FOUND;
    }
    const std::string &remote_addr_str = remote_addr_it->second;

    struct fi_info *hints = nullptr;
    struct fi_info *info = nullptr;
    int ret = 0;

    hints = fi_allocinfo();
    if (!hints) {
        NIXL_ERROR << "fi_allocinfo failed";
        return NIXL_ERR_BACKEND;
    }

    hints->ep_attr->type = FI_EP_RDM;
    hints->fabric_attr->prov_name = strdup(provider_name.c_str());

    ret = fi_getinfo(FI_VERSION(1, 0), remote_addr_str.c_str(), nullptr, 0, hints, &info);
    if (ret) {
        NIXL_ERROR << "fi_getinfo for remote agent failed: " << fi_strerror(-ret);
        fi_freeinfo(hints);
        return NIXL_ERR_BACKEND;
    }

    struct fi_info *remote_fi = info;
    if (!remote_fi) {
        NIXL_ERROR << "No provider info returned for remote agent";
        fi_freeinfo(hints);
        fi_freeinfo(info);
        return NIXL_ERR_BACKEND;
    }

    fid_ep *remote_ep = nullptr;
    ret = fi_endpoint(domain, remote_fi, &remote_ep, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_endpoint for remote failed: " << fi_strerror(-ret);
        fi_freeinfo(hints);
        fi_freeinfo(info);
        return NIXL_ERR_BACKEND;
    }

    ret = fi_ep_bind(remote_ep, &cq->fid, FI_SEND | FI_RECV);
    if (ret) {
        NIXL_ERROR << "fi_ep_bind to CQ for remote failed: " << fi_strerror(-ret);
        fi_close(&remote_ep->fid);
        fi_freeinfo(hints);
        fi_freeinfo(info);
        return NIXL_ERR_BACKEND;
    }

    ret = fi_ep_bind(remote_ep, &eq->fid, FI_SOURCE | FI_RMA | FI_MSG);
    if (ret) {
        NIXL_ERROR << "fi_ep_bind to EQ for remote failed: " << fi_strerror(-ret);
        fi_close(&remote_ep->fid);
        fi_freeinfo(hints);
        fi_freeinfo(info);
        return NIXL_ERR_BACKEND;
    }

    ret = fi_enable(remote_ep);
    if (ret) {
        NIXL_ERROR << "fi_enable for remote failed: " << fi_strerror(-ret);
        fi_close(&remote_ep->fid);
        fi_freeinfo(hints);
        fi_freeinfo(info);
        return NIXL_ERR_BACKEND;
    }

    ret = fi_connect(remote_ep, remote_fi->dest_addr, local_agent_name.c_str(), local_agent_name.length() + 1);
    if (ret) {
        NIXL_ERROR << "fi_connect failed: " << fi_strerror(-ret);
        fi_close(&remote_ep->fid);
        fi_freeinfo(hints);
        fi_freeinfo(info);
        return NIXL_ERR_BACKEND;
    }

    fi_freeinfo(hints);
    fi_freeinfo(info);

    // wait for connection to complete via EQ
    struct fi_eq_cm_entry entry;
    uint32_t event;
    ssize_t n_events = fi_eq_read(eq, &event, &entry, 1, -1);
    if (n_events < 0) {
        NIXL_ERROR << "fi_eq_read failed during connect: " << fi_strerror(-n_events);
        fi_close(&remote_ep->fid);
        return NIXL_ERR_BACKEND;
    }
    if (event != FI_CONNECTED || entry.fid != &remote_ep->fid) {
        NIXL_ERROR << "Unexpected EQ event during connect: " << event;
        fi_close(&remote_ep->fid);
        return NIXL_ERR_BACKEND;
    }

    connected_eps[remote_agent] = remote_ep;

    NIXL_DEBUG << "OFI backend: Connected to " << remote_agent;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOFI_Engine::disconnect(const std::string &remote_agent) {
    std::lock_guard<std::mutex> lock(ep_lock);

    if (is_connectionless) {
        // connectionless provider, remove address mapping
        auto it = shm_addrs.find(remote_agent);
        if (it == shm_addrs.end()) {
            NIXL_WARN << "OFI backend: No address mapping for " << remote_agent;
            return NIXL_ERR_NOT_FOUND;
        }

        int ret = fi_av_remove(av, &it->second, 1, 0);
        if (ret) {
            NIXL_ERROR << "fi_av_remove failed: " << fi_strerror(-ret);
            return NIXL_ERR_BACKEND;
        }

        shm_addrs.erase(it);
        NIXL_DEBUG << "OFI backend: Removed address mapping for " << remote_agent;
        return NIXL_SUCCESS;
    }

    // connection-oriented case
    auto it = connected_eps.find(remote_agent);
    if (it == connected_eps.end()) {
        NIXL_WARN << "OFI backend: No active connection to " << remote_agent;
        return NIXL_ERR_NOT_FOUND;
    }

    int ret = fi_close(&it->second->fid);
    if (ret) {
        NIXL_ERROR << "fi_close (remote_ep) failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    connected_eps.erase(it);
    NIXL_DEBUG << "OFI backend: Disconnected from " << remote_agent;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOFI_Engine::registerMem(const nixlBlobDesc &mem,
                                     const nixl_mem_t &nixl_mem,
                                     nixlBackendMD* &out) {
    nixlOFI_Metadata *ofi_meta = new nixlOFI_Metadata();
    if (!ofi_meta) {
        return NIXL_ERR_BACKEND;
    }

    int ret = 0;
    if (nixl_mem == DRAM_SEG) {
        ret = fi_mr_reg(domain, reinterpret_cast<void*>(mem.addr), mem.len,
                       FI_REMOTE_READ | FI_REMOTE_WRITE | FI_SEND | FI_RECV,
                       0, 0, 0, &ofi_meta->mr, nullptr);
    } else if (nixl_mem == VRAM_SEG) {
        struct fi_mr_attr mr_attr = {};
        struct iovec iov = {};

        iov.iov_base = reinterpret_cast<void*>(mem.addr);
        iov.iov_len = mem.len;

        mr_attr.mr_iov = &iov;
        mr_attr.iov_count = 1;
        mr_attr.access = FI_REMOTE_READ | FI_REMOTE_WRITE | FI_SEND | FI_RECV;

        // prioritize device interfaces based on availability and device ID
        if (hmem_ze_supported && mem.devId >= 0) {
            mr_attr.iface = FI_HMEM_ZE;
            mr_attr.device.ze = mem.devId;
            NIXL_DEBUG << "Using ZE HMEM interface for device " << mem.devId;
        } else if (hmem_synapseai_supported && mem.devId >= 0) {
            mr_attr.iface = FI_HMEM_SYNAPSEAI;
            mr_attr.device.synapseai = mem.devId;
            NIXL_DEBUG << "Using SynapseAI HMEM interface for device " << mem.devId;
        } else if (hmem_cuda_supported && mem.devId >= 0) {
            mr_attr.iface = FI_HMEM_CUDA;
            mr_attr.device.cuda = mem.devId;
            NIXL_DEBUG << "Using CUDA HMEM interface for device " << mem.devId;
        } else {
            NIXL_ERROR << "VRAM memory requested but no supported HMEM interface available. "
                      << "CUDA: " << hmem_cuda_supported 
                      << ", ZE: " << hmem_ze_supported 
                      << ", SynapseAI: " << hmem_synapseai_supported
                      << ", DeviceID: " << mem.devId;
            delete ofi_meta;
            return NIXL_ERR_NOT_SUPPORTED;
        }

        ret = fi_mr_regattr(domain, &mr_attr, 0, &ofi_meta->mr);
    } else {
        NIXL_ERROR << "Unsupported memory type: " << nixl_mem;
        delete ofi_meta;
        return NIXL_ERR_NOT_SUPPORTED;
    }

    if (ret) {
        NIXL_ERROR << "fi_mr_reg failed: " << fi_strerror(-ret);
        delete ofi_meta;
        return NIXL_ERR_BACKEND;
    }

    ofi_meta->desc = fi_mr_desc(ofi_meta->mr);
    if (!ofi_meta->desc) {
        NIXL_ERROR << "fi_mr_desc failed";
        fi_close(&ofi_meta->mr->fid);
        delete ofi_meta;
        return NIXL_ERR_BACKEND;
    }

    out = ofi_meta;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOFI_Engine::deregisterMem(nixlBackendMD *meta) {
    nixlOFI_Metadata *ofi_meta = static_cast<nixlOFI_Metadata*>(meta);
    if (!ofi_meta) {
        return NIXL_ERR_INVALID_PARAM;
    }

    int ret = fi_close(&ofi_meta->mr->fid);
    if (ret) {
        NIXL_ERROR << "fi_close (mr) failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    delete ofi_meta;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOFI_Engine::unloadMD(nixlBackendMD* input) {
    return deregisterMem(input);
}

nixl_status_t nixlOFI_Engine::prepXfer(const nixl_xfer_op_t &operation,
                                  const nixl_meta_dlist_t &local,
                                  const nixl_meta_dlist_t &remote,
                                  const std::string &remote_agent,
                                  nixlBackendReqH* &handle,
                                  const nixl_opt_b_args_t* opt_args) const {
    return postXfer(operation, local, remote, remote_agent, handle, opt_args);
}

nixl_status_t nixlOFI_Engine::postXfer(const nixl_xfer_op_t &operation,
                                  const nixl_meta_dlist_t &local,
                                  const nixl_meta_dlist_t &remote,
                                  const std::string &remote_agent,
                                  nixlBackendReqH* &handle,
                                  const nixl_opt_b_args_t* opt_args) const {
    fid_ep *target_ep = ep;
    fi_addr_t dest_addr = FI_ADDR_UNSPEC;

    if (is_connectionless) {
        auto shm_it = shm_addrs.find(remote_agent);
        if (shm_it == shm_addrs.end()) {
            NIXL_ERROR << "OFI backend: No address mapping for " << remote_agent;
            return NIXL_ERR_NOT_FOUND;
        }
        dest_addr = shm_it->second;
    } else {
        auto it = connected_eps.find(remote_agent);
        if (it == connected_eps.end()) {
            NIXL_ERROR << "OFI backend: Not connected to " << remote_agent;
            return NIXL_ERR_NOT_FOUND;
        }
        target_ep = it->second;
    }

    if (local.descCount() != remote.descCount()) {
        NIXL_ERROR << "Mismatched descriptor counts: local=" << local.descCount()
                   << ", remote=" << remote.descCount();
        return NIXL_ERR_INVALID_PARAM;
    }

    nixlOFI_Request *ofi_req = new nixlOFI_Request();
    if (!ofi_req) {
        return NIXL_ERR_BACKEND;
    }
    ofi_req->cq = cq;

    int ret = 0;
    for (size_t i = 0; i < static_cast<size_t>(local.descCount()); ++i) {
        const nixlMetaDesc &local_desc = local[i];
        const nixlMetaDesc &remote_desc = remote[i];

        nixlOFI_Metadata *local_meta = static_cast<nixlOFI_Metadata*>(local_desc.metadataP);
        nixlOFI_Metadata *remote_meta = static_cast<nixlOFI_Metadata*>(remote_desc.metadataP);

        struct fi_rma_iov rma_iov = {
            .addr = (uint64_t)remote_desc.addr,
            .len = remote_desc.len,
            .key = fi_mr_key(remote_meta->mr)
        };

        switch (operation) {
            case NIXL_READ:
                ret = fi_read(target_ep, reinterpret_cast<void*>(local_desc.addr),
                             local_desc.len, local_meta->desc, dest_addr,
                             rma_iov.addr, rma_iov.key, &ofi_req->wr_id);
                break;
            case NIXL_WRITE:
                ret = fi_write(target_ep, reinterpret_cast<void*>(local_desc.addr),
                              local_desc.len, local_meta->desc, dest_addr,
                              rma_iov.addr, rma_iov.key, &ofi_req->wr_id);
                break;
            default:
                NIXL_ERROR << "Unsupported operation type";
                delete ofi_req;
                return NIXL_ERR_NOT_SUPPORTED;
        }

        if (ret) {
            NIXL_ERROR << "OFI transfer failed: " << fi_strerror(-ret);
            delete ofi_req;
            return NIXL_ERR_BACKEND;
        }
    }

    handle = ofi_req;
    return NIXL_SUCCESS;

}


nixl_status_t nixlOFI_Engine::checkXfer(nixlBackendReqH* handle) const {
    nixlOFI_Request *ofi_req = static_cast<nixlOFI_Request*>(handle);
    if (!ofi_req) {
        return NIXL_ERR_INVALID_PARAM;
    }

    struct fi_cq_data_entry entry;
    int ret = fi_cq_read(ofi_req->cq, &entry, 1);
    if (ret == 1) {
        return NIXL_SUCCESS;
    } else if (ret == -FI_EAGAIN) {
        return NIXL_IN_PROG;
    } else if (ret < 0) {
        struct fi_cq_err_entry err_entry;
        int err_ret = fi_cq_readerr(ofi_req->cq, &err_entry, 0);
        if (err_ret > 0) {
            NIXL_ERROR << "CQ error: " << fi_strerror(err_entry.err) << " (" << err_entry.err << ")";
        } else {
            NIXL_ERROR << "fi_cq_read failed: " << fi_strerror(-ret);
        }
        return NIXL_ERR_BACKEND;
    }
    return NIXL_IN_PROG;
}

nixl_status_t nixlOFI_Engine::releaseReqH(nixlBackendReqH* handle) const {
    nixlOFI_Request *ofi_req = static_cast<nixlOFI_Request*>(handle);
    if (!ofi_req) {
        return NIXL_ERR_INVALID_PARAM;
    }
    delete ofi_req;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOFI_Engine::getConnInfo(std::string &conn_info) const {
    conn_info = local_addr;
    return NIXL_SUCCESS;
}

nixl_status_t nixlOFI_Engine::loadRemoteConnInfo(const std::string &remote_agent, const std::string &conn_info) {
    std::lock_guard<std::mutex> lock(ep_lock);
    remote_addrs[remote_agent] = conn_info;
    return NIXL_SUCCESS;
}

void nixlOFI_Engine::eq_event_loop() {
    while (!eq_thread_stop) {
        struct fi_eq_cm_entry entry;
        uint32_t event;
        ssize_t ret = fi_eq_read(eq, &event, &entry, 1, eq_timeout_ms);

        if (ret == -FI_EAGAIN) {
            continue;
        } else if (ret < 0) {
            if (ret == -FI_EINTR && eq_thread_stop) {
                // interrupt
                break;
            }
            NIXL_ERROR << "fi_eq_read failed in event loop: " << fi_strerror(-ret);
            // TODO: error handling
            continue;
        }

        switch (event) {
            case FI_CONNREQ:
            {
                NIXL_DEBUG << "FI_CONNREQ event received";
                fid_ep *new_ep = nullptr;

                // accept
                int connreq_ret = fi_endpoint(domain, fi, &new_ep, nullptr);
                if (connreq_ret) {
                    NIXL_ERROR << "fi_endpoint for accepted connection failed: " << fi_strerror(-connreq_ret);
                    break;
                }
                connreq_ret = fi_ep_bind(new_ep, &cq->fid, FI_SEND | FI_RECV);
                if (connreq_ret) {
                    NIXL_ERROR << "fi_ep_bind to CQ for accepted connection failed: " << fi_strerror(-connreq_ret);
                    fi_close(&new_ep->fid);
                    break;
                }
                connreq_ret = fi_ep_bind(new_ep, &eq->fid, FI_SOURCE | FI_RMA | FI_MSG);
                if (connreq_ret) {
                    NIXL_ERROR << "fi_ep_bind to EQ for accepted connection failed: " << fi_strerror(-connreq_ret);
                    fi_close(&new_ep->fid);
                    break;
                }
                connreq_ret = fi_accept(new_ep, nullptr, 0);
                if (connreq_ret) {
                    NIXL_ERROR << "fi_accept failed: " << fi_strerror(-connreq_ret);
                    fi_close(&new_ep->fid);
                    break;
                }
                connreq_ret = fi_enable(new_ep);
                if (connreq_ret) {
                    NIXL_ERROR << "fi_enable for accepted connection failed: " << fi_strerror(-connreq_ret);
                    fi_close(&new_ep->fid);
                    break;
                }

                std::string remote_agent_name = "connected_agent_" + std::to_string(reinterpret_cast<uintptr_t>(new_ep));

                std::lock_guard<std::mutex> lock(ep_lock);
                connected_eps[remote_agent_name] = new_ep;
                NIXL_DEBUG << "Accepted connection from " << remote_agent_name;
                break;
            }
            case FI_CONNECTED:
                NIXL_DEBUG << "FI_CONNECTED event received for outgoing connection";
                // TODO: async model
                break;
            case FI_SHUTDOWN:
                NIXL_DEBUG << "FI_SHUTDOWN event received";
                {
                    std::lock_guard<std::mutex> lock(ep_lock);
                    for (auto it = connected_eps.begin(); it != connected_eps.end(); ++it) {
                        if (&it->second->fid == entry.fid) {
                            fi_close(&it->second->fid);
                            connected_eps.erase(it);
                            break;
                        }
                    }
                }
                break;
            default:
                NIXL_WARN << "Unhandled EQ event: " << event;
                break;
        }
    }
}

bool nixlOFI_Engine::isConnectionlessProvider() const {
    if (provider_name == "shm" || provider_name == "udp" ||
        provider_name.find("ofi_rxm") != std::string::npos) {
        return true;
    }
    
    // also check the actual provider name from libfabric
    if (fi && fi->fabric_attr && fi->fabric_attr->prov_name) {
        std::string actual_provider = fi->fabric_attr->prov_name;
        if (actual_provider == "shm" || actual_provider == "udp" ||
            actual_provider.find("shm") != std::string::npos) {
            return true;
        }
    }
    
    return false;
}

nixl_status_t nixlOFI_Engine::setupEndpoint(bool connection_oriented) {
    int ret = 0;

    // create endpoint
    ret = fi_endpoint(domain, fi, &ep, nullptr);
    if (ret) {
        NIXL_ERROR << "fi_endpoint failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    // create and bind completion queue
    struct fi_cq_attr cq_attr = {};
    cq_attr.size = 128; // use fi->tx_attr->size + fi->rx_attr->size?
    cq_attr.format = FI_CQ_FORMAT_CONTEXT;
    ret = fi_cq_open(domain, &cq_attr, &cq, nullptr);

    if (ret) {
        NIXL_ERROR << "fi_cq_open failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    ret = fi_ep_bind(ep, &cq->fid, FI_SEND | FI_RECV);
    if (ret) {
        NIXL_ERROR << "fi_ep_bind to CQ failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    if (connection_oriented) {
        // event queue for connection management
        struct fi_eq_attr eq_attr = {};
        eq_attr.size = 64;
        eq_attr.wait_obj = FI_WAIT_UNSPEC;
        ret = fi_eq_open(fabric, &eq_attr, &eq, nullptr);
        if (ret) {
            NIXL_ERROR << "fi_eq_open failed: " << fi_strerror(-ret);
            return NIXL_ERR_BACKEND;
        }

        // bind endpoint to EQ for connection management
        ret = fi_ep_bind(ep, &eq->fid, 0);
        if (ret) {
            NIXL_ERROR << "fi_ep_bind to EQ failed: " << fi_strerror(-ret);
            return NIXL_ERR_BACKEND;
        }

        // create passive endpoint for listening
        ret = fi_passive_ep(fabric, fi, &pep, nullptr);
        if (ret) {
            NIXL_ERROR << "fi_passive_ep failed: " << fi_strerror(-ret);
            return NIXL_ERR_BACKEND;
        }

        ret = fi_pep_bind(pep, &eq->fid, 0);
        if (ret) {
            NIXL_ERROR << "fi_pep_bind to EQ failed: " << fi_strerror(-ret);
            fi_close(&pep->fid);
            return NIXL_ERR_BACKEND;
        }

        ret = fi_listen(pep);
        if (ret) {
            NIXL_ERROR << "fi_listen failed: " << fi_strerror(-ret);
            fi_close(&pep->fid);
            return NIXL_ERR_BACKEND;
        }
    } else {
        // address vector for connectionless communication
        struct fi_av_attr av_attr = {};
        av_attr.type = FI_AV_MAP;
        ret = fi_av_open(domain, &av_attr, &av, nullptr);
        if (ret) {
            NIXL_ERROR << "fi_av_open failed: " << fi_strerror(-ret);
            return NIXL_ERR_BACKEND;
        }

        ret = fi_ep_bind(ep, &av->fid, 0);
        if (ret) {
            NIXL_ERROR << "fi_ep_bind to AV failed: " << fi_strerror(-ret);
            return NIXL_ERR_BACKEND;
        }

        ret = fi_enable(ep);
        if (ret) {
            NIXL_ERROR << "fi_enable failed: " << fi_strerror(-ret);
            return NIXL_ERR_BACKEND;
        }
    }
    return NIXL_SUCCESS;
}

nixl_status_t nixlOFI_Engine::getEndpointAddress(fid_ep* endpoint, std::string& address) {
    if (!endpoint) {
        return NIXL_ERR_INVALID_PARAM;
    }

    size_t addrlen = 0;
    int ret = fi_getname(&endpoint->fid, nullptr, &addrlen);
    if (ret != 0 || addrlen == 0) {
        NIXL_ERROR << "fi_getname failed to get address length: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    std::vector<char> addr_buf(addrlen);
    ret = fi_getname(&endpoint->fid, addr_buf.data(), &addrlen);
    if (ret) {
        NIXL_ERROR << "fi_getname failed: " << fi_strerror(-ret);
        return NIXL_ERR_BACKEND;
    }

    address = std::string(addr_buf.data(), addrlen);
    return NIXL_SUCCESS;
}

void nixlOFI_Engine::detectHmemCapabilities(struct fi_info* fi_info,
                                            const std::string& provider_name,
                                            bool& cuda_supported,
                                            bool& ze_supported,
                                            bool& synapseai_supported) {
    if (!fi_info || !(fi_info->domain_attr->mr_mode & FI_MR_HMEM)) {
        NIXL_DEBUG << "Provider " << provider_name << " does not support HMEM";
        cuda_supported = false;
        ze_supported = false;
        synapseai_supported = false;
        return;
    }

    struct {
        const char* name;
        enum fi_hmem_iface iface;
        bool& flag;
    } hmem_checks[] = {
        {"NVIDIA CUDA", FI_HMEM_CUDA, cuda_supported},
        {"Gaudi SynapseAI", FI_HMEM_SYNAPSEAI, synapseai_supported},
        {"Intel Level Zero", FI_HMEM_ZE, ze_supported}
    };

    // use libfabric's HMEM detection for each interface
    for (const auto& check : hmem_checks) {
        struct fi_info *hmem_hints = fi_dupinfo(fi_info);
        struct fi_info *hmem_info = nullptr;

        if (hmem_hints) {
            // test specific HMEM interface support
            hmem_hints->caps |= FI_HMEM;

            int ret = fi_getinfo(FI_VERSION(1, 0), nullptr, nullptr, 0, hmem_hints, &hmem_info);
            if (ret == 0 && hmem_info) {
                // verify the provider actually supports this HMEM interface
                check.flag = (hmem_info->caps & FI_HMEM) != 0;
                if (check.flag) {
                    NIXL_DEBUG << check.name << " HMEM support detected for provider " << provider_name;
                }
                fi_freeinfo(hmem_info);
            } else {
                check.flag = false;
                NIXL_DEBUG << check.name << " HMEM support not available: " << fi_strerror(-ret);
            }
            fi_freeinfo(hmem_hints);
        } else {
            // fallback
            check.flag = false;
            NIXL_WARN << "Failed to duplicate fi_info for " << check.name << " detection";
        }
    }
}
