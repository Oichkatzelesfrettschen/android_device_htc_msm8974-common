#include "../XtraFormatGuard.h"

#include <stdio.h>

static int failures = 0;

static void expectStatus(const char* description, const void* data, size_t length,
                         XtraFormatStatus expected)
{
    const XtraFormatStatus actual = classifyXtraFormat(data, length);
    if (actual != expected) {
        fprintf(stderr, "%s: expected %d, got %d\n", description,
                static_cast<int>(expected), static_cast<int>(actual));
        ++failures;
    }
}

static void testHeaders()
{
    uint8_t header[16] = {
        0x01, 0x34, 0x08, 0x0a, 0x00, 0x1a, 0x02,
        0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    expectStatus("null", NULL, sizeof(header), XTRA_FORMAT_INVALID);
    for (size_t length = 0; length < sizeof(header); ++length) {
        expectStatus("truncated", header, length, XTRA_FORMAT_INVALID);
    }
    expectStatus("generation 2", header, sizeof(header), XTRA_FORMAT_SUPPORTED);
    header[1] = 0x32;
    header[6] = 0x03;
    expectStatus("generation 3", header, sizeof(header), XTRA_FORMAT_SUPPORTED);
    header[1] = 0x1b;
    header[6] = 0x01;
    expectStatus("generation 1", header, sizeof(header), XTRA_FORMAT_GENERATION_1);
    header[6] = 0x02;
    expectStatus("subtype mismatch", header, sizeof(header), XTRA_FORMAT_INVALID);
    header[1] = 0x34;
    header[6] = 0x04;
    expectStatus("unknown generation", header, sizeof(header), XTRA_FORMAT_INVALID);
    header[6] = 0x02;
    header[0] = 0x00;
    expectStatus("bad major", header, sizeof(header), XTRA_FORMAT_INVALID);
    header[0] = 0x01;
    const uint8_t signature[4] = {0x08, 0x0a, 0x00, 0x1a};
    for (size_t offset = 0; offset < sizeof(signature); ++offset) {
        header[offset + 2] = static_cast<uint8_t>(signature[offset] ^ 0xff);
        expectStatus("bad signature", header, sizeof(header), XTRA_FORMAT_INVALID);
        header[offset + 2] = signature[offset];
    }
}

struct CorpusCase {
    const char* filename;
    XtraFormatStatus expected;
};

static void testCorpus(const char* directory)
{
    const CorpusCase cases[] = {
        {"x1-xtra.bin", XTRA_FORMAT_GENERATION_1},
        {"x2-xtra.bin", XTRA_FORMAT_GENERATION_1},
        {"x3-xtra.bin", XTRA_FORMAT_GENERATION_1},
        {"x1-xtra2.bin", XTRA_FORMAT_SUPPORTED},
        {"x2-xtra2.bin", XTRA_FORMAT_SUPPORTED},
        {"x3-xtra2.bin", XTRA_FORMAT_SUPPORTED},
        {"x1-xtra3grc.bin", XTRA_FORMAT_SUPPORTED},
        {"x2-xtra3grc.bin", XTRA_FORMAT_SUPPORTED},
        {"x3-xtra3grc.bin", XTRA_FORMAT_SUPPORTED}
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        char path[1024];
        const int written = snprintf(path, sizeof(path), "%s/%s", directory,
                                     cases[index].filename);
        if (written < 0 || static_cast<size_t>(written) >= sizeof(path)) {
            fprintf(stderr, "corpus path too long: %s\n", cases[index].filename);
            ++failures;
            continue;
        }
        FILE* file = fopen(path, "rb");
        if (file == NULL) {
            perror(path);
            ++failures;
            continue;
        }
        uint8_t header[16];
        const size_t length = fread(header, 1, sizeof(header), file);
        fclose(file);
        expectStatus(cases[index].filename, header, length, cases[index].expected);
    }
}

int main(int argc, char** argv)
{
    testHeaders();
    if (argc != 2) {
        fprintf(stderr, "usage: %s XTRA_CORPUS_DIRECTORY\n", argv[0]);
        return 2;
    }
    testCorpus(argv[1]);
    if (failures != 0) {
        fprintf(stderr, "%d XTRA format tests failed\n", failures);
        return 1;
    }
    puts("XTRA format header and nine retained corpus cases passed");
    return 0;
}
