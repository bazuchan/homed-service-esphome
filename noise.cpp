#include "noise.h"
#include <cstring>

// Noise_NNpsk0_25519_ChaChaPoly_SHA256 initiator implementation, backed by noise-c
// (https://github.com/esphome-libs/noise-c, reference backend — no external crypto lib)

static const char *NOISE_PROTOCOL_NAME = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";

// Prologue is hardcoded: "NoiseAPIInit" + 0x00 0x00 (empty client hello)
static const QByteArray NOISE_PROLOGUE = QByteArray("NoiseAPIInit\x00\x00", 14);

NoiseNNpsk0::NoiseNNpsk0(const QByteArray &psk) : m_handshake(nullptr), m_sendCipher(nullptr), m_recvCipher(nullptr), m_complete(false)
{
    if (noise_handshakestate_new_by_name(&m_handshake, NOISE_PROTOCOL_NAME, NOISE_ROLE_INITIATOR) != NOISE_ERROR_NONE)
        return;

    noise_handshakestate_set_prologue(m_handshake, NOISE_PROLOGUE.constData(), NOISE_PROLOGUE.size());
    noise_handshakestate_set_pre_shared_key(m_handshake, reinterpret_cast<const uint8_t *>(psk.constData()), psk.size());

    if (noise_handshakestate_start(m_handshake) != NOISE_ERROR_NONE)
    {
        noise_handshakestate_free(m_handshake);
        m_handshake = nullptr;
    }
}

NoiseNNpsk0::~NoiseNNpsk0(void)
{
    if (m_handshake)
        noise_handshakestate_free(m_handshake);

    if (m_sendCipher)
        noise_cipherstate_free(m_sendCipher);

    if (m_recvCipher)
        noise_cipherstate_free(m_recvCipher);
}

// Initiator writes message 1: psk, e
// Returns: e_pub (32 bytes) + encrypted empty payload tag (16 bytes) = 48 bytes
QByteArray NoiseNNpsk0::writeHandshake(void)
{
    QByteArray message(128, 0);
    NoiseBuffer buf;

    if (!m_handshake || noise_handshakestate_get_action(m_handshake) != NOISE_ACTION_WRITE_MESSAGE)
        return QByteArray();

    noise_buffer_set_output(buf, reinterpret_cast<uint8_t *>(message.data()), message.size());

    if (noise_handshakestate_write_message(m_handshake, &buf, nullptr) != NOISE_ERROR_NONE)
        return QByteArray();

    message.resize(static_cast<int>(buf.size));
    return message;
}

// Initiator reads message 2: e, ee (NNpsk0 — psk0 only modifies message 1)
// data should be 48 bytes: re_pub (32) + tag (16)
bool NoiseNNpsk0::readHandshake(const QByteArray &data)
{
    QByteArray message = data, payload(128, 0);
    NoiseBuffer messageBuf, payloadBuf;

    if (!m_handshake || noise_handshakestate_get_action(m_handshake) != NOISE_ACTION_READ_MESSAGE)
        return false;

    noise_buffer_set_input(messageBuf, reinterpret_cast<uint8_t *>(message.data()), message.size());
    noise_buffer_set_output(payloadBuf, reinterpret_cast<uint8_t *>(payload.data()), payload.size());

    if (noise_handshakestate_read_message(m_handshake, &messageBuf, &payloadBuf) != NOISE_ERROR_NONE)
        return false;

    if (noise_handshakestate_get_action(m_handshake) != NOISE_ACTION_SPLIT)
        return false;

    if (noise_handshakestate_split(m_handshake, &m_sendCipher, &m_recvCipher) != NOISE_ERROR_NONE)
        return false;

    noise_handshakestate_free(m_handshake);
    m_handshake = nullptr;
    m_complete = true;

    return true;
}

QByteArray NoiseNNpsk0::encrypt(const QByteArray &plaintext)
{
    QByteArray buffer;
    NoiseBuffer buf;

    if (!m_sendCipher)
        return QByteArray();

    buffer = plaintext;
    buffer.append(QByteArray(static_cast<int>(noise_cipherstate_get_mac_length(m_sendCipher)), 0));

    noise_buffer_set_inout(buf, reinterpret_cast<uint8_t *>(buffer.data()), static_cast<size_t>(plaintext.size()), static_cast<size_t>(buffer.size()));

    if (noise_cipherstate_encrypt_with_ad(m_sendCipher, nullptr, 0, &buf) != NOISE_ERROR_NONE)
        return QByteArray();

    buffer.resize(static_cast<int>(buf.size));
    return buffer;
}

QByteArray NoiseNNpsk0::decrypt(const QByteArray &ciphertext)
{
    QByteArray buffer = ciphertext;
    NoiseBuffer buf;

    if (!m_recvCipher)
        return QByteArray();

    noise_buffer_set_input(buf, reinterpret_cast<uint8_t *>(buffer.data()), static_cast<size_t>(buffer.size()));

    if (noise_cipherstate_decrypt_with_ad(m_recvCipher, nullptr, 0, &buf) != NOISE_ERROR_NONE)
        return QByteArray();

    buffer.resize(static_cast<int>(buf.size));
    return buffer;
}
