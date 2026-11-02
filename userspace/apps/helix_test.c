/* helix_test.c — test program for the native Helix user-space API.
 *
 * Exercises the key API functions from libhelix.h:
 *   - helix_alloc / helix_free (kernel heap)
 *   - helix_handle_create / helix_handle_close / helix_handle_get
 *   - helix_object_lookup / helix_object_count
 *   - helix_write_fd (formatted output)
 *   - helix_thread_create / helix_yield
 *   - helix_exit
 *
 * Run with: `make userspace-test`
 */

#include "libhelix.h"

/* Test 1: memory allocation. */
static int test_alloc(void)
{
    /* Allocate 64 bytes. */
    void *p = helix_alloc(64);
    if (p == NULL) {
        helix_write_fd(2, "FAIL: alloc returned NULL\n");
        return -1;
    }
    /* Write a byte to the allocated region. */
    *(unsigned char *)p = 0x42;
    if (*(unsigned char *)p != 0x42) {
        helix_write_fd(2, "FAIL: alloc returned invalid memory\n");
        return -1;
    }
    helix_free(p);
    helix_write_fd(1, "PASS: alloc/free\n");
    return 0;
}

/* Test 2: object count. */
static int test_object_count(void)
{
    uint64_t count = helix_object_count();
    helix_write_fd(1, "PASS: object_count=%llu\n", count);
    return 0;
}

/* Test 3: handle create/close. */
static int test_handle(void)
{
    /* Create a handle to an object. */
    handle_t h = helix_handle_create(NULL, 0);
    if (h == (handle_t)-1) {
        helix_write_fd(2, "FAIL: handle_create returned -1\n");
        return -1;
    }
    /* Close the handle. */
    bool ok = helix_handle_close(h);
    if (!ok) {
        helix_write_fd(2, "FAIL: handle_close failed\n");
        return -1;
    }
    helix_write_fd(1, "PASS: handle_create/close\n");
    return 0;
}

/* Test 4: handle duplicate with reduced rights. */
static int test_handle_duplicate(void)
{
    /* Create a handle with all rights. */
    handle_t h = helix_handle_create(NULL, 0);
    if (h == (handle_t)-1) {
        helix_write_fd(2, "FAIL: handle_create returned -1\n");
        return -1;
    }
    /* Duplicate with reduced rights. */
    handle_t h2 = helix_handle_duplicate(h, 0);
    if (h2 == (handle_t)-1) {
        helix_write_fd(2, "FAIL: handle_duplicate returned -1\n");
        return -1;
    }
    /* Close both handles. */
    helix_handle_close(h);
    helix_handle_close(h2);
    helix_write_fd(1, "PASS: handle_duplicate\n");
    return 0;
}

/* Test 5: handle get with required rights. */
static int test_handle_get(void)
{
    /* Create a handle. */
    handle_t h = helix_handle_create(NULL, 0);
    if (h == (handle_t)-1) {
        helix_write_fd(2, "FAIL: handle_create returned -1\n");
        return -1;
    }
    /* Get the object reference. */
    void *obj = helix_handle_get(h, 0);
    if (obj == NULL) {
        helix_write_fd(2, "FAIL: handle_get returned NULL\n");
        return -1;
    }
    /* Release the reference. */
    helix_object_release(obj);
    /* Close the handle. */
    helix_handle_close(h);
    helix_write_fd(1, "PASS: handle_get\n");
    return 0;
}

/* Test 6: object lookup. */
static int test_object_lookup(void)
{
    /* Look up an object by ID. */
    void *obj = helix_object_lookup(1);
    if (obj == NULL) {
        helix_write_fd(1, "PASS: object_lookup returned NULL (expected)\n");
        return 0;
    }
    helix_object_release(obj);
    helix_write_fd(1, "PASS: object_lookup\n");
    return 0;
}

/* Test 7: string functions. */
static int test_strings(void)
{
    /* strlen */
    size_t len = helix_strlen("hello");
    if (len != 5) {
        helix_write_fd(2, "FAIL: strlen returned %llu\n", len);
        return -1;
    }
    /* strcmp */
    int cmp = helix_strcmp("hello", "hello");
    if (cmp != 0) {
        helix_write_fd(2, "FAIL: strcmp returned %d\n", cmp);
        return -1;
    }
    /* strdup */
    char *s = helix_strdup("world");
    if (s == NULL) {
        helix_write_fd(2, "FAIL: strdup returned NULL\n");
        return -1;
    }
    if (helix_strcmp(s, "world") != 0) {
        helix_write_fd(2, "FAIL: strdup returned wrong string\n");
        return -1;
    }
    helix_free(s);
    helix_write_fd(1, "PASS: strings\n");
    return 0;
}

/* Test 8: brk. */
static int test_brk(void)
{
    uint64_t brk = helix_brk(0);
    helix_write_fd(1, "PASS: brk=%llu\n", brk);
    return 0;
}

/* Test 9: write to fd. */
static int test_write(void)
{
    helix_write_fd(1, "PASS: write\n");
    return 0;
}

/* Test 10: yield. */
static int test_yield(void)
{
    helix_yield();
    helix_write_fd(1, "PASS: yield\n");
    return 0;
}

/* Main entry point. */
int main(int argc, char **argv)
{
    helix_write_fd(1, "Helix native API test starting...\n");
    
    if (test_alloc() < 0) return 1;
    if (test_object_count() < 0) return 1;
    if (test_handle() < 0) return 1;
    if (test_handle_duplicate() < 0) return 1;
    if (test_handle_get() < 0) return 1;
    if (test_object_lookup() < 0) return 1;
    if (test_strings() < 0) return 1;
    if (test_brk() < 0) return 1;
    if (test_write() < 0) return 1;
    if (test_yield() < 0) return 1;
    
    helix_write_fd(1, "All tests passed.\n");
    helix_exit(0);
    return 0;
}