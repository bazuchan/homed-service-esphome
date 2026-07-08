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

void Controller::publishExposes(DeviceObject *device, bool remove)
{
    // HA discovery still uses the common library (numeric endpoint IDs in HA config)
    if (m_haEnabled)
    {
        device->publishExposes(this,
            device->address(),
            QString("%1_%2").arg(uniqueId(), device->address()),
            m_haPrefix, true, m_haUpdate,
            m_devices->names(), remove);
    }

    // Publish expose JSON with objectId as endpoint keys for stable topic routing
    QString devTopic = deviceTopic(device);

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
            QVariant objOpt = device->options().value(objectId);
            if (objOpt.isValid())
                options.insert(items.first(), objOpt);

            if (entityType == "light")
            {
                QVariant lightOpt = device->options().value("light");
                if (lightOpt.isValid()) options.insert("light", lightOpt);
                QVariant ctOpt = device->options().value("colorTemperature");
                if (ctOpt.isValid()) options.insert("colorTemperature", ctOpt);
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

    QString topic = mqttTopic("fd/%1/%2/%3").arg(serviceTopic(), deviceTopic(device), objectId);
    mqttPublish(topic, ep->state());
}

void Controller::availabilityChanged(DeviceObject *device)
{
    publishDeviceStatus(device);

    if (device->availability() == Availability::Online)
        m_propertiesTimer->start(UPDATE_PROPERTIES_DELAY);
}
