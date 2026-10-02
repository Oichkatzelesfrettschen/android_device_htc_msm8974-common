#include "fake_loc_api.h"

#include "loc_api_v02_client.h"

namespace {

FakeScenario scenario = FAKE_WINDOW;
FakeObservations observations = {};
uint64_t windowStart = 0;
uint16_t windowHours = 0;
locClientCallbacksType callbacks = {};
void* pendingIndication = nullptr;
char clientHandle;

}  // namespace

extern "C" void fakeLocReset(FakeScenario selectedScenario, uint64_t startUtc,
                              uint16_t durationHours)
{
    scenario = selectedScenario;
    observations = {};
    windowStart = startUtc;
    windowHours = durationHours;
    callbacks = {};
    pendingIndication = nullptr;
}

extern "C" FakeObservations fakeLocObservations()
{
    return observations;
}

extern "C" locClientStatusEnumType locClientOpen(
        locClientEventMaskType eventMask, const locClientCallbacksType* clientCallbacks,
        locClientHandleType* handle, const void*)
{
    ++observations.opens;
    if (eventMask != 0 || clientCallbacks == nullptr ||
        clientCallbacks->size != sizeof(*clientCallbacks) ||
        clientCallbacks->respIndCb == nullptr || handle == nullptr)
        ++observations.invalidRequests;
    callbacks = *clientCallbacks;
    *handle = &clientHandle;
    return eLOC_CLIENT_SUCCESS;
}

extern "C" locClientStatusEnumType locClientClose(locClientHandleType* handle)
{
    ++observations.closes;
    if (handle == nullptr || *handle != &clientHandle)
        ++observations.invalidRequests;
    *handle = LOC_CLIENT_INVALID_HANDLE_VALUE;
    return eLOC_CLIENT_SUCCESS;
}

extern "C" void loc_sync_process_ind(locClientHandleType handle, uint32_t indicationId,
                                       void* indication)
{
    ++observations.callbacks;
    if (handle != &clientHandle ||
        indicationId != QMI_LOC_GET_PREDICTED_ORBITS_DATA_VALIDITY_IND_V02 ||
        pendingIndication == nullptr || indication == nullptr)
        ++observations.invalidRequests;
    else
        *static_cast<qmiLocGetPredictedOrbitsDataValidityIndMsgT_v02*>(pendingIndication) =
                *static_cast<qmiLocGetPredictedOrbitsDataValidityIndMsgT_v02*>(indication);
}

extern "C" locClientStatusEnumType loc_sync_send_req(
        locClientHandleType handle, uint32_t requestId, locClientReqUnionType,
        uint32_t timeoutMs, uint32_t indicationId, void* indication)
{
    ++observations.requests;
    if (handle != &clientHandle ||
        requestId != QMI_LOC_GET_PREDICTED_ORBITS_DATA_VALIDITY_REQ_V02 ||
        indicationId != QMI_LOC_GET_PREDICTED_ORBITS_DATA_VALIDITY_IND_V02 ||
        timeoutMs != 4000 || indication == nullptr) {
        ++observations.invalidRequests;
        return eLOC_CLIENT_FAILURE_INVALID_PARAMETER;
    }
    if (scenario == FAKE_TRANSPORT_FAILURE)
        return eLOC_CLIENT_FAILURE_TIMEOUT;

    qmiLocGetPredictedOrbitsDataValidityIndMsgT_v02 response = {};
    response.status = scenario == FAKE_MODEM_FAILURE
            ? eQMI_LOC_GENERAL_FAILURE_V02 : eQMI_LOC_SUCCESS_V02;
    response.validityInfo_valid = scenario != FAKE_MODEM_FAILURE;
    response.validityInfo.startTimeInUTC = windowStart;
    response.validityInfo.durationHours = windowHours;
    locClientRespIndUnionType responseUnion = {};
    responseUnion.pGetPredictedOrbitsDataValidityInd = &response;
    pendingIndication = indication;
    callbacks.respIndCb(handle, indicationId, responseUnion, nullptr);
    pendingIndication = nullptr;
    return eLOC_CLIENT_SUCCESS;
}
