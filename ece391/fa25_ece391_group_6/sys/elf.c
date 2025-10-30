/*! @file elf.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‍‌⁠​⁠⁠‌​⁠⁠‌
    @brief ELF file loader
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#ifdef ELF_TRACE
#define TRACE
#endif

#ifdef ELF_DEBUG
#define DEBUG
#endif

#include "elf.h"

#include <stdint.h>

#include "conf.h"
#include "error.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "uio.h"

// Offsets into e_ident

#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_ABIVERSION 8
#define EI_PAD 9

// ELF header e_ident[EI_CLASS] values

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

// ELF header e_ident[EI_DATA] values

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// ELF header e_ident[EI_VERSION] values

#define EV_NONE 0
#define EV_CURRENT 1

// ELF header e_type values

enum elf_et { ET_NONE = 0, ET_REL, ET_EXEC, ET_DYN, ET_CORE };

/*! @struct elf64_ehdr
    @brief ELF header struct
*/
struct elf64_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

/*! @enum elf_pt
    @brief Program header p_type values
*/
enum elf_pt { PT_NULL = 0, PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_NOTE, PT_SHLIB, PT_PHDR, PT_TLS };

// Program header p_flags bits

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/*! @struct elf64_phdr
    @brief Program header struct
*/
struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

// ELF header e_machine values (short list)

#define EM_RISCV 243
/**
 * \brief Validates and loads an ELF file into memory.
 *
 * This function validates an ELF file, then loads its contents into memory,
 * returning the start of the entry point through \p eptr.
 *
 * The loader processes only program header entries of type `PT_LOAD`. The layouts
 * of structures and magic values can be found in the Linux ELF header file
 * `<uapi/linux/elf.h>`
 * The implementation should ensure that all loaded sections of the program are
 * mapped within the memory range `0x80100000` to `0x81000000`.
 *
 * Let's do some reading! The following documentation will be very helpful!
 * [Helpful doc](https://linux.die.net/man/5/elf)
 * Good luck!
 * [Educational video](https://www.youtube.com/watch?v=dQw4w9WgXcQ)
 *
 * \param[in]  uio  Pointer to an user I/O corresponding to the ELF file.
 * \param[out] eptr   Double pointer used to return the ELF file's entry point.
 *
 * \return 0 on success, or a negative error code on failure.
 */
int elf_load(struct uio* uio, void (**eptr)(void)) {
    // FIXME
    const unsigned long long load_base = 0x80100000ULL;
    const unsigned long long load_limit = 0x81000000ULL;
    struct elf64_ehdr ehdr;
    unsigned long long filesize = 0;
    unsigned long long ph_table_end;
    unsigned long long pos = 0;
    long readcnt;
    int rc;

    if (uio == NULL || eptr == NULL) return -EINVAL;

    rc = uio_cntl(uio, FCNTL_GETEND, &filesize);
    if (rc < 0) return rc;

    rc = uio_cntl(uio, FCNTL_SETPOS, &pos);
    if (rc < 0) return rc;

    readcnt = uio_read(uio, &ehdr, sizeof(ehdr));
    if (readcnt < 0) return (int)readcnt;
    if ((unsigned long)readcnt != sizeof(ehdr)) return -EBADFMT;

    if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' || ehdr.e_ident[2] != 'L' ||
        ehdr.e_ident[3] != 'F')
        return -EBADFMT;
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) return -EBADFMT;
    if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) return -EBADFMT;
    if (ehdr.e_ident[EI_VERSION] != EV_CURRENT) return -EBADFMT;
    if (ehdr.e_type != ET_EXEC) return -EBADFMT;
    if (ehdr.e_machine != EM_RISCV) return -EBADFMT;
    if (ehdr.e_ehsize != sizeof(struct elf64_ehdr)) return -EBADFMT;
    if (ehdr.e_phentsize != sizeof(struct elf64_phdr)) return -EBADFMT;
    if (ehdr.e_phnum == 0) return -EBADFMT;

    if (ehdr.e_entry < load_base || ehdr.e_entry >= load_limit) return -EBADFMT;

    if (ehdr.e_phoff > filesize) return -EBADFMT;
    ph_table_end = ehdr.e_phoff + (unsigned long long)ehdr.e_phnum * ehdr.e_phentsize;
    if (ph_table_end > filesize || ph_table_end < ehdr.e_phoff) return -EBADFMT;

    for (uint16_t i = 0; i < ehdr.e_phnum; ++i) {
        struct elf64_phdr phdr;
        unsigned long long ph_offset = ehdr.e_phoff + (unsigned long long)i * ehdr.e_phentsize;

        rc = uio_cntl(uio, FCNTL_SETPOS, &ph_offset);
        if (rc < 0) return rc;

        readcnt = uio_read(uio, &phdr, sizeof(phdr));
        if (readcnt < 0) return (int)readcnt;
        if ((unsigned long)readcnt != sizeof(phdr)) return -EBADFMT;

        if (phdr.p_type != PT_LOAD) continue;

        if (phdr.p_memsz < phdr.p_filesz) return -EBADFMT;
        if (phdr.p_memsz == 0) continue;

        if (phdr.p_vaddr < load_base) return -EBADFMT;
        if (phdr.p_vaddr + phdr.p_memsz > load_limit || phdr.p_vaddr + phdr.p_memsz < phdr.p_vaddr)
            return -EBADFMT;

        if (phdr.p_offset > filesize) return -EBADFMT;
        if (phdr.p_offset + phdr.p_filesz > filesize || phdr.p_offset + phdr.p_filesz < phdr.p_offset)
            return -EBADFMT;

        if (phdr.p_filesz > 0) {
            unsigned long long seg_pos = phdr.p_offset;
            uintptr_t dest = (uintptr_t)phdr.p_vaddr;

            rc = uio_cntl(uio, FCNTL_SETPOS, &seg_pos);
            if (rc < 0) return rc;

            readcnt = uio_read(uio, (void*)dest, (unsigned long)phdr.p_filesz);
            if (readcnt < 0) return (int)readcnt;
            if ((unsigned long long)readcnt != phdr.p_filesz) return -EIO;
        }

        if (phdr.p_memsz > phdr.p_filesz) {
            uintptr_t bss_start = (uintptr_t)phdr.p_vaddr + phdr.p_filesz;
            unsigned long clear_sz = (unsigned long)(phdr.p_memsz - phdr.p_filesz);
            memset((void*)bss_start, 0, clear_sz);
        }
    }

    *eptr = (void (*)(void))(uintptr_t)ehdr.e_entry;
    return 0;
}
