/* bignum.h — unsigned big integers for public-key signature checks (RSA verification).
 *
 * A number is an array of k little-endian 32-bit words. Modular arithmetic uses
 * Montgomery multiplication, which needs only multiplies, adds and shifts, so there is
 * no big division. Verification handles public values only, so these routines are not
 * constant-time; never use them on a secret key. */
#ifndef HELIX_BIGNUM_H
#define HELIX_BIGNUM_H

#include <stddef.h>
#include <stdint.h>

#define BN_MAX_BITS  4096u
#define BN_MAX_WORDS (BN_MAX_BITS / 32u)

/* Loads a big-endian byte string into k words. Returns 0 if the value does not fit. */
int bn_from_be(uint32_t *w, size_t k, const uint8_t *be, size_t len);

/* Writes the value as len big-endian bytes (high bytes zero-filled). */
void bn_to_be(const uint32_t *w, size_t k, uint8_t *be, size_t len);

/* Compares k-word values: <0, 0 or >0. */
int bn_cmp(const uint32_t *a, const uint32_t *b, size_t k);

/* a -= b in place (requires a >= b for a meaningful result). */
void bn_sub(uint32_t *a, const uint32_t *b, size_t k);

/* Number of significant bits (0 for zero). */
size_t bn_bits(const uint32_t *w, size_t k);

/* Montgomery context for an odd modulus n with k words. R = 2^(32k). */
typedef struct {
    uint32_t n[BN_MAX_WORDS];
    uint32_t r2[BN_MAX_WORDS];   /* R^2 mod n, used to enter Montgomery form */
    uint32_t ninv;               /* -n^-1 mod 2^32 */
    size_t k;
} mont_t;

/* Returns 0 if n is even, zero, or k is out of range. */
int mont_init(mont_t *m, const uint32_t *n, size_t k);

/* r = a * b * R^-1 mod n. Inputs must be below n. r may alias a or b. */
void mont_mul(const mont_t *m, uint32_t *r, const uint32_t *a, const uint32_t *b);

/* out = base^e mod n for a big-endian exponent of elen bytes. base must be below n. */
void mont_modexp(const mont_t *m, uint32_t *out, const uint32_t *base,
                 const uint8_t *e_be, size_t elen);

#endif /* HELIX_BIGNUM_H */
