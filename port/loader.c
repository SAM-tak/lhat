// L^ (lhat) -- how a unit's text is read.
//
// The file system, for a host that wants one. **A program is not given this
// unless it is handed over**: lhat_program_init takes the loader, and nothing
// embedded reaches a file system without having been told to.
//
// A host reading units from an archive, or out of memory it built itself,
// writes its own of this shape and hands that over instead.

#include "../src/port.h"

#include <stdio.h>

char *lhat_load_file(void *context, const char *path, size_t *length)
{
    (void)context;

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    // One past the end, so the text is NUL terminated whatever it holds.
    char *buffer = (char *)lhat_alloc((size_t)size + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }
    size_t read = fread(buffer, 1, (size_t)size, file);
    fclose(file);

    buffer[read] = '\0';
    *length = read;
    return buffer;
}
