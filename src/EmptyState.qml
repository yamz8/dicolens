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
        font.pixelSize: win.px(26)
        font.bold: true
    }
    Label {
        Layout.alignment: Qt.AlignHCenter
        Layout.maximumWidth: 460
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: qsTr("A DICOM file, a folder of them, or a hospital CD. Drop it here, or:")
        color: win.muted
        font.pixelSize: win.px(15)
    }
    RowLayout {
        Layout.alignment: Qt.AlignHCenter
        Layout.topMargin: 14
        spacing: 12
        Button {
            id: openButton
            objectName: "openFiles"
            text: qsTr("Open Files") + "   O"
            highlighted: true
            // Keys go through the window's shortcuts, so O and Space mean
            // the same wherever the pointer last was.
            focusPolicy: Qt.NoFocus
            Material.roundedScale: theme.radius
            Material.accent: theme.accent
            onClicked: backend.open()
            // Material draws a highlighted button through a shadow effect,
            // which the software renderer leaves out, background and all:
            // in the looks, and wherever that effect is missing, the main
            // action would be missing or unreadable on the accent. So it's
            // drawn plainly here.
            background: Rectangle {
                implicitWidth: 64
                implicitHeight: openButton.Material.buttonHeight
                radius: theme.radius
                color: theme.accent
                opacity: openButton.down ? 0.8 : 1
            }
            // A highlighted button colors its caption by Material's own
            // rule, not Material.foreground, so it can't read on every accent.
            contentItem: Label {
                text: openButton.text
                font: openButton.font
                color: theme.accentForeground
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
        Button {
            text: qsTr("Open Folder") + "   Shift+O"
            flat: true
            focusPolicy: Qt.NoFocus
            Material.roundedScale: theme.radius
            onClicked: backend.openFolder()
        }
    }
    Label {
        Layout.alignment: Qt.AlignHCenter
        Layout.topMargin: 18
        text: qsTr("? shows every shortcut")
        color: win.muted
        font.pixelSize: win.px(13)
    }
}
