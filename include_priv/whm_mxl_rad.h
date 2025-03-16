/******************************************************************************

         Copyright (c) 2023 - 2025 MaxLinear, Inc.

  This software may be distributed under the terms of the BSD license.
  See README for more details.

*******************************************************************************/
#ifndef __WHM_MXL_RAD_H__
#define __WHM_MXL_RAD_H__

#include "wld/wld.h"
#include "whm_mxl_monitor.h"
#include "whm_mxl_zwdfs.h"

/* General Definitions Section */
#define CCA_TH_SIZE 5

/* Macros Section */

/* Struct Definition Section */
typedef struct {
    /**
     * Data for naStation monitor
     */
    mxl_nastaData_t naSta;

    /* vendor object */
    amxd_object_t* pBus;

    /* BSS Color */
    int randomColor;
    bool bssColorRandomize;

     /* CCA Threhshold */
    int ccaTh[CCA_TH_SIZE];

    /* Indicates whether IEEE 802.11be functionality is enabled */
    bool isIEEE80211BeEnabled;

    /* First non DFS Channel */
    bool firstNonDfs;

    /* timer for delelete vap */
    amxp_timer_t* delVapTimer;

    /* delVapTimer retries */
    int delVapTimerRetries;

    /* ACS FallBack params */
    int AcsFbPrimChan;
    int AcsFbSecChan;
    int AcsFbBw;
} mxl_VendorData_t;

typedef struct {
    uint32_t wiphyId;       // wiphy id
    bool wiphyDfsAntenna;   // flag set when the device is meant for DFS only
} mxl_VendorWiphyInfo_t;

typedef struct {
    uint32_t passive;
    uint32_t active;
    uint32_t num_probe_reqs;
    uint32_t probe_reqs_interval;
    uint32_t num_chans_in_chunk;
    uint32_t break_time;
    uint32_t break_time_busy;
    uint32_t window_slice;
    uint32_t window_slice_overlap;
    uint32_t cts_to_self_duration;
} mxl_bgScanParams_t;

/* Function Declarations Section */
mxl_VendorData_t* mxl_rad_getVendorData(const T_Radio* pRad);

int whm_mxl_rad_supports(T_Radio* pRad, char* buf _UNUSED, int bufsize _UNUSED);
int whm_mxl_rad_createHook(T_Radio* pRad);
void whm_mxl_rad_destroyHook(T_Radio* pRad);
int whm_mxl_rad_enable(T_Radio* pRad, int val, int set);

int whm_mxl_rad_addVapExt(T_Radio* pRad, T_AccessPoint* pAP);
int whm_mxl_rad_delVapIf(T_Radio* pRad, char* vapName);
int whm_mxl_rad_addEndpointIf(T_Radio* pRad, char* buf, int bufsize);
void whm_mxl_rad_delVap_timer_init(T_Radio* pRad);
void whm_mxl_rad_delVap_timer_deinit(T_Radio* pRad);
void whm_mxl_rad_delVap_timer_enable(T_Radio* pRad, bool enable);

int whm_mxl_rad_stats(T_Radio* pRad);
int whm_mxl_rad_antennaCtrl(T_Radio* pRad, int val, int set);
int whm_mxl_rad_beamforming(T_Radio* rad, beamforming_type_t type, int val, int set);
swl_rc_ne whm_mxl_rad_startScan(T_Radio* pRadio);
swl_rc_ne whm_mxl_rad_supvendModesChanged(T_Radio* pRad, T_AccessPoint* pAP, amxd_object_t* object, amxc_var_t* params);
swl_rc_ne whm_mxl_rad_regDomain(T_Radio* pRad, char* val, int bufsize, int set);
int whm_mxl_rad_autoChannelEnable(T_Radio* pRad, int enable, int set);
swl_rc_ne whm_mxl_rad_setChanspec(T_Radio* pRad, bool direct);
int8_t *whm_mxl_rad_txPercentToPower(uint8_t percent);
uint8_t *whm_mxl_rad_txPowerToPercent(int8_t power);
int whm_mxl_rad_status(T_Radio* pRad);
swl_rc_ne whm_mxl_rad_supstd(T_Radio* pRad, swl_radioStandard_m radioStandards);
swl_rc_ne whm_mxl_getBgScanParams(T_Radio* pRad, mxl_bgScanParams_t *pBgScanParams);
swl_rc_ne whm_mxl_setBgScanParams(T_Radio* pRad, mxl_bgScanParams_t *pBgScanParams);

#endif /* __WHM_MXL_RAD_H__ */
