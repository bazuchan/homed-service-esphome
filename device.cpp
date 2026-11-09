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

bool DeviceList::nameAvailable(const QString &name, DeviceObject *except) const
{
    if (name.isEmpty())
        return false;

    for (const auto &device : *this)
    {
        if (device.data() == except)
            continue;
        // Commands accept both names and addresses, so neither may shadow another device.
        if (device->name() == name || device->host() == name || device->address() == name)
            return false;
    }
    return true;
}

QString DeviceList::uniqueName(const QString &name, DeviceObject *except) const
{
    QString base = mqttSafe(name);
    if (base.isEmpty())
        base = "Device";
    QString candidate = base;
    int suffix = 2;
    while (!nameAvailable(candidate, except))
        candidate = QString("%1 (%2)").arg(base).arg(suffix++);
    return candidate;
}

bool DeviceList::renameDevice(DeviceObject *device, const QString &name)
{
    QString value = mqttSafe(name);
    if (value.isEmpty() || value == device->name())
        return true;
    if (!nameAvailable(value, device))
        return false;

    emit deviceAboutToRename(device);
    device->setName(value);
    emit deviceRenamed(device);
    return true;
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

    QJsonObject specialSlots = json.value("specialSlots").toObject();
    for (auto it = specialSlots.begin(); it != specialSlots.end(); it++)
        device->specialSlots().insert(it.key().toInt(), it.value().toString());

    device->setParentAddress(json.value("parentAddress").toString());

    QJsonObject snapshot = {{"entityFormat", json.value("entityFormat")}, {"entities", json.value("entities")}, {"entityOptions", json.value("entityOptions")}};
    device->setEntitySnapshot(snapshot);
    setupEndpoints(device, snapshot);
    return device;
}

Expose DeviceList::createExpose(const QString &type, const QString &objectId, bool special)
{
    if (type == "switch")
        return special ? Expose(new SwitchObject) : Expose(new ExposeObject(objectId));
    if (type == "light") return Expose(new LightObject);
    if (type == "cover") return Expose(new CoverObject);
    if (type == "climate") return Expose(new ThermostatObject);
    if (type == "lock") return Expose(new LockObject);
    if (type == "binary_sensor") return Expose(new BinaryObject(objectId));
    if (type == "sensor" || type == "text_sensor") return Expose(new SensorObject(objectId));
    if (type == "select") return Expose(new SelectObject(objectId));
    if (type == "number") return Expose(new NumberObject(objectId));
    if (type == "button") return Expose(new ButtonObject(objectId));
    return Expose();
}

void DeviceList::setupEndpoints(const Device &device, const QJsonObject &snapshot)
{
    // Legacy records lack endpoint IDs, special flags and complete options; keep them until live discovery replaces them.
    if (snapshot.value("entityFormat").toInt() != 2)
    {
        logInfo << device << "entity cache needs live discovery before publishing exposes";
        return;
    }

    bool valid = snapshot.value("entities").isArray() && snapshot.value("entityOptions").isObject();
    QJsonArray entities = snapshot.value("entities").toArray();

    for (const auto &item : entities)
    {
        QJsonObject entity = item.toObject();
        int endpointId = entity.value("endpointId").toInt();
        QJsonObject meta = entity.value("meta").toObject();
        QString type = meta.value("type").toString(), objectId = meta.value("objectId").toString();
        bool special = meta.value("special").toBool();
        Expose expose = createExpose(type, objectId, special);

        if (endpointId < 1 || endpointId > 255 || device->endpoints().contains(endpointId) || objectId.isEmpty()
            || meta.value("key").toDouble() <= 0 || !meta.value("special").isBool() || expose.isNull())
        {
            valid = false;
            break;
        }

        auto ep = QSharedPointer<EndpointObject>::create(static_cast<quint8>(endpointId), device);
        ep->meta() = meta.toVariantMap();
        ep->meta().remove("deviceId");
        expose->setParent(ep.data());
        ep->exposes().append(expose);
        device->endpoints().insert(static_cast<quint8>(endpointId), ep);
    }

    if (!valid)
    {
        device->endpoints().clear();
        logWarning << device << "invalid entity cache, waiting for live discovery";
        return;
    }

    device->options() = snapshot.value("entityOptions").toObject().toVariantMap();
    device->setEntitiesReady(true);
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

        QJsonObject snapshot = device->entitySnapshot();
        if (device->entitiesReady())
        {
            QJsonArray entities;
            for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
            {
                QVariantMap meta = it.value()->meta();
                // Protocol device IDs are session-local and must come from the next discovery, never from disk.
                meta.remove("deviceId");
                entities.append(QJsonObject {{"endpointId", it.key()}, {"meta", QJsonObject::fromVariantMap(meta)}});
            }
            snapshot = QJsonObject {{"entityFormat", 2}, {"entities", entities}, {"entityOptions", QJsonObject::fromVariantMap(device->options())}};
        }
        for (auto it = snapshot.begin(); it != snapshot.end(); it++)
            json.insert(it.key(), it.value());

        json.insert("publishedEndpoints", QJsonArray::fromStringList(device->publishedEndpoints()));

        QJsonObject specialSlots;
        for (auto it = device->specialSlots().begin(); it != device->specialSlots().end(); it++)
            specialSlots.insert(QString::number(it.key()), it.value());
        json.insert("specialSlots", specialSlots);

        if (!device->parentAddress().isEmpty())
            json.insert("parentAddress", device->parentAddress());

        array.append(json);
    }

    return array;
}

void DeviceList::writeDatabase(void)
{
    QJsonObject json;
    json.insert("devices", serialize());
    json.insert("timestamp", static_cast<double>(QDateTime::currentSecsSinceEpoch()));
    json.insert("version", SERVICE_VERSION);

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
