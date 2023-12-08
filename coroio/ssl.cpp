#include "ssl.hpp"
#include <stdexcept>

namespace NNet {

TSslContext::TSslContext() {
    static int init = 0;
    if (!init) {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        ERR_load_crypto_strings();
        init = 1;
    }
}

TSslContext::~TSslContext() {
    SSL_CTX_free(Ctx);
}

TSslContext TSslContext::Client(const std::function<void(const char*)>& logFunc) {
    TSslContext ctx;
    ctx.Ctx = SSL_CTX_new(TLS_client_method());
    ctx.LogFunc = logFunc;
    return ctx;
}

TSslContext TSslContext::Server(const char* certfile, const char* keyfile, const std::function<void(const char*)>& logFunc) {
    TSslContext ctx;
    ctx.Ctx = SSL_CTX_new(TLS_server_method());
    ctx.LogFunc = logFunc;

    if (SSL_CTX_use_certificate_file(ctx.Ctx, certfile,  SSL_FILETYPE_PEM) != 1) {
        throw std::runtime_error("SSL_CTX_use_certificate_file failed");
    }

    if (SSL_CTX_use_PrivateKey_file(ctx.Ctx, keyfile, SSL_FILETYPE_PEM) != 1) {
        throw std::runtime_error("SSL_CTX_use_PrivateKey_file failed");
    }

    if (SSL_CTX_check_private_key(ctx.Ctx) != 1) {
        throw std::runtime_error("SSL_CTX_check_private_key failed");
    }

    return ctx;
}

} // namespace NNet
