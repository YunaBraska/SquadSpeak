pragma Singleton
import QtQuick

QtObject {
    property string mode: "system"
    property string paletteName: "plum"
    readonly property bool dark: mode === "dark" || (mode === "system" && Qt.styleHints.colorScheme === Qt.Dark)
    readonly property int controlRadius: 6
    readonly property int smallRadius: 3
    readonly property int panelRadius: 8
    // Each palette has the same contrast roles in both appearances.
    readonly property var colors: ({
        plum: {
            dark: ["#211e27", "#29242f", "#393044", "#19171f", "#675972", "#eee8f5", "#b2a6c0", "#b69ae0", "#21152f"],
            light: ["#f6f3f8", "#fffcff", "#eae4f2", "#ffffff", "#aaa0b6", "#342c42", "#74677f", "#7958b5", "#ffffff"]
        },
        ocean: {
            dark: ["#17232b", "#1d2c36", "#293f4c", "#111d25", "#526f80", "#e3f1f8", "#a0b9c8", "#7fc5e5", "#092633"],
            light: ["#f0f6f9", "#fbfdff", "#dfedf4", "#ffffff", "#96aebb", "#1b3645", "#536f80", "#176d94", "#ffffff"]
        },
        forest: {
            dark: ["#1b2521", "#22312a", "#314439", "#141d18", "#5c7665", "#e6f1e8", "#a7c0ae", "#9bcea6", "#142b1b"],
            light: ["#f2f6f1", "#fcfefb", "#e1eddf", "#ffffff", "#9aae9c", "#293e2c", "#607664", "#40734b", "#ffffff"]
        },
        graphite: {
            dark: ["#202124", "#292a2e", "#393b40", "#17181b", "#6a6d75", "#f0f1f4", "#b3b7c0", "#b9c5dd", "#202937"],
            light: ["#f4f5f7", "#ffffff", "#e7e9ef", "#ffffff", "#a4a9b5", "#282d37", "#676f7f", "#536784", "#ffffff"]
        }
    })
    readonly property var tones: (colors[paletteName] || colors.plum)[dark ? "dark" : "light"]
    readonly property color background: tones[0]
    readonly property color surface: tones[1]
    readonly property color raised: tones[2]
    readonly property color input: tones[3]
    readonly property color border: tones[4]
    readonly property color text: tones[5]
    readonly property color muted: tones[6]
    readonly property color accent: tones[7]
    readonly property color accentText: tones[8]
    readonly property color warning: dark ? "#e4bb77" : "#896018"
    readonly property color danger: dark ? "#f295ad" : "#b74462"
}
