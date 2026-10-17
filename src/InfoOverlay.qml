import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Who, what, and where in the corners, as radiology viewers lay it out,
// and the patient directions at the image's edges.
Item {
    id: overlay

    property string hoverText
    property real zoom: 1
    property bool measuring: false

    function n(value, decimals) {
        // Reads the language so the numbers re-render when it changes.
        return translator.language, Number(value).toLocaleString(Qt.locale(), 'f', decimals)
    }

    component Line: Label {
        color: win.text
        style: Text.Outline
        styleColor: "#b0000000"
        font.pixelSize: 13
        visible: text !== ""
        elide: Text.ElideRight
        Layout.maximumWidth: overlay.width * 0.45
        Layout.alignment: parent && parent.alignRight ? Qt.AlignRight : Qt.AlignLeft
    }

    ColumnLayout {
        anchors.top: parent.top
        anchors.left: parent.left
        spacing: 2
        Line { text: backend.info.patient || ""; font.bold: true; font.pixelSize: 15 }
        Line { text: backend.info.patientDetails || ""; color: "#c8c8d0" }
        Line { text: backend.info.study || "" }
        Line { text: backend.info.date || ""; color: "#c8c8d0" }
    }

    ColumnLayout {
        readonly property bool alignRight: true
        anchors.top: parent.top
        anchors.right: parent.right
        spacing: 2
        Line { text: backend.info.institution || ""; color: "#c8c8d0" }
        Line { text: backend.info.series || ""; font.bold: true }
        Line { text: [backend.info.modality, backend.info.size].filter(x => x).join("  ·  ") }
        Line {
            text: (backend.playing ? "▶  " : "") + qsTr("Image %1 of %2").arg(overlay.n(backend.sliceIndex + 1, 0)).arg(overlay.n(backend.sliceCount, 0))
            color: theme.accent
            font.bold: true
        }
    }

    ColumnLayout {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.bottomMargin: 6
        spacing: 2
        Line {
            text: overlay.measuring ? qsTr("Measuring: drag across the image. M or Esc stops.") : ""
            color: theme.accent
        }
        Line { text: overlay.hoverText; font.family: "monospace" }
        Line {
            text: backend.color ? "" : qsTr("W %1  L %2").arg(overlay.n(backend.windowWidth, 0)).arg(overlay.n(backend.windowCenter, 0))
                  + (backend.windowName ? "  ·  " + backend.windowName : "")
                  + (backend.inverted ? "  ·  " + qsTr("Inverted") : "")
        }
        Line { text: Math.abs(overlay.zoom - 1) > 0.01 ? qsTr("Zoom %1%").arg(overlay.n(overlay.zoom * 100, 0)) : ""; color: "#c8c8d0" }
    }

    ColumnLayout {
        readonly property bool alignRight: true
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.bottomMargin: 6
        spacing: 2
        Line { text: backend.info.geometry || ""; color: "#c8c8d0" }
    }

    // Directions belong to the image, which isn't mirrored.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: false
        visible: backend.edges.length === 4
        Line { anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; text: backend.edges[0] || ""; color: theme.accent; font.bold: true }
        Line { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: backend.edges[1] || ""; color: theme.accent; font.bold: true }
        Line { anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter; text: backend.edges[2] || ""; color: theme.accent; font.bold: true }
        Line { anchors.bottom: parent.bottom; anchors.bottomMargin: 6; anchors.horizontalCenter: parent.horizontalCenter; text: backend.edges[3] || ""; color: theme.accent; font.bold: true }
    }
}
