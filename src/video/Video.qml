import QtQuick
import QtMultimedia

Rectangle {
    id: root
    color: "black"
    property real gamma: 1.0
    property alias videoSink: video.videoSink
    property alias contentRect: video.contentRect
    signal shaderError()

    VideoOutput {
        id: video
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectFit
        layer.enabled: root.gamma !== 1.0
        layer.effect: ShaderEffect {
            property real gamma: root.gamma
            fragmentShader: "qrc:/player/video/gamma.frag.qsb"
            onStatusChanged: if (status === ShaderEffect.Error) root.shaderError()
        }
    }
}
