#include <kernel.h>
#include <lwip.h>
#include <lwip/priv/tcp_priv.h>

/* Network interface flags */
#define IFF_UP          (1 << 0)
#define IFF_BROADCAST   (1 << 1)
#define IFF_LOOPBACK    (1 << 3)
#define IFF_RUNNING     (1 << 6)
#define IFF_NOARP       (1 << 7)
#define IFF_MULTICAST   (1 << 12)

static heap lwip_heap;

/* Pretty silly. LWIP offers lwip_cyclic_timers for use elsewhere, but
   says to use LWIP_ARRAYSIZE(), which isn't possible with an
   incomplete type. Plus there's no terminator to the array. So we
   just have to manually create our own here. Check
   lwip/src/core/timeouts.c if we switch on any other LWIP components
   and add an entry here accordingly. Barf */

struct net_lwip_timer {
    u64 interval_ms;
    lwip_cyclic_timer_handler handler;
    char * name;
};

static struct net_lwip_timer net_lwip_timers[] = {
    {TCP_TMR_INTERVAL, tcp_tmr, "tcp"},
    {IP_TMR_INTERVAL, ip_reass_tmr, "ip"},
    {ARP_TMR_INTERVAL, etharp_tmr, "arp"},
    {DHCP_COARSE_TIMER_MSECS, dhcp_coarse_tmr, "dhcp coarse"},
    {DHCP_FINE_TIMER_MSECS, dhcp_fine_tmr, "dhcp fine"},
    {ND6_TMR_INTERVAL, nd6_tmr, "nd6"},
    {IP6_REASS_TMR_INTERVAL, ip6_reass_tmr, "ip6 reass"},
    {MLD6_TMR_INTERVAL, mld6_tmr, "mld6"},
};

closure_function(2, 1, void, dispatch_lwip_timer,
                 lwip_cyclic_timer_handler, handler, const char *, name,
                 u64, overruns /* ignored */)
{
#ifdef LWIP_DEBUG
    lwip_debug("dispatching timer for %s\n", bound(name));
#endif
    bound(handler)();
}

void sys_timeouts_init(void)
{
    int n = sizeof(net_lwip_timers) / sizeof(struct net_lwip_timer);
    for (int i = 0; i < n; i++) {
        struct net_lwip_timer * t = (struct net_lwip_timer *)&net_lwip_timers[i];
        timestamp interval = milliseconds(t->interval_ms);
        register_timer(runloop_timers, CLOCK_ID_MONOTONIC_RAW, interval, false, interval,
                       closure(lwip_heap, dispatch_lwip_timer, t->handler, t->name));
#ifdef LWIP_DEBUG
        lwip_debug("registered %s timer with period of %ld ms\n", t->name, t->interval_ms);
#endif
    }
}

void lwip_debug(char * format, ...)
{
    vlist a;
    vstart(a, format);
    log_vprintf("LWIP", format, &a);
}

void *lwip_allocate(u64 size)
{
    /* To maintain the malloc/free interface with mcache, allocations must stay
       within the range of objcaches and not fall back to parent allocs. */
    assert(size <= U64_FROM_BIT(MAX_LWIP_ALLOC_ORDER));
    void *p = allocate_zero(lwip_heap, size);
    return ((p != INVALID_ADDRESS) ? p : 0);
}

void lwip_deallocate(void *x)
{
    /* no size info; mcache won't care */
    deallocate(lwip_heap, x, -1ull);
}

static void lwip_ext_callback(struct netif* netif, netif_nsc_reason_t reason,
                              const netif_ext_callback_args_t* args)
{
    char ifname[4];
    netif_name_cpy(ifname, netif);
    if (reason & LWIP_NSC_IPV4_ADDRESS_CHANGED) {
        u8 *n = (u8 *)&netif->ip_addr;
        rprintf("%s: assigned %d.%d.%d.%d\n", ifname, n[0], n[1], n[2], n[3]);
    }
    if ((reason & LWIP_NSC_IPV6_ADDR_STATE_CHANGED) &&
            (netif_ip6_addr_state(netif, args->ipv6_addr_state_changed.addr_index) & IP6_ADDR_VALID))
        rprintf("%s: assigned %s\n", ifname, ipaddr_ntoa(args->ipv6_addr_state_changed.address));
}

u32_t lwip_rand(void)
{
    return random_u64();
}

/* unsigned only ... don't imagine we'd have negative interface numbers! */
int lwip_atoi(const char *p)
{
    u64 i;
    return u64_from_value(alloca_wrap_cstring(p), &i) ? i : -1;
}

/* mildly unfortunate to add another level of indirection to resolve types
   that are equivalent ... maybe a runtime types.h would be in order */
void lwip_memcpy(void *a, const void *b, unsigned long len)
{
    runtime_memcpy(a, b, len);
}

int lwip_strlen(char *a)
{
    return runtime_strlen(a);
}

void lwip_memset(void *x, unsigned char v, unsigned long len)
{
    runtime_memset(x, v, len);
}

int lwip_memcmp(const void *x, const void *y, unsigned long len)
{
    return runtime_memcmp(x, y, len);
}

int lwip_strcmp(const char *x, const char *y)
{
    return runtime_strcmp(x, y);
}

int lwip_strncmp(const char *x, const char *y, unsigned long len)
{
    for (int i = 0; i < len; i++) {
        if ((*x) != (*y)) return -1;
        if ((!*x) || (!*y)) return -1;
    }
    return 0;
}

struct netif *netif_get_default(void)
{
    return netif_default;
}
KLIB_EXPORT(netif_get_default);

u16 ifflags_from_netif(struct netif *netif)
{
    u16 flags = 0;
    if (netif_is_up(netif))
        flags |= IFF_UP;
    if (netif->flags & NETIF_FLAG_BROADCAST)
        flags |= IFF_BROADCAST;
    if (netif_is_loopback(netif))
        flags |= IFF_LOOPBACK;
    if (netif_is_link_up(netif))
        flags |= IFF_RUNNING;
    if (!(netif->flags & NETIF_FLAG_ETHARP))
        flags |= IFF_NOARP;
    if (netif->flags & NETIF_FLAG_IGMP)
        flags |= IFF_MULTICAST;
    return flags;
}

void netif_name_cpy(char *dest, struct netif *netif)
{
    runtime_memcpy(dest, netif->name, sizeof(netif->name));
    dest[sizeof(netif->name)] = '0' + netif->num;
    dest[sizeof(netif->name) + 1] = '\0';
}

KLIB_EXPORT(ipaddr_ntoa);
KLIB_EXPORT(dns_gethostbyname);
KLIB_EXPORT(pbuf_alloc);
KLIB_EXPORT(pbuf_free);
KLIB_EXPORT(udp_new);
KLIB_EXPORT(udp_sendto);
KLIB_EXPORT(udp_recv);

#define MAX_ADDR_LEN 20

#define MAX_IP6_ADDR_LEN    39

static boolean get_config_addr(tuple root, symbol s, ip4_addr_t *addr)
{
    string v = get_string(root, s);
    if (!v)
        return false;
    int len = MIN(buffer_length(v), MAX_ADDR_LEN);
    char str[MAX_ADDR_LEN + 1];
    runtime_memcpy(str, buffer_ref(v, 0), len);
    str[len] = '\0';
    if (ip4addr_aton(str, addr) == 1)
        return true;
    return false;
}

static boolean get_static_config(tuple root, struct netif *n, boolean trace) {
    ip4_addr_t ip;
    ip4_addr_t netmask;
    ip4_addr_t gw;

    string b = get_string(root, sym(ip6addr));
    if (b && (buffer_length(b) <= MAX_IP6_ADDR_LEN)) {
        bytes len = buffer_length(b);
        char str[len + 1];
        runtime_memcpy(str, buffer_ref(b, 0), len);
        str[len] = '\0';
        ip6_addr_t ip6;
        if (ip6addr_aton(str, &ip6))
            netif_add_ip6_address(n, &ip6, 0);
    }
    if (!get_config_addr(root, sym(ipaddr), &ip))
        return false;

    if (!get_config_addr(root, sym(netmask), &netmask))
        ip4_addr_set_u32(&netmask, lwip_htonl(0xffffff00)); // 255.255.255.0

    if (!get_config_addr(root, sym(gateway), &gw)) {
        // common best practices are: network + 1 or broadcast - 1,
        // so we will use latter if former is in use.
        u32_t ip_after_network = (netmask.addr & ip.addr) + lwip_htonl(1);
        if (ip_after_network == ip.addr)
            ip4_addr_set_u32(&gw, (~(netmask.addr) | ip.addr) - lwip_htonl(1));
        else
            ip4_addr_set_u32(&gw, ip_after_network);
    }

    if (trace) {
        rprintf("NET: static IP config:\n");
        rprintf(" address\t%s\n", ip4addr_ntoa(&ip));
        rprintf(" netmask\t%s\n", ip4addr_ntoa(&netmask));
        rprintf(" gateway\t%s\n", ip4addr_ntoa(&gw));
    }
    netif_set_addr(n, &ip, &netmask, &gw);
    netif_set_up(n);
    return true;
}

void init_network_iface(tuple root) {
    struct netif *n = netif_find("en1");
    if (!n) {
        rprintf("no network interface found\n");
        return;
    }

    boolean trace = get(root, sym(trace)) != 0;
    u64 mtu;
    if (get_u64(root, sym(mtu), &mtu)) {
        if (mtu < U64_FROM_BIT(16)) {
            if (trace)
                rprintf("NET: setting MTU for interface %c%c%d to %ld\n",
                        n->name[0], n->name[1], n->num, mtu);
            n->mtu = mtu;
        } else {
            msg_err("invalid MTU %ld; ignored\n", mtu);
        }
    }

    n->output_ip6 = ethip6_output;
    netif_create_ip6_linklocal_address(n, 1);
    netif_set_flags(n, NETIF_FLAG_MLD6);
    netif_set_default(n);
    if (!get_static_config(root, n, trace)) {
        dhcp_start(n);
    }
}

extern void lwip_init();

void init_net(kernel_heaps kh)
{
    heap h = heap_general(kh);
    heap backed = heap_backed(kh);
    lwip_heap = allocate_mcache(h, backed, 5, MAX_LWIP_ALLOC_ORDER, PAGESIZE_2M);
    lwip_init();
    NETIF_DECLARE_EXT_CALLBACK(netif_callback);
    netif_add_ext_callback(&netif_callback, lwip_ext_callback);
}
