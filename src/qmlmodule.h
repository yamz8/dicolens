#pragma once

#include <QtGlobal>

// The views and the backend are the Dicolens QML module, and register
// themselves when QML imports it, from code the build generates into the
// core library. A static library leaves out what nothing refers to, so
// each program that loads the QML refers to it here, as Qt's own plugins do.
inline void keepQmlModule() {
    extern void qml_register_types_Dicolens();
    volatile auto keep = &qml_register_types_Dicolens;
    Q_UNUSED(keep);
}
