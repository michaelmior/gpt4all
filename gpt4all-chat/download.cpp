#include "download.h"
#include "network.h"
#include "modellist.h"
#include "llm.h"

#include <QCoreApplication>
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QDir>
#include <QStandardPaths>
#include <QSettings>

class MyDownload: public Download { };
Q_GLOBAL_STATIC(MyDownload, downloadInstance)
Download *Download::globalInstance()
{
    return downloadInstance();
}

Download::Download()
    : QObject(nullptr)
    , m_hashAndSave(new HashAndSaveFile)
{
    connect(this, &Download::requestHashAndSave, m_hashAndSave,
        &HashAndSaveFile::hashAndSave, Qt::QueuedConnection);
    connect(m_hashAndSave, &HashAndSaveFile::hashAndSaveFinished, this,
        &Download::handleHashAndSaveFinished, Qt::QueuedConnection);
    connect(&m_networkManager, &QNetworkAccessManager::sslErrors, this,
        &Download::handleSslErrors);
    connect(ModelList::globalInstance(), &ModelList::localModelsPathChanged, this, &Download::updateModelList);
    updateModelList();
    updateReleaseNotes();
    m_startTime = QDateTime::currentDateTime();
}

bool operator==(const ModelInfo& lhs, const ModelInfo& rhs) {
    return lhs.filename == rhs.filename && lhs.md5sum == rhs.md5sum;
}

bool operator==(const ReleaseInfo& lhs, const ReleaseInfo& rhs) {
    return lhs.version == rhs.version;
}

bool compareVersions(const QString &a, const QString &b) {
    QStringList aParts = a.split('.');
    QStringList bParts = b.split('.');

    for (int i = 0; i < std::min(aParts.size(), bParts.size()); ++i) {
        int aInt = aParts[i].toInt();
        int bInt = bParts[i].toInt();

        if (aInt > bInt) {
            return true;
        } else if (aInt < bInt) {
            return false;
        }
    }

    return aParts.size() > bParts.size();
}

ReleaseInfo Download::releaseInfo() const
{
    const QString currentVersion = QCoreApplication::applicationVersion();
    if (m_releaseMap.contains(currentVersion))
        return m_releaseMap.value(currentVersion);
    return ReleaseInfo();
}

bool Download::hasNewerRelease() const
{
    const QString currentVersion = QCoreApplication::applicationVersion();
    QList<QString> versions = m_releaseMap.keys();
    std::sort(versions.begin(), versions.end(), compareVersions);
    if (versions.isEmpty())
        return false;
    return compareVersions(versions.first(), currentVersion);
}

bool Download::isFirstStart() const
{
    QSettings settings;
    settings.sync();
    QString lastVersionStarted = settings.value("download/lastVersionStarted").toString();
    bool first = lastVersionStarted != QCoreApplication::applicationVersion();
    settings.setValue("download/lastVersionStarted", QCoreApplication::applicationVersion());
    settings.sync();
    return first;
}

void Download::updateModelList()
{
    QUrl jsonUrl("http://gpt4all.io/models/models.json");
    QNetworkRequest request(jsonUrl);
    QSslConfiguration conf = request.sslConfiguration();
    conf.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(conf);
    QNetworkReply *jsonReply = m_networkManager.get(request);
    connect(jsonReply, &QNetworkReply::finished, this, &Download::handleModelsJsonDownloadFinished);
}

void Download::updateReleaseNotes()
{
    QUrl jsonUrl("http://gpt4all.io/meta/release.json");
    QNetworkRequest request(jsonUrl);
    QSslConfiguration conf = request.sslConfiguration();
    conf.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(conf);
    QNetworkReply *jsonReply = m_networkManager.get(request);
    connect(jsonReply, &QNetworkReply::finished, this, &Download::handleReleaseJsonDownloadFinished);
}

void Download::downloadModel(const QString &modelFile)
{
    QFile *tempFile = new QFile(ModelList::globalInstance()->incompleteDownloadPath(modelFile));
    QDateTime modTime = tempFile->fileTime(QFile::FileModificationTime);
    bool success = tempFile->open(QIODevice::WriteOnly | QIODevice::Append);
    qWarning() << "Opening temp file for writing:" << tempFile->fileName();
    if (!success) {
        const QString error
            = QString("ERROR: Could not open temp file: %1 %2").arg(tempFile->fileName()).arg(modelFile);
        qWarning() << error;
        ModelList::globalInstance()->updateData(modelFile, ModelList::DownloadErrorRole, error);
        return;
    }
    size_t incomplete_size = tempFile->size();
    if (incomplete_size > 0) {
        if (modTime < m_startTime) {
            qWarning() << "File last modified before app started, rewinding by 1MB";
            if (incomplete_size >= 1024 * 1024) {
                incomplete_size -= 1024 * 1024;
            } else {
                incomplete_size = 0;
            }
        }
        tempFile->seek(incomplete_size);
    }

    if (!ModelList::globalInstance()->contains(modelFile)) {
        qWarning() << "ERROR: Could not find file:" << modelFile;
        return;
    }

    ModelList::globalInstance()->updateData(modelFile, ModelList::DownloadingRole, true);
    ModelInfo info = ModelList::globalInstance()->modelInfo(modelFile);
    QString url = !info.url.isEmpty() ? info.url : "http://gpt4all.io/models/" + modelFile;
    Network::globalInstance()->sendDownloadStarted(modelFile);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::User, modelFile);
    request.setRawHeader("range", QString("bytes=%1-").arg(incomplete_size).toUtf8());
    QSslConfiguration conf = request.sslConfiguration();
    conf.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(conf);
    QNetworkReply *modelReply = m_networkManager.get(request);
    connect(modelReply, &QNetworkReply::downloadProgress, this, &Download::handleDownloadProgress);
    connect(modelReply, &QNetworkReply::finished, this, &Download::handleModelDownloadFinished);
    connect(modelReply, &QNetworkReply::readyRead, this, &Download::handleReadyRead);
    m_activeDownloads.insert(modelReply, tempFile);
}

void Download::cancelDownload(const QString &modelFile)
{
    for (int i = 0; i < m_activeDownloads.size(); ++i) {
        QNetworkReply *modelReply = m_activeDownloads.keys().at(i);
        QUrl url = modelReply->request().url();
        if (url.toString().endsWith(modelFile)) {
            Network::globalInstance()->sendDownloadCanceled(modelFile);

            // Disconnect the signals
            disconnect(modelReply, &QNetworkReply::downloadProgress, this, &Download::handleDownloadProgress);
            disconnect(modelReply, &QNetworkReply::finished, this, &Download::handleModelDownloadFinished);

            modelReply->abort(); // Abort the download
            modelReply->deleteLater(); // Schedule the reply for deletion

            QFile *tempFile = m_activeDownloads.value(modelReply);
            tempFile->deleteLater();
            m_activeDownloads.remove(modelReply);

            ModelList::globalInstance()->updateData(modelFile, ModelList::DownloadingRole, false);
            break;
        }
    }
}

void Download::installModel(const QString &modelFile, const QString &apiKey)
{
    Q_ASSERT(!apiKey.isEmpty());
    if (apiKey.isEmpty())
        return;

    Network::globalInstance()->sendInstallModel(modelFile);
    QString filePath = ModelList::globalInstance()->localModelsPath() + modelFile + ".txt";
    QFile file(filePath);
    if (file.open(QIODeviceBase::WriteOnly | QIODeviceBase::Text)) {
        QTextStream stream(&file);
        stream << apiKey;
        file.close();
    }
}

void Download::removeModel(const QString &modelFile)
{
    const bool isChatGPT = modelFile.startsWith("chatgpt-");
    const QString filePath = ModelList::globalInstance()->localModelsPath()
        + modelFile
        + (isChatGPT ? ".txt" : QString());

    QFile incompleteFile(ModelList::globalInstance()->incompleteDownloadPath(modelFile));
    if (incompleteFile.exists()) {
        incompleteFile.remove();
    }

    QFile file(filePath);
    if (file.exists()) {
        Network::globalInstance()->sendRemoveModel(modelFile);
        file.remove();
    }

    ModelList::globalInstance()->updateData(modelFile, ModelList::BytesReceivedRole, 0);
    ModelList::globalInstance()->updateData(modelFile, ModelList::BytesTotalRole, 0);
    ModelList::globalInstance()->updateData(modelFile, ModelList::TimestampRole, 0);
    ModelList::globalInstance()->updateData(modelFile, ModelList::SpeedRole, QString());
    ModelList::globalInstance()->updateData(modelFile, ModelList::DownloadErrorRole, QString());
}

void Download::handleSslErrors(QNetworkReply *reply, const QList<QSslError> &errors)
{
    QUrl url = reply->request().url();
    for (const auto &e : errors)
        qWarning() << "ERROR: Received ssl error:" << e.errorString() << "for" << url;
}

void Download::handleModelsJsonDownloadFinished()
{
#if 0
    QByteArray jsonData = QString(""
    "["
    "  {"
    "    \"md5sum\": \"61d48a82cb188cceb14ebb8082bfec37\","
    "    \"filename\": \"ggml-gpt4all-j-v1.1-breezy.bin\","
    "    \"filesize\": \"3785248281\""
    "  },"
    "  {"
    "    \"md5sum\": \"879344aaa9d62fdccbda0be7a09e7976\","
    "    \"filename\": \"ggml-gpt4all-j-v1.2-jazzy.bin\","
    "    \"filesize\": \"3785248281\","
    "    \"isDefault\": \"true\""
    "  },"
    "  {"
    "    \"md5sum\": \"5b5a3f9b858d33b29b52b89692415595\","
    "    \"filesize\": \"3785248281\","
    "    \"filename\": \"ggml-gpt4all-j.bin\""
    "  }"
    "]"
    ).toUtf8();
    printf("%s\n", jsonData.toStdString().c_str());
    fflush(stdout);
#else
    QNetworkReply *jsonReply = qobject_cast<QNetworkReply *>(sender());
    if (!jsonReply)
        return;

    QByteArray jsonData = jsonReply->readAll();
    jsonReply->deleteLater();
#endif
    parseModelsJsonFile(jsonData);
}

void Download::parseModelsJsonFile(const QByteArray &jsonData)
{
    QJsonParseError err;
    QJsonDocument document = QJsonDocument::fromJson(jsonData, &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "ERROR: Couldn't parse: " << jsonData << err.errorString();
        return;
    }

    QJsonArray jsonArray = document.array();
    const QString currentVersion = QCoreApplication::applicationVersion();

    for (const QJsonValue &value : jsonArray) {
        QJsonObject obj = value.toObject();

        QString modelName = obj["name"].toString();
        QString modelFilename = obj["filename"].toString();
        QString modelFilesize = obj["filesize"].toString();
        QString requiresVersion = obj["requires"].toString();
        QString deprecatedVersion = obj["deprecated"].toString();
        QString url = obj["url"].toString();
        QByteArray modelMd5sum = obj["md5sum"].toString().toLatin1().constData();
        bool isDefault = obj.contains("isDefault") && obj["isDefault"] == QString("true");
        bool disableGUI = obj.contains("disableGUI") && obj["disableGUI"] == QString("true");
        QString description = obj["description"].toString();

        // If the currentVersion version is strictly less than required version, then continue
        if (!requiresVersion.isEmpty()
            && requiresVersion != currentVersion
            && compareVersions(requiresVersion, currentVersion)) {
            continue;
        }

        // If the current version is strictly greater than the deprecated version, then continue
        if (!deprecatedVersion.isEmpty()
            && compareVersions(currentVersion, deprecatedVersion)) {
            continue;
        }

        modelFilesize = ModelList::toFileSize(modelFilesize.toULongLong());

        if (!ModelList::globalInstance()->contains(modelFilename))
            ModelList::globalInstance()->addModel(modelFilename);

        if (!modelName.isEmpty())
            ModelList::globalInstance()->updateData(modelFilename, ModelList::NameRole, modelName);
        ModelList::globalInstance()->updateData(modelFilename, ModelList::FilesizeRole, modelFilesize);
        ModelList::globalInstance()->updateData(modelFilename, ModelList::Md5sumRole, modelMd5sum);
        ModelList::globalInstance()->updateData(modelFilename, ModelList::DefaultRole, isDefault);
        ModelList::globalInstance()->updateData(modelFilename, ModelList::DescriptionRole, description);
        ModelList::globalInstance()->updateData(modelFilename, ModelList::RequiresVersionRole, requiresVersion);
        ModelList::globalInstance()->updateData(modelFilename, ModelList::DeprecatedVersionRole, deprecatedVersion);
        ModelList::globalInstance()->updateData(modelFilename, ModelList::UrlRole, url);
        ModelList::globalInstance()->updateData(modelFilename, ModelList::DisableGUIRole, disableGUI);
    }

    const QString chatGPTDesc = tr("WARNING: requires personal OpenAI API key and usage of this "
        "model will send your chats over the network to OpenAI. Your API key will be stored on disk "
        "and only used to interact with OpenAI models. If you don't have one, you can apply for "
        "an API key <a href=\"https://platform.openai.com/account/api-keys\">here.</a>");

    {
        const QString modelFilename = "chatgpt-gpt-3.5-turbo.txt";
        if (!ModelList::globalInstance()->contains(modelFilename))
            ModelList::globalInstance()->addModel(modelFilename);
        ModelList::globalInstance()->updateData(modelFilename, ModelList::NameRole, "ChatGPT-3.5 Turbo");
        ModelList::globalInstance()->updateData(modelFilename, ModelList::ChatGPTRole, true);
        ModelList::globalInstance()->updateData(modelFilename, ModelList::DescriptionRole, tr("OpenAI's ChatGPT model GPT-3.5 Turbo. ") + chatGPTDesc);
        ModelList::globalInstance()->updateData(modelFilename, ModelList::RequiresVersionRole, "2.4.2");
    }

    {
        const QString modelFilename = "chatgpt-gpt-4.txt";
        if (!ModelList::globalInstance()->contains(modelFilename))
            ModelList::globalInstance()->addModel(modelFilename);
        ModelList::globalInstance()->updateData(modelFilename, ModelList::NameRole, "ChatGPT-4");
        ModelList::globalInstance()->updateData(modelFilename, ModelList::ChatGPTRole, true);
        ModelList::globalInstance()->updateData(modelFilename, ModelList::DescriptionRole, tr("OpenAI's ChatGPT model GPT-4. ") + chatGPTDesc);
        ModelList::globalInstance()->updateData(modelFilename, ModelList::RequiresVersionRole, "2.4.2");
    }
}

void Download::handleReleaseJsonDownloadFinished()
{
    QNetworkReply *jsonReply = qobject_cast<QNetworkReply *>(sender());
    if (!jsonReply)
        return;

    QByteArray jsonData = jsonReply->readAll();
    jsonReply->deleteLater();
    parseReleaseJsonFile(jsonData);
}

void Download::parseReleaseJsonFile(const QByteArray &jsonData)
{
    QJsonParseError err;
    QJsonDocument document = QJsonDocument::fromJson(jsonData, &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "ERROR: Couldn't parse: " << jsonData << err.errorString();
        return;
    }

    QJsonArray jsonArray = document.array();

    m_releaseMap.clear();
    for (const QJsonValue &value : jsonArray) {
        QJsonObject obj = value.toObject();

        QString version = obj["version"].toString();
        QString notes = obj["notes"].toString();
        QString contributors = obj["contributors"].toString();
        ReleaseInfo releaseInfo;
        releaseInfo.version = version;
        releaseInfo.notes = notes;
        releaseInfo.contributors = contributors;
        m_releaseMap.insert(version, releaseInfo);
    }

    emit hasNewerReleaseChanged();
    emit releaseInfoChanged();
}

void Download::handleErrorOccurred(QNetworkReply::NetworkError code)
{
    QNetworkReply *modelReply = qobject_cast<QNetworkReply *>(sender());
    if (!modelReply)
        return;

    QString modelFilename = modelReply->request().attribute(QNetworkRequest::User).toString();
    const QString error
        = QString("ERROR: Network error occurred attempting to download %1 code: %2 errorString %3")
            .arg(modelFilename)
            .arg(code)
            .arg(modelReply->errorString());
    qWarning() << error;
    ModelList::globalInstance()->updateData(modelFilename, ModelList::DownloadErrorRole, error);
    Network::globalInstance()->sendDownloadError(modelFilename, (int)code, modelReply->errorString());
    cancelDownload(modelFilename);
}

void Download::handleDownloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    QNetworkReply *modelReply = qobject_cast<QNetworkReply *>(sender());
    if (!modelReply)
        return;
    QFile *tempFile = m_activeDownloads.value(modelReply);
    if (!tempFile)
        return;
    QString contentRange = modelReply->rawHeader("content-range");
    if (contentRange.contains("/")) {
        QString contentTotalSize = contentRange.split("/").last();
        bytesTotal = contentTotalSize.toLongLong();
    }

    const QString modelFilename = modelReply->request().attribute(QNetworkRequest::User).toString();
    const qint64 lastUpdate = ModelList::globalInstance()->data(modelFilename, ModelList::TimestampRole).toLongLong();
    const qint64 currentUpdate = QDateTime::currentMSecsSinceEpoch();
    if (currentUpdate - lastUpdate < 1000)
        return;

    const qint64 lastBytesReceived = ModelList::globalInstance()->data(modelFilename, ModelList::BytesReceivedRole).toLongLong();
    const qint64 currentBytesReceived = tempFile->pos();

    qint64 timeDifference = currentUpdate - lastUpdate;
    qint64 bytesDifference = currentBytesReceived - lastBytesReceived;
    qint64 speed = (bytesDifference / timeDifference) * 1000; // bytes per second
    QString speedText;
    if (speed < 1024)
        speedText = QString::number(static_cast<double>(speed), 'f', 2) + " B/s";
    else if (speed < 1024 * 1024)
        speedText = QString::number(static_cast<double>(speed / 1024.0), 'f', 2) + " KB/s";
    else
        speedText = QString::number(static_cast<double>(speed / (1024.0 * 1024.0)), 'f', 2) + " MB/s";

    ModelList::globalInstance()->updateData(modelFilename, ModelList::BytesReceivedRole, currentBytesReceived);
    ModelList::globalInstance()->updateData(modelFilename, ModelList::BytesTotalRole, bytesTotal);
    ModelList::globalInstance()->updateData(modelFilename, ModelList::SpeedRole, speedText);
    ModelList::globalInstance()->updateData(modelFilename, ModelList::TimestampRole, currentUpdate);
}

HashAndSaveFile::HashAndSaveFile()
    : QObject(nullptr)
{
    moveToThread(&m_hashAndSaveThread);
    m_hashAndSaveThread.setObjectName("hashandsave thread");
    m_hashAndSaveThread.start();
}

void HashAndSaveFile::hashAndSave(const QString &expectedHash, const QString &saveFilePath,
        QFile *tempFile, QNetworkReply *modelReply)
{
    Q_ASSERT(!tempFile->isOpen());
    QString modelFilename = modelReply->request().attribute(QNetworkRequest::User).toString();

    // Reopen the tempFile for hashing
    if (!tempFile->open(QIODevice::ReadOnly)) {
        const QString error
            = QString("ERROR: Could not open temp file for hashing: %1 %2").arg(tempFile->fileName()).arg(modelFilename);
        qWarning() << error;
        emit hashAndSaveFinished(false, error, tempFile, modelReply);
        return;
    }

    QCryptographicHash hash(QCryptographicHash::Md5);
    while(!tempFile->atEnd())
        hash.addData(tempFile->read(16384));
    if (hash.result().toHex() != expectedHash) {
        tempFile->close();
        const QString error
            = QString("ERROR: Download error MD5SUM did not match: %1 != %2 for %3").arg(hash.result().toHex()).arg(expectedHash).arg(modelFilename);
        qWarning() << error;
        tempFile->remove();
        emit hashAndSaveFinished(false, error, tempFile, modelReply);
        return;
    }

    // The file save needs the tempFile closed
    tempFile->close();

    // Attempt to *move* the verified tempfile into place - this should be atomic
    // but will only work if the destination is on the same filesystem
    if (tempFile->rename(saveFilePath)) {
        emit hashAndSaveFinished(true, QString(), tempFile, modelReply);
        return;
    }

    // Reopen the tempFile for copying
    if (!tempFile->open(QIODevice::ReadOnly)) {
        const QString error
            = QString("ERROR: Could not open temp file at finish: %1 %2").arg(tempFile->fileName()).arg(modelFilename);
        qWarning() << error;
        emit hashAndSaveFinished(false, error, tempFile, modelReply);
        return;
    }

    // Save the model file to disk
    QFile file(saveFilePath);
    if (file.open(QIODevice::WriteOnly)) {
        QByteArray buffer;
        while (!tempFile->atEnd()) {
            buffer = tempFile->read(16384);
            file.write(buffer);
        }
        file.close();
        tempFile->close();
        emit hashAndSaveFinished(true, QString(), tempFile, modelReply);
    } else {
        QFile::FileError error = file.error();
        const QString errorString
            = QString("ERROR: Could not save model to location: %1 failed with code %2").arg(saveFilePath).arg(error);
        qWarning() << errorString;
        tempFile->close();
        emit hashAndSaveFinished(false, errorString, tempFile, modelReply);
        return;
    }
}

void Download::handleModelDownloadFinished()
{
    QNetworkReply *modelReply = qobject_cast<QNetworkReply *>(sender());
    if (!modelReply)
        return;

    QString modelFilename = modelReply->request().attribute(QNetworkRequest::User).toString();
    QFile *tempFile = m_activeDownloads.value(modelReply);
    m_activeDownloads.remove(modelReply);

    if (modelReply->error()) {
        qWarning() << "ERROR: downloading:" << modelReply->errorString();
        modelReply->deleteLater();
        tempFile->deleteLater();
        ModelList::globalInstance()->updateData(modelFilename, ModelList::DownloadingRole, false);
        ModelList::globalInstance()->updateData(modelFilename, ModelList::DownloadErrorRole, modelReply->errorString());
        return;
    }

    // The hash and save needs the tempFile closed
    tempFile->close();

    if (!ModelList::globalInstance()->contains(modelFilename)) {
        qWarning() << "ERROR: downloading no such file:" << modelFilename;
        modelReply->deleteLater();
        tempFile->deleteLater();
        return;
    }

    // Notify that we are calculating hash
    ModelList::globalInstance()->updateData(modelFilename, ModelList::CalcHashRole, true);
    QByteArray md5sum =  ModelList::globalInstance()->modelInfo(modelFilename).md5sum;
    const QString saveFilePath = ModelList::globalInstance()->localModelsPath() + modelFilename;
    emit requestHashAndSave(md5sum, saveFilePath, tempFile, modelReply);
}

void Download::handleHashAndSaveFinished(bool success, const QString &error,
        QFile *tempFile, QNetworkReply *modelReply)
{
    // The hash and save should send back with tempfile closed
    Q_ASSERT(!tempFile->isOpen());
    QString modelFilename = modelReply->request().attribute(QNetworkRequest::User).toString();
    Network::globalInstance()->sendDownloadFinished(modelFilename, success);
    ModelList::globalInstance()->updateData(modelFilename, ModelList::CalcHashRole, false);
    modelReply->deleteLater();
    tempFile->deleteLater();

    ModelList::globalInstance()->updateData(modelFilename, ModelList::DownloadingRole, false);
    if (!success)
        ModelList::globalInstance()->updateData(modelFilename, ModelList::DownloadErrorRole, error);
    else
        ModelList::globalInstance()->updateData(modelFilename, ModelList::DownloadErrorRole, QString());
}

void Download::handleReadyRead()
{
    QNetworkReply *modelReply = qobject_cast<QNetworkReply *>(sender());
    if (!modelReply)
        return;

    QFile *tempFile = m_activeDownloads.value(modelReply);
    QByteArray buffer;
    while (!modelReply->atEnd()) {
        buffer = modelReply->read(16384);
        tempFile->write(buffer);
    }
    tempFile->flush();
}
