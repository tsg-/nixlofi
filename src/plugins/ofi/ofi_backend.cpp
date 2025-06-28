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
 * Unless required by applicable law-or-agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ofi_backend.h"

nixlOFI_Engine::nixlOFI_Engine(const nixlBackendInitParams* init_params) {
    // TODO: constructor
}

nixlOFI_Engine::~nixlOFI_Engine() {
    // TODO: destructor
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
    return mems;
}

nixl_status_t nixlOFI_Engine::connect(const std::string &remote_agent) {
    // TODO: connection logic
    return NIXL_SUCCESS;
}

nixl_status_t nixlOFI_Engine::disconnect(const std::string &remote_agent) {
    // TODO: disconnection logic
    return NIXL_SUCCESS;
}

nixl_status_t nixlOFI_Engine::registerMem(const nixlBlobDesc &mem,
                                     const nixl_mem_t &nixl_mem,
                                     nixlBackendMD* &out) {
    // TODO: memory registration
    return NIXL_SUCCESS;
}

nixl_status_t nixlOFI_Engine::deregisterMem(nixlBackendMD *meta) {
    // TODO: memory deregistration
    return NIXL_SUCCESS;
}

nixl_status_t nixlOFI_Engine::postXfer(const nixl_xfer_op_t &operation,
                                  const nixl_meta_dlist_t &local,
                                  const nixl_meta_dlist_t &remote,
                                  const std::string &remote_agent,
                                  nixlBackendReqH* &handle,
                                  const nixl_opt_b_args_t* opt_args) const {
    // TODO: transfer posting
    return NIXL_SUCCESS;
}

nixl_status_t nixlOFI_Engine::checkXfer(nixlBackendReqH* handle) const {
    // TODO: transfer checking
    return NIXL_SUCCESS;
}
