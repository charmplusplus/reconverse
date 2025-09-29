#include "converse.h"
#include <cstring>

typedef struct { /*IPv4 IP address*/
  unsigned char data[4];
} skt_ip_t;
extern skt_ip_t _skt_invalid_ip;

static inline bool operator<(const skt_ip_t &a, const skt_ip_t &b) {
  return memcmp(&a, &b, sizeof(a)) < 0;
}