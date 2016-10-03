//--------------------------------------------------------------------------
// Copyright (C) 2014-2016 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2005-2013 Sourcefire, Inc.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

// service_base.cc author Ron Dempster <Ron.Dempster@sourcefire.com>

#include "service_base.h"

#include <limits.h>

#include "service_api.h"
#include "service_battle_field.h"
#include "service_bgp.h"
#include "service_bootp.h"
#include "service_dcerpc.h"
#include "service_direct_connect.h"
#include "service_flap.h"
#include "service_ftp.h"
#include "service_irc.h"
#include "service_lpr.h"
#include "service_mdns.h"
#include "service_mysql.h"
#include "service_netbios.h"
#include "service_nntp.h"
#include "service_ntp.h"
#include "service_radius.h"
#include "service_rexec.h"
#include "service_rfb.h"
#include "service_rlogin.h"
#include "service_rpc.h"
#include "service_rshell.h"
#include "service_rsync.h"
#include "service_rtmp.h"
#include "service_smtp.h"
#include "service_snmp.h"
#include "service_ssh.h"
#include "service_ssl.h"
#include "service_telnet.h"
#include "service_tftp.h"
#include "appid_session.h"
#include "appid_config.h"
#include "fw_appid.h"
#include "lua_detector_api.h"
#include "lua_detector_module.h"
#include "appid_utils/ip_funcs.h"
#include "detector_plugins/detector_dns.h"
#include "detector_plugins/detector_pattern.h"
#include "detector_plugins/detector_sip.h"
#include "log/messages.h"
#include "main/snort_debug.h"
#include "search_engines/search_tool.h"
#include "utils/util.h"
#include "sfip/sf_ip.h"

/*#define SERVICE_DEBUG 1
  #define SERVICE_DEBUG_PORT  0 */

#define BUFSIZE         512

#define STATE_ID_INCONCLUSIVE_SERVICE_WEIGHT 3
#define STATE_ID_INVALID_CLIENT_THRESHOLD    9
#define STATE_ID_MAX_VALID_COUNT             5
#define STATE_ID_NEEDED_DUPE_DETRACT_COUNT   3

/* If this is greater than 1, more than 1 service detector can be searched for
 * and tried per flow based on port/pattern (if a valid detector doesn't
 * already exist). */
#define MAX_CANDIDATE_SERVICES 10

static void* service_flowdata_get(AppIdSession* flow, unsigned service_id);
static int service_flowdata_add(AppIdSession* flow, void* data, unsigned service_id, AppIdFreeFCN
    fcn);
static void AppIdAddHostInfo(AppIdSession* flow, SERVICE_HOST_INFO_CODE code, const void* info);
static int AppIdAddDHCP(AppIdSession* flowp, unsigned op55_len, const uint8_t* op55, unsigned
    op60_len, const uint8_t* op60, const uint8_t* mac);
static void AppIdAddHostIP(AppIdSession* flow, const uint8_t* mac, uint32_t ip4,
    int32_t zone, uint32_t subnetmask, uint32_t leaseSecs, uint32_t router);
static void AppIdAddSMBData(AppIdSession* flow, unsigned major, unsigned minor, uint32_t flags);
static void AppIdServiceAddMisc(AppIdSession* flow, AppId miscId);

struct ServiceMatch
{
    struct ServiceMatch* next;
    unsigned count;
    unsigned size;
    RNAServiceElement* svc;
};

static const uint8_t zeromac[6] = { 0, 0, 0, 0, 0, 0 };
static unsigned smOrderedListSize = 32;
static THREAD_LOCAL DHCPInfo* dhcp_info_free_list;
static THREAD_LOCAL FpSMBData* smb_data_free_list;
static THREAD_LOCAL ServiceMatch** smOrderedList = nullptr;
static THREAD_LOCAL ServiceMatch* free_service_match;
static THREAD_LOCAL ServiceConfig* serviceConfig = nullptr;
static THREAD_LOCAL RNAServiceElement* ftp_service = nullptr;
static THREAD_LOCAL ServicePatternData* free_pattern_data = nullptr;

const ServiceApi serviceapi =
{
    &service_flowdata_get,
    &service_flowdata_add,
    &AppIdAddDHCP,
    &AppIdAddHostIP,
    &AppIdAddSMBData,
    &AppIdServiceAddService,
    &AppIdServiceFailService,
    &AppIdServiceInProcess,
    &AppIdServiceIncompatibleData,
    &AppIdAddHostInfo,
    &AppIdAddPayload,
    &AppIdAddUser,
    &AppIdServiceAddServiceSubtype,
    &AppIdServiceAddMisc,
    &AppIdAddDnsQueryInfo,
    &AppIdAddDnsResponseInfo,
    &AppIdResetDnsInfo,
};

#ifdef SERVICE_DEBUG
static const char* serviceIdStateName[] =
{
    "NEW",
    "VALID",
    "PORT",
    "PATTERN",
    "BRUTE_FORCE"
};
#endif

/*C service API */
static void ServiceRegisterPattern(RNAServiceValidationFCN, IpProtocol, const uint8_t*, unsigned,
        int, struct Detector*, int, const char* );
static void CServiceRegisterPattern(RNAServiceValidationFCN, IpProtocol, const uint8_t* ,
        unsigned, int , const char*);
static void ServiceRegisterPatternUser(RNAServiceValidationFCN, IpProtocol, const uint8_t*,
        unsigned, int, const char*);
void appSetServiceValidator( RNAServiceValidationFCN, AppId, unsigned extractsInfo);
static int CServiceAddPort(const RNAServiceValidationPort*, RNAServiceValidationModule*);
static void CServiceRemovePorts(RNAServiceValidationFCN validate);

static IniServiceAPI svc_init_api =
{
    &CServiceRegisterPattern,
    &CServiceAddPort,
    &CServiceRemovePorts,
    &ServiceRegisterPatternUser,
    &appSetServiceValidator,
    0,
    0,
    nullptr
};

extern RNAServiceValidationModule timbuktu_service_mod;
extern RNAServiceValidationModule bit_service_mod;
extern RNAServiceValidationModule tns_service_mod;
extern RNAServiceValidationModule http_service_mod;

static RNAServiceValidationModule* static_service_list[] =
{
    &bgp_service_mod,
    &bootp_service_mod,
    &dcerpc_service_mod,
    &dns_service_mod,
    &flap_service_mod,
    &ftp_service_mod,
    &irc_service_mod,
    &lpr_service_mod,
    &mysql_service_mod,
    &netbios_service_mod,
    &nntp_service_mod,
    &ntp_service_mod,
    &radius_service_mod,
    &rexec_service_mod,
    &rfb_service_mod,
    &rlogin_service_mod,
    &rpc_service_mod,
    &rshell_service_mod,
    &rsync_service_mod,
    &rtmp_service_mod,
    &smtp_service_mod,
    &snmp_service_mod,
    &ssh_service_mod,
    &ssl_service_mod,
    &telnet_service_mod,
    &tftp_service_mod,
    &sip_service_mod,
    &directconnect_service_mod,
    &battlefield_service_mod,
    &mdns_service_mod,
    &timbuktu_service_mod,
    &tns_service_mod,
    &bit_service_mod,
    &pattern_service_mod,
    &http_service_mod
};

const uint32_t NUM_STATIC_SERVICES =
        sizeof(static_service_list) / sizeof(RNAServiceValidationModule*);

void appSetServiceValidator(RNAServiceValidationFCN fcn, AppId appId, unsigned extractsInfo)
{
    AppInfoTableEntry* pEntry = appInfoEntryGet(appId);
    if (!pEntry)
    {
        ErrorMessage("AppId: invalid direct service AppId, %d", appId);
        return;
    }
    extractsInfo &= (APPINFO_FLAG_SERVICE_ADDITIONAL | APPINFO_FLAG_SERVICE_UDP_REVERSED);
    if (!extractsInfo)
    {
        DebugFormat(DEBUG_APPID, "Ignoring direct service without info for AppId %d", appId);
        return;
    }
    pEntry->svrValidator = ServiceGetServiceElement(fcn, nullptr);
    if (pEntry->svrValidator)
        pEntry->flags |= extractsInfo;
    else
        ErrorMessage("AppId: failed to find a service element for AppId %d", appId);
}

/**free ServiceMatch List.
 */
void AppIdFreeServiceMatchList(ServiceMatch* sm)
{
    ServiceMatch* tmpSm;

    if (!sm)
        return;

    for (tmpSm = sm; tmpSm->next; tmpSm = tmpSm->next)
        ;
    tmpSm->next = free_service_match;
    free_service_match = sm;
}

int AddFTPServiceState(AppIdSession* fp)
{
    if (!ftp_service)
        return -1;
    return fp->add_flow_data_id(21, ftp_service);
}

/**allocate one ServiceMatch element.
 */
static inline ServiceMatch* allocServiceMatch(void)
{
    ServiceMatch* sm;

    if ((sm = free_service_match))
    {
        free_service_match = sm->next;
        memset(sm, 0, sizeof(*sm));
        return sm;
    }
    return (ServiceMatch*)snort_calloc(sizeof(ServiceMatch));
}

static int pattern_match(void* id, void*, int index, void* data, void*)
{
    ServiceMatch** matches = (ServiceMatch**)data;
    ServicePatternData* pd = (ServicePatternData*)id;
    ServiceMatch* sm;

    if (pd->position >= 0 && pd->position != index)
        return 0;

    for (sm=*matches; sm; sm=sm->next)
        if (sm->svc == pd->svc)
            break;
    if (sm)
        sm->count++;
    else
    {
        sm=allocServiceMatch();
        sm->count++;
        sm->svc = pd->svc;
        sm->size = pd->size;
        sm->next = *matches;
        *matches = sm;
    }
    return 0;
}

AppId getPortServiceId(IpProtocol proto, uint16_t port, const AppIdConfig* pConfig)
{
    AppId appId;

    if (proto == IpProtocol::TCP)
        appId = pConfig->tcp_port_only[port];
    else if (proto == IpProtocol::UDP)
        appId = pConfig->udp_port_only[port];
    else
        appId = pConfig->ip_protocol[(uint16_t)proto];

    checkSandboxDetection(appId);

    return appId;
}

static inline uint16_t sslPortRemap(
    uint16_t port
    )
{
    switch (port)
    {
    case 465:
        return 25;
    case 563:
        return 119;
    case 585:
    case 993:
        return 143;
    case 990:
        return 21;
    case 992:
        return 23;
    case 994:
        return 6667;
    case 995:
        return 110;
    default:
        return 0;
    }
}

static inline RNAServiceElement* AppIdGetNexServiceByPort( IpProtocol protocol, uint16_t port,
    const RNAServiceElement* const lasService, AppIdSession* rnaData)
{
    RNAServiceElement* service = nullptr;
    SF_LIST* list = nullptr;

    if (AppIdServiceDetectionLevel(rnaData) == 1)
    {
        unsigned remappedPort = sslPortRemap(port);
        if (remappedPort)
            list = serviceConfig->tcp_services[remappedPort];
    }
    else if (protocol == IpProtocol::TCP)
    {
        list = serviceConfig->tcp_services[port];
    }
    else
    {
        list = serviceConfig->udp_services[port];
    }

    if (list)
    {
        SF_LNODE* iter = nullptr;

        service = (RNAServiceElement*)sflist_first(list, &iter);
        if (lasService)
        {
            while ( service && ((service->validate != lasService->validate) ||
                (service->userdata != lasService->userdata)))
                service = (RNAServiceElement*)sflist_next(&iter);
            if (service)
                service = (RNAServiceElement*)sflist_next(&iter);
        }
    }

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (port == SERVICE_DEBUG_PORT)
#endif
    fprintf(SF_DEBUG_FILE, "Port service for protocol %u port %u, service %s\n",
        (unsigned)protocol, (unsigned)port, (service && service->name) ? service->name :
        "UNKNOWN");
#endif

    return service;
}

static inline RNAServiceElement* AppIdNexServiceByPattern(AppIdServiceIDState* id_state
#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    , uint16_t port
#endif
#endif
    )
{
    RNAServiceElement* service = nullptr;

    while (id_state->currenService)
    {
        id_state->currenService = id_state->currenService->next;
        if (id_state->currenService && id_state->currenService->svc->current_ref_count)
        {
            service = id_state->currenService->svc;
            break;
        }
    }

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (port == SERVICE_DEBUG_PORT)
#endif
    fprintf(SF_DEBUG_FILE, "Next pattern service %s\n",
        (service && service->name) ? service->name : "UNKNOWN");
#endif

    return service;
}

const RNAServiceElement* ServiceGetServiceElement(RNAServiceValidationFCN fcn, Detector* userdata)
{
    RNAServiceElement* li;

    for (li=serviceConfig->tcp_service_list; li; li=li->next)
    {
        if ((li->validate == fcn) && (li->userdata == userdata))
            return li;
    }

    for (li=serviceConfig->udp_service_list; li; li=li->next)
    {
        if ((li->validate == fcn) && (li->userdata == userdata))
            return li;
    }
    return nullptr;
}

static void ServiceRegisterPattern(RNAServiceValidationFCN fcn, IpProtocol proto,
        const uint8_t* pattern, unsigned size, int position, struct Detector* userdata,
        int provides_user, const char* name)
{
    SearchTool** patterns;
    ServicePatternData** pd_list;
    int* count;
    ServicePatternData* pd;
    RNAServiceElement** list;
    RNAServiceElement* li;

    if ((IpProtocol)proto == IpProtocol::TCP)
    {
        patterns = &serviceConfig->tcp_patterns;
        pd_list = &serviceConfig->tcp_pattern_data;

        count = &serviceConfig->tcp_pattern_count;
        list = &serviceConfig->tcp_service_list;
    }
    else if ((IpProtocol)proto == IpProtocol::UDP)
    {
        patterns = &serviceConfig->udp_patterns;
        pd_list = &serviceConfig->udp_pattern_data;

        count = &serviceConfig->udp_pattern_count;
        list = &serviceConfig->udp_service_list;
    }
    else
    {
        ErrorMessage("Invalid protocol when registering a pattern: %u\n",(unsigned)proto);
        return;
    }

    for (li=*list; li; li=li->next)
    {
        if ((li->validate == fcn) && (li->userdata == userdata))
            break;
    }
    if (!li)
    {
        li = new RNAServiceElement;
        li->next = *list;
        *list = li;
        li->validate = fcn;
        li->userdata = userdata;
        li->detectorType = UINT_MAX;
        li->provides_user = provides_user;
        li->name = name;
    }

    if (!(*patterns))
    {
        *patterns = new SearchTool("ac_full");
        if (!(*patterns))
        {
            ErrorMessage("Error initializing the pattern table for protocol %u\n",(unsigned)proto);
            return;
        }
    }

    if (free_pattern_data)
    {
        pd = free_pattern_data;
        free_pattern_data = pd->next;
        memset(pd, 0, sizeof(*pd));
    }
    else
        pd = (ServicePatternData*)snort_calloc(sizeof(ServicePatternData));

    pd->svc = li;
    pd->size = size;
    pd->position = position;
    (*patterns)->add(pattern, size, pd, false);
    (*count)++;
    pd->next = *pd_list;
    *pd_list = pd;
    li->ref_count++;
}

void ServiceRegisterPatternDetector(RNAServiceValidationFCN fcn,
    IpProtocol proto, const uint8_t* pattern, unsigned size,
    int position, struct Detector* userdata, const char* name)
{
    ServiceRegisterPattern(fcn, proto, pattern, size, position, userdata, 0, name);
}

static void ServiceRegisterPatternUser(RNAServiceValidationFCN fcn, IpProtocol proto,
    const uint8_t* pattern, unsigned size, int position, const char* name)
{
    ServiceRegisterPattern(fcn, proto, pattern, size, position, nullptr, 1, name);
}

static void CServiceRegisterPattern(RNAServiceValidationFCN fcn, IpProtocol proto,
    const uint8_t* pattern, unsigned size, int position, const char* name)
{
    ServiceRegisterPattern(fcn, proto, pattern, size, position, nullptr, 0, name);
}

static void RemoveServicePortsByType(RNAServiceValidationFCN validate, SF_LIST** services,
    RNAServiceElement* list, struct Detector* userdata)
{
    RNAServiceElement* li;
    unsigned i;

    for (li=list; li; li=li->next)
    {
        if (li->validate == validate && li->userdata == userdata)
            break;
    }
    if (li == nullptr)
        return;

    for (i=0; i < RNA_SERVICE_MAX_PORT; i++)
    {
        SF_LIST* listTmp;

        if ( ( listTmp = services[i] ) )
        {
            SF_LNODE* iter;
            RNAServiceElement* liTmp;

            liTmp = (RNAServiceElement*)sflist_first(listTmp, &iter);
            while (liTmp)
            {
                if (liTmp == li)
                {
                    li->ref_count--;
                    sflist_remove_node(listTmp, iter);
                    // FIXIT-M: Revisit this for better solution to calling sflist_first after
                    // deleting a node... ultimate solution for use of sflist would be move
                    // to STL
                    liTmp = (RNAServiceElement*)sflist_first(listTmp, &iter);
                }
                else
                    liTmp = (RNAServiceElement*)sflist_next(&iter);
            }
        }
    }
}

/**
 * \brief Remove all ports registered for all services
 *
 * This function takes care of removing ports for all services including C service modules,
 * Lua detector modules and services associated with C detector modules.
 *
 * @return void
 */
static void RemoveAllServicePorts()
{
    int i;

    for ( i= 0; i < RNA_SERVICE_MAX_PORT; i++)
    {
        if (serviceConfig->tcp_services[i])
        {
            sflist_free(serviceConfig->tcp_services[i]);
            serviceConfig->tcp_services[i] = nullptr;
        }
    }
    for (i = 0; i < RNA_SERVICE_MAX_PORT; i++)
    {
        if (serviceConfig->udp_services[i])
        {
            sflist_free(serviceConfig->udp_services[i]);
            serviceConfig->udp_services[i] = nullptr;
        }
    }
    for (i = 0; i < RNA_SERVICE_MAX_PORT; i++)
    {
        if (serviceConfig->udp_reversed_services[i])
        {
            sflist_free(serviceConfig->udp_reversed_services[i]);
            serviceConfig->udp_reversed_services[i] = nullptr;
        }
    }
}

void ServiceRemovePorts(RNAServiceValidationFCN validate, struct Detector* userdata)
{
    RemoveServicePortsByType(validate, serviceConfig->tcp_services,
        serviceConfig->tcp_service_list, userdata);
    RemoveServicePortsByType(validate, serviceConfig->udp_services,
        serviceConfig->udp_service_list, userdata);
    RemoveServicePortsByType(validate, serviceConfig->udp_reversed_services,
        serviceConfig->udp_reversed_service_list, userdata);
}

static void CServiceRemovePorts(RNAServiceValidationFCN validate)
{
    ServiceRemovePorts(validate, nullptr);
}

int ServiceAddPort(const RNAServiceValidationPort* pp, RNAServiceValidationModule* svm,
    struct Detector* userdata)
{
    SF_LIST** services;
    RNAServiceElement** list = nullptr;
    RNAServiceElement* li;
    RNAServiceElement* serviceElement;

    DebugFormat(DEBUG_INSPECTOR, "Adding service %s for protocol %u on port %u\n",
        svm->name, (unsigned)pp->proto, (unsigned)pp->port);
    if (pp->proto == IpProtocol::TCP)
    {
        services = serviceConfig->tcp_services;
        list = &serviceConfig->tcp_service_list;
    }
    else if (pp->proto == IpProtocol::UDP)
    {
        if (!pp->reversed_validation)
        {
            services = serviceConfig->udp_services;
            list = &serviceConfig->udp_service_list;
        }
        else
        {
            services = serviceConfig->udp_reversed_services;
            list = &serviceConfig->udp_reversed_service_list;
        }
    }
    else
    {
        ErrorMessage("Service %s did not have a valid protocol (%u)\n",
            svm->name, (unsigned)pp->proto);
        return 0;
    }

    for (li=*list; li; li=li->next)
    {
        if (li->validate == pp->validate && li->userdata == userdata)
            break;
    }
    if (!li)
    {
        li = new RNAServiceElement;
        li->next = *list;
        *list = li;
        li->validate = pp->validate;
        li->provides_user = svm->provides_user;
        li->userdata = userdata;
        li->detectorType = UINT_MAX;
        li->name = svm->name;
    }

    if (pp->proto == IpProtocol::TCP && pp->port == 21 && !ftp_service)
    {
        ftp_service = li;
        li->ref_count++;
    }

    /*allocate a new list if this is first detector for this port. */
    if (!services[pp->port])
    {
        services[pp->port] = (SF_LIST*)snort_alloc(sizeof(SF_LIST));
        sflist_init(services[pp->port]);
    }

    /*search and add if not present. */
    SF_LNODE* iter = nullptr;
    for (serviceElement = (RNAServiceElement*)sflist_first(services[pp->port], &iter);
        serviceElement && (serviceElement != li);
        serviceElement = (RNAServiceElement*)sflist_next(&iter))
        ;

    if (!serviceElement)
        sflist_add_tail(services[pp->port], li);

    li->ref_count++;
    return 0;
}

static int CServiceAddPort(const RNAServiceValidationPort* pp, RNAServiceValidationModule* svm)
{
    return ServiceAddPort(pp, svm, nullptr);
}

void add_service_to_active_list(RNAServiceValidationModule* service)
{
    service->next = serviceConfig->active_service_list;
    serviceConfig->active_service_list = service;
}

static int serviceLoadForConfigCallback(void* symbol)
{
    static unsigned service_module_index = 0;
    RNAServiceValidationModule* svm = (RNAServiceValidationModule*)symbol;
    const RNAServiceValidationPort* pp;

    if (service_module_index >= 65536)
    {
        ErrorMessage("Maximum number of service modules exceeded");
        return -1;
    }

    svm->api = &serviceapi;
    for (pp = svm->pp; pp && pp->validate; pp++)
        if (CServiceAddPort(pp, svm))
            return -1;

    if (svm->init(&svc_init_api))
        ErrorMessage("Error initializing service %s\n",svm->name);

    svm->next = serviceConfig->active_service_list;
    serviceConfig->active_service_list = svm;

    svm->flow_data_index = service_module_index | APPID_SESSION_DATA_SERVICE_MODSTATE_BIT;
    service_module_index++;

    return 0;
}

int serviceLoadCallback(void* symbol)
{
    return serviceLoadForConfigCallback(symbol);
}

static int load_service_detectors()
{
    svc_init_api.instance_id = pAppidActiveConfig->mod_config->instance_id;
    svc_init_api.debug = pAppidActiveConfig->mod_config->debug;
    svc_init_api.pAppidConfig = pAppidActiveConfig;

    for ( unsigned i = 0; i < NUM_STATIC_SERVICES; i++)
    {
        if (serviceLoadForConfigCallback(static_service_list[i]))
            return -1;
    }

    return 0;
}

void init_service_plugins()
{
    serviceConfig = new ServiceConfig;

    if ( load_service_detectors() )
        exit(-1);
}

void finalize_service_patterns()
{
    ServicePatternData* curr;
    ServicePatternData* lists[] = { serviceConfig->tcp_pattern_data,
                                    serviceConfig->udp_pattern_data };
    for ( unsigned i = 0; i < (sizeof(lists) / sizeof(*lists)); i++)
    {
        curr = lists[i];
        while (curr != nullptr)
        {
            if (curr->svc != nullptr)
            {
                bool isActive = true;
                if (curr->svc->userdata && !curr->svc->userdata->isActive)
                {
                    /* C detectors don't have userdata here, but they're always
                     * active.  So, this check is really just for Lua
                     * detectors. */
                    isActive = false;
                }
                if (isActive)
                {
                    curr->svc->current_ref_count = curr->svc->ref_count;
                }
            }
            curr = curr->next;
        }
    }

    if (serviceConfig->tcp_patterns)
        serviceConfig->tcp_patterns->prep();
    if (serviceConfig->udp_patterns)
        serviceConfig->udp_patterns->prep();
}

void clean_service_plugins()
{
    ServicePatternData* pattern;
    RNAServiceElement* se;
    ServiceMatch* sm;
    RNAServiceValidationModule* svm;
    FpSMBData* sd;
    DHCPInfo* info;

    if (serviceConfig->tcp_patterns)
    {
        delete serviceConfig->tcp_patterns;
        serviceConfig->tcp_patterns = nullptr;
    }

    if (serviceConfig->udp_patterns)
    {
        delete serviceConfig->udp_patterns;
        serviceConfig->udp_patterns = nullptr;
    }

    while ((pattern = serviceConfig->tcp_pattern_data))
    {
        serviceConfig->tcp_pattern_data = pattern->next;
        snort_free(pattern);
    }
    while ((pattern = serviceConfig->udp_pattern_data))
    {
        serviceConfig->udp_pattern_data = pattern->next;
        snort_free(pattern);
    }

    while ((pattern = free_pattern_data))
    {
        free_pattern_data = pattern->next;
        snort_free(pattern);
    }

    while ((se = serviceConfig->tcp_service_list))
    {
        serviceConfig->tcp_service_list = se->next;
        delete se;
    }

    while ((se = serviceConfig->udp_service_list))
    {
        serviceConfig->udp_service_list = se->next;
        delete se;
    }

    while ((se = serviceConfig->udp_reversed_service_list))
    {
        serviceConfig->udp_reversed_service_list = se->next;
        delete se;
    }

    while ((sd = smb_data_free_list))
    {
        smb_data_free_list = sd->next;
        snort_free(sd);
    }

    while ((info = dhcp_info_free_list))
    {
        dhcp_info_free_list = info->next;
        snort_free(info);
    }

    while ((sm = free_service_match))
    {
        free_service_match = sm->next;
        snort_free(sm);
    }

    if (smOrderedList)
    {
        // FIXIT-M: still allocated with calloc/realloc - vector coming soon...
        free(smOrderedList);
        smOrderedListSize = 32;
    }

    RemoveAllServicePorts();

    for (svm = serviceConfig->active_service_list; svm; svm = svm->next)
    {
        if (svm->clean)
            svm->clean();
    }

    clean_service_port_patterns();

    delete serviceConfig;
}

static int AppIdPatternPrecedence(const void* a, const void* b)
{
    const ServiceMatch* sm1 = (ServiceMatch*)a;
    const ServiceMatch* sm2 = (ServiceMatch*)b;

    /*higher precedence should be before lower precedence */
    if (sm1->count != sm2->count)
        return (sm2->count - sm1->count);
    else
        return (sm2->size - sm1->size);
}

/**Perform pattern match of a packet and construct a list of services sorted in order of
 * precedence criteria. Criteria is count and then size. The first service in the list is
 * returned. The list itself is saved in AppIdServiceIDState. If
 * appId is already identified, then use it instead of searching again. RNA will capability
 * to try out other inferior matches. If appId is unknown i.e. searched and not found by FRE then
 * dont do any pattern match. This is a way degrades RNA detector selection if FRE is running on
 * this sensor.
*/
static inline RNAServiceElement* AppIdGetServiceByPattern(const Packet* pkt, IpProtocol proto,
    const int, AppIdServiceIDState* id_state)
{
    SearchTool* patterns = nullptr;
    ServiceMatch* match_list;
    ServiceMatch* sm;
    uint32_t count;
    uint32_t i;
    RNAServiceElement* service = nullptr;

    if (proto == IpProtocol::TCP)
        patterns = serviceConfig->tcp_patterns;
    else
        patterns = serviceConfig->udp_patterns;

    if (!patterns)
    {
#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
        if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
        fprintf(SF_DEBUG_FILE, "Pattern bailing due to no patterns\n");
#endif
        return nullptr;
    }

    if (!smOrderedList)
    {
        // FIXIT-M: - using calloc because this may be realloc'ed later, change to vector asap
        smOrderedList = (ServiceMatch**)calloc(smOrderedListSize, sizeof(ServiceMatch*));
        if (!smOrderedList)
        {
            ErrorMessage("Pattern bailing due to failed allocation");
            return nullptr;
        }
    }

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
    {
#endif
    fprintf(SF_DEBUG_FILE, "Matching\n");
    DumpHex(SF_DEBUG_FILE, pkt->data, pkt->dsize);
#if SERVICE_DEBUG_PORT
}

#endif
#endif
    /*FRE didn't search */
    match_list = nullptr;
    patterns->find_all((char*)pkt->data, pkt->dsize, &pattern_match, false, (void*)&match_list);

    count = 0;
    for (sm = match_list; sm; sm = sm->next)
    {
        if (count >= smOrderedListSize)
        {
            ServiceMatch** tmp;
            smOrderedListSize *= 2;
            assert(smOrderedListSize > 0);
            tmp = (ServiceMatch**)realloc(smOrderedList,
                    smOrderedListSize * sizeof(*smOrderedList));
            if (!tmp)
            {
                ErrorMessage("Realloc failure %u\n",smOrderedListSize);
                smOrderedListSize /= 2;

                /*free the remaining elements. */
                AppIdFreeServiceMatchList(sm);

                break;
            }
            ErrorMessage("Realloc %u\n",smOrderedListSize);

            smOrderedList = tmp;
        }

        smOrderedList[count++] = sm;
    }

    if (!count)
        return nullptr;

    qsort(smOrderedList, count, sizeof(*smOrderedList), AppIdPatternPrecedence);

    /*rearrange the matchlist now */
    for (i = 0; i < (count-1); i++)
        smOrderedList[i]->next = smOrderedList[i+1];
    smOrderedList[i]->next = nullptr;

    service = smOrderedList[0]->svc;

    if (id_state)
    {
        id_state->svc = service;
        if (id_state->serviceList != nullptr)
        {
            AppIdFreeServiceMatchList(id_state->serviceList);
        }
        id_state->serviceList = smOrderedList[0];
        id_state->currenService = smOrderedList[0];
    }
    else
        AppIdFreeServiceMatchList(smOrderedList[0]);

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
    fprintf(SF_DEBUG_FILE, "Pattern service for protocol %u (%u->%u), %s\n",
        (unsigned)proto, (unsigned)pkt->ptrs.sp, (unsigned)pkt->ptrs.dp,
        (service && service->name) ? service->name.c_str() : "UNKNOWN");
#endif
    return service;
}

static inline RNAServiceElement* AppIdGetServiceByBruteForce(IpProtocol protocol,
    const RNAServiceElement* lasService)
{
    RNAServiceElement* service;

    if (lasService)
        service = lasService->next;
    else
        service = ((protocol == IpProtocol::TCP) ? serviceConfig->tcp_service_list :
            serviceConfig->udp_service_list);

    while (service && !service->current_ref_count)
        service = service->next;

    return service;
}

static void AppIdAddHostInfo(AppIdSession*, SERVICE_HOST_INFO_CODE, const void*)
{
}

void AppIdFreeDhcpData(DhcpFPData* dd)
{
    snort_free(dd);
}

static int AppIdAddDHCP(AppIdSession* flowp, unsigned op55_len, const uint8_t* op55, unsigned
    op60_len, const uint8_t* op60, const uint8_t* mac)
{
    if (op55_len && op55_len <= DHCP_OPTION55_LEN_MAX
            && !flowp->getAppIdFlag(APPID_SESSION_HAS_DHCP_FP))
    {
        DhcpFPData* rdd;

        rdd = (DhcpFPData*)snort_calloc(sizeof(*rdd));
        if (flowp->add_flow_data(rdd, APPID_SESSION_DATA_DHCP_FP_DATA,
            (AppIdFreeFCN)AppIdFreeDhcpData))
        {
            AppIdFreeDhcpData(rdd);
            return -1;
        }

        flowp->setAppIdFlag(APPID_SESSION_HAS_DHCP_FP);
        rdd->op55_len = (op55_len > DHCP_OP55_MAX_SIZE) ? DHCP_OP55_MAX_SIZE : op55_len;
        memcpy(rdd->op55, op55, rdd->op55_len);
        rdd->op60_len =  (op60_len > DHCP_OP60_MAX_SIZE) ? DHCP_OP60_MAX_SIZE : op60_len;
        if (op60_len)
            memcpy(rdd->op60, op60, rdd->op60_len);
        memcpy(rdd->mac, mac, sizeof(rdd->mac));
    }
    return 0;
}

void AppIdFreeDhcpInfo(DHCPInfo* dd)
{
    if (dd)
    {
        dd->next = dhcp_info_free_list;
        dhcp_info_free_list = dd;
    }
}

static void AppIdAddHostIP(AppIdSession* flow, const uint8_t* mac, uint32_t ip, int32_t zone,
    uint32_t subnetmask, uint32_t leaseSecs, uint32_t router)
{
    DHCPInfo* info;
    unsigned flags;

    if (memcmp(mac, zeromac, 6) == 0 || ip == 0)
        return;

    if (!flow->getAppIdFlag(APPID_SESSION_DO_RNA)
            || flow->getAppIdFlag(APPID_SESSION_HAS_DHCP_INFO))
        return;

    flags = isIPv4HostMonitored(ntohl(ip), zone);
    if (!(flags & IPFUNCS_HOSTS_IP))
        return;

    if (dhcp_info_free_list)
    {
        info = dhcp_info_free_list;
        dhcp_info_free_list = info->next;
    }
    else
        info = (DHCPInfo*)snort_calloc(sizeof(DHCPInfo));

    if (flow->add_flow_data(info, APPID_SESSION_DATA_DHCP_INFO,
        (AppIdFreeFCN)AppIdFreeDhcpInfo))
    {
        AppIdFreeDhcpInfo(info);
        return;
    }
    flow->setAppIdFlag(APPID_SESSION_HAS_DHCP_INFO);
    info->ipAddr = ip;
    memcpy(info->macAddr, mac, sizeof(info->macAddr));
    info->subnetmask = subnetmask;
    info->leaseSecs = leaseSecs;
    info->router = router;
}

void AppIdFreeSMBData(FpSMBData* sd)
{
    if (sd)
    {
        sd->next = smb_data_free_list;
        smb_data_free_list = sd;
    }
}

static void AppIdAddSMBData(AppIdSession* flow, unsigned major, unsigned minor, uint32_t flags)
{
    FpSMBData* sd;

    if (flags & FINGERPRINT_UDP_FLAGS_XENIX)
        return;

    if (smb_data_free_list)
    {
        sd = smb_data_free_list;
        smb_data_free_list = sd->next;
    }
    else
        sd = (FpSMBData*)snort_calloc(sizeof(FpSMBData));

    if (flow->add_flow_data(sd, APPID_SESSION_DATA_SMB_DATA, (AppIdFreeFCN)AppIdFreeSMBData))
    {
        AppIdFreeSMBData(sd);
        return;
    }

    flow->setAppIdFlag(APPID_SESSION_HAS_SMB_INFO);
    sd->major = major;
    sd->minor = minor;
    sd->flags = flags & FINGERPRINT_UDP_FLAGS_MASK;
}

static int AppIdServiceAddServiceEx(AppIdSession* flow, const Packet* pkt, int dir,
    const RNAServiceElement* svc_element,
    AppId appId, const char* vendor, const char* version)
{
    AppIdServiceIDState* id_state;
    uint16_t port;
    const sfip_t* ip;

    if (!flow || !pkt || !svc_element)
    {
        ErrorMessage("Invalid arguments to absinthe_add_appId");
        return SERVICE_EINVALID;
    }

    flow->serviceData = svc_element;

    if (vendor)
    {
        if (flow->serviceVendor)
            snort_free(flow->serviceVendor);
        flow->serviceVendor = snort_strdup(vendor);
    }
    if (version)
    {
        if (flow->serviceVersion)
            snort_free(flow->serviceVersion);
        flow->serviceVersion = snort_strdup(version);
    }
    flow->setAppIdFlag(APPID_SESSION_SERVICE_DETECTED);
    flow->serviceAppId = appId;

    checkSandboxDetection(appId);

    if (flow->getAppIdFlag(APPID_SESSION_IGNORE_HOST))
        return SERVICE_SUCCESS;

    if (!flow->getAppIdFlag(APPID_SESSION_UDP_REVERSED))
    {
        if (dir == APP_ID_FROM_INITIATOR)
        {
            ip = pkt->ptrs.ip_api.get_dst();
            port = pkt->ptrs.dp;
        }
        else
        {
            ip = pkt->ptrs.ip_api.get_src();
            port = pkt->ptrs.sp;
        }
        if (flow->service_port)
            port = flow->service_port;
    }
    else
    {
        if (dir == APP_ID_FROM_INITIATOR)
        {
            ip = pkt->ptrs.ip_api.get_src();
            port = pkt->ptrs.sp;
        }
        else
        {
            ip = pkt->ptrs.ip_api.get_dst();
            port = pkt->ptrs.dp;
        }
    }

    /* If we ended up with UDP reversed, make sure we're pointing to the
     * correct host tracker entry. */
    if (flow->getAppIdFlag(APPID_SESSION_UDP_REVERSED))
    {
        flow->id_state = AppIdGetServiceIDState(ip, flow->protocol, port,
            AppIdServiceDetectionLevel(flow));
    }

    if (!(id_state = flow->id_state))
    {
        if (!(id_state = AppIdAddServiceIDState(ip, flow->protocol, port, AppIdServiceDetectionLevel(
                flow))))
        {
            ErrorMessage("Add service failed to create state");
            return SERVICE_ENOMEM;
        }
        flow->id_state = id_state;
        flow->service_ip = *ip;
        flow->service_port = port;
    }
    else
    {
        if (id_state->serviceList)
        {
            AppIdFreeServiceMatchList(id_state->serviceList);
            id_state->serviceList = nullptr;
            id_state->currenService = nullptr;
        }
        if (!sfip_is_set(&flow->service_ip))
        {
            flow->service_ip = *ip;
            flow->service_port = port;
        }
#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
        if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
        fprintf(SF_DEBUG_FILE, "Service %d for protocol %u on port %u (%u->%u) is valid\n",
            (int)appId, (unsigned)flow->proto, (unsigned)flow->service_port,
            (unsigned)pkt->ptrs.sp, (unsigned)pkt->ptrs.dp);
#endif
    }
    id_state->reset_time = 0;
    if (id_state->state != SERVICE_ID_VALID)
    {
        id_state->state = SERVICE_ID_VALID;
        id_state->valid_count = 0;
        id_state->detract_count = 0;
        id_state->last_detract.clear();
        id_state->invalid_client_count = 0;
        id_state->last_invalid_client.clear();
    }
    id_state->svc = svc_element;

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
    {
        char ipstr[INET6_ADDRSTRLEN];

        ipstr[0] = 0;
        sfip_ntop(&flow->service_ip, ipstr, sizeof(ipstr));
        fprintf(SF_DEBUG_FILE, "Valid: %s:%u:%u %p %d\n", ipstr, (unsigned)flow->proto,
            (unsigned)flow->service_port, id_state, (int)id_state->state);
    }
#endif

    if (!id_state->valid_count)
    {
        id_state->valid_count++;
        id_state->invalid_client_count = 0;
        id_state->last_invalid_client.clear();
        id_state->detract_count = 0;
        id_state->last_detract.clear();
    }
    else if (id_state->valid_count < STATE_ID_MAX_VALID_COUNT)
        id_state->valid_count++;

    /* Done looking for this session. */
    id_state->searching = false;

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
    fprintf(SF_DEBUG_FILE, "Service %d for protocol %u on port %u (%u->%u) is valid\n",
        (int)appId, (unsigned)flow->proto, (unsigned)flow->service_port, (unsigned)pkt->ptrs.sp,
        (unsigned)pkt->ptrs.dp);
#endif
    return SERVICE_SUCCESS;
}

int AppIdServiceAddServiceSubtype(AppIdSession* flow, const Packet* pkt, int dir,
    const RNAServiceElement* svc_element,
    AppId appId, const char* vendor, const char* version,
    RNAServiceSubtype* subtype)
{
    flow->subtype = subtype;
    if (!svc_element->current_ref_count)
    {
#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
        if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
        fprintf(SF_DEBUG_FILE,
            "Service %d for protocol %u on port %u (%u->%u) is valid, but skipped\n",
            (int)appId, (unsigned)flow->proto, (unsigned)flow->service_port,
            (unsigned)pkt->ptrs.sp, (unsigned)pkt->ptrs.dp);
#endif
        return SERVICE_SUCCESS;
    }
    return AppIdServiceAddServiceEx(flow, pkt, dir, svc_element, appId, vendor, version);
}

int AppIdServiceAddService(AppIdSession* flow, const Packet* pkt, int dir,
    const RNAServiceElement* svc_element,
    AppId appId, const char* vendor, const char* version,
    const RNAServiceSubtype* subtype)
{
    RNAServiceSubtype* new_subtype = nullptr;
    RNAServiceSubtype* tmp_subtype;

    if (!svc_element->current_ref_count)
    {
#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
        if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
        fprintf(SF_DEBUG_FILE,
            "Service %d for protocol %u on port %u (%u->%u) is valid, but skipped\n",
            (int)appId, (unsigned)flow->proto, (unsigned)flow->service_port,
            (unsigned)pkt->ptrs.sp, (unsigned)pkt->ptrs.dp);
#endif
        return SERVICE_SUCCESS;
    }

    for (; subtype; subtype = subtype->next)
    {
        tmp_subtype = (RNAServiceSubtype*)snort_calloc(sizeof(RNAServiceSubtype));
        if (subtype->service)
            tmp_subtype->service = snort_strdup(subtype->service);

        if (subtype->vendor)
            tmp_subtype->vendor = snort_strdup(subtype->vendor);

        if (subtype->version)
            tmp_subtype->version = snort_strdup(subtype->version);

        tmp_subtype->next = new_subtype;
        new_subtype = tmp_subtype;
    }
    flow->subtype = new_subtype;
    return AppIdServiceAddServiceEx(flow, pkt, dir, svc_element, appId, vendor, version);
}

int AppIdServiceInProcess(AppIdSession* flow, const Packet* pkt, int dir,
    const RNAServiceElement* svc_element)
{
    AppIdServiceIDState* id_state;

    if (!flow || !pkt)
    {
        ErrorMessage("Invalid arguments to service_in_process");
        return SERVICE_EINVALID;
    }

    if (dir == APP_ID_FROM_INITIATOR || flow->getAppIdFlag(APPID_SESSION_IGNORE_HOST|
        APPID_SESSION_UDP_REVERSED))
        return SERVICE_SUCCESS;

    if (!(id_state = flow->id_state))
    {
        uint16_t port;
        const sfip_t* ip;

        ip = pkt->ptrs.ip_api.get_src();
        port = flow->service_port ? flow->service_port : pkt->ptrs.sp;

        if (!(id_state = AppIdAddServiceIDState(ip, flow->protocol, port, AppIdServiceDetectionLevel(
                flow))))
        {
            ErrorMessage("In-process service failed to create state");
            return SERVICE_ENOMEM;
        }
        flow->id_state = id_state;
        flow->service_ip = *ip;
        flow->service_port = port;
        id_state->state = SERVICE_ID_NEW;
        id_state->svc = svc_element;
    }
    else
    {
        if (!sfip_is_set(&flow->service_ip))
        {
            const sfip_t* ip = pkt->ptrs.ip_api.get_src();
            flow->service_ip = *ip;
            if (!flow->service_port)
                flow->service_port = pkt->ptrs.sp;
        }
#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
        if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
        fprintf(SF_DEBUG_FILE, "Service for protocol %u on port %u is in process (%u->%u), %p %s",
            (unsigned)flow->proto, (unsigned)flow->service_port, (unsigned)pkt->ptrs.sp,
            (unsigned)pkt->ptrs.dp,
            svc_element->validate, svc_element->name ? : "UNKNOWN");
#endif
    }

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
    {
        char ipstr[INET6_ADDRSTRLEN];

        ipstr[0] = 0;
        sfip_ntop(&flow->service_ip, ipstr, sizeof(ipstr));
        fprintf(SF_DEBUG_FILE, "Inprocess: %s:%u:%u %p %d\n", ipstr, (unsigned)flow->proto,
            (unsigned)flow->service_port,
            id_state, (int)id_state->state);
    }
#endif

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
    fprintf(SF_DEBUG_FILE, "Service for protocol %u on port %u is in process (%u->%u), %s\n",
        (unsigned)flow->proto, (unsigned)flow->service_port, (unsigned)pkt->ptrs.sp,
        (unsigned)pkt->ptrs.dp,
        svc_element->name ? : "UNKNOWN");
#endif

    return SERVICE_SUCCESS;
}

/**Called when service can not be identified on a flow but the checks failed on client request
 * rather than server response. When client request fails a check, it may be specific to a client
 * therefore we should not fail the service right away. If the same behavior is seen from the same
 * client ultimately we will have to fail the service. If the same behavior is seen from different
 * clients going to same service then this most likely the service is something else.
 */
int AppIdServiceIncompatibleData(AppIdSession* flow, const Packet* pkt, int dir,
    const RNAServiceElement* svc_element, unsigned flow_data_index, const AppIdConfig*)
{
    AppIdServiceIDState* id_state;

    if (!flow || !pkt)
    {
        ErrorMessage("Invalid arguments to service_incompatible_data");
        return SERVICE_EINVALID;
    }

    if (flow_data_index != APPID_SESSION_DATA_NONE)
        flow->free_flow_data_by_id(flow_data_index);

    /* If we're still working on a port/pattern list of detectors, then ignore
     * individual fails until we're done looking at everything. */
    if (    (flow->serviceData == nullptr)                                                /* we're
                                                                                          working
                                                                                          on a list
                                                                                          of
                                                                                          detectors,
                                                                                          and... */
        && (flow->candidate_service_list != nullptr)
        && (flow->id_state != nullptr) )
    {
        if (sflist_count(flow->candidate_service_list) != 0)
        {
            return SERVICE_SUCCESS;
        }
        else if ((flow->num_candidate_services_tried >= MAX_CANDIDATE_SERVICES)
            || (flow->id_state->state == SERVICE_ID_BRUTE_FORCE) )
        {
            return SERVICE_SUCCESS;
        }
    }

    flow->setAppIdFlag(APPID_SESSION_SERVICE_DETECTED);
    flow->clearAppIdFlag(APPID_SESSION_CONTINUE);

    flow->serviceAppId = APP_ID_NONE;

    if (flow->getAppIdFlag(APPID_SESSION_IGNORE_HOST|APPID_SESSION_UDP_REVERSED) || (svc_element &&
        !svc_element->current_ref_count))
        return SERVICE_SUCCESS;

    if (dir == APP_ID_FROM_INITIATOR)
    {
        flow->setAppIdFlag(APPID_SESSION_INCOMPATIBLE);
        return SERVICE_SUCCESS;
    }

    if (!(id_state = flow->id_state))
    {
        uint16_t port;
        const sfip_t* ip;

        ip = pkt->ptrs.ip_api.get_src();
        port = flow->service_port ? flow->service_port : pkt->ptrs.sp;

        if (!(id_state = AppIdAddServiceIDState(ip, flow->protocol, port, AppIdServiceDetectionLevel(
                flow))))
        {
            ErrorMessage("Incompatible service failed to create state");
            return SERVICE_ENOMEM;
        }
        flow->id_state = id_state;
        flow->service_ip = *ip;
        flow->service_port = port;
        id_state->state = SERVICE_ID_NEW;
        id_state->svc = svc_element;
    }
    else
    {
        if (!sfip_is_set(&flow->service_ip))
        {
            const sfip_t* ip = pkt->ptrs.ip_api.get_src();
            flow->service_ip = *ip;
            if (!flow->service_port)
                flow->service_port = pkt->ptrs.sp;
#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
            if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
            fprintf(SF_DEBUG_FILE,
                "service_IC: Changed State to %s for protocol %u on port %u (%u->%u), count %u, %s\n",
                serviceIdStateName[id_state->state], (unsigned)flow->proto,
                (unsigned)flow->service_port,
                (unsigned)pkt->ptrs.sp, (unsigned)pkt->ptrs.dp, id_state->invalid_client_count,
                (id_state->svc && id_state->svc->name) ? id_state->svc->name : "UNKNOWN");
#endif
        }
        id_state->reset_time = 0;
    }

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
    fprintf(SF_DEBUG_FILE,
        "service_IC: State %s for protocol %u on port %u (%u->%u), count %u, %s\n",
        serviceIdStateName[id_state->state], (unsigned)flow->proto, (unsigned)flow->service_port,
        (unsigned)pkt->ptrs.sp, (unsigned)pkt->ptrs.dp, id_state->invalid_client_count,
        (id_state->svc && id_state->svc->name) ? id_state->svc->name : "UNKNOWN");
#endif

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
    {
        char ipstr[INET6_ADDRSTRLEN];

        ipstr[0] = 0;
        sfip_ntop(&flow->service_ip, ipstr, sizeof(ipstr));
        fprintf(SF_DEBUG_FILE, "Incompat: %s:%u:%u %p %d %s\n", ipstr, (unsigned)flow->proto,
            (unsigned)flow->service_port,
            id_state, (int)id_state->state,
            (id_state->svc && id_state->svc->name) ? id_state->svc->name : "UNKNOWN");
    }
#endif

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
    {
        char ipstr[INET6_ADDRSTRLEN];

        ipstr[0] = 0;
        sfip_ntop(&flow->service_ip, ipstr, sizeof(ipstr));
        fprintf(SF_DEBUG_FILE, "Incompat End: %s:%u:%u %p %d %s\n", ipstr, (unsigned)flow->proto,
            (unsigned)flow->service_port,
            id_state, (int)id_state->state, (id_state->svc && id_state->svc->name) ?
            id_state->svc->name : "UNKNOWN");
    }
#endif

    return SERVICE_SUCCESS;
}

int AppIdServiceFailService(AppIdSession* flow, const Packet* pkt, int dir,
    const RNAServiceElement* svc_element, unsigned flow_data_index, const AppIdConfig*)
{
    AppIdServiceIDState* id_state;

    if (flow_data_index != APPID_SESSION_DATA_NONE)
        flow->free_flow_data_by_id(flow_data_index);

    /* If we're still working on a port/pattern list of detectors, then ignore
     * individual fails until we're done looking at everything. */
    if ( (flow->serviceData == nullptr)
        && (flow->candidate_service_list != nullptr)
        && (flow->id_state != nullptr) )
    {
        if (sflist_count(flow->candidate_service_list) != 0)
            return SERVICE_SUCCESS;
        else if ( (flow->num_candidate_services_tried >= MAX_CANDIDATE_SERVICES)
            || (flow->id_state->state == SERVICE_ID_BRUTE_FORCE) )
            return SERVICE_SUCCESS;
    }

    flow->serviceAppId = APP_ID_NONE;

    flow->setAppIdFlag(APPID_SESSION_SERVICE_DETECTED);
    flow->clearAppIdFlag(APPID_SESSION_CONTINUE);

    /* detectors should be careful in marking flow UDP_REVERSED otherwise the same detector
     * gets all future flows. UDP_REVERSE should be marked only when detector positively
     * matches opposite direction patterns. */

    if (flow->getAppIdFlag(APPID_SESSION_IGNORE_HOST | APPID_SESSION_UDP_REVERSED)
            || (svc_element && !svc_element->current_ref_count))
        return SERVICE_SUCCESS;

    /* For subsequent packets, avoid marking service failed on client packet,
     * otherwise the service will show up on client side. */
    if (dir == APP_ID_FROM_INITIATOR)
    {
        flow->setAppIdFlag(APPID_SESSION_INCOMPATIBLE);
        return SERVICE_SUCCESS;
    }

    if (!(id_state = flow->id_state))
    {
        uint16_t port;
        const sfip_t* ip;

        ip = pkt->ptrs.ip_api.get_src();
        port = flow->service_port ? flow->service_port : pkt->ptrs.sp;

        if (!(id_state = AppIdAddServiceIDState(ip, flow->protocol, port, AppIdServiceDetectionLevel(
                flow))))
        {
            ErrorMessage("Fail service failed to create state");
            return SERVICE_ENOMEM;
        }
        flow->id_state = id_state;
        flow->service_ip = *ip;
        flow->service_port = port;
        id_state->state = SERVICE_ID_NEW;
        id_state->svc = svc_element;
    }
    else
    {
        if (!sfip_is_set(&flow->service_ip))
        {
            const sfip_t* ip = pkt->ptrs.ip_api.get_src();
            flow->service_ip = *ip;
            if (!flow->service_port)
                flow->service_port = pkt->ptrs.sp;
        }
#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
        if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
        fprintf(SF_DEBUG_FILE,
            "service_fail: State %s for protocol %u on port %u (%u->%u), count %u, valid count %u, currSvc %s\n",
            serviceIdStateName[id_state->state], (unsigned)flow->proto,
            (unsigned)flow->service_port,
            (unsigned)pkt->ptrs.sp, (unsigned)pkt->ptrs.dp, id_state->invalid_client_count,
            id_state->valid_count,
            (svc_element && svc_element->name) ? svc_element->name : "UNKNOWN");
#endif
    }
    id_state->reset_time = 0;

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
    fprintf(SF_DEBUG_FILE,
        "service_fail: State %s for protocol %u on port %u (%u->%u), count %u, valid count %u, currSvc %s\n",
        serviceIdStateName[id_state->state], (unsigned)flow->proto, (unsigned)flow->service_port,
        (unsigned)pkt->ptrs.sp, (unsigned)pkt->ptrs.dp, id_state->invalid_client_count,
        id_state->valid_count,
        (svc_element && svc_element->name) ? svc_element->name : "UNKNOWN");
#endif

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
    {
        char ipstr[INET6_ADDRSTRLEN];

        ipstr[0] = 0;
        sfip_ntop(&flow->service_ip, ipstr, sizeof(ipstr));
        fprintf(SF_DEBUG_FILE, "Fail: %s:%u:%u %p %d %s\n", ipstr, (unsigned)flow->proto,
            (unsigned)flow->service_port,
            id_state, (int)id_state->state,
            (id_state->svc && id_state->svc->name) ? id_state->svc->name : "UNKNOWN");
    }
#endif

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (pkt->ptrs.dp == SERVICE_DEBUG_PORT || pkt->ptrs.sp == SERVICE_DEBUG_PORT)
#endif
    {
        char ipstr[INET6_ADDRSTRLEN];

        ipstr[0] = 0;
        sfip_ntop(&flow->service_ip, ipstr, sizeof(ipstr));
        fprintf(SF_DEBUG_FILE, "Fail End: %s:%u:%u %p %d %s\n", ipstr, (unsigned)flow->proto,
            (unsigned)flow->service_port,
            id_state, (int)id_state->state, (id_state->svc && id_state->svc->name) ?
            id_state->svc->name : "UNKNOWN");
    }
#endif

    return SERVICE_SUCCESS;
}

/* Handle some exception cases on failure:
 *  - valid_count: If we have a detector that should be valid, but it keeps
 *    failing, consider restarting the detector search.
 *  - invalid_client_count: If our service detector search had trouble
 *    simply because of unrecognized client data, then consider retrying
 *    the search again. */
static void HandleFailure(AppIdSession* flowp,
    AppIdServiceIDState* id_state,
    const sfip_t* client_ip,
    unsigned timeout)
{
    /* If we had a valid detector, check for too many fails.  If so, start
     * search sequence again. */
    if (id_state->state == SERVICE_ID_VALID)
    {
        /* Too many invalid clients?  If so, count it as an invalid detect. */
        if (id_state->invalid_client_count >= STATE_ID_INVALID_CLIENT_THRESHOLD)
        {
            if (id_state->valid_count <= 1)
            {
                id_state->state = SERVICE_ID_NEW;
                id_state->invalid_client_count = 0;
                id_state->last_invalid_client.clear();
                id_state->valid_count = 0;
                id_state->detract_count = 0;
                id_state->last_detract.clear();
            }
            else
            {
                id_state->valid_count--;
                id_state->last_invalid_client = *client_ip;
                id_state->invalid_client_count = 0;
            }
        }
        /* Just a plain old fail.  If too many of these happen, start
         * search process over. */
        else if (id_state->invalid_client_count == 0)
        {
            if (sfip_fast_eq6(&id_state->last_detract, client_ip))
                id_state->detract_count++;
            else
                id_state->last_detract = *client_ip;

            if (id_state->detract_count >= STATE_ID_NEEDED_DUPE_DETRACT_COUNT)
            {
                if (id_state->valid_count <= 1)
                {
                    id_state->state = SERVICE_ID_NEW;
                    id_state->invalid_client_count = 0;
                    id_state->last_invalid_client.clear();
                    id_state->valid_count = 0;
                    id_state->detract_count = 0;
                    id_state->last_detract.clear();
                }
                else
                    id_state->valid_count--;
            }
        }
    }
    /* If we were port/pattern searching and timed out, just restart over next
     * time. */
    else if (timeout && (flowp->candidate_service_list != nullptr))
    {
        id_state->state = SERVICE_ID_NEW;
    }
    /* If we were working on a port/pattern list of detectors, see if we
     * should restart search (because of invalid clients) or just let it
     * naturally continue onto brute force next. */
    else if (    (flowp->candidate_service_list != nullptr)
        && (id_state->state == SERVICE_ID_BRUTE_FORCE) )
    {
        /* If we're getting some invalid clients, keep retrying
         * port/pattern search until we either find something or until we
         * just see too many invalid clients. */
        if (    (id_state->invalid_client_count > 0)
            && (id_state->invalid_client_count < STATE_ID_INVALID_CLIENT_THRESHOLD) )
        {
            id_state->state = SERVICE_ID_NEW;
        }
    }

    /* Done looking for this session. */
    id_state->searching = false;
}

/**Changes in_process service state to failed state when a flow is terminated.
 *
 * RNA used to repeat the same service detector if the detector remained in process till the flow terminated. Thus RNA
 * got stuck on this one detector and never tried another service detector. This function will treat such a detector
 * as returning incompatibleData when the flow is terminated. The intent here to make RNA try other service detectors but
 * unlike incompatibleData status, we dont want to undermine confidence in the service.
 *
 * @note Packet may be nullptr when this function is called upon session timeout.
 */
void FailInProcessService(AppIdSession* flowp, const AppIdConfig*)
{
    AppIdServiceIDState* id_state;

    if (flowp->getAppIdFlag(APPID_SESSION_SERVICE_DETECTED|APPID_SESSION_UDP_REVERSED))
        return;

    id_state = AppIdGetServiceIDState(&flowp->service_ip, flowp->protocol, flowp->service_port,
        AppIdServiceDetectionLevel(flowp));

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (flowp->service_port == SERVICE_DEBUG_PORT)
#endif
    fprintf(SF_DEBUG_FILE, "FailInProcess %" PRIx64 ", %08X:%u proto %u\n",
        flowp->common.flags, flowp->common.initiator_ip.ip32[3],
        (unsigned)flowp->service_port, (unsigned)flowp->proto);
#endif

    if (!id_state || (id_state->svc && !id_state->svc->current_ref_count))
        return;

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (flowp->service_port == SERVICE_DEBUG_PORT)
#endif
    fprintf(SF_DEBUG_FILE, "FailInProcess: State %s for protocol %u on port %u, count %u, %s\n",
        serviceIdStateName[id_state->state], (unsigned)flowp->proto, (unsigned)flowp->service_port,
        id_state->invalid_client_count, (id_state->svc && id_state->svc->name) ?
        id_state->svc->name : "UNKNOWN");
#endif

    id_state->invalid_client_count += STATE_ID_INCONCLUSIVE_SERVICE_WEIGHT;

    // FIXIT-M: we need a Flow to get the ip address of client/server...
#ifdef REMOVED_WHILE_NOT_IN_USE
    sfip_t* tmp_ip = _dpd.sessionAPI->get_session_ip_address(flowp->ssn, SSN_DIR_FROM_SERVER);
    if (sfip_fast_eq6(tmp_ip, &flowp->service_ip))
        tmp_ip = _dpd.sessionAPI->get_session_ip_address(flowp->ssn, SSN_DIR_FROM_CLIENT);

    HandleFailure(flowp, id_state, tmp_ip, 1);
#endif

#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
    if (flowp->service_port == SERVICE_DEBUG_PORT)
#endif
    fprintf(SF_DEBUG_FILE,
        "FailInProcess: Changed State to %s for protocol %u on port %u, count %u, %s\n",
        serviceIdStateName[id_state->state], (unsigned)flowp->proto, (unsigned)flowp->service_port,
        id_state->invalid_client_count, (id_state->svc && id_state->svc->name) ?
        id_state->svc->name : "UNKNOWN");
#endif
}

/* This function should be called to find the next service detector to try when
 * we have not yet found a valid detector in the host tracker.  It will try
 * both port and/or pattern (but not brute force - that should be done outside
 * of this function).  This includes UDP reversed services.  A valid id_state
 * (even if just initialized to the NEW state) should exist before calling this
 * function.  The state coming out of this function will reflect the state in
 * which the next detector was found.  If nothing is found, it'll indicate that
 * brute force should be tried next as a state (and return nullptr).  This
 * function can be called once or multiple times (to run multiple detectors in
 * parallel) per flow.  Do not call this function if a detector has already
 * been specified (serviceData).  Basically, this function handles going
 * through the main port/pattern search (and returning which detector to add
 * next to the list of detectors to try (even if only 1)). */
static const RNAServiceElement* AppIdGetNexService(const Packet* p, const int dir,
    AppIdSession* rnaData, AppIdServiceIDState* id_state)
{
    auto proto = rnaData->protocol;

    /* If NEW, just advance onto trying ports. */
    if (id_state->state == SERVICE_ID_NEW)
    {
        id_state->state = SERVICE_ID_PORT;
        id_state->svc   = nullptr;
    }

    /* See if there are any port detectors to try.  If not, move onto patterns. */
    if (id_state->state == SERVICE_ID_PORT)
    {
        id_state->svc = AppIdGetNexServiceByPort(proto, (uint16_t)((dir ==
            APP_ID_FROM_RESPONDER) ? p->ptrs.sp : p->ptrs.dp), id_state->svc, rnaData);
        if (id_state->svc != nullptr)
        {
            return id_state->svc;
        }
        else
        {
            id_state->state = SERVICE_ID_PATTERN;
            id_state->svc   = nullptr;
            if (id_state->serviceList != nullptr)
            {
                id_state->currenService = id_state->serviceList;
            }
            else
            {
                id_state->serviceList    = nullptr;
                id_state->currenService = nullptr;
            }
        }
    }

    if (id_state->state == SERVICE_ID_PATTERN)
    {
        /* If we haven't found anything yet, try to see if we get any hits
         * first with UDP reversed services before moving onto pattern matches. */
        if (dir == APP_ID_FROM_INITIATOR)
        {
            if (!rnaData->getAppIdFlag(APPID_SESSION_ADDITIONAL_PACKET)
                    && (proto == IpProtocol::UDP) && !rnaData->tried_reverse_service )
            {
                SF_LNODE* iter;
                AppIdServiceIDState* reverse_id_state;
                const RNAServiceElement* reverse_service = nullptr;
                const sfip_t* reverse_ip = p->ptrs.ip_api.get_src();
                rnaData->tried_reverse_service = true;
                if ((reverse_id_state = AppIdGetServiceIDState(reverse_ip, proto, p->ptrs.sp,
                        AppIdServiceDetectionLevel(rnaData))))
                {
                    reverse_service = reverse_id_state->svc;
                }
                if ( reverse_service
                    || (serviceConfig->udp_reversed_services[p->ptrs.sp] &&
                    (reverse_service = ( RNAServiceElement*)sflist_first(
                        serviceConfig->udp_reversed_services[p->ptrs.sp], &iter)))
                    || (p->dsize &&
                        (reverse_service = AppIdGetServiceByPattern(p, proto, dir, nullptr))) )
                {
                    id_state->svc = reverse_service;
                    return id_state->svc;
                }
            }
            return nullptr;
        }
        /* Try pattern match detectors.  If not, give up, and go to brute
         * force. */
        else    /* APP_ID_FROM_RESPONDER */
        {
            if (id_state->serviceList == nullptr)    /* no list yet (need to make one) */
            {
                id_state->svc = AppIdGetServiceByPattern(p, proto, dir, id_state);
            }
            else    /* already have a pattern service list (just use it) */
            {
                id_state->svc = AppIdNexServiceByPattern(id_state
#ifdef SERVICE_DEBUG
#if SERVICE_DEBUG_PORT
                    , flow->service_port
#endif
#endif
                    );
            }

            if (id_state->svc != nullptr)
            {
                return id_state->svc;
            }
            else
            {
                id_state->state = SERVICE_ID_BRUTE_FORCE;
                id_state->svc   = nullptr;
                return nullptr;
            }
        }
    }

    /* Don't do anything if it was in VALID or BRUTE FORCE. */
    return nullptr;
}

int AppIdDiscoverService(Packet* p, const int dir, AppIdSession* rnaData,
        const AppIdConfig* pConfig)
{
    const sfip_t* ip;
    int ret = SERVICE_NOMATCH;
    const RNAServiceElement* service;
    AppIdServiceIDState* id_state;
    uint16_t port;
    ServiceValidationArgs args;

    /* Get packet info. */
    auto proto = rnaData->protocol;
    if (sfip_is_set(&rnaData->service_ip))
    {
        ip   = &rnaData->service_ip;
        port = rnaData->service_port;
    }
    else
    {
        if (dir == APP_ID_FROM_RESPONDER)
        {
            ip   = p->ptrs.ip_api.get_src();
            port = p->ptrs.sp;
        }
        else
        {
            ip   = p->ptrs.ip_api.get_dst();
            port = p->ptrs.dp;
        }
    }

    /* Get host tracker state. */
    id_state = rnaData->id_state;
    if (id_state == nullptr)
    {
        id_state = AppIdGetServiceIDState(ip, proto, port, AppIdServiceDetectionLevel(rnaData));

        /* Create it if it doesn't exist yet. */
        if (id_state == nullptr)
        {
            if (!(id_state = AppIdAddServiceIDState(ip, proto, port,
                    AppIdServiceDetectionLevel(rnaData))))
            {
                ErrorMessage("Discover service failed to create state");
                return SERVICE_ENOMEM;
            }
            memset(id_state, 0, sizeof(*id_state));
        }
        rnaData->id_state = id_state;
    }

    if (rnaData->serviceData == nullptr)
    {
        /* If a valid service already exists in host tracker, give it a try. */
        if ((id_state->svc != nullptr) && (id_state->state == SERVICE_ID_VALID))
        {
            rnaData->serviceData = id_state->svc;
        }
        /* If we've gotten to brute force, give next detector a try. */
        else if (    (id_state->state == SERVICE_ID_BRUTE_FORCE)
            && (rnaData->num_candidate_services_tried == 0)
            && !id_state->searching )
        {
            rnaData->serviceData = AppIdGetServiceByBruteForce(proto, id_state->svc);
            id_state->svc = rnaData->serviceData;
        }
    }

    args.data = p->data;
    args.size = p->dsize;
    args.dir = dir;
    args.flowp = rnaData;
    args.pkt = p;
    args.pConfig = pConfig;
    args.app_id_debug_session_flag = app_id_debug_session_flag;
    args.app_id_debug_session = app_id_debug_session;

    /* If we already have a service to try, then try it out. */
    if (rnaData->serviceData != nullptr)
    {
        service = rnaData->serviceData;
        args.userdata = service->userdata;
        ret = service->validate(&args);
        if (ret == SERVICE_NOT_COMPATIBLE)
            rnaData->got_incompatible_services = 1;
        if (app_id_debug_session_flag)
            LogMessage("AppIdDbg %s %s returned %d\n", app_id_debug_session,
                service->name ? service->name : "UNKNOWN", ret);
    }
    /* Else, try to find detector(s) to use based on ports and patterns. */
    else
    {
        if (rnaData->candidate_service_list == nullptr)
        {
            rnaData->candidate_service_list = (SF_LIST*)snort_calloc(sizeof(SF_LIST));
            sflist_init(rnaData->candidate_service_list);
            rnaData->num_candidate_services_tried = 0;

            /* This is our first time in for this session, and we're about to
             * search for a service, because we don't have any solid history on
             * this IP/port yet.  If some other session is also currently
             * searching on this host tracker entry, reset state here, so that
             * we can start search over again with this session. */
            if (id_state->searching)
                id_state->state = SERVICE_ID_NEW;
            id_state->searching = true;
        }

        /* See if we've got more detector(s) to add to the candidate list. */
        if (    (id_state->state == SERVICE_ID_NEW)
            || (id_state->state == SERVICE_ID_PORT)
            || ((id_state->state == SERVICE_ID_PATTERN) && (dir == APP_ID_FROM_RESPONDER)) )
        {
            while (rnaData->num_candidate_services_tried < MAX_CANDIDATE_SERVICES)
            {
                const RNAServiceElement* tmp = AppIdGetNexService(p, dir, rnaData, id_state);
                if (tmp != nullptr)
                {
                    SF_LNODE* iter = nullptr;
                    /* Add to list (if not already there). */
                    service = (RNAServiceElement*)sflist_first(rnaData->candidate_service_list,
                        &iter);
                    while (service && (service != tmp))
                        service = (RNAServiceElement*)sflist_next(&iter);
                    if (service == nullptr)
                    {
                        sflist_add_tail(rnaData->candidate_service_list, (void*)tmp);
                        rnaData->num_candidate_services_tried++;
                    }
                }
                else
                {
                    break;
                }
            }
        }

        /* Run all of the detectors that we currently have. */
        ret = SERVICE_INPROCESS;
        SF_LNODE* iter;
        service = (RNAServiceElement*)sflist_first(rnaData->candidate_service_list, &iter);
        while (service)
        {
            int result;

            args.userdata = service->userdata;
            result = service->validate(&args);
            if (result == SERVICE_NOT_COMPATIBLE)
                rnaData->got_incompatible_services = 1;
            if (app_id_debug_session_flag)
                LogMessage("AppIdDbg %s %s returned %d\n", app_id_debug_session,
                    service->name ? service->name : "UNKNOWN", result);

            if (result == SERVICE_SUCCESS)
            {
                ret = SERVICE_SUCCESS;
                rnaData->serviceData = service;
                sflist_free(rnaData->candidate_service_list);
                rnaData->candidate_service_list = nullptr;
                break;    /* done */
            }
            else if (result != SERVICE_INPROCESS)    /* fail */
            {
                sflist_remove_node(rnaData->candidate_service_list, iter);
                service = (RNAServiceElement*)sflist_first(rnaData->candidate_service_list, &iter);
            }
            else
                service = (RNAServiceElement*)sflist_next(&iter);
        }

        /* If we tried everything and found nothing, then fail. */
        if (ret != SERVICE_SUCCESS)
        {
            if (    (sflist_count(rnaData->candidate_service_list) == 0)
                && (    (rnaData->num_candidate_services_tried >= MAX_CANDIDATE_SERVICES)
                || (id_state->state == SERVICE_ID_BRUTE_FORCE) ) )
            {
                AppIdServiceFailService(rnaData, p, dir, nullptr, APPID_SESSION_DATA_NONE,
                    pConfig);
                ret = SERVICE_NOMATCH;
            }
        }
    }

    if (service != nullptr)
    {
        id_state->reset_time = 0;
    }
    else if (dir == APP_ID_FROM_RESPONDER)    /* we have seen bidirectional exchange and have not
                                                 identified any service */
    {
        if (app_id_debug_session_flag)
            LogMessage("AppIdDbg %s no RNA service detector\n", app_id_debug_session);
        AppIdServiceFailService(rnaData, p, dir, nullptr, APPID_SESSION_DATA_NONE, pConfig);
        ret = SERVICE_NOMATCH;
    }

    /* Handle failure exception cases in states. */
    if ((ret != SERVICE_INPROCESS) && (ret != SERVICE_SUCCESS))
    {
        const sfip_t* tmp_ip;
        if (dir == APP_ID_FROM_RESPONDER)
            tmp_ip = p->ptrs.ip_api.get_dst();
        else
            tmp_ip = p->ptrs.ip_api.get_src();

        if (rnaData->got_incompatible_services)
        {
            if (id_state->invalid_client_count < STATE_ID_INVALID_CLIENT_THRESHOLD)
            {
                if (sfip_fast_equals_raw(&id_state->last_invalid_client, tmp_ip))
                    id_state->invalid_client_count++;
                else
                {
                    id_state->invalid_client_count += 3;
                    id_state->last_invalid_client = *tmp_ip;
                }
            }
        }

        HandleFailure(rnaData, id_state, tmp_ip, 0);
    }

    /* Can free up any pattern match lists if done with them. */
    if (    (id_state->state == SERVICE_ID_BRUTE_FORCE)
        || (id_state->state == SERVICE_ID_VALID) )
    {
        if (id_state->serviceList != nullptr)
        {
            AppIdFreeServiceMatchList(id_state->serviceList);
        }
        id_state->serviceList    = nullptr;
        id_state->currenService = nullptr;
    }

    return ret;
}

static void* service_flowdata_get(AppIdSession* flow, unsigned service_id)
{
    return flow->get_flow_data(service_id);
}

static int service_flowdata_add(AppIdSession* flow, void* data, unsigned service_id, AppIdFreeFCN
    fcn)
{
    return flow->add_flow_data(data, service_id, fcn);
}

static void dumpServices(FILE* stream, SF_LIST* const* parray)
{
    int i,n = 0;
    for (i = 0; i < RNA_SERVICE_MAX_PORT; i++)
    {
        if (parray[i] && (sflist_count(parray[i]) != 0))
        {
            if ( n !=  0)
                fprintf(stream," ");

            n++;
            fprintf(stream,"%d",i);
        }
    }
}

void dumpPorts(FILE* stream)
{
    fprintf(stream,"(tcp ");
    dumpServices(stream, serviceConfig->tcp_services);
    fprintf(stream,") \n");
    fprintf(stream,"(udp ");
    dumpServices(stream, serviceConfig->udp_services);
    fprintf(stream,") \n");
}

static void AppIdServiceAddMisc(AppIdSession* flow, AppId miscId)
{
    if (flow != nullptr)
        flow->misc_app_id = miscId;
}
