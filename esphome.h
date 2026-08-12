#ifndef ESPHOME_H
#define ESPHOME_H

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include "device.h"
#include "noise.h"
#include "proto.h"

// ESPHome API message type IDs
namespace MsgType {
    static const quint16 HelloRequest            = 1;
    static const quint16 HelloResponse           = 2;
    static const quint16 DisconnectRequest       = 5;
    static const quint16 DisconnectResponse      = 6;
    static const quint16 PingRequest             = 7;
    static const quint16 PingResponse            = 8;
    static const quint16 DeviceInfoRequest       = 9;
    static const quint16 DeviceInfoResponse      = 10;
    static const quint16 ListEntitiesRequest     = 11;
    static const quint16 ListEntitiesBinary      = 12;
    static const quint16 ListEntitiesCover       = 13;
    static const quint16 ListEntitiesLight       = 15;
    static const quint16 ListEntitiesSensor      = 16;
    static const quint16 ListEntitiesSwitch      = 17;
    static const quint16 ListEntitiesTextSensor  = 18;
    static const quint16 ListEntitiesDone        = 19;
    static const quint16 SubscribeStates         = 20;
    static const quint16 StateBinary             = 21;
    static const quint16 StateCover              = 22;
    static const quint16 StateLight              = 24;
    static const quint16 StateSensor             = 25;
    static const quint16 StateSwitch             = 26;
    static const quint16 StateTextSensor         = 27;
    static const quint16 CoverCommand           = 30;
    static const quint16 LightCommand           = 32;
    static const quint16 SwitchCommand          = 33;
    static const quint16 ListEntitiesClimate     = 46;
    static const quint16 StateClimate            = 47;
    static const quint16 ClimateCommand         = 48;
    static const quint16 ListEntitiesNumber      = 49;
    static const quint16 StateNumber             = 50;
    static const quint16 NumberCommand          = 51;
    static const quint16 ListEntitiesSelect      = 52;
    static const quint16 StateSelect             = 53;
    static const quint16 SelectCommand          = 54;
    static const quint16 ListEntitiesLock        = 58;
    static const quint16 StateLock               = 59;
    static const quint16 LockCommand            = 60;
    static const quint16 ListEntitiesButton      = 61;
    static const quint16 ButtonCommand          = 62;
}

class EspHomeDevice : public QObject
{
    Q_OBJECT

public:

    enum class State { Disconnected, Connecting, ClientHello, ServerHello, NoiseHandshake, Ready, ListEntities, Subscribe };

    EspHomeDevice(const Device &device, DeviceList *devices, QObject *parent = nullptr);
    ~EspHomeDevice(void);

    void connectToDevice(void);
    void disconnectFromDevice(void);
    void sendCommand(DeviceObject *device, quint8 endpointId, const QString &action, const QVariant &value);

    inline Device device(void) { return m_device; }
    inline State state(void) { return m_state; }

signals:

    void entitiesDiscovered(DeviceObject *device);
    void stateChanged(DeviceObject *device, quint8 endpointId);
    void availabilityChanged(DeviceObject *device);
    void lastSeenUpdated(DeviceObject *device);

private slots:

    void onConnected(void);
    void onDisconnected(void);
    void onReadyRead(void);
    void onError(QAbstractSocket::SocketError error);
    void onPingTimer(void);
    void onReconnectTimer(void);
    void onHandshakeTimer(void);

private:

    Device m_device;
    DeviceList *m_devices; // shared list this connection's main device (and any sub-devices it discovers) live in -- see subDevice()
    QTcpSocket *m_socket;
    QTimer *m_pingTimer, *m_reconnectTimer, *m_handshakeTimer;
    State m_state;
    NoiseNNpsk0 *m_noise;
    QByteArray m_rxBuf; // accumulates TCP stream data
    quint8 m_noiseHandshakeState; // 0=waiting server hello, 1=waiting noise response
    QMap<quint32, QString> m_subDeviceNames; // ESPHome sub-device id -> name, from DeviceInfoResponse.devices

    // Pending entity list while discovering
    struct EntityInfo {
        quint32 key;
        quint32 deviceId; // 0 = main device; otherwise an ESPHome sub-device id (see m_subDeviceNames)
        QString type;
        QString name;
        QString icon;
        QString deviceClass;
        QString unit;
        QString stateClass;
        int accuracyDecimals;
        float minValue, maxValue, step;
        QStringList selectOptions;
        QStringList lightOptions;
        float minMireds, maxMireds;
        bool supportsPosition, supportsTilt; // cover
        QStringList climateModes, climateFanModes, climatePresets; // climate
        bool climateSupportsAction; // climate
        bool toggleCategory; // switch: entity_category == CONFIG or DIAGNOSTIC
    };
    QList<EntityInfo> m_pendingEntities;

    void sendRaw(const QByteArray &data);
    void sendMessage(quint16 type, const QByteArray &payload = QByteArray());

    void processMessage(quint16 type, const QByteArray &payload);
    void processEntityInfo(quint16 type, const QByteArray &payload);
    void processStateUpdate(quint16 type, const QByteArray &payload);
    void applyDiscoveredEntities(void);
    void applyEntitiesToDevice(const Device &device, const QList<EntityInfo> &entities);
    Device subDevice(quint32 deviceId);
    QList<Device> knownSubDevices(void) const;

    bool findEndpointByKey(quint32 key, Device &outDevice, quint8 &outEndpointId) const;
};

class EspHomeManager : public QObject
{
    Q_OBJECT

public:

    EspHomeManager(DeviceList *devices, QObject *parent = nullptr);

    void connectAll(void);
    void connectDevice(DeviceObject *device);
    void disconnectDevice(DeviceObject *device);
    void sendCommand(DeviceObject *device, quint8 endpointId, const QString &action, const QVariant &value);

signals:

    void entitiesDiscovered(DeviceObject *device);
    void stateChanged(DeviceObject *device, quint8 endpointId);
    void availabilityChanged(DeviceObject *device);
    void lastSeenUpdated(DeviceObject *device);

private:

    DeviceList *m_devices;
    QMap<QString, EspHomeDevice *> m_connections; // host → connection

};

#endif
