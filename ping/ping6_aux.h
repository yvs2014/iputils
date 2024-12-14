#ifndef PING6_AUX_H
#define PING6_AUX_H

#include "common.h"
#include "stdint.h"

unsigned int if_name2index(const char *ifname);
ssize_t build_niquery(struct ping_rts *rts, uint8_t *_nih,
	unsigned packet_size __attribute__((__unused__)));
ssize_t build_echo(const struct ping_rts *rts, uint8_t *hdr);

int print6_icmp(uint8_t type, uint8_t code, uint32_t info);
void print6_echo_reply(const uint8_t *hdr, size_t len);
void print6_ni_reply(const uint8_t *hdr, size_t len);

#endif
