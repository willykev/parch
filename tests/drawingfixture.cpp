// A stand-in drawing program. Its inputs and observation files live in its
// private working directory, and its drawing and diagnostics use the two pipes.
#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static void record(const char *name, const char *format, ...)
{
    FILE *file = fopen(name, "a");
    if (!file) return;
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fclose(file);
}

static int copy(const char *name)
{
    FILE *file = fopen(name, "rb");
    if (!file) return 0;
    char bytes[4096];
    size_t n;
    while ((n = fread(bytes, 1, sizeof bytes, file))) fwrite(bytes, 1, n, stdout);
    fclose(file);
    return 1;
}

static void environment(void)
{
    for (char **field = environ; *field; ++field) record("log.environ", "%s\n", *field);
    char *cwd = getcwd(NULL, 0);
    if (cwd) record("log.folder", "%s\n", cwd);
    free(cwd);
    // The parent inspects the running process's network namespace from outside the home.
    const struct timespec pause = {0, 10000000};
    for (int n = 0; n < 1000 && access("inspected", F_OK) != 0; ++n) nanosleep(&pause, NULL);
}

static int math(const char *formula, const char *ruled, int size, const char *color)
{
    if (strncmp(formula, "sleep", 5) == 0) {
        pid_t child = fork();
        if (child == 0) { sleep(60); _exit(0); }
        if (child < 0) return 1;
        record("log.child", "%ld\n", (long)child);
        waitpid(child, NULL, 0);
    } else if (strncmp(formula, "flood", 5) == 0) {
        char bytes[4096] = {0};
        for (int i = 0; i < 1000; ++i) fwrite(bytes, 1, sizeof bytes, stdout);
        return 0;
    } else if (strncmp(formula, "environ", 7) == 0) {
        environment();
    } else if (strncmp(formula, "refuse", 6) == 0) {
        fprintf(stderr, "ERR    1 %s — Parse error: fake refusal of %s\n", formula, formula);
        return 1;
    } else if (strncmp(formula, "crash", 5) == 0) {
        record("log", "end %ld\n", (long)getpid());
        raise(SIGSEGV);
    }
    if (!copy("formula.svg")) {
        size_t width = strlen(formula);
        int deep = 14 + 100 * size;
        printf("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %zu 20\" width=\"%zupt\" height=\"20pt\">"
               "<rect width=\"%zu\" height=\"14\" fill=\"%s\"/></svg>\n", width, width, width, color);
        printf("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %zu %d\" width=\"%zupt\" height=\"%dpt\"></svg>\n",
               width, deep, width, deep);
    }
    fprintf(stderr, "OK     1 %s\nOK     2 %s\n", formula, ruled);
    return 0;
}

static void limit(const char *name, int kind)
{
    struct rlimit values;
    if (getrlimit(kind, &values) == 0) printf("%s %llu %llu\n", name,
        (unsigned long long)values.rlim_cur, (unsigned long long)values.rlim_max);
}

int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (strcmp(name, "limits") == 0) {
        limit("Max address space", RLIMIT_AS);
        limit("Max cpu time", RLIMIT_CPU);
        limit("Max core file size", RLIMIT_CORE);
        limit("Max file size", RLIMIT_FSIZE);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) { puts("merman-cli 0.0.7"); return 0; }
    const char *format = "svg", *scale = "1", *color = "black";
    int size = 16;
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--format") == 0) format = argv[++i];
        else if (strcmp(argv[i], "--scale") == 0) scale = argv[++i];
        else if (strcmp(argv[i], "--color") == 0) color = argv[++i];
        else if (strcmp(argv[i], "--font-size") == 0) size = atoi(argv[++i]);
    }
    record("log", "start %ld\n", (long)getpid());
    char *line = NULL, *ruled = NULL;
    size_t capacity = 0, second = 0;
    if (getline(&line, &capacity, stdin) < 0) return 1;
    line[strcspn(line, "\n")] = 0;
    if (strncmp(line, "slow", 4) == 0) {
        const struct timespec pause = {0, 300000000};
        nanosleep(&pause, NULL);
    }
    int code = 0;
    if (strcmp(name, "render-svg") == 0) {
        if (getline(&ruled, &second, stdin) >= 0) ruled[strcspn(ruled, "\n")] = 0;
        code = math(line, ruled ? ruled : "", size, color);
    } else {
        record("log", "run %s %s\n", format, scale);
        if (strncmp(line, "refuse", 6) == 0) {
            fputs("Diagram parse error (flowchart-v2): fake refusal\n", stderr);
            code = 1;
        } else if (strcmp(format, "svg") == 0) {
            fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 40 20\" aria-roledescription=\"flowchart-v2\">"
                  "<text x=\"1\" y=\"12\">Fake label</text></svg>", stdout);
        } else if (!copy("picture.png")) code = 1;
    }
    free(ruled);
    free(line);
    record("log", "end %ld\n", (long)getpid());
    return code;
}
