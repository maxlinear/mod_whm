/******************************************************************************

         Copyright (c) 2023 - 2025, MaxLinear, Inc.

  This software may be distributed under the terms of the BSD license.
  See README for more details.

*******************************************************************************/

/*  *****************************************************************************
*         File Name    : whm_mxl_evt.c                                         *
*         Description  : Vendor events callback                                *
*                                                                              *
*  *****************************************************************************/

#include "swl/swl_common.h"
#include <swla/swla_mac.h>
#include "swla/swla_chanspec.h"

#include "wld/wld_radio.h"
#include "wld/wld_nl80211_compat.h"
#include "wld/wld_nl80211_api.h"
#include "wld/wld_nl80211_attr.h"
#include "wld/wld_nl80211_events.h"
#include "wld/wld_chanmgt.h"
#include "wld/wld_util.h"

#include "whm_mxl_utils.h"
#include <vendor_cmds_copy.h>

#include "whm_mxl_rad.h"
#include "whm_mxl_parser.h"
#include "whm_mxl_monitor.h"
#include "whm_mxl_evt.h"

#define ME "mxlEvt"

static void s_updateNewChanspec(T_Radio* pRad, swl_chanspec_t* pChanSpec, wld_channelChangeReason_e reason) {
    ASSERTS_NOT_NULL(pRad, , ME, "NULL");
    ASSERTS_NOT_NULL(pChanSpec, , ME, "NULL");
    ASSERTS_TRUE(pChanSpec->channel > 0, , ME, "invalid chan");
    swl_rc_ne rc = wld_chanmgt_reportCurrentChanspec(pRad, *pChanSpec, reason);
    ASSERTS_FALSE(rc < SWL_RC_OK, , ME, "no changes to be reported");
    wld_rad_updateOperatingClass(pRad);
}

static void s_vendorEvtCb(void* pRef, void* pData _UNUSED, struct nlmsghdr* nlh, struct nlattr* tb[]) {
    T_Radio* pRad = (T_Radio*) pRef;
    ASSERT_NOT_NULL(pRad, , ME, "pRad NULL");
    ASSERT_NOT_NULL(tb, , ME, "tb NULL");

    int64_t subcmd = -1;

    SAH_TRACEZ_INFO(ME, "%s: treat vendor event data", pRad->Name);

    struct genlmsghdr* gnlh = (struct genlmsghdr*) nlmsg_data(nlh);
    ASSERT_EQUALS(gnlh->cmd, NL80211_CMD_VENDOR, , ME, "unexpected cmd %d", gnlh->cmd);

    SAH_TRACEZ_INFO(ME, "%s: nlh->nlmsg_len %d", pRad->Name, nlh->nlmsg_len);

    if(tb[NL80211_ATTR_VENDOR_ID]) {
        uint32_t vendorId = nla_get_u32(tb[NL80211_ATTR_VENDOR_ID]);
        SAH_TRACEZ_INFO(ME, "%s: vendorId 0x%04x", pRad->Name, vendorId);
    }

    if(tb[NL80211_ATTR_VENDOR_SUBCMD]) {
        subcmd = nla_get_u32(tb[NL80211_ATTR_VENDOR_SUBCMD]);
        SAH_TRACEZ_INFO(ME, "%s: subcmd %"PRId64"", pRad->Name, subcmd);
    }

    switch(subcmd) {
    case LTQ_NL80211_VENDOR_EVENT_UNCONNECTED_STA: {
        SAH_TRACEZ_INFO(ME, "%s: parse NaSta event", pRad->Name);
        if(mxl_parseNaStaStats(pRad, tb, NASTA_STATS_REQ_ASYNC, true) == SWL_RC_OK) {
            whm_mxl_monitor_checkRunNaStaList(pRad);
        }
        break;
    }
    case LTQ_NL80211_VENDOR_EVENT_CHAN_DATA: {
        SAH_TRACEZ_INFO(ME, "%s: received chan data event", pRad->Name);
        mxl_parseChanDataEvt(pRad, tb);
        break;
    }
    case LTQ_NL80211_VENDOR_EVENT_CSI_STATS: {
        SAH_TRACEZ_INFO(ME, "%s: received csi stats event", pRad->Name);
        mxl_parseCsiStatsEvt(pRad, tb);
        break;
    }
    default: {
        SAH_TRACEZ_INFO(ME, "%s: unknown vendor event %"PRId64"", pRad->Name, subcmd);
        break;
    }
    }
}

static T_Radio* s_mxl_fetchRadio(void* userData, char* ifname) {
    if (!swl_str_isEmpty(ifname)) {
        T_AccessPoint* pAP = wld_vap_from_name(ifname);
        if (pAP != NULL) {
            if (pAP->pRadio != NULL) {
                return pAP->pRadio;
            }
        } else {
            T_EndPoint* pEP = wld_vep_from_name(ifname);
            if (pEP != NULL) {
                if (pEP->pRadio != NULL) {
                    return pEP->pRadio;
                }
            }
        }
    }

    if (debugIsRadPointer(userData))
        return (T_Radio*) userData;

    return NULL;
}

static void mxl_6ghz_chanspec_from_centreFreq(swl_chanspec_t* chanSpec, int32_t centrFreq) {
    int32_t centrChannel = 0;

    /*Fetching the centre channel from the centre freq received from the ACS-COMPLETED event*/
    centrChannel = (centrFreq - SWL_CHANNEL_6G_FREQ_START) / SWL_CHANNEL_INTER_FREQ_5MHZ;

    /**
    * Updating the extensionHigh variable in the swl_chanspec_t structure
    * Based on the centre channel in case of 320MHz
    **/
    if(chanSpec->bandwidth == SWL_BW_320MHZ) {
        if(((centrChannel - 63) % 64) == 0) {
            chanSpec->extensionHigh = SWL_CHANSPEC_EXT_HIGH;
        } else {
            chanSpec->extensionHigh = SWL_CHANSPEC_EXT_LOW;
        }
    }
}

static void s_mxl_ACSCompletedEvt(void* userData, char* ifName, char* event _UNUSED, char* params) {
    T_Radio* pRad = s_mxl_fetchRadio(userData, ifName);
    ASSERT_NOT_NULL(pRad, , ME, "Could not get radio from ifname(%s)", ifName);
    ASSERTI_NOT_EQUALS(pRad, mxl_rad_getZwDfsRadio(), , ME, "ignore acs event on zwdfs radio (ifname:%s, radName:%s)",
                       ifName, pRad->Name);

    // expected msg format:
    // <3>ACS-COMPLETED freq=2412 channel=1 OperatingChannelBandwidt=20 ExtensionChannel=0 cf1=2412 cf2=0 reason=UNKNOWN dfs_chan=0
    swl_chanspec_t chanSpec = SWL_CHANSPEC_EMPTY;
    int32_t freq = 0;
    int32_t channel = 0;
    int32_t operCbw = 0;
    int32_t centreFreq = 0;

    ASSERTW_EQUALS(pRad->autoChannelEnable, true, , ME, "%s: ACS event when ACS not enabled", pRad->Name);

    chanSpec.band = pRad->operatingFrequencyBand;
    if (!wld_wpaCtrl_getValueIntExt(params, "freq", &freq)) {
        SAH_TRACEZ_ERROR(ME, "%s: cannot get frequency", pRad->Name);
        return;
    }

    swl_chanspec_channelFromMHz(&chanSpec, freq);
    ASSERT_EQUALS(pRad->operatingFrequencyBand, chanSpec.band, , ME, "%s: unmatched radio freqBand(%s)", pRad->Name, swl_freqBandExt_str[chanSpec.band]);

    if (!wld_wpaCtrl_getValueIntExt(params, "channel", &channel) || !channel) {
        SAH_TRACEZ_ERROR(ME, "%s: cannot get channel", pRad->Name);
        return;
    }

    if (!wld_wpaCtrl_getValueIntExt(params, "OperatingChannelBandwidt", &operCbw) || !operCbw) {
        SAH_TRACEZ_ERROR(ME, "%s: cannot get bandwidth", pRad->Name);
        return;
    }

    if (!wld_wpaCtrl_getValueIntExt(params, "cf1", &centreFreq)) {
        SAH_TRACEZ_ERROR(ME, "%s: cannot get frequency", pRad->Name);
        return;
    }

    chanSpec.bandwidth = swl_chanspec_intToBw(operCbw);
    chanSpec.channel = channel;

    if(wld_rad_is_6ghz(pRad)) {
        /* Updating the chanspec in case of 320MHZ based on centre frequency*/
        mxl_6ghz_chanspec_from_centreFreq(&chanSpec, centreFreq);
    }

    /* Update all channel instances with new channel in case ACS enabled, otherwise
     * manual channel switch to different channel later (without disabling ACS prior)
     * may not work as expected */
    SAH_TRACEZ_INFO(ME, "%s: ACS-COMPLETED Updating current chanspec %s", pRad->Name, swl_typeChanspecExt_toBuf32(chanSpec).buf);
    s_updateNewChanspec(pRad, &chanSpec, CHAN_REASON_AUTO);
}

/*
 * Update main 5GHz RadioStatus when receiving DFS CAC events on the zwdfs radio interface
 * - zwdfs CAC started   => notify BG DFS clear started and set ChannelMgt RadioStatus to BG_CAC/BG_CAC_NS
 * - zwdfs CAC completed => notify BG DFS clear ended and set ChannelMgt RadioStatus to Up
 */
static void s_mxl_DfsCacEvts(void* userData _UNUSED, char* ifName, char* event, char* params) {
    T_Radio* pRadZwDfs = mxl_rad_getZwDfsRadio();
    ASSERTS_NOT_NULL(pRadZwDfs, , ME, "NULL");
    ASSERTS_TRUE(swl_str_matches(ifName, pRadZwDfs->Name), ,ME, "%s: not zwdfs radio", ifName);

    T_Radio* pRad5GHzData = wld_getRadioByFrequency(SWL_FREQ_BAND_5GHZ);
    ASSERT_NOT_NULL(pRad5GHzData, , ME, "NULL");

    if(swl_str_matches(event, "DFS-CAC-START")) {
        ASSERTI_TRUE((wld_rad_isUpAndReady(pRad5GHzData) && !wld_rad_isDoingDfsScan(pRad5GHzData)), ,ME,
                      "%s: not ready", pRad5GHzData->Name);
        wld_bgdfs_notifyClearStarted(pRad5GHzData, pRadZwDfs->targetChanspec.chanspec.channel,
                                     pRadZwDfs->targetChanspec.chanspec.bandwidth, BGDFS_TYPE_CLEAR);
        if(pRad5GHzData->channel != pRad5GHzData->currentChanspec.chanspec.channel) {
            pRad5GHzData->detailedState = CM_RAD_BG_CAC;
        } else {
            pRad5GHzData->detailedState = CM_RAD_BG_CAC_NS;
        }
    } else if(swl_str_matches(event, "DFS-CAC-COMPLETED")) {
        ASSERTI_TRUE(((pRad5GHzData->detailedState  == CM_RAD_BG_CAC) || (pRad5GHzData->detailedState  == CM_RAD_BG_CAC_NS)), ,ME,
                      "%s: detailedState %s", pRad5GHzData->Name, cstr_chanmgt_rad_state[pRad5GHzData->detailedState]);
        bool success = wld_wpaCtrl_getValueInt(params, "success");
        wld_bgdfs_notifyClearEnded(pRad5GHzData, (success ? DFS_RESULT_OK : DFS_RESULT_OTHER));
        pRad5GHzData->detailedState = CM_RAD_UP;
    }
    wld_rad_updateState(pRad5GHzData, false);
}

SWL_TABLE(mxl_WpaCtrlEvents,
          ARR(char* evtName; void* evtParser; ),
          ARR(swl_type_charPtr, swl_type_voidPtr),
          ARR(
              {"ACS-COMPLETED", &s_mxl_ACSCompletedEvt},
              {"DFS-CAC-START", &s_mxl_DfsCacEvts},
              {"DFS-CAC-COMPLETED", &s_mxl_DfsCacEvts},
              ));

static evtParser_f s_mxl_getEventParser(char* eventName) {
    evtParser_f* pfEvtHdlr = (evtParser_f*) swl_table_getMatchingValue(&mxl_WpaCtrlEvents, 1, 0, eventName);
    ASSERTS_NOT_NULL(pfEvtHdlr, NULL, ME, "no handler defined for evt(%s)", eventName);
    return *pfEvtHdlr;
}

static void s_mxl_WpaCtrlEvtMsg(void* userData, char* ifName, char* msgData) {
    ASSERTS_STR(msgData, , ME, "NULL or no content msgData");
    char* pEvent = strstr(msgData, WPA_MSG_LEVEL_INFO);
    ASSERTS_NOT_NULL(pEvent, , ME, "Not a valid WPA ctrl event");
    pEvent += sizeof(WPA_MSG_LEVEL_INFO) - 1;

    uint32_t eventNameLen = strlen(pEvent);
    char* pParams = strchr(pEvent, ' ');
    if (pParams) {
        eventNameLen = pParams - pEvent;
        pParams++;
    }
    char eventName[eventNameLen + 1];
    swl_str_copy(eventName, sizeof(eventName), pEvent);
    evtParser_f fEvtHdlr = s_mxl_getEventParser(eventName);
    ASSERTS_NOT_NULL(fEvtHdlr, , ME, "%s: No parser for evt(%s)", ifName, eventName);

    SAH_TRACEZ_INFO(ME, "%s: receive msg '%s'", ifName, msgData);
    fEvtHdlr(userData, ifName, eventName, pParams);
}

static void s_mxl_ObssCoexBwChngd(T_Radio* pRad, uint32_t channel, uint32_t operCbw) {
    ASSERT_NOT_NULL(pRad, , ME, "pRad is NULL");
    swl_chanspec_t newChanSpec = SWL_CHANSPEC_EMPTY;
    ASSERT_TRUE(wld_rad_is_24ghz(pRad), , ME, "%s: Received OBSS BW change event for wrong band", pRad->Name);
    pRad->targetChanspec.reason = CHAN_REASON_OBSS_COEX;
    pRad->obssCoexistenceActive = true;
    /* Prepare and update new chanspec */
    ASSERT_TRUE(wld_rad_hasChannel(pRad, channel), , ME, "%s: Bad channel(%d) from parsed event", pRad->Name, channel);
    newChanSpec.bandwidth = swl_chanspec_intToBw(operCbw);
    newChanSpec.channel = channel;
    SAH_TRACEZ_NOTICE(ME, "%s: Update chanspec due to 20/40 coexistence on channel(%d)", pRad->Name, channel);
    s_updateNewChanspec(pRad, &newChanSpec, CHAN_REASON_OBSS_COEX);
}

static void s_mxl_apBwChanged(void* userData, char* ifName _UNUSED, char* event _UNUSED, char* params _UNUSED) {
    /* Expected msg format:
     * <3>AP-BW-CHANGED freq=%d Channel=%d OperatingChannelBandwidth=%d ExtensionChannel=%d cf1=%d cf2=%d reason=%s dfs_chan=%d
     */
    T_Radio* pRad = (T_Radio*) userData;
    uint32_t channel = wld_wpaCtrl_getValueInt(params, "Channel");
    uint32_t operCbw = wld_wpaCtrl_getValueInt(params, "OperatingChannelBandwidth");
    char reason[64] = {0};
    if (wld_wpaCtrl_getValueStr(params, "reason", reason, sizeof(reason)) > 0) {
        if (swl_str_matches(reason, "OBSS")) {
            s_mxl_ObssCoexBwChngd(pRad, channel, operCbw);
        }
    }
}

SWL_TABLE(mxl_CustomWpaCtrlEvents,
          ARR(char* evtName; void* evtParser; ),
          ARR(swl_type_charPtr, swl_type_voidPtr),
          ARR(
              {"AP-BW-CHANGED", &s_mxl_apBwChanged},
              ));

static evtParser_f s_mxl_getCustomEventParser(char* eventName) {
    evtParser_f* pfEvtHdlr = (evtParser_f*) swl_table_getMatchingValue(&mxl_CustomWpaCtrlEvents, 1, 0, eventName);
    ASSERTS_NOT_NULL(pfEvtHdlr, NULL, ME, "no handler defined for evt(%s)", eventName);
    return *pfEvtHdlr;
}

static swl_rc_ne s_mxl_WpaCustomCtrlEvtMsg(void* userData, char* ifName, char* msgData) {
    ASSERTS_STR(msgData, SWL_RC_ERROR, ME, "NULL or no content msgData");
    char* pEvent = strstr(msgData, WPA_MSG_LEVEL_INFO);
    ASSERTS_NOT_NULL(pEvent, SWL_RC_ERROR, ME, "Not a valid WPA ctrl event");
    pEvent += sizeof(WPA_MSG_LEVEL_INFO) - 1;
    uint32_t eventNameLen = strlen(pEvent);
    char* pParams = strchr(pEvent, ' ');
    if (pParams) {
        eventNameLen = pParams - pEvent;
        pParams++;
    }
    char eventName[eventNameLen + 1];
    swl_str_copy(eventName, sizeof(eventName), pEvent);
    evtParser_f fEvtHdlr = s_mxl_getCustomEventParser(eventName);
    ASSERTS_NOT_NULL(fEvtHdlr, SWL_RC_ERROR, ME, "%s: No parser for evt(%s)", ifName, eventName)
    SAH_TRACEZ_INFO(ME, "%s: receive msg '%s'", ifName, msgData);
    fEvtHdlr(userData, ifName, eventName, pParams);
    return SWL_RC_OK;
}

static swl_rc_ne s_mxl_setRadioWpaCtrlEvtHandlers(T_Radio* pRad) {
    void* userdata = NULL;
    wld_wpaCtrl_radioEvtHandlers_cb handlers = {0};

    ASSERT_NOT_NULL(pRad->hostapd, SWL_RC_INVALID_PARAM, ME, "hostapd is NULL");
    ASSERT_NOT_NULL(pRad->hostapd->wpaCtrlMngr, SWL_RC_INVALID_PARAM, ME, "wpaCtrlMngr is NULL");

    if (!wld_wpaCtrlMngr_getEvtHandlers(pRad->hostapd->wpaCtrlMngr, &userdata, &handlers)) {
        SAH_TRACEZ_ERROR(ME, "%s: Failed to get event handlers", pRad->Name);
        return SWL_RC_ERROR;
    }

    if (userdata == NULL)
        userdata = pRad;

    handlers.fProcEvtMsg = s_mxl_WpaCtrlEvtMsg;
    handlers.fCustProcEvtMsg = s_mxl_WpaCustomCtrlEvtMsg;
    if (!wld_wpaCtrlMngr_setEvtHandlers(pRad->hostapd->wpaCtrlMngr, userdata, &handlers)) {
        SAH_TRACEZ_ERROR(ME, "%s: Failed to set event handlers", pRad->Name);
        return SWL_RC_ERROR;
    }

    return SWL_RC_OK;
}

swl_rc_ne mxl_evt_setVendorEvtHandlers(T_Radio* pRad) {
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "pRad NULL");

    /*
     * set the nl80211 vendor event handler
     * after nl80211Listener is created (ie when radio wiphyId is known: after successful wrad_support)
     */
    if (pRad->nl80211Listener != NULL) {
        wld_nl80211_addVendorEvtListener(wld_nl80211_getSharedState(), pRad->nl80211Listener, s_vendorEvtCb);
    }

    s_mxl_setRadioWpaCtrlEvtHandlers(pRad);
    return SWL_RC_OK;
}
