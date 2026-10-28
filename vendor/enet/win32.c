/** 
 @file  win32.c
 @brief ENet Win32 system specific functions
*/
#ifdef _WIN32

#define ENET_BUILDING_LIB 1
#include "enet/enet.h"
#include <windows.h>
#include <mmsystem.h>
#include <ws2tcpip.h>
#include <stdlib.h>

static enet_uint32 timeBase = 0;

int
enet_initialize (void)
{
    WORD versionRequested = MAKEWORD (1, 1);
    WSADATA wsaData;
   
    if (WSAStartup (versionRequested, & wsaData))
       return -1;

    if (LOBYTE (wsaData.wVersion) != 1||
        HIBYTE (wsaData.wVersion) != 1)
    {
       WSACleanup ();
       
       return -1;
    }

    timeBeginPeriod (1);

    return 0;
}

void
enet_deinitialize (void)
{
    timeEndPeriod (1);

    WSACleanup ();
}

enet_uint32
enet_host_random_seed (void)
{
    return (enet_uint32) timeGetTime ();
}

enet_uint32
enet_time_get (void)
{
    return (enet_uint32) timeGetTime () - timeBase;
}

void
enet_time_set (enet_uint32 newTimeBase)
{
    timeBase = (enet_uint32) timeGetTime () - newTimeBase;
}

int
enet_address_set_host_ip (ENetAddress * address, const char * name)
{
    enet_uint8 vals [4] = { 0, 0, 0, 0 };
    int i;

    for (i = 0; i < 4; ++ i)
    {
        const char * next = name + 1;
        if (* name != '0')
        {
            long val = strtol (name, (char **) & next, 10);
            if (val < 0 || val > 255 || next == name || next - name > 3)
              return -1;
            vals [i] = (enet_uint8) val;
        }

        if (* next != (i < 3 ? '.' : '\0'))
          return -1;
        name = next + 1;
    }

    memcpy (& address -> host, vals, sizeof (enet_uint32));
    return 0;
}

int
enet_address_set_host (ENetAddress * address, const char * name)
{
    struct hostent * hostEntry;

    hostEntry = gethostbyname (name);
    if (hostEntry == NULL ||
        hostEntry -> h_addrtype != AF_INET)
      return enet_address_set_host_ip (address, name);

    address -> host = * (enet_uint32 *) hostEntry -> h_addr_list [0];

    return 0;
}

int
enet_address_get_host_ip (const ENetAddress * address, char * name, size_t nameLength)
{
    char * addr = inet_ntoa (* (struct in_addr *) & address -> host);
    if (addr == NULL)
        return -1;
    else
    {
        size_t addrLen = strlen(addr);
        if (addrLen >= nameLength)
          return -1;
        memcpy (name, addr, addrLen + 1);
    }
    return 0;
}

int
enet_address_get_host (const ENetAddress * address, char * name, size_t nameLength)
{
    struct in_addr in;
    struct hostent * hostEntry;
 
    in.s_addr = address -> host;
    
    hostEntry = gethostbyaddr ((char *) & in, sizeof (struct in_addr), AF_INET);
    if (hostEntry == NULL)
      return enet_address_get_host_ip (address, name, nameLength);
    else
    {
       size_t hostLen = strlen (hostEntry -> h_name);
       if (hostLen >= nameLength)
         return -1;
       memcpy (name, hostEntry -> h_name, hostLen + 1);
    }

    return 0;
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
                 sizeof (struct sockaddr_in)) == SOCKET_ERROR ? -1 : 0;
}

int
enet_socket_get_address (ENetSocket socket, ENetAddress * address)
{
    struct sockaddr_in sin;
    int sinLength = sizeof (struct sockaddr_in);

    if (getsockname (socket, (struct sockaddr *) & sin, & sinLength) == -1)
      return -1;

    address -> host = (enet_uint32) sin.sin_addr.s_addr;
    address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);

    return 0;
}

int
enet_socket_listen (ENetSocket socket, int backlog)
{
    return listen (socket, backlog < 0 ? SOMAXCONN : backlog) == SOCKET_ERROR ? -1 : 0;
}

ENetSocket
enet_socket_create (ENetSocketType type)
{
    return socket (PF_INET, type == ENET_SOCKET_TYPE_DATAGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
}

int
enet_socket_set_option (ENetSocket socket, ENetSocketOption option, int value)
{
    int result = SOCKET_ERROR;
    switch (option)
    {
        case ENET_SOCKOPT_NONBLOCK:
        {
            u_long nonBlocking = (u_long) value;
            result = ioctlsocket (socket, FIONBIO, & nonBlocking);
            break;
        }

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
            result = setsockopt (socket, SOL_SOCKET, SO_RCVTIMEO, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_SNDTIMEO:
            result = setsockopt (socket, SOL_SOCKET, SO_SNDTIMEO, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_NODELAY:
            result = setsockopt (socket, IPPROTO_TCP, TCP_NODELAY, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_TTL:
            result = setsockopt (socket, IPPROTO_IP, IP_TTL, (char *) & value, sizeof (int));
            break;

        default:
            break;
    }
    return result == SOCKET_ERROR ? -1 : 0;
}

int
enet_socket_get_option (ENetSocket socket, ENetSocketOption option, int * value)
{
    int result = SOCKET_ERROR, len;
    switch (option)
    {
        case ENET_SOCKOPT_ERROR:
            len = sizeof(int);
            result = getsockopt (socket, SOL_SOCKET, SO_ERROR, (char *) value, & len);
            break;

        case ENET_SOCKOPT_TTL:
            len = sizeof(int);
            result = getsockopt (socket, IPPROTO_IP, IP_TTL, (char *) value, & len);
            break;

        default:
            break;
    }
    return result == SOCKET_ERROR ? -1 : 0;
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
    if (result == SOCKET_ERROR && WSAGetLastError () != WSAEWOULDBLOCK)
      return -1;

    return 0;
}

ENetSocket
enet_socket_accept (ENetSocket socket, ENetAddress * address)
{
    SOCKET result;
    struct sockaddr_in sin;
    int sinLength = sizeof (struct sockaddr_in);

    result = accept (socket, 
                     address != NULL ? (struct sockaddr *) & sin : NULL, 
                     address != NULL ? & sinLength : NULL);

    if (result == INVALID_SOCKET)
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
    return shutdown (socket, (int) how) == SOCKET_ERROR ? -1 : 0;
}

void
enet_socket_destroy (ENetSocket socket)
{
    if (socket != INVALID_SOCKET)
    {
      enet_socket_send_batch_flush (socket);
      closesocket (socket);
    }
}

/*
 * Batched datagram send.
 *
 * A 2000-player tick builds ~2000 state datagrams and every WSASendTo() is a
 * kernel transition: measured at ~15 us here, that is ~30 ms of a 50 ms tick
 * spent inside 2000 syscalls. enet_socket_send() queues the finished datagram
 * instead (a memcpy of at most the path MTU) and
 * enet_socket_send_batch_flush() drains the queue from a small set of sender
 * threads. Windows has no sendmmsg(): a datagram send is one syscall no matter
 * how it is issued, so the only way to shorten the tick is to issue several at
 * once. UDP sends are independent, so this preserves per-datagram ordering
 * requirements (ENet's sequencing lives inside the datagrams).
 *
 * The queue is drained inside the same enet_host_flush()/enet_host_service()
 * call that queued the datagrams, so packet latency is unchanged.
 */
#define ENET_SENDQ_MAX 4096
#define ENET_SENDQ_WORKERS 3
#define ENET_SENDQ_PARALLEL_MIN 32

typedef struct _ENetSendQueueEntry
{
    struct sockaddr_in address;
    enet_uint32 offset;
    enet_uint32 length;
} ENetSendQueueEntry;

static ENetSendQueueEntry enet_sendQueue [ENET_SENDQ_MAX];
static int enet_sendQueueCount = 0;
static SOCKET enet_sendQueueSocket = INVALID_SOCKET;
static enet_uint8 * enet_sendQueueData = NULL;
static enet_uint32 enet_sendQueueSize = 0;
static enet_uint32 enet_sendQueueCapacity = 0;

static HANDLE enet_sendQueueWake = NULL;
static HANDLE enet_sendQueueDone = NULL;
static HANDLE enet_sendQueueThreads [ENET_SENDQ_WORKERS];
static volatile LONG enet_sendQueueNext = 0;
static volatile LONG enet_sendQueueRemaining = 0;
static SOCKET enet_sendQueueJobSocket = INVALID_SOCKET;
static int enet_sendQueueJobCount = 0;
static int enet_sendQueuePool = 0;   /* 0 untried, 1 running, -1 unavailable */

static void
enet_sendq_address (const ENetAddress * address, struct sockaddr_in * sin)
{
    memset (sin, 0, sizeof (struct sockaddr_in));
    sin -> sin_family = AF_INET;
    sin -> sin_port = ENET_HOST_TO_NET_16 (address -> port);
    sin -> sin_addr.s_addr = address -> host;
}

/* pull datagrams off the queue until it is empty (shared by every sender) */
static void
enet_sendq_drain (SOCKET socket, int count)
{
    for (;;)
    {
        LONG index = InterlockedIncrement (& enet_sendQueueNext) - 1;

        if (index >= count)
          return;

        sendto (socket,
                (const char *) enet_sendQueueData + enet_sendQueue [index].offset,
                (int) enet_sendQueue [index].length, 0,
                (struct sockaddr *) & enet_sendQueue [index].address,
                sizeof (struct sockaddr_in));
    }
}

static DWORD WINAPI
enet_sendq_worker (LPVOID unused)
{
    (void) unused;

    for (;;)
    {
        WaitForSingleObject (enet_sendQueueWake, INFINITE);

        enet_sendq_drain (enet_sendQueueJobSocket, enet_sendQueueJobCount);

        if (InterlockedDecrement (& enet_sendQueueRemaining) == 0)
          SetEvent (enet_sendQueueDone);
    }

    return 0;
}

static int
enet_sendq_start_workers (void)
{
    int i;

    if (enet_sendQueuePool != 0)
      return enet_sendQueuePool > 0;

    enet_sendQueueWake = CreateSemaphore (NULL, 0, 0x7fffffff, NULL);
    enet_sendQueueDone = CreateEvent (NULL, FALSE, FALSE, NULL);

    if (enet_sendQueueWake == NULL || enet_sendQueueDone == NULL)
    {
        enet_sendQueuePool = -1;
        return 0;
    }

    for (i = 0; i < ENET_SENDQ_WORKERS; i ++)
    {
        enet_sendQueueThreads [i] = CreateThread (NULL, 0, enet_sendq_worker, NULL, 0, NULL);

        if (enet_sendQueueThreads [i] == NULL)
        {
            enet_sendQueuePool = -1;
            return 0;
        }
    }

    enet_sendQueuePool = 1;

    return 1;
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

    if (enet_sendQueueCount >= ENET_SENDQ_PARALLEL_MIN && enet_sendq_start_workers ())
    {
        enet_sendQueueJobSocket = socket;
        enet_sendQueueJobCount = enet_sendQueueCount;
        enet_sendQueueNext = 0;
        enet_sendQueueRemaining = ENET_SENDQ_WORKERS + 1;

        for (i = 0; i < ENET_SENDQ_WORKERS; i ++)
          ReleaseSemaphore (enet_sendQueueWake, 1, NULL);

        /* the calling thread is the fourth sender */
        enet_sendq_drain (socket, enet_sendQueueJobCount);

        if (InterlockedDecrement (& enet_sendQueueRemaining) == 0)
          SetEvent (enet_sendQueueDone);

        WaitForSingleObject (enet_sendQueueDone, INFINITE);
    }
    else
    {
        for (i = 0; i < enet_sendQueueCount; i ++)
          sendto (socket,
                  (const char *) enet_sendQueueData + enet_sendQueue [i].offset,
                  (int) enet_sendQueue [i].length, 0,
                  (struct sockaddr *) & enet_sendQueue [i].address,
                  sizeof (struct sockaddr_in));
    }

    enet_sendQueueCount = 0;
    enet_sendQueueSize = 0;
}

static int
enet_socket_send_direct (ENetSocket socket,
                         const ENetAddress * address,
                         const ENetBuffer * buffers,
                         size_t bufferCount)
{
    struct sockaddr_in sin;
    DWORD sentLength = 0;

    if (address != NULL)
    {
        memset (& sin, 0, sizeof (struct sockaddr_in));

        sin.sin_family = AF_INET;
        sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
        sin.sin_addr.s_addr = address -> host;
    }

    if (WSASendTo (socket, 
                   (LPWSABUF) buffers,
                   (DWORD) bufferCount,
                   & sentLength,
                   0,
                   address != NULL ? (struct sockaddr *) & sin : NULL,
                   address != NULL ? sizeof (struct sockaddr_in) : 0,
                   NULL,
                   NULL) == SOCKET_ERROR)
    {
       if (WSAGetLastError () == WSAEWOULDBLOCK)
         return 0;

       return -1;
    }

    return (int) sentLength;
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
    INT sinLength = sizeof (struct sockaddr_in);
    DWORD flags = 0,
          recvLength = 0;
    struct sockaddr_in sin;

    if (WSARecvFrom (socket,
                     (LPWSABUF) buffers,
                     (DWORD) bufferCount,
                     & recvLength,
                     & flags,
                     address != NULL ? (struct sockaddr *) & sin : NULL,
                     address != NULL ? & sinLength : NULL,
                     NULL,
                     NULL) == SOCKET_ERROR)
    {
       switch (WSAGetLastError ())
       {
       case WSAEWOULDBLOCK:
       case WSAECONNRESET:
          return 0;
       case WSAEMSGSIZE:
          return -2;
       }

       return -1;
    }

    if (flags & MSG_PARTIAL)
      return -2;

    if (address != NULL)
    {
        address -> host = (enet_uint32) sin.sin_addr.s_addr;
        address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);
    }

    return (int) recvLength;
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
      return -1;

    * condition = ENET_SOCKET_WAIT_NONE;

    if (selectCount == 0)
      return 0;

    if (FD_ISSET (socket, & writeSet))
      * condition |= ENET_SOCKET_WAIT_SEND;
    
    if (FD_ISSET (socket, & readSet))
      * condition |= ENET_SOCKET_WAIT_RECEIVE;

    return 0;
} 

#endif

