/*! @file vioblk.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‍‌⁠​⁠⁠‌​⁠⁠‌
    @brief VirtIO block device
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include "devimpl.h"
#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include <limits.h>

#include "conf.h"
#include "console.h"
#include "device.h"
#include "error.h"
#include "heap.h"
#include "intr.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"  // FCNTL
#include "virtio.h"

// COMPILE-TIME PARAMETERS
//

#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif

// INTERNAL CONSTANT DEFINITIONS
//
#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1

struct virtio_blk_req{
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};//描述符

struct vioblk_req_ticket{
    volatile int done;
    volatile uint8_t status;
    struct condition cv;
}; //管理线程用的

struct vioblk_storage{
    struct storage sto;   
    volatile struct virtio_mmio_regs *regs;
    int irqno;

    int opened;

    uint16_t qlen;
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;

    uint16_t free_desc;
    uint16_t last_used_idx;

    struct vioblk_req_ticket *tickets;

    struct lock lock;
}; //类似struct viorng_serial


// VirtIO block device feature bits (number, *not* mask)

#define VIRTIO_BLK_F_SIZE_MAX 1
#define VIRTIO_BLK_F_SEG_MAX 2
#define VIRTIO_BLK_F_GEOMETRY 4
#define VIRTIO_BLK_F_RO 5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH 9
#define VIRTIO_BLK_F_TOPOLOGY 10
#define VIRTIO_BLK_F_CONFIG_WCE 11
#define VIRTIO_BLK_F_MQ 12
#define VIRTIO_BLK_F_DISCARD 13
#define VIRTIO_BLK_F_WRITE_ZEROES 14

// INTERNAL FUNCTION DECLARATIONS
//

/**
 * @brief Sets the virtq avail and virtq used queues such that they are available for use. (Hint,
 * read virtio.h) Enables the interupt line for the virtio device and sets necessary flags in vioblk
 * device.
 * @param sto Storage IO struct for the storage device
 * @return Return 0 on success or negative error code if error. If the given sto is already opened,
 * then return -EBUSY.
 */
static int vioblk_storage_open(struct storage* sto);

/**
 * @brief Resets the virtq avail and virtq used queues and sets necessary flags in vioblk device. If
 * the given sto is not opened, this function does nothing.
 * @param sto Storage IO struct for the storage device
 * @return None
 */
static void vioblk_storage_close(struct storage* sto);

/**
 * @brief Reads bytecnt number of bytes from the disk and writes them to buf. Achieves this by
 * repeatedly setting the appropriate registers to request a block from the disk, waiting until the
 * data has been populated in block buffer cache, and then writes that data out to buf. Thread
 * sleeps while waiting for the disk to service the request.
 * @param sto Storage IO struct for the storage device
 * @param pos The starting position for the read within the VirtIO device
 * @param buf A pointer to the buffer to fill with the read data
 * @param bytecnt The number of bytes to read from the VirtIO device into the buffer
 * @return The number of bytes read from the device, or negative error code if error
 */
static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                 unsigned long bytecnt);

/**
 * @brief Writes bytecnt number of bytes from the parameter buf to the disk. The size of the virtio
 * device should not change. You should only overwrite existing data. Write should also not create
 * any new files. Achieves this by filling up the block buffer cache and then setting the
 * appropriate registers to request the disk write the contents of the cache to the specified block
 * location. Thread sleeps while waiting for the disk to service the request.
 * @param sto Storage IO struct for the storage device
 * @param pos The starting position for the write within the VirtIO device
 * @param buf A pointer to the buffer with the data to write
 * @param bytecnt The number of bytes to write to the VirtIO device from the buffer
 * @return The number of bytes written to the device, or negative error code if error
 */
static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                 unsigned long bytecnt);

/**
 * @brief Given a file io object, a specific command, and possibly some arguments, execute the
 * corresponding functions on the VirtIO block device.
 * @details Any commands such as FCNTL_GETEND should pass back through the arg variable. Do not
 * directly return the value.
 * @details FCNTL_GETEND should return the capacity of the VirtIO block device in bytes.
 * @param sto Storage IO struct for the storage device
 * @param op Operation to execute. vioblk should support FCNTL_GETEND.
 * @param arg Argument specific to the operation being performed
 * @return Status code on the operation performed
 */
static int vioblk_storage_cntl(struct storage* sto, int op, void* arg);

/**
 * @brief The interrupt handler for the VirtIO device. When an interrupt occurs, the system will
 * call this function.
 * @param irqno The interrupt request number for the VirtIO device
 * @param aux A generic pointer for auxiliary data.
 * @return None
 */
static void vioblk_isr(int irqno, void* aux);

/**
 * @brief 
 * @param vbd 
 * @param type
 * @param sector
 * @param buf
 * @param len
 * @return None
 */
static long vioblk_io(struct vioblk_storage *vbd, uint32_t type,
                       uint64_t sector, void *buf, unsigned int len);


// INTERNAL GLOBAL VARIABLES
//
static const struct storage_intf vioblk_intf={
    .blksz=512,
    .open=&vioblk_storage_open,
    .close=&vioblk_storage_close,
    .fetch=&vioblk_storage_fetch,
    .store=&vioblk_storage_store,
    .cntl=&vioblk_storage_cntl,
};

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO block device. Declared and called directly from virtio.c.
/**
 * @brief Initializes virtio block device with the necessary IO operation functions and sets the
 * required feature bits.
 * @param regs Memory mapped register of Virtio
 * @param irqno Interrupt request number of the device
 * @return None
 */
void vioblk_attach(volatile struct virtio_mmio_regs* regs, int irqno) {
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct vioblk_storage* vbd;
    unsigned int blksz;
    int result;

    trace("%s(regs=%p,irqno=%d)", __func__, regs, irqno);

    assert(regs->device_id == VIRTIO_ID_BLOCK); //断言这是块设备（Device ID=2）

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;   //设置 DRIVER 状态位
    __sync_synchronize();  // fence o,io

    // Negotiate features. We need:
    //  - VIRTIO_F_RING_RESET and
    //  - VIRTIO_F_INDIRECT_DESC
    // We want:
    //  - VIRTIO_BLK_F_BLK_SIZE and
    //  - VIRTIO_BLK_F_TOPOLOGY.

    virtio_featset_init(needed_features);
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);    //支持单独 virtqueue reset
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC); //支持间接描述符
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE); //从 config 读真实块大小
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY); //物理块/对齐/最佳 I/O 尺寸信息
    result = virtio_negotiate_features(regs, enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // If the device provides a block size, use it. Otherwise, use 512.

    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;

    // blksz must be a power of two
    assert(((blksz - 1) & blksz) == 0);

    // FIXME，在fixme之前已经完成了feature_ok
    regs->queue_sel=0;
    __sync_synchronize();
    uint32_t qmax=regs->queue_num_max;
    if(qmax==0){
        kprintf("%p: vioblk queue 0 not available\n",regs);
        regs->status|=VIRTIO_STAT_FAILED;
        return;
    }
    uint32_t qlimit = (qmax < 128) ? qmax : 128;
    size_t ticket_limit = HEAP_ALLOC_MAX / sizeof(struct vioblk_req_ticket);
    if (ticket_limit == 0) {
        ticket_limit = 1;
    }
    if (qlimit > ticket_limit) {
        qlimit = ticket_limit;
    }
    if (qlimit == 0) {
        qlimit = 1;
    }
    uint16_t qlen=(uint16_t)qlimit; //选择合适的qlen，受堆限制
    regs->queue_num=qlen;
    __sync_synchronize();
    
    //分配区域大小
    size_t desc_sz=sizeof(struct virtq_desc) * qlen;
    size_t avail_sz=sizeof(struct virtq_avail)+sizeof(uint16_t) * qlen;
    size_t used_sz=sizeof(struct virtq_used)+sizeof(struct virtq_used_elem) * qlen;
    struct virtq_desc *desc=(struct virtq_desc*)kcalloc(1, desc_sz);
    struct virtq_avail *avail=(struct virtq_avail*)kcalloc(1, avail_sz);
    struct virtq_used *used=(struct virtq_used*)kcalloc(1, used_sz);
    if(!desc || !avail || !used){
        if(desc){
            kfree(desc);
        }
        if(avail){
            kfree(avail);
        }
        if(used){
            kfree(used);
        }
        kprintf("%p:vioblk: fail allocating virtqueue\n",regs);
        regs->status|=VIRTIO_STAT_FAILED;
        return;
    }
    memset(desc, 0, desc_sz); //数据清零，避免脏数据
    memset(avail, 0, avail_sz);
    memset(used, 0, used_sz);
    __sync_synchronize();
    //regs->queue_ready=1; //队列就绪
    __sync_synchronize();

    //vbd驱动初始化
    vbd=(struct vioblk_storage*)kcalloc(1, sizeof(*vbd));
    if(!vbd){
        kprintf("%p: vioblk: fail allocating device state\n",regs);
        regs->queue_ready=0;
        __sync_synchronize();
        kfree(desc);
        kfree(avail);
        kfree(used);
        regs->status|=VIRTIO_STAT_FAILED;
        return;
    }
    memset(vbd, 0, sizeof(*vbd));
    vbd->regs=regs;
    vbd->irqno=irqno;
    vbd->qlen=qlen;
    vbd->desc=desc;
    vbd->avail=avail;
    vbd->used=used;
    vbd->free_desc=0;
    vbd->last_used_idx=0;
    lock_init(&vbd->lock);
    //condition_init(&vbd->ready);

    //也属于vbd驱动初始化一部分，这里是初始化vbd->tickets
    vbd->tickets=(struct vioblk_req_ticket*)kcalloc(qlen, sizeof(*vbd->tickets));
    if(!vbd->tickets){
        kprintf("%p: vioblk: fail allocating tickets\n",regs);
        regs->queue_ready=0;
        __sync_synchronize();
        kfree(desc);
        kfree(avail);
        kfree(used);
        kfree(vbd);
        regs->status|=VIRTIO_STAT_FAILED;
        return;
    }
    for(uint16_t i=0;i<qlen;i++){
        vbd->tickets[i].done=0;
        vbd->tickets[i].status=0xFF;
        condition_init(&vbd->tickets[i].cv, "vioblk-tk");
    }
    //将驱动挂在设备接口上
    storage_init(&vbd->sto, &vioblk_intf, regs->config.blk.capacity*512ULL);

    //注册ISR
    //register_isr(irqno, &vioblk_isr, vbd);    //////////////参考devimpl.h定义，改这个
    //enable_intr_source(irqno);               /////////////参考intr.h定义，改这个

    virtio_attach_virtq(regs, 0, vbd->qlen, (uint64_t)vbd->desc, (uint64_t)vbd->used,
                        (uint64_t)vbd->avail);

    //注册一个存储设备实例
    int inst=register_device("vioblk", DEV_STORAGE, vbd);
    if(inst<0){
        kprintf("%p: vioblk: fail registering device\n",regs);
        regs->queue_ready=0;
        __sync_synchronize();
        kfree(vbd->tickets);
        kfree(desc);
        kfree(avail);
        kfree(used);
        kfree(vbd);
        regs->status|=VIRTIO_STAT_FAILED;
        return;
    }

    //DRIVER_OK
    regs->status|=VIRTIO_STAT_DRIVER_OK;
    __sync_synchronize();
}

static int vioblk_storage_open(struct storage* sto) {
    // FIXME
    struct vioblk_storage *vbd=(struct vioblk_storage*) sto;
    lock_acquire(&vbd->lock);

    if(vbd->opened){
        lock_release(&vbd->lock);
        return -EBUSY;
    }else{
        if(vbd->regs->queue_ready!=1){
            lock_release(&vbd->lock);
            return -EBUSY;
        }
        vbd->used->idx=0;
        vbd->avail->idx=0;
        vbd->last_used_idx=0;
        vbd->opened=1;
        virtio_enable_virtq(vbd->regs, 0);
        enable_intr_source(vbd->irqno, VIOBLK_INTR_PRIO, vioblk_isr, vbd);
    }
    lock_release(&vbd->lock);
    return 0;
    //return -ENOTSUP;
}

static void vioblk_storage_close(struct storage* sto) {
    // FIXME
    struct vioblk_storage *vbd=(struct vioblk_storage*) sto;
    lock_acquire(&vbd->lock);

    if(!vbd->opened){
        lock_release(&vbd->lock);
        return;
    }
    disable_intr_source(vbd->irqno);
    vbd->opened=0;
    virtio_reset_virtq(vbd->regs, 0);
    for(uint16_t i=0; i<vbd->qlen; i++){
        struct vioblk_req_ticket *tk=&vbd->tickets[i];
        if(!tk->done){
            tk->status=1;
            tk->done=1;
            condition_broadcast(&tk->cv);
        }
    }

    lock_release(&vbd->lock);
    return;
}

static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                 unsigned long bytecnt) {
    // FIXME
    struct vioblk_storage *vbd=(struct vioblk_storage*) sto;
    if((pos%512)!=0 || (bytecnt%512)!=0){
        return -EINVAL;
    }
    return vioblk_io(vbd, VIRTIO_BLK_T_IN, pos/512, buf, bytecnt);
    //return -ENOTSUP;
}

static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                 unsigned long bytecnt) {
    // FIXME
    struct vioblk_storage *vbd=(struct vioblk_storage*) sto;
    if((pos%512)!=0 || (bytecnt%512)!=0){
        return -EINVAL;
    }
    return vioblk_io(vbd, VIRTIO_BLK_T_OUT, pos/512, (void*)buf, bytecnt);
    //return -ENOTSUP;
}

static int vioblk_storage_cntl(struct storage* sto, int op, void* arg) {
    // FIXME
    switch(op){
        case FCNTL_MMAP:
            kprintf("MMAP is not supported yet\n");
            return -ENOTSUP;
        case FCNTL_GETEND:
            if(!arg){
                return -EINVAL;
            }
            *(unsigned long long*)arg=sto->capacity;
            return 0;
        default:
            return -ENOTSUP;
    }
}

static void vioblk_isr(int irqno, void* aux) {
    // FIXME
    
    //这里参考2.7.15.receiving used buffers
    struct vioblk_storage *vbd=aux;
    uint32_t is=vbd->regs->interrupt_status;
    lock_acquire(&vbd->lock);

    while(vbd->last_used_idx!=vbd->used->idx){
        struct virtq_used_elem *e=
            &vbd->used->ring[vbd->last_used_idx%vbd->qlen];
        uint16_t id=e->id; //只有head入队，所以id就是这一个请求的head
        struct vioblk_req_ticket *tk=&vbd->tickets[id];
        tk->status=*(uint8_t*)vbd->desc[(id+2)%vbd->qlen].addr;
        tk->done=1;
        condition_broadcast(&tk->cv);
        vbd->last_used_idx++;
    }
    vbd->regs->interrupt_ack=is;
    lock_release(&vbd->lock);
    return;
}

static long vioblk_io(struct vioblk_storage *vbd, uint32_t type,
                       uint64_t sector, void *buf, unsigned int len){
    if((len%512)!=0){
        return -EINVAL;
    }
    lock_acquire(&vbd->lock);

    //分配一下req和status
    struct virtio_blk_req *req=(struct virtio_blk_req*)kcalloc(1, sizeof(*req));
    if(!req){
        lock_release(&vbd->lock);
        return -ENOMEM;
    }
    uint8_t *status=(uint8_t*)kcalloc(1, sizeof(uint8_t));
    if(!status){
        kfree(req);
        lock_release(&vbd->lock);
        return -ENOMEM;
    }
    *status=0xFF;
    req->type=type;
    req->reserved=0;
    req->sector=sector;

    //三段式开始
    uint16_t head=vbd->free_desc;
    uint16_t d0=head;    //请求头header
    uint16_t d1=(uint16_t)((head+1)%vbd->qlen); //data段
    uint16_t d2=(uint16_t)((head+2)%vbd->qlen); //status段
    vbd->free_desc=(uint16_t)((head+3)%vbd->qlen); //下次从这里开始

    //d0
    vbd->desc[d0].addr=(uint64_t)(uintptr_t)req;
    vbd->desc[d0].len=sizeof(*req);
    vbd->desc[d0].flags=VIRTQ_DESC_F_NEXT;
    vbd->desc[d0].next=d1;

    //d1
    vbd->desc[d1].addr=(uint64_t)(uintptr_t)buf;
    vbd->desc[d1].len=len;
    vbd->desc[d1].flags=(type==VIRTIO_BLK_T_IN ? VIRTQ_DESC_F_WRITE : 0) | VIRTQ_DESC_F_NEXT;
    vbd->desc[d1].next=d2;

    //d2
    vbd->desc[d2].addr=(uint64_t)(uintptr_t)status;
    vbd->desc[d2].len=1;
    vbd->desc[d2].flags=VIRTQ_DESC_F_WRITE;
    vbd->desc[d2].next=0;

    //设置ticket
    struct vioblk_req_ticket *tk=&vbd->tickets[head];
    tk->done=0;
    tk->status=0xFF;

    //avail入队
    uint16_t ai=vbd->avail->idx; //idx记录有多少请求
    vbd->avail->ring[ai%vbd->qlen]=d0;
    __sync_synchronize();
    vbd->avail->idx=(uint16_t)(ai+1);
    __sync_synchronize();
    vbd->regs->queue_notify=0; //提交请求，0就是队列号
    
    while(!tk->done){
        condition_wait(&tk->cv);              ////////不能同时等两个条件，参考thread.h
    }
    int ret=(tk->status==0) ? (int)len : -EIO;
    kfree(status);
    kfree(req);
    lock_release(&vbd->lock);
    return ret;
}
