#include "mcp_common.h"

/* Standalone byte-preservation regression; runnable on Linux or Windows.
   Build with function sections/GC so unrelated server globals aren't needed. */
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL at line %d: %s (errno=%d)\n", \
                __LINE__, #condition, errno); \
        goto cleanup; \
    } \
} while (0)

int main(void) {
    const size_t length = 2 * 64 * 1024 + 1;
    unsigned char *expected = malloc(length);
    unsigned char *actual = malloc(length);
    char source[] = "mcp-copy-test-XXXXXX";
    char destination[sizeof(source) + 8] = {0};
    int fd = -1, created = 0, result = 1;
    FILE *copied = NULL;
#ifdef _WIN32
    /* Reproduce the Windows CRT default even if the user's default differs. */
    int previous_mode = _fmode;
    _fmode = _O_TEXT;
#endif

    CHECK(expected && actual);
    for (size_t i = 0; i < length; i++) expected[i] = (unsigned char)i;
    expected[7] = '\r';
    expected[8] = '\n';
    expected[9] = 0x1a;
    expected[64 * 1024 - 1] = '\r';
    expected[64 * 1024] = '\n';

    fd = mkstemp(source);
    CHECK(fd >= 0);
    created = 1;
#ifdef _WIN32
    CHECK(_setmode(fd, _O_BINARY) != -1);
#endif
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = write(fd, expected + offset, length - offset);
        CHECK(written > 0);
        offset += (size_t)written;
    }
    int closed = close(fd);
    fd = -1;
    CHECK(closed == 0);
    snprintf(destination, sizeof(destination), "%s.copy", source);

    CHECK(atomic_copy_file(source, destination) == 0);
    copied = fopen(destination, "rb");
    CHECK(copied != NULL);
    CHECK(fread(actual, 1, length, copied) == length);
    CHECK(fgetc(copied) == EOF && !ferror(copied));
    CHECK(memcmp(expected, actual, length) == 0);
    CHECK(atomic_copy_file(source, destination) == -1 && errno == EEXIST);
    result = 0;
    puts("PASS: binary copy preserves every byte, CRLF, Ctrl-Z and buffer boundaries");

cleanup:
    if (copied) fclose(copied);
    if (fd >= 0) close(fd);
    if (destination[0]) remove(destination);
    if (created) remove(source);
    free(expected);
    free(actual);
#ifdef _WIN32
    _fmode = previous_mode;
#endif
    return result;
}
