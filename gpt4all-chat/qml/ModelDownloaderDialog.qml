import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import chatlistmodel
import download
import llm
import modellist
import network

Dialog {
    id: modelDownloaderDialog
    modal: true
    opacity: 0.9
    closePolicy: ModelList.count === 0 ? Popup.NoAutoClose : (Popup.CloseOnEscape | Popup.CloseOnPressOutside)
    padding: 20
    bottomPadding: 30
    background: Rectangle {
        anchors.fill: parent
        color: theme.backgroundDarkest
        border.width: 1
        border.color: theme.dialogBorder
        radius: 10
    }

    onOpened: {
        Network.sendModelDownloaderDialog();
    }

    property string defaultModelPath: ModelList.defaultLocalModelsPath()
    property alias modelPath: settings.modelPath
    Settings {
        id: settings
        property string modelPath: modelDownloaderDialog.defaultModelPath
    }

    Component.onCompleted: {
        ModelList.localModelsPath = settings.modelPath
    }

    Component.onDestruction: {
        settings.sync()
    }

    PopupDialog {
        id: downloadingErrorPopup
        anchors.centerIn: parent
        shouldTimeOut: false
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 30

        Label {
            id: listLabel
            text: qsTr("Available Models:")
            Layout.alignment: Qt.AlignLeft
            Layout.fillWidth: true
            color: theme.textColor
        }

        Label {
            visible: !ModelList.downloadableModels.count
            Layout.fillWidth: true
            Layout.fillHeight: true
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
            text: qsTr("Network error: could not retrieve http://gpt4all.io/models/models.json")
            color: theme.mutedTextColor
        }

        ScrollView {
            id: scrollView
            ScrollBar.vertical.policy: ScrollBar.AlwaysOn
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ListView {
                id: modelListView
                model: ModelList.downloadableModels
                boundsBehavior: Flickable.StopAtBounds

                delegate: Item {
                    id: delegateItem
                    width: modelListView.width
                    height: modelNameText.height + modelNameText.padding
                        + descriptionText.height + descriptionText.padding
                    Rectangle {
                        anchors.fill: parent
                        color: index % 2 === 0 ? theme.backgroundLight : theme.backgroundLighter
                    }

                    Text {
                        id: modelNameText
                        text: name !== "" ? name : filename
                        padding: 20
                        anchors.top: parent.top
                        anchors.left: parent.left
                        font.bold: isDefault
                        color: theme.assistantColor
                        Accessible.role: Accessible.Paragraph
                        Accessible.name: qsTr("Model file")
                        Accessible.description: qsTr("Model file to be downloaded")
                    }

                    Text {
                        id: descriptionText
                        text: "    - " + description
                        leftPadding: 20
                        rightPadding: 20
                        anchors.top: modelNameText.bottom
                        anchors.left: modelNameText.left
                        anchors.right: parent.right
                        wrapMode: Text.WordWrap
                        textFormat: Text.StyledText
                        color: theme.textColor
                        linkColor: theme.textColor
                        Accessible.role: Accessible.Paragraph
                        Accessible.name: qsTr("Description")
                        Accessible.description: qsTr("The description of the file")
                        onLinkActivated: Qt.openUrlExternally(link)
                    }

                    Text {
                        id: isDefaultText
                        text: qsTr("(default)")
                        visible: isDefault
                        anchors.top: modelNameText.top
                        anchors.left: modelNameText.right
                        padding: 20
                        color: theme.textColor
                        Accessible.role: Accessible.Paragraph
                        Accessible.name: qsTr("Default file")
                        Accessible.description: qsTr("Whether the file is the default model")
                    }

                    Text {
                        text: filesize
                        anchors.top: modelNameText.top
                        anchors.left: isDefaultText.visible ? isDefaultText.right : modelNameText.right
                        padding: 20
                        color: theme.textColor
                        Accessible.role: Accessible.Paragraph
                        Accessible.name: qsTr("File size")
                        Accessible.description: qsTr("The size of the file")
                    }

                    Label {
                        id: speedLabel
                        anchors.top: modelNameText.top
                        anchors.right: itemProgressBar.left
                        padding: 20
                        color: theme.textColor
                        text: speed
                        visible: isDownloading
                        Accessible.role: Accessible.Paragraph
                        Accessible.name: qsTr("Download speed")
                        Accessible.description: qsTr("Download speed in bytes/kilobytes/megabytes per second")
                    }

                    ProgressBar {
                        id: itemProgressBar
                        anchors.top: modelNameText.top
                        anchors.right: downloadButton.left
                        anchors.topMargin: 20
                        anchors.rightMargin: 20
                        width: 100
                        visible: isDownloading
                        value: bytesReceived / bytesTotal
                        background: Rectangle {
                            implicitWidth: 200
                            implicitHeight: 30
                            color: theme.backgroundDarkest
                            radius: 3
                        }

                        contentItem: Item {
                            implicitWidth: 200
                            implicitHeight: 25

                            Rectangle {
                                width: itemProgressBar.visualPosition * parent.width
                                height: parent.height
                                radius: 2
                                color: theme.assistantColor
                            }
                        }
                        Accessible.role: Accessible.ProgressBar
                        Accessible.name: qsTr("Download progressBar")
                        Accessible.description: qsTr("Shows the progress made in the download")
                    }

                    Item {
                        visible: calcHash
                        anchors.top: modelNameText.top
                        anchors.right: parent.right

                        Label {
                            id: calcHashLabel
                            anchors.right: busyCalcHash.left
                            padding: 20
                            color: theme.textColor
                            text: qsTr("Calculating MD5...")
                            Accessible.role: Accessible.Paragraph
                            Accessible.name: text
                            Accessible.description: qsTr("Whether the file hash is being calculated")
                        }

                        MyBusyIndicator {
                            id: busyCalcHash
                            anchors.right: parent.right
                            padding: 20
                            running: calcHash
                            Accessible.role: Accessible.Animation
                            Accessible.name: qsTr("Busy indicator")
                            Accessible.description: qsTr("Displayed when the file hash is being calculated")
                        }
                    }

                    Item {
                        anchors.top: modelNameText.top
                        anchors.topMargin: 15
                        anchors.right: parent.right
                        visible: installed || downloadError !== ""

                        Label {
                            anchors.verticalCenter: removeButton.verticalCenter
                            anchors.right: removeButton.left
                            anchors.rightMargin: 15
                            color: theme.textColor
                            textFormat: Text.StyledText
                            text: installed ? qsTr("Already installed") : qsTr("Downloading <a href=\"#error
                            \">error</a>")
                            Accessible.role: Accessible.Paragraph
                            Accessible.name: text
                            Accessible.description: qsTr("Whether the file is already installed on your system")
                            onLinkActivated: {
                                downloadingErrorPopup.text = downloadError;
                                downloadingErrorPopup.open();
                            }
                        }

                        MyButton {
                            id: removeButton
                            text: "Remove"
                            anchors.right: parent.right
                            anchors.rightMargin: 20
                            Accessible.description: qsTr("Remove button to remove model from filesystem")
                            onClicked: {
                                Download.removeModel(filename);
                            }
                        }
                    }

                    Item {
                        visible: isChatGPT && !installed
                        anchors.top: modelNameText.top
                        anchors.topMargin: 15
                        anchors.right: parent.right

                        TextField {
                            id: openaiKey
                            anchors.right: installButton.left
                            anchors.rightMargin: 15
                            color: theme.textColor
                            background: Rectangle {
                                color: theme.backgroundLighter
                                radius: 10
                            }
                            placeholderText: qsTr("enter $OPENAI_API_KEY")
                            placeholderTextColor: theme.backgroundLightest
                            Accessible.role: Accessible.EditableText
                            Accessible.name: placeholderText
                            Accessible.description: qsTr("Whether the file hash is being calculated")
                        }

                        Button {
                            id: installButton
                            contentItem: Text {
                                color: openaiKey.text === "" ? theme.backgroundLightest : theme.textColor
                                text: "Install"
                            }
                            enabled: openaiKey.text !== ""
                            anchors.right: parent.right
                            anchors.rightMargin: 20
                            background: Rectangle {
                                opacity: .5
                                border.color: theme.backgroundLightest
                                border.width: 1
                                radius: 10
                                color: theme.backgroundLight
                            }
                            onClicked: {
                                Download.installModel(filename, openaiKey.text);
                            }
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Install button")
                            Accessible.description: qsTr("Install button to install chatgpt model")
                        }
                    }

                    MyButton {
                        id: downloadButton
                        text: isDownloading ? qsTr("Cancel") : isIncomplete ? qsTr("Resume") : qsTr("Download")
                        anchors.top: modelNameText.top
                        anchors.right: parent.right
                        anchors.topMargin: 15
                        anchors.rightMargin: 20
                        visible: !isChatGPT && !installed && !calcHash && downloadError === ""
                        Accessible.description: qsTr("Cancel/Resume/Download button to stop/restart/start the download")
                        onClicked: {
                            if (!isDownloading) {
                                Download.downloadModel(filename);
                            } else {
                                Download.cancelDownload(filename);
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignCenter
            Layout.fillWidth: true
            spacing: 20
            FolderDialog {
                id: modelPathDialog
                title: "Please choose a directory"
                currentFolder: "file://" + ModelList.localModelsPath
                onAccepted: {
                    modelPathDisplayField.text = selectedFolder
                    ModelList.localModelsPath = modelPathDisplayField.text
                    settings.modelPath = ModelList.localModelsPath
                    settings.sync()
                }
            }
            Label {
                id: modelPathLabel
                text: qsTr("Download path:")
                color: theme.textColor
                Layout.row: 1
                Layout.column: 0
            }
            MyDirectoryField {
                id: modelPathDisplayField
                text: ModelList.localModelsPath
                Layout.fillWidth: true
                ToolTip.text: qsTr("Path where model files will be downloaded to")
                ToolTip.visible: hovered
                Accessible.role: Accessible.ToolTip
                Accessible.name: modelPathDisplayField.text
                Accessible.description: ToolTip.text
                onEditingFinished: {
                    if (isValid) {
                        ModelList.localModelsPath = modelPathDisplayField.text
                        settings.modelPath = ModelList.localModelsPath
                        settings.sync()
                    } else {
                        text = ModelList.localModelsPath
                    }
                }
            }
            MyButton {
                text: qsTr("Browse")
                Accessible.description: qsTr("Opens a folder picker dialog to choose where to save model files")
                onClicked: modelPathDialog.open()
            }
        }
    }
}
