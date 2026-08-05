#define _WINDOWS 1
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

int
ncc_windows_sdk_winsock_smoke(void)
{
    WSADATA wsa;
    struct addrinfo hints = {0};

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    return (int)sizeof(wsa) + hints.ai_family + hints.ai_socktype;
}
