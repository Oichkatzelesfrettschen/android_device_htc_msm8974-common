#include "fake_loc_api.h"

#include <cstring>

#include "loc_api_v02_client.h"

namespace {

FakeScenario scenario = FAKE_SUCCESS;
FakeObservations observations = {};
const char* expectedPayload = nullptr;
unsigned int expectedLength = 0;
locClientCallbacksType callbacks = {};
void* pendingIndication = nullptr;
char clientHandle;

}  // namespace

extern "C" void fakeLocReset(FakeScenario selectedScenario, const char* payload,
                              unsigned int length)
{
    scenario = selectedScenario;
    observations = {};
    expectedPayload = payload;
    expectedLength = length;
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
        indicationId != QMI_LOC_INJECT_PREDICTED_ORBITS_DATA_IND_V02 ||
        pendingIndication == nullptr || indication == nullptr)
        ++observations.invalidRequests;
    else
        *static_cast<qmiLocInjectPredictedOrbitsDataIndMsgT_v02*>(pendingIndication) =
                *static_cast<qmiLocInjectPredictedOrbitsDataIndMsgT_v02*>(indication);
}

extern "C" locClientStatusEnumType loc_sync_send_req(
        locClientHandleType handle, uint32_t requestId, locClientReqUnionType requestUnion,
        uint32_t timeoutMs, uint32_t indicationId, void* indication)
{
    ++observations.requests;
    const qmiLocInjectPredictedOrbitsDataReqMsgT_v02* request =
            requestUnion.pInjectPredictedOrbitsDataReq;
    if (handle != &clientHandle ||
        requestId != QMI_LOC_INJECT_PREDICTED_ORBITS_DATA_REQ_V02 ||
        indicationId != QMI_LOC_INJECT_PREDICTED_ORBITS_DATA_IND_V02 ||
        timeoutMs != 4000 || request == nullptr || indication == nullptr) {
        ++observations.invalidRequests;
        return eLOC_CLIENT_FAILURE_INVALID_PARAMETER;
    }
    const unsigned int partSize = QMI_LOC_MAX_PREDICTED_ORBITS_PART_LEN_V02;
    const unsigned int expectedParts = (expectedLength + partSize - 1) / partSize;
    const unsigned int offset = (observations.requests - 1) * partSize;
    const unsigned int expectedPartLength =
            expectedLength - offset < partSize ? expectedLength - offset : partSize;
    if (request->totalSize != expectedLength ||
        request->totalParts != expectedParts ||
        request->partNum != observations.requests ||
        request->partData_len != expectedPartLength ||
        request->formatType_valid != 1 ||
        request->formatType != eQMI_LOC_PREDICTED_ORBITS_XTRA_V02 ||
        std::memcmp(request->partData, expectedPayload + offset, expectedPartLength) != 0)
        ++observations.invalidRequests;

    if (scenario == FAKE_TRANSPORT_FAILURE && observations.requests == 2)
        return eLOC_CLIENT_FAILURE_TIMEOUT;

    qmiLocInjectPredictedOrbitsDataIndMsgT_v02 response = {};
    response.status = scenario == FAKE_REJECT_FINAL &&
                      observations.requests == expectedParts
            ? eQMI_LOC_INVALID_PARAMETER_V02 : eQMI_LOC_SUCCESS_V02;
    response.partNum_valid = 1;
    response.partNum = scenario == FAKE_PART_MISMATCH && observations.requests == 2
            ? request->partNum + 1 : request->partNum;
    locClientRespIndUnionType responseUnion = {};
    responseUnion.pInjectPredictedOrbitsDataInd = &response;
    pendingIndication = indication;
    callbacks.respIndCb(handle, indicationId, responseUnion, nullptr);
    pendingIndication = nullptr;
    return eLOC_CLIENT_SUCCESS;
}
