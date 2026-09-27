#define LOG_TAG "LocSvc_eng"

#include "XtraQmiInjector.h"

#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

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
            const_cast<qmiLocInjectPredictedOrbitsDataIndMsgT_v02*>(
                    response.pInjectPredictedOrbitsDataInd));
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

XtraInjectionResult injectXtraWithModemStatus(const char* data, int length)
{
    XtraInjectionResult result = {false, -1, -1, 0};
    if (data == nullptr || length <= 0 ||
        length > static_cast<int>(UINT16_MAX *
                                  QMI_LOC_MAX_PREDICTED_ORBITS_PART_LEN_V02))
        return result;

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

    const unsigned int partSize = QMI_LOC_MAX_PREDICTED_ORBITS_PART_LEN_V02;
    const unsigned int totalParts =
            (static_cast<unsigned int>(length) + partSize - 1) / partSize;
    qmiLocInjectPredictedOrbitsDataReqMsgT_v02 request = {};
    request.totalSize = length;
    request.totalParts = totalParts;
    request.formatType_valid = 1;
    request.formatType = eQMI_LOC_PREDICTED_ORBITS_XTRA_V02;

    locClientReqUnionType requestUnion = {};
    requestUnion.pInjectPredictedOrbitsDataReq = &request;

    for (unsigned int part = 1; part <= totalParts; ++part) {
        const unsigned int offset = (part - 1) * partSize;
        const unsigned int remaining = static_cast<unsigned int>(length) - offset;
        request.partNum = part;
        request.partData_len = remaining < partSize ? remaining : partSize;
        memcpy(request.partData, data + offset, request.partData_len);

        qmiLocInjectPredictedOrbitsDataIndMsgT_v02 indication = {};
        clientStatus = gClientFunctions.sendRequest(
                handle, QMI_LOC_INJECT_PREDICTED_ORBITS_DATA_REQ_V02,
                requestUnion, kRequestTimeoutMs,
                QMI_LOC_INJECT_PREDICTED_ORBITS_DATA_IND_V02, &indication);
        result.clientStatus = clientStatus;
        result.modemStatus = indication.status;
        result.partNumber = part;
        if (clientStatus != eLOC_CLIENT_SUCCESS ||
                indication.status != eQMI_LOC_SUCCESS_V02 ||
                (indication.partNum_valid && indication.partNum != part)) {
            ALOGE("XTRA QMI injection rejected: part=%u/%u client=%d modem=%d indication_part=%u",
                  part, totalParts, clientStatus, indication.status,
                  indication.partNum);
            break;
        }
        if (part == totalParts)
            result.accepted = true;
    }

    gClientFunctions.close(&handle);
    return result;
}
