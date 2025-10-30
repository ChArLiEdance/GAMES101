#include "testsuite_1.h"

#include <stddef.h>

#include "cache.h"
#include "console.h"
#include "devimpl.h"
#include "error.h"
#include "filesys.h"
#include "ktfs.h"
#include "string.h"
#include "uio.h"

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

    inode_bitmap[0] = 0x03;                // inodes 0 and 1 in use
    block_bitmap[0] = 0x3F;                // blocks 0-5 in use
    inodes = (struct ktfs_inode*)(dev->data + STUB_BLKSZ * 3);
    memset(inodes, 0, sizeof(struct ktfs_inode) * 16);

    inodes[0].size = sizeof(struct ktfs_dir_entry);
    inodes[0].block[0] = 4;  // directory data stored in block 4

    inodes[1].size = 4;
    inodes[1].block[0] = 5;  // file data stored in block 5

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

    inode_bitmap[0] = 0x0F;  // inodes 0-3 in use
    block_bitmap[0] = 0xFF;  // blocks 0-7 in use
    block_bitmap[1] = 0x7F;  // blocks 8-14 in use

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

static int run_single_test(const char* name, int (*fn)(void)) {
    int ret = fn();
    kprintf("[%-28s] %s\n", name, (ret == 0) ? "PASS" : "FAIL");
    return ret;
}

static int test_cache_create_invalid(void);
static int test_cache_basic_fetch(void);
static int test_cache_hit_reuses_block(void);
static int test_cache_dirty_flush(void);
static int test_cache_flush_busy_reference(void);
static int test_cache_misaligned_access(void);
static int test_cache_eviction_lru(void);
static int test_ktfs_open_and_read(void);
static int test_ktfs_open_invalid(void);
static int test_ktfs_cntl_setpos(void);
static int test_ktfs_read_indirect(void);
static int test_ktfs_read_double_indirect(void);

void run_testsuite_1() {
    int failures = 0;

    failures += (run_single_test("cache_create_invalid", &test_cache_create_invalid) != 0);
    failures += (run_single_test("cache_basic_fetch", &test_cache_basic_fetch) != 0);
    failures += (run_single_test("cache_hit_reuses_block", &test_cache_hit_reuses_block) != 0);
    failures += (run_single_test("cache_dirty_flush", &test_cache_dirty_flush) != 0);
    failures +=
        (run_single_test("cache_flush_busy_reference", &test_cache_flush_busy_reference) != 0);
    failures += (run_single_test("cache_misaligned_access", &test_cache_misaligned_access) != 0);
    failures += (run_single_test("cache_eviction_lru", &test_cache_eviction_lru) != 0);
    failures += (run_single_test("ktfs_open_and_read", &test_ktfs_open_and_read) != 0);
    failures += (run_single_test("ktfs_open_invalid", &test_ktfs_open_invalid) != 0);
    failures += (run_single_test("ktfs_cntl_setpos", &test_ktfs_cntl_setpos) != 0);
    failures += (run_single_test("ktfs_read_indirect", &test_ktfs_read_indirect) != 0);
    failures += (run_single_test("ktfs_read_double_indirect", &test_ktfs_read_double_indirect) != 0);

    if (failures == 0)
        kprintf("All kernel unit tests passed.\n");
    else
        kprintf("%d kernel unit test(s) failed.\n", failures);
}

static struct stub_device global_stub;

static int test_cache_create_invalid(void) {
    struct stub_device* dev = &global_stub;
    struct cache* cache = NULL;
    int result;

    stub_device_init(dev);

    result = create_cache(NULL, &cache);
    if (result != -EINVAL) {
        kprintf("expected create_cache(NULL, ...) to fail with -EINVAL, got %s\n", error_name(result));
        return -EINVAL;
    }

    result = create_cache(&dev->storage, NULL);
    if (result != -EINVAL) {
        kprintf("expected create_cache(..., NULL) to fail with -EINVAL, got %s\n", error_name(result));
        return -EINVAL;
    }

    result = create_cache(&dev->storage, &cache);
    if (result != 0) {
        kprintf("create_cache returned %s\n", error_name(result));
        return result;
    }

    return 0;
}

static int test_cache_basic_fetch(void) {
    struct stub_device* dev = &global_stub;
    struct cache* cache = NULL;
    void* blk = NULL;
    int result;
    unsigned long i;

    stub_device_init(dev);
    for (i = 0; i < STUB_CAPACITY; i++) dev->data[i] = (unsigned char)(i & 0xFF);

    result = create_cache(&dev->storage, &cache);
    if (result != 0) return result;

    result = cache_get_block(cache, 0, &blk);
    if (result != 0) {
        kprintf("cache_get_block failed: %s\n", error_name(result));
        return result;
    }
    if (blk == NULL) {
        kprintf("cache_get_block returned NULL block pointer\n");
        return -EINVAL;
    }
    if (memcmp(blk, dev->data, STUB_BLKSZ) != 0) {
        kprintf("cache_get_block data mismatch\n");
        return -EINVAL;
    }
    if (dev->fetch_calls != 1) {
        kprintf("expected one fetch call, observed %u\n", dev->fetch_calls);
        return -EINVAL;
    }

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
    if (first != second) {
        kprintf("cache reused a different buffer instance\n");
        cache_release_block(cache, second, 0);
        return -EINVAL;
    }
    if (dev->fetch_calls != 1) {
        kprintf("expected a cache hit without extra fetches, observed %u\n", dev->fetch_calls);
        cache_release_block(cache, second, 0);
        return -EINVAL;
    }

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
    if (result != 0) {
        kprintf("cache_flush failed: %s\n", error_name(result));
        return result;
    }

    if (dev->store_calls != 1) {
        kprintf("expected one store call after flush, observed %u\n", dev->store_calls);
        return -EINVAL;
    }
    if (dev->data[0] != 0xAA || dev->data[1] != 0x55) {
        kprintf("flush did not propagate modified bytes\n");
        return -EINVAL;
    }

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
    if (blk != same) {
        kprintf("expected identical pointer on re-fetch\n");
        cache_release_block(cache, same, 0);
        cache_release_block(cache, blk, 0);
        return -EINVAL;
    }

    bytes = blk;
    bytes[0] = 0x11;
    bytes[1] = 0x22;

    cache_release_block(cache, blk, 1);

    result = cache_flush(cache);
    if (result != -EBUSY) {
        kprintf("expected cache_flush to report -EBUSY while block pinned, got %s\n",
                error_name(result));
        cache_release_block(cache, same, 0);
        return -EINVAL;
    }
    if (dev->store_calls != 0) {
        kprintf("flush wrote back data while block still referenced\n");
        cache_release_block(cache, same, 0);
        return -EINVAL;
    }

    cache_release_block(cache, same, 0);

    result = cache_flush(cache);
    if (result != 0) {
        kprintf("cache_flush after release failed: %s\n", error_name(result));
        return result;
    }
    if (dev->store_calls != 1) {
        kprintf("expected one store after successful flush, observed %u\n", dev->store_calls);
        return -EINVAL;
    }
    if (dev->data[0] != 0x11 || dev->data[1] != 0x22) {
        kprintf("cache_flush did not persist dirty bytes\n");
        return -EINVAL;
    }

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
    if (result != -EINVAL) {
        kprintf("misaligned cache_get_block should fail with -EINVAL, got %s\n", error_name(result));
        return -EINVAL;
    }

    result = cache_get_block(cache, 0, NULL);
    if (result != -EINVAL) {
        kprintf("cache_get_block with NULL pptr should fail with -EINVAL, got %s\n", error_name(result));
        return -EINVAL;
    }

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
    if (dev->fetch_calls != 64) {
        kprintf("expected 64 fetches, observed %u\n", dev->fetch_calls);
        return -EINVAL;
    }

    result = cache_get_block(cache, 64ULL * STUB_BLKSZ, &blk);
    if (result != 0) return result;
    cache_release_block(cache, blk, 0);

    if (dev->fetch_calls != 65) {
        kprintf("fetch count after adding block 64 should be 65, observed %u\n", dev->fetch_calls);
        return -EINVAL;
    }

    result = cache_get_block(cache, 0, &blk);
    if (result != 0) return result;
    cache_release_block(cache, blk, 0);

    if (dev->fetch_calls != 66) {
        kprintf("expected refetch after eviction, observed %u\n", dev->fetch_calls);
        return -EINVAL;
    }

    return 0;
}

static int test_ktfs_open_and_read(void) {
    struct stub_device* dev = &global_stub;
    struct cache* cache = NULL;
    struct uio* file = NULL;
    char buffer[5] = {0};
    unsigned long long value = 0;
    int result;

    stub_device_init(dev);
    stub_populate_filesystem(dev);

    result = create_cache(&dev->storage, &cache);
    if (result != 0) return result;

    result = mount_ktfs("testfs", cache);
    if (result != 0) {
        kprintf("mount_ktfs failed: %s\n", error_name(result));
        return result;
    }

    result = open_file("testfs", "hello", &file);
    if (result != 0) {
        kprintf("open_file failed: %s\n", error_name(result));
        return result;
    }

    result = (int)uio_read(file, buffer, 4);
    if (result != 4) {
        kprintf("uio_read returned %s\n", error_name(result));
        uio_close(file);
        return (result < 0) ? result : -EINVAL;
    }
    buffer[4] = '\0';
    if (strcmp(buffer, "TEST") != 0) {
        kprintf("expected \"TEST\", got \"%s\"\n", buffer);
        uio_close(file);
        return -EINVAL;
    }

    value = 0;
    result = uio_cntl(file, FCNTL_GETPOS, &value);
    if (result != 0 || value != 4ULL) {
        kprintf("FCNTL_GETPOS failed\n");
        uio_close(file);
        return -EINVAL;
    }

    value = 0;
    result = uio_cntl(file, FCNTL_GETEND, &value);
    if (result != 0 || value != 4ULL) {
        kprintf("FCNTL_GETEND failed\n");
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

    result = mount_ktfs("fs_invalid", cache);
    if (result != 0) return result;

    result = open_file("fs_invalid", "", &file);
    if (result != -ENOTSUP) {
        kprintf("expected empty filename to be rejected, got %s\n", error_name(result));
        return -EINVAL;
    }

    result = open_file("fs_invalid", "\\", &file);
    if (result != -ENOTSUP) {
        kprintf("expected root listing to be unsupported, got %s\n", error_name(result));
        return -EINVAL;
    }

    result = open_file("fs_invalid", "missing", &file);
    if (result != -ENOENT) {
        kprintf("expected open of missing file to fail with -ENOENT, got %s\n", error_name(result));
        return -EINVAL;
    }

    result = open_file("fs_invalid", "hello", &file);
    if (result != 0) {
        kprintf("open_file on valid file failed: %s\n", error_name(result));
        return result;
    }

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

    result = mount_ktfs("fs_cntl", cache);
    if (result != 0) return result;

    result = open_file("fs_cntl", "hello", &file);
    if (result != 0) {
        kprintf("open_file failed: %s\n", error_name(result));
        return result;
    }

    pos = 2;
    result = uio_cntl(file, FCNTL_SETPOS, &pos);
    if (result != 0) {
        kprintf("FCNTL_SETPOS with in-range value failed: %s\n", error_name(result));
        uio_close(file);
        return result;
    }

    pos = 5;
    result = uio_cntl(file, FCNTL_SETPOS, &pos);
    if (result != -EINVAL) {
        kprintf("FCNTL_SETPOS should reject positions past EOF, got %s\n", error_name(result));
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

    result = mount_ktfs("fs_indirect", cache);
    if (result != 0) return result;

    result = open_file("fs_indirect", "indirect", &file);
    if (result != 0) {
        kprintf("open_file on indirect file failed: %s\n", error_name(result));
        return result;
    }

    pos = KTFS_BLKSZ * 4ULL - 8ULL;
    result = uio_cntl(file, FCNTL_SETPOS, &pos);
    if (result != 0) {
        kprintf("FCNTL_SETPOS near block boundary failed: %s\n", error_name(result));
        uio_close(file);
        return result;
    }

    result = (int)uio_read(file, buffer, sizeof(buffer));
    if (result != (int)sizeof(buffer)) {
        kprintf("uio_read across block boundary failed: %s\n", error_name(result));
        uio_close(file);
        return (result < 0) ? result : -EINVAL;
    }
    for (i = 0; i < 8; i++) {
        if (buffer[i] != 'D') {
            kprintf("expected 'D' in direct block tail, saw 0x%02x\n", (unsigned char)buffer[i]);
            uio_close(file);
            return -EINVAL;
        }
    }
    for (i = 8; i < 16; i++) {
        if (buffer[i] != 'E') {
            kprintf("expected 'E' in indirect block head, saw 0x%02x\n", (unsigned char)buffer[i]);
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

    result = mount_ktfs("fs_dindir", cache);
    if (result != 0) return result;

    result = open_file("fs_dindir", "dindir", &file);
    if (result != 0) {
        kprintf("open_file on double-indirect file failed: %s\n", error_name(result));
        return result;
    }

    pos = (KTFS_NUM_DIRECT_DATA_BLOCKS + (KTFS_BLKSZ / sizeof(uint32_t))) * (unsigned long long)KTFS_BLKSZ;
    result = uio_cntl(file, FCNTL_SETPOS, &pos);
    if (result != 0) {
        kprintf("FCNTL_SETPOS to double-indirect region failed: %s\n", error_name(result));
        uio_close(file);
        return result;
    }

    result = (int)uio_read(file, buffer, sizeof(buffer));
    if (result != (int)sizeof(buffer)) {
        kprintf("uio_read from double-indirect region failed: %s\n", error_name(result));
        uio_close(file);
        return (result < 0) ? result : -EINVAL;
    }
    if (memcmp(buffer, "DOUBLE-INDIRECT!", sizeof(buffer)) != 0) {
        kprintf("double-indirect data mismatch\n");
        uio_close(file);
        return -EINVAL;
    }

    uio_close(file);
    return 0;
}
