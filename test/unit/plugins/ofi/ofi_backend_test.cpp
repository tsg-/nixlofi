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

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>
#include <cstring>

#include "ofi_backend.h"
#include "common/nixl_log.h"

class OFIBackendTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_params.localAgentName = "test_agent";
        custom_params["provider"] = "verbs;ofi_rxm";
        custom_params["eq_timeout_ms"] = "100";
        init_params.customParams = &custom_params;
    }

    void TearDown() override {
    }

    nixlBackendInitParams init_params;
    std::map<std::string, std::string> custom_params;
};

TEST_F(OFIBackendTest, ConstructorBasic) {
    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));

    // check if engine was created without initialization error
    EXPECT_NE(engine.get(), nullptr);
}

TEST_F(OFIBackendTest, SupportMethods) {
    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));

    EXPECT_FALSE(engine->supportsNotif());
    EXPECT_TRUE(engine->supportsRemote());
    EXPECT_FALSE(engine->supportsLocal());
    EXPECT_TRUE(engine->supportsProgTh());
}

TEST_F(OFIBackendTest, GetSupportedMems) {
    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));

    nixl_mem_list_t mems = engine->getSupportedMems();
    EXPECT_FALSE(mems.empty());
    EXPECT_EQ(mems[0], DRAM_SEG);
}

TEST_F(OFIBackendTest, InvalidProvider) {
    custom_params["provider"] = "invalid_provider";

    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));
    EXPECT_NE(engine.get(), nullptr);
}

TEST_F(OFIBackendTest, CustomTimeout) {
    custom_params["eq_timeout_ms"] = "500";

    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));
    EXPECT_NE(engine.get(), nullptr);
}

TEST_F(OFIBackendTest, InvalidTimeout) {
    custom_params["eq_timeout_ms"] = "invalid";

    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));
    EXPECT_NE(engine.get(), nullptr);
}

TEST_F(OFIBackendTest, TimeoutOutOfRange) {
    custom_params["eq_timeout_ms"] = "70000";

    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));
    EXPECT_NE(engine.get(), nullptr);
}

TEST_F(OFIBackendTest, GetConnInfo) {
    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));
    
    std::string conn_info;
    nixl_status_t status = engine->getConnInfo(conn_info);

    EXPECT_EQ(status, NIXL_SUCCESS);
    EXPECT_FALSE(conn_info.empty());
}

TEST_F(OFIBackendTest, LoadRemoteConnInfo) {
    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));

    std::string remote_agent = "remote_test_agent";
    std::string conn_info = "dummy_connection_info";

    nixl_status_t status = engine->loadRemoteConnInfo(remote_agent, conn_info);
    EXPECT_EQ(status, NIXL_SUCCESS);
}

TEST_F(OFIBackendTest, ConnectWithoutRemoteInfo) {
    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));

    std::string remote_agent = "nonexistent_agent";
    nixl_status_t status = engine->connect(remote_agent);

    EXPECT_EQ(status, NIXL_ERR_NOT_FOUND);
}

TEST_F(OFIBackendTest, DisconnectNonexistentAgent) {
    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));

    std::string remote_agent = "nonexistent_agent";
    nixl_status_t status = engine->disconnect(remote_agent);

    EXPECT_EQ(status, NIXL_ERR_NOT_FOUND);
}

TEST_F(OFIBackendTest, RegisterMemoryDRAM) {
    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));

    size_t buffer_size = 1024;
    void* buffer = malloc(buffer_size);
    ASSERT_NE(buffer, nullptr);

    nixlBlobDesc mem_desc;
    mem_desc.addr = buffer;
    mem_desc.len = buffer_size;
    mem_desc.devId = 0;

    nixlBackendMD* metadata = nullptr;
    nixl_status_t status = engine->registerMem(mem_desc, DRAM_SEG, metadata);

    if (status == NIXL_SUCCESS) {
        EXPECT_NE(metadata, nullptr);
        
        nixl_status_t deregister_status = engine->deregisterMem(metadata);
        EXPECT_EQ(deregister_status, NIXL_SUCCESS);
    }

    free(buffer);
}

TEST_F(OFIBackendTest, DeregisterNullMetadata) {
    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));
    
    nixl_status_t status = engine->deregisterMem(nullptr);
    EXPECT_EQ(status, NIXL_ERR_INVALID_PARAM);
}

TEST_F(OFIBackendTest, CheckXferWithNullHandle) {
    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));
    
    nixl_status_t status = engine->checkXfer(nullptr);
    EXPECT_EQ(status, NIXL_ERR_INVALID_PARAM);
}

TEST_F(OFIBackendTest, ReleaseReqHWithNullHandle) {
    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));
    
    nixl_status_t status = engine->releaseReqH(nullptr);
    EXPECT_EQ(status, NIXL_ERR_INVALID_PARAM);
}

class OFISHMProviderTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_params.localAgentName = "shm_test_agent";
        custom_params["provider"] = "shm";
        custom_params["eq_timeout_ms"] = "100";
        init_params.customParams = &custom_params;
    }

    void TearDown() override {
    }

    nixlBackendInitParams init_params;
    std::map<std::string, std::string> custom_params;
};

TEST_F(OFISHMProviderTest, SHMProviderDetection) {
    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));
    EXPECT_NE(engine.get(), nullptr);
}

TEST_F(OFISHMProviderTest, SHMConnectionlessConnect) {
    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));

    std::string remote_agent = "shm_remote_agent";
    std::string conn_info = "dummy_shm_address";

    nixl_status_t status = engine->loadRemoteConnInfo(remote_agent, conn_info);
    EXPECT_EQ(status, NIXL_SUCCESS);

    status = engine->connect(remote_agent);
    EXPECT_TRUE(status == NIXL_SUCCESS || status == NIXL_ERR_BACKEND);
}

TEST_F(OFISHMProviderTest, SHMConnectionlessDisconnect) {
    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));

    std::string remote_agent = "nonexistent_shm_agent";
    nixl_status_t status = engine->disconnect(remote_agent);

    EXPECT_EQ(status, NIXL_ERR_NOT_FOUND);
}

TEST_F(OFISHMProviderTest, SHMSupportMethods) {
    std::unique_ptr<nixlOFI_Engine> engine(new nixlOFI_Engine(&init_params));

    EXPECT_FALSE(engine->supportsNotif());
    EXPECT_TRUE(engine->supportsRemote());
    EXPECT_FALSE(engine->supportsLocal());
    EXPECT_TRUE(engine->supportsProgTh());
}

class OFIMetadataTest : public ::testing::Test {
protected:
    void SetUp() override {
        metadata = new nixlOFI_Metadata();
    }

    void TearDown() override {
        delete metadata;
    }

    nixlOFI_Metadata* metadata;
};

TEST_F(OFIMetadataTest, DefaultConstructor) {
    EXPECT_EQ(metadata->mr, nullptr);
    EXPECT_EQ(metadata->desc, nullptr);
}

class OFIRequestTest : public ::testing::Test {
protected:
    void SetUp() override {
        request = new nixlOFI_Request();
    }

    void TearDown() override {
        delete request;
    }

    nixlOFI_Request* request;
};

TEST_F(OFIRequestTest, DefaultConstructor) {
    EXPECT_EQ(request->cq, nullptr);
    EXPECT_EQ(request->wr_id, 0);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
