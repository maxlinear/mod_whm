/******************************************************************************

         Copyright (c) 2023 - 2024, MaxLinear, Inc.

  This software may be distributed under the terms of the BSD license.
  See README for more details.

*******************************************************************************/
#ifndef __WHM_MXL_VAP_H__
#define __WHM_MXL_VAP_H__

#include "wld/wld.h"
#include "wld/wld_linuxIfUtils.h"

/* General Definitions Section */
typedef enum {
    MXL_MAP_OFF = 0,
    MXL_BACKHAUL_MAP = 1,
    MXL_FRONTHAUL_MAP,
    MXL_HYBRID_MAP,
    MXL_MAP_TYPE_MAX
} whm_mxl_multi_ap_type_e;

typedef struct {
    /* MxL Multi AP Type */
    whm_mxl_multi_ap_type_e mxlMultiApType;
    /* vendor_vht is used in 2.4GHz 11n mode */
    bool vendorVht;
    /* Vendor object */
    amxd_object_t* pVendorBus;

    /* OWE Transition SSID */
    char OWETransSSID[SSID_NAME_LEN];
    /* OWE Transition BSSID */
    char OWETransBSSID[ETHER_ADDR_STR_LEN];
    /* MLO ID */
    int32_t mloId;
} mxl_VapVendorData_t;

/* Macros Section */

/* Function Declarations Section */
int whm_mxl_vap_createHook(T_AccessPoint* pAP);
void whm_mxl_vap_destroyHook(T_AccessPoint* pAP);
mxl_VapVendorData_t* mxl_vap_getVapVendorData(const T_AccessPoint* pAP);
swl_rc_ne whm_mxl_vap_getSingleStationStats(T_AssociatedDevice* pAD);
int whm_mxl_vap_getStationStats(T_AccessPoint* pAP);

int whm_mxl_vap_updateApStats(T_AccessPoint* pAP);
int whm_mxl_vap_enable(T_AccessPoint* pAP, int enable, int set);
int whm_mxl_vap_ssid(T_AccessPoint* pAP, char* buf, int bufsize, int set);
int whm_mxl_vap_bssid(T_Radio* pR, T_AccessPoint* pAP, unsigned char* buf, int bufsize, int set);
int whm_mxl_vap_multiap_update_type(T_AccessPoint* pAP);
int whm_mxl_vap_clean_sta(T_AccessPoint* pAP, char* macStr, int macStrLen);
swl_rc_ne whm_mxl_vap_updated_neighbor(T_AccessPoint* pAP, T_ApNeighbour* pApNeighbor);
swl_rc_ne whm_mxl_vap_transfer_sta(T_AccessPoint* pAP, wld_transferStaArgs_t* params);
bool mxl_isApReadyToProcessVendorCmd(T_AccessPoint* pAP);
swl_rc_ne whm_mxl_vap_addWdsIfaceEvent(T_AccessPoint* pAP, wld_wds_intf_t* wdsIntf);
swl_rc_ne whm_mxl_vap_delWdsIfaceEvent(T_AccessPoint* pAP, wld_wds_intf_t* wdsIntf);

#endif /* __WHM_MXL_VAP_H__ */
