#include "noise.h"
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <cstring>

// Noise_NNpsk0_25519_ChaChaPoly_SHA256 initiator implementation

static const char *NOISE_PROTOCOL_NAME = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";

// Prologue is hardcoded: "NoiseAPIInit" + 0x00 0x00 (empty client hello)
static const QByteArray NOISE_PROLOGUE = QByteArray("NoiseAPIInit\x00\x00", 14);

NoiseNNpsk0::NoiseNNpsk0(const QByteArray &psk) : m_psk(psk), m_n(0), m_sendN(0), m_recvN(0), m_ephemeral(nullptr), m_complete(false)
{
    // Initialize h = SHA256(protocol_name)
    m_h = sha256(QByteArray(NOISE_PROTOCOL_NAME));
    m_ck = m_h;

    // MixHash(prologue)
    mixHash(NOISE_PROLOGUE);

    // Generate ephemeral key pair
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_keygen(ctx, &m_ephemeral);
    EVP_PKEY_CTX_free(ctx);
}

NoiseNNpsk0::~NoiseNNpsk0(void)
{
    if (m_ephemeral)
        EVP_PKEY_free(m_ephemeral);
}

// Initiator writes message 1: e, psk
// Returns: e_pub (32 bytes) + encrypted tag (16 bytes) = 48 bytes
QByteArray NoiseNNpsk0::writeHandshake(void)
{
    // NNpsk0 message 1 token order: psk, e
    // Token 'psk': MixKeyAndHash(psk) — must come first (position 0)
    mixKeyAndHash(m_psk);

    // Token 'e': write ephemeral public key, MixHash(e_pub), MixKey(e_pub) in PSK mode
    QByteArray ePub = pubKey();
    mixHash(ePub);
    mixKey(ePub);

    QByteArray tag = encryptAndHash(QByteArray()); // encrypt empty payload → 16-byte tag

    return ePub + tag;
}

// Initiator reads message 2: e, ee  (NNpsk0 — psk0 only modifies msg1)
// data should be 48 bytes: re_pub (32) + tag (16)
bool NoiseNNpsk0::readHandshake(const QByteArray &data)
{
    if (data.size() < 48)
        return false;

    QByteArray rePub = data.left(32);
    QByteArray ciphertext = data.mid(32); // 16 bytes (tag for empty payload)

    // Token 'e': read re_pub, MixHash(re_pub), MixKey(re_pub) in PSK mode
    mixHash(rePub);
    mixKey(rePub);

    // Token 'ee': DH(e, re), MixKey(shared)
    QByteArray shared = dh(rePub);
    if (shared.isEmpty())
        return false;
    mixKey(shared);

    // DecryptAndHash(ciphertext) - decrypt empty payload (just 16-byte tag)
    // NNpsk0 msg2 tokens are: e, ee  — no psk token in message 2
    QByteArray plaintext;
    if (!decryptAndHash(ciphertext, plaintext))
        return false;

    // Split: HKDF(ck, "") → k1, k2
    QByteArray k1, k2;
    hkdf2(m_ck, QByteArray(), k1, k2);
    m_sendKey = k1;
    m_recvKey = k2;
    m_sendN = 0;
    m_recvN = 0;
    m_complete = true;

    return true;
}

QByteArray NoiseNNpsk0::encrypt(const QByteArray &plaintext)
{
    return aeadEncrypt(m_sendKey, m_sendN++, QByteArray(), plaintext);
}

QByteArray NoiseNNpsk0::decrypt(const QByteArray &ciphertext)
{
    QByteArray plaintext;
    if (!aeadDecrypt(m_recvKey, m_recvN++, QByteArray(), ciphertext, plaintext))
        return QByteArray();
    return plaintext;
}

QByteArray NoiseNNpsk0::sha256(const QByteArray &data)
{
    QByteArray result(SHA256_DIGEST_LENGTH, 0);
    SHA256(reinterpret_cast<const unsigned char *>(data.constData()), data.size(),
           reinterpret_cast<unsigned char *>(result.data()));
    return result;
}

QByteArray NoiseNNpsk0::hmacSha256(const QByteArray &key, const QByteArray &data)
{
    QByteArray result(32, 0);
    unsigned int len = 32;
    HMAC(EVP_sha256(),
         key.constData(), key.size(),
         reinterpret_cast<const unsigned char *>(data.constData()), data.size(),
         reinterpret_cast<unsigned char *>(result.data()), &len);
    return result;
}

void NoiseNNpsk0::hkdf2(const QByteArray &ck, const QByteArray &ikm, QByteArray &out1, QByteArray &out2)
{
    QByteArray temp = hmacSha256(ck, ikm);
    out1 = hmacSha256(temp, QByteArray(1, '\x01'));
    out2 = hmacSha256(temp, out1 + QByteArray(1, '\x02'));
}

void NoiseNNpsk0::hkdf3(const QByteArray &ck, const QByteArray &ikm, QByteArray &out1, QByteArray &out2, QByteArray &out3)
{
    QByteArray temp = hmacSha256(ck, ikm);
    out1 = hmacSha256(temp, QByteArray(1, '\x01'));
    out2 = hmacSha256(temp, out1 + QByteArray(1, '\x02'));
    out3 = hmacSha256(temp, out2 + QByteArray(1, '\x03'));
}

void NoiseNNpsk0::mixHash(const QByteArray &data)
{
    m_h = sha256(m_h + data);
}

void NoiseNNpsk0::mixKey(const QByteArray &ikm)
{
    QByteArray newCk, tempK;
    hkdf2(m_ck, ikm, newCk, tempK);
    m_ck = newCk;
    m_k = tempK;
    m_n = 0;
}

void NoiseNNpsk0::mixKeyAndHash(const QByteArray &ikm)
{
    QByteArray newCk, tempH, tempK;
    hkdf3(m_ck, ikm, newCk, tempH, tempK);
    m_ck = newCk;
    mixHash(tempH);
    m_k = tempK;
    m_n = 0;
}

QByteArray NoiseNNpsk0::encryptAndHash(const QByteArray &plaintext)
{
    QByteArray ciphertext = aeadEncrypt(m_k, m_n++, m_h, plaintext);
    mixHash(ciphertext);
    return ciphertext;
}

bool NoiseNNpsk0::decryptAndHash(const QByteArray &ciphertext, QByteArray &plaintext)
{
    if (!aeadDecrypt(m_k, m_n++, m_h, ciphertext, plaintext))
        return false;
    mixHash(ciphertext);
    return true;
}

QByteArray NoiseNNpsk0::pubKey(void)
{
    QByteArray result(32, 0);
    size_t len = 32;
    EVP_PKEY_get_raw_public_key(m_ephemeral, reinterpret_cast<unsigned char *>(result.data()), &len);
    return result;
}

QByteArray NoiseNNpsk0::dh(const QByteArray &remotePub)
{
    EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                         reinterpret_cast<const unsigned char *>(remotePub.constData()), 32);
    if (!peer)
        return QByteArray();

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(m_ephemeral, nullptr);
    EVP_PKEY_derive_init(ctx);
    EVP_PKEY_derive_set_peer(ctx, peer);

    size_t secretLen = 32;
    QByteArray secret(32, 0);
    EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char *>(secret.data()), &secretLen);

    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(peer);
    return secret;
}

QByteArray NoiseNNpsk0::nonce(quint64 n)
{
    // 12 bytes: 4 zero bytes + 8-byte little-endian counter
    QByteArray result(12, 0);
    for (int i = 0; i < 8; i++)
        result[4 + i] = static_cast<char>((n >> (8 * i)) & 0xFF);
    return result;
}

QByteArray NoiseNNpsk0::aeadEncrypt(const QByteArray &key, quint64 n, const QByteArray &aad, const QByteArray &plaintext)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    QByteArray iv = nonce(n);
    QByteArray ciphertext(plaintext.size() + 16, 0);
    int outLen = 0, finalLen = 0;

    EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr);
    EVP_EncryptInit_ex(ctx, nullptr, nullptr,
        reinterpret_cast<const unsigned char *>(key.constData()),
        reinterpret_cast<const unsigned char *>(iv.constData()));

    if (!aad.isEmpty())
        EVP_EncryptUpdate(ctx, nullptr, &outLen, reinterpret_cast<const unsigned char *>(aad.constData()), aad.size());

    EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(ciphertext.data()), &outLen,
        reinterpret_cast<const unsigned char *>(plaintext.constData()), plaintext.size());
    EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(ciphertext.data()) + outLen, &finalLen);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16,
        reinterpret_cast<unsigned char *>(ciphertext.data()) + outLen + finalLen);

    EVP_CIPHER_CTX_free(ctx);
    return ciphertext;
}

bool NoiseNNpsk0::aeadDecrypt(const QByteArray &key, quint64 n, const QByteArray &aad, const QByteArray &ciphertext, QByteArray &plaintext)
{
    if (ciphertext.size() < 16)
        return false;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    QByteArray iv = nonce(n);
    int cipherLen = ciphertext.size() - 16;
    plaintext.resize(cipherLen);
    int outLen = 0, finalLen = 0;

    EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr);
    EVP_DecryptInit_ex(ctx, nullptr, nullptr,
        reinterpret_cast<const unsigned char *>(key.constData()),
        reinterpret_cast<const unsigned char *>(iv.constData()));

    if (!aad.isEmpty())
        EVP_DecryptUpdate(ctx, nullptr, &outLen, reinterpret_cast<const unsigned char *>(aad.constData()), aad.size());

    EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plaintext.data()), &outLen,
        reinterpret_cast<const unsigned char *>(ciphertext.constData()), cipherLen);

    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
        const_cast<char *>(ciphertext.constData()) + cipherLen);

    bool ok = (EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(plaintext.data()) + outLen, &finalLen) > 0);
    EVP_CIPHER_CTX_free(ctx);

    if (!ok)
        plaintext.clear();

    return ok;
}
