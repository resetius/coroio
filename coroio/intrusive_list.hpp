#pragma once

#include <assert.h>

namespace NNet {
namespace NDetail {

template<typename T> class TIntrusiveList;

/**
 * @brief Links of an object in a TIntrusiveList.
 *
 * Derive the object from it: `struct TItem: TIntrusiveNode<TItem> { ... };`.
 * The object is in at most one list at a time, and the list does not own it.
 */
template<typename T>
class TIntrusiveNode {
public:
    TIntrusiveNode() = default;
    TIntrusiveNode(const TIntrusiveNode&) = delete;
    TIntrusiveNode& operator=(const TIntrusiveNode&) = delete;

    /// Returns true if the object is in a list.
    bool IsLinked() const {
        return Next_ != nullptr;
    }

    /// Removes the object from its list in O(1). It must be in a list.
    void Unlink() {
        assert(IsLinked());
        Prev_->Next_ = Next_;
        Next_->Prev_ = Prev_;
        Prev_ = nullptr;
        Next_ = nullptr;
    }

private:
    friend class TIntrusiveList<T>;

    TIntrusiveNode* Prev_ = nullptr;
    TIntrusiveNode* Next_ = nullptr;
};

/**
 * @brief Doubly linked list of objects derived from TIntrusiveNode.
 *
 * The list does not own its objects: it only links them, without allocating.
 * It is circular around a sentinel node, so linking and unlinking do not branch.
 * The sentinel points to itself, so the list can be neither copied nor moved.
 * Objects must be unlinked before they are destroyed.
 *
 * @code{.cpp}
 * struct TItem: NDetail::TIntrusiveNode<TItem> { int Value = 0; };
 * NDetail::TIntrusiveList<TItem> list;
 * TItem a, b;
 * list.PushBack(a);
 * list.PushFront(b);
 * for (auto& item : list) { ... } // b, a
 * list.Erase(a);
 * @endcode
 */
template<typename T>
class TIntrusiveList {
    using TNode = TIntrusiveNode<T>;

public:
    class TIterator {
    public:
        explicit TIterator(TNode* node)
            : Node_(node)
        { }

        T& operator*() const {
            return static_cast<T&>(*Node_);
        }

        T* operator->() const {
            return static_cast<T*>(Node_);
        }

        TIterator& operator++() {
            Node_ = Node_->Next_;
            return *this;
        }

        bool operator==(const TIterator& other) const = default;

    private:
        TNode* Node_;
    };

    TIntrusiveList() {
        Sentinel_.Prev_ = &Sentinel_;
        Sentinel_.Next_ = &Sentinel_;
    }

    TIntrusiveList(const TIntrusiveList&) = delete;
    TIntrusiveList& operator=(const TIntrusiveList&) = delete;

    bool Empty() const {
        return Sentinel_.Next_ == &Sentinel_;
    }

    /// The first object. The list must not be empty.
    T& Front() {
        assert(!Empty());
        return static_cast<T&>(*Sentinel_.Next_);
    }

    void PushFront(T& item) {
        LinkBefore(Sentinel_.Next_, item);
    }

    void PushBack(T& item) {
        LinkBefore(&Sentinel_, item);
    }

    /// Removes an object from the list it is in, in O(1).
    static void Erase(T& item) {
        static_cast<TNode&>(item).Unlink();
    }

    TIterator begin() {
        return TIterator(Sentinel_.Next_);
    }

    TIterator end() {
        return TIterator(&Sentinel_);
    }

private:
    static void LinkBefore(TNode* next, T& item) {
        TNode& node = item;
        node.Prev_ = next->Prev_;
        node.Next_ = next;
        next->Prev_->Next_ = &node;
        next->Prev_ = &node;
    }

    TNode Sentinel_;
};

} // namespace NDetail
} // namespace NNet
