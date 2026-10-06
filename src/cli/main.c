#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "convert.h"
#include "util/threads.h"
#include "util/platform.h"

#include <sys/stat.h>
#if defined(_WIN32) && !defined(IMG2JXL_FORCE_POSIX)
#include <windows.h>
#define IMG_PATH_MAX MAX_PATH
#define IMG_STRDUP _strdup
#define img_stricmp _stricmp
#else
#include <dirent.h>
#include <limits.h>
#include <sys/types.h>
#define IMG_PATH_MAX PATH_MAX
#define IMG_STRDUP strdup
#define img_stricmp strcasecmp
#endif

typedef struct {
    char **items;
    int count;
    int cap;
} strlist_t;

static void sl_append(strlist_t *l, const char *s)
{
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->items = (char **)realloc(l->items, (size_t)l->cap * sizeof(char *));
        if (!l->items) {
            fprintf(stderr, "img2jxl: out of memory\n");
            exit(1);
        }
    }
    l->items[l->count++] = IMG_STRDUP(s);
}

static void sl_free(strlist_t *l)
{
    for (int i = 0; i < l->count; i++)
        free(l->items[i]);
    free(l->items);
}

static int is_dir_sep(char c)
{
    return c == '/' || c == '\\';
}

static const char *base_name(const char *path)
{
    const char *s1 = strrchr(path, '/'), *s2 = strrchr(path, '\\');
    const char *sep = s1 > s2 ? s1 : s2;
    return sep ? sep + 1 : path;
}

/* Collect supported images from a directory, recursively. */
#if defined(_WIN32) && !defined(IMG2JXL_FORCE_POSIX)
static void scan_dir(const char *dir, strlist_t *out)
{
    char pattern[IMG_PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        char full[IMG_PATH_MAX];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            scan_dir(full, out);
        else if (img2jxl_is_supported_file(full))
            sl_append(out, full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#else
static void scan_dir(const char *dir, strlist_t *out)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char full[IMG_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            scan_dir(full, out);
        else if (img2jxl_is_supported_file(full))
            sl_append(out, full);
    }
    closedir(d);
}
#endif

static void add_input(const char *path, strlist_t *out)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        scan_dir(path, out);
    else
        sl_append(out, path);
}

typedef struct {
    char in_path[IMG_PATH_MAX];
    char out_path[IMG_PATH_MAX];
    int done;
    img2jxl_result_t result;
} job_t;

typedef struct {
    job_t *job;
    const jxl_opts_t *opts;
    int keep_time;
} job_arg_t;

static void job_run(void *arg)
{
    job_arg_t *a = (job_arg_t *)arg;
    job_t *j = a->job;
    img2jxl_convert(j->in_path, j->out_path, a->opts, a->keep_time, &j->result);
    j->done = 1;
}

static void usage(void)
{
    fprintf(stderr,
        "img2jxl - convert BMP/TGA/PNM/ICO/JPEG/PNG/GIF/QOI/WebP/TIFF images to JPEG XL (lossless)\n\n"
        "usage: img2jxl [options] file-or-folder...\n"
        "  -o PATH        output file (single input) or output directory (batch)\n"
        "  -e 1-10        encode effort (1=lightning .. 10=glacier, default 10)\n"
        "  --no-modular   let the encoder choose VarDCT/Modular per frame (default: Modular)\n"
        "  -j N           parallel worker threads (default: CPU count)\n"
        "  --auto         optimize: drop useless alpha / detect gray\n"
        "  --no-keep-time do not copy creation/modification times from input\n"
        "  -h             this help\n\n"
        "folders are scanned recursively; JXL inputs are decoded and re-encoded\n"
        "with the chosen settings (in place without -o)\n"
        "default: effort 10, Modular mode, distance 0 (lossless), timestamps kept,\n"
        "bit depth matched to source\n");
}

static void make_out_name(const char *in, char *out, size_t outlen)
{
    const char *base = base_name(in);
    const char *dot = strrchr(base, '.');
    if (dot && dot != base)
        snprintf(out, outlen, "%.*s.jxl", (int)(dot - in), in);
    else
        snprintf(out, outlen, "%s.jxl", in);
}

static int out_path_is_jxl(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *base = base_name(path);
    return dot && dot >= base && !img_stricmp(dot + 1, "jxl");
}

int main(int argc, char **argv)
{
    jxl_opts_t opts;
    opts.effort = 10;
    opts.modular = 1;
    opts.auto_optimize = 0;
    int keep_time = 1;
    int nthreads = img_num_cpus();
    const char *out_spec = NULL;
    strlist_t inputs = {0};

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        } else if (!strcmp(a, "-o") && i + 1 < argc) {
            out_spec = argv[++i];
        } else if (!strcmp(a, "-e") && i + 1 < argc) {
            opts.effort = atoi(argv[++i]);
            if (opts.effort < 1 || opts.effort > 10) {
                fprintf(stderr, "img2jxl: -e must be 1-10\n");
                return 2;
            }
        } else if (!strcmp(a, "-m") || !strcmp(a, "--modular")) {
            opts.modular = 1;
        } else if (!strcmp(a, "--no-modular")) {
            opts.modular = 0;
        } else if (!strcmp(a, "-j") && i + 1 < argc) {
            nthreads = atoi(argv[++i]);
            if (nthreads < 1 || nthreads > 256) {
                fprintf(stderr, "img2jxl: -j must be 1-256\n");
                return 2;
            }
        } else if (!strcmp(a, "--auto")) {
            opts.auto_optimize = 1;
        } else if (!strcmp(a, "--keep-time")) {
            keep_time = 1;
        } else if (!strcmp(a, "--no-keep-time")) {
            keep_time = 0;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "img2jxl: unknown option '%s'\n", a);
            usage();
            return 2;
        } else {
            add_input(a, &inputs);
        }
    }

    if (inputs.count == 0) {
        usage();
        return 2;
    }

    int nfiles = inputs.count;

    /* resolve output paths: explicit .jxl = single output, otherwise a dir;
     * no -o at all = next to each input */
    char out_dir[IMG_PATH_MAX] = "";
    char single_out[IMG_PATH_MAX] = "";
    int out_is_dir = 0;
    if (out_spec) {
        if (nfiles == 1 && out_path_is_jxl(out_spec)) {
            snprintf(single_out, sizeof(single_out), "%s", out_spec);
        } else {
            snprintf(out_dir, sizeof(out_dir), "%s", out_spec);
            img_mkdir(out_dir);                 /* ok if it already exists */
            out_is_dir = 1;
        }
    }

    job_t *jobs = (job_t *)calloc((size_t)nfiles, sizeof(job_t));
    job_arg_t *args = (job_arg_t *)calloc((size_t)nfiles, sizeof(job_arg_t));
    if (!jobs || !args) {
        fprintf(stderr, "img2jxl: out of memory\n");
        return 1;
    }

    for (int i = 0; i < nfiles; i++) {
        job_t *j = &jobs[i];
        snprintf(j->in_path, sizeof(j->in_path), "%s", inputs.items[i]);
        if (out_is_dir) {
            snprintf(j->out_path, sizeof(j->out_path), "%s/%s", out_dir,
                     base_name(inputs.items[i]));
            char *dot = strrchr(j->out_path, '.');
            if (dot && !is_dir_sep(dot[-1]))
                snprintf(dot, sizeof(j->out_path) - (size_t)(dot - j->out_path), ".jxl");
            else
                snprintf(j->out_path + strlen(j->out_path),
                         sizeof(j->out_path) - strlen(j->out_path), ".jxl");
        } else if (single_out[0]) {
            snprintf(j->out_path, sizeof(j->out_path), "%s", single_out);
        } else {
            make_out_name(inputs.items[i], j->out_path, sizeof(j->out_path));
        }
        args[i].job = j;
        args[i].opts = &opts;
        args[i].keep_time = keep_time;
    }

    thread_pool_t *pool = pool_create(nthreads);
    if (!pool) {
        fprintf(stderr, "img2jxl: failed to create thread pool\n");
        return 1;
    }
    for (int i = 0; i < nfiles; i++)
        pool_submit(pool, job_run, &args[i]);

    /* report in input order as soon as each job finishes */
    int failures = 0;
    for (int i = 0; i < nfiles; i++) {
        while (!jobs[i].done)
            img_msleep(5);
        job_t *j = &jobs[i];
        img2jxl_result_t *r = &j->result;
        if (r->ok) {
            const int same = !strcmp(j->in_path, j->out_path);
            char frbuf[32] = "";
            if (r->out_frames > 1)
                snprintf(frbuf, sizeof(frbuf), " x%d frames", r->out_frames);
            printf("[ ok ] %s (%s %d-bit %dx%d%s) -> %s  %llu -> %llu bytes, %.2fs\n",
                   j->in_path, img2jxl_color_name(r->out_color), r->out_depth,
                   r->out_w, r->out_h, frbuf, same ? "(in place)" : j->out_path,
                   r->in_size, r->out_size, r->secs);
        } else {
            printf("[FAIL] %s: %s\n", j->in_path, r->err);
            failures++;
        }
        fflush(stdout);
    }
    pool_wait(pool);
    pool_destroy(pool);
    sl_free(&inputs);
    free(jobs);
    free(args);
    return failures ? 1 : 0;
}
