/******************************************************************************

         Copyright (c) 2023 - 2025 MaxLinear, Inc.

  This software may be distributed under the terms of the BSD license.
  See README for more details.

*******************************************************************************/
#ifndef __WHM_MXL_WMM_H__
#define __WHM_MXL_WMM_H__

#define NUM_QUEUES_PER_AC 2
#define WHM_MXL_SAVE_WMM_QUEUES(qNum, markToQ) \
    { \
        qNum[WLD_AC_BE] = markToQ.acBe[0]; \
        qNum[WLD_AC_BK] = markToQ.acBk[0]; \
        qNum[WLD_AC_VI] = markToQ.acVi[0]; \
        qNum[WLD_AC_VO] = markToQ.acVo[0]; \
    }

/* Modify s_wmmQueuesStatusName enum to string also if adding status */
typedef enum {
    MXL_WMM_QUEUES_DISABLED = 0,
    MXL_WMM_QUEUES_ENABLED,
    MXL_WMM_QUEUES_MAX
} whm_mxl_wmm_status_e;

typedef enum {
    WHM_MXL_WMM_STAT_TYPE_PACKETS,
    WHM_MXL_WMM_STAT_TYPE_BYTES,
    WHM_MXL_WMM_STAT_TYPE_MAX
} whm_mxl_wmm_stat_type_e;

typedef struct {
    int acBe[NUM_QUEUES_PER_AC];
    int acBk[NUM_QUEUES_PER_AC];
    int acVi[NUM_QUEUES_PER_AC];
    int acVo[NUM_QUEUES_PER_AC];
} mxl_mark_to_queue_t;

swl_rc_ne whm_mxl_getApWmmStats(T_AccessPoint* pAP, T_Stats* pApStats);
swl_rc_ne whm_mxl_getRadWmmStats(T_Radio* pRad, T_Stats* stats);
swl_rc_ne whm_mxl_initWmmQueues(T_AccessPoint* pAP);
swl_rc_ne whm_mxl_deinitWmmQueues(T_AccessPoint* pAP);
bool whm_mxl_wmmUpdateStatus(T_AccessPoint* pAP, whm_mxl_wmm_status_e status);
whm_mxl_wmm_status_e whm_mxl_wmmGetStatus(T_AccessPoint* pAP);

#endif /* __WHM_MXL_WMM_H__ */
