/* debfetch: the MPC browser installer's downloader (the MPC has libcurl and OpenSSL, but no https wget or ar).
 *   debfetch get <url> <sha256> <file>   download to <file> unless it's already there with that SHA-256; check it
 *   debfetch data <file.deb> <dir>       unpack the package's files (its data.tar.*) into <dir>
 * libcurl, libcrypto are the MPC's own, opened at run time. Exit status 0 = done. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *lib(const char *name) {
    void *h = dlopen(name, RTLD_NOW);
    if (!h) { fprintf(stderr, "debfetch: can't load %s\n", name); exit(1); }
    return h;
}
#define SYM(h, n) (*(void **)&n = dlsym(h, #n), n ? (void)0 : (fprintf(stderr, "debfetch: no %s\n", #n), exit(1)))

static int sha256_ok(const char *path, const char *want) {
    static void *c;
    static void *(*EVP_MD_CTX_new)(void);
    static void (*EVP_MD_CTX_free)(void *);
    static const void *(*EVP_sha256)(void);
    static int (*EVP_DigestInit_ex)(void *, const void *, void *);
    static int (*EVP_DigestUpdate)(void *, const void *, size_t);
    static int (*EVP_DigestFinal_ex)(void *, unsigned char *, unsigned *);
    if (!c) {
        c = lib("libcrypto.so.3");
        SYM(c, EVP_MD_CTX_new); SYM(c, EVP_MD_CTX_free); SYM(c, EVP_sha256);
        SYM(c, EVP_DigestInit_ex); SYM(c, EVP_DigestUpdate); SYM(c, EVP_DigestFinal_ex);
    }
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    void *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    static unsigned char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) EVP_DigestUpdate(ctx, buf, n);
    fclose(f);
    unsigned char md[32];
    unsigned ml = 0;
    EVP_DigestFinal_ex(ctx, md, &ml);
    EVP_MD_CTX_free(ctx);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", md[i]);
    return ml == 32 && !strcmp(hex, want);
}

static int download(const char *url, const char *path) {
    static void *c;
    static void *(*curl_easy_init)(void);
    static int (*curl_easy_setopt)(void *, int, ...);
    static int (*curl_easy_perform)(void *);
    static void (*curl_easy_cleanup)(void *);
    if (!c) {
        c = lib("libcurl.so.4");
        SYM(c, curl_easy_init); SYM(c, curl_easy_setopt); SYM(c, curl_easy_perform); SYM(c, curl_easy_cleanup);
    }
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    void *h = curl_easy_init();
    enum { URL = 10002, WRITEDATA = 10001, FOLLOWLOCATION = 52, FAILONERROR = 45, CONNECTTIMEOUT = 78,
           LOW_SPEED_LIMIT = 19, LOW_SPEED_TIME = 20 };
    curl_easy_setopt(h, URL, url);
    curl_easy_setopt(h, WRITEDATA, f);   /* default write function: fwrite to this FILE * */
    curl_easy_setopt(h, FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, FAILONERROR, 1L);
    curl_easy_setopt(h, CONNECTTIMEOUT, 30L);
    curl_easy_setopt(h, LOW_SPEED_LIMIT, 1000L);   /* give up on a stalled download after a minute */
    curl_easy_setopt(h, LOW_SPEED_TIME, 60L);
    const int r = curl_easy_perform(h);
    curl_easy_cleanup(h);
    if (fclose(f) || r) { fprintf(stderr, "debfetch: download failed (curl %d): %s\n", r, url); return -1; }
    return 0;
}

/* a .deb is an "ar" archive; its data.tar.{xz,zst,gz} holds the files */
static int unpack(const char *deb, const char *dir) {
    FILE *f = fopen(deb, "rb");
    if (!f) { perror(deb); return -1; }
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "!<arch>\n", 8)) { fprintf(stderr, "debfetch: %s isn't a package\n", deb); return -1; }
    char hdr[60];
    while (fread(hdr, 1, 60, f) == 60) {
        char name[17];
        memcpy(name, hdr, 16);
        name[16] = 0;
        for (int i = 15; i >= 0 && (name[i] == ' ' || name[i] == '/'); i--) name[i] = 0;
        const long size = strtol(hdr + 48, NULL, 10);
        if (!strncmp(name, "data.tar", 8)) {
            const char *z = strstr(name, ".xz") ? "xzcat" : strstr(name, ".zst") ? "zstd -dc" : strstr(name, ".gz") ? "gzip -dc" : "cat";
            char cmd[1024];
            snprintf(cmd, sizeof cmd, "%s | tar -x -C '%s'", z, dir);
            FILE *p = popen(cmd, "w");
            if (!p) return -1;
            static char buf[1 << 16];
            long left = size;
            while (left > 0) {
                const size_t n = fread(buf, 1, left < (long)sizeof buf ? (size_t)left : sizeof buf, f);
                if (!n) break;
                fwrite(buf, 1, n, p);
                left -= (long)n;
            }
            fclose(f);
            const int st = pclose(p);
            if (left || st) { fprintf(stderr, "debfetch: unpacking %s failed\n", deb); return -1; }
            return 0;
        }
        fseek(f, size + (size & 1), SEEK_CUR);
    }
    fclose(f);
    fprintf(stderr, "debfetch: no data in %s\n", deb);
    return -1;
}

int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "get")) {
        if (sha256_ok(argv[4], argv[3])) return 0;
        char part[1024];
        snprintf(part, sizeof part, "%s.part", argv[4]);
        for (int attempt = 0; attempt < 3; attempt++) {
            if (!download(argv[2], part) && sha256_ok(part, argv[3])) return rename(part, argv[4]) ? 1 : 0;
            fprintf(stderr, "debfetch: %s: bad or incomplete download, trying again\n", argv[2]);
            sleep(3);
        }
        unlink(part);
        return 1;
    }
    if (argc == 4 && !strcmp(argv[1], "data")) return unpack(argv[2], argv[3]) ? 1 : 0;
    fprintf(stderr, "usage: debfetch get <url> <sha256> <file> | debfetch data <file.deb> <dir>\n");
    return 2;
}
