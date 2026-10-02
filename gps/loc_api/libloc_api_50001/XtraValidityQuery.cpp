#define LOG_TAG "LocSvc_eng"

#include "XtraValidityQuery.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>

#include <log/log.h>

#include "loc_api_v02_client.h"

namespace {

const char kLocApiLibrary[] = "libloc_api_v02.so";
const uint32_t kRequestTimeoutMs = 4000;

typedef void (*LocSyncProcessIndication)(locClientHandleType, uint32_t, void*);
typedef locClientStatusEnumType (*LocSyncSendRequest)(
        locClientHandleType, uint32_t, locClientReqUnionType, uint32_t,
        uint32_t, void*);
typedef locClientStatusEnumType (*LocClientOpen)(
        locClientEventMaskType, const locClientCallbacksType*,
        locClientHandleType*, const void*);
typedef locClientStatusEnumType (*LocClientClose)(locClientHandleType*);

struct LocClientFunctions {
    void* library;
    LocSyncProcessIndication processIndication;
    LocSyncSendRequest sendRequest;
    LocClientOpen open;
    LocClientClose close;
    bool ready;
};

LocClientFunctions gClientFunctions = {};
pthread_once_t gClientInitOnce = PTHREAD_ONCE_INIT;

void initializeClientFunctions()
{
    gClientFunctions.library = dlopen(kLocApiLibrary, RTLD_NOW);
    if (gClientFunctions.library == nullptr) {
        ALOGE("XTRA QMI client load failed: %s", dlerror());
        return;
    }

    gClientFunctions.processIndication =
            reinterpret_cast<LocSyncProcessIndication>(
                    dlsym(gClientFunctions.library, "loc_sync_process_ind"));
    gClientFunctions.sendRequest = reinterpret_cast<LocSyncSendRequest>(
            dlsym(gClientFunctions.library, "loc_sync_send_req"));
    gClientFunctions.open = reinterpret_cast<LocClientOpen>(
            dlsym(gClientFunctions.library, "locClientOpen"));
    gClientFunctions.close = reinterpret_cast<LocClientClose>(
            dlsym(gClientFunctions.library, "locClientClose"));
    if (gClientFunctions.processIndication == nullptr ||
            gClientFunctions.sendRequest == nullptr ||
            gClientFunctions.open == nullptr ||
            gClientFunctions.close == nullptr) {
        ALOGE("XTRA QMI client symbol missing: %s", dlerror());
        return;
    }

    /* The production getLocApi() initializes the shared synchronous QMI
       request slots before the GPS HAL accepts XTRA injections. */
    gClientFunctions.ready = true;
}

void responseCallback(locClientHandleType handle, uint32_t responseId,
                      const locClientRespIndUnionType response, void*)
{
    gClientFunctions.processIndication(
            handle, responseId,
            const_cast<qmiLocGetPredictedOrbitsDataValidityIndMsgT_v02*>(
                    response.pGetPredictedOrbitsDataValidityInd));
}

void eventCallback(locClientHandleType, uint32_t,
                   const locClientEventIndUnionType, void*)
{
}

void errorCallback(locClientHandleType, locClientErrorEnumType error, void*)
{
    ALOGE("XTRA QMI client error: %d", error);
}

}  // namespace

/* GET_PREDICTED_ORBITS_DATA_VALIDITY reads the window the engine holds and
   carries no payload, so a second client reads it without touching the
   production client's injection. Data injected through a second client is
   acknowledged part by part yet never reaches this window, so injection
   stays on the production client. */
XtraValidity queryXtraValidity()
{
    XtraValidity result = {false, false, -1, -1, 0, 0};
    pthread_once(&gClientInitOnce, initializeClientFunctions);
    if (!gClientFunctions.ready)
        return result;

    locClientCallbacksType callbacks = {};
    callbacks.size = sizeof(callbacks);
    callbacks.eventIndCb = eventCallback;
    callbacks.respIndCb = responseCallback;
    callbacks.errorCb = errorCallback;

    locClientHandleType handle = LOC_CLIENT_INVALID_HANDLE_VALUE;
    locClientStatusEnumType clientStatus =
            gClientFunctions.open(0, &callbacks, &handle, nullptr);
    result.clientStatus = clientStatus;
    if (clientStatus != eLOC_CLIENT_SUCCESS ||
            handle == LOC_CLIENT_INVALID_HANDLE_VALUE)
        return result;

    locClientReqUnionType requestUnion = {};
    qmiLocGetPredictedOrbitsDataValidityIndMsgT_v02 indication = {};
    clientStatus = gClientFunctions.sendRequest(
            handle, QMI_LOC_GET_PREDICTED_ORBITS_DATA_VALIDITY_REQ_V02,
            requestUnion, kRequestTimeoutMs,
            QMI_LOC_GET_PREDICTED_ORBITS_DATA_VALIDITY_IND_V02, &indication);
    result.clientStatus = clientStatus;
    if (clientStatus == eLOC_CLIENT_SUCCESS) {
        result.answered = true;
        result.modemStatus = indication.status;
        if (indication.status == eQMI_LOC_SUCCESS_V02 &&
                indication.validityInfo_valid) {
            result.known = true;
            result.startUtc = indication.validityInfo.startTimeInUTC;
            result.durationHours = indication.validityInfo.durationHours;
        }
    } else {
        ALOGE("XTRA validity query transport failure: client=%d", clientStatus);
    }

    gClientFunctions.close(&handle);
    return result;
}

bool xtraValidityCurrent(const XtraValidity& validity, uint64_t nowUtc)
{
    return validity.known && nowUtc >= validity.startUtc &&
            nowUtc - validity.startUtc <
                    static_cast<uint64_t>(validity.durationHours) * 3600u;
}
