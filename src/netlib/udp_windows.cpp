#include "netlib/udp.h"
#include <cassert>
#include <winsock2.h>
#include "netlib/internal/init.h"
#include "netlib/resolver.h"

namespace net {

error ConnectUDP(const std::string& host, uint16_t port, std::shared_ptr<UDPSocket>* clientSock) {
    if (clientSock == nullptr) {
        assert(0 && "clientSock must not be nullptr");
        return error::illegal_argument;
    }

    internal::init();

    std::string remoteAddr;
    error err = LookupAddress(host, &remoteAddr);
    if (err != error::nil) {
        return err;
    }

    SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET) {
        return error::wrap(etype::os, WSAGetLastError());
    }

    *clientSock = std::make_shared<UDPSocket>(fd, std::move(remoteAddr), port);
    return error::nil;
}

error ListenUDP(uint16_t port, std::shared_ptr<UDPSocket>* serverSock) {
    if (serverSock == nullptr) {
        assert(0 && "serverSock must not be nullptr");
        return error::illegal_argument;
    }

    internal::init();

    SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET) {
        return error::wrap(etype::os, WSAGetLastError());
    }

    struct sockaddr_in serverAddr = {0};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*) &serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        closesocket(fd);
        return error::wrap(etype::os, err);
    }

    *serverSock = std::make_shared<UDPSocket>(fd);
    return error::nil;
}

UDPSocket::~UDPSocket() {
    Close();
}

error UDPSocket::Close() {
    if (m_closed) {
        return error::nil;
    }

    if (closesocket(m_fd) == SOCKET_ERROR) {
        return error::wrap(etype::os, WSAGetLastError());
    }
    m_closed = true;
    return error::nil;
}

error UDPSocket::Read(char* buf, size_t len, int* nbytes) {
    if (m_closed) {
        assert(0 && "Already closed");
        return error::illegal_state;
    }

    int size = recv(m_fd, buf, len, 0);
    if (size == SOCKET_ERROR) {
        return error::wrap(etype::os, WSAGetLastError());
    }
    if (size == 0) {
        return error::eof;
    }
    if (nbytes != nullptr) {
        *nbytes = size;
    }
    return error::nil;
}

error UDPSocket::ReadFrom(char* buf, size_t len, int* nbytes,
            std::string* addr, uint16_t* port) {
    if (m_closed) {
        assert(0 && "Already closed");
        return error::illegal_state;
    }

    struct sockaddr_in from = {0};
    int fromlen = sizeof(from);
    int size = recvfrom(m_fd, buf, len, 0, (struct sockaddr*) &from, &fromlen);
    if (size == SOCKET_ERROR) {
        return error::wrap(etype::os, WSAGetLastError());
    }
    *addr = inet_ntoa(from.sin_addr);
    *port = ntohs(from.sin_port);
    if (size == 0) {
        return error::eof;
    }
    if (nbytes != nullptr) {
        *nbytes = size;
    }
    return error::nil;
}

error UDPSocket::Write(const char* buf, size_t len, int* nbytes) {
    if (m_closed) {
        assert(0 && "Already closed");
        return error::illegal_state;
    }
    if (m_remoteAddr.empty() && m_remotePort == 0) {
        return error::illegal_state;
    }

    return WriteTo(buf, len, m_remoteAddr, m_remotePort, nbytes);
}

error UDPSocket::WriteTo(const char* buf, size_t len,
        const std::string& addr, uint16_t port, int* nbytes) {
    if (m_closed) {
        assert(0 && "Already closed");
        return error::illegal_state;
    }

    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_addr.S_un.S_addr = inet_addr(addr.c_str());
    to.sin_port = htons(port);
    int size = sendto(m_fd, buf, len, 0, (struct sockaddr*) &to, sizeof(to));
    if (size == SOCKET_ERROR) {
        return error::wrap(etype::os, WSAGetLastError());
    }
    if (nbytes != nullptr) {
        *nbytes = size;
    }
    return error::nil;
}

error UDPSocket::SetTimeout(int64_t timeoutMilliseconds) {
    if (m_closed) {
        assert(0 && "Already closed");
        return error::illegal_state;
    }

    DWORD soTimeout = (DWORD) ((timeoutMilliseconds > 0) ? timeoutMilliseconds : 0);
    if (setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*) &soTimeout, sizeof(soTimeout)) == SOCKET_ERROR) {
        return error::wrap(etype::os, WSAGetLastError());
    }
    if (setsockopt(m_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*) &soTimeout, sizeof(soTimeout)) == SOCKET_ERROR) {
        return error::wrap(etype::os, WSAGetLastError());
    }
    return error::nil;
}

} // namespace net
