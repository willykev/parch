// Adapted from sprawl's src/Icon.qml (MIT, copyright 2026 willykev).
import QtQuick
import QtQuick.Shapes

// A line icon, drawn from a path on a 24 unit grid in whatever colour it is
// given, so it follows the theme as text does.
Item {
    id: icon

    property string path
    property color color: theme.foreground
    property real size: 16

    implicitWidth: size
    implicitHeight: size

    Shape {
        anchors.centerIn: parent
        width: 24
        height: 24
        scale: icon.size / 24
        // Curves drawn on the GPU: smooth at any size with no multisampling.
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: icon.color
            strokeWidth: 2
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: icon.path }
        }
    }
}
