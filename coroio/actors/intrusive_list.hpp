#pragma once

#include <memory>

namespace NNet {
namespace NActors {

template<typename T>
struct TIntrusiveListNode {
    std::unique_ptr<T> Next = nullptr;
    T* Prev = nullptr;
};

template<typename T>
class TIntrusiveList {
public:
    TIntrusiveList() = default;
    TIntrusiveList(const TIntrusiveList&) = delete;
    TIntrusiveList& operator=(const TIntrusiveList&) = delete;

    size_t Size() const {
        return Size_;
    }

    void PushBack(std::unique_ptr<T>&& node) {
        node->Prev = Tail_;
        node->Next = nullptr;

        if (Tail_) {
            Tail_->Next = std::move(node);
            Tail_ = Tail_->Next.get();
        } else {
            Head_ = std::move(node);
            Tail_ = Head_.get();
        }
        Size_++;
    }

    std::unique_ptr<T> Erase(T* node) {
        if (!node) {
            return nullptr;
        }

        std::unique_ptr<T>& owner = node->Prev
            ? node->Prev->Next
            : Head_;
        if (!owner) {
            return nullptr;
        }

        std::unique_ptr<T> result = std::move(owner);
        owner = std::move(node->Next);
        if (owner) {
            owner->Prev = node->Prev;
        } else {
            Tail_ = node->Prev;
        }

        result->Next = nullptr;
        result->Prev = nullptr;

        if (!Head_) {
            Tail_ = nullptr;
        }

        assert(Size_ > 0);
        Size_--;
        return result;
    }

    T* Front() {
        return Head_.get();
    }

private:
    std::unique_ptr<T> Head_;
    T* Tail_ = nullptr;
    size_t Size_ = 0;
};

} // namespace NActors
} // namespace NNet