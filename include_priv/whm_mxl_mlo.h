/******************************************************************************

         Copyright (c) 2023 - 2025 MaxLinear, Inc.

  This software may be distributed under the terms of the BSD license.
  See README for more details.

*******************************************************************************/
#ifndef __WHM_MXL_MLO_H__
#define __WHM_MXL_MLO_H__

#include "wld/wld.h"
#include "wld/wld_linuxIfUtils.h"

// MxL MLO implementation limits the Max no. of Links in an MLD to 3.
// NOTE: Single Link MLD is not supported as well.
#define MAX_MLD_VAPS                15
#define MAX_MLD_LINKS               3
#define NO_MLO_ID                   (-1)
#define IGNORE_MLO_ID               (-2)

typedef enum {
    MLD_LINK_NONE = 0,
    MLD_LINK_CREATE,
    MLD_LINK_DELETE,
    MLD_LINK_MOVE,
    MLD_LINK_MAX
} whm_mxl_mld_link_action_e;

typedef enum {
    MLD_TYPE_NONE = 0,
    MLD_TYPE_SINGLE_LINK = 1,
    MLD_TYPE_DUAL_LINK = 2,
    MLD_TYPE_TRI_LINK = 3,
    MLD_TYPE_MAX
} whm_mxl_mld_type_e;

typedef struct {
    amxc_llist_t mlds;
    bool init;
} whm_mxl_mld_mngr_t;

typedef struct {
    amxc_llist_it_t it;
    int32_t mloId;
    swl_macBin_t apMldMac;
    bool externalMldMac;
    amxc_llist_t affiliatedLinks;
    whm_mxl_mld_type_e mldType;
} whm_mxl_mld_t;

typedef struct {
    amxc_llist_it_t it;
    int32_t mloId;
    /* IEEE 802.11be Link ID
     * Unique identifier of the affiliated AP within the MLD
     * Use this for NL or any other MLO specific operations based
     * on the link ID
     */
    uint8_t linkId;
    T_SSID* pLinkSSID;
    T_AccessPoint* pLinkAp;
    whm_mxl_mld_t* pMld;
    bool configured;
} whm_mxl_link_t;

whm_mxl_link_t* whm_mxl_mlo_getFirstLink(whm_mxl_mld_t* pMld);
whm_mxl_link_t* whm_mxl_mlo_getNextLink(whm_mxl_link_t* pLink);

whm_mxl_mld_t* whm_mxl_mlo_getFirstMld(whm_mxl_mld_mngr_t* pMldMngr);
whm_mxl_mld_t* whm_mxl_mlo_getNextMld(whm_mxl_mld_t* pMld);

#define whm_mxl_mlo_forEachLinkInMld(pLink, pMld) \
    for (pLink = whm_mxl_mlo_getFirstLink(pMld); pLink; pLink = whm_mxl_mlo_getNextLink(pLink))

#define whm_mxl_mlo_forEachMldVap(pMld, pMlds) \
    for (pMld = whm_mxl_mlo_getFirstMld(pMlds); pMld; pMld = whm_mxl_mlo_getNextMld(pMld))

bool whm_mxl_mlo_checkMloEnable(T_AccessPoint* pAP);
bool whm_mxl_mlo_isLinkUsable(whm_mxl_link_t* pLink);
bool whm_mxl_mlo_isPartOfMld(T_AccessPoint* pAP);
bool whm_mxl_mlo_checkMldConfigChange(T_AccessPoint* pAP);
amxd_object_t* whm_mxl_mlo_getMloObject(T_AccessPoint* pAP);
swl_rc_ne whm_mxl_mlo_configureMld(T_AccessPoint* pAP, int32_t newMloId);
swl_rc_ne whm_mxl_mlo_configureMldMac(T_AccessPoint* pAP, swl_macBin_t* pMldMac, bool forceUpdate);
swl_rc_ne whm_mxl_mlo_setMldUnit(T_AccessPoint* pAP);
swl_rc_ne whm_mxl_mlo_deleteLink(T_AccessPoint* pAP);
whm_mxl_mld_t* whm_mxl_mlo_getMldVap(T_AccessPoint* pAP);
whm_mxl_link_t* whm_mxl_mlo_getMldLink(T_AccessPoint* pAP);
whm_mxl_mld_mngr_t *whm_mxl_mlo_get_mldMngr(void);
swl_rc_ne whm_mxl_mlo_initMld(void);
swl_rc_ne whm_mxl_mlo_deinitMld(void);
#endif /* __WHM_MXL_MLO_H__ */
