// viorng.c - VirtIO rng device
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "error.h"
#include "string.h"
#include "thread.h"
#include "devimpl.h"
#include "misc.h"
#include "conf.h"
#include "intr.h"
#include "console.h"

// INTERNAL CONSTANT DEFINITIONS
//

#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif

#ifndef VIORNG_NAME
#define VIORNG_NAME "viorng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

// INTERNAL TYPE DEFINITIONS
//
/*
    struct viorng_serial
    Define the necessary items required to 
    implement the VirtIO entropy device, including avail, used virtqueues,
    and descriptors

*/
struct viorng_serial {
    // FIXME your code goes here
    struct serial ser;
    volatile struct virtio_mmio_regs * regs;
    int irqno;
    char opened;

    int qlen; 
    //virtio queue structures
    struct virtq_desc* desc;
    struct virtq_avail* avail;
    volatile struct virtq_used * used;

    struct condition ready;
    struct lock lock;

    int last_used_idx;
    //buffer
    char entropy_buf[VIORNG_BUFSZ];

};

// INTERNAL FUNCTION DECLARATIONS
//

static int viorng_serial_open(struct serial * ser);

static void viorng_serial_close(struct serial * ser);
static int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz);

static void viorng_isr(int irqno, void * aux);

// INTERNAL GLOBAL VARIABLES
//

static const struct serial_intf viorng_serial_intf = {
    .blksz = 1,
    .open = &viorng_serial_open,
    .close = &viorng_serial_close,
    .recv = &viorng_serial_recv
};

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO rng device. Declared and called directly from virtio.c.

void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct viorng_serial * vrng;
    int result;
    
    assert (regs->device_id == VIRTIO_ID_RNG);

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    // fence o,io
    __sync_synchronize();

    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // FIXME your code goes here 

    // Allocate and initialize device struct
    vrng  = kcalloc(1, sizeof(struct viorng_serial));
    if(vrng==NULL)
    {
        kfree(vrng);
        return;
    }
    //wont change
    vrng->qlen=1;//viorng_queue_length
    //alloc the space for the desc avail used
    vrng->desc=kcalloc(vrng->qlen,sizeof(struct virtq_desc));
    vrng->avail=kcalloc(1,VIRTQ_AVAIL_SIZE(vrng->qlen));
    vrng->used= kcalloc(1,VIRTQ_USED_SIZE(vrng->qlen));
    if(vrng->desc==NULL||vrng->avail==NULL||vrng->used==NULL)
    {
        if(vrng->desc!=NULL)
        {
            kfree(vrng->desc);
        }
        if(vrng->avail!=NULL)
        {
            kfree(vrng->avail);
        }
        if(vrng->used!=NULL)
        {
            kfree((void*)vrng->used);
        }
        kfree(vrng);
        return;
    }
    vrng->regs = regs;
    vrng->irqno = irqno;
    vrng->opened = 0;
    vrng->last_used_idx=0;
    //init desc
    vrng->desc[0].addr = (uint64_t)vrng->entropy_buf;
    vrng->desc[0].len = VIORNG_BUFSZ;
    vrng->desc[0].flags = VIRTQ_DESC_F_WRITE;
    vrng->desc[0].next = 0;
    //init avail
    vrng->avail->flags=0;
    vrng->avail->idx=0;
    //init used
    vrng->used->flags=0;
    vrng->used->idx=0;
    vrng->used->ring[0].id=0;
    vrng->used->ring[0].len=0;
    //attach virqueue 
    virtio_attach_virtq(regs,0,vrng->qlen,(uint64_t)vrng->desc,(uint64_t)vrng->used,(uint64_t)vrng->avail);

    regs->status |= VIRTIO_STAT_DRIVER_OK; //set the driver to OK
    // fence o,oi
    __sync_synchronize();

    condition_init(&vrng->ready,"viorngready");
    lock_init(&vrng->lock);
    // FIXME your code goes here
    serial_init(&vrng->ser, &viorng_serial_intf);
    register_device(VIORNG_NAME, DEV_SERIAL, vrng);

    
}

/*
    Input: struct serial * ser -- Serial device
    Output: returns 0 on success . 
            If the given ser is already opened, then return -EBUSY.
    Discription:
        makes the virtq avail and virtq used queues available for use
        Enables the interrupt source for the device, with the correct ISR.
    Sideeffects:
        None
*/
int viorng_serial_open(struct serial * ser) {
    // FIXME your code goes here
    struct viorng_serial* const vrng= (void*)ser-offsetof(struct viorng_serial, ser);
    if(vrng->opened)
    {
        return -EBUSY;
    }
    //init the idx and the last_used_idx
    vrng->avail->idx=0;
    vrng->used->idx=0;
    vrng->last_used_idx=0;
    virtio_enable_virtq(vrng->regs, 0);
    enable_intr_source(vrng->irqno, VIORNG_IRQ_PRIO, viorng_isr, vrng);
    vrng->opened=1;
    return 0;
}

/*
    Input: struct serial * ser -- Serial device
    Output: None
    Discription:
        This function resets the virtq avail 
        and virtq used queues and prevents further interrupts. 
    Sideeffects:
        None
*/
void viorng_serial_close(struct serial * ser) {
    // FIXME your code goes here
    struct viorng_serial* const vrng= (void*)ser-offsetof(struct viorng_serial, ser);
    if(!vrng->opened)
    {
        return;
    }
    // disable interrupt source
    disable_intr_source(vrng->irqno);
    //reset the virtqueue
    virtio_reset_virtq(vrng->regs,0);
    vrng->opened=0;
    condition_broadcast(&vrng->ready);
    vrng->last_used_idx=0;
}

/*
    Input: struct serial * ser -- Serial device
           void * buf -- buffer
           unsigned int bufsz -- buffersize
    Output: Returns the number of bytes of randomness successfully obtained. 
            If the given ser is not opened, then return -EINVAL.
    Discription:
        This function reads up to bufsz bytes from the VirtIO Entropy 
        device and writes them to buf. This is achieved
        by setting the appropriate registers to request entropy from the device, 
        waiting until the randomness has been
        placed into a buffer, and then writing that data out to buf. 
    Sideeffects:
        None
*/
int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    // FIXME your code goes here
    struct viorng_serial* const vrng= (void*)ser-offsetof(struct viorng_serial, ser);
    lock_acquire(&vrng->lock);

    if(!vrng->opened)
    {
        lock_release(&vrng->lock);
        return -EINVAL;
    }
    if(bufsz==0)
    {
        lock_release(&vrng->lock);
        return 0;
    }
    //if bufsz larger than VIORNGsize, use the second one instead
    unsigned int request_size=(bufsz>VIORNG_BUFSZ)?VIORNG_BUFSZ:bufsz;

    vrng->desc[0].len=request_size;
    vrng->avail->ring[vrng->avail->idx%vrng->qlen]=0;
    __sync_synchronize();
    vrng->avail->idx++;

    //notify device
    virtio_notify_avail(vrng->regs,0);
    //keep spainng until new idx change
    int pie=disable_interrupts();   
    while(vrng->used->idx==vrng->last_used_idx)
    {
        condition_wait(&vrng->ready);
    }
    restore_interrupts(pie);
    unsigned int bytes_received = vrng->used->ring[vrng->last_used_idx%vrng->qlen].len;
    if(bytes_received>request_size)
    {
        bytes_received=request_size;
    } 
    //memory copy
    memcpy(buf, vrng->entropy_buf, bytes_received);
    //reset the ring idex
    vrng->last_used_idx=vrng->used->idx;
    lock_release(&vrng->lock);
    return (int)bytes_received;
}

/*
    Input: int irqno --
           void * aux --   
    Output: None
    Discription:
        This function sets the appropriate device registers
        and wakes the thread up after waiting for the device to finish
        servicing a request. 
    Sideeffects:
        None
*/
void viorng_isr(int irqno, void * aux) {
    struct viorng_serial* const vrng =aux;
    (void)irqno;

    vrng->regs->interrupt_ack=vrng->regs->interrupt_status;
    condition_broadcast(&vrng->ready);
    // FIXME your code goes here
}