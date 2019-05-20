#include "net/stream.h"
#include <cassert>

namespace net {

error Reader::ReadFull(char* buf, size_t len) {
    if (buf == nullptr) {
        assert(0 && "buf must not be nullptr");
        return error::illegal_argument;
    }

    size_t offset = 0;
    int nbytes;
    while (true) {
        error err = Read(&buf[offset], len - offset, &nbytes);
        if (err != error::nil) {
            return err;
        }
        offset += nbytes;
        if (offset >= len) {
            return error::nil;
        }
    }
}

error Reader::ReadLine(char* buf, size_t len) {
    if (buf == nullptr) {
        assert(0 && "buf must not be nullptr");
        return error::illegal_argument;
    }

    size_t offset = 0;
    int nbytes;
    while (true) {
        error err = Read(&buf[offset], 1, &nbytes);
        if (err != error::nil) {
            return err;
        }
        if (buf[offset] == '\n') {
            buf[offset + 1] = '\0';
            return error::nil;
        }
        offset += nbytes;
        if (offset + 1 >= len) { // buf is full
            buf[offset] = '\0';
            return error::nil;
        }
    }
}

error Writer::WriteFull(const char* buf, size_t len) {
    if (buf == nullptr) {
        assert(0 && "buf must not be nullptr");
        return error::illegal_argument;
    }

    size_t offset = 0;
    int nbytes;
    while (true) {
        error err = Write(&buf[offset], len - offset, &nbytes);
        if (err != error::nil) {
            return err;
        }
        offset += nbytes;
        if (offset >= len) {
            return error::nil;
        }
    }
}

} // namespace net
