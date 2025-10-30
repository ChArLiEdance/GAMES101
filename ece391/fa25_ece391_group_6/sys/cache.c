/*! @file cache.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‍‌⁠​⁠⁠‌​⁠⁠‌
    @brief Block cache for a storage device.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef CACHE_TRACE
#define TRACE
#endif

#ifdef CACHE_DEBUG
#define DEBUG
#endif

#include "cache.h"

#include "conf.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "thread.h"

// INTERNAL TYPE DEFINITIONS
//
#define CACHE_BLOCK 64

struct cache_block 
{
    unsigned long long  pos;
    unsigned long long last_used;
    unsigned int reference;
    char valid;
    char dirty;
    unsigned char* data;
};

struct cache
{
    struct storage* storage;
    struct cache_block block[CACHE_BLOCK];
    struct lock lock;
    unsigned long long use_counter;
};


// INTERNAL FUNCTION DECLARATIONS
//

/**
 * @brief Creates/initializes a cache with the passed backing storage device (disk) and makes it
 * available through cptr.
 * @param disk Pointer to the backing storage device.
 * @param cptr Pointer to the cache to create.
 * @return 0 on success, negative error code if error
 */
int create_cache(struct storage* disk, struct cache** cptr) {
    // FIXME
    struct cache* cache;

    if(disk==NULL||cptr==NULL) return -EINVAL;
    if(storage_blksz(disk)!=CACHE_BLKSZ) return -ENOTSUP;

    cache=kcalloc(1,sizeof(*cache));
    if(cache==NULL) return -ENOMEM;

    for(int i=0;i<CACHE_BLOCK;i++)
    {
        cache->block[i].data=kcalloc(1,CACHE_BLKSZ);
        if(cache->block[i].data==NULL)
        {
            while(i-->0)
            {
                kfree(cache->block[i].data);
                cache->block[i].data=NULL;
            }
            kfree(cache);
            return -ENOMEM;
        }
    }
    cache->storage=disk;
    lock_init(&cache->lock);
    *cptr=cache;
    return 0; 
}

/**
 * @brief Reads a CACHE_BLKSZ sized block from the backing interface into the cache.
 * @param cache Pointer to the cache.
 * @param pos Position in the backing storage device. Must be aligned to a multiple of the block
 * size of the backing interface.
 * @param pptr Pointer to the block pointer read from the cache. Assume that CACHE_BLKSZ will always
 * be equal to the block size of the storage disk. Any replacement policy is permitted, as long as
 * your design meets the above specifications.
 * @return 0 on success, negative error code if error
 */
int cache_get_block(struct cache* cache, unsigned long long pos, void** pptr) {
    // FIXME
    struct cache_block* block=NULL;
    struct cache_block* empty_block=NULL;
    long result;

    if(cache==NULL||pptr==NULL) return -EINVAL;
    if((pos%CACHE_BLKSZ)!=0) return -EINVAL;

    lock_acquire(&cache->lock);

    for(int i=0;i<CACHE_BLOCK;i++)
    {
        if(cache->block[i].valid&&cache->block[i].pos==pos)
        {
            block=&cache->block[i];
            break;
        }
    }
    if(block==NULL)
    {
        for(int i=0;i<CACHE_BLOCK;i++)
        {
            if(!cache->block[i].valid)
            {
                empty_block=&cache->block[i];
                break;
            }
            if(cache->block[i].reference==0)
            {
                if(empty_block==NULL||cache->block[i].last_used<empty_block->last_used)
                {
                    empty_block=&cache->block[i];
                }
            }
        }

        if(empty_block==NULL)
        {
            lock_release(&cache->lock);
            return -EBUSY;
        }

        block=empty_block;

        if(block->valid&&block->dirty)
        {
            __sync_synchronize();
            result=storage_store(cache->storage, block->pos, block->data, CACHE_BLKSZ);
            __sync_synchronize();
            if(result<0)
            {
                lock_release(&cache->lock);
                return result;
            }
            block->dirty=0;
        }

        __sync_synchronize();
        result=storage_fetch(cache->storage, pos, block->data, CACHE_BLKSZ);
        __sync_synchronize();
        if(result<0)
        {
            block->valid=0;
            block->reference=0;
            lock_release(&cache->lock);
            return result;
        }

        block->pos=pos;
        block->valid=1;
        block->dirty=0;
    }

    block->reference++;
    block->last_used= ++cache->use_counter;
    *pptr=block->data;

    lock_release(&cache->lock);
    return 0;
}

/**
 * @brief Releases a block previously obtained from cache_get_block().
 * @param cache Pointer to the cache.
 * @param pblk Pointer to a block that was made available in cache_get_block() (which means that
 * pblk == *pptr for some pptr).
 * @param dirty Indicates whether the block has been modified (1) or not (0). If dirty == 1, the
 * block has been written to. If dirty == 0, the block has not been written to.
 * @return 0 on success, negative error code if error
 */
void cache_release_block(struct cache* cache, void* pblk, int dirty) {
    // FIXME
    if(cache==NULL||pblk==NULL) return;
    
    lock_acquire(&cache->lock);
    for(int i=0;i<CACHE_BLOCK;i++)
    {
        struct cache_block* block=&cache->block[i];
        if(block->valid&&block->data==pblk)
        {
            if(dirty) block->dirty=1;
            if(block->reference>0) block->reference--;
            //block->last_used= ++cache->use_counter;
            break;
        }
    }
    lock_release(&cache->lock);
}

/**
 * @brief Flushes the cache to the backing device
 * @param cache Pointer to the cache to flush
 * @return 0 on success, error code if error
 */
int cache_flush(struct cache* cache) {
    // FIXME
    int result;
    int status=0;

    if(cache==NULL) return -EINVAL;

    lock_acquire(&cache->lock);
    for(int i=0;i<CACHE_BLOCK;i++)
    {
        struct cache_block* block=&cache->block[i];
        if(block->valid&&block->dirty)
        {
            if(block->reference!=0)
            {
                status=-EBUSY;
                continue;
            }

            __sync_synchronize();
            result=storage_store(cache->storage, block->pos, block->data, CACHE_BLKSZ);
            __sync_synchronize();
            if(result<0)
            {
                status= result;
                break;
            }
            block->dirty=0;
        }
    }
    lock_release(&cache->lock);
    return status;
}
