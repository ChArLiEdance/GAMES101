/*! @file ktfs.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‍‌⁠​⁠⁠‌​⁠⁠‌
    @brief KTFS Implementation.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef KTFS_TRACE
#define TRACE
#endif

#ifdef KTFS_DEBUG
#define DEBUG
#endif

#include "ktfs.h"

#include <stddef.h>

#include "cache.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "filesys.h"
#include "fsimpl.h"
#include "heap.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"
#include "uioimpl.h"

// INTERNAL TYPE DEFINITIONS
//
struct ktfs{
    struct filesystem fs;
    struct cache* cache;
    struct ktfs_superblock super;//superblock
    unsigned int inode_bitmap_start;// 1
    unsigned int block_bitmap_start;//1+K 
    unsigned int inode_block_start;//1+K+B
    unsigned int data_block_start;//1+K+B+N+0
    unsigned int inode_per_block;//  512/16==32
    unsigned int dirent_per_block;// 512/16==32
    unsigned int total_inodes;
    struct lock lock;
};

/// @brief File struct for a file in the Keegan Teal Filesystem
struct ktfs_file {
    // Fill to fulfill spec
    struct uio base;
    struct ktfs_dir_entry directory;
    unsigned long long pos;
    unsigned long long size;
    struct ktfs_inode inode;
    struct ktfs* ktfs;
};


// INTERNAL FUNCTION DECLARATIONS
//

int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr);
void ktfs_close(struct uio* uio);
int ktfs_cntl(struct uio* uio, int cmd, void* arg);
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len);
long ktfs_store(struct uio* uio, const void* buf, unsigned long len);
int ktfs_create(struct filesystem* fs, const char* name);
int ktfs_delete(struct filesystem* fs, const char* name);
void ktfs_flush(struct filesystem* fs);

void ktfs_listing_close(struct uio* uio);
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz);

static int ktfs_read_root_directory_inode(struct ktfs* ktfs, uint32_t ino, struct ktfs_inode* inode);
static int ktfs_read_block_entry(struct ktfs* ktfs, uint32_t blockno, unsigned int block_i, uint32_t* blockno_out);
static int ktfs_map(struct ktfs* ktfs, const struct ktfs_inode* inode, unsigned int block_i, uint32_t* blockno);
static int ktfs_search_directory(struct ktfs* ktfs, const struct ktfs_inode* dir_inode, const char*name, struct ktfs_dir_entry* directory_out, struct ktfs_inode* inode_out);
static int ktfs_find(struct ktfs* ktfs, const char* path, struct ktfs_dir_entry* directory_out, struct ktfs_inode* inode_out);
//uio
static const struct uio_intf ktfs_uio_intf ={
    .close=&ktfs_close, 
    .read=&ktfs_fetch,
    .write=&ktfs_store,
    .cntl=&ktfs_cntl,
};
/**
 * @brief Mounts the file system with associated backing cache
 * @param cache Pointer to cache struct for the file system
 * @return 0 if mount successful, negative error code if error
 */
int mount_ktfs(const char* name, struct cache* cache) {
    // FIXME
    struct ktfs* ktfs;
    void* block;
    int result;
    if(name==NULL||cache==NULL) return -EINVAL;
    //malloc space for ktfs;
    ktfs=kcalloc(1,sizeof(*ktfs));
    if(ktfs==NULL)
    {
        //fail malloc
        return -ENOENT;
    }
    ktfs->cache=cache;
    lock_init(&ktfs->lock);
    //get the superblock
    result=cache_get_block(cache,0,&block);
    if(result!=0)
    {
        //error
        kfree(ktfs);
        return result;
    }
    
    //put the superblock into ktfs
    //dirve region layout from super fields
    memcpy(&ktfs->super,block,sizeof(ktfs->super));
    cache_release_block(cache,block,0);
    
    //from the picture
    ktfs->inode_bitmap_start=1;//1
    ktfs->block_bitmap_start=ktfs->inode_bitmap_start+ktfs->super.inode_bitmap_block_count;//1+K
    ktfs->inode_block_start=ktfs->block_bitmap_start+ktfs->super.bitmap_block_count;//1+K+B
    ktfs->data_block_start=ktfs->inode_block_start+ktfs->super.inode_block_count;//1+K+B+N
    ktfs->inode_per_block=KTFS_BLKSZ/sizeof(struct ktfs_inode);//512/16
    ktfs->dirent_per_block=KTFS_BLKSZ/sizeof(struct ktfs_dir_entry);//512/16
    ktfs->total_inodes=ktfs->super.inode_block_count*ktfs->inode_per_block;
    
    //connect the fs operator
    ktfs->fs.open=&ktfs_open;   
    ktfs->fs.create=&ktfs_create;
    ktfs->fs.delete=&ktfs_delete;
    ktfs->fs.flush=&ktfs_flush;

    //Attaches a filesystem to a mount point name
    result=attach_filesystem(name,&ktfs->fs);
    if(result!=0)
    {
        kfree(ktfs);    
        return result;
    }
    return 0;       //success: KTFS is mounted
}

/**
 * @brief Opens a file or ls (listing) with the given name and returns a pointer to the uio through
 * the double pointer
 * @param name The name of the file to open or "\" for listing (CP3)
 * @param uioptr Will return a pointer to a file or ls (list) uio pointer through this double
 * pointer
 * @return 0 if open successful, negative error code if error
 */
int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr) {
    // FIXME
    struct ktfs* ktfs;
    struct ktfs_dir_entry directory;
    struct ktfs_inode inode;
    struct ktfs_file* file;
    int result;
    if(fs==NULL||uioptr==NULL) return -EINVAL;
    //put the fs into ktfs
    ktfs=(struct ktfs*) fs;
    if(name==NULL||*name=='\0'||(name[0]=='\\'&&name[1]=='\0')) return -ENOTSUP;
    //path lookup find in file dictory with name
    result= ktfs_find(ktfs, name, &directory, &inode);
    if(result!=0)  return result;
    file=kcalloc(1, sizeof(*file));
    if(file==NULL) return -ENOMEM;
    //after setup the ktfsfile put all things in it
    file->ktfs=ktfs;
    //cache inode 
    memcpy(&file->inode, &inode, sizeof(inode));
    //keep dentry for identity
    memcpy(&file->directory, &directory, sizeof(directory));
    //start beginning of file
    file->pos=0;
    //cache size 
    file->size=inode.size;
    //uio vtable and return &file->base outward
    *uioptr=uio_init1(&file->base, &ktfs_uio_intf);
    return 0;
}

/**
 * @brief Closes the file that is represented by the uio struct
 * @param uio The file io to be closed
 * @return None
 */
void ktfs_close(struct uio* uio) {
    // FIXME
    struct ktfs_file* file;
    if(uio==NULL) return;
    //find the file using uio
    file=(struct ktfs_file*)uio;
    kfree(file);
}

/**
 * @brief Reads data from file attached to uio into provided argument buffer
 * @param uio uio of file to be read
 * @param buf Buffer to be filled
 * @param len Number of bytes to read
 * @return Number of bytes read if successful, negative error code if error
 */
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len) {
    // FIXME
    struct ktfs_file* file;
    struct ktfs* ktfs;
    unsigned long long readbyte=0;
    unsigned long long remain;
    int result;
    if(uio==NULL||buf==NULL) return -EINVAL;
    
    file=(struct ktfs_file*)uio;
    ktfs=file->ktfs;

    if(file->pos>=file->size-file->pos) return 0;
    //compute available bytes
    remain=file->size-file->pos;

    if(remain<len) len=remain;
    //loop until we deal with all len data
    while(readbyte<len)
    {
        //current pos
        unsigned long long current_pos=file->pos;
        //the max B can read this time
        unsigned long long chunk=KTFS_BLKSZ-current_pos%KTFS_BLKSZ;
        uint32_t blockno;
        void* blockptr;
        
        //current B should read
        if(chunk>(len-readbyte)) chunk=len-readbyte;
        
        //map logical block index to on-disk block number
        result=ktfs_map(ktfs, &file->inode, current_pos/KTFS_BLKSZ, &blockno);
        if(result!=0) return (readbyte==0)? result:readbyte;
        //fetch the whole block via cache
        result=cache_get_block(ktfs->cache, (unsigned long long)blockno*KTFS_BLKSZ, &blockptr);
        if(result!=0) return (readbyte==0)? result:readbyte;
        
        //copy just the slice we need from the block into caller buffer
        memcpy((char*)buf+readbyte, (unsigned char*)blockptr+current_pos%KTFS_BLKSZ, chunk);
        //release cache
        cache_release_block(ktfs->cache, blockptr,0);
        readbyte+=chunk;
        file->pos+=chunk;
    }
    
    return readbyte;
}

/**
 * @brief Write data from the provided argument buffer into file attached to uio
 * @param uio The file to be written to
 * @param buf The buffer to be read from
 * @param len Number of bytes to write from the buffer to the file
 * @return Number of bytes written from the buffer to the file system if sucessful, negative error
 * code if error
 */
long ktfs_store(struct uio* uio, const void* buf, unsigned long len) {
    // FIXME
    return -ENOTSUP;
}

/**
 * @brief Create a new file in the file system
 * @param fs The file system in which to create the file
 * @param name The name of the file
 * @return 0 if successful, negative error code if error
 */
int ktfs_create(struct filesystem* fs, const char* name) {
    // FIXME
    return -ENOTSUP;
}

/**
 * @brief Deletes a certain file from the file system with the given name
 * @param fs The file system to delete the file from
 * @param name The name of the file to be deleted
 * @return 0 if successful, negative error code if error
 */
int ktfs_delete(struct filesystem* fs, const char* name) {
    // FIXME
    return -ENOTSUP;
}

/**
 * @brief Given a file io object, a specific command, and possibly some arguments, execute the
 * corresponding functions
 * @details Any commands such as (FCNTL_GETEND, FCNTL_GETPOS, ...) should pass back through the arg
 * variable. Do not directly return the value.
 * @details FCNTL_GETEND should pass back the size of the file in bytes through the arg variable.
 * @details FCNTL_SETEND should set the size of the file to the value passed in through arg.
 * @details FCNTL_GETPOS should pass back the current position of the file pointer in bytes through
 * the arg variable.
 * @details FCNTL_SETPOS should set the current position of the file pointer to the value passed in
 * through arg.
 * @param uio the uio object of the file to perform the control function
 * @param cmd the operation to execute. KTFS should support FCNTL_GETEND, FCNTL_SETEND (CP2),
 * FCNTL_GETPOS, FCNTL_SETPOS.
 * @param arg the argument to pass in, may be different for different control functions
 * @return 0 if successful, negative error code if error
 */
int ktfs_cntl(struct uio* uio, int cmd, void* arg) {
    // FIXME
    struct ktfs_file* file;
    unsigned long long* ullptr;
    if(uio==NULL) return -EINVAL;

    file=(struct ktfs_file*)uio;

    switch(cmd)
    {
        case FCNTL_GETEND:
        //return file size arg
            if(arg==NULL) return -EINVAL;
            ullptr=arg;
            *ullptr=file->size;
            return 0;
        case FCNTL_GETPOS:
            //return current file offset arg
            if(arg==NULL) return -EINVAL;
            ullptr=arg;
            *ullptr=file->pos;
            return 0;
        case FCNTL_SETPOS:
            //set current file offset to arg
            if(arg==NULL) return -EINVAL;
            ullptr=arg;
            if(*ullptr>file->size) return -EINVAL;
            file->pos=*ullptr;
            return 0;
        default:
            return -ENOTSUP;
    }
    return -ENOTSUP;
}

/**
 * @brief Flushes the cache to the backing device
 * @return 0 if flush successful, negative error code if error
 */
void ktfs_flush(struct filesystem* fs) {
    // FIXME
    struct ktfs* ktfs;
    if(fs==NULL) return;
    //recover mount object
    ktfs=(struct ktfs*)fs;
    //delegate to cache implementation
    cache_flush(ktfs->cache);
    return;
}

/**
 * @brief Closes the listing device represented by the uio pointer
 * @param uio The uio pointer of ls
 * @return None
 */
void ktfs_listing_close(struct uio* uio) {
    // FIXME
    return;
}

/**
 * @brief Reads all of the files names in the file system using ls and copies them into the
 * providied buffer
 * @param uio The uio pointer of ls
 * @param buf The buffer to copy the file names to
 * @param bufsz The size of the buffer
 * @return The size written to the buffer
 */
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz) {
    // FIXME
    return -ENOTSUP;
}

/**
 * @brief Load an inode from the disk inode table
 * @details 
 * @param ktfs ptr to the ktfs mount context
 * @param ino Inode number to load
 * @param inode destination buffer for inode contents
 * @return 0 on success, negative error code otherwise
 */
static int ktfs_read_root_directory_inode(struct ktfs* ktfs, uint32_t ino, struct ktfs_inode* inode)
{
    unsigned int block_idx;
    unsigned int offset;
    void* block;
    struct ktfs_inode* array;
    int result;

    if(ino>=ktfs->total_inodes)  return -ENOENT;

    //compute the inode table block index
    block_idx=ktfs->inode_block_start+(ino/ktfs->inode_per_block);
    //the offset of the block
    offset=ino%ktfs->inode_per_block;

    //use cache to get the block
    result=cache_get_block(ktfs->cache,(unsigned long long)block_idx*KTFS_BLKSZ, &block);
    if(result!=0) return result;

    //treat block as an array of inodes
    array=(struct ktfs_inode*)block;
    //copy out the inode
    memcpy(inode, &array[offset],sizeof(*inode));
   
    // release cache ref
    cache_release_block(ktfs->cache,block, 0);
    return 0;
}       

/**
 * @brief Read a 32-bit entry from an indirect block
 * @details indrect block are arrays of block numbers.
 * @details This helper performs bounds checking, fetches the block the vis the cache,
 * @details and return the requested element
 * @param ktfs ptr to ktfs mount context.
 * @param biockno block containing the array of block number
 * @param block_i element index within the block
 * @param blolkno_out output for the extracted block number
 * @return 0 on success, negative error code otherwise
 */
static int ktfs_read_block_entry(struct ktfs* ktfs, uint32_t blockno, unsigned int block_i, uint32_t* blockno_out)
{
    void* blk;
    uint32_t* entries;
    int result;

    if(blockno==0) return -ENOENT;
    //index out of the bounds
    if(block_i>=(KTFS_BLKSZ/sizeof(uint32_t))) return -EINVAL;

    result= cache_get_block(ktfs->cache, (unsigned long long)blockno*KTFS_BLKSZ, &blk);
    if(result!=0)   return result;

    entries=(uint32_t*)blk;
    //extract the requested entry
    *blockno_out=entries[block_i];

    cache_release_block(ktfs->cache, blk, 0);
    return 0;
}


/**
 * @brief translate a logical block index within a file to a physical block number
 * @details ktfs adopts the direct, indirect, didirect pointer scheme
 * @details we iterate through those levels, reusing read block entry function to pull block number from indirect tables
 * @details final return the block number
 * @param ktfs ptr to ktfs mount context.
 * @param inode inode describing the file
 * @param block_i zero-based logical block index
 * @param blockno output block for phycial block number
 * @return 0 on success, negative error code otherwise
 */
static int ktfs_map(struct ktfs* ktfs, const struct ktfs_inode* inode, unsigned int block_i, uint32_t* blockno)
{
    uint32_t tmp;
    int result;

    //if it is direct bolck
    if(block_i<KTFS_NUM_DIRECT_DATA_BLOCKS)
    {
        //just pull direct pointer
        tmp=inode->block[block_i];
        if(tmp==0) return -ENOENT;
        //return physical block number
        *blockno=tmp;
        return 0;
    }
    //indirect block
    if(block_i-KTFS_NUM_DIRECT_DATA_BLOCKS<KTFS_BLKSZ/sizeof(uint32_t))
    {
        //if no indirect allocated
        if(inode->indirect==0) return -ENOENT;
        //use read block to find the indirect block 
        result= ktfs_read_block_entry(ktfs, inode->indirect, block_i-KTFS_NUM_DIRECT_DATA_BLOCKS, &tmp);
        
        if(result!=0) return result;
        if(tmp==0) return -ENOENT;
        *blockno=tmp;
        return 0;
    }
    //calculate the seconddirectblock 
    unsigned int seconddirectblock=block_i-KTFS_NUM_DIRECT_DATA_BLOCKS-(KTFS_BLKSZ/sizeof(uint32_t));
    uint32_t ditmp;
    for(int i=0;i<KTFS_NUM_DINDIRECT_BLOCKS;i++)
    {
        unsigned int span=(KTFS_BLKSZ/sizeof(uint32_t))*(KTFS_BLKSZ/sizeof(uint32_t));
        if(seconddirectblock<span)
        {
            if(inode->dindirect[i]==0) return -ENOENT;
            //get the indirect block first
            result=ktfs_read_block_entry(ktfs, inode->dindirect[i],seconddirectblock/(KTFS_BLKSZ/sizeof(uint32_t)), &ditmp);
            if(result!=0) return result;
            if(ditmp==0) return -ENOENT;

            //get the actual data block
            result=ktfs_read_block_entry(ktfs, ditmp, seconddirectblock%(KTFS_BLKSZ/sizeof(uint32_t)), &tmp);
            if(result!=0) return result;
            if(tmp==0) return -ENOENT;
            *blockno=tmp;
            return 0;
        }
        seconddirectblock-=span;
    }
    return -EINVAL;
}   

/**
 * @brief locate a directory entry by  name within a directory inode
 * @details directory entry are fixed-size records. we compute which houses each record
 * @details resolve the block using the same logic, and compare entry names until a match is found or we exhaust the directory
 * @param ktfs ptr to ktfs mount context. 
 * @param dir_inode direcotry inode for research
 * @param name target entry name
 * @param directory_out output which match the entry data
 * @param inode_out output for the referenced inode
 * @return 0 on success, negative error code if failure
 */
static int ktfs_search_directory(struct ktfs* ktfs, const struct ktfs_inode* dir_inode, const char*name, struct ktfs_dir_entry* directory_out, struct ktfs_inode* inode_out)
{

    int result;
    // iterate over fixed-size directory entry
    for(int i=0; i<(dir_inode->size/sizeof(struct ktfs_dir_entry));i++)
    {
        uint32_t blockno;
        void* blockptr;
        struct ktfs_dir_entry* entry;

        //map which directory block have the i entry
        result= ktfs_map(ktfs, dir_inode, i/ktfs->dirent_per_block, &blockno);
        if(result!=0) return result;

        //get the directory block from the cache
        result=cache_get_block(ktfs->cache,(unsigned long long) blockno*KTFS_BLKSZ, &blockptr);
        if(result!=0) return result;

        //compute the in-block index and take that entry
        entry =&((struct ktfs_dir_entry*)blockptr)[i%ktfs->dirent_per_block];

        //compare the name
        if(entry->inode!=0&&entry->name[0]!='\0'&&strncmp(entry->name, name, KTFS_MAX_FILENAME_LEN)==0)
        {
            if(directory_out!=NULL) memcpy(directory_out, entry, sizeof(*entry));
            //load  the inode referenced by this dentry
            if(inode_out!=NULL)
            {
                void* blk;
                if(entry->inode>=ktfs->total_inodes) return -ENOENT;

                //get the block have the same name
                result=cache_get_block(ktfs->cache, (unsigned long long)(ktfs->inode_block_start+(entry->inode/ktfs->inode_per_block))*KTFS_BLKSZ, &blk);
                if(result!=0) return result;

                struct ktfs_inode* arr = (struct ktfs_inode*)blk; 
                //copy inode out 
                memcpy(inode_out, &arr[entry->inode%ktfs->inode_per_block], sizeof(*inode_out));

                cache_release_block(ktfs->cache, blk, 0);
            }

            cache_release_block(ktfs->cache, blockptr, 0);
            return 0;
        }
        cache_release_block(ktfs->cache, blockptr, 0);
    }
    return -ENOENT;
}


/**
 * @brief resolve a root-level path to its directory entry and inode
 * @details reject any additional separators and look up the remaining file name within the root inode directory
 * @param ktfs ptr to ktfs mount context.
 * @param name name for root directory 
 * @param directory_out oputput for directory entry data
 * @param inode_out outputfor the resolved data
 * @return 0 on success, nagative error code otherwise
 */
static int ktfs_find(struct ktfs* ktfs, const char* name, struct ktfs_dir_entry* directory_out, struct ktfs_inode* inode_out)
{
    struct ktfs_inode root_inode;
    struct ktfs_inode file_inode;
    struct ktfs_dir_entry directory;
    const char* temp_name;
    int result;

    if(name==NULL) return -EINVAL;

    // start from the provided name
    temp_name=name;
    //skip /
    while(*temp_name=='/')  temp_name++;
    //if empty name end
    if(*temp_name=='\0') return -EINVAL;
    //if have second directory end
    if(strchr(name,'/')!=NULL) return -ENOTSUP;

    //load root directory inode using the inode number in superblock
    result=ktfs_read_root_directory_inode(ktfs, ktfs->super.root_directory_inode, &root_inode);
    if(result!=0) return result;

    //scan toor for target name
    result=ktfs_search_directory(ktfs, &root_inode, temp_name, &directory, &file_inode);
    if(result!=0) return result;
    
    // if find return direcotry and inode
    if(directory_out!=NULL) memcpy(directory_out, &directory, sizeof(directory));
    if(inode_out!=NULL)  memcpy(inode_out, &file_inode, sizeof(file_inode));
    return 0;
  
}

