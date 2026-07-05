#include "io_csv.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static void mkparents(const char *path) {
    char buf[1024];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *slash = strrchr(buf, '/');
    if (!slash) {
        return;
    }
    *slash = '\0';
    /* create intermediate directories */
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0777);
            *p = '/';
        }
    }
    mkdir(buf, 0777);
}

static void write_field(FILE *f, const char *s) {
    if (s == NULL) {
        return; /* empty cell */
    }
    int needs_quote = 0;
    for (const char *p = s; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            needs_quote = 1;
            break;
        }
    }
    if (!needs_quote) {
        fputs(s, f);
        return;
    }
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        if (*p == '"') {
            fputc('"', f);
        }
        fputc(*p, f);
    }
    fputc('"', f);
}

int csv_write(const char *path,
              const char *const *headers, long ncols,
              const char *const *cells, long nrows) {
    mkparents(path);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return 1;
    }

    for (long c = 0; c < ncols; c++) {
        if (c) {
            fputc(',', f);
        }
        write_field(f, headers[c]);
    }
    fputs("\r\n", f);

    for (long r = 0; r < nrows; r++) {
        for (long c = 0; c < ncols; c++) {
            if (c) {
                fputc(',', f);
            }
            write_field(f, cells[r * ncols + c]);
        }
        fputs("\r\n", f);
    }

    fclose(f);
    return 0;
}
