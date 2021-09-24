#include <net_system_structs.h>
#include <unix_internal.h>
#include <filesystem.h>
#include <socket.h>

declare_closure_struct(1, 0, void, sharedbuf_free,
    struct sharedbuf *, shb);

declare_closure_struct(1, 0, void, unixsock_free,
    struct unixsock *, s);

struct sockaddr_un {
    u16 sun_family;
    char sun_path[108];
};

typedef struct sharedbuf {
    buffer b;
    struct refcount refcount;
    closure_struct(sharedbuf_free, free);
    struct sockaddr_un from_addr;
} *sharedbuf;

#define UNIXSOCK_BUF_MAX_SIZE   PAGESIZE
#define UNIXSOCK_QUEUE_MAX_LEN  64

typedef struct unixsock {
    struct sock sock; /* must be first */
    queue data;
    filesystem fs;
    tuple fs_entry;
    struct sockaddr_un local_addr;
    queue conn_q;
    boolean connecting;
    struct unixsock *peer;
    closure_struct(unixsock_free, free);
    struct refcount refcount;
} *unixsock;

static inline void sharedbuf_deallocate(sharedbuf shb)
{
    heap h = shb->b->h;
    deallocate_buffer(shb->b);
    deallocate(h, shb, sizeof(*shb));
}

define_closure_function(1, 0, void, sharedbuf_free,
                        sharedbuf, shb)
{
    sharedbuf_deallocate(bound(shb));
}

define_closure_function(1, 0, void, unixsock_free,
                        unixsock, s)
{
    unixsock s = bound(s);
    deallocate(s->sock.h, s, sizeof(*s));
}

static inline sharedbuf sharedbuf_allocate(heap h, u64 len)
{
    sharedbuf shb = allocate(h, sizeof(*shb));
    if (shb == INVALID_ADDRESS)
        return shb;
    shb->b = allocate_buffer(h, len);
    if (shb->b == INVALID_ADDRESS) {
        deallocate(h, shb, sizeof(*shb));
        return INVALID_ADDRESS;
    }
    init_closure(&shb->free, sharedbuf_free, shb);
    init_refcount(&shb->refcount, 1, (thunk)&shb->free);
    return shb;
}

static inline void sharedbuf_reserve(sharedbuf shb)
{
    refcount_reserve(&shb->refcount);
}

static inline void sharedbuf_release(sharedbuf shb)
{
    refcount_release(&shb->refcount);
}

/* A socket is in connecting state when connect() has been called but the
 * connection has not yet been accepted by the peer. */
static inline boolean unixsock_is_connecting(unixsock s)
{
    return (s->connecting && !s->peer);
}

static inline boolean unixsock_is_connected(unixsock s)
{
    return (!s->connecting && s->peer);
}

static unixsock unixsock_alloc(heap h, int type, u32 flags);

static void unixsock_dealloc(unixsock s)
{
    if (s->sock.type == SOCK_DGRAM && s->peer)
        refcount_release(&s->peer->refcount);
    deallocate_queue(s->data);
    s->data = 0;
    deallocate_closure(s->sock.f.read);
    deallocate_closure(s->sock.f.write);
    deallocate_closure(s->sock.f.events);
    deallocate_closure(s->sock.f.close);
    socket_deinit(&s->sock);
    refcount_release(&s->refcount);
}

static inline void unixsock_notify_reader(unixsock s)
{
    blockq_wake_one(s->sock.rxbq);
    fdesc_notify_events(&s->sock.f);
}

static inline void unixsock_notify_writer(unixsock s)
{
    blockq_wake_one(s->sock.txbq);
    fdesc_notify_events(&s->sock.f);
}

closure_function(8, 1, sysreturn, unixsock_read_bh,
                 unixsock, s, thread, t, void *, dest, sg_list, sg, u64, length, io_completion, completion, struct sockaddr_un *, from_addr, socklen_t *, from_length,
                 u64, flags)
{
    unixsock s = bound(s);
    void *dest = bound(dest);
    u64 length = bound(length);

    sharedbuf shb;
    sysreturn rv;

    if ((flags & BLOCKQ_ACTION_NULLIFY) && (s->peer || s->sock.type == SOCK_DGRAM)) {
        rv = -ERESTARTSYS;
        goto out;
    }
    shb = queue_peek(s->data);
    if (shb == INVALID_ADDRESS) {
        if (s->sock.type == SOCK_STREAM && !s->peer) {
            rv = 0;
            goto out;
        } else if (s->sock.f.flags & SOCK_NONBLOCK) {
            rv = -EAGAIN;
            goto out;
        }
        return BLOCKQ_BLOCK_REQUIRED;
    }
    rv = 0;
    do {
        buffer b = shb->b;
        u64 xfer = MIN(buffer_length(b), length);
        if (dest) {
            buffer_read(b, dest, xfer);
            dest = (u8 *)dest + xfer;
        } else if (xfer > 0) {
            sg_buf sgb = sg_list_tail_add(bound(sg), xfer);
            if (!sgb)
                break;
            sharedbuf_reserve(shb);
            sgb->buf = buffer_ref(b, 0);
            sgb->size = xfer;
            sgb->offset = 0;
            sgb->refcount = &shb->refcount;
            buffer_consume(b, xfer);
        }
        rv += xfer;
        length -= xfer;
        if (!buffer_length(b) || (s->sock.type == SOCK_DGRAM)) {
            assert(dequeue(s->data) == shb);
            if (s->sock.type == SOCK_DGRAM) {
                struct sockaddr_un *from_addr = bound(from_addr);
                socklen_t *from_length = bound(from_length);
                if (from_addr && from_length) {
                    runtime_memcpy(from_addr, &shb->from_addr, MIN(*from_length, sizeof(shb->from_addr)));
                    *from_length = __builtin_offsetof(struct sockaddr_un, sun_path) + runtime_strlen(from_addr->sun_path) + 1;
                }
            }
            sharedbuf_release(shb);
            shb = queue_peek(s->data);
            if (shb == INVALID_ADDRESS) { /* no more data available to read */
                fdesc_notify_events(&s->sock.f);
                break;
            }
        }
    } while ((s->sock.type == SOCK_STREAM) && (length > 0));
    if (s->peer) {
        unixsock_notify_writer(s->peer);
    }
out:
    blockq_handle_completion(s->sock.rxbq, flags, bound(completion), bound(t),
            rv);
    closure_finish();
    return rv;
}

static sysreturn unixsock_read_with_addr(unixsock s, void *dest, u64 length, u64 offset_arg, thread t, boolean bh, io_completion completion, void *addr, socklen_t *addrlen)
{
    if ((s->sock.type == SOCK_STREAM) && (length == 0))
        return io_complete(completion, t, 0);

    blockq_action ba = closure(s->sock.h, unixsock_read_bh, s, t, dest, 0, length,
            completion, addr, addrlen);
    return blockq_check(s->sock.rxbq, t, ba, bh);
}

closure_function(1, 6, sysreturn, unixsock_read,
                 unixsock, s,
                 void *, dest, u64, length, u64, offset_arg, thread, t, boolean, bh, io_completion, completion)
{
    return unixsock_read_with_addr(bound(s), dest, length, offset_arg, t, bh, completion, 0, 0);
}

static sysreturn unixsock_write_check(unixsock s, u64 len)
{
    if ((s->sock.type == SOCK_STREAM) && (len == 0))
        return 0;
    if ((s->sock.type == SOCK_DGRAM) && (len > UNIXSOCK_BUF_MAX_SIZE))
        return -EMSGSIZE;
    return 1;   /* any value > 0 will do */
}

static sysreturn unixsock_write_to(void *src, sg_list sg, u64 length,
                                   unixsock dest, unixsock from)
{
    if (queue_full(dest->data)) {
        return -EAGAIN;
    }

    sysreturn rv = 0;
    do {
        u64 xfer = MIN(UNIXSOCK_BUF_MAX_SIZE, length);
        sharedbuf shb = sharedbuf_allocate(dest->sock.h, xfer);
        if (shb == INVALID_ADDRESS) {
            if (rv == 0) {
                rv = -ENOMEM;
            }
            break;
        }
        if (from && from->sock.type == SOCK_DGRAM)
            runtime_memcpy(&shb->from_addr, &from->local_addr, sizeof(struct sockaddr_un));
        if (src) {
            assert(buffer_write(shb->b, src, xfer));
            src = (u8 *) src + xfer;
        } else {
            u64 len = sg_copy_to_buf(buffer_ref(shb->b, 0), sg, xfer);
            assert(len == xfer);
            buffer_produce(shb->b, xfer);
        }
        assert(enqueue(dest->data, shb));
        rv += xfer;
        length -= xfer;
    } while ((length > 0) && !queue_full(dest->data));
    if ((rv > 0) || ((rv == 0) && (dest->sock.type == SOCK_DGRAM))) {
        unixsock_notify_reader(dest);
    }
    return rv;
}

static int lookup_socket(unixsock *s, char *path)
{
    process p = current->p;
    fs_status fss = filesystem_get_socket(p->cwd_fs, p->cwd, path, (void **)s);
    if (fss == FS_STATUS_INVAL)
        return -ECONNREFUSED;
    return sysreturn_from_fs_status(fss);
}

closure_function(8, 1, sysreturn, unixsock_write_bh,
                 unixsock, s, thread, t, void *, src, sg_list, sg, u64, length, io_completion, completion, struct sockaddr_un *, addr, socklen_t, addrlen,
                 u64, flags)
{
    unixsock s = bound(s);
    void *src = bound(src);
    u64 length = bound(length);
    unixsock dest;

    sysreturn rv;

    if ((flags & BLOCKQ_ACTION_NULLIFY) && (s->peer || s->sock.type == SOCK_DGRAM)) {
        rv = -ERESTARTSYS;
        goto out;
    }
    if (s->sock.type == SOCK_STREAM) {
        if (!s->peer) {
            rv = -EPIPE;
            goto out;
        }
    }
    dest = s->peer;
    if (s->sock.type == SOCK_DGRAM) {
        struct sockaddr_un *addr = bound(addr);
        if (addr && bound(addrlen)) {
            if (bound(addrlen) < sizeof(struct sockaddr_un) ||
                addr->sun_family != AF_UNIX) {
                rv = -EINVAL;
                goto out;
            }
            struct sockaddr_un daddr;
            runtime_memcpy(&daddr, addr, sizeof(daddr));
            daddr.sun_path[sizeof(daddr.sun_path)-1] = 0;
            rv = lookup_socket(&dest, daddr.sun_path);
            if (rv != 0)
                goto out;
        } else if (!dest || !dest->data) {
            rv = -ENOTCONN;
            goto out;
        }
    }

    rv = unixsock_write_to(src, bound(sg), length, dest, s);
    if ((rv == -EAGAIN) && !(s->sock.f.flags & SOCK_NONBLOCK)) {
        return BLOCKQ_BLOCK_REQUIRED;
    }
    if (queue_full(dest->data)) { /* no more space available to write */
        fdesc_notify_events(&s->sock.f);
    }
out:
    blockq_handle_completion(s->sock.txbq, flags, bound(completion), bound(t),
            rv);
    closure_finish();
    return rv;
}

static sysreturn unixsock_write_with_addr(unixsock s, void *src, u64 length, u64 offset, thread t, boolean bh, io_completion completion, struct sockaddr_un *addr, socklen_t addrlen)
{
    sysreturn rv = unixsock_write_check(s, length);
    if (rv <= 0)
        return io_complete(completion, t, rv);

    blockq_action ba = closure(s->sock.h, unixsock_write_bh, s, t, src, 0, length,
            completion, addr, addrlen);
    return blockq_check(s->sock.txbq, t, ba, bh);
}

closure_function(1, 6, sysreturn, unixsock_write,
                 unixsock, s,
                 void *, src, u64, length, u64, offset, thread, t, boolean, bh, io_completion, completion)
{
    return unixsock_write_with_addr(bound(s), src, length, offset, t, bh, completion, 0, 0);
}

closure_function(1, 6, sysreturn, unixsock_sg_read,
                 unixsock, s,
                 sg_list, sg, u64, length, u64, offset, thread, t, boolean, bh, io_completion, completion)
{
    unixsock s = bound(s);
    blockq_action ba = closure(s->sock.h, unixsock_read_bh, s, t, 0, sg, length,
        completion, 0, 0);
    if (ba == INVALID_ADDRESS)
        return io_complete(completion, t, -ENOMEM);
    return blockq_check(s->sock.rxbq, t, ba, bh);
}

closure_function(1, 6, sysreturn, unixsock_sg_write,
                 unixsock, s,
                 sg_list, sg, u64, length, u64, offset, thread, t, boolean, bh, io_completion, completion)
{
    unixsock s = bound(s);
    sysreturn rv = unixsock_write_check(s, length);
    if (rv <= 0)
        return io_complete(completion, t, rv);
    blockq_action ba = closure(s->sock.h, unixsock_write_bh, s, t, 0, sg, length,
        completion, 0, 0);
    if (ba == INVALID_ADDRESS)
        return io_complete(completion, t, -ENOMEM);
    return blockq_check(s->sock.txbq, t, ba, bh);
}

closure_function(1, 1, u32, unixsock_events,
                 unixsock, s,
                 thread, t /* ignore */)
{
    unixsock s = bound(s);
    u32 events = 0;
    if (s->conn_q) {    /* listening state */
        if (!queue_empty(s->conn_q)) {
            events |= EPOLLIN;
        }
    } else if (s->connecting) {
        if (s->peer) {
            /* An ongoing connection attempt has been accepted by the peer. */
            events |= EPOLLOUT;
        }
    } else {
        if (!queue_empty(s->data)) {
            events |= EPOLLIN;
        }
        if (s->sock.type == SOCK_DGRAM || (s->peer && !queue_full(s->peer->data))) {
            events |= EPOLLOUT;
        }
        if (!s->peer && s->sock.type != SOCK_DGRAM) {
            events |= EPOLLHUP;
        }
    }
    return events;
}

closure_function(1, 2, sysreturn, unixsock_ioctl,
                 unixsock, s,
                 unsigned long, request, vlist, ap)
{
    unixsock s = bound(s);
    return socket_ioctl(&s->sock, request, ap);
}

closure_function(1, 2, sysreturn, unixsock_close,
                 unixsock, s,
                 thread, t, io_completion, completion)
{
    unixsock s = bound(s);
    if (s->peer) {
        s->peer->peer = 0;
        if (s->peer->data) {
            socket_flush_q(&s->peer->sock);
            fdesc_notify_events(&s->peer->sock.f);
        }
    }
    if (s->conn_q) {
        /* Notify any connecting sockets that connection is being refused. */
        unixsock child;
        while ((child = dequeue(s->conn_q)) != INVALID_ADDRESS) {
            child->connecting = false;
            socket_flush_q(&child->sock);
        }

        deallocate_queue(s->conn_q);
    }
    if (s->fs_entry) {
        filesystem_clear_socket(s->fs, s->fs_entry);
    }
    unixsock_dealloc(s);
    return io_complete(completion, t, 0);
}

static sysreturn unixsock_bind(struct sock *sock, struct sockaddr *addr,
        socklen_t addrlen)
{
    unixsock s = (unixsock) sock;
    struct sockaddr_un *unixaddr = (struct sockaddr_un *) addr;
    sysreturn ret;
    if (s->fs_entry) {
        ret = -EADDRINUSE;
        goto out;
    }

    if (addrlen < sizeof(unixaddr->sun_family)) {
        ret = -EINVAL;
        goto out;
    }

    if (addrlen > sizeof(*unixaddr)) {
        ret = -ENAMETOOLONG;
        goto out;
    }

    /* Ensure that the NULL-terminated path string fits in unixaddr->sun_path
     * (add terminator character if not found). */
    int term;
    for (term = 1; term < addrlen - sizeof(unixaddr->sun_family); term++) {
        if (unixaddr->sun_path[term] == '\0') {
            break;
        }
    }
    if (term == addrlen - sizeof(unixaddr->sun_family)) {
        /* Terminator character not found: add it if possible. */
        if (addrlen == sizeof(*unixaddr)) {
            ret = -ENAMETOOLONG;
            goto out;
        }
        /* TODO: is this string not const? */
        unixaddr->sun_path[term] = '\0';
    }

    process p = current->p;
    s->fs = p->cwd_fs;
    fs_status fss = filesystem_mk_socket(&s->fs, p->cwd, unixaddr->sun_path, s, &s->fs_entry);
    if (fss != FS_STATUS_OK) {
        ret = (fss == FS_STATUS_EXIST) ? -EADDRINUSE : sysreturn_from_fs_status(fss);
        goto out;
    }
    runtime_memcpy(&s->local_addr, addr, addrlen);
    ret = 0;
out:
    socket_release(sock);
    return ret;
}

static sysreturn unixsock_listen(struct sock *sock, int backlog)
{
    unixsock s = (unixsock) sock;
    sysreturn ret = 0;
    switch (sock->type) {
    case SOCK_STREAM:
        if (!s->conn_q) {
            s->conn_q = allocate_queue(sock->h, backlog);
            if (s->conn_q == INVALID_ADDRESS) {
                msg_err("failed to allocate connection queue\n");
                s->conn_q = 0;
                ret = -ENOMEM;
            }
        }
        break;
    default:
        ret = -EOPNOTSUPP;
    }
    socket_release(sock);
    return ret;
}

closure_function(2, 1, sysreturn, connect_bh,
                 unixsock, s, thread, t,
                 u64, bqflags)
{
    unixsock s = bound(s);
    thread t = bound(t);
    sysreturn rv;

    if ((bqflags & BLOCKQ_ACTION_NULLIFY) && s->connecting) {
        rv = -ERESTARTSYS;
        goto out;
    }
    if (!s->connecting && !s->peer) {   /* the listening socket has been shut down */
        rv = -ECONNREFUSED;
        goto out;
    }
    if (!s->peer) {
        if (s->sock.f.flags & SOCK_NONBLOCK) {
            rv = -EINPROGRESS;
            goto out;
        }
        return BLOCKQ_BLOCK_REQUIRED;
    }
    rv = 0;
out:
    socket_release(&s->sock);
    syscall_return(t, rv);
    closure_finish();
    return rv;
}

static sysreturn unixsock_connect(struct sock *sock, struct sockaddr *addr,
        socklen_t addrlen)
{
    unixsock s = (unixsock) sock;
    sysreturn rv;
    if (unixsock_is_connecting(s)) {
        rv = -EALREADY;
        goto out;
    } else if (unixsock_is_connected(s)) {
        rv = -EISCONN;
        goto out;
    }

    struct sockaddr_un *unixaddr = (struct sockaddr_un *) addr;
    unixsock listener, peer;
    rv = lookup_socket(&listener, unixaddr->sun_path);
    if (rv != 0)
        goto out;
    if (!s->connecting) {
        if (s->sock.type & SOCK_DGRAM) {
            if (!(listener->sock.type == SOCK_DGRAM)) {
                rv = -ECONNREFUSED;
                goto out;
            }
            s->peer = listener;
            refcount_reserve(&listener->refcount);
            rv = 0;
            goto out;
        }
        if (!listener->conn_q || queue_full(listener->conn_q)) {
            rv = -ECONNREFUSED;
            goto out;
        }
        peer = unixsock_alloc(sock->h, sock->type, 0);
        if (!peer) {
            rv = -ENOMEM;
            goto out;
        }

        peer->peer = s;
        assert(enqueue(listener->conn_q, peer));
        s->connecting = true;
        unixsock_notify_reader(listener);
    }
    blockq_action ba = closure(sock->h, connect_bh, s, current);
    return blockq_check(sock->txbq, current, ba, false);
out:
    socket_release(sock);
    return rv;
}

closure_function(5, 1, sysreturn, accept_bh,
                 unixsock, s, thread, t, struct sockaddr *, addr, socklen_t *, addrlen, int, flags,
                 u64, bqflags)
{
    unixsock s = bound(s);
    thread t = bound(t);
    struct sockaddr *addr = bound(addr);
    sysreturn rv;

    if (bqflags & BLOCKQ_ACTION_NULLIFY) {
        rv = -ERESTARTSYS;
        goto out;
    }
    unixsock child = dequeue(s->conn_q);
    if (child == INVALID_ADDRESS) {
        if (s->sock.f.flags & SOCK_NONBLOCK) {
            rv = -EAGAIN;
            goto out;
        }
        return BLOCKQ_BLOCK_REQUIRED;
    }
    if (queue_length(s->conn_q) == 0) {
        fdesc_notify_events(&s->sock.f);
    }
    child->sock.f.flags |= bound(flags);
    rv = child->sock.fd;
    if (addr) {
        socklen_t *addrlen = bound(addrlen);
        socklen_t actual_len = sizeof(child->peer->local_addr.sun_family);
        if (child->peer->local_addr.sun_path[0]) {  /* pathname socket */
            actual_len += runtime_strlen(child->peer->local_addr.sun_path) + 1;
        }
        runtime_memcpy(addr, &child->peer->local_addr,
                MIN(*addrlen, actual_len));
        *addrlen = actual_len;
    }
    child->peer->peer = child;
    child->connecting = false;
    child->peer->connecting = false;
    unixsock_notify_writer(child->peer);
out:
    socket_release(&s->sock);
    syscall_return(t, rv);
    closure_finish();
    return rv;
}

static sysreturn unixsock_accept4(struct sock *sock, struct sockaddr *addr,
        socklen_t *addrlen, int flags)
{
    unixsock s = (unixsock) sock;
    sysreturn rv;
    if (s->sock.type != SOCK_STREAM) {
        rv = -EOPNOTSUPP;
        goto out;
    }
    if (!s->conn_q || (flags & ~(SOCK_NONBLOCK|SOCK_CLOEXEC))) {
        rv = -EINVAL;
        goto out;
    }
    blockq_action ba = closure(sock->h, accept_bh, s, current, addr, addrlen,
            flags);
    return blockq_check(sock->rxbq, current, ba, false);
out:
    socket_release(sock);
    return rv;
}

sysreturn unixsock_sendto(struct sock *sock, void *buf, u64 len, int flags,
        struct sockaddr *dest_addr, socklen_t addrlen)
{
    unixsock s = (unixsock) sock;
    sysreturn rv;
    if (dest_addr || addrlen) {
        if (sock->type == SOCK_STREAM) {
            if (s->peer)
                rv = -EISCONN;
            else
                rv = -EOPNOTSUPP;
            goto out;
        }
        if (!(dest_addr && addrlen)) {
            rv = -EFAULT;
            goto out;
        }
        if (addrlen < sizeof(struct sockaddr_un)) {
            rv = -EINVAL;
            goto out;
        }
    }
    return unixsock_write_with_addr(s, buf, len, 0, current, false,
        (io_completion)&sock->f.io_complete, (struct sockaddr_un *)dest_addr, addrlen);
out:
    socket_release(sock);
    return rv;
}

sysreturn unixsock_recvfrom(struct sock *sock, void *buf, u64 len, int flags,
        struct sockaddr *src_addr, socklen_t *addrlen)
{
    if (src_addr || addrlen) {
        if (!(src_addr && addrlen)) {
            socket_release(sock);
            return -EFAULT;
        }
    }
    return unixsock_read_with_addr((unixsock)sock, buf, len, 0, current, false,
        (io_completion)&sock->f.io_complete, src_addr, addrlen);
}

closure_function(2, 2, void, sendmsg_complete,
                 struct sock *, sock, sg_list, sg,
                 thread, t, sysreturn, rv)
{
    sg_list sg = bound(sg);
    deallocate_sg_list(sg);
    socket_release(bound(sock));
    apply(syscall_io_complete, t, rv);
    closure_finish();
}

sysreturn unixsock_sendmsg(struct sock *sock, const struct msghdr *msg,
        int flags)
{
    sg_list sg = allocate_sg_list();
    sysreturn rv;
    if (sg == INVALID_ADDRESS) {
        rv = -ENOMEM;
        goto out;
    }
    if (!iov_to_sg(sg, msg->msg_iov, msg->msg_iovlen))
        goto err_dealloc_sg;
    io_completion complete = closure(sock->h, sendmsg_complete, sock, sg);
    if (complete == INVALID_ADDRESS)
        goto err_dealloc_sg;
    return apply(sock->f.sg_write, sg, sg->count, 0, current, false, complete);
  err_dealloc_sg:
    deallocate_sg_list(sg);
    rv = -ENOMEM;
  out:
    socket_release(sock);
    return rv;
}

closure_function(4, 2, void, recvmsg_complete,
                 struct sock *, sock, sg_list, sg, struct iovec *, iov, int, iovlen,
                 thread, t, sysreturn, rv)
{
    thread_resume(t);
    sg_list sg = bound(sg);
    sg_to_iov(sg, bound(iov), bound(iovlen));
    deallocate_sg_list(sg);
    socket_release(bound(sock));
    apply(syscall_io_complete, t, rv);
    closure_finish();
}

sysreturn unixsock_recvmsg(struct sock *sock, struct msghdr *msg, int flags)
{
    sg_list sg = allocate_sg_list();
    sysreturn rv;
    if (sg == INVALID_ADDRESS) {
        rv = -ENOMEM;
        goto out;
    }
    io_completion complete = closure(sock->h, recvmsg_complete, sock, sg,
        msg->msg_iov, msg->msg_iovlen);
    if (complete == INVALID_ADDRESS)
        goto err_dealloc_sg;

    /* Non-connected sockets are not supported, so source address is not set. */
    msg->msg_namelen = 0;

    return apply(sock->f.sg_read, sg,
        iov_total_len(msg->msg_iov, msg->msg_iovlen), 0, current, false,
        complete);
  err_dealloc_sg:
    deallocate_sg_list(sg);
    rv = -ENOMEM;
  out:
    socket_release(sock);
    return rv;
}

static unixsock unixsock_alloc(heap h, int type, u32 flags)
{
    unixsock s = allocate(h, sizeof(*s));
    if (s == INVALID_ADDRESS) {
        msg_err("failed to allocate socket structure\n");
        return 0;
    }
    s->data = allocate_queue(h, UNIXSOCK_QUEUE_MAX_LEN);
    if (s->data == INVALID_ADDRESS) {
        msg_err("failed to allocate data buffer\n");
        goto err_queue;
    }
    if (socket_init(current->p, h, AF_UNIX, type, flags, &s->sock) < 0) {
        msg_err("failed to initialize socket\n");
        goto err_socket;
    }
    s->sock.f.read = closure(h, unixsock_read, s);
    s->sock.f.write = closure(h, unixsock_write, s);
    s->sock.f.sg_read = closure(h, unixsock_sg_read, s);
    s->sock.f.sg_write = closure(h, unixsock_sg_write, s);
    s->sock.f.events = closure(h, unixsock_events, s);
    s->sock.f.ioctl = closure(h, unixsock_ioctl, s);
    s->sock.f.close = closure(h, unixsock_close, s);
    s->sock.bind = unixsock_bind;
    s->sock.listen = unixsock_listen;
    s->sock.connect = unixsock_connect;
    s->sock.accept4 = unixsock_accept4;
    s->sock.sendto = unixsock_sendto;
    s->sock.recvfrom = unixsock_recvfrom;
    s->sock.sendmsg = unixsock_sendmsg;
    s->sock.recvmsg = unixsock_recvmsg;
    s->fs_entry = 0;
    s->local_addr.sun_family = AF_UNIX;
    s->local_addr.sun_path[0] = '\0';
    s->conn_q = 0;
    s->connecting = false;
    s->peer = 0;
    init_closure(&s->free, unixsock_free, s);
    init_refcount(&s->refcount, 1, (thunk)&s->free);
    s->sock.fd = allocate_fd(current->p, s);
    if (s->sock.fd == INVALID_PHYSICAL) {
        apply(s->sock.f.close, 0, io_completion_ignore);
        return 0;
    }
    return s;
err_socket:
    deallocate_queue(s->data);
err_queue:
    deallocate(h, s, sizeof(*s));
    return 0;
}

sysreturn unixsock_open(int type, int protocol) {
    unix_heaps uh = get_unix_heaps();
    heap h = heap_general((kernel_heaps)uh);
    unixsock s;

    if (((type & SOCK_TYPE_MASK) != SOCK_STREAM) &&
            ((type & SOCK_TYPE_MASK) != SOCK_DGRAM)) {
        return -ESOCKTNOSUPPORT;
    }
    s = unixsock_alloc(h, type & SOCK_TYPE_MASK, type & ~SOCK_TYPE_MASK);
    if (!s) {
        return -ENOMEM;
    }
    return s->sock.fd;
}

sysreturn socketpair(int domain, int type, int protocol, int sv[2]) {
    unix_heaps uh = get_unix_heaps();
    heap h = heap_general((kernel_heaps)uh);
    unixsock s1, s2;

    if (domain != AF_UNIX) {
        return set_syscall_error(current, EAFNOSUPPORT);
    }
    if (((type & SOCK_TYPE_MASK) != SOCK_STREAM) &&
            ((type & SOCK_TYPE_MASK) != SOCK_DGRAM)) {
        return -ESOCKTNOSUPPORT;
    }
    if (!validate_user_memory(sv, 2 * sizeof(int), true)) {
        return -EFAULT;
    }
    s1 = unixsock_alloc(h, type & SOCK_TYPE_MASK, type & ~SOCK_TYPE_MASK);
    if (!s1) {
        return -ENOMEM;
    }
    s2 = unixsock_alloc(h, type & SOCK_TYPE_MASK, type & ~SOCK_TYPE_MASK);
    if (!s2) {
        unixsock_dealloc(s1);
        return -ENOMEM;
    }
    s1->peer = s2;
    s2->peer = s1;
    if (((type & SOCK_TYPE_MASK) == SOCK_DGRAM)) {
        refcount_reserve(&s1->refcount);
        refcount_reserve(&s2->refcount);
    }
    sv[0] = s1->sock.fd;
    sv[1] = s2->sock.fd;
    return 0;
}
