
#include "flash.h"
#include "hal.h"

// #pragma GCC optimize("O0")

/*
  this driver has been tested with STM32F427 and STM32F412
 */

#ifndef BOARD_FLASH_SIZE
#error "You must define BOARD_FLASH_SIZE in kbyte"
#endif

#define KB(x)   ((x*1024))
// Refer Flash memory map in the User Manual to fill the following fields per microcontroller
#define STM32_FLASH_BASE    0x08000000
#define STM32_FLASH_SIZE    KB(BOARD_FLASH_SIZE)

// optionally disable interrupts during flash writes
#define STM32_FLASH_DISABLE_ISR 0

// the 2nd bank of flash needs to be handled differently
#define STM32_FLASH_BANK2_START (STM32_FLASH_BASE+0x00080000)


#if BOARD_FLASH_SIZE == 512
#define STM32_FLASH_NPAGES  7
static const uint32_t flash_memmap[STM32_FLASH_NPAGES] = { KB(16), KB(16), KB(16), KB(16), KB(64),
                                                           KB(128), KB(128), KB(128) };

#elif BOARD_FLASH_SIZE == 1024
#define STM32_FLASH_NPAGES  12
static const uint32_t flash_memmap[STM32_FLASH_NPAGES] = { KB(16), KB(16), KB(16), KB(16), KB(64),
                                                           KB(128), KB(128), KB(128), KB(128), KB(128), KB(128), KB(128) };

#elif BOARD_FLASH_SIZE == 2048
#define STM32_FLASH_NPAGES  24
static const uint32_t flash_memmap[STM32_FLASH_NPAGES] = { KB(16), KB(16), KB(16), KB(16), KB(64),
                                                           KB(128), KB(128), KB(128), KB(128), KB(128), KB(128), KB(128),
                                                           KB(16), KB(16), KB(16), KB(16), KB(64),
                                                           KB(128), KB(128), KB(128), KB(128), KB(128), KB(128), KB(128)};
#endif

// keep a cache of the page addresses
static uint32_t flash_pageaddr[STM32_FLASH_NPAGES];
static bool flash_pageaddr_initialised;


#define FLASH_KEY1      0x45670123
#define FLASH_KEY2      0xCDEF89AB
/* Some compiler options will convert short loads and stores into byte loads
 * and stores.  We don't want this to happen for IO reads and writes!
 */
/* # define getreg16(a)       (*(volatile uint16_t *)(a)) */
static inline uint16_t getreg16(unsigned int addr)
{
    uint16_t retval;
    __asm__ __volatile__("\tldrh %0, [%1]\n\t" : "=r"(retval) : "r"(addr));
    return retval;
}

/* define putreg16(v,a)       (*(volatile uint16_t *)(a) = (v)) */
static inline void putreg16(uint16_t val, unsigned int addr)
{
    __asm__ __volatile__("\tstrh %0, [%1]\n\t": : "r"(val), "r"(addr));
}

static void stm32_flash_wait_idle(void)
{
	while (FLASH->SR & FLASH_SR_BSY) {
        // nop
    }
}

static void stm32_flash_unlock(void)
{
    stm32_flash_wait_idle();

    if (FLASH->CR & FLASH_CR_LOCK) {
        /* Unlock sequence */
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }

    // disable the data cache - see stm32 errata 2.1.11
    FLASH->ACR &= ~FLASH_ACR_DCEN;
}

void stm32_flash_lock(void)
{
    stm32_flash_wait_idle();
    FLASH->CR |= FLASH_CR_LOCK;

    // reset and re-enable the data cache - see stm32 errata 2.1.11
    FLASH->ACR |= FLASH_ACR_DCRST;
    FLASH->ACR &= ~FLASH_ACR_DCRST;
    FLASH->ACR |= FLASH_ACR_DCEN;
}



/*
  get the memory address of a page
 */
uint32_t stm32_flash_getpageaddr(uint32_t page)
{
    if (page >= STM32_FLASH_NPAGES) {
        return 0;
    }

    if (!flash_pageaddr_initialised) {
        uint32_t address = STM32_FLASH_BASE;
        uint8_t i;

        for (i = 0; i < STM32_FLASH_NPAGES; i++) {
            flash_pageaddr[i] = address;
            address += stm32_flash_getpagesize(i);
        }
        flash_pageaddr_initialised = true;
    }

    return flash_pageaddr[page];
}

/*
  get size in bytes of a page
 */
uint32_t stm32_flash_getpagesize(uint32_t page)
{
    return flash_memmap[page];
}

/*
  return total number of pages
 */
uint32_t stm32_flash_getnumpages()
{
    return STM32_FLASH_NPAGES;
}

static bool stm32_flash_ispageerased(uint32_t page)
{
    uint32_t addr;
    uint32_t count;

    if (page >= STM32_FLASH_NPAGES) {
        return false;
    }

    for (addr = stm32_flash_getpageaddr(page), count = stm32_flash_getpagesize(page);
        count; count--, addr++) {
        if ((*(volatile uint8_t *)(addr)) != 0xff) {
            return false;
        }
    }

    return true;
}

/*
  erase a page
 */
bool stm32_flash_erasepage(uint32_t page)
{
    if (page >= STM32_FLASH_NPAGES) {
        return false;
    }

#if STM32_FLASH_DISABLE_ISR
    syssts_t sts = chSysGetStatusAndLockX();
#endif
    stm32_flash_wait_idle();
    stm32_flash_unlock();
    
    // clear any previous errors
    FLASH->SR = 0xF3;

    stm32_flash_wait_idle();

    // the snb mask is not contiguous, calculate the mask for the page
    uint8_t snb = (((page % 12) << 3) | ((page / 12) << 7));
    
	FLASH->CR = FLASH_CR_PSIZE_1 | snb | FLASH_CR_SER;
	FLASH->CR |= FLASH_CR_STRT;

    stm32_flash_wait_idle();

    if (FLASH->SR) {
        // an error occurred
        FLASH->SR = 0xF3;
        stm32_flash_lock();
#if STM32_FLASH_DISABLE_ISR
        chSysRestoreStatusX(sts);
#endif
        return false;
    }
    
    stm32_flash_lock();
#if STM32_FLASH_DISABLE_ISR
    chSysRestoreStatusX(sts);
#endif

    return stm32_flash_ispageerased(page);
}


int32_t stm32_flash_write(uint32_t addr, const void *buf, uint32_t count)
{
    uint16_t *hword = (uint16_t *)buf;
    uint32_t written = count;

    /* STM32 requires half-word access */
    if (count & 1) {
        return -1;
    }

    if ((addr+count) >= STM32_FLASH_BASE+STM32_FLASH_SIZE) {
        return -1;
    }

    /* Get flash ready and begin flashing */

    if (!(RCC->CR & RCC_CR_HSION)) {
        return -1;
    }

#if STM32_FLASH_DISABLE_ISR
    syssts_t sts = chSysGetStatusAndLockX();
#endif
    
    stm32_flash_unlock();

    // clear previous errors
    FLASH->SR = 0xF3;

    /* TODO: implement up_progmem_write() to support other sizes than 16-bits */
    FLASH->CR &= ~(FLASH_CR_PSIZE);
    FLASH->CR |= FLASH_CR_PSIZE_0 | FLASH_CR_PG;

    for (;count; count -= 2, hword++, addr += 2) {
        /* Write half-word and wait to complete */

        putreg16(*hword, addr);

        stm32_flash_wait_idle();

        if (FLASH->SR) {
            // we got an error
            FLASH->SR = 0xF3;
            FLASH->CR &= ~(FLASH_CR_PG);
            goto failed;
        }

        if (getreg16(addr) != *hword) {
            FLASH->CR &= ~(FLASH_CR_PG);
            goto failed;
        }
    }

    FLASH->CR &= ~(FLASH_CR_PG);

    stm32_flash_lock();
#if STM32_FLASH_DISABLE_ISR
    chSysRestoreStatusX(sts);
#endif
    return written;

failed:
    stm32_flash_lock();
#if STM32_FLASH_DISABLE_ISR
    chSysRestoreStatusX(sts);
#endif
    return -1;
}
