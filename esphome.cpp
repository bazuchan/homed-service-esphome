#include "esphome.h"
#include "logger.h"
#include <QDateTime>
#include <cmath>

static const int RECONNECT_INTERVAL = 10000;  // 10 seconds
static const int PING_INTERVAL      = 60000;  // 60 seconds

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

// ==================== EspHomeDevice ====================

EspHomeDevice::EspHomeDevice(const Device &device, QObject *parent)
    : QObject(parent), m_device(device), m_state(State::Disconnected), m_noise(nullptr), m_noiseHandshakeState(0)
{
    m_socket = new QTcpSocket(this);
    m_pingTimer = new QTimer(this);
    m_reconnectTimer = new QTimer(this);

    m_pingTimer->setInterval(PING_INTERVAL);
    m_pingTimer->setSingleShot(false);
    m_reconnectTimer->setInterval(RECONNECT_INTERVAL);
    m_reconnectTimer->setSingleShot(true);

    connect(m_socket, &QTcpSocket::connected, this, &EspHomeDevice::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &EspHomeDevice::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &EspHomeDevice::onReadyRead);
    connect(m_socket, &QAbstractSocket::errorOccurred, this, &EspHomeDevice::onError);
    connect(m_pingTimer, &QTimer::timeout, this, &EspHomeDevice::onPingTimer);
    connect(m_reconnectTimer, &QTimer::timeout, this, &EspHomeDevice::onReconnectTimer);
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
    m_state = State::Disconnected;
    m_socket->disconnectFromHost();
}

void EspHomeDevice::onConnected(void)
{
    logInfo << m_device << "TCP connected";
    m_state = State::ClientHello;

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
            for (const auto &f : fields)
            {
                if (f.field() == 4 && f.wireType() == 2)
                    m_device->setEsphomeVersion(f.string());
                else if (f.field() == 6 && f.wireType() == 2)
                    m_device->setModelName(f.string());
                else if (f.field() == 12 && f.wireType() == 2)
                    m_device->setManufacturerName(f.string());
            }

            // Request entity list
            m_pendingEntities.clear();
            m_state = State::ListEntities;
            sendMessage(MsgType::ListEntitiesRequest);
            break;
        }

        case MsgType::ListEntitiesBinary:
        case MsgType::ListEntitiesLight:
        case MsgType::ListEntitiesSensor:
        case MsgType::ListEntitiesSwitch:
        case MsgType::ListEntitiesTextSensor:
        case MsgType::ListEntitiesNumber:
        case MsgType::ListEntitiesSelect:
        case MsgType::ListEntitiesButton:
            processEntityInfo(type, payload);
            break;

        case MsgType::ListEntitiesDone:
            applyDiscoveredEntities();
            m_state = State::Subscribe;
            sendMessage(MsgType::SubscribeStates);
            emit entitiesDiscovered(m_device.data());
            break;

        case MsgType::StateBinary:
        case MsgType::StateLight:
        case MsgType::StateSensor:
        case MsgType::StateSwitch:
        case MsgType::StateTextSensor:
        case MsgType::StateNumber:
        case MsgType::StateSelect:
            processStateUpdate(type, payload);
            break;

        case MsgType::PingResponse:
            m_device->updateLastSeen();
            emit lastSeenUpdated(m_device.data());
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
                else if (f.field() == 9 && f.wireType() == 2) info.deviceClass = f.string();
                break;

            case MsgType::ListEntitiesBinary:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 5 && f.wireType() == 2) info.deviceClass = f.string();
                break;

            case MsgType::ListEntitiesSensor:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 5 && f.wireType() == 2) info.deviceClass = f.string();
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
                break;

            case MsgType::ListEntitiesTextSensor:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 8 && f.wireType() == 2) info.deviceClass = f.string();
                break;

            case MsgType::ListEntitiesLight:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 9 && f.wireType() == 5) info.minMireds = f.floatVal();
                else if (f.field() == 10 && f.wireType() == 5) info.maxMireds = f.floatVal();
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

            case MsgType::ListEntitiesSelect:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 6 && f.wireType() == 2) info.selectOptions.append(f.string());
                break;

            case MsgType::ListEntitiesNumber:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 6 && f.wireType() == 5) info.minValue = f.floatVal();
                else if (f.field() == 7 && f.wireType() == 5) info.maxValue = f.floatVal();
                else if (f.field() == 8 && f.wireType() == 5) info.step = f.floatVal();
                else if (f.field() == 11 && f.wireType() == 2) info.unit = f.string();
                break;

            case MsgType::ListEntitiesButton:
                if (f.field() == 2 && f.wireType() == 5) info.key = f.fixed32();
                else if (f.field() == 8 && f.wireType() == 2) info.deviceClass = f.string();
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
        case MsgType::ListEntitiesSensor:    info.type = "sensor"; break;
        case MsgType::ListEntitiesTextSensor:info.type = "text_sensor"; break;
        case MsgType::ListEntitiesLight:     info.type = "light"; break;
        case MsgType::ListEntitiesSelect:    info.type = "select"; break;
        case MsgType::ListEntitiesNumber:    info.type = "number"; break;
        case MsgType::ListEntitiesButton:    info.type = "button"; break;
    }

    m_pendingEntities.append(info);
}

void EspHomeDevice::applyDiscoveredEntities(void)
{
    // Rebuild endpoints from discovered entities
    m_device->endpoints().clear();
    m_device->options().clear();

    // objectId is a single camelCase token, no underscores -- see camelCase()
    // above for why. Built from the entity's own name (e.g. "Back Side
    // Temperature" -> "backSideTemperature"), falling back to its device_class
    // or HOMEd type only when the name is empty. A numeric suffix (no
    // separator, so it doesn't reintroduce an underscore) is appended only on
    // an actual collision -- distinctly-named entities never get one, so
    // titles never get a stray digit appended by homed-web's title fallback.
    QMap<QString, int> objectIdCount;
    QStringList objectIds;

    for (const auto &info : m_pendingEntities)
    {
        QString base = camelCase(!info.name.isEmpty() ? info.name : (!info.deviceClass.isEmpty() ? info.deviceClass : info.type));
        int count = objectIdCount.value(base, 0) + 1;

        objectIdCount.insert(base, count);
        objectIds.append(count > 1 ? QString("%1%2").arg(base).arg(count) : base);
    }

    for (int i = 0; i < m_pendingEntities.count(); i++)
    {
        const EntityInfo &info = m_pendingEntities.at(i);
        const QString &objectId = objectIds.at(i);
        quint8 endpointId = static_cast<quint8>(i + 1);

        auto ep = QSharedPointer<EndpointObject>::create(endpointId, m_device);
        ep->meta().insert("key", info.key);
        ep->meta().insert("type", info.type);
        ep->meta().insert("objectId", objectId);
        if (info.accuracyDecimals > 0)
            ep->meta().insert("round", info.accuracyDecimals);

        QString title = info.name.isEmpty() ? objectId : info.name;

        if (info.type == "switch")
        {
            ep->exposes().append(QSharedPointer<SwitchObject>::create());
            m_device->options().insert(objectId, QVariantMap {{"title", title}});
        }
        else if (info.type == "binary_sensor")
        {
            auto expose = QSharedPointer<BinaryObject>::create(objectId);
            QVariantMap opts = {{"title", title}};
            if (!info.deviceClass.isEmpty()) opts.insert("class", info.deviceClass);
            m_device->options().insert(objectId, opts);
            ep->exposes().append(expose);
        }
        else if (info.type == "sensor" || info.type == "text_sensor")
        {
            auto expose = QSharedPointer<SensorObject>::create(objectId);
            QVariantMap opts = {{"title", title}};
            if (!info.unit.isEmpty()) opts.insert("unit", info.unit);
            if (!info.deviceClass.isEmpty()) opts.insert("class", info.deviceClass);
            if (!info.stateClass.isEmpty()) opts.insert("state", info.stateClass);
            if (info.accuracyDecimals > 0) opts.insert("round", info.accuracyDecimals);
            m_device->options().insert(objectId, opts);
            ep->exposes().append(expose);
        }
        else if (info.type == "light")
        {
            auto expose = QSharedPointer<LightObject>::create();
            m_device->options().insert(objectId, QVariantMap {{"title", title}});

            // LightObject's expose name is hardcoded "light" (homed-common), so
            // unlike every other type this can't key off objectId the same way.
            // Two writes: the plain "light"/"colorTemperature" keys are what
            // LightObject::request() itself reads via option() for HA discovery
            // (m_name is always literal "light" there, so with >1 light on a
            // device this pre-existing homed-common limitation still means HA
            // only sees the last-discovered light's capabilities -- unchanged
            // from before). The objectId-scoped copy is what our own
            // Controller::publishExposes reads, so the web UI gets each light's
            // real capabilities instead of the last light's clobbering the rest.
            m_device->options().insert("light", info.lightOptions);
            m_device->options().insert(objectId + "_light", info.lightOptions);
            if (info.minMireds > 0 || info.maxMireds > 0)
            {
                QVariantMap colorTemperature {{"min", static_cast<int>(info.minMireds)}, {"max", static_cast<int>(info.maxMireds)}};
                m_device->options().insert("colorTemperature", colorTemperature);
                m_device->options().insert(objectId + "_colorTemperature", colorTemperature);
            }
            ep->exposes().append(expose);
        }
        else if (info.type == "select")
        {
            auto expose = QSharedPointer<SelectObject>::create(objectId);
            m_device->options().insert(objectId, QVariantMap {{"enum", info.selectOptions}, {"control", true}, {"title", title}});
            ep->exposes().append(expose);
        }
        else if (info.type == "number")
        {
            auto expose = QSharedPointer<NumberObject>::create(objectId);
            QVariantMap opts;
            opts.insert("min", static_cast<double>(info.minValue));
            opts.insert("max", static_cast<double>(info.maxValue));
            opts.insert("step", static_cast<double>(info.step > 0 ? info.step : 1.0f));
            opts.insert("control", true);
            opts.insert("title", title);
            if (!info.unit.isEmpty()) opts.insert("unit", info.unit);
            m_device->options().insert(objectId, opts);
            ep->exposes().append(expose);
        }
        else if (info.type == "button")
        {
            auto expose = QSharedPointer<ButtonObject>::create(objectId);
            m_device->options().insert(objectId, QVariantMap {{"type", "button"}, {"control", true}, {"title", title}});
            ep->exposes().append(expose);
        }

        // Required for expose->option()/title() to see this device's options
        // (title/unit/class/etc, inserted above) -- ExposeObject::option() walks
        // up via m_parent, which nothing sets automatically.
        for (auto &expose : ep->exposes())
            expose->setParent(ep.data());

        m_device->endpoints().insert(endpointId, ep);
    }

    logInfo << m_device << "discovered" << m_pendingEntities.count() << "entities";
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

    // Proto3 omits default (false) bool values — supply the "off" default explicitly
    if (type == MsgType::StateSwitch && !state.contains("status"))
        state.insert("status", "off");
    else if (type == MsgType::StateBinary && !state.contains("_state"))
        state.insert("_state", false);
    else if (type == MsgType::StateLight && !state.contains("status"))
        state.insert("status", "off");

    // Find endpoint by key
    quint8 endpointId = endpointByKey(key);
    if (endpointId == 0)
        return;

    auto ep = m_device->endpoints().value(endpointId).staticCast<EndpointObject>();
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

    // For entities that use objectId as key in state JSON
    if (state.contains("_state"))
    {
        state.insert(objectId, state.value("_state"));
        state.remove("_state");
    }

    // Update endpoint state
    for (auto it = state.begin(); it != state.end(); it++)
        ep->setState(it.key(), it.value());

    m_device->updateLastSeen();
    emit stateChanged(m_device.data(), endpointId);
}

quint8 EspHomeDevice::endpointByKey(quint32 key) const
{
    for (auto it = m_device->endpoints().begin(); it != m_device->endpoints().end(); it++)
        if (it.value()->meta().value("key").toUInt() == key)
            return it.key();
    return 0;
}

void EspHomeDevice::sendCommand(quint8 endpointId, const QString &action, const QVariant &value)
{
    logInfo << m_device << "sendCommand ep" << endpointId << action << value.toString() << "state" << static_cast<int>(m_state);

    if (m_state != State::Subscribe)
    {
        logWarning << m_device << "sendCommand: not in Subscribe state, dropping";
        return;
    }

    auto ep = m_device->endpoints().value(endpointId).staticCast<EndpointObject>();
    if (!ep)
    {
        logWarning << m_device << "sendCommand: endpoint" << endpointId << "not found";
        return;
    }

    quint32 key = ep->meta().value("key").toUInt();
    QString type = ep->meta().value("type").toString();
    QString objectId = ep->meta().value("objectId").toString();
    logInfo << m_device << "sendCommand: type" << type << "key" << key << "objectId" << objectId;

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
        logInfo << m_device << "sendCommand switch: key" << key << "state" << state;
        sendMessage(MsgType::SwitchCommand, cmd.data());
    }
    else if (type == "light")
    {
        ProtoEncoder cmd;
        cmd.addFixed32(1, key);

        if (action == "status")
        {
            QString v = value.toString().toLower();
            cmd.addBool(2, true); // has_state
            cmd.addBool(3, v == "on" || v == "1" || v == "true");
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
        connectDevice(m_devices->at(i).data());
}

void EspHomeManager::connectDevice(DeviceObject *device)
{
    if (m_connections.contains(device->host()))
        return;

    auto conn = new EspHomeDevice(m_devices->byHost(device->host()), this);
    connect(conn, &EspHomeDevice::entitiesDiscovered, this, &EspHomeManager::entitiesDiscovered);
    connect(conn, &EspHomeDevice::stateChanged, this, &EspHomeManager::stateChanged);
    connect(conn, &EspHomeDevice::availabilityChanged, this, &EspHomeManager::availabilityChanged);
    connect(conn, &EspHomeDevice::lastSeenUpdated, this, &EspHomeManager::lastSeenUpdated);
    m_connections.insert(device->host(), conn);
    conn->connectToDevice();
}

void EspHomeManager::disconnectDevice(DeviceObject *device)
{
    auto conn = m_connections.value(device->host());
    if (!conn)
        return;

    conn->disconnectFromDevice();
    m_connections.remove(device->host());
    delete conn;
}

void EspHomeManager::sendCommand(DeviceObject *device, quint8 endpointId, const QString &action, const QVariant &value)
{
    auto conn = m_connections.value(device->host());
    if (conn)
        conn->sendCommand(endpointId, action, value);
}
