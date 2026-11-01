/*
 * lwip_dns_compat.c — dns_gethostbyname() for callers that have room for one address.
 *
 * WHAT IDF'S LWIP DOES
 *
 * With CONFIG_LWIP_DNS_MAX_HOST_IP above 1, a cache hit copies every address the name has
 * into the caller's buffer, as many as it has, up to that number:
 *
 *     components/lwip/lwip/src/core/dns.c (v5.5.5)
 *       dns_lookup()   ip_addr_copy(addr[out_idx], dns_table[i].ipaddr[j]); out_idx++   :726
 *
 * netconn and netdb (getaddrinfo, gethostbyname) were changed with it and pass an array of
 * DNS_MAX_HOST_IP. lwIP's own SNTP client was not: sntp_request() hands dns_gethostbyname()
 * a single ip_addr_t on the tcpip thread's stack (apps/sntp/sntp.c:629, the same in
 * esp-lwip 2.2.0-esp and master, checked 11.10.2026). pool.ntp.org has four addresses, so
 * the second, third and fourth — 72 bytes — land on sntp_request()'s saved s0 and return
 * address and on the frames of sys_check_timeouts() and tcpip_thread() above it.
 *
 * WHEN IT HAPPENS
 *
 * The first request resolves the name (ERR_INPROGRESS, no copy). Only a retry within the
 * record's TTL hits the cache, and SNTP retries only when the server did not answer: 15 s
 * later, then 30. On 11.10.2026 the panel sat on a network whose provider answered DNS but
 * nothing else, and rebooted every 40-80 s: "Instruction access fault", MEPC 0, RA 0, S1 =
 * sntp_request, task tiT, with 91.236.251.24 and 91.236.251.5 (pool.ntp.org) in the dumped
 * stack 24 bytes apart. On a network that answers NTP
 * it does not happen at all — the next request is an hour later and the record has expired.
 *
 * HOW
 *
 * -Wl,--wrap=dns_gethostbyname: resolve into an array of DNS_MAX_HOST_IP here and give the
 * caller the first entry, which is what a one-address caller asked for. In this firmware
 * the only caller of dns_gethostbyname() is sntp_request(); netconn calls
 * dns_gethostbyname_addrtype() with its array and is not wrapped. A future caller that does
 * pass an array through dns_gethostbyname() gets one address instead of all of them —
 * dns_gethostbyname_addrtype() is the call for that. The asynchronous answer
 * (ERR_INPROGRESS, then the callback) never touches `addr` and is unchanged.
 */

#include "lwip/opt.h"
#include "lwip/dns.h"

#if LWIP_DNS && (DNS_MAX_HOST_IP > 1)

err_t __real_dns_gethostbyname(const char *hostname, ip_addr_t *addr,
                               dns_found_callback found, void *callback_arg);
err_t __wrap_dns_gethostbyname(const char *hostname, ip_addr_t *addr,
                               dns_found_callback found, void *callback_arg);

/* The wrap repeats lwIP's prototype; this stops the build if lwip/dns.h changes it. */
static err_t (*const s_prototype_check)(const char *, ip_addr_t *, dns_found_callback, void *)
    __attribute__((unused)) = dns_gethostbyname;

err_t __wrap_dns_gethostbyname(const char *hostname, ip_addr_t *addr,
                               dns_found_callback found, void *callback_arg)
{
    if (addr == NULL) {
        /* lwIP answers ERR_ARG itself; keep that. */
        return __real_dns_gethostbyname(hostname, addr, found, callback_arg);
    }
    ip_addr_t all[DNS_MAX_HOST_IP];
    err_t err = __real_dns_gethostbyname(hostname, all, found, callback_arg);
    if (err == ERR_OK) {
        ip_addr_copy(*addr, all[0]);
    }
    return err;
}

#endif /* LWIP_DNS && DNS_MAX_HOST_IP > 1 */
