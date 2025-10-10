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

#define ME "mxlMlo"

/**
 * @brief Counts number of links associated with an MLD using MloID,
 * excluding dummy VAP
 *
 * @param mloId ID of the MLD
 * @return return No. of links in the MLD
 */
int32_t whm_mxl_mlo_getLinkCount(int32_t mloId) {
    ASSERT_FALSE((mloId == NO_LINK_ID), 0, ME, "MLO: bad mloId(%d)", mloId);
    int32_t linkCount = 0;
    T_Radio* pRad = NULL;
    wld_for_eachRad(pRad) {
        T_AccessPoint* pAP = NULL;
        wld_rad_forEachAp(pAP, pRad) {
            mxl_VapVendorData_t* vapVendor = mxl_vap_getVapVendorData(pAP);
            if (vapVendor == NULL || (whm_mxl_utils_isDummyVap(pAP)))
                continue;
            if(vapVendor->mldLink.mloId == mloId)
                linkCount++;
        }
    }
    return linkCount;
}

/**
 * @brief Fetch the Sibling AP object based on the MloId, excluding dummy VAP
 *
 * @param pAP   AccessPoint
 * @param mloId MloId
 * @return return AP object with Matching MloId, otherwise NULL.
 */
T_AccessPoint* whm_mxl_mlo_getSiblingAP(T_AccessPoint* pAP, int32_t mloId) {
    ASSERTS_NOT_NULL(pAP, NULL, ME, "pAP is NULL");
    T_Radio* tRad = NULL;
    T_AccessPoint* tAP = NULL;
    ASSERT_FALSE((mloId == NO_LINK_ID), NULL, ME, "MLO: Source AP(%s) is not a link", pAP->alias);

    wld_for_eachRad(tRad) {
        if (tRad == pAP->pRadio)
            continue;
        wld_rad_forEachAp(tAP, tRad) {
            mxl_VapVendorData_t* vapVendor = mxl_vap_getVapVendorData(tAP);
            if (vapVendor == NULL || (whm_mxl_utils_isDummyVap(tAP)))
                continue;
            if(vapVendor->mldLink.mloId == mloId)
                return tAP;
        }
    }
    return NULL;
}

/**
 * @brief Check if the AP object is part of usable MLD
 *
 * @param pAP AccessPoint
 * @return return true if AP object is part of MLO VAP, false otherwise.
 */
bool whm_mxl_mlo_checkMloEnable(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, false, ME, "pAP is NULL");
    mxl_VapVendorData_t* vapVendor = mxl_vap_getVapVendorData(pAP);
    ASSERT_NOT_NULL(vapVendor, false, ME, "VapVendorData is NULL");

    if (vapVendor->mldLink.mloId == NO_LINK_ID ||
        whm_mxl_mlo_getSiblingAP(pAP, vapVendor->mldLink.mloId) == NULL)
        return false;

    return true;
}

amxd_object_t* whm_mxl_mlo_getMloObject(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, NULL, ME, "pAP is NULL");
    amxd_object_t* pVendorObj = amxd_object_get(pAP->pBus, "Vendor");
    ASSERT_NOT_NULL(pVendorObj, NULL, ME, "pVendorObj NULL");
    return amxd_object_get(pVendorObj, "MLO");
}


static swl_rc_ne s_createMldLink(T_AccessPoint* pAP, int32_t mloId) {
    int32_t mldLinkCount = whm_mxl_mlo_getLinkCount(mloId);
    SAH_TRACEZ_INFO(ME, "MLO: %d links in MLD(%d)", mldLinkCount, mloId);
    mxl_VapVendorData_t* vapVendor = mxl_vap_getVapVendorData(pAP);
    ASSERT_NOT_NULL(vapVendor, SWL_RC_ERROR, ME, "vapVendor is NULL");

    // Handle Max Link Situation
    ASSERT_FALSE((mldLinkCount >= MAX_MLD_LINKS), SWL_RC_ERROR, ME,
                 "MLO: MLD(%d) already used by %d links", mloId, mldLinkCount);

    // Handle Single Link MLD
    // Since MLD is not to be configured in hostapd yet, restart is triggered
    // after second link is set.
    if (mldLinkCount == 0) {
        SAH_TRACEZ_INFO(ME, "MLO: Single Link MLD(%d)! Set next link", mloId);
        vapVendor->mldLink.mloId = mloId;
        return SWL_RC_OK;
    }

    // Fetch the Sibling AP with matching MloID & other parameters
    T_AccessPoint* pSiblingAP = whm_mxl_mlo_getSiblingAP(pAP, mloId);
    ASSERT_NOT_NULL(pSiblingAP, SWL_RC_ERROR, ME, "sibling AP is not found");
    T_SSID* pSSID = (T_SSID*) pAP->pSSID;
    T_SSID* pSiblingSSID = (T_SSID*) pSiblingAP->pSSID;
    amxd_object_t* pMLO = whm_mxl_mlo_getMloObject(pAP);
    ASSERT_NOT_NULL(pMLO, SWL_RC_ERROR, ME, "MLO is not Mapped");
    amxd_object_t* pSiblingMLO = whm_mxl_mlo_getMloObject(pSiblingAP);
    ASSERT_NOT_NULL(pSiblingMLO, SWL_RC_ERROR, ME, "Sibling MLO is not Mapped");
    mxl_VapVendorData_t* sibVapVendor = mxl_vap_getVapVendorData(pSiblingAP);
    ASSERT_NOT_NULL(sibVapVendor, SWL_RC_ERROR, ME, "vapVendor is NULL");
    SAH_TRACEZ_INFO(ME, "MLO: %s Sibling link is %s with MloID %d",
                    pAP->alias, pSiblingAP->alias, mloId);

    // Validate MLD parameters (SSID, Security)
    ASSERT_TRUE(swl_str_matches(pSSID->SSID, pSiblingSSID->SSID), SWL_RC_ERROR,
                ME, "MLO: Mismatch of SSID between links (%s & %s)",
                pSSID->SSID, pSiblingSSID->SSID);
    ASSERT_TRUE(wld_ap_sec_checkSharedSecConfigs(pAP, pSiblingAP), SWL_RC_ERROR,
                ME, "MLO: Mismatch of Security Mode between links (%s & %s)",
                pAP->alias, pSiblingAP->alias);

    // Set apMldMac for both links with the current link BSSID
    swl_macChar_t mainLinkMac;
    SWL_MAC_BIN_TO_CHAR(&mainLinkMac, pSSID->BSSID);
    swl_str_copy(vapVendor->mldLink.apMldMac.cMac, SWL_MAC_CHAR_LEN, mainLinkMac.cMac);
    swl_str_copy(sibVapVendor->mldLink.apMldMac.cMac, SWL_MAC_CHAR_LEN, mainLinkMac.cMac);
    amxd_object_set_value(cstring_t, pMLO, "ApMldMac", mainLinkMac.cMac);
    amxd_object_set_value(cstring_t, pSiblingMLO, "ApMldMac", mainLinkMac.cMac);

    vapVendor->mldLink.mloId = mloId;

    // Dynamic Re-Configuration support is not present for MxL MLO yet in
    // below layers. So, restart is triggered.
    whm_mxl_restartHapd(pAP->pRadio);

    return SWL_RC_OK;
}

static swl_rc_ne s_deleteMldLink(T_AccessPoint* pAP, int32_t mloId) {
    int32_t mldLinkCount = whm_mxl_mlo_getLinkCount(mloId);
    SAH_TRACEZ_INFO(ME, "MLO: %d links in MLD(%d)", mldLinkCount, mloId);

    // Anomaly Detection... Link Count must be < 3 and > 0
    ASSERT_FALSE((mldLinkCount == 0), SWL_RC_ERROR, ME,
                 "MLO: MLD(%d) has no Links, Deletion failed", mloId);
    ASSERT_FALSE((mldLinkCount > MAX_MLD_LINKS), SWL_RC_ERROR, ME,
                 "MLO: MLD(%d) has more than 2 Links, Deletion failed", mloId);

    // Clear ApMldMac & MloId
    amxd_object_t* pMLO = whm_mxl_mlo_getMloObject(pAP);
    ASSERT_NOT_NULL(pMLO, SWL_RC_ERROR, ME, "MLO is not Mapped");
    mxl_VapVendorData_t* vapVendor = mxl_vap_getVapVendorData(pAP);
    ASSERT_NOT_NULL(vapVendor, SWL_RC_ERROR, ME, "vapVendor is NULL");

    swl_mac_charClear(&vapVendor->mldLink.apMldMac);
    amxd_object_set_value(cstring_t, pMLO, "ApMldMac", SWL_MAC_CHAR_NULL);
    vapVendor->mldLink.mloId = NO_LINK_ID;

    // Dynamic Re-Configuration support is not present for MxL MLO yet in
    // below layers. So, restart is triggered after last link is deleted.
    if (mldLinkCount == 1) {
        whm_mxl_restartHapd(pAP->pRadio);
        return SWL_RC_OK;
    }

    SAH_TRACEZ_INFO(ME, "MLO: Waiting for last link deletion to trigger restart");
    return SWL_RC_OK;
}

static swl_rc_ne s_moveMldLink(T_AccessPoint* pAP, int32_t currMloId,
                               int32_t newMloId) {
    if (s_deleteMldLink(pAP, currMloId) < SWL_RC_OK)
        return SWL_RC_ERROR;
    SAH_TRACEZ_INFO(ME, "MLO: MLD(%d), Deleted link(%s)", currMloId, pAP->alias);

    if (s_createMldLink(pAP, newMloId) < SWL_RC_OK)
        return SWL_RC_ERROR;
    SAH_TRACEZ_INFO(ME, "MLO: MLD(%d), Created link(%s)", newMloId, pAP->alias);

    return SWL_RC_OK;
}

static whm_mxl_mld_link_action_e s_getMldLinkAction(int32_t currMloId, int32_t newMloId) {
    if (currMloId == newMloId)
        return MLD_LINK_NONE;
    else if (currMloId == NO_LINK_ID && newMloId != NO_LINK_ID)
        return MLD_LINK_CREATE;
    else if (currMloId != NO_LINK_ID && newMloId == NO_LINK_ID)
        return MLD_LINK_DELETE;
    else
        return MLD_LINK_MOVE;
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
 * | NO_LINK_ID | NO_LINK_ID |  No Change  |
 * | NO_LINK_ID |     X      | Create Link |
 * |     X      | NO_LINK_ID | Delete Link |
 * |     X      |     Y      |  Move Link  |
 *
 * @param[in] pAP         Pointer to the Access Point structure.
 * @param[in] currMloId   Current MLO ID associated with the AP.
 * @param[in] newMloId    New MLO ID to be associated with the AP.
 * @return swl_rc_ne
 */
swl_rc_ne whm_mxl_mlo_confVap(T_AccessPoint* pAP, int32_t currMloId, int32_t newMloId) {
    ASSERT_NOT_NULL(pAP, SWL_RC_INVALID_PARAM, ME, "pAP is NULL");
    ASSERT_FALSE((currMloId < NO_LINK_ID), SWL_RC_INVALID_PARAM, ME,
                 "MLO: bad mloId(%d)", currMloId);
    ASSERT_FALSE((newMloId < NO_LINK_ID), SWL_RC_INVALID_PARAM, ME,
                 "MLO: bad mloId(%d)", newMloId);

    whm_mxl_mld_link_action_e linkAction = s_getMldLinkAction(currMloId, newMloId);

    switch (linkAction) {
        case MLD_LINK_NONE: {
            ASSERTI_FALSE(true, SWL_RC_OK, ME,
                         "MLO: No change in Link(%s) with MloID %d",
                         pAP->alias, currMloId);
        }
        case MLD_LINK_CREATE: {
            if (s_createMldLink(pAP, newMloId) >= SWL_RC_OK) {
                SAH_TRACEZ_INFO(ME, "MLO: Created a link(%s) with MloID %d",
                                pAP->alias, newMloId);
                return SWL_RC_OK;
            } else {
                SAH_TRACEZ_ERROR(ME, "MLO: Failed to create link(%s) with MloID %d",
                                 pAP->alias, newMloId);
                return SWL_RC_ERROR;
            }
        }
        case MLD_LINK_DELETE: {
            if (s_deleteMldLink(pAP, currMloId) >= SWL_RC_OK) {
                SAH_TRACEZ_INFO(ME, "MLO: Deleted link(%s) with MloID %d",
                                pAP->alias, currMloId);
                return SWL_RC_OK;
            } else {
                SAH_TRACEZ_ERROR(ME, "MLO: Failed to delete link(%s) with MloID %d",
                                 pAP->alias, currMloId);
                return SWL_RC_ERROR;
            }
        }
        case MLD_LINK_MOVE: {
            if (s_moveMldLink(pAP, currMloId, newMloId) >= SWL_RC_OK) {
                SAH_TRACEZ_INFO(ME, "MLO: Moved the link(%s) from %d to %d",
                                pAP->alias, currMloId, newMloId);
                return SWL_RC_OK;
            } else {
                SAH_TRACEZ_ERROR(ME, "MLO: Failed to move link(%s) from %d to %d",
                                 pAP->alias, currMloId, newMloId);
                return SWL_RC_ERROR;
            }
        }
        default:
            SAH_TRACEZ_ERROR(ME, "MLO: Invalid link action(%d) for link(%s)",
                             linkAction, pAP->alias);
    }

    return SWL_RC_NOT_IMPLEMENTED;
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
    amxd_object_t* pMLO = whm_mxl_mlo_getMloObject(pAP);
    ASSERT_NOT_NULL(pMLO, SWL_RC_ERROR, ME, "MLO is not Mapped");
    mxl_VapVendorData_t* vapVendor = mxl_vap_getVapVendorData(pAP);
    int32_t currMloId = vapVendor->mldLink.mloId;
    int32_t newMloId = pAP->pSSID->mldUnit;

    SAH_TRACEZ_INFO(ME, "MLO: modify MloId(%d) with MLDUnit (%d) for %s",
                    currMloId, newMloId, pAP->alias);

    return whm_mxl_mlo_confVap(pAP, currMloId, newMloId);
}
