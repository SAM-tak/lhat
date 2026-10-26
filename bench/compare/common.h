#ifndef COMPARE_COMMON_H
#define COMPARE_COMMON_H

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <time.h>
#include <sys/resource.h>
#endif

typedef struct {
    const char *path;
    int rounds, size, seed, warmups, samples;
    double expected, abs_tolerance, rel_tolerance;
} CompareArgs;

static double now_ns(void)
{
#ifdef _WIN32
    LARGE_INTEGER ticks, frequency;
    QueryPerformanceCounter(&ticks);
    QueryPerformanceFrequency(&frequency);
    return (double)ticks.QuadPart * (1e9 / (double)frequency.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
#endif
}

static int parse_integer(const char *text, int minimum, int maximum, int *out)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < minimum || value > maximum) return 0;
    *out = (int)value;
    return 1;
}

static int parse_real(const char *text, double *out)
{
    char *end;
    errno = 0;
    *out = strtod(text, &end);
    return !errno && end != text && !*end && isfinite(*out);
}

static int parse_args(int argc, char **argv, CompareArgs *args)
{
    if (argc != 11 ||
        !parse_integer(argv[2], 1, 1000000000, &args->rounds) ||
        !parse_integer(argv[3], 1, 1000000, &args->size) ||
        !parse_integer(argv[4], 0, 1000000, &args->seed) ||
        !parse_integer(argv[5], 0, 1000, &args->warmups) ||
        !parse_integer(argv[6], 1, 10000, &args->samples) ||
        !parse_real(argv[7], &args->expected) ||
        !parse_real(argv[8], &args->abs_tolerance) || args->abs_tolerance < 0 ||
        !parse_real(argv[9], &args->rel_tolerance) || args->rel_tolerance < 0 ||
        strcmp(argv[10], "v1") != 0) {
        fprintf(stderr, "usage: runner source rounds size seed warmups samples expected abs_tol rel_tol v1\n");
        return 0;
    }
    args->path = argv[1];
    return 1;
}

static int valid_result(const CompareArgs *args, double value)
{
    if (isfinite(value) && fabs(value - args->expected) <=
        args->abs_tolerance + args->rel_tolerance * fabs(args->expected)) return 1;
    fprintf(stderr, "checksum mismatch: got %.17g, expected %.17g\n", value, args->expected);
    return 0;
}

static size_t peak_process_bytes(void)
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof info))
        return (size_t)info.PeakWorkingSetSize;
    return 0;
#else
    struct rusage info;
    if (getrusage(RUSAGE_SELF, &info) != 0) return 0;
#ifdef __APPLE__
    return (size_t)info.ru_maxrss;
#else
    return (size_t)info.ru_maxrss * 1024;
#endif
#endif
}

#endif
