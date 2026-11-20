import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Who, what, and where in the corners, as radiology viewers lay it out,
// and the patient directions at the image's edges.
Item {
    id: overlay

    property var source
    // The 3D view, when the pane shows one.
    property var volume: null
    // Of four views, all but the first leave out who and where, which the
    // first already says.
    property bool compact: false
    property string hoverText
    property real zoom: 1
    // The measuring tool, as the slice view numbers them.
    property int tool: 0

    function n(value, decimals) {
        // Reads the language so the numbers re-render when it changes.
        const language = translator.language
        return language ? Number(value).toLocaleString(Qt.locale(), 'f', decimals) : ""
    }

    component Line: Label {
        property bool alignRight: false
        color: win.canvasText
        style: Text.Outline
        styleColor: "#b0000000"
        font.pixelSize: win.px(13)
        visible: text !== ""
        elide: Text.ElideRight
        Layout.maximumWidth: overlay.width * 0.45
        Layout.alignment: alignRight ? Qt.AlignRight : Qt.AlignLeft
    }

    ColumnLayout {
        anchors.top: parent.top
        anchors.left: parent.left
        spacing: 2
        visible: !overlay.compact
        Line { text: overlay.source.info.patient || ""; font.bold: true; font.pixelSize: win.px(15) }
        Line { text: overlay.source.info.patientDetails || ""; color: win.canvasDim }
        Line { text: overlay.source.info.study || "" }
        Line { text: overlay.source.info.date || ""; color: win.canvasDim }
    }

    ColumnLayout {
        anchors.top: parent.top
        anchors.right: parent.right
        spacing: 2
        Line { alignRight: true; text: overlay.compact ? "" : overlay.source.info.institution || ""; color: win.canvasDim }
        Line { alignRight: true; text: overlay.compact ? "" : overlay.source.info.series || ""; font.bold: true }
        Line { alignRight: true; text: overlay.compact ? "" : [overlay.source.info.modality, overlay.source.info.size].filter(x => x).join("  ·  ") }
        Line {
            alignRight: true
            text: overlay.volume ? qsTr("3D  ·  %1").arg(overlay.volume.lookName)
                : (overlay.source.playing ? "▶  " : "") + qsTr("Image %1 of %2").arg(overlay.n(overlay.source.sliceIndex + 1, 0)).arg(overlay.n(overlay.source.sliceCount, 0))
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
            text: overlay.tool === 1 ? qsTr("Ruler: drag across the image. Delete removes the last. Esc stops.")
                : overlay.tool === 2 ? qsTr("Area: drag a box around it. Delete removes the last. Esc stops.")
                : overlay.tool === 3 ? qsTr("Angle: drag one arm, then click the other's end. Esc stops.")
                : overlay.tool === 4 ? qsTr("Cobb angle: drag along one end plate, then along the other. Esc stops.")
                : ""
            color: theme.accent
        }
        Line { text: overlay.hoverText; font.family: "monospace" }
        Line { text: overlay.source.fusionName; color: theme.accent }
        Line { text: overlay.volume && overlay.volume.cut > 0 ? qsTr("Cut %1 mm into it").arg(overlay.n(overlay.volume.cut, 0)) : "" }
        Line {
            // A 3D look of its own doesn't follow the window; a surface and a MIP do.
            visible: text !== "" && (!overlay.volume || overlay.volume.followsWindow)
            text: overlay.source.color ? "" : qsTr("W %1  L %2").arg(overlay.n(overlay.source.windowWidth, 0)).arg(overlay.n(overlay.source.windowCenter, 0))
                  + (overlay.source.windowName ? "  ·  " + overlay.source.windowName : "")
                  + (overlay.source.colorMapName ? "  ·  " + overlay.source.colorMapName : "")
                  + (overlay.source.inverted ? "  ·  " + qsTr("Inverted") : "")
        }
        Line { text: Math.abs(overlay.zoom - 1) > 0.01 ? qsTr("Zoom %1%").arg(overlay.n(overlay.zoom * 100, 0)) : ""; color: win.canvasDim }
    }

    ColumnLayout {
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.bottomMargin: 6
        spacing: 2
        Line { alignRight: true; text: overlay.volume ? "" : overlay.source.info.geometry || ""; color: win.canvasDim }
    }

    // Directions belong to the image, which isn't mirrored.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        readonly property var edges: overlay.volume ? overlay.volume.edges : overlay.source.edges
        visible: edges.length === 4
        Line { anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; text: parent.edges[0] || ""; color: theme.accent; font.bold: true }
        Line { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: parent.edges[1] || ""; color: theme.accent; font.bold: true }
        Line { anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter; text: parent.edges[2] || ""; color: theme.accent; font.bold: true }
        Line { anchors.bottom: parent.bottom; anchors.bottomMargin: 6; anchors.horizontalCenter: parent.horizontalCenter; text: parent.edges[3] || ""; color: theme.accent; font.bold: true }
    }
}
