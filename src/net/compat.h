// src/net/compat.h — minimal POSIX <-> Win32 shims so the net stack (server,
// master, bot) builds on Linux as well as Windows. Windows builds include the
// real headers; POSIX builds get socket/sleep/process shims.
#pragma once

#ifdef _WIN32

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#else

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <strings.h>
#include <csignal>
#include <chrono>
#include <thread>

// MSVC-style localtime_s(&tm, &time_t) -> POSIX localtime_r
static inline struct tm* localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out); }

typedef int SOCKET;
typedef int BOOL;
typedef unsigned long ULONG;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket(s) close(s)
#define ioctlsocket(s, cmd, arg) ioctl((s), (cmd), (arg))
#define ADDR_ANY INADDR_ANY
#define _stricmp strcasecmp

inline void Sleep(unsigned ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

#endif
