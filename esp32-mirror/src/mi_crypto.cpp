#include "mi_crypto.h"

#include <string.h>

#include <mbedtls/ccm.h>
#include <mbedtls/md.h>

// HKDF-SHA256 implemented from HMAC-SHA256 (RFC 5869). arduino-esp32's prebuilt
// mbedTLS ships without mbedtls_hkdf(), so we do extract+expand ourselves.
bool hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                 const uint8_t *salt, size_t salt_len,
                 const uint8_t *info, size_t info_len,
                 uint8_t *out, size_t out_len) {
  if (info_len > 256) return false;
  // Extract: PRK = HMAC(salt, IKM)
  uint8_t prk[32];
  if (!hmac_sha256(salt, salt_len, ikm, ikm_len, prk)) return false;
  // Expand
  uint8_t t[32];
  size_t tlen = 0;
  uint8_t buf[32 + 256 + 1];
  size_t generated = 0;
  uint8_t counter = 1;
  while (generated < out_len) {
    size_t off = 0;
    if (tlen) {
      memcpy(buf, t, tlen);
      off += tlen;
    }
    memcpy(buf + off, info, info_len);
    off += info_len;
    buf[off++] = counter;
    if (!hmac_sha256(prk, 32, buf, off, t)) return false;
    tlen = 32;
    size_t n = (out_len - generated) < 32 ? (out_len - generated) : 32;
    memcpy(out + generated, t, n);
    generated += n;
    counter++;
  }
  return true;
}

bool hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data,
                 size_t data_len, uint8_t *out32) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return false;
  return mbedtls_md_hmac(md, key, key_len, data, data_len, out32) == 0;
}

bool aes_ccm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *pt, size_t pt_len, uint8_t *out) {
  mbedtls_ccm_context ctx;
  mbedtls_ccm_init(&ctx);
  bool ok = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) == 0;
  if (ok) {
    // tag is written directly after the ciphertext at out + pt_len
    ok = mbedtls_ccm_encrypt_and_tag(&ctx, pt_len, nonce, 12, nullptr, 0, pt,
                                     out, out + pt_len, 4) == 0;
  }
  mbedtls_ccm_free(&ctx);
  return ok;
}

bool aes_ccm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *in, size_t in_len, uint8_t *out) {
  if (in_len < 4) return false;
  size_t ct_len = in_len - 4;
  mbedtls_ccm_context ctx;
  mbedtls_ccm_init(&ctx);
  bool ok = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) == 0;
  if (ok) {
    ok = mbedtls_ccm_auth_decrypt(&ctx, ct_len, nonce, 12, nullptr, 0, in, out,
                                  in + ct_len, 4) == 0;
  }
  mbedtls_ccm_free(&ctx);
  return ok;
}

void mi_data_nonce(const uint8_t iv[4], uint32_t seq, uint8_t nonce[12]) {
  memcpy(nonce, iv, 4);
  memset(nonce + 4, 0, 4);
  nonce[8] = (uint8_t)(seq & 0xff);
  nonce[9] = (uint8_t)((seq >> 8) & 0xff);
  nonce[10] = (uint8_t)((seq >> 16) & 0xff);
  nonce[11] = (uint8_t)((seq >> 24) & 0xff);
}
