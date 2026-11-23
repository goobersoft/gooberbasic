/* GooberBASIC networking: one host and up to 7 clients share an array of
 * NET_SLOTS numbers, and can send each other text messages.
 *
 *   net_host(port [, mode])          start a game others can join (you are player 0)
 *   net_join(address, port [, mode]) join one (you become player 1-7 once connected)
 *   net_set(slot, value)             change a shared value: everyone sees it
 *   net_get(slot)                    read one
 *   net_send(text [, to])            a message to everyone (NET_ALL) or one player
 *   net_recv()                       the next message that arrived ("" if none); net_from() says who sent it
 *
 * mode is NET_TCP (the default) or NET_UDP. Both carry the same packets:
 *
 * The host owns the array. A client's net_set() changes its own copy at once
 * and is sent to the host, which applies it and passes it on to everyone.
 * Every change is stamped with a version (the host's "tick", or the
 * client's "write sequence"), and each slot travels with its version.
 * Packets are numbered; each one acknowledges the newest packet received and
 * the 32 before it (a bit each). When a packet is acknowledged, the sender
 * knows which slot versions got there; every packet carries the slots whose
 * newest version hasn't, so over UDP a lost packet just means those slots go
 * again. When they don't all fit, the next packet starts where this one
 * stopped. A slot older than the version already applied is ignored, so a
 * late UDP packet can't put an old value back.
 *
 * Messages are numbered per connection and delivered in order exactly once:
 * over UDP each one is resent until the other side acknowledges it; over TCP
 * it is sent once. The host relays clients' messages to their recipients.
 *
 * Nothing blocks: sockets are read and written when net_poll() runs, which
 * flip(), delay() and net_update() do (and net_status(), net_recv()). Sends
 * go out at most once a frame (16 ms), with a keepalive every 200 ms; a
 * connection that hears nothing for 5 seconds is dropped. IPv4 only.
 * GOOBER_NET_LOSS=N drops N% of the UDP packets sent, to test all that. */
#include "common.h"
#include "vm.h"

#define NET_SLOTS 256
#define NET_MAX_PEERS 8          /* the host (0) and clients 1-7 */
#define NET_MSG_MAX 255          /* bytes in one message */
#define NET_QUEUE 64             /* messages waiting to go, per connection */
#define NET_INBOX 256            /* messages received and not yet net_recv()'d */
#define UDP_MAX 1200             /* a datagram that crosses the internet unfragmented */
#define TCP_MAX 16384
#define WBUF_SIZE 65536
#define SLOT_BYTES 13             /* in a packet: slot (1), version (4), value (8) */
#define PKT_SLOTS (UDP_MAX / SLOT_BYTES)
#define HISTORY 33                /* packets remembered until acknowledged: the newest + 32 bits */
#define SEND_MS 16
#define KEEPALIVE_MS 200
#define HELLO_MS 250
#define TIMEOUT_MS 5000

enum { NET_TCP = 1, NET_UDP = 2 };
enum { ST_OFF, ST_CONNECTING, ST_CONNECTED, ST_LOST };
enum { P_HELLO = 1, P_WELCOME, P_REJECT, P_STATE, P_BYE };
#define PROTO_VERSION 1
#define TO_ALL 255

#if defined(__EMSCRIPTEN__)
#define NET_STUB 1                /* the browser has no raw sockets */
#elif defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#define sock_errno() WSAGetLastError()
#define E_AGAIN(e) ((e) == WSAEWOULDBLOCK)
#define E_INPROGRESS(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
#define E_RESET(e) ((e) == WSAECONNRESET)   /* UDP: an ICMP "port unreachable" from an earlier send */
static void set_nonblock(sock_t s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
static uint64_t now_ms(void) { return GetTickCount64(); }
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
#define sock_errno() errno
#define E_AGAIN(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#define E_INPROGRESS(e) ((e) == EINPROGRESS)
#define E_RESET(e) ((e) == ECONNREFUSED)
static void set_nonblock(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct {
    uint32_t id;
    uint8_t from, to;
    uint8_t len;
    char text[NET_MSG_MAX + 1];
} Msg;

#ifndef NET_STUB
/* one connection: the host has one per client (links[1..7]), a client one to the host (links[0]) */
typedef struct {
    bool used;
    bool welcomed;            /* the hello/welcome handshake is done */
    bool connecting;          /* client TCP: connect() still in progress */
    sock_t sock;              /* TCP */
    struct sockaddr_in addr;  /* UDP: where the other side is */
    uint64_t last_recv, last_send, started;
    uint32_t out_seq;         /* the number of our last STATE packet */
    uint32_t in_seq, in_bits; /* the newest of its packets received; bit k: in_seq - 1 - k too */
    uint32_t sent_ack, sent_msg_ack;   /* what our last packet acknowledged */
    uint32_t acked[NET_SLOTS];   /* per slot: the newest of our versions it is known to have */
    uint32_t seen[NET_SLOTS];    /* per slot: the newest of its versions applied */
    struct { uint32_t seq; int n; uint8_t slot[PKT_SLOTS]; uint32_t ver[PKT_SLOTS]; } hist[HISTORY];
    unsigned cursor;          /* the slot a full packet stopped at */
    Msg out[NET_QUEUE];       /* messages not yet acknowledged (UDP) or not yet sent (TCP) */
    int nout;
    uint32_t next_msg;        /* the id our next message gets */
    uint32_t msg_recv;        /* the newest of its message ids delivered */
    uint8_t rbuf[2 + TCP_MAX];
    int rlen;
    uint8_t wbuf[WBUF_SIZE];
    int wlen;
} Link;
#endif

static struct {
    int mode;                 /* 0 = off, NET_TCP, NET_UDP */
    bool host;
    int status;
    int my_id;
    char err[256];
    double val[NET_SLOTS];
    uint32_t ver[NET_SLOTS];  /* host: the tick each slot last changed; client: the write sequence */
    uint32_t tick;            /* host: the state's version; client: the newest write sequence sent */
    bool dirty;               /* changes stamped tick + 1 are waiting for the next send */
    uint8_t peers;            /* bit i: player i is in the game */
    Msg inbox[NET_INBOX];
    int in_head, in_count;
    int last_from;
#ifndef NET_STUB
    sock_t sock;              /* the host's listening (TCP) or only (UDP) socket; a UDP client's socket */
    Link links[NET_MAX_PEERS];
    uint64_t last_round;
    bool wsa;
    int loss;                 /* GOOBER_NET_LOSS=30: drop 30% of the UDP packets we send (for testing) */
    uint64_t loss_rng;
#endif
} N = {
    .my_id = -1, .last_from = -1,
#ifndef NET_STUB
    .sock = BAD_SOCK,
#endif
};

static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(N.err, sizeof N.err, fmt, ap);
    va_end(ap);
}

static __attribute__((unused)) void inbox_put(const Msg *m) {
    if (N.in_count == NET_INBOX) { N.in_head = (N.in_head + 1) % NET_INBOX; N.in_count--; }   /* drop the oldest */
    N.inbox[(N.in_head + N.in_count) % NET_INBOX] = *m;
    N.in_count++;
}

#ifdef NET_STUB

void net_poll(void) {}
void net_shutdown(void) { N.status = ST_OFF; }
static bool net_start(bool host, const char *address, int port, int mode) {
    (void)host; (void)address; (void)port; (void)mode;
    set_err("networking is not available in the browser");
    N.status = ST_LOST;
    return false;
}
static bool queue_msg(int to, const char *text, int len) { (void)to; (void)text; (void)len; return false; }

#else

/* ---- packets: little-endian bytes ---- */

typedef struct { uint8_t *p; int len, cap; bool full; } Out;
typedef struct { const uint8_t *p; int len, pos; bool bad; } In;

static void w8(Out *o, uint32_t v) { if (o->len + 1 > o->cap) { o->full = true; return; } o->p[o->len++] = (uint8_t)v; }
static void w16(Out *o, uint32_t v) { w8(o, v & 255); w8(o, v >> 8); }
static void w32(Out *o, uint32_t v) { w16(o, v & 0xFFFF); w16(o, v >> 16); }
static void w64(Out *o, uint64_t v) { w32(o, (uint32_t)v); w32(o, (uint32_t)(v >> 32)); }
static uint32_t r8(In *i) { if (i->pos + 1 > i->len) { i->bad = true; return 0; } return i->p[i->pos++]; }
static uint32_t r16(In *i) { uint32_t a = r8(i); return a | r8(i) << 8; }
static uint32_t r32(In *i) { uint32_t a = r16(i); return a | r16(i) << 16; }
static uint64_t r64(In *i) { uint64_t a = r32(i); return a | (uint64_t)r32(i) << 32; }
static void header(Out *o, int type) { w8(o, 'G'); w8(o, 'N'); w8(o, PROTO_VERSION); w8(o, (uint32_t)type); }

static int link_index(Link *l) { return (int)(l - N.links); }
static int max_packet(void) { return N.mode == NET_UDP ? UDP_MAX : TCP_MAX; }

static void drop_link(Link *l, const char *why);

/* TCP: append a frame (2-byte length + packet) and send what the socket takes */
static void tcp_flush(Link *l) {
    while (l->wlen > 0) {
        int n = (int)send(l->sock, (const char *)l->wbuf, (size_t)l->wlen, MSG_NOSIGNAL);
        if (n < 0) {
            if (E_AGAIN(sock_errno())) return;
            drop_link(l, "the connection was lost");
            return;
        }
        memmove(l->wbuf, l->wbuf + n, (size_t)(l->wlen - n));
        l->wlen -= n;
    }
}

static void send_packet(Link *l, const uint8_t *p, int len) {
    l->last_send = now_ms();
    if (N.mode == NET_UDP) {
        if (N.loss) {                           /* GOOBER_NET_LOSS: pretend it got lost (own random numbers, not rnd()'s) */
            N.loss_rng = N.loss_rng * 6364136223846793005ull + 1442695040888963407ull;
            if ((int)((N.loss_rng >> 33) % 100) < N.loss) return;
        }
        sendto(N.sock, (const char *)p, (size_t)len, 0, (struct sockaddr *)&l->addr, sizeof l->addr);
        return;
    }
    if (l->wlen + 2 + len > WBUF_SIZE) { drop_link(l, "the connection is too slow"); return; }
    l->wbuf[l->wlen++] = (uint8_t)(len & 255);
    l->wbuf[l->wlen++] = (uint8_t)(len >> 8);
    memcpy(l->wbuf + l->wlen, p, (size_t)len);
    l->wlen += len;
    tcp_flush(l);
}

static void send_simple(Link *l, int type, int arg) {
    uint8_t buf[8];
    Out o = {buf, 0, sizeof buf, false};
    header(&o, type);
    if (arg >= 0) w8(&o, (uint32_t)arg);
    send_packet(l, buf, o.len);
}

static uint8_t peers_mask(void) {
    if (!N.host) return N.peers;
    uint8_t m = 1;
    for (int i = 1; i < NET_MAX_PEERS; i++)
        if (N.links[i].used && N.links[i].welcomed) m |= (uint8_t)(1u << i);
    return m;
}

static bool slots_pending(Link *l) {
    for (int i = 0; i < NET_SLOTS; i++) if (N.ver[i] > l->acked[i]) return true;
    return false;
}

/* a STATE packet: its number, our acknowledgements, messages, then the slots it lacks */
static void send_state(Link *l) {
    uint8_t buf[TCP_MAX];
    Out o = {buf, 0, max_packet(), false};
    uint32_t seq = ++l->out_seq;
    header(&o, P_STATE);
    w32(&o, seq);
    w32(&o, l->in_seq);
    w32(&o, l->in_bits);
    w32(&o, l->msg_recv);
    w8(&o, peers_mask());

    int nmsg_at = o.len, nmsg = 0;
    w8(&o, 0);
    for (int k = 0; k < l->nout; k++) {
        Msg *m = &l->out[k];
        if (o.len + 7 + m->len > o.cap - 8 * SLOT_BYTES) break;   /* leave room for some slots */
        w32(&o, m->id); w8(&o, m->from); w8(&o, m->to); w8(&o, m->len);
        memcpy(o.p + o.len, m->text, m->len);
        o.len += m->len;
        nmsg++;
    }
    o.p[nmsg_at] = (uint8_t)nmsg;
    if (N.mode == NET_TCP && nmsg) {           /* TCP delivers them: forget them now */
        memmove(l->out, l->out + nmsg, sizeof(Msg) * (size_t)(l->nout - nmsg));
        l->nout -= nmsg;
    }

    int nslot_at = o.len, nslot = 0;
    w16(&o, 0);
    unsigned start = l->cursor;
    for (int j = 0; j < NET_SLOTS; j++) {
        int i = (int)((start + (unsigned)j) % NET_SLOTS);
        if (N.ver[i] <= l->acked[i]) continue;
        if (o.len + SLOT_BYTES > o.cap || nslot == PKT_SLOTS) { l->cursor = (unsigned)i; break; }
        uint64_t bits;
        memcpy(&bits, &N.val[i], 8);
        w8(&o, (uint32_t)i);
        w32(&o, N.ver[i]);
        w64(&o, bits);
        if (N.mode == NET_TCP) l->acked[i] = N.ver[i];   /* it will arrive */
        else {
            int h = (int)(seq % HISTORY);
            l->hist[h].slot[nslot] = (uint8_t)i;
            l->hist[h].ver[nslot] = N.ver[i];
        }
        nslot++;
    }
    o.p[nslot_at] = (uint8_t)(nslot & 255);
    o.p[nslot_at + 1] = (uint8_t)(nslot >> 8);
    if (N.mode == NET_UDP) { l->hist[seq % HISTORY].seq = seq; l->hist[seq % HISTORY].n = nslot; }
    l->sent_ack = l->in_seq;
    l->sent_msg_ack = l->msg_recv;
    send_packet(l, buf, o.len);
}

/* it has our packet `seq`: so it has the slot versions in it */
static void packet_acked(Link *l, uint32_t seq) {
    int h = (int)(seq % HISTORY);
    if (!seq || l->hist[h].seq != seq) return;  /* too old, or already counted */
    for (int k = 0; k < l->hist[h].n; k++) {
        int i = l->hist[h].slot[k];
        if (l->hist[h].ver[k] > l->acked[i]) l->acked[i] = l->hist[h].ver[k];
    }
    l->hist[h].seq = 0;
}

static void link_reset(Link *l) {
    if (N.mode == NET_TCP && l->used && l->sock != BAD_SOCK) sock_close(l->sock);
    memset(l, 0, offsetof(Link, rbuf));
    l->sock = BAD_SOCK;
    l->rlen = l->wlen = 0;
}

static void drop_link(Link *l, const char *why) {
    if (!l->used) return;
    link_reset(l);
    if (!N.host) {                              /* the client lost the host: the game is over */
        if (N.status != ST_LOST) set_err("%s", why);
        N.status = ST_LOST;
        N.peers = 0;
    }
}

static Link *new_link(void) {
    for (int i = 1; i < NET_MAX_PEERS; i++)
        if (!N.links[i].used) {
            Link *l = &N.links[i];
            link_reset(l);
            l->used = true;
            l->next_msg = 1;
            l->last_recv = l->started = now_ms();
            return l;
        }
    return NULL;
}

/* ---- reading packets ---- */

static void relay_or_keep(const Msg *m) {
    if ((m->to == TO_ALL || m->to == N.my_id) && m->from != N.my_id) inbox_put(m);
    if (!N.host) return;
    for (int i = 1; i < NET_MAX_PEERS; i++) {
        Link *l = &N.links[i];
        if (!l->used || !l->welcomed || i == m->from) continue;
        if (m->to != TO_ALL && m->to != i) continue;
        if (l->nout == NET_QUEUE) continue;     /* that player isn't keeping up: it misses this one */
        Msg c = *m;
        c.id = l->next_msg++;
        l->out[l->nout++] = c;
    }
}

static void read_state(Link *l, In *in) {
    uint32_t seq = r32(in), ack = r32(in), ack_bits = r32(in), msg_ack = r32(in);
    uint32_t peers = r8(in);
    if (in->bad) return;
    if (seq > l->in_seq) {                      /* note what we got, to acknowledge it */
        uint32_t d = seq - l->in_seq;
        l->in_bits = d >= 32 ? 0 : (l->in_bits << d) | (l->in_seq ? 1u << (d - 1) : 0);
        l->in_seq = seq;
    } else if (seq < l->in_seq && l->in_seq - seq <= 32) {
        l->in_bits |= 1u << (l->in_seq - seq - 1);
    }
    if (ack <= l->out_seq) {
        packet_acked(l, ack);
        for (int k = 0; k < 32; k++) if (ack_bits >> k & 1) packet_acked(l, ack - 1 - (uint32_t)k);
    }
    int k = 0;                                  /* messages it has: stop resending them */
    while (k < l->nout && l->out[k].id <= msg_ack) k++;
    if (k) { memmove(l->out, l->out + k, sizeof(Msg) * (size_t)(l->nout - k)); l->nout -= k; }
    if (!N.host) N.peers = (uint8_t)peers;

    int nmsg = (int)r8(in);
    for (int j = 0; j < nmsg && !in->bad; j++) {
        Msg m;
        m.id = r32(in); m.from = (uint8_t)r8(in); m.to = (uint8_t)r8(in); m.len = (uint8_t)r8(in);
        if (in->bad || in->pos + m.len > in->len) { in->bad = true; return; }
        memcpy(m.text, in->p + in->pos, m.len);
        m.text[m.len] = 0;
        in->pos += m.len;
        if (m.id != l->msg_recv + 1) continue;  /* a repeat (or a gap a resend will fill) */
        l->msg_recv = m.id;
        if (N.host) m.from = (uint8_t)link_index(l);   /* a client can't pretend to be someone else */
        relay_or_keep(&m);
    }

    int nslot = (int)r16(in);
    for (int j = 0; j < nslot && !in->bad; j++) {
        int i = (int)r8(in);
        uint32_t ver = r32(in);
        uint64_t bits = r64(in);
        if (in->bad || ver <= l->seen[i]) continue;   /* we have this one, or newer */
        l->seen[i] = ver;
        double v;
        memcpy(&v, &bits, 8);
        if (N.host) {
            if (v != N.val[i] || N.ver[i] == 0) { N.val[i] = v; N.ver[i] = N.tick + 1; N.dirty = true; }
        } else if (N.ver[i] <= l->acked[i]) {   /* not while our own change to it is on its way */
            N.val[i] = v;
        }
    }
}

/* host: turn someone away (1 = full, 2 = another version) */
static void reject(Link *l, const struct sockaddr_in *from, int why) {
    if (l) {
        send_simple(l, P_REJECT, why);
        if (N.mode == NET_TCP) tcp_flush(l);
        drop_link(l, "");
        return;
    }
    uint8_t buf[8];
    Out o = {buf, 0, sizeof buf, false};
    header(&o, P_REJECT);
    w8(&o, (uint32_t)why);
    sendto(N.sock, (const char *)buf, (size_t)o.len, 0, (const struct sockaddr *)from, sizeof *from);
}

static void handle_packet(Link *l, const uint8_t *p, int len, const struct sockaddr_in *from) {
    In in = {p, len, 0, false};
    if (r8(&in) != 'G' || r8(&in) != 'N') return;
    uint32_t version = r8(&in), type = r8(&in);
    if (in.bad) return;

    if (N.host) {
        if (type == P_HELLO) {
            if (version != PROTO_VERSION) { reject(l, from, 2); return; }
            if (!l) {                           /* UDP: someone new */
                l = new_link();
                if (!l) { reject(NULL, from, 1); return; }   /* full: tell them without keeping them */
                l->addr = *from;
            }
            l->welcomed = true;
            l->last_recv = now_ms();
            send_simple(l, P_WELCOME, link_index(l));   /* again if they ask again (a lost welcome) */
            return;
        }
        if (!l || !l->welcomed) return;
        l->last_recv = now_ms();
        if (type == P_STATE) read_state(l, &in);
        else if (type == P_BYE) drop_link(l, "");
        return;
    }

    /* client: everything comes from the host on links[0] */
    l = &N.links[0];
    l->last_recv = now_ms();
    if (type == P_WELCOME) {
        uint32_t id = r8(&in);
        if (in.bad || id < 1 || id >= NET_MAX_PEERS) return;
        if (!l->welcomed) { l->welcomed = true; N.my_id = (int)id; N.status = ST_CONNECTED; }
    } else if (type == P_REJECT) {
        uint32_t why = r8(&in);
        drop_link(l, why == 1 ? "the game is full" : "the host runs a different version");
    } else if (type == P_STATE && l->welcomed) {
        read_state(l, &in);
    } else if (type == P_BYE) {
        drop_link(l, "the host ended the game");
    }
}

static void udp_receive(void) {
    uint8_t buf[UDP_MAX + 64];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = (int)recvfrom(N.sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) {
            if (E_RESET(sock_errno())) continue;   /* the other side's port is closed: the timeout decides */
            return;
        }
        Link *l = NULL;
        for (int i = 0; i < NET_MAX_PEERS; i++) {
            Link *c = &N.links[i];
            if (c->used && c->addr.sin_addr.s_addr == from.sin_addr.s_addr && c->addr.sin_port == from.sin_port) { l = c; break; }
        }
        if (!N.host && !l) continue;            /* a client only listens to its host */
        handle_packet(l, buf, n, &from);
    }
}

static void tcp_receive(Link *l) {
    for (;;) {
        int room = (int)sizeof l->rbuf - l->rlen;
        int n = (int)recv(l->sock, (char *)l->rbuf + l->rlen, (size_t)room, 0);
        if (n == 0) { drop_link(l, N.host ? "" : "the host closed the connection"); return; }
        if (n < 0) {
            if (E_AGAIN(sock_errno())) return;
            drop_link(l, "the connection was lost");
            return;
        }
        l->rlen += n;
        int pos = 0;
        while (l->rlen - pos >= 2) {
            int len = l->rbuf[pos] | l->rbuf[pos + 1] << 8;
            if (len > TCP_MAX) { drop_link(l, "the other side sent garbage"); return; }
            if (l->rlen - pos - 2 < len) break;
            handle_packet(l, l->rbuf + pos + 2, len, NULL);
            if (!l->used) return;
            pos += 2 + len;
        }
        memmove(l->rbuf, l->rbuf + pos, (size_t)(l->rlen - pos));
        l->rlen -= pos;
    }
}

static void tcp_accept(void) {
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        sock_t s = accept(N.sock, (struct sockaddr *)&from, &fl);
        if (s == BAD_SOCK) return;
        set_nonblock(s);
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        Link *l = new_link();
        if (!l) {
            uint8_t buf[10] = {5, 0, 'G', 'N', PROTO_VERSION, P_REJECT, 1};
            send(s, (const char *)buf, 7, MSG_NOSIGNAL);
            sock_close(s);
            continue;
        }
        l->sock = s;
        l->addr = from;
    }
}

/* client TCP: has connect() finished? */
static void tcp_check_connect(Link *l) {
    fd_set wf, ef;
    FD_ZERO(&wf); FD_ZERO(&ef);
    FD_SET(l->sock, &wf); FD_SET(l->sock, &ef);
    struct timeval tv = {0, 0};
    if (select((int)l->sock + 1, NULL, &wf, &ef, &tv) <= 0) return;
    int e = 0;
    socklen_t el = sizeof e;
    getsockopt(l->sock, SOL_SOCKET, SO_ERROR, (char *)&e, &el);
    if (e || FD_ISSET(l->sock, &ef)) { drop_link(l, "could not connect to the host"); return; }
    l->connecting = false;
    send_simple(l, P_HELLO, -1);
}

/* ms since t; 0 if t is later (a packet that arrived after `now` was read) */
static uint64_t since(uint64_t now, uint64_t t) { return now > t ? now - t : 0; }

void net_poll(void) {
    if (!N.mode) return;
    if (N.mode == NET_UDP) udp_receive();
    else {
        if (N.host) tcp_accept();
        for (int i = 0; i < NET_MAX_PEERS; i++) {
            Link *l = &N.links[i];
            if (!l->used) continue;
            if (l->connecting) { tcp_check_connect(l); continue; }
            tcp_receive(l);
        }
    }

    uint64_t now = now_ms();                    /* after receiving: everything heard is in the past */
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        Link *l = &N.links[i];
        if (!l->used) continue;
        if (!l->welcomed) {
            if (since(now, l->started) > TIMEOUT_MS) { drop_link(l, "the host did not answer"); continue; }
            /* client UDP: knock until the host answers (TCP said hello once connected) */
            if (!N.host && N.mode == NET_UDP && since(now, l->last_send) >= HELLO_MS) send_simple(l, P_HELLO, -1);
            continue;
        }
        if (since(now, l->last_recv) > TIMEOUT_MS) drop_link(l, N.host ? "" : "lost the connection to the host");
    }

    if (since(now, N.last_round) >= SEND_MS) {
        N.last_round = now;
        if (N.dirty) { N.tick++; N.dirty = false; }
        for (int i = 0; i < NET_MAX_PEERS; i++) {
            Link *l = &N.links[i];
            if (!l->used || !l->welcomed) continue;
            if (l->nout || slots_pending(l) || l->sent_ack != l->in_seq || l->sent_msg_ack != l->msg_recv
                || since(now, l->last_send) >= KEEPALIVE_MS)
                send_state(l);
        }
    }
    if (N.mode == NET_TCP)
        for (int i = 0; i < NET_MAX_PEERS; i++)
            if (N.links[i].used && N.links[i].wlen) tcp_flush(&N.links[i]);
}

void net_shutdown(void) {
    if (N.mode) {
        for (int i = 0; i < NET_MAX_PEERS; i++) {
            Link *l = &N.links[i];
            if (!l->used) continue;
            if (l->welcomed || (N.mode == NET_TCP && !l->connecting)) {
                send_simple(l, P_BYE, -1);
                if (N.mode == NET_TCP) tcp_flush(l);
            }
            link_reset(l);
        }
        if (N.sock != BAD_SOCK) sock_close(N.sock);
    }
    N.sock = BAD_SOCK;
    N.mode = 0;
    N.host = false;
    N.status = ST_OFF;
    N.my_id = -1;
    N.peers = 0;
    N.tick = 0;
    N.dirty = false;
    N.in_head = N.in_count = 0;
    N.last_from = -1;
    memset(N.val, 0, sizeof N.val);
    memset(N.ver, 0, sizeof N.ver);
}

static bool resolve(const char *address, int port, struct sockaddr_in *out) {
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, address, &out->sin_addr) == 1) return true;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    if (getaddrinfo(address, NULL, &hints, &res) != 0 || !res) return false;
    out->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

static bool net_start(bool host, const char *address, int port, int mode) {
    net_shutdown();
#ifdef _WIN32
    if (!N.wsa) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { set_err("could not start Winsock"); N.status = ST_LOST; return false; } N.wsa = true; }
#endif
    N.err[0] = 0;
    const char *loss = getenv("GOOBER_NET_LOSS");
    N.loss = loss ? atoi(loss) : 0;
    N.loss_rng = now_ms();
    for (int i = 0; i < NET_MAX_PEERS; i++) link_reset(&N.links[i]);
    struct sockaddr_in addr;
    if (!host && !resolve(address, port, &addr)) { set_err("can't find %s", address); N.status = ST_LOST; return false; }

    sock_t s = socket(AF_INET, mode == NET_UDP ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (s == BAD_SOCK) { set_err("could not open a socket"); N.status = ST_LOST; return false; }
    set_nonblock(s);
    N.mode = mode;
    N.host = host;

    if (host) {
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
        struct sockaddr_in any;
        memset(&any, 0, sizeof any);
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = htonl(INADDR_ANY);
        any.sin_port = htons((uint16_t)port);
        if (bind(s, (struct sockaddr *)&any, sizeof any) != 0 || (mode == NET_TCP && listen(s, 8) != 0)) {
            sock_close(s);
            N.mode = 0;
            set_err("port %d is in use", port);
            N.status = ST_LOST;
            return false;
        }
        N.sock = s;
        N.my_id = 0;
        N.status = ST_CONNECTED;
        return true;
    }

    Link *l = &N.links[0];
    l->used = true;
    l->next_msg = 1;
    l->addr = addr;
    l->started = l->last_recv = now_ms();
    N.status = ST_CONNECTING;
    if (mode == NET_UDP) {
        N.sock = s;
        send_simple(l, P_HELLO, -1);
        return true;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    l->sock = s;
    if (connect(s, (struct sockaddr *)&addr, sizeof addr) == 0) {
        send_simple(l, P_HELLO, -1);
    } else if (E_INPROGRESS(sock_errno())) {
        l->connecting = true;
    } else {
        drop_link(l, "could not connect to the host");
        return false;
    }
    return true;
}

static bool queue_msg(int to, const char *text, int len) {
    Msg m;
    memset(&m, 0, sizeof m);
    m.from = (uint8_t)N.my_id;
    m.to = to < 0 ? TO_ALL : (uint8_t)to;
    m.len = (uint8_t)len;
    memcpy(m.text, text, (size_t)len);
    if (!N.host) {
        Link *l = &N.links[0];
        if (!l->used || !l->welcomed || l->nout == NET_QUEUE) return false;
        m.id = l->next_msg++;
        l->out[l->nout++] = m;
        return true;
    }
    if (to > 0 && (to >= NET_MAX_PEERS || !N.links[to].used || !N.links[to].welcomed)) return false;
    for (int i = 1; i < NET_MAX_PEERS; i++) {   /* room for it everywhere it goes? */
        Link *l = &N.links[i];
        if (l->used && l->welcomed && (to < 0 || to == i) && l->nout == NET_QUEUE) return false;
    }
    relay_or_keep(&m);
    return true;
}

#endif /* NET_STUB */

/* ---- the commands ---- */

#define ARG_I(n) (args[n].as.i)
#define ARG_F(n) (args[n].as.f)
#define ARG_S(n) (str_chars(args[n].as.s))

static int need_mode(VM *vm, const char *what, int argc, Value *args, int at) {
    int mode = argc > at ? ARG_I(at) : NET_TCP;
    if (mode != NET_TCP && mode != NET_UDP) vm_error(vm, "%s: mode must be NET_TCP or NET_UDP, got %d", what, mode);
    return mode;
}
static void need_port(VM *vm, const char *what, int port) {
    if (port < 1 || port > 65535) vm_error(vm, "%s: port must be 1-65535, got %d", what, port);
}
static int need_slot(VM *vm, const char *what, int slot) {
    if (slot < 0 || slot >= NET_SLOTS) vm_error(vm, "%s: slot %d out of range 0-%d", what, slot, NET_SLOTS - 1);
    return slot;
}

static Value n_host(VM *vm, int argc, Value *args) {
    need_port(vm, "net_host", ARG_I(0));
    return v_int(net_start(true, NULL, ARG_I(0), need_mode(vm, "net_host", argc, args, 1)));
}
static Value n_join(VM *vm, int argc, Value *args) {
    need_port(vm, "net_join", ARG_I(1));
    return v_int(net_start(false, ARG_S(0), ARG_I(1), need_mode(vm, "net_join", argc, args, 2)));
}
static Value n_close(VM *vm, int argc, Value *args) { (void)vm; (void)argc; (void)args; net_shutdown(); N.err[0] = 0; return v_int(0); }
static Value n_update(VM *vm, int argc, Value *args) { (void)vm; (void)argc; (void)args; net_poll(); return v_int(0); }
static Value n_status(VM *vm, int argc, Value *args) { (void)vm; (void)argc; (void)args; net_poll(); return v_int(N.status); }
static Value n_error(VM *vm, int argc, Value *args) { (void)argc; (void)args; return v_str(str_from_c(vm, N.err)); }
static Value n_is_host(VM *vm, int argc, Value *args) { (void)vm; (void)argc; (void)args; return v_int(N.host && N.status == ST_CONNECTED); }
static Value n_id(VM *vm, int argc, Value *args) { (void)vm; (void)argc; (void)args; return v_int(N.status == ST_CONNECTED ? N.my_id : -1); }

static uint8_t current_peers(void) {
    if (N.status != ST_CONNECTED) return 0;
#ifndef NET_STUB
    if (N.host) return peers_mask();
#endif
    return N.peers;
}
static Value n_peers(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc; (void)args;
    int n = 0;
    for (uint8_t m = current_peers(); m; m &= (uint8_t)(m - 1)) n++;
    return v_int(n);
}
static Value n_peer(VM *vm, int argc, Value *args) {
    (void)vm; (void)argc;
    int id = ARG_I(0);
    return v_int(id >= 0 && id < NET_MAX_PEERS && (current_peers() >> id & 1));
}

static Value n_set(VM *vm, int argc, Value *args) {
    (void)argc;
    int i = need_slot(vm, "net_set", ARG_I(0));
    double v = ARG_F(1);
    if (N.val[i] == v && N.ver[i]) return v_int(0);   /* unchanged: nothing to send */
    N.val[i] = v;
    if (N.mode) { N.ver[i] = N.tick + 1; N.dirty = true; }
    return v_int(0);
}
static Value n_get(VM *vm, int argc, Value *args) {
    (void)argc;
    return v_float(N.val[need_slot(vm, "net_get", ARG_I(0))]);
}

static Value n_send(VM *vm, int argc, Value *args) {
    int len = str_len(args[0].as.s);
    int to = argc > 1 ? ARG_I(1) : -1;
    if (len < 1 || len > NET_MSG_MAX) vm_error(vm, "net_send: a message is 1-%d characters, got %d", NET_MSG_MAX, len);
    if (to < -1 || to >= NET_MAX_PEERS) vm_error(vm, "net_send: player %d? (0-%d, or NET_ALL)", to, NET_MAX_PEERS - 1);
    if (N.status != ST_CONNECTED || to == N.my_id) return v_int(0);
    return v_int(queue_msg(to, ARG_S(0), len));
}
static Value n_messages(VM *vm, int argc, Value *args) { (void)vm; (void)argc; (void)args; net_poll(); return v_int(N.in_count); }
static Value n_recv(VM *vm, int argc, Value *args) {
    (void)argc; (void)args;
    net_poll();
    if (!N.in_count) { N.last_from = -1; return v_str(NULL); }
    Msg *m = &N.inbox[N.in_head];
    N.in_head = (N.in_head + 1) % NET_INBOX;
    N.in_count--;
    N.last_from = m->from;
    return v_str(str_new(vm, m->text, m->len));
}
static Value n_from(VM *vm, int argc, Value *args) { (void)vm; (void)argc; (void)args; return v_int(N.last_from); }

NativeDef gb_net_natives[] = {
    {"net_host", 'i', "i|i", n_host},
    {"net_join", 'i', "si|i", n_join},
    {"net_close", 'v', "", n_close},
    {"net_update", 'v', "", n_update},
    {"net_status", 'i', "", n_status},
    {"net_error", 's', "", n_error},
    {"net_is_host", 'i', "", n_is_host},
    {"net_id", 'i', "", n_id},
    {"net_peers", 'i', "", n_peers},
    {"net_peer", 'i', "i", n_peer},
    {"net_set", 'v', "if", n_set},
    {"net_get", 'f', "i", n_get},
    {"net_send", 'i', "s|i", n_send},
    {"net_messages", 'i', "", n_messages},
    {"net_recv", 's', "", n_recv},
    {"net_from", 'i', "", n_from},
    {NULL, 0, NULL, NULL},
};

ConstDef gb_net_consts[] = {
    {"NET_TCP", NET_TCP}, {"NET_UDP", NET_UDP},
    {"NET_OFF", ST_OFF}, {"NET_CONNECTING", ST_CONNECTING}, {"NET_CONNECTED", ST_CONNECTED}, {"NET_LOST", ST_LOST},
    {"NET_ALL", -1}, {"NET_SLOTS", NET_SLOTS}, {"NET_MAX_PLAYERS", NET_MAX_PEERS},
    {NULL, 0},
};
