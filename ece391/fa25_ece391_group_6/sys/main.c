/*! @file main.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‍‌⁠​⁠⁠‌​⁠⁠‌
    @brief main function of the kernel (called from start.s)
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include "cache.h"
#include "conf.h"
#include "console.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "dev/virtio.h"
#include "device.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "intr.h"
#include "string.h"
#include "thread.h"
#include "timer.h"
#include "elf.h"
#include "uio.h"

//#define INITEXE "usr/bin/hello"  // FIXME
#define INITEXE "usr/games/trek"
#define CMNTNAME "c"
#define DEVMNTNAME "dev"
#define CDEVNAME "vioblk"
#define CDEVINST 0

#ifndef NUART  // number of UARTs
#define NUART 2
#endif

#ifndef NVIODEV  // number of VirtIO devices
#define NVIODEV 8
#endif

static void attach_devices(void);
static void mount_cdrive(void);  // mount primary storage device ("C drive")
static void run_init(void);
static void wait_for_terminal_attach(struct uio* term);

void main(void) {
    extern char _kimg_end[];  // provided by kernel.ld
    console_init();
    intrmgr_init();
    devmgr_init();
    thrmgr_init();
    heap_init(_kimg_end, RAM_END);

    attach_devices();

    enable_interrupts();

    mount_cdrive();
    run_init();
}

void attach_devices(void) {
    int i;
    int result;

    rtc_attach((void*)RTC_MMIO_BASE);

    for (i = 0; i < NUART; i++) attach_uart((void*)UART_MMIO_BASE(i), UART0_INTR_SRCNO + i);

    for (i = 0; i < NVIODEV; i++) attach_virtio((void*)VIRTIO_MMIO_BASE(i), VIRTIO0_INTR_SRCNO + i);

    result = mount_devfs(DEVMNTNAME);

    if (result != 0) {
        kprintf("mount_devfs(%s) failed: %s\n", CDEVNAME, error_name(result));
        halt_failure();
    }
}

void mount_cdrive(void) {
    struct storage* hd;
    struct cache* cache;
    int result;

    hd = find_storage(CDEVNAME, CDEVINST);

    if (hd == NULL) {
        kprintf("Storage device %s%d not found\n", CDEVNAME, CDEVINST);
        halt_failure();
    }

    result = storage_open(hd);

    if (result != 0) {
        kprintf("storage_open failed on %s%d: %s\n", CDEVNAME, CDEVINST, error_name(result));
        halt_failure();
    }

    result = create_cache(hd, &cache);

    if (result != 0) {
        kprintf("create_cache(%s%d) failed: %s\n", CDEVNAME, CDEVINST, error_name(result));
        halt_failure();
    }

    result = mount_ktfs(CMNTNAME, cache);

    if (result != 0) {
        kprintf("mount_ktfs(%s, cache(%s%d)) failed: %s\n", CMNTNAME, CDEVNAME, CDEVINST,
                error_name(result));
        halt_failure();
    }
}

static void wait_for_terminal_attach(struct uio* term) {
    static const char prompt[] = "\r\n*** Connect to serial1 (trek) ***\r\n";

    if (term == NULL) return;

    while (uio_write(term, prompt, sizeof(prompt) - 1) < 0) {
        sleep_ms(100);
    }
    // Small pause so the banner is visible before the game starts spewing output.
    sleep_ms(50);
}

void run_init(void) {
    struct uio* initexe;
    struct uio* console_uio;
    void (*entry)(void);
    int result;

    result = open_file(CMNTNAME, INITEXE, &initexe);

    if (result != 0) {
        kprintf(INITEXE ": %s; terminating\n", error_name(result));
        halt_failure();
    }

    result = open_file(DEVMNTNAME, "uart", &console_uio);
    if (result != 0) {
        kprintf("open_file(" DEVMNTNAME ",uart): %s; terminating\n", error_name(result));
        halt_failure();
    }

    wait_for_terminal_attach(console_uio);

    result = elf_load(initexe, &entry);
    if (result != 0) {
        kprintf(INITEXE ": elf_load failed: %s; terminating\n", error_name(result));
        halt_failure();
    }

    uio_close(initexe);

    ((void (*)(struct uio*))entry)(console_uio);

    uio_close(console_uio);
}
