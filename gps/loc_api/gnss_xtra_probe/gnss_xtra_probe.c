/* Copyright (c) 2011, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation, nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Read-only QMI LOC v02 probe for the predicted-orbits (XTRA) data path.
 *
 * Sends QMI_LOC_GET_PREDICTED_ORBITS_DATA_SOURCE_REQ_V02 and
 * QMI_LOC_GET_PREDICTED_ORBITS_DATA_VALIDITY_REQ_V02 (location_service_v02.h)
 * and prints the modem's answer: the injection size ceiling
 * (qmiLocPredictedOrbitsAllowedSizesStructT_v02.maxFileSizeInBytes and
 * maxPartSize) and the currently loaded data's validity window. Neither
 * request carries a payload (each request struct is a placeholder-only
 * "DO NOT USE THIS FIELD" type in location_service_v02.h), and this
 * program issues no QMI_LOC_INJECT_PREDICTED_ORBITS_DATA_REQ, no NV or
 * EFS write, no mode change, and no SPC command: it only reads the two
 * indications above.
 *
 * The QMI LOC v02 client is resolved by dlsym against the already-loaded
 * production libloc_api_v02.so, not linked at build time and not
 * reimplemented: this program borrows the same locClientOpen /
 * loc_sync_send_req / loc_sync_process_ind / locClientClose entry points
 * gps.msm8974 already calls in-process, so it carries no risk to the
 * position-fix path and needs no change to any shipping HAL library.
 */

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "loc_api_v02_client.h"

#define PROBE_LOC_API_SO "libloc_api_v02.so"
#define PROBE_TIMEOUT_MSEC 4000

typedef void (*loc_sync_req_init_fn)(void);
typedef void (*loc_sync_process_ind_fn)(locClientHandleType, uint32_t, void*);
typedef locClientStatusEnumType (*loc_sync_send_req_fn)(
    locClientHandleType, uint32_t, locClientReqUnionType, uint32_t, uint32_t,
    void*);
typedef locClientStatusEnumType (*locClientOpen_fn)(
    locClientEventMaskType, const locClientCallbacksType*,
    locClientHandleType*, const void*);
typedef locClientStatusEnumType (*locClientClose_fn)(locClientHandleType*);

static loc_sync_req_init_fn p_loc_sync_req_init;
static loc_sync_process_ind_fn p_loc_sync_process_ind;
static loc_sync_send_req_fn p_loc_sync_send_req;
static locClientOpen_fn p_locClientOpen;
static locClientClose_fn p_locClientClose;

static const char* qmi_loc_status_name(qmiLocStatusEnumT_v02 status)
{
    switch (status) {
    case eQMI_LOC_SUCCESS_V02: return "SUCCESS";
    case eQMI_LOC_GENERAL_FAILURE_V02: return "GENERAL_FAILURE";
    case eQMI_LOC_UNSUPPORTED_V02: return "UNSUPPORTED";
    case eQMI_LOC_INVALID_PARAMETER_V02: return "INVALID_PARAMETER";
    case eQMI_LOC_ENGINE_BUSY_V02: return "ENGINE_BUSY";
    case eQMI_LOC_PHONE_OFFLINE_V02: return "PHONE_OFFLINE";
    case eQMI_LOC_TIMEOUT_V02: return "TIMEOUT";
    case eQMI_LOC_CONFIG_NOT_SUPPORTED_V02: return "CONFIG_NOT_SUPPORTED";
    case eQMI_LOC_INSUFFICIENT_MEMORY_V02: return "INSUFFICIENT_MEMORY";
    case eQMI_LOC_MAX_GEOFENCE_PROGRAMMED_V02: return "MAX_GEOFENCE_PROGRAMMED";
    case eQMI_LOC_XTRA_VERSION_CHECK_FAILURE_V02:
        return "XTRA_VERSION_CHECK_FAILURE";
    default: return "UNKNOWN";
    }
}

/* Mirrors LocApiV02's globalRespCb: every member of the response union
   aliases the same pointer bytes, so any member reads the indication
   payload the sync layer already matched against ind_id. */
static void probe_resp_cb(locClientHandleType handle, uint32_t respId,
                           const locClientRespIndUnionType respPayload,
                           void* pClientCookie)
{
    (void)pClientCookie;
    p_loc_sync_process_ind(handle, respId,
                            (void*)respPayload.pGetPredictedOrbitsDataSourceInd);
}

static void probe_event_cb(locClientHandleType handle, uint32_t eventId,
                            const locClientEventIndUnionType eventPayload,
                            void* pClientCookie)
{
    (void)handle;
    (void)eventId;
    (void)eventPayload;
    (void)pClientCookie;
    /* No event mask is registered (locClientOpen is called with mask 0),
       so the modem sends no asynchronous events to forward here. */
}

static void probe_error_cb(locClientHandleType handle,
                            locClientErrorEnumType errorId,
                            void* pClientCookie)
{
    (void)handle;
    (void)pClientCookie;
    fprintf(stderr, "gnss_xtra_probe: service error indication, errorId=%d\n",
            errorId);
}

static int probe_resolve_symbols(void* lib)
{
    p_loc_sync_req_init = (loc_sync_req_init_fn)dlsym(lib, "loc_sync_req_init");
    p_loc_sync_process_ind =
        (loc_sync_process_ind_fn)dlsym(lib, "loc_sync_process_ind");
    p_loc_sync_send_req = (loc_sync_send_req_fn)dlsym(lib, "loc_sync_send_req");
    p_locClientOpen = (locClientOpen_fn)dlsym(lib, "locClientOpen");
    p_locClientClose = (locClientClose_fn)dlsym(lib, "locClientClose");

    if (!p_loc_sync_req_init || !p_loc_sync_process_ind ||
        !p_loc_sync_send_req || !p_locClientOpen || !p_locClientClose) {
        fprintf(stderr, "gnss_xtra_probe: missing symbol in %s: %s\n",
                PROBE_LOC_API_SO, dlerror());
        return -1;
    }
    return 0;
}

static void print_allowed_sizes(
    const qmiLocGetPredictedOrbitsDataSourceIndMsgT_v02* ind)
{
    printf("GET_PREDICTED_ORBITS_DATA_SOURCE: status=%s\n",
           qmi_loc_status_name(ind->status));
    if (ind->allowedSizes_valid) {
        printf("  maxFileSizeInBytes=%u\n",
               ind->allowedSizes.maxFileSizeInBytes);
        printf("  maxPartSize=%u\n", ind->allowedSizes.maxPartSize);
    } else {
        printf("  allowedSizes not reported by this modem\n");
    }
    if (ind->serverList_valid) {
        uint32_t i;
        for (i = 0; i < ind->serverList.serverList_len; i++) {
            printf("  server[%u]=%s\n", i,
                   ind->serverList.serverList[i].serverUrl);
        }
    }
}

static void print_validity(
    const qmiLocGetPredictedOrbitsDataValidityIndMsgT_v02* ind)
{
    printf("GET_PREDICTED_ORBITS_DATA_VALIDITY: status=%s\n",
           qmi_loc_status_name(ind->status));
    if (ind->validityInfo_valid) {
        time_t start = (time_t)ind->validityInfo.startTimeInUTC;
        time_t end = start + (time_t)ind->validityInfo.durationHours * 3600;
        time_t now = time(NULL);
        printf("  startTimeInUTC=%lld durationHours=%u\n",
               (long long)ind->validityInfo.startTimeInUTC,
               ind->validityInfo.durationHours);
        printf("  window: %s", ctime(&start));
        printf("  through: %s", ctime(&end));
        printf("  now: %s", ctime(&now));
        printf("  %s\n", (now >= start && now < end)
                              ? "VALID as of now"
                              : "NOT valid as of now");
    } else {
        printf("  validityInfo not reported (no predicted orbits loaded)\n");
    }
}

int main(void)
{
    void* lib;
    locClientHandleType handle = LOC_CLIENT_INVALID_HANDLE_VALUE;
    locClientCallbacksType callbacks;
    locClientReqUnionType req_union;
    locClientStatusEnumType status;
    qmiLocGetPredictedOrbitsDataSourceIndMsgT_v02 source_ind;
    qmiLocGetPredictedOrbitsDataValidityIndMsgT_v02 validity_ind;
    int rc = 1;

    lib = dlopen(PROBE_LOC_API_SO, RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "gnss_xtra_probe: dlopen(%s) failed: %s\n",
                PROBE_LOC_API_SO, dlerror());
        return 1;
    }
    if (probe_resolve_symbols(lib) != 0) {
        return 1;
    }

    p_loc_sync_req_init();

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.size = sizeof(callbacks);
    callbacks.eventIndCb = probe_event_cb;
    callbacks.respIndCb = probe_resp_cb;
    callbacks.errorCb = probe_error_cb;

    /* eventRegMask 0: no asynchronous event class is requested, only the
       two synchronous queries below. */
    status = p_locClientOpen(0, &callbacks, &handle, NULL);
    if (status != eLOC_CLIENT_SUCCESS ||
        handle == LOC_CLIENT_INVALID_HANDLE_VALUE) {
        fprintf(stderr, "gnss_xtra_probe: locClientOpen failed, status=%d\n",
                status);
        return 1;
    }

    memset(&req_union, 0, sizeof(req_union));
    memset(&source_ind, 0, sizeof(source_ind));
    status = p_loc_sync_send_req(
        handle, QMI_LOC_GET_PREDICTED_ORBITS_DATA_SOURCE_REQ_V02, req_union,
        PROBE_TIMEOUT_MSEC, QMI_LOC_GET_PREDICTED_ORBITS_DATA_SOURCE_IND_V02,
        &source_ind);
    if (status != eLOC_CLIENT_SUCCESS) {
        fprintf(stderr,
                "gnss_xtra_probe: GET_PREDICTED_ORBITS_DATA_SOURCE failed, "
                "client status=%d\n",
                status);
    } else {
        print_allowed_sizes(&source_ind);
        rc = 0;
    }

    memset(&req_union, 0, sizeof(req_union));
    memset(&validity_ind, 0, sizeof(validity_ind));
    status = p_loc_sync_send_req(
        handle, QMI_LOC_GET_PREDICTED_ORBITS_DATA_VALIDITY_REQ_V02, req_union,
        PROBE_TIMEOUT_MSEC, QMI_LOC_GET_PREDICTED_ORBITS_DATA_VALIDITY_IND_V02,
        &validity_ind);
    if (status != eLOC_CLIENT_SUCCESS) {
        fprintf(stderr,
                "gnss_xtra_probe: GET_PREDICTED_ORBITS_DATA_VALIDITY failed, "
                "client status=%d\n",
                status);
    } else {
        print_validity(&validity_ind);
        rc = 0;
    }

    p_locClientClose(&handle);
    return rc;
}
