
#include "AF.h"
#include "OSAL.h"
#include "OSAL_Clock.h"
#include "OSAL_PwrMgr.h"
#include "ZComDef.h"
#include "ZDApp.h"
#include "ZDNwkMgr.h"
#include "ZDObject.h"
#include "math.h"

#include "nwk_util.h"
#include "zcl.h"
#include "zcl_app.h"
#include "zcl_diagnostic.h"
#include "zcl_general.h"
#include "zcl_lighting.h"
#include "zcl_ms.h"

#include "bdb.h"
#include "bdb_interface.h"
#include "gp_interface.h"

#include "Debug.h"

#include "OnBoard.h"

/* HAL */
#include "hal_adc.h"
#include "hal_drivers.h"
#include "hal_key.h"
#include "hal_led.h"

#include "battery.h"
#include "commissioning.h"
#include "factory_reset.h"
#include "utils.h"
#include "version.h"

#if defined (OTA_CLIENT) && (OTA_CLIENT == TRUE)
#include "zcl_ota.h"
#include "hal_ota.h"
#endif

/*********************************************************************
 * MACROS
 */
#define HAL_KEY_P0_EDGE_BITS HAL_KEY_BIT0

#define HAL_KEY_P1_EDGE_BITS (HAL_KEY_BIT1 | HAL_KEY_BIT2)

#define HAL_KEY_CODE_RELEASE_KEY HAL_KEY_CODE_NOKEY

/*********************************************************************
 * CONSTANTS
 */

/*********************************************************************
 * TYPEDEFS
 */

/*********************************************************************
 * GLOBAL VARIABLES
 */

extern bool requestNewTrustCenterLinkKey;
byte zclApp_TaskID;

/*********************************************************************
 * GLOBAL FUNCTIONS
 */

/*********************************************************************
 * LOCAL VARIABLES
 */

static uint8 currentSensorsReadingPhase = 0;

afAddrType_t inderect_DstAddr = {.addrMode = (afAddrMode_t)AddrNotPresent, .endPoint = 0, .addr.shortAddr = 0};

#if defined (OTA_CLIENT) && (OTA_CLIENT == TRUE)
#define DEVICE_POLL_RATE                 8000   // Poll rate for end device
#endif

/*********************************************************************
 * LOCAL FUNCTIONS
 */
static void zclApp_HandleKeys(byte shift, byte keys);
static void zclApp_Report(void);

static void zclApp_BasicResetCB(void);
static void zclApp_RestoreAttributesFromNV(void);
static void zclApp_SaveAttributesToNV(void);
static void zclApp_StopReloadTimer(void);
static void zclApp_StartReloadTimer(void);

static ZStatus_t zclApp_ReadWriteAuthCB(afAddrType_t *srcAddr, zclAttrRec_t *pAttr, uint8 oper);

static void zclApp_ReadSensors(void);

void DelayMs(unsigned int delaytime);

#if defined (OTA_CLIENT) && (OTA_CLIENT == TRUE)
static void zclApp_ProcessOTAMsgs( zclOTA_CallbackMsg_t* pMsg );
#endif

/*********************************************************************
 * ZCL General Profile Callback table
 */
static zclGeneral_AppCallbacks_t zclApp_CmdCallbacks = {
    zclApp_BasicResetCB, // Basic Cluster Reset command
    NULL, // Identify Trigger Effect command
    NULL, // On/Off cluster commands
    NULL, // On/Off cluster enhanced command Off with Effect
    NULL, // On/Off cluster enhanced command On with Recall Global Scene
    NULL, // On/Off cluster enhanced command On with Timed Off
    NULL, // RSSI Location command
    NULL  // RSSI Location Response command
};

void zclApp_Init(byte task_id) {
    zclApp_RestoreAttributesFromNV();
        
    // this is important to allow connects throught routers
    // to make this work, coordinator should be compiled with this flag #define TP2_LEGACY_ZC
    requestNewTrustCenterLinkKey = FALSE;

    zclApp_TaskID = task_id;

    zclGeneral_RegisterCmdCallbacks(1, &zclApp_CmdCallbacks);
    zcl_registerAttrList(zclApp_FirstEP.EndPoint, zclApp_AttrsFirstEPCount, zclApp_AttrsFirstEP);
    bdb_RegisterSimpleDescriptor(&zclApp_FirstEP);
    zcl_registerReadWriteCB(zclApp_FirstEP.EndPoint, NULL, zclApp_ReadWriteAuthCB);

    zcl_registerForMsg(zclApp_TaskID);
    
#if defined (OTA_CLIENT) && (OTA_CLIENT == TRUE)
    // Register for callback events from the ZCL OTA
    zclOTA_Register(zclApp_TaskID);
#endif

    // Register for all key events - This app will handle all key events
    RegisterForKeys(zclApp_TaskID);
    LREP("Started build %s \r\n", zclApp_DateCodeNT);
   
    zclApp_StartReloadTimer();
    
#if HAL_OTA_XNV_IS_SPI
//  XNV_SPI_INIT();
#endif
}

uint16 zclApp_event_loop(uint8 task_id, uint16 events) {
    afIncomingMSGPacket_t *MSGpkt;
    devStates_t zclApp_NwkState; //---
    
    (void)task_id; // Intentionally unreferenced parameter
    if (events & SYS_EVENT_MSG) {
        while ((MSGpkt = (afIncomingMSGPacket_t *)osal_msg_receive(zclApp_TaskID))) {
            switch (MSGpkt->hdr.event) {
            case KEY_CHANGE:
                zclApp_HandleKeys(((keyChange_t *)MSGpkt)->state, ((keyChange_t *)MSGpkt)->keys);
                
                break;
            case ZDO_STATE_CHANGE:
                zclApp_NwkState = (devStates_t)(MSGpkt->hdr.status);
                LREP("NwkState=%d\r\n", zclApp_NwkState);
                if (zclApp_NwkState == DEV_END_DEVICE) {

                } 
                break;
#if defined (OTA_CLIENT) && (OTA_CLIENT == TRUE)
            case ZCL_OTA_CALLBACK_IND:
                zclApp_ProcessOTAMsgs( (zclOTA_CallbackMsg_t*)MSGpkt  );
                break;
#endif
            case ZCL_INCOMING_MSG:
                if (((zclIncomingMsg_t *)MSGpkt)->attrCmd) {
                    osal_mem_free(((zclIncomingMsg_t *)MSGpkt)->attrCmd);
                }
                break;

            default:
                break;
            }
            // Release the memory
            osal_msg_deallocate((uint8 *)MSGpkt);
        }
        // return unprocessed events
        return (events ^ SYS_EVENT_MSG);
    }
    
    if (events & APP_REPORT_BATTERY_EVT) {
        LREPMaster("APP_REPORT_BATTERY_EVT\r\n");
        zclBattery_Report();

        return (events ^ APP_REPORT_BATTERY_EVT);
    }   
    if (events & APP_REPORT_EVT) {
        LREPMaster("APP_REPORT_EVT\r\n");
        zclApp_Report();
        
        return (events ^ APP_REPORT_EVT);
    }

    if (events & APP_READ_SENSORS_EVT) {
        LREPMaster("APP_READ_SENSORS_EVT\r\n");
        zclApp_ReadSensors();
        return (events ^ APP_READ_SENSORS_EVT);
    }    
    if (events & APP_SAVE_ATTRS_EVT) {
        LREPMaster("APP_SAVE_ATTRS_EVT\r\n");
        zclApp_SaveAttributesToNV();
        
        return (events ^ APP_SAVE_ATTRS_EVT);
    }

    // Discard unknown events
    return 0;
}

static void zclApp_HandleKeys(byte portAndAction, byte keyCode) {
    LREP("zclApp_HandleKeys portAndAction=0x%X keyCode=0x%X\r\n", portAndAction, keyCode);
    zclFactoryResetter_HandleKeys(portAndAction, keyCode);
    zclCommissioning_HandleKeys(portAndAction, keyCode);
    if (portAndAction & HAL_KEY_PRESS) {
        LREPMaster("Key press\r\n");
    }

    bool contact = portAndAction & HAL_KEY_PRESS ? TRUE : FALSE;
    uint8 endPoint = 0;
    if (portAndAction & HAL_KEY_PORT0) {
        LREPMaster("Key press PORT0\r\n");
        if (contact) {
          zclGeneral_SendOnOff_CmdToggle( 1, &inderect_DstAddr, FALSE, 0 );
          HalLedSet(HAL_LED_2, HAL_LED_MODE_TOGGLE);
//          HalOTAChkDL ( HAL_OTA_CRC_OSET );
/*          
          uint8 raw[32] = {0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 
                           0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                           0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                           0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
          };   

          uint8 raw[256];
          for (uint16 i=0; i < 256; i++){
            raw[i] = 0xAB;
          }
            
          HalOTAWrite(0x00000000, raw, 256, HAL_OTA_DL);
          
          DelayMs(100);
          
          OTA_ImageHeader_t header;
          HalOTARead(0x00000000, (uint8 *)&header, 256, HAL_OTA_DL);
*/          
        }
#ifdef POWER_SAVING         
        osal_pwrmgr_task_state(zclApp_TaskID, PWRMGR_HOLD);
#endif 
//        DelayMs(300); //test WDT
    } else if (portAndAction & HAL_KEY_PORT1) {     
        LREPMaster("Key press PORT1\r\n");

     } else if (portAndAction & HAL_KEY_PORT2) {
       LREPMaster("Key press PORT2\r\n");
       if (contact) {
//          HalSPIEraseChip();
          
          HalLedSet(HAL_LED_1, HAL_LED_MODE_BLINK);
          osal_start_timerEx(zclApp_TaskID, APP_REPORT_EVT, 200);
       }
     }
     LREP("contact=%d endpoint=%d\r\n", contact, endPoint);
     uint16 alarmStatus = 0;
     if (!contact) {
        alarmStatus |= BV(0);
     } 
}

static void zclApp_ReadSensors(void) {
    LREP("currentSensorsReadingPhase %d\r\n", currentSensorsReadingPhase);
//    uint8 response[10];
//    uint8 i = 0;
    /**
     * FYI: split reading sensors into phases, so single call wouldn't block processor
     * for extensive ammount of time
     * */
    switch (currentSensorsReadingPhase++) {
    case 0:
//        HalLedSet(HAL_LED_1, HAL_LED_MODE_BLINK);    
      zclBattery_Report();      
        break;
    default:
        osal_stop_timerEx(zclApp_TaskID, APP_READ_SENSORS_EVT);
        osal_clear_event(zclApp_TaskID, APP_READ_SENSORS_EVT);
        currentSensorsReadingPhase = 0;
        break;
    }

}

static void zclApp_Report(void) { osal_start_reload_timer(zclApp_TaskID, APP_READ_SENSORS_EVT, 100); }

static void zclApp_BasicResetCB(void) {
    LREPMaster("BasicResetCB\r\n");
    zclApp_ResetAttributesToDefaultValues();
    zclApp_SaveAttributesToNV();
}

static ZStatus_t zclApp_ReadWriteAuthCB(afAddrType_t *srcAddr, zclAttrRec_t *pAttr, uint8 oper) {
    LREPMaster("AUTH CB called\r\n");

    osal_start_timerEx(zclApp_TaskID, APP_SAVE_ATTRS_EVT, 100);
    return ZSuccess;
}

static void zclApp_SaveAttributesToNV(void) {
//    uint8 writeStatus = osal_nv_write(NW_APP_CONFIG, 0, sizeof(application_config_t), &zclApp_Config);
//    LREP("Saving attributes to NV write=%d\r\n", writeStatus);
    zclApp_StopReloadTimer();
    zclApp_StartReloadTimer();
}

static void zclApp_StopReloadTimer(void) {
    osal_stop_timerEx(zclApp_TaskID, APP_REPORT_BATTERY_EVT);
    osal_clear_event(zclApp_TaskID, APP_REPORT_BATTERY_EVT);    
}

static void zclApp_StartReloadTimer(void) {
    osal_start_reload_timer(zclApp_TaskID, APP_REPORT_BATTERY_EVT, (uint32)zclApp_Config.CfgBatteryPeriod * 60000);
}

static void zclApp_RestoreAttributesFromNV(void) {
    uint8 status = osal_nv_item_init(NW_APP_CONFIG, sizeof(application_config_t), NULL);
    LREP("Restoring attributes from NV  status=%d \r\n", status);
    if (status == NV_ITEM_UNINIT) {
        uint8 writeStatus = osal_nv_write(NW_APP_CONFIG, 0, sizeof(application_config_t), &zclApp_Config);
        LREP("NV was empty, writing %d\r\n", writeStatus);
    }
    if (status == ZSUCCESS) {
        LREPMaster("Reading from NV\r\n");
        osal_nv_read(NW_APP_CONFIG, 0, sizeof(application_config_t), &zclApp_Config);
    }
}

void DelayMs(unsigned int delaytime) {
  while(delaytime--)
  {
    uint16 microSecs = 1000;
    while(microSecs--)
    {
      asm("nop"); asm("nop"); asm("nop"); asm("nop"); asm("nop"); asm("nop");
    }
  }
}

#if defined (OTA_CLIENT) && (OTA_CLIENT == TRUE)
/*********************************************************************
 * @fn      zclSampleSw_ProcessOTAMsgs
 *
 * @brief   Called to process callbacks from the ZCL OTA.
 *
 * @param   none
 *
 * @return  none
 */
static void zclApp_ProcessOTAMsgs( zclOTA_CallbackMsg_t* pMsg )
{
  uint8 RxOnIdle;

  switch(pMsg->ota_event)
  {
  case ZCL_OTA_START_CALLBACK:
    if (pMsg->hdr.status == ZSuccess)
    {
      // Speed up the poll rate
      RxOnIdle = TRUE;
      ZMacSetReq( ZMacRxOnIdle, &RxOnIdle );
//      NLME_SetPollRate( 2000 );
      NLME_SetPollRate( 300 );
    }
    break;

  case ZCL_OTA_DL_COMPLETE_CALLBACK:
    if (pMsg->hdr.status == ZSuccess)
    {
      // Reset the CRC Shadow and reboot.  The bootloader will see the
      // CRC shadow has been cleared and switch to the new image
      HalOTAInvRC();
      SystemReset();
    }
    else
    {
#if (ZG_BUILD_ENDDEVICE_TYPE)    
      // slow the poll rate back down.
      RxOnIdle = FALSE;
      ZMacSetReq( ZMacRxOnIdle, &RxOnIdle );
      NLME_SetPollRate(DEVICE_POLL_RATE);
#endif
    }
    break;

  default:
    break;
  }
}
#endif // defined (OTA_CLIENT) && (OTA_CLIENT == TRUE)
/****************************************************************************
****************************************************************************/
