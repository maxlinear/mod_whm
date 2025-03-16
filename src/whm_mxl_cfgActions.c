/******************************************************************************

         Copyright (c) 2023 - 2025 MaxLinear, Inc.

  This software may be distributed under the terms of the BSD license.
  See README for more details.

*******************************************************************************/

/*  *****************************************************************************
*         File Name    : whm_mxl_cfgActions.c                                  *
*         Description  : MxL Schedule and Call Actions to PWHM State Machine   *
*                                                                              *
*  *****************************************************************************/

#include "swl/swl_common.h"
#include <swla/swla_mac.h>
#include "swla/swla_chanspec.h"

#include "wld/wld_radio.h"
#include "wld/wld_accesspoint.h"
#include "wld/wld_util.h"
#include "wld/wld_hostapd_ap_api.h"
#include "wld/wld_hostapd_cfgFile.h"
#include "wld/wld_wpaCtrlInterface.h"
#include "wld/Utils/wld_autoCommitMgr.h"
#include "wld/wld_wpaCtrl_api.h"

#include "whm_mxl_cfgActions.h"
#include "whm_mxl_hostapd_cfg.h"
#include "whm_mxl_utils.h"
#include "whm_mxl_rad.h"
#include "whm_mxl_vap.h"
#include "whm_mxl_zwdfs.h"

#define ME "mxlAct"

static swl_rc_ne s_doHapdRestart(T_Radio* pRad, T_AccessPoint* pAP _UNUSED) {
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "pRad is NULL");
    pRad->pFA->mfn_wrad_secDmn_restart(pRad, SET);
    return SWL_RC_OK;
}

static swl_rc_ne s_doHapdToggle(T_Radio* pRad, T_AccessPoint* pAP _UNUSED) {
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "pRad is NULL");
    pRad->pFA->mfn_wrad_toggle(pRad, SET);
    return SWL_RC_OK;
}

static swl_rc_ne s_doHapdSighup(T_Radio* pRad, T_AccessPoint* pAP _UNUSED) {
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "pRad is NULL");
    pRad->pFA->mfn_wrad_secDmn_refresh(pRad, SET);
    return SWL_RC_OK;
}

static swl_rc_ne s_doUpdateBeacon(T_Radio* pRad _UNUSED, T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, SWL_RC_INVALID_PARAM, ME, "pAP is NULL");
    swl_rc_ne rc = SWL_RC_OK;
    ASSERTI_TRUE(wld_wpaCtrlInterface_isReady(pAP->wpaCtrlInterface), SWL_RC_INVALID_STATE, ME, "%s: wpaCtrl disconnected", pAP->alias);
    if(!(wld_ap_hostapd_updateBeacon(pAP, "updateBeacon"))) {
        SAH_TRACEZ_ERROR(ME, "%s: Failed to update becaon", pAP->alias);
        rc = SWL_RC_ERROR;
    }
    wld_ap_doSync(pAP);
    return rc;
}

static swl_rc_ne s_doHapdConfUpdate(T_Radio* pRad, T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "pRad is NULL");
    T_AccessPoint* tgtAP = pAP;
    /* Replace with master VAP AP when no particular AP context passed */
    if (pAP == NULL) {
        T_AccessPoint* masterVap = wld_rad_getFirstVap(pRad);
        ASSERT_NOT_NULL(masterVap, SWL_RC_INVALID_PARAM, ME, "masterVap is NULL");
        tgtAP = masterVap;
    }
    wld_ap_doSync(tgtAP);
    return SWL_RC_OK;
}

static swl_rc_ne s_doNoAction(T_Radio* pRad _UNUSED, T_AccessPoint* pAP _UNUSED) {
    SAH_TRACEZ_INFO(ME, "No Action");
    return SWL_RC_OK;
}

SWL_TABLE(sActionHandlers,
          ARR(whm_mxl_hapd_action_e action; void* actionCb; ),
          ARR(swl_type_uint32, swl_type_voidPtr, ),
          ARR(//hapd actions mapping to execute functions
              {HAPD_ACTION_ERROR,               NULL},
              {HAPD_ACTION_NONE,                s_doNoAction},
              {HAPD_ACTION_NEED_UPDATE_CONF,    s_doHapdConfUpdate},
              {HAPD_ACTION_NEED_UPDATE_BEACON,  s_doUpdateBeacon},
              {HAPD_ACTION_NEED_SIGHUP,         s_doHapdSighup},
              {HAPD_ACTION_NEED_TOGGLE,         s_doHapdToggle},
              {HAPD_ACTION_NEED_RESTART,        s_doHapdRestart},
              ));

SWL_TABLE(sVendorParamsOdlToConf,
          ARR(char* paramOdlName; char* paramHapdName; ),
          ARR(swl_type_charPtr, swl_type_charPtr, ),
          ARR(//Convert ODL name to HAPD conf name
              //Radio Parameters
              {"ApMaxNumSta",                   "ap_max_num_sta"},
              {"OverrideMBSSID",                "override_6g_mbssid_default_mode"},
              {"SetProbeReqCltMode",            "sProbeReqCltMode"},
              {"SetBfMode",                     "sBfMode"},
              {"SetPowerSelection",             "sPowerSelection"},
              {"Ignore40MhzIntolerant",         "ignore_40_mhz_intolerant"},
              {"ProbeReqListTimer",             "ProbeReqListTimer"},
              {"DfsChStateFile",                "dfs_channels_state_file_location"},
#ifdef CONFIG_VENDOR_MXL_PROPRIETARY
              {"DfsDebugChan",                  "dfs_debug_chan"},
#endif /* CONFIG_VENDOR_MXL_PROPRIETARY */
              {"SubBandDFS",                    "sub_band_dfs"},
              {"TwtResponderSupport",           "twt_responder_support"},
              {"HeMacTwtResponderSupport",      "he_mac_twt_responder_support"},
              {"DynamicEdca",                   "dynamic_edca"},
              {"HeDebugMode",                   "enable_he_debug_mode"},
              {"HeBeacon",                      "he_beacon"},
              {"DuplicateBeaconEnabled",        "duplicate_beacon_enabled"},
              {"DuplicateBeaconBw",             "duplicate_beacon_bw"},
              {"SetQAMplus",                    "sQAMplus"},
              {"Ieee80211Be",                   "ieee80211be"},
              {"SetRadarRssiTh",                "sRadarRssiTh"},
              {"ObssBeaconRssiThreshold",       "obss_beacon_rssi_threshold"},
              {"ObssInterval",                  "obss_interval"},
              {"ScanPassiveDwell",              "scan_passive_dwell"},
              {"ScanActiveDwell",               "scan_active_dwell"},
              {"ScanPassiveTotalPerChannel",    "scan_passive_total_per_channel"},
              {"ScanActiveTotalPerChannel",     "scan_active_total_per_channel"},
              {"ChannelTransitionDelayFactor",  "channel_transition_delay_factor"},
              {"ScanActivityThreshold",         "scan_activity_threshold"},
#ifdef CONFIG_VENDOR_MXL_PROPRIETARY
              {"AcsFallbackChan",               "acs_fallback_chan"},
              {"AcsScanMode",                   "acs_scan_mode"},
              {"AcsUpdateDoSwitch",             "acs_update_do_switch"},
              {"AcsFils",                       "acs_fils"},
              {"Acs6gOptChList",                "acs_6g_opt_ch_list"},
              {"AcsStrictChList",               "acs_strict_chanlist"},
#endif /* CONFIG_VENDOR_MXL_PROPRIETARY */
              {"BackgroundCac",                 "background_cac"},
              {"StartAfter",                    "start_after"},
              {"StartAfterDelay",               "start_after_delay"},
              {"StartAfterWatchdogTime",        "start_after_watchdog_time"},
              {"PunctureBitMap",                "punct_bitmap"},
              {"AfcdSock",                      "afcd_sock"},
              {"AfcOpClass",                    "afc_op_class"},
              {"AfcFrequencyRange",             "afc_freq_range"},
              {"AfcCertIds",                    "afc_cert_ids"},
              {"AfcSerialNumber",               "afc_serial_number"},
              {"AfcLinearPolygon",              "afc_linear_polygon"},
              {"AfcLocationType",               "afc_location_type"},
              {"AfcRequestId",                  "afc_request_id"},
              {"AfcRequestVersion",             "afc_request_version"},
              //VAP Parameters
              {"EnableHairpin",                 "enable_hairpin"},
              {"ApMaxInactivity",               "ap_max_inactivity"},
              {"UnsolBcastProbeRespInterval",   "unsol_bcast_probe_resp_interval"},
              {"FilsDiscoveryMaxInterval",      "fils_discovery_max_interval"},
              {"BssTransition",                 "bss_transition"},
              {"ManagementFramesRate",          "management_frames_rate"},
              {"MgmtFramePowerControl",         "mgmt_frame_power_control"},
              {"ClientDisallow",                "multi_ap_client_disallow"},
              {"NumResSta",                     "num_res_sta"},
              {"VendorVht",                     "vendor_vht"},
              {"MLOEnable",                     "mlo_enable"},
              {"ApMldMac",                      "ap_mld_mac"},
              {"WdsSingleMlAssoc",              "wds_single_ml_assoc"},
              {"WdsPrimaryLink",                "wds_primary_link"},
              {"SoftBlockAclEnable",            "soft_block_acl_enable"},
              {"SoftBlockAclWaitTime",          "soft_block_acl_wait_time"},
              {"SoftBlockAclAllowTime",         "soft_block_acl_allow_time"},
              {"SoftBlockAclOnAuthReq",         "soft_block_acl_on_auth_req"},
              {"SoftBlockAclOnProbeReq",        "soft_block_acl_on_probe_req"},
              {"OWETransitionBSSID",            "owe_transition_bssid"},
              {"OWETransitionSSID",             "owe_transition_ssid"},
              {"DynamicMulticastMode",          "dynamic_multicast_mode"},
              {"DynamicMulticastRate",          "dynamic_multicast_rate"},
              {"DisableBeaconProtection",       "disable_beacon_prot"},
              {"SetBridgeMode",                 "sBridgeMode"},
              ));

SWL_TABLE(sRadCfgParamsActionMap,
          ARR(char* param; whm_mxl_hapd_action_e action; ),
          ARR(swl_type_charPtr, swl_type_uint32, ),
          ARR(//params/object set and applied with hostapd actions
              //Actions applied with hostapd restart
              {"Ieee80211Be",                   HAPD_ACTION_NEED_RESTART},
              {"Ignore40MhzIntolerant",         HAPD_ACTION_NEED_RESTART},
              {"HtCapabilities",                HAPD_ACTION_NEED_RESTART},
              {"VhtCapabilities",               HAPD_ACTION_NEED_RESTART},
#ifdef CONFIG_VENDOR_MXL_PROPRIETARY
              {"AcsFallbackChan",               HAPD_ACTION_NEED_RESTART},
              {"AcsScanMode",                   HAPD_ACTION_NEED_RESTART},
              {"AcsUpdateDoSwitch",             HAPD_ACTION_NEED_RESTART},
              {"AcsFils",                       HAPD_ACTION_NEED_RESTART},
              {"Acs6gOptChList",                HAPD_ACTION_NEED_RESTART},
              {"AcsStrictChList",               HAPD_ACTION_NEED_RESTART},
#endif /* CONFIG_VENDOR_MXL_PROPRIETARY */
              {"AfcdSock",                      HAPD_ACTION_NEED_RESTART},
              {"AfcOpClass",                    HAPD_ACTION_NEED_RESTART},
              {"AfcFrequencyRange",             HAPD_ACTION_NEED_RESTART},
              {"AfcCertIds",                    HAPD_ACTION_NEED_RESTART},
              {"AfcSerialNumber",               HAPD_ACTION_NEED_RESTART},
              {"AfcLinearPolygon",              HAPD_ACTION_NEED_RESTART},
              {"AfcLocationType",               HAPD_ACTION_NEED_RESTART},
              {"AfcRequestId",                  HAPD_ACTION_NEED_RESTART},
              {"AfcRequestVersion",             HAPD_ACTION_NEED_RESTART},
              //Actions applied with hostapd toggle
              {"OverrideMBSSID",                HAPD_ACTION_NEED_TOGGLE},
              {"ApMaxNumSta",                   HAPD_ACTION_NEED_TOGGLE},
              {"SetProbeReqCltMode",            HAPD_ACTION_NEED_TOGGLE},
              {"SetBfMode",                     HAPD_ACTION_NEED_TOGGLE},
              {"SetPowerSelection",             HAPD_ACTION_NEED_TOGGLE},
              {"ObssInterval",                  HAPD_ACTION_NEED_TOGGLE},
              {"ObssBeaconRssiThreshold",       HAPD_ACTION_NEED_TOGGLE},
              {"ProbeReqListTimer",             HAPD_ACTION_NEED_TOGGLE},
              {"DfsChStateFile",                HAPD_ACTION_NEED_TOGGLE},
#ifdef CONFIG_VENDOR_MXL_PROPRIETARY
              {"DfsDebugChan",                  HAPD_ACTION_NEED_TOGGLE},
#endif /* CONFIG_VENDOR_MXL_PROPRIETARY */
              {"SubBandDFS",                    HAPD_ACTION_NEED_TOGGLE},
              {"TwtResponderSupport",           HAPD_ACTION_NEED_TOGGLE},
              {"HeMacTwtResponderSupport",      HAPD_ACTION_NEED_TOGGLE},
              {"DynamicEdca",                   HAPD_ACTION_NEED_TOGGLE},
              {"HeDebugMode",                   HAPD_ACTION_NEED_TOGGLE},
              {"HeBeacon",                      HAPD_ACTION_NEED_TOGGLE},
              {"DuplicateBeaconEnabled",        HAPD_ACTION_NEED_TOGGLE},
              {"DuplicateBeaconBw",             HAPD_ACTION_NEED_TOGGLE},
              {"SetQAMplus",                    HAPD_ACTION_NEED_TOGGLE},
              {"SetRadarRssiTh",                HAPD_ACTION_NEED_TOGGLE},
              {"ScanPassiveDwell",              HAPD_ACTION_NEED_TOGGLE},
              {"ScanActiveDwell",               HAPD_ACTION_NEED_TOGGLE},
              {"ScanPassiveTotalPerChannel",    HAPD_ACTION_NEED_TOGGLE},
              {"ScanActiveTotalPerChannel",     HAPD_ACTION_NEED_TOGGLE},
              {"ChannelTransitionDelayFactor",  HAPD_ACTION_NEED_TOGGLE},
              {"ScanActivityThreshold",         HAPD_ACTION_NEED_TOGGLE},
              {"FirstNonDfs",                   HAPD_ACTION_NEED_TOGGLE},
              {"BackgroundCac",                 HAPD_ACTION_NEED_TOGGLE},
              {"StartAfter",                    HAPD_ACTION_NEED_TOGGLE},
              {"StartAfterDelay",               HAPD_ACTION_NEED_TOGGLE},
              {"StartAfterWatchdogTime",        HAPD_ACTION_NEED_TOGGLE},
              {"PunctureBitMap",                HAPD_ACTION_NEED_TOGGLE},
              ));

static swl_rc_ne whm_mxl_hostapd_getRadParamAction(whm_mxl_hapd_action_e* pOutMappedAction, const char* paramName) {
    whm_mxl_hapd_action_e* pMappedAction = (whm_mxl_hapd_action_e*) swl_table_getMatchingValue(&sRadCfgParamsActionMap, 1, 0, paramName);
    ASSERTS_NOT_NULL(pMappedAction, SWL_RC_ERROR, ME, "NULL");
    W_SWL_SETPTR(pOutMappedAction, *pMappedAction);
    return SWL_RC_OK;
}

static whm_mxl_actionHandler_f whm_mxl_getActionHdlr(uint32_t action) {
    whm_mxl_actionHandler_f* pfActionHdlr = (whm_mxl_actionHandler_f*) swl_table_getMatchingValue(&sActionHandlers, 1, 0, &action);
    ASSERTS_NOT_NULL(pfActionHdlr, NULL, ME, "no internal hdlr defined for action(%d)", action);
    return *pfActionHdlr;
}

/**
 * @brief Determine which actions to take when specific RADIO parameter is changed
 *
 * @param pRad radio
 * @param paramName parameter name in data model
 * @return return code of executed action.
 */
swl_rc_ne whm_mxl_determineRadParamAction(T_Radio* pRad, const char* paramName, const char* paramValue) {
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "No Radio Mapped");
    ASSERT_NOT_NULL(paramName, SWL_RC_INVALID_PARAM, ME, "paramName is NULL");
    whm_mxl_hapd_action_e action = HAPD_ACTION_NONE;
    swl_rc_ne rc;
    whm_mxl_actionHandler_f pfRadActionHdlr;
    bool ret = 0;

    /* First try setting parameter dynamically via ctrl interface before applying any action */
    T_AccessPoint* masterVap = wld_rad_getFirstVap(pRad);
    if (masterVap != NULL) {
        if(wld_wpaCtrlInterface_isReady(masterVap->wpaCtrlInterface) && (paramValue != NULL)) {
            const char* pConfName= (char*) swl_table_getMatchingValue(&sVendorParamsOdlToConf, 1, 0, paramName);
            ret = wld_ap_hostapd_setParamValue(masterVap, pConfName, paramValue, paramName);
            /* TO DO: maybe fallback to specific action if SET failed? */
        }
    }

    whm_mxl_hostapd_getRadParamAction(&action, paramName);
    ASSERT_NOT_EQUALS(action, HAPD_ACTION_ERROR, SWL_RC_INVALID_PARAM, ME, "Action HAPD_ACTION_ERROR");
    SAH_TRACEZ_INFO(ME, "%s: paramName=%s action=%d", pRad->Name, paramName, action);
    if ((pRad->status == RST_ERROR) || (pRad->status == RST_UNKNOWN)) {
        action = HAPD_ACTION_NONE;
        SAH_TRACEZ_INFO(ME, "%s: Invalid radio state(%d), forcing action HAPD_ACTION_NONE", pRad->Name, pRad->status);
    }
    pfRadActionHdlr = whm_mxl_getActionHdlr(action);
    ASSERTS_NOT_NULL(pfRadActionHdlr, SWL_RC_NOT_IMPLEMENTED, ME, "No handler for action(%d)", action);
    rc = pfRadActionHdlr(pRad, NULL);
    return rc;
}

SWL_TABLE(sVapCfgParamsActionMap,
          ARR(char* param; whm_mxl_hapd_action_e action; ),
          ARR(swl_type_charPtr, swl_type_uint32, ),
          ARR(//params/object set and applied with hostapd actions
              //Actions applied with hostapd restart
              {"UnsolBcastProbeRespInterval",   HAPD_ACTION_NEED_RESTART},
              {"FilsDiscoveryMaxInterval",      HAPD_ACTION_NEED_RESTART},
              //Actions applied with hostapd toggle
              {"EnableHairpin",                 HAPD_ACTION_NEED_TOGGLE},
              {"ManagementFramesRate",          HAPD_ACTION_NEED_TOGGLE},
              {"NumResSta",                     HAPD_ACTION_NEED_TOGGLE},
              {"VendorVht",                     HAPD_ACTION_NEED_TOGGLE},
              {"MLOEnable",                     HAPD_ACTION_NEED_TOGGLE},
              {"ApMldMac",                      HAPD_ACTION_NEED_TOGGLE},
              {"WdsSingleMlAssoc",              HAPD_ACTION_NEED_TOGGLE},
              {"WdsPrimaryLink",                HAPD_ACTION_NEED_TOGGLE},
              {"SoftBlockAclEnable",            HAPD_ACTION_NEED_TOGGLE},
              {"SoftBlockAclWaitTime",          HAPD_ACTION_NEED_TOGGLE},
              {"SoftBlockAclAllowTime",         HAPD_ACTION_NEED_TOGGLE},
              {"SoftBlockAclOnAuthReq",         HAPD_ACTION_NEED_TOGGLE},
              {"SoftBlockAclOnProbeReq",        HAPD_ACTION_NEED_TOGGLE},
              {"DynamicMulticastMode",          HAPD_ACTION_NEED_TOGGLE},
              {"DynamicMulticastRate",          HAPD_ACTION_NEED_TOGGLE},
              {"DisableBeaconProtection",       HAPD_ACTION_NEED_TOGGLE},
              {"SetBridgeMode",                 HAPD_ACTION_NEED_TOGGLE},
              //Actions applied with sighup to hostpad
              {"OWETransitionBSSID",            HAPD_ACTION_NEED_SIGHUP},
              {"OWETransitionSSID",             HAPD_ACTION_NEED_SIGHUP},
              {"ClientDisallow",                HAPD_ACTION_NEED_SIGHUP},
              //Actions applied with update beacon
              {"BssTransition",                 HAPD_ACTION_NEED_UPDATE_BEACON},
              {"ApMaxInactivity",               HAPD_ACTION_NEED_UPDATE_BEACON},
              //Action applied with update hostapd conf
              {"MgmtFramePowerControl",         HAPD_ACTION_NEED_UPDATE_CONF},
              ));

static swl_rc_ne whm_mxl_hostapd_getVapParamAction(whm_mxl_hapd_action_e* pOutMappedAction, const char* paramName) {
    whm_mxl_hapd_action_e* pMappedAction = (whm_mxl_hapd_action_e*) swl_table_getMatchingValue(&sVapCfgParamsActionMap, 1, 0, paramName);
    ASSERTS_NOT_NULL(pMappedAction, SWL_RC_ERROR, ME, "NULL");
    W_SWL_SETPTR(pOutMappedAction, *pMappedAction);
    return SWL_RC_OK;
}

/**
 * @brief Determine which actions to take when specific VAP parameter is changed
 *
 * @param pAP accesspoint
 * @param paramName parameter name in data model
 * @param paramValue param value represented as a string
 * @return return code of executed action.
 */
swl_rc_ne whm_mxl_determineVapParamAction(T_AccessPoint* pAP, const char* paramName, const char* paramValue) {
    ASSERT_NOT_NULL(pAP, SWL_RC_INVALID_PARAM, ME, "No pAP Mapped");
    ASSERT_NOT_NULL(paramName, SWL_RC_INVALID_PARAM, ME, "paramName is NULL");
    T_Radio* pRad = pAP->pRadio;
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "No Radio Mapped");
    whm_mxl_hapd_action_e action = HAPD_ACTION_NONE;
    swl_rc_ne rc;
    whm_mxl_actionHandler_f pfVapActionHdlr;
    bool ret = 0;

    /* First try setting parameter dynamically via ctrl interface before applying any action */
    if(wld_wpaCtrlInterface_isReady(pAP->wpaCtrlInterface) && (paramValue != NULL)) {
        const char* pConfName= (char*) swl_table_getMatchingValue(&sVendorParamsOdlToConf, 1, 0, paramName);
        ret = wld_ap_hostapd_setParamValue(pAP, pConfName, paramValue, paramName);
    }

    whm_mxl_hostapd_getVapParamAction(&action, paramName);
    ASSERT_NOT_EQUALS(action, HAPD_ACTION_ERROR, SWL_RC_INVALID_PARAM, ME, "Action HAPD_ACTION_ERROR");
    SAH_TRACEZ_INFO(ME, "%s: paramName=%s action=%d", pAP->alias, paramName, action);
    if ((pRad->status == RST_ERROR) || (pRad->status == RST_UNKNOWN)) {
        action = HAPD_ACTION_NONE;
        SAH_TRACEZ_INFO(ME, "%s: Invalid radio state(%d), forcing action HAPD_ACTION_NONE", pRad->Name, pRad->status);
    } else if (pAP->status == APSTI_DISABLED) {
        action = HAPD_ACTION_NEED_UPDATE_CONF;
        SAH_TRACEZ_INFO(ME, "%s: AP state(%d), forcing action HAPD_ACTION_NEED_UPDATE_CONF", pRad->Name, pAP->status);
    }
    pfVapActionHdlr = whm_mxl_getActionHdlr(action);
    ASSERTS_NOT_NULL(pfVapActionHdlr, SWL_RC_NOT_IMPLEMENTED, ME, "No handler for action(%d)", action);
    rc = pfVapActionHdlr(pRad, pAP);
    return rc;
}

static swl_rc_ne s_doEpUpdate(T_Radio* pRad _UNUSED, T_EndPoint* pEP) {
    ASSERT_NOT_NULL(pEP, SWL_RC_INVALID_PARAM, ME, "No pEP Mapped");
    pEP->pFA->mfn_wendpoint_update(pEP, SET);
    return SWL_RC_OK;
}

static swl_rc_ne s_doEpNoAction(T_Radio* pRad _UNUSED, T_EndPoint* pEP _UNUSED) {
    SAH_TRACEZ_INFO(ME, "No EP Action");
    return SWL_RC_OK;
}

SWL_TABLE(sEpActionHandlers,
          ARR(whm_mxl_hapd_action_e action; void* actionCb; ),
          ARR(swl_type_uint32, swl_type_voidPtr, ),
          ARR(//hapd actions mapping to execute functions
              {WPA_SUPP_ACTION_ERROR,               NULL},
              {WPA_SUPP_ACTION_NONE,                s_doEpNoAction},
              {WPA_SUPP_ACTION_NEED_UPDATE,         s_doEpUpdate},
              ));

SWL_TABLE(sEpCfgParamsActionMap,
          ARR(char* param; whm_mxl_supplicant_action_e action; ),
          ARR(swl_type_charPtr, swl_type_uint32, ),
          ARR(//params/object set and applied with hostapd actions
              {"Wds",              WPA_SUPP_ACTION_NEED_UPDATE},
              {"VendorElements",   WPA_SUPP_ACTION_NEED_UPDATE},
              {"MultiApProfile",   WPA_SUPP_ACTION_NEED_UPDATE},
              {"WpsCredAddSae",    WPA_SUPP_ACTION_NEED_UPDATE},
              ));

static swl_rc_ne whm_mxl_hostapd_getEpParamAction(whm_mxl_supplicant_action_e* pOutMappedAction, const char* paramName) {
    whm_mxl_supplicant_action_e* pMappedAction = (whm_mxl_supplicant_action_e*) swl_table_getMatchingValue(&sEpCfgParamsActionMap, 1, 0, paramName);
    ASSERTS_NOT_NULL(pMappedAction, SWL_RC_ERROR, ME, "NULL");
    W_SWL_SETPTR(pOutMappedAction, *pMappedAction);
    return SWL_RC_OK;
}

static whm_mxl_actionEpHandler_f whm_mxl_getEpActionHdlr(uint32_t action) {
    whm_mxl_actionEpHandler_f* pfActionHdlr = (whm_mxl_actionEpHandler_f*) swl_table_getMatchingValue(&sEpActionHandlers, 1, 0, &action);
    ASSERTS_NOT_NULL(pfActionHdlr, NULL, ME, "no internal hdlr defined for EP action(%d)", action);
    return *pfActionHdlr;
}

/**
 * @brief Determine which actions to take when specific EP parameter is changed
 *
 * @param pEP endpoint
 * @param paramName parameter name in data model
 * @return return code of executed action.
 */
swl_rc_ne whm_mxl_determineEpParamAction(T_EndPoint* pEP, const char* paramName) {
    ASSERT_NOT_NULL(pEP, SWL_RC_INVALID_PARAM, ME, "No pEP Mapped");
    ASSERT_NOT_NULL(paramName, SWL_RC_INVALID_PARAM, ME, "paramName is NULL");
    T_Radio* pRad = pEP->pRadio;
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "No Radio Mapped");
    whm_mxl_supplicant_action_e action = WPA_SUPP_ACTION_NONE;
    swl_rc_ne rc;
    whm_mxl_actionEpHandler_f pfEpActionHdlr;

    whm_mxl_hostapd_getEpParamAction(&action, paramName);
    ASSERT_NOT_EQUALS(action, WPA_SUPP_ACTION_ERROR, SWL_RC_INVALID_PARAM, ME, "Action WPA_SUPP_ACTION_ERROR");
    SAH_TRACEZ_INFO(ME, "%s: paramName=%s action=%d", pEP->Name, paramName, action);
    if ((pRad->status == RST_ERROR) || (pRad->status == RST_UNKNOWN)) {
        action = WPA_SUPP_ACTION_NONE;
        SAH_TRACEZ_INFO(ME, "%s: Invalid radio state(%d), forcing action WPA_SUPP_ACTION_NONE", pRad->Name, pRad->status);
    }
    pfEpActionHdlr = whm_mxl_getEpActionHdlr(action);
    ASSERTS_NOT_NULL(pfEpActionHdlr, SWL_RC_NOT_IMPLEMENTED, ME, "No handler for action(%d)", action);
    rc = pfEpActionHdlr(pRad, pEP);
    return rc;
}

/**
 * @brief Request hostapd restart from pwhm state machine
 *
 * @param pRad radio
 * @return return code of executed action.
 */
swl_rc_ne whm_mxl_restartHapd(T_Radio* pRad) {
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "No Radio Mapped");
    return s_doHapdRestart(pRad, NULL);
}

/**
 * @brief Request hostapd toggle (interface disable --> enable) from pwhm state machine
 *
 * @param pRad radio
 * @return return code of executed action.
 */
swl_rc_ne whm_mxl_toggleHapd(T_Radio* pRad) {
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "No Radio Mapped");
    return s_doHapdToggle(pRad, NULL);
}

/**
 * @brief Request hostapd refresh (SIGHUP) from pwhm state machine
 *
 * @param pRad radio
 * @return return code of executed action.
 */
swl_rc_ne whm_mxl_sighupHapd(T_Radio* pRad) {
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "pRad is NULL");
    return s_doHapdSighup(pRad, NULL);
}

/**
 * @brief Do UPDATE_BEACON on requeseted AP and sync configuration file
 *
 * @param pRad radio
 * @return return code of executed action.
 */
swl_rc_ne whm_mxl_updateBeaconHapd(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, SWL_RC_INVALID_PARAM, ME, "pAP is NULL");
    return s_doUpdateBeacon(NULL, pAP);
}

/**
 * @brief Request to update hostapd configuration file
 *
 * @param pRad radio
 * @param pAP accesspoint - can also pass NULL to sync via master VAP AP
 * @return return code of executed action.
 */
swl_rc_ne whm_mxl_confModHapd(T_Radio* pRad, T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "pRad is NULL");
    /* pAP NULL is a valid input - hence not checked */
    return s_doHapdConfUpdate(pRad, pAP);
}

/**
 * @brief Request to restart security daemon for all the radios
 *
 * @return return code of executed action.
 */
swl_rc_ne whm_mxl_restartAllRadios() {
    T_Radio* pRad;
    wld_for_eachRad(pRad) {
        if (pRad && pRad->pBus) {
            SAH_TRACEZ_INFO(ME, "%s: Restarting radio", pRad->Name);
            whm_mxl_restartHapd(pRad);
        }
    }
    T_Radio* zwdfsRadio = mxl_rad_getZwDfsRadio();
    if (zwdfsRadio) {
        SAH_TRACEZ_INFO(ME, "Restarting ZWDFS radio");
        whm_mxl_restartHapd(zwdfsRadio);
    } else {
        SAH_TRACEZ_WARNING(ME, "Unable to restart ZWDFS radio, radio ctx does not exist");
    }
    return SWL_RC_OK;
}

static bool s_whm_mxl_setParam(T_AccessPoint* pAP, const char* param, const char* newValue) {
    ASSERTS_NOT_NULL(param, false, ME, "NULL");
    ASSERTS_FALSE(swl_str_isEmpty(newValue), false, ME, "newValue does not exist");
    return wld_ap_hostapd_setParamValue(pAP, param, newValue, param);
}

/**
 * @brief Do hostapd SET for array Vendor VAP parameters
 *
 * @param pAP accesspoint
 * @param params set of hostapd config parameters
 * @return return code of executed action.
 */
swl_rc_ne whm_mxl_set_vendorMultipleParams(T_AccessPoint* pAP, const char* params[], uint32_t nParams) {
    ASSERTS_NOT_NULL(pAP, SWL_RC_INVALID_PARAM, ME, "pAP is NULL");
    swl_mapChar_t newVapParams;
    swl_mapChar_t* pNewVapParams = &newVapParams;
    swl_mapChar_init(pNewVapParams);
    whm_mxl_vap_updateConfigMap(pAP, pNewVapParams);
    for(uint32_t i = 0; i < nParams; i++) {
        s_whm_mxl_setParam(pAP, params[i], swl_mapChar_get(pNewVapParams, (char*) params[i]));
    }
    swl_mapChar_cleanup(pNewVapParams);
    return SWL_RC_OK;
}

static swl_rc_ne s_bApSetParams(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, SWL_RC_INVALID_PARAM, ME, "No pAP Mapped");
    T_Radio* pRad = pAP->pRadio;
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "No Radio Mapped");
    char bApSsid[256] = {0};
    char bApPass[256] = {0};
    swl_mapChar_t newVapParams;
    swl_mapChar_t* pNewVapParams = &newVapParams;
    const char* bApParams[] = {
        "multi_ap_client_disallow"
    };
    whm_mxl_set_vendorMultipleParams(pAP, bApParams, SWL_ARRAY_SIZE(bApParams));

    /* Fetch and prepare bAP credentials */
    swl_mapChar_init(pNewVapParams);
    wld_hostapd_cfgFile_setVapConfig(pAP, pNewVapParams, (swl_mapChar_t*) NULL);
    swl_str_catFormat(bApSsid, sizeof(bApSsid), "SET %s \"%s\"", "multi_ap_backhaul_ssid", swl_mapChar_get(pNewVapParams, "ssid"));
    if (!swl_str_isEmpty(swl_mapChar_get(pNewVapParams, "wpa_passphrase"))) {
        swl_str_catFormat(bApPass, sizeof(bApPass), "SET %s %s", "multi_ap_backhaul_wpa_passphrase",
                            swl_mapChar_get(pNewVapParams, "wpa_passphrase"));
    } else {
        swl_str_catFormat(bApPass, sizeof(bApPass), "SET %s %s", "multi_ap_backhaul_wpa_psk",
                            swl_mapChar_get(pNewVapParams, "wpa_psk"));
    }

    /* Update colocated fAPs with the newly created bAP credentials */
    amxc_llist_for_each(ap_it, &pRad->llAP) {
        T_AccessPoint* pOtherAp = amxc_llist_it_get_data(ap_it, T_AccessPoint, it);
        mxl_VapVendorData_t* mxlVapVendorData = mxl_vap_getVapVendorData(pOtherAp);
        if ((mxlVapVendorData == NULL) || (whm_mxl_utils_isDummyVap(pOtherAp))) {
            continue;
        }
        if (mxlVapVendorData->mxlMultiApType == MXL_FRONTHAUL_MAP) {
            /* Update bAP credentials */
            SAH_TRACEZ_INFO(ME, "%s: sending cmd:%s to AP(%s)", pAP->alias, bApSsid, pOtherAp->alias);
            wld_ap_hostapd_sendCommand(pOtherAp, bApSsid, "multi ap backhaul ssid");
            SAH_TRACEZ_INFO(ME, "%s: sending cmd:%s to AP(%s)", pAP->alias, bApPass, pOtherAp->alias);
            wld_ap_hostapd_sendCommand(pOtherAp, bApPass, "multi ap backhaul password");
        }
    }
    swl_mapChar_cleanup(pNewVapParams);

    return SWL_RC_OK;
}

static swl_rc_ne s_hybridApSetParams(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, SWL_RC_INVALID_PARAM, ME, "No pAP Mapped");
    char hybridApSsid[256] = {0};
    char hybridApPass[256] = {0};
    swl_mapChar_t newVapParams;
    swl_mapChar_t* pNewVapParams = &newVapParams;

    const char* hybridApParams[] = {
        "multi_ap_client_disallow"
    };
    whm_mxl_set_vendorMultipleParams(pAP, hybridApParams, SWL_ARRAY_SIZE(hybridApParams));

    /* Fetch and update hybrid AP credentials */
    swl_mapChar_init(pNewVapParams);
    wld_hostapd_cfgFile_setVapConfig(pAP, pNewVapParams, (swl_mapChar_t*) NULL);
    swl_str_catFormat(hybridApSsid, sizeof(hybridApSsid), "SET %s \"%s\"", "multi_ap_backhaul_ssid", swl_mapChar_get(pNewVapParams, "ssid"));
    if (!swl_str_isEmpty(swl_mapChar_get(pNewVapParams, "wpa_passphrase"))) {
        swl_str_catFormat(hybridApPass, sizeof(hybridApPass), "SET %s %s", "multi_ap_backhaul_wpa_passphrase",
                            swl_mapChar_get(pNewVapParams, "wpa_passphrase"));
    } else {
        swl_str_catFormat(hybridApPass, sizeof(hybridApPass), "SET %s %s", "multi_ap_backhaul_wpa_psk",
                            swl_mapChar_get(pNewVapParams, "wpa_psk"));
    }
    SAH_TRACEZ_INFO(ME, "%s: sending cmd:%s", pAP->alias, hybridApSsid);
    wld_ap_hostapd_sendCommand(pAP, hybridApSsid, "multi ap backhaul ssid");
    SAH_TRACEZ_INFO(ME, "%s: sending cmd:%s", pAP->alias, hybridApPass);
    wld_ap_hostapd_sendCommand(pAP, hybridApPass, "multi ap backhaul password");

    swl_mapChar_cleanup(pNewVapParams);

    return SWL_RC_OK;
}

/**
 * @brief Request multi AP type update from pwhm state machine 
 *
 * @param pAP accesspoint
 * @return return code of executed action.
 */
swl_rc_ne whm_mxl_updateMultiAp(T_AccessPoint* pAP) {
    T_Radio* pRad = pAP->pRadio;
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "No Radio Mapped");
    mxl_VapVendorData_t* mxlVapVendorData = mxl_vap_getVapVendorData(pAP);
    ASSERTS_NOT_NULL(mxlVapVendorData, SWL_RC_INVALID_PARAM, ME, "mxlVapVendorData is NULL");
    whm_mxl_multi_ap_type_e multiApType = mxlVapVendorData->mxlMultiApType;

    ASSERT_TRUE((multiApType < MXL_MAP_TYPE_MAX), SWL_RC_ERROR, ME, "%s: Invalid Multi AP type(%d)", pAP->alias, multiApType);
    ASSERTI_FALSE((pRad->status == RST_ERROR) || (pRad->status == RST_UNKNOWN), SWL_RC_INVALID_STATE, ME, "%s: Invalid radio state(%d)", pRad->Name, pRad->status);
    ASSERTI_TRUE(wld_wpaCtrlInterface_isReady(pAP->wpaCtrlInterface), SWL_RC_INVALID_STATE, ME, "%s: wpaCtrl disconnected", pAP->alias);

    /* Update generic multi ap params */
    wld_secDmn_action_rc_ne rc = wld_ap_hostapd_setNoSecParams(pAP);
    ASSERT_FALSE(rc < SECDMN_ACTION_OK_DONE, SWL_RC_ERROR, ME, "%s: fail to set common params", pAP->alias);

    switch (multiApType) {
        case MXL_BACKHAUL_MAP: {
            s_bApSetParams(pAP);
            break;
        }
        case MXL_HYBRID_MAP: {
            s_hybridApSetParams(pAP);
            break;
        }
        case MXL_FRONTHAUL_MAP:
            /* Fallthrough */
        case MXL_MAP_OFF:
            break;
        default:
            break;
    }
    whm_mxl_toggleHapd(pRad);
    return SWL_RC_OK;
}

swl_rc_ne whm_mxl_handleMbssidOverride(T_Radio* pRad, bool overideMbssid) {
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "pRad is NULL");
    ASSERT_FALSE((pRad->status == RST_ERROR) || (pRad->status == RST_UNKNOWN), SWL_RC_OK, ME, "%s: Invalid radio state(%d)", pRad->Name, pRad->status);
    T_AccessPoint* masterVap = wld_rad_getFirstVap(pRad);
    ASSERT_NOT_NULL(masterVap, SWL_RC_INVALID_PARAM, ME, "masterVap is NULL");
    if ((pRad->status == RST_ERROR) || (pRad->status == RST_UNKNOWN)) {
        return SWL_RC_OK;
    }
    if (wld_wpaCtrlInterface_isReady(masterVap->wpaCtrlInterface)) {
        wld_ap_hostapd_setParamValue(masterVap, "override_6g_mbssid_default_mode", (overideMbssid ? "1" : "0"), "override 6G mbssid");
        /* Reset multibss_enable in hostapd after setting/resetting 6G mbssid override and let hostapd manage */
        wld_ap_hostapd_setParamValue(masterVap, "multibss_enable", "0", "disable multibss");
    }
    whm_mxl_toggleHapd(pRad);
    return SWL_RC_OK;
}

swl_rc_ne whm_mxl_updateSsidAdvertisement(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, SWL_RC_INVALID_PARAM, ME, "pAP is NULL");
    T_Radio* pRad = pAP->pRadio;
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "pRad is NULL");
    ASSERT_FALSE((pRad->status == RST_ERROR) || (pRad->status == RST_UNKNOWN), SWL_RC_INVALID_STATE, ME, "%s: Invalid radio state(%d)", pRad->Name, pRad->status);
    ASSERT_TRUE(wld_secDmn_isAlive(pRad->hostapd), SWL_RC_ERROR, ME, "hostapd not active");
    /* Set new hidden ssid mode */
    SAH_TRACEZ_NOTICE(ME, "%s: ssid advertisement mode changed - toggling interface", pAP->alias);
    wld_ap_hostapd_setParamValue(pAP, "ignore_broadcast_ssid", pAP->SSIDAdvertisementEnabled ? "0" : "2", "update ssid advertisement mode");
    return whm_mxl_toggleHapd(pRad);
}

swl_rc_ne whm_mxl_hiddenSsidUpdate(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, SWL_RC_INVALID_PARAM, ME, "pAP is NULL");
    T_Radio* pRad = pAP->pRadio;
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "pRad is NULL");
    ASSERT_FALSE((pRad->status == RST_ERROR) || (pRad->status == RST_UNKNOWN), SWL_RC_INVALID_STATE, ME, "%s: Invalid radio state(%d)", pRad->Name, pRad->status);
    T_SSID* pSSID = (T_SSID*) pAP->pSSID;
    ASSERTI_NOT_NULL(pSSID, SWL_RC_ERROR, ME, "pSSID is NULL");
    ASSERT_TRUE((!pAP->SSIDAdvertisementEnabled), SWL_RC_INVALID_STATE, ME, "%s: hidden ssid disabled no update needed", pAP->alias);
    ASSERT_TRUE(wld_secDmn_isAlive(pRad->hostapd), SWL_RC_ERROR, ME, "hostapd not active");
    /* Update hidden SSID */
    SAH_TRACEZ_NOTICE(ME, "%s: Hidden ssid enabled - updating ssid and toggling interface", pAP->alias);
    wld_ap_hostapd_setParamValue(pAP, "ssid", pSSID->SSID, "set new hidden ssid");
    wld_ap_hostapd_reloadSecKey(pAP, "reload hidden ssid sec key");
    return whm_mxl_toggleHapd(pRad);
}

/**
 * @brief wraper for the generic pwhm send hostapd command
 *
 * @param pAP accesspoint
 * @param cmd the command to send
 * @param reason the command caller
 * @return true when the SET cmd is executed successfully. Otherwise false.
 */
bool whm_mxl_hostapd_sendCommand(T_AccessPoint* pAP, char* cmd, const char* reason) {
    ASSERT_NOT_NULL(pAP, SWL_RC_INVALID_PARAM, ME, "No pAP Mapped");
    return wld_ap_hostapd_sendCommand(pAP, cmd, reason);
}

/**
 * @brief Send hostapd command and check if the response includes specific string
 *
 * @param pAP accesspoint
 * @param cmd the command to send
 * @param reason the command caller
 * @param expectedResponse the specific string to check if included in the response from hostapd
 * @return true when the SET cmd is executed successfully. Otherwise false.
 */
bool whm_mxl_wpaCtrl_sendCmdCheckSpecificResponse(T_AccessPoint* pAP, char* cmd, const char* reason, char* expectedResponse) {
    ASSERTS_NOT_NULL(pAP, false, ME, "NULL");
    ASSERTS_TRUE(wld_wpaCtrlInterface_isReady(pAP->wpaCtrlInterface), false, ME, "%s: wpactrl link not ready", pAP->alias);
    SAH_TRACEZ_INFO(ME, "%s: send hostapd cmd %s for %s",
                    wld_wpaCtrlInterface_getName(pAP->wpaCtrlInterface), cmd, reason);
    char reply[MSG_LENGTH] = {'\0'};
    // send the command
    ASSERTS_TRUE(wld_wpaCtrl_sendCmdSynced(pAP->wpaCtrlInterface, cmd, reply, sizeof(reply) - 1), false, ME, "sending cmd %s failed", cmd);
    SAH_TRACEZ_INFO(ME, "Hostapd cmd reply is %s", reply);
    // check the response
    ASSERT_TRUE(swl_str_nmatches(reply, expectedResponse, strlen(expectedResponse)), false, ME, "cmd(%s) reply(%s): unmatch expect(%s)", cmd, reply, expectedResponse);

    return true;
}
