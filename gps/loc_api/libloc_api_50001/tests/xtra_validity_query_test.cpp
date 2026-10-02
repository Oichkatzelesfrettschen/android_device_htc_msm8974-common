#include "../XtraValidityQuery.h"
#include "fake_loc_api.h"
#include "loc_api_v02_client.h"

#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char* description)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++failures;
    }
}

const uint64_t kGpsEpoch = 315964800u;
const uint64_t kStart = 1790485200u;

void checkQuery(FakeScenario scenario, uint64_t start, uint16_t hours,
                bool expectedKnown, int expectedClient, int expectedModem,
                unsigned int expectedCallbacks)
{
    fakeLocReset(scenario, start, hours);
    const XtraValidity validity = queryXtraValidity();
    const FakeObservations observed = fakeLocObservations();
    expect(validity.answered == (expectedClient == eLOC_CLIENT_SUCCESS), "answered");
    expect(validity.known == expectedKnown, "known");
    expect(validity.clientStatus == expectedClient, "client status");
    expect(validity.modemStatus == expectedModem, "modem status");
    if (expectedKnown) {
        expect(validity.startUtc == start, "window start");
        expect(validity.durationHours == hours, "window hours");
    }
    expect(observed.opens == 1 && observed.closes == 1, "client lifecycle");
    expect(observed.requests == 1, "one validity request");
    expect(observed.callbacks == expectedCallbacks, "indication callbacks");
    expect(observed.invalidRequests == 0, "request shape");
}

}  // namespace

int main()
{
    checkQuery(FAKE_WINDOW, kStart, 168, true, eLOC_CLIENT_SUCCESS,
               eQMI_LOC_SUCCESS_V02, 1);
    checkQuery(FAKE_EPOCH_WINDOW, kGpsEpoch, 168, true, eLOC_CLIENT_SUCCESS,
               eQMI_LOC_SUCCESS_V02, 1);
    checkQuery(FAKE_MODEM_FAILURE, kStart, 168, false, eLOC_CLIENT_SUCCESS,
               eQMI_LOC_GENERAL_FAILURE_V02, 1);
    checkQuery(FAKE_TRANSPORT_FAILURE, kStart, 168, false,
               eLOC_CLIENT_FAILURE_TIMEOUT, -1, 0);

    XtraValidity window = {true, true, 0, 0, kStart, 168};
    expect(xtraValidityCurrent(window, kStart), "window start is current");
    expect(xtraValidityCurrent(window, kStart + 168u * 3600u - 1u),
           "last second is current");
    expect(!xtraValidityCurrent(window, kStart + 168u * 3600u),
           "window end is not current");
    expect(!xtraValidityCurrent(window, kStart - 1u), "before start is not current");
    XtraValidity epoch = {true, true, 0, 0, kGpsEpoch, 168};
    expect(!xtraValidityCurrent(epoch, kStart), "GPS-epoch default is not current");
    window.known = false;
    expect(!xtraValidityCurrent(window, kStart), "unknown window is not current");

    if (failures != 0) {
        std::fprintf(stderr, "%d XTRA validity query tests failed\n", failures);
        return 1;
    }
    std::puts("XTRA validity query, modem failure, transport and window bounds passed");
    return 0;
}
