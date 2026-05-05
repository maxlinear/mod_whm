/******************************************************************************

         Copyright (c) 2023 - 2025 MaxLinear, Inc.

  This software may be distributed under the terms of the BSD license.
  See README for more details.

*******************************************************************************/

/*  *****************************************************************************
*         File Name    : whm_mxl_mlo.c                                         *
*         Description  : MLO related API                                       *
*                                                                              *
*  *****************************************************************************/

#include <swl/swl_common.h>
#include <swl/swl_string.h>
#include <swl/map/swl_mapCharFmt.h>
#include <swla/swla_mac.h>

#include "wld/wld.h"
#include "wld/wld_util.h"
#include "wld/wld_radio.h"
#include "wld/wld_accesspoint.h"
#include "wld/wld_ssid.h"
#include "wld/wld_mld.h"
#include "wld/wld_nl80211_compat.h"
#include "wld/wld_nl80211_api.h"
#include "wld/wld_ap_nl80211.h"
#include "wld/wld_hostapd_ap_api.h"
#include "wld/wld_rad_hostapd_api.h"

#include "whm_mxl_utils.h"
#include "whm_mxl_vap.h"
#include "whm_mxl_rad.h"
#include "whm_mxl_cfgActions.h"
#include "whm_mxl_hostapd_cfg.h"
#include "whm_mxl_mlo.h"
#include "whm_mxl_nl80211.h"

#include <vendor_cmds_copy.h>

#define ME "mxlMlo"

#define MLO_COMMIT_DELAY_DEFAULT_MS 5000
#define MLO_COMMIT_BOOT_DELAY_DEFAULT_MS 10000

static whm_mxl_mld_mngr_t mldMngr = {.init = false};

static const char* s_mldUnusableReason_str[] = {
    "InvalidType",
    "InvalidMldMac",
    "LinkDisabled",
    "LinkNotBE",
    "SSIDMismatch",
    "SharedSecMismatch",
    "InternalError"
};
SWL_ASSERT_STATIC(SWL_ARRAY_SIZE(s_mldUnusableReason_str) == MLD_UNUSABLE_MAX,
                   "s_mldUnusableReason_str not correctly defined");

static bool s_unusableReasonMaskToString(char* buffer, size_t bufferSize,
                                             whm_mxl_mld_unusable_reason_m mask) {
    ASSERT_NOT_NULL(buffer, false, ME, "NULL");
    ASSERT_TRUE(bufferSize > 0, false, ME, "Empty");
    buffer[0] = '\0';

    for (size_t i = 0; i < (size_t)MLD_UNUSABLE_MAX; i++) {
        if (SWL_BIT_IS_SET(mask, i)) {
            swl_strlst_cat(buffer, bufferSize, ",", s_mldUnusableReason_str[i]);
        }
    }
    return true;
}

static swl_rc_ne s_doMloCommit(void) {
    T_Radio* pRad = NULL;
    swl_rc_ne rc = SWL_RC_NOT_AVAILABLE;
    /* Shecudle restart on first found Radio */
    wld_for_eachRad(pRad) {
        if (pRad) {
            SAH_TRACEZ_INFO(ME, "MLO: Schedule restart from radio %s", pRad->Name);
            rc = whm_mxl_restartHapd(pRad);
            break;
        }
    }
    ASSERT_TRUE((rc == SWL_RC_OK), rc, ME, "MLO: No radio found to schedule restart");
    return rc;
}

static void s_mloCommit_th(amxp_timer_t* timer _UNUSED, void* userdata) {
    whm_mxl_mld_mngr_t* pMldMngr = (whm_mxl_mld_mngr_t*) userdata;
    ASSERT_NOT_NULL(pMldMngr, , ME, "pMldMngr is NULL");
    SAH_TRACEZ_INFO(ME, "MLO: Commit timer expired, executing pending MLO commit action");
    s_doMloCommit();
}

static void s_mloStartCommit(void) {
    whm_mxl_mld_mngr_t* pMldMngr = whm_mxl_mlo_get_mldMngr();
    ASSERT_TRUE(pMldMngr->init, , ME, "mldMngr not initialized");
    amxp_timer_t* pTimer = NULL;
    swl_timeSpecMono_t time;
    swl_timespec_getMono(&time);
    swl_timeSpecMono_t* initTime = wld_getInitTime();

    pTimer = pMldMngr->mloCommitMngr.commitTimer;
    if (!pTimer || !pMldMngr->mloCommitMngr.enable) {
        s_doMloCommit();
        return;
    }

    amxp_timer_state_t state = pTimer->state;
    if ((state == amxp_timer_running) || (state == amxp_timer_started)) {
        amxp_timer_stop(pTimer);
        SAH_TRACEZ_INFO(ME, "MLO: Commit timer already running, restarting it");
    }

    int64_t mSecSinceInit = swl_timespec_diffToMillisec(initTime, &time);
    uint32_t minDelay = 0;
    if ((mSecSinceInit > 0 ) && (mSecSinceInit < pMldMngr->mloCommitMngr.bootDelay)) {
        minDelay = pMldMngr->mloCommitMngr.bootDelay - (uint32_t) mSecSinceInit;
    }
    uint32_t finalDelay = SWL_MAX(pMldMngr->mloCommitMngr.delay, minDelay);
    SAH_TRACEZ_INFO(ME, "MLO: Starting MLO commit timer with delay %u", finalDelay);
    amxp_timer_start(pTimer, finalDelay);
}

static void s_setCommitMngrDefaults(whm_mxl_mld_mngr_t* pMldMngr) {
    ASSERT_NOT_NULL(pMldMngr, , ME, "pMldMngr is NULL");
    pMldMngr->mloCommitMngr.commitTimer = NULL;
    pMldMngr->mloCommitMngr.enable = true;
    pMldMngr->mloCommitMngr.delay = MLO_COMMIT_DELAY_DEFAULT_MS;
    pMldMngr->mloCommitMngr.bootDelay = MLO_COMMIT_BOOT_DELAY_DEFAULT_MS;
}

whm_mxl_mld_t* whm_mxl_mlo_getMldVap(T_AccessPoint* pAP) {
    mxl_VapVendorData_t* pVapVendor = mxl_vap_getVapVendorData(pAP);
    ASSERT_NOT_NULL(pVapVendor, NULL, ME, "VapVendorData is NULL");
    whm_mxl_mld_t *pMld = pVapVendor->pMld;
    ASSERTI_NOT_NULL(pMld, NULL, ME, "pMld is NULL");
    return pMld; 
}

whm_mxl_link_t* whm_mxl_mlo_getMldLink(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, NULL, ME, "pAP is NULL");
    mxl_VapVendorData_t* pVapVendor = mxl_vap_getVapVendorData(pAP);
    ASSERT_NOT_NULL(pVapVendor, NULL, ME, "VapVendorData is NULL");
    whm_mxl_link_t *pLink = pVapVendor->pLink;
    ASSERTI_NOT_NULL(pLink, NULL, ME, "pLink is NULL");
    return pLink;
}

whm_mxl_link_t* whm_mxl_mlo_getFirstLink(whm_mxl_mld_t* pMld) {
    ASSERT_NOT_NULL(pMld, NULL, ME, "pMld is NULL");
    amxc_llist_it_t* it = amxc_llist_get_first(&pMld->affiliatedLinks);
    ASSERTS_NOT_NULL(it, NULL, ME, "Empty");
    whm_mxl_link_t *pLink = (whm_mxl_link_t *)amxc_llist_it_get_data(it, whm_mxl_link_t, it);
    ASSERT_NOT_NULL(pLink, NULL, ME, "pLink is NULL");
    return pLink;
}

whm_mxl_link_t* whm_mxl_mlo_getNextLink(whm_mxl_link_t* pLink) {
    ASSERT_NOT_NULL(pLink, NULL, ME, "pLink is NULL");
    amxc_llist_it_t* it = amxc_llist_it_get_next(&pLink->it);
    ASSERTS_NOT_NULL(it, NULL, ME, "Last");
    whm_mxl_link_t* pNextLink = (whm_mxl_link_t *)amxc_llist_it_get_data(it, whm_mxl_link_t, it);
    ASSERT_NOT_NULL(pNextLink, NULL, ME, "pNextLink is NULL");
    return pNextLink;
}

whm_mxl_mld_t* whm_mxl_mlo_getFirstMld(whm_mxl_mld_mngr_t* pMldMngr) {
    ASSERT_NOT_NULL(pMldMngr, NULL, ME, "pMldMngr is NULL");
    amxc_llist_it_t* it = amxc_llist_get_first(&pMldMngr->mlds);
    ASSERTS_NOT_NULL(it, NULL, ME, "Empty");
    whm_mxl_mld_t *pMld = (whm_mxl_mld_t *)amxc_llist_it_get_data(it, whm_mxl_mld_t, it);
    ASSERT_NOT_NULL(pMld, NULL, ME, "pMld is NULL");
    return pMld;
}

whm_mxl_mld_t* whm_mxl_mlo_getNextMld(whm_mxl_mld_t* pMld) {
    ASSERT_NOT_NULL(pMld, NULL, ME, "pMld is NULL");
    amxc_llist_it_t* it = amxc_llist_it_get_next(&pMld->it);
    ASSERTS_NOT_NULL(it, NULL, ME, "Last");
    whm_mxl_mld_t* pNextMldVap = (whm_mxl_mld_t *)amxc_llist_it_get_data(it, whm_mxl_mld_t, it);
    ASSERT_NOT_NULL(pNextMldVap, NULL, ME, "pNextMldVap is NULL");
    return pNextMldVap;
}

static whm_mxl_mld_t* s_getMldVap(int32_t mloId) {
    whm_mxl_mld_mngr_t* pMldMngr = whm_mxl_mlo_get_mldMngr();
    ASSERT_TRUE(pMldMngr->init, NULL, ME, "mldMngr not initialized");
    whm_mxl_mld_t *pMld = NULL;

    whm_mxl_mlo_forEachMldVap(pMld, pMldMngr) {
        if (pMld->mloId == mloId) {
            return pMld;
        }
    }

    return NULL;
}

static bool s_checkVendorSecSharedConfig(T_AccessPoint* pAP1, T_AccessPoint* pAP2)
{
    ASSERT_NOT_NULL(pAP1, false, ME, "pAP1 is NULL");
    ASSERT_NOT_NULL(pAP2, false, ME, "pAP2 is NULL");
    mxl_VapVendorData_t* pVapVendorAp1 = mxl_vap_getVapVendorData(pAP1);
    ASSERT_NOT_NULL(pVapVendorAp1, false, ME, "pVapVendorAp1 is NULL");
    mxl_VapVendorData_t* pVapVendorAp2 = mxl_vap_getVapVendorData(pAP2);
    ASSERT_NOT_NULL(pVapVendorAp2, false, ME, "pVapVendorAp2 is NULL");

    if (pVapVendorAp1->EnableWPA3PersonalCompatibility != pVapVendorAp2->EnableWPA3PersonalCompatibility) {
        SAH_TRACEZ_INFO(ME, "MLO: WPA3 Personal Compatibility Mismatch between links (%s: %d & %s: %d)",
                        pAP1->alias, pVapVendorAp1->EnableWPA3PersonalCompatibility,
                        pAP2->alias, pVapVendorAp2->EnableWPA3PersonalCompatibility);
        return false;
    }
    if (pVapVendorAp1->disableBeaconProt != pVapVendorAp2->disableBeaconProt) {
        SAH_TRACEZ_INFO(ME, "MLO: Disable Beacon Protection Mismatch between links (%s: %d & %s: %d)",
                        pAP1->alias, pVapVendorAp1->disableBeaconProt,
                        pAP2->alias, pVapVendorAp2->disableBeaconProt);
        return false;
    }
    return true;
}

/**
 * @brief Evaluate MLD usability without modifying any stored state.
 *
 * Checks all conditions that determine whether an MLD can operate:
 *   - MLD type must be DUAL_LINK or TRI_LINK (not SINGLE_LINK or NONE)
 *   - AP MLD MAC must be set (non-null)
 *   - All affiliated links must be enabled
 *   - All affiliated links must be operating in 802.11 BE mode
 *   - All affiliated links must have matching SSIDs
 *   - All affiliated links must have valid and compatible security configs
 *
 * If a NULL pointer or invalid object is encountered indicating broken
 * internals, returns M_MLD_UNUSABLE_INTERNAL_ERROR.
 *
 * @param[in] pMld Pointer to mod_whm MLD structure
 * @return Bitmap of whm_mxl_mld_unusable_reason_m. Returns MLD_UNUSABLE_NONE (0) if usable.
 */
whm_mxl_mld_unusable_reason_m whm_mxl_mlo_checkMldUsability(whm_mxl_mld_t* pMld) {
    ASSERT_NOT_NULL(pMld, M_MLD_UNUSABLE_INTERNAL_ERROR, ME, "pMld is NULL");
    whm_mxl_mld_unusable_reason_m reasons = MLD_UNUSABLE_NONE;

    if (pMld->mldType == MLD_TYPE_NONE || pMld->mldType == MLD_TYPE_SINGLE_LINK) {
        SAH_TRACEZ_INFO(ME, "MLO: MLD(%d) with Invalid Type %d",
                        pMld->mloId, pMld->mldType);
        W_SWL_BIT_SET(reasons, MLD_UNUSABLE_INVALID_TYPE);
    }
    if (swl_mac_binIsNull(&pMld->apMldMac)) {
        SAH_TRACEZ_INFO(ME, "MLO: MLD(%d) with Null Mac", pMld->mloId);
        W_SWL_BIT_SET(reasons, MLD_UNUSABLE_INVALID_MLD_MAC);
    }

    whm_mxl_link_t* pRefLink = whm_mxl_mlo_getFirstLink(pMld);
    ASSERT_NOT_NULL(pRefLink, M_MLD_UNUSABLE_INTERNAL_ERROR, ME,
                    "MLD(%d) has no links", pMld->mloId);
    whm_mxl_link_t* pLink = NULL;
    whm_mxl_mlo_forEachLinkInMld(pLink, pMld) {
        T_AccessPoint* pCurAP = pLink->pLinkAp;
        ASSERT_NOT_NULL(pCurAP, M_MLD_UNUSABLE_INTERNAL_ERROR, ME,
                        "MLD(%d) pLinkAp is NULL", pMld->mloId);
        T_SSID* pCurSSID = pLink->pLinkSSID;
        ASSERT_NOT_NULL(pCurSSID, M_MLD_UNUSABLE_INTERNAL_ERROR, ME,
                        "MLD(%d) pLinkSSID is NULL", pMld->mloId);
        if(!wld_ap_hasStackEnabled(pCurAP)) {
            SAH_TRACEZ_INFO(ME, "MLO: %s Link Disabled", pCurAP->alias);
            W_SWL_BIT_SET(reasons, MLD_UNUSABLE_LINK_DISABLED);
        }
        if (!whm_mxl_rad_checkForceEnableBe(pCurAP->pRadio)) {
            SAH_TRACEZ_INFO(ME, "MLO: %s Radio not operating in BE",
                            pCurAP->pRadio->Name);
            W_SWL_BIT_SET(reasons, MLD_UNUSABLE_LINK_NOT_BE);
        }

        // Cross-link checks
        if (pLink != pRefLink) {
            T_AccessPoint* pRefAP = pRefLink->pLinkAp;
            T_SSID* pRefSSID = pRefLink->pLinkSSID;
            if (!swl_str_matches(pRefSSID->SSID, pCurSSID->SSID)) {
                SAH_TRACEZ_INFO(ME, "MLO: SSID mismatch between links (%s & %s)",
                                pRefSSID->SSID, pCurSSID->SSID);
                W_SWL_BIT_SET(reasons, MLD_UNUSABLE_SSID_MISMATCH);
            }
            if (!wld_ap_sec_checkSharedSecConfigs(pRefAP, pCurAP)) {
                SAH_TRACEZ_INFO(ME, "MLO: Security mismatch between links (%s & %s)",
                                pRefAP->alias, pCurAP->alias);
                W_SWL_BIT_SET(reasons, MLD_UNUSABLE_SHARED_SEC_MISMATCH);
            }
            if (!s_checkVendorSecSharedConfig(pRefAP, pCurAP)) {
                SAH_TRACEZ_INFO(ME, "MLO: Vendor security mismatch between links (%s & %s)",
                                pRefAP->alias, pCurAP->alias);
                W_SWL_BIT_SET(reasons, MLD_UNUSABLE_SHARED_SEC_MISMATCH);
            }
        }
    }
    return reasons;
}

/**
 * @brief Get the MLD usability status.
 *
 * Returns the isUsable value stored on the MLD.
 *
 * @note Does NOT recompute.
 *
 * @param[in] pMld Pointer to mod_whm MLD structure
 * @return true if MLD is currently marked as usable, false otherwise.
 */
bool whm_mxl_mlo_getMldStatus(whm_mxl_mld_t* pMld) {
    ASSERT_NOT_NULL(pMld, false, ME, "pMld is NULL");
    return pMld->isUsable;
}

/**
 * @brief Recompute and update the stored MLD usability status.
 *
 * Calls checkMldUsability() internally, then updates pMld->isUsable
 * and pMld->reasons with the fresh result.
 *
 * @param[in] pMld Pointer to mod_whm MLD structure
 * @return true if MLD is now usable, false otherwise.
 */
bool whm_mxl_mlo_updateMldStatus(whm_mxl_mld_t* pMld) {
    ASSERT_NOT_NULL(pMld, false, ME, "pMld is NULL");

    whm_mxl_mld_unusable_reason_m newReasons = whm_mxl_mlo_checkMldUsability(pMld);
    bool newUsable = (newReasons == MLD_UNUSABLE_NONE);
    bool oldUsable = pMld->isUsable;

    pMld->isUsable = newUsable;
    pMld->reasons = newReasons;

    if (oldUsable != newUsable) {
        char reasonBuf[256] = {0};
        s_unusableReasonMaskToString(reasonBuf, sizeof(reasonBuf), newReasons);
        SAH_TRACEZ_INFO(ME, "MLD(%d) usability changed: %d -> %d (reasons: %s)",
                        pMld->mloId, oldUsable, newUsable,
                        newUsable ? "none" : reasonBuf);
    }

    return newUsable;
}

/**
 * @brief Check if the MLD config change requires a restart.
 *
 * @param[in] pAP AccessPoint that is part of an MLD
 * @return true if usability state would change (restart needed), false otherwise.
 */
bool whm_mxl_mlo_checkMldConfigChange(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, false, ME, "pAP is NULL");
    whm_mxl_mld_t* pMld = whm_mxl_mlo_getMldVap(pAP);
    ASSERTI_NOT_NULL(pMld, false, ME, "%s is not part of MLD", pAP->alias);

    bool oldStatus = whm_mxl_mlo_getMldStatus(pMld);
    whm_mxl_mld_unusable_reason_m newReasons = whm_mxl_mlo_checkMldUsability(pMld);
    bool newStatus = (newReasons == MLD_UNUSABLE_NONE);

    return (oldStatus != newStatus);
}

/**
 * @brief Request MLD action (evaluate usability change & restart if needed).
 *
 * This is the single entry point for all MLO-triggered restarts.
 * Evaluates usability via whm_mxl_mlo_updateMldStatus() and restarts Hostapd
 * if change is detected.
 *
 * @param[in] pAP         AccessPoint that is part of an MLD
 * @param[in] forceAction If true, bypass secDmn-alive checks and perform the
 *                        restart action.
 */
void whm_mxl_mlo_requestMldAction(T_AccessPoint* pAP, bool forceAction) {
    ASSERT_NOT_NULL(pAP, , ME, "pAP is NULL");
    T_Radio* pRad = pAP->pRadio;
    ASSERT_NOT_NULL(pRad, , ME, "No Radio Mapped");

    whm_mxl_mld_t* pMld = whm_mxl_mlo_getMldVap(pAP);
    ASSERT_NOT_NULL(pMld, , ME, "%s: requestMldAction called but not part of MLD", pAP->alias);

    if (forceAction) {
        whm_mxl_mlo_updateMldStatus(pMld);
        SAH_TRACEZ_INFO(ME, "%s: MLD(%d) force restart", pAP->alias, pMld->mloId);
        s_mloStartCommit();
        return;
    }

    bool oldUsable = whm_mxl_mlo_getMldStatus(pMld);
    bool newUsable = whm_mxl_mlo_updateMldStatus(pMld);

    if (oldUsable == newUsable) {
        SAH_TRACEZ_INFO(ME, "%s: MLD(%d) usability unchanged (%d), no action",
                        pAP->alias, pMld->mloId, oldUsable);
        return;
    } else if (!wld_secDmn_isAlive(pRad->hostapd)) {
        SAH_TRACEZ_INFO(ME, "%s: secDmn is not alive, skip restart", pAP->alias);
        return;
    }

    SAH_TRACEZ_INFO(ME, "%s: MLD(%d) usability changed (%d -> %d), restarting hostapd",
                    pAP->alias, pMld->mloId, oldUsable, newUsable);
    s_mloStartCommit();
}

amxd_object_t* whm_mxl_mlo_getMloObject(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, NULL, ME, "pAP is NULL");
    amxd_object_t* pVendorObj = amxd_object_get(pAP->pBus, "Vendor");
    ASSERT_NOT_NULL(pVendorObj, NULL, ME, "pVendorObj NULL");
    return amxd_object_get(pVendorObj, "MLO");
}

static uint32_t s_getMldVapLinkCount(whm_mxl_mld_t* pMld) {
    ASSERT_NOT_NULL(pMld, 0, ME, "pMld is NULL");
    uint32_t size = amxc_llist_size(&pMld->affiliatedLinks);
    SAH_TRACEZ_INFO(ME, "MLO: MLD(%d) has %u links", pMld->mloId, size);
    return size;
}

static void s_setMldMac(T_AccessPoint* pAP, whm_mxl_mld_t* pMld) {
    ASSERT_NOT_NULL(pAP, , ME, "pAP is NULL");
    ASSERT_NOT_NULL(pMld, , ME, "pMld is NULL");
    swl_macBin_t mldMacBin = SWL_MAC_BIN_NEW();
    char* mldMac = NULL;

    amxd_object_t* pMlObj = whm_mxl_mlo_getMloObject(pAP);
    if (pMlObj) {
        mldMac = amxd_object_get_cstring_t(pMlObj, "ApMldMac", NULL);
        swl_mac_charToBin(&mldMacBin, (swl_macChar_t*) mldMac);
        free(mldMac);
    }

    if (swl_mac_binIsNull(&mldMacBin)) {
        /* If no MLD MAC provided - Use MAC of first created link */
        T_SSID* pSSID = (T_SSID*) pAP->pSSID;
        ASSERT_NOT_NULL(pSSID, , ME, "pSSID is NULL");
        memcpy(pMld->apMldMac.bMac, pSSID->BSSID, SWL_MAC_BIN_LEN);
        SAH_TRACEZ_INFO(ME, "MLO: %s Use link MAC "SWL_MAC_FMT,
                        pAP->alias, SWL_MAC_ARG(pMld->apMldMac.bMac));
    } else {
        /* Copy existing MLD MAC to mld VAP */
        memcpy(pMld->apMldMac.bMac, mldMacBin.bMac, SWL_MAC_BIN_LEN);
        pMld->externalMldMac = true;
        SAH_TRACEZ_INFO(ME, "MLO: %s Use pre-configured MAC "SWL_MAC_FMT,
                        pAP->alias, SWL_MAC_ARG(mldMacBin.bMac));
    }
}

const char* s_mldTypeToStr(whm_mxl_mld_type_e type) {
    switch (type) {
        case MLD_TYPE_NONE: return "None";
        case MLD_TYPE_SINGLE_LINK: return "Single Link MLD";
        case MLD_TYPE_DUAL_LINK: return "Dual Band MLD";
        case MLD_TYPE_TRI_LINK: return "TriBand MLD";
        default: return "Unknown";
    }
}

static void s_setMldType(whm_mxl_mld_t* pMld) {
    ASSERT_NOT_NULL(pMld, , ME, "pMld is NULL");
    whm_mxl_mld_type_e prevType = pMld->mldType;

    pMld->mldType = (whm_mxl_mld_type_e)s_getMldVapLinkCount(pMld);
    SAH_TRACEZ_INFO(ME, "MLO: MldId(%d) Set link type [%s] --> [%s]", pMld->mloId,
                s_mldTypeToStr(prevType), s_mldTypeToStr(pMld->mldType));
}

static bool s_checkNewLink(T_AccessPoint* pAP, whm_mxl_mld_t* pMld) {
    ASSERT_NOT_NULL(pAP, false, ME, "pAP is NULL");
    ASSERT_NOT_NULL(pMld, false, ME, "pMld is NULL");
    T_Radio* pNewLinkRad = pAP->pRadio;
    ASSERT_NOT_NULL(pNewLinkRad, false, ME, "pNewLinkRad is NULL");

    whm_mxl_link_t* pLink = NULL;
    whm_mxl_mlo_forEachLinkInMld(pLink, pMld) {
        T_AccessPoint* pSiblingAP = pLink->pLinkAp;
        T_Radio* pSiblingRad = pSiblingAP->pRadio;
        if (pSiblingRad == pNewLinkRad) {
            SAH_TRACEZ_WARNING(ME, "MLO: Link %s on same radio as existing link %s for MloId(%d)",
                            pAP->alias, pSiblingAP->alias, pMld->mloId);
            return false;
        }
    }
    return true;
}

static void s_setLinkId(T_Radio* pRad, whm_mxl_link_t* pLink) {
    ASSERT_NOT_NULL(pRad, , ME, "pRad is NULL");
    ASSERT_NOT_NULL(pLink, , ME, "pLink is NULL");
    if (wld_rad_is_24ghz(pRad)) {
        pLink->linkId = LINK_ID_2G;
    } else if (wld_rad_is_5ghz(pRad)) {
        pLink->linkId = LINK_ID_5G;
    } else if (wld_rad_is_6ghz(pRad)) {
        pLink->linkId = LINK_ID_6G;
    } else {
        T_AccessPoint* pAP = pLink->pLinkAp;
        if (pAP) {
            SAH_TRACEZ_WARNING(ME, "MLO: Unable to set linkId for link %s - unknown band", pAP->alias);
        }
    }
}

static swl_rc_ne s_createLink(T_AccessPoint* pAP, whm_mxl_mld_t* pMld) {
    ASSERT_NOT_NULL(pAP, SWL_RC_ERROR, ME, "pAP is NULL");
    ASSERT_NOT_NULL(pMld, SWL_RC_ERROR, ME, "pMld is NULL");
    int32_t mloId = pMld->mloId;
    T_SSID* pSSID = (T_SSID*) pAP->pSSID;
    ASSERT_NOT_NULL(pSSID, SWL_RC_ERROR, ME, "pSSID is NULL");
    mxl_VapVendorData_t* vapVendor = mxl_vap_getVapVendorData(pAP);
    ASSERT_NOT_NULL(vapVendor, SWL_RC_ERROR, ME, "vapVendor is NULL");

    ASSERT_TRUE(s_checkNewLink(pAP, pMld), SWL_RC_ERROR, ME,
        "MLO: Cannot add link %s to MLD(%d) - check failed", pAP->alias, mloId);
    uint32_t currentLinksCount = s_getMldVapLinkCount(pMld);
    ASSERT_FALSE((currentLinksCount >= MAX_MLD_LINKS), SWL_RC_ERROR, ME,
         "MLO: NEW MLD(%d) already used by %d links - unable to create new link", mloId, currentLinksCount);

    whm_mxl_link_t* pNewLink = calloc(1, sizeof(whm_mxl_link_t));
    ASSERT_NOT_NULL(pNewLink, SWL_RC_ERROR, ME, "%s: fail to alloc newLink (mloId %d)", pAP->alias, mloId);

    pNewLink->pLinkAp = pAP;
    pNewLink->pLinkSSID = pSSID;
    pNewLink->pMld = pMld;
    pNewLink->mloId = mloId;
    pNewLink->linkId = INVALID_LINK_ID;
    s_setLinkId(pAP->pRadio, pNewLink);
    // Link AP to its MLD VAP
    vapVendor->pMld = pMld;
    vapVendor->pLink = pNewLink;
    // Append to MLD VAP list
    amxc_llist_append(&pMld->affiliatedLinks, &pNewLink->it);

    s_setMldType(pMld);

    if (swl_mac_binIsNull(&pMld->apMldMac)) {
        s_setMldMac(pAP, pMld);
    }

    /* Sync MLD MAC to AP vendor DM */
    amxd_object_t* pMlObj = whm_mxl_mlo_getMloObject(pAP);
    if (!pMlObj) {
        SAH_TRACEZ_WARNING(ME, "MLO: Unable to update MLD MAC for link %s", pAP->alias);
    } else {
        swl_macChar_t mldMac = SWL_MAC_CHAR_NEW();
        SWL_MAC_BIN_TO_CHAR(&mldMac, pMld->apMldMac.bMac);
        amxd_object_set_cstring_t(pMlObj, "ApMldMac", mldMac.cMac);
    }

    whm_mxl_mlo_requestMldAction(pAP, true);

    return SWL_RC_OK;
}

static void s_deleteMldVap(whm_mxl_mld_t* pMld) {
    ASSERT_NOT_NULL(pMld, , ME, "pMld is NULL");
    SAH_TRACEZ_INFO(ME, "MLO: Deleting MLD VAP with MLO ID %d", pMld->mloId);
    amxc_llist_it_take(&pMld->it);
    free(pMld);
}

static swl_rc_ne s_deleteLink(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, SWL_RC_ERROR, ME, "pAP is NULL");
    mxl_VapVendorData_t* pVendor = mxl_vap_getVapVendorData(pAP);
    ASSERT_NOT_NULL(pVendor, SWL_RC_ERROR, ME, "pVendor is NULL");
    whm_mxl_mld_t *pMld = pVendor->pMld;
    ASSERTI_NOT_NULL(pMld, SWL_RC_ERROR, ME, "No MLD VAP associated with pAP %s", pAP->alias);
    whm_mxl_link_t* pLink = pVendor->pLink;
    ASSERTI_NOT_NULL(pLink, SWL_RC_ERROR, ME, "No MLD Link associated with pAP %s", pAP->alias);

    // Find a sibling link BEFORE freeing this link
    whm_mxl_link_t* pSiblingLink = NULL;
    whm_mxl_link_t* pIter = NULL;
    whm_mxl_mlo_forEachLinkInMld(pIter, pMld) {
        if (pIter != pLink) {
            pSiblingLink = pIter;
            break;
        }
    }
    T_AccessPoint* pSiblingAP = pSiblingLink ? pSiblingLink->pLinkAp : NULL;

    // Clear ApMldMac if was created from one of the links MAC
    if (!pMld->externalMldMac) {
        amxd_object_t* pMlObj = whm_mxl_mlo_getMloObject(pAP);
        ASSERT_NOT_NULL(pMlObj, SWL_RC_ERROR, ME, "MLO is not Mapped");
        amxd_object_set_value(cstring_t, pMlObj, "ApMldMac", SWL_MAC_CHAR_NULL);
    }

    // Remove link from MLD VAP list - free link memory
    amxc_llist_it_take(&pLink->it);
    /* Reset mainLink if the deleted link was the Main Link */
    if (pMld->mainLink == pLink) {
        pMld->mainLink = NULL;
    }
    free(pLink);
    // Unlink MLD from VAP Vendor data
    pVendor->pLink = NULL;
    pVendor->pMld = NULL;

    SAH_TRACEZ_INFO(ME, "MLO: MLD(%d), Deleted link (%s)", pMld->mloId, pAP->alias);

    s_setMldType(pMld);

    // If last link in MLD VAP - delete MLD VAP
    if (amxc_llist_is_empty(&pMld->affiliatedLinks)) {
        SAH_TRACEZ_INFO(ME, "MLO: Last link removed - deleting MLD VAP with MLO ID (%d)", pMld->mloId);
        s_deleteMldVap(pMld);
        // No sibling exists, no action needed
        return SWL_RC_OK;
    }

    // Request MLD action on sibling to re-evaluate MLD usability
    if (pSiblingAP) {
        whm_mxl_mlo_requestMldAction(pSiblingAP, true);
    }

    return SWL_RC_OK;
}

static uint32_t s_getMldVapsCount(void) {
    whm_mxl_mld_mngr_t *pMldMngr = whm_mxl_mlo_get_mldMngr();
    ASSERT_NOT_NULL(pMldMngr, 0, ME, "pMldMngr is NULL");
    uint32_t size = amxc_llist_size(&pMldMngr->mlds);
    SAH_TRACEZ_INFO(ME, "MLO: %u MLD Vaps", size);
    return size;
}

static whm_mxl_mld_t* s_createMld(int32_t mloId) {
    ASSERT_TRUE((s_getMldVapsCount() <= MAX_MLD_VAPS), NULL, ME,
                "Max MLD VAPs (%d) reached", MAX_MLD_VAPS);
    ASSERTI_TRUE((mloId >= 0), NULL, ME,
            "Invalid MLO ID (%d)", mloId);
    whm_mxl_mld_t* pMld = calloc(1, sizeof(whm_mxl_mld_t));
    ASSERT_NOT_NULL(pMld, NULL, ME, "fail to alloc pMld (mloId %d)", mloId);
    swl_macBin_t newMac = SWL_MAC_BIN_NEW();
    /* Init new MLD VAP Info */
    pMld->mloId = mloId;
    pMld->mldType = MLD_TYPE_NONE;
    pMld->externalMldMac = false;
    pMld->mainLink = NULL;
    pMld->isUsable = false;
    pMld->reasons = MLD_UNUSABLE_NONE;
    memcpy(pMld->apMldMac.bMac, newMac.bMac, SWL_MAC_BIN_LEN);
    amxc_llist_init(&pMld->affiliatedLinks);
                    
    /* Append new MLD VAP to MLD list */
    amxc_llist_append(&mldMngr.mlds, &pMld->it);
    SAH_TRACEZ_INFO(ME, "MLO: New pMld created with MLO ID %d", mloId);
    return pMld;
}

static swl_rc_ne s_moveLink(T_AccessPoint* pAP, int32_t newMloId) {
    ASSERTI_TRUE((newMloId >= 0), SWL_RC_INVALID_PARAM, ME,
            "Unable to move link to new MLO ID (%d)", newMloId);
    if (s_deleteLink(pAP) < SWL_RC_OK) {
        return SWL_RC_ERROR;
    }

    whm_mxl_mld_t* pMld = s_getMldVap(newMloId);
    if (pMld == NULL) {
        /* Create new MLD VAP */
        pMld = s_createMld(newMloId);
        ASSERT_NOT_NULL(pMld, SWL_RC_ERROR, ME, "Failed to create MLD VAP for MLO ID %d", newMloId);
    }

    if (s_createLink(pAP, pMld) < SWL_RC_OK) {
        return SWL_RC_ERROR;
    }
    SAH_TRACEZ_INFO(ME, "MLO: MLD(%d), Created link(%s)", newMloId, pAP->alias);

    return SWL_RC_OK;
}

const char* s_mldLinkActionToStr(whm_mxl_mld_link_action_e action) {
    switch (action) {
        case MLD_LINK_NONE: return "No Change";
        case MLD_LINK_CREATE: return "Create Link";
        case MLD_LINK_DELETE: return "Delete Link";
        case MLD_LINK_MOVE: return "Move Link";
        default: return "Unknown";
    }
}

static whm_mxl_mld_link_action_e s_getMldLinkAction(int32_t currMloId, int32_t newMloId) {
    if (currMloId == newMloId)
        return MLD_LINK_NONE;
    else if (currMloId == NO_MLO_ID && newMloId != NO_MLO_ID)
        return MLD_LINK_CREATE;
    else if (currMloId != NO_MLO_ID && newMloId == NO_MLO_ID)
        return MLD_LINK_DELETE;
    else
        return MLD_LINK_MOVE;
}

static int32_t s_getMloId(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, NO_MLO_ID, ME, "pAP is NULL");
    whm_mxl_link_t* pLink = whm_mxl_mlo_getMldLink(pAP);
    ASSERTS_NOT_NULL(pLink, NO_MLO_ID, ME, "pLink is NULL");
    whm_mxl_mld_t* pMld = whm_mxl_mlo_getMldVap(pAP);
    ASSERTS_NOT_NULL(pMld, NO_MLO_ID, ME, "pLink is NULL");
    return pMld->mloId;
}

/**
 * @brief Configures the MLO link state for a given Access Point (AP).
 *
 * This function determines the appropriate Link action(whm_mxl_mld_link_action_e)
 * to take on the MLO link based on the current and new MLO IDs.
 *
 * | currMloId  |  newMloId  |    Action   |
 * |------------|------------|-------------|
 * |     X      |     X      |  No Change  |
 * | NO_MLO_ID  | NO_MLO_ID  |  No Change  |
 * | NO_MLO_ID  |     X      | Create Link |
 * |     X      | NO_MLO_ID  | Delete Link |
 * |     X      |     Y      |  Move Link  |
 *
 * @param[in] pAP         Pointer to the Access Point structure.
 * @param[in] newMloId    New MLO ID to be associated with the AP.
 * @return swl_rc_ne
 */
swl_rc_ne whm_mxl_mlo_configureMld(T_AccessPoint* pAP, int32_t newMloId) {
    ASSERT_NOT_NULL(pAP, SWL_RC_ERROR, ME, "pAP is NULL");
    mxl_VapVendorData_t* pVendor = mxl_vap_getVapVendorData(pAP);
    ASSERT_NOT_NULL(pVendor, SWL_RC_ERROR, ME, "pVendor is NULL");
    ASSERT_FALSE((newMloId < NO_MLO_ID), SWL_RC_INVALID_PARAM, ME,
                 "MLO: bad mloId(%d)", newMloId);
    swl_rc_ne rc = SWL_RC_OK;
    whm_mxl_mld_t* pMld = s_getMldVap(newMloId);

    /* Check if theres already an exisitng MLD VAP for given MLO ID */
    if ((pMld == NULL) && (newMloId != NO_MLO_ID)) {
        /* Create new MLD VAP */
        pMld = s_createMld(newMloId);
        ASSERT_NOT_NULL(pMld, SWL_RC_ERROR, ME, "Failed to create MLD VAP for MLO ID %d", newMloId);
    }

    int32_t currMloId = s_getMloId(pAP);
    whm_mxl_mld_link_action_e linkAction = s_getMldLinkAction(currMloId, newMloId);
    SAH_TRACEZ_INFO(ME, "MLO: MloId(%d) action[%s] for link %s", newMloId, s_mldLinkActionToStr(linkAction), pAP->alias);

    switch (linkAction) {
        case MLD_LINK_NONE: {
            ASSERTI_FALSE(true, SWL_RC_OK, ME,
                         "MLO: No change in Link(%s) with MloID %d",
                         pAP->alias, currMloId);
        }
        case MLD_LINK_CREATE: {
            rc = s_createLink(pAP, pMld);
            break;
        }
        case MLD_LINK_DELETE: {
            rc = s_deleteLink(pAP);
            break;
        }
        case MLD_LINK_MOVE: {
            rc = s_moveLink(pAP, newMloId);
            break;
        }
        default:
            SAH_TRACEZ_ERROR(ME, "MLO: Invalid link action(%d) for link(%s)",
                             linkAction, pAP->alias);
    }

    return rc;
}

/**
 * @brief This function updates the MLD MAC address for the MLD VAP associated with the given AP.
 *
 * @param[in] pAP           Pointer to the Access Point structure.
 * @param[in] pMldMac       Pointer to the new MLD MAC address in binary format.
 * @param[in] checkSameMac  Flag indicating whether to check if the new MAC is the same as the current one.
 * @return swl_rc_ne
 */
swl_rc_ne whm_mxl_mlo_configureMldMac(T_AccessPoint* pAP, swl_macBin_t* pMldMac, bool checkSameMac) {
    ASSERT_NOT_NULL(pAP, SWL_RC_ERROR, ME, "pAP is NULL");
    ASSERT_NOT_NULL(pMldMac, SWL_RC_ERROR, ME, "pMldMac is NULL");
    mxl_VapVendorData_t* pVendor = mxl_vap_getVapVendorData(pAP);
    ASSERT_NOT_NULL(pVendor, SWL_RC_ERROR, ME, "pVendor is NULL");
    
    whm_mxl_mld_t *pMld = pVendor->pMld;
    if (pMld) {
        ASSERTI_NOT_NULL(pVendor->pLink, SWL_RC_ERROR, ME, "pLink is NULL");
        if (checkSameMac) {
            /* Skip same MAC check when MLD is finalized after all conf has been loaded 
             * This will force derving MLD MAC from link MAC if no MAC was provided
             */
            ASSERT_FALSE(swl_mac_binMatches(&pMld->apMldMac, pMldMac), SWL_RC_OK, ME,
                         "MLO: ApMldMac equals new MACAddress for link %s", pAP->alias);
        }
        if (swl_mac_binIsNull(pMldMac)) {
            /* Set MLD MAC based on link MAC */
            T_SSID* pSSID = (T_SSID*) pAP->pSSID;
            ASSERT_NOT_NULL(pSSID, SWL_RC_ERROR, ME, "pSSID is NULL");
            memcpy(pMld->apMldMac.bMac, pSSID->BSSID, SWL_MAC_BIN_LEN);
            SAH_TRACEZ_INFO(ME, "MLO: %s Zero MAC provided - use link own MAC "SWL_MAC_FMT,
                            pAP->alias, SWL_MAC_ARG(pMld->apMldMac.bMac));
        } else {
            /* Set MLD MAC */
            memcpy(pMld->apMldMac.bMac, pMldMac->bMac, SWL_MAC_BIN_LEN);
            pMld->externalMldMac = true;
            SAH_TRACEZ_INFO(ME, "MLO: %s Updated MLD MAC to "SWL_MAC_FMT,
                            pAP->alias, SWL_MAC_ARG(pMld->apMldMac.bMac));
        }
        /* Update new MLD MAC to all links */
        whm_mxl_link_t* pLink = NULL;
        whm_mxl_mlo_forEachLinkInMld(pLink, pMld) {
            T_AccessPoint* pApLink = pLink->pLinkAp;
            if (!pApLink) {
                continue;
            }
            amxd_object_t* pMlObj = whm_mxl_mlo_getMloObject(pApLink);
            swl_macChar_t mldMac = SWL_MAC_CHAR_NEW();
            if (!pMlObj) {
                SAH_TRACEZ_ERROR(ME, "MLO: Unable to update MLD MAC for link %s", pApLink->alias);
                continue;
            }
            SWL_MAC_BIN_TO_CHAR(&mldMac, pMld->apMldMac.bMac);
            amxd_object_set_cstring_t(pMlObj, "ApMldMac", mldMac.cMac);
        }
        whm_mxl_mlo_requestMldAction(pAP, false);
    }
    
    return SWL_RC_OK;
}

swl_rc_ne whm_mxl_mlo_deleteLink(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, SWL_RC_ERROR, ME, "pAP is NULL");
    mxl_VapVendorData_t* pVendor = mxl_vap_getVapVendorData(pAP);
    ASSERT_NOT_NULL(pVendor, SWL_RC_ERROR, ME, "pVendor is NULL");
    whm_mxl_mld_t *pMld = pVendor->pMld;
    whm_mxl_link_t *pLink = pVendor->pLink;
    /* Free the link */
    if (pLink) {
        /* Reset mainLink if the deleted link was the Main Link */
        if (pMld && pMld->mainLink == pLink) {
            pMld->mainLink = NULL;
        }
        amxc_llist_it_take(&pLink->it);
        free(pLink);
    }
    /* Destroy MLD VAP if no affiliated links remain */
    if (pMld && amxc_llist_is_empty(&pMld->affiliatedLinks)) {
        amxc_llist_it_take(&pMld->it);
        free(pMld);
    }
    pVendor->pLink = NULL;
    pVendor->pMld = NULL;

    return SWL_RC_OK;
}

static void s_finalizeMld(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, , ME, "pAP is NULL");
    mxl_VapVendorData_t* pVendor = mxl_vap_getVapVendorData(pAP);
    ASSERT_NOT_NULL(pVendor, , ME, "pVendor is NULL");
    
    whm_mxl_mld_t *pMld = pVendor->pMld;
    ASSERTS_NOT_NULL(pMld, , ME, "pMld is NULL");
    SAH_TRACEZ_INFO(ME, "MLO: %s: Finalizing MLD", pAP->alias);
    if (swl_mac_binIsNull(&pMld->apMldMac)) {
        whm_mxl_mlo_configureMldMac(pAP, &pMld->apMldMac, false);
        SAH_TRACEZ_INFO(ME, "MLO: %s: Finalizing MLD MAC", pAP->alias);
    }
}

/**
 * @brief FTA handler for (re)configuring Links in an MLD
 *
 * When MLDUnit's set/link is modified, this FTA handler will be invoked
 * from PWHM. This function will configure respective parameters for MLD
 * configuration in Hostapd.
 *
 * @param[in] VAP used as a Link in the MLD
 * @return SWL_RC_OK on success (i.e request sent), error code otherwise
 */
swl_rc_ne whm_mxl_mlo_setMldUnit(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, SWL_RC_INVALID_PARAM, ME, "No pAP Mapped");
    ASSERT_NOT_NULL(pAP->pSSID, SWL_RC_INVALID_PARAM, ME, "pSSID is NULL");
    ASSERT_NOT_NULL(pAP->pRadio, SWL_RC_INVALID_PARAM, ME, "pRadio is NULL");
    swl_rc_ne rc = SWL_RC_OK;

    int32_t newMloId = pAP->pSSID->mldUnit;

    SAH_TRACEZ_INFO(ME, "MLO: modify MLDUnit (%d) for %s",
                    newMloId, pAP->alias);

    rc = whm_mxl_mlo_configureMld(pAP, newMloId);
    ASSERT_TRUE(rc >= SWL_RC_OK, rc, ME,
                "MLO: Failed to configure MLDUnit (%d) for %s",
                newMloId, pAP->alias);
    /* Sync MLD MAC configuration after first commit */
    if (!wld_rad_firstCommitFinished(pAP->pRadio) || !pAP->initDone) {
        swla_delayExec_add((swla_delayExecFun_cbf) s_finalizeMld, pAP);
    }
    return rc;
}

/**
 * @brief Dummy FTA handler for Endpoint MLDUnit configuration.
 *
 * MxL hardware does not support Endpoint (bSTA) MLO This handler intentionally
 * performs no operation and returns SWL_RC_OK to avoid unintended changes in
 * wld when the wifiGen_ep_setMldUnit is invoked.
 *
 * @param[in] pEP Pointer to the EndPoint structure (unused).
 * @return SWL_RC_OK always.
 */
swl_rc_ne whm_mxl_mlo_setEndpointMldUnit(T_EndPoint* pEP _UNUSED) {
    SAH_TRACEZ_ERROR(ME, "MLO: Endpoint MLO not supported on MxL hardware - ignoring setMldUnit");
    return SWL_RC_OK;
}

whm_mxl_mld_mngr_t *whm_mxl_mlo_get_mldMngr(void) {
    return &mldMngr;
}

swl_rc_ne whm_mxl_mlo_initMld(void) {
    whm_mxl_mld_mngr_t *pMldMngr = whm_mxl_mlo_get_mldMngr();
    amxc_llist_init(&pMldMngr->mlds);
    s_setCommitMngrDefaults(pMldMngr);
    amxp_timer_new(&pMldMngr->mloCommitMngr.commitTimer, s_mloCommit_th, pMldMngr);
    pMldMngr->init = true;
    return SWL_RC_OK;
}

static void s_deinitLinks(amxc_llist_it_t* it) {
    ASSERTS_NOT_NULL(it, , ME, "NULL");
    whm_mxl_link_t* pLink = amxc_container_of(it, whm_mxl_link_t, it);
    ASSERTS_NOT_NULL(pLink, , ME, "NULL");
    amxc_llist_it_take(&pLink->it);
    SAH_TRACEZ_INFO(ME, "MLO: deinit link with mloId %d (%p)", pLink->mloId, pLink);
    free(pLink);
}

static void s_deinitMlds(amxc_llist_it_t* it) {
    ASSERTS_NOT_NULL(it, , ME, "NULL");
    whm_mxl_mld_t* pMld = amxc_container_of(it, whm_mxl_mld_t, it);
    ASSERTS_NOT_NULL(pMld, , ME, "pMld is NULL");
    amxc_llist_it_take(&pMld->it);
    amxc_llist_clean(&pMld->affiliatedLinks, s_deinitLinks);
    SAH_TRACEZ_INFO(ME, "MLO: deinit pMld %d (%p)", pMld->mloId, pMld);
    free(pMld);
}

swl_rc_ne whm_mxl_mlo_deinitMld(void) {
    whm_mxl_mld_mngr_t *pMldMngr = whm_mxl_mlo_get_mldMngr();
    amxc_llist_clean(&pMldMngr->mlds, s_deinitMlds);
    amxp_timer_delete(&pMldMngr->mloCommitMngr.commitTimer);
    pMldMngr->mloCommitMngr.commitTimer = NULL;
    pMldMngr->init = false;
    SAH_TRACEZ_WARNING(ME, "MLO: MLD deinit done");
    return SWL_RC_OK;
}

static bool s_isLastLink(whm_mxl_link_t *pLink) {
    ASSERT_NOT_NULL(pLink, false, ME, "pLink is NULL");
    return (whm_mxl_mlo_getNextLink(pLink) == NULL);
}

/**
 * @brief Get AP MLD info from the driver for the given mod_whm MLD
 *
 * Queries the driver via `whm_mxl_nl80211_getApMldList` on the first available
 * link, then finds the matching entry by comparing the MLD MAC address.
 *
 * @param[in] pMld Pointer to mod_whm MLD structure
 * @return Pointer to heap-allocated copy of matching entry (caller must free),
 *         or NULL on failure
 */
whm_mxl_nl80211_apMld_t* whm_mxl_mlo_getApMldInfo(whm_mxl_mld_t* pMld) {
    ASSERT_NOT_NULL(pMld, NULL, ME, "pMld is NULL");

    // Get first link to use as the NL80211 query interface
    whm_mxl_link_t* pFirstLink = whm_mxl_mlo_getFirstLink(pMld);
    ASSERT_NOT_NULL(pFirstLink, NULL, ME, "MLD(%d) has no links", pMld->mloId);
    ASSERT_NOT_NULL(pFirstLink->pLinkAp, NULL, ME, "MLD(%d) pLinkAp null", pMld->mloId);

    // Query the driver for the AP MLD info
    whm_mxl_nl80211_apMld_t* pNlApMld = NULL;
    uint32_t count = 0;
    swl_rc_ne rc = whm_mxl_nl80211_getApMldList(pFirstLink->pLinkAp, &pNlApMld, &count);
    ASSERT_FALSE(rc < SWL_RC_OK || !pNlApMld, NULL , ME, "fail in NL query");

    // Find matching entry by MLD MAC address
    whm_mxl_nl80211_apMld_t* pResult = NULL;
    for (uint32_t i = 0; i < count; i++) {
        whm_mxl_nl80211_apMld_t* pEntry = &pNlApMld[i];
        if (memcmp(pMld->apMldMac.bMac, pEntry->mld_addr, SWL_MAC_BIN_LEN) == 0) {
            SAH_TRACEZ_INFO(ME, "MLD(%d) found matching driver entry at index %u "
                            "(MLD MAC "SWL_MAC_FMT")",
                            pMld->mloId, i, SWL_MAC_ARG(pMld->apMldMac.bMac));
            // Allocate and copy the matched entry
            pResult = calloc(1, sizeof(whm_mxl_nl80211_apMld_t));
            if (pResult) {
                memcpy(pResult, pEntry, sizeof(whm_mxl_nl80211_apMld_t));
            } else {
                SAH_TRACEZ_ERROR(ME, "MLD(%d) failed to allocate copy of matched entry",
                                 pMld->mloId);
            }
            break;
        }
    }

    if (!pResult) {
        SAH_TRACEZ_WARNING(ME, "MLD(%d) no matching entry in driver AP MLD data "
                           "(MLD MAC "SWL_MAC_FMT", count=%u)",
                           pMld->mloId, SWL_MAC_ARG(pMld->apMldMac.bMac), count);
    }

    free(pNlApMld);
    return pResult;
}

/**
 * @brief Copy NL80211 AP MLD data into Vendor MLD
 *
 * @param[in,out] pMld     Pointer to mod_whm MLD structure
 * @param[in]     pMldInfo Pointer to NL80211 AP MLD entry from driver
 * @return SWL_RC_OK on success, error code otherwise
 */
swl_rc_ne whm_mxl_mlo_copyNl80211ApMldToMld(whm_mxl_mld_t* pMld,
                                             whm_mxl_nl80211_apMld_t* pMldInfo) {
    ASSERT_NOT_NULL(pMld, SWL_RC_INVALID_PARAM, ME, "pMld is NULL");
    ASSERT_NOT_NULL(pMldInfo, SWL_RC_INVALID_PARAM, ME, "pMldInfo is NULL");

    /* Determine main link: link whose BSSID matches the AP MLD MAC address */
    whm_mxl_link_t* pMainLink = NULL;
    whm_mxl_link_t* pLink = NULL;
    whm_mxl_mlo_forEachLinkInMld(pLink, pMld) {
        T_SSID* pSSID = pLink->pLinkSSID;
        ASSERT_NOT_NULL(pSSID, SWL_RC_ERROR, ME, "pSSID is NULL");
        if (memcmp(pSSID->BSSID, pMldInfo->mld_addr, SWL_MAC_BIN_LEN) == 0) {
            pMainLink = pLink;
            break;
        }
    }

    if (!pMainLink) {
        SAH_TRACEZ_WARNING(ME, "MLD(%d) No link found with " SWL_MAC_FMT " MAC",
                           pMld->mloId, SWL_MAC_ARG(pMldInfo->mld_addr));
        /* Fallback: use first interface in AP MLD data */
        const char* mainIfName = (pMldInfo->ifname[0][0] != '\0')
                                 ? (const char*)pMldInfo->ifname[0] : NULL;
        ASSERT_NOT_NULL(mainIfName, SWL_RC_ERROR, ME, "MLD(%d) mainIfName is NULL",
                        pMld->mloId);
        pMainLink = whm_mxl_mlo_getMldLink(wld_vap_from_name(mainIfName));

    }
    ASSERT_NOT_NULL(pMainLink, SWL_RC_ERROR, ME,
                    "MLD(%d) pMainLink not resolved", pMld->mloId);

    pMld->mainLink = pMainLink;
    SAH_TRACEZ_INFO(ME, "MLD(%d) Main Link set to linkId=%u",
                    pMld->mloId, pMainLink->linkId);

    return SWL_RC_OK;
}

/**
 * @brief Update Generic MLD (wld_mld_t) using Vendor MLD (whm_mxl_mlt_t)
 *
 * Iterates all links in the given Vendor MLD and it sets (in order):
 *   1. Link ID
 *   2. MLO Role
 *   3. Configured status
 *
 * This triggers WLD_MLD_EVT_UPDATE which causes wld's DM pipeline
 * (s_updateDmParams / s_createDmInstance) to populate APMLD entries.
 *
 * @param[in] pMld Pointer to mod_whm MLD structure (must have mainLink set)
 * @return SWL_RC_OK on success, error code otherwise
 */
swl_rc_ne whm_mxl_mlo_updateGenericMld(whm_mxl_mld_t* pMld) {
    ASSERT_NOT_NULL(pMld, SWL_RC_INVALID_PARAM, ME, "pMld is NULL");
    ASSERT_NOT_NULL(pMld->mainLink, SWL_RC_INVALID_PARAM, ME,
                    "MLD(%d) mainlink not set", pMld->mloId);

    whm_mxl_link_t* pLink = NULL;
    whm_mxl_mlo_forEachLinkInMld(pLink, pMld) {
        T_SSID* pSSID = pLink->pLinkSSID;
        ASSERT_NOT_NULL(pSSID, SWL_RC_ERROR, ME, "pSSID is NULL");
        ASSERT_NOT_NULL(pSSID->pMldLink, SWL_RC_ERROR, ME, "pMldLink is NULL");

        wld_ssid_setMLDLinkID(pSSID, (int16_t)pLink->linkId);
        swl_mlo_role_e role = (pLink == pMld->mainLink)
                              ? SWL_MLO_ROLE_PRIMARY
                              : SWL_MLO_ROLE_AUXILIARY;
        wld_ssid_setMLDRole(pSSID, role);
        wld_mld_setLinkConfigured(pSSID->pMldLink, true);
        SAH_TRACEZ_INFO(ME, "MLD(%d) %s: set pWHM LinkID=%u, MLDRole=%s, link configured",
                        pMld->mloId, pSSID->Name, pLink->linkId,
                        (role == SWL_MLO_ROLE_PRIMARY) ? "Primary" : "Auxiliary");
    }

    return SWL_RC_OK;
}

static void s_mloCommitMngrEnable_pwf(void* priv _UNUSED,
                                  amxd_object_t* object _UNUSED,
                                  amxd_param_t* param _UNUSED,
                                  const amxc_var_t* const newValue) {
    whm_mxl_mld_mngr_t* pMldMngr = whm_mxl_mlo_get_mldMngr();
    ASSERT_NOT_NULL(pMldMngr, , ME, "NULL");
    bool enable = amxc_var_dyncast(bool, newValue);
    SAH_TRACEZ_INFO(ME, "MLO: Commit Manager %s", enable ? "enabled" : "disabled");
    pMldMngr->mloCommitMngr.enable = enable;
}

static void s_mloCommitMngrCommitDelay_pwf(void* priv _UNUSED,
                                           amxd_object_t* object _UNUSED,
                                           amxd_param_t* param _UNUSED,
                                           const amxc_var_t* const newValue) {
    whm_mxl_mld_mngr_t* pMldMngr = whm_mxl_mlo_get_mldMngr();
    ASSERT_NOT_NULL(pMldMngr, , ME, "NULL");
    uint32_t delay = amxc_var_dyncast(uint32_t, newValue);
    SAH_TRACEZ_INFO(ME, "MLO: Commit Manager delay set from %u to %u",
                    pMldMngr->mloCommitMngr.delay, delay);
    pMldMngr->mloCommitMngr.delay = delay;
}

static void s_mloCommitMngrBootDelay_pwf(void* priv _UNUSED,
                                         amxd_object_t* object _UNUSED,
                                         amxd_param_t* param _UNUSED,
                                         const amxc_var_t* const newValue) {
    whm_mxl_mld_mngr_t* pMldMngr = whm_mxl_mlo_get_mldMngr();
    ASSERT_NOT_NULL(pMldMngr, , ME, "NULL");
    uint32_t bootDelay = amxc_var_dyncast(uint32_t, newValue);
    SAH_TRACEZ_INFO(ME, "MLO: Commit Manager boot delay set from %u to %u",
                    pMldMngr->mloCommitMngr.bootDelay, bootDelay);
    pMldMngr->mloCommitMngr.bootDelay = bootDelay;
}

SWLA_DM_HDLRS(sMloCommitMngrDmHdlrs,
              ARR(SWLA_DM_PARAM_HDLR("Enable", s_mloCommitMngrEnable_pwf),
                  SWLA_DM_PARAM_HDLR("CommitDelay", s_mloCommitMngrCommitDelay_pwf),
                  SWLA_DM_PARAM_HDLR("BootDelay", s_mloCommitMngrBootDelay_pwf)));

void _whm_mxl_mlo_commitMngr_configure_ocf(const char* const sig_name,
                                           const amxc_var_t* const data,
                                           void* const priv) {
    swla_dm_procObjEvtOfLocalDm(&sMloCommitMngrDmHdlrs, sig_name, data, priv);
}

amxd_status_t _whm_mxl_mlo_debug(amxd_object_t* object _UNUSED,
                                      amxd_function_t* func _UNUSED,
                                      amxc_var_t* args _UNUSED,
                                      amxc_var_t* retval) {
    whm_mxl_mld_mngr_t* pMldMngr = whm_mxl_mlo_get_mldMngr();
    ASSERT_NOT_NULL(pMldMngr, amxd_status_unknown_error, ME, "NULL");
    whm_mxl_mld_t *pMld = NULL;

    amxc_var_set_type(retval, AMXC_VAR_ID_LIST);

    whm_mxl_mlo_forEachMldVap(pMld, pMldMngr) {
        amxc_var_t* pEntry = amxc_var_add(amxc_htable_t, retval, NULL);
        if (!pEntry) {
            SAH_TRACEZ_ERROR(ME, "MLO: Failed to add MLD entry to retval");
            continue;
        }
        amxc_string_t mlTypeStr;
        swl_macChar_t mldMacChar;
        swl_mac_binToChar(&mldMacChar, &pMld->apMldMac);
        amxc_string_init(&mlTypeStr, 0);
        amxc_var_add_key(cstring_t, pEntry, "MLD MAC", mldMacChar.cMac);
        amxc_var_add_key(int32_t, pEntry, "MloId", pMld->mloId);
        amxc_string_appendf(&mlTypeStr, "%s", s_mldTypeToStr(pMld->mldType));
        amxc_var_add_key(cstring_t, pEntry, "MLD Type",  amxc_string_get(&mlTypeStr, 0));
        amxc_string_clean(&mlTypeStr);
        amxc_var_add_key(cstring_t, pEntry, "ExternalMAC", pMld->externalMldMac ? "True" : "False");
        amxc_var_add_key(cstring_t, pEntry, "Usable", pMld->isUsable ? "True" : "False");
        char reasonBuf[256] = {0};
        s_unusableReasonMaskToString(reasonBuf, sizeof(reasonBuf), pMld->reasons);
        amxc_var_add_key(cstring_t, pEntry, "UnusableReasons",
                         (pMld->reasons == MLD_UNUSABLE_NONE) ? "none" : reasonBuf);
        whm_mxl_link_t *pLink = NULL;
        amxc_string_t linksStr;
        amxc_string_init(&linksStr, 0);
        whm_mxl_mlo_forEachLinkInMld(pLink, pMld) {
            T_AccessPoint* pLinkAp = pLink->pLinkAp;
            if (!pLinkAp) {
                SAH_TRACEZ_ERROR(ME, "MLO: no pAP for link in MLD (%d) - skipping", pMld->mloId);
                continue;
            }
            if (s_isLastLink(pLink)) {
                amxc_string_appendf(&linksStr, "%s", pLinkAp->alias);
            } else {
                amxc_string_appendf(&linksStr, "%s, ", pLinkAp->alias);
            }
        }
        amxc_var_add_key(cstring_t, pEntry, "Vaps", amxc_string_get(&linksStr, 0));
        amxc_string_clean(&linksStr);
    }
    return amxd_status_ok;
}

/**
 * @brief ODL function to get APMLD list via Vendor NL80211 Command
 *
 * On the first enabled vap found, queries for APMLD List using
 * LTQ_NL80211_VENDOR_SUBCMD_GET_ML_VAP_LIST
 *
 * @param object The ODL object (WiFi.Vendor.MLO)
 * @param func The ODL function object
 * @param args Input arguments (unused)
 * @param retval Output return value (list of APMLD entries from all radios)
 * @return amxd_status_t Status of the operation
 */
amxd_status_t _whm_mxl_getApMldList(amxd_object_t* object _UNUSED,
                                   amxd_function_t* func _UNUSED,
                                   amxc_var_t* args _UNUSED,
                                   amxc_var_t* retval) {
    T_Radio* pRad = NULL;

    /* Initialize output list */
    amxc_var_set_type(retval, AMXC_VAR_ID_LIST);

    wld_for_eachRad(pRad) {
        if (!pRad || !pRad->enable) continue;
        T_AccessPoint* pAP = whm_mxl_utils_getFirstEnabledVap(pRad);
        ASSERT_NOT_NULL(pAP, amxd_status_unknown_error, ME, "pAP is NULL");

        whm_mxl_nl80211_apMld_t* pMldList = NULL;
        uint32_t count = 0;
        swl_rc_ne rc = whm_mxl_nl80211_getApMldList(pAP, &pMldList, &count);
        ASSERT_FALSE(rc < SWL_RC_OK, amxd_status_unknown_error, ME, "fail in nl call");

        /* Convert each entry to ODL format */
        for (uint32_t i = 0; i < count; i++) {
            whm_mxl_nl80211_apMld_t* pEntry = &pMldList[i];
            amxc_var_t* pMldEntry = amxc_var_add(amxc_htable_t, retval, NULL);
            if (!pMldEntry) {
                SAH_TRACEZ_ERROR(ME, "pMldEntry is NULL");
                free(pMldList);
                return amxd_status_unknown_error;
            }

            swl_macChar_t mldMacChar = SWL_MAC_CHAR_NEW();
            SWL_MAC_BIN_TO_CHAR(&mldMacChar, pEntry->mld_addr);
            amxc_var_add_key(int32_t, pMldEntry, "MldId", pEntry->mld_id);
            amxc_var_add_key(cstring_t, pMldEntry, "MldAddress", mldMacChar.cMac);
            amxc_var_add_key(cstring_t, pMldEntry, "SSID", (char*)pEntry->ssid);
            amxc_var_t* pIfnamesArray = amxc_var_add_key(amxc_llist_t, pMldEntry, "Links", NULL);
            if (pIfnamesArray) {
                for (int j = 0; j < MAX_MLD_LINKS; j++) {
                    if (pEntry->ifname[j][0] != '\0') {
                        amxc_var_add(cstring_t, pIfnamesArray, (char*)pEntry->ifname[j]);
                    }
                }
            } else {
                SAH_TRACEZ_WARNING(ME, "Failed to create ifnames array for MLD %d", pEntry->mld_id);
            }
        }

        free(pMldList);
        break;
    }

    return amxd_status_ok;
}

/**
 * @brief ODL function to get STA MLD list via Vendor NL80211 Command
 *
 * Queries for STA MLD List using LTQ_NL80211_VENDOR_SUBCMD_GET_ML_STA_LIST
 * on the MLD's main link. The function takes an MLDUnit as input and queries
 * for STA MLD list on the main link of that MLD.
 *
 * @param object The ODL object (WiFi.Vendor.MLO)
 * @param func The ODL function object
 * @param args Input arguments - MLDUnit: MLD ID to query (integer)
 * @param retval Output return value (list of STA MLD entries)
 * @return amxd_status_t Status of the operation
 */
amxd_status_t _whm_mxl_getStaMldList(amxd_object_t* object _UNUSED,
                                     amxd_function_t* func _UNUSED,
                                     amxc_var_t* args,
                                     amxc_var_t* retval) {
    /* Initialize output list */
    amxc_var_set_type(retval, AMXC_VAR_ID_LIST);

    /* Get AP from MLDUnit from arguments */
    int32_t mldUnit = GET_INT32(args, "MLDUnit");
    ASSERT_TRUE(mldUnit >= 0, amxd_status_invalid_value, ME, "Invalid MLDUnit");
    whm_mxl_mld_t* pMld = s_getMldVap(mldUnit);
    ASSERT_NOT_NULL(pMld, amxd_status_invalid_value, ME, "MLD (%d) not found", mldUnit);
    ASSERT_NOT_NULL(pMld->mainLink, amxd_status_unknown_error, ME, "mainLink NULL");
    T_AccessPoint* pAP = pMld->mainLink->pLinkAp;
    ASSERT_NOT_NULL(pAP, amxd_status_unknown_error, ME, "pAP is null");

    whm_mxl_nl80211_staMld_t* pStaMldList = NULL;
    uint32_t count = 0;
    swl_rc_ne rc = whm_mxl_nl80211_getStaMldList(pAP, &pStaMldList, &count);
    ASSERT_FALSE(rc < SWL_RC_OK, amxd_status_unknown_error, ME,
                 "Failed to get STA MLD list via NL80211");

    /* Mapping arrays for enums */
    const char* supportedModeStr[] = {"MLSR", "EMLSR", "STR"};
    const char* linkTypeStr[] = {"None", "SingleLink", "DualLink", "TriLink"};

    /* Convert each entry to ODL format */
    for (uint32_t i = 0; i < count; i++) {
        whm_mxl_nl80211_staMld_t* pEntry = &pStaMldList[i];
        amxc_var_t* pStaEntry = amxc_var_add(amxc_htable_t, retval, NULL);
        if (!pStaEntry) {
            SAH_TRACEZ_ERROR(ME, "Failed to create STA entry");
            free(pStaMldList);
            return amxd_status_unknown_error;
        }

        swl_macChar_t mldMacChar = SWL_MAC_CHAR_NEW();
        SWL_MAC_BIN_TO_CHAR(&mldMacChar, pEntry->mld_addr);

        amxc_var_add_key(cstring_t, pStaEntry, "MldAddress", mldMacChar.cMac);
        amxc_var_add_key(uint16_t, pStaEntry, "AID", pEntry->aid);
        if (pEntry->link_type < (sizeof(linkTypeStr) / sizeof(linkTypeStr[0]))) {
            amxc_var_add_key(cstring_t, pStaEntry, "LinkType", linkTypeStr[pEntry->link_type]);
        } else {
            amxc_var_add_key(cstring_t, pStaEntry, "LinkType", "Unknown");
        }
        if (pEntry->supported_mode < (sizeof(supportedModeStr) / sizeof(supportedModeStr[0]))) {
            amxc_var_add_key(cstring_t, pStaEntry, "SupportedMode",
                           supportedModeStr[pEntry->supported_mode]);
        } else {
            amxc_var_add_key(cstring_t, pStaEntry, "SupportedMode", "Unknown");
        }

        amxc_var_t* pLinksArray = amxc_var_add_key(amxc_llist_t, pStaEntry, "Links", NULL);
        if (!pLinksArray) {
            SAH_TRACEZ_ERROR(ME, "Failed to create LinksArray %s", mldMacChar.cMac);
            free(pStaMldList);
            return amxd_status_unknown_error;
        }
        for (int j = 0; j < MAX_MLD_LINKS; j++) {
            if (pEntry->ifname[j][0] == '\0') {
                continue;
            }

            amxc_var_t* pLink = amxc_var_add(amxc_htable_t, pLinksArray, NULL);
            if (!pLink) {
                SAH_TRACEZ_ERROR(ME, "Failed to create Link entry %d", j);
                free(pStaMldList);
                return amxd_status_unknown_error;
            }
            amxc_var_add_key(cstring_t, pLink, "Ifname", (char*)pEntry->ifname[j]);
            swl_macChar_t staMacChar = SWL_MAC_CHAR_NEW();
            SWL_MAC_BIN_TO_CHAR(&staMacChar, pEntry->sta_addr[j]);
            amxc_var_add_key(cstring_t, pLink, "StaAddress", staMacChar.cMac);
            amxc_var_add_key(uint16_t, pLink, "StationID", pEntry->sid[j]);
        }
    }

    free(pStaMldList);

    return amxd_status_ok;
}
