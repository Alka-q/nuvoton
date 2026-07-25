/******************************************************************************
 * @file     main.c
 * @version  V1.00
 * @brief    UART RX+TX DMA with PDMA Timeout for variable-length reception
 *           and echo transmission on Nuvoton M031.
 *
 *           - UART0 (PB.12/PB.13): printf debug
 *           - UART1 (PB.2/PB.3):  RX via DMA Ch0 + TX via DMA Ch1
 *           - PDMA Ch0:            UART1 RX + hardware timeout
 *           - PDMA Ch1:            UART1 TX (echo)
 *
 *****************************************************************************/
#include <stdio.h>
#include <string.h>
#include "NuMicro.h"

/*  Definitions  */
#define UART_RX_DMA_CH  0           /* PDMA channel for UART1 RX  */
#define UART_TX_DMA_CH  1           /* PDMA channel for UART1 TX */
#define MAX_RX_SIZE     64          /* Maximum reception buffer size */
#define MAX_TX_SIZE     128         /* Maximum TX buffer (echo prefix + data + suffix) */

/*
 * PDMA Timeout calculation (for idle gap detection):
 *   HCLK       = 48 MHz  (HIRC, divider = 1)
 *   TOUTPSC=0  -> HCLK / 256 = 187,500 Hz  -> 5.33 us per tick
 *   115200 baud -> 1 byte ~ 87 us (10 bits)
 *   5 byte idle = ~435 us -> 435 / 5.33 ~ 82 ticks
 */
#define PDMA_TIMEOUT_TICK   82

/*  Global Variables  */
static uint8_t           g_u8RxBuf[MAX_RX_SIZE];
static volatile uint32_t g_u32RxLen    = 0;
static volatile uint8_t  g_u8RxReady   = 0;

static uint8_t           g_u8TxBuf[MAX_TX_SIZE];
static volatile uint8_t  g_u8TxDone    = 1;
static volatile uint8_t  g_u8TxPending = 0;

/*  Forward Declarations  */
static void SYS_Init(void);
static void UART0_Init(void);
static void UART1_Init(void);
static void PDMA_Init(void);
static void PDMA_RX_Restart(void);
static void PDMA_TX_Start(uint8_t *buf, uint32_t len);

/*  Main  */
int32_t main(void)
{
    uint32_t i;
    uint32_t echoLen;

    SYS_Init();
    UART0_Init();
    UART1_Init();
    PDMA_Init();
    
    printf("\n\nCORE @ %d Hz\n", SystemCoreClock);
    printf("UART RX+TX DMA with PDMA Timeout\n");
    printf("UART1 RX(PB.2) DMA Ch%d, TX(PB.3) DMA Ch%d\n", UART_RX_DMA_CH, UART_TX_DMA_CH);
    printf("Max RX: %d bytes, Timeout: %d ticks (~430us)\n", MAX_RX_SIZE, PDMA_TIMEOUT_TICK);
    printf("Waiting for data...\n\n");

    while (1)
    {
        /* RX received a message - prepare echo */
        if (g_u8RxReady)
        {
            g_u8RxReady = 0;

            /* Prepare echo: "Echo: <data>\r\n" */
            echoLen = snprintf((char *)g_u8TxBuf, MAX_TX_SIZE, "RX [%d bytes] | Echo: ", g_u32RxLen);
            for (i = 0; i < g_u32RxLen && echoLen < (MAX_TX_SIZE - 3); i++)
                g_u8TxBuf[echoLen++] = g_u8RxBuf[i];
            g_u8TxBuf[echoLen++] = '\r';
            g_u8TxBuf[echoLen++] = '\n';

            g_u8TxPending = 1;
        }

        /* Send echo via TX DMA */
        if (g_u8TxPending && g_u8TxDone)
        {
            g_u8TxPending = 0;
            g_u8TxDone = 0;

            PDMA_TX_Start(g_u8TxBuf, echoLen);
        }
    }
}

/*  System / Clock / Pin Init  */
static void SYS_Init(void)
{
    SYS_UnlockReg();

    /* HIRC 48 MHz */
    CLK_EnableXtalRC(CLK_PWRCTL_HIRCEN_Msk);
    CLK_WaitClockReady(CLK_STATUS_HIRCSTB_Msk);
    CLK_SetHCLK(CLK_CLKSEL0_HCLKSEL_HIRC, CLK_CLKDIV0_HCLK(1));

    /* Peripheral clocks: UART1, PDMA */
    CLK_EnableModuleClock(UART1_MODULE);
    CLK_EnableModuleClock(PDMA_MODULE);

    CLK_SetModuleClock(UART1_MODULE, CLK_CLKSEL1_UART1SEL_HIRC, CLK_CLKDIV0_UART1(1));

    SystemCoreClockUpdate();

    /* PCLK = HCLK */
    CLK->PCLKDIV = CLK_PCLKDIV_APB0DIV_DIV1 | CLK_PCLKDIV_APB1DIV_DIV1;

    /* UART1 pins: PB.2 (RXD), PB.3 (TXD) */
    SYS->GPB_MFPL = (SYS->GPB_MFPL & ~(SYS_GPB_MFPL_PB2MFP_Msk | SYS_GPB_MFPL_PB3MFP_Msk))
                    | (SYS_GPB_MFPL_PB2MFP_UART1_RXD | SYS_GPB_MFPL_PB3MFP_UART1_TXD);

    SYS_LockReg();
}

/*  UART0 Init (printf)  */
static void UART0_Init(void)
{
    SYS_ResetModule(UART0_RST);
    UART_Open(UART0, 115200);
}

/*  UART1 Init (RX + TX)  */
static void UART1_Init(void)
{
    SYS_ResetModule(UART1_RST);
    UART_Open(UART1, 115200);
}

/*  PDMA Init - both RX (Ch0) and TX (Ch1) channels  */
static void PDMA_Init(void)
{
    SYS_ResetModule(PDMA_RST);

    /* Enable both RX and TX channels */
    PDMA_Open(PDMA, (1UL << UART_RX_DMA_CH) | (1UL << UART_TX_DMA_CH));

    /* --- RX Channel (Ch0): UART1 RX with timeout --- */
    PDMA_SetTransferCnt(PDMA, UART_RX_DMA_CH, PDMA_WIDTH_8, MAX_RX_SIZE);
    PDMA_SetTransferAddr(PDMA, UART_RX_DMA_CH,
                         (uint32_t)&UART1->DAT, PDMA_SAR_FIX,
                         (uint32_t)g_u8RxBuf,   PDMA_DAR_INC);
    PDMA_SetTransferMode(PDMA, UART_RX_DMA_CH, PDMA_UART1_RX, FALSE, 0);
    PDMA_SetBurstType(PDMA, UART_RX_DMA_CH, PDMA_REQ_SINGLE, 0);

    /* Timeout for idle gap detection */
    PDMA_SetTimeOut(PDMA, UART_RX_DMA_CH, 1, PDMA_TIMEOUT_TICK);
    PDMA_EnableTimeout(PDMA, (1UL << UART_RX_DMA_CH));
    PDMA->TOUTPSC = (PDMA->TOUTPSC & ~PDMA_TOUTPSC_TOUTPSC0_Msk) | (0UL << PDMA_TOUTPSC_TOUTPSC0_Pos);

    PDMA_EnableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TIMEOUT);
    PDMA_EnableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TRANS_DONE);

    /* --- TX Channel (Ch1): UART1 TX --- */
    PDMA_EnableInt(PDMA, UART_TX_DMA_CH, PDMA_INT_TRANS_DONE);

    NVIC_EnableIRQ(PDMA_IRQn);

    /* Enable UART1 RX PDMA function */
    UART_ENABLE_INT(UART1, UART_INTEN_RXPDMAEN_Msk);
}

/*  PDMA RX Restart - reset channel for next reception  */
static void PDMA_RX_Restart(void)
{
    PDMA_DisableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TIMEOUT);
    PDMA_DisableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TRANS_DONE);

    UART_DISABLE_INT(UART1, UART_INTEN_RXPDMAEN_Msk);

    PDMA_STOP(PDMA, UART_RX_DMA_CH);

    PDMA_SetTransferCnt(PDMA, UART_RX_DMA_CH, PDMA_WIDTH_8, MAX_RX_SIZE);
    PDMA_SetTransferAddr(PDMA, UART_RX_DMA_CH,
                         (uint32_t)&UART1->DAT, PDMA_SAR_FIX,
                         (uint32_t)g_u8RxBuf,   PDMA_DAR_INC);
    PDMA_SetTransferMode(PDMA, UART_RX_DMA_CH, PDMA_UART1_RX, FALSE, 0);
    PDMA_SetBurstType(PDMA, UART_RX_DMA_CH, PDMA_REQ_SINGLE, 0);

    PDMA_SetTimeOut(PDMA, UART_RX_DMA_CH, 1, PDMA_TIMEOUT_TICK);
    PDMA_EnableTimeout(PDMA, (1UL << UART_RX_DMA_CH));

    PDMA_EnableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TIMEOUT);
    PDMA_EnableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TRANS_DONE);

    /* Re-enable PDMA channel (PAUSE/STOP cleared the CHEN bit in CHCTL) */
    PDMA->CHCTL |= (1UL << UART_RX_DMA_CH);

    UART_ENABLE_INT(UART1, UART_INTEN_RXPDMAEN_Msk);
}

/*  PDMA TX Start - configure channel and fire transfer  */
static void PDMA_TX_Start(uint8_t *buf, uint32_t len)
{
    PDMA_SetTransferCnt(PDMA, UART_TX_DMA_CH, PDMA_WIDTH_8, len);
    PDMA_SetTransferAddr(PDMA, UART_TX_DMA_CH,
                         (uint32_t)buf,          PDMA_SAR_INC,
                         (uint32_t)&UART1->DAT,  PDMA_DAR_FIX);
    PDMA_SetTransferMode(PDMA, UART_TX_DMA_CH, PDMA_UART1_TX, FALSE, 0);
    PDMA_SetBurstType(PDMA, UART_TX_DMA_CH, PDMA_REQ_SINGLE, 0);

    /* Re-enable PDMA channel */
    PDMA->CHCTL |= (1UL << UART_TX_DMA_CH);

    /* Enable UART TX PDMA - starts the transfer */
    UART_ENABLE_INT(UART1, UART_INTEN_TXPDMAEN_Msk);
}

/*  PDMA IRQ Handler  */
void PDMA_IRQHandler(void)
{
    uint32_t status = PDMA_GET_INT_STATUS(PDMA);

    if (status & PDMA_INTSTS_ABTIF_Msk)
    {
        PDMA_CLR_ABORT_FLAG(PDMA, PDMA_GET_ABORT_STS(PDMA));
    }

    /* --- RX Ch0: Transfer Done (buffer full) --- */
    if (status & PDMA_INTSTS_TDIF_Msk)
    {
        if (PDMA_GET_TD_STS(PDMA) & (1UL << UART_RX_DMA_CH))
        {
            PDMA_CLR_TD_FLAG(PDMA, (1UL << UART_RX_DMA_CH));

            g_u32RxLen = MAX_RX_SIZE;
            g_u8RxReady = 1;

            UART_DISABLE_INT(UART1, UART_INTEN_RXPDMAEN_Msk);
            PDMA_RX_Restart();
        }

        /* --- TX Ch1: Transfer Done (echo sent) --- */
        if (PDMA_GET_TD_STS(PDMA) & (1UL << UART_TX_DMA_CH))
        {
            PDMA_CLR_TD_FLAG(PDMA, (1UL << UART_TX_DMA_CH));

            UART_DISABLE_INT(UART1, UART_INTEN_TXPDMAEN_Msk);
            g_u8TxDone = 1;
        }
    }

    /* --- RX Ch0: Request Timeout --- */
    if (status & PDMA_INTSTS_REQTOF0_Msk)
    {
        PDMA_CLR_TMOUT_FLAG(PDMA, UART_RX_DMA_CH);

        uint32_t remaining = (PDMA->DSCT[UART_RX_DMA_CH].CTL & PDMA_DSCT_CTL_TXCNT_Msk) >> PDMA_DSCT_CTL_TXCNT_Pos;
        g_u32RxLen = MAX_RX_SIZE - (remaining + 1);
        g_u8RxReady = 1;

        UART_DISABLE_INT(UART1, UART_INTEN_RXPDMAEN_Msk);
        PDMA_RX_Restart();
    }
}


/*** (C) COPYRIGHT 2017 Nuvoton Technology Corp. ***/
