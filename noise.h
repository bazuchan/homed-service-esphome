#ifndef NOISE_H
#define NOISE_H

#include <QByteArray>
#include <openssl/evp.h>

class NoiseNNpsk0
{

public:

    NoiseNNpsk0(const QByteArray &psk);
    ~NoiseNNpsk0(void);

    QByteArray writeHandshake(void);
    bool readHandshake(const QByteArray &data);

    QByteArray encrypt(const QByteArray &plaintext);
    QByteArray decrypt(const QByteArray &ciphertext);

    bool complete(void) { return m_complete; }

private:

    QByteArray m_h, m_ck, m_k, m_psk;
    quint64 m_n;

    QByteArray m_sendKey, m_recvKey;
    quint64 m_sendN, m_recvN;

    EVP_PKEY *m_ephemeral;
    bool m_complete;

    QByteArray sha256(const QByteArray &data);
    QByteArray hmacSha256(const QByteArray &key, const QByteArray &data);
    void hkdf2(const QByteArray &ck, const QByteArray &ikm, QByteArray &out1, QByteArray &out2);
    void hkdf3(const QByteArray &ck, const QByteArray &ikm, QByteArray &out1, QByteArray &out2, QByteArray &out3);

    void mixHash(const QByteArray &data);
    void mixKey(const QByteArray &ikm);
    void mixKeyAndHash(const QByteArray &ikm);

    QByteArray encryptAndHash(const QByteArray &plaintext);
    bool decryptAndHash(const QByteArray &ciphertext, QByteArray &plaintext);

    QByteArray pubKey(void);
    QByteArray dh(const QByteArray &remotePub);

    QByteArray nonce(quint64 n);
    QByteArray aeadEncrypt(const QByteArray &key, quint64 n, const QByteArray &aad, const QByteArray &plaintext);
    bool aeadDecrypt(const QByteArray &key, quint64 n, const QByteArray &aad, const QByteArray &ciphertext, QByteArray &plaintext);

};

#endif
