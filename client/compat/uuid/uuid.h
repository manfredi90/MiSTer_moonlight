/* Sostituto minimale di libuuid per libgamestream (servono solo UUID casuali). */
#ifndef MG_COMPAT_UUID_H
#define MG_COMPAT_UUID_H

#include <stdio.h>
#include <openssl/rand.h>

typedef unsigned char uuid_t[16];

static inline void uuid_generate_random(uuid_t out) {
  RAND_bytes(out, 16);
  out[6] = (out[6] & 0x0F) | 0x40; /* versione 4 */
  out[8] = (out[8] & 0x3F) | 0x80; /* variante RFC 4122 */
}

static inline void uuid_unparse(const uuid_t u, char *out) {
  sprintf(out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
          u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
          u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

#endif
