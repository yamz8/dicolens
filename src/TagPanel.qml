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
            onTextChanged: { backend.tagFilter = text; partner.tagFilter = text }
            Keys.onEscapePressed: panel.leaveSearch()
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: win.inUse.tags
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: row
                required property int depth
                required property string tag
                required property string name
                required property string vr
                required property string value
                // A sequence item's heading rather than an element.
                required property bool item
                width: list.width
                height: Math.max(win.px(26), valueLabel.implicitHeight + 8)

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12 + row.depth * 12
                    anchors.rightMargin: 14
                    spacing: 10

                    Label {
                        text: row.tag
                        visible: !row.item
                        color: win.muted
                        font.family: "monospace"
                        font.pixelSize: win.px(12)
                        Layout.preferredWidth: win.px(92)
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 4
                    }
                    Label {
                        text: row.item ? row.name
                              : (row.name || qsTr("Unknown")) + "  " + row.vr
                        color: row.item ? theme.accent : row.name ? win.secondary : win.muted
                        font.pixelSize: win.px(12)
                        font.bold: row.item
                        elide: Text.ElideRight
                        Layout.preferredWidth: row.item ? -1 : Math.max(win.px(110), list.width * 0.36 - row.depth * 12)
                        Layout.fillWidth: row.item
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 4
                    }
                    Label {
                        id: valueLabel
                        text: row.value
                        visible: !row.item
                        color: win.text
                        font.pixelSize: win.px(12)
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
