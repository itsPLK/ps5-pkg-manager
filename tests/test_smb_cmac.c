/* RFC 4493 section 4 vectors, plus the upstream implementation as an oracle.
 * Link the patched library and an upstream signing object with its four public
 * symbols renamed to reference_* (tools/run_smb_probe.sh --test-cmac).
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
void smb3_aes_cmac_128(uint8_t *, uint8_t *, uint64_t, uint8_t *);
void reference_cmac(uint8_t *, uint8_t *, uint64_t, uint8_t *);
static void unhex(const char *s, uint8_t *out) {
    assert(strlen(s) % 2 == 0);
    while (*s) { unsigned int v; assert(sscanf(s, "%2x", &v) == 1); *out++ = v; s += 2; }
}
static void *differential(void *arg) {
    unsigned int seed = (unsigned int)(uintptr_t)arg;
    uint8_t key[16], fast[16], ref[16];
    uint8_t *allocation = malloc(2 * 1024 * 1024 + 32);
    assert(allocation);
    uint8_t *data = allocation + 1; /* Deliberately unaligned input. */
    for (size_t i = 0; i < 2 * 1024 * 1024 + 16; i++) {
        seed = seed * 1664525 + 1013904223; data[i] = seed >> 24;
    }
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(seed >> (i % 4) * 8);
    const size_t sizes[] = {0,1,15,16,17,31,32,33,55,64,65,127,128,129,65535,65536,65537,524288,2097152};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        smb3_aes_cmac_128(key, data, sizes[i], fast);
        reference_cmac(key, data, sizes[i], ref);
        assert(memcmp(fast, ref, 16) == 0);
    }
    free(allocation);
    return NULL;
}
int main(void) {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int a, b, c, d;
    printf("CPU AES-NI: %s\n", __get_cpuid(1, &a, &b, &c, &d) && (c & bit_AES) ? "available" : "unavailable (portable fallback)");
#endif
    uint8_t key[16], data[64], out[16], expected[16];
    unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
    unhex("6bc1bee22e409f96e93d7e117393172a"
          "ae2d8a571e03ac9c9eb76fac45af8e51"
          "30c81c46a35ce411e5fbc1191a0a52ef"
          "f69f2445df4f9b17ad2b417be66c3710", data);
    const size_t sizes[] = {0,16,40,64};
    const char *macs[] = {"bb1d6929e95937287fa37d129b756746", "070a16b46b4d4144f79bdd9dd04a287c",
                         "dfa66747de9ae63030ca32611497c827", "51f0bebf7e3b9d92fc49741779363cfe"};
    for (int i = 0; i < 4; i++) {
        unhex(macs[i], expected); smb3_aes_cmac_128(key, data, sizes[i], out);
        if (memcmp(expected, out, 16) != 0) {
            fprintf(stderr, "Vector %d: ", i);
            for (int j = 0; j < 16; j++) fprintf(stderr, "%02x", out[j]);
            fputc('\n', stderr);
        }
        assert(memcmp(expected, out, 16) == 0);
    }
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; i++) assert(pthread_create(&threads[i], NULL, differential, (void *)(i+1)) == 0);
    for (int i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
    puts("CMAC: RFC 4493 vectors, unaligned/block boundaries, large messages and concurrent distinct keys passed");
    return 0;
}
