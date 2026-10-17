import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Before anything is open: what Dicolens opens, and how.
ColumnLayout {
    spacing: 10

    Label {
        Layout.alignment: Qt.AlignHCenter
        text: qsTr("Open a scan")
        color: win.text
        font.pixelSize: 26
        font.bold: true
    }
    Label {
        Layout.alignment: Qt.AlignHCenter
        Layout.maximumWidth: 460
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: qsTr("A DICOM file, a folder of them, or a hospital CD. Drop it here, or:")
        color: win.muted
        font.pixelSize: 15
    }
    RowLayout {
        Layout.alignment: Qt.AlignHCenter
        Layout.topMargin: 14
        spacing: 12
        Button {
            text: qsTr("Open Files") + "   O"
            highlighted: true
            Material.roundedScale: theme.radius
            Material.background: theme.accent
            Material.foreground: theme.accentForeground
            onClicked: backend.open()
        }
        Button {
            text: qsTr("Open Folder") + "   Shift+O"
            flat: true
            Material.roundedScale: theme.radius
            onClicked: backend.openFolder()
        }
    }
    Label {
        Layout.alignment: Qt.AlignHCenter
        Layout.topMargin: 18
        text: qsTr("? shows every shortcut")
        color: win.muted
        font.pixelSize: 13
    }
}
