#ifndef _GSM_HANDLER_H_
#define _GSM_HANDLER_H_

#include <stdio.h>
#include <stdint.h>
#include "esp_event.h"
#include "esp_err.h"

#define MAX_RETRY_COUNTS 20
#define MAX_HARD_RESET_COUNTS 10

ESP_EVENT_DECLARE_BASE(ESP_GSM_EVENT);


// define the struct for the pins and uart which we will be using 
typedef struct 
{

    uint8_t gsmRxPin;
    uint8_t gsmTxPin;
    uint8_t gsmUartNumber;
    uint16_t gsmUartBaudRate;

    // apn config 
    char gsmApn[30];
    // declare the buffer sizes
    uint16_t gsmRxBufferSize;
    uint16_t gsmTxBufferSize;
    uint16_t gsmUartEventStackSize;
    uint16_t gsmUartEventQueueSize;
    uint8_t gsmUartEventTaskPriority;

} gsmHandlerConfig_t;

// setup the enum for the event handler 
enum
{
    GS_EVENT_POWER_OK,
    GSM_EVENT_NETWORK_OK,
    GSM_EVENT_GPRS_OK,
    GSM_EVENT_ERROR
};


void gsmHandlerTask(void *pvParameters);
esp_err_t gsmAddEventHandler(esp_event_handler_t gsmEventHandler);
esp_err_t gsmRemoveEventHandler(esp_event_handler_t gsmEventHandler);

#endif
