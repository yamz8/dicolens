QT += core gui qml quick quickcontrols2 testlib dbus
CONFIG += c++20 testcase
TARGET = dicolens_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/backend.h ../src/filepicker.h ../src/theme.h ../src/translator.h
SOURCES += dicolens_tests.cpp ../src/backend.cpp ../src/dicom.cpp ../src/dictionary.cpp ../src/ljpeg.cpp \
    ../src/pixels.cpp ../src/study.cpp ../src/theme.cpp ../src/translator.cpp
LIBS += -lz
RESOURCES += ../src/resources.qrc
