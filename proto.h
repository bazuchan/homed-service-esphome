#ifndef PROTO_H
#define PROTO_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariant>

// Minimal protobuf encoder/decoder for ESPHome API messages

class ProtoEncoder
{

public:

    ProtoEncoder(void) {}

    void addVarint(int field, quint64 value);
    void addFixed32(int field, quint32 value);
    void addFloat(int field, float value);
    void addBool(int field, bool value);
    void addString(int field, const QString &value);
    void addBytes(int field, const QByteArray &value);

    QByteArray data(void) const { return m_data; }

private:

    QByteArray m_data;

    void writeTag(int field, int wireType);
    void writeVarint(quint64 value);

};

class ProtoField
{

public:

    ProtoField(int field, int wireType, quint64 varintValue, float floatValue, QByteArray rawValue)
        : m_field(field), m_wireType(wireType), m_varint(varintValue), m_float(floatValue), m_raw(rawValue) {}

    int field(void) const { return m_field; }
    int wireType(void) const { return m_wireType; }
    quint64 varint(void) const { return m_varint; }
    bool boolean(void) const { return m_varint != 0; }
    float floatVal(void) const { return m_float; }
    quint32 fixed32(void) const { return static_cast<quint32>(m_varint); }
    QString string(void) const { return QString::fromUtf8(m_raw); }
    QByteArray bytes(void) const { return m_raw; }

private:

    int m_field, m_wireType;
    quint64 m_varint;
    float m_float;
    QByteArray m_raw;

};

class ProtoDecoder
{

public:

    ProtoDecoder(const QByteArray &data) : m_data(data), m_pos(0) {}

    bool hasMore(void) const { return m_pos < m_data.size(); }
    bool readField(ProtoField &field);

    // Helper: decode all fields from data
    static QList<ProtoField> decode(const QByteArray &data);

private:

    QByteArray m_data;
    int m_pos;

    bool readVarint(quint64 &value);

};

#endif
