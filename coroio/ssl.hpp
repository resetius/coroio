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
 * @struct TSslContext
 * @brief Encapsulates an OpenSSL context (SSL_CTX) with optional logging.
 *
 * TSslContext holds a pointer to an OpenSSL SSL_CTX, which is used to configure
 * SSL/TLS parameters. It optionally accepts a logging function to report state changes
 * or errors during SSL operations.
 *
 * The context can be created for client or server mode:
 *  - Use @ref Client() to create a client context.
 *  - Use @ref Server() to create a server context using certificate and key files.
 *  - Use @ref ServerFromMem() to create a server context from in-memory certificate and key data.
 *
 * The context is movable but not copyable. Upon destruction, any associated resources
 * are released.
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
     * @brief Creates a client SSL context.
     *
     * @param logFunc Optional logging function.
     * @return A TSslContext configured for client mode.
     */
    static TSslContext Client(const std::function<void(const char*)>& logFunc = {});
    /**
     * @brief Creates a server SSL context using certificate and key files.
     *
     * @param certfile Path to the certificate file.
     * @param keyfile  Path to the key file.
     * @param logFunc  Optional logging function.
     * @return A TSslContext configured for server mode.
     */
    static TSslContext Server(const char* certfile, const char* keyfile, const std::function<void(const char*)>& logFunc = {});
    /**
     * @brief Creates a server SSL context from in-memory certificate and key data.
     *
     * @param certfile Pointer to the certificate data in memory.
     * @param keyfile  Pointer to the key data in memory.
     * @param logFunc  Optional logging function.
     * @return A TSslContext configured for server mode.
     */
    static TSslContext ServerFromMem(const void* certfile, const void* keyfile, const std::function<void(const char*)>& logFunc = {});

private:
    TSslContext();
};

/**
 * @class TSslSocket
 * @brief Implements an SSL/TLS layer on top of an underlying connection.
 *
 * TSslSocket wraps an existing connection (of type @c TSocket) with SSL/TLS functionality.
 * It creates a new SSL instance (via @c SSL_new()) using the provided TSslContext,
 * and sets up memory BIOs for reading (Rbio) and writing (Wbio).
 *
 * The class provides asynchronous operations for both server and client handshakes:
 *  - @ref AcceptHandshake() is used in server mode.
 *  - @ref Connect() is used in client mode.
 *
 * Once the handshake is complete, TSslSocket exposes asynchronous read and write
 * methods (@ref ReadSome() and @ref WriteSome()) that perform SSL_read() and SSL_write(),
 * using an internal I/O loop (via @ref DoIO() and @ref DoHandshake()).
 *
 * Additionally, TSslSocket allows setting the TLS SNI (via @ref SslSetTlsExtHostName).
 *
 * @tparam TSocket The underlying socket type over which SSL/TLS is layered.
 */
template<typename TSocket>
class TSslSocket {
public:
    using TPoller = typename TSocket::TPoller;

    /**
     * @brief Constructs a TSslSocket from an underlying socket and an SSL context.
     *
     * Creates a new SSL instance using the provided context, sets up memory BIOs for
     * I/O, and configures SSL for partial writes.
     *
     * @param socket An rvalue reference to the underlying connection handle.
     * @param ctx    Reference to the TSslContext to use.
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

    /**
     * @brief Destructor.
     *
     * Frees the SSL instance (and associated BIOs) and destroys any active handshake task.
     */
    ~TSslSocket()
    {
        if (Ssl) { SSL_free(Ssl); }
        if (Handshake) { Handshake.destroy(); }
    }

    /**
     * @brief Sets the TLS SNI (Server Name Indication) extension host name.
     *
     * This is useful for virtual hosting when connecting to servers that rely on SNI.
     *
     * @param host The server host name.
     */
    void SslSetTlsExtHostName(const std::string& host) {
        SSL_set_tlsext_host_name(Ssl, host.c_str());
    }

    /**
     * @brief Asynchronously accepts an incoming SSL connection.
     *
     * Waits for an incoming connection on the underlying socket, wraps it in a TSslSocket,
     * and performs the handshake.
     *
     * @return A TFuture yielding a TSslSocket representing the accepted connection.
     */
    TFuture<TSslSocket<TSocket>> Accept() {
        auto underlying = std::move(co_await Socket.Accept());
        auto socket = TSslSocket(std::move(underlying), *Ctx);
        co_await socket.AcceptHandshake();
        co_return std::move(socket);
    }

    /**
     * @brief Performs the server-side SSL handshake.
     *
     * Configures the SSL state to accept a connection, then performs the handshake asynchronously.
     *
     * @return A TFuture that completes when the handshake is successful.
     */
    TFuture<void> AcceptHandshake() {
        assert(!Handshake);
        SSL_set_accept_state(Ssl);
        co_return co_await DoHandshake();
    }

    /**
     * @brief Initiates the client-side SSL handshake.
     *
     * Connects to the remote address, sets the SSL state to connect, and performs the handshake.
     *
     * @param address  The remote address to connect to.
     * @param deadline Optional timeout for the connection attempt.
     * @return A TFuture that completes when the handshake is successful.
     */
    TFuture<void> Connect(const TAddress& address, TTime deadline = TTime::max()) {
        assert(!Handshake);
        co_await Socket.Connect(address, deadline);
        SSL_set_connect_state(Ssl);
        co_return co_await DoHandshake();
    }

    /**
     * @brief Asynchronously reads data from the SSL connection.
     *
     * Performs SSL_read() and, if needed, loops using asynchronous I/O via DoIO() until data is available.
     *
     * @param data Pointer to the buffer.
     * @param size Maximum number of bytes to read.
     * @return A TFuture yielding the number of bytes read.
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
     * @brief Asynchronously writes data to the SSL connection.
     *
     * Writes the full buffer in a loop using SSL_write() and asynchronous I/O until completion.
     *
     * @param data Pointer to the data.
     * @param size The number of bytes to write.
     * @return A TFuture yielding the total number of bytes written.
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

    /**
     * @brief Returns the underlying poller.
     *
     * @return The poller associated with the underlying socket.
     */
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

