#pragma once

#include "base.hpp"
#include "socket.hpp"
#include "poller.hpp"

#include <stack>

namespace NNet {

template<typename T, size_t PoolSize = 1024>
class TArenaAllocator {
public:
    TArenaAllocator() {
        AllocatePool();
    }

    ~TArenaAllocator() {
        for (auto block : Pools_) {
            ::operator delete(block);
        }
    }

    T* allocate() {
        if (FreePages_.empty()) {
            AllocatePool();
        }
        T* ret = FreePages_.top();
        FreePages_.pop();
        return ret;
    }

    void deallocate(T* obj) {
        FreePages_.push(obj);
    }

private:
    void AllocatePool() {
        T* pool = static_cast<T*>(::operator new(PoolSize * sizeof(T)));
        Pools_.emplace_back(pool);
        for (size_t i = 0; i < PoolSize; i++) {
            FreePages_.push(&pool[i]);
        }
    }

    std::vector<T*> Pools_;
    std::stack<T*> FreePages_;
};

class TIOCp: public TPollerBase {
public:
    using TSocket = NNet::TPollerDrivenSocket<TIOCp>;
    using TFileHandle = NNet::TPollerDrivenFileHandle<TIOCp>;

    TIOCp();
    ~TIOCp();

    void Read(int fd, void* buf, int size, std::coroutine_handle<> handle);
    void Write(int fd, const void* buf, int size, std::coroutine_handle<> handle);
    void Recv(int fd, void* buf, int size, std::coroutine_handle<> handle);
    void Send(int fd, const void* buf, int size, std::coroutine_handle<> handle);
    void Accept(int fd, struct sockaddr* addr, socklen_t* len, std::coroutine_handle<> handle);
    void Connect(int fd, const sockaddr* addr, socklen_t len, std::coroutine_handle<> handle);
    void Cancel(int fd);
    void Register(int fd);
    int Result();

    void Poll();

private:
    struct TIO {
        OVERLAPPED overlapped;
        TEvent event;
        struct sockaddr* addr = nullptr; // for accept
        socklen_t* len = nullptr; // for accept
        int sock = -1; // for accept

        TIO() {
            memset(&overlapped, 0, sizeof(overlapped));
        }
    };

    long GetTimeoutMs();
    TIO* NewTIO();
    void FreeTIO(TIO*);

    HANDLE Port_;

    TArenaAllocator<TIO> Allocator_;
    int Result_ = -1;
};

}
