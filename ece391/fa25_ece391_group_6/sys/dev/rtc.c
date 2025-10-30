// rtc.c - Goldfish RTC driver
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#include "rtc.h"
#include "conf.h"
#include "misc.h"
#include "devimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"

#include "error.h"

#include <stdint.h>

// INTERNAL TYPE DEFINITIONS
// 

struct rtc_regs {
    uint32_t time_low;  // read first, latches time_high
    uint32_t time_high; //
};

struct rtc_device {
    struct serial base; // must be first
    volatile struct rtc_regs * regs;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int rtc_open(struct serial * ser);
static void rtc_close(struct serial * ser);
static int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz);

static uint64_t read_real_time(volatile struct rtc_regs * regs);

// INTERNAL GLOBAL VARIABLES AND CONSTANTS
//

static const struct serial_intf rtc_serial_intf = {
    .blksz = 8,
    .open = &rtc_open,
    .close = &rtc_close,
    .recv = &rtc_recv
};

// EXPORTED FUNCTION DEFINITIONS
// 

/*
    Inputs: void * mmio_base -- base address pointing to RTC MMIO registers
    Outputs: None
    Descriptions: registers and initialize the RTC device
                 writing the interface and the device MMIO register base.
    Side Effects: malloc a space in heap
*/
void rtc_attach(void * mmio_base) {
    
    
    // FIXME your code goes here
    assert(mmio_base!=NULL);
    //malloc space for the device
    struct rtc_device *dev=kcalloc(1, sizeof(*dev));
    //initialize the dev
    assert(dev!=NULL);
    // initialize the RTC device
    dev->base=(struct serial){0};
    dev->regs=(volatile struct rtc_regs *)mmio_base;
    //register the device
    serial_init(&dev->base,&rtc_serial_intf);
    register_device("rtc", DEV_SERIAL, dev);
}

int rtc_open(struct serial * ser) {
    trace("%s()", __func__);
    return 0;
}

void rtc_close(struct serial * ser) {
    trace("%s()", __func__);
}
/*
    Inputs: struct serial * ser -- RTC serial interface object
           void * buf -- Output buffer to receive a 64-bit timestamp
           unsigned int bufsz -- Size of the output buffer
    Outputs: e number of bytes written to buf
    Descriptions: This function gets the current real-time clock value 
                 and copies it to the given buffer
    Side Effects: None
*/
int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    

    // FIXME your code goes here
    assert(ser!=NULL);
    assert(buf!=NULL);
    //check if it has enough space
    if(bufsz<sizeof(uint64_t))
    {
        return 0;
    }
    //n use the offsetof macro to calculate the base address of the rtc device
    //structure from the value of ser
    struct rtc_device *dev=(struct rtc_device*)((char*)ser-offsetof(struct rtc_device, base));
    uint64_t time=read_real_time(dev->regs);
    //copy to the buffer
    memcpy(buf, &time, sizeof(time));
    return (int)sizeof(uint64_t);
}
/*
    Inputs: volatile struct rtc_regs * regs -- RTC MMIO registers base
    Outputs: uint64_t -- timestamp
    Descriptions: t reads and returns the full 64-bit timestamp
    Side Effects: None
*/
uint64_t read_real_time(volatile struct rtc_regs * regs) {
    

    // FIXME your code goes here
    //read low first
    uint32_t low=regs->time_low;
    uint32_t high=regs->time_high;

    return ((uint64_t)high<<32) | ((uint64_t)low);
}