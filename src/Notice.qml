import QtQuick
import QtQuick.Controls

// A moment's word that something worked: saved, copied.
Rectangle {
    id: notice
    property var source
    width: label.implicitWidth + 32
    height: label.implicitHeight + 18
    radius: Math.min(4, theme.radius)
    color: win.panel
    border.color: win.line
    opacity: notice.source.notice !== "" ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: 160 } }

    Label {
        id: label
        anchors.centerIn: parent
        text: notice.source.notice
        color: win.text
    }
}
