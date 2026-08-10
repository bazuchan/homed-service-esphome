#ifndef DEVICE_H
#define DEVICE_H

#define STORE_DATABASE_DELAY    20

// Overridable at build time via qmake DEFINES (see homed-esphome.pro), which
// ci/build.sh sets from the release tag -- this fallback is only for local
// dev builds run without a version.
#ifndef SERVICE_VERSION
#define SERVICE_VERSION "0.1.0"
#endif

#include <QDateTime>
#include <QFile>
#include <QSettings>
#include <QTimer>
#include "endpoint.h"
#include "expose.h"

class EndpointObject : public AbstractEndpointObject
{

public:

    EndpointObject(quint8 id, const Device &device) : AbstractEndpointObject(id, device) {}

    inline QMap <QString, QVariant> &stateMap(void) { return m_state; }
    inline void setState(const QString &key, const QVariant &value) { m_state.insert(key, value); }

    QJsonObject state(void) const { return QJsonObject::fromVariantMap(m_state); }

private:

    QMap <QString, QVariant> m_state;

};

class DeviceObject : public AbstractDeviceObject
{

public:

    DeviceObject(const QString &name, const QString &host, quint16 port, const QByteArray &encryptionKey)
        : AbstractDeviceObject(name), m_host(host), m_port(port), m_encryptionKey(encryptionKey), m_lastSeen(0) {}

    inline QString host(void) { return m_host; }
    inline void setHost(const QString &value) { m_host = value; }

    inline quint16 port(void) { return m_port; }
    inline void setPort(quint16 value) { m_port = value; }

    inline QByteArray encryptionKey(void) { return m_encryptionKey; }
    inline void setEncryptionKey(const QByteArray &value) { m_encryptionKey = value; }

    inline QString esphomeVersion(void) { return m_esphomeVersion; }
    inline void setEsphomeVersion(const QString &value) { m_esphomeVersion = value; }

    inline qint64 lastSeen(void) { return m_lastSeen; }
    inline void setLastSeen(qint64 value) { m_lastSeen = value; }
    inline void updateLastSeen(void) { m_lastSeen = QDateTime::currentSecsSinceEpoch(); }

    inline QString address(void) { return mqttSafe(m_host); }

    inline QStringList publishedEndpoints(void) { return m_publishedEndpoints; }
    inline void setPublishedEndpoints(const QStringList &value) { m_publishedEndpoints = value; }

private:

    QString m_host, m_esphomeVersion;
    quint16 m_port;
    QByteArray m_encryptionKey;
    qint64 m_lastSeen;
    QStringList m_publishedEndpoints;

};

class DeviceList : public QObject, public QList <Device>
{
    Q_OBJECT

public:

    DeviceList(QSettings *config, QObject *parent);
    ~DeviceList(void);

    inline bool names(void) { return m_names; }

    void init(void);
    void store(bool sync = false);

    Device byName(const QString &name, int *index = nullptr);
    Device byHost(const QString &host);
    Device parse(const QJsonObject &json);

private:

    QTimer *m_timer;
    QFile m_file;
    bool m_names, m_sync;

    void unserialize(const QJsonArray &devices);
    QJsonArray serialize(void);
    void setupEndpoints(const Device &device, const QJsonArray &entities);

private slots:

    void writeDatabase(void);

};

inline QDebug operator << (QDebug debug, DeviceObject *device) { return debug << "device" << device->name(); }
inline QDebug operator << (QDebug debug, const Device &device) { return debug << "device" << device->name(); }

#endif
