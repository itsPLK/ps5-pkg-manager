/* Host-only diagnostics using the production connection/read implementation.
 * Including the implementation exposes negotiated state without adding a
 * public application API. Unused scanner/parser functions are dead stripped.
 */
#include <stdarg.h>
#include <time.h>
#include "../src/smb_client.c"
static app_settings_t probe_settings;
void pkg_cache_get_settings(app_settings_t *out) { *out = probe_settings; }
void install_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}
static const char *env_or_empty(const char *name) {
    const char *v = getenv(name); return v ? v : "";
}
static int verify_reads(smb_file_session_t *s, uint8_t *buf, size_t chunk) {
    uint8_t *reference = malloc(chunk);
    if (!reference) return -1;
    uint64_t offsets[] = {0, 1, 65535, s->file_size / 2, s->file_size > 31 ? s->file_size - 31 : 0, s->file_size};
    int result = -1;
    for (size_t i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
        uint64_t off = offsets[i];
        if (off > s->file_size) continue;
        size_t want = s->file_size - off < chunk ? (size_t)(s->file_size - off) : chunk;
        ssize_t rd = smb_file_session_read(s, buf, chunk, off);
        if (rd != (ssize_t)want) goto done;
        size_t n = 0;
        while (n < want) {
            ssize_t got = smb2_pread(s->ctx, s->fh, reference + n, (uint32_t)(want - n), off + n);
            if (got <= 0) goto done;
            n += (size_t)got;
        }
        if (memcmp(buf, reference, want) != 0) goto done;
    }
    puts("Read verification: sequential/unaligned/random/tail/EOF match synchronous libsmb2 reads");
    result = 0;
 done:
    free(reference);
    return result;
}
int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        fprintf(stderr, "Usage: %s smb://server/share[/file] [MiB=256] [chunk-KiB=2048]\nCredentials: SMB_USER, SMB_PASSWORD, SMB_DOMAIN. SMB_VERIFY=1 checks random reads.\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    smb_share_config_t *cfg = &probe_settings.smb_shares[0];
    probe_settings.smb_share_count = 1; cfg->enabled = 1;
    char rel[256];
    if (smb_client_parse_url(argv[1], cfg->server, sizeof(cfg->server), &cfg->port,
                             cfg->share, sizeof(cfg->share), rel, sizeof(rel)) != 0) return 2;
    snprintf(cfg->username, sizeof(cfg->username), "%s", env_or_empty("SMB_USER"));
    snprintf(cfg->password, sizeof(cfg->password), "%s", env_or_empty("SMB_PASSWORD"));
    snprintf(cfg->workgroup, sizeof(cfg->workgroup), "%s", env_or_empty("SMB_DOMAIN"));
    char error[512] = {0};
    if (smb_client_test_connection(cfg, error, sizeof(error)) != 0) {
        fprintf(stderr, "%s\n", error); return 1;
    }
    if (!*rel) {
        smb_share_info_t shares[MAX_SMB_BROWSE_SHARES];
        int n = smb_client_list_shares(cfg, shares, MAX_SMB_BROWSE_SHARES, error, sizeof(error));
        if (n < 0) fprintf(stderr, "Share listing unavailable (direct access worked): %s\n", error);
        else for (int i = 0; i < n; i++) printf("share: %s\n", shares[i].name);
        smb_dir_entry_t entries[64];
        n = smb_client_list_dir(cfg, "", entries, 64, error, sizeof(error));
        if (n < 0) { fprintf(stderr, "Directory listing failed: %s\n", error); return 1; }
        for (int i = 0; i < n; i++) printf("%s %llu %s\n", entries[i].is_dir ? "dir" : "file",
                                         (unsigned long long)entries[i].size, entries[i].name);
        return 0;
    }
    unsigned long mib = argc > 2 ? strtoul(argv[2], NULL, 10) : 256;
    unsigned long kib = argc > 3 ? strtoul(argv[3], NULL, 10) : 2048;
    if (!mib || mib > 65536 || !kib || kib > 16384) return 2;
    size_t chunk = kib * 1024, limit = (size_t)mib * 1024 * 1024;
    smb_file_session_t *s = smb_file_session_open(argv[1]);
    if (!s) { fprintf(stderr, "Could not open file\n"); return 1; }
    printf("dialect=0x%04x signing=%d encryption=%d credits=%u max_read=%u\n",
           s->ctx->dialect, s->ctx->sign, s->ctx->seal, s->ctx->credits, smb2_get_max_read_size(s->ctx));
    if (limit > s->file_size) limit = s->file_size;
    uint8_t *buf = malloc(chunk);
    if (!buf) { smb_file_session_close(s); return 1; }
    int result = 1;
    if (getenv("SMB_VERIFY") && verify_reads(s, buf, chunk) != 0) {
        fprintf(stderr, "Read verification FAILED\n"); goto done;
    }
    uint64_t start = smb_dbg_now_us_internal(); clock_t cpu = clock(); size_t total = 0;
    while (total < limit) {
        size_t want = limit - total < chunk ? limit - total : chunk;
        ssize_t rd = smb_file_session_read(s, buf, want, total);
        if (rd <= 0) { fprintf(stderr, "Read failed at %zu\n", total); goto done; }
        total += (size_t)rd;
    }
    double elapsed = (smb_dbg_now_us_internal() - start) / 1e6;
    printf("bytes=%zu chunk=%zu seconds=%.3f MB/s=%.2f CPU_seconds=%.3f\n",
           total, chunk, elapsed, elapsed > 0 ? total / 1e6 / elapsed : 0,
           (clock() - cpu) / (double)CLOCKS_PER_SEC);
    result = 0;
 done:
    free(buf); smb_file_session_close(s); return result;
}
