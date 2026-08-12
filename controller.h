#ifndef CONTROLLER_H
#define CONTROLLER_H

#define UPDATE_PROPERTIES_DELAY     1000

#include <QMetaEnum>
#include "homed.h"
#include "esphome.h"

class Controller : public HOMEd
{
    Q_OBJECT

public:

    enum class Command
    {
        restartService,
        addDevice,
        removeDevice,
        updateDevice,
        getProperties
    };

    Controller(const QString &configFile);

    Q_ENUM(Command)

private:

    QTimer *m_propertiesTimer;
    DeviceList *m_devices;
    EspHomeManager *m_esphome;

    QMetaEnum m_commands;
    QString m_haPrefix, m_haStatus;
    bool m_haEnabled, m_haUpdate;

    void publishExposes(DeviceObject *device, bool remove = false);
    void publishHaDiscovery(DeviceObject *device, const QString &devTopic, bool remove);
    void publishStatus(void);
    void publishDeviceState(DeviceObject *device);
    void clearStaleTopics(DeviceObject *device);
    QString deviceTopic(DeviceObject *device);
    quint8 endpointForAction(DeviceObject *device, const QString &action);

public slots:

    void quit(void) override;

private slots:

    void mqttConnected(void) override;
    void mqttReceived(const QByteArray &message, const QMqttTopicName &topic) override;

    void updateProperties(void);
    void publishDeviceStatus(DeviceObject *device);
    void entitiesDiscovered(DeviceObject *device);
    void stateChanged(DeviceObject *device, quint8 endpointId);
    void availabilityChanged(DeviceObject *device);

};

#endif
