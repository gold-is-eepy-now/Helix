/* accounts.h — user accounts: names, numeric ids and salted password hashes.
 *
 * The account table lives in /etc/users on the ext4 root, one line per user:
 *     name:uid:iterations:salt-hex:hash-hex
 * The hash is PBKDF2-HMAC-SHA256 (sha256.h) over the password with a random
 * 16-byte salt. Plain passwords are never stored or logged.
 *
 * The root identity is the built-in console user (uid 0). It has no entry in
 * the table; the console is always available to the machine's owner.
 */
#ifndef HELIX_ACCOUNTS_H
#define HELIX_ACCOUNTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ACCT_NAME_MAX   32u
#define ACCT_PASS_MAX   128u
#define ACCT_MAX        32u
#define ACCT_FIRST_UID  1000u

/* Load /etc/users from the mounted root filesystem, creating /etc if needed.
 * Call after ext4 is mounted. Returns false if the file could not be read. */
bool accounts_init(void);

uint32_t accounts_count(void);
const char *accounts_name(uint32_t index);

/* Names must be 1-32 characters: letters, digits, '-' and '_', starting with
 * a letter or '_'. */
bool accounts_name_valid(const char *name);

/* Create an account and save the table. Fails if the name is taken or the
 * table is full or the filesystem is not writable. */
bool accounts_create(const char *name, const char *password, uint32_t *uid_out);

/* Check a password. On success, reports the account's uid. */
bool accounts_verify(const char *name, const char *password, uint32_t *uid_out);

bool accounts_set_password(const char *name, const char *password);

/* The identity the console is using. Starts as root. */
const char *accounts_session(void);
uint32_t accounts_session_uid(void);
void accounts_set_session(const char *name, uint32_t uid);

/* Fill buf with random bytes: RDRAND when the CPU has it, otherwise a weak
 * fallback that mixes the timestamp counter. Reports which one it used. */
void accounts_random(uint8_t *buf, size_t n, bool *used_rdrand);

#endif /* HELIX_ACCOUNTS_H */
