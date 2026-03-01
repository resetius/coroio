#pragma once

#if __has_include(<openssl/bio.h>)
#define HAVE_OPENSSL

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
#include "socket.hpp"

namespace NNet {

/**
 * @brief Owns an OpenSSL `SSL_CTX` and optional log callback.
 *
 * Move-only. Create via the static factory methods, then pass a reference to
 * `TSslSocket`. The context must outlive every `TSslSocket` that uses it.
 *
 * @code
 * // TLS client
 * TSslContext ctx = TSslContext::Client();
 *
 * // TLS server (PEM files)
 * TSslContext ctx = TSslContext::Server("server.crt", "server.key");
 *
 * // TLS server (in-memory PEM)
 * TSslContext ctx = TSslContext::ServerFromMem(certPem, keyPem);
 * @endcode
 */
struct TSslContext {
    SSL_CTX* Ctx; ///< The underlying OpenSSL context.
    std::function<void(const char*)> LogFunc = {}; ///< Optional logging callback.

    TSslContext(TSslContext&& other)
        : Ctx(other.Ctx)
        , LogFunc(other.LogFunc)
    {
        other.Ctx = nullptr;
    }

    ~TSslContext();

    /**
     * @brief Creates a TLS client context (no certificate required).
     *
     * @param logFunc Optional callback for SSL state-change messages.
     */
    static TSslContext Client(const std::function<void(const char*)>& logFunc = {});

    /**
     * @brief Creates a TLS server context from PEM files on disk.
     *
     * @param certfile Path to the PEM certificate file (or chain).
     * @param keyfile  Path to the PEM private key file.
     * @param logFunc  Optional callback for SSL state-change messages.
     */
    static TSslContext Server(const char* certfile, const char* keyfile, const std::function<void(const char*)>& logFunc = {});

    /**
     * @brief Creates a TLS server context from PEM data already in memory.
     *
     * Both `certfile` and `keyfile` must point to null-terminated PEM strings.
     *
     * @param certfile PEM certificate (or chain) in memory.
     * @param keyfile  PEM private key in memory.
     * @param logFunc  Optional callback for SSL state-change messages.
     */
    static TSslContext ServerFromMem(const void* certfile, const void* keyfile, const std::function<void(const char*)>& logFunc = {});

private:
    TSslContext();
};

/**
 * @brief TLS layer over any connected socket, exposing the same `ReadSome`/`WriteSome` interface.
 *
 * Takes **ownership** of the underlying socket (moved in). References the
 * `TSslContext` — the context must outlive this object. Move-only.
 *
 * **Client usage:**
 * @code
 * TSslContext ctx = TSslContext::Client();
 * TSslSocket ssl(std::move(socket), ctx);
 * ssl.SslSetTlsExtHostName("example.com");   // SNI — call before Connect()
 * co_await ssl.Connect(addr);
 * ssize_t n = co_await ssl.ReadSome(buf, size);
 * @endcode
 *
 * **Server usage:**
 * @code
 * TSslContext ctx = TSslContext::Server("server.crt", "server.key");
 * TSslSocket listener(std::move(listeningSocket), ctx);
 * auto client = co_await listener.Accept();  // TCP accept + TLS handshake
 * @endcode
 *
 * @tparam TSocket Underlying connected socket type (e.g. `TDefaultPoller::TSocket`).
 */
template<typename TSocket>
class TSslSocket {
public:
    using TPoller = typename TSocket::TPoller;

    /**
     * @brief Constructs a TSslSocket, taking ownership of the underlying socket.
     *
     * @param socket Moved-in socket (TCP-connected for client; bound+listening for server).
     * @param ctx    TLS context — must outlive this object.
     */
    TSslSocket(TSocket&& socket, TSslContext& ctx)
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

    /// Frees the SSL instance, associated BIOs, and any in-progress handshake coroutine.
    ~TSslSocket()
    {
        if (Ssl) { SSL_free(Ssl); }
        if (Handshake) { Handshake.destroy(); }
    }

    /**
     * @brief Sets the TLS SNI host name sent in the ClientHello.
     *
     * Required for servers that host multiple certificates on one IP. Must be
     * called **before** `Connect()`.
     *
     * @param host The server hostname (e.g. `"example.com"`).
     */
    void SslSetTlsExtHostName(const std::string& host) {
        SSL_set_tlsext_host_name(Ssl, host.c_str());
    }

    /**
     * @brief Accepts a TCP connection and performs the server-side TLS handshake.
     *
     * Calls `Socket.Accept()` then `AcceptHandshake()` on the resulting socket.
     * Use this on a bound+listening `TSslSocket` in a server accept loop.
     *
     * @return A fully-handshaked `TSslSocket` ready for `ReadSome`/`WriteSome`.
     */
    TFuture<TSslSocket<TSocket>> Accept() {
        auto underlying = std::move(co_await Socket.Accept());
        auto socket = TSslSocket(std::move(underlying), *Ctx);
        co_await socket.AcceptHandshake();
        co_return std::move(socket);
    }

    /**
     * @brief Performs the server-side TLS handshake on an already-accepted TCP socket.
     *
     * Called automatically by `Accept()`. Call directly only if you accepted the
     * TCP connection separately and want to add TLS on top.
     */
    TFuture<void> AcceptHandshake() {
        assert(!Handshake);
        SSL_set_accept_state(Ssl);
        co_return co_await DoHandshake();
    }

    /**
     * @brief TCP-connects to `address` and performs the client-side TLS handshake.
     *
     * Call `SslSetTlsExtHostName` before this if the server requires SNI.
     *
     * @param address  Remote address to connect to.
     * @param deadline Optional connection timeout (defaults to no timeout).
     * @throws std::system_error on TCP connect failure or timeout.
     * @throws std::runtime_error on TLS handshake failure.
     */
    TFuture<void> Connect(const TAddress& address, TTime deadline = TTime::max()) {
        assert(!Handshake);
        co_await Socket.Connect(address, deadline);
        SSL_set_connect_state(Ssl);
        co_return co_await DoHandshake();
    }

    /**
     * @brief Reads up to `size` decrypted bytes into `data`.
     *
     * Waits for the handshake to complete if it hasn't yet. Returns bytes read
     * (>0), `0` on clean TLS shutdown, or a negative value on transient error.
     * Throws `std::runtime_error` on fatal TLS errors.
     */
    TFuture<ssize_t> ReadSome(void* data, size_t size) {
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

    /**
     * @brief Encrypts and sends all `size` bytes from `data`.
     *
     * Waits for the handshake to complete if it hasn't yet. Returns `size` on
     * success (all bytes are always written). Throws `std::runtime_error` on
     * TLS error or connection close.
     */
    TFuture<ssize_t> WriteSome(const void* data, size_t size) {
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

    /// Returns the poller associated with the underlying socket.
    auto Poller() {
        return Socket.Poller();
    }

private:
    TFuture<void> DoIO() {
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

    TFuture<void> DoHandshake() {
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

    TSocket Socket;
    TSslContext* Ctx = nullptr;

    SSL* Ssl = nullptr;
    BIO* Rbio = nullptr;
    BIO* Wbio = nullptr;

    const char* LastState = nullptr;

    std::coroutine_handle<> Handshake;
    std::vector<std::coroutine_handle<>> Waiters;
};

} // namespace NNet

#endif

