#include <openssl/evp.h>
#include <stddef.h>

int __ascii_iswalpha(unsigned int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int EVP_CIPHER_CTX_get_iv(EVP_CIPHER_CTX *ctx, void *iv, size_t len)
{
    return EVP_CIPHER_CTX_get_updated_iv(ctx, iv, len);
}

int EVP_CIPHER_CTX_set_iv(EVP_CIPHER_CTX *ctx, const void *iv, int len)
{
    if (len != EVP_CIPHER_CTX_get_iv_length(ctx))
        return 0;
    return EVP_CipherInit_ex(ctx, NULL, NULL, NULL, iv, -1);
}
