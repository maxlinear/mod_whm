/******************************************************************************

         Copyright (c) 2023 - 2025 MaxLinear, Inc.

  This software may be distributed under the terms of the BSD license.
  See README for more details.

*******************************************************************************/
#ifndef __WHM_MXL_MLO_H__
#define __WHM_MXL_MLO_H__

#include "wld/wld.h"
#include "wld/wld_linuxIfUtils.h"
#include "whm_mxl_utils.h"
#include "whm_mxl_nl80211.h"

// MxL MLO implementation limits the Max no. of Links in an MLD to 3.
// NOTE: Single Link MLD is not supported as well.
#define MAX_MLD_VAPS                15
#define MAX_MLD_LINKS               3
#define NO_MLO_ID                   (-1)
#define IGNORE_MLO_ID               (-2)
// Maximum number of STA MLDs supported by lower layers (OTF MLO System SW Spec)
// Each STA is assigned an SID per band. The STA MLD ID range is 0..127 (AIDs 64..191),
// giving a maximum of 128 STA MLDs.
#define MAX_STA_MLD_COUNT           128

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

/**
 * enum of MLD unusable reasons.
 * Each value is used as a bit index: bitmask = (1 << enumValue).
 * Use the M_MLD_UNUSABLE_* macros for bitmap operations.
 */
typedef enum {
    MLD_UNUSABLE_INVALID_TYPE,            // Invalid MLD type (e.g. single-link, unsupported by HW)
    MLD_UNUSABLE_INVALID_MLD_MAC,         // AP MLD MAC is null/unset
    MLD_UNUSABLE_LINK_DISABLED,           // One or more affiliated links disabled
    MLD_UNUSABLE_LINK_NOT_BE,             // One or more links not operating in 802.11be
    MLD_UNUSABLE_SSID_MISMATCH,           // SSIDs differ across affiliated links
    MLD_UNUSABLE_SHARED_SEC_MISMATCH,     // Security config incompatible across links
    MLD_UNUSABLE_INTERNAL_ERROR,          // Catch-all: NULL ptrs, invalid objects, misc errors
    MLD_UNUSABLE_MAX
} whm_mxl_mld_unusable_reason_e;

#define MLD_UNUSABLE_NONE                   (0)
#define M_MLD_UNUSABLE_INVALID_TYPE         (1 << MLD_UNUSABLE_INVALID_TYPE)
#define M_MLD_UNUSABLE_INVALID_MLD_MAC      (1 << MLD_UNUSABLE_INVALID_MLD_MAC)
#define M_MLD_UNUSABLE_LINK_DISABLED        (1 << MLD_UNUSABLE_LINK_DISABLED)
#define M_MLD_UNUSABLE_LINK_NOT_BE          (1 << MLD_UNUSABLE_LINK_NOT_BE)
#define M_MLD_UNUSABLE_SSID_MISMATCH        (1 << MLD_UNUSABLE_SSID_MISMATCH)
#define M_MLD_UNUSABLE_SHARED_SEC_MISMATCH  (1 << MLD_UNUSABLE_SHARED_SEC_MISMATCH)
#define M_MLD_UNUSABLE_INTERNAL_ERROR       (1 << MLD_UNUSABLE_INTERNAL_ERROR)

typedef uint32_t whm_mxl_mld_unusable_reason_m;
typedef struct whm_mxl_link whm_mxl_link_t;

typedef struct {
    amxp_timer_t* commitTimer;
    bool enable;
    uint32_t delay;
    uint32_t bootDelay;
} whm_mxl_mloCommitCfg_t;

typedef struct {
    amxc_llist_t mlds;
    bool init;
    whm_mxl_mloCommitCfg_t mloCommitMngr;
} whm_mxl_mld_mngr_t;

typedef struct {
    amxc_llist_it_t it;
    int32_t mloId;
    swl_macBin_t apMldMac;
    bool externalMldMac;
    amxc_llist_t affiliatedLinks;
    whm_mxl_link_t* mainLink;
    whm_mxl_mld_type_e mldType;
    uint8_t genericUpdateRetryCtr;          // Retry counter for deferred generic MLD update
    bool isUsable;                          // MLD Operational/Usable flag
    whm_mxl_mld_unusable_reason_m reasons;  // Bitmap of unusable reasons
} whm_mxl_mld_t;

struct whm_mxl_link {
    amxc_llist_it_t it;
    int32_t mloId;
    uint8_t linkId;
    T_SSID* pLinkSSID;
    T_AccessPoint* pLinkAp;
    whm_mxl_mld_t* pMld;
};

// TODO: Generalize and order this API based on utilization
whm_mxl_link_t* whm_mxl_mlo_getFirstLink(whm_mxl_mld_t* pMld);
whm_mxl_link_t* whm_mxl_mlo_getNextLink(whm_mxl_link_t* pLink);

whm_mxl_mld_t* whm_mxl_mlo_getFirstMld(whm_mxl_mld_mngr_t* pMldMngr);
whm_mxl_mld_t* whm_mxl_mlo_getNextMld(whm_mxl_mld_t* pMld);

#define whm_mxl_mlo_forEachLinkInMld(pLink, pMld) \
    for (pLink = whm_mxl_mlo_getFirstLink(pMld); pLink; pLink = whm_mxl_mlo_getNextLink(pLink))

#define whm_mxl_mlo_forEachMldVap(pMld, pMlds) \
    for (pMld = whm_mxl_mlo_getFirstMld(pMlds); pMld; pMld = whm_mxl_mlo_getNextMld(pMld))

whm_mxl_mld_unusable_reason_m whm_mxl_mlo_checkMldUsability(whm_mxl_mld_t* pMld);
bool whm_mxl_mlo_getMldStatus(whm_mxl_mld_t* pMld);
bool whm_mxl_mlo_updateMldStatus(whm_mxl_mld_t* pMld);
bool whm_mxl_mlo_checkMldConfigChange(T_AccessPoint* pAP);
void whm_mxl_mlo_requestMldAction(T_AccessPoint* pAP, bool forceAction);

amxd_object_t* whm_mxl_mlo_getMloObject(T_AccessPoint* pAP);
swl_rc_ne whm_mxl_mlo_configureMld(T_AccessPoint* pAP, int32_t newMloId);
swl_rc_ne whm_mxl_mlo_configureMldMac(T_AccessPoint* pAP, swl_macBin_t* pMldMac, bool forceUpdate);
swl_rc_ne whm_mxl_mlo_setMldUnit(T_AccessPoint* pAP);
swl_rc_ne whm_mxl_mlo_setEndpointMldUnit(T_EndPoint* pEP);
swl_rc_ne whm_mxl_mlo_deleteLink(T_AccessPoint* pAP);
whm_mxl_mld_t* whm_mxl_mlo_getMldVap(T_AccessPoint* pAP);
whm_mxl_link_t* whm_mxl_mlo_getMldLink(T_AccessPoint* pAP);
whm_mxl_mld_mngr_t *whm_mxl_mlo_get_mldMngr(void);
swl_rc_ne whm_mxl_mlo_initMld(void);
swl_rc_ne whm_mxl_mlo_deinitMld(void);
whm_mxl_nl80211_apMld_t* whm_mxl_mlo_getApMldInfo(whm_mxl_mld_t* pMld);
swl_rc_ne whm_mxl_mlo_copyNl80211ApMldToMld(whm_mxl_mld_t* pMld, whm_mxl_nl80211_apMld_t* pMldInfo);
swl_rc_ne whm_mxl_mlo_updateGenericMld(whm_mxl_mld_t* pMld);

#endif /* __WHM_MXL_MLO_H__ */
