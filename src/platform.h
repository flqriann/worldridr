#pragma once
// Plattform-Anpassungen: unter Windows WinSock/Win32, unter Linux BSD-Sockets und POSIX.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int sockl_t;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
typedef int SOCKET;
typedef socklen_t sockl_t;
typedef uint32_t DWORD;
typedef int BOOL;
typedef unsigned long u_long;
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#define INVALID_SOCKET (-1)
#define closesocket close
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAECONNRESET ECONNRESET
#define WSAGetLastError() errno
#define MAKEWORD(a, b) 0
struct WSADATA { int dummy; };
inline int WSAStartup(int, WSADATA*) { return 0; }
#define FIONBIO 0
inline int ioctlsocket(SOCKET s, int, u_long* nb) { int fl = fcntl(s, F_GETFL, 0); return fcntl(s, F_SETFL, *nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)); }
#endif
