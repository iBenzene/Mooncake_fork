// Copyright 2024 KVCache.AI
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mooncake_store_c.h"

#include <glog/logging.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "client.h"
#include "types.h"

namespace {

// Helper function to convert C++ ErrorCode to C error code
mooncake_error_code_t ErrorCodeToC(mooncake::ErrorCode error_code) {
    return static_cast<mooncake_error_code_t>(mooncake::toInt(error_code));
}

// Helper function to convert C++ Slice to C slice
void ConvertSliceToC(const mooncake::Slice& cpp_slice,
                     mooncake_slice_t* c_slice) {
    c_slice->ptr = cpp_slice.ptr;
    c_slice->size = cpp_slice.size;
}

// Helper function to convert C slice to C++ Slice
mooncake::Slice ConvertSliceToCpp(const mooncake_slice_t* c_slice) {
    return mooncake::Slice{c_slice->ptr, c_slice->size};
}

// Helper function to convert C config to C++ ReplicateConfig
mooncake::ReplicateConfig ConvertConfigToCpp(
    const mooncake_replicate_config_t* c_config) {
    mooncake::ReplicateConfig cpp_config;
    cpp_config.replica_num = c_config->replica_num;
    cpp_config.with_soft_pin = c_config->with_soft_pin != 0;
    if (c_config->preferred_segment != nullptr) {
        cpp_config.preferred_segment = c_config->preferred_segment;
    }
    return cpp_config;
}

// Wrapper struct for QueryResult
struct QueryResultWrapper {
    mooncake::QueryResult result;
    explicit QueryResultWrapper(mooncake::QueryResult&& res)
        : result(std::move(res)) {}
};

}  // namespace

// ============================================================================
// Client Lifecycle Management
// ============================================================================

mooncake_store_client_t mooncake_client_create(
    const char* local_hostname, const char* metadata_connstring,
    const char* protocol, const char* device_names,
    const char* master_server_entry) {
    if (!local_hostname || !metadata_connstring || !protocol ||
        !master_server_entry) {
        LOG(ERROR) << "mooncake_client_create: NULL parameters";
        return nullptr;
    }

    std::optional<std::string> devices_opt = std::nullopt;
    if (device_names != nullptr && strlen(device_names) > 0) {
        devices_opt = std::string(device_names);
    }

    auto client_opt = mooncake::Client::Create(
        local_hostname, metadata_connstring, protocol, devices_opt,
        master_server_entry);

    if (!client_opt.has_value()) {
        LOG(ERROR) << "Failed to create Mooncake Store client";
        return nullptr;
    }

    // Allocate and return the client pointer
    auto* client_ptr = new std::shared_ptr<mooncake::Client>(
        std::move(client_opt.value()));
    return static_cast<mooncake_store_client_t>(client_ptr);
}

void mooncake_client_destroy(mooncake_store_client_t client) {
    if (client == nullptr) return;

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);
    delete client_ptr;
}

int mooncake_client_get_endpoint(mooncake_store_client_t client,
                                 char* buf_out, size_t buf_len) {
    if (client == nullptr || buf_out == nullptr || buf_len == 0) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);
    std::string endpoint = (*client_ptr)->GetTransportEndpoint();

    if (endpoint.size() + 1 > buf_len) {
        return MOONCAKE_BUFFER_OVERFLOW;
    }

    strncpy(buf_out, endpoint.c_str(), buf_len - 1);
    buf_out[buf_len - 1] = '\0';
    return MOONCAKE_OK;
}

// ============================================================================
// Memory Management
// ============================================================================

int mooncake_client_mount_segment(mooncake_store_client_t client,
                                  const void* buffer, size_t size) {
    if (client == nullptr || buffer == nullptr || size == 0) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);
    auto result = (*client_ptr)->MountSegment(buffer, size);

    if (!result.has_value()) {
        return ErrorCodeToC(result.error());
    }
    return MOONCAKE_OK;
}

int mooncake_client_unmount_segment(mooncake_store_client_t client,
                                    const void* buffer, size_t size) {
    if (client == nullptr || buffer == nullptr || size == 0) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);
    auto result = (*client_ptr)->UnmountSegment(buffer, size);

    if (!result.has_value()) {
        return ErrorCodeToC(result.error());
    }
    return MOONCAKE_OK;
}

int mooncake_client_register_memory(mooncake_store_client_t client, void* addr,
                                    size_t length, const char* location,
                                    int remote_accessible,
                                    int update_metadata) {
    if (client == nullptr || addr == nullptr || location == nullptr) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);
    auto result = (*client_ptr)->RegisterLocalMemory(
        addr, length, location, remote_accessible != 0,
        update_metadata != 0);

    if (!result.has_value()) {
        return ErrorCodeToC(result.error());
    }
    return MOONCAKE_OK;
}

int mooncake_client_unregister_memory(mooncake_store_client_t client,
                                      void* addr, int update_metadata) {
    if (client == nullptr || addr == nullptr) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);
    auto result =
        (*client_ptr)->unregisterLocalMemory(addr, update_metadata != 0);

    if (!result.has_value()) {
        return ErrorCodeToC(result.error());
    }
    return MOONCAKE_OK;
}

// ============================================================================
// Data Operations
// ============================================================================

int mooncake_client_put(mooncake_store_client_t client, const char* key,
                        const mooncake_slice_t* slices, size_t slice_count,
                        const mooncake_replicate_config_t* config) {
    if (client == nullptr || key == nullptr || slices == nullptr ||
        config == nullptr) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);

    // Convert C slices to C++ slices
    std::vector<mooncake::Slice> cpp_slices;
    cpp_slices.reserve(slice_count);
    for (size_t i = 0; i < slice_count; ++i) {
        cpp_slices.push_back(ConvertSliceToCpp(&slices[i]));
    }

    // Convert config
    auto cpp_config = ConvertConfigToCpp(config);

    // Call Put
    auto result = (*client_ptr)->Put(key, cpp_slices, cpp_config);

    if (!result.has_value()) {
        return ErrorCodeToC(result.error());
    }
    return MOONCAKE_OK;
}

int mooncake_client_get(mooncake_store_client_t client, const char* key,
                        mooncake_slice_t* slices, size_t slice_count) {
    if (client == nullptr || key == nullptr || slices == nullptr) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);

    // Convert C slices to C++ slices
    std::vector<mooncake::Slice> cpp_slices;
    cpp_slices.reserve(slice_count);
    for (size_t i = 0; i < slice_count; ++i) {
        cpp_slices.push_back(ConvertSliceToCpp(&slices[i]));
    }

    // Call Get
    auto result = (*client_ptr)->Get(key, cpp_slices);

    if (!result.has_value()) {
        return ErrorCodeToC(result.error());
    }

    // Update C slices (in case sizes changed)
    for (size_t i = 0; i < slice_count && i < cpp_slices.size(); ++i) {
        ConvertSliceToC(cpp_slices[i], &slices[i]);
    }

    return MOONCAKE_OK;
}

int mooncake_client_query(mooncake_store_client_t client, const char* key,
                          mooncake_query_result_t** result) {
    if (client == nullptr || key == nullptr || result == nullptr) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);

    auto query_result = (*client_ptr)->Query(key);
    if (!query_result.has_value()) {
        return ErrorCodeToC(query_result.error());
    }

    // Create wrapper and C result
    auto* wrapper = new QueryResultWrapper(std::move(query_result.value()));
    auto* c_result = new mooncake_query_result_t;
    c_result->replica_count = wrapper->result.replicas.size();
    c_result->internal_data = wrapper;

    *result = c_result;
    return MOONCAKE_OK;
}

int mooncake_client_get_with_query(
    mooncake_store_client_t client, const char* key,
    const mooncake_query_result_t* query_result, mooncake_slice_t* slices,
    size_t slice_count) {
    if (client == nullptr || key == nullptr || query_result == nullptr ||
        slices == nullptr) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);
    auto* wrapper =
        static_cast<QueryResultWrapper*>(query_result->internal_data);

    // Convert C slices to C++ slices
    std::vector<mooncake::Slice> cpp_slices;
    cpp_slices.reserve(slice_count);
    for (size_t i = 0; i < slice_count; ++i) {
        cpp_slices.push_back(ConvertSliceToCpp(&slices[i]));
    }

    // Call Get with query result
    auto result = (*client_ptr)->Get(key, wrapper->result, cpp_slices);

    if (!result.has_value()) {
        return ErrorCodeToC(result.error());
    }

    // Update C slices
    for (size_t i = 0; i < slice_count && i < cpp_slices.size(); ++i) {
        ConvertSliceToC(cpp_slices[i], &slices[i]);
    }

    return MOONCAKE_OK;
}

void mooncake_query_result_free(mooncake_query_result_t* result) {
    if (result == nullptr) return;

    if (result->internal_data != nullptr) {
        auto* wrapper = static_cast<QueryResultWrapper*>(result->internal_data);
        delete wrapper;
    }
    delete result;
}

int mooncake_client_remove(mooncake_store_client_t client, const char* key) {
    if (client == nullptr || key == nullptr) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);
    auto result = (*client_ptr)->Remove(key);

    if (!result.has_value()) {
        return ErrorCodeToC(result.error());
    }
    return MOONCAKE_OK;
}

int mooncake_client_remove_by_regex(mooncake_store_client_t client,
                                    const char* regex_pattern,
                                    long* removed_count) {
    if (client == nullptr || regex_pattern == nullptr) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);
    auto result = (*client_ptr)->RemoveByRegex(regex_pattern);

    if (!result.has_value()) {
        return ErrorCodeToC(result.error());
    }

    if (removed_count != nullptr) {
        *removed_count = result.value();
    }
    return MOONCAKE_OK;
}

int mooncake_client_remove_all(mooncake_store_client_t client,
                               long* removed_count) {
    if (client == nullptr) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);
    auto result = (*client_ptr)->RemoveAll();

    if (!result.has_value()) {
        return ErrorCodeToC(result.error());
    }

    if (removed_count != nullptr) {
        *removed_count = result.value();
    }
    return MOONCAKE_OK;
}

int mooncake_client_is_exist(mooncake_store_client_t client, const char* key,
                             int* exists) {
    if (client == nullptr || key == nullptr || exists == nullptr) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);
    auto result = (*client_ptr)->IsExist(key);

    if (!result.has_value()) {
        return ErrorCodeToC(result.error());
    }

    *exists = result.value() ? 1 : 0;
    return MOONCAKE_OK;
}

// ============================================================================
// Batch Operations
// ============================================================================

int mooncake_client_batch_put(mooncake_store_client_t client,
                              const char** keys,
                              const mooncake_slice_t** slices,
                              const size_t* slice_counts, size_t key_count,
                              const mooncake_replicate_config_t* config,
                              int* error_codes) {
    if (client == nullptr || keys == nullptr || slices == nullptr ||
        slice_counts == nullptr || config == nullptr) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);

    // Convert keys
    std::vector<std::string> cpp_keys;
    cpp_keys.reserve(key_count);
    for (size_t i = 0; i < key_count; ++i) {
        cpp_keys.push_back(keys[i]);
    }

    // Convert slices
    std::vector<std::vector<mooncake::Slice>> cpp_slices;
    cpp_slices.reserve(key_count);
    for (size_t i = 0; i < key_count; ++i) {
        std::vector<mooncake::Slice> slice_vec;
        slice_vec.reserve(slice_counts[i]);
        for (size_t j = 0; j < slice_counts[i]; ++j) {
            slice_vec.push_back(ConvertSliceToCpp(&slices[i][j]));
        }
        cpp_slices.push_back(std::move(slice_vec));
    }

    // Convert config
    auto cpp_config = ConvertConfigToCpp(config);

    // Call BatchPut
    auto results = (*client_ptr)->BatchPut(cpp_keys, cpp_slices, cpp_config);

    // Process results
    int success_count = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].has_value()) {
            success_count++;
            if (error_codes != nullptr) {
                error_codes[i] = MOONCAKE_OK;
            }
        } else {
            if (error_codes != nullptr) {
                error_codes[i] = ErrorCodeToC(results[i].error());
            }
        }
    }

    return success_count;
}

int mooncake_client_batch_get(mooncake_store_client_t client,
                              const char** keys, mooncake_slice_t** slices,
                              const size_t* slice_counts, size_t key_count,
                              int* error_codes) {
    if (client == nullptr || keys == nullptr || slices == nullptr ||
        slice_counts == nullptr) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);

    // Convert keys
    std::vector<std::string> cpp_keys;
    cpp_keys.reserve(key_count);
    for (size_t i = 0; i < key_count; ++i) {
        cpp_keys.push_back(keys[i]);
    }

    // Convert slices
    std::unordered_map<std::string, std::vector<mooncake::Slice>> cpp_slices;
    for (size_t i = 0; i < key_count; ++i) {
        std::vector<mooncake::Slice> slice_vec;
        slice_vec.reserve(slice_counts[i]);
        for (size_t j = 0; j < slice_counts[i]; ++j) {
            slice_vec.push_back(ConvertSliceToCpp(&slices[i][j]));
        }
        cpp_slices[cpp_keys[i]] = std::move(slice_vec);
    }

    // Call BatchGet
    auto results = (*client_ptr)->BatchGet(cpp_keys, cpp_slices);

    // Process results and update C slices
    int success_count = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].has_value()) {
            success_count++;
            if (error_codes != nullptr) {
                error_codes[i] = MOONCAKE_OK;
            }
            // Update C slices
            const auto& result_slices = cpp_slices[cpp_keys[i]];
            for (size_t j = 0; j < slice_counts[i] && j < result_slices.size();
                 ++j) {
                ConvertSliceToC(result_slices[j], &slices[i][j]);
            }
        } else {
            if (error_codes != nullptr) {
                error_codes[i] = ErrorCodeToC(results[i].error());
            }
        }
    }

    return success_count;
}

int mooncake_client_batch_is_exist(mooncake_store_client_t client,
                                   const char** keys, size_t key_count,
                                   int* exists, int* error_codes) {
    if (client == nullptr || keys == nullptr || exists == nullptr) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);

    // Convert keys
    std::vector<std::string> cpp_keys;
    cpp_keys.reserve(key_count);
    for (size_t i = 0; i < key_count; ++i) {
        cpp_keys.push_back(keys[i]);
    }

    // Call BatchIsExist
    auto results = (*client_ptr)->BatchIsExist(cpp_keys);

    // Process results
    int success_count = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].has_value()) {
            success_count++;
            exists[i] = results[i].value() ? 1 : 0;
            if (error_codes != nullptr) {
                error_codes[i] = MOONCAKE_OK;
            }
        } else {
            exists[i] = 0;
            if (error_codes != nullptr) {
                error_codes[i] = ErrorCodeToC(results[i].error());
            }
        }
    }

    return success_count;
}

// ============================================================================
// Utility Functions
// ============================================================================

const char* mooncake_error_message(mooncake_error_code_t error_code) {
    auto cpp_error_code = mooncake::fromInt(static_cast<int32_t>(error_code));
    return mooncake::toString(cpp_error_code).c_str();
}

void mooncake_replicate_config_init(mooncake_replicate_config_t* config) {
    if (config == nullptr) return;

    config->replica_num = 1;
    config->with_soft_pin = 0;
    config->preferred_segment = nullptr;
}

int mooncake_client_get_metrics(mooncake_store_client_t client, char* buf_out,
                                size_t buf_len) {
    if (client == nullptr || buf_out == nullptr || buf_len == 0) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);
    auto result = (*client_ptr)->GetSummaryMetrics();

    if (!result.has_value()) {
        return ErrorCodeToC(result.error());
    }

    const std::string& metrics = result.value();
    if (metrics.size() + 1 > buf_len) {
        return MOONCAKE_BUFFER_OVERFLOW;
    }

    strncpy(buf_out, metrics.c_str(), buf_len - 1);
    buf_out[buf_len - 1] = '\0';
    return MOONCAKE_OK;
}

int mooncake_client_serialize_metrics(mooncake_store_client_t client,
                                      char* buf_out, size_t buf_len) {
    if (client == nullptr || buf_out == nullptr || buf_len == 0) {
        return MOONCAKE_INVALID_PARAMS;
    }

    auto* client_ptr =
        static_cast<std::shared_ptr<mooncake::Client>*>(client);
    auto result = (*client_ptr)->SerializeMetrics();

    if (!result.has_value()) {
        return ErrorCodeToC(result.error());
    }

    const std::string& metrics = result.value();
    if (metrics.size() + 1 > buf_len) {
        return MOONCAKE_BUFFER_OVERFLOW;
    }

    strncpy(buf_out, metrics.c_str(), buf_len - 1);
    buf_out[buf_len - 1] = '\0';
    return MOONCAKE_OK;
}
