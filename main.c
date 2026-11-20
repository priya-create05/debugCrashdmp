#define configUSE_TRACE_FACILITY            1
#define INCLUDE_uxTaskGetStackHighWaterMark 1



#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

/* 
 * Linker-defined symbols (TI Clang / GCC linker command file symbols)
 * Note: Depending on your exact project setup, verify whether your linker script 
 * uses __bss_end__ or __bss_end.
 */
extern uint32_t __bss_end__;

/**
 * @brief Thread-Aware Logging Function
 * Automatically prefixes logs with [ISR], [Main/Startup], or the active [TaskName].
 */
void app_log(const char *format, ...) {
    // 1. Check if executing inside an Interrupt Service Routine (IPSR != 0)
    if (__get_IPSR() != 0) {
        printf("[ISR] ");
    } 
    // 2. Check if FreeRTOS scheduler has not started yet
    else if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        printf("[Main/Startup] ");
    } 
    // 3. Normal FreeRTOS Task context
    else {
        TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
        char *task_name = (current_task != NULL) ? pcTaskGetName(current_task) : "UnknownTask";
        printf("[%s] ", task_name);
    }

    // Print the variable argument message payload
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    printf("\r\n");
}

/**
 * @brief Method 1: Get current Main Stack headroom (Bytes)
 * Measures distance between current Main Stack Pointer (MSP) and BSS end.
 */
uint32_t get_current_main_stack_free(void) {
    uint32_t current_msp = __get_MSP();
    uint32_t lower_bound = (uint32_t)&__bss_end__;
    
    if (current_msp > lower_bound) {
        return current_msp - lower_bound;
    }
    return 0; // Stack overflow condition indicator
}

/* --- Sample Task 1: Sensor Task --- */
void vSensorTask(void *pvParameters) {
    for(;;) {
        app_log("Acquiring sensor data... Main Stack Free: %u bytes", get_current_main_stack_free());
        
        // Check its own task stack high water mark (returns unused words, multiply by 4 for bytes)
        UBaseType_t task_stack_free_words = uxTaskGetStackHighWaterMark(NULL);
        app_log("SensorTask Stack Headroom: %u bytes remaining", (unsigned int)(task_stack_free_words * 4));

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* --- Sample Task 2: Communication Task --- */
void vCommTask(void *pvParameters) {
    for(;;) {
        app_log("Transmitting packets over UART/SPI...");
        
        UBaseType_t task_stack_free_words = uxTaskGetStackHighWaterMark(NULL);
        app_log("CommTask Stack Headroom: %u bytes remaining", (unsigned int)(task_stack_free_words * 4));

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

int main(void) {
    // 1. Hardware Initialization (MSPM0 driver library setup, clocks, GPIO, UART for printf)
    // SYSCFG_DL_init(); 

    app_log("System booting up. Initial Main Stack free space: %u bytes", get_current_main_stack_free());

    // 2. Create FreeRTOS Tasks
    // Stack sizes are specified in words (e.g., 256 words = 1024 bytes)
    xTaskCreate(vSensorTask, "SensorTask", 256, NULL, 2, NULL);
    xTaskCreate(vCommTask,  "CommTask",   256, NULL, 1, NULL);

    app_log("Starting FreeRTOS Scheduler...");

    // 3. Start Scheduler (Execution hands over to tasks, main code stops here)
    vTaskStartScheduler();

    // Should never reach here unless out of heap memory
    for(;;);
}




#include "ti_msp_dl_config.h"
#include <stdint.h>


#define configCHECK_FOR_STACK_OVERFLOW 2
/* Forward declaration of the C logging function */
void handle_fault_dump(uint32_t pc_address);

/* 
 * Naked assembly Hard Fault Handler 
 * __attribute__((naked)) prevents the compiler from adding prologue/epilogue stack frames.
 */
void HardFault_Handler(void) __attribute__((naked));

void HardFault_Handler(void)
{
    __asm volatile
    (
        /* 1. Check bit 2 of the Link Register (LR) to see which stack was active */
        "MOVS R0, #4           \n\t"
        "MOV R1, LR            \n\t"
        "TST R0, R1            \n\t"
        "BEQ _UseMSP           \n\t"
        
        /* 2a. Bit 2 is 1: A FreeRTOS task crashed -> Use Process Stack Pointer (PSP) */
        "MRS R0, PSP           \n\t" 
        "B _ExtractPC          \n\t"
        
    "_UseMSP:                  \n\t"
        /* 2b. Bit 2 is 0: Kernel, ISR, or pre-scheduler crashed -> Use Main Stack Pointer (MSP) */
        "MRS R0, MSP           \n\t" 
        
    "_ExtractPC:               \n\t"
        /* 3. Extract PC from stack offset 24 and put it into R0 
         * (R0 becomes the first argument for our C function per AAPCS rules) */
        "LDR R0, [R0, #24]     \n\t" 
        
        /* 4. Load the address of our C function into R2 and branch to it */
        "LDR R2, =handle_fault_dump \n\t"
        "BX R2                 \n\t"
    );
}

/* 
 * C function that receives the crashing PC address and streams it over UART 
*/
void handle_fault_dump(uint32_t pc_address)
{
    /* Send notification header over UART using polling */
    char *msg = "\r\n[HARD FAULT CRASH] Program Counter (PC): 0x";
    while (*msg) {
        while (DL_UART_Main_isTransmitFIFOFull(UART_0_INST));
        DL_UART_Main_transmitData(UART_0_INST, *msg++);
    }

    /* Convert the 32-bit hex address into ASCII characters and transmit */
    char hex_chars[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        uint8_t nibble = (pc_address >> (i * 4)) & 0x0F;
        while (DL_UART_Main_isTransmitFIFOFull(UART_0_INST));
        DL_UART_Main_transmitData(UART_0_INST, hex_chars[nibble]);
    }

    /* Send a newline suffix */
    char *end_msg = "\r\nSystem halted.\r\n";
    while (*end_msg) {
        while (DL_UART_Main_isTransmitFIFOFull(UART_0_INST));
        DL_UART_Main_transmitData(UART_0_INST, *end_msg++);
    }

    /* Permanent lock to freeze execution */
    while (1) {
        // Optional: Toggle an error LED here if desired
    }
}
