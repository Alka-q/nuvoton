/******************************************************************************
 * @file     main.c
 * @version  V1.00
 * @brief    UART TX DMA with SysTick periodic trigger (100ms)
 *           on Nuvoton M031.
 *
 *           - UART0 (PB.12/PB.13): printf debug
 *           - UART1 (PB.3):       Data TX via DMA
 *           - PDMA Ch1:            UART1 TX
 *           - SysTick:             100ms periodic trigger
 *
 * Copyright (C) 2017 Nuvoton Technology Corp. All rights reserved.
 *****************************************************************************/
#include <stdio.h>
#include <string.h>
#include "NuMicro.h"

/*  Definitions */
#define UART_TX_DMA_CH  1           /* PDMA channel for UART1 TX */
#define MAX_TX_SIZE     64          /* Maximum transmission buffer size */

/*  Global Variables */
static uint8_t           g_u8TxBuf[MAX_TX_SIZE];
static volatile uint8_t  g_u8TxReady  = 0;    /* SysTick sets every 500ms */
static volatile uint8_t  g_u8TxDone   = 1;    /* PDMA TX complete */
static volatile uint32_t g_u32TxCount = 0;    /* incrementing counter */

/*  Forward Declarations */
static void SYS_Init(void);
static void UART0_Init(void);
static void UART1_Init(void);
static void PDMA_TX_Init(void);
static void PDMA_TX_Start(uint8_t *buf, uint32_t len);

int32_t main(void)
{
    uint32_t len;

    SYS_Init();
    UART0_Init();
    UART1_Init();
    PDMA_TX_Init();

    /* 
       HCLK / 1000 = 48000000 / 10 = 4800000.
       100 milisaniyede bir SysTick_Handler kesmesi üretir.
       max value = (SystemCoreClock / 10) < 24-bit 
    */
    if (SysTick_Config(SystemCoreClock / 10))
        while (1); 

    printf("\n\nCORE @ %d Hz\n", SystemCoreClock);
    printf("UART TX DMA with SysTick 500ms Period\n");
    printf("UART1 TX(PB.3) DMA Ch%d, Max %d bytes\n", UART_TX_DMA_CH, MAX_TX_SIZE);
    printf("Sending data every 500ms...\n\n");

    while (1)
    {
        if (g_u8TxReady && g_u8TxDone)
        {
            g_u8TxReady = 0;
            g_u8TxDone  = 0;

            /* Prepare variable data: "Msg #<counter>\r\n" */
            len = snprintf((char *)g_u8TxBuf, MAX_TX_SIZE, "Msg #%u\r\n", g_u32TxCount++);

            /* Start TX DMA transfer */
            PDMA_TX_Start(g_u8TxBuf, len);
        }
    }
}

/*  System / Clock / Pin Init */
static void SYS_Init(void)
{
    SYS_UnlockReg();
    
    /* HIRC 48 MHz */
    CLK_EnableXtalRC(CLK_PWRCTL_HIRCEN_Msk);
    CLK_WaitClockReady(CLK_STATUS_HIRCSTB_Msk);
    CLK_SetHCLK(CLK_CLKSEL0_HCLKSEL_HIRC, CLK_CLKDIV0_HCLK(1));

    CLK_EnableModuleClock(UART1_MODULE);
    CLK_EnableModuleClock(PDMA_MODULE);
    CLK_SetModuleClock(UART1_MODULE, CLK_CLKSEL1_UART1SEL_HIRC, CLK_CLKDIV0_UART1(1));
    
    SystemCoreClockUpdate();

    /* PCLK = HCLK */
    CLK->PCLKDIV = CLK_PCLKDIV_APB0DIV_DIV1 | CLK_PCLKDIV_APB1DIV_DIV1;

    /* UART1 pins: PB.3 (TXD only) */
    SYS->GPB_MFPL = (SYS->GPB_MFPL & ~SYS_GPB_MFPL_PB3MFP_Msk)
                    | SYS_GPB_MFPL_PB3MFP_UART1_TXD;

    SYS_LockReg();
}

/*  UART0 Init */
static void UART0_Init(void)
{
    SYS_ResetModule(UART0_RST);
    UART_Open(UART0, 115200);
}

/*  UART1 Init (TX) */
static void UART1_Init(void)
{
    SYS_ResetModule(UART1_RST);
    UART_Open(UART1, 115200);
}

/*  PDMA TX Init - one-time channel setup */
static void PDMA_TX_Init(void)
{
    SYS_ResetModule(PDMA_RST);

    /* Enable PDMA channel 1 */
    PDMA_Open(PDMA, (1UL << UART_TX_DMA_CH));

    /* Enable PDMA Transfer Done interrupt */
    PDMA_EnableInt(PDMA, UART_TX_DMA_CH, PDMA_INT_TRANS_DONE);

    NVIC_EnableIRQ(PDMA_IRQn);
}

/*  PDMA TX Start - configure channel and fire transfer */
static void PDMA_TX_Start(uint8_t *buf, uint32_t len)
{
    /* Set transfer count */
    PDMA_SetTransferCnt(PDMA, UART_TX_DMA_CH, PDMA_WIDTH_8, len);

    /* Source: RAM buffer (increment), Destination: UART1 DAT (fixed) */
    PDMA_SetTransferAddr(PDMA, UART_TX_DMA_CH,
                         (uint32_t)buf,          PDMA_SAR_INC,
                         (uint32_t)&UART1->DAT,  PDMA_DAR_FIX);

    /* Mode: UART1 TX, basic */
    PDMA_SetTransferMode(PDMA, UART_TX_DMA_CH, PDMA_UART1_TX, FALSE, 0);

    /* Single request */
    PDMA_SetBurstType(PDMA, UART_TX_DMA_CH, PDMA_REQ_SINGLE, 0);

    /* Re-enable PDMA channel (STOP/PAUSE may have cleared CHEN) */
    PDMA->CHCTL |= (1UL << UART_TX_DMA_CH);

    /* Enable UART TX PDMA - this actually starts the transfer */
    UART_ENABLE_INT(UART1, UART_INTEN_TXPDMAEN_Msk);
}

/*  PDMA IRQ Handler */
void PDMA_IRQHandler(void)
{
    uint32_t status = PDMA_GET_INT_STATUS(PDMA);

    if (status & PDMA_INTSTS_ABTIF_Msk)
    {
        PDMA_CLR_ABORT_FLAG(PDMA, PDMA_GET_ABORT_STS(PDMA));
    }

    if (status & PDMA_INTSTS_TDIF_Msk)
    {
        if (PDMA_GET_TD_STS(PDMA) & (1UL << UART_TX_DMA_CH))
        {
            PDMA_CLR_TD_FLAG(PDMA, (1UL << UART_TX_DMA_CH));

            /* Disable UART TX PDMA */
            UART_DISABLE_INT(UART1, UART_INTEN_TXPDMAEN_Msk);

            g_u8TxDone = 1;
        }
    }
}

/*  SysTick Handler - triggered every 100ms */
void SysTick_Handler(void) 
{
    static uint32_t clkCount;
    clkCount++;
    g_u8TxReady = (clkCount % 5 == 0) ? 1 : 0; // 5 * 100 = 500ms
}


/*** (C) COPYRIGHT 2017 Nuvoton Technology Corp. ***/
