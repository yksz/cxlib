#include "netlib/tcp.h"
#include <cassert>
#include <cerrno>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include "netlib/internal/init.h"
#include "netlib/resolver.h"

namespace net {

static const int kBlockingMode = 0;
static const int kNonBlockingMode = 1;

static void toTimeval(int64_t milliseconds, struct timeval* dest) {
    milliseconds = (milliseconds > 0) ? milliseconds : 0;
    dest->tv_sec = milliseconds / 1000;
    dest->tv_usec = milliseconds % 1000 * 1000;
}

static error waitUntilReady(const int& fd,
        fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
        int64_t timeoutMilliseconds) {
    if (readfds != nullptr) {
        FD_ZERO(readfds);
        FD_SET(fd, readfds);
    }
    if (writefds != nullptr) {
        FD_ZERO(writefds);
        FD_SET(fd, writefds);
    }
    if (exceptfds != nullptr) {
        FD_ZERO(exceptfds);
        FD_SET(fd, exceptfds);
    }
    struct timeval timeout;
    toTimeval(timeoutMilliseconds, &timeout);

    int result = select(fd + 1, readfds, writefds, exceptfds, &timeout);
    if (result == -1) {
        return error::wrap(etype::os, errno);
    } else if (result == 0) {
        return error::timedout;
    } else {
        int soErr = 0;
        socklen_t optlen = sizeof(soErr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &optlen);
        if (soErr != 0) {
            return error::wrap(etype::os, soErr);
        }
        return error::nil;
    }
}

error ConnectTCP(const std::string& host, uint16_t port, int64_t timeoutMilliseconds,
        std::shared_ptr<TCPSocket>* clientSock) {
    if (clientSock == nullptr) {
        assert(0 && "clientSock must not be nullptr");
        return error::illegal_argument;
    }

    internal::init();

    std::string remoteAddr;
    error addrErr = LookupAddress(host, &remoteAddr);
    if (addrErr != error::nil) {
        return addrErr;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return error::wrap(etype::os, errno);
    }

    struct sockaddr_in serverAddr = {0};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = inet_addr(remoteAddr.c_str());

    if (timeoutMilliseconds <= 0) { // connect in blocking mode
        if (connect(fd, (struct sockaddr*) &serverAddr, sizeof(serverAddr)) == -1) {
            int connErr = errno;
            close(fd);
            return error::wrap(etype::os, connErr);
        }
    } else { // connect in non blocking mode
        ioctl(fd, FIONBIO, &kNonBlockingMode);
        if (connect(fd, (struct sockaddr*) &serverAddr, sizeof(serverAddr)) == -1) {
            int connErr = errno;
            if (connErr != EINPROGRESS) {
                close(fd);
                return error::wrap(etype::os, connErr);
            }
        }
        fd_set writefds;
        error err = waitUntilReady(fd, nullptr, &writefds, nullptr, timeoutMilliseconds);
        if (err != error::nil) {
            close(fd);
            return err;
        }
        ioctl(fd, FIONBIO, &kBlockingMode);
    }

    *clientSock = std::make_shared<TCPSocket>(fd, remoteAddr, port);
    return error::nil;
}

error ListenTCP(uint16_t port, std::shared_ptr<TCPListener>* serverSock) {
    if (serverSock == nullptr) {
        assert(0 && "serverSock must not be nullptr");
        return error::illegal_argument;
    }

    internal::init();

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return error::wrap(etype::os, errno);
    }

    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == -1) {
        int err = errno;
        close(fd);
        return error::wrap(etype::os, err);
    }

    struct sockaddr_in serverAddr = {0};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*) &serverAddr, sizeof(serverAddr)) == -1) {
        int err = errno;
        close(fd);
        return error::wrap(etype::os, err);
    }

    if (listen(fd, SOMAXCONN) == -1) {
        int err = errno;
        close(fd);
        return error::wrap(etype::os, err);
    }

    *serverSock = std::make_shared<TCPListener>(fd);
    return error::nil;
}

TCPSocket::~TCPSocket() {
    Close();
}

error TCPSocket::Close() {
    if (m_closed) {
        return error::nil;
    }

    if (close(m_fd) == -1) {
        return error::wrap(etype::os, errno);
    }
    m_closed = true;
    return error::nil;
}

error TCPSocket::Read(char* buf, size_t len, int* nbytes) {
    if (m_closed) {
        assert(0 && "Already closed");
        return error::illegal_state;
    }

    int size = recv(m_fd, buf, len, 0);
    if (size == -1) {
        return error::wrap(etype::os, errno);
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
    if (size == -1) {
        return error::wrap(etype::os, errno);
    }
    if (nbytes != nullptr) {
        *nbytes = size;
    }
    return error::nil;
}

error TCPSocket::SetTimeout(int64_t timeoutMilliseconds) {
    if (m_closed) {
        assert(0 && "Already closed");
        return error::illegal_state;
    }

    struct timeval soTimeout;
    toTimeval(timeoutMilliseconds, &soTimeout);
    if (setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &soTimeout, sizeof(soTimeout)) == -1) {
        return error::wrap(etype::os, errno);
    }
    if (setsockopt(m_fd, SOL_SOCKET, SO_SNDTIMEO, &soTimeout, sizeof(soTimeout)) == -1) {
        return error::wrap(etype::os, errno);
    }

#ifdef TCP_USER_TIMEOUT
    unsigned int userTimeout = timeoutMilliseconds;
    if (setsockopt(m_fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &userTimeout, sizeof(userTimeout)) == -1) {
        return error::wrap(etype::os, errno);
    }
#endif

    m_timeoutMilliseconds = timeoutMilliseconds;
    return error::nil;
}

error TCPSocket::SetKeepAlive(bool on) {
    if (m_closed) {
        assert(0 && "Already closed");
        return error::illegal_state;
    }

    int enabled = on;
    if (setsockopt(m_fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) == -1) {
        return error::wrap(etype::os, errno);
    }
    return error::nil;
}

error TCPSocket::SetKeepAlivePeriod(int periodSeconds) {
    if (periodSeconds < 1) {
        assert(0 && "periodSeconds must not be less than 1");
        return error::illegal_argument;
    }
    if (m_closed) {
        assert(0 && "Already closed");
        return error::illegal_state;
    }

#ifdef TCP_KEEPIDLE
    int keepidle = periodSeconds;
    if (setsockopt(m_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) == -1) {
        return error::wrap(etype::os, errno);
    }
#elif TCP_KEEPALIVE
    int keepalive = periodSeconds;
    if (setsockopt(m_fd, IPPROTO_TCP, TCP_KEEPALIVE, &keepalive, sizeof(keepalive)) == -1) {
        return error::wrap(etype::os, errno);
    }
#endif // TCP_KEEPIDLE

#ifdef TCP_KEEPINTVL
    int keepintvl = periodSeconds;
    if (setsockopt(m_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl)) == -1) {
        return error::wrap(etype::os, errno);
    }
#endif // TCP_KEEPINTVL

    return error::nil;
}

TCPListener::~TCPListener() {
    Close();
}

error TCPListener::Close() {
    if (m_closed) {
        return error::nil;
    }

    if (close(m_fd) == -1) {
        return error::wrap(etype::os, errno);
    }
    m_closed = true;
    return error::nil;
}

error TCPListener::Accept(std::shared_ptr<TCPSocket>* clientSock) {
    if (clientSock == nullptr) {
        assert(0 && "clientSock must not be nullptr");
        return error::illegal_argument;
    }
    if (m_closed) {
        assert(0 && "Already closed");
        return error::illegal_state;
    }

    if (m_timeoutMilliseconds > 0) {
        fd_set readfds;
        error err = waitUntilReady(m_fd, &readfds, nullptr, nullptr, m_timeoutMilliseconds);
        if (err != error::nil) {
            return err;
        }
    }

    struct sockaddr_in clientAddr = {0};
    socklen_t addrlen = sizeof(clientAddr);
    int clientFD = accept(m_fd, (struct sockaddr*) &clientAddr, &addrlen);
    if (clientFD == -1) {
        return error::wrap(etype::os, errno);
    }

    std::string remoteAddr = inet_ntoa(clientAddr.sin_addr);
    uint16_t remotePort = ntohs(clientAddr.sin_port);
    *clientSock = std::make_shared<TCPSocket>(clientFD, std::move(remoteAddr), remotePort);
    return error::nil;
}

error TCPListener::SetTimeout(int64_t timeoutMilliseconds) {
    if (m_closed) {
        assert(0 && "Already closed");
        return error::illegal_state;
    }

    m_timeoutMilliseconds = timeoutMilliseconds;
    return error::nil;
}

} // namespace net
