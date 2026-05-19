#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Track 2 / ADR-0011 — BIP39 rendering for the trust-anchor (plan
 * §10.1/§10.4). Pure mapping bits → fixed standard words; the wordlist is
 * the frozen BIP39 English list (bip39_wordlist.h). The browser wallet
 * (WS-4) and Panel MUST use the identical list and the identical
 * MSB-first 11-bit slicing so the OLED and the panel show the same words.
 *
 * No mnemonic checksum is computed here: these are not seed phrases. The
 * fingerprint binds device_id (cross-device replay defence, §10.1) and the
 * verification authority is the physical OLED compare + 2-button hold, not
 * a BIP39 checksum.
 */

/*
 * Render `nwords` BIP39 words from the most-significant `nwords * 11` bits
 * of `data`. `data_len` must be >= ceil(nwords*11/8). Words are written
 * space-separated, lowercase, NUL-terminated into `out`. Returns the
 * number of characters written (excluding NUL), or -1 on bad args /
 * output overflow. Deterministic and allocation-free.
 */
int bip39_words_from_bits(const uint8_t *data, size_t data_len,
                          int nwords, char *out, size_t out_sz);

/*
 * Canonical packing of the top `nbits` of `digest` into `out`
 * (ceil(nbits/8) bytes), MSB-first, trailing low bits of the final byte
 * zeroed. This is the on-chain `claim_code` byte form (plan §10.4): both
 * the device and WS-4 derive it from the same bits the words encode, so a
 * words→bytes round-trip is unambiguous. `out` must hold ceil(nbits/8)
 * bytes. Returns the number of bytes written, or -1 on bad args.
 */
int bip39_pack_top_bits(const uint8_t *digest, size_t digest_len,
                         int nbits, uint8_t *out, size_t out_len);
