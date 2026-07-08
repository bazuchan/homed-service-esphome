include(../homed-common/homed-common.pri)
include(../homed-common/homed-endpoint.pri)
include(../homed-common/homed-parser.pri)

HEADERS += \
    controller.h \
    device.h \
    esphome.h \
    noise.h \
    proto.h

SOURCES += \
    controller.cpp \
    device.cpp \
    esphome.cpp \
    noise.cpp \
    proto.cpp

QT += network

LIBS += -lcrypto -lssl

QMAKE_CXXFLAGS += -Wno-deprecated-declarations
