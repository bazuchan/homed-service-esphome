#include "controller.h"
#include "logger.h"

Controller::Controller(const QString &configFile)
    : HOMEd(SERVICE_VERSION, configFile, true),
      m_propertiesTimer(new QTimer(this)),
      m_devices(new DeviceList(getConfig(), this)),
      m_esphome(new EspHomeManager(m_devices, this)),
      m_commands(QMetaEnum::fromType<Command>())
{
    m_haPrefix  = getConfig()->value("homeassistant/prefix", "homeassistant").toString();
    m_haStatus  = getConfig()->value("homeassistant/status", "homeassistant/status").toString();
    m_haEnabled = getConfig()->value("homeassistant/enabled", false).toBool();
    m_haUpdate  = getConfig()->value("homeassistant/update", false).toBool();

    m_propertiesTimer->setSingleShot(true);
    connect(m_propertiesTimer, &QTimer::timeout, this, &Controller::updateProperties);

    connect(m_esphome, &EspHomeManager::entitiesDiscovered, this, &Controller::entitiesDiscovered);
    connect(m_esphome, &EspHomeManager::stateChanged, this, &Controller::stateChanged);
    connect(m_esphome, &EspHomeManager::availabilityChanged, this, &Controller::availabilityChanged);
    connect(m_esphome, &EspHomeManager::lastSeenUpdated, this, &Controller::publishDeviceStatus);

    m_devices->init();
    m_esphome->connectAll();
}

void Controller::quit(void)
{
    delete m_esphome;
    m_devices->store(true);
    HOMEd::quit();
}

QString Controller::deviceTopic(DeviceObject *device)
{
    return m_devices->names() ? device->name() : device->address();
}

// Home Assistant's MQTT discovery topic (<prefix>/<component>/<node_id>/<object_id>/config)
// only allows [a-zA-Z0-9_-] in node_id/object_id -- stricter than general MQTT
// topics, which tolerate dots fine (see https://www.home-assistant.io/integrations/mqtt/#discovery-topic).
// device->address() is a raw IP ("172.21.7.121") when used as our own fd/td
// topic segment, but that same string also becomes part of uniqueId, which is
// used directly as the discovery topic's node_id -- sanitize only that copy.
static QString haSafeId(const QString &value)
{
    QString result = value;

    for (QChar &c : result)
        if (!c.isLetterOrNumber() && c != '_' && c != '-')
            c = '_';

    return result;
}

void Controller::publishExposes(DeviceObject *device, bool remove)
{
    QString devTopic = deviceTopic(device);

    // Built ourselves rather than via DeviceObject::publishExposes() (homed-common):
    // that call also unconditionally republishes its own numeric-endpoint-keyed
    // copy of the expose/<service>/<device> topic (AbstractDeviceObject::addExposeData),
    // which collides with the objectId-keyed one published below -- both land on
    // the same retained topic, so a client already subscribed during a live
    // (re)publish sees both messages and never prunes the stale numeric-keyed one.
    // Also lets state_topic/command_topic use our actual objectId-based fd//td
    // topics directly instead of homed-common's numeric-endpoint convention.
    if (m_haEnabled)
        publishHaDiscovery(device, devTopic, remove);

    // Publish expose JSON with objectId as endpoint keys for stable topic routing
    if (!remove)
    {
        QMap<QString, QVariant> data;

        for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
        {
            auto ep = it.value().staticCast<EndpointObject>();
            if (!ep || ep->exposes().isEmpty())
                continue;

            QString objectId = ep->meta().value("objectId").toString();
            QString entityType = ep->meta().value("type").toString();
            if (objectId.isEmpty())
                continue;

            QList<QString> items;
            for (const auto &expose : ep->exposes())
                items.append(expose->name());

            QMap<QString, QVariant> epData;
            epData.insert("items", QVariant(items));

            QMap<QString, QVariant> options;

            if (entityType == "light")
            {
                // "light" (LightObject's expose name is hardcoded in
                // homed-common) must always stay a plain capabilities array --
                // homed-web's exposeList() does .concat() on it, so anything
                // else (e.g. a title object) landing here breaks rendering.
                // Read from the objectId-scoped keys esphome.cpp stores these
                // under (a second light on the device would otherwise collide
                // on a shared "light"/"colorTemperature" key) -- but the output
                // key stays plain "light"/"colorTemperature", since that's what
                // homed-web's expose.js expects for this endpoint's own options.
                // Title is intentionally not published here at all -- light's
                // "light"/"light_<endpointId>" key is capabilities-only (see
                // esphome.cpp), so light entities always keep the generic
                // auto-humanized "Light" HA-discovery name.
                QVariant lightOpt = device->options().value(objectId + "_light");
                options.insert("light", lightOpt.isValid() ? lightOpt : QVariant(QVariantList()));

                QVariant ctOpt = device->options().value(objectId + "_colorTemperature");
                if (ctOpt.isValid()) options.insert("colorTemperature", ctOpt);
            }
            else if (entityType == "climate")
            {
                // homed-web's exposeList() reads these as flat sibling keys in
                // this endpoint's own options (not nested under "thermostat"),
                // conditionally including each in the rendered card only when
                // present -- so devices without e.g. a preset/fan mode just
                // omit that key rather than publishing an empty one. Same
                // "<name>_<endpointId>" keys esphome.cpp wrote for
                // ThermostatObject's own option() calls (HA discovery), reused
                // here for the web UI's copy of the same data.
                static const QList<QString> climateKeys = {"systemMode", "operationMode", "fanMode", "targetTemperature", "runningStatus"};
                for (const auto &key : climateKeys)
                {
                    QVariant opt = device->options().value(QString("%1_%2").arg(key).arg(it.key()));
                    if (opt.isValid())
                        options.insert(key, opt);
                }
            }
            else
            {
                // icon (like title/unit/class) rides along inside this same
                // per-objectId map from esphome.cpp -- homed-web's addExpose()
                // resolves options[property]/options[name] per row, never a
                // top-level "icon" sibling, so it has to sit next to title,
                // not beside it.
                QVariant objOpt = device->options().value(objectId);
                if (objOpt.isValid())
                    options.insert(items.first(), objOpt);
            }

            if (!options.isEmpty())
                epData.insert("options", options);

            data.insert(objectId, epData);
        }

        mqttPublish(mqttTopic("expose/%1/%2").arg(serviceTopic(), devTopic),
            QJsonObject::fromVariantMap(data), true);
        m_propertiesTimer->start(UPDATE_PROPERTIES_DELAY);
    }
    else
    {
        mqttPublish(mqttTopic("expose/%1/%2").arg(serviceTopic(), devTopic),
            QJsonObject(), true);
    }
}

void Controller::publishHaDiscovery(DeviceObject *device, const QString &devTopic, bool remove)
{
    QString nodeId = QString("%1_%2").arg(uniqueId(), haSafeId(device->address()));
    QJsonObject identity;
    QJsonArray availability;

    if (!remove)
    {
        identity.insert("identifiers", QJsonArray {nodeId});
        identity.insert("name", device->name());
        identity.insert("via_device", uniqueId());

        availability.append(QJsonObject {{"topic", mqttTopic("device/%1/%2").arg(serviceTopic(), devTopic)}, {"value_template", "{{ value_json.status }}"}});
        availability.append(QJsonObject {{"topic", mqttTopic("service/%1").arg(serviceTopic())}, {"value_template", "{{ value_json.status }}"}});
    }

    for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
    {
        auto ep = it.value().staticCast<EndpointObject>();
        if (!ep)
            continue;

        QString objectId = ep->meta().value("objectId").toString();
        if (objectId.isEmpty())
            continue;

        for (const auto &expose : ep->exposes())
        {
            if (!expose->discovery())
                continue;

            QString configTopic = QString("%1/%2/%3/%4/config").arg(m_haPrefix, expose->component(), nodeId, objectId);
            QJsonObject json;

            if (!remove)
            {
                // Our own fd//td topics, keyed by objectId -- matches exactly
                // what stateChanged()/mqttReceived() publish/subscribe to, no
                // separate numeric-endpoint scheme to keep in sync.
                expose->setStateTopic(mqttTopic("fd/%1/%2/%3").arg(serviceTopic(), devTopic, objectId));
                expose->setCommandTopic(mqttTopic("td/%1/%2/%3").arg(serviceTopic(), devTopic, objectId));

                QString icon = ep->meta().value("icon").toString();

                json = expose->request();
                json.insert("availability", availability);
                json.insert("availability_mode", "all");
                json.insert("device", identity);
                json.insert("name", expose->title());
                json.insert("unique_id", QString("%1_%2").arg(nodeId, objectId));
                // Built directly from ep->meta() (populated at discovery time
                // straight from ESPHome's own icon field) rather than through
                // ExposeObject's option()/request() -- unlike title, icon has
                // no homed-common-native slot to collide with at all, so no
                // need to route it through that mechanism the way title/class
                // sometimes have to.
                if (!icon.isEmpty())
                    json.insert("icon", icon);
            }

            mqttPublish(configTopic, json, true);
        }
    }
}

void Controller::clearStaleTopics(DeviceObject *device)
{
    QString devTopic = deviceTopic(device);
    QStringList published = device->publishedEndpoints();

    // Collect current valid endpoint objectIds
    QStringList current;
    for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
    {
        auto ep = it.value().staticCast<EndpointObject>();
        if (ep)
        {
            QString objectId = ep->meta().value("objectId").toString();
            if (!objectId.isEmpty())
                current.append(objectId);
        }
    }

    // One-time migration: if nothing tracked yet but endpoints exist, clear old numeric IDs
    if (published.isEmpty() && !device->endpoints().isEmpty())
    {
        for (int i = 1; i <= device->endpoints().count(); i++)
            mqttPublish(mqttTopic("fd/%1/%2/%3").arg(serviceTopic(), devTopic).arg(i), QJsonObject(), true);
    }

    // Clear stale tracked topics
    for (const QString &key : published)
    {
        if (current.contains(key))
            continue;
        mqttPublish(mqttTopic("fd/%1/%2/%3").arg(serviceTopic(), devTopic, key), QJsonObject(), true);
    }
}

void Controller::publishStatus(void)
{
    QJsonArray devices;

    for (int i = 0; i < m_devices->count(); i++)
    {
        DeviceObject *device = m_devices->at(i).data();
        QJsonObject json;
        json.insert("name",             device->name());
        json.insert("host",             device->host());
        json.insert("port",             device->port());
        json.insert("manufacturerName", device->manufacturerName());
        json.insert("modelName",        device->modelName());
        json.insert("esphomeVersion",   device->esphomeVersion());
        json.insert("lastSeen",         device->lastSeen());
        json.insert("active",           device->active());
        json.insert("discovery",        device->discovery());
        devices.append(json);
    }

    mqttPublishStatus({{"status", "online"}, {"devices", devices}, {"names", m_devices->names()}, {"version", SERVICE_VERSION}});
}

void Controller::publishDeviceStatus(DeviceObject *device)
{
    QString status = device->availability() == Availability::Online ? "online" : "offline";
    mqttPublish(mqttTopic("device/%1/%2").arg(serviceTopic(), deviceTopic(device)),
        {{"lastSeen", device->lastSeen()}, {"status", status}}, true);
}

void Controller::mqttConnected(void)
{
    mqttSubscribe(mqttTopic("command/%1").arg(serviceTopic()));
    mqttSubscribe(mqttTopic("td/%1/#").arg(serviceTopic()));

    for (int i = 0; i < m_devices->count(); i++)
    {
        clearStaleTopics(m_devices->at(i).data());
        publishExposes(m_devices->at(i).data());
    }

    if (m_haEnabled)
    {
        mqttPublishDiscovery("ESPHome", SERVICE_VERSION, m_haPrefix);
        mqttSubscribe(m_haStatus);
    }

    for (int i = 0; i < m_devices->count(); i++)
        publishDeviceStatus(m_devices->at(i).data());

    mqttPublishService();
    publishStatus();
}

void Controller::mqttReceived(const QByteArray &message, const QMqttTopicName &topic)
{
    QString subTopic = topic.name().replace(0, mqttTopic().length(), QString());
    QJsonObject json = QJsonDocument::fromJson(message).object();

    if (subTopic == QString("command/%1").arg(serviceTopic()))
    {
        switch (static_cast<Command>(m_commands.keyToValue(json.value("action").toString().toUtf8().constData())))
        {
            case Command::restartService:
                logWarning << "Restart request received...";
                mqttPublish(topic.name(), QJsonObject(), true);
                QCoreApplication::exit(EXIT_RESTART);
                break;

            case Command::addDevice:
            {
                QString host = json.value("host").toString();
                quint16 port = static_cast<quint16>(json.value("port").toInt(6053));
                QString keyB64 = json.value("key").toString();
                QString name = mqttSafe(json.value("name").toString());

                if (host.isEmpty() || keyB64.isEmpty())
                {
                    logWarning << "addDevice: missing host or key";
                    break;
                }

                QByteArray keyBytes = QByteArray::fromBase64(keyB64.toUtf8());
                if (keyBytes.size() != 32)
                {
                    logWarning << "addDevice: invalid key (must be 32 bytes base64)";
                    break;
                }

                if (!m_devices->byHost(host).isNull())
                {
                    logWarning << "addDevice: device already exists for host" << host;
                    break;
                }

                if (name.isEmpty())
                    name = mqttSafe(host);

                auto device = Device(new DeviceObject(name, host, port, keyBytes));
                m_devices->append(device);
                m_devices->store();

                logInfo << "added device" << name << "at" << host;

                m_esphome->connectDevice(device.data());
                publishStatus();

                QJsonObject ev = {{"event", "added"}, {"device", name}};
                mqttPublish(mqttTopic("event/%1").arg(serviceTopic()), ev);
                break;
            }

            case Command::removeDevice:
            {
                QString id = json.value("device").toString();
                int index = -1;
                Device device = m_devices->byName(id, &index);

                if (device.isNull())
                    device = m_devices->byHost(id);

                if (device.isNull())
                {
                    logWarning << "removeDevice: not found:" << id;
                    break;
                }

                index = m_devices->indexOf(device);
                m_esphome->disconnectDevice(device.data());
                publishExposes(device.data(), true);
                mqttPublish(mqttTopic("device/%1/%2").arg(serviceTopic(), deviceTopic(device.data())), QJsonObject(), true);
                m_devices->removeAt(index);
                m_devices->store();
                publishStatus();

                QJsonObject ev = {{"event", "removed"}, {"device", device->name()}};
                mqttPublish(mqttTopic("event/%1").arg(serviceTopic()), ev);
                break;
            }

            case Command::updateDevice:
            {
                QString id = json.value("device").toString();
                Device device = m_devices->byName(id);
                if (device.isNull()) device = m_devices->byHost(id);
                if (device.isNull()) break;

                if (json.contains("name"))
                    device->setName(mqttSafe(json.value("name").toString()));
                if (json.contains("active"))
                    device->setActive(json.value("active").toBool());
                if (json.contains("discovery"))
                    device->setDiscovery(json.value("discovery").toBool());

                m_devices->store();
                publishExposes(device.data());
                publishStatus();
                break;
            }

            case Command::getProperties:
            {
                QString id = json.value("device").toString();
                Device device = m_devices->byName(id);
                if (device.isNull()) device = m_devices->byHost(id);
                if (!device.isNull())
                    updateProperties();
                break;
            }
        }
    }
    else if (subTopic.startsWith(QString("td/%1/").arg(serviceTopic())))
    {
        QList<QString> parts = subTopic.remove(QString("td/%1/").arg(serviceTopic())).split('/');
        QString deviceId = parts.value(0);
        QString objectId = parts.value(1);

        Device device = m_devices->byName(deviceId);
        if (device.isNull()) device = m_devices->byHost(deviceId);
        if (device.isNull() || objectId.isEmpty())
            return;

        quint8 endpointId = 0;
        for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
        {
            auto ep = it.value().staticCast<EndpointObject>();
            if (ep && ep->meta().value("objectId").toString() == objectId)
            {
                endpointId = it.key();
                break;
            }
        }

        if (endpointId == 0)
            return;

        for (auto it = json.begin(); it != json.end(); it++)
        {
            if (!it.value().toVariant().isValid())
                continue;
            m_esphome->sendCommand(device.data(), endpointId, it.key(), it.value().toVariant());
        }
    }
    else if (topic.name() == m_haStatus)
    {
        if (message != "online")
            return;
        m_propertiesTimer->start(UPDATE_PROPERTIES_DELAY);
    }
}

void Controller::updateProperties(void)
{
    for (int i = 0; i < m_devices->count(); i++)
    {
        DeviceObject *device = m_devices->at(i).data();
        if (!device->active())
            continue;

        for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
        {
            auto ep = it.value().staticCast<EndpointObject>();
            if (!ep || ep->stateMap().isEmpty())
                continue;
            stateChanged(device, it.key());
        }
    }
}

void Controller::entitiesDiscovered(DeviceObject *device)
{
    QStringList endpoints;
    for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
    {
        auto ep = it.value().staticCast<EndpointObject>();
        if (ep)
        {
            QString objectId = ep->meta().value("objectId").toString();
            if (!objectId.isEmpty())
                endpoints.append(objectId);
        }
    }
    device->setPublishedEndpoints(endpoints);

    m_devices->store();
    clearStaleTopics(device);
    publishExposes(device);
    publishDeviceStatus(device);
    publishStatus();

    QJsonObject ev = {{"event", "updated"}, {"device", device->name()}};
    mqttPublish(mqttTopic("event/%1").arg(serviceTopic()), ev);
}

void Controller::stateChanged(DeviceObject *device, quint8 endpointId)
{
    if (!mqttStatus())
        return;

    auto ep = device->endpoints().value(endpointId).staticCast<EndpointObject>();
    if (!ep || ep->stateMap().isEmpty())
        return;

    QString objectId = ep->meta().value("objectId").toString();
    if (objectId.isEmpty())
        return;

    mqttPublish(mqttTopic("fd/%1/%2/%3").arg(serviceTopic(), deviceTopic(device), objectId), ep->state());
}

void Controller::availabilityChanged(DeviceObject *device)
{
    publishDeviceStatus(device);

    if (device->availability() == Availability::Online)
        m_propertiesTimer->start(UPDATE_PROPERTIES_DELAY);
}
