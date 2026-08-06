/**
 * security.h - Integrity checks and deployment evidence helpers.
 */

#ifndef PLC_SECURITY_H
#define PLC_SECURITY_H

#include <stddef.h>

#define SEC_SHA256_HEX_LEN 65

void sec_sha256_bytes(const unsigned char *data, size_t len,
                      char hex_out[SEC_SHA256_HEX_LEN]);
void sec_sha256_string(const char *str, char hex_out[SEC_SHA256_HEX_LEN]);
int sec_sha256_file(const char *path, char hex_out[SEC_SHA256_HEX_LEN]);
int sec_is_sha256_hex(const char *hex);
int sec_hash_equal(const char *a, const char *b);
int sec_write_integrity_report(const char *path,
                               const char *src_path,
                               const char *out_path,
                               const char *source_hash,
                               const char *output_hash,
                               int target,
                               int format,
                               const char *cpu_arch,
                               int hardening_level,
                               int deterministic,
                               int success,
                               int symbols,
                               int ast_nodes,
                               int safety_sil,
                               int quality_score);
void sec_memzero(void *ptr, size_t len);

#endif /* PLC_SECURITY_H */
