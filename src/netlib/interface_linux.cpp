#include "netlib/interface.h"
#include <cassert>
#include <cerrno>
#include <memory>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace net {

static error getIndex(int fd, struct ifreq* ifr, int* index) {
    if (ioctl(fd, SIOCGIFINDEX, ifr) == -1) {
        return error::wrap(etype::os, errno);
    }
    *index = ifr->ifr_ifindex;
    return error::nil;
}

static error getName(int fd, struct ifreq* ifr, std::string* name) {
#ifdef SIOCGIFNAME
    if (ioctl(fd, SIOCGIFNAME, ifr) == -1) {
        return error::wrap(etype::os, errno);
    }
    *name = ifr->ifr_name;
#endif
    return error::nil;
}

static error getHardwareAddress(int fd, struct ifreq* ifr,
        std::array<uint8_t, 6>* hardwareAddress) {
    if (ioctl(fd, SIOCGIFHWADDR, ifr) == -1) {
        return error::wrap(etype::os, errno);
    }
    for (size_t i = 0; i < hardwareAddress->size(); i++) {
        (*hardwareAddress)[i] = ifr->ifr_hwaddr.sa_data[i];
    }
    return error::nil;
}

static error getFlags(int fd, struct ifreq* ifr, bool* isUp, bool* isLoopback) {
    if (ioctl(fd, SIOCGIFFLAGS, ifr) == -1) {
        return error::wrap(etype::os, errno);
    }
    *isUp = ifr->ifr_flags & IFF_UP;
    *isLoopback = ifr->ifr_flags & IFF_LOOPBACK;
    return error::nil;
}

error GetNetworkInterfaces(std::vector<NetworkInterface>* infs) {
    if (infs == nullptr) {
        assert(0 && "infs must not be nullptr");
        return error::illegal_argument;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        return error::wrap(etype::os, errno);
    }

    struct ifconf ifc;

    // get the buffer size
    ifc.ifc_buf = nullptr;
    if (ioctl(fd, SIOCGIFCONF, &ifc) == -1) {
        int err = errno;
        close(fd);
        return error::wrap(etype::os, err);
    }

    std::unique_ptr<char[]> buf(new char[ifc.ifc_len]);
    ifc.ifc_buf = buf.get();
    if (ioctl(fd, SIOCGIFCONF, &ifc) == -1) {
        int err = errno;
        close(fd);
        return error::wrap(etype::os, err);
    }

    error err = error::nil;
    struct ifreq* it = ifc.ifc_req;
    struct ifreq* end = it + (ifc.ifc_len / sizeof(struct ifreq));
    for (; it != end; it++) {
        NetworkInterface inf = {0};
        err = getIndex(fd, it, &inf.Index);
        if (err != error::nil) {
            goto fail;
        }
        err = getName(fd, it, &inf.Name);
        if (err != error::nil) {
            goto fail;
        }
        err = getHardwareAddress(fd, it, &inf.HardwareAddress);
        if (err != error::nil) {
            goto fail;
        }
        err = getFlags(fd, it, &inf.IsUp, &inf.IsLoopback);
        if (err != error::nil) {
            goto fail;
        }
        infs->push_back(inf);
    }

fail:
    close(fd);
    return err;
}

} // namespace net
