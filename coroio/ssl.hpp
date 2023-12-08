#pragma once

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <stdexcept>
#include <functional>

#include "base.hpp"
#include "corochain.hpp"

namespace NNet {

struct TSslContext {
    SSL_CTX* Ctx;

    TSslContext(TSslContext&& other)
        : Ctx(other.Ctx)
    {
        other.Ctx = nullptr;
    }

    ~TSslContext();

    static TSslContext Client();
    static TSslContext Server(const char* certfile, const char* keyfile);

private:
    TSslContext();
};

template<typename THandle>
class TSslSocket {
public:
    TSslSocket(THandle& socket, TSslContext& ctx, const std::function<void(const char*)>& logFunc = {})
        : Socket(socket)
        , Ctx(ctx)
        , Ssl(SSL_new(Ctx.Ctx))
        , Rbio(BIO_new(BIO_s_mem()))
        , Wbio(BIO_new(BIO_s_mem()))
        , LogFunc(logFunc)
    {
        SSL_set_bio(Ssl, Rbio, Wbio);
    }

    TSslSocket(TSslSocket&& other)
        : Socket(other.Socket)
        , Ctx(other.Ctx)
        , Ssl(other.Ssl)
        , Rbio(other.Rbio)
        , Wbio(other.Wbio)
    {
        other.Ssl = nullptr;
        other.Rbio = other.Wbio = nullptr;
    }

    TSslSocket(const TSslSocket& ) = delete;
    TSslSocket& operator=(const TSslSocket&) = delete;

    ~TSslSocket()
    {
        if (Ssl) { SSL_free(Ssl); }
    }

    TValueTask<void> Accept() {
        SSL_set_accept_state(Ssl);
        co_return co_await DoHandshake();
    }

    TValueTask<void> Connect() {
        co_await Socket.Connect();
        SSL_set_connect_state(Ssl);
        co_return co_await DoHandshake();
    }

    TValueTask<ssize_t> ReadSome(void* data, size_t size) {
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
        char buf[1024];
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

        if (LogFunc) {
            LogFunc("SSL Handshake established\n");
        }
    }

    void LogState() {
        if (!LogFunc) return;

        char buf[1024];

        const char * state = SSL_state_string_long(Ssl);
        if (state != LastState) {
            if (state) {
                snprintf(buf, sizeof(buf), "SSL-STATE: %s", state);
                LogFunc(buf);
            }
            LastState = state;
        }
    }

    THandle& Socket;
    TSslContext& Ctx;

    SSL* Ssl = nullptr;
    BIO* Rbio = nullptr;
    BIO* Wbio = nullptr;

    std::function<void(const char*)> LogFunc = {};
    const char* LastState = nullptr;
};

} // namespace NNet
