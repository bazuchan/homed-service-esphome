#ifndef NOISE_H
#define NOISE_H

#include <QByteArray>
#include <noise/protocol.h>

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

    NoiseHandshakeState *m_handshake;
    NoiseCipherState *m_sendCipher;
    NoiseCipherState *m_recvCipher;
    bool m_complete;

};

#endif
