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
#include <swl/map/swl_mapCharFmt.h>
#include <swla/swla_mac.h>

#include "wld/wld.h"
#include "wld/wld_util.h"
#include "wld/wld_radio.h"
#include "wld/wld_accesspoint.h"
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

#include <vendor_cmds_copy.h>

#define ME "mxlMlo"

static whm_mxl_mld_mngr_t mldMngr = {.init = false};

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

bool s_isMlBasicConditionsMet(whm_mxl_link_t* pLink) {
    ASSERT_NOT_NULL(pLink, false, ME, "NULL");
    whm_mxl_mld_t *pMld = pLink->pMld;
    ASSERTI_NOT_NULL(pMld, false, ME, "pMld is NULL");

    if (swl_mac_binIsNull(&pMld->apMldMac)) {
        return false;
    }
    return true;
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

bool s_isMlSharedConfigValid(whm_mxl_link_t* pLink) {
    whm_mxl_mld_t *pMld = pLink->pMld;
    ASSERTI_NOT_NULL(pMld, false, ME, "pMld is NULL");
    T_AccessPoint* pRefAP = pLink->pLinkAp;;
    ASSERTI_NOT_NULL(pRefAP, false, ME, "pRefAP is NULL");
    T_SSID* pRefSSID = (T_SSID*) pRefAP->pSSID;
    ASSERT_NOT_NULL(pRefSSID, false, ME, "pRefSSID is NULL");

    // Validate Params between links
    whm_mxl_link_t* sibLink = NULL;
    whm_mxl_mlo_forEachLinkInMld(sibLink, pMld) {
        if (!s_isMlBasicConditionsMet(sibLink)) {
            return false;
        }
        T_AccessPoint* pSiblingAP = sibLink->pLinkAp;
        ASSERTI_NOT_NULL(pSiblingAP, false, ME, "pSiblingAP is NULL");
        T_SSID* pSiblingSSID = (T_SSID*) pSiblingAP->pSSID;
        ASSERTI_NOT_NULL(pSiblingSSID, false, ME, "pSiblingSSID is NULL");
        // Validate MLD parameters (SSID, Security)
        ASSERTI_TRUE(swl_str_matches(pRefSSID->SSID, pSiblingSSID->SSID), false,
                    ME, "MLO: Mismatch of SSID between links (%s & %s)",
                    pRefSSID->SSID, pSiblingSSID->SSID);
        ASSERTI_TRUE(wld_ap_sec_checkSharedSecConfigs(pRefAP, pSiblingAP), false,
                    ME, "MLO: Mismatch of Security Mode between links (%s & %s)",
                    pRefAP->alias, pSiblingAP->alias);
        // Validate Vendor Security config
        ASSERTI_TRUE(s_checkVendorSecSharedConfig(pRefAP, pSiblingAP), false,
                    ME, "MLO: Mismatch of Vendor Security Config between links (%s & %s)",
                    pRefAP->alias, pSiblingAP->alias);
    }

    return true;
}

static void s_saveLinkConfigStatus(whm_mxl_link_t* pLink, bool status) {
    ASSERTS_NOT_NULL(pLink, , ME, "NULL");
    T_AccessPoint* pAP = pLink->pLinkAp;
    if (pAP) {
        SAH_TRACEZ_INFO(ME, "MLO: Link %s configured status set to %d", pAP->alias, status);
    }
    pLink->configured = status;
}

bool whm_mxl_mlo_isLinkUsable(whm_mxl_link_t* pLink) {
    ASSERTS_NOT_NULL(pLink, false, ME, "NULL");
    ASSERTI_NOT_NULL(pLink->pLinkAp, false, ME, "Link has no AP assigned");
    ASSERTI_NOT_NULL(pLink->pMld, false, ME, "No MLD VAP assigned to link(%s)", pLink->pLinkAp->alias);
    bool status = true;

    if (!s_isMlBasicConditionsMet(pLink)) {
        SAH_TRACEZ_INFO(ME, "MLO: Link %s basic conditions not met", pLink->pLinkAp->alias);
        status = false;
    }
    if (!s_isMlSharedConfigValid(pLink)) {
        SAH_TRACEZ_INFO(ME, "MLO: Link %s shared config not valid", pLink->pLinkAp->alias);
        status = false;
    }
    s_saveLinkConfigStatus(pLink, status);
    return status;
}

/**
 * @brief Check if the AP object part of MLD and link is usable
 *
 * @param pAP AccessPoint
 * @return return true if AP object is MLO configured. False otherwise.
 */
bool whm_mxl_mlo_checkMloEnable(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, false, ME, "pAP is NULL");
    ASSERTI_TRUE(whm_mxl_mlo_isPartOfMld(pAP), false, ME, "%s is not part of MLD", pAP->alias);
    whm_mxl_mld_t *pMld = whm_mxl_mlo_getMldVap(pAP);
    ASSERTS_NOT_NULL(pMld, false, ME, "no MLD VAP assigned to %s", pAP->alias);
    whm_mxl_link_t *pLink = whm_mxl_mlo_getMldLink(pAP);
    ASSERTI_NOT_NULL(pLink, false, ME, "pLink is NULL");

    if (!whm_mxl_mlo_isLinkUsable(pLink)) {
        SAH_TRACEZ_INFO(ME, "MLO: Link %s is not usable", pAP->alias);
        return false;
    }

    /* MLO is considered enabled only for DUAL-BAND or TRIBAND configuration */
    if (pMld->mldType == MLD_TYPE_DUAL_LINK ||
        pMld->mldType == MLD_TYPE_TRI_LINK) {
        return true;
    }
    return false;
}

bool whm_mxl_mlo_isPartOfMld(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, false, ME, "pAP is NULL");
    mxl_VapVendorData_t* pVapVendor = mxl_vap_getVapVendorData(pAP);
    ASSERT_NOT_NULL(pVapVendor, false, ME, "VapVendorData is NULL");
    ASSERTS_NOT_NULL(pVapVendor->pMld, false, ME, "no MLD VAP assigned to %s", pAP->alias);
    ASSERTI_NOT_NULL(pVapVendor->pLink, false, ME, "pLink is NULL");

    return true;
}

bool whm_mxl_mlo_checkMldConfigChange(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, false, ME, "pAP is NULL");
    ASSERTI_TRUE(whm_mxl_mlo_isPartOfMld(pAP), false, ME, "%s is not part of MLD", pAP->alias);
    whm_mxl_link_t *pLink = whm_mxl_mlo_getMldLink(pAP);
    ASSERTS_NOT_NULL(pLink, false, ME, "pLink is NULL");
    bool curIsConfigured = pLink->configured;
    bool isUsable = whm_mxl_mlo_isLinkUsable(pLink);
    if (curIsConfigured != isUsable) {
        return true;
    }
    return false;
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
    pNewLink->configured = false;
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

    if (!s_isMlBasicConditionsMet(pNewLink) || !s_isMlSharedConfigValid(pNewLink)) {
        /* Configuration between links not aligned yet - skip restart */
        return SWL_RC_CONTINUE;
    }

    if ((pMld->mldType == MLD_TYPE_DUAL_LINK) ||
        (pMld->mldType == MLD_TYPE_TRI_LINK)) {
        whm_mxl_restartHapd(pAP->pRadio);
    } else {
        SAH_TRACEZ_INFO(ME, "MLO: New link created with MLO ID %d - waiting for more links!", mloId);
    }

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
    whm_mxl_mld_type_e prevType = pMld->mldType;
    bool needRestart;

    // Clear ApMldMac if was created from one of the links MAC
    if (!pMld->externalMldMac) {
        amxd_object_t* pMlObj = whm_mxl_mlo_getMloObject(pAP);
        ASSERT_NOT_NULL(pMlObj, SWL_RC_ERROR, ME, "MLO is not Mapped");
        amxd_object_set_value(cstring_t, pMlObj, "ApMldMac", SWL_MAC_CHAR_NULL);
    }

    // Remove link from MLD VAP list - free link memory
    amxc_llist_it_take(&pLink->it);
    free(pLink);
    // Unlink MLD from VAP Vendor data
    pVendor->pLink = NULL;
    pVendor->pMld = NULL;

    SAH_TRACEZ_INFO(ME, "MLO: MLD(%d), Deleted link (%s)", pMld->mloId, pAP->alias);

    s_setMldType(pMld);

    needRestart = ((prevType == MLD_TYPE_SINGLE_LINK) && (pMld->mldType == MLD_TYPE_NONE)) ? false : true;

    // If last link in MLD VAP - delete MLD VAP
    if (amxc_llist_is_empty(&pMld->affiliatedLinks)) {
        SAH_TRACEZ_INFO(ME, "MLO: Last link removed - deleting MLD VAP with MLO ID (%d)", pMld->mloId);
        s_deleteMldVap(pMld);
    }

    if (needRestart) {
        whm_mxl_restartHapd(pAP->pRadio);
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
        whm_mxl_restartHapd(pAP->pRadio);
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

whm_mxl_mld_mngr_t *whm_mxl_mlo_get_mldMngr(void) {
    return &mldMngr;
}

swl_rc_ne whm_mxl_mlo_initMld(void) {
    whm_mxl_mld_mngr_t *pMldMngr = whm_mxl_mlo_get_mldMngr();
    amxc_llist_init(&pMldMngr->mlds);
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
    pMldMngr->init = false;
    SAH_TRACEZ_WARNING(ME, "MLO: MLD deinit done");
    return SWL_RC_OK;
}

amxd_status_t _whm_mxl_mlo_dumpApMlds(amxd_object_t* object _UNUSED,
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
        amxc_var_add_key(bool, pEntry, "ExternalMAC", pMld->externalMldMac);
        whm_mxl_link_t *pLink = NULL;
        amxc_string_t linksStr;
        amxc_string_init(&linksStr, 0);
        whm_mxl_mlo_forEachLinkInMld(pLink, pMld) {
            T_AccessPoint* pLinkAp = pLink->pLinkAp;
            if (!pLinkAp) {
                SAH_TRACEZ_ERROR(ME, "MLO: no pAP for link in MLD(%d) - skipping", pMld->mloId);
                continue;
            }
            amxc_string_appendf(&linksStr, "%s,", pLinkAp->alias);
        }
        amxc_var_add_key(cstring_t, pEntry, "Vaps", amxc_string_get(&linksStr, 0));
        amxc_string_clean(&linksStr);
    }
    return amxd_status_ok;
}
