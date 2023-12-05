#include <string.h>
#include <assert.h>
#include "sockutils.hpp"

namespace NNet {

TLineSplitter::TLineSplitter(int maxLen)
    : WPos(0)
    , RPos(0)
    , Size(0)
    , Cap(maxLen * 2)
    , Data(Cap, 0)
    , View(Data)
{ }

TLine TLineSplitter::Pop() {
    auto end = View.substr(RPos, Size);
    auto begin = View.substr(0, Size - end.size());

    auto p1 = end.find('\n');
    if (p1 == std::string_view::npos) {
        auto p2 = begin.find('\n');
        if (p2 == std::string_view::npos) {
            return {};
        }

        RPos = p2 + 1;
        Size -= end.size() + p2 + 1;
        return TLine { end, begin.substr(0, p2 + 1) };
    } else {
        RPos += p1 + 1;
        Size -= p1 + 1;
        return TLine { end.substr(0, p1 + 1), {} };
    }
}

void TLineSplitter::Push(const char* buf, size_t size) {
    if (Size + size > Data.size()) {
        throw std::runtime_error("Overflow");
    }

    auto first = std::min(size, Cap - WPos);
    memcpy(&Data[WPos], buf, first);
    memcpy(&Data[0], buf + first, std::max<size_t>(0, size - first));
    WPos = (WPos + size) % Cap;
    Size = Size + size;
}

TZeroCopyLineSplitter::TZeroCopyLineSplitter(int maxLen)
    : WPos(0)
    , RPos(0)
    , Size(0)
    , Cap(maxLen * 2)
    , Data(Cap, 0)
    , View(Data)
{ }

TLine TZeroCopyLineSplitter::Pop() {
    auto end = View.substr(RPos, Size);
    auto begin = View.substr(0, Size - end.size());

    auto p1 = end.find('\n');
    if (p1 == std::string_view::npos) {
        auto p2 = begin.find('\n');
        if (p2 == std::string_view::npos) {
            return {};
        }

        RPos = p2 + 1;
        Size -= end.size() + p2 + 1;
        return TLine { end, begin.substr(0, p2 + 1) };
    } else {
        RPos += p1 + 1;
        Size -= p1 + 1;
        return TLine { end.substr(0, p1 + 1), {} };
    }
}

std::span<char> TZeroCopyLineSplitter::Acquire(size_t size) {
    size = std::min(size, Cap - Size);
    if (size == 0) {
        throw std::runtime_error("Overflow");
    }
    auto first = std::min(size, Cap - WPos);
    if (first) {
        return {&Data[WPos], first};
    } else {
        return {&Data[0], size};
    }
}

void TZeroCopyLineSplitter::Commit(size_t size) {
    WPos = (WPos + size) % Cap;
    Size += size;
}

void TZeroCopyLineSplitter::Push(const char* p, size_t len) {
    while (len != 0) {
        auto buf = Acquire(len);
        memcpy(buf.data(), p, buf.size());
        Commit(buf.size());
        len -= buf.size();
        p += buf.size();
    }
}

} // namespace NNet
