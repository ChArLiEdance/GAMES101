// uart.c -  NS8250-compatible serial port
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef UART_TRACE
#define TRACE
#endif

#ifdef UART_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "misc.h"
#include "uart.h"
#include "devimpl.h"
#include "intr.h"
#include "heap.h"
#include "thread.h"
#include "console.h"

#include "error.h"

#include <stdint.h>

// COMPILE-TIME CONSTANT DEFINITIONS
//

#ifndef UART_RBUFSZ
#define UART_RBUFSZ 64
#endif

#ifndef UART_INTR_PRIO
#define UART_INTR_PRIO 1
#endif

#ifndef UART_DEVNAME
#define UART_DEVNAME "uart"
#endif


// INTERNAL TYPE DEFINITIONS
// 

struct uart_regs {
    union {
        char rbr; // DLAB=0 read
        char thr; // DLAB=0 write
        uint8_t dll; // DLAB=1
    };
    
    union {
        uint8_t ier; // DLAB=0
        uint8_t dlm; // DLAB=1
    };
    
    union {
        uint8_t iir; // read
        uint8_t fcr; // write
    };

    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
};

#define LCR_DLAB (1 << 7)
#define LSR_OE (1 << 1)
#define LSR_DR (1 << 0)
#define LSR_THRE (1 << 5)
#define IER_DRIE (1 << 0)
#define IER_THREIE (1 << 1)

// Simple fixed-size ring buffer

struct ringbuf {
    unsigned int hpos; // head of queue (from where elements are removed)
    unsigned int tpos; // tail of queue (where elements are inserted)
    char data[UART_RBUFSZ];
};

// UART device structure

struct uart_serial {
    struct serial base;
    volatile struct uart_regs * regs;
    int irqno;
    char opened;

    unsigned long rxovrcnt; ///< number of times OE was set
    
    struct condition rxbnotempty; ///< signalled when rxbuf becomes not empty
    struct condition txbnotfull;  ///< signalled when txbuf becomes not full

    struct ringbuf rxbuf;
    struct ringbuf txbuf;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int uart_serial_open(struct serial * ser);
static void uart_serial_close(struct serial * ser);
static int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz);
static int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz);

static void uart_isr(int srcno, void * aux);

// Ring buffer (struct rbuf) functions

static void rbuf_init(struct ringbuf * rbuf);
static int rbuf_empty(const struct ringbuf * rbuf);
static int rbuf_full(const struct ringbuf * rbuf);
static void rbuf_putc(struct ringbuf * rbuf, char c);
static char rbuf_getc(struct ringbuf * rbuf);

// INTERNAL GLOBAL VARIABLES
//

static const struct serial_intf uart_serial_intf = {
    .blksz = 1,
    .open = &uart_serial_open,
    .close = &uart_serial_close,
    .recv = &uart_serial_recv,
    .send = &uart_serial_send
};

// EXPORTED FUNCTION DEFINITIONS
// 


void attach_uart(void * mmio_base, int irqno) {
    struct uart_serial * uart;

    trace("%s(%p,%d)", __func__, mmio_base, irqno);
    
    // UART0 is used for the console and should not be attached as a normal
    // device. It should already be initialized by console_init(). We still
    // register the device (to reserve the name uart0), but pass a NULL device
    // pointer, so that find_serial("uart", 0) returns NULL.

    if (mmio_base == (void*)UART0_MMIO_BASE) {
        register_device(UART_DEVNAME, DEV_SERIAL, NULL);
        return;
    }
    
    uart = kcalloc(1, sizeof(struct uart_serial));

    uart->regs = mmio_base;
    uart->irqno = irqno;
    uart->opened = 0;

    // Initialize condition variables. The ISR is registered when our interrupt
    // source is enabled in uart_serial_open().

    condition_init(&uart->rxbnotempty, "uart.rxnotempty");
    condition_init(&uart->txbnotfull, "uart.txnotfull");


    // Initialize hardware

    uart->regs->ier = 0;
    uart->regs->lcr = LCR_DLAB;
    // fence o,o ?
    uart->regs->dll = 0x01;
    uart->regs->dlm = 0x00;
    // fence o,o ?
    uart->regs->lcr = 0; // DLAB=0

    serial_init(&uart->base, &uart_serial_intf);
    register_device(UART_DEVNAME, DEV_SERIAL, uart);
}

/**  
 * Open the UART device. 
 * Initialize ring buffers, enables DR interruptes 
 * and registers PLIC source with ISR.
 * 
 * @param ser pointer to the UART serial device
 * @return 0 on success; -EBUSY otherwise
*/
int uart_serial_open(struct serial * ser) {
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    if (uart->opened)
        return -EBUSY;
    
    // Reset receive and transmit buffers
    
    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);

    // Read receive buffer register to flush any stale data in hardware buffer

    uart->regs->rbr; // forces a read because uart->regs is volatile

    // Enable interrupts when data ready (DR) status asserted

    // FIXME your code goes here
    uart->regs->ier=IER_DRIE;
    enable_intr_source(uart->irqno, UART_INTR_PRIO, uart_isr, uart);
    uart->opened=1;
    return 0;
}

/**  
 * Close the UART device. 
 * Disable all UART interrupts  
 * and mark the device closed
 * 
 * @param ser pointer to the UART serial device
 * @return none
*/
void uart_serial_close(struct serial * ser) {
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    // FIXME your code goes here
    if(!uart->opened){
        return;
    }
    uart->regs->ier=0;
    disable_intr_source(uart->irqno);
    uart->opened=0;

    /////cp3
    condition_broadcast(&uart->rxbnotempty);
    condition_broadcast(&uart->txbnotfull);
}

/**  
 * Receive up to bufsz bytes from the UART into buf. 
 * 
 * @param ser pointer to the UART serial device
 * @param buf pointer to the destination buffer
 * @param bufsz max number of bytes to read
 * @return numbers of bytes received; -EINVAL otherwise
*/
int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    // FIXME your code goes here

    /* these are cp1
    struct uart_serial* const uart=(void*)ser-offsetof(struct uart_serial, base);
    if(!uart->opened){
        return -EINVAL;
    }
    unsigned int bytesread=0;
    char *char_buf=(char*) buf;
    while(bytesread<bufsz){
        while(rbuf_empty(&uart->rxbuf)){
            uart->regs->ier |= IER_THREIE;//空循环加东西
        }
        char_buf[bytesread++]=rbuf_getc(&uart->rxbuf);
    }
    return bytesread;
    */
    struct uart_serial* const uart=(void*)ser-offsetof(struct uart_serial, base);
    if(!uart->opened || buf==NULL){
        return -EINVAL;
    }
    if(bufsz==0){
        return 0;
    }

    char *dst=(char*)buf;
    unsigned int n=0;
    
    int pie=disable_interrupts();
    while(rbuf_empty(&uart->rxbuf)){
        uart->regs->ier |= IER_DRIE;
        condition_wait(&uart->rxbnotempty);
    }
    while(n<bufsz&&!rbuf_empty(&uart->rxbuf)){
        dst[n++]=rbuf_getc(&uart->rxbuf);
    }
    restore_interrupts(pie);
    return (int)n;
}


/**  
 * Send up to bufsz bytes from buf via UART. \
 * 
 * @param ser pointer to the UART serial device
 * @param buf pointer to the source buffer
 * @param bufsz number of bytes to send
 * @return number of bytes sent; -EINVAL otherwise
*/
int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz) {
    // FIXME your code goes here

    /* these are cp1
    struct uart_serial* const uart=(void*)ser-offsetof(struct uart_serial,base);
    if(!uart->opened){
        return -EINVAL;
    }
    unsigned int bytessend=0;
    char *char_buf=(char*) buf;
    while(bytessend<bufsz){
        while(rbuf_full(&uart->txbuf)){
            uart->regs->ier |=IER_THREIE;//空循环加东西
        }
        rbuf_putc(&uart->txbuf, char_buf[bytessend++]);
    }
    uart->regs->ier |=IER_THREIE;
    return bytessend;
    */
    struct uart_serial* const uart=(void*)ser-offsetof(struct uart_serial,base);
    if(!uart->opened || buf==NULL){
        return -EINVAL;
    }
    if(bufsz==0){
        return 0;
    }
    const char* src=(const char*)buf;
    unsigned int n=0;

    while(n<bufsz){
        int pie=disable_interrupts();
        while(rbuf_full(&uart->txbuf)){
            uart->regs->ier|=IER_THREIE;
            condition_wait(&uart->txbnotfull);
        }
        restore_interrupts(pie);
        while(n<bufsz && !rbuf_full(&uart->txbuf)){
            rbuf_putc(&uart->txbuf, src[n++]);
        }
        uart->regs->ier|=IER_THREIE;
    }
    return (int)n;
}

/**  
 * UART Interrupt Service Routine. 
 * 
 * @param srcno PLIC source number
 * @param aux back pointer to uart_serial
 * @return none
*/
void uart_isr(int srcno, void * aux) {
    // FIXME your code goes here

    /* these are cp1
    struct uart_serial* const uart=(struct uart_serial*) aux;
    if(uart->regs->lsr & LSR_DR){
        if(!rbuf_full(&uart->rxbuf)){
            rbuf_putc(&uart->rxbuf, uart->regs->rbr);
        }else{
            uart->regs->ier &= ~IER_DRIE;
        }
    }
    if(uart->regs->lsr & LSR_THRE){
        if(!rbuf_empty(&uart->txbuf)){
            uart->regs->thr=rbuf_getc(&uart->txbuf);
        }else{
            uart->regs->ier &= ~IER_THREIE;
        }
    }
        */
    struct uart_serial* const uart=(struct uart_serial*) aux;
    volatile struct uart_regs* const R=uart->regs;
    uint8_t lsr=R->lsr;

    if(lsr&LSR_DR){
        if(!rbuf_full(&uart->rxbuf)){
            rbuf_putc(&uart->rxbuf,R->rbr);
            condition_broadcast(&uart->rxbnotempty);
        }else{
            R->ier&=~IER_DRIE;
        }
    }
    if(lsr&LSR_THRE){
        if(!rbuf_empty(&uart->txbuf)){
            R->thr=rbuf_getc(&uart->txbuf);
            condition_broadcast(&uart->txbnotfull);
        }else{
            R->ier&=~IER_THREIE;
        }
    }
}

void rbuf_init(struct ringbuf * rbuf) {
    rbuf->hpos = 0;
    rbuf->tpos = 0;
}



int rbuf_empty(const struct ringbuf * rbuf) {
    return (rbuf->hpos == rbuf->tpos);
}


int rbuf_full(const struct ringbuf * rbuf) {
    return (rbuf->tpos - rbuf->hpos == UART_RBUFSZ);
}


void rbuf_putc(struct ringbuf * rbuf, char c) {
    uint_fast16_t tpos;

    tpos = rbuf->tpos;
    rbuf->data[tpos % UART_RBUFSZ] = c;
    asm volatile ("" ::: "memory");
    rbuf->tpos = tpos + 1;
}

char rbuf_getc(struct ringbuf * rbuf) {
    uint_fast16_t hpos;
    char c;

    hpos = rbuf->hpos;
    c = rbuf->data[hpos % UART_RBUFSZ];
    asm volatile ("" ::: "memory");
    rbuf->hpos = hpos + 1;
    return c;
}

// The functions below provide polled uart input and output for the console.

#define UART0 (*(volatile struct uart_regs*)UART0_MMIO_BASE)

void console_device_init(void) {
    UART0.ier = 0x00;

    // Configure UART0. We set the baud rate divisor to 1, the lowest value,
    // for the fastest baud rate. In a physical system, the actual baud rate
    // depends on the attached oscillator frequency. In a virtualized system,
    // it doesn't matter.
    
    UART0.lcr = LCR_DLAB;
    UART0.dll = 0x01;
    UART0.dlm = 0x00;

    // The com0_putc and com0_getc functions assume DLAB=0.

    UART0.lcr = 0;
}

void console_device_putc(char c) {
    // Spin until THR is empty
    while (!(UART0.lsr & LSR_THRE))
        continue;

    UART0.thr = c;
}

char console_device_getc(void) {
    // Spin until RBR contains a byte
    while (!(UART0.lsr & LSR_DR))
        continue;
    
    return UART0.rbr;
}