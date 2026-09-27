#ifndef TEST_LOG_LOG_H
#define TEST_LOG_LOG_H

#include <stdio.h>

#define ALOGE(...) do { if (false) fprintf(stderr, __VA_ARGS__); } while (0)

#endif
