/*
* Copyright (C) 2015 NXP Semiconductors
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

/*!
 * \file phDTALib.c
 *
 * Project: NFC DTA
 *
 */

/*
 ************************* Header Files ****************************************
 */

/* Preprocessor includes for different platform */
#ifdef WIN32
#include <windows.h>
#include "phNfcTypes.h"
#else
#include <utils/Log.h>
#include "data_types.h"
#endif

#include "phMwIf.h"
#include "phDTALib.h"
#include "phDTAOSAL.h"
#include "phOsal_LinkList.h"
#include "phOsal_Queue.h"
#include "phDTATst.h"
#ifdef __cplusplus
extern "C" {
#endif

phDtaLib_sHandle_t      g_DtaLibHdl = {0};
uint8_t gx_status = FALSE;   /**<(Check) change the type def.... also NFA_STATUS_FAILED */
uint8_t gs_paramBuffer[400]; /**< Buffer for passing Data during operations */

#define PHMWIF_THREAD_DELETE_DUMMY_EVT 0xFF /**<Dummy event to exit thread*/
#define P2P_ENABLED     1 /*in case of enable, P2P flag will have 1,2,4 or addition of these values*/
#define P2P_DISABLED    0

static const uint8_t PHDTALIB_LLCP_GEN_BYTES_INITIATOR[]   = {0x46,0x66,0x6D, /**< LLCP magic bytes */
                                                              0x01,0x01,0x12, /**< major, minor Version TLV*/
                                                              0x02,0x02,0x07,0xFE, /**< MIUX TLV*/
                                                              0x03,0x02,0x00,0x03, /**< WKS TLV*/
                                                              0x04,0x01,0x64}; /**< LTO TLV*/
static const uint8_t PHDTALIB_LLCP_GEN_BYTES_TARGET[]   = {0x46,0x66,0x6D,0x01,0x01,0x12,0x03,0x02,0x00,0x03,0x04,0x01,0x64};
static const uint8_t PHDTALIB_LLCP_GEN_BYTES_LEN_INITIATOR = 0x11;
static const uint8_t PHDTALIB_LLCP_GEN_BYTES_LEN_TARGET = 0x0D;

static void      phDtaLibi_CbMsgHandleThrd(void *param);
static void*     phDtaLibi_MemAllocCb(void* memHdl, uint32_t size);
static int32_t   phDtaLibi_MemFreeCb(void* memHdl, void* ptrToMem);
static void phDtaLibi_SetMwIfConfig();

/**
 * Initialize DTA Lib
 */
DTASTATUS phDtaLib_Init() {
    OSALSTATUS dwOsalStatus = OSALSTATUS_FAILED;
    MWIFSTATUS dwMwifStatus = MWIFSTATUS_FAILED;
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;
    phOsal_QueueCreateParams_t sQueueCreatePrms;
    phOsal_Config_t config;
    uint32_t dwThreadId = 0;

    config.dwCallbackThreadId = 0;
    config.pContext = NULL;
    config.pLogFile = (uint8_t *)"phOSALLog";

    phOsal_SetLogLevel(PHOSAL_LOGLEVEL_DATA_BUFFERS);
    LOG_FUNCTION_ENTRY;
    dwOsalStatus = phOsal_Init(&config);
    if(OSALSTATUS_SUCCESS != dwOsalStatus)
    {
        phOsal_LogInfo((const uint8_t*)"OSAL Init Failed !\n");
    }

    if(dtaLibHdl->bDtaInitialized)
    {
        phOsal_LogInfo((const uint8_t*)"DTALib>DTALib already Initialized!!.Just returning\n");
        return DTASTATUS_SUCCESS;
    }
    phOsal_LogDebugString((const uint8_t*)"DTALib>Version:",(const uint8_t*)DTALIB_VERSION_STR);
    phOsal_LogDebug((const uint8_t*)"DTALib> Creating queue to push/pull messages sent from clbaks to dtalib\n");
    sQueueCreatePrms.memHdl = NULL;
    sQueueCreatePrms.MemAllocCb = phDtaLibi_MemAllocCb;
    sQueueCreatePrms.MemFreeCb = phDtaLibi_MemFreeCb;
    sQueueCreatePrms.wQLength = 20;
    sQueueCreatePrms.eOverwriteMode = PHOSAL_QUEUE_NO_OVERWRITE;

    dwOsalStatus = phOsal_QueueCreate(&dtaLibHdl->queueHdl, &sQueueCreatePrms);
    if(dwOsalStatus != OSALSTATUS_SUCCESS)
    {
        phOsal_LogError((const uint8_t*)"DTALib> Unable to create Queue");
    }

    phOsal_LogDebug((const uint8_t*)"DTALib> Creating queue to push/pull Congested Datab\n");
    sQueueCreatePrms.memHdl = NULL;
    sQueueCreatePrms.MemAllocCb = phDtaLibi_MemAllocCb;
    sQueueCreatePrms.MemFreeCb = phDtaLibi_MemFreeCb;
    sQueueCreatePrms.wQLength = 15;
    sQueueCreatePrms.eOverwriteMode = PHOSAL_QUEUE_NO_OVERWRITE;

    dwOsalStatus = phOsal_QueueCreate(&dtaLibHdl->qHdlCongestData, &sQueueCreatePrms);
    if(dwOsalStatus != OSALSTATUS_SUCCESS)
    {
        phOsal_LogError((const uint8_t*)"DTALib> Unable to create qHdlCongestData Queue");
    }

    phOsal_LogDebug((const uint8_t*)"DTALib> Creating callback Message Handler thread\n");
    dtaLibHdl->blStopCbMsgHandleThrd = FALSE;
    dwOsalStatus = phOsal_ThreadCreate(&dtaLibHdl->dtaThreadHdl,
            phDtaLibi_CbMsgHandleThrd, dtaLibHdl);
    if(dwOsalStatus != OSALSTATUS_SUCCESS)
    {
        phOsal_LogError((const uint8_t*)"DTALib> Unable to create Thread");
    }

    dwOsalStatus = phOsal_ThreadSetPriority(&dtaLibHdl->dtaThreadHdl, 0);
    if(dwOsalStatus != OSALSTATUS_SUCCESS)
    {
        phOsal_LogError((const uint8_t*)"DTALib> Unable to set priority to Thread");
    }
    dwThreadId = phOsal_ThreadGetTaskId();

    phOsal_LogDebug((const uint8_t*)"DTALib> Calling MwIf Init\n");
    dwMwifStatus = phMwIf_Init(&dtaLibHdl->mwIfHdl);
    if(MWIFSTATUS_SUCCESS != dwMwifStatus)
    {
        phOsal_LogInfo((const uint8_t*)"DTALib>MwIf Init Failed !\n");
        return DTASTATUS_FAILED;
    }

    phOsal_LogDebug((const uint8_t*)"DTALib> Calling phMwIf_RegisterCallback\n");
    dwMwifStatus = phMwIf_RegisterCallback(dtaLibHdl->mwIfHdl, dtaLibHdl, phDtaLibi_EvtCb);
    if(MWIFSTATUS_SUCCESS != dwMwifStatus)
    {
        phOsal_LogInfo((const uint8_t*)"DTALib>MwIf Register callback Failed !\n");
        return DTASTATUS_FAILED;
    }
    dtaLibHdl->bDtaInitialized = TRUE;
    LOG_FUNCTION_EXIT;
    return DTASTATUS_SUCCESS;
}

/**
 * De-Initialize DTA Lib
 */
DTASTATUS phDtaLib_DeInit() {
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;
    phDtaLib_sQueueData_t* psQueueData=NULL;
    uint8_t abConfigIDData[8] = {0};
    OSALSTATUS dwOsalStatus = 0;
    LOG_FUNCTION_ENTRY;
    psQueueData = (phDtaLib_sQueueData_t*)malloc(sizeof(phDtaLib_sQueueData_t));
    if(!dtaLibHdl->bDtaInitialized)
    {
        phOsal_LogInfo((const uint8_t*)"DTALib>DTALib already DeInitialized!!.Just returning\n");
        return DTASTATUS_SUCCESS;
    }
#ifdef NFC_NXP_CHIP_PN548AD
    /*FIXME:For PN548 only: CON_DEVICES_LIMIT to be set to 01 and then reset to 03 after Deinit*/
     abConfigIDData[0] = 0x03;
     phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CON_DEVICES_LIMIT, 0x01, abConfigIDData);
#endif

     phOsal_LogDebug((const uint8_t*)"DTALib> Calling MwIf De-Init\n");
     phMwIf_DeInit(dtaLibHdl->mwIfHdl);

    dtaLibHdl->blStopCbMsgHandleThrd = TRUE;
    if (psQueueData)
    {
        /*Pushing Dummy event to exit phDtaLibi_CbMsgHandleThrd thread*/
        psQueueData->uEvtType.eDpEvtType = PHMWIF_THREAD_DELETE_DUMMY_EVT;
    }
    else
    {
        phOsal_LogErrorString((const uint8_t*)"DTALib> :Error:Unable to allocate memory", (const uint8_t*)__FUNCTION__);
        return DTASTATUS_FAILED;
    }
    dwOsalStatus = phOsal_QueuePush(dtaLibHdl->queueHdl, psQueueData, 0);
    if(dwOsalStatus != OSALSTATUS_SUCCESS)
    {
        phOsal_LogErrorString((const uint8_t*)"DTALib> :Unable to Push to Queue", (const uint8_t*)__FUNCTION__);
        dtaLibHdl->blStopCbMsgHandleThrd = FALSE;
        return DTASTATUS_FAILED;
    }
    dwOsalStatus = phOsal_ThreadDelete((void *)dtaLibHdl->dtaThreadHdl);
    if(dwOsalStatus != OSALSTATUS_SUCCESS)
    {
        phOsal_LogDebugString ((const uint8_t*)"DTALib>%s:Unable to Delete Thread ",(const uint8_t*)__FUNCTION__);
        return DTASTATUS_FAILED;
    }
    phOsal_LogDebugString ((const uint8_t*)"DTALib>%s:phDtaLibi_CbMsgHandleThrd Thread delete successful ",(const uint8_t*)__FUNCTION__);

    dwOsalStatus = phOsal_QueueDestroy(dtaLibHdl->queueHdl);
    if(dwOsalStatus != OSALSTATUS_SUCCESS)
    {
        phOsal_LogDebug((const uint8_t*)"DTALib> Unable to Delete Queue\n");
        return DTASTATUS_FAILED;
    }

    dwOsalStatus = phOsal_QueueDestroy(dtaLibHdl->qHdlCongestData);
    if(dwOsalStatus != OSALSTATUS_SUCCESS)
    {
        phOsal_LogDebug((const uint8_t*)"DTALib> Unable to Delete qHdlCongestData Queue\n");
        return DTASTATUS_FAILED;
    }

    dtaLibHdl->bDtaInitialized = FALSE;
    dtaLibHdl->bLlcpInitialized = FALSE;
    LOG_FUNCTION_EXIT;
    return DTASTATUS_SUCCESS;
}

DTASTATUS phDtaLibi_ReInit()
{
    MWIFSTATUS dwMwifStatus = MWIFSTATUS_SUCCESS;
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;
    LOG_FUNCTION_ENTRY;
    dwMwifStatus = phMwIf_DeInit(dtaLibHdl->mwIfHdl);
    if(MWIFSTATUS_SUCCESS != dwMwifStatus)
    {
        phOsal_LogInfo((const uint8_t*)"DTALib>MwIf DeInit Failed !\n");
        return DTASTATUS_FAILED;
    }

    phOsal_LogInfoString((const uint8_t*)"DTALib>Calling:phMwIf_Init from",(const uint8_t*)__FUNCTION__);
    dwMwifStatus = phMwIf_Init(&dtaLibHdl->mwIfHdl);
    if(MWIFSTATUS_SUCCESS != dwMwifStatus)
    {
        phOsal_LogInfo((const uint8_t*)"DTALib>MwIf Init Failed !\n");
        return DTASTATUS_FAILED;
    }

    phOsal_LogInfoString((const uint8_t*)"DTALib>Calling:phMwIf_RegisterCallback from",(const uint8_t*)__FUNCTION__);
    dwMwifStatus = phMwIf_RegisterCallback(dtaLibHdl->mwIfHdl, dtaLibHdl, phDtaLibi_EvtCb);
    if(MWIFSTATUS_SUCCESS != dwMwifStatus)
    {
        phOsal_LogInfo((const uint8_t*)"DTALib>MwIf Register callback Failed !\n");
        return DTASTATUS_FAILED;
    }
    LOG_FUNCTION_EXIT;
    return dwMwifStatus;
}

/**
 * Initilaise NFCEE
 */
DTASTATUS phDtaLib_EeInit(phDtaLib_eNfceeDevType_t DevType)
{
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;

    LOG_FUNCTION_ENTRY;

    phOsal_LogDebug((const uint8_t*)"DTALib> Calling MwIf NFC EE Init");
    phOsal_LogDebugU32h((const uint8_t*)"DTALib> Device Type:",(uint32_t)DevType);
    /*phMwIf_EeInit(dtaLibHdl->mwIfHdl);*/ /*EE Init will be invoked during enable discovery*/

    LOG_FUNCTION_EXIT;
    return DTASTATUS_SUCCESS;
}

/**
 * Apply Required configuration needed to work with P2P with LLCP & SNEP
 */
DTASTATUS phDtaLib_ConfigureP2p(phDtaLib_eP2PType_t p2pType)
{
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;
    LOG_FUNCTION_ENTRY;
    dtaLibHdl->p2pType  = p2pType;
    LOG_FUNCTION_EXIT;
    return DTASTATUS_SUCCESS;
}

/**
 * Enable Discovery
 */
DTASTATUS phDtaLib_EnableDiscovery(phDtaLib_sDiscParams_t* discParams)
{
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;
    phMwIf_sDiscCfgPrms_t sMwIfDiscCfgParams;
    phMwIf_sLlcpSrvrRegnParams_t sLlcpSrvrPrms;
    MWIFSTATUS  dwMwIfStatus;

    LOG_FUNCTION_ENTRY;
    if(!discParams)
    {
        phOsal_LogErrorString((const uint8_t*)"DTALib> :Invalid Params", (const uint8_t*)__FUNCTION__);
        return DTASTATUS_INVALID_PARAM;
    }
    dtaLibHdl->sAppDiscCfgParams = *discParams;
    /*Reset all the disc configuration before updating it with new configuration provided by app*/
    memset(&sMwIfDiscCfgParams,0,sizeof(phMwIf_sDiscCfgPrms_t));
    sMwIfDiscCfgParams.discParams.dwPollP2P    = discParams->dwPollP2P;
    sMwIfDiscCfgParams.discParams.dwPollRdWrt  = discParams->dwPollRdWrt;
    sMwIfDiscCfgParams.discParams.dwListenP2P  = discParams->dwListenP2P;
    sMwIfDiscCfgParams.discParams.dwListenUICC = discParams->dwListenUICC;
    sMwIfDiscCfgParams.discParams.dwListenHCE  = discParams->dwListenHCE;
    sMwIfDiscCfgParams.discParams.dwListenESE  = discParams->dwListenESE;
    phDtaLibi_SetMwIfConfig();

    if((dtaLibHdl->sTestProfile.Pattern_Number == 0x06) ||
       (dtaLibHdl->sTestProfile.Pattern_Number == 0x08) ||
       (dtaLibHdl->sTestProfile.Pattern_Number == 0x0A))
    {
        /*Configure TypeF Polling Bitrate to 424 for pattern 0x06,0x08,0x0A as per DTA Spec*/
        gs_paramBuffer[0] = 0x02;
        phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PF_BIT_RATE, 1, gs_paramBuffer);
        phOsal_LogDebugString ((const uint8_t*)"DTALib>Configured TypeF Poll Bitrate to 424Kbps for pattern number 0x06,0x08,0x09 & 0x0A",
                                (const uint8_t*)__FUNCTION__);
    }
    if(dtaLibHdl->sTestProfile.Pattern_Number == 0x09)
    {
        gs_paramBuffer[0] = 0x00;
        phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PN_NFC_DEP_SPEED, 1, gs_paramBuffer);
    }

    if((dtaLibHdl->sTestProfile.Pattern_Number == 0x0A) ||
       (dtaLibHdl->sTestProfile.Pattern_Number == 0x0B))
    {
        /*Disable Type B Poll for pattern 0x0A & 0x0B as per DTA Spec*/
        sMwIfDiscCfgParams.discParams.dwPollP2P   &= (~DTALIB_RFTECHNOLOGY_MASK_B);
        sMwIfDiscCfgParams.discParams.dwPollRdWrt &= (~DTALIB_RFTECHNOLOGY_MASK_B);
        phOsal_LogDebugString ((const uint8_t*)"DTALib>Disabled Type B polling for pattern number 0x0A & 0x0B",
                                (const uint8_t*)__FUNCTION__);
    }

    if(dtaLibHdl->sTestProfile.Pattern_Number == PHDTALIB_PATTERN_NUM_ANALOG_TEST)
    {
        /*For Analog Mode, Enable Poll[A,B,F(212&424)] & Listen[F-P2P,CE-UICC Type A&B].
         * Only Do bitrate configuration using Setconfig in this block.
         * Other configuration are assumed to be provided in Discovery Config params*/

        /*Configure TypeF Polling with Bitrate 212 as well as 424 using Proprietary value 0x80*/
        gs_paramBuffer[0] = 0x80;
        phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PF_BIT_RATE, 1, gs_paramBuffer);
        phOsal_LogDebugString ((const uint8_t*)"DTALib> :Analog Test Mode",(const uint8_t*)__FUNCTION__);
    }

    if (((dtaLibHdl->sTestProfile.Pattern_Number == PHDTALIB_LLCP_CO_SET_SAP_OR_CL) ||
        (dtaLibHdl->sTestProfile.Pattern_Number == PHDTALIB_LLCP_CO_SET_NAME_OR_CL) ||
        (dtaLibHdl->sTestProfile.Pattern_Number == PHDTALIB_LLCP_CO_SET_SNL_OR_CL)) &&
        (dtaLibHdl->bLlcpInitialized != TRUE))
    {
        uint8_t abConfigIDData[8]={0};
        phMwIf_sLlcpInitParams_t sLlcpInitPrms;
        abConfigIDData[0] = 0x0A;
        phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_LN_WT, 0x01, abConfigIDData);
        abConfigIDData[0] = 0x0F;
        phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_NFCDEP_OP, 0x01, abConfigIDData);
        phOsal_LogDebugString ((const uint8_t*)"DTALib> :LLCP Init",(const uint8_t*)__FUNCTION__);
        dtaLibHdl->bIsLlcpCoRemoteServerLinkCongested               = FALSE;
        sLlcpInitPrms.sGenBytesInitiator.pbBuff       = (uint8_t *)PHDTALIB_LLCP_GEN_BYTES_INITIATOR;
        sLlcpInitPrms.sGenBytesInitiator.dwBuffLength = PHDTALIB_LLCP_GEN_BYTES_LEN_INITIATOR;
        sLlcpInitPrms.sGenBytesTarget.pbBuff          = (uint8_t *)PHDTALIB_LLCP_GEN_BYTES_TARGET;
        sLlcpInitPrms.sGenBytesTarget.dwBuffLength    = PHDTALIB_LLCP_GEN_BYTES_LEN_TARGET;
        sLlcpInitPrms.pfMwIfLlcpCb                    = phDtaLibi_LlcpEvtCb;
        sLlcpInitPrms.pvApplHdl                       = (void*)dtaLibHdl;
        dwMwIfStatus = phMwIf_LlcpInit(dtaLibHdl->mwIfHdl,
                                       &sLlcpInitPrms);
        if(dwMwIfStatus != MWIFSTATUS_SUCCESS)
        {
            phOsal_LogErrorString((const uint8_t*)"DTALib> :Error in Initializing LLCP",(const uint8_t*)__FUNCTION__);
            return MWIFSTATUS_FAILED;
        }

        phOsal_LogDebug((const uint8_t*)"DTALib> Registering Conn Oriented Inbound Server \n");
        sLlcpSrvrPrms.bSap                  = PHDTALIB_SAP_IUT_CO_IN_DEST;
        sLlcpSrvrPrms.pbServiceName         = (uint8_t *)gs_abInboundCoService;
        sLlcpSrvrPrms.pfMwIfLlcpServerCb    = phDtaLibi_LlcpEvtCb;
        sLlcpSrvrPrms.pvApplHdl             = dtaLibHdl;
        dwMwIfStatus = phMwIf_LlcpRegisterServerConnOriented(dtaLibHdl->mwIfHdl,
                                                             &sLlcpSrvrPrms,
                                                             &(dtaLibHdl->pvCOServerHandle));

        PH_ON_ERROR_RETURN(MWIFSTATUS_SUCCESS,dwMwIfStatus,
                (const uint8_t*)"DTALib> Error in registering Server");

        phOsal_LogDebug((const uint8_t*)"DTALib> Registering Conn Less Inbound Server \n");
        sLlcpSrvrPrms.bSap                  = PHDTALIB_SAP_IUT_CL_IN_DEST;
        sLlcpSrvrPrms.pbServiceName         = (uint8_t *)gs_abInboundClService;
        sLlcpSrvrPrms.pfMwIfLlcpServerCb    = phDtaLibi_LlcpEvtCb;
        sLlcpSrvrPrms.pvApplHdl             = dtaLibHdl;
        dwMwIfStatus = phMwIf_LlcpRegisterServerConnLess(dtaLibHdl->mwIfHdl,
                                                          &sLlcpSrvrPrms,
                                                          &(dtaLibHdl->pvCLServerHandle));
        dtaLibHdl->bLlcpInitialized = TRUE;
    }
    phOsal_LogDebugU32h((const uint8_t*)"DEBUG> DTALib> :ESE Enabled",discParams->dwListenHCE);
    if(discParams->dwListenHCE)
    {/*Conigure Card Emulation from Host*/
        phOsal_LogDebugString ((const uint8_t*)"DTALib> :HCE Enabled",(const uint8_t*)__FUNCTION__);

/***********************HCE(A & B) Configuration************************/
        if( (discParams->dwListenHCE & DTALIB_RFTECHNOLOGY_MASK_A) || (discParams->dwListenHCE & DTALIB_RFTECHNOLOGY_MASK_B)){
                dwMwIfStatus = phMwIf_HceInit(dtaLibHdl->mwIfHdl);
                if(dwMwIfStatus != MWIFSTATUS_SUCCESS)
                {
                        phOsal_LogErrorString((const uint8_t*)"DTALib> :Error in Initializing HCE",(const uint8_t*)__FUNCTION__);
                        return MWIFSTATUS_FAILED;
                }
                dwMwIfStatus = phMwIf_HceConfigure(dtaLibHdl->mwIfHdl, discParams->dwListenHCE);
                if(dwMwIfStatus != MWIFSTATUS_SUCCESS)
                {
                        phOsal_LogErrorString((const uint8_t*)"DTALib> :Error in configuring HCE",(const uint8_t*)__FUNCTION__);
                        return MWIFSTATUS_FAILED;
                }
        }
/***********************HCE(A & B) Configuration END************************/

/***********************HCE(F) Configuration************************/
        if(discParams->dwListenHCE & DTALIB_RFTECHNOLOGY_MASK_F){
            if(phMwIf_HceFInit(dtaLibHdl->mwIfHdl) != MWIFSTATUS_SUCCESS){
                phOsal_LogErrorString((const uint8_t*)"DTALib> :Error in Initializing HCE",(const uint8_t*)__FUNCTION__);
                return MWIFSTATUS_FAILED;
            }
            if((phMwIf_HceFConfigure(dtaLibHdl->mwIfHdl,discParams->dwListenHCE)) != MWIFSTATUS_SUCCESS){
                phOsal_LogErrorString((const uint8_t*)"DTALib> :Error in configuring HCE",(const uint8_t*)__FUNCTION__);
                return MWIFSTATUS_FAILED;
            }
        }
/***********************HCE(F) Configuration END************************/
    }

    if(discParams->dwListenUICC)
    {/*Configure Card Emulation from UICC*/
        phOsal_LogDebugString ((const uint8_t*)"DTALib> :UICC Enabled",(const uint8_t*)__FUNCTION__);
        dwMwIfStatus = phMwIf_EeInit(dtaLibHdl->mwIfHdl,PHMWIF_UICC_CE);
        if(dwMwIfStatus != MWIFSTATUS_SUCCESS)
        {
            phOsal_LogErrorString((const uint8_t*)"DTALib> :Error in Initializing UICC",(const uint8_t*)__FUNCTION__);
            return MWIFSTATUS_FAILED;
        }
        dwMwIfStatus = phMwIf_EeConfigure(dtaLibHdl->mwIfHdl,PHMWIF_UICC_CE,
                                          discParams->dwListenUICC);
        if(dwMwIfStatus != MWIFSTATUS_SUCCESS)
        {
            phOsal_LogErrorString((const uint8_t*)"DTALib> :Error in configuring UICC",(const uint8_t*)__FUNCTION__);
            return MWIFSTATUS_FAILED;
        }
    }

    if(discParams->dwListenESE)
    {/*Configure  Card Emulation from ESE*/
        phOsal_LogDebugString ((const uint8_t*)"DTALib> :ESE Enabled",(const uint8_t*)__FUNCTION__);
        dwMwIfStatus = phMwIf_EeInit(dtaLibHdl->mwIfHdl,PHMWIF_SE_CE);
        if(dwMwIfStatus != MWIFSTATUS_SUCCESS)
        {
            phOsal_LogErrorString((const uint8_t*)"DTALib> :Error in Initializing ESE",(const uint8_t*)__FUNCTION__);
            return MWIFSTATUS_FAILED;
        }
        dwMwIfStatus = phMwIf_EeConfigure(dtaLibHdl->mwIfHdl,PHMWIF_SE_CE,
                                          discParams->dwListenESE);
        if(dwMwIfStatus != MWIFSTATUS_SUCCESS)
        {
            phOsal_LogErrorString((const uint8_t*)"DTALib> :Error in configuring ESE",(const uint8_t*)__FUNCTION__);
            return MWIFSTATUS_FAILED;
        }
    }

    phOsal_LogDebug((const uint8_t*)"DTALib> Calling Enable Discovery\n");
    dwMwIfStatus = phMwIf_EnableDiscovery(dtaLibHdl->mwIfHdl, &sMwIfDiscCfgParams);
    if(dwMwIfStatus != MWIFSTATUS_SUCCESS)
    {
        phOsal_LogErrorString((const uint8_t*)"DTALib> :Error in Enabling Discovery",(const uint8_t*)__FUNCTION__);
        return DTASTATUS_FAILED;
    }

    dtaLibHdl->sPrevMwIfDiscCfgParams = sMwIfDiscCfgParams;
    LOG_FUNCTION_EXIT;
    return DTASTATUS_SUCCESS;
}

/**
 * Disable Discovery
 */
DTASTATUS phDtaLib_DisableDiscovery() {
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;
    MWIFSTATUS  dwMwIfStatus = MWIFSTATUS_SUCCESS;
    LOG_FUNCTION_ENTRY;

    if(dtaLibHdl->sAppDiscCfgParams.dwListenHCE)
    {
        dwMwIfStatus |= phMwIf_HceDeInit(dtaLibHdl->mwIfHdl);
    }
    if(dtaLibHdl->sAppDiscCfgParams.dwListenHCE & DTALIB_RFTECHNOLOGY_MASK_F)
    {
        dwMwIfStatus |= phMwIf_HceFDeInit(dtaLibHdl->mwIfHdl);
    }
    if(dtaLibHdl->sAppDiscCfgParams.dwListenUICC ||
       dtaLibHdl->sAppDiscCfgParams.dwListenESE)
    {
        dwMwIfStatus |= phMwIf_EeDeInit(dtaLibHdl->mwIfHdl);
    }

    phOsal_LogDebug((const uint8_t*)"DTALib> Calling MwIf Disable Discovery\n");
    dwMwIfStatus |= phMwIf_DisableDiscovery(dtaLibHdl->mwIfHdl);
    if(dwMwIfStatus !=  MWIFSTATUS_SUCCESS)
    {
        phOsal_LogError((const uint8_t*)"DTALib> DTA may not exit Properly...\n");
    }
    if(dtaLibHdl->bLlcpInitialized == TRUE)
    {
        dwMwIfStatus = phDtaLibi_ReInit();
        if(dwMwIfStatus !=  MWIFSTATUS_SUCCESS)
        {
            phOsal_LogError((const uint8_t*)"DTALib> LLCP DeInitialization is UnSuccessfull...\n");
        }
        else
        {
            dtaLibHdl->bLlcpInitialized = FALSE;
        }
    }
    LOG_FUNCTION_EXIT;
    return DTASTATUS_SUCCESS;
}

/**
 * Get MW, FW and DTA LIB version
 */
DTASTATUS phDtaLib_GetIUTInfo(phDtaLib_sIUTInfo_t* psIUTInfo) {
    phDtaLib_sHandle_t*   dtaLibHdl = &g_DtaLibHdl;
    phMwIf_sVersionInfo_t sVersionInfo;
    LOG_FUNCTION_ENTRY;

    phOsal_LogDebugString((const uint8_t*)"DTALib>Version:",(const uint8_t*)DTALIB_VERSION_STR);
    phOsal_LogDebug((const uint8_t*)"DTALib> Calling MwIf Version Info\n");
    phMwIf_VersionInfo(dtaLibHdl->mwIfHdl, &sVersionInfo);
    psIUTInfo->mwVerMajor = sVersionInfo.mwVerMajor;
    psIUTInfo->mwVerMinor = sVersionInfo.mwVerMinor;
    psIUTInfo->fwVerMajor = sVersionInfo.fwVerMajor;
    psIUTInfo->fwVerMinor = sVersionInfo.fwVerMinor;
    psIUTInfo->dtaLibVerMajor = DTALIBVERSION_MAJOR;
    psIUTInfo->dtaLibVerMinor = DTALIBVERSION_MINOR;

    LOG_FUNCTION_EXIT;
    return DTASTATUS_SUCCESS;
}

/**
 * Set Test Profile. Like pattern number, test case ID....
 */
DTASTATUS phDtaLib_SetTestProfile(phDtaLib_sTestProfile_t TestProfile) {
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;
    LOG_FUNCTION_ENTRY;
    dtaLibHdl->sTestProfile = TestProfile;
    LOG_FUNCTION_EXIT;
    return DTASTATUS_SUCCESS;
}

/**
 * DTA Library call back events
 */
void phDtaLibi_EvtCb(void* mwIfHandle,
                     void* applHdl,
                     phMWIf_eEvtType_t eEvtType,
                     phMWIf_uEvtInfo_t* puEvtData)
{
    OSALSTATUS dwOsalStatus = 0;
    phDtaLib_sHandle_t *dtaLibHdl = (phDtaLib_sHandle_t *) applHdl;
    phDtaLib_sQueueData_t* psQueueData=NULL;
    LOG_FUNCTION_ENTRY;
    phOsal_LogDebugU32h((const uint8_t*)"DTALib> :DTA Event cb Handle=",(size_t)(mwIfHandle));

    /*Prepare Data to push to Queue*/
    psQueueData = (phDtaLib_sQueueData_t*)malloc(sizeof(phDtaLib_sQueueData_t));
    if (psQueueData)
    {
        psQueueData->uEvtType.eDpEvtType = eEvtType;
        if(puEvtData)
            psQueueData->uEvtInfo.uDpEvtInfo = *puEvtData;
    }
    else
    {
        phOsal_LogErrorString((const uint8_t*)"DTALib> :Error:Unable to allocate memory", (const uint8_t*)__FUNCTION__);
        return;
    }
    switch (eEvtType) {
    case PHMWIF_CE_DATA_EVT:
        if(!puEvtData->sData.pvDataBuf)
        {
            phOsal_LogErrorString((const uint8_t*)"DTALib>: Invalid Data Recvd in P2P",(const uint8_t*)__FUNCTION__);
            return;
        }

        phOsal_LogDebugString((const uint8_t*)"DTALib> : ", (const uint8_t*)__FUNCTION__);
        phOsal_LogDebugU32h((const uint8_t*)"Allocating memory for CE data Size=0x", puEvtData->sData.dwSize);
        psQueueData->uEvtInfo.uDpEvtInfo.sData.pvDataBuf = malloc(puEvtData->sData.dwSize);
        if(!psQueueData->uEvtInfo.uDpEvtInfo.sData.pvDataBuf)
        {
            phOsal_LogErrorString((const uint8_t*)"DTALib> : Unable to allocate memory for data", (const uint8_t*)__FUNCTION__);
            free(psQueueData);
            return;
        }
        memcpy(psQueueData->uEvtInfo.uDpEvtInfo.sData.pvDataBuf,puEvtData->sData.pvDataBuf,puEvtData->sData.dwSize );
        psQueueData->uEvtInfo.uDpEvtInfo.sData.dwSize = puEvtData->sData.dwSize;
        break;
    case PHMWIF_T1T_TAG_ACTIVATED_EVT:
        break;
    case PHMWIF_T2T_TAG_ACTIVATED_EVT:
        break;
    case PHMWIF_T3T_TAG_ACTIVATED_EVT:
        break;
    case PHMWIF_ISODEP_ACTIVATED_EVT:
        break;
    case PHMWIF_NFCDEP_ACTIVATED_EVT:
        break;
    case PHMWIF_CE_ACTIVATED_EVT:
        break;
    case PHMWIF_CE_NDEF_WRITE_START_EVT:
        break;
    case PHMWIF_CE_NDEF_WRITE_CPLT_EVT:
        break;
    case PHMWIF_DEACTIVATED_EVT:
        break;
    default:
        phOsal_LogErrorString((const uint8_t*)"DTALib> : Error Callback event not handled:",(const uint8_t*)__FUNCTION__);
        free(psQueueData);
        return;
    }

    dwOsalStatus = phOsal_QueuePush(dtaLibHdl->queueHdl, psQueueData, 0);
    if(dwOsalStatus != OSALSTATUS_SUCCESS)
        phOsal_LogErrorString((const uint8_t*)"DTALib> :Unable to Push to Queue", (const uint8_t*)__FUNCTION__);
    else
    {
        phOsal_LogDebugString((const uint8_t*)"DTALib> : ", (const uint8_t*)__FUNCTION__);
        phOsal_LogDebugU32h((const uint8_t*)"Pushed an Object of evttype=0x", eEvtType);
    }
    LOG_FUNCTION_EXIT;
}

/**
 * Call for Middleware interface NDEF Check
 */
MWIFSTATUS phDtaLibi_CheckNDEF(phMwIf_sNdefDetectParams_t* psTagOpsParams)
{
    MWIFSTATUS dwMwIfStatus = 0;
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;

    LOG_FUNCTION_ENTRY;
    phOsal_LogDebug((const uint8_t*)"DTALib> Calling MwIf Tag operation:checkNdef");
    dwMwIfStatus = phMwIf_TagOperation(dtaLibHdl->mwIfHdl,
                                        PHMWIF_TAGOP_CHECK_NDEF,
                                        (void *)psTagOpsParams);
    if(dwMwIfStatus != MWIFSTATUS_SUCCESS)
    {
        phOsal_LogErrorString((const uint8_t*)"DTALib::Error in phMwIf_CheckNdef", (const uint8_t*)__FUNCTION__);
        phOsal_LogErrorU32h((const uint8_t*)"status=0x",dwMwIfStatus);
        return dwMwIfStatus;
    }

    LOG_FUNCTION_EXIT;
    return dwMwIfStatus;
}

/**
 * NDEF Read
 */
DTASTATUS phDtaLibi_ReadNDEF(phMwIf_uTagOpsParams_t* psTagOpsParams)
{
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;
    MWIFSTATUS dwMwIfStatus = 0;
    LOG_FUNCTION_ENTRY;
    dwMwIfStatus = phMwIf_TagOperation(dtaLibHdl->mwIfHdl,
                                            PHMWIF_TAGOP_READ_NDEF,
                                            psTagOpsParams);
    if (dwMwIfStatus != MWIFSTATUS_SUCCESS)
    {
        phOsal_LogErrorString((const uint8_t*)"DTALib>:Error in phMwIf_TagOperation", (const uint8_t*)__FUNCTION__);
        phOsal_LogErrorU32h((const uint8_t*)"status=0x",dwMwIfStatus);
        return DTASTATUS_FAILED;
    }
    LOG_FUNCTION_EXIT;
    return DTASTATUS_SUCCESS;
}

/**
 * Set the Tag Read Only
 */
DTASTATUS phDtaLibi_SetTagReadOnly(phMwIf_uTagOpsParams_t* psTagOpsParams)
{
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;
    MWIFSTATUS dwMwIfStatus = 0;
    LOG_FUNCTION_ENTRY;
    dwMwIfStatus = phMwIf_TagOperation(dtaLibHdl->mwIfHdl,
                                       PHMWIF_TAGOP_SET_READONLY,
                                       psTagOpsParams);
    if (dwMwIfStatus != MWIFSTATUS_SUCCESS)
    {
        phOsal_LogErrorString((const uint8_t*)"DTALib>:Error in Setting Tag read only:", (const uint8_t*)__FUNCTION__);
        phOsal_LogErrorU32h((const uint8_t*)"status=0x",dwMwIfStatus);
        return DTASTATUS_FAILED;
    }
    LOG_FUNCTION_EXIT;
    return DTASTATUS_SUCCESS;
}

/**
 * NDEF Write
 */
DTASTATUS phDtaLibi_WriteNDEF(phMwIf_uTagOpsParams_t* psTagOpsParams)
{
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;
    MWIFSTATUS dwMwIfStatus = 0;
    LOG_FUNCTION_ENTRY;
    dwMwIfStatus = phMwIf_TagOperation(dtaLibHdl->mwIfHdl,
                                       PHMWIF_TAGOP_WRITE_NDEF,
                                       psTagOpsParams);
    if (dwMwIfStatus != MWIFSTATUS_SUCCESS)
    {
        phOsal_LogErrorString((const uint8_t*)"DTALib>:Error in writing NDEF:", (const uint8_t*)__FUNCTION__);
        phOsal_LogErrorU32h((const uint8_t*)"status=0x",dwMwIfStatus);
        return DTASTATUS_FAILED;
    }
    LOG_FUNCTION_EXIT;
    return DTASTATUS_SUCCESS;
}

/**
 * Thread to handle DTA functionalities separately
 */
void phDtaLibi_CbMsgHandleThrd(void *param) {
    phDtaLib_sHandle_t *dtaLibHdl = (phDtaLib_sHandle_t *) param;
    OSALSTATUS dwOsalStatus = 0;
    MWIFSTATUS dwMwIfStatus = 0;
    DTASTATUS  dwDtaStatus=0;
    phMwIf_eProtocolType_t eProtocolype;
    phMWIf_eDiscType_t eDiscType;
    phMWIf_eDeactivateType_t eDeactType;
    BOOLEAN   bDiscStartReqd = TRUE;
    BOOLEAN   bDiscStopReqd  = TRUE;
    phDtaLib_sQueueData_t* psQueueData=NULL;
    phdtaLib_sEvtData_t*  psEvtData=NULL;
    uint32_t dwEventType = 0;
    LOG_FUNCTION_ENTRY;
    while (1) {
        bDiscStartReqd = TRUE;
        bDiscStopReqd  = TRUE;
        phOsal_LogDebugString((const uint8_t*)"DTALib> : waiting for data", (const uint8_t*)__FUNCTION__);
        dwOsalStatus = phOsal_QueuePull(dtaLibHdl->queueHdl,(void**) &psQueueData, 0);
        if (dwOsalStatus != 0) {
            phOsal_LogErrorString((const uint8_t*)"DTALib> : Exiting-QueuPull ", (const uint8_t*)__FUNCTION__);
            phOsal_LogErrorU32h((const uint8_t*)"Status = ", dwOsalStatus);
            return;
        }
        if((dtaLibHdl->blStopCbMsgHandleThrd = TRUE) && (psQueueData->uEvtType.eDpEvtType == PHMWIF_THREAD_DELETE_DUMMY_EVT))
        {
            phOsal_LogDebugString((const uint8_t*)"DTALib> : Stopping phDtaLibi_CbMsgHandleThrd thread", (const uint8_t*)__FUNCTION__);
            break;
        }
        phOsal_LogDebugString((const uint8_t*)"DTALib> : Received  an object", (const uint8_t*)__FUNCTION__);
        phOsal_LogDebugU32h((const uint8_t*)"Status = ", dwOsalStatus);
        if(dtaLibHdl->dtaApplCb != NULL)
        {
            dtaLibHdl->dtaApplCb(dtaLibHdl->dtaApplHdl, PHDTALIB_TEST_CASE_EXEC_STARTED, psEvtData);
        }
        dwEventType = psQueueData->uEvtType.eDpEvtType;
        switch (dwEventType)
        {
        case PHMWIF_T1T_TAG_ACTIVATED_EVT:
            phDtaLibi_T1TOperations(dtaLibHdl->sTestProfile);
            free(psQueueData);
            break;
        case PHMWIF_T2T_TAG_ACTIVATED_EVT:
            phDtaLibi_T2TOperations(dtaLibHdl->sTestProfile);
            bDiscStartReqd = TRUE;
            bDiscStopReqd  = FALSE;
            free(psQueueData);
            break;
        case PHMWIF_T3T_TAG_ACTIVATED_EVT:
         {
             phDtaLibi_T3TOperations(dtaLibHdl->sTestProfile,
                                    &(psQueueData->uEvtInfo.uDpEvtInfo.sActivationPrms),&bDiscStartReqd,&bDiscStopReqd);
            free(psQueueData);
            break;
        }
        case PHMWIF_ISODEP_ACTIVATED_EVT:
            phDtaLibi_T4TOperations(dtaLibHdl->sTestProfile,&bDiscStartReqd,&bDiscStopReqd);
            free(psQueueData);
            break;
        case PHMWIF_NFCDEP_ACTIVATED_EVT:
            if((dtaLibHdl->p2pType == PHDTALIB_P2P_LLCP_WITHOUT_CONN_PARAMS)||
               (dtaLibHdl->p2pType == PHDTALIB_P2P_LLCP_WITH_CONN_PARAMS))
            {/*Dont process this if its LLCP or SNEP as LLCP gets activated later*/
                phOsal_LogDebugString((const uint8_t*)"DTALib>:PHMWIF_NFCDEP_ACTIVATED_EVT not processed as its LLCP mode ", (const uint8_t*)__FUNCTION__);
                bDiscStartReqd = FALSE;
                bDiscStopReqd  = FALSE;
                break;
            }
            eDiscType = psQueueData->uEvtInfo.uDpEvtInfo.sActivationPrms.eActivatedRfTechTypeAndMode;
            if ((eDiscType == PHMWIF_DISCOVERY_TYPE_POLL_A)
                    || (eDiscType == PHMWIF_DISCOVERY_TYPE_POLL_F))
            {
                phOsal_LogDebug((const uint8_t*)"DTALib> We are Initiator now !! \n");
                phDtaLibi_NfcdepTargetOperations(&bDiscStartReqd,&bDiscStopReqd);
            } /* Handle remote device == Initiator, for Listen mode test cases */
            else if ((eDiscType == PHMWIF_DISCOVERY_TYPE_LISTEN_A)
                    || (eDiscType == PHMWIF_DISCOVERY_TYPE_LISTEN_F))
            {
                phOsal_LogDebug((const uint8_t*)"DTALib-DEP> We are Target now !! \n");
                phDtaLibi_NfcdepInitiatorOperations();
            } else
            {
                phOsal_LogError((const uint8_t*)"DTALib> Invalid Discovery Type received");
            }
            free(psQueueData);
            break;
        case PHMWIF_DEACTIVATED_EVT:
            eDeactType = psQueueData->uEvtInfo.uDpEvtInfo.eDeactivateType;
            free(psQueueData);
            /*FIXME: LLCP tests require Stop/Start after each TC execution and deactivation,
             * else we get RFlinkloss and RFStuck issue*/
            if ((dtaLibHdl->sTestProfile.Pattern_Number == PHDTALIB_LLCP_CO_SET_SAP_OR_CL) ||
                (dtaLibHdl->sTestProfile.Pattern_Number == PHDTALIB_LLCP_CO_SET_NAME_OR_CL) ||
                (dtaLibHdl->sTestProfile.Pattern_Number == PHDTALIB_LLCP_CO_SET_SNL_OR_CL) )
            {
                bDiscStartReqd = TRUE;
                bDiscStopReqd  = TRUE;
            }
            else
            {
                bDiscStartReqd = FALSE;
                bDiscStopReqd  = FALSE;
            }
            break;

        case PHMWIF_CE_ACTIVATED_EVT:
            eProtocolype = psQueueData->uEvtInfo.uDpEvtInfo.sActivationPrms.eProtocolType;
            phOsal_LogDebugU32h((const uint8_t*)"DTALib> Card emulation activated dwProtocol = ",eProtocolype);
            if(eProtocolype == PHMWIF_PROTOCOL_ISO_DEP)
            {
                phOsal_LogDebug((const uint8_t*)"DTALib>HCE with ISO-DEP activated");
                dwDtaStatus = phDtaLibi_HceOperations();
                if(dwDtaStatus != DTASTATUS_SUCCESS)
                    phOsal_LogError((const uint8_t*)"DTALib>Unable to complete HCE Operations");
            }
            else if(eProtocolype == PHMWIF_PROTOCOL_UNKNOWN)
            {
                phOsal_LogDebug((const uint8_t*)"DTALib>UICC with PROTOCOL UNKNOWN");
                dwDtaStatus = phDtaLibi_UICCOperations();
                if(dwDtaStatus != DTASTATUS_SUCCESS)
                    phOsal_LogError((const uint8_t*)"DTALib>Unable to complete UICC Operations");
            }
            else if(eProtocolype == PHMWIF_PROTOCOL_T3T)
            {
                phOsal_LogDebug((const uint8_t*)"DTALib>HCE with T3T activated");
                dwDtaStatus = phDtaLibi_HceFOperations();
                if(dwDtaStatus != DTASTATUS_SUCCESS)
                    phOsal_LogError((const uint8_t*)"DTALib>Unable to complete HCE Operations");
            }
            else
            {
                phOsal_LogError((const uint8_t*)"DTALib>Invalid Protocol in HCE. Can't be handled");
            }
            free(psQueueData);
            break;
        case PHMWIF_CE_DATA_EVT:
            /*CE DATA EVT will be read internal to HCE operations.So no
             * processing needed here*/
            phOsal_LogDebug((const uint8_t*)"DTALib>CE_DATA_EVT not processed");
            bDiscStartReqd = FALSE;
            bDiscStopReqd  = FALSE;
            break;
        case PHMWIF_LLCP_ACTIVATED_EVT:
            phOsal_LogDebug((const uint8_t*)"DTALib>LLCP ACTIVATED EVT not processed");
            bDiscStartReqd = FALSE;
            bDiscStopReqd  = FALSE;
            break;
        case PHMWIF_LLCP_DEACTIVATED_EVT:
            /*Disc Stop/Start is done in device deactivated event*/
            phDtaLibi_LlcpHandleDeactivatedEvent();
            bDiscStartReqd = FALSE;
            bDiscStopReqd  = FALSE;
            free(psQueueData);
            phOsal_LogDebug((const uint8_t*)"DTALib>LLCP DEACTIVATED EVT not processed");
            break;
        case PHMWIF_LLCP_SERVER_CONN_REQ_EVT:
            phOsal_LogDebug((const uint8_t*)"DTALib>PHMWIF_LLCP_SERVER_CONN_REQ_EVT");
            phDtaLibi_LlcpOperations(&dtaLibHdl->sTestProfile,
                                     psQueueData->uEvtType.eLlcpEvtType,
                                     &psQueueData->uEvtInfo.uLlcpEvtInfo);
            bDiscStartReqd = FALSE;
            bDiscStopReqd  = FALSE;
            free(psQueueData);
            break;
        case PHMWIF_LLCP_SERVER_CONN_LESS_DATA_EVT:
            phOsal_LogDebug((const uint8_t*)"DTALib>PHMWIF_LLCP_SERVER_CONN_LESS_DATA_EVT");
            phDtaLibi_LlcpConnLessOperations(&dtaLibHdl->sTestProfile,
                                             psQueueData->uEvtType.eLlcpEvtType,
                                             &psQueueData->uEvtInfo.uLlcpEvtInfo);
            bDiscStartReqd = FALSE;
            bDiscStopReqd  = FALSE;
            free(psQueueData);
            break;
        case PHMWIF_LLCP_SERVER_CONN_ORIENTED_DATA_EVT:
            phOsal_LogDebug((const uint8_t*)"DTALib>PHMWIF_LLCP_SERVER_CONN_ORIENTED_DATA_EVT");
            phDtaLibi_LlcpOperations(&dtaLibHdl->sTestProfile,
                                     psQueueData->uEvtType.eLlcpEvtType,
                                     &psQueueData->uEvtInfo.uLlcpEvtInfo);

            bDiscStartReqd = FALSE;
            bDiscStopReqd  = FALSE;
            free(psQueueData);
            break;
        case PHMWIF_LLCP_P2P_LINK_UNCONGESTED_EVT:
            phOsal_LogDebug((const uint8_t*)"DTALib>PHMWIF_LLCP_P2P_LINK_UNCONGESTED_EVT");
            phDtaLibi_LlcpOperations(&dtaLibHdl->sTestProfile,
                                     psQueueData->uEvtType.eLlcpEvtType,
                                     &psQueueData->uEvtInfo.uLlcpEvtInfo);

            bDiscStartReqd = FALSE;
            bDiscStopReqd  = FALSE;
            free(psQueueData);
            break;
        case PHMWIF_LLCP_CO_REMOTE_CLIENT_DICONNECTED_EVT:
            phOsal_LogDebug((const uint8_t*)"DTALib>PHMWIF_LLCP_CO_REMOTE_CLIENT_DICONNECTED_EVT");
            phDtaLibi_LlcpHandleCoClientDisconnect(&dtaLibHdl->sTestProfile,
                                                   psQueueData->uEvtType.eLlcpEvtType,
                                                   &psQueueData->uEvtInfo.uLlcpEvtInfo);
            bDiscStartReqd = FALSE;
            bDiscStopReqd  = FALSE;
            free(psQueueData);
            break;
        default:
            phOsal_LogError((const uint8_t*)"DTALib> Invalid event received");
            bDiscStartReqd = FALSE;
            bDiscStopReqd  = FALSE;
            break;
        }


        phOsal_LogDebugString((const uint8_t*)"DTALib> : ", (const uint8_t*)__FUNCTION__);
        phOsal_LogDebugU32h((const uint8_t*)"DiscStopReqd= ", bDiscStopReqd);
        phOsal_LogDebugU32h((const uint8_t*)"DiscStartReqd= ", bDiscStartReqd);

        if(bDiscStopReqd)
        {
            dwMwIfStatus = phMwIf_DisableDiscovery(dtaLibHdl->mwIfHdl);
            if (dwMwIfStatus != MWIFSTATUS_SUCCESS)
            {
                phOsal_LogErrorString((const uint8_t*)"DTALib> :Could not stop discovery", (const uint8_t*)__FUNCTION__);
                //break; DTALib Message Handler Thread should never exit
            }
        }

        if(bDiscStartReqd)
        {
            dwMwIfStatus = phMwIf_EnableDiscovery(dtaLibHdl->mwIfHdl,
                                                 &dtaLibHdl->sPrevMwIfDiscCfgParams);
            if (dwMwIfStatus != MWIFSTATUS_SUCCESS)
            {
                phOsal_LogErrorString((const uint8_t*)"DTALib> :Could not restart discovery", (const uint8_t*)__FUNCTION__);
                //break;DTALib Message Handler Thread should never exit
            }
        }
        if(dtaLibHdl->dtaApplCb != NULL)
        {
            dtaLibHdl->dtaApplCb(dtaLibHdl->dtaApplHdl, PHDTALIB_TEST_CASE_EXEC_COMPLETED, psEvtData);
        }
    }
    if(dtaLibHdl->dtaApplCb != NULL)
    {
        dtaLibHdl->dtaApplCb(dtaLibHdl->dtaApplHdl, PHDTALIB_TEST_EXEC_DEINIT, psEvtData);
    }
    LOG_FUNCTION_EXIT;
    return;
}

/**
 * DTA Library call back events for LLCP
 */
void phDtaLibi_LlcpEvtCb (void*                   pvMwIfHandle,
                          void*                   pvApplHdl,
                          phMWIf_eLlcpEvtType_t   eLlcpEvtType,
                          phMwIf_uLlcpEvtInfo_t*  puLlcpEvtInfo)
{
    OSALSTATUS dwOsalStatus = 0;
    phDtaLib_sQueueData_t* psQueueData = NULL;
    phDtaLib_sHandle_t *dtaLibHdl = (phDtaLib_sHandle_t *) pvApplHdl;
    LOG_FUNCTION_ENTRY;
    phOsal_LogDebugU32h((const uint8_t*)"DTALib> :Llcp Event cb Handle=",(size_t)pvMwIfHandle);

    /*Prepare Data to push to Queue*/
    psQueueData = (phDtaLib_sQueueData_t*)malloc(sizeof(phDtaLib_sQueueData_t));
    if (psQueueData)
    {
        psQueueData->uEvtType.eLlcpEvtType = eLlcpEvtType;
        if(puLlcpEvtInfo)
            psQueueData->uEvtInfo.uLlcpEvtInfo = *puLlcpEvtInfo;
    }
    else
    {
        phOsal_LogErrorString((const uint8_t*)"DTALib> :Error:Unable to allocate memory", (const uint8_t*)__FUNCTION__);
        return;
    }

    switch(eLlcpEvtType)
    {
    case PHMWIF_LLCP_ACTIVATED_EVT:
        dtaLibHdl->bIsLlcpCoRemoteClientDisconnected = FALSE;
        break;

    case PHMWIF_LLCP_DEACTIVATED_EVT:
        break;

    case PHMWIF_LLCP_SERVER_CONN_REQ_EVT:
        break;

    case PHMWIF_LLCP_SERVER_CONN_LESS_DATA_EVT:
        break;
    case PHMWIF_LLCP_SERVER_CONN_ORIENTED_DATA_EVT:
        break;
    case PHMWIF_LLCP_CO_REMOTE_CLIENT_DICONNECTED_EVT:
        dtaLibHdl->bIsLlcpCoRemoteClientDisconnected = TRUE;
        break;
    case PHMWIF_LLCP_P2P_LINK_CONGESTED_EVT:
        dtaLibHdl->bIsLlcpCoRemoteServerLinkCongested = TRUE;
        dtaLibHdl->uLlcpLinkHdl = psQueueData->uEvtInfo.uLlcpEvtInfo.uLlcpLinkHdl;
        phOsal_LogDebugString((const uint8_t*)"DTALib> : PHMWIF_LLCP_P2P_LINK_CONGESTED_EVT ", (const uint8_t*)__FUNCTION__);
        free(psQueueData);
        return;
    case PHMWIF_LLCP_P2P_LINK_UNCONGESTED_EVT:
        dtaLibHdl->bIsLlcpCoRemoteServerLinkCongested = FALSE;
        psQueueData->uEvtType.eLlcpEvtType = PHMWIF_LLCP_P2P_LINK_UNCONGESTED_EVT;
        dtaLibHdl->uLlcpLinkHdl = psQueueData->uEvtInfo.uLlcpEvtInfo.uLlcpLinkHdl;
        phOsal_LogDebugString((const uint8_t*)"DTALib> : PHMWIF_LLCP_P2P_LINK_UNCONGESTED_EVT ", (const uint8_t*)__FUNCTION__);
        break;
    case PHMWIF_LLCP_ERROR_EVT:
    default:
        phOsal_LogErrorString((const uint8_t*)"DTALib> : Error Callback event not handled:",(const uint8_t*)__FUNCTION__);
        free(psQueueData);
        return;
    }

    dwOsalStatus = phOsal_QueuePush(dtaLibHdl->queueHdl, psQueueData, 0);
    if(dwOsalStatus != OSALSTATUS_SUCCESS)
        phOsal_LogErrorString((const uint8_t*)"DTALib> :Unable to Push to Queue", (const uint8_t*)__FUNCTION__);
    else
    {
        phOsal_LogDebugString((const uint8_t*)"DTALib> : ", (const uint8_t*)__FUNCTION__);
        phOsal_LogDebugU32h((const uint8_t*)"Pushed an Object with evtype ", eLlcpEvtType);
    }

    LOG_FUNCTION_EXIT;
    return;
}

/**
 * Common Memory allocation/Free  callback functions
 */
void* phDtaLibi_MemAllocCb(void* memHdl, uint32_t size)
{
    void* addr = malloc(size);
    phOsal_LogDebugU32h((const uint8_t*)"DTALib> Allocating mem Size", size);
    phOsal_LogDebugU32h((const uint8_t*)"Memory Allocatioon  Handle=", (size_t)memHdl);
    phOsal_LogDebugU32h((const uint8_t*)"Addr=0x", (size_t)addr);
    return addr;
}

int32_t phDtaLibi_MemFreeCb(void* memHdl, void* ptrToMem)
{
    phOsal_LogDebugU32h((const uint8_t*)"DTALib> Freeing mem Handle=", (size_t)memHdl);
    phOsal_LogDebugU32h((const uint8_t*)"DTALib> Freeing mem Addr=", (size_t)ptrToMem);
    free(ptrToMem);
    return 0;
}

/**
 * \brief Function to Print the Buffer with a specific Message prompt
 * \param pubuf - 8bit Buffer to be printed
 * \param uilen - length of the Buffer to be printed
 * \param psmessage - Message to be printed before printing the Buffer
 */
void phDtaLibi_PrintBuffer(uint8_t *pubuf,uint16_t uilen,const uint8_t *pcsmessage)
{
    uint16_t i;
    phOsal_LogDebugString((const uint8_t*)"",(const uint8_t*)pcsmessage);
    for(i = 0;i<uilen;i++)
    {
        phOsal_LogDebugU32h((const uint8_t*)"0x",pubuf[i]);
    }
    phOsal_LogDebugU32h((const uint8_t*)" LEN= ",uilen);
}

/**
 * Function to configure(standard/proprietary ID's) the NFC controller before starting discovery
 */
void phDtaLibi_SetMwIfConfig()
{
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;
    uint8_t abConfigIDData[16];

    LOG_FUNCTION_ENTRY;
    phOsal_LogDebug((const uint8_t*)"DTALib> SetConfigProp Started\n");

    /*Do custom Set Config from DTA*/
    abConfigIDData[0] = 0x00;
    phMwIf_SetConfigProp(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PROP_READER_TAG_DETECTOR_CFG, 0x01, abConfigIDData);

    if(dtaLibHdl->sAppDiscCfgParams.dwListenUICC ||
       dtaLibHdl->sAppDiscCfgParams.dwListenESE)
    {
        phOsal_LogDebugString((const uint8_t*)"DTALib> CE Enabled in EE Mode  ", (const uint8_t*)__FUNCTION__);
        if(dtaLibHdl->sAppDiscCfgParams.dwListenUICC)
        {
            /*Enabled SWP Interface1 if UICC is enabled*/
            abConfigIDData[0] = 0x01;
            phMwIf_SetConfigProp(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PROP_SWP_ACTIVATION_STATE_INT1, 0x01, abConfigIDData);
        }
        else
        {
            abConfigIDData[0] = 0x00;
            phMwIf_SetConfigProp(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PROP_SWP_ACTIVATION_STATE_INT1, 0x01, abConfigIDData);
        }
        if(dtaLibHdl->sAppDiscCfgParams.dwListenESE)
        {
            /*Enable SWP Interface2 if eSE is enabled*/
            abConfigIDData[0] = 0x01;
            phMwIf_SetConfigProp(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PROP_SWP_ACTIVATION_STATE_INT2, 0x01, abConfigIDData);
            /*Enable connection to eSE from Host over DWP is eSE is enabled*/
            abConfigIDData[0] = 0x02;
            phMwIf_SetConfigProp(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PROP_NFCEE_ESE_CONN_CONFIG, 0x01, abConfigIDData);
        }
        else
        {
            abConfigIDData[0] = 0x00;
            phMwIf_SetConfigProp(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PROP_SWP_ACTIVATION_STATE_INT2, 0x01, abConfigIDData);
            abConfigIDData[0] = 0x00;
            phMwIf_SetConfigProp(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PROP_NFCEE_ESE_CONN_CONFIG, 0x01, abConfigIDData);
        }

        if(phDtaLibi_ReInit() != DTASTATUS_SUCCESS)
        {
            phOsal_LogDebugString((const uint8_t*)"DTALib> phDtaLibi_ReInit() Failed\n ", (const uint8_t*)__FUNCTION__);
        }
    }

    if(strcmp(dtaLibHdl->sTestProfile.Certification_Release, "CR8") == 0x00)
    {
        abConfigIDData[0] = 0x01;
        phMwIf_SetConfigProp(dtaLibHdl->mwIfHdl, PHMWIF_CERTIFICATION_RELEASE_CONFIG_PROP_CFG, 0x01, abConfigIDData);
#ifdef NFC_NXP_CHIP_PN548AD
    /*FIXME:For PN548 only: CON_DEVICES_LIMIT to be set to 01 and then reset to 03 after Deinit*/
    abConfigIDData[0] = 0x01;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CON_DEVICES_LIMIT, 0x01, abConfigIDData);
#endif
    abConfigIDData[0] = 0x03;
    phMwIf_SetConfigProp(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PROP_READER_FELICA_TSN_CFG, 0x01, abConfigIDData);
    }
    else if(strcmp(dtaLibHdl->sTestProfile.Certification_Release, "CR9") == 0x00)
    {
        abConfigIDData[0] = 0x03;
        phMwIf_SetConfigProp(dtaLibHdl->mwIfHdl, PHMWIF_CERTIFICATION_RELEASE_CONFIG_PROP_CFG, 0x01, abConfigIDData);
        if(dtaLibHdl->sTestProfile.DtaDebugFlag == TRUE){
            /*FIXME:For PN548 only: CON_DEVICES_LIMIT to be set to 01 and then reset to 03 after Deinit*/
            abConfigIDData[0] = dtaLibHdl->sTestProfile.ConnDeviceLimit;
            phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CON_DEVICES_LIMIT, 0x01, abConfigIDData);
            abConfigIDData[0] = dtaLibHdl->sTestProfile.TimeSlotNumber;
            phMwIf_SetConfigProp(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PROP_READER_FELICA_TSN_CFG, 0x01, abConfigIDData);
        }
        else if(dtaLibHdl->sTestProfile.DtaDebugFlag == FALSE){
            abConfigIDData[0] = 0x02;
            phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CON_DEVICES_LIMIT, 0x01, abConfigIDData);
            abConfigIDData[0] = 0xF3;
            phMwIf_SetConfigProp(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PROP_READER_FELICA_TSN_CFG, 0x01, abConfigIDData);
        }
    }

    abConfigIDData[0] = 0x00;
    phMwIf_SetConfigProp(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PROP_READER_JEWEL_RID_CFG, 0x01, abConfigIDData);
    abConfigIDData[0] = 0x00;
    phMwIf_SetConfigProp(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PROP_LISTEN_PROFILE_SEL_CFG, 0x01, abConfigIDData);

    phOsal_LogDebug((const uint8_t*)"DTALib> Calling MwIf SetConfig\n");
    gs_paramBuffer[0] = 0x00;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PN_ATR_REQ_GEN_BYTES_CFG, 0, gs_paramBuffer);//ATR_REQ General bytes
    gs_paramBuffer[0] = 0x00;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PN_ATR_RES_GEN_BYTES_CFG, 0, gs_paramBuffer);//ATR_RES General bytes
    abConfigIDData[0] = 0x30;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PN_ATR_REQ_CFG, 1, abConfigIDData);
    abConfigIDData[0] = 0x01;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PN_NFC_DEP_SPEED, 0x01, abConfigIDData);
    abConfigIDData[0] = 0x00;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PI_BIT_RATE, 0x01, abConfigIDData);
    abConfigIDData[0] = 0x08;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_LA_BIT_FRAME_SDD, 0x01, abConfigIDData);
    abConfigIDData[0] = 0x03;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_LA_PLATFORM_CFG, 0x01, abConfigIDData);
    abConfigIDData[0] = 0x60;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_LA_SEL_INFO, 0x01, abConfigIDData);
    abConfigIDData[0] = 0x01;
    abConfigIDData[1] = 0x02;
    abConfigIDData[2] = 0x03;
    abConfigIDData[3] = 0x04;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_LA_NFCID1, 0x04, abConfigIDData);
    abConfigIDData[0] = 0x06;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_LF_CON_BITR_F, 0x01, abConfigIDData);
    if(dtaLibHdl->sAppDiscCfgParams.dwListenP2P == P2P_DISABLED)
        abConfigIDData[0] = 0x00;
    else
        abConfigIDData[0] = 0x02;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_LF_PROTOCOL_TYPE, 0x01, abConfigIDData);
    abConfigIDData[0] = 0x02;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_LI_BIT_RATE, 0x01, abConfigIDData);
    abConfigIDData[0] = 0x08;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_LN_WT, 0x01, abConfigIDData);
    abConfigIDData[0] = 0x01;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_RF_FIELD_INFO, 0x01, abConfigIDData);
    abConfigIDData[0] = 0x01;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_RF_NFCEE_ACTION, 0x01, abConfigIDData);
    abConfigIDData[0] = 0x0E;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_NFCDEP_OP, 0x01, abConfigIDData);
    abConfigIDData[0] = 0x01;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PF_BIT_RATE, 0x01, abConfigIDData);
    abConfigIDData[0] = 0x00;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_PF_RC_CODE, 0x01, abConfigIDData);
    abConfigIDData[0] = 0xE8;
    abConfigIDData[1] = 0x03;
    phMwIf_SetConfig(dtaLibHdl->mwIfHdl, PHMWIF_NCI_CONFIG_TOTAL_DURATION, 0x02, abConfigIDData);
    LOG_FUNCTION_EXIT;
}

void phDtaLib_RegisterCallback( void *dtaApplHdl, phdtaLib_EvtCb_t dtaApplCb)
{
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;
    LOG_FUNCTION_ENTRY;
    if(dtaApplHdl == NULL || dtaApplCb == NULL)
    {
        phOsal_LogErrorString((const uint8_t*)"DTALib> :Error:Invalid params", (const uint8_t*)__FUNCTION__);
        return;
    }
    dtaLibHdl->dtaApplHdl = dtaApplHdl;
    dtaLibHdl->dtaApplCb = dtaApplCb;
    LOG_FUNCTION_EXIT;
}

DTASTATUS phDtaLib_SetConfig(uint8_t uConfigID, uint8_t ulen, uint8_t * psBuf){
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;
    MWIFSTATUS dwMwIfStatus = 0;
    LOG_FUNCTION_ENTRY;
    dwMwIfStatus = phMwIf_SetConfig(dtaLibHdl->mwIfHdl, uConfigID, ulen, psBuf);
    if(dwMwIfStatus != DTASTATUS_SUCCESS){
        phOsal_LogErrorString((const uint8_t*)"DTALib> :Set configuration is unsuccessful", (const uint8_t*)__FUNCTION__);
        return DTASTATUS_FAILED;
    }
    LOG_FUNCTION_EXIT;
    return DTASTATUS_SUCCESS;
}

DTASTATUS phDtaLib_GetConfig(uint8_t bnumIDs, uint8_t *pbparamIDs, uint8_t *pvOutBuff,uint32_t *dwLenOutBuff){
    phDtaLib_sHandle_t *dtaLibHdl = &g_DtaLibHdl;
    MWIFSTATUS dwMwIfStatus = 0;
    LOG_FUNCTION_ENTRY;
    dwMwIfStatus = phMwIf_GetConfig(dtaLibHdl->mwIfHdl, bnumIDs, pbparamIDs, pvOutBuff, dwLenOutBuff);
    if(dwMwIfStatus != DTASTATUS_SUCCESS){
        phOsal_LogErrorString((const uint8_t*)"DTALib> :Get configuration is unsuccessful", (const uint8_t*)__FUNCTION__);
        return DTASTATUS_FAILED;
    }
    LOG_FUNCTION_EXIT;
    return DTASTATUS_SUCCESS;
}
#ifdef __cplusplus
}
#endif
