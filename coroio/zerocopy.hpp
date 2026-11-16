#pragma once

#include <vector>

namespace NNet {

/**
 * @brief Bytes sent without copying (TPollerDrivenSocket::WriteSomeZeroCopy()).
 *
 * The kernel reads them until the send's notification, which comes after the
 * send's result, e.g. once TCP has the ACK. Keep Data unchanged while Busy().
 * The poller holds a reference until then, so the sender may drop its own at
 * any time.
 */
struct TZeroCopyBuffer {
    std::vector<char> Data;
    /// Sends whose notification is not posted yet. Counted by the poller.
    unsigned Sends = 0;

    bool Busy() const {
        return Sends != 0;
    }
};

} // namespace NNet
