#ifndef FAKE_LOC_API_H
#define FAKE_LOC_API_H

enum FakeScenario {
    FAKE_SUCCESS,
    FAKE_REJECT_FINAL,
    FAKE_TRANSPORT_FAILURE,
    FAKE_PART_MISMATCH
};

struct FakeObservations {
    unsigned int opens;
    unsigned int closes;
    unsigned int requests;
    unsigned int invalidRequests;
    unsigned int callbacks;
};

extern "C" void fakeLocReset(FakeScenario scenario, const char* payload,
                              unsigned int length);
extern "C" FakeObservations fakeLocObservations();

#endif
