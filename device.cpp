#include "device.h"
#include "logger.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// ==================== DeviceList ====================

DeviceList::DeviceList(QSettings *config, QObject *parent) : QObject(parent), m_names(false), m_sync(false)
{
    QString path = config->value("device/database", "/opt/homed-esphome/database.json").toString();
    m_names = config->value("mqtt/names", false).toBool();
    m_file.setFileName(path);
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &DeviceList::writeDatabase);
}

DeviceList::~DeviceList(void)
{
    writeDatabase();
}

void DeviceList::init(void)
{
    QJsonObject json;
    QDir().mkpath(QFileInfo(m_file).absolutePath());

    if (m_file.open(QFile::ReadOnly))
    {
        json = QJsonDocument::fromJson(m_file.readAll()).object();
        m_file.close();
    }

    unserialize(json.value("devices").toArray());
}

void DeviceList::store(bool sync)
{
    if (sync)
    {
        m_timer->stop();
        writeDatabase();
        return;
    }

    m_sync = false;
    m_timer->start(STORE_DATABASE_DELAY * 1000);
}

Device DeviceList::byName(const QString &name, int *index)
{
    for (int i = 0; i < count(); i++)
    {
        if (at(i)->name() != name)
            continue;

        if (index)
            *index = i;

        return at(i);
    }

    return Device();
}

Device DeviceList::byHost(const QString &host)
{
    for (int i = 0; i < count(); i++)
        if (at(i)->host() == host)
            return at(i);

    return Device();
}

Device DeviceList::parse(const QJsonObject &json)
{
    QString name = json.value("name").toString();
    QString host = json.value("host").toString();
    quint16 port = static_cast<quint16>(json.value("port").toInt(6053));
    QByteArray key = QByteArray::fromBase64(json.value("encryptionKey").toString().toUtf8());

    if (name.isEmpty() || host.isEmpty() || key.isEmpty())
        return Device();

    Device device = Device(new DeviceObject(name, host, port, key));
    device->setActive(json.value("active").toBool(true));
    device->setDiscovery(json.value("discovery").toBool(true));
    device->setCloud(json.value("cloud").toBool(true));
    device->setLastSeen(static_cast<qint64>(json.value("lastSeen").toDouble(0)));
    device->setManufacturerName(json.value("manufacturerName").toString());

    QStringList publishedEndpoints;
    for (const auto &ep : json.value("publishedEndpoints").toArray())
        publishedEndpoints.append(ep.toString());
    device->setPublishedEndpoints(publishedEndpoints);
    device->setModelName(json.value("modelName").toString());
    device->setEsphomeVersion(json.value("esphomeVersion").toString());

    setupEndpoints(device, json.value("entities").toArray());
    return device;
}

void DeviceList::setupEndpoints(const Device &device, const QJsonArray &entities)
{
    device->endpoints().clear();

    for (int i = 0; i < entities.count(); i++)
    {
        QJsonObject entity = entities.at(i).toObject();
        QString type = entity.value("type").toString();
        QString objectId = entity.value("objectId").toString();
        quint32 key = static_cast<quint32>(entity.value("key").toDouble());
        quint8 endpointId = static_cast<quint8>(i + 1);

        if (type.isEmpty() || objectId.isEmpty() || key == 0)
            continue;

        auto ep = QSharedPointer<EndpointObject>::create(endpointId, device);
        ep->meta().insert("key", key);
        ep->meta().insert("type", type);
        ep->meta().insert("objectId", objectId);

        QString entityName = entity.value("name").toString();
        QString title = entityName.isEmpty() ? objectId : entityName;

        if (type == "switch")
        {
            ep->exposes().append(QSharedPointer<SwitchObject>::create());
            device->options().insert(objectId, QVariantMap {{"title", title}});
        }
        else if (type == "binary_sensor")
        {
            auto expose = QSharedPointer<BinaryObject>::create(objectId);
            QVariantMap opts = {{"title", title}};
            if (!entity.value("deviceClass").toString().isEmpty())
                opts.insert("class", entity.value("deviceClass").toString());
            device->options().insert(objectId, opts);
            ep->exposes().append(expose);
        }
        else if (type == "sensor" || type == "text_sensor")
        {
            auto expose = QSharedPointer<SensorObject>::create(objectId);
            QVariantMap opts = {{"title", title}};
            if (!entity.value("unit").toString().isEmpty())
                opts.insert("unit", entity.value("unit").toString());
            if (!entity.value("deviceClass").toString().isEmpty())
                opts.insert("class", entity.value("deviceClass").toString());
            if (!entity.value("stateClass").toString().isEmpty())
                opts.insert("state", entity.value("stateClass").toString());
            if (entity.value("accuracyDecimals").toInt() > 0)
                opts.insert("round", entity.value("accuracyDecimals").toInt());
            device->options().insert(objectId, opts);
            ep->exposes().append(expose);
        }
        else if (type == "light")
        {
            auto expose = QSharedPointer<LightObject>::create();
            QStringList lightOptions = entity.value("lightOptions").toString().split(',', Qt::SkipEmptyParts);
            device->options().insert("light", lightOptions);
            if (entity.contains("minMireds") && entity.contains("maxMireds"))
            {
                device->options().insert("colorTemperature",
                    QVariantMap {{"min", entity.value("minMireds").toInt()}, {"max", entity.value("maxMireds").toInt()}});
            }
            ep->exposes().append(expose);
        }
        else if (type == "select")
        {
            auto expose = QSharedPointer<SelectObject>::create(objectId);
            QStringList opts = entity.value("options").toString().split('\n', Qt::SkipEmptyParts);
            device->options().insert(objectId, QVariantMap {{"enum", opts}, {"control", true}, {"title", title}});
            ep->exposes().append(expose);
        }
        else if (type == "number")
        {
            auto expose = QSharedPointer<NumberObject>::create(objectId);
            QVariantMap opts;
            opts.insert("min", entity.value("minValue").toDouble());
            opts.insert("max", entity.value("maxValue").toDouble());
            opts.insert("step", entity.value("step").toDouble(1.0));
            opts.insert("control", true);
            opts.insert("title", title);
            if (!entity.value("unit").toString().isEmpty())
                opts.insert("unit", entity.value("unit").toString());
            device->options().insert(objectId, opts);
            ep->exposes().append(expose);
        }
        else if (type == "button")
        {
            auto expose = QSharedPointer<ButtonObject>::create();
            device->options().insert(objectId, QVariantMap {{"type", "button"}, {"control", true}, {"title", title}});
            ep->exposes().append(expose);
        }

        device->endpoints().insert(endpointId, ep);
    }
}

void DeviceList::unserialize(const QJsonArray &devices)
{
    for (const auto &item : devices)
    {
        Device device = parse(item.toObject());
        if (device.isNull())
            continue;
        append(device);
    }
}

QJsonArray DeviceList::serialize(void)
{
    QJsonArray array;

    for (int i = 0; i < count(); i++)
    {
        const Device &device = at(i);
        QJsonObject json;
        json.insert("name", device->name());
        json.insert("host", device->host());
        json.insert("port", device->port());
        json.insert("encryptionKey", QString::fromUtf8(device->encryptionKey().toBase64()));
        json.insert("active", device->active());
        json.insert("discovery", device->discovery());
        json.insert("cloud", device->cloud());
        json.insert("lastSeen", device->lastSeen());
        json.insert("manufacturerName", device->manufacturerName());
        json.insert("modelName", device->modelName());
        json.insert("esphomeVersion", device->esphomeVersion());

        QJsonArray entities;
        for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
        {
            const auto &ep = it.value();
            QJsonObject entity;
            entity.insert("key", static_cast<double>(ep->meta().value("key").toUInt()));
            entity.insert("type", ep->meta().value("type").toString());
            entity.insert("objectId", ep->meta().value("objectId").toString());

            QString type = ep->meta().value("type").toString();
            QString objectId = ep->meta().value("objectId").toString();

            QString entityTitle = device->options().value(objectId).toMap().value("title").toString();
            if (!entityTitle.isEmpty() && entityTitle != objectId)
                entity.insert("name", entityTitle);

            if (type == "binary_sensor")
            {
                entity.insert("deviceClass", device->options().value(objectId).toMap().value("class").toString());
            }
            else if (type == "sensor" || type == "text_sensor")
            {
                QVariantMap opts = device->options().value(objectId).toMap();
                if (opts.contains("unit")) entity.insert("unit", opts.value("unit").toString());
                if (opts.contains("class")) entity.insert("deviceClass", opts.value("class").toString());
                if (opts.contains("state")) entity.insert("stateClass", opts.value("state").toString());
                if (opts.contains("round")) entity.insert("accuracyDecimals", opts.value("round").toInt());
            }
            else if (type == "light")
            {
                QStringList opts = device->options().value("light").toStringList();
                entity.insert("lightOptions", opts.join(','));
                QVariantMap ct = device->options().value("colorTemperature").toMap();
                if (ct.contains("min")) entity.insert("minMireds", ct.value("min").toInt());
                if (ct.contains("max")) entity.insert("maxMireds", ct.value("max").toInt());
            }
            else if (type == "select")
            {
                QStringList opts = device->options().value(objectId).toMap().value("enum").toStringList();
                entity.insert("options", opts.join('\n'));
            }
            else if (type == "number")
            {
                QVariantMap opts = device->options().value(objectId).toMap();
                entity.insert("minValue", opts.value("min").toDouble());
                entity.insert("maxValue", opts.value("max").toDouble());
                entity.insert("step", opts.value("step", 1.0).toDouble());
                if (opts.contains("unit")) entity.insert("unit", opts.value("unit").toString());
            }

            entities.append(entity);
        }

        json.insert("entities", entities);
        json.insert("publishedEndpoints", QJsonArray::fromStringList(device->publishedEndpoints()));
        array.append(json);
    }

    return array;
}

void DeviceList::writeDatabase(void)
{
    QJsonObject json;
    json.insert("devices", serialize());
    json.insert("timestamp", static_cast<double>(QDateTime::currentSecsSinceEpoch()));
    json.insert("version", "0.1.0");

    QJsonDocument doc(json);
    QByteArray data = doc.toJson(QJsonDocument::Compact);

    if (!m_file.open(QFile::WriteOnly | QFile::Truncate))
    {
        logWarning << "Failed to open database file for writing:" << m_file.fileName();
        return;
    }

    m_file.write(data);
    m_file.close();
}
