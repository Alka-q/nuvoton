/******************************************************************************
 * @file     main.c
 * @brief    FreeRTOS Quectel M66 GSM Module UART1 DMA Communication Example
 *           
 *           System Architecture:
 *           - GSM Module (Quectel M66) <==> UART1 (PB.2 RX, PB.3 TX)
 *           - Debug Console           <==> UART0 (PB.12 RX, PB.13 TX)
 *           - PDMA Channel 0: UART1 RX with Timeout (idle detection)
 *           - PDMA Channel 1: UART1 TX
 * 
 *           FreeRTOS Flow:
 *           - UART1 RX DMA ISR -> Stream Buffer RX -> Network Manager Task (NMT)
 *           - Application Core Task (APP) -> Queue -> Network Manager Task -> PDMA TX -> GSM
 *
 *****************************************************************************/
#include <stdio.h>
#include <string.h>

/* Kernel includes */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "stream_buffer.h"

/* Hardware includes */
#include "NuMicro.h"

/* 
 * Global Debug Switch:
 * 1: Debug prints enabled
 * 0: Debug prints disabled (completely compiled out)
 */
#ifndef ENABLE_DEBUG_LOG
#define ENABLE_DEBUG_LOG       1
#endif

#if ENABLE_DEBUG_LOG
    #define DEBUG_PRINT(...)    printf(__VA_ARGS__)
#else
    #define DEBUG_PRINT(...)    ((void)0)
#endif

#define UART_RX_DMA_CH          (0)
#define UART_TX_DMA_CH          (1)
#define GSM_RX_DMA_BUF_SIZE     (256)
#define PDMA_TIMEOUT_TICK       (82 * 3)

#define M66_RST(state)          PB4 = (state)
#define SET_M66_RST             GPIO_SetMode(PB, BIT4, GPIO_MODE_OUTPUT)

typedef enum{
  OFF,
  ON
}onoff_t;

typedef union{
    uint8_t gFlag;
    struct{
        uint8_t flag_1 : 1;
        uint8_t flag_2 : 1;
        uint8_t flag_3 : 1;
        uint8_t flag_4 : 1;
        uint8_t flag_5 : 1;
        uint8_t flag_6 : 1;
        uint8_t flag_7 : 1;
        uint8_t flag_8 : 1;
        uint8_t flag[8];
    }sFlag;
}guFlag;
guFlag uFlag;

QueueHandle_t           gsmQueueHandle  = NULL;
StreamBufferHandle_t    xGsmRxStreamBuf = NULL;
/*  */
uint8_t g_u8RxDmaBuf[GSM_RX_DMA_BUF_SIZE];

/* Forward Declarations */
static void prvSetupHardware(void);
static void UART0_Init(void);
static void UART1_Init(void);
static void PDMA_Init(void); 
static void PDMA_RX_Restart(void);
static void PDMA_TX_Start(const uint8_t *buf, uint32_t len);

static void vApplicationCoreTask(void *pvParameters);
static void vNetworkManagerTask(void *pvParameters);
QueueHandle_t getQueueB(void);

void PDMA_IRQHandler(void);

int main(void)
{
    prvSetupHardware();
    
    SET_M66_RST;
    M66_RST(ON);
    
    DEBUG_PRINT("\n=============================================\n");
    DEBUG_PRINT("   Nuvoton M03LE3AE FreeRTOS M66 Demo \n");
    DEBUG_PRINT("   System Clock: %d Hz\n", SystemCoreClock);
    DEBUG_PRINT("=============== System Design ===============\n");
    DEBUG_PRINT("   UART1rx -> ISR -> (SB) -> [NMT] -> [APP]\n");
    DEBUG_PRINT("   UART1tx <- (SB) <- [NMT] <- Qu <- [APP]\n");
    DEBUG_PRINT("=============================================\n\n");
    
    gsmQueueHandle = xQueueCreate(2, 64);
    xGsmRxStreamBuf = xStreamBufferCreate(GSM_RX_DMA_BUF_SIZE, 1);
    
    /* Create Application Core Task (APP) - Priority 1 */
    xTaskCreate(vApplicationCoreTask, "AppTask", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 1, NULL); 
    /* Create Network Manager Task (NMT) - Priority 2 */
    xTaskCreate(vNetworkManagerTask, "AppTask", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 2, NULL);

    DEBUG_PRINT("Starting FreeRTOS Scheduler...\n");
    vTaskStartScheduler();
    
    for (;;);
}

/* Application Core Task: */
static void vApplicationCoreTask(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    DEBUG_PRINT("[APP] Application Core Task active.\n");
    
    uint8_t rxIndex = 0;
    uint8_t rxCmdBuf[32] = {0};
    uint8_t ch;
    for (;;)
    {
        while( ch = getchar() )
        {
            if( /*ch == '\r' ||*/ ch == '\n')
            {
                rxCmdBuf[rxIndex] = '\0';
                DEBUG_PRINT("[APP] Alinan komut: %s\n", rxCmdBuf);
                
                char gsmCmd[64];
                snprintf(gsmCmd, sizeof(gsmCmd), "%s\r\n", rxCmdBuf);
                xQueueSend(gsmQueueHandle, &gsmCmd, pdMS_TO_TICKS(100));
                rxIndex = 0;
            }
            else
            {
                rxCmdBuf[rxIndex++] = ch;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* Network Manager Task:  */
static void vNetworkManagerTask(void *pvParameters)
{
    DEBUG_PRINT("[NMT] Network Manager Task active.\n");
    
    char rxStreamBuff[128];
    char txQueueBuff[64];
    
    for(;;)
    {
        /* 1. Uygulama Görevinden (Kullanicidan) Komut Gelmesi (Non-blocking: 10ms) */
        if(xQueueReceive(gsmQueueHandle, txQueueBuff, pdMS_TO_TICKS(10)) == pdPASS)
        {
            DEBUG_PRINT("[NMT] GSM'e gonderiyor: %s", txQueueBuff );
            PDMA_TX_Start((const uint8_t*)txQueueBuff, strlen(txQueueBuff));
        }
        /* 2. Cihazdan (UART1 RX) Yanitindan Stream Buffer'a Veri Gelmesi */
        uint8_t xByteReceived = xStreamBufferReceive(xGsmRxStreamBuf, &rxStreamBuff, 
                                                     sizeof(rxStreamBuff) - 1, pdMS_TO_TICKS(10));
        if( xByteReceived > 0 )
        {
            rxStreamBuff[xByteReceived] = '\0';
            DEBUG_PRINT("%s", rxStreamBuff);
        }
    }
}

QueueHandle_t getQueueB(void)
{
    return gsmQueueHandle;
}

/* Hardware setup */
static void prvSetupHardware(void)
{
    SYS_UnlockReg();

    /* Enable HIRC 48 MHz */
    CLK_EnableXtalRC(CLK_PWRCTL_HIRCEN_Msk);
    CLK_WaitClockReady(CLK_STATUS_HIRCSTB_Msk);
    CLK_SetHCLK(CLK_CLKSEL0_HCLKSEL_HIRC, CLK_CLKDIV0_HCLK(1));

    /* Enable module clocks: UART0, UART1, PDMA */
    CLK_EnableModuleClock(UART0_MODULE);
    CLK_EnableModuleClock(UART1_MODULE);
    CLK_EnableModuleClock(PDMA_MODULE);

    /* Select module clock sources */
    CLK_SetModuleClock(UART0_MODULE, CLK_CLKSEL1_UART0SEL_HIRC, CLK_CLKDIV0_UART0(1));
    CLK_SetModuleClock(UART1_MODULE, CLK_CLKSEL1_UART1SEL_HIRC, CLK_CLKDIV0_UART1(1));

    SystemCoreClockUpdate();

    /* Set APB Clock division */
    CLK->PCLKDIV = CLK_PCLKDIV_APB0DIV_DIV1 | CLK_PCLKDIV_APB1DIV_DIV1;

    /* UART0 Pins: PB.12 (RXD), PB.13 (TXD) for printf Debug */
//    SYS->GPB_MFPH = (SYS->GPB_MFPH & ~(SYS_GPB_MFPH_PB12MFP_Msk | SYS_GPB_MFPH_PB13MFP_Msk))
//                    | (SYS_GPB_MFPH_PB12MFP_UART0_RXD | SYS_GPB_MFPH_PB13MFP_UART0_TXD);
    
    /* UART0 Pins: PA.0 (RXD), PA.1 (TXD) for printf Debug */
    SYS->GPA_MFPL = (SYS->GPA_MFPL & ~(SYS_GPA_MFPL_PA0MFP_Msk | SYS_GPA_MFPL_PA1MFP_Msk))
                    | (SYS_GPA_MFPL_PA0MFP_UART0_RXD | SYS_GPA_MFPL_PA1MFP_UART0_TXD);

    /* UART1 Pins: PB.2 (RXD), PB.3 (TXD) for Quectel M66 GSM Module */
    SYS->GPB_MFPL = (SYS->GPB_MFPL & ~(SYS_GPB_MFPL_PB2MFP_Msk | SYS_GPB_MFPL_PB3MFP_Msk))
                    | (SYS_GPB_MFPL_PB2MFP_UART1_RXD | SYS_GPB_MFPL_PB3MFP_UART1_TXD);

    SYS_LockReg();

    /* Init UART peripherals */
    UART0_Init();
    UART1_Init();
    
    /* Init PDMA UART1 RX Ch0 and TX Ch1 */
    PDMA_Init();
}

/* PDMA Hardware Init for UART1 RX (Ch0) and TX (Ch1) */
static void PDMA_Init(void)
{
    SYS_ResetModule(PDMA_RST);

    /* Enable RX (Ch0) and TX (Ch1) */
    PDMA_Open(PDMA, (1UL << UART_RX_DMA_CH) | (1UL << UART_TX_DMA_CH));

    /* RX Channel 0: UART1 RX with Timeout */
    PDMA_SetTransferCnt(PDMA, UART_RX_DMA_CH, PDMA_WIDTH_8, GSM_RX_DMA_BUF_SIZE);
    PDMA_SetTransferAddr(PDMA, UART_RX_DMA_CH,
                         (uint32_t)&UART1->DAT, PDMA_SAR_FIX,
                         (uint32_t)g_u8RxDmaBuf, PDMA_DAR_INC);
    PDMA_SetTransferMode(PDMA, UART_RX_DMA_CH, PDMA_UART1_RX, FALSE, 0);
    PDMA_SetBurstType(PDMA, UART_RX_DMA_CH, PDMA_REQ_SINGLE, 0);

    /* Idle gap detection timeout */
    PDMA_SetTimeOut(PDMA, UART_RX_DMA_CH, 1, PDMA_TIMEOUT_TICK);
    PDMA_EnableTimeout(PDMA, (1UL << UART_RX_DMA_CH));
    PDMA->TOUTPSC = (PDMA->TOUTPSC & ~PDMA_TOUTPSC_TOUTPSC0_Msk) | (0UL << PDMA_TOUTPSC_TOUTPSC0_Pos);

    PDMA_EnableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TIMEOUT);
    PDMA_EnableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TRANS_DONE);

    /* TX Channel 1: UART1 TX */
    PDMA_EnableInt(PDMA, UART_TX_DMA_CH, PDMA_INT_TRANS_DONE);

    /* Set NVIC priority to max safe syscall priority */
    NVIC_SetPriority(PDMA_IRQn, configLIBRARY_LOWEST_INTERRUPT_PRIORITY);
    NVIC_EnableIRQ(PDMA_IRQn);

    /* Enable UART1 RX PDMA */
    UART_ENABLE_INT(UART1, UART_INTEN_RXPDMAEN_Msk);
}

/* Restart RX PDMA channel after transfer/timeout */
static void PDMA_RX_Restart(void)
{
    PDMA_DisableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TIMEOUT);
    PDMA_DisableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TRANS_DONE);

    UART_DISABLE_INT(UART1, UART_INTEN_RXPDMAEN_Msk);

    PDMA_STOP(PDMA, UART_RX_DMA_CH);

    PDMA_SetTransferCnt(PDMA, UART_RX_DMA_CH, PDMA_WIDTH_8, GSM_RX_DMA_BUF_SIZE);
    PDMA_SetTransferAddr(PDMA, UART_RX_DMA_CH,
                         (uint32_t)&UART1->DAT, PDMA_SAR_FIX,
                         (uint32_t)g_u8RxDmaBuf, PDMA_DAR_INC);
    PDMA_SetTransferMode(PDMA, UART_RX_DMA_CH, PDMA_UART1_RX, FALSE, 0);
    PDMA_SetBurstType(PDMA, UART_RX_DMA_CH, PDMA_REQ_SINGLE, 0);

    PDMA_SetTimeOut(PDMA, UART_RX_DMA_CH, 1, PDMA_TIMEOUT_TICK);
    PDMA_EnableTimeout(PDMA, (1UL << UART_RX_DMA_CH));

    PDMA_EnableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TIMEOUT);
    PDMA_EnableInt(PDMA, UART_RX_DMA_CH, PDMA_INT_TRANS_DONE);

    PDMA->CHCTL |= (1UL << UART_RX_DMA_CH);

    UART_ENABLE_INT(UART1, UART_INTEN_RXPDMAEN_Msk);
}

/* Trigger TX PDMA Transfer */
static void PDMA_TX_Start(const uint8_t *buf, uint32_t len)
{
    PDMA_SetTransferCnt(PDMA, UART_TX_DMA_CH, PDMA_WIDTH_8, len);
    PDMA_SetTransferAddr(PDMA, UART_TX_DMA_CH,
                         (uint32_t)buf,          PDMA_SAR_INC,
                         (uint32_t)&UART1->DAT,  PDMA_DAR_FIX);
    PDMA_SetTransferMode(PDMA, UART_TX_DMA_CH, PDMA_UART1_TX, FALSE, 0);
    PDMA_SetBurstType(PDMA, UART_TX_DMA_CH, PDMA_REQ_SINGLE, 0);

    PDMA->CHCTL |= (1UL << UART_TX_DMA_CH);

    UART_ENABLE_INT(UART1, UART_INTEN_TXPDMAEN_Msk);
}

/* PDMA Interrupt Handler */
void PDMA_IRQHandler(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint32_t status = PDMA_GET_INT_STATUS(PDMA);

    if (status & PDMA_INTSTS_ABTIF_Msk)
    {
        PDMA_CLR_ABORT_FLAG(PDMA, PDMA_GET_ABORT_STS(PDMA));
    }

    /* RX Ch0 Transfer Done (buffer full) */
    if (status & PDMA_INTSTS_TDIF_Msk)
    {
        if (PDMA_GET_TD_STS(PDMA) & (1UL << UART_RX_DMA_CH))
        {
            PDMA_CLR_TD_FLAG(PDMA, (1UL << UART_RX_DMA_CH));
            
            uint16_t xByteSend = xStreamBufferSendFromISR(xGsmRxStreamBuf, (void*)g_u8RxDmaBuf, 
                                                          GSM_RX_DMA_BUF_SIZE, &xHigherPriorityTaskWoken);
            if( xByteSend > 0) {
              ; 
            }
            
            UART_DISABLE_INT(UART1, UART_INTEN_RXPDMAEN_Msk);
            PDMA_RX_Restart();
        }

        /* TX Ch1 Transfer Done */
        if (PDMA_GET_TD_STS(PDMA) & (1UL << UART_TX_DMA_CH))
        {
            PDMA_CLR_TD_FLAG(PDMA, (1UL << UART_TX_DMA_CH));

            UART_DISABLE_INT(UART1, UART_INTEN_TXPDMAEN_Msk);
        }
    }

    /* RX Ch0 Request Timeout (idle line detected) */
    if (status & PDMA_INTSTS_REQTOF0_Msk)
    {
        PDMA_CLR_TMOUT_FLAG(PDMA, UART_RX_DMA_CH);

        uint32_t remaining = (PDMA->DSCT[UART_RX_DMA_CH].CTL & PDMA_DSCT_CTL_TXCNT_Msk) >> PDMA_DSCT_CTL_TXCNT_Pos;
        uint32_t rxLen = GSM_RX_DMA_BUF_SIZE - (remaining + 1);

        if (rxLen > 0 )
        {
            // receive stream buffer
          uint16_t xByteSend = xStreamBufferSendFromISR(xGsmRxStreamBuf, (void*)g_u8RxDmaBuf, rxLen, &xHigherPriorityTaskWoken);
          if( xByteSend > 0) {
              ; 
          }
        }

        UART_DISABLE_INT(UART1, UART_INTEN_RXPDMAEN_Msk);
        PDMA_RX_Restart();
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}


/* UART0 Init (Debug Console) */
static void UART0_Init(void)
{
    SYS_ResetModule(UART0_RST);
    UART_Open(UART0, 115200);
}

/* UART1 Init (Quectel M66 Module) */
static void UART1_Init(void)
{
    SYS_ResetModule(UART1_RST);
    UART_Open(UART1, 115200);
}