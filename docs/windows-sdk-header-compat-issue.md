# Windows SDK Header Compatibility Gap

## Summary

`ncc` should be able to compile n00b translation units that include Windows-native dependency headers without requiring n00b-side replacement headers for common WinSock and CRT/POSIX compatibility names.

During the native Windows n00b build, n00b translation units include picoquic public headers. Picoquic can build on Windows natively, but its public headers choose the Windows include path only when `_WINDOWS` is defined:

```c
#ifdef _WINDOWS
#include <WS2tcpip.h>
#include <Ws2def.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif
```

The n00b compile currently does not define `_WINDOWS`, so picoquic falls into the POSIX branch and native Windows fails on missing headers such as `arpa/inet.h`. Defining `_WINDOWS` is the correct dependency-level direction, but then `ncc`-parsed translation units must tolerate the real Windows SDK headers pulled in by `winsock2.h` and `ws2tcpip.h`.

## Observed Symptoms

With n00b built by `..\ncc\build-msvc\ncc.exe` through `n00b\build.ps1`, the build reaches `src/net/quic/endpoint.c` and fails while preprocessing picoquic headers:

```text
subprojects\picoquic\picoquic\picoquic.h:32:10: fatal error: 'arpa/inet.h' file not found
```

Earlier n00b Windows bring-up already avoided real SDK headers in `ncc`-parsed socket translation units by using `include/internal/win32_sockets.h`, because Windows SDK headers expose Clang-MSVC dialect details that `ncc` does not fully parse yet, such as SDK pointer-qualifier and intrinsic-token surfaces.

## Suggested Compile-Only Repro

Create `C:\tmp\ncc_windows_sdk_repro.c`:

```c
#define _WINDOWS 1
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

int main(void)
{
    WSADATA wsa;
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
}
```

From the `ncc` repo, compile it without linking:

```powershell
.\build-msvc\ncc.exe -D_WINDOWS -D_WIN32_WINNT=0x0601 -c C:\tmp\ncc_windows_sdk_repro.c -o C:\tmp\ncc_windows_sdk_repro.obj
```

Expected result: `ncc` accepts the normal Windows SDK WinSock header path and emits an object file.

If this direct SDK repro passes, try the dependency-facing shape that n00b hits by compiling a TU that includes picoquic's public header with `_WINDOWS` defined:

```c
#define _WINDOWS 1
#include "picoquic.h"
int main(void) { return PICOQUIC_ERROR_CLASS == 0x400 ? 0 : 1; }
```

Compile that with the same `ncc.exe -c` command plus an include path pointing at `picoquic\picoquic`. The n00b failure comes from this public-header include path being reached inside n00b-owned translation units.

## Desired ncc Fix

`ncc` should support the relevant Clang-MSVC Windows SDK header dialect well enough that n00b can define `_WINDOWS` and allow picoquic/picotls public headers to include the real Windows SDK headers normally.

At minimum, investigate and cover:

- Windows SDK pointer qualifiers and annotations encountered through `winsock2.h`, `ws2tcpip.h`, and related headers.
- Clang-MSVC intrinsic/vector tokens reached through SDK/UCRT include chains.
- MSVC integer literal suffixes and macro spellings that appear after preprocessing.
- Normal Windows dependency macros such as `_WIN32` plus `_WINDOWS` in ncc-driven dependency builds.

## Temporary n00b Workaround

The n00b side can work around this by defining `_WINDOWS` and providing project-owned shim headers for Windows socket include names, forwarding to `include/internal/win32_sockets.h`. That should be treated as temporary. Once `ncc` can parse the Windows SDK headers directly, those n00b shims should be removed and picoquic/picotls should use their normal Windows include path.
