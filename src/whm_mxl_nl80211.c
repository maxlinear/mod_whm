/******************************************************************************

         Copyright (c) 2023 - 2025 MaxLinear, Inc.

  This software may be distributed under the terms of the BSD license.
  See README for more details.

*******************************************************************************/

/*******************************************************************************
*                                                                              *
*         File Name    : whm_mxl_nl80211.c                                     *
*         Description  : NL80211 vendor command APIs                           *
*                                                                              *
*******************************************************************************/

#include <swl/swl_common.h>
#include <swla/swla_mac.h>

#include "wld/wld.h"
#include "wld/wld_util.h"
#include "wld/wld_accesspoint.h"
#include "wld/wld_nl80211_compat.h"
#include "wld/wld_nl80211_api.h"
#include "wld/wld_ap_nl80211.h"

#include "whm_mxl_utils.h"
#include "whm_mxl_nl80211.h"
#include "whm_mxl_mlo.h"
#include <vendor_cmds_copy.h>

#define ME "mxlNlA"

/* Private data structure for s_getMldListCb */
typedef struct {
    whm_mxl_nl80211_apMld_t* pMldList;  /* Single pointer - allocated in callback */
    uint32_t count;
    swl_rc_ne rc;
} s_nl80211_apMldList_t;

/**
 * @brief Callback for parsing Vendor NL80211 GET_ML_VAP_LIST response
 *
 * The vendor data contains an array of whm_mxl_nl80211_apMld_t structures.
 * Allocates memory for the array which must be freed by caller using free().
 */
static swl_rc_ne s_getMldListCb(swl_rc_ne rc, struct nlmsghdr* nlh, void* priv) {
    s_nl80211_apMldList_t* pPriv = (s_nl80211_apMldList_t*)priv;
    ASSERT_NOT_NULL(pPriv, SWL_RC_ERROR, ME, "priv is NULL");

    /* Use helper function to extract vendor data */
    void* pVendorData = NULL;
    size_t vendorDataLen = 0;

    rc = wld_nl80211_getVendorDataFromVendorMsg(rc, nlh, &pVendorData, &vendorDataLen);
    if (rc < SWL_RC_OK) {
        SAH_TRACEZ_ERROR(ME, "Failed to get vendor data: %d", rc);
        goto cleanup;
    }

    /* check if vendor data is properly aligned to struct size */
    if (vendorDataLen % sizeof(whm_mxl_nl80211_apMld_t) != 0) {
        SAH_TRACEZ_ERROR(ME, "vendorDataLen %zu not aligned to apMld size %zu",
                         vendorDataLen, sizeof(whm_mxl_nl80211_apMld_t));
        rc = SWL_RC_ERROR;
        goto cleanup;
    }

    uint32_t count = vendorDataLen / sizeof(whm_mxl_nl80211_apMld_t);
    if (count > MAX_MLD_VAPS) {
        SAH_TRACEZ_ERROR(ME, "MLD count %u exceeds MAX_MLD_VAPS (%d)", count, MAX_MLD_VAPS);
        rc = SWL_RC_ERROR;
        goto cleanup;
    } else if (count == 0) {
        SAH_TRACEZ_INFO(ME, "No ML VAP entries in response");
        pPriv->pMldList = NULL;
        pPriv->count = 0;
        rc = SWL_RC_OK;
        goto cleanup;
    }

    SAH_TRACEZ_INFO(ME, "Received info on %u MLD(s) (data len: %zu bytes)",
                    count, vendorDataLen);

    /* Allocate memory for the whole chunk */
    pPriv->pMldList = malloc(vendorDataLen);
    if (!pPriv->pMldList) {
        SAH_TRACEZ_ERROR(ME, "Failed to allocate ML VAP list array (%zu bytes)", vendorDataLen);
        rc = SWL_RC_ERROR;
        goto cleanup;
    }

    /* Copy the entire vendor data chunk */
    memcpy(pPriv->pMldList, pVendorData, vendorDataLen);
    pPriv->count = count;
    rc = SWL_RC_OK;
    SAH_TRACEZ_INFO(ME, "Successfully parsed %u ML VAP list entries", count);

cleanup:
    pPriv->rc = rc;
    return rc;
}

swl_rc_ne whm_mxl_nl80211_getApMldList(
    T_AccessPoint* pAp,
    whm_mxl_nl80211_apMld_t** ppMldList,
    uint32_t* pCount
) {
    ASSERT_NOT_NULL(pAp, SWL_RC_INVALID_PARAM, ME, "pAp is NULL");
    ASSERT_NOT_NULL(ppMldList, SWL_RC_INVALID_PARAM, ME, "ppMldList is NULL");
    ASSERT_NOT_NULL(pCount, SWL_RC_INVALID_PARAM, ME, "pCount is NULL");

    s_nl80211_apMldList_t priv = {
        .pMldList = NULL,
        .count = 0,
        .rc = SWL_RC_ERROR
    };

    /* Initialize output */
    *ppMldList = NULL;
    *pCount = 0;

    SAH_TRACEZ_INFO(ME, "Sending LTQ_NL80211_VENDOR_SUBCMD_GET_ML_VAP_LIST for AP %s",
                    pAp->alias);

    /* Send vendor command - no input data needed for this command */
    swl_rc_ne rc = wld_ap_nl80211_sendVendorSubCmd(pAp, OUI_MXL,
        LTQ_NL80211_VENDOR_SUBCMD_GET_ML_VAP_LIST, NULL, 0, VENDOR_SUBCMD_IS_SYNC,
        VENDOR_SUBCMD_IS_WITHOUT_ACK, 0, s_getMldListCb, &priv);
    ASSERT_FALSE(rc < SWL_RC_OK, rc, ME, "Failed to send NL80211 cmd, rc %d", rc);

    /* Transfer ownership of allocated memory to caller */
    *ppMldList = priv.pMldList;
    *pCount = priv.count;

    return priv.rc;
}

/* Private data structure for s_getStaMldListCb */
typedef struct {
    whm_mxl_nl80211_staMld_t* pStaMldList;  /* Single pointer - allocated in callback */
    uint32_t count;
    swl_rc_ne rc;
} s_nl80211_staMldList_t;

/**
 * @brief Callback for parsing Vendor NL80211 GET_ML_STA_LIST response
 *
 * The vendor data contains an array of whm_mxl_nl80211_staMld_t structures.
 * Allocates memory for the array which must be freed by caller using free().
 */
static swl_rc_ne s_getStaMldListCb(swl_rc_ne rc, struct nlmsghdr* nlh, void* priv) {
    s_nl80211_staMldList_t* pPriv = (s_nl80211_staMldList_t*)priv;
    ASSERT_NOT_NULL(pPriv, SWL_RC_ERROR, ME, "priv is NULL");

    /* Use helper function to extract vendor data */
    void* pVendorData = NULL;
    size_t vendorDataLen = 0;

    rc = wld_nl80211_getVendorDataFromVendorMsg(rc, nlh, &pVendorData, &vendorDataLen);
    if (rc < SWL_RC_OK) {
        SAH_TRACEZ_ERROR(ME, "Failed to get vendor data: %d", rc);
        goto cleanup;
    }

    /* check if vendor data is properly aligned to struct size */
    if (vendorDataLen % sizeof(whm_mxl_nl80211_staMld_t) != 0) {
        SAH_TRACEZ_ERROR(ME, "Vendor data length %zu not aligned to staMld size %zu",
                         vendorDataLen, sizeof(whm_mxl_nl80211_staMld_t));
        rc = SWL_RC_ERROR;
        goto cleanup;
    }

    uint32_t count = vendorDataLen / sizeof(whm_mxl_nl80211_staMld_t);
    if (count > MAX_STA_MLD_COUNT) {
        SAH_TRACEZ_ERROR(ME, "STA MLD count %u exceeds %d", count, MAX_STA_MLD_COUNT);
        rc = SWL_RC_ERROR;
        goto cleanup;
    } else if (count == 0) {
        SAH_TRACEZ_INFO(ME, "No ML STA entries in response");
        pPriv->pStaMldList = NULL;
        pPriv->count = 0;
        rc = SWL_RC_OK;
        goto cleanup;
    }

    SAH_TRACEZ_INFO(ME, "Received info on %u STA MLD(s) (data len: %zu bytes)",
                    count, vendorDataLen);

    /* Allocate memory for the whole chunk */
    pPriv->pStaMldList = malloc(vendorDataLen);
    if (!pPriv->pStaMldList) {
        SAH_TRACEZ_ERROR(ME, "Failed to allocate ML STA list array (%zu bytes)", vendorDataLen);
        rc = SWL_RC_ERROR;
        goto cleanup;
    }

    /* Copy the entire vendor data chunk */
    memcpy(pPriv->pStaMldList, pVendorData, vendorDataLen);
    pPriv->count = count;
    rc = SWL_RC_OK;
    SAH_TRACEZ_INFO(ME, "Successfully parsed %u ML STA list entries", count);

cleanup:
    pPriv->rc = rc;
    return rc;
}

swl_rc_ne whm_mxl_nl80211_getStaMldList(
    T_AccessPoint* pAp,
    whm_mxl_nl80211_staMld_t** ppStaMldList,
    uint32_t* pCount
) {
    ASSERT_NOT_NULL(pAp, SWL_RC_INVALID_PARAM, ME, "pAp is NULL");
    ASSERT_NOT_NULL(ppStaMldList, SWL_RC_INVALID_PARAM, ME, "ppStaMldList is NULL");
    ASSERT_NOT_NULL(pCount, SWL_RC_INVALID_PARAM, ME, "pCount is NULL");

    s_nl80211_staMldList_t priv = {
        .pStaMldList = NULL,
        .count = 0,
        .rc = SWL_RC_ERROR
    };

    /* Initialize output */
    *ppStaMldList = NULL;
    *pCount = 0;

    SAH_TRACEZ_INFO(ME, "Sending LTQ_NL80211_VENDOR_SUBCMD_GET_ML_STA_LIST for AP %s",
                    pAp->alias ? pAp->alias : "unknown");

    /* Send vendor command - no input data needed for this command */
    swl_rc_ne rc = wld_ap_nl80211_sendVendorSubCmd(pAp, OUI_MXL,
        LTQ_NL80211_VENDOR_SUBCMD_GET_ML_STA_LIST, NULL, 0, VENDOR_SUBCMD_IS_SYNC,
        VENDOR_SUBCMD_IS_WITHOUT_ACK, 0, s_getStaMldListCb, &priv);
    ASSERT_FALSE(rc < SWL_RC_OK, rc, ME, "Failed to send NL80211 cmd, rc %d", rc);

    /* Transfer ownership of allocated memory to caller */
    *ppStaMldList = priv.pStaMldList;
    *pCount = priv.count;

    return priv.rc;
}
