// Standalone ESPHome Native API debug client.
//
// Connects to a single device, runs the same handshake/discovery sequence as
// EspHomeDevice, and prints every message it receives with every protobuf
// field decoded generically (number, wire type, and all plausible
// interpretations of the value) instead of only the fields esphome.cpp
// currently knows how to read. Useful for finding field-mapping bugs (e.g.
// entities that get silently dropped because a field number assumption is
// wrong for a given ESPHome version) without rebuilding/redeploying the
// whole service.
//
// Usage:
//   ESPHOME_KEY=<base64 32-byte noise PSK> ./esphome-debug-client <host> [port]
//
// Build (from repo root, after ci/build.sh or ./build.sh has fetched
// ../homed-common at least once):
//   cd tools && qmake debug-client.pro && make

#include "../noise.h"
#include "../proto.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QTcpSocket>
#include <QTextStream>
#include <cstdio>
#include <cstdlib>

static QTextStream out(stdout);

static const char *msgTypeName(quint16 type)
{
    switch (type)
    {
        case 1:  return "HelloRequest";
        case 2:  return "HelloResponse";
        case 3:  return "ConnectRequest";
        case 4:  return "ConnectResponse";
        case 5:  return "DisconnectRequest";
        case 6:  return "DisconnectResponse";
        case 7:  return "PingRequest";
        case 8:  return "PingResponse";
        case 9:  return "DeviceInfoRequest";
        case 10: return "DeviceInfoResponse";
        case 11: return "ListEntitiesRequest";
        case 12: return "ListEntitiesBinarySensorResponse";
        case 13: return "ListEntitiesCoverResponse";
        case 14: return "ListEntitiesFanResponse";
        case 15: return "ListEntitiesLightResponse";
        case 16: return "ListEntitiesSensorResponse";
        case 17: return "ListEntitiesSwitchResponse";
        case 18: return "ListEntitiesTextSensorResponse";
        case 19: return "ListEntitiesDoneResponse";
        case 20: return "SubscribeStatesRequest";
        case 21: return "BinarySensorStateResponse";
        case 22: return "CoverStateResponse";
        case 23: return "FanStateResponse";
        case 24: return "LightStateResponse";
        case 25: return "SensorStateResponse";
        case 26: return "SwitchStateResponse";
        case 27: return "TextSensorStateResponse";
        case 46: return "ListEntitiesClimateResponse";
        case 47: return "ClimateStateResponse";
        case 49: return "ListEntitiesNumberResponse";
        case 50: return "NumberStateResponse";
        case 52: return "ListEntitiesSelectResponse";
        case 53: return "SelectStateResponse";
        case 61: return "ListEntitiesButtonResponse";
        default: return "?";
    }
}

// Prints every field of a decoded message, with every plausible interpretation
// of its value -- this is the point of the tool, don't assume a schema.
static void dumpFields(const QByteArray &payload)
{
    auto fields = ProtoDecoder::decode(payload);

    out << "  " << fields.count() << " field(s) decoded from " << payload.size() << " byte payload\n";

    for (const auto &f : fields)
    {
        out << "    field " << f.field() << " wireType " << f.wireType();

        switch (f.wireType())
        {
            case 0: // varint
                out << " varint=" << f.varint();
                break;

            case 2: // length-delimited (string/bytes/nested message)
            {
                QString asString = f.string();
                bool printable = true;

                for (QChar c : asString)
                {
                    if (c.unicode() < 0x20 && c != '\t')
                    {
                        printable = false;
                        break;
                    }
                }

                out << " len=" << f.bytes().size();

                if (printable && !asString.isEmpty())
                    out << " string=\"" << asString << "\"";

                out << " hex=" << QString::fromLatin1(f.bytes().toHex(' '));
                break;
            }

            case 5: // 32-bit (float or protobuf "fixed32", e.g. the entity key)
                out << " fixed32=" << f.fixed32() << " hex=0x" << QString::number(f.fixed32(), 16) << " float=" << f.floatVal();
                break;

            default:
                out << " (unhandled wire type)";
                break;
        }

        out << "\n";
    }

    out.flush();
}

// Accumulates raw TCP bytes and pulls out complete [0x01][len_hi][len_lo][payload]
// frames, same framing EspHomeDevice::onReadyRead uses for both the plaintext
// handshake phase and the post-handshake encrypted phase.
class FrameReader
{

public:

    explicit FrameReader(QTcpSocket &socket) : m_socket(socket) {}

    // Blocks (with a timeout) until a full frame is available, or returns
    // false on timeout/disconnect.
    bool readFrame(QByteArray &framePayload, int timeoutMs = 10000)
    {
        while (true)
        {
            if (tryExtractFrame(framePayload))
                return true;

            if (!m_socket.waitForReadyRead(timeoutMs))
                return false;

            m_buffer.append(m_socket.readAll());
        }
    }

private:

    bool tryExtractFrame(QByteArray &framePayload)
    {
        if (m_buffer.size() < 3)
            return false;

        if (static_cast<quint8>(m_buffer.at(0)) != 0x01)
        {
            out << "bad frame indicator: " << static_cast<quint8>(m_buffer.at(0)) << "\n";
            out.flush();
            ::exit(1);
        }

        quint16 frameSize = (static_cast<quint8>(m_buffer.at(1)) << 8) | static_cast<quint8>(m_buffer.at(2));

        if (m_buffer.size() < 3 + frameSize)
            return false;

        framePayload = m_buffer.mid(3, frameSize);
        m_buffer.remove(0, 3 + frameSize);
        return true;
    }

    QTcpSocket &m_socket;
    QByteArray m_buffer;

};

static void sendFrame(QTcpSocket &socket, const QByteArray &payload)
{
    quint16 len = static_cast<quint16>(payload.size());
    QByteArray frame;
    frame.append('\x01');
    frame.append(static_cast<char>((len >> 8) & 0xFF));
    frame.append(static_cast<char>(len & 0xFF));
    frame.append(payload);
    socket.write(frame);
    socket.waitForBytesWritten(2000);
}

static void sendMessage(QTcpSocket &socket, NoiseNNpsk0 &noise, quint16 type, const QByteArray &body = QByteArray())
{
    quint16 dataLen = static_cast<quint16>(body.size());
    QByteArray inner;
    inner.append(static_cast<char>((type >> 8) & 0xFF));
    inner.append(static_cast<char>(type & 0xFF));
    inner.append(static_cast<char>((dataLen >> 8) & 0xFF));
    inner.append(static_cast<char>(dataLen & 0xFF));
    inner.append(body);
    sendFrame(socket, noise.encrypt(inner));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    if (argc < 2)
    {
        out << "usage: ESPHOME_KEY=<base64 psk> " << argv[0] << " <host> [port]\n";
        out.flush();
        return 1;
    }

    QString host = QString::fromLocal8Bit(argv[1]);
    quint16 port = argc > 2 ? static_cast<quint16>(QString::fromLocal8Bit(argv[2]).toUInt()) : 6053;

    const char *keyEnv = std::getenv("ESPHOME_KEY");
    if (!keyEnv || !*keyEnv)
    {
        out << "ESPHOME_KEY environment variable not set (base64-encoded 32-byte noise PSK)\n";
        out.flush();
        return 1;
    }

    QByteArray psk = QByteArray::fromBase64(QByteArray(keyEnv));
    if (psk.size() != 32)
    {
        out << "ESPHOME_KEY did not decode to 32 bytes (got " << psk.size() << ")\n";
        out.flush();
        return 1;
    }

    QTcpSocket socket;
    out << "connecting to " << host << ":" << port << "...\n";
    out.flush();
    socket.connectToHost(host, port);

    if (!socket.waitForConnected(5000))
    {
        out << "connect failed: " << socket.errorString() << "\n";
        out.flush();
        return 1;
    }

    out << "TCP connected\n";
    out.flush();

    FrameReader reader(socket);
    NoiseNNpsk0 noise(psk);

    // Client hello: [0x01][0x00][0x00] (empty), then the noise handshake frame
    QByteArray clientHello;
    clientHello.append('\x01');
    clientHello.append('\x00');
    clientHello.append('\x00');
    socket.write(clientHello);

    QByteArray handshakeData = noise.writeHandshake();
    QByteArray noisePayload;
    noisePayload.append('\x00');
    noisePayload.append(handshakeData);
    sendFrame(socket, noisePayload);

    // Server hello: [0x01][name\0][mac\0]
    QByteArray serverHello;
    if (!reader.readFrame(serverHello))
    {
        out << "timed out waiting for server hello\n";
        out.flush();
        return 1;
    }

    if (serverHello.isEmpty() || serverHello.at(0) != '\x01')
    {
        out << "bad server hello\n";
        out.flush();
        return 1;
    }

    int nameEnd = serverHello.indexOf('\x00', 1);
    out << "server name: " << QString::fromUtf8(serverHello.mid(1, nameEnd > 1 ? nameEnd - 1 : -1)) << "\n";
    out.flush();

    // Noise handshake response: [0x00][re_pub+tag]
    QByteArray noiseResponse;
    if (!reader.readFrame(noiseResponse))
    {
        out << "timed out waiting for noise handshake response\n";
        out.flush();
        return 1;
    }

    if (noiseResponse.isEmpty() || noiseResponse.at(0) != '\x00')
    {
        out << "noise handshake rejected: " << QString::fromUtf8(noiseResponse.mid(1)) << "\n";
        out.flush();
        return 1;
    }

    if (!noise.readHandshake(noiseResponse.mid(1)))
    {
        out << "noise handshake failed (wrong key?)\n";
        out.flush();
        return 1;
    }

    out << "noise handshake complete\n";
    out.flush();

    ProtoEncoder hello;
    hello.addString(1, "esphome-debug-client");
    hello.addVarint(2, 1); // api_version_major
    hello.addVarint(3, 14); // api_version_minor
    sendMessage(socket, noise, 1 /* HelloRequest */, hello.data());

    int sensorCount = 0, listEntitiesTotal = 0, stateCount = 0;
    bool subscribed = false;

    while (true)
    {
        // Once we've subscribed, a read timeout just means "no more state
        // updates right now" -- that's a normal end condition, not an error.
        QByteArray framePayload;
        if (!reader.readFrame(framePayload, 10000))
        {
            if (subscribed)
                out << "\nno further state updates after " << stateCount << " (waited 10s) -- exiting\n";
            else
                out << "timed out waiting for next message\n";
            break;
        }

        QByteArray plaintext = noise.decrypt(framePayload);
        if (plaintext.isEmpty())
        {
            out << "decryption failed on a received frame\n";
            break;
        }

        if (plaintext.size() < 4)
            continue;

        quint16 msgType = (static_cast<quint8>(plaintext.at(0)) << 8) | static_cast<quint8>(plaintext.at(1));
        quint16 dataLen = (static_cast<quint8>(plaintext.at(2)) << 8) | static_cast<quint8>(plaintext.at(3));
        QByteArray msgData = plaintext.mid(4, dataLen);

        out << "\n<<< message type " << msgType << " (" << msgTypeName(msgType) << ")\n";
        out.flush();
        dumpFields(msgData);

        switch (msgType)
        {
            case 2: // HelloResponse
                sendMessage(socket, noise, 9 /* DeviceInfoRequest */);
                break;

            case 10: // DeviceInfoResponse
                sendMessage(socket, noise, 11 /* ListEntitiesRequest */);
                break;

            case 16: // ListEntitiesSensorResponse
                sensorCount++;
                listEntitiesTotal++;
                break;

            case 12: case 13: case 15: case 17: case 18: case 46: case 49: case 52: case 61:
                listEntitiesTotal++;
                break;

            case 19: // ListEntitiesDoneResponse
                out << "\n=== " << listEntitiesTotal << " entities total, " << sensorCount << " of them sensors -- subscribing to state updates ===\n";
                out.flush();
                sendMessage(socket, noise, 20 /* SubscribeStatesRequest */);
                subscribed = true;
                break;

            case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 47: case 50: case 53:
                stateCount++;
                break;

            default:
                break;
        }
    }

    out << "\n=== received " << stateCount << " state update(s) total ===\n";
    out.flush();

    socket.disconnectFromHost();
    return 0;
}
