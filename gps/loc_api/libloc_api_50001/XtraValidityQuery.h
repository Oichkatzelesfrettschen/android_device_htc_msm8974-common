#ifndef XTRA_VALIDITY_QUERY_H
#define XTRA_VALIDITY_QUERY_H

#include <stdint.h>

struct XtraValidity {
    bool known;
    int clientStatus;
    int modemStatus;
    uint64_t startUtc;
    uint16_t durationHours;
};

XtraValidity queryXtraValidity();

// True when the window the modem holds covers nowUtc.
bool xtraValidityCurrent(const XtraValidity& validity, uint64_t nowUtc);

#endif
