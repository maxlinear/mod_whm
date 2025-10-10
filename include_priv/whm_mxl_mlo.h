/******************************************************************************

         Copyright (c) 2023 - 2025 MaxLinear, Inc.

  This software may be distributed under the terms of the BSD license.
  See README for more details.

*******************************************************************************/
#ifndef __WHM_MXL_MLO_H__
#define __WHM_MXL_MLO_H__

#include "wld/wld.h"
#include "wld/wld_linuxIfUtils.h"

// MxL MLO implementation limits the Max no. of Links in an MLD to 2.
// NOTE: Single Link MLD is not supported as well.
#define MAX_MLD_LINKS 2
#define NO_LINK_ID    (-1)

typedef struct {
    int32_t mloId;
    swl_macChar_t apMldMac;
    bool wdsSingleMlAssoc;
    bool wdsPrimaryLink;
} whm_mxl_mlo_link_t;

typedef enum {
    MLD_LINK_NONE = 0,
    MLD_LINK_CREATE,
    MLD_LINK_DELETE,
    MLD_LINK_MOVE,
    MLD_LINK_MAX
} whm_mxl_mld_link_action_e;

int32_t whm_mxl_mlo_getLinkCount(int32_t id);
T_AccessPoint* whm_mxl_mlo_getSiblingAP(T_AccessPoint* pAP, int32_t mloId);
bool whm_mxl_mlo_checkMloEnable(T_AccessPoint* pAP);
amxd_object_t* whm_mxl_mlo_getMloObject(T_AccessPoint* pAP);
swl_rc_ne whm_mxl_mlo_confVap(T_AccessPoint* pAP, int32_t currMloID, int32_t newMloID);
swl_rc_ne whm_mxl_mlo_setMldUnit(T_AccessPoint* pAP);

#endif /* __WHM_MXL_MLO_H__ */
