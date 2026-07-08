#include "proto.h"
#include <cstring>

// ==================== Encoder ====================

void ProtoEncoder::writeTag(int field, int wireType)
{
    writeVarint(static_cast<quint64>((field << 3) | wireType));
}

void ProtoEncoder::writeVarint(quint64 value)
{
    while (value >= 0x80)
    {
        m_data.append(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    m_data.append(static_cast<char>(value & 0x7F));
}

void ProtoEncoder::addVarint(int field, quint64 value)
{
    writeTag(field, 0);
    writeVarint(value);
}

void ProtoEncoder::addFixed32(int field, quint32 value)
{
    writeTag(field, 5);
    m_data.append(static_cast<char>(value & 0xFF));
    m_data.append(static_cast<char>((value >> 8) & 0xFF));
    m_data.append(static_cast<char>((value >> 16) & 0xFF));
    m_data.append(static_cast<char>((value >> 24) & 0xFF));
}

void ProtoEncoder::addFloat(int field, float value)
{
    quint32 bits;
    memcpy(&bits, &value, 4);
    addFixed32(field, bits);
}

void ProtoEncoder::addBool(int field, bool value)
{
    addVarint(field, value ? 1 : 0);
}

void ProtoEncoder::addString(int field, const QString &value)
{
    addBytes(field, value.toUtf8());
}

void ProtoEncoder::addBytes(int field, const QByteArray &value)
{
    writeTag(field, 2);
    writeVarint(static_cast<quint64>(value.size()));
    m_data.append(value);
}

// ==================== Decoder ====================

bool ProtoDecoder::readVarint(quint64 &value)
{
    value = 0;
    int shift = 0;
    while (m_pos < m_data.size())
    {
        quint8 b = static_cast<quint8>(m_data.at(m_pos++));
        value |= (static_cast<quint64>(b & 0x7F) << shift);
        if (!(b & 0x80))
            return true;
        shift += 7;
        if (shift >= 64)
            return false;
    }
    return false;
}

bool ProtoDecoder::readField(ProtoField &result)
{
    if (m_pos >= m_data.size())
        return false;

    quint64 tag;
    if (!readVarint(tag))
        return false;

    int field = static_cast<int>(tag >> 3);
    int wireType = static_cast<int>(tag & 0x07);

    quint64 varintValue = 0;
    float floatValue = 0.0f;
    QByteArray rawValue;

    switch (wireType)
    {
        case 0: // varint
            if (!readVarint(varintValue))
                return false;
            break;

        case 1: // 64-bit
            if (m_pos + 8 > m_data.size())
                return false;
            m_pos += 8;
            break;

        case 2: // length-delimited
        {
            quint64 len;
            if (!readVarint(len))
                return false;
            if (m_pos + static_cast<int>(len) > m_data.size())
                return false;
            rawValue = m_data.mid(m_pos, static_cast<int>(len));
            m_pos += static_cast<int>(len);
            break;
        }

        case 5: // 32-bit
        {
            if (m_pos + 4 > m_data.size())
                return false;
            quint32 bits = static_cast<quint8>(m_data.at(m_pos))
                         | (static_cast<quint8>(m_data.at(m_pos + 1)) << 8)
                         | (static_cast<quint8>(m_data.at(m_pos + 2)) << 16)
                         | (static_cast<quint8>(m_data.at(m_pos + 3)) << 24);
            memcpy(&floatValue, &bits, 4);
            varintValue = bits;
            m_pos += 4;
            break;
        }

        default:
            return false;
    }

    result = ProtoField(field, wireType, varintValue, floatValue, rawValue);
    return true;
}

QList<ProtoField> ProtoDecoder::decode(const QByteArray &data)
{
    ProtoDecoder dec(data);
    QList<ProtoField> fields;
    ProtoField f(0, 0, 0, 0.0f, QByteArray());
    while (dec.hasMore() && dec.readField(f))
        fields.append(f);
    return fields;
}
