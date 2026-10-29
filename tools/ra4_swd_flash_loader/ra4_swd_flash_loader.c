/*
 * Minimal RA4M1 FLASH_LP code-flash loader.
 *
 * This program is linked into RA4M1 SRAM and must execute entirely from SRAM.
 * It has no startup code, libc, interrupts, Flash-resident calls, or Arduino
 * runtime dependencies. The host owns the mailbox and resumes this entry point
 * once for each bounded erase/program/cleanup command.
 *
 * Register sequence is based on Renesas FSP r_flash_lp.c (BSD-3-Clause):
 * zephyr/modules/hal/renesas/drivers/ra/fsp/src/r_flash_lp/r_flash_lp.c
 */

#include <stdint.h>

#define LOADER_MAILBOX_MAGIC         0x4450374CUL
#define LOADER_MAILBOX_VERSION       1U

#define LOADER_COMMAND_NONE          0U
#define LOADER_COMMAND_ERASE         1U
#define LOADER_COMMAND_PROGRAM       2U
#define LOADER_COMMAND_CLEANUP       3U

#define LOADER_STATUS_EMPTY          0U
#define LOADER_STATUS_READY          1U
#define LOADER_STATUS_RUNNING        2U
#define LOADER_STATUS_DONE           3U
#define LOADER_STATUS_ERROR          4U

#define LOADER_ERROR_BAD_MAILBOX     1U
#define LOADER_ERROR_BAD_RANGE       2U
#define LOADER_ERROR_TIMEOUT         3U
#define LOADER_ERROR_ERASE           4U
#define LOADER_ERROR_PROGRAM         5U
#define LOADER_ERROR_UNKNOWN_COMMAND 6U

#define CODE_FLASH_START             0x00004000UL
#define CODE_FLASH_END               0x00040000UL
#define CODE_FLASH_BLOCK_SIZE        0x00000800UL
#define CODE_FLASH_WRITE_SIZE        8U

#define FACI_LP_BASE                 0x407EC000UL
#define FACI_FPMCR                   (FACI_LP_BASE + 0x100UL)
#define FACI_FASR                    (FACI_LP_BASE + 0x104UL)
#define FACI_FSARL                   (FACI_LP_BASE + 0x108UL)
#define FACI_FSARH                   (FACI_LP_BASE + 0x110UL)
#define FACI_FCR                    (FACI_LP_BASE + 0x114UL)
#define FACI_FEARL                   (FACI_LP_BASE + 0x118UL)
#define FACI_FEARH                   (FACI_LP_BASE + 0x120UL)
#define FACI_FRESETR                 (FACI_LP_BASE + 0x124UL)
#define FACI_FSTATR1                 (FACI_LP_BASE + 0x12CUL)
#define FACI_FWBL0                   (FACI_LP_BASE + 0x130UL)
#define FACI_FWBH0                   (FACI_LP_BASE + 0x138UL)
#define FACI_FWBL1                   (FACI_LP_BASE + 0x140UL)
#define FACI_FWBH1                   (FACI_LP_BASE + 0x144UL)
#define FACI_FSTATR2                 (FACI_LP_BASE + 0x1F0UL)
#define FACI_FENTRYR                 (FACI_LP_BASE + 0x3FB2UL)
#define FACI_PFBER                   (FACI_LP_BASE + 0x3FC8UL)
#define FACI_DFLCTL                  (FACI_LP_BASE + 0x3FC6UL)

#define FCACHE_FCACHEE               0x4001C100UL
#define SYSTEM_OPCCR                 0x4001E0A0UL
#define SYSTEM_SCKDIVCR              0x4001E020UL
#define SYSTEM_SCKSCR                0x4001E026UL

#define FPMCR_READ_MODE              0x08U
#define FPMCR_DATAFLASH_PE_MODE      0x10U
#define FPMCR_DISCHARGE_1            0x12U
#define FPMCR_DISCHARGE_2            0x92U
#define FPMCR_CODEFLASH_PE_MODE      0x82U
#define FPMCR_LVPE                   0x40U
#define FPR_UNLOCK                   0xA5U

#define FENTRYR_CODEFLASH_PE_MODE    0xAA01U
#define FENTRYR_DATAFLASH_PE_MODE    0xAA80U
#define FENTRYR_READ_MODE            0xAA00U

#define FCR_CLEAR                    0x00U
#define FCR_WRITE                    0x81U
#define FCR_ERASE                    0x84U
#define FCR_PROCESSING               0x80U

#define FSTATR1_FRDY                 0x40UL
#define FSTATR2_ERASE_ERRORS         0x11UL
#define FSTATR2_PROGRAM_ERRORS       0x12UL

#define COMMAND_TIMEOUT_LOOPS        100000000UL
#define MODE_TIMEOUT_LOOPS           1000000UL
#define SYSTEM_CLOCK_MHZ             48UL
#define FCLK_HZ                      24000000UL
#define FISR_PCKA_24MHZ              ((FCLK_HZ / 1000000UL) - 1UL)

typedef struct
{
    volatile uint32_t magic;             /* 0x00 */
    volatile uint32_t version;           /* 0x04 */
    volatile uint32_t command;           /* 0x08 */
    volatile uint32_t status;            /* 0x0C */
    volatile uint32_t address;           /* 0x10 */
    volatile uint32_t length;            /* 0x14 */
    volatile uint32_t source;            /* 0x18 */
    volatile uint32_t completed_units;   /* 0x1C */
    volatile uint32_t last_fstatr1;      /* 0x20 */
    volatile uint32_t last_fstatr2;      /* 0x24 */
    volatile uint32_t error;             /* 0x28 */
} loader_mailbox_t;

static inline volatile uint8_t *reg8(uint32_t address)
{
    return (volatile uint8_t *) address;
}

static inline volatile uint16_t *reg16(uint32_t address)
{
    return (volatile uint16_t *) address;
}

static inline volatile uint32_t *reg32(uint32_t address)
{
    return (volatile uint32_t *) address;
}

static void delay_us(uint32_t microseconds)
{
    uint32_t loops = (microseconds * SYSTEM_CLOCK_MHZ) / 4UL;
    while (loops != 0U)
    {
        __asm volatile (
            "1: sub %[count], %[count], #1\n"
            "cmp %[count], #0\n"
            "bne.n 1b\n"
            : [count] "+r" (loops)
            :
            : "cc");
    }
}

static void write_fpmcr(uint8_t value)
{
    *reg32(FACI_LP_BASE + 0x180UL) = FPR_UNLOCK;
    *reg8(FACI_FPMCR) = value;
    *reg8(FACI_FPMCR) = (uint8_t) ~value;
    *reg8(FACI_FPMCR) = value;
    __asm volatile ("nop");
}

static int wait_ready(uint32_t timeout)
{
    volatile uint32_t loops = timeout;

    /*
     * FSTATR1.FRDY is still high immediately after an FCR command write on
     * this tightly optimized SRAM path.  Observe the sequencer entering its
     * busy state before accepting a later high level as command completion;
     * otherwise FCR_CLEAR can cancel the command before it starts.
     */
    while ((*reg32(FACI_FSTATR1) & FSTATR1_FRDY) != 0U)
    {
        if (loops-- == 0U)
        {
            return 0;
        }
    }

    loops = timeout;
    while ((*reg32(FACI_FSTATR1) & FSTATR1_FRDY) == 0U)
    {
        if (loops-- == 0U)
        {
            return 0;
        }
    }

    *reg8(FACI_FCR) = FCR_CLEAR;
    loops = timeout;
    while ((*reg32(FACI_FSTATR1) & FSTATR1_FRDY) != 0U)
    {
        if (loops-- == 0U)
        {
            return 0;
        }
    }
    return 1;
}

static int enter_code_flash_pe(void)
{
    *reg8(FACI_PFBER) = 0U;
    *reg16(FCACHE_FCACHEE) = 0U;
    *reg16(FACI_FENTRYR) = FENTRYR_CODEFLASH_PE_MODE;

    write_fpmcr(FPMCR_DISCHARGE_1);
    delay_us(3U);

    if ((*reg8(SYSTEM_OPCCR) & 0x03U) == 0U)
    {
        write_fpmcr(FPMCR_DISCHARGE_2);
        write_fpmcr(FPMCR_CODEFLASH_PE_MODE);
        delay_us(6U);
    }
    else
    {
        write_fpmcr((uint8_t) (FPMCR_DISCHARGE_2 | FPMCR_LVPE));
        write_fpmcr((uint8_t) (FPMCR_CODEFLASH_PE_MODE | FPMCR_LVPE));
        delay_us(4U);
    }
    return 1;
}

static int leave_data_flash_pe(void)
{
    write_fpmcr(FPMCR_READ_MODE);
    delay_us(6U);
    *reg16(FACI_FENTRYR) = FENTRYR_READ_MODE;

    volatile uint32_t loops = MODE_TIMEOUT_LOOPS;
    while (*reg16(FACI_FENTRYR) != 0U)
    {
        if (loops-- == 0U)
        {
            return 0;
        }
    }
    return 1;
}

static int leave_flash_pe(void)
{
    write_fpmcr(FPMCR_DISCHARGE_2);
    delay_us(3U);
    write_fpmcr(FPMCR_DISCHARGE_1);
    write_fpmcr(FPMCR_READ_MODE);
    delay_us(6U);
    *reg16(FACI_FENTRYR) = FENTRYR_READ_MODE;

    volatile uint32_t loops = MODE_TIMEOUT_LOOPS;
    while (*reg16(FACI_FENTRYR) != 0U)
    {
        if (loops-- == 0U)
        {
            return 0;
        }
    }

    *reg16(FCACHE_FCACHEE) = 1U;
    *reg8(FACI_PFBER) = 1U;
    return 1;
}

static int configure_flash_interface(void)
{
    /* The host uses reset-halt, so no Arduino clock startup has run. OFS1 on
     * the UNO R4 selects 48 MHz HOCO. Establish ICLK=48/FCLK=24 MHz ourselves,
     * retaining the reset peripheral dividers. Never write configuration Flash.
     */
    if ((*reg32(0x00000404UL) & 0x00007000UL) != 0x00004000UL)
    {
        return 0;
    }
    *reg16(0x4001E3FEUL) = 0xA501U;
    *reg8(0x4001E036UL) = 0U;
    uint32_t remaining = MODE_TIMEOUT_LOOPS;
    while ((*reg8(0x4001E03CUL) & 1U) == 0U)
    {
        if (remaining-- == 0U) { return 0; }
    }
    *reg16(FCACHE_FCACHEE) = 0U;
    *reg8(0x4001E031UL) = 1U; /* Flash reads above 32 MHz need one wait. */
    *reg32(SYSTEM_SCKDIVCR) =
        (*reg32(SYSTEM_SCKDIVCR) & ~0x77000000UL) | 0x10000000UL;
    *reg8(SYSTEM_SCKSCR) = 0U;
    __asm volatile ("dsb\nisb" ::: "memory");
    *reg16(0x4001E3FEUL) = 0xA500U;
    /*
     * FISR is reset to zero by the normal Arduino image because it does not
     * enable the Code Flash FSP driver.  FLASH_LP requires FISR.PCKA to match
     * the live FCLK before Code Flash program/erase.  This UNO R4 profile uses
     * 48 MHz HOCO with FCLK divided by two.  Fail closed if a future Sketch
     * changes that clock arrangement instead of programming with a stale rate.
     */
    if ((*reg8(SYSTEM_SCKSCR) & 0x07U) != 0U ||
        ((*reg32(SYSTEM_SCKDIVCR) >> 28) & 0x07U) != 1U)
    {
        return 0;
    }

    *reg16(FACI_FENTRYR) = FENTRYR_DATAFLASH_PE_MODE;
    delay_us(6U);
    if ((*reg8(SYSTEM_OPCCR) & 0x03U) == 0U)
    {
        write_fpmcr(FPMCR_DATAFLASH_PE_MODE);
    }
    else
    {
        write_fpmcr((uint8_t) (FPMCR_DATAFLASH_PE_MODE | FPMCR_LVPE));
    }

    *reg8(FACI_DFLCTL) = 1U;
    *reg32(FACI_LP_BASE + 0x1D8UL) = FISR_PCKA_24MHZ;
    return leave_data_flash_pe();
}

static int valid_erase_range(uint32_t address, uint32_t length)
{
    if (length == 0U || (address & (CODE_FLASH_BLOCK_SIZE - 1U)) != 0U)
    {
        return 0;
    }
    if (length != CODE_FLASH_BLOCK_SIZE ||
        address < CODE_FLASH_START ||
        address >= CODE_FLASH_END ||
        address + length > CODE_FLASH_END)
    {
        return 0;
    }
    return 1;
}

static int valid_program_range(uint32_t address, uint32_t length, uint32_t source)
{
    if (length == 0U || (address & (CODE_FLASH_WRITE_SIZE - 1U)) != 0U ||
        (length & (CODE_FLASH_WRITE_SIZE - 1U)) != 0U)
    {
        return 0;
    }
    if (address < CODE_FLASH_START || address >= CODE_FLASH_END ||
        address + length > CODE_FLASH_END || source < 0x20000000UL ||
        source + length < source || source + length > 0x20008000UL)
    {
        return 0;
    }
    return 1;
}

static int erase_block(uint32_t address, loader_mailbox_t *mailbox)
{
    *reg8(FACI_FASR) = 0U;
    *reg16(FACI_FSARH) = (uint16_t) (address >> 16);
    *reg16(FACI_FSARL) = (uint16_t) address;
    *reg32(FACI_FEARH) = (uint32_t) ((address + CODE_FLASH_BLOCK_SIZE - 1U) >> 16);
    *reg16(FACI_FEARL) = (uint16_t) (address + CODE_FLASH_BLOCK_SIZE - 1U);
    *reg8(FACI_FCR) = FCR_ERASE;

    if (!wait_ready(COMMAND_TIMEOUT_LOOPS))
    {
        mailbox->last_fstatr1 = *reg32(FACI_FSTATR1);
        mailbox->last_fstatr2 = *reg32(FACI_FSTATR2);
        return 0;
    }
    mailbox->last_fstatr1 = *reg32(FACI_FSTATR1);
    mailbox->last_fstatr2 = *reg32(FACI_FSTATR2);
    return (mailbox->last_fstatr2 & FSTATR2_ERASE_ERRORS) == 0U;
}

static int program_units(
    uint32_t address,
    uint32_t length,
    uint32_t source,
    loader_mailbox_t *mailbox)
{
    uint32_t units = length / CODE_FLASH_WRITE_SIZE;
    uint32_t index;

    *reg8(FACI_FASR) = 0U;
    for (index = 0U; index < units; ++index)
    {
        uint32_t target = address + index * CODE_FLASH_WRITE_SIZE;
        const uint16_t *data = (const uint16_t *) (source + index * CODE_FLASH_WRITE_SIZE);

        *reg16(FACI_FSARH) = (uint16_t) (target >> 16);
        *reg16(FACI_FSARL) = (uint16_t) target;
        *reg32(FACI_FWBL0) = data[0];
        *reg32(FACI_FWBH0) = data[1];
        *reg32(FACI_FWBL1) = data[2];
        *reg32(FACI_FWBH1) = data[3];
        *reg8(FACI_FCR) = FCR_WRITE;

        if (!wait_ready(COMMAND_TIMEOUT_LOOPS))
        {
            mailbox->completed_units = index;
            mailbox->last_fstatr1 = *reg32(FACI_FSTATR1);
            mailbox->last_fstatr2 = *reg32(FACI_FSTATR2);
            return 0;
        }
        mailbox->completed_units = index + 1U;
        mailbox->last_fstatr1 = *reg32(FACI_FSTATR1);
        mailbox->last_fstatr2 = *reg32(FACI_FSTATR2);
        if ((mailbox->last_fstatr2 & FSTATR2_PROGRAM_ERRORS) != 0U)
        {
            return 0;
        }
    }
    return 1;
}

static void process_command(loader_mailbox_t *mailbox)
{
    uint32_t command = mailbox->command;
    int success = 0;

    mailbox->status = LOADER_STATUS_RUNNING;
    mailbox->error = 0U;
    mailbox->completed_units = 0U;

    if (command == LOADER_COMMAND_ERASE)
    {
        if (!valid_erase_range(mailbox->address, mailbox->length))
        {
            mailbox->error = LOADER_ERROR_BAD_RANGE;
        }
        else if (!enter_code_flash_pe())
        {
            mailbox->error = LOADER_ERROR_TIMEOUT;
        }
        else
        {
            success = erase_block(mailbox->address, mailbox);
            if (!success)
            {
                mailbox->error =
                    (mailbox->last_fstatr2 & FSTATR2_ERASE_ERRORS) != 0U
                    ? LOADER_ERROR_ERASE
                    : LOADER_ERROR_TIMEOUT;
            }
            if (!leave_flash_pe() && success)
            {
                success = 0;
                mailbox->error = LOADER_ERROR_TIMEOUT;
            }
        }
    }
    else if (command == LOADER_COMMAND_PROGRAM)
    {
        if (!valid_program_range(mailbox->address, mailbox->length, mailbox->source))
        {
            mailbox->error = LOADER_ERROR_BAD_RANGE;
        }
        else if (!enter_code_flash_pe())
        {
            mailbox->error = LOADER_ERROR_TIMEOUT;
        }
        else
        {
            success = program_units(
                mailbox->address,
                mailbox->length,
                mailbox->source,
                mailbox);
            if (!success)
            {
                mailbox->error =
                    (mailbox->last_fstatr2 & FSTATR2_PROGRAM_ERRORS) != 0U
                    ? LOADER_ERROR_PROGRAM
                    : LOADER_ERROR_TIMEOUT;
            }
            if (!leave_flash_pe() && success)
            {
                success = 0;
                mailbox->error = LOADER_ERROR_TIMEOUT;
            }
        }
    }
    else if (command == LOADER_COMMAND_CLEANUP)
    {
        /*
         * Erase/program commands already return to read mode. Cleanup is
         * deliberately idempotent so the host can request it after an error.
         */
        *reg8(FACI_FCR) = FCR_CLEAR;
        *reg8(FACI_PFBER) = 1U;
        *reg16(FCACHE_FCACHEE) = 1U;
        success = 1;
    }
    else
    {
        mailbox->error = LOADER_ERROR_UNKNOWN_COMMAND;
    }

    mailbox->command = LOADER_COMMAND_NONE;
    mailbox->status = success ? LOADER_STATUS_DONE : LOADER_STATUS_ERROR;
}

__attribute__((used, noreturn, noinline, section(".text.loader")))
void loader_entry(loader_mailbox_t *mailbox)
{
    /*
     * The halted Arduino application may have enabled peripherals whose
     * handlers live in Code Flash. No interrupt may vector while Code Flash
     * is in P/E mode, so disable interrupts before touching the mailbox or
     * Flash controller.
     */
    __asm volatile ("cpsid i" ::: "memory");

    if (mailbox == (loader_mailbox_t *) 0 ||
        mailbox->magic != LOADER_MAILBOX_MAGIC ||
        mailbox->version != LOADER_MAILBOX_VERSION)
    {
        while (1)
        {
        }
    }

    if (!configure_flash_interface())
    {
        mailbox->status = LOADER_STATUS_ERROR;
        mailbox->error = LOADER_ERROR_TIMEOUT;
        while (1)
        {
        }
    }

    mailbox->status = LOADER_STATUS_READY;
    mailbox->error = 0U;
    mailbox->completed_units = 0U;

    while (1)
    {
        if (mailbox->command != LOADER_COMMAND_NONE &&
            mailbox->status != LOADER_STATUS_RUNNING)
        {
            process_command(mailbox);
        }
    }
}
