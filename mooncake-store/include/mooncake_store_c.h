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

#ifndef MOONCAKE_STORE_C_H
#define MOONCAKE_STORE_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// ============================================================================
// Type Definitions
// ============================================================================

/**
 * @brief Opaque handle to a Mooncake Store client instance
 */
typedef void* mooncake_store_client_t;

/**
 * @brief Error codes for Mooncake Store operations
 * 
 * These match the ErrorCode enum in types.h
 */
typedef enum {
    MOONCAKE_OK = 0,
    MOONCAKE_INTERNAL_ERROR = -1,
    MOONCAKE_BUFFER_OVERFLOW = -10,
    MOONCAKE_SEGMENT_NOT_FOUND = -101,
    MOONCAKE_SEGMENT_ALREADY_EXISTS = -102,
    MOONCAKE_NO_AVAILABLE_HANDLE = -200,
    MOONCAKE_INVALID_VERSION = -300,
    MOONCAKE_INVALID_KEY = -400,
    MOONCAKE_WRITE_FAIL = -500,
    MOONCAKE_INVALID_PARAMS = -600,
    MOONCAKE_INVALID_WRITE = -700,
    MOONCAKE_INVALID_READ = -701,
    MOONCAKE_INVALID_REPLICA = -702,
    MOONCAKE_REPLICA_IS_NOT_READY = -703,
    MOONCAKE_OBJECT_NOT_FOUND = -704,
    MOONCAKE_OBJECT_ALREADY_EXISTS = -705,
    MOONCAKE_OBJECT_HAS_LEASE = -706,
    MOONCAKE_LEASE_EXPIRED = -707,
    MOONCAKE_TRANSFER_FAIL = -800,
    MOONCAKE_RPC_FAIL = -900,
    MOONCAKE_FILE_NOT_FOUND = -1100,
} mooncake_error_code_t;

/**
 * @brief Represents a contiguous memory slice for data transfer
 */
struct mooncake_slice {
    void* ptr;     ///< Pointer to the data buffer
    size_t size;   ///< Size of the buffer in bytes
};
typedef struct mooncake_slice mooncake_slice_t;

/**
 * @brief Configuration for object replication
 */
struct mooncake_replicate_config {
    size_t replica_num;              ///< Number of replicas (default: 1)
    int with_soft_pin;               ///< Enable soft pin mechanism (0=false, 1=true)
    const char* preferred_segment;   ///< Preferred segment name (can be NULL)
};
typedef struct mooncake_replicate_config mooncake_replicate_config_t;

/**
 * @brief Query result containing replica information
 */
struct mooncake_query_result {
    int replica_count;               ///< Number of replicas
    void* internal_data;             ///< Internal data (opaque)
};
typedef struct mooncake_query_result mooncake_query_result_t;

// ============================================================================
// Client Lifecycle Management
// ============================================================================

/**
 * @brief Creates and initializes a new Mooncake Store client
 * 
 * @param local_hostname Local host address (e.g., "localhost:17813" or IP:Port)
 * @param metadata_connstring Connection string for metadata service 
 *                            (e.g., "P2PHANDSHAKE" or "http://localhost:8080/metadata")
 * @param protocol Transfer protocol ("tcp" or "rdma")
 * @param device_names Comma-separated RDMA device names (can be NULL for auto-discovery)
 * @param master_server_entry Master server address 
 *                           (e.g., "localhost:50051" for non-HA mode,
 *                            "etcd://host1:2379;host2:2379" for HA mode)
 * @return Handle to the client instance, or NULL on failure
 * 
 * @note All char* parameters are copied internally and can be freed after the call
 */
mooncake_store_client_t mooncake_client_create(
    const char* local_hostname,
    const char* metadata_connstring,
    const char* protocol,
    const char* device_names,
    const char* master_server_entry);

/**
 * @brief Destroys a Mooncake Store client and releases all resources
 * 
 * @param client Client handle to destroy
 */
void mooncake_client_destroy(mooncake_store_client_t client);

/**
 * @brief Gets the transport endpoint of the client
 * 
 * @param client Client handle
 * @param buf_out Output buffer for the endpoint string
 * @param buf_len Size of the output buffer
 * @return 0 on success, error code on failure
 */
int mooncake_client_get_endpoint(mooncake_store_client_t client,
                                 char* buf_out,
                                 size_t buf_len);

// ============================================================================
// Memory Management
// ============================================================================

/**
 * @brief Registers a memory segment for storage allocation
 * 
 * @param client Client handle
 * @param buffer Pointer to the memory buffer
 * @param size Size of the buffer in bytes
 * @return 0 on success, error code on failure
 */
int mooncake_client_mount_segment(mooncake_store_client_t client,
                                  const void* buffer,
                                  size_t size);

/**
 * @brief Unregisters a memory segment
 * 
 * @param client Client handle
 * @param buffer Pointer to the memory buffer
 * @param size Size of the buffer in bytes
 * @return 0 on success, error code on failure
 */
int mooncake_client_unmount_segment(mooncake_store_client_t client,
                                    const void* buffer,
                                    size_t size);

/**
 * @brief Registers local memory for data transfer
 * 
 * @param client Client handle
 * @param addr Memory address to register
 * @param length Size of the memory region
 * @param location Device location (e.g., "cpu:0", "cuda:0")
 * @param remote_accessible Whether memory can be accessed remotely (0=false, 1=true)
 * @param update_metadata Whether to update metadata service (0=false, 1=true)
 * @return 0 on success, error code on failure
 */
int mooncake_client_register_memory(mooncake_store_client_t client,
                                    void* addr,
                                    size_t length,
                                    const char* location,
                                    int remote_accessible,
                                    int update_metadata);

/**
 * @brief Unregisters local memory
 * 
 * @param client Client handle
 * @param addr Memory address to unregister
 * @param update_metadata Whether to update metadata service (0=false, 1=true)
 * @return 0 on success, error code on failure
 */
int mooncake_client_unregister_memory(mooncake_store_client_t client,
                                      void* addr,
                                      int update_metadata);

// ============================================================================
// Data Operations
// ============================================================================

/**
 * @brief Stores an object with the given key
 * 
 * @param client Client handle
 * @param key Object key (null-terminated string)
 * @param slices Array of memory slices containing the data
 * @param slice_count Number of slices in the array
 * @param config Replication configuration
 * @return 0 on success, error code on failure
 * 
 * @note The memory pointed to by slices must be registered via mooncake_client_register_memory
 */
int mooncake_client_put(mooncake_store_client_t client,
                        const char* key,
                        const mooncake_slice_t* slices,
                        size_t slice_count,
                        const mooncake_replicate_config_t* config);

/**
 * @brief Retrieves an object by key
 * 
 * @param client Client handle
 * @param key Object key (null-terminated string)
 * @param slices Array of memory slices to store the retrieved data
 * @param slice_count Number of slices in the array
 * @return 0 on success, error code on failure
 * 
 * @note The memory pointed to by slices must be registered and pre-allocated
 */
int mooncake_client_get(mooncake_store_client_t client,
                        const char* key,
                        mooncake_slice_t* slices,
                        size_t slice_count);

/**
 * @brief Queries object metadata without transferring data
 * 
 * @param client Client handle
 * @param key Object key (null-terminated string)
 * @param result Output parameter for query result (must be freed with mooncake_query_result_free)
 * @return 0 on success, error code on failure
 */
int mooncake_client_query(mooncake_store_client_t client,
                          const char* key,
                          mooncake_query_result_t** result);

/**
 * @brief Retrieves an object using pre-queried metadata
 * 
 * @param client Client handle
 * @param key Object key (null-terminated string)
 * @param query_result Previously obtained query result
 * @param slices Array of memory slices to store the retrieved data
 * @param slice_count Number of slices in the array
 * @return 0 on success, error code on failure
 */
int mooncake_client_get_with_query(mooncake_store_client_t client,
                                   const char* key,
                                   const mooncake_query_result_t* query_result,
                                   mooncake_slice_t* slices,
                                   size_t slice_count);

/**
 * @brief Frees a query result
 * 
 * @param result Query result to free
 */
void mooncake_query_result_free(mooncake_query_result_t* result);

/**
 * @brief Removes an object and all its replicas
 * 
 * @param client Client handle
 * @param key Object key (null-terminated string)
 * @return 0 on success, error code on failure
 */
int mooncake_client_remove(mooncake_store_client_t client, const char* key);

/**
 * @brief Removes all objects matching a regex pattern
 * 
 * @param client Client handle
 * @param regex_pattern Regular expression pattern (null-terminated string)
 * @param removed_count Output parameter for number of removed objects (can be NULL)
 * @return 0 on success, error code on failure
 */
int mooncake_client_remove_by_regex(mooncake_store_client_t client,
                                    const char* regex_pattern,
                                    long* removed_count);

/**
 * @brief Removes all objects from the store
 * 
 * @param client Client handle
 * @param removed_count Output parameter for number of removed objects (can be NULL)
 * @return 0 on success, error code on failure
 */
int mooncake_client_remove_all(mooncake_store_client_t client,
                               long* removed_count);

/**
 * @brief Checks if an object exists
 * 
 * @param client Client handle
 * @param key Object key (null-terminated string)
 * @param exists Output parameter (0=not exists, 1=exists)
 * @return 0 on success, error code on failure
 */
int mooncake_client_is_exist(mooncake_store_client_t client,
                             const char* key,
                             int* exists);

// ============================================================================
// Batch Operations
// ============================================================================

/**
 * @brief Stores multiple objects in batch
 * 
 * @param client Client handle
 * @param keys Array of object keys (null-terminated strings)
 * @param slices Array of slice arrays (one per key)
 * @param slice_counts Array of slice counts (one per key)
 * @param key_count Number of keys/objects
 * @param config Replication configuration
 * @param error_codes Output array for error codes (one per key, can be NULL)
 * @return Number of successful operations
 */
int mooncake_client_batch_put(mooncake_store_client_t client,
                              const char** keys,
                              const mooncake_slice_t** slices,
                              const size_t* slice_counts,
                              size_t key_count,
                              const mooncake_replicate_config_t* config,
                              int* error_codes);

/**
 * @brief Retrieves multiple objects in batch
 * 
 * @param client Client handle
 * @param keys Array of object keys (null-terminated strings)
 * @param slices Array of slice arrays (one per key)
 * @param slice_counts Array of slice counts (one per key)
 * @param key_count Number of keys/objects
 * @param error_codes Output array for error codes (one per key, can be NULL)
 * @return Number of successful operations
 */
int mooncake_client_batch_get(mooncake_store_client_t client,
                              const char** keys,
                              mooncake_slice_t** slices,
                              const size_t* slice_counts,
                              size_t key_count,
                              int* error_codes);

/**
 * @brief Checks if multiple objects exist in batch
 * 
 * @param client Client handle
 * @param keys Array of object keys (null-terminated strings)
 * @param key_count Number of keys
 * @param exists Output array (0=not exists, 1=exists, one per key)
 * @param error_codes Output array for error codes (one per key, can be NULL)
 * @return Number of successful operations
 */
int mooncake_client_batch_is_exist(mooncake_store_client_t client,
                                   const char** keys,
                                   size_t key_count,
                                   int* exists,
                                   int* error_codes);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * @brief Gets a human-readable error message for an error code
 * 
 * @param error_code Error code
 * @return Error message string (do not free)
 */
const char* mooncake_error_message(mooncake_error_code_t error_code);

/**
 * @brief Initializes a replicate config with default values
 * 
 * @param config Config structure to initialize
 */
void mooncake_replicate_config_init(mooncake_replicate_config_t* config);

/**
 * @brief Gets summary metrics in human-readable format
 * 
 * @param client Client handle
 * @param buf_out Output buffer for the metrics string
 * @param buf_len Size of the output buffer
 * @return 0 on success, error code on failure
 */
int mooncake_client_get_metrics(mooncake_store_client_t client,
                                char* buf_out,
                                size_t buf_len);

/**
 * @brief Gets metrics in Prometheus format
 * 
 * @param client Client handle
 * @param buf_out Output buffer for the metrics string
 * @param buf_len Size of the output buffer
 * @return 0 on success, error code on failure
 */
int mooncake_client_serialize_metrics(mooncake_store_client_t client,
                                      char* buf_out,
                                      size_t buf_len);

#ifdef __cplusplus
}
#endif  // __cplusplus

#endif  // MOONCAKE_STORE_C_H
