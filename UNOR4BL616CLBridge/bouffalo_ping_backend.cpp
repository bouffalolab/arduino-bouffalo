#include "ping_service.h"

#include <Arduino.h>
#include <string.h>

extern "C" {
#include <lwip/inet_chksum.h>
#include <lwip/netdb.h>
#include <lwip/pbuf.h>
#include <lwip/prot/icmp.h>
#include <lwip/prot/ip4.h>
#include <lwip/raw.h>
#include <lwip/tcpip.h>
}

namespace bridge {

namespace {

constexpr uint16_t kPingIdentifier = 0xB616U;
constexpr size_t kPingPayloadSize = 16U;
constexpr uint32_t kPingReplyTimeoutMs = 1000U;

struct PingSession {
    struct raw_pcb *pcb = nullptr;
    ip_addr_t target = {};
    uint16_t sequence = 0U;
    volatile bool waiting = false;
    volatile bool replied = false;
    volatile bool destination_unreachable = false;
};

bool resolve_hostname(void *, const char *hostname, uint8_t address[4])
{
    if (hostname == nullptr || address == nullptr) {
        return false;
    }
    struct hostent *host = gethostbyname(hostname);
    if (host == nullptr || host->h_addr_list == nullptr ||
        host->h_addr_list[0] == nullptr || host->h_length < 4) {
        return false;
    }
    memcpy(address, host->h_addr_list[0], 4U);
    return true;
}

u8_t receive_icmp(
    void *context,
    struct raw_pcb *,
    struct pbuf *packet,
    const ip_addr_t *source)
{
    PingSession *session = static_cast<PingSession *>(context);
    if (session == nullptr || packet == nullptr || source == nullptr ||
        !session->waiting ||
        packet->tot_len < sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr)) {
        return 0U;
    }

    uint8_t header[sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr)] = {};
    if (pbuf_copy_partial(packet, header, sizeof(header), 0U) != sizeof(header)) {
        return 0U;
    }

    const struct ip_hdr *ip = reinterpret_cast<const struct ip_hdr *>(header);
    const size_t ip_header_size = static_cast<size_t>(IPH_HL(ip)) * 4U;
    if (ip_header_size < sizeof(struct ip_hdr) ||
        ip_header_size + sizeof(struct icmp_echo_hdr) > packet->tot_len) {
        return 0U;
    }

    struct icmp_echo_hdr icmp = {};
    if (pbuf_copy_partial(
            packet,
            &icmp,
            sizeof(icmp),
            static_cast<u16_t>(ip_header_size)) != sizeof(icmp)) {
        return 0U;
    }

    if (ICMPH_TYPE(&icmp) == ICMP_DUR) {
        session->destination_unreachable = true;
        return 0U;
    }
    if (!ip_addr_cmp(&session->target, source) ||
        ICMPH_TYPE(&icmp) != ICMP_ER ||
        ICMPH_CODE(&icmp) != 0U ||
        icmp.id != htons(kPingIdentifier) ||
        icmp.seqno != htons(session->sequence)) {
        return 0U;
    }

    session->replied = true;
    pbuf_free(packet);
    return 1U;
}

bool send_echo(PingSession &session, uint8_t ttl)
{
    uint8_t request[sizeof(struct icmp_echo_hdr) + kPingPayloadSize] = {};
    struct icmp_echo_hdr *header =
        reinterpret_cast<struct icmp_echo_hdr *>(request);
    ICMPH_TYPE_SET(header, ICMP_ECHO);
    ICMPH_CODE_SET(header, 0U);
    header->id = htons(kPingIdentifier);
    header->seqno = htons(session.sequence);
    for (size_t index = sizeof(struct icmp_echo_hdr); index < sizeof(request); ++index) {
        request[index] = static_cast<uint8_t>(index);
    }
    header->chksum = inet_chksum(request, sizeof(request));

    struct pbuf *packet = pbuf_alloc(PBUF_IP, sizeof(request), PBUF_RAM);
    if (packet == nullptr ||
        pbuf_take(packet, request, sizeof(request)) != ERR_OK) {
        if (packet != nullptr) {
            pbuf_free(packet);
        }
        return false;
    }

    LOCK_TCPIP_CORE();
    if (session.pcb != nullptr) {
        session.pcb->ttl = ttl;
    }
    const err_t result = session.pcb == nullptr
        ? ERR_ARG
        : raw_sendto(session.pcb, packet, &session.target);
    UNLOCK_TCPIP_CORE();
    pbuf_free(packet);
    return result == ERR_OK;
}

PingResult echo(
    void *,
    const uint8_t address[4],
    uint8_t ttl,
    uint8_t count,
    uint32_t *average_rtt_ms)
{
    if (address == nullptr || average_rtt_ms == nullptr || count == 0U) {
        return PingResult::Error;
    }

    PingSession session = {};
    ip4_addr_t target = {};
    IP4_ADDR(&target, address[0], address[1], address[2], address[3]);
    ip_addr_copy_from_ip4(session.target, target);

    LOCK_TCPIP_CORE();
    session.pcb = raw_new(IP_PROTO_ICMP);
    if (session.pcb != nullptr) {
        raw_recv(session.pcb, receive_icmp, &session);
        (void)raw_bind(session.pcb, IP_ADDR_ANY);
    }
    UNLOCK_TCPIP_CORE();
    if (session.pcb == nullptr) {
        return PingResult::Error;
    }

    uint32_t total_rtt_ms = 0U;
    uint8_t successful_replies = 0U;
    bool send_failed = false;
    for (uint8_t sequence = 1U; sequence <= count; ++sequence) {
        session.sequence = sequence;
        session.replied = false;
        session.destination_unreachable = false;
        session.waiting = true;
        const uint32_t started_at = millis();
        if (!send_echo(session, ttl)) {
            session.waiting = false;
            send_failed = true;
            continue;
        }

        while (!session.replied && !session.destination_unreachable &&
               millis() - started_at < kPingReplyTimeoutMs) {
            delay(1);
        }
        session.waiting = false;
        if (session.replied) {
            total_rtt_ms += millis() - started_at;
            ++successful_replies;
        }
    }

    LOCK_TCPIP_CORE();
    raw_remove(session.pcb);
    UNLOCK_TCPIP_CORE();

    if (successful_replies != 0U) {
        *average_rtt_ms = total_rtt_ms / successful_replies;
        return PingResult::Success;
    }
    if (session.destination_unreachable) {
        return PingResult::DestinationUnreachable;
    }
    return send_failed ? PingResult::Error : PingResult::Timeout;
}

} // namespace

const PingBackend &bouffalo_ping_backend()
{
    static const PingBackend backend = {
        nullptr,
        resolve_hostname,
        echo,
    };
    return backend;
}

} // namespace bridge
