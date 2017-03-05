#include "net/tcp.h"
#include <cassert>
#include <cstring>
#include <winsock2.h>
#include "net/internal/init.h"
#include "net/resolver.h"

namespace net {

static unsigned long kBlockingMode = 0;
static unsigned long kNonBlockingMode = 1;

error ConnectTCP(const std::string& host, uint16_t port, int64_t timeoutMilliseconds,
        std::shared_ptr<TCPSocket>* clientSock) {
    if (clientSock == nullptr) {
        assert(0 && "clientSock must not be nullptr");
        return error::illegal_argument;
    }

    internal::init();

    std::string ipAddr;
    error luErr = LookupAddress(host, &ipAddr);
    if (luErr != error::nil) {
        return luErr;
    }

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        return toError(WSAGetLastError());
    }

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.S_un.S_addr = inet_addr(ipAddr.c_str());

    if (timeoutMilliseconds <= 0) { // connect in blocking mode
        if (connect(fd, (struct sockaddr*) &serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            int err = WSAGetLastError();
            closesocket(fd);
            return toError(err);
        }
        *clientSock = std::make_shared<TCPSocket>(fd, ipAddr);
        return error::nil;
    }

    ioctlsocket(fd, FIONBIO, &kNonBlockingMode);
    if (connect(fd, (struct sockaddr*) &serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            closesocket(fd);
            return toError(err);
        }
    }

    fd_set writefds;
    fd_set exceptfds;
    FD_ZERO(&writefds);
    FD_ZERO(&exceptfds);
    FD_SET(fd, &writefds);
    FD_SET(fd, &exceptfds);

    struct timeval connTimeout;
    timeoutMilliseconds = (timeoutMilliseconds > 0) ? timeoutMilliseconds : 0;
    connTimeout.tv_sec = timeoutMilliseconds / 1000;
    connTimeout.tv_usec = timeoutMilliseconds % 1000 * 1000;

    int connErr = 0;
    int result = select(0, nullptr, &writefds, &exceptfds, &connTimeout);
    if (result == SOCKET_ERROR) {
        connErr = WSAGetLastError();
        goto fail;
    } else if (result == 0) {
        connErr = WSAETIMEDOUT;
        goto fail;
    } else if (!FD_ISSET(fd, &exceptfds)) {
        connErr = 0;
    } else {
        int soErr = 0;
        int optlen = sizeof(soErr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*) &soErr, &optlen);
        if (soErr != 0) {
            connErr = soErr;
            goto fail;
        }
    }
    if (connErr == 0) {
        ioctlsocket(fd, FIONBIO, &kBlockingMode);
        *clientSock = std::make_shared<TCPSocket>(fd, ipAddr);
        return error::nil;
    }

fail:
    closesocket(fd);
    return toError(connErr);
}

error ListenTCP(uint16_t port, std::unique_ptr<TCPListener>* serverSock) {
    if (serverSock == nullptr) {
        assert(0 && "serverSock must not be nullptr");
        return error::illegal_argument;
    }

    internal::init();

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        return toError(WSAGetLastError());
    }

    BOOL enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*) &enabled, sizeof(enabled)) == SOCKET_ERROR) {
        goto fail;
    }

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr*) &serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        goto fail;
    }

    if (listen(fd, SOMAXCONN) == SOCKET_ERROR) {
        goto fail;
    } else {
        *serverSock = std::unique_ptr<TCPListener>(new TCPListener(fd));
        return error::nil;
    }

fail:
    int err = WSAGetLastError();
    closesocket(fd);
    return toError(err);
}

TCPSocket::~TCPSocket() {
    Close();
}

error TCPSocket::Close() {
    if (m_closed) {
        return error::nil;
    }

    if (closesocket(m_fd) == SOCKET_ERROR) {
        return toError(WSAGetLastError());
    }
    m_closed = true;
    return error::nil;
}

bool TCPSocket::IsClosed() {
    return m_closed;
}

error TCPSocket::Read(char* buf, size_t len, int* nbytes) {
    if (m_closed) {
        assert(0 && "Already closed");
        return error::illegal_state;
    }

    int size = recv(m_fd, buf, len, 0);
    if (size == SOCKET_ERROR) {
        return toError(WSAGetLastError());
    }
    if (size == 0) {
        return error::eof;
    }
    if (nbytes != nullptr) {
        *nbytes = size;
    }
    return error::nil;
}

error TCPSocket::Write(const char* buf, size_t len, int* nbytes) {
    if (m_closed) {
        assert(0 && "Already closed");
        return error::illegal_state;
    }

    int size = send(m_fd, buf, len, 0);
    if (size == SOCKET_ERROR) {
        return toError(WSAGetLastError());
    }
    if (nbytes != nullptr) {
        *nbytes = size;
    }
    return error::nil;
}

error TCPSocket::SetSocketTimeout(int64_t timeoutMilliseconds) {
    if (m_closed) {
        assert(0 && "Already closed");
        return error::illegal_state;
    }

    DWORD soTimeout = (DWORD) ((timeoutMilliseconds > 0) ? timeoutMilliseconds : 0);

    if (setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*) &soTimeout, sizeof(soTimeout)) == SOCKET_ERROR) {
        return toError(WSAGetLastError());
    }
    if (setsockopt(m_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*) &soTimeout, sizeof(soTimeout)) == SOCKET_ERROR) {
        return toError(WSAGetLastError());
    }
    return error::nil;
}

TCPListener::~TCPListener() {
    Close();
}

error TCPListener::Close() {
    if (m_closed) {
        return error::nil;
    }

    if (closesocket(m_fd) == SOCKET_ERROR) {
        return toError(WSAGetLastError());
    }
    m_closed = true;
    return error::nil;
}

bool TCPListener::IsClosed() {
    return m_closed;
}

error TCPListener::Accept(std::shared_ptr<TCPSocket>* clientSock) {
    if (clientSock == nullptr) {
        assert(0 && "clientSock must not be nullptr");
        return error::illegal_argument;
    }

    struct sockaddr_in clientAddr;
    int len = sizeof(clientAddr);

    SOCKET clientFD = accept(m_fd, (struct sockaddr*) &clientAddr, &len);
    if (clientFD == INVALID_SOCKET) {
        return toError(WSAGetLastError());
    }

    *clientSock = std::make_shared<TCPSocket>(clientFD, inet_ntoa(clientAddr.sin_addr));
    return error::nil;
}

} // namespace net
