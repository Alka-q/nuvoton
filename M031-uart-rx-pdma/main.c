/******************************************************************************
 * @file     main.c
 * @version  V1.00
 * @brief    UART RX DMA with PDMA Timeout for variable-length reception
 *           on Nuvoton M031.
 *
 *           - UART0 (PB.12/PB.13): printf debug
 *           - UART1 (PB.2/PB.3):  Data RX via DMA
 *           - PDMA Ch0:            UART1 RX + hardware timeout
 *
 *****************************************************************************/
#include <stdio.h>
#include <string.h>
#include "NuMicro.h"

/*------------------- Definitions -------------------*/
//#define DEBUG_PORT              UART1
#define UART_RX_DMA_CH          0
#define MAX_RX_SIZE             64

/*
 * PDMA Timeout calculation (for idle gap detection):
 *   HCLK       = 48 MHz
 *   TOUTPSC=0  = PDMA channel 0 time-out clock source is HCLK/2^8. @pdma_reg.h 
 *   TOUTPSC=0  -> HCLK / 256 = 187,500Hz  -> 5.33 us per tick
 *   115200 baud -> 1 byte ~ 87 us (10 bits)
 *   5 byte idle = ~435 us -> 435 / 5.33 ~ 82 ticks
 */
#define PDMA_TIMEOUT_TICK       82

/*-------------------------------Global Variables -------------------------------*/
static uint8_t           g_u8RxBuf[MAX_RX_SIZE];
static volatile uint32_t g_u32RxLen   = 0;
static volatile uint8_t  g_u8RxReady  = 0;

/*---------------------------- Forward Declarations -----------------------------*/
static void SYS_Init(void);
static void UART0_Init(void);
static void UART1_Init(void);
static void PDMA_Init(void);
static void PDMA_ReRX(void);


int main(void)
{
    uint32_t i;

    SYS_Init();
    UART0_Init();
    UART1_Init();
    PDMA_Init();

    printf("\n\nCORE @ %d Hz\n", SystemCoreClock);
    printf("UART RX DMA with PDMA Timeout - Variable Length\n");
    printf("UART1 RX(PB.2) DMA Ch%d, Max %d bytes, Timeout %d ticks\n", UART_RX_DMA_CH, MAX_RX_SIZE, PDMA_TIMEOUT_TICK);
    printf("Waiting for data...\n\n");

    while (1)
    {
        if (g_u8RxReady)
        {
            g_u8RxReady = 0;

            printf("RX [%d bytes]: ", g_u32RxLen);
            for (i = 0; i < g_u32RxLen; i++)
            {
                if (g_u8RxBuf[i] >= 0x20 && g_u8RxBuf[i] < 0x7F)
                    printf("%c", g_u8RxBuf[i]);
                else
                    printf("%X", g_u8RxBuf[i]);
            }
            printf("\n");
        }
    }
}


/*-------------------------- System / Clock / Pin Init --------------------------*/
static void SYS_Init(void)
{
    SYS_UnlockReg();

    /* HIRC 48 MHz */
    CLK_EnableXtalRC(CLK_PWRCTL_HIRCEN_Msk);
    CLK_WaitClockReady(CLK_STATUS_HIRCSTB_Msk);
    CLK_SetHCLK(CLK_CLKSEL0_HCLKSEL_HIRC, CLK_CLKDIV0_HCLK(1));

    /* Peripheral clocks: UART0, UART1, PDMA */
//    CLK_EnableModuleClock(UART0_MODULE);
    CLK_EnableModuleClock(UART1_MODULE);
    CLK_EnableModuleClock(PDMA_MODULE);

//    CLK_SetModuleClock(UART0_MODULE, CLK_CLKSEL1_UART0SEL_HIRC, CLK_CLKDIV0_UART0(1));
    CLK_SetModuleClock(UART1_MODULE, CLK_CLKSEL1_UART1SEL_HIRC, CLK_CLKDIV0_UART1(1));

    SystemCoreClockUpdate();

    /* PCLK = HCLK */
    CLK->PCLKDIV = CLK_PCLKDIV_APB0DIV_DIV1 | CLK_PCLKDIV_APB1DIV_DIV1;

    /* UART0 pins: PB.12 (RXD), PB.13 (TXD) */
//    SYS->GPB_MFPH = (SYS->GPB_MFPH & ~(SYS_GPB_MFPH_PB12MFP_Msk | SYS_GPB_MFPH_PB13MFP_Msk))
//                    | (SYS_GPB_MFPH_PB12MFP_UART0_RXD | SYS_GPB_MFPH_PB13MFP_UART0_TXD);

    /* UART1 pins: PB.2 (RXD), PB.3 (TXD) */
    SYS->GPB_MFPL = (SYS->GPB_MFPL & ~(SYS_GPB_MFPL_PB2MFP_Msk | SYS_GPB_MFPL_PB3MFP_Msk))
                    | (SYS_GPB_MFPL_PB2MFP_UART1_RXD | SYS_GPB_MFPL_PB3MFP_UART1_TXD);

    SYS_LockReg();
}

/*--------------------------------- UART0 Init ----------------------------------*/
static void UART0_Init(void)
{
    SYS_ResetModule(UART0_RST);
    UART_Open(UART0, 115200);
}
/*--------------------------------- UART1 Init ----------------------------------*/
static void UART1_Init(void)
{
    SYS_ResetModule(UART1_RST);
    UART_Open(UART1, 115200);
}
/*--------------------------------- PDMA Init -----------------------------------*/
static void PDMA_Init(void)
{
    SYS_ResetModule(PDMA_RST);

    PDMA_Open(PDMA, (1UL << UART_RX_DMA_CH));

    /* Transfer count: up to MAX_RX_SIZE bytes */
    PDMA_SetTransferCnt(PDMA, UART_RX_DMA_CH, PDMA_WIDTH_8, MAX_RX_SIZE);

    /* Source: UART1 DAT register (fixed), Destination: buffer (incrementing) */
    PDMA_SetTransferAddr(PDMA, UART_RX_DMA_CH, (uint32_t)&UART1->DAT, PDMA_SAR_FIX, (uint32_t)g_u8RxBuf, PDMA_DAR_INC);

    /* Mode: UART1 RX, no scatter-gather */
    PDMA_SetTransferMode(PDMA, UART_RX_DMA_CH, PDMA_UART1_RX, FALSE, 0);

    /* Single request (required for peripheral transfers) */
    PDMA_SetBurstType(PDMA, UART_RX_DMA_CH, PDMA_REQ_SINGLE, 0);

    /* --- PDMA Timeout for idle gap detection --- */
    PDMA_SetTimeOut(PDMA, UART_RX_DMA_CH, 1, PDMA_TIMEOUT_TICK);
    PDMA_EnableTimeout(PDMA, (1UL << UART_RX_DMA_CH));

    /* Timeout prescaler: TOUTPSC0 = 0 -> HCLK/256 */
    PDMA->TOUTPSC = (PDMA->TOUTPSC & ~PDMA_TOUTPSC_TOUTPSC0_Msk) | (0UL << PDMA_TOUTPSC_TOUTPSC0_Pos);

    /* --- Interrupts --- */
    PDMA_EnableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TIMEOUT);
    PDMA_EnableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TRANS_DONE);   /* buffer full */
    NVIC_EnableIRQ(PDMA_IRQn);

    /* Enable UART1 RX PDMA function */
    UART_ENABLE_INT(UART1, UART_INTEN_RXPDMAEN_Msk);
}

/*---------------------- PDMA Re-RX - reset channel -----------------------------*/
static void PDMA_ReRX(void)
{
    PDMA_DisableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TIMEOUT);
    PDMA_DisableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TRANS_DONE);

    UART_DISABLE_INT(UART1, UART_INTEN_RXPDMAEN_Msk); 
    
    PDMA_STOP(PDMA, UART_RX_DMA_CH);
    PDMA_SetTransferCnt(PDMA, UART_RX_DMA_CH, PDMA_WIDTH_8, MAX_RX_SIZE);
    PDMA_SetTransferAddr(PDMA, UART_RX_DMA_CH, (uint32_t)&UART1->DAT, PDMA_SAR_FIX, (uint32_t)g_u8RxBuf, PDMA_DAR_INC);
    PDMA_SetTransferMode(PDMA, UART_RX_DMA_CH, PDMA_UART1_RX, FALSE, 0);
    PDMA_SetBurstType(PDMA, UART_RX_DMA_CH, PDMA_REQ_SINGLE, 0);

    PDMA_SetTimeOut(PDMA, UART_RX_DMA_CH, 1, PDMA_TIMEOUT_TICK);
    PDMA_EnableTimeout(PDMA, (1UL << UART_RX_DMA_CH));

    PDMA_EnableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TIMEOUT);
    PDMA_EnableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TRANS_DONE);
    // Re-enable PDMA channel (PAUSE/STOP cleared the CHEN bit in CHCTL) 
    PDMA->CHCTL |= (1UL << UART_RX_DMA_CH);

    UART_ENABLE_INT(UART1, UART_INTEN_RXPDMAEN_Msk);
}

/*--------------------------------- PDMA Init -----------------------------------*/
void PDMA_IRQHandler(void)
{
    uint32_t status = PDMA_GET_INT_STATUS(PDMA);

    if (status & PDMA_INTSTS_ABTIF_Msk)
    {
        /* Target abort */
        PDMA_CLR_ABORT_FLAG(PDMA, PDMA_GET_ABORT_STS(PDMA));
    }

    /* Transfer Done: buffer full (message = MAX_RX_SIZE ) */
    if (status & PDMA_INTSTS_TDIF_Msk)
    {
        if (PDMA_GET_TD_STS(PDMA) & (1UL << UART_RX_DMA_CH))
        {
            PDMA_CLR_TD_FLAG(PDMA, (1UL << UART_RX_DMA_CH));

            g_u32RxLen = MAX_RX_SIZE;
            g_u8RxReady = 1;

            //UART_DISABLE_INT(UART1, UART_INTEN_RXPDMAEN_Msk);
            PDMA_ReRX();
        }
    }

    /* Request Timeout: idle gap detected (message < MAX_RX_SIZE) */
    if (status & PDMA_INTSTS_REQTOF0_Msk)
    {
        PDMA_CLR_TMOUT_FLAG(PDMA, UART_RX_DMA_CH);

        // Calculate actual received length
        uint32_t remaining = (PDMA->DSCT[UART_RX_DMA_CH].CTL & PDMA_DSCT_CTL_TXCNT_Msk) >> PDMA_DSCT_CTL_TXCNT_Pos;
        g_u32RxLen = MAX_RX_SIZE - (remaining + 1);
        g_u8RxReady = 1;

        //UART_DISABLE_INT(UART1, UART_INTEN_RXPDMAEN_Msk);
        PDMA_ReRX();
    }
}


/*** (C) COPYRIGHT 2017 Nuvoton Technology Corp. ***/