// L^ (lhat) -- how the adapter compares two spellings of one file
// (09 の 5.2 with D6).
//
// Without a host's path map, a breakpoint's file and a unit's source are both
// filesystem paths and are normalised before being compared. What is pinned
// here is the part that costs a syscall: a link is walked through, so a
// breakpoint set through one binds to the unit behind it.
//
// The link is made rather than assumed. On Windows that is a directory
// junction, which needs no elevation -- only *making* a symbolic link does,
// and a junction is a reparse point just the same, which is exactly what
// _fullpath fails to walk and GetFinalPathNameByHandle walks.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adapter.h"

#include "testutil.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define remove_dir(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define remove_dir(p) rmdir(p)
#endif

// A directory `link` that stands for `target`. False when the platform will
// not make one, which the caller reports rather than fails on.
static bool link_directory(const char *link, const char *target)
{
#ifdef _WIN32
    // A junction is a reparse point whose data is a substitute name in the
    // "\??\" device form plus a print name, each counted in bytes and each
    // NUL-terminated, with the print name laid after the substitute one.
    wchar_t given[MAX_PATH];
    wchar_t wide_target[MAX_PATH];
    wchar_t wide_link[MAX_PATH];
    if (MultiByteToWideChar(CP_ACP, 0, target, -1, given, MAX_PATH) == 0 ||
        MultiByteToWideChar(CP_ACP, 0, link, -1, wide_link, MAX_PATH) == 0) {
        return false;
    }
    // A junction names its target absolutely, in the object manager's form.
    // A relative name makes one the filesystem accepts and then cannot walk
    // through, which looks like a link that resolves to nothing.
    if (GetFullPathNameW(given, MAX_PATH, wide_target, NULL) == 0 ||
        !CreateDirectoryW(wide_link, NULL)) {
        return false;
    }

    wchar_t device[MAX_PATH + 8];
    _snwprintf(device, MAX_PATH + 8, L"\\??\\%s", wide_target);
    size_t substitute = wcslen(device) * sizeof(wchar_t);
    size_t print = wcslen(wide_target) * sizeof(wchar_t);

    // REPARSE_DATA_BUFFER's mount-point arm, built by hand: the SDK's own
    // declaration lives in ntifs.h, which is not on a user-mode include path.
    struct {
        DWORD tag;
        WORD length;
        WORD reserved;
        WORD substitute_offset;
        WORD substitute_length;
        WORD print_offset;
        WORD print_length;
        wchar_t names[MAX_PATH * 4];
    } buffer;
    memset(&buffer, 0, sizeof buffer);
    buffer.tag = IO_REPARSE_TAG_MOUNT_POINT;
    buffer.substitute_offset = 0;
    buffer.substitute_length = (WORD)substitute;
    buffer.print_offset = (WORD)(substitute + sizeof(wchar_t));
    buffer.print_length = (WORD)print;
    memcpy(buffer.names, device, substitute + sizeof(wchar_t));
    memcpy((char *)buffer.names + buffer.print_offset, wide_target,
           print + sizeof(wchar_t));
    // Past the four WORDs of the arm's own header, both names and both
    // terminators.
    buffer.length =
        (WORD)(8 + substitute + sizeof(wchar_t) + print + sizeof(wchar_t));

    HANDLE handle = CreateFileW(wide_link, GENERIC_WRITE, 0, NULL,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS |
                                    FILE_FLAG_OPEN_REPARSE_POINT,
                                NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(wide_link);
        return false;
    }
    DWORD wrote = 0;
    BOOL ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &buffer,
                              (DWORD)(buffer.length + 8), NULL, 0, &wrote,
                              NULL);
    CloseHandle(handle);
    if (!ok) {
        RemoveDirectoryW(wide_link);
        return false;
    }
    return true;
#else
    return symlink(target, link) == 0;
#endif
}

static void unlink_directory(const char *link)
{
#ifdef _WIN32
    wchar_t wide[MAX_PATH];
    if (MultiByteToWideChar(CP_ACP, 0, link, -1, wide, MAX_PATH) != 0) {
        RemoveDirectoryW(wide);  // a junction is removed as the directory it is
    }
#else
    unlink(link);
#endif
}

static bool make_dir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0777) == 0;
#endif
}

// A name nothing else in this run uses, under the directory ctest starts in.
static void temp_name(char *out, size_t capacity, const char *what)
{
    snprintf(out, capacity, "lhat_dap_path_%s_%lu", what,
             (unsigned long)
#ifdef _WIN32
                 GetCurrentProcessId()
#else
                 getpid()
#endif
    );
}

static void expect_same(const char *left, const char *right, const char *why)
{
    char *a = dap_normalize_path(left);
    char *b = dap_normalize_path(right);
    LHAT_CHECK(a != NULL && b != NULL, "%s: expected both to normalize", why);
    if (a != NULL && b != NULL) {
#ifdef _WIN32
        bool same = _stricmp(a, b) == 0;
#else
        bool same = strcmp(a, b) == 0;
#endif
        LHAT_CHECK(same, "%s: \"%s\" and \"%s\" are one file but normalize to "
                         "\"%s\" and \"%s\"",
                   why, left, right, a, b);
    }
    free(a);
    free(b);
}

static void test_the_spelling_alone(void)
{
    LHAT_TEST("09 の 5.2: what a spelling can settle is settled without a disk");
    {
        // The same file reached the long way round. Neither exists, so this
        // is the fallback path -- and it still has to agree with itself.
        expect_same("nowhere/at/all.lh", "nowhere/of/../at/all.lh",
                    "'..' takes the step it undoes back");
        expect_same("nowhere/./a.lh", "nowhere/a.lh", "'.' is no step");
    }

    LHAT_TEST("and a path that names nothing still answers");
    {
        char *answer = dap_normalize_path("no_such_file_at_all.lh");
        LHAT_CHECK(answer != NULL, "expected a spelling back");
        LHAT_CHECK(answer == NULL || strstr(answer, "no_such_file_at_all") != NULL,
                   "expected the name to survive, got \"%s\"", answer);
        free(answer);
    }

    LHAT_TEST("and nothing answers nothing");
    LHAT_CHECK(dap_normalize_path(NULL) == NULL, "expected NULL for NULL");
}

static void test_through_a_link(void)
{
    char real[128];
    char link[128];
    temp_name(real, sizeof real, "real");
    temp_name(link, sizeof link, "link");
    unlink_directory(link);
    remove_dir(real);

    LHAT_TEST("D6: a file reached through a link is the file behind it");
    if (!make_dir(real)) {
        LHAT_CHECK(false, "could not make a directory to link to");
        return;
    }
    char inside[256];
    snprintf(inside, sizeof inside, "%s/a.lh", real);
    FILE *file = fopen(inside, "wb");
    if (file != NULL) {
        fputs("let^ a = 1\n", file);
        fclose(file);
    }

    if (!link_directory(link, real)) {
        // Nothing was made, so nothing is asserted -- but say so rather than
        // pass quietly, since the platform's refusal is the whole reason this
        // test could ever look green without testing anything.
        LHAT_CHECK(file != NULL, "expected the file to be written");
        printf("  (skipped: this platform would not make a link)\n");
        remove(inside);
        remove_dir(real);
        return;
    }

    char through[256];
    snprintf(through, sizeof through, "%s/a.lh", link);
    expect_same(through, inside, "a link and the directory behind it");

    unlink_directory(link);
    remove(inside);
    remove_dir(real);
}

int main(void)
{
    test_the_spelling_alone();
    test_through_a_link();
    return lhat_test_report("test_dap_path");
}
