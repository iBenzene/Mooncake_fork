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

/**
 * @file mooncake_store_c_example.c
 * @brief Example program demonstrating the Mooncake Store C API
 * 
 * This example shows how to:
 * 1. Create a Mooncake Store client
 * 2. Mount storage segments
 * 3. Register memory for data transfer
 * 4. Put and Get objects
 * 5. Query object metadata
 * 6. Remove objects
 * 7. Cleanup resources
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mooncake_store_c.h"

#define SEGMENT_SIZE (512 * 1024 * 1024)    // 512 MB
#define LOCAL_BUFFER_SIZE (128 * 1024 * 1024)  // 128 MB
#define DATA_SIZE (1024 * 1024)  // 1 MB

int main(int argc, char* argv[]) {
    int ret = 0;
    
    printf("=== Mooncake Store C API Example ===\n\n");

    // ========================================================================
    // 1. Create Client
    // ========================================================================
    printf("1. Creating Mooncake Store client...\n");
    mooncake_store_client_t client = mooncake_client_create(
        "localhost:17813",           // local_hostname
        "P2PHANDSHAKE",              // metadata_connstring
        "tcp",                       // protocol
        NULL,                        // device_names (auto-discovery)
        "localhost:50051"            // master_server_entry
    );

    if (client == NULL) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }
    printf("   ✓ Client created successfully\n\n");

    // ========================================================================
    // 2. Register Local Memory for Data Transfer
    // ========================================================================
    printf("2. Registering local memory...\n");
    void* local_buffer = malloc(LOCAL_BUFFER_SIZE);
    if (local_buffer == NULL) {
        fprintf(stderr, "Failed to allocate local buffer\n");
        mooncake_client_destroy(client);
        return 1;
    }

    ret = mooncake_client_register_memory(
        client,
        local_buffer,
        LOCAL_BUFFER_SIZE,
        "cpu:0",
        1,  // remote_accessible
        1   // update_metadata
    );

    if (ret != MOONCAKE_OK) {
        fprintf(stderr, "Failed to register memory: %s\n",
                mooncake_error_message(ret));
        free(local_buffer);
        mooncake_client_destroy(client);
        return 1;
    }
    printf("   ✓ Local memory registered (%d MB)\n\n", LOCAL_BUFFER_SIZE / (1024 * 1024));

    // ========================================================================
    // 3. Mount Storage Segment
    // ========================================================================
    printf("3. Mounting storage segment...\n");
    void* segment_buffer = malloc(SEGMENT_SIZE);
    if (segment_buffer == NULL) {
        fprintf(stderr, "Failed to allocate segment buffer\n");
        mooncake_client_unregister_memory(client, local_buffer, 1);
        free(local_buffer);
        mooncake_client_destroy(client);
        return 1;
    }

    ret = mooncake_client_mount_segment(client, segment_buffer, SEGMENT_SIZE);
    if (ret != MOONCAKE_OK) {
        fprintf(stderr, "Failed to mount segment: %s\n",
                mooncake_error_message(ret));
        free(segment_buffer);
        mooncake_client_unregister_memory(client, local_buffer, 1);
        free(local_buffer);
        mooncake_client_destroy(client);
        return 1;
    }
    printf("   ✓ Storage segment mounted (%d MB)\n\n", SEGMENT_SIZE / (1024 * 1024));

    // ========================================================================
    // 4. Put an Object
    // ========================================================================
    printf("4. Storing an object...\n");
    const char* test_key = "test_key_c_api";
    const char* test_data = "Hello from Mooncake Store C API!";
    size_t data_len = strlen(test_data) + 1;  // Include null terminator

    // Prepare data in local buffer
    memcpy(local_buffer, test_data, data_len);

    // Create slice
    mooncake_slice_t put_slice;
    put_slice.ptr = local_buffer;
    put_slice.size = data_len;

    // Create replication config
    mooncake_replicate_config_t config;
    mooncake_replicate_config_init(&config);
    config.replica_num = 1;

    ret = mooncake_client_put(client, test_key, &put_slice, 1, &config);
    if (ret != MOONCAKE_OK) {
        fprintf(stderr, "Failed to put object: %s\n",
                mooncake_error_message(ret));
        goto cleanup;
    }
    printf("   ✓ Object stored with key: '%s'\n", test_key);
    printf("   ✓ Data: '%s'\n\n", test_data);

    // ========================================================================
    // 5. Check if Object Exists
    // ========================================================================
    printf("5. Checking if object exists...\n");
    int exists = 0;
    ret = mooncake_client_is_exist(client, test_key, &exists);
    if (ret != MOONCAKE_OK) {
        fprintf(stderr, "Failed to check existence: %s\n",
                mooncake_error_message(ret));
        goto cleanup;
    }
    printf("   ✓ Object exists: %s\n\n", exists ? "YES" : "NO");

    // ========================================================================
    // 6. Query Object Metadata
    // ========================================================================
    printf("6. Querying object metadata...\n");
    mooncake_query_result_t* query_result = NULL;
    ret = mooncake_client_query(client, test_key, &query_result);
    if (ret != MOONCAKE_OK) {
        fprintf(stderr, "Failed to query object: %s\n",
                mooncake_error_message(ret));
        goto cleanup;
    }
    printf("   ✓ Object has %d replica(s)\n\n", query_result->replica_count);

    // ========================================================================
    // 7. Get the Object
    // ========================================================================
    printf("7. Retrieving object...\n");
    // Clear buffer
    memset(local_buffer, 0, data_len);

    // Create slice for get
    mooncake_slice_t get_slice;
    get_slice.ptr = local_buffer;
    get_slice.size = data_len;

    ret = mooncake_client_get(client, test_key, &get_slice, 1);
    if (ret != MOONCAKE_OK) {
        fprintf(stderr, "Failed to get object: %s\n",
                mooncake_error_message(ret));
        mooncake_query_result_free(query_result);
        goto cleanup;
    }
    printf("   ✓ Object retrieved successfully\n");
    printf("   ✓ Retrieved data: '%s'\n\n", (char*)local_buffer);

    // Verify data
    if (strcmp((char*)local_buffer, test_data) == 0) {
        printf("   ✅ Data verification: PASSED\n\n");
    } else {
        printf("   ❌ Data verification: FAILED\n\n");
    }

    // ========================================================================
    // 8. Get with Query Result (Optimized Path)
    // ========================================================================
    printf("8. Retrieving object with pre-queried metadata...\n");
    memset(local_buffer, 0, data_len);
    get_slice.ptr = local_buffer;
    get_slice.size = data_len;

    ret = mooncake_client_get_with_query(client, test_key, query_result,
                                         &get_slice, 1);
    if (ret != MOONCAKE_OK) {
        fprintf(stderr, "Failed to get with query: %s\n",
                mooncake_error_message(ret));
        mooncake_query_result_free(query_result);
        goto cleanup;
    }
    printf("   ✓ Object retrieved via optimized path\n");
    printf("   ✓ Retrieved data: '%s'\n\n", (char*)local_buffer);

    mooncake_query_result_free(query_result);

    // ========================================================================
    // 9. Batch Operations Example
    // ========================================================================
    printf("9. Testing batch operations...\n");
    
    const char* keys[] = {"batch_key_1", "batch_key_2", "batch_key_3"};
    const char* data[] = {"Data 1", "Data 2", "Data 3"};
    size_t key_count = 3;

    // Put batch
    mooncake_slice_t* put_slices[3];
    size_t slice_counts[3];
    
    for (size_t i = 0; i < key_count; i++) {
        put_slices[i] = malloc(sizeof(mooncake_slice_t));
        put_slices[i]->ptr = (void*)((char*)local_buffer + i * 1024);
        put_slices[i]->size = strlen(data[i]) + 1;
        memcpy(put_slices[i]->ptr, data[i], put_slices[i]->size);
        slice_counts[i] = 1;
    }

    int error_codes[3];
    int success = mooncake_client_batch_put(
        client, keys, (const mooncake_slice_t**)put_slices,
        slice_counts, key_count, &config, error_codes);

    printf("   ✓ Batch put: %d/%zu objects stored\n", success, key_count);

    // Check batch existence
    int exists_results[3];
    success = mooncake_client_batch_is_exist(
        client, keys, key_count, exists_results, error_codes);
    
    printf("   ✓ Batch exists check: %d/%zu succeeded\n", success, key_count);
    for (size_t i = 0; i < key_count; i++) {
        printf("      - %s: %s\n", keys[i], 
               exists_results[i] ? "EXISTS" : "NOT FOUND");
    }

    // Cleanup batch slices
    for (size_t i = 0; i < key_count; i++) {
        free(put_slices[i]);
    }

    printf("\n");

    // ========================================================================
    // 10. Remove Objects
    // ========================================================================
    printf("10. Removing objects...\n");
    
    ret = mooncake_client_remove(client, test_key);
    if (ret != MOONCAKE_OK) {
        fprintf(stderr, "Failed to remove object: %s\n",
                mooncake_error_message(ret));
        goto cleanup;
    }
    printf("   ✓ Object '%s' removed\n", test_key);

    // Remove batch objects
    long removed_count = 0;
    ret = mooncake_client_remove_by_regex(client, "batch_key_.*", &removed_count);
    if (ret != MOONCAKE_OK) {
        fprintf(stderr, "Failed to remove by regex: %s\n",
                mooncake_error_message(ret));
        goto cleanup;
    }
    printf("   ✓ Removed %ld objects matching 'batch_key_.*'\n\n", removed_count);

    // ========================================================================
    // 11. Get Metrics
    // ========================================================================
    printf("11. Retrieving client metrics...\n");
    char metrics_buf[4096];
    ret = mooncake_client_get_metrics(client, metrics_buf, sizeof(metrics_buf));
    if (ret == MOONCAKE_OK) {
        printf("   ✓ Metrics:\n%s\n", metrics_buf);
    }

    printf("\n=== Example completed successfully! ===\n");

cleanup:
    // ========================================================================
    // Cleanup
    // ========================================================================
    printf("\nCleaning up resources...\n");
    
    if (segment_buffer) {
        mooncake_client_unmount_segment(client, segment_buffer, SEGMENT_SIZE);
        free(segment_buffer);
        printf("   ✓ Segment unmounted\n");
    }

    if (local_buffer) {
        mooncake_client_unregister_memory(client, local_buffer, 1);
        free(local_buffer);
        printf("   ✓ Local memory unregistered\n");
    }

    if (client) {
        mooncake_client_destroy(client);
        printf("   ✓ Client destroyed\n");
    }

    return ret == MOONCAKE_OK ? 0 : 1;
}
