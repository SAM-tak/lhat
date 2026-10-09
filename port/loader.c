// L^ (lhat) -- how a unit's text is read.
//
// The file system, for a host that wants one. **A program is not given this
// unless it is handed over**: lhat_program_init takes the loader, and nothing
// embedded reaches a file system without having been told to.
//
// A host reading units from an archive, or out of memory it built itself,
// writes its own of this shape and hands that over instead.

#include "lhat/port.h"

#include <errno.h>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// UTF-8 to a lhat_alloc'd UTF-16 string, or NULL when it is not UTF-8.
static wchar_t *widen(const char *text)
{
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *wide = length > 0 ? (wchar_t *)lhat_alloc((size_t)length * sizeof *wide) : NULL;
    if (wide != NULL) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length);
    return wide;
}
#endif

FILE *lhat_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t *wide_path = widen(path);
    wchar_t *wide_mode = widen(mode);
    FILE *file = NULL;
    if (wide_path == NULL || wide_mode == NULL) errno = EINVAL;
    else file = _wfopen(wide_path, wide_mode);
    lhat_free(wide_path);
    lhat_free(wide_mode);
    return file;
#else
    return fopen(path, mode);
#endif
}

char *lhat_load_file(void *context, const char *path, size_t *length)
{
    (void)context;

    FILE *file = lhat_fopen(path, "rb");
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
