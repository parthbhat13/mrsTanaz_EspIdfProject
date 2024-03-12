/*
    I intend to put the license which basically allows 
    the developer to use this code but they have to put my name in it 
    ive forgotten what is that license called as... so ust consider this 

    Developed By: Parth Yatin Temkar (RAiMECH Aero Pvt. Ltd)
*/

#include "gsmHandler.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_modem_api.h"

#include "esp_log.h"

static const char TAG[] = "GSM_HANDLER";


// i dont like my variables hanging around 
// so we make a struct to maintain them 
typedef struct 
{
    uint8_t stateMachine;
    uint8_t hardResetCounter;
    uint8_t isPPPLost;
} gsmRuntimeVaribles_t;


// typedef for the gsm error
typedef uint8_t gsm_err_t;

// GSM error codes which can be used to identify the error
#define GSM_OK 0
#define GSM_NO_SIM 1
#define GSM_NO_RSSI 2
#define GSM_NO_SERVICE 3
#define GSM_NOT_DETECTED 4
#define GSM_UNKNOWN_ERROR 5
#define GSM_NO_RESPONSE 6

// make an array to show gsm error to name to print
const char *gsmErrorCodes[] = {"GSM_OK", "GSM_NO_SIM", "GSM_NO_RSSI", "GSM_NO_SERVICE", "GSM_NOT_DETECTED", "GSM_UNKNOWN_ERROR", "GSM_NO_RESPONSE"};
#define gsmErrorToName(x) (gsmErrorCodes[x])


// declare the event based variables 
ESP_EVENT_DEFINE_BASE(ESP_GSM_EVENT);
esp_event_loop_handle_t gsmEventLoopHandle;

// esp modem related declarations 
esp_modem_dce_t *gsmDce;
esp_netif_t *espGsmNetif;

// just to keep things neat we used a struct for even our local variables 
gsmRuntimeVaribles_t gsmRunTimeVars;
// internal functions 
// declare the gsmModem based event groups 
static void onPPPChangedEventCallback(void *arg, esp_event_base_t eventBase, int32_t eventId, void *eventData);
static void onIPEventCallback(void *arg, esp_event_base_t eventBase, int32_t eventId, void *eventData);

gsm_err_t checkATSync(void);
gsm_err_t checkCPINReady(void);
gsm_err_t checkRssi(void);
gsm_err_t checkNetworkRegistration(void);
gsm_err_t checkGsmDataMode(void);
gsm_err_t setGsmInCommandMode(void);
gsm_err_t resetGsmAt(void);

void destroyGsm(void);

// global functions 
void gsmHandlerTask(void *pvParameters)
{
    esp_err_t err = ESP_FAIL;
    gsm_err_t gsmError;


    // pass in all the important data from the pvParamaters 
    gsmHandlerConfig_t *gsmConfigurationMain = (gsmHandlerConfig_t *)pvParameters;

    ESP_LOGI(TAG, "Starting GSM Task");

    // configure the event loop first and then start configuring the modem things 
    esp_event_loop_args_t gsmEventLoopArgs =
    {
        .queue_size = 10,
        .task_name = NULL
    };

    err = esp_event_loop_create(&gsmEventLoopArgs, &gsmEventLoopHandle);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed To Create Event Loop: %d : Reason: %s", err, esp_err_to_name(err));
        vTaskDelete(NULL);
    }


    // create the esp's internal netif thingy
    err = esp_netif_init();
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed Init NetIf %d : Reason: %s", err, esp_err_to_name(err));
        vTaskDelete(NULL);
    }

    err = esp_event_loop_create_default();
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed at esp netif event loop %d : Reason: %s", err, esp_err_to_name(err));
        vTaskDelete(NULL);
    }

    err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &onIPEventCallback, NULL);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed Register IP EventHandler %d : Reason: %s", err, esp_err_to_name(err));
        vTaskDelete(NULL);
    }

    err = esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &onPPPChangedEventCallback, NULL);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed Register PPP EventHandler: %d : Reason: %s", err, esp_err_to_name(err));
        vTaskDelete(NULL);
    }

    // now configure the DTE and its thingy
    esp_modem_dte_config_t gsmDteConfig = //= ESP_MODEM_DTE_DEFAULT_CONFIG();
    {
        .uart_config.tx_io_num = gsmConfigurationMain->gsmTxPin,
        .uart_config.rx_io_num = gsmConfigurationMain->gsmRxPin,
        .uart_config.rts_io_num = UART_PIN_NO_CHANGE,
        .uart_config.cts_io_num = UART_PIN_NO_CHANGE,
        .uart_config.baud_rate = gsmConfigurationMain->gsmUartBaudRate,
        .uart_config.data_bits = UART_DATA_8_BITS,
        .uart_config.stop_bits = UART_STOP_BITS_1,
        .uart_config.parity = UART_PARITY_DISABLE,
        .uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE,
        .uart_config.source_clk = UART_SCLK_APB,
        .uart_config.rx_buffer_size = gsmConfigurationMain->gsmRxBufferSize,
        .uart_config.tx_buffer_size = gsmConfigurationMain->gsmTxBufferSize,
        .uart_config.event_queue_size = gsmConfigurationMain->gsmUartEventQueueSize,
        .task_stack_size = gsmConfigurationMain->gsmUartEventStackSize,
        .task_priority = gsmConfigurationMain->gsmUartEventTaskPriority,
        .dte_buffer_size = gsmConfigurationMain->gsmRxBufferSize / 2
    };

    // configure the dce 
    esp_modem_dce_config_t gsmDceConfig = ESP_MODEM_DCE_DEFAULT_CONFIG(&gsmConfigurationMain->gsmApn[0]);

    // configure the netif adapter 
    esp_netif_config_t gsmNetifPPPConfig = ESP_NETIF_DEFAULT_PPP();

    // configure the the objects 
    if(espGsmNetif == NULL)
    {
        espGsmNetif = esp_netif_new(&gsmNetifPPPConfig);
        assert(espGsmNetif);
    }

    if(gsmDce == NULL)
    {
        gsmDce = esp_modem_new_dev(ESP_MODEM_DCE_BG96, &gsmDteConfig, &gsmDceConfig, espGsmNetif);
        assert(gsmDce);
    }

    ESP_LOGI(TAG, "GSM Stack Configured.");

    // make sure to set the state machine to start from ZERO
    gsmRunTimeVars.stateMachine = 0;


    // trust me usually i happen to create a struct for the variables 
    // but i cannot think straight in terms of how many different types of variables i might need 
    // all i can think of now is the GSM should work independently 
    // keeping that in mind let mrs.Taanaaz handle the whole logic part... 

   
    while(1)
    {
        // run the event loop thingy here 
        // okay honestly we already have 2 event loops running inside of the espmodem library
        // i am using this here as im just used to have a callback function in my main logic 
        // makes my life simpler.. but i happen to have multiple thoughts on this implementation of mine.... whooppss!!!!
        esp_event_loop_run(gsmEventLoopHandle, 10 / portTICK_PERIOD_MS);


        // make a state machine.. because why not? i want the things to go smooth as butter 
        switch(gsmRunTimeVars.stateMachine)
        {
            // start with the AT command like just ask "AT"
            case 0:
                gsmError = checkATSync();
                if(gsmError != GSM_OK)
                {
                    // ideally we should restart the gsm with some delay maybe? so go to some last state 
                    ESP_LOGE(TAG, "Switching State Machine To IDEAL!!!");
                    gsmRunTimeVars.stateMachine = 99;
                }
                else 
                {
                    gsmRunTimeVars.stateMachine = 1;
                }
                break;

            case 1: 
                // check for the CPIN here 
                gsmError = checkCPINReady();
                if(gsmError != GSM_OK)
                {
                    ESP_LOGE(TAG, "Failed At CPIN: Reason: %s", gsmErrorToName(gsmError));
                    // ideally we should again probably restart the gsm but okay 
                    // not sure if mrs.tanaaz is using the power on pin here or not 
                    ESP_LOGE(TAG, "Switching State Machine To IDEAL!!");
                    gsmRunTimeVars.stateMachine = 99;
                }
                else 
                {
                    gsmRunTimeVars.stateMachine = 2;
                }
                break;

            case 2:

                gsmError = checkRssi();
                if(gsmError != GSM_OK)
                {
                    ESP_LOGE(TAG, "Failed To Get RSSI, Reason: %s", gsmErrorToName(gsmError));
                    // again just simply restart the gsm 
                    gsmRunTimeVars.stateMachine = 99;
                }
                else 
                {
                    gsmRunTimeVars.stateMachine = 3;
                }
                break;

            case 3:
                gsmError = checkNetworkRegistration();
                if(gsmError != GSM_OK)
                {
                    ESP_LOGE(TAG, "Failed To Get Network Registration: Reason: %s", gsmErrorToName(gsmError));
                    gsmRunTimeVars.stateMachine = 99;
                }
                else 
                {
                    gsmRunTimeVars.stateMachine = 4;
                }
                break;

            case 4:
                gsmError = checkGsmDataMode();
                if(gsmError != GSM_OK)
                {
                    ESP_LOGE(TAG, "Failed To Set GSM In Data Mode: Reason: %s", gsmErrorToName(gsmError));
                    gsmRunTimeVars.stateMachine = 99;
                }
                else 
                {
                    gsmRunTimeVars.stateMachine = 5;
                }
                break;

            case 5:
                // technically this is an ideal mode 
                // meaning? we do not do anything over here just check if there are any issues with the gsm
                // and based on that we take the actions. 
                break;



            case 99:
                // this is where we just restart the gsm 
                // and add to the max gsm hard reset counter 
                gsmRunTimeVars.hardResetCounter++;
                gsmHardReset();
                vTaskDelay(2000 / portTICK_PERIOD_MS);
                ESP_LOGW(TAG,"GSM HardResetDone!, Counts: %d", gsmRunTimeVars.hardResetCounter);
                if(gsmRunTimeVars.hardResetCounter >= MAX_HARD_RESET_COUNTS)
                {
                    ESP_LOGE(TAG, "Tried Enough Resetting GSM... That's ALL");
                    // add any logic which we want to over here 
                    while(1) {vTaskDelay(100 / portTICK_PERIOD_MS);}
                }

                break;

            default: break;


        }

        vTaskDelay(90 / portTICK_PERIOD_MS);


    }
}


esp_err_t gsmAddEventHandler(esp_event_handler_t gsmEventHandler);
esp_err_t gsmRemoveEventHandler(esp_event_handler_t gsmEventHandler);



// internal functions 
static void onPPPChangedEventCallback(void *arg, esp_event_base_t eventBase, int32_t eventId, void *eventData)
{
    ESP_LOGI(TAG, "PPP State Change Event | ID: %d", eventId);

    if (eventId == NETIF_PPP_ERRORUSER)
    {
        // User Interrupted event From esp-netif
        esp_netif_t *netIf = eventData;
        ESP_LOGI(TAG, "User Interrupted Event From netif: %p", netIf);
    }
}

static void onIPEventCallback(void *arg, esp_event_base_t eventBase, int32_t eventId, void *eventData)
{
    ESP_LOGI(TAG, "IP Event | ID: %d", eventId);

    if (eventId == IP_EVENT_PPP_GOT_IP)
    {
        esp_netif_dns_info_t dnsInfo;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)eventData;
        esp_netif_t *netif = event->esp_netif;

        ESP_LOGI(TAG, "Modem Connect to PPP Server");
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        ESP_LOGI(TAG, "IP          : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask     : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway     : " IPSTR, IP2STR(&event->ip_info.gw));
        esp_netif_get_dns_info(netif, 0, &dnsInfo);
        ESP_LOGI(TAG, "Name Server1: " IPSTR, IP2STR(&dnsInfo.ip.u_addr.ip4));
        esp_netif_get_dns_info(netif, 1, &dnsInfo);
        ESP_LOGI(TAG, "Name Server2: " IPSTR, IP2STR(&dnsInfo.ip.u_addr.ip4));
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        
       
    }

    else if (eventId == IP_EVENT_PPP_LOST_IP)
    {
        ESP_LOGE(TAG, "Modem Disconnected From PPP Server");
    }

    else if (eventId == IP_EVENT_GOT_IP6)
    {
        ESP_LOGI(TAG, "Got IPv6 Event");
        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)eventData;
        ESP_LOGI(TAG, "Got IPv6 address " IPV6STR, IPV62STR(event->ip6_info.ip));
    }
}

// internal functions 
gsm_err_t checkAtSync(void)
{
    uint8_t atSyncRetryCounter = 0;

    // Create the gsm Error Instance
    esp_err_t err;

    // Here we connect to the network
    while (atSyncRetryCounter != MAX_RETRY_COUNTS)
    {
        ESP_LOGI(TAG, "Syncing AT Command..");

        err = esp_modem_sync(gsmDce);

        if (err != ESP_OK)
        {
            // Retry as it failed
            ESP_LOGE(TAG, "Failed AT Sync Retrying --> %d", atSyncRetryCounter);
            atSyncRetryCounter++;

            if (atSyncRetryCounter == 4)
            {
                // so we should give the power pulse to the gsm module
                giveGsmPowerPulse();
            }

            if (atSyncRetryCounter == 10)
            {
                // Send a reset pulse here now, even after trying for so long if we failed, then again try to reset
                giveGsmPowerPulse();
            }

            // if the error persists gsm have failed with basic at commands so skip it
            if (atSyncRetryCounter == MAX_RETRY_COUNTS)
            {
                return GSM_NOT_DETECTED;
            }
        }

        else if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "AT Sync Successful");
            atSyncRetryCounter = MAX_RETRY_COUNTS;
            return GSM_OK;
        }

        vTaskDelay(2000/portTICK_PERIOD_MS);
    }

    return GSM_OK;
}

// function to check the CPIN status
gsm_err_t checkCPINReady(void)
{
    uint8_t checkSimRetryCounter = 0; // Variable to save the Counts for checking the SIM

    // at commands data array
    char atCommandData[50];

    // clear the data in the array once
    memset(atCommandData, 0, sizeof(atCommandData));

    // esp error state for the gsm esp library
    esp_err_t err;

    // once we have got the at success we check if the simcard is installed or not
    while (checkSimRetryCounter != MAX_RETRY_COUNTS)
    {
        ESP_LOGI(TAG, "Checking SIM Card..");

        // here we send the at command to check for the simcard
        // AT+CPIN? and the response would be +CPIN: READY
        err = esp_modem_at(gsmDce, "AT+CPIN?", &atCommandData[0]);

        if (err != ESP_OK)
        {
            // Retry as it failed
            ESP_LOGE(TAG, "Failed SIM Check Retrying --> %d", checkSimRetryCounter);
            checkSimRetryCounter++;

            // if the error persists gsm have failed with basic at commands so skip it
            if (checkSimRetryCounter == MAX_RETRY_COUNTS)
            {
                // Set the Error State
               
                return GSM_NO_RESPONSE;
            }
        }

        else if (err == ESP_OK)
        {
            // compare the string we have received
            if (strstr(atCommandData, "+CPIN: READY") != NULL)
            {
                ESP_LOGI(TAG, "SIM Card Available");
                checkSimRetryCounter = MAX_RETRY_COUNTS;
                memset(atCommandData, 0, sizeof(atCommandData));
                return GSM_OK;
            }

            //+CME ERROR: SIM not inserted
            else if (strstr(atCommandData, "+CME ERROR:") != NULL)
            {
                if(checkSimRetryCounter >= 5)
                {
                    ESP_LOGE(TAG, "SIM Card Not Installed Failed");
                    checkSimRetryCounter = MAX_RETRY_COUNTS;
                    memset(atCommandData, 0, sizeof(atCommandData));
                    return GSM_NO_SIM;
                }
                else 
                {
                    checkSimRetryCounter++;
                }

            }

            else
            {
                // Retry as it failed
                ESP_LOGE(TAG, "Failed SIM Check Retrying --> %d", checkSimRetryCounter);
                checkSimRetryCounter++;

                // if the error persists gsm have failed with basic at commands so skip it
                if (checkSimRetryCounter == MAX_RETRY_COUNTS)
                {
                    // Set the Error State
                    ESP_LOGE(TAG, "GSM SIM ERROR STR: %s", &atCommandData[0]);
                    memset(atCommandData, 0, sizeof(atCommandData));
                    return GSM_UNKNOWN_ERROR;
                }
            }
        }

        vTaskDelay(2500 / portTICK_PERIOD_MS);
    }

    return GSM_OK;
}

// check RSSI
gsm_err_t checkRssi(void)
{
    esp_err_t err;

    uint8_t rssiRetryCounter = 0; // Variable to save the Counts for RSSI Retry
    int rssi, ber;

    // +CSQ: 16,99
    // Here we try to get the RSSI values
    while (rssiRetryCounter != MAX_RETRY_COUNTS)
    {
        
        ESP_LOGI(TAG, "Getting Signal Strength..");

        err = esp_modem_get_signal_quality(gsmDce, &rssi, &ber);

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed To Get RSSI Retrying --> %d", rssiRetryCounter);
            rssiRetryCounter++;
        }

        else if (err == ESP_OK)
        {
            // check the rssi and ber information over here
            if (rssi == 99)
            {
                if (rssiRetryCounter < MAX_RETRY_COUNTS - 1)
                {
                    ESP_LOGE(TAG, "Failed To Get RSSI Retrying --> %d", rssiRetryCounter);
                    rssiRetryCounter++;
                }
                else
                {
                    ESP_LOGE(TAG, "No RSSI, No Signal");
                    return GSM_NO_RSSI;
                }
            }
            else
            {
                ESP_LOGI(TAG, "Got RSSI: %d, BER: %d", rssi, ber);
                // basically according to the GSM data sheet of simcom here is the format we shall follow 
                // 0 --> -113 dBm or less 
                // 1 --> -111dBm
                // 2-30 --> -109 - -53dBm
                // 31 or greater --> -53dBm or greater
                rssiRetryCounter = MAX_RETRY_COUNTS;
                return GSM_OK;
            }
        }

        vTaskDelay(2000/portTICK_PERIOD_MS);
    }

    return GSM_OK;
}

// function to check the network service registration
gsm_err_t checkNetworkRegistration(void)
{
    esp_err_t err;

    uint8_t registrationRetryCounter = 0;

    // at commands data array
    char atCommandData[120];

    // clear the data in the array once
    memset(atCommandData, 0, sizeof(atCommandData));

    // check for the network service
    // worst case scenario if someone installs a non working simcard with no service
    // while loop is not required here as we are just checking for the network service
    // expected error message would be +CPSI: NO SERVICE,Online

    while (registrationRetryCounter != MAX_RETRY_COUNTS)
    {
        err = esp_modem_at(gsmDce, "AT+CPSI?", &atCommandData[0]);

        ESP_LOGI(TAG, "Checking Network Service Status..");
        if (err != ESP_OK)
        {
            registrationRetryCounter++;
            ESP_LOGE(TAG, "Failed To Get Network Service Status");
            if (registrationRetryCounter == MAX_RETRY_COUNTS)
            {
                return GSM_NO_RESPONSE;
            }
        }

        else if (err == ESP_OK)
        {
            // compare the string we have received
            if (strstr(atCommandData, "+CPSI: NO SERVICE") != NULL)
            {
                registrationRetryCounter++;
                ESP_LOGE(TAG, "No Network Service");
                if (registrationRetryCounter == MAX_RETRY_COUNTS)
                {
                    return GSM_NO_SERVICE;
                }
            }
            else
            {
                ESP_LOGW(TAG, "Network Service Msg: %s", atCommandData);
                ESP_LOGI(TAG, "Network Service Available");
                return GSM_OK;
            }

            if (registrationRetryCounter == MAX_RETRY_COUNTS)
            {
                return GSM_UNKNOWN_ERROR;
            }
        }
        vTaskDelay(2000/portTICK_PERIOD_MS);
    }

    return GSM_OK;
}

// function to setup the gsm in data mode
gsm_err_t checkGsmDataMode(void)
{
    esp_err_t err;

    uint8_t dataModeRetryCounter = 0; // Variable to save the counts for Data Mode Retry

    // Set the modem to work in data mode
    while (dataModeRetryCounter != MAX_RETRY_COUNTS)
    {
        ESP_LOGI(TAG, "Setting GSM In Data Mode..");

        err = esp_modem_set_mode(gsmDce, ESP_MODEM_MODE_DATA);

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed To Set Data Mode Retrying --> %d", dataModeRetryCounter);
            dataModeRetryCounter++;
        }
        else if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Set GSM In Data Mode Success");
            dataModeRetryCounter = MAX_RETRY_COUNTS;
            return GSM_OK;
        }

        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    return GSM_NO_RESPONSE;
}

// function to set the gsm in the command mode again and then do the other things 
gsm_err_t setGsmInCommandMode(void)
{
    esp_err_t err;

    uint8_t commandModeRetryCounter = 0; // Variable to save the counts for Data Mode Retry

    // Set the modem to work in data mode
    while (commandModeRetryCounter != MAX_RETRY_COUNTS)
    {
        ESP_LOGI(TAG, "Setting GSM In Command Mode..");

        err = esp_modem_set_mode(gsmDce, ESP_MODEM_MODE_COMMAND);

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed To Set Command Mode Retrying --> %d", commandModeRetryCounter);
            commandModeRetryCounter++;
        }
        else if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Set GSM In Command Mode Success");
            commandModeRetryCounter = MAX_RETRY_COUNTS;
            return GSM_OK;
        }

        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    return GSM_NO_RESPONSE;
}



