/******************************************************************************

         Copyright (c) 2023 - 2025 MaxLinear, Inc.

  This software may be distributed under the terms of the BSD license.
  See README for more details.

*******************************************************************************/

#ifndef __WHM_MXL_NL80211_H__
#define __WHM_MXL_NL80211_H__

#include <swl/swl_common.h>
#include <vendor_cmds_copy.h>

#include "wld/wld.h"

typedef struct ml_vap_list whm_mxl_nl80211_apMld_t;
typedef struct mxl_ml_sta_list whm_mxl_nl80211_staMld_t;

/**
 * @brief Get APMLD list from driver via NL80211 vendor command
 *
 * Sends LTQ_NL80211_VENDOR_SUBCMD_GET_ML_VAP_LIST to retrieve the list of
 * APMLDs available/created. The driver response may contain multiple ml_vap_list
 * structures.
 *
 * @param pAp AccessPoint to query (interface for the NL command)
 * @param ppMldList Output pointer to list of APMLD entries.
 *              Memory is allocated by this function and must be
 *              freed by caller using free()
 * @param pCount Output count of entries in the array
 * @return swl_rc_ne SWL_RC_OK on success, error code otherwise
 */
swl_rc_ne whm_mxl_nl80211_getApMldList(
    T_AccessPoint* pAp,
    whm_mxl_nl80211_apMld_t** ppMldList,
    uint32_t* pCount
);

/**
 * @brief Get STA MLD list from driver via NL80211 vendor command
 *
 * Sends LTQ_NL80211_VENDOR_SUBCMD_GET_ML_STA_LIST to retrieve the list of
 * MLO-capable STAs connected on the given AP(Link). The driver response may
 * contain multiple mxl_ml_sta_list structures.
 *
 * NOTE: This command only returns valid data when queried on an enabled AP that
 * is part of an MLD (affiliated link).
 *
 * @param pAp AccessPoint to query (must be an affiliated link in an MLD)
 * @param ppStaMldList Output pointer to list of STA MLD entries.
 *                 Memory is allocated by this function and must be
 *                 freed by caller using free()
 * @param pCount Output count of entries in the array
 * @return swl_rc_ne SWL_RC_OK on success, error code otherwise
 */
swl_rc_ne whm_mxl_nl80211_getStaMldList(
    T_AccessPoint* pAp,
    whm_mxl_nl80211_staMld_t** ppStaMldList,
    uint32_t* pCount
);

#endif /* __WHM_MXL_NL80211_H__ */
