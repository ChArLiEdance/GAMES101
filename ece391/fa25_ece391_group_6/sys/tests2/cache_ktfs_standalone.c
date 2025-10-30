#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "devimpl.h"
#include "error.h"
#include "fsimpl.h"
#include "ktfs.h"
#include "thread.h"
#include "uio.h"
#include "uioimpl.h"

int mount_ktfs(const char* name, struct cache* cache);

// ---------------------------------------------------------------------------
// Minimal kernel runtime stubs
// ---------------------------------------------------------------------------

void* kmalloc(size_t size) { return malloc(size); }

void* kcalloc(size_t nelts, size_t eltsz) { return calloc(nelts, eltsz); }

void kfree(void* ptr) { free(ptr); }

void panic_actual(const char* filename, int lineno, const char* msg) {
    fprintf(stderr, "panic at %s:%d: %s\n", filename, lineno, msg);
    abort();
}

void assert_failed(const char* filename, int lineno, const char* stmt) {
    fprintf(stderr, "assertion failed at %s:%d: %s\n", filename, lineno, stmt);
    abort();
}

static void log_message(const char* tag, const char* filename, int lineno, const char* fmt,
                        va_list ap) {
    fprintf(stderr, "[%s] %s:%d: ", tag, filename, lineno);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void debug_actual(const char* filename, int lineno, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_message("DEBUG", filename, lineno, fmt, ap);
    va_end(ap);
}

void trace_actual(const char* filename, int lineno, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_message("TRACE", filename, lineno, fmt, ap);
    va_end(ap);
}

void lock_init(struct lock* lock) {
    (void)lock;
}

void lock_acquire(struct lock* lock) {
    (void)lock;
}

void lock_release(struct lock* lock) {
    (void)lock;
}

int storage_open(struct storage* sto) {
    return (sto->intf->open != NULL) ? sto->intf->open(sto) : 0;
}

void storage_close(struct storage* sto) {
    if (sto->intf->close != NULL) sto->intf->close(sto);
}

long storage_fetch(struct storage* sto, unsigned long long pos, void* buf, unsigned long bytecnt) {
    if (sto->intf->fetch == NULL) return -ENOTSUP;
    return sto->intf->fetch(sto, pos, buf, bytecnt);
}

long storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                   unsigned long bytecnt) {
    if (sto->intf->store == NULL) return -ENOTSUP;
    return sto->intf->store(sto, pos, buf, bytecnt);
}

int storage_cntl(struct storage* sto, int op, void* arg) {
    if (sto->intf->cntl == NULL) return -ENOTSUP;
    return sto->intf->cntl(sto, op, arg);
}

unsigned int storage_blksz(const struct storage* sto) { return sto->intf->blksz; }

unsigned long long storage_capacity(const struct storage* sto) { return sto->capacity; }

// ---------------------------------------------------------------------------
// Minimal filesystem registry
// ---------------------------------------------------------------------------

struct mount_entry {
    const char* name;
    struct filesystem* fs;
};

static struct mount_entry mount_table[8];
static size_t mount_count;

int attach_filesystem(const char* mpname, struct filesystem* fs) {
    size_t i;

    if (mpname == NULL || fs == NULL) return -EINVAL;

    for (i = 0; i < mount_count; i++) {
        if (strcmp(mount_table[i].name, mpname) == 0) return -EEXIST;
    }
    if (mount_count >= sizeof(mount_table) / sizeof(mount_table[0])) return -EMFILE;

    mount_table[mount_count].name = mpname;
    mount_table[mount_count].fs = fs;
    mount_count++;
    return 0;
}

int open_file(const char* mpname, const char* flname, struct uio** uioptr) {
    size_t i;

    if (mpname == NULL || flname == NULL || uioptr == NULL) return -EINVAL;

    for (i = 0; i < mount_count; i++) {
        if (strcmp(mount_table[i].name, mpname) == 0) {
            return mount_table[i].fs->open(mount_table[i].fs, flname, uioptr);
        }
    }
    return -ENOENT;
}

// ---------------------------------------------------------------------------
// Stub block device and KTFS image helpers
// ---------------------------------------------------------------------------

#define STUB_BLKSZ 512U
#define STUB_TOTAL_BLOCKS 64U
#define STUB_CAPACITY (STUB_BLKSZ * STUB_TOTAL_BLOCKS)

struct stub_device {
    struct storage storage;
    unsigned char data[STUB_CAPACITY];
    unsigned int fetch_calls;
    unsigned int store_calls;
};

static const struct storage_intf stub_storage_intf;

static struct stub_device* sto_to_stub(struct storage* sto) {
    return (struct stub_device*)((char*)sto - offsetof(struct stub_device, storage));
}

static int stub_storage_open(struct storage* sto __attribute__((unused))) { return 0; }

static void stub_storage_close(struct storage* sto __attribute__((unused))) {}

static long stub_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                               unsigned long bytecnt) {
    struct stub_device* dev = sto_to_stub(sto);

    if (buf == NULL) return -EINVAL;
    if (pos % STUB_BLKSZ != 0) return -EINVAL;
    if (bytecnt % STUB_BLKSZ != 0) return -EINVAL;
    if (pos + bytecnt > STUB_CAPACITY) return -EINVAL;

    dev->fetch_calls++;
    memcpy(buf, dev->data + pos, bytecnt);
    return (long)bytecnt;
}

static long stub_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                               unsigned long bytecnt) {
    struct stub_device* dev = sto_to_stub(sto);

    if (buf == NULL) return -EINVAL;
    if (pos % STUB_BLKSZ != 0) return -EINVAL;
    if (bytecnt % STUB_BLKSZ != 0) return -EINVAL;
    if (pos + bytecnt > STUB_CAPACITY) return -EINVAL;

    dev->store_calls++;
    memcpy(dev->data + pos, buf, bytecnt);
    return (long)bytecnt;
}

static int stub_storage_cntl(struct storage* sto, int op, void* arg) {
    struct stub_device* dev = sto_to_stub(sto);
    unsigned long long* ullptr;

    switch (op) {
        case FCNTL_GETEND:
            if (arg == NULL) return -EINVAL;
            ullptr = arg;
            *ullptr = STUB_CAPACITY;
            return 0;
        default:
            return -ENOTSUP;
    }
}

static const struct storage_intf stub_storage_intf = {.blksz = STUB_BLKSZ,
                                                      .open = &stub_storage_open,
                                                      .close = &stub_storage_close,
                                                      .fetch = &stub_storage_fetch,
                                                      .store = &stub_storage_store,
                                                      .cntl = &stub_storage_cntl};

static void stub_device_init(struct stub_device* dev) {
    memset(dev, 0, sizeof(*dev));
    storage_init(&dev->storage, &stub_storage_intf, STUB_CAPACITY);
}

static void stub_populate_filesystem(struct stub_device* dev) {
    struct ktfs_superblock* super = (struct ktfs_superblock*)dev->data;
    struct ktfs_inode* inodes;
    struct ktfs_dir_entry* dirent;
    unsigned char* inode_bitmap = dev->data + STUB_BLKSZ;
    unsigned char* block_bitmap = dev->data + STUB_BLKSZ * 2;
    unsigned char* file_data;

    memset(dev->data, 0, sizeof(dev->data));

    super->block_count = STUB_TOTAL_BLOCKS;
    super->inode_bitmap_block_count = 1;
    super->bitmap_block_count = 1;
    super->inode_block_count = 1;
    super->root_directory_inode = 0;

    inode_bitmap[0] = 0x03;
    block_bitmap[0] = 0x3F;
    inodes = (struct ktfs_inode*)(dev->data + STUB_BLKSZ * 3);
    memset(inodes, 0, sizeof(struct ktfs_inode) * 16);

    inodes[0].size = sizeof(struct ktfs_dir_entry);
    inodes[0].block[0] = 4;

    inodes[1].size = 4;
    inodes[1].block[0] = 5;

    dirent = (struct ktfs_dir_entry*)(dev->data + STUB_BLKSZ * 4);
    memset(dirent, 0, sizeof(*dirent));
    dirent->inode = 1;
    strncpy(dirent->name, "hello", KTFS_MAX_FILENAME_LEN);
    dirent->name[KTFS_MAX_FILENAME_LEN] = '\0';

    file_data = dev->data + STUB_BLKSZ * 5;
    memcpy(file_data, "TEST", 4);
}

static void stub_populate_complex_filesystem(struct stub_device* dev) {
    struct ktfs_superblock* super = (struct ktfs_superblock*)dev->data;
    struct ktfs_inode* inodes;
    struct ktfs_dir_entry* dirents;
    unsigned char* inode_bitmap = dev->data + STUB_BLKSZ;
    unsigned char* block_bitmap = dev->data + STUB_BLKSZ * 2;
    uint32_t* indirect_table;
    uint32_t* dindirect_l1;
    uint32_t* dindirect_l2;
    unsigned long long dindirect_offset;
    unsigned char* blk;
    unsigned int i;

    memset(dev->data, 0, sizeof(dev->data));

    super->block_count = STUB_TOTAL_BLOCKS;
    super->inode_bitmap_block_count = 1;
    super->bitmap_block_count = 1;
    super->inode_block_count = 1;
    super->root_directory_inode = 0;

    inode_bitmap[0] = 0x0F;
    block_bitmap[0] = 0xFF;
    block_bitmap[1] = 0x7F;

    inodes = (struct ktfs_inode*)(dev->data + STUB_BLKSZ * 3);
    memset(inodes, 0, sizeof(struct ktfs_inode) * 16);

    inodes[0].size = sizeof(struct ktfs_dir_entry) * 3;
    inodes[0].block[0] = 4;

    inodes[1].size = 4;
    inodes[1].block[0] = 5;

    inodes[2].size = KTFS_BLKSZ * 5;
    inodes[2].block[0] = 6;
    inodes[2].block[1] = 7;
    inodes[2].block[2] = 8;
    inodes[2].block[3] = 9;
    inodes[2].indirect = 11;

    dindirect_offset =
        (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ / sizeof(uint32_t))) * (unsigned long long)KTFS_BLKSZ;
    inodes[3].size = dindirect_offset + 16;
    inodes[3].dindirect[0] = 12;

    dirents = (struct ktfs_dir_entry*)(dev->data + STUB_BLKSZ * 4);
    memset(dirents, 0, STUB_BLKSZ);
    dirents[0].inode = 1;
    strncpy(dirents[0].name, "hello", KTFS_MAX_FILENAME_LEN);
    dirents[0].name[KTFS_MAX_FILENAME_LEN] = '\0';
    dirents[1].inode = 2;
    strncpy(dirents[1].name, "indirect", KTFS_MAX_FILENAME_LEN);
    dirents[1].name[KTFS_MAX_FILENAME_LEN] = '\0';
    dirents[2].inode = 3;
    strncpy(dirents[2].name, "dindir", KTFS_MAX_FILENAME_LEN);
    dirents[2].name[KTFS_MAX_FILENAME_LEN] = '\0';

    blk = dev->data + STUB_BLKSZ * 5;
    memcpy(blk, "TEST", 4);

    for (i = 0; i < 4; i++) {
        blk = dev->data + STUB_BLKSZ * (6 + i);
        memset(blk, 'A' + (int)i, STUB_BLKSZ);
    }

    blk = dev->data + STUB_BLKSZ * 10;
    memset(blk, 'E', STUB_BLKSZ);

    indirect_table = (uint32_t*)(dev->data + STUB_BLKSZ * 11);
    memset(indirect_table, 0, STUB_BLKSZ);
    indirect_table[0] = 10;

    dindirect_l1 = (uint32_t*)(dev->data + STUB_BLKSZ * 12);
    memset(dindirect_l1, 0, STUB_BLKSZ);
    dindirect_l1[0] = 13;

    dindirect_l2 = (uint32_t*)(dev->data + STUB_BLKSZ * 13);
    memset(dindirect_l2, 0, STUB_BLKSZ);
    dindirect_l2[0] = 14;

    blk = dev->data + STUB_BLKSZ * 14;
    memset(blk, 'Z', STUB_BLKSZ);
    memcpy(blk, "DOUBLE-INDIRECT!", 16);
}

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

static int failures;

static void report_result(const char* name, int result) {
    printf("[%-28s] %s\n", name, (result == 0) ? "PASS" : "FAIL");
    if (result != 0) failures++;
}

static struct stub_device global_stub;

static int test_cache_create_invalid(void) {
    struct stub_device* dev = &global_stub;
    struct cache* cache = NULL;
    int result;

    stub_device_init(dev);

    result = create_cache(NULL, &cache);
    if (result != -EINVAL) return -EINVAL;

    result = create_cache(&dev->storage, NULL);
    if (result != -EINVAL) return -EINVAL;

    result = create_cache(&dev->storage, &cache);
    return result;
}

static int test_cache_basic_fetch(void) {
    struct stub_device* dev = &global_stub;
    struct cache* cache = NULL;
    void* blk = NULL;
    unsigned long i;
    int result;

    stub_device_init(dev);
    for (i = 0; i < STUB_CAPACITY; i++) dev->data[i] = (unsigned char)(i & 0xFF);

    result = create_cache(&dev->storage, &cache);
    if (result != 0) return result;

    result = cache_get_block(cache, 0, &blk);
    if (result != 0) return result;
    if (blk == NULL) return -EINVAL;
    if (memcmp(blk, dev->data, STUB_BLKSZ) != 0) return -EINVAL;
    if (dev->fetch_calls != 1) return -EINVAL;

    cache_release_block(cache, blk, 0);
    return 0;
}

static int test_cache_hit_reuses_block(void) {
    struct stub_device* dev = &global_stub;
    struct cache* cache = NULL;
    void* first = NULL;
    void* second = NULL;
    int result;

    stub_device_init(dev);
    memset(dev->data, 0x5A, sizeof(dev->data));

    result = create_cache(&dev->storage, &cache);
    if (result != 0) return result;

    result = cache_get_block(cache, 0, &first);
    if (result != 0) return result;
    cache_release_block(cache, first, 0);

    result = cache_get_block(cache, 0, &second);
    if (result != 0) return result;
    if (first != second) return -EINVAL;
    if (dev->fetch_calls != 1) return -EINVAL;

    cache_release_block(cache, second, 0);
    return 0;
}

static int test_cache_dirty_flush(void) {
    struct stub_device* dev = &global_stub;
    struct cache* cache = NULL;
    void* blk = NULL;
    unsigned char* bytes;
    int result;

    stub_device_init(dev);
    memset(dev->data, 0, sizeof(dev->data));

    result = create_cache(&dev->storage, &cache);
    if (result != 0) return result;

    result = cache_get_block(cache, 0, &blk);
    if (result != 0) return result;

    bytes = blk;
    bytes[0] = 0xAA;
    bytes[1] = 0x55;

    cache_release_block(cache, blk, 1);

    result = cache_flush(cache);
    if (result != 0) return result;
    if (dev->store_calls != 1) return -EINVAL;
    if (dev->data[0] != 0xAA || dev->data[1] != 0x55) return -EINVAL;

    return 0;
}

static int test_cache_flush_busy_reference(void) {
    struct stub_device* dev = &global_stub;
    struct cache* cache = NULL;
    void* blk = NULL;
    void* same = NULL;
    unsigned char* bytes;
    int result;

    stub_device_init(dev);
    memset(dev->data, 0, sizeof(dev->data));

    result = create_cache(&dev->storage, &cache);
    if (result != 0) return result;

    result = cache_get_block(cache, 0, &blk);
    if (result != 0) return result;

    result = cache_get_block(cache, 0, &same);
    if (result != 0) return result;
    if (blk != same) return -EINVAL;

    bytes = blk;
    bytes[0] = 0x11;
    bytes[1] = 0x22;

    cache_release_block(cache, blk, 1);

    result = cache_flush(cache);
    if (result != -EBUSY) return -EINVAL;
    if (dev->store_calls != 0) return -EINVAL;

    cache_release_block(cache, same, 0);

    result = cache_flush(cache);
    if (result != 0) return result;
    if (dev->store_calls != 1) return -EINVAL;
    if (dev->data[0] != 0x11 || dev->data[1] != 0x22) return -EINVAL;

    return 0;
}

static int test_cache_misaligned_access(void) {
    struct stub_device* dev = &global_stub;
    struct cache* cache = NULL;
    void* blk = NULL;
    int result;

    stub_device_init(dev);
    result = create_cache(&dev->storage, &cache);
    if (result != 0) return result;

    result = cache_get_block(cache, STUB_BLKSZ / 2, &blk);
    if (result != -EINVAL) return -EINVAL;

    result = cache_get_block(cache, 0, NULL);
    if (result != -EINVAL) return -EINVAL;

    return 0;
}

static int test_cache_eviction_lru(void) {
    struct stub_device* dev = &global_stub;
    struct cache* cache = NULL;
    void* blk = NULL;
    unsigned int i;
    int result;

    stub_device_init(dev);
    result = create_cache(&dev->storage, &cache);
    if (result != 0) return result;

    for (i = 0; i < 64; i++) {
        result = cache_get_block(cache, (unsigned long long)i * STUB_BLKSZ, &blk);
        if (result != 0) return result;
        cache_release_block(cache, blk, 0);
    }
    if (dev->fetch_calls != 64) return -EINVAL;

    result = cache_get_block(cache, 64ULL * STUB_BLKSZ, &blk);
    if (result != 0) return result;
    cache_release_block(cache, blk, 0);
    if (dev->fetch_calls != 65) return -EINVAL;

    result = cache_get_block(cache, 0, &blk);
    if (result != 0) return result;
    cache_release_block(cache, blk, 0);
    if (dev->fetch_calls != 66) return -EINVAL;

    return 0;
}

static int test_ktfs_open_and_read(void) {
    struct stub_device* dev = &global_stub;
    struct cache* cache = NULL;
    struct uio* file = NULL;
    char buffer[5] = {0};
    unsigned long long value;
    int result;

    stub_device_init(dev);
    stub_populate_filesystem(dev);

    result = create_cache(&dev->storage, &cache);
    if (result != 0) return result;

    result = mount_ktfs("standalone", cache);
    if (result != 0) return result;

    result = open_file("standalone", "hello", &file);
    if (result != 0) return result;

    result = (int)uio_read(file, buffer, 4);
    if (result != 4) {
        uio_close(file);
        return (result < 0) ? result : -EINVAL;
    }
    buffer[4] = '\0';
    if (strcmp(buffer, "TEST") != 0) {
        uio_close(file);
        return -EINVAL;
    }

    value = 0;
    result = uio_cntl(file, FCNTL_GETPOS, &value);
    if (result != 0 || value != 4ULL) {
        uio_close(file);
        return -EINVAL;
    }

    value = 0;
    result = uio_cntl(file, FCNTL_GETEND, &value);
    if (result != 0 || value != 4ULL) {
        uio_close(file);
        return -EINVAL;
    }

    uio_close(file);
    return 0;
}

static int test_ktfs_open_invalid(void) {
    struct stub_device* dev = &global_stub;
    struct cache* cache = NULL;
    struct uio* file = NULL;
    int result;

    stub_device_init(dev);
    stub_populate_filesystem(dev);

    result = create_cache(&dev->storage, &cache);
    if (result != 0) return result;

    result = mount_ktfs("badfs", cache);
    if (result != 0) return result;

    result = open_file("badfs", "", &file);
    if (result != -ENOTSUP) return -EINVAL;

    result = open_file("badfs", "\\", &file);
    if (result != -ENOTSUP) return -EINVAL;

    result = open_file("badfs", "missing", &file);
    if (result != -ENOENT) return -EINVAL;

    result = open_file("badfs", "hello", &file);
    if (result != 0) return result;

    uio_close(file);
    return 0;
}

static int test_ktfs_cntl_setpos(void) {
    struct stub_device* dev = &global_stub;
    struct cache* cache = NULL;
    struct uio* file = NULL;
    unsigned long long pos;
    int result;

    stub_device_init(dev);
    stub_populate_filesystem(dev);

    result = create_cache(&dev->storage, &cache);
    if (result != 0) return result;

    result = mount_ktfs("cntlfs", cache);
    if (result != 0) return result;

    result = open_file("cntlfs", "hello", &file);
    if (result != 0) return result;

    pos = 2;
    result = uio_cntl(file, FCNTL_SETPOS, &pos);
    if (result != 0) {
        uio_close(file);
        return result;
    }

    pos = 5;
    result = uio_cntl(file, FCNTL_SETPOS, &pos);
    if (result != -EINVAL) {
        uio_close(file);
        return -EINVAL;
    }

    uio_close(file);
    return 0;
}

static int test_ktfs_read_indirect(void) {
    struct stub_device* dev = &global_stub;
    struct cache* cache = NULL;
    struct uio* file = NULL;
    unsigned long long pos;
    char buffer[16];
    int result;
    unsigned int i;

    stub_device_init(dev);
    stub_populate_complex_filesystem(dev);

    result = create_cache(&dev->storage, &cache);
    if (result != 0) return result;

    result = mount_ktfs("indirfs", cache);
    if (result != 0) return result;

    result = open_file("indirfs", "indirect", &file);
    if (result != 0) return result;

    pos = KTFS_BLKSZ * 4ULL - 8ULL;
    result = uio_cntl(file, FCNTL_SETPOS, &pos);
    if (result != 0) {
        uio_close(file);
        return result;
    }

    result = (int)uio_read(file, buffer, sizeof(buffer));
    if (result != (int)sizeof(buffer)) {
        uio_close(file);
        return (result < 0) ? result : -EINVAL;
    }
    for (i = 0; i < 8; i++) {
        if (buffer[i] != 'D') {
            uio_close(file);
            return -EINVAL;
        }
    }
    for (i = 8; i < 16; i++) {
        if (buffer[i] != 'E') {
            uio_close(file);
            return -EINVAL;
        }
    }

    uio_close(file);
    return 0;
}

static int test_ktfs_read_double_indirect(void) {
    struct stub_device* dev = &global_stub;
    struct cache* cache = NULL;
    struct uio* file = NULL;
    unsigned long long pos;
    char buffer[16] = {0};
    int result;

    stub_device_init(dev);
    stub_populate_complex_filesystem(dev);

    result = create_cache(&dev->storage, &cache);
    if (result != 0) return result;

    result = mount_ktfs("dindir", cache);
    if (result != 0) return result;

    result = open_file("dindir", "dindir", &file);
    if (result != 0) return result;

    pos = (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ / sizeof(uint32_t))) * (unsigned long long)KTFS_BLKSZ;
    result = uio_cntl(file, FCNTL_SETPOS, &pos);
    if (result != 0) {
        uio_close(file);
        return result;
    }

    result = (int)uio_read(file, buffer, sizeof(buffer));
    if (result != (int)sizeof(buffer)) {
        uio_close(file);
        return (result < 0) ? result : -EINVAL;
    }
    if (memcmp(buffer, "DOUBLE-INDIRECT!", sizeof(buffer)) != 0) {
        uio_close(file);
        return -EINVAL;
    }

    uio_close(file);
    return 0;
}

int main(void) {
    failures = 0;

    report_result("cache_create_invalid", test_cache_create_invalid());
    report_result("cache_basic_fetch", test_cache_basic_fetch());
    report_result("cache_hit_reuses_block", test_cache_hit_reuses_block());
    report_result("cache_dirty_flush", test_cache_dirty_flush());
    report_result("cache_flush_busy_reference", test_cache_flush_busy_reference());
    report_result("cache_misaligned_access", test_cache_misaligned_access());
    report_result("cache_eviction_lru", test_cache_eviction_lru());
    report_result("ktfs_open_and_read", test_ktfs_open_and_read());
    report_result("ktfs_open_invalid", test_ktfs_open_invalid());
    report_result("ktfs_cntl_setpos", test_ktfs_cntl_setpos());
    report_result("ktfs_read_indirect", test_ktfs_read_indirect());

    // Note: This test will expose current double-indirect limitations in ktfs.c.
    report_result("ktfs_read_double_indirect", test_ktfs_read_double_indirect());

    if (failures == 0)
        printf("All standalone tests passed.\n");
    else
        printf("%d standalone test(s) failed.\n", failures);

    return (failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
