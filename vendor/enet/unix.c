/** 
 @file  unix.c
 @brief ENet Unix system specific functions
*/
#ifndef _WIN32

/* sendmmsg() is only declared with _GNU_SOURCE; the batch send below needs it */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define ENET_BUILDING_LIB 1
#include "enet/enet.h"

#ifdef __APPLE__
#ifdef HAS_POLL
#undef HAS_POLL
#endif
#ifndef HAS_FCNTL
#define HAS_FCNTL 1
#endif
#ifndef HAS_INET_PTON
#define HAS_INET_PTON 1
#endif
#ifndef HAS_INET_NTOP
#define HAS_INET_NTOP 1
#endif
#ifndef HAS_MSGHDR_FLAGS
#define HAS_MSGHDR_FLAGS 1
#endif
#ifndef HAS_SOCKLEN_T
#define HAS_SOCKLEN_T 1
#endif
#ifndef HAS_GETADDRINFO
#define HAS_GETADDRINFO 1
#endif
#ifndef HAS_GETNAMEINFO
#define HAS_GETNAMEINFO 1
#endif
#endif

#ifdef HAS_FCNTL
#include <fcntl.h>
#endif

#ifdef HAS_POLL
#include <poll.h>
#endif

#if !defined(HAS_SOCKLEN_T) && !defined(__socklen_t_defined)
typedef int socklen_t;
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static enet_uint32 timeBase = 0;

int
enet_initialize (void)
{
    return 0;
}

void
enet_deinitialize (void)
{
}

enet_uint32
enet_host_random_seed (void)
{
    return (enet_uint32) time (NULL);
}

enet_uint32
enet_time_get (void)
{
    struct timeval timeVal;

    gettimeofday (& timeVal, NULL);

    return timeVal.tv_sec * 1000 + timeVal.tv_usec / 1000 - timeBase;
}

void
enet_time_set (enet_uint32 newTimeBase)
{
    struct timeval timeVal;

    gettimeofday (& timeVal, NULL);
    
    timeBase = timeVal.tv_sec * 1000 + timeVal.tv_usec / 1000 - newTimeBase;
}

int
enet_address_set_host_ip (ENetAddress * address, const char * name)
{
#ifdef HAS_INET_PTON
    if (! inet_pton (AF_INET, name, & address -> host))
#else
    if (! inet_aton (name, (struct in_addr *) & address -> host))
#endif
        return -1;

    return 0;
}

int
enet_address_set_host (ENetAddress * address, const char * name)
{
#ifdef HAS_GETADDRINFO
    struct addrinfo hints, * resultList = NULL, * result = NULL;

    memset (& hints, 0, sizeof (hints));
    hints.ai_family = AF_INET;

    if (getaddrinfo (name, NULL, NULL, & resultList) != 0)
      return -1;

    for (result = resultList; result != NULL; result = result -> ai_next)
    {
        if (result -> ai_family == AF_INET && result -> ai_addr != NULL && result -> ai_addrlen >= sizeof (struct sockaddr_in))
        {
            struct sockaddr_in * sin = (struct sockaddr_in *) result -> ai_addr;

            address -> host = sin -> sin_addr.s_addr;

            freeaddrinfo (resultList);

            return 0;
        }
    }

    if (resultList != NULL)
      freeaddrinfo (resultList);
#else
    struct hostent * hostEntry = NULL;
#ifdef HAS_GETHOSTBYNAME_R
    struct hostent hostData;
    char buffer [2048];
    int errnum;

#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__) || defined(__GNU__)
    gethostbyname_r (name, & hostData, buffer, sizeof (buffer), & hostEntry, & errnum);
#else
    hostEntry = gethostbyname_r (name, & hostData, buffer, sizeof (buffer), & errnum);
#endif
#else
    hostEntry = gethostbyname (name);
#endif

    if (hostEntry != NULL && hostEntry -> h_addrtype == AF_INET)
    {
        address -> host = * (enet_uint32 *) hostEntry -> h_addr_list [0];

        return 0;
    }
#endif

    return enet_address_set_host_ip (address, name);
}

int
enet_address_get_host_ip (const ENetAddress * address, char * name, size_t nameLength)
{
#ifdef HAS_INET_NTOP
    if (inet_ntop (AF_INET, & address -> host, name, nameLength) == NULL)
#else
    char * addr = inet_ntoa (* (struct in_addr *) & address -> host);
    if (addr != NULL)
    {
        size_t addrLen = strlen(addr);
        if (addrLen >= nameLength)
          return -1;
        memcpy (name, addr, addrLen + 1);
    } 
    else
#endif
        return -1;
    return 0;
}

int
enet_address_get_host (const ENetAddress * address, char * name, size_t nameLength)
{
#ifdef HAS_GETNAMEINFO
    struct sockaddr_in sin;
    int err;

    memset (& sin, 0, sizeof (struct sockaddr_in));

    sin.sin_family = AF_INET;
    sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
    sin.sin_addr.s_addr = address -> host;

    err = getnameinfo ((struct sockaddr *) & sin, sizeof (sin), name, nameLength, NULL, 0, NI_NAMEREQD);
    if (! err)
    {
        if (name != NULL && nameLength > 0 && ! memchr (name, '\0', nameLength))
          return -1;
        return 0;
    }
    if (err != EAI_NONAME)
      return -1;
#else
    struct in_addr in;
    struct hostent * hostEntry = NULL;
#ifdef HAS_GETHOSTBYADDR_R
    struct hostent hostData;
    char buffer [2048];
    int errnum;

    in.s_addr = address -> host;

#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__) || defined(__GNU__)
    gethostbyaddr_r ((char *) & in, sizeof (struct in_addr), AF_INET, & hostData, buffer, sizeof (buffer), & hostEntry, & errnum);
#else
    hostEntry = gethostbyaddr_r ((char *) & in, sizeof (struct in_addr), AF_INET, & hostData, buffer, sizeof (buffer), & errnum);
#endif
#else
    in.s_addr = address -> host;

    hostEntry = gethostbyaddr ((char *) & in, sizeof (struct in_addr), AF_INET);
#endif

    if (hostEntry != NULL)
    {
       size_t hostLen = strlen (hostEntry -> h_name);
       if (hostLen >= nameLength)
         return -1;
       memcpy (name, hostEntry -> h_name, hostLen + 1);
       return 0;
    }
#endif

    return enet_address_get_host_ip (address, name, nameLength);
}

int
enet_socket_bind (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in sin;

    memset (& sin, 0, sizeof (struct sockaddr_in));

    sin.sin_family = AF_INET;

    if (address != NULL)
    {
       sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
       sin.sin_addr.s_addr = address -> host;
    }
    else
    {
       sin.sin_port = 0;
       sin.sin_addr.s_addr = INADDR_ANY;
    }

    return bind (socket,
                 (struct sockaddr *) & sin,
                 sizeof (struct sockaddr_in)); 
}

int
enet_socket_get_address (ENetSocket socket, ENetAddress * address)
{
    struct sockaddr_in sin;
    socklen_t sinLength = sizeof (struct sockaddr_in);

    if (getsockname (socket, (struct sockaddr *) & sin, & sinLength) == -1)
      return -1;

    address -> host = (enet_uint32) sin.sin_addr.s_addr;
    address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);

    return 0;
}

int 
enet_socket_listen (ENetSocket socket, int backlog)
{
    return listen (socket, backlog < 0 ? SOMAXCONN : backlog);
}

ENetSocket
enet_socket_create (ENetSocketType type)
{
    return socket (PF_INET, type == ENET_SOCKET_TYPE_DATAGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
}

int
enet_socket_set_option (ENetSocket socket, ENetSocketOption option, int value)
{
    int result = -1;
    switch (option)
    {
        case ENET_SOCKOPT_NONBLOCK:
#ifdef HAS_FCNTL
            result = fcntl (socket, F_SETFL, (value ? O_NONBLOCK : 0) | (fcntl (socket, F_GETFL) & ~O_NONBLOCK));
#else
            result = ioctl (socket, FIONBIO, & value);
#endif
            break;

        case ENET_SOCKOPT_BROADCAST:
            result = setsockopt (socket, SOL_SOCKET, SO_BROADCAST, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_REUSEADDR:
            result = setsockopt (socket, SOL_SOCKET, SO_REUSEADDR, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_RCVBUF:
            result = setsockopt (socket, SOL_SOCKET, SO_RCVBUF, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_SNDBUF:
            result = setsockopt (socket, SOL_SOCKET, SO_SNDBUF, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_RCVTIMEO:
        {
            struct timeval timeVal;
            timeVal.tv_sec = value / 1000;
            timeVal.tv_usec = (value % 1000) * 1000;
            result = setsockopt (socket, SOL_SOCKET, SO_RCVTIMEO, (char *) & timeVal, sizeof (struct timeval));
            break;
        }

        case ENET_SOCKOPT_SNDTIMEO:
        {
            struct timeval timeVal;
            timeVal.tv_sec = value / 1000;
            timeVal.tv_usec = (value % 1000) * 1000;
            result = setsockopt (socket, SOL_SOCKET, SO_SNDTIMEO, (char *) & timeVal, sizeof (struct timeval));
            break;
        }

        case ENET_SOCKOPT_NODELAY:
            result = setsockopt (socket, IPPROTO_TCP, TCP_NODELAY, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_TTL:
            result = setsockopt (socket, IPPROTO_IP, IP_TTL, (char *) & value, sizeof (int));
            break;

        default:
            break;
    }
    return result == -1 ? -1 : 0;
}

int
enet_socket_get_option (ENetSocket socket, ENetSocketOption option, int * value)
{
    int result = -1;
    socklen_t len;
    switch (option)
    {
        case ENET_SOCKOPT_ERROR:
            len = sizeof (int);
            result = getsockopt (socket, SOL_SOCKET, SO_ERROR, value, & len);
            break;

        case ENET_SOCKOPT_TTL:
            len = sizeof (int);
            result = getsockopt (socket, IPPROTO_IP, IP_TTL, (char *) value, & len);
            break;

        default:
            break;
    }
    return result == -1 ? -1 : 0;
}

int
enet_socket_connect (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in sin;
    int result;

    memset (& sin, 0, sizeof (struct sockaddr_in));

    sin.sin_family = AF_INET;
    sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
    sin.sin_addr.s_addr = address -> host;

    result = connect (socket, (struct sockaddr *) & sin, sizeof (struct sockaddr_in));
    if (result == -1 && errno == EINPROGRESS)
      return 0;

    return result;
}

ENetSocket
enet_socket_accept (ENetSocket socket, ENetAddress * address)
{
    int result;
    struct sockaddr_in sin;
    socklen_t sinLength = sizeof (struct sockaddr_in);

    result = accept (socket, 
                     address != NULL ? (struct sockaddr *) & sin : NULL, 
                     address != NULL ? & sinLength : NULL);
    
    if (result == -1)
      return ENET_SOCKET_NULL;

    if (address != NULL)
    {
        address -> host = (enet_uint32) sin.sin_addr.s_addr;
        address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);
    }

    return result;
} 
    
int
enet_socket_shutdown (ENetSocket socket, ENetSocketShutdown how)
{
    return shutdown (socket, (int) how);
}

void
enet_socket_destroy (ENetSocket socket)
{
    if (socket != -1)
    {
      enet_socket_send_batch_flush (socket);
      close (socket);
    }
}

/*
 * Batched datagram send.
 *
 * A 2000-player tick builds ~2000 state datagrams, and every sendmsg() is a
 * syscall: measured at ~7 us each that is 14 ms of pure syscall time per tick
 * on Linux, more on Windows. enet_socket_send() therefore queues the finished
 * datagram (a memcpy of at most the path MTU) and
 * enet_socket_send_batch_flush() hands the whole queue to sendmmsg(), which
 * sends many datagrams per syscall. The queue is drained inside the same
 * enet_host_flush()/enet_host_service() call that queued the datagrams, so
 * latency is unchanged; only the number of syscalls drops.
 *
 * The queue is process-global but drained before another socket is queued, so
 * it is safe for the single-host-per-thread programs this library is used by
 * (client, server, master, bot).
 */
#define ENET_SENDQ_MAX 4096

typedef struct _ENetSendQueueEntry
{
    struct sockaddr_in address;
    enet_uint32 offset;
    enet_uint32 length;
} ENetSendQueueEntry;

static ENetSendQueueEntry enet_sendQueue [ENET_SENDQ_MAX];
static int enet_sendQueueCount = 0;
static ENetSocket enet_sendQueueSocket = -1;
static enet_uint8 * enet_sendQueueData = NULL;
static enet_uint32 enet_sendQueueSize = 0;
static enet_uint32 enet_sendQueueCapacity = 0;

static void
enet_sendq_address (const ENetAddress * address, struct sockaddr_in * sin)
{
    memset (sin, 0, sizeof (struct sockaddr_in));
    sin -> sin_family = AF_INET;
    sin -> sin_port = ENET_HOST_TO_NET_16 (address -> port);
    sin -> sin_addr.s_addr = address -> host;
}

/* returns the queued length, or -1 when the datagram has to be sent directly */
static int
enet_sendq_push (ENetSocket socket,
                 const ENetAddress * address,
                 const ENetBuffer * buffers,
                 size_t bufferCount)
{
    size_t total = 0, i, at;
    enet_uint8 * destination;

    if (address == NULL || buffers == NULL || bufferCount == 0)
      return -1;

    for (i = 0; i < bufferCount; i ++)
      total += buffers [i].dataLength;

    if (total == 0)
      return -1;

    if (enet_sendQueueSocket != socket ||
        enet_sendQueueCount >= ENET_SENDQ_MAX ||
        enet_sendQueueSize + total > enet_sendQueueCapacity)
      enet_socket_send_batch_flush (socket);

    if (enet_sendQueueSize + total > enet_sendQueueCapacity)
    {
        enet_uint32 capacity = enet_sendQueueCapacity ? enet_sendQueueCapacity : (1u << 20);
        enet_uint8 * grown;

        while (capacity < enet_sendQueueSize + total)
          capacity <<= 1;

        grown = (enet_uint8 *) realloc (enet_sendQueueData, capacity);
        if (grown == NULL)
          return -1;

        enet_sendQueueData = grown;
        enet_sendQueueCapacity = capacity;
    }

    destination = enet_sendQueueData + enet_sendQueueSize;
    at = 0;
    for (i = 0; i < bufferCount; i ++)
    {
        if (buffers [i].dataLength > 0)
          memcpy (destination + at, buffers [i].data, buffers [i].dataLength);

        at += buffers [i].dataLength;
    }

    enet_sendq_address (address, & enet_sendQueue [enet_sendQueueCount].address);
    enet_sendQueue [enet_sendQueueCount].offset = enet_sendQueueSize;
    enet_sendQueue [enet_sendQueueCount].length = (enet_uint32) total;
    enet_sendQueueSocket = socket;
    enet_sendQueueSize += (enet_uint32) total;
    enet_sendQueueCount ++;

    return (int) total;
}

void
enet_socket_send_batch_flush (ENetSocket socket)
{
    int i;

    if (enet_sendQueueCount == 0)
      return;

    if (enet_sendQueueSocket != socket)
    {
        /* another socket queued in between: never send to the wrong one */
        enet_sendQueueCount = 0;
        enet_sendQueueSize = 0;
        return;
    }

#if defined(__linux__)
    {
        static struct mmsghdr * messages = NULL;
        static struct iovec * vectors = NULL;
        static int allocated = 0;

        if (allocated < enet_sendQueueCount)
        {
            struct mmsghdr * grownMessages = (struct mmsghdr *) realloc (messages, (size_t) enet_sendQueueCount * sizeof (struct mmsghdr));
            struct iovec * grownVectors = (struct iovec *) realloc (vectors, (size_t) enet_sendQueueCount * sizeof (struct iovec));

            if (grownMessages != NULL && grownVectors != NULL)
            {
                messages = grownMessages;
                vectors = grownVectors;
                allocated = enet_sendQueueCount;
            }
        }

        if (allocated >= enet_sendQueueCount)
        {
            int sent = 0;

            for (i = 0; i < enet_sendQueueCount; i ++)
            {
                vectors [i].iov_base = enet_sendQueueData + enet_sendQueue [i].offset;
                vectors [i].iov_len = enet_sendQueue [i].length;

                memset (& messages [i], 0, sizeof (struct mmsghdr));
                messages [i].msg_hdr.msg_name = & enet_sendQueue [i].address;
                messages [i].msg_hdr.msg_namelen = sizeof (struct sockaddr_in);
                messages [i].msg_hdr.msg_iov = & vectors [i];
                messages [i].msg_hdr.msg_iovlen = 1;
            }

            /* sendmmsg() reports how many datagrams it took, not bytes */
            while (sent < enet_sendQueueCount)
            {
                int sentNow = sendmmsg (socket, messages + sent,
                                        (unsigned int) (enet_sendQueueCount - sent), MSG_NOSIGNAL);

                if (sentNow <= 0)
                  break;

                sent += sentNow;
            }

            enet_sendQueueCount = 0;
            enet_sendQueueSize = 0;

            return;
        }
    }
#endif

    /* non-Linux hosts, or the mmsghdr allocation failed */
    for (i = 0; i < enet_sendQueueCount; i ++)
      sendto (socket, enet_sendQueueData + enet_sendQueue [i].offset, enet_sendQueue [i].length,
              MSG_NOSIGNAL, (struct sockaddr *) & enet_sendQueue [i].address,
              sizeof (struct sockaddr_in));

    enet_sendQueueCount = 0;
    enet_sendQueueSize = 0;
}

static int
enet_socket_send_direct (ENetSocket socket,
                         const ENetAddress * address,
                         const ENetBuffer * buffers,
                         size_t bufferCount)
{
    struct msghdr msgHdr;
    struct sockaddr_in sin;
    int sentLength;

    memset (& msgHdr, 0, sizeof (struct msghdr));

    if (address != NULL)
    {
        memset (& sin, 0, sizeof (struct sockaddr_in));

        sin.sin_family = AF_INET;
        sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
        sin.sin_addr.s_addr = address -> host;

        msgHdr.msg_name = & sin;
        msgHdr.msg_namelen = sizeof (struct sockaddr_in);
    }

    msgHdr.msg_iov = (struct iovec *) buffers;
    msgHdr.msg_iovlen = bufferCount;

    sentLength = sendmsg (socket, & msgHdr, MSG_NOSIGNAL);
    
    if (sentLength == -1)
    {
       if (errno == EWOULDBLOCK)
         return 0;

       return -1;
    }

    return sentLength;
}

int
enet_socket_send (ENetSocket socket,
                  const ENetAddress * address,
                  const ENetBuffer * buffers,
                  size_t bufferCount)
{
    int queued = enet_sendq_push (socket, address, buffers, bufferCount);

    if (queued >= 0)
      return queued;

    return enet_socket_send_direct (socket, address, buffers, bufferCount);
}

int
enet_socket_receive (ENetSocket socket,
                     ENetAddress * address,
                     ENetBuffer * buffers,
                     size_t bufferCount)
{
    struct msghdr msgHdr;
    struct sockaddr_in sin;
    int recvLength;

    memset (& msgHdr, 0, sizeof (struct msghdr));

    if (address != NULL)
    {
        msgHdr.msg_name = & sin;
        msgHdr.msg_namelen = sizeof (struct sockaddr_in);
    }

    msgHdr.msg_iov = (struct iovec *) buffers;
    msgHdr.msg_iovlen = bufferCount;

    recvLength = recvmsg (socket, & msgHdr, MSG_NOSIGNAL);

    if (recvLength == -1)
    {
       if (errno == EWOULDBLOCK)
         return 0;

       return -1;
    }

#ifdef HAS_MSGHDR_FLAGS
    if (msgHdr.msg_flags & MSG_TRUNC)
      return -2;
#endif

    if (address != NULL)
    {
        address -> host = (enet_uint32) sin.sin_addr.s_addr;
        address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);
    }

    return recvLength;
}

int
enet_socketset_select (ENetSocket maxSocket, ENetSocketSet * readSet, ENetSocketSet * writeSet, enet_uint32 timeout)
{
    struct timeval timeVal;

    timeVal.tv_sec = timeout / 1000;
    timeVal.tv_usec = (timeout % 1000) * 1000;

    return select (maxSocket + 1, readSet, writeSet, NULL, & timeVal);
}

int
enet_socket_wait (ENetSocket socket, enet_uint32 * condition, enet_uint32 timeout)
{
#ifdef HAS_POLL
    struct pollfd pollSocket;
    int pollCount;
    
    pollSocket.fd = socket;
    pollSocket.events = 0;

    if (* condition & ENET_SOCKET_WAIT_SEND)
      pollSocket.events |= POLLOUT;

    if (* condition & ENET_SOCKET_WAIT_RECEIVE)
      pollSocket.events |= POLLIN;

    pollCount = poll (& pollSocket, 1, timeout);

    if (pollCount < 0)
    {
        if (errno == EINTR && * condition & ENET_SOCKET_WAIT_INTERRUPT)
        {
            * condition = ENET_SOCKET_WAIT_INTERRUPT;

            return 0;
        }

        return -1;
    }

    * condition = ENET_SOCKET_WAIT_NONE;

    if (pollCount == 0)
      return 0;

    if (pollSocket.revents & POLLOUT)
      * condition |= ENET_SOCKET_WAIT_SEND;
    
    if (pollSocket.revents & POLLIN)
      * condition |= ENET_SOCKET_WAIT_RECEIVE;

    return 0;
#else
    fd_set readSet, writeSet;
    struct timeval timeVal;
    int selectCount;

    timeVal.tv_sec = timeout / 1000;
    timeVal.tv_usec = (timeout % 1000) * 1000;

    FD_ZERO (& readSet);
    FD_ZERO (& writeSet);

    if (* condition & ENET_SOCKET_WAIT_SEND)
      FD_SET (socket, & writeSet);

    if (* condition & ENET_SOCKET_WAIT_RECEIVE)
      FD_SET (socket, & readSet);

    selectCount = select (socket + 1, & readSet, & writeSet, NULL, & timeVal);

    if (selectCount < 0)
    {
        if (errno == EINTR && * condition & ENET_SOCKET_WAIT_INTERRUPT)
        {
            * condition = ENET_SOCKET_WAIT_INTERRUPT;

            return 0;
        }
      
        return -1;
    }

    * condition = ENET_SOCKET_WAIT_NONE;

    if (selectCount == 0)
      return 0;

    if (FD_ISSET (socket, & writeSet))
      * condition |= ENET_SOCKET_WAIT_SEND;

    if (FD_ISSET (socket, & readSet))
      * condition |= ENET_SOCKET_WAIT_RECEIVE;

    return 0;
#endif
}

#endif

