#include "ssl.hpp"

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

    Ctx = SSL_CTX_new(TLS_client_method());
}

TSslContext::~TSslContext() {
    SSL_CTX_free(Ctx);
}

} // namespace NNet
