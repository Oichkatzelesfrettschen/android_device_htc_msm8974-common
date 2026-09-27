#ifndef XTRA_QMI_INJECTOR_H
#define XTRA_QMI_INJECTOR_H

struct XtraInjectionResult {
    bool accepted;
    int clientStatus;
    int modemStatus;
    unsigned int partNumber;
};

XtraInjectionResult injectXtraWithModemStatus(const char* data, int length);

#endif
