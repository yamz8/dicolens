import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A report that came with the scans, to read beside them: a structured
// report's findings as text, or a PDF's title with a way to open it.
Rectangle {
    id: panel
    color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.98)

    readonly property var report: backend.reports[backend.reportIndex] || ({})

    function scroll(amount) {
        flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, flick.contentY + amount))
    }

    // Clicks stay in the panel rather than reaching the image under it.
    MouseArea { anchors.fill: parent }

    ColumnLayout {
        id: header
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 32
        width: Math.min(parent.width - 64, 760)
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Label {
                text: backend.reportTitle
                color: win.text
                font.bold: true
                font.pixelSize: 20
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                // Reads the language so the numbers follow it.
                text: translator.language !== undefined && backend.reports.length > 1
                      ? qsTr("Report %1 of %2").arg(Number(backend.reportIndex + 1).toLocaleString(Qt.locale(), 'f', 0))
                                               .arg(Number(backend.reports.length).toLocaleString(Qt.locale(), 'f', 0))
                      : ""
                color: theme.accent
                font.bold: true
                visible: text !== ""
            }
        }
        Label {
            text: [panel.report.study, panel.report.date].filter(x => x).map(x => "\u2068" + x + "\u2069").join("  ·  ")
            color: win.muted
            visible: text !== ""
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 14
            Layout.preferredHeight: 1
            color: win.line
        }
    }

    Flickable {
        id: flick
        anchors.top: header.bottom
        anchors.bottom: footer.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 16
        anchors.bottomMargin: 12
        contentHeight: content.implicitHeight + 24
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        visible: !backend.reportIsPdf

        ColumnLayout {
            id: content
            x: (flick.width - width) / 2
            width: header.width
            spacing: 6

            Repeater {
                model: backend.reportLines
                delegate: Label {
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    Layout.leftMargin: modelData.depth * 20
                    Layout.topMargin: modelData.heading && index > 0 ? 14 : 0
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    font.bold: modelData.heading
                    font.pixelSize: modelData.heading ? 16 : 15
                    color: modelData.heading ? theme.accent : win.text
                    text: modelData.heading || !modelData.name ? (modelData.heading ? modelData.name : modelData.value)
                                                               : modelData.name + ": " + modelData.value
                }
            }
        }
    }

    // A PDF is read in the PDF viewer.
    ColumnLayout {
        anchors.centerIn: parent
        visible: backend.reportIsPdf
        spacing: 16
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("This report is a PDF.")
            color: win.text
            font.pixelSize: 16
        }
        Button {
            id: openButton
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Open in the PDF Viewer")
            highlighted: true
            Material.roundedScale: theme.radius
            Material.accent: theme.accent
            contentItem: Label {
                text: openButton.text
                color: theme.accentForeground
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: backend.openReport(backend.reportIndex)
        }
    }

    Label {
        id: footer
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 20
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: (backend.reports.length > 1 ? qsTr("← → Other reports") + "   ·   " : "")
              + (backend.reportIsPdf ? qsTr("Enter opens it") + "   ·   " : "")
              + qsTr("Esc closes")
        color: win.muted
    }
}
