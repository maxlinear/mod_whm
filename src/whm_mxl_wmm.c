/******************************************************************************

         Copyright (c) 2023 - 2025 MaxLinear, Inc.

  This software may be distributed under the terms of the BSD license.
  See README for more details.

*******************************************************************************/

/* *****************************************************************************
*         File Name    : whm_mxl_wmm.c                                         *
*         Description  : WMM statistics related API                            *
*                                                                              *
*  *****************************************************************************/

#include <swl/swl_common.h>
#include "swla/swla_exec.h"

#include "wld/wld.h"
#include "wld/wld_radio.h"

#include "whm_mxl_rad.h"
#include "whm_mxl_vap.h"
#include "whm_mxl_utils.h"

#define ME "mxlWmm"

#define MXL_WMM_CHECK_EXEC(result, label, ...) \
    do { \
        if ((result).outBuf) { free((result).outBuf); (result).outBuf = NULL; } \
        if ((result).errBuf) { free((result).errBuf); (result).errBuf = NULL; } \
        if ((result).exitInfo.isSignaled || ((result).exitInfo.exitStatus != 0)) { \
            SAH_TRACEZ_ERROR(ME, __VA_ARGS__); \
            goto label; \
        } \
    } while(0)

/*
 * Mapping WiFi Access Categories to Destination Queues
 * DSCP Range   Destination Queue   Access Category   TID
 * 0-7          Q3                  AC_BE             0
 * 8-15         Q4                  AC_BK             1
 * 16-23        Q4                  AC_BK             2
 * 24-31        Q3                  AC_BE             3
 * 32-39        Q2                  AC_VI             4
 * 40-47        Q2                  AC_VI             5
 * 48-55        Q1                  AC_VO             6
 * 56-63        Q1                  AC_VO             7
 */

#define MAX_NUM_WMM_QUEUES         5 /* Default (needed for verification), Background, Best Effort, Video, Voice */
#define WMM_MARK_TO_QUEUE_FILE     "/sys/kernel/debug/ppa/qos_helper/mark-to-queue"
/*
 * Example mark-to-queue format:
 * ...
 * wlan0.1 qos data:
 * Mark:   0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15
 * DP(q): 42   43   43   42   41   41   40   40   40   40   40   40   40   40   40   40
 *        BE   BK   BK   BE   VI   VI   VO   VO
 * ...
 *
 * Mark 0: Best Effort queue (0) : will be stored locally at index 0 to match WLD_AC_BE
 * Mark 1: Background queue  (1) : will be stored locally at index 1 to match WLD_AC_BK
 * Mark 4: Video queue       (2) : will be stored locally at index 2 to match WLD_AC_VI
 * Mark 6: Voice queue       (3) : will be stored locally at index 3 to match WLD_AC_VO
 * 
 * Mark:   0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15
 *         BE   BK   BK   BE   VI   VI   VO   VO
 */

#define WMM_PACKETS_STATS_FILE  "/sys/kernel/debug/pp/qos_queues_stats"
#define WMM_BYTES_STATS_FILE    "/sys/kernel/debug/pp/qos_queues_stats_bytes"
#define WMM_BPS_STATS_FILE      "/sys/kernel/debug/pp/qos_queues_bps" // Available but not used at the moment
/*
 * Example qos_queues_stats for packets and bytes format:
 * ...
 * +------------------+-------------+-------------+-------------+-------------+
 * |       Qnum       | Q Occupancy |   Forward   |  WRED Drop  | CoDel Drop  |
 * +------------------+-------------+-------------+-------------+-------------+
 * | 213(1008)-rlm-43 | 0           | 1715        | 0           | 0           |
 * | 218(1040)-rlm-41 | 0           | 164         | 0           | 0           |
 * | 219(1041)-rlm-40 | 0           | 82          | 0           | 0           |
 * | 220(1042)-rlm-42 | 0           | 339         | 0           | 0           |
 * +------------------+-------------+-------------+-------------+-------------+
 * ...
 *
 * Qnum:      Ends with number which maps to the Marks (read from mark-to-queue)
 * Forward:   Number of successful TX packets
 * WRED Drop: Number of failed TX packets
 *
 * NOTE: only TX packets/bytes available right now
 */

static const char* s_wmmQueuesStatusName[] = {"WMM_QUEUES_DISABLED", "WMM_QUEUES_ENABLED", "INVALID_STATUS"};

static void s_mxl_wmm_updateQueuesObj(T_AccessPoint* pAP, int* queues) {
    ASSERT_NOT_NULL(pAP, , ME, "pAP is NULL");
    /* WiFi.Accesspoint.{}. */
    amxd_object_t* apObj = pAP->pBus;
    ASSERT_NOT_NULL(apObj, , ME, "pBus is NULL");
    /* WiFi.Accesspoint.{}.Vendor. */
    amxd_object_t* apVendorObj = amxd_object_get(apObj, "Vendor");
    ASSERT_NOT_NULL(apVendorObj, , ME, "apVendorObj is NULL");
    /* WiFi.Accesspoint.{}.Vendor.WMMStats */
    amxd_object_t* wmmStatsObj = amxd_object_get(apVendorObj, "WMMStats");
    ASSERT_NOT_NULL(wmmStatsObj, , ME, "No WMMStats vendor obj");
    /* WiFi.Accesspoint.{}.Vendor.WMMStats.QueueNum */
    amxd_object_t* wmmQueueNumObj = amxd_object_get(wmmStatsObj, "QueueNum");
    ASSERT_NOT_NULL(wmmQueueNumObj, , ME, "No QueueNum vendor obj");
    /* MAYBE REPLACE ALL ABOVE WITH amxd_object_findf (WMMStats.QueueNum)? */

    SWLA_OBJECT_SET_PARAM_UINT32(wmmQueueNumObj, "BE_Q", queues[WLD_AC_BE]);
    SWLA_OBJECT_SET_PARAM_UINT32(wmmQueueNumObj, "BK_Q", queues[WLD_AC_BK]);
    SWLA_OBJECT_SET_PARAM_UINT32(wmmQueueNumObj, "VI_Q", queues[WLD_AC_VI]);
    SWLA_OBJECT_SET_PARAM_UINT32(wmmQueueNumObj, "VO_Q", queues[WLD_AC_VO]);
}

static void s_updateWmmStats(T_Stats* stats,
                             int acIdx,
                             uint32_t txValue,
                             uint32_t txValueFail,
                             whm_mxl_wmm_stat_type_e statType,
                             bool accumulate) {
    ASSERT_NOT_NULL(stats, , ME, "stats is NULL");
    if (accumulate) {
        switch (statType) {
            case WHM_MXL_WMM_STAT_TYPE_PACKETS:
                stats->WmmPacketsSent[acIdx] += txValue;
                stats->WmmFailedSent[acIdx] += txValueFail;
                break;
            case WHM_MXL_WMM_STAT_TYPE_BYTES:
                stats->WmmBytesSent[acIdx] += txValue;
                stats->WmmFailedBytesSent[acIdx] += txValueFail;
                break;
            /* If implemented - add new stats here */
            default:
                SAH_TRACEZ_ERROR(ME, "bad WMM stat type (%d)", statType);
                break;
        }
    } else {
        switch (statType) {
            case WHM_MXL_WMM_STAT_TYPE_PACKETS:
                stats->WmmPacketsSent[acIdx] = txValue;
                stats->WmmFailedSent[acIdx] = txValueFail;
                break;
            case WHM_MXL_WMM_STAT_TYPE_BYTES:
                stats->WmmBytesSent[acIdx] = txValue;
                stats->WmmFailedBytesSent[acIdx] = txValueFail;
                break;
            /* If implemented - add new stats here */
            default:
                SAH_TRACEZ_ERROR(ME, "bad WMM stat type (%d)", statType);
                break;
        }
    }
}

/**
 * @brief Retrieves and processes WMM statistics from a specified file.
 *
 * This function reads WMM statistics from a given file and updates the provided
 * statistics structure based on the specified accumulation mode and statistic type.
 *
 * @param pAP Pointer to the access point structure.
 * @param stats Pointer to the statistics structure to be updated.
 * @param queues Array of queue numbers to be processed.
 * @param accumulate Boolean flag indicating whether to accumulate statistics or overwrite them.
 * @param statsFile Path to the file containing the WMM statistics.
 * @param statType Type of statistics to be processed (packets or bytes).
 * @return swl_rc_ne Returns SWL_RC_OK on success, or SWL_RC_ERROR on failure.
 */
static swl_rc_ne s_wmmGetStats(T_AccessPoint* pAP,
                               T_Stats* stats,
                               int* queues,
                               bool accumulate,
                               const char* statsFile,
                               whm_mxl_wmm_stat_type_e statType) {
    ASSERT_NOT_NULL(queues, SWL_RC_ERROR, ME, "queues array is NULL");
    char line[128] = {0}; /* lines are up to 85 chars long, so size 128 should be fine */
    char wmm_marker[WLD_AC_MAX][16];
    int acIdx, numEntries;
    /* Convert queue number to line wmm marker */
    for (acIdx = 0; acIdx < WLD_AC_MAX; acIdx++) {
        snprintf(wmm_marker[acIdx], sizeof(wmm_marker[acIdx]), ")-rlm-%d", queues[acIdx]);
    }

    FILE *fp = fopen(statsFile, "r");
    ASSERT_NOT_NULL(fp, SWL_RC_ERROR, ME, "%s: Error opening stats file", pAP->alias);

    uint32_t txValue;
    uint32_t txValueFail;
    while (fgets(line, sizeof(line), fp) != NULL) {
        for (acIdx = 0; acIdx < WLD_AC_MAX; acIdx++) {
            if (!strstr(line, wmm_marker[acIdx]))
                continue;

            numEntries = sscanf(line, "|%*[^|]|%*[^|]|%u |%u", &txValue, &txValueFail);
            if (numEntries != 2) {
                SAH_TRACEZ_ERROR(ME, "%s: Cannot extract %d statistics", pAP->alias, acIdx);
                goto err;
            }
            s_updateWmmStats(stats, acIdx, txValue, txValueFail, statType, accumulate);
        }
    }
    fclose(fp);
    return SWL_RC_OK;

err:
    fclose(fp);
    return SWL_RC_ERROR;
}

static swl_rc_ne s_mxl_wmmGetPacketsStats(T_AccessPoint* pAP, T_Stats* stats, int* queues, bool accumulate) {
    return s_wmmGetStats(pAP, stats, queues, accumulate, WMM_PACKETS_STATS_FILE, WHM_MXL_WMM_STAT_TYPE_PACKETS);
}

static swl_rc_ne s_mxl_wmmGetBytesStats(T_AccessPoint* pAP, T_Stats* stats, int* queues, bool accumulate) {
    return s_wmmGetStats(pAP, stats, queues, accumulate, WMM_BYTES_STATS_FILE, WHM_MXL_WMM_STAT_TYPE_BYTES);
}

/* Check that mark to queue is properly conifgured - Should be in the following order
 * BE - 0 and 3 same
 * BK - 1 and 2 same
 * VI - 4 and 5 same
 * VO - 6 and 7 same
 * Mark:   0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15
 *         BE   BK   BK   BE   VI   VI   VO   VO
 */
static bool s_mxl_validateQueues(T_AccessPoint* pAP, mxl_mark_to_queue_t* pMarkToQ) {
    ASSERT_NOT_NULL(pAP, false, ME, "pAP is NULL");
    ASSERT_NOT_NULL(pMarkToQ, false, ME, "pMarkToQ is NULL");
    /* Verify that all the ACs are not the same - probably not initialized */
    if ((pMarkToQ->acBe[0] == pMarkToQ->acBk[0]) &&
        (pMarkToQ->acBk[0] == pMarkToQ->acVi[0]) &&
        (pMarkToQ->acVi[0] == pMarkToQ->acVo[0])) {
        SAH_TRACEZ_ERROR(ME, "%s: Queues not initialized  BE:[%d][%d] BK:[%d][%d] VI:[%d][%d] VO:[%d][%d]", pAP->alias,
                         pMarkToQ->acBe[0], pMarkToQ->acBe[1], pMarkToQ->acBk[0], pMarkToQ->acBk[1],
                         pMarkToQ->acVi[0], pMarkToQ->acVi[1], pMarkToQ->acVo[0], pMarkToQ->acVo[1]);
        return false;
    /* Verify AC are properly conifugred - checking invalid combinations */
    } else if ((pMarkToQ->acBe[0] != pMarkToQ->acBe[1]) ||
               (pMarkToQ->acBk[0] != pMarkToQ->acBk[1]) ||
               (pMarkToQ->acVi[0] != pMarkToQ->acVi[1]) ||
               (pMarkToQ->acVo[0] != pMarkToQ->acVo[1]) ||
               (pMarkToQ->acBe[0] == pMarkToQ->acBk[0]) ||
               (pMarkToQ->acBe[0] == pMarkToQ->acVi[0]) ||
               (pMarkToQ->acBe[0] == pMarkToQ->acVo[0])) {
        SAH_TRACEZ_ERROR(ME, "%s: Queues misconfigured BE:[%d][%d] BK:[%d][%d] VI:[%d][%d] VO:[%d][%d]", pAP->alias,
                         pMarkToQ->acBe[0], pMarkToQ->acBe[1], pMarkToQ->acBk[0], pMarkToQ->acBk[1],
                         pMarkToQ->acVi[0], pMarkToQ->acVi[1], pMarkToQ->acVo[0], pMarkToQ->acVo[1]);
        return false;
    }
    return true;
}

static swl_rc_ne s_mxl_getWmmStats(T_AccessPoint* pAP, T_Stats* stats, bool accumulate) {
    ASSERT_NOT_NULL(pAP, SWL_RC_ERROR, ME, "pAP is NULL");
    ASSERT_NOT_NULL(stats, SWL_RC_ERROR, ME, "stats is NULL");
    FILE *fp = fopen(WMM_MARK_TO_QUEUE_FILE, "r");
    ASSERT_NOT_NULL(fp, SWL_RC_ERROR, ME, "%s: Error opening mark 2 queue file", pAP->alias);
    char line[128] = {0}; /* lines are up to 85 chars long, so size 128 should be fine */
    bool found_interface = false;
    int numEntries;
    swl_rc_ne rc = SWL_RC_OK;
    char iface_marker[64] = {0};

    snprintf(iface_marker, sizeof(iface_marker), "%s qos data:", pAP->alias);
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, iface_marker)) {
            found_interface = true;
            break;
        }
    }

    if (!found_interface) {
        SAH_TRACEZ_INFO(ME, "%s: Interface not found", pAP->alias);
        goto err;
    }

    /* Read 'Mark:' line */
    if (!fgets(line, sizeof(line), fp)) {
        SAH_TRACEZ_ERROR(ME, "%s: Error reading Mark line", pAP->alias);
        goto err;
    }

    /* Read 'DP(q):' line */
    if (!fgets(line, sizeof(line), fp)) {
        SAH_TRACEZ_ERROR(ME, "%s: Error reading DP(q) line", pAP->alias);
        goto err;
    }

    /* Check 'DP(q):' line format and get values */
    int queueNum[WLD_AC_MAX] = {0};
    mxl_mark_to_queue_t markToQ = {0};
    numEntries = sscanf(line, "DP(q): %d %d %d %d %d %d %d %d",
                         &markToQ.acBe[0], &markToQ.acBk[0], &markToQ.acBk[1], &markToQ.acBe[1],
                         &markToQ.acVi[0], &markToQ.acVi[1], &markToQ.acVo[0], &markToQ.acVo[1]);
    if (numEntries != (WLD_AC_MAX * NUM_QUEUES_PER_AC)) {
        SAH_TRACEZ_ERROR(ME, "%s: Error: Invalid DP(q) line format (%d)(%s)", pAP->alias, numEntries, line);
        goto err;
    }

    /* Validate queue numbering is correct */
    if(!s_mxl_validateQueues(pAP, &markToQ)) {
        goto err;
    }

    fclose(fp);

    /* Save mark to queue numbering */
    WHM_MXL_SAVE_WMM_QUEUES(queueNum, markToQ);
    /* Sync queue numbering to DM */
    s_mxl_wmm_updateQueuesObj(pAP, queueNum);
    /* Get Packets Sent/Failed statistics */
    rc = s_mxl_wmmGetPacketsStats(pAP, stats, queueNum, accumulate);
    if (rc < SWL_RC_OK) {
        SAH_TRACEZ_ERROR(ME, "%s: Failed to get WMM packets stats", pAP->alias);
    }
    /* Get Bytes Sent/Failed statistics */
    rc = s_mxl_wmmGetBytesStats(pAP, stats, queueNum, accumulate);
    if (rc < SWL_RC_OK) {
        SAH_TRACEZ_ERROR(ME, "%s: Failed to get WMM bytes stats", pAP->alias);
    }

    return rc;

err:
    fclose(fp);
    return SWL_RC_ERROR;
}

static bool s_whm_mxl_isWmmStatsEnabled(T_Radio* pRad) {
    ASSERT_NOT_NULL(pRad, false, ME, "pRad is NULL");
    mxl_VendorData_t* pRadVendorData = mxl_rad_getVendorData(pRad);
    ASSERT_NOT_NULL(pRadVendorData, false, ME, "pRadVendorData is NULL");
    return pRadVendorData->wmmStatsEnable;
}

/**
 * @brief Fetch and fill WMM statistics for requested AP
 *
 * @param pAP accesspoint context
 * @param stats pointer to stats struct
 * @return return code of executed function.
 */
swl_rc_ne whm_mxl_getApWmmStats(T_AccessPoint* pAP, T_Stats* stats) {
    ASSERT_NOT_NULL(pAP, SWL_RC_INVALID_PARAM, ME, "pAP is NULL");
    ASSERT_NOT_NULL(stats, SWL_RC_INVALID_PARAM, ME, "stats is NULL");
    ASSERTS_FALSE(whm_mxl_utils_isDummyVap(pAP), SWL_RC_OK, ME, "%s: Skip WMM Queus stats for dummy vap", pAP->alias);
    ASSERTI_TRUE(s_whm_mxl_isWmmStatsEnabled(pAP->pRadio), SWL_RC_OK, ME, "%s: WMM stats disabled on radio", pAP->alias);
    ASSERTI_TRUE(pAP->enable && (pAP->status == APSTI_ENABLED), SWL_RC_OK, ME, "%s: Skip WMM stats from disabled AP", pAP->alias);
    return s_mxl_getWmmStats(pAP, stats, false);
}

/**
 * @brief Fetch and fill WMM statistics for requested radio
 *
 * @param pRad radio context
 * @param stats pointer to stats struct
 * @return return code of executed function.
 */
swl_rc_ne whm_mxl_getRadWmmStats(T_Radio* pRad, T_Stats* stats) {
    ASSERT_NOT_NULL(pRad, SWL_RC_INVALID_PARAM, ME, "pRad is NULL");
    ASSERT_NOT_NULL(stats, SWL_RC_INVALID_PARAM, ME, "stats is NULL");
    ASSERTI_TRUE(s_whm_mxl_isWmmStatsEnabled(pRad), SWL_RC_OK, ME, "%s: WMM stats disabled on radio", pRad->Name);
    swl_rc_ne rc = SWL_RC_OK;

    /* Iterate over all VAPs in radio and accumulate WMM statistics */
    T_AccessPoint* pAP = NULL;
    wld_rad_forEachAp(pAP, pRad) {
        /* Skip dummy VAP */
        if (pAP && whm_mxl_utils_isDummyVap(pAP)) {
            continue;
        }
        if (pAP && pAP->enable &&(pAP->status == APSTI_ENABLED)) {
            rc = s_mxl_getWmmStats(pAP, stats, true);
            if (rc < SWL_RC_OK) {
                SAH_TRACEZ_ERROR(ME, "%s: Failed to update WMM stats", pAP->alias);
            }
        }
    }
    return rc;
}

/**
 * @brief Initialize WMM queues using tc filter commands
 *
 * @param pAP accesspoint
 * @return return code of executed function.
 */
swl_rc_ne whm_mxl_initWmmQueues(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, SWL_RC_ERROR, ME, "pAP is NULL");
    ASSERTS_FALSE(whm_mxl_utils_isDummyVap(pAP), SWL_RC_OK, ME, "%s: Skip WMM Queus init for dummy vap", pAP->alias);
    ASSERT_TRUE(s_whm_mxl_isWmmStatsEnabled(pAP->pRadio), SWL_RC_INVALID_STATE, ME, "%s: WMM stats disabled on radio", pAP->alias);

    /* Init WMM Queus */
    swl_exec_result_t result;
    memset(&result, 0, sizeof(swl_exec_result_t));

    SAH_TRACEZ_INFO(ME, "%s: AP Enabeld - Going to init WMM", pAP->alias);

    /* Create Queus for default VAPs */
    SWL_EXEC_BUF_EXT(&result, "tc", "qdisc replace dev %s root handle 10: prio bands 4 priomap 3 2 1 0", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc qdisc replace dev failed", pAP->alias);

    /* Create queue mappings for each VAP interface queues according to WiFi Access Categories to Queue IDs mapping */
    SWL_EXEC_BUF_EXT(&result, "tc", "qdisc add dev %s handle ffff: clsact", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc qdisc add dev failed", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter add dev %s ingress pref 1 protocol all flower skip_sw classid 10:1 action ok cookie 0006", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter add dev failed pref 1", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter add dev %s ingress pref 2 protocol all flower skip_sw classid 10:1 action ok cookie 0007", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter add dev failed pref 2", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter add dev %s ingress pref 3 protocol all flower skip_sw classid 10:2 action ok cookie 0004", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter add dev failed pref 3", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter add dev %s ingress pref 4 protocol all flower skip_sw classid 10:2 action ok cookie 0005", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter add dev failed pref 4", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter add dev %s ingress pref 5 protocol all flower skip_sw classid 10:3 action ok cookie 0000", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter add dev failed pref 5", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter add dev %s ingress pref 6 protocol all flower skip_sw classid 10:3 action ok cookie 0003", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter add dev failed pref 6", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter add dev %s ingress pref 7 protocol all flower skip_sw classid 10:4 action ok cookie 0001", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter add dev failed pref 7", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter add dev %s ingress pref 8 protocol all flower skip_sw classid 10:4 action ok cookie 0002", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter add dev failed pref 8", pAP->alias);

    whm_mxl_wmmUpdateStatus(pAP, MXL_WMM_QUEUES_ENABLED);
    SAH_TRACEZ_INFO(ME, "%s: AP Enabled - WMM queues init DONE", pAP->alias);
    return SWL_RC_OK;

error:
    SAH_TRACEZ_ERROR(ME, "%s: AP Enabled - WMM queues init FAILED", pAP->alias);
    return SWL_RC_ERROR;
}

/**
 * @brief Deinitialize WMM queues using tc filter commands
 *
 * @param pAP accesspoint
 * @return return code of executed function.
 */
swl_rc_ne whm_mxl_deinitWmmQueues(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, SWL_RC_ERROR, ME, "pAP is NULL");
    ASSERTS_FALSE(whm_mxl_utils_isDummyVap(pAP), SWL_RC_OK, ME, "%s: Skip WMM Queues deinit for dummy vap", pAP->alias);
    ASSERT_TRUE(s_whm_mxl_isWmmStatsEnabled(pAP->pRadio), SWL_RC_INVALID_STATE, ME, "%s: wmm stats disabled on radio", pAP->alias);
    mxl_VapVendorData_t* mxlVapVendorData = mxl_vap_getVapVendorData(pAP);
    ASSERT_NOT_NULL(mxlVapVendorData, SWL_RC_ERROR, ME, "mxlVapVendorData is NULL");
    ASSERT_FALSE((mxlVapVendorData->mxlWmmQueuesStatus == MXL_WMM_QUEUES_DISABLED), SWL_RC_INVALID_STATE, ME, 
                 "%s: WMM queues status already in disabled state", pAP->alias);

    /* Deinit WMM Queues */
    swl_exec_result_t result;
    memset(&result, 0, sizeof(swl_exec_result_t));

    SAH_TRACEZ_INFO(ME, "%s: AP Disabled - Going to deinit WMM", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter del dev %s ingress pref 8 protocol all flower skip_sw classid 10:4 action ok cookie 0002", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter del dev failed pref 8", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter del dev %s ingress pref 7 protocol all flower skip_sw classid 10:4 action ok cookie 0001", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter del dev failed pref 7", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter del dev %s ingress pref 6 protocol all flower skip_sw classid 10:3 action ok cookie 0003", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter del dev failed pref 6", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter del dev %s ingress pref 5 protocol all flower skip_sw classid 10:3 action ok cookie 0000", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter del dev failed pref 5", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter del dev %s ingress pref 4 protocol all flower skip_sw classid 10:2 action ok cookie 0005", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter del dev failed pref 4", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter del dev %s ingress pref 3 protocol all flower skip_sw classid 10:2 action ok cookie 0004", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter del dev failed pref 3", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter del dev %s ingress pref 2 protocol all flower skip_sw classid 10:1 action ok cookie 0007", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter del dev failed pref 2", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "filter del dev %s ingress pref 1 protocol all flower skip_sw classid 10:1 action ok cookie 0006", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc filter del dev failed pref 1", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "qdisc del dev %s handle ffff: clsact", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc qdisc del dev failed", pAP->alias);

    SWL_EXEC_BUF_EXT(&result, "tc", "qdisc del dev %s root handle 10: prio bands 4 priomap 3 2 1 0", pAP->alias);
    MXL_WMM_CHECK_EXEC(result, error, "%s: tc qdisc del dev failed", pAP->alias);

    whm_mxl_wmmUpdateStatus(pAP, MXL_WMM_QUEUES_DISABLED);
    SAH_TRACEZ_INFO(ME, "%s: AP Disabled - WMM queues deinit DONE", pAP->alias);
    return SWL_RC_OK;

error:
    SAH_TRACEZ_ERROR(ME, "%s: AP Disabled - WMM queues deinit FAILED", pAP->alias);
    return SWL_RC_ERROR;
}

static void s_whm_mxl_updateApWmmStatusObj(T_AccessPoint* pAP, whm_mxl_wmm_status_e status) {
    ASSERT_NOT_NULL(pAP, , ME, "pAP is NULL");
    /* WiFi.Accesspoint.{}. */
    amxd_object_t* apObj = pAP->pBus;
    ASSERT_NOT_NULL(apObj, , ME, "pBus is NULL");
    /* WiFi.Accesspoint.{}.Vendor. */
    amxd_object_t* vendorObj = amxd_object_get(apObj, "Vendor");
    ASSERT_NOT_NULL(vendorObj, , ME, "vendorObj is NULL");
    /* WiFi.Accesspoint.{}.Vendor.WMMStats. */
    amxd_object_t* wmmStatsObj = amxd_object_get(vendorObj, "WMMStats");
    ASSERT_NOT_NULL(wmmStatsObj, , ME, "No WMMStats vendor obj");
    switch(status) {
        case MXL_WMM_QUEUES_DISABLED: 
            SWLA_OBJECT_SET_PARAM_CSTRING(wmmStatsObj, "Status", "Disabled");
            break;
        case MXL_WMM_QUEUES_ENABLED:
            SWLA_OBJECT_SET_PARAM_CSTRING(wmmStatsObj, "Status", "Enabled");
            break;
        default:
            SWLA_OBJECT_SET_PARAM_CSTRING(wmmStatsObj, "Status", "Invalid");
            break;
    }
}

/**
 * @brief Update WMM queues status in AP context and data model.
 *
 * @param pAP accesspoint context
 * @param status new status of WMM queues
 * @return return code of executed function.
 */
bool whm_mxl_wmmUpdateStatus(T_AccessPoint* pAP, whm_mxl_wmm_status_e status) {
    ASSERT_NOT_NULL(pAP, false, ME, "pAP is NULL");
    ASSERT_TRUE((status < MXL_WMM_QUEUES_MAX), false, ME, "%s: invalid WMM status", pAP->alias);
    mxl_VapVendorData_t* mxlVapVendorData = mxl_vap_getVapVendorData(pAP);
    ASSERT_NOT_NULL(mxlVapVendorData, false, ME, "mxlVapVendorData is NULL");
    SAH_TRACEZ_INFO(ME, "%s: Updating WMM status [%s] --> [%s]", pAP->alias,
                    s_wmmQueuesStatusName[mxlVapVendorData->mxlWmmQueuesStatus],
                    s_wmmQueuesStatusName[status]);
    mxlVapVendorData->mxlWmmQueuesStatus = status;
    s_whm_mxl_updateApWmmStatusObj(pAP, status);
    return true;
}

whm_mxl_wmm_status_e whm_mxl_wmmGetStatus(T_AccessPoint* pAP) {
    ASSERT_NOT_NULL(pAP, MXL_WMM_QUEUES_MAX, ME, "pAP is NULL");
    mxl_VapVendorData_t* mxlVapVendorData = mxl_vap_getVapVendorData(pAP);
    ASSERT_NOT_NULL(mxlVapVendorData, MXL_WMM_QUEUES_MAX, ME, "mxlVapVendorData is NULL");
    return mxlVapVendorData->mxlWmmQueuesStatus;
}

static void s_setWmmStats(amxc_var_t* pRetMap, T_Stats* pStats) {
    /* Packets Sent */
    amxc_var_add_key(uint32_t, pRetMap, "PacketsSent[BE]", pStats->WmmPacketsSent[WLD_AC_BE]);
    amxc_var_add_key(uint32_t, pRetMap, "PacketsSent[BK]", pStats->WmmPacketsSent[WLD_AC_BK]);
    amxc_var_add_key(uint32_t, pRetMap, "PacketsSent[VI]", pStats->WmmPacketsSent[WLD_AC_VI]);
    amxc_var_add_key(uint32_t, pRetMap, "PacketsSent[VO]", pStats->WmmPacketsSent[WLD_AC_VO]);
    /* Packets Failed */
    amxc_var_add_key(uint32_t, pRetMap, "PacketsFailed[BE]", pStats->WmmFailedSent[WLD_AC_BE]);
    amxc_var_add_key(uint32_t, pRetMap, "PacketsFailed[BK]", pStats->WmmFailedSent[WLD_AC_BK]);
    amxc_var_add_key(uint32_t, pRetMap, "PacketsFailed[VI]", pStats->WmmFailedSent[WLD_AC_VI]);
    amxc_var_add_key(uint32_t, pRetMap, "PacketsFailed[VO]", pStats->WmmFailedSent[WLD_AC_VO]);
    /* Bytes Sent */
    amxc_var_add_key(uint32_t, pRetMap, "BytesSent[BE]", pStats->WmmBytesSent[WLD_AC_BE]);
    amxc_var_add_key(uint32_t, pRetMap, "BytesSent[BK]", pStats->WmmBytesSent[WLD_AC_BK]);
    amxc_var_add_key(uint32_t, pRetMap, "BytesSent[VI]", pStats->WmmBytesSent[WLD_AC_VI]);
    amxc_var_add_key(uint32_t, pRetMap, "BytesSent[VO]", pStats->WmmBytesSent[WLD_AC_VO]);
    /* Bytes Failed */
    amxc_var_add_key(uint32_t, pRetMap, "BytesFailed[BE]", pStats->WmmFailedBytesSent[WLD_AC_BE]);
    amxc_var_add_key(uint32_t, pRetMap, "BytesFailed[BK]", pStats->WmmFailedBytesSent[WLD_AC_BK]);
    amxc_var_add_key(uint32_t, pRetMap, "BytesFailed[VI]", pStats->WmmFailedBytesSent[WLD_AC_VI]);
    amxc_var_add_key(uint32_t, pRetMap, "BytesFailed[VO]", pStats->WmmFailedBytesSent[WLD_AC_VO]);
}

amxd_status_t _whm_mxl_wmm_getWmmVapStats(amxd_object_t* object,
                                          amxd_function_t* func _UNUSED,
                                          amxc_var_t* args _UNUSED,
                                          amxc_var_t* retval) {
    /* WiFi.AccessPoint.{}.Vendor. */
    amxd_object_t* apObj = amxd_object_get_parent(object);
    ASSERT_NOT_NULL(apObj, amxd_status_unknown_error, ME, "apObj is NULL");
    T_AccessPoint* pAP = (T_AccessPoint*) apObj->priv;
    ASSERT_NOT_NULL(pAP, amxd_status_unknown_error, ME, "pAP is NULL");
    ASSERT_NOT_NULL(pAP->pSSID, amxd_status_unknown_error, ME, "SSID is NULL");
    swl_rc_ne rc = SWL_RC_ERROR;
    SAH_TRACEZ_INFO(ME, "%s: Getting VAP WMM stats", pAP->alias);
    rc = whm_mxl_getApWmmStats(pAP, &pAP->pSSID->stats);
    amxc_var_init(retval);
    amxc_var_set_type(retval, AMXC_VAR_ID_HTABLE);
    s_setWmmStats(retval, &pAP->pSSID->stats);
    amxc_var_add_key(cstring_t, retval, "Result", swl_rc_toString(rc));
    return amxd_status_ok;
}

amxd_status_t _whm_mxl_wmm_getWmmRadStats(amxd_object_t* object,
                                          amxd_function_t* func _UNUSED,
                                          amxc_var_t* args _UNUSED,
                                          amxc_var_t* retval) {
    /* WiFi.Radio.{}.Vendor. */
    amxd_object_t* radObj = amxd_object_get_parent(object);
    ASSERT_NOT_NULL(radObj, amxd_status_unknown_error, ME, "vendorObj is NULL");
    T_Radio* pRad = (T_Radio*) radObj->priv;
    ASSERT_NOT_NULL(pRad, amxd_status_unknown_error, ME, "pRad is NULL");
    T_Stats stats = {0};
    swl_rc_ne rc = SWL_RC_ERROR;
    SAH_TRACEZ_INFO(ME, "%s: Getting Radio WMM stats", pRad->Name);
    rc = whm_mxl_getRadWmmStats(pRad, &stats);
    amxc_var_init(retval);
    amxc_var_set_type(retval, AMXC_VAR_ID_HTABLE);
    s_setWmmStats(retval, &stats);
    amxc_var_add_key(cstring_t, retval, "Result", swl_rc_toString(rc));
    return amxd_status_ok;
}

static swl_rc_ne s_wmmCheckEnable(T_Radio* pRad, bool enable) {
    ASSERT_NOT_NULL(pRad, SWL_RC_ERROR, ME, "pRad is NULL");
    ASSERTI_FALSE((pRad->status == RST_ERROR) || (pRad->status == RST_UNKNOWN), SWL_RC_INVALID_STATE, ME, "%s: Radio not enabled", pRad->Name);
    ASSERTI_TRUE(wld_secDmn_isAlive(pRad->hostapd), SWL_RC_INVALID_STATE, ME, "hostapd not active");
    T_AccessPoint* pAP = NULL;

    wld_rad_forEachAp(pAP, pRad) {
        /* Skip dummy VAP or disabled VAPs */
        if (!pAP || whm_mxl_utils_isDummyVap(pAP) || (pAP->status != APSTI_ENABLED)) {
            continue;
        }
        if (enable) {
            if (whm_mxl_initWmmQueues(pAP) < SWL_RC_OK) {
                SAH_TRACEZ_ERROR(ME, "%s: Failed to init WMM queues - Dynamic Enable", pAP->alias);
            }
        } else {
            if (whm_mxl_deinitWmmQueues(pAP) < SWL_RC_OK) {
                SAH_TRACEZ_ERROR(ME, "%s: Failed to deinit WMM queues - Dynamic Disable", pAP->alias);
            }
        }
    }

    return SWL_RC_OK;
}

static void s_setWmmStatsEnable_pwf(void* priv _UNUSED,
                                    amxd_object_t* object,
                                    amxd_param_t* param _UNUSED,
                                    const amxc_var_t* const newParamValues) {
    SAH_TRACEZ_IN(ME);
    /* WiFi.Radio.{}.Vendor.WMMStats */
    amxd_object_t* radObj = amxd_object_get_parent(amxd_object_get_parent(object));
    T_Radio* pRad = wld_rad_fromObj(radObj);
    ASSERT_NOT_NULL(pRad, , ME, "No Radio Mapped");
    mxl_VendorData_t* pRadVendorData = mxl_rad_getVendorData(pRad);
    ASSERT_NOT_NULL(pRadVendorData, , ME, "pRadVendorData is NULL");
    bool wmmStatsEnable = amxc_var_dyncast(bool, newParamValues);
    SAH_TRACEZ_NOTICE(ME, "%s: Setting wmmStatsEnable (%d) --> (%d)", pRad->Name, pRadVendorData->wmmStatsEnable, wmmStatsEnable);
    pRadVendorData->wmmStatsEnable = wmmStatsEnable;
    s_wmmCheckEnable(pRad, wmmStatsEnable);
    SAH_TRACEZ_OUT(ME);
}

SWLA_DM_HDLRS(sWmmStatsObjDmHdlrs,
              ARR(SWLA_DM_PARAM_HDLR("Enable", s_setWmmStatsEnable_pwf))
              );

void _whm_mxl_rad_setWmmStatsObj_ocf(const char* const sig_name,
                                        const amxc_var_t* const data,
                                        void* const priv) {
    swla_dm_procObjEvtOfLocalDm(&sWmmStatsObjDmHdlrs, sig_name, data, priv);
}
