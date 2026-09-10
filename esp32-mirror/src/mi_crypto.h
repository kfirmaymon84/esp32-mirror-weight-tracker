#pragma once
#include <stddef.h>
#include <stdint.h>

// Thin wrappers over mbedTLS for the Mi-protocol crypto.
// All return true on success.

bool hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                 const uint8_t *salt, size_t salt_len,
                 const uint8_t *info, size_t info_len,
                 uint8_t *out, size_t out_len);

// Writes 32 bytes to out32.
bool hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t *out32);

// AES-128-CCM, 4-byte tag, 12-byte nonce, no AAD.
// encrypt: out must hold pt_len + 4 bytes (ciphertext || tag).
bool aes_ccm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *pt, size_t pt_len, uint8_t *out);

// decrypt: in is (ciphertext || tag), in_len includes the 4-byte tag.
// out must hold in_len - 4 bytes.
bool aes_ccm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                     const uint8_t *in, size_t in_len, uint8_t *out);

// 12-byte CCM nonce = iv(4) + 0x00*4 + seq(4, little-endian).
void mi_data_nonce(const uint8_t iv[4], uint32_t seq, uint8_t nonce[12]);
