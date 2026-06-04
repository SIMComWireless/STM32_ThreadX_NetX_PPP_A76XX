/**
  ******************************************************************************
  * @file    fault_debug.c
  * @brief   HardFault diagnostic — dumps registers and fault status via LPUART1
  ******************************************************************************
  * @note
  *   Outputs register dump directly to LPUART1 data register (no HAL/DMA).
  *   Works even when the system is in a faulted state.
  *
  *   Usage: just include this file in the build. It overrides the default
  *   HardFault_Handler and prints diagnostic info before halting.
  ******************************************************************************
  */

#include "stm32l4xx.h"
#include <stdint.h>

/* ---------- LPUART1 direct register access ------------------------------- */

/* LPUART1 base address — matches STM32L4R5 memory map */
#define LPUART1_BASE_ADDR   0x40002400UL
#define LPUART1_ISR         (*(volatile uint32_t *)(LPUART1_BASE_ADDR + 0x1C))
#define LPUART1_TDR         (*(volatile uint32_t *)(LPUART1_BASE_ADDR + 0x28))
#define LPUART_ISR_TXE_TXFNF  (1UL << 7)  /* TX data register empty flag */

/* SCB registers for fault analysis */
#define SCB_CFSR    (*(volatile uint32_t *)0xE000ED28UL)  /* Configurable Fault Status */
#define SCB_HFSR    (*(volatile uint32_t *)0xE000ED2CUL)  /* Hard Fault Status */
#define SCB_MMFAR   (*(volatile uint32_t *)0xE000ED34UL)  /* MemManage Fault Address */
#define SCB_BFAR    (*(volatile uint32_t *)0xE000ED38UL)  /* Bus Fault Address */
#define SCB_DFSR    (*(volatile uint32_t *)0xE000ED30UL)  /* Debug Fault Status */

/* ---------- Private helpers ---------------------------------------------- */

/**
 * @brief  Send one character directly to LPUART1 (polled, no interrupt/DMA)
 */
static void fault_putc(char c)
{
    while (!(LPUART1_ISR & LPUART_ISR_TXE_TXFNF))
        ;
    LPUART1_TDR = (uint32_t)c;
}

/**
 * @brief  Send a string directly to LPUART1
 */
static void fault_puts(const char *s)
{
    while (*s)
    {
        fault_putc(*s++);
    }
}

/**
 * @brief  Print a 32-bit value as hex (8 digits)
 */
static void fault_print_hex(uint32_t val)
{
    const char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
    {
        fault_putc(hex[(val >> i) & 0x0F]);
    }
}

/**
 * @brief  Print a labeled register value
 */
static void fault_print_reg(const char *name, uint32_t val)
{
    fault_puts(name);
    fault_puts("= 0x");
    fault_print_hex(val);
    fault_puts("\r\n");
}

/**
 * @brief  Decode and print CFSR (Configurable Fault Status Register)
 */
static void fault_decode_cfsr(uint32_t cfsr)
{
    fault_puts("--- CFSR Decode ---\r\n");

    /* MemManage faults (bits 0-7) */
    if (cfsr & 0xFF)
    {
        fault_puts("  MemManage:");
        if (cfsr & (1 << 0)) fault_puts(" IACCVIOL");
        if (cfsr & (1 << 1)) fault_puts(" DACCVIOL");
        if (cfsr & (1 << 3)) fault_puts(" MUNSTKERR");
        if (cfsr & (1 << 4)) fault_puts(" MSTKERR");
        if (cfsr & (1 << 7)) fault_puts(" MMARVALID");
        fault_puts("\r\n");
    }

    /* Bus faults (bits 8-15) */
    if (cfsr & 0xFF00)
    {
        fault_puts("  BusFault:");
        if (cfsr & (1 << 8))  fault_puts(" IBUSERR");
        if (cfsr & (1 << 9))  fault_puts(" PRECISERR");
        if (cfsr & (1 << 10)) fault_puts(" IMPRECISERR");
        if (cfsr & (1 << 11)) fault_puts(" UNSTKERR");
        if (cfsr & (1 << 12)) fault_puts(" STKERR");
        if (cfsr & (1 << 15)) fault_puts(" BFARVALID");
        fault_puts("\r\n");
    }

    /* Usage faults (bits 16-31) */
    if (cfsr & 0xFFFF0000)
    {
        fault_puts("  UsageFault:");
        if (cfsr & (1 << 16)) fault_puts(" UNDEFINSTR");
        if (cfsr & (1 << 17)) fault_puts(" INVSTATE");
        if (cfsr & (1 << 18)) fault_puts(" INVPC");
        if (cfsr & (1 << 19)) fault_puts(" NOCP");
        if (cfsr & (1 << 24)) fault_puts(" UNALIGNED");
        if (cfsr & (1 << 25)) fault_puts(" DIVBYZERO");
        fault_puts("\r\n");
    }

    /* Fault addresses */
    if (cfsr & (1 << 7))   fault_print_reg("  MMFAR ", SCB_MMFAR);
    if (cfsr & (1 << 15))  fault_print_reg("  BFAR  ", SCB_BFAR);
}

/* ---------- HardFault Handler (naked assembly to grab stack pointer) ----- */

/**
 * @brief  HardFault_Handler — extracts stack frame and calls C diagnostic
 * @note   Uses __attribute__((naked)) to preserve the stack pointer.
 *         The stack frame pushed by hardware contains:
 *         R0, R1, R2, R3, R12, LR, PC, xPSR (in that order)
 */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile (
        "TST   LR, #4          \n"  /* Test EXC_RETURN bit 2 */
        "ITE   EQ              \n"
        "MRSEQ R0, MSP         \n"  /* Use MSP if bit 2 = 0 */
        "MRSNE R0, PSP         \n"  /* Use PSP if bit 2 = 1 */
        "B     hard_fault_diag \n"  /* Jump to C handler with R0 = stack frame ptr */
    );
}

/**
 * @brief  HardFault C diagnostic — called from assembly with stack frame pointer
 * @param  stack_frame  Pointer to the exception stack frame (R0-R3, R12, LR, PC, xPSR)
 */
void hard_fault_diag(uint32_t *stack_frame) __attribute__((used));
void hard_fault_diag(uint32_t *stack_frame)
{
    uint32_t r0  = stack_frame[0];
    uint32_t r1  = stack_frame[1];
    uint32_t r2  = stack_frame[2];
    uint32_t r3  = stack_frame[3];
    uint32_t r12 = stack_frame[4];
    uint32_t lr  = stack_frame[5];
    uint32_t pc  = stack_frame[6];
    uint32_t xpsr = stack_frame[7];

    uint32_t cfsr = SCB_CFSR;
    uint32_t hfsr = SCB_HFSR;

    /* Print banner */
    fault_puts("\r\n");
    fault_puts("========================================\r\n");
    fault_puts("  HARDFAULT DETECTED\r\n");
    fault_puts("========================================\r\n");

    /* Exception stack frame */
    fault_puts("--- Exception Stack Frame ---\r\n");
    fault_print_reg("R0   ", r0);
    fault_print_reg("R1   ", r1);
    fault_print_reg("R2   ", r2);
    fault_print_reg("R3   ", r3);
    fault_print_reg("R12  ", r12);
    fault_print_reg("LR   ", lr);
    fault_print_reg("PC   ", pc);        /* <-- Faulting instruction address */
    fault_print_reg("xPSR ", xpsr);

    /* Stack pointer at time of fault */
    fault_print_reg("SP   ", (uint32_t)stack_frame);

    /* SCB fault registers */
    fault_puts("\r\n--- Fault Status Registers ---\r\n");
    fault_print_reg("HFSR ", hfsr);
    fault_print_reg("CFSR ", cfsr);
    fault_print_reg("DFSR ", SCB_DFSR);

    /* Decode CFSR into human-readable flags */
    fault_puts("\r\n");
    fault_decode_cfsr(cfsr);

    /* HFSR analysis */
    fault_puts("\r\n--- HFSR Decode ---\r\n");
    if (hfsr & (1 << 1)) fault_puts("  VECTBL: Bus fault on vector table read\r\n");
    if (hfsr & (1 << 30)) fault_puts("  FORCED: Configurable fault escalated to HardFault\r\n");
    if (hfsr & (1 << 31)) fault_puts("  DEBUGEVT: Debug event\r\n");

    fault_puts("\r\n");
    fault_puts("Halted. Connect debugger and check PC=0x");
    fault_print_hex(pc);
    fault_puts("\r\n");

    /* Halt */
    while (1)
    {
        __asm volatile ("wfi");
    }
}
