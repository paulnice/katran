/* Copyright (C) 2018-present, Facebook, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef __HANDLE_ICMP_H
#define __HANDLE_ICMP_H

/*
 * This file contains all routines which are responsible for parsing
 * and handling ICMP packets
 */

#include <uapi/linux/ip.h>
#include <uapi/linux/ipv6.h>
#include <uapi/linux/icmp.h>
#include <uapi/linux/icmpv6.h>
#include <stddef.h>
#include <uapi/linux/bpf.h>
#include <uapi/linux/if_ether.h>

#include "balancer_consts.h"
#include "balancer_structs.h"

__attribute__((__always_inline__))
static inline int swap_mac_and_send(void *data, void *data_end) {
  struct eth_hdr *eth;
  unsigned char tmp_mac[ETH_ALEN];
  eth = data;
  memcpy(tmp_mac, eth->eth_source, ETH_ALEN);
  memcpy(eth->eth_source, eth->eth_dest , ETH_ALEN);
  memcpy(eth->eth_dest, tmp_mac, ETH_ALEN);
  return XDP_TX;
}

__attribute__((__always_inline__))
static inline int send_icmp_reply(void *data, void *data_end) {
  struct iphdr *iph;
  struct icmphdr *icmp_hdr;
  __u32 tmp_addr = 0;
  __u64 off = 0;
  __u32 csum = 0;
  __u32 csum1 = 0;
  __u16 *next_iph_u16;
  if ((data + sizeof(struct eth_hdr)
      + sizeof(struct iphdr) + sizeof(struct icmphdr)) > data_end) {
    return XDP_DROP;
  }
  off += sizeof(struct eth_hdr);
  iph = data + off;
  off += sizeof(struct iphdr);
  icmp_hdr = data + off;
  icmp_hdr->type = ICMP_ECHOREPLY;
  // the only diff between icmp echo and reply hdrs is type;
  // in first case it's 8; in second it's 0; so instead of recalc
  // checksum from ground up we will just adjust it.
  icmp_hdr->checksum += 0x0008;
  iph->ttl = DEFAULT_TTL;
  tmp_addr = iph->daddr;
  iph->daddr = iph->saddr;
  iph->saddr = tmp_addr;
  iph->check = 0;
  next_iph_u16 = (__u16 *)iph;
  #pragma clang loop unroll(full)
  for (int i = 0; i < sizeof(struct iphdr) >> 1; i++) {
     csum += *next_iph_u16++;
  }
  iph->check = ~((csum & 0xffff) + (csum >> 16));
  return swap_mac_and_send(data, data_end);
}

__attribute__((__always_inline__))
static inline int send_icmp6_reply(void *data, void *data_end) {
  struct ipv6hdr *ip6h;
  struct icmp6hdr *icmp_hdr;
  __be32 tmp_addr[4];
  __u64 off = 0;
  if ((data + sizeof(struct eth_hdr)
      + sizeof(struct ipv6hdr) + sizeof(struct icmp6hdr)) > data_end) {
    return XDP_DROP;
  }
  off += sizeof(struct eth_hdr);
  ip6h = data + off;
  off += sizeof(struct ipv6hdr);
  icmp_hdr = data + off;
  icmp_hdr->icmp6_type = ICMPV6_ECHO_REPLY;
  // the only diff between icmp echo and reply hdrs is type;
  // in first case it's 128; in second it's 129; so instead of recalc
  // checksum from ground up we will just adjust it.
  icmp_hdr->icmp6_cksum -= 0x0001;
  ip6h->hop_limit = DEFAULT_TTL;
  memcpy(tmp_addr, ip6h->saddr.s6_addr32, 16);
  memcpy(ip6h->saddr.s6_addr32, ip6h->daddr.s6_addr32, 16);
  memcpy(ip6h->daddr.s6_addr32, tmp_addr, 16);
  return swap_mac_and_send(data, data_end);
}

__attribute__((__always_inline__))
static inline int parse_icmpv6(void *data, void *data_end, __u64 off,
                               struct packet_description *pckt) {
  struct icmp6hdr *icmp_hdr;
  struct ipv6hdr *ip6h;
  icmp_hdr = data + off;
  if (icmp_hdr + 1 > data_end) {
    return XDP_DROP;
  }
  if (icmp_hdr->icmp6_type == ICMPV6_ECHO_REQUEST) {
    return send_icmp6_reply(data, data_end);
  }
  if ((icmp_hdr->icmp6_type != ICMPV6_PKT_TOOBIG) &&
      (icmp_hdr->icmp6_type != ICMPV6_DEST_UNREACH)) {
    return XDP_PASS;
  }
  off += sizeof(struct icmp6hdr);
  // data partition of icmp 'pkt too big' contains header (and as much data as
  // as possible) of the packet, which has trigered this icmp.
  ip6h = data + off;
  if (ip6h + 1 > data_end) {
    return XDP_DROP;
  }
  pckt->flow.proto = ip6h->nexthdr;
  pckt->flags |= F_ICMP;
  memcpy(pckt->flow.srcv6, ip6h->daddr.s6_addr32, 16);
  memcpy(pckt->flow.dstv6, ip6h->saddr.s6_addr32, 16);
  return FURTHER_PROCESSING;
}

__attribute__((__always_inline__))
static inline int parse_icmp(void *data, void *data_end, __u64 off,
                             struct packet_description *pckt) {
  struct icmphdr *icmp_hdr;
  struct iphdr *iph;
  icmp_hdr = data + off;
  if (icmp_hdr + 1 > data_end) {
    return XDP_DROP;
  }
  if (icmp_hdr->type == ICMP_ECHO) {
    return send_icmp_reply(data, data_end);
  }
  if (icmp_hdr->type != ICMP_DEST_UNREACH) {
    return XDP_PASS;
  }
  off += sizeof(struct icmphdr);
  iph = data + off;
  if (iph + 1 > data_end) {
    return XDP_DROP;
  }
  if (iph->ihl != 5) {
    return XDP_DROP;
  }
  pckt->flow.proto = iph->protocol;
  pckt->flags |= F_ICMP;
  pckt->flow.src = iph->daddr;
  pckt->flow.dst = iph->saddr;
  return FURTHER_PROCESSING;
}
#endif // of __HANDLE_ICMP_H
