// L^ (lhat) -- how a unit's text is read.
//
// The file system, which is what a host without an opinion wants. A host that
// reads units out of an archive, or out of memory it built itself, copies
// this file and changes lhat_load_file -- or, for one program rather than all
// of them, calls lhat_program_set_loader.

#include "port.h"

#include <stdio.h>

char *lhat_load_file(const char *path, size_t *length)
{
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
