/* Native queue and allocation lifetime test; no radio or TCP delivery claimed. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lwip/ip.h"
#include "lwip/ip6_frag.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/stats.h"

struct ip_globals ip_data;
struct stats_ lwip_stats;
static unsigned live_charge, allocations, releases;
static unsigned length_errors;
static int chain_input;
static int ingress_mode;
static unsigned radio_releases;
extern void *test_detach(int, void *);
static void radio_free(struct pbuf *p) { assert(p); ++radio_releases; }
void bridge_lwip_mem_lifecycle(void *allocation, unsigned before, unsigned after) {
  assert(allocation && live_charge >= before);
  live_charge = live_charge - before + after;
  if (!before) ++allocations;
  if (!after) ++releases;
}
extern struct pbuf *bridge_pbuf_alloc_reserved(u16_t, unsigned, int *);

static struct pbuf *fragment(unsigned id, unsigned start, unsigned size,
                             int more, int extension) {
  unsigned headers = IP6_HLEN + (extension ? 8 : 0);
  struct pbuf *p = pbuf_alloc(PBUF_RAW, headers + IP6_FRAG_HLEN + size, PBUF_RAM);
  assert(p);
  memset(p->payload, 0, p->len);
  struct ip6_hdr *ip = p->payload;
  ip->_v_tc_fl = lwip_htonl(6UL << 28);
  ip->_plen = lwip_htons((extension ? 8 : 0) + IP6_FRAG_HLEN + size);
  ip->_nexth = extension ? IP6_NEXTH_DESTOPTS : IP6_NEXTH_FRAGMENT;
  ip->_hoplim = 64;
  ip->src.addr[0] = lwip_htonl(0x20010db8);
  ip->src.addr[3] = lwip_htonl(1);
  ip->dest.addr[0] = lwip_htonl(0x20010db8);
  ip->dest.addr[3] = lwip_htonl(2);
  if (extension) ((u8_t *)p->payload)[IP6_HLEN] = IP6_NEXTH_FRAGMENT;
  struct ip6_frag_hdr *frag = (void *)((u8_t *)p->payload + headers);
  frag->_nexth = IP6_NEXTH_TCP;
  frag->_fragment_offset = lwip_htons(start | (more ? IP6_FRAG_MORE_FLAG : 0));
  frag->_identification = lwip_htonl(id);
  for (unsigned n = 0; n < size; ++n) ((u8_t *)(frag + 1))[n] = (u8_t)(start + n);
  if (ingress_mode) {
    u8_t frame[2048] = {0};
    unsigned length = 14 + p->tot_len;
    assert(length <= sizeof(frame));
    frame[12] = 0x86; frame[13] = 0xdd;
    assert(pbuf_copy_partial(p, frame + 14, p->tot_len, 0) == p->tot_len);
    pbuf_free(p);
    struct pbuf_custom radio;
    radio.custom_free_function = radio_free;
    p = pbuf_alloced_custom(PBUF_RAW, length, PBUF_REF, &radio, frame, length);
    assert(p);
    unsigned before = radio_releases;
    p = test_detach(ingress_mode == 1, p);
    assert(p && p != &radio.pbuf && radio_releases == before + 1);
    u8_t check[2048];
    assert(pbuf_copy_partial(p, check, length, 0) == length && !memcmp(frame, check, length));
    assert(!pbuf_remove_header(p, 14));
    ip = p->payload;
  }
  if (chain_input) {
    const unsigned first_len = headers + IP6_FRAG_HLEN + 8;
    assert(p->len > first_len);
    struct pbuf *first = pbuf_alloc(PBUF_RAW, first_len, PBUF_RAM);
    struct pbuf *tail = pbuf_alloc(PBUF_RAW, p->len - first_len, PBUF_RAM);
    assert(first && tail);
    memcpy(first->payload, p->payload, first_len);
    memcpy(tail->payload, (u8_t *)p->payload + first_len, tail->len);
    pbuf_cat(first, tail);
    pbuf_free(p);
    p = first;
    ip = p->payload;
  }
  ip_data.current_ip6_header = ip;
  memcpy(ip6_current_src_addr(), &ip->src, 16);
  memcpy(ip6_current_dest_addr(), &ip->dest, 16);
  assert(!pbuf_remove_header(p, headers));
  return p;
}

static void empty(void) {
  assert(!live_charge && !lwip_stats.mem.used && allocations == releases);
}
static void complete(int extension, int reverse) {
  struct pbuf *p = fragment(1, reverse ? 24 : 0, 24, !reverse, extension);
  assert(!ip6_reass(p) && live_charge && live_charge == lwip_stats.mem.used);
  unsigned held = live_charge;
  p = fragment(1, reverse ? 0 : 24, 24, reverse, extension);
  p = ip6_reass(p);
  assert(p && p->tot_len == IP6_HLEN + (extension ? 8 : 0) + 48);
  assert(live_charge > held && live_charge == lwip_stats.mem.used);
  u8_t payload[48];
  assert(pbuf_copy_partial(p, payload, sizeof(payload), IP6_HLEN + (extension ? 8 : 0)) == sizeof(payload));
  for (unsigned n = 0; n < sizeof(payload); ++n) assert(payload[n] == n);
  struct ip6_hdr *ip = p->payload;
#ifdef EXPECT_LENGTH_BUG
  assert(lwip_ntohs(ip->_plen) == 40 + (extension ? 8 : 0));
  ++length_errors;
#else
  assert(lwip_ntohs(ip->_plen) == 48 + (extension ? 8 : 0));
#endif
  assert(ip->_nexth == (extension ? IP6_NEXTH_DESTOPTS : IP6_NEXTH_TCP));
  if (extension) assert(((u8_t *)p->payload)[40] == IP6_NEXTH_TCP);
  /* Native header/partial consumption does not release a retained block. */
  pbuf_ref(p);
  held = live_charge;
  assert(pbuf_free(p) == 0 && live_charge == held);
  pbuf_free(p);
  empty();
}
static void complete_chunked(void) {
  assert(!ip6_reass(fragment(30, 0, 1024, 1, 1)));
  struct pbuf *p = ip6_reass(fragment(30, 1024, 24, 0, 1));
  assert(p && pbuf_clen(p) == 3 && p->tot_len == 1096);
  u8_t bytes[1048];
  assert(pbuf_copy_partial(p, bytes, sizeof(bytes), 48) == sizeof(bytes));
  for (unsigned n = 0; n < sizeof(bytes); ++n) assert(bytes[n] == (u8_t)n);
  struct ip6_hdr *ip = p->payload;
#ifdef EXPECT_LENGTH_BUG
  assert(lwip_ntohs(ip->_plen) == 1048); ++length_errors;
#else
  assert(lwip_ntohs(ip->_plen) == 1056);
#endif
  pbuf_free(p); empty();
}
int main(void) {
  mem_init(); memp_init();
  for (ingress_mode = 0; ingress_mode < 3; ++ingress_mode)
    for (chain_input = 0; chain_input < 2; ++chain_input)
      for (int ext = 0; ext < 2; ++ext)
        for (int reverse = 0; reverse < 2; ++reverse) complete(ext, reverse);
  chain_input = ingress_mode = 0;
  for (ingress_mode = 1; ingress_mode <= 2; ++ingress_mode) complete_chunked();
  ingress_mode = 0;
  /* Duplicate and overlapping fragments release the rejected input only. */
  assert(!ip6_reass(fragment(2, 0, 24, 1, 0)));
  unsigned held = live_charge;
  assert(!ip6_reass(fragment(2, 0, 24, 1, 0)) && live_charge == held);
  assert(!ip6_reass(fragment(2, 16, 24, 1, 0)) && live_charge == held);
  for (unsigned n = 0; n <= IPV6_REASS_MAXAGE; ++n) ip6_reass_tmr();
  empty();
  /* Noninitial-only, missing-middle and explicit cleanup paths. */
  assert(!ip6_reass(fragment(3, 24, 24, 0, 1)));
  assert(!ip6_reass(fragment(3, 0, 16, 1, 1)));
  ip6_reass_cleanup(); empty();
  /* Exhaust native metadata: oldest incomplete datagrams are reclaimed. */
  for (unsigned n = 0; n < 20; ++n) assert(!ip6_reass(fragment(100 + n, 0, 24, 1, 0)));
  ip6_reass_cleanup(); empty();
  /* Exhaust the native pbuf-count gate with one datagram. */
  for (unsigned n = 0; n < IP_REASS_MAX_PBUFS; ++n)
    assert(!ip6_reass(fragment(4, n * 24, 24, 1, 0)));
  held = live_charge;
  assert(!ip6_reass(fragment(4, IP_REASS_MAX_PBUFS * 24, 24, 1, 0)) && live_charge == held);
  ip6_reass_cleanup(); empty();
  /* Length before the Fragment Header and 16-bit offset overflow fail closed. */
  struct pbuf *malformed = fragment(5, 0, 24, 1, 0);
  ip_data.current_ip6_header->_plen = lwip_htons(4);
  assert(!ip6_reass(malformed)); empty();
  assert(!ip6_reass(fragment(6, 65528, 24, 0, 0))); empty();
  /* Atomic allocation protects the same shared 4 KiB floor as target ingress. */
  struct pbuf *blocks[64]; unsigned count = 0; int denied = 0;
  while (count < 64) {
    struct pbuf *p = bridge_pbuf_alloc_reserved(1024, 4096, &denied);
    if (!p) break;
    blocks[count++] = p;
    assert(live_charge <= MEM_SIZE - 4096);
  }
  assert(count && count < 64 && denied && !lwip_stats.mem.err);
  struct pbuf *control = pbuf_alloc(PBUF_RAW, 512, PBUF_RAM);
  assert(control); pbuf_free(control);
  while (count) pbuf_free(blocks[--count]);
  empty();
  /* First clone chunk fits, second hits the atomic floor. Production ingress
   * must return its partial chain and the radio reference, for both STA/AP. */
  struct pbuf *fill[128];
  while (count < 128) {
    struct pbuf *p = bridge_pbuf_alloc_reserved(256, 4096, &denied);
    if (!p) break;
    fill[count++] = p;
  }
  assert(denied && count > 4 && count < 128);
  for (unsigned n = 0; n < 4; ++n) pbuf_free(fill[--count]);
  held = live_charge;
  unsigned before_allocations = allocations;
  for (int sta = 0; sta < 2; ++sta) {
    u8_t frame[1514] = {0};
    frame[12] = 0x86; frame[13] = 0xdd; frame[14] = 0x60;
    frame[18] = 5; frame[19] = 180; frame[20] = IP6_NEXTH_FRAGMENT;
    frame[57] = 8; /* noninitial fragment receives no control exemption */
    struct pbuf_custom radio;
    radio.custom_free_function = radio_free;
    struct pbuf *p = pbuf_alloced_custom(PBUF_RAW, sizeof(frame), PBUF_REF,
                                       &radio, frame, sizeof(frame));
    unsigned released = radio_releases;
    assert(!test_detach(sta, p) && radio_releases == released + 1);
    assert(live_charge == held && live_charge == lwip_stats.mem.used);
  }
  assert(allocations == before_allocations + 2 && !lwip_stats.mem.err);
  while (count) pbuf_free(fill[--count]);
  empty();
#ifdef EXPECT_LENGTH_BUG
  assert(length_errors == 26 && radio_releases == 38);
  puts("REPRODUCED supplier IPv6 length error: all 26 direct/STA/AP/chunked cases undercount by 8 bytes");
#else
  assert(!length_errors);
  puts("PASS native IPv6 reassembly: ordered/reversed/extensions/duplicate/overlap/timeout/cleanup/metadata/pbuf-limit/shared-reserve/partial-clone-rollback");
#endif
}
