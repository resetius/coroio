#pragma once

#include <vector>

namespace NNet {
namespace NActors {

template<typename T>
struct TUnboundedVectorQueue {
    TUnboundedVectorQueue(size_t capacity = 16)
        : Data(RoundUpToPowerOfTwo(capacity))
        , LastIndex(Data.size() - 1)
    { }

    void Push(T&& item) {
        EnsureCapacity();
        Data[Tail] = std::move(item);
        Tail = (Tail + 1) & LastIndex;
    }

    T& Front() {
        return Data[Head];
    }

    void Pop() {
        Head = (Head + 1) & LastIndex;
    }

    size_t Size() const {
        return (Data.size() + Tail - Head) & LastIndex;
    }

    bool Empty() const {
        return Head == Tail;
    }

private:
    static size_t RoundUpToPowerOfTwo(size_t value) {
        size_t power = 1;
        while (power < value) {
            power <<= 1;
        }
        return power;
    }

    void EnsureCapacity() {
        if (Size() == Data.size() - 1) [[unlikely]] {
            std::vector<T> newData(Data.size() * 2);
            auto size = Size();
            for (size_t i = 0; i < size; ++i) {
                newData[i] = std::move(Data[(Head + i) & LastIndex]);
            }
            Data = std::move(newData);
            Head = 0;
            Tail = size;
            LastIndex = Data.size() - 1;
        }
    }

    std::vector<T> Data;
    size_t Head = 0;
    size_t Tail = 0;
    size_t LastIndex = 0;
};

} // namespace NActors
} // namespace NNet