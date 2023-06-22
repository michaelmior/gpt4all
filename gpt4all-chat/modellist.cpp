#include "modellist.h"

bool InstalledModels::filterAcceptsRow(int sourceRow,
                                       const QModelIndex &sourceParent) const
{
    QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    bool isInstalled = sourceModel()->data(index, ModelList::InstalledRole).toBool();
    return isInstalled;
}

DownloadableModels::DownloadableModels(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    connect(this, &DownloadableModels::rowsInserted, this, &DownloadableModels::countChanged);
    connect(this, &DownloadableModels::rowsRemoved, this, &DownloadableModels::countChanged);
    connect(this, &DownloadableModels::modelReset, this, &DownloadableModels::countChanged);
    connect(this, &DownloadableModels::layoutChanged, this, &DownloadableModels::countChanged);
}

bool DownloadableModels::filterAcceptsRow(int sourceRow,
                                       const QModelIndex &sourceParent) const
{
    QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    bool isDownloadable = !sourceModel()->data(index, ModelList::DescriptionRole).toString().isEmpty();
    return isDownloadable;
}

int DownloadableModels::count() const
{
    return rowCount();
}

class MyModelList: public ModelList { };
Q_GLOBAL_STATIC(MyModelList, modelListInstance)
ModelList *ModelList::globalInstance()
{
    return modelListInstance();
}

ModelList::ModelList()
    : QAbstractListModel(nullptr)
    , m_installedModels(new InstalledModels(this))
    , m_downloadableModels(new DownloadableModels(this))
{
    m_installedModels->setSourceModel(this);
    m_downloadableModels->setSourceModel(this);
    m_watcher = new QFileSystemWatcher(this);
    QSettings settings;
    settings.sync();
    m_localModelsPath = settings.value("modelPath", defaultLocalModelsPath()).toString();
    const QString exePath = QCoreApplication::applicationDirPath() + QDir::separator();
    m_watcher->addPath(exePath);
    m_watcher->addPath(m_localModelsPath);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &ModelList::updateModelsFromDirectory);
    updateModelsFromDirectory();
}

QString ModelList::incompleteDownloadPath(const QString &modelFile)
{
    return localModelsPath() + "incomplete-" + modelFile;
}

const QList<ModelInfo> ModelList::exportModelList() const
{
    QMutexLocker locker(&m_mutex);
    QList<ModelInfo> infos;
    for (ModelInfo *info : m_models)
        infos.append(*info);
    return infos;
}

ModelInfo ModelList::defaultModelInfo() const
{
    QMutexLocker locker(&m_mutex);

    QSettings settings;
    settings.sync();

    // The user default model can be set by the user in the settings dialog. The "default" user
    // default model is "Application default" which signals we should use the default model that was
    // specified by the models.json file.
    const QString defaultModelName = settings.value("userDefaultModel").toString();
    const bool hasDefaultName = !defaultModelName.isEmpty() && defaultModelName != "Application default";
    ModelInfo *defaultModel = nullptr;
    for (ModelInfo *info : m_models) {
        if (!info->installed)
            continue;
        defaultModel = info;
        if (!hasDefaultName && defaultModel->isDefault) break;
        if (hasDefaultName && defaultModel->filename == defaultModelName) break;
    }
    if (defaultModel)
        return *defaultModel;
    return ModelInfo();
}

bool ModelList::contains(const QString &filename) const
{
    QMutexLocker locker(&m_mutex);
    return m_modelMap.contains(filename);
}

void ModelList::addModel(const QString &filename)
{
    QMutexLocker locker(&m_mutex);
    Q_ASSERT(!m_modelMap.contains(filename));
    if (m_modelMap.contains(filename)) {
        qWarning() << "ERROR: model list already contains" << filename;
        return;
    }

    beginInsertRows(QModelIndex(), m_models.size(), m_models.size());
    ModelInfo *info = new ModelInfo;
    info->filename = filename;
    m_models.append(info);
    m_modelMap.insert(filename, info);
    endInsertRows();
}

int ModelList::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent)
    QMutexLocker locker(&m_mutex);
    return m_models.size();
}

QVariant ModelList::dataInternal(const ModelInfo *info, int role) const
{
    switch (role) {
        case NameRole:
            return info->name;
        case FilenameRole:
            return info->filename;
        case DirpathRole:
            return info->dirpath;
        case FilesizeRole:
            return info->filesize;
        case Md5sumRole:
            return info->md5sum;
        case CalcHashRole:
            return info->calcHash;
        case InstalledRole:
            return info->installed;
        case DefaultRole:
            return info->isDefault;
        case ChatGPTRole:
            return info->isChatGPT;
        case DisableGUIRole:
            return info->disableGUI;
        case DescriptionRole:
            return info->description;
        case RequiresVersionRole:
            return info->requiresVersion;
        case DeprecatedVersionRole:
            return info->deprecatedVersion;
        case UrlRole:
            return info->url;
        case BytesReceivedRole:
            return info->bytesReceived;
        case BytesTotalRole:
            return info->bytesTotal;
        case TimestampRole:
            return info->timestamp;
        case SpeedRole:
            return info->speed;
        case DownloadingRole:
            return info->isDownloading;
        case IncompleteRole:
            return info->isIncomplete;
        case DownloadErrorRole:
            return info->downloadError;
    }

    return QVariant();
}

QVariant ModelList::data(const QString &filename, int role) const
{
    QMutexLocker locker(&m_mutex);
    ModelInfo *info = m_modelMap.value(filename);
    return dataInternal(info, role);
}

QVariant ModelList::data(const QModelIndex &index, int role) const
{
    QMutexLocker locker(&m_mutex);
    if (!index.isValid() || index.row() < 0 || index.row() >= m_models.size())
        return QVariant();
    const ModelInfo *info = m_models.at(index.row());
    return dataInternal(info, role);
}

void ModelList::updateData(const QString &filename, int role, const QVariant &value)
{
    QMutexLocker locker(&m_mutex);
    if (!m_modelMap.contains(filename)) {
        qWarning() << "ERROR: cannot update as model map does not contain" << filename;
        return;
    }

    ModelInfo *info = m_modelMap.value(filename);
    const int index = m_models.indexOf(info);
    if (index == -1) {
        qWarning() << "ERROR: cannot update as model list does not contain" << filename;
        return;
    }

    switch (role) {
    case NameRole:
        info->name = value.toString(); break;
    case FilenameRole:
        info->filename = value.toString(); break;
    case DirpathRole:
        info->dirpath = value.toString(); break;
    case FilesizeRole:
        info->filesize = value.toString(); break;
    case Md5sumRole:
        info->md5sum = value.toByteArray(); break;
    case CalcHashRole:
        info->calcHash = value.toBool(); break;
    case InstalledRole:
        info->installed = value.toBool(); break;
    case DefaultRole:
        info->isDefault = value.toBool(); break;
    case ChatGPTRole:
        info->isChatGPT = value.toBool(); break;
    case DisableGUIRole:
        info->disableGUI = value.toBool(); break;
    case DescriptionRole:
        info->description = value.toString(); break;
    case RequiresVersionRole:
        info->requiresVersion = value.toString(); break;
    case DeprecatedVersionRole:
        info->deprecatedVersion = value.toString(); break;
    case UrlRole:
        info->url = value.toString(); break;
    case BytesReceivedRole:
        info->bytesReceived = value.toLongLong(); break;
    case BytesTotalRole:
        info->bytesTotal = value.toLongLong(); break;
    case TimestampRole:
        info->timestamp = value.toLongLong(); break;
    case SpeedRole:
        info->speed = value.toString(); break;
    case DownloadingRole:
        info->isDownloading = value.toBool(); break;
    case IncompleteRole:
        info->isIncomplete = value.toBool(); break;
    case DownloadErrorRole:
        info->downloadError = value.toString(); break;
    }

    // Extra guarantee that these always remains in sync with filesystem
    QFileInfo fileInfo(info->dirpath + info->filename);
    if (info->installed != fileInfo.exists()) {
        info->installed = fileInfo.exists();
        emit dataChanged(createIndex(index, 0), createIndex(index, 0), {InstalledRole});
    }
    QFileInfo incompleteInfo(incompleteDownloadPath(info->filename));
    if (info->isIncomplete != incompleteInfo.exists()) {
        info->isIncomplete = incompleteInfo.exists();
        emit dataChanged(createIndex(index, 0), createIndex(index, 0), {IncompleteRole});
    }

    emit dataChanged(createIndex(index, 0), createIndex(index, 0), {role});
}

ModelInfo ModelList::modelInfo(const QString &filename) const
{
    QMutexLocker locker(&m_mutex);
    if (!m_modelMap.contains(filename))
        return ModelInfo();
    return *m_modelMap.value(filename);
}

QString ModelList::defaultLocalModelsPath() const
{
    QString localPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + "/";
    QString testWritePath = localPath + QString("test_write.txt");
    QString canonicalLocalPath = QFileInfo(localPath).canonicalFilePath() + "/";
    QDir localDir(localPath);
    if (!localDir.exists()) {
        if (!localDir.mkpath(localPath)) {
            qWarning() << "ERROR: Local download directory can't be created:" << canonicalLocalPath;
            return canonicalLocalPath;
        }
    }

    if (QFileInfo::exists(testWritePath))
        return canonicalLocalPath;

    QFile testWriteFile(testWritePath);
    if (testWriteFile.open(QIODeviceBase::ReadWrite)) {
        testWriteFile.close();
        return canonicalLocalPath;
    }

    qWarning() << "ERROR: Local download path appears not writeable:" << canonicalLocalPath;
    return canonicalLocalPath;
}

QString ModelList::localModelsPath() const
{
    return m_localModelsPath;
}

void ModelList::setLocalModelsPath(const QString &modelPath)
{
    QString filePath = (modelPath.startsWith("file://") ?
                        QUrl(modelPath).toLocalFile() : modelPath);
    QString canonical = QFileInfo(filePath).canonicalFilePath() + "/";
    if (m_localModelsPath != canonical) {
        m_localModelsPath = canonical;
        emit localModelsPathChanged();
    }
}

QString ModelList::modelDirPath(const QString &modelName, bool isChatGPT)
{
    QVector<QString> possibleFilePaths;
    if (isChatGPT)
        possibleFilePaths << "/" + modelName + ".txt";
    else {
        possibleFilePaths << "/ggml-" + modelName + ".bin";
        possibleFilePaths << "/" + modelName + ".bin";
    }
    for (const QString &modelFilename : possibleFilePaths) {
        QString appPath = QCoreApplication::applicationDirPath() + modelFilename;
        QFileInfo infoAppPath(appPath);
        if (infoAppPath.exists())
            return QCoreApplication::applicationDirPath();

        QString downloadPath = localModelsPath() + modelFilename;
        QFileInfo infoLocalPath(downloadPath);
        if (infoLocalPath.exists())
            return localModelsPath();
    }
    return QString();
}

void ModelList::updateModelsFromDirectory()
{
    const QString exePath = QCoreApplication::applicationDirPath() + QDir::separator();
    const QString localPath = localModelsPath();
    {
        QDir dir(exePath);
        QStringList allFiles = dir.entryList(QDir::Files);

        // All files that end with .bin and have 'ggml' somewhere in the name
        QStringList fileNames;
        for(const QString& filename : allFiles) {
            if (filename.endsWith(".bin") && filename.contains("ggml")) {
                fileNames.append(filename);
            }
        }

        for (const QString& f : fileNames) {
            QString filePath = exePath + f;
            QFileInfo info(filePath);
            if (!info.exists())
                continue;
            if (!contains(f))
                addModel(f);
            updateData(f, DirpathRole, exePath);
            updateData(f, FilesizeRole, toFileSize(info.size()));
        }
    }

    if (localPath != exePath) {
        QDir dir(localPath);
        QStringList allFiles = dir.entryList(QDir::Files);
        QStringList fileNames;
        for(const QString& filename : allFiles) {
            if ((filename.endsWith(".bin") && filename.contains("ggml"))
                || (filename.endsWith(".txt") && filename.startsWith("chatgpt-"))) {
                fileNames.append(filename);
            }
        }

        for (const QString& f : fileNames) {
            QString filePath = localPath + f;
            QFileInfo info(filePath);
            if (!info.exists())
                continue;
            if (!contains(f))
                addModel(f);
            updateData(f, ChatGPTRole, f.startsWith("chatgpt-"));
            updateData(f, DirpathRole, localPath);
            updateData(f, FilesizeRole, toFileSize(info.size()));
        }
    }
}
