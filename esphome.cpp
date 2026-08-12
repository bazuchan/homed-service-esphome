#include "esphome.h"
#include "logger.h"
#include <QDateTime>
#include <cmath>

static const int RECONNECT_INTERVAL = 10000;  // 10 seconds
static const int PING_INTERVAL      = 60000;  // 60 seconds
static const int HANDSHAKE_TIMEOUT  = 15000;  // 15 seconds -- TCP connects but the device never completes (or never starts) the ESPHome handshake

// mqttSafe() doesn't touch spaces; used consistently by subDevice()/findEndpointByKey()/onDisconnected() so their address lookups keep matching.
static QString subDeviceAddressSuffix(const QString &name)
{
    return mqttSafe(name).replace(' ', '_');
}

// homed-web's exposeMeta() (js/expose.js) parses every objectId as
// expose.split('_'), taking list[0] as the display name and list[1] (if
// numeric) as a disambiguating suffix appended back onto the title. An
// objectId with *any* underscore in it trips that parsing -- multi-word
// names get truncated to their first word, and a numeric second word gets
// silently appended to the title. Zero underscores sidesteps both: split('_')
// then returns a single-element array, so list[0] is the whole string and
// list[1] is undefined. Collapses any separator (spaces in a human name,
// underscores in a wire device_class like "signal_strength") into a single
// camelCase token -- no underscores, ever.
static QString camelCase(const QString &text)
{
    QString result;
    bool capitalizeNext = false;

    for (const QChar &c : text)
    {
        if (!c.isLetterOrNumber())
        {
            capitalizeNext = true;
            continue;
        }

        result.append(capitalizeNext && !result.isEmpty() ? c.toUpper() : c.toLower());
        capitalizeNext = false;
    }

    return result;
}

// ClimateMode (api.proto). FAN_ONLY maps to "fan", not "fan_only" -- that's
// the token homed-common's ThermostatObject stores internally, translating
// to/from HA's "fan_only" only in its own HA discovery templates.
static QString climateModeName(quint64 value)
{
    switch (value)
    {
        case 0: return "off";
        case 1: return "heat_cool";
        case 2: return "cool";
        case 3: return "heat";
        case 4: return "fan";
        case 5: return "dry";
        case 6: return "auto";
        default: return QString();
    }
}

// ClimateFanMode (api.proto)
static QString climateFanModeName(quint64 value)
{
    switch (value)
    {
        case 0: return "on";
        case 1: return "off";
        case 2: return "auto";
        case 3: return "low";
        case 4: return "medium";
        case 5: return "high";
        case 6: return "middle";
        case 7: return "focus";
        case 8: return "diffuse";
        case 9: return "quiet";
        default: return QString();
    }
}

// ClimatePreset (api.proto)
static QString climatePresetName(quint64 value)
{
    switch (value)
    {
        case 0: return "none";
        case 1: return "home";
        case 2: return "away";
        case 3: return "boost";
        case 4: return "comfort";
        case 5: return "eco";
        case 6: return "sleep";
        case 7: return "activity";
        default: return QString();
    }
}

// ClimateAction (api.proto) -- collapsed to the single "running" boolean
// homed-common's ThermostatObject action_template expects; anything other
// than idle/off counts as actively running.
static bool climateActionRunning(quint64 value)
{
    return value != 0 && value != 4;
}

// Reverse of climateModeName()/climateFanModeName()/climatePresetName() --
// -1 (found=false) when the value isn't one of the standard tokens (a custom
// fan mode/preset name instead, sent back to the device as a string field).
static int climateModeValue(const QString &name)
{
    static const QStringList names = {"off", "heat_cool", "cool", "heat", "fan", "dry", "auto"};
    return names.indexOf(name);
}

static int climateFanModeValue(const QString &name)
{
    static const QStringList names = {"on", "off", "auto", "low", "medium", "high", "middle", "focus", "diffuse", "quiet"};
    return names.indexOf(name);
}

static int climatePresetValue(const QString &name)
{
    static const QStringList names = {"none", "home", "away", "boost", "comfort", "eco", "sleep", "activity"};
    return names.indexOf(name);
}

// ==================== EspHomeDevice ====================

EspHomeDevice::EspHomeDevice(const Device &device, DeviceList *devices, QObject *parent)
    : QObject(parent), m_device(device), m_devices(devices), m_state(State::Disconnected), m_noise(nullptr), m_noiseHandshakeState(0)
{
    m_socket = new QTcpSocket(this);
    m_pingTimer = new QTimer(this);
    m_reconnectTimer = new QTimer(this);
    m_handshakeTimer = new QTimer(this);

    m_pingTimer->setInterval(PING_INTERVAL);
    m_pingTimer->setSingleShot(false);
    m_reconnectTimer->setInterval(RECONNECT_INTERVAL);
    m_reconnectTimer->setSingleShot(true);
    m_handshakeTimer->setInterval(HANDSHAKE_TIMEOUT);
    m_handshakeTimer->setSingleShot(true);

    connect(m_socket, &QTcpSocket::connected, this, &EspHomeDevice::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &EspHomeDevice::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &EspHomeDevice::onReadyRead);
    connect(m_socket, &QAbstractSocket::errorOccurred, this, &EspHomeDevice::onError);
    connect(m_pingTimer, &QTimer::timeout, this, &EspHomeDevice::onPingTimer);
    connect(m_reconnectTimer, &QTimer::timeout, this, &EspHomeDevice::onReconnectTimer);
    connect(m_handshakeTimer, &QTimer::timeout, this, &EspHomeDevice::onHandshakeTimer);
}

EspHomeDevice::~EspHomeDevice(void)
{
    delete m_noise;
}

void EspHomeDevice::connectToDevice(void)
{
    if (m_state != State::Disconnected)
        return;

    m_state = State::Connecting;
    m_rxBuf.clear();
    m_noiseHandshakeState = 0;

    logInfo << m_device << "connecting to" << m_device->host() << "port" << m_device->port();
    m_socket->connectToHost(m_device->host(), m_device->port());
}

void EspHomeDevice::disconnectFromDevice(void)
{
    m_pingTimer->stop();
    m_reconnectTimer->stop();
    m_handshakeTimer->stop();
    m_state = State::Disconnected;
    m_socket->disconnectFromHost();
}

void EspHomeDevice::onConnected(void)
{
    logInfo << m_device << "TCP connected";
    m_state = State::ClientHello;
    m_handshakeTimer->start();

    delete m_noise;
    m_noise = new NoiseNNpsk0(m_device->encryptionKey());

    // Send NOISE_HELLO (empty client hello) + Noise write message
    // Frame: [0x01][size_hi][size_lo][payload]
    // Empty client hello: [0x01][0x00][0x00]
    QByteArray clientHello;
    clientHello.append('\x01'); // indicator
    clientHello.append('\x00'); // size hi
    clientHello.append('\x00'); // size lo

    // Noise write message: [0x01][size_hi][size_lo][0x00][48-byte handshake data]
    QByteArray handshakeData = m_noise->writeHandshake(); // 48 bytes
    quint16 frameLen = static_cast<quint16>(handshakeData.size() + 1); // +1 for 0x00 prefix
    QByteArray noiseFrame;
    noiseFrame.append('\x01');
    noiseFrame.append(static_cast<char>((frameLen >> 8) & 0xFF));
    noiseFrame.append(static_cast<char>(frameLen & 0xFF));
    noiseFrame.append('\x00'); // success byte prefix
    noiseFrame.append(handshakeData);

    sendRaw(clientHello + noiseFrame);
    m_state = State::ServerHello;
    m_noiseHandshakeState = 0;
}

void EspHomeDevice::onDisconnected(void)
{
    logInfo << m_device << "disconnected";
    m_pingTimer->stop();
    m_handshakeTimer->stop();
    m_rxBuf.clear();
    m_noiseHandshakeState = 0;

    bool wasReady = (m_state == State::Ready || m_state == State::Subscribe || m_state == State::ListEntities);
    m_state = State::Disconnected;

    delete m_noise;
    m_noise = nullptr;

    if (wasReady)
    {
        m_device->setAvailability(Availability::Offline);
        emit availabilityChanged(m_device.data());

        // sub-devices share this connection -- they're unreachable too now, same as the parent
        for (const auto &sub : knownSubDevices())
        {
            sub->setAvailability(Availability::Offline);
            emit availabilityChanged(sub.data());
        }
    }

    if (m_device->active())
        m_reconnectTimer->start();
}

void EspHomeDevice::onError(QAbstractSocket::SocketError error)
{
    logWarning << m_device << "socket error:" << m_socket->errorString();
    Q_UNUSED(error)
}

void EspHomeDevice::onPingTimer(void)
{
    if (m_state == State::Ready || m_state == State::Subscribe)
        sendMessage(MsgType::PingRequest);
}

void EspHomeDevice::onReconnectTimer(void)
{
    m_state = State::Disconnected;
    connectToDevice();
}

void EspHomeDevice::onHandshakeTimer(void)
{
    // TCP connects fine but the device never sends anything (or hangs partway
    // through the handshake) -- onDisconnected() only fires from the remote
    // actually closing the connection or a socket-level error, neither of
    // which happens on a silently-open-but-idle socket, so without this the
    // connection (and its reconnectTimer) would just sit stuck forever.
    // abort() (rather than disconnectFromHost()) is deliberate: a graceful
    // close still depends on the same uncooperative peer responding, which is
    // exactly what we're trying to escape. abort() tears the socket down
    // immediately regardless, and still triggers onDisconnected() -> the
    // normal m_reconnectTimer path.
    logWarning << m_device << "handshake timed out after" << HANDSHAKE_TIMEOUT / 1000 << "s, reconnecting";
    m_socket->abort();
}

void EspHomeDevice::onReadyRead(void)
{
    m_rxBuf.append(m_socket->readAll());

    while (!m_rxBuf.isEmpty())
    {
        if (m_rxBuf.size() < 3)
            return; // need at least 3 bytes for frame header

        if (m_rxBuf.at(0) != '\x01')
        {
            logWarning << m_device << "bad frame indicator:" << (quint8)m_rxBuf.at(0);
            m_socket->disconnectFromHost();
            return;
        }

        quint16 frameSize = (static_cast<quint8>(m_rxBuf.at(1)) << 8) | static_cast<quint8>(m_rxBuf.at(2));

        if (m_rxBuf.size() < 3 + frameSize)
            return; // incomplete frame

        QByteArray framePayload = m_rxBuf.mid(3, frameSize);
        m_rxBuf.remove(0, 3 + frameSize);

        if (m_state == State::ServerHello)
        {
            // Received server hello: [0x01][name\0][mac\0]
            if (framePayload.isEmpty() || framePayload.at(0) != '\x01')
            {
                logWarning << m_device << "bad server hello";
                m_socket->disconnectFromHost();
                return;
            }

            int nameEnd = framePayload.indexOf('\x00', 1);
            if (nameEnd > 1)
            {
                QString serverName = mqttSafe(QString::fromUtf8(framePayload.mid(1, nameEnd - 1)));
                logInfo << m_device << "server name:" << serverName;

                if (!serverName.isEmpty() && m_device->name() == m_device->address())
                {
                    logInfo << m_device << "adopting device name" << serverName;
                    m_device->setName(serverName);
                }
            }

            m_state = State::NoiseHandshake;
        }
        else if (m_state == State::NoiseHandshake)
        {
            // Received noise handshake response
            if (framePayload.isEmpty() || framePayload.at(0) != '\x00')
            {
                logWarning << m_device << "noise handshake rejected:" << framePayload.mid(1);
                m_socket->disconnectFromHost();
                return;
            }

            QByteArray noiseData = framePayload.mid(1); // skip 0x00 byte
            if (!m_noise->readHandshake(noiseData))
            {
                logWarning << m_device << "noise handshake failed (bad key?)";
                m_socket->disconnectFromHost();
                return;
            }

            logInfo << m_device << "noise handshake complete";
            m_state = State::Ready;
            m_handshakeTimer->stop();
            m_pingTimer->start();

            m_device->updateLastSeen();
            m_device->setAvailability(Availability::Online);

            // Send HelloRequest
            ProtoEncoder hello;
            hello.addString(1, "homed-esphome");
            hello.addVarint(2, 1); // api_version_major
            hello.addVarint(3, 14); // api_version_minor
            sendMessage(MsgType::HelloRequest, hello.data());
        }
        else if (m_state == State::Ready || m_state == State::ListEntities || m_state == State::Subscribe)
        {
            // Decrypt noise frame
            if (!m_noise || !m_noise->complete())
            {
                logWarning << m_device << "received data before noise complete";
                return;
            }

            QByteArray plaintext = m_noise->decrypt(framePayload);
            if (plaintext.isEmpty())
            {
                logWarning << m_device << "decryption failed";
                m_socket->disconnectFromHost();
                return;
            }

            if (plaintext.size() < 4)
                continue;

            quint16 msgType = (static_cast<quint8>(plaintext.at(0)) << 8) | static_cast<quint8>(plaintext.at(1));
            quint16 dataLen = (static_cast<quint8>(plaintext.at(2)) << 8) | static_cast<quint8>(plaintext.at(3));
            QByteArray msgData = plaintext.mid(4, dataLen);

            processMessage(msgType, msgData);
        }
    }
}

void EspHomeDevice::sendRaw(const QByteArray &data)
{
    m_socket->write(data);
}

void EspHomeDevice::sendMessage(quint16 type, const QByteArray &payload)
{
    if (!m_noise || !m_noise->complete())
        return;

    quint16 dataLen = static_cast<quint16>(payload.size());
    QByteArray inner;
    inner.append(static_cast<char>((type >> 8) & 0xFF));
    inner.append(static_cast<char>(type & 0xFF));
    inner.append(static_cast<char>((dataLen >> 8) & 0xFF));
    inner.append(static_cast<char>(dataLen & 0xFF));
    inner.append(payload);

    QByteArray encrypted = m_noise->encrypt(inner);
    quint16 frameLen = static_cast<quint16>(encrypted.size());

    QByteArray frame;
    frame.append('\x01');
    frame.append(static_cast<char>((frameLen >> 8) & 0xFF));
    frame.append(static_cast<char>(frameLen & 0xFF));
    frame.append(encrypted);

    sendRaw(frame);
}

void EspHomeDevice::processMessage(quint16 type, const QByteArray &payload)
{
    switch (type)
    {
        case MsgType::HelloResponse:
        {
            auto fields = ProtoDecoder::decode(payload);
            for (const auto &f : fields)
            {
                if (f.field() == 3 && f.wireType() == 2)
                    logInfo << m_device << "server info:" << f.string();
            }

            // Request device info
            sendMessage(MsgType::DeviceInfoRequest);
            break;
        }

        case MsgType::DeviceInfoResponse:
        {
            auto fields = ProtoDecoder::decode(payload);
            m_subDeviceNames.clear();
            for (const auto &f : fields)
            {
                if (f.field() == 4 && f.wireType() == 2)
                    m_device->setEsphomeVersion(f.string());
                else if (f.field() == 6 && f.wireType() == 2)
                    m_device->setModelName(f.string());
                else if (f.field() == 12 && f.wireType() == 2)
                    m_device->setManufacturerName(f.string());
                else if (f.field() == 20 && f.wireType() == 2)
                {
                    // nested DeviceInfo { uint32 device_id = 1; string name = 2; uint32 area_id = 3; }
                    auto subFields = ProtoDecoder::decode(f.bytes());
                    quint32 subId = 0;
                    QString subName;

                    for (const auto &sf : subFields)
                    {
                        if (sf.field() == 1 && sf.wireType() == 0) subId = static_cast<quint32>(sf.varint());
                        else if (sf.field() == 2 && sf.wireType() == 2) subName = sf.string();
                    }

                    if (subId > 0)
                        m_subDeviceNames.insert(subId, subName);
                }
            }

            // Request entity list
            m_pendingEntities.clear();
            m_state = State::ListEntities;
            sendMessage(MsgType::ListEntitiesRequest);
            break;
        }

        case MsgType::ListEntitiesBinary:
        case MsgType::ListEntitiesCover:
        case MsgType::ListEntitiesLight:
        case MsgType::ListEntitiesSensor:
        case MsgType::ListEntitiesSwitch:
        case MsgType::ListEntitiesTextSensor:
        case MsgType::ListEntitiesClimate:
        case MsgType::ListEntitiesNumber:
        case MsgType::ListEntitiesSelect:
        case MsgType::ListEntitiesLock:
        case MsgType::ListEntitiesButton:
            processEntityInfo(type, payload);
            break;

        case MsgType::ListEntitiesDone:
            applyDiscoveredEntities(); // emits entitiesDiscovered for the main device and any sub-devices
            m_state = State::Subscribe;
            sendMessage(MsgType::SubscribeStates);
            break;

        case MsgType::StateBinary:
        case MsgType::StateCover:
        case MsgType::StateLight:
        case MsgType::StateSensor:
        case MsgType::StateSwitch:
        case MsgType::StateTextSensor:
        case MsgType::StateClimate:
        case MsgType::StateNumber:
        case MsgType::StateSelect:
        case MsgType::StateLock:
            processStateUpdate(type, payload);
            break;

        case MsgType::PingResponse:
            m_device->updateLastSeen();
            emit lastSeenUpdated(m_device.data());

            // sub-devices share this connection -- a ping response confirms they're just as reachable as the parent
            for (const auto &sub : knownSubDevices())
            {
                sub->updateLastSeen();
                emit lastSeenUpdated(sub.data());
            }
            break;

        case MsgType::DisconnectRequest:
            sendMessage(MsgType::DisconnectResponse);
            m_socket->disconnectFromHost();
            break;

        default:
            break;
    }
}

void EspHomeDevice::processEntityInfo(quint16 type, const QByteArray &payload)
{
    auto fields = ProtoDecoder::decode(payload);
    EntityInfo info = {};

    logInfo << m_device << "received entity info message type" << type << "payload size" << payload.size() << "(" << fields.count() << "fields)";

    for (const auto &f : fields)
    {
        if (f.field() == 3 && f.wireType() == 2) { info.name = f.string(); continue; }

        switch (type)
        {
            case MsgType::ListEntitiesSwitch:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 5 && f.wireType() == 2) info.icon = f.string();
                else if (f.field() == 8 && f.wireType() == 0) info.toggleCategory = (f.varint() == 1 || f.varint() == 2); // CONFIG or DIAGNOSTIC
                else if (f.field() == 9 && f.wireType() == 2) info.deviceClass = f.string();
                else if (f.field() == 10 && f.wireType() == 0) info.deviceId = static_cast<quint32>(f.varint());
                break;

            case MsgType::ListEntitiesBinary:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 5 && f.wireType() == 2) info.deviceClass = f.string();
                else if (f.field() == 8 && f.wireType() == 2) info.icon = f.string();
                else if (f.field() == 10 && f.wireType() == 0) info.deviceId = static_cast<quint32>(f.varint());
                break;

            case MsgType::ListEntitiesCover:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 6 && f.wireType() == 0) info.supportsPosition = f.boolean();
                else if (f.field() == 7 && f.wireType() == 0) info.supportsTilt = f.boolean();
                else if (f.field() == 8 && f.wireType() == 2) info.deviceClass = f.string();
                else if (f.field() == 10 && f.wireType() == 2) info.icon = f.string();
                else if (f.field() == 13 && f.wireType() == 0) info.deviceId = static_cast<quint32>(f.varint());
                break;

            case MsgType::ListEntitiesSensor:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                // field 5 is icon, not device_class (that's field 9 below) --
                // unlike binary_sensor/switch/text_sensor/button, where 5/8/9
                // really is device_class depending on the message.
                else if (f.field() == 5 && f.wireType() == 2) info.icon = f.string();
                else if (f.field() == 6 && f.wireType() == 2) info.unit = f.string();
                else if (f.field() == 7 && f.wireType() == 0) info.accuracyDecimals = static_cast<int>(f.varint());
                else if (f.field() == 9 && f.wireType() == 2) info.deviceClass = f.string();
                else if (f.field() == 10 && f.wireType() == 0)
                {
                    switch (f.varint())
                    {
                        case 1: info.stateClass = "measurement"; break;
                        case 2: info.stateClass = "total_increasing"; break;
                        case 3: info.stateClass = "total"; break;
                        default: break;
                    }
                }
                else if (f.field() == 14 && f.wireType() == 0) info.deviceId = static_cast<quint32>(f.varint());
                break;

            case MsgType::ListEntitiesTextSensor:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 5 && f.wireType() == 2) info.icon = f.string();
                else if (f.field() == 8 && f.wireType() == 2) info.deviceClass = f.string();
                else if (f.field() == 9 && f.wireType() == 0) info.deviceId = static_cast<quint32>(f.varint());
                break;

            case MsgType::ListEntitiesLight:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 9 && f.wireType() == 5) info.minMireds = f.floatVal();
                else if (f.field() == 10 && f.wireType() == 5) info.maxMireds = f.floatVal();
                else if (f.field() == 14 && f.wireType() == 2) info.icon = f.string();
                else if (f.field() == 16 && f.wireType() == 0) info.deviceId = static_cast<quint32>(f.varint());
                else if (f.field() == 12 && f.wireType() == 0)
                {
                    // supported_color_modes: determine light capabilities
                    quint64 mode = f.varint();
                    // COLOR_MODE_BRIGHTNESS=3, COLOR_MODE_COLOR_TEMPERATURE=11, COLOR_MODE_RGB=35, etc.
                    if (mode >= 3 && !info.lightOptions.contains("level"))
                        info.lightOptions.append("level");
                    if (mode == 11 || mode == 47) // CT or RGB+CT
                        if (!info.lightOptions.contains("colorTemperature"))
                            info.lightOptions.append("colorTemperature");
                    if (mode == 35 || mode == 39 || mode == 47 || mode == 51) // RGB variants
                        if (!info.lightOptions.contains("color"))
                            info.lightOptions.append("color");
                }
                break;

            case MsgType::ListEntitiesClimate:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 19 && f.wireType() == 2) info.icon = f.string();
                else if (f.field() == 26 && f.wireType() == 0) info.deviceId = static_cast<quint32>(f.varint());
                else if (f.field() == 7 && f.wireType() == 0)
                {
                    QString mode = climateModeName(f.varint());
                    if (!mode.isEmpty() && !info.climateModes.contains(mode)) info.climateModes.append(mode);
                }
                else if (f.field() == 8 && f.wireType() == 5) info.minValue = f.floatVal();
                else if (f.field() == 9 && f.wireType() == 5) info.maxValue = f.floatVal();
                else if (f.field() == 10 && f.wireType() == 5) info.step = f.floatVal();
                else if (f.field() == 12 && f.wireType() == 0) info.climateSupportsAction = f.boolean();
                else if (f.field() == 13 && f.wireType() == 0)
                {
                    QString mode = climateFanModeName(f.varint());
                    if (!mode.isEmpty() && !info.climateFanModes.contains(mode)) info.climateFanModes.append(mode);
                }
                else if (f.field() == 15 && f.wireType() == 2) info.climateFanModes.append(f.string());
                else if (f.field() == 16 && f.wireType() == 0)
                {
                    QString preset = climatePresetName(f.varint());
                    if (!preset.isEmpty() && !info.climatePresets.contains(preset)) info.climatePresets.append(preset);
                }
                else if (f.field() == 17 && f.wireType() == 2) info.climatePresets.append(f.string());
                break;

            case MsgType::ListEntitiesSelect:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 5 && f.wireType() == 2) info.icon = f.string();
                else if (f.field() == 6 && f.wireType() == 2) info.selectOptions.append(f.string());
                else if (f.field() == 9 && f.wireType() == 0) info.deviceId = static_cast<quint32>(f.varint());
                break;

            case MsgType::ListEntitiesNumber:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 5 && f.wireType() == 2) info.icon = f.string();
                else if (f.field() == 6 && f.wireType() == 5) info.minValue = f.floatVal();
                else if (f.field() == 7 && f.wireType() == 5) info.maxValue = f.floatVal();
                else if (f.field() == 8 && f.wireType() == 5) info.step = f.floatVal();
                else if (f.field() == 11 && f.wireType() == 2) info.unit = f.string();
                else if (f.field() == 14 && f.wireType() == 0) info.deviceId = static_cast<quint32>(f.varint());
                break;

            case MsgType::ListEntitiesLock:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 5 && f.wireType() == 2) info.icon = f.string();
                else if (f.field() == 12 && f.wireType() == 0) info.deviceId = static_cast<quint32>(f.varint());
                break;

            case MsgType::ListEntitiesButton:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 5 && f.wireType() == 2) info.icon = f.string();
                else if (f.field() == 8 && f.wireType() == 2) info.deviceClass = f.string();
                else if (f.field() == 9 && f.wireType() == 0) info.deviceId = static_cast<quint32>(f.varint());
                break;
        }
    }

    if (info.key == 0)
    {
        logWarning << m_device << "ignoring entity of type" << type << "with key 0 (" << fields.count() << "fields decoded)";
        return;
    }

    switch (type)
    {
        case MsgType::ListEntitiesSwitch:    info.type = "switch"; break;
        case MsgType::ListEntitiesBinary:    info.type = "binary_sensor"; break;
        case MsgType::ListEntitiesCover:     info.type = "cover"; break;
        case MsgType::ListEntitiesSensor:    info.type = "sensor"; break;
        case MsgType::ListEntitiesTextSensor:info.type = "text_sensor"; break;
        case MsgType::ListEntitiesLight:     info.type = "light"; break;
        case MsgType::ListEntitiesClimate:   info.type = "climate"; break;
        case MsgType::ListEntitiesSelect:    info.type = "select"; break;
        case MsgType::ListEntitiesNumber:    info.type = "number"; break;
        case MsgType::ListEntitiesLock:      info.type = "lock"; break;
        case MsgType::ListEntitiesButton:    info.type = "button"; break;
    }

    m_pendingEntities.append(info);
}

void EspHomeDevice::applyDiscoveredEntities(void)
{
    QMap<quint32, QList<EntityInfo>> grouped;
    for (const auto &info : m_pendingEntities)
        grouped[info.deviceId].append(info);

    QList<Device> touched;

    applyEntitiesToDevice(m_device, grouped.value(0));
    touched.append(m_device);

    for (auto it = grouped.begin(); it != grouped.end(); it++)
    {
        Device sub;

        if (it.key() == 0)
            continue;

        sub = subDevice(it.key());
        if (sub.isNull())
            continue;

        applyEntitiesToDevice(sub, it.value());
        touched.append(sub);
    }

    for (const auto &dev : touched)
        emit entitiesDiscovered(dev.data());
}

// Finds (creating and persisting if needed) the DeviceObject representing an ESPHome
// sub-device -- address is "<parent address>_<name>", not "<parent address>_<deviceId>":
// device_id is ESPHome's own auto-assigned wire integer (declaration-order-based, not a
// stable identifier -- reordering/adding/removing sub-devices in the YAML can reassign
// it), so keying on it would silently orphan a previously-discovered sub-device's whole
// objectId/specialSlots mapping the moment its id shifted. The declared name is the
// stable, user-controlled identity instead (same reasoning as specialSlots' name-match
// reservation for entities) -- a sub-device that stops being reported (removed from the
// ESPHome config, or the parent temporarily offline) is simply left untouched here, never
// deleted, so its mapping is intact if it's ever reconfigured back. port/key are copied
// from the parent purely so DeviceList::parse()'s non-empty checks pass (a sub-device
// never opens its own connection -- see EspHomeManager::connectAll()/sendCommand()).
Device EspHomeDevice::subDevice(quint32 deviceId)
{
    QString subName = m_subDeviceNames.value(deviceId, QString::number(deviceId));
    QString address = QString("%1_%2").arg(m_device->address(), subDeviceAddressSuffix(subName));
    Device sub = m_devices->byHost(address);

    if (sub.isNull())
    {
        sub = Device(new DeviceObject(subName, address, m_device->port(), m_device->encryptionKey()));
        sub->setParentAddress(m_device->address());
        m_devices->append(sub);
        logInfo << m_device << "discovered sub-device" << subName << "(id" << deviceId << ")";
    }

    sub->setName(subName);
    sub->setManufacturerName(m_device->manufacturerName());
    sub->setModelName(QString("Subdev %1 of %2").arg(subName, m_device->name()));
    sub->setParentAddress(m_device->address());
    sub->setAvailability(Availability::Online); // shares the parent's connection, so successfully discovering it here means it's reachable

    return sub;
}

void EspHomeDevice::applyEntitiesToDevice(const Device &device, const QList<EntityInfo> &entities)
{
    // Rebuild endpoints from discovered entities
    device->endpoints().clear();
    device->options().clear();

    // objectId is a single camelCase token, no underscores -- see camelCase()
    // above for why. Built from the entity's own name (e.g. "Back Side
    // Temperature" -> "backSideTemperature"), falling back to its device_class
    // or HOMEd type only when the name is empty. A numeric suffix (no
    // separator, so it doesn't reintroduce an underscore) is appended only on
    // an actual collision -- distinctly-named entities never get one, so
    // titles never get a stray digit appended by homed-web's title fallback.
    QMap<QString, int> objectIdCount;
    QStringList objectIds;

    for (const auto &info : entities)
    {
        QString base = camelCase(!info.name.isEmpty() ? info.name : (!info.deviceClass.isEmpty() ? info.deviceClass : info.type));
        int count = objectIdCount.value(base, 0) + 1;

        objectIdCount.insert(base, count);
        objectIds.append(count > 1 ? QString("%1%2").arg(base).arg(count) : base);
    }

    // Special entities get a stable number (persisted in DeviceObject::specialSlots(), reused by name match) instead of sharing "common" -- see Controller::publishExposes().
    auto isSpecialType = [](const EntityInfo &info)
    {
        return info.type == "light" || info.type == "cover" || info.type == "climate" || info.type == "lock"
            || (info.type == "switch" && !info.toggleCategory);
    };

    QMap<int, QString> &slotMap = device->specialSlots();
    QList<quint8> endpointIds;
    QSet<int> usedThisRound;

    for (const auto &info : entities)
    {
        if (!isSpecialType(info))
        {
            endpointIds.append(0); // resolved in the common pass below
            continue;
        }

        int number = 0;
        for (auto it = slotMap.begin(); it != slotMap.end(); it++)
        {
            if (it.value() == info.name && !usedThisRound.contains(it.key()))
            {
                number = it.key();
                break;
            }
        }

        if (number == 0)
        {
            number = 1;
            while (slotMap.contains(number) || usedThisRound.contains(number))
                number++;
        }

        slotMap.insert(number, info.name);
        usedThisRound.insert(number);
        endpointIds.append(static_cast<quint8>(number));
    }

    {
        int next = 1;
        for (int i = 0; i < endpointIds.count(); i++)
        {
            if (endpointIds.at(i) != 0)
                continue;

            while (usedThisRound.contains(next))
                next++;

            endpointIds[i] = static_cast<quint8>(next);
            usedThisRound.insert(next);
            next++;
        }
    }

    for (int i = 0; i < entities.count(); i++)
    {
        const EntityInfo &info = entities.at(i);
        const QString &objectId = objectIds.at(i);
        quint8 endpointId = endpointIds.at(i);
        bool special = isSpecialType(info);

        auto ep = QSharedPointer<EndpointObject>::create(endpointId, device);
        ep->meta().insert("key", info.key);
        ep->meta().insert("type", info.type);
        ep->meta().insert("objectId", objectId);
        ep->meta().insert("special", special);
        if (info.accuracyDecimals > 0)
            ep->meta().insert("round", info.accuracyDecimals);
        if (!info.icon.isEmpty())
            ep->meta().insert("icon", info.icon);

        QString title = info.name.isEmpty() ? objectId : info.name;
        ep->meta().insert("title", title); // separate from the device->options() title slots below, which only exist to feed HA discovery

        if (info.type == "switch")
        {
            QVariantMap opts = {{"title", title}};
            if (!info.icon.isEmpty()) opts.insert("icon", info.icon);

            ep->exposes().append(QSharedPointer<SwitchObject>::create());

            if (info.toggleCategory)
            {
                // config/diagnostic switches aren't a device's primary function -- route through "common" as a toggle, like select/number/button
                opts.insert("type", "toggle");
                device->options().insert(objectId, opts);
            }
            else
            {
                // SwitchObject's name is hardcoded "switch" -- title() resolves "switch_<endpointId>" then "switch", never objectId
                device->options().insert(QString("switch_%1").arg(endpointId), opts);
                device->options().insert(objectId, opts); // objectId-keyed copy: Controller::publishExposes reads this for the web UI
            }
        }
        else if (info.type == "lock")
        {
            QVariantMap opts = {{"title", title}};
            if (!info.icon.isEmpty()) opts.insert("icon", info.icon);

            ep->exposes().append(QSharedPointer<LockObject>::create());

            // LockObject's name is hardcoded "lock" -- title() resolves "lock_<endpointId>" then "lock", never objectId
            device->options().insert(QString("lock_%1").arg(endpointId), opts);
            device->options().insert(objectId, opts); // objectId-keyed copy: Controller::publishExposes reads this for the web UI
        }
        else if (info.type == "binary_sensor")
        {
            auto expose = QSharedPointer<BinaryObject>::create(objectId);
            QVariantMap opts = {{"type", "binary"}, {"title", title}};
            if (!info.deviceClass.isEmpty()) opts.insert("class", info.deviceClass);
            if (!info.icon.isEmpty()) opts.insert("icon", info.icon);
            device->options().insert(objectId, opts);
            ep->exposes().append(expose);
        }
        else if (info.type == "sensor" || info.type == "text_sensor")
        {
            auto expose = QSharedPointer<SensorObject>::create(objectId);
            QVariantMap opts = {{"type", "sensor"}, {"title", title}};
            if (!info.unit.isEmpty()) opts.insert("unit", info.unit);
            if (!info.deviceClass.isEmpty()) opts.insert("class", info.deviceClass);
            if (!info.stateClass.isEmpty()) opts.insert("state", info.stateClass);
            if (info.accuracyDecimals > 0) opts.insert("round", info.accuracyDecimals);
            if (!info.icon.isEmpty()) opts.insert("icon", info.icon);
            device->options().insert(objectId, opts);
            ep->exposes().append(expose);
        }
        else if (info.type == "light")
        {
            auto expose = QSharedPointer<LightObject>::create();

            // LightObject::request()/title() both resolve through
            // AbstractMetaObject::option(), which for any *unnamed* lookup
            // (m_name is hardcoded literal "light" for every light in
            // homed-common) always checks "light_<endpointId>" (this
            // endpoint's own numeric id) before falling back to plain
            // "light" -- same convention homed-service-matter uses for
            // light_<endpointId>/colorTemperature_<id>. That's the ONLY slot
            // request() reads (as a QStringList, via option().toStringList()),
            // so it's capabilities-only: there is no separate slot for a
            // custom title independent of that value (a title map landing
            // here instead would silently zero out capability detection, since
            // .toStringList() on a map returns empty) -- light entities keep
            // the generic auto-humanized "Light" HA discovery name, matching
            // homed-service-matter's own light entities. The objectId-scoped
            // copy is what our own Controller::publishExposes reads instead,
            // so the web UI still gets each light's own real capabilities.
            device->options().insert(QString("light_%1").arg(endpointId), info.lightOptions);
            device->options().insert("light", info.lightOptions);
            device->options().insert(objectId + "_light", info.lightOptions);
            if (info.minMireds > 0 || info.maxMireds > 0)
            {
                QVariantMap colorTemperature {{"min", static_cast<int>(info.minMireds)}, {"max", static_cast<int>(info.maxMireds)}};
                device->options().insert(QString("colorTemperature_%1").arg(endpointId), colorTemperature);
                device->options().insert("colorTemperature", colorTemperature);
                device->options().insert(objectId + "_colorTemperature", colorTemperature);
            }
            ep->exposes().append(expose);
        }
        else if (info.type == "cover")
        {
            auto expose = QSharedPointer<CoverObject>::create();

            // Same capabilities-only-slot situation as light above: CoverObject
            // reads its device_class via a bare option().toString() call, which
            // resolves "cover_<endpointId>" before "cover" -- storing a title
            // map there instead would make .toString() return empty (device
            // always falling back to "curtain"), so cover entities also keep
            // the generic auto-humanized "Cover" HA discovery name. ESPHome's
            // device_class covers many values (garage, shutter, blind, awning,
            // etc); homed-common's CoverObject only distinguishes "blind" from
            // everything else (defaulting the rest to "curtain"), so pass it
            // through as-is and let that existing logic sort it out.
            device->options().insert(QString("cover_%1").arg(endpointId), info.deviceClass);
            device->options().insert("cover", info.deviceClass);
            ep->meta().insert("supportsPosition", info.supportsPosition);
            ep->exposes().append(expose);
        }
        else if (info.type == "climate")
        {
            auto expose = QSharedPointer<ThermostatObject>::create();

            // Unlike light/cover, none of ThermostatObject::request()'s own
            // option() calls read the "thermostat" key itself (they're all
            // separately-named sub-fields -- systemMode/operationMode/fanMode/
            // targetTemperature/runningStatus), so it's free for a title with
            // no collision, and each sub-field gets its own automatic
            // "<name>_<endpointId>" priority slot from AbstractMetaObject::
            // option() the same way light's capabilities do.
            device->options().insert(QString("thermostat_%1").arg(endpointId), QVariantMap {{"title", title}});

            QVariantMap systemMode {{"enum", info.climateModes}};
            device->options().insert(QString("systemMode_%1").arg(endpointId), systemMode);
            device->options().insert("systemMode", systemMode);

            if (!info.climatePresets.isEmpty())
            {
                QVariantMap operationMode {{"enum", info.climatePresets}};
                device->options().insert(QString("operationMode_%1").arg(endpointId), operationMode);
                device->options().insert("operationMode", operationMode);
            }

            if (!info.climateFanModes.isEmpty())
            {
                QVariantMap fanMode {{"enum", info.climateFanModes}};
                device->options().insert(QString("fanMode_%1").arg(endpointId), fanMode);
                device->options().insert("fanMode", fanMode);
            }

            QVariantMap targetTemperature {{"min", static_cast<double>(info.minValue)}, {"max", static_cast<double>(info.maxValue)}, {"step", static_cast<double>(info.step > 0 ? info.step : 0.5f)}};
            device->options().insert(QString("targetTemperature_%1").arg(endpointId), targetTemperature);
            device->options().insert("targetTemperature", targetTemperature);

            device->options().insert(QString("runningStatus_%1").arg(endpointId), info.climateSupportsAction);
            device->options().insert("runningStatus", info.climateSupportsAction);

            ep->meta().insert("climateHasPreset", !info.climatePresets.isEmpty());
            ep->meta().insert("climateHasFanMode", !info.climateFanModes.isEmpty());

            ep->exposes().append(expose);
        }
        else if (info.type == "select")
        {
            auto expose = QSharedPointer<SelectObject>::create(objectId);
            QVariantMap opts = {{"type", "select"}, {"enum", info.selectOptions}, {"control", true}, {"title", title}};
            if (!info.icon.isEmpty()) opts.insert("icon", info.icon);
            device->options().insert(objectId, opts);
            ep->exposes().append(expose);
        }
        else if (info.type == "number")
        {
            auto expose = QSharedPointer<NumberObject>::create(objectId);
            QVariantMap opts;
            opts.insert("type", "number");
            opts.insert("min", static_cast<double>(info.minValue));
            opts.insert("max", static_cast<double>(info.maxValue));
            opts.insert("step", static_cast<double>(info.step > 0 ? info.step : 1.0f));
            opts.insert("control", true);
            opts.insert("title", title);
            if (!info.icon.isEmpty()) opts.insert("icon", info.icon);
            if (!info.unit.isEmpty()) opts.insert("unit", info.unit);
            device->options().insert(objectId, opts);
            ep->exposes().append(expose);
        }
        else if (info.type == "button")
        {
            auto expose = QSharedPointer<ButtonObject>::create(objectId);
            QVariantMap opts = {{"type", "button"}, {"control", true}, {"title", title}};
            if (!info.icon.isEmpty()) opts.insert("icon", info.icon);
            device->options().insert(objectId, opts);
            ep->exposes().append(expose);
        }

        // Required for expose->option()/title() to see this device's options
        // (title/unit/class/etc, inserted above) -- ExposeObject::option() walks
        // up via m_parent, which nothing sets automatically.
        for (auto &expose : ep->exposes())
            expose->setParent(ep.data());

        device->endpoints().insert(endpointId, ep);
    }

    logInfo << device << "discovered" << entities.count() << "entities";
}

void EspHomeDevice::processStateUpdate(quint16 type, const QByteArray &payload)
{
    auto fields = ProtoDecoder::decode(payload);
    quint32 key = 0;
    QMap<QString, QVariant> state;

    for (const auto &f : fields)
    {
        if (f.field() == 1 && f.wireType() == 5)
        {
            key = f.fixed32();
            continue;
        }

        switch (type)
        {
            case MsgType::StateSwitch:
                if (f.field() == 2 && f.wireType() == 0)
                    state.insert("status", f.boolean() ? "on" : "off");
                break;

            case MsgType::StateBinary:
                // placeholder: objectId needed from entity info
                if (f.field() == 2 && f.wireType() == 0)
                    state.insert("_state", f.boolean());
                break;

            case MsgType::StateSensor:
                if (f.field() == 2 && f.wireType() == 5)
                    state.insert("_state", static_cast<double>(f.floatVal()));
                break;

            case MsgType::StateTextSensor:
                if (f.field() == 2 && f.wireType() == 2)
                    state.insert("_state", f.string());
                break;

            case MsgType::StateLight:
                if (f.field() == 2 && f.wireType() == 0)
                    state.insert("status", f.boolean() ? "on" : "off");
                else if (f.field() == 3 && f.wireType() == 5)
                    state.insert("level", static_cast<int>(f.floatVal() * 255.0f));
                else if (f.field() == 4 && f.wireType() == 5)
                    state.insert("_r", static_cast<double>(f.floatVal()));
                else if (f.field() == 5 && f.wireType() == 5)
                    state.insert("_g", static_cast<double>(f.floatVal()));
                else if (f.field() == 6 && f.wireType() == 5)
                    state.insert("_b", static_cast<double>(f.floatVal()));
                else if (f.field() == 8 && f.wireType() == 5)
                    state.insert("colorTemperature", static_cast<int>(f.floatVal()));
                break;

            case MsgType::StateCover:
                if (f.field() == 2 && f.wireType() == 0)
                    state.insert("_legacyState", f.varint());
                else if (f.field() == 3 && f.wireType() == 5)
                    state.insert("_position", static_cast<double>(f.floatVal()));
                break;

            case MsgType::StateClimate:
                if (f.field() == 2 && f.wireType() == 0)
                    state.insert("systemMode", climateModeName(f.varint()));
                else if (f.field() == 3 && f.wireType() == 5)
                    state.insert("temperature", static_cast<double>(f.floatVal()));
                else if (f.field() == 4 && f.wireType() == 5)
                    state.insert("targetTemperature", static_cast<double>(f.floatVal()));
                else if (f.field() == 8 && f.wireType() == 0)
                    state.insert("running", climateActionRunning(f.varint()));
                else if (f.field() == 9 && f.wireType() == 0)
                    state.insert("fanMode", climateFanModeName(f.varint()));
                else if (f.field() == 11 && f.wireType() == 2 && !f.string().isEmpty())
                    state.insert("fanMode", f.string()); // custom_fan_mode overrides the enum above
                else if (f.field() == 12 && f.wireType() == 0)
                    state.insert("operationMode", climatePresetName(f.varint()));
                else if (f.field() == 13 && f.wireType() == 2 && !f.string().isEmpty())
                    state.insert("operationMode", f.string()); // custom_preset overrides
                break;

            case MsgType::StateLock:
                if (f.field() == 2 && f.wireType() == 0)
                    state.insert("status", (f.varint() == 1) ? "off" : "on"); // LOCK_STATE_LOCKED=1 -> "off", matching LockObject's state_locked/state_unlocked convention
                break;

            case MsgType::StateSelect:
                if (f.field() == 2 && f.wireType() == 2)
                    state.insert("_state", f.string());
                break;

            case MsgType::StateNumber:
                if (f.field() == 2 && f.wireType() == 5)
                    state.insert("_state", static_cast<double>(f.floatVal()));
                break;
        }
    }

    if (key == 0)
        return;

    // Proto3 omits default (false/zero-enum) values — supply the explicit
    // zero-value default (matches each field's own enum ordering above)
    if (type == MsgType::StateSwitch && !state.contains("status"))
        state.insert("status", "off");
    else if (type == MsgType::StateBinary && !state.contains("_state"))
        state.insert("_state", false);
    else if (type == MsgType::StateLight && !state.contains("status"))
        state.insert("status", "off");
    else if (type == MsgType::StateLock && !state.contains("status"))
        state.insert("status", "off");
    else if (type == MsgType::StateClimate)
    {
        if (!state.contains("systemMode"))
            state.insert("systemMode", "off");
        if (!state.contains("running"))
            state.insert("running", false);
    }

    // Find endpoint by key -- may belong to a sub-device, not necessarily m_device
    Device device;
    quint8 endpointId;
    if (!findEndpointByKey(key, device, endpointId))
        return;

    auto ep = device->endpoints().value(endpointId).staticCast<EndpointObject>();
    if (!ep)
        return;

    QString objectId = ep->meta().value("objectId").toString();
    QString entityType = ep->meta().value("type").toString();

    // Honor the sensor's accuracy_decimals (published as the "round" option in
    // applyDiscoveredEntities) on the value itself, not just as a display hint --
    // ESPHome sends full float precision on every state update regardless.
    if (type == MsgType::StateSensor && state.contains("_state") && ep->meta().contains("round"))
    {
        int decimals = ep->meta().value("round").toInt();
        double factor = std::pow(10, decimals);
        state.insert("_state", std::round(state.value("_state").toDouble() * factor) / factor);
    }

    // Handle RGB light: combine r,g,b into color array
    if (type == MsgType::StateLight && state.contains("_r"))
    {
        QVariantList color;
        color.append(static_cast<int>(state.value("_r").toDouble() * 255.0));
        color.append(static_cast<int>(state.value("_g").toDouble() * 255.0));
        color.append(static_cast<int>(state.value("_b").toDouble() * 255.0));
        state.remove("_r");
        state.remove("_g");
        state.remove("_b");
        state.insert("color", color);
    }

    // Cover: derive open/closed + a 0-100 position from whichever field the
    // device actually reports. supports_position devices send a real 0.0-1.0
    // position; others only send the deprecated legacy_state (0=open,
    // 1=closed, proto3-omitted -- i.e. defaults to open -- like every other
    // zero-value default above), so synthesize a position from that instead,
    // both so the web UI's position slider still has something sane to show
    // and so open/close via the slider's extremes still works either way.
    if (type == MsgType::StateCover)
    {
        if (ep->meta().value("supportsPosition").toBool() && state.contains("_position"))
        {
            double position = state.value("_position").toDouble();
            state.insert("position", static_cast<int>(std::round(position * 100.0)));
            state.insert("cover", position > 0.0 ? "open" : "closed");
        }
        else
        {
            bool closed = state.value("_legacyState", 0).toUInt() != 0;
            state.insert("cover", closed ? "closed" : "open");
            state.insert("position", closed ? 0 : 100);
        }

        state.remove("_position");
        state.remove("_legacyState");
    }

    // Climate: proto3 omits an enum field entirely when its value is that
    // enum's zero value (CLIMATE_PRESET_NONE="none", CLIMATE_FAN_ON="on") --
    // same zero-value omission every other default above works around, but
    // gated on whether the entity actually supports presets/fan modes at all
    // (a device without either shouldn't gain a phantom "none"/"on" reading).
    // Without this, an ESPHome device whose current preset genuinely *is*
    // "none" never sends operationMode at all, so the web UI never receives
    // a state update for it and shows no current value.
    if (type == MsgType::StateClimate)
    {
        if (ep->meta().value("climateHasPreset").toBool() && !state.contains("operationMode"))
            state.insert("operationMode", climatePresetName(0));
        if (ep->meta().value("climateHasFanMode").toBool() && !state.contains("fanMode"))
            state.insert("fanMode", climateFanModeName(0));
    }

    // For entities that use objectId as key in state JSON
    if (state.contains("_state"))
    {
        state.insert(objectId, state.value("_state"));
        state.remove("_state");
    }

    // Update endpoint state
    for (auto it = state.begin(); it != state.end(); it++)
        ep->setState(it.key(), it.value());

    device->updateLastSeen(); // credited to whichever (sub-)device the state actually came from
    emit stateChanged(device.data(), endpointId);
}

// This connection's currently-known sub-devices, shared by lastSeen/availability propagation and findEndpointByKey().
QList<Device> EspHomeDevice::knownSubDevices(void) const
{
    QList<Device> list;

    for (auto it = m_subDeviceNames.begin(); it != m_subDeviceNames.end(); it++)
    {
        Device sub = m_devices->byHost(QString("%1_%2").arg(m_device->address(), subDeviceAddressSuffix(it.value())));
        if (!sub.isNull())
            list.append(sub);
    }

    return list;
}

bool EspHomeDevice::findEndpointByKey(quint32 key, Device &outDevice, quint8 &outEndpointId) const
{
    for (auto it = m_device->endpoints().begin(); it != m_device->endpoints().end(); it++)
    {
        if (it.value()->meta().value("key").toUInt() != key)
            continue;
        outDevice = m_device;
        outEndpointId = it.key();
        return true;
    }

    for (const auto &sub : knownSubDevices())
    {
        for (auto it = sub->endpoints().begin(); it != sub->endpoints().end(); it++)
        {
            if (it.value()->meta().value("key").toUInt() != key)
                continue;
            outDevice = sub;
            outEndpointId = it.key();
            return true;
        }
    }

    return false;
}

void EspHomeDevice::sendCommand(DeviceObject *device, quint8 endpointId, const QString &action, const QVariant &value)
{
    logInfo << device << "sendCommand ep" << endpointId << action << value.toString() << "state" << static_cast<int>(m_state);

    if (m_state != State::Subscribe)
    {
        logWarning << device << "sendCommand: not in Subscribe state, dropping";
        return;
    }

    auto ep = device->endpoints().value(endpointId).staticCast<EndpointObject>();
    if (!ep)
    {
        logWarning << device << "sendCommand: endpoint" << endpointId << "not found";
        return;
    }

    quint32 key = ep->meta().value("key").toUInt();
    QString type = ep->meta().value("type").toString();
    QString objectId = ep->meta().value("objectId").toString();
    logInfo << device << "sendCommand: type" << type << "key" << key << "objectId" << objectId;

    if (type == "switch")
    {
        ProtoEncoder cmd;
        cmd.addFixed32(1, key);
        QString valueStr = value.toString().toLower();
        bool state;
        if (valueStr == "toggle")
        {
            state = (ep->stateMap().value("status").toString() != "on");
        }
        else
        {
            state = (valueStr == "on" || valueStr == "1" || valueStr == "true");
        }
        cmd.addBool(2, state);
        logInfo << device << "sendCommand switch: key" << key << "state" << state;
        sendMessage(MsgType::SwitchCommand, cmd.data());
    }
    else if (type == "lock")
    {
        ProtoEncoder cmd;
        cmd.addFixed32(1, key);
        QString valueStr = value.toString().toLower();
        bool locked = valueStr == "toggle" ? (ep->stateMap().value("status").toString() != "off") : (valueStr == "off" || valueStr == "0" || valueStr == "false");
        cmd.addVarint(2, locked ? 1 : 0); // LOCK_LOCK=1, LOCK_UNLOCK=0
        sendMessage(MsgType::LockCommand, cmd.data());
    }
    else if (type == "light")
    {
        ProtoEncoder cmd;
        cmd.addFixed32(1, key);

        if (action == "status")
        {
            QString v = value.toString().toLower();
            bool state = v == "toggle" ? (ep->stateMap().value("status").toString() != "on") : (v == "on" || v == "1" || v == "true");
            cmd.addBool(2, true); // has_state
            cmd.addBool(3, state);
        }
        else if (action == "level")
        {
            cmd.addBool(2, true); // has_state
            cmd.addBool(3, true); // state = on
            cmd.addBool(4, true); // has_brightness
            cmd.addFloat(5, value.toFloat() / 255.0f);
        }
        else if (action == "color")
        {
            QVariantList list = value.toList();
            if (list.size() >= 3)
            {
                cmd.addBool(2, true); // has_state
                cmd.addBool(3, true);
                cmd.addBool(6, true); // has_rgb
                cmd.addFloat(7, list.at(0).toFloat() / 255.0f); // red
                cmd.addFloat(8, list.at(1).toFloat() / 255.0f); // green
                cmd.addFloat(9, list.at(2).toFloat() / 255.0f); // blue
            }
        }
        else if (action == "colorTemperature")
        {
            cmd.addBool(2, true); // has_state
            cmd.addBool(3, true);
            cmd.addBool(12, true); // has_color_temperature
            cmd.addFloat(13, value.toFloat());
        }

        sendMessage(MsgType::LightCommand, cmd.data());
    }
    else if (type == "cover")
    {
        ProtoEncoder cmd;
        cmd.addFixed32(1, key);

        if (action == "cover")
        {
            QString v = value.toString().toLower();
            if (v == "stop")
            {
                cmd.addBool(8, true); // stop
            }
            else
            {
                // ESPHome's own frontends translate open/close to position 1.0/0.0
                // even for covers without supports_position -- the server maps it
                // back to a plain open()/close() call internally either way.
                cmd.addBool(4, true); // has_position
                cmd.addFloat(5, v == "open" ? 1.0f : 0.0f);
            }
        }
        else if (action == "position")
        {
            cmd.addBool(4, true); // has_position
            cmd.addFloat(5, value.toFloat() / 100.0f);
        }

        sendMessage(MsgType::CoverCommand, cmd.data());
    }
    else if (type == "climate")
    {
        ProtoEncoder cmd;
        cmd.addFixed32(1, key);

        if (action == "systemMode")
        {
            int mode = climateModeValue(value.toString());
            if (mode >= 0)
            {
                cmd.addBool(2, true); // has_mode
                cmd.addVarint(3, static_cast<quint64>(mode));
            }
        }
        else if (action == "targetTemperature")
        {
            cmd.addBool(4, true); // has_target_temperature
            cmd.addFloat(5, value.toFloat());
        }
        else if (action == "fanMode")
        {
            int mode = climateFanModeValue(value.toString());
            if (mode >= 0)
            {
                cmd.addBool(12, true); // has_fan_mode
                cmd.addVarint(13, static_cast<quint64>(mode));
            }
            else
            {
                cmd.addBool(16, true); // has_custom_fan_mode
                cmd.addString(17, value.toString());
            }
        }
        else if (action == "operationMode")
        {
            int preset = climatePresetValue(value.toString());
            if (preset >= 0)
            {
                cmd.addBool(18, true); // has_preset
                cmd.addVarint(19, static_cast<quint64>(preset));
            }
            else
            {
                cmd.addBool(20, true); // has_custom_preset
                cmd.addString(21, value.toString());
            }
        }

        sendMessage(MsgType::ClimateCommand, cmd.data());
    }
    else if (type == "select")
    {
        ProtoEncoder cmd;
        cmd.addFixed32(1, key);
        cmd.addString(2, value.toString());
        sendMessage(MsgType::SelectCommand, cmd.data());
    }
    else if (type == "number")
    {
        ProtoEncoder cmd;
        cmd.addFixed32(1, key);
        cmd.addFloat(2, value.toFloat());
        sendMessage(MsgType::NumberCommand, cmd.data());
    }
    else if (type == "button")
    {
        ProtoEncoder cmd;
        cmd.addFixed32(1, key);
        sendMessage(MsgType::ButtonCommand, cmd.data());
    }
}

// ==================== EspHomeManager ====================

EspHomeManager::EspHomeManager(DeviceList *devices, QObject *parent) : QObject(parent), m_devices(devices)
{
}

void EspHomeManager::connectAll(void)
{
    for (int i = 0; i < m_devices->count(); i++)
    {
        // sub-devices don't get their own connection -- they ride along on their parent's (see EspHomeDevice::subDevice())
        if (!m_devices->at(i)->parentAddress().isEmpty())
            continue;
        connectDevice(m_devices->at(i).data());
    }
}

void EspHomeManager::connectDevice(DeviceObject *device)
{
    if (!device->parentAddress().isEmpty() || m_connections.contains(device->host()))
        return;

    auto conn = new EspHomeDevice(m_devices->byHost(device->host()), m_devices, this);
    connect(conn, &EspHomeDevice::entitiesDiscovered, this, &EspHomeManager::entitiesDiscovered);
    connect(conn, &EspHomeDevice::stateChanged, this, &EspHomeManager::stateChanged);
    connect(conn, &EspHomeDevice::availabilityChanged, this, &EspHomeManager::availabilityChanged);
    connect(conn, &EspHomeDevice::lastSeenUpdated, this, &EspHomeManager::lastSeenUpdated);
    m_connections.insert(device->host(), conn);
    conn->connectToDevice();
}

void EspHomeManager::disconnectDevice(DeviceObject *device)
{
    // a sub-device's host never has its own entry in m_connections, so this is already a no-op for it -- only its parent connection can be disconnected
    auto conn = m_connections.value(device->host());
    if (!conn)
        return;

    conn->disconnectFromDevice();
    m_connections.remove(device->host());
    delete conn;
}

void EspHomeManager::sendCommand(DeviceObject *device, quint8 endpointId, const QString &action, const QVariant &value)
{
    QString connHost = device->parentAddress().isEmpty() ? device->host() : device->parentAddress();
    auto conn = m_connections.value(connHost);
    if (conn)
        conn->sendCommand(device, endpointId, action, value);
}
