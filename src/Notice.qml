import QtQuick
import QtQuick.Controls

// A moment's word that something worked: saved, copied.
Rectangle {
    width: label.implicitWidth + 32
    height: label.implicitHeight + 18
    radius: Math.min(4, theme.radius)
    color: "#e8202027"
    border.color: win.line
    opacity: backend.notice !== "" ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: 160 } }

    Label {
        id: label
        anchors.centerIn: parent
        text: backend.notice
        color: win.text
    }
}
