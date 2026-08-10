TEMPLATE = app
TARGET = esphome-debug-client

CONFIG += c++17 console
CONFIG -= app_bundle
QT = core network

SOURCES += \
    debug-client.cpp \
    ../noise.cpp \
    ../proto.cpp

HEADERS += \
    ../noise.h \
    ../proto.h

# noise-c (esphome-libs/noise-c, esp-port branch) -- vendored, reference backend
# only, no external crypto dependency. Kept in sync with ../homed-esphome.pro.
NOISE_C_DIR = ../noise-c

INCLUDEPATH += $$NOISE_C_DIR/include $$NOISE_C_DIR/src

DEFINES += \
    NOISE_USE_REFERENCE_BACKEND=1 \
    NOISE_USE_LIBSODIUM=0 \
    NOISE_USE_OPENSSL=0 \
    NOISE_USE_CUSTOM_RAND=0

SOURCES += \
    $$NOISE_C_DIR/src/backend/ref/cipher-chachapoly.c \
    $$NOISE_C_DIR/src/backend/ref/dh-curve25519.c \
    $$NOISE_C_DIR/src/backend/ref/hash-sha256.c \
    $$NOISE_C_DIR/src/crypto/chacha/chacha.c \
    $$NOISE_C_DIR/src/crypto/donna/poly1305-donna.c \
    $$NOISE_C_DIR/src/crypto/sha2/sha256.c \
    $$NOISE_C_DIR/src/crypto/x25519/x25519.c \
    $$NOISE_C_DIR/src/protocol/cipherstate.c \
    $$NOISE_C_DIR/src/protocol/dhstate.c \
    $$NOISE_C_DIR/src/protocol/errors.c \
    $$NOISE_C_DIR/src/protocol/handshakestate.c \
    $$NOISE_C_DIR/src/protocol/hashstate.c \
    $$NOISE_C_DIR/src/protocol/internal.c \
    $$NOISE_C_DIR/src/protocol/names.c \
    $$NOISE_C_DIR/src/protocol/patterns.c \
    $$NOISE_C_DIR/src/protocol/rand_os.c \
    $$NOISE_C_DIR/src/protocol/randstate.c \
    $$NOISE_C_DIR/src/protocol/signstate.c \
    $$NOISE_C_DIR/src/protocol/symmetricstate.c \
    $$NOISE_C_DIR/src/protocol/util.c
