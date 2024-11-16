#pragma once

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <stdexcept>
#include <functional>

#include "base.hpp"
#include "corochain.hpp"
#include "sockutils.hpp"
#include "promises.hpp"

namespace NNet {

struct TSslContext {
    SSL_CTX* Ctx;
    std::function<void(const char*)> LogFunc = {};

    TSslContext(TSslContext&& other)
        : Ctx(other.Ctx)
        , LogFunc(other.LogFunc)
    {
        other.Ctx = nullptr;
    }

    ~TSslContext();

    static TSslContext Client(const std::function<void(const char*)>& logFunc = {});
    static TSslContext Server(const char* certfile, const char* keyfile, const std::function<void(const char*)>& logFunc = {});
    static TSslContext ServerFromMem(const void* certfile, const void* keyfile, const std::function<void(const char*)>& logFunc = {});

private:
    TSslContext();
};

template<typename THandle>
class TSslSocket {
public:
    using TPoller = typename THandle::TPoller;

    TSslSocket(THandle&& socket, TSslContext& ctx)
        : Socket(std::move(socket))
        , Ctx(&ctx)
        , Ssl(SSL_new(Ctx->Ctx))
        , Rbio(BIO_new(BIO_s_mem()))
        , Wbio(BIO_new(BIO_s_mem()))
    {
        SSL_set_bio(Ssl, Rbio, Wbio);
        SSL_set_mode(Ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);
        SSL_set_mode(Ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    }

    TSslSocket(TSslSocket&& other)
    {
        *this = std::move(other);
    }

    TSslSocket& operator=(TSslSocket&& other) {
        if (this != &other) {
            Socket = std::move(other.Socket);
            Ctx = other.Ctx;
            Ssl = other.Ssl;
            Rbio = other.Rbio;
            Wbio = other.Wbio;
            Handshake = other.Handshake;
            other.Ssl = nullptr;
            other.Rbio = other.Wbio = nullptr;
            other.Handshake = nullptr;
        }
        return *this;
    }

    TSslSocket(const TSslSocket& ) = delete;
    TSslSocket& operator=(const TSslSocket&) = delete;

    TSslSocket() = default;

    ~TSslSocket()
    {
        if (Ssl) { SSL_free(Ssl); }
        if (Handshake) { Handshake.destroy(); }
    }

    TValueTask<TSslSocket<THandle>> Accept() {
        auto underlying = std::move(co_await Socket.Accept());
        auto socket = TSslSocket(std::move(underlying), *Ctx);
        SSL_set_accept_state(socket.Ssl);
        co_return std::move(socket);
    }

    TValueTask<void> AcceptHandshake() {
        assert(!Handshake);
        SSL_set_accept_state(Ssl);
        co_return co_await DoHandshake();
    }

    TValueTask<void> Connect(TTime deadline = TTime::max()) {
        assert(!Handshake);
        co_await Socket.Connect(deadline);
        SSL_set_connect_state(Ssl);
        co_return co_await DoHandshake();
    }

    TValueTask<ssize_t> ReadSome(void* data, size_t size) {
        co_await WaitHandshake();

        int n = SSL_read(Ssl, data, size);
        int status;
        if (n > 0) {
            co_return n;
        }

        do {
            co_await DoIO();
            n = SSL_read(Ssl, data, size);
            status = SSL_get_error(Ssl, n);
        } while (n < 0 && (status == SSL_ERROR_WANT_READ || status == SSL_ERROR_WANT_WRITE));

        co_return n;
    }

    TValueTask<ssize_t> WriteSome(const void* data, size_t size) {
        co_await WaitHandshake();

        auto r = size;
        const char* p = (const char*)data;
        while (size != 0) {
            auto n = SSL_write(Ssl, p, size);
            auto status = SSL_get_error(Ssl, n);
            if (!(status == SSL_ERROR_WANT_READ || status == SSL_ERROR_WANT_WRITE || status == SSL_ERROR_NONE)) {
                throw std::runtime_error("SSL error: " + std::to_string(status));
            }
            if (n <= 0) {
                throw std::runtime_error("SSL error: " + std::to_string(status));
            }

            co_await DoIO();

            size -= n;
            p += n;
        }
        co_return r;
    }

    auto Poller() {
        return Socket.Poller();
    }

private:
    TValueTask<void> DoIO() {
        char buf[1024];
        int n;
        while ((n = BIO_read(Wbio, buf, sizeof(buf))) > 0) {
            co_await TByteWriter(Socket).Write(buf, n);
        }
        if (n < 0 && !BIO_should_retry(Wbio)) {
            throw std::runtime_error("Cannot read Wbio");
        }

        if (SSL_get_error(Ssl, n) == SSL_ERROR_WANT_READ) {
            auto size = co_await Socket.ReadSome(buf, sizeof(buf));
            if (size == 0) {
                throw std::runtime_error("Connection closed");
            }
            const char* p = buf;
            while (size != 0) {
                auto n = BIO_write(Rbio, p, size);
                if (n <= 0) {
                    throw std::runtime_error("Cannot write Rbio");
                }
                size -= n;
                p += n;
            }
        }

        co_return;
    }

    TValueTask<void> DoHandshake() {
        int r;
        LogState();
        while ((r = SSL_do_handshake(Ssl)) != 1) {
            LogState();
            int status = SSL_get_error(Ssl, r);
            if (status == SSL_ERROR_WANT_READ || status == SSL_ERROR_WANT_WRITE) {
                co_await DoIO();
            } else {
                throw std::runtime_error("SSL error: " + std::to_string(r));
            }
        }

        LogState();

        co_await DoIO();

        if (Ctx->LogFunc) {
            Ctx->LogFunc("SSL Handshake established\n");
        }

        for (auto w : Waiters) {
            w.resume();
        }
        Waiters.clear();

        co_return;
    }

    void StartHandshake() {
        assert(!Handshake);
        Handshake = RunHandshake();
    }

    TVoidSuspendedTask RunHandshake() {
        // TODO: catch exception
        co_await DoHandshake();
        co_return;
    }

    auto WaitHandshake() {
        if (!SSL_is_init_finished(Ssl) && !Handshake) {
            StartHandshake();
        }
        struct TAwaitable {
            bool await_ready() {
                return !handshake || handshake.done();
            }

            void await_suspend(std::coroutine_handle<> h) {
                waiters->push_back(h);
            }

            void await_resume() { }

            std::coroutine_handle<> handshake;
            std::vector<std::coroutine_handle<>>* waiters;
        };

        return TAwaitable { Handshake, &Waiters };
    };

    void LogState() {
        if (!Ctx->LogFunc) return;

        char buf[1024];

        const char * state = SSL_state_string_long(Ssl);
        if (state != LastState) {
            if (state) {
                snprintf(buf, sizeof(buf), "SSL-STATE: %s", state);
                Ctx->LogFunc(buf);
            }
            LastState = state;
        }
    }

    THandle Socket;
    TSslContext* Ctx = nullptr;

    SSL* Ssl = nullptr;
    BIO* Rbio = nullptr;
    BIO* Wbio = nullptr;

    const char* LastState = nullptr;

    std::coroutine_handle<> Handshake;
    std::vector<std::coroutine_handle<>> Waiters;
};

} // namespace NNet
