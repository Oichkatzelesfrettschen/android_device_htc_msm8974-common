#include "../XtraQmiInjector.h"
#include "fake_loc_api.h"
#include "loc_api_v02_client.h"

#include <climits>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* description)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++failures;
    }
}

void checkScenario(FakeScenario scenario, const std::vector<char>& data,
                   bool expectedAccepted, int expectedClientStatus,
                   int expectedModemStatus, unsigned int expectedPart,
                   unsigned int expectedRequests)
{
    fakeLocReset(scenario, data.data(), data.size());
    const XtraInjectionResult result =
            injectXtraWithModemStatus(data.data(), data.size());
    const FakeObservations observed = fakeLocObservations();
    expect(result.accepted == expectedAccepted, "acceptance");
    expect(result.clientStatus == expectedClientStatus, "client status");
    expect(result.modemStatus == expectedModemStatus, "modem status");
    expect(result.partNumber == expectedPart, "last attempted part");
    expect(observed.opens == 1 && observed.closes == 1, "client lifecycle");
    expect(observed.requests == expectedRequests, "request count");
    expect(observed.callbacks == expectedRequests -
            (scenario == FAKE_TRANSPORT_FAILURE ? 1U : 0U),
           "indication callback count");
    expect(observed.invalidRequests == 0, "request metadata and payload");
}

}  // namespace

int main()
{
    std::vector<char> data(2057);
    for (size_t index = 0; index < data.size(); ++index)
        data[index] = static_cast<char>(index * 31U + 7U);

    checkScenario(FAKE_SUCCESS, data, true, eLOC_CLIENT_SUCCESS,
                  eQMI_LOC_SUCCESS_V02, 3, 3);
    checkScenario(FAKE_REJECT_FINAL, data, false, eLOC_CLIENT_SUCCESS,
                  eQMI_LOC_INVALID_PARAMETER_V02, 3, 3);
    checkScenario(FAKE_TRANSPORT_FAILURE, data, false,
                  eLOC_CLIENT_FAILURE_TIMEOUT, eQMI_LOC_SUCCESS_V02, 2, 2);
    checkScenario(FAKE_PART_MISMATCH, data, false, eLOC_CLIENT_SUCCESS,
                  eQMI_LOC_SUCCESS_V02, 2, 2);

    fakeLocReset(FAKE_SUCCESS, data.data(), data.size());
    expect(!injectXtraWithModemStatus(nullptr, data.size()).accepted,
           "null payload rejected");
    expect(!injectXtraWithModemStatus(data.data(), 0).accepted,
           "empty payload rejected");
    expect(!injectXtraWithModemStatus(data.data(),
            static_cast<int>(USHRT_MAX *
                    QMI_LOC_MAX_PREDICTED_ORBITS_PART_LEN_V02 + 1U)).accepted,
           "more than 65535 parts rejected before payload access");
    const FakeObservations invalidInputObserved = fakeLocObservations();
    expect(invalidInputObserved.opens == 0 && invalidInputObserved.requests == 0,
           "invalid inputs do not open a QMI client");

    if (failures != 0) {
        std::fprintf(stderr, "%d XTRA QMI injector tests failed\n", failures);
        return 1;
    }
    std::puts("XTRA QMI multipart, rejection, transport, mismatch, and bounds passed");
    return 0;
}
