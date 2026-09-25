/* PKG Manager: reuse the AES schedule for an entire SMB3 CMAC message.
 * AES-NI on x86 (including PS5); CommonCrypto on Apple. The caller retains
 * libsmb2's portable implementation for other CPUs and unavailable backends.
 * All state is local to the call: parallel SMB sessions never share keys.
 */
#ifndef PKGMGR_CMAC_ACCEL_H
#define PKGMGR_CMAC_ACCEL_H

#if !defined(PKGMGR_CMAC_PORTABLE) && (defined(__x86_64__) || defined(__i386__)) && (defined(__clang__) || defined(__GNUC__))
#include <wmmintrin.h>
#include <cpuid.h>
#define PKGMGR_CMAC_ACCEL 1
#define CMAC_TARGET __attribute__((target("aes,sse2")))
typedef struct { __m128i round[11]; } cmac_cipher;

static CMAC_TARGET int cmac_cipher_init(cmac_cipher *c, const uint8_t *key)
{
        __m128i k = _mm_loadu_si128((const __m128i *)key);
        c->round[0] = k;
#define CMAC_EXPAND(i, rcon) do { \
        __m128i a = _mm_aeskeygenassist_si128(k, rcon); \
        a = _mm_shuffle_epi32(a, 0xff); \
        k = _mm_xor_si128(k, _mm_slli_si128(k, 4)); \
        k = _mm_xor_si128(k, _mm_slli_si128(k, 4)); \
        k = _mm_xor_si128(k, _mm_slli_si128(k, 4)); \
        k = _mm_xor_si128(k, a); \
        c->round[i] = k; \
} while (0)
        CMAC_EXPAND(1, 0x01); CMAC_EXPAND(2, 0x02);
        CMAC_EXPAND(3, 0x04); CMAC_EXPAND(4, 0x08);
        CMAC_EXPAND(5, 0x10); CMAC_EXPAND(6, 0x20);
        CMAC_EXPAND(7, 0x40); CMAC_EXPAND(8, 0x80);
        CMAC_EXPAND(9, 0x1b); CMAC_EXPAND(10, 0x36);
#undef CMAC_EXPAND
        return 0;
}

static CMAC_TARGET int cmac_cipher_encrypt(cmac_cipher *c, uint8_t block[16])
{
        __m128i b = _mm_loadu_si128((const __m128i *)block);
        b = _mm_xor_si128(b, c->round[0]);
        for (int i = 1; i < 10; i++) b = _mm_aesenc_si128(b, c->round[i]);
        b = _mm_aesenclast_si128(b, c->round[10]);
        _mm_storeu_si128((__m128i *)block, b);
        return 0;
}
static void cmac_cipher_close(cmac_cipher *c) { (void)c; }
static int cmac_cipher_available(void)
{
        unsigned int a, b, c, d;
        return __get_cpuid(1, &a, &b, &c, &d) && (c & bit_AES);
}

#elif !defined(PKGMGR_CMAC_PORTABLE) && defined(__APPLE__) && defined(HAVE_COMMONCRYPTO_COMMONCRYPTOR_H)
#include <CommonCrypto/CommonCryptor.h>
#define PKGMGR_CMAC_ACCEL 1
#define CMAC_TARGET
typedef struct { CCCryptorRef cryptor; } cmac_cipher;
static int cmac_cipher_init(cmac_cipher *c, const uint8_t *key)
{
        return CCCryptorCreate(kCCEncrypt, kCCAlgorithmAES128, kCCOptionECBMode,
                               key, 16, NULL, &c->cryptor) == kCCSuccess ? 0 : -1;
}
static int cmac_cipher_encrypt(cmac_cipher *c, uint8_t block[16])
{
        size_t moved = 0;
        return CCCryptorUpdate(c->cryptor, block, 16, block, 16, &moved) == kCCSuccess
               && moved == 16 ? 0 : -1;
}
static void cmac_cipher_close(cmac_cipher *c) { CCCryptorRelease(c->cryptor); }
static int cmac_cipher_available(void) { return 1; }
#endif

#ifdef PKGMGR_CMAC_ACCEL
static void cmac_double(uint8_t key[16])
{
        unsigned int carry = key[0] >> 7;
        for (int i = 0; i < 15; i++) key[i] = (uint8_t)((key[i] << 1) | (key[i+1] >> 7));
        key[15] = (uint8_t)((key[15] << 1) ^ (carry ? 0x87 : 0));
}

static CMAC_TARGET int cmac_accel_compute(const uint8_t key[16], const uint8_t *msg,
                                          uint64_t len, uint8_t mac[16])
{
        cmac_cipher cipher;
        uint8_t subkey[16] = {0};
        uint8_t state[16] = {0};
        int rc = -1;
        if (cmac_cipher_init(&cipher, key) != 0) return -1;
        if (cmac_cipher_encrypt(&cipher, subkey) != 0) goto done;
        cmac_double(subkey); /* K1: complete final block */
        if (len == 0 || len % 16 != 0) cmac_double(subkey); /* K2: padding */
        while (len > 16) {
                for (int i = 0; i < 16; i++) state[i] ^= msg[i];
                if (cmac_cipher_encrypt(&cipher, state) != 0) goto done;
                msg += 16;
                len -= 16;
        }
        for (unsigned int i = 0; i < len; i++) state[i] ^= msg[i];
        if (len < 16) state[len] ^= 0x80;
        for (int i = 0; i < 16; i++) state[i] ^= subkey[i];
        if (cmac_cipher_encrypt(&cipher, state) != 0) goto done;
        memcpy(mac, state, 16);
        rc = 0;
 done:
        cmac_cipher_close(&cipher);
        return rc;
}
#endif

/* Return -1 to use the original portable implementation. */
static int pkgmgr_cmac_accel(const uint8_t key[16], const uint8_t *msg,
                             uint64_t len, uint8_t mac[16])
{
#ifdef PKGMGR_CMAC_ACCEL
        if (cmac_cipher_available()) return cmac_accel_compute(key, msg, len, mac);
#else
        (void)key; (void)msg; (void)len; (void)mac;
#endif
        return -1;
}
#endif
