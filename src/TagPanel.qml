import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every tag of the shown slice's file, sequences indented, searchable.
Rectangle {
    id: panel
    color: win.panel

    readonly property bool searching: search.activeFocus

    function focusSearch() {
        search.forceActiveFocus()
        search.selectAll()
    }

    function leaveSearch() {
        search.text = ""
        search.focus = false
        list.forceActiveFocus()
    }

    Rectangle {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        width: 1
        color: win.line
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 1
        spacing: 0

        TextField {
            id: search
            Layout.fillWidth: true
            Layout.margins: 12
            Material.roundedScale: theme.radius
            placeholderText: qsTr("Search tags")
            onTextChanged: backend.tagFilter = text
            Keys.onEscapePressed: panel.leaveSearch()
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: backend.tags
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: row
                required property var modelData
                width: list.width
                height: Math.max(26, value.implicitHeight + 8)

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12 + row.modelData.depth * 12
                    anchors.rightMargin: 14
                    spacing: 10

                    Label {
                        text: row.modelData.tag
                        visible: !row.modelData.item
                        color: win.muted
                        font.family: "monospace"
                        font.pixelSize: 12
                        Layout.preferredWidth: 92
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 4
                    }
                    Label {
                        text: row.modelData.item ? row.modelData.name
                              : (row.modelData.name || qsTr("Unknown")) + "  " + row.modelData.vr
                        color: row.modelData.item ? theme.accent : row.modelData.name ? "#c8c8d0" : win.muted
                        font.pixelSize: 12
                        font.bold: row.modelData.item
                        elide: Text.ElideRight
                        Layout.preferredWidth: row.modelData.item ? -1 : Math.max(110, list.width * 0.36 - row.modelData.depth * 12)
                        Layout.fillWidth: row.modelData.item
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 4
                    }
                    Label {
                        id: value
                        text: row.modelData.value
                        visible: !row.modelData.item
                        color: win.text
                        font.pixelSize: 12
                        wrapMode: Text.WrapAnywhere
                        maximumLineCount: 4
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 4
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: list.count === 0 && search.text !== ""
                text: qsTr("No tags match")
                color: win.muted
            }
        }
    }
}
