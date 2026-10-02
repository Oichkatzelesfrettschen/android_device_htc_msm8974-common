#ifndef FAKE_LOC_API_H
#define FAKE_LOC_API_H

#include <stdint.h>

enum FakeScenario {
    FAKE_WINDOW,
    FAKE_NO_WINDOW,
    FAKE_MODEM_FAILURE,
    FAKE_TRANSPORT_FAILURE
};

struct FakeObservations {
    unsigned int opens;
    unsigned int closes;
    unsigned int requests;
    unsigned int invalidRequests;
    unsigned int callbacks;
};

extern "C" void fakeLocReset(FakeScenario scenario, uint64_t startUtc,
                              uint16_t durationHours);
extern "C" FakeObservations fakeLocObservations();

#endif
