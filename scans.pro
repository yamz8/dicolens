QT += core gui qml quick quickcontrols2 dbus

CONFIG += c++20 release
TARGET = scans
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/cli.h \
    src/dicom.h \
    src/filepicker.h \
    src/ljpeg.h \
    src/pixels.h \
    src/portalfilepicker.h \
    src/sliceview.h \
    src/study.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/cli.cpp \
    src/dicom.cpp \
    src/dictionary.cpp \
    src/ljpeg.cpp \
    src/pixels.cpp \
    src/portalfilepicker.cpp \
    src/sliceview.cpp \
    src/study.cpp \
    src/theme.cpp \
    src/translator.cpp

LIBS += -lz

RESOURCES += src/resources.qrc
