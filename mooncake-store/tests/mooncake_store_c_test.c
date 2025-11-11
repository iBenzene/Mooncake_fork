// Simple compilation test for mooncake_store_c.h
#include "mooncake_store_c.h"

#include <stdio.h>

int main() {
    printf("Mooncake Store C API - Compilation Test\n");
    printf("========================================\n\n");

    // Test 1: Check error messages
    printf("Test 1: Error Messages\n");
    printf("  MOONCAKE_OK: %s\n", mooncake_error_message(MOONCAKE_OK));
    printf("  MOONCAKE_OBJECT_NOT_FOUND: %s\n",
           mooncake_error_message(MOONCAKE_OBJECT_NOT_FOUND));
    printf("  MOONCAKE_INVALID_PARAMS: %s\n",
           mooncake_error_message(MOONCAKE_INVALID_PARAMS));
    printf("\n");

    // Test 2: Check config initialization
    printf("Test 2: Config Initialization\n");
    mooncake_replicate_config_t config;
    mooncake_replicate_config_init(&config);
    printf("  Default replica_num: %zu\n", config.replica_num);
    printf("  Default with_soft_pin: %d\n", config.with_soft_pin);
    printf("  Default preferred_segment: %s\n",
           config.preferred_segment == NULL ? "NULL" : config.preferred_segment);
    printf("\n");

    // Test 3: Check slice structure
    printf("Test 3: Slice Structure\n");
    mooncake_slice_t slice;
    slice.ptr = NULL;
    slice.size = 0;
    printf("  Slice initialized: ptr=%p, size=%zu\n", slice.ptr, slice.size);
    printf("\n");

    printf("All compilation tests passed!\n");
    printf("Note: This only tests compilation, not runtime functionality.\n");
    printf("Run mooncake_store_c_example for full functional tests.\n");

    return 0;
}
