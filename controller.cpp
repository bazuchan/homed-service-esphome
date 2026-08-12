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

// Resolves a JSON key on the shared "common" td topic by objectId -- special (numbered-slot) entities have their own dedicated td topic instead.
quint8 Controller::endpointForAction(DeviceObject *device, const QString &action)
{
    for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
    {
        auto ep = it.value().staticCast<EndpointObject>();
        if (ep && ep->meta().value("objectId").toString() == action)
            return it.key();
    }

    return 0;
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

    // Built ourselves rather than via DeviceObject::publishExposes() (homed-common) -- that also republishes a colliding numeric-endpoint-keyed copy and doesn't know about our shared fd//td topics.
    if (m_haEnabled)
        publishHaDiscovery(device, devTopic, remove);

    // light/cover/climate/non-toggle switch get a stable numbered key (esphome.cpp's DeviceObject::specialSlots()); everything else is objectId-named and shares "common".
    if (!remove)
    {
        QList<QString> commonItems;
        QMap<QString, QVariant> commonOptions;
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

            QList<QString> epItems;
            for (const auto &expose : ep->exposes())
                epItems.append(expose->name());

            if (ep->meta().value("special").toBool())
            {
                QMap<QString, QVariant> options;
                options.insert("name", ep->meta().value("title").toString());

                QString icon = ep->meta().value("icon").toString();
                if (!icon.isEmpty())
                    options.insert("icon", icon);

                if (entityType == "light")
                {
                    QVariant lightOpt = device->options().value(objectId + "_light");
                    options.insert("light", lightOpt.isValid() ? lightOpt : QVariant(QVariantList()));

                    QVariant ctOpt = device->options().value(objectId + "_colorTemperature");
                    if (ctOpt.isValid()) options.insert("colorTemperature", ctOpt);
                }
                else if (entityType == "climate")
                {
                    static const QList<QString> climateKeys = {"systemMode", "operationMode", "fanMode", "targetTemperature", "runningStatus"};
                    for (const auto &key : climateKeys)
                    {
                        QVariant opt = device->options().value(QString("%1_%2").arg(key).arg(it.key()));
                        if (opt.isValid())
                            options.insert(key, opt);
                    }
                }
                // switch/lock/cover: nothing else populated yet (see esphome.cpp)

                QMap<QString, QVariant> slotData;
                slotData.insert("items", QVariant(epItems));
                slotData.insert("options", options);

                data.insert(QString::number(it.key()), slotData);
            }
            else
            {
                commonItems.append(epItems);

                // icon rides along inside this same per-objectId map, next to title -- homed-web reads options[property], never a top-level "icon"
                QVariant objOpt = device->options().value(objectId);
                if (objOpt.isValid())
                    commonOptions.insert(epItems.first(), objOpt);
            }
        }

        QMap<QString, QVariant> commonData;
        commonData.insert("items", QVariant(commonItems));
        if (!commonOptions.isEmpty())
            commonData.insert("options", commonOptions);

        data.insert("common", commonData);

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
                // Special entities get their own fd//td topic (suffixed with their stable number); everything else shares the device-wide "common" one
                QString topic = ep->meta().value("special").toBool()
                    ? QString("%1/%2").arg(devTopic).arg(it.key())
                    : devTopic;

                expose->setStateTopic(mqttTopic("fd/%1/%2").arg(serviceTopic(), topic));
                expose->setCommandTopic(mqttTopic("td/%1/%2").arg(serviceTopic(), topic));

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

    // one-time migration from the oldest (pre-objectId) scheme: numeric endpoint IDs used to be the fd topic's own final path segment
    if (published.isEmpty() && !device->endpoints().isEmpty())
    {
        for (int i = 1; i <= device->endpoints().count(); i++)
            mqttPublish(mqttTopic("fd/%1/%2/%3").arg(serviceTopic(), devTopic).arg(i), QJsonObject(), true);
    }

    // one-time migration from the objectId-per-entity-topic scheme: those retained topics are stale now (see publishExposes()/publishDeviceState()); publishedEndpoints() isn't repopulated after this, so this only runs once more
    for (const QString &key : published)
        mqttPublish(mqttTopic("fd/%1/%2/%3").arg(serviceTopic(), devTopic, key), QJsonObject(), true);

    if (!published.isEmpty())
        device->setPublishedEndpoints(QStringList());
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

        if (!device->parentAddress().isEmpty())
        {
            Device parent = m_devices->byHost(device->parentAddress());
            json.insert("subdeviceOf", parent.isNull() ? device->parentAddress() : parent->name());
        }

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

                // sub-devices ride on the parent's connection (see EspHomeDevice::subDevice()) -- removing the parent would otherwise orphan them: still published, but permanently unreachable since their parentAddress no longer resolves to a live connection
                for (int i = m_devices->count() - 1; i >= 0; i--)
                {
                    Device sub = m_devices->at(i);
                    if (sub == device || sub->parentAddress() != device->address())
                        continue;

                    publishExposes(sub.data(), true);
                    mqttPublish(mqttTopic("device/%1/%2").arg(serviceTopic(), deviceTopic(sub.data())), QJsonObject(), true);
                    m_devices->removeAt(i);

                    QJsonObject subEv = {{"event", "removed"}, {"device", sub->name()}};
                    mqttPublish(mqttTopic("event/%1").arg(serviceTopic()), subEv);
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
        // td/<service>/<device> (shared "common") or td/<service>/<device>/<number> (one special entity's own topic)
        QList<QString> parts = subTopic.mid(QString("td/%1/").arg(serviceTopic()).length()).split('/');
        QString deviceId = parts.value(0);
        QString slotStr = parts.value(1);

        Device device = m_devices->byName(deviceId);
        if (device.isNull()) device = m_devices->byHost(deviceId);
        if (device.isNull())
            return;

        bool slotOk = false;
        quint8 slotEndpointId = slotStr.isEmpty() ? 0 : static_cast<quint8>(slotStr.toUInt(&slotOk));
        if (!slotStr.isEmpty() && !slotOk)
            return;

        for (auto it = json.begin(); it != json.end(); it++)
        {
            quint8 endpointId;

            if (!it.value().toVariant().isValid())
                continue;

            endpointId = slotEndpointId ? slotEndpointId : endpointForAction(device.data(), it.key());
            if (endpointId == 0)
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

        publishDeviceState(device);
    }
}

void Controller::entitiesDiscovered(DeviceObject *device)
{
    // publishedEndpoints is only clearStaleTopics()'s one-time migration marker now, not repopulated here
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
    Q_UNUSED(endpointId) // which endpoint changed doesn't matter -- publishDeviceState() re-publishes the right topic(s) regardless
    publishDeviceState(device);
}

void Controller::publishDeviceState(DeviceObject *device)
{
    if (!mqttStatus())
        return;

    QString devTopic = deviceTopic(device);
    QMap<QString, QVariant> commonState;
    bool hasCommonState = false;

    // special entities publish their own fd topic; everything else merges into one shared "common" snapshot -- same split as publishExposes()
    for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
    {
        auto ep = it.value().staticCast<EndpointObject>();
        if (!ep || ep->stateMap().isEmpty())
            continue;

        if (ep->meta().value("special").toBool())
        {
            mqttPublish(mqttTopic("fd/%1/%2/%3").arg(serviceTopic(), devTopic).arg(it.key()), ep->state());
            continue;
        }

        hasCommonState = true;
        for (auto stateIt = ep->stateMap().begin(); stateIt != ep->stateMap().end(); stateIt++)
            commonState.insert(stateIt.key(), stateIt.value());
    }

    if (hasCommonState)
        mqttPublish(mqttTopic("fd/%1/%2").arg(serviceTopic(), devTopic), QJsonObject::fromVariantMap(commonState));
}

void Controller::availabilityChanged(DeviceObject *device)
{
    publishDeviceStatus(device);

    if (device->availability() == Availability::Online)
        m_propertiesTimer->start(UPDATE_PROPERTIES_DELAY);
}
