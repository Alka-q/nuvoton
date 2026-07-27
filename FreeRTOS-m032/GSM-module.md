> # *Proje Tasarımı* 
---
```mermaid
graph 
    GSM[GSM-M66] ==> ISR[UART1-RX-DMA-ISR]
    ISR ==>|Stream Buffer RX| NMT[Network Manager Task]
    NMT ==>|Stream Buffer TX| GSM
    NMT ==>|Queue| APP[Application Core Task]
    APP ==>|Queue| NMT
```
---
- **UART0**: GSM modülü ortak UART0 kullanır.
- **UART1**: printf (debug) işlemlerini UART1 yapar.
- **RX Yönü**: GSM -> UART1 RX -> DMA -> ISR -> Stream Buffer -> Network Manager Task.
- **TX Yönü**: Network Manager Task -> Stream Buffer -> DMA/UART1 TX -> GSM
- **Application Core**: AT komutu yanıtlarını parse eder.
- Proje tasarımı **FreeRTOS** (Nuvoton M032 / ARM Cortex-M0).