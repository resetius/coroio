#pragma once

#include <memory>

namespace NNet {
namespace NActors {

template<typename T>
struct TIntrusiveListNode {
    std::unique_ptr<T> Next = nullptr;
    TIntrusiveListNode<T>* Prev = nullptr;
    int Tag = 0;
};

template<typename T>
class TIntrusiveList {
public:
    TIntrusiveList(int tag = 0)
        : Root_{nullptr, nullptr, tag}
        , Tail_(&Root_)
        , Tag_(tag)
    { }

    TIntrusiveList(const TIntrusiveList&) = delete;
    TIntrusiveList& operator=(const TIntrusiveList&) = delete;

    size_t Size() const {
        return Size_;
    }

    bool Empty() const {
        return Size_ == 0;
    }

    void PushBack(std::unique_ptr<T>&& node) {
        node->Prev = Tail_;
        node->Next = nullptr;
        node->Tag = Tag_;

        Tail_->Next = std::move(node);
        Tail_ = Tail_->Next.get();

        Size_++;
    }

    std::unique_ptr<T> Erase(T* node) {
        assert(node);
        assert(node != &Root_);

        if (node->Tag != Tag_) {
            return nullptr;
        }

        std::unique_ptr<T>& owner = node->Prev->Next;
        std::unique_ptr<T> result = std::move(owner);
        owner = std::move(node->Next);

        if (owner) {
            owner->Prev = node->Prev;
        } else {
            Tail_ = node->Prev;
        }

        result->Next = nullptr;
        result->Prev = nullptr;

        assert(Size_ > 0);
        Size_--;
        return result;
    }

    T* Front() {
        return Root_.Next.get();
    }

    std::unique_ptr<T> PopFront() noexcept {
        auto result = std::move(Root_.Next);
        Root_.Next = std::move(result->Next);

        if (Root_.Next) {
            Root_.Next->Prev = &Root_;
        } else {
            Tail_ = &Root_;
        }

        result->Next = nullptr;
        result->Prev = nullptr;
        Size_--;
        return result;
    }

private:
    TIntrusiveListNode<T> Root_;
    TIntrusiveListNode<T>* Tail_;
    size_t Size_ = 0;
    int Tag_;
};

} // namespace NActors
} // namespace NNet