#include "safedaemon.h"

SafeDaemon::SafeDaemon(QObject *parent) : QObject(parent) {
    this->settings = new QSettings(ORG_NAME, APP_NAME, this);
    this->apiFactory = new SafeApiFactory(API_HOST, this);
    this->server = new QLocalServer(this);
    this->online = false;

    connect(server, &QLocalServer::newConnection, this, &SafeDaemon::handleClientConnection);
    this->bindServer(this->server,
                     QDir::homePath() +
                     QDir::separator() + SAFE_DIR +
                     QDir::separator() + SOCKET_FILE);

    if (this->authUser())
        init();
}

SafeDaemon::~SafeDaemon()
{
    this->online = false;
    this->apiFactory->deleteLater();
    this->watcher->deleteLater();
    this->swatcher->deleteLater();
    this->localStateDb->deleteLater();
    this->remoteStateDb->deleteLater();
}

bool SafeDaemon::authUser() {
    QString login = this->settings->value("login", "").toString();
    QString password = this->settings->value("password", "").toString();

    if (login.length() < 1 || password.length() < 1) {
        this->online = false;
        qDebug() << "Unauthorized";
        return false;
    } else if(!this->apiFactory->authUser(login, password)) {
        this->online = false;
        qWarning() << "Authentication failed";
        return false;
    }

    return true;
}

void SafeDaemon::init()
{
    this->online = true;
    fetchUsage();

    // debug clean dbs
    purgeDb(LOCAL_STATE_DATABASE);
    purgeDb(REMOTE_STATE_DATABASE);
    // open dbs
    this->localStateDb = new SafeStateDb(LOCAL_STATE_DATABASE);
    this->remoteStateDb = new SafeStateDb(REMOTE_STATE_DATABASE);
    // index all remote files
    fullRemoteIndex();
    // setup watcher (to track remote events from now)
    this->settings->setValue("last_updated", (quint32)QDateTime::currentDateTime().toTime_t());
    this->swatcher = new SafeWatcher((ulong)this->settings->value("last_updated").toDouble(),
                                     this->apiFactory, this);
    connect(this->swatcher, &SafeWatcher::timestampChanged, [&](ulong ts){
        this->settings->setValue("last_updated", (quint32)ts);
    });
    connect(this->swatcher, &SafeWatcher::fileAdded, this, &SafeDaemon::remoteFileAdded);
    connect(this->swatcher, &SafeWatcher::fileDeleted, this, &SafeDaemon::remoteFileDeleted);
    connect(this->swatcher, &SafeWatcher::fileMoved, this, &SafeDaemon::remoteFileMoved);
    connect(this->swatcher, &SafeWatcher::directoryCreated, this, &SafeDaemon::remoteDirectoryCreated);
    connect(this->swatcher, &SafeWatcher::directoryDeleted, this, &SafeDaemon::remoteDirectoryDeleted);
    connect(this->swatcher, &SafeWatcher::directoryMoved, this, &SafeDaemon::remoteDirectoryMoved);

    // local index
    if(this->settings->value("init", true).toBool()) {
        fullIndex(QDir(getFilesystemPath()));
        //this->settings->setValue("init", false);
    } else {
        checkIndex(QDir(getFilesystemPath()));
    }
    // start watching for remote events
    this->swatcher->watch();
    // start watching for fs events
    this->initWatcher(getFilesystemPath());
}

void SafeDaemon::deauthUser()
{
    this->online = false;
    this->watcher->stop();

    this->apiFactory->deleteLater();
    this->swatcher->deleteLater();
    this->watcher->deleteLater();
    this->localStateDb->deleteLater();
    this->remoteStateDb->deleteLater();
    this->settings->setValue("init", true);

    this->apiFactory = new SafeApiFactory(API_HOST, this);
    purgeDb(LOCAL_STATE_DATABASE);
    purgeDb(REMOTE_STATE_DATABASE);
}

void SafeDaemon::purgeDb(const QString &name)
{
    QString path = SafeStateDb::formPath(name);
    QFile(path).remove();
}

void SafeDaemon::initWatcher(const QString &path) {
    this->watcher = new FSWatcher(path, this);
    connect(this->watcher, &FSWatcher::added, this, &SafeDaemon::fileAdded);
    connect(this->watcher, &FSWatcher::modified, this, &SafeDaemon::fileModified);
    connect(this->watcher, &FSWatcher::deleted, this, &SafeDaemon::fileDeleted);
    connect(this->watcher, &FSWatcher::moved, this, &SafeDaemon::fileMoved);
    this->watcher->watch();
}

bool SafeDaemon::isListening() {
    return this->server->isListening();
}

QString SafeDaemon::socketPath() {
    return this->server->fullServerName();
}

void SafeDaemon::finishTransfer(const QString &path)
{
    if(this->activeTransfers.contains(path)) {
        this->activeTransfers.take(path)->deleteLater();
        fetchUsage();
    }
}

void SafeDaemon::storeTransfer(const QString &path, SafeApi *api)
{
    this->activeTransfers.insert(path, api);
}

QString SafeDaemon::getFilesystemPath()
{
    QString root = this->settings->value("root_name", DEFAULT_ROOT_NAME).toString();
    return QDir::cleanPath(QDir::homePath() + QDir::separator() + root);
}

QJsonObject SafeDaemon::formSettingsReply(const QJsonArray &requestFields) {
    QJsonObject result, values;

    result.insert("type", QJsonValue(QString("settings")));
    foreach (auto field, requestFields) {
        QString value = this->settings->value(field.toString(), "").toString();
        if (value.length() > 0) {
            values.insert(field.toString(), value);
        }
    }
    result.insert("values", values);
    return result;
}

void SafeDaemon::bindServer(QLocalServer *server, QString path)
{
    QFile socket_file(path);
    if (!server->listen(path)) {
        /* try to remove old socket file */
        if (socket_file.exists() && socket_file.remove()) {
            /* retry bind */
            if (!server->listen(path)) {
                qWarning() << "Unable to bind socket to" << path;
            }
        } else {
            qWarning() << "Unable to bind socket on" << path << ", try to remove it manually.";
        }
    }
}

void SafeDaemon::handleClientConnection()
{
    auto socket = this->server->nextPendingConnection();
    if(!socket->waitForReadyRead(2000)) {
        qWarning() << "No data from socket connection";
    }

    QObject::connect(socket, &QLocalSocket::disconnected, &QLocalSocket::deleteLater);
    if (!socket->isValid() || socket->bytesAvailable() < 1) {
        socket->disconnectFromServer();
        return;
    }

    QTextStream stream(socket);
    stream.autoDetectUnicode();
    QString data(stream.readAll());

    /* JSON parsing */
    QJsonParseError jsonError;
    QJsonDocument jsonMessage = QJsonDocument::fromJson(data.toLatin1(), &jsonError);
    if (jsonError.error) {
        qWarning() << "JSON error:" << jsonError.errorString();
        return;
    } else if (!jsonMessage.isObject()) {
        qWarning() << "Not an object:" << jsonMessage;
        return;
    }

    /* Login */
    QJsonObject message = jsonMessage.object();
    QString type = message["type"].toString();

    if (type == GET_SETTINGS_TYPE) {
        QJsonObject response = formSettingsReply(message["fields"].toArray());
        stream <<  QJsonDocument(response).toJson();
        stream.flush();
    } else if (type == SET_SETTINGS_TYPE) {
        QJsonObject args = message["args"].toObject();
        for (QJsonObject::ConstIterator i = args.begin(); i != args.end(); ++i) {
            this->settings->setValue(i.key(), i.value().toString());
        }
    } else if(type == ACTION_TYPE) {
        QString verb = message["verb"].toString();
        if(verb == "get_public_link") {
            QString file = message["args"].toObject().value("file").toString();
            QString link = getPublicLink(QFileInfo(file));
            qDebug() << "Got file:" << file << "link for it:" << link;
            stream << link;
            stream.flush();
        } else if (verb == "open_in_browser") {
            QString file = message["args"].toObject().value("file").toString();
            QString link = getFolderLink(QFileInfo(file));
            qDebug() << "Got file:" << file << "link for it:" << link;
            stream << link;
            stream.flush();
        } else if (verb == "logout") {
            deauthUser();
            this->settings->setValue("login", "");
            this->settings->setValue("password", "");
        } else if (verb == "login") {
            QJsonObject args = message["args"].toObject();
            QString login = args["login"].toString();
            QString password = args["password"].toString();
            if (login.length() < 1 || password.length() < 1) {
                return;
            }
            this->settings->setValue("login", login);
            this->settings->setValue("password", password);
            if(this->authUser())
                init();
        } else if (verb == "chdir") {
            QString dir = message["args"].toObject().value("dir").toString();
            QFileInfo d(dir);
            if(d.exists() && d.isReadable() && d.isDir()) {
                this->settings->setValue("root_name", dir);
                deauthUser();
                init();
            }
        }
    } else if (type == API_CALL_TYPE) {
        // XXX
    } else if (type == NOOP_TYPE) {
        // State variables
        notifyEventQuota(this->used_bytes, this->total_bytes);
        notifyEventSync(this->activeTransfers.count());
        notifyEventAuth(this->online, this->apiFactory->login());

        if(this->messagesQueue.isEmpty()) {
            QJsonObject obj;
            obj.insert("type", QString("noop"));
            stream << QJsonDocument(obj).toJson();
        } else {
            QJsonObject obj;
            QJsonArray messages;
            foreach(QJsonObject m, this->messagesQueue){
                messages.append(m);
            }
            obj.insert("type", QString("queue"));
            obj.insert("messages", messages);
            stream << QJsonDocument(obj).toJson();
            this->messagesQueue.clear();
        }
        stream.flush();

    } else {
        qWarning() << "Got message of unknown type:" << type;
    }

    socket->close();
}

void SafeDaemon::fetchUsage()
{
    auto api = this->apiFactory->newApi();
    connect(api, &SafeApi::getDiskQuotaComplete, [&](ulong id, ulong used_bytes, ulong total_bytes){
        this->used_bytes = used_bytes;
        this->total_bytes = total_bytes;
    });
    connect(api, &SafeApi::errorRaised, [](ulong id, quint16 code, QString text){
        qWarning() << "Error fetching quota:" << text << "(" << code << ")";
    });
    api->getDiskQuota();
}

void SafeDaemon::notifyEventQuota(ulong used, ulong total)
{
    QJsonObject obj;
    QJsonObject values;
    values.insert("used_bytes", (qint64)used);
    values.insert("total_bytes", (qint64)total);

    obj.insert("type", QString("event"));
    obj.insert("category", QString("disk_quota"));
    obj.insert("values", values);
    this->messagesQueue.append(obj);
}

void SafeDaemon::notifyEventAuth(bool auth, QString login)
{
    QJsonObject obj;
    QJsonObject values;
    values.insert("authorized", auth);
    values.insert("login", login);

    obj.insert("type", QString("event"));
    obj.insert("category", QString("auth"));
    obj.insert("values", values);
    this->messagesQueue.append(obj);
}

void SafeDaemon::notifyEventSync(ulong count)
{
    QJsonObject obj;
    QJsonObject values;
    values.insert("count", (qint64)count);
    values.insert("timestamp", this->settings->value("last_updated").toDouble());

    obj.insert("type", QString("event"));
    obj.insert("category", QString("sync"));
    obj.insert("values", values);
    this->messagesQueue.append(obj);
}

QJsonObject SafeDaemon::fetchFileInfo(const QString &id)
{
    QJsonObject info;
    QEventLoop loop;
    auto api = this->apiFactory->newApi();
    connect(api, &SafeApi::getPropsComplete, [&](ulong id, QJsonObject props){
        info = props.value("object").toObject();
        loop.exit();
    });
    connect(api, &SafeApi::errorRaised, [&](ulong id, quint16 code, QString text){
        qWarning() << "Error fetching info:" << text << "(" << code << ")";
        loop.exit();
    });

    api->getProps(id);
    loop.exec();
    return info;
}

QJsonObject SafeDaemon::fetchDirInfo(const QString &id)
{
    QJsonObject info;
    QEventLoop loop;
    auto api = this->apiFactory->newApi();
    connect(api, &SafeApi::getPropsComplete, [&](ulong id, QJsonObject props){
        info = props.value("object").toObject();
        loop.exit();
    });
    connect(api, &SafeApi::errorRaised, [&](ulong id, quint16 code, QString text){
        qWarning() << "Error fetching info:" << text << "(" << code << ")";
        loop.exit();
    });

    api->getProps(id);
    loop.exec();
    return info;
}

void SafeDaemon::prepareTree(const QFileInfo &info, const QString &root)
{
    qDebug() << "Preparing tree" << info.path() << "root:" << root;
    QString relative = relativePath(info);
    QStack<QString> stack;
    while(relative != root && relative.length() > 1) {
        QString path(getFilesystemPath() + QDir::separator() + relative);
        qDebug() << "pushing relative" << relative;
        stack.push(relative);
        relative = relativePath(QFileInfo(path));
        qDebug() << "next relative" << relative;
    }

    qDebug() << "relative exit";

    while(!stack.empty()) {
        QString relative = stack.pop();
        QString path(getFilesystemPath() + QDir::separator() + relative);
        if(!this->remoteStateDb->existsDir(relative)) {
            QString pid = fetchDirId(relativePath(QFileInfo(path)));
            qDebug() << "preparing dir" << path << "(" << relative << ")" << "in"
                     << pid << "(" << relativePath(QFileInfo(path)) << ")";
            createDir(pid, path);
        }
    }
}

QString SafeDaemon::getPublicLink(const QFileInfo &info)
{
    QString objLink;
    QString id;
    if(info.isDir()) {
        id = this->remoteStateDb->getDirId(relativeFilePath(info));
    } else {
        id = this->remoteStateDb->getFileId(relativeFilePath(info));
    }
    if(id.isEmpty()){
        return QString();
    }

    QEventLoop loop;
    auto api = this->apiFactory->newApi();
    connect(api, &SafeApi::publicObjectComplete, [&](ulong id, QString link){
        objLink = link;
        loop.exit();
    });
    connect(api, &SafeApi::errorRaised, [&](ulong id, quint16 code, QString text){
        qWarning() << "Error getting public link:" << text << "(" << code << ")";
        loop.exit();
    });

    api->publicObject(id);
    loop.exec();
    return objLink;
}

QString SafeDaemon::getFolderLink(const QFileInfo &info)
{
    QString pid(fetchDirId(relativePath(info)));
    if(pid.isEmpty()){
        return QString();
    }

    QString prefix("https://www.2safe.com/web/");
    return prefix + pid + QDir::separator() + info.fileName();
}

void SafeDaemon::fileAdded(const QString &path, bool isDir) {
    QFileInfo info;

    if(isDir) {
        info.setFile(QDir(path).path());
    } else {
        info.setFile(path);
    }

    if (!this->isFileAllowed(info)) {
        qDebug() << "Ignoring object" << info.filePath();
        return;
    }

    QString relative(relativePath(info));
    QString relativeF(relativeFilePath(info));

    if(isDir) {
        qDebug() << "Directory added: " << relativeF;

        if(this->remoteStateDb->existsDir(relativeF)) {
            return;
        }

        if(this->localStateDb->existsDir(relativeF)) {
            return;
        }

        QString dirId = createDir(fetchDirId(relative), info.filePath());
        this->localStateDb->removeDir(relativeF);
        this->localStateDb->insertDir(relativeF, info.dir().dirName(), getMtime(info), dirId);
        fullIndex(QDir(path));
        return;
    }

    this->localStateDb->removeFile(relativeF);
    this->localStateDb->insertFile(relative, relativeF, info.fileName(),
                                   getMtime(info), makeHash(info));
    this->localStateDb->updateDirHash(relative);

    if(this->remoteStateDb->existsFile(relativeF)){
        // XXX: check for cause (mtime/hash)
        return;
    }

    qDebug() << "File added: " << info.filePath();
    queueUploadFile(fetchDirId(relative), info);
}

void SafeDaemon::fileModified(const QString &path) {
    QFileInfo info(path);
    if (!this->isFileAllowed(info)) {
        qDebug() << "Ignoring object" << info.filePath();
        return;
    }

    QString relative(relativePath(info));
    QString relativeF(relativeFilePath(info));

    this->localStateDb->removeFile(relativeF);
    this->localStateDb->insertFile(relative, relativeF, info.fileName(),
                                   getMtime(info), makeHash(info));
    this->localStateDb->updateDirHash(relative);

    if(this->remoteStateDb->existsFile(relativeF)){
        // XXX: check for cause (mtime/hash)
        if(getMtime(info) > this->remoteStateDb->getFileMtime(relativeF)) {
        } else {
            return;
        }
    }

    qDebug() << "File modified: " << info.filePath();
    queueUploadFile(fetchDirId(relative), info);
}

void SafeDaemon::fileDeleted(const QString &path, bool isDir)
{
    QFileInfo info(path);
    QString relativeF(relativeFilePath(info));

    if (!this->isFileAllowed(info)) {
        qDebug() << "Ignoring object" << info.filePath();
        return;
    }

    if(isDir) {
        if(!this->localStateDb->existsDir(relativeF)) {
            //qDebug() << relativeF << "not tracked, ignore";
            return;
        }
        qDebug() << "Directory deleted: " << info.filePath();
    } else {
        if(!this->localStateDb->existsFile(relativeF)) {
            //qDebug() << relativeF << "not tracked, ignore";
            return;
        }
        qDebug() << "Local file deleted: " << info.filePath();
    }

    if(isDir) {
        this->localStateDb->removeDir(relativeF);
        this->localStateDb->removeDirRecursively(relativeF);
        remoteRemoveDir(info);
        return;
    }

    remoteRemoveFile(info);
    this->localStateDb->removeFile(relativeF);
    updateDirHash(info.dir());
}

void SafeDaemon::fileMoved(const QString &path1, const QString &path2, bool isDir)
{
    qDebug() << "File moved from" << path1 << "to" << path2;
    QString relative1(relativeFilePath(path1));
    QString relative2(relativeFilePath(path2));

    if(this->localStateDb->existsFile(relative2)) {
        fileModified(path2);
    } else {
        // XXX: proper moving without reuploading
        fileDeleted(path2, isDir);
        fileAdded(path2, isDir);
    }

    if(isDir) {
        this->localStateDb->removeDir(relative1);
    } else {
        this->localStateDb->removeFile(relative1);
    }
}

void SafeDaemon::fileCopied(const QString &path1, const QString &path2)
{

}

void SafeDaemon::remoteFileAdded(QString id, QString pid, QString name)
{
    qDebug() << "[REMOTE EVENT] file added:" << name;
    QString dir = this->remoteStateDb->getDirPathById(pid);
    QString path = (dir == QString(QDir::separator()))
            ? name : (dir + QString(QDir::separator()) + name);
    SafeFile file(fetchFileInfo(id));

    this->remoteStateDb->removeFile(path);
    this->remoteStateDb->insertFile(dir, path, name, file.mtime, file.chksum, id);

    if(this->localStateDb->existsFile(path)) {
        return; // wait for cause (mtime, hash)
    }

    queueDownloadFile(id, getFilesystemPath() + QDir::separator() + path);
}

void SafeDaemon::remoteFileDeleted(QString id, QString pid, QString name)
{
    qDebug() << "[REMOTE EVENT] file deleted:" << name;
    QString dir = this->remoteStateDb->getDirPathById(pid);
    QString path = (dir == QString(QDir::separator()))
            ? name : (dir + QString(QDir::separator()) + name);
    this->remoteStateDb->removeFile(path);

    if(this->localStateDb->existsFile(path)) {
        this->localStateDb->removeFile(path);
        QFile(getFilesystemPath() + QDir::separator() + path).remove();
    }
}

void SafeDaemon::remoteDirectoryCreated(QString id, QString pid, QString name)
{
    qDebug() << "[REMOTE EVENT] directory created:" << name;
    QString dir = this->remoteStateDb->getDirPathById(pid);
    QString path = (dir == QString(QDir::separator()))
            ? name : (dir + QString(QDir::separator()) + name);
    SafeFile info(fetchDirInfo(id));

    this->remoteStateDb->removeDir(path);
    this->remoteStateDb->insertDir(path, name, info.mtime, id);

    if(this->localStateDb->existsDir(path)) {
        return;
    } else {
        this->localStateDb->insertDir(path, name, info.mtime, id);
    }

    QString dirPath(getFilesystemPath() + QDir::separator() + path);
    QDir().mkdir(dirPath);
    this->watcher->addRecursiveWatch(dirPath);
}

void SafeDaemon::remoteDirectoryDeleted(QString id, QString pid, QString name)
{
    qDebug() << "[REMOTE EVENT] directory deleted:" << name;
    QString path(this->remoteStateDb->getDirPathById(id));
    this->remoteStateDb->removeDirById(id);
    this->remoteStateDb->removeDirByIdRecursively(id);

    if (this->localStateDb->existsDir(path)){
        this->localStateDb->removeDir(path);
        this->localStateDb->removeDirRecursively(path);
    }

    if(path.length() > 1) {
        QDir dir(getFilesystemPath() + QDir::separator() + path);
        if (dir.exists()) {
            dir.removeRecursively();
        }
    }
}

void SafeDaemon::remoteFileMoved(QString id, QString pid1, QString n1, QString pid2, QString n2)
{
    QString dir1 = this->remoteStateDb->getDirPathById(pid1);
    QString dir2 = this->remoteStateDb->getDirPathById(pid2);
    QString path1 = (dir1 == QString(QDir::separator()))
            ? n1 : (dir1 + QString(QDir::separator()) + n1);
    QString path2 = (dir2 == QString(QDir::separator()))
            ? n2 : (dir2 + QString(QDir::separator()) + n2);

    qDebug() << "[REMOTE EVENT] file moved:" << path1 << "to" << path2;

    this->remoteStateDb->removeFileById(id);

    if (this->localStateDb->existsFile(path1)){
        this->localStateDb->removeFile(path1);
    }

    SafeFile file(fetchFileInfo(id));
    this->localStateDb->insertFile(dir2, path2, file.name, file.mtime, file.chksum, id);

    QDir().rename(getFilesystemPath() + QDir::separator() + path1,
                  getFilesystemPath() + QDir::separator() + path2);
}

void SafeDaemon::remoteDirectoryMoved(QString id, QString pid1, QString n1, QString pid2, QString n2)
{
    QString dir1 = this->remoteStateDb->getDirPathById(pid1);
    QString dir2 = this->remoteStateDb->getDirPathById(pid2);
    QString path1 = (dir1 == QString(QDir::separator()))
            ? n1 : (dir1 + QString(QDir::separator()) + n1);
    QString path2 = (dir2 == QString(QDir::separator()))
            ? n2 : (dir2 + QString(QDir::separator()) + n2);

    qDebug() << "[REMOTE EVENT] directory moved:" << path1 << "to" << path2;

    this->remoteStateDb->removeDirById(id);

    if (this->localStateDb->existsDir(path1)){
        this->localStateDb->removeDir(path1);
    }

    SafeDir info(fetchDirInfo(id));
    this->localStateDb->insertDir(path2, info.name, info.mtime, id);

    QDir().rename(getFilesystemPath() + QDir::separator() + path1,
                  getFilesystemPath() + QDir::separator() + path2);
}

QString SafeDaemon::createDir(const QString &parent_id, const QString &path)
{
    QEventLoop loop;
    QString dirId;

    auto api = this->apiFactory->newApi();
    connect(api, &SafeApi::makeDirComplete, [&](ulong id, ulong dir_id){
        qDebug() << "Created remote directory:" << dir_id << "in" << parent_id;
        dirId = dir_id;
        loop.exit();
    });
    connect(api, &SafeApi::errorRaised, [&](ulong id, quint16 code, QString text){
        qWarning() << "Error creating dir:" << text << "(" << code << ")";
        loop.exit();
    });

    api->makeDir(parent_id, QDir(path).dirName());
    loop.exec();
    return dirId;
}

void SafeDaemon::remoteRemoveDir(const QFileInfo &info)
{
    QEventLoop loop;
    QString relative(relativeFilePath(info));
    QString id(this->remoteStateDb->getDirId(relative));
    if(id.isEmpty()) {
        qWarning() << "Directory" << relative << "isn't exists in the remote index";
        return;
    }

    auto api = this->apiFactory->newApi();
    connect(api, &SafeApi::removeDirComplete, [&](ulong id){
        qDebug() << "Remote directory deleted:" << relative;
        loop.exit();
    });
    connect(api, &SafeApi::errorRaised, [&](ulong id, quint16 code, QString text){
        qWarning() << "Error deleting remote dir:" << text << "(" << code << ")";
        loop.exit();
    });

    api->removeDir(id, true, true);
    loop.exec();
}

void SafeDaemon::queueUploadFile(const QString &dir_id, const QFileInfo &info)
{
    QTimer *timer = new QTimer(this);
    QString path(info.filePath());

    timer->setInterval(2000);
    timer->setSingleShot(true);
    timer->setTimerType(Qt::VeryCoarseTimer);

    bool active = this->activeTransfers.contains(path);
    if(active) {
        finishTransfer(path);
    }

    connect(timer, &QTimer::timeout, [=](){
        uploadFile(dir_id, info);
    });

    bool queued = this->pendingTransfers.contains(path);
    if(queued) {
        this->pendingTransfers[path]->stop();
        this->pendingTransfers.take(path)->deleteLater();
    }
    this->pendingTransfers.insert(path, timer);
    timer->start();
}

void SafeDaemon::uploadFile(const QString &dir_id, const QFileInfo &info)
{
    QString path(info.filePath());
    auto api = this->apiFactory->newApi();

    connect(api, &SafeApi::pushFileProgress, [=](ulong id, ulong bytes, ulong totalBytes){
        qDebug() << "U/Progress:" << bytes << "/" << totalBytes;
    });
    connect(api, &SafeApi::pushFileComplete, [=, this](ulong id, SafeFile fileInfo) {
        qDebug() << "New file uploaded:" << fileInfo.name;
        finishTransfer(path);
    });
    connect(api, &SafeApi::errorRaised, [=](ulong id, quint16 code, QString text){
        qWarning() << "Error uploading:" << text << "(" << code << ")";
        finishTransfer(path);
    });

    this->activeTransfers[path] = api;
    api->pushFile(dir_id, path, info.fileName(), true);
}

void SafeDaemon::queueDownloadFile(const QString &id, const QFileInfo &info)
{
    QTimer *timer = new QTimer(this);
    QString path(info.filePath());

    timer->setInterval(2000);
    timer->setSingleShot(true);
    timer->setTimerType(Qt::VeryCoarseTimer);

    bool active = this->activeTransfers.contains(path);
    if(active) {
        finishTransfer(path);
    }

    connect(timer, &QTimer::timeout, [=](){
        downloadFile(id, info);
    });

    bool queued = this->pendingTransfers.contains(path);
    if(queued) {
        this->pendingTransfers[path]->stop();
        this->pendingTransfers.take(path)->deleteLater();
    }
    this->pendingTransfers.insert(path, timer);
    timer->start();
}

void SafeDaemon::downloadFile(const QString &id, const QFileInfo &info)
{
    QString path(info.filePath());
    auto api = this->apiFactory->newApi();

    connect(api, &SafeApi::pullFileProgress, [=](ulong id, ulong bytes, ulong totalBytes){
        qDebug() << "D/Progress:" << bytes << "/" << totalBytes;
    });
    connect(api, &SafeApi::pullFileComplete, [=, this](ulong id) {
        qDebug() << "File downloaded:" << path;
        finishTransfer(path);
        QString file_id = this->remoteStateDb->getFileId(relativeFilePath(info));
        this->localStateDb->insertFile(relativePath(info), relativeFilePath(info),
                                       info.fileName(), getMtime(info), makeHash(info), file_id);
    });
    connect(api, &SafeApi::errorRaised, [=](ulong id, quint16 code, QString text){
        qWarning() << "Error downloading:" << text << "(" << code << ")";
        finishTransfer(path);
    });

    this->activeTransfers[path] = api;
    api->pullFile(id, path);
}

void SafeDaemon::remoteRemoveFile(const QFileInfo &info)
{
    QString path(info.filePath());
    finishTransfer(path);
    QString id(this->remoteStateDb->getFileId(relativeFilePath(info)));
    if(id.isEmpty()) {
        qWarning() << "File" << relativeFilePath(info) << "isn't exists in the remote index";
        return;
    }

    auto api = this->apiFactory->newApi();
    connect(api, &SafeApi::removeFileComplete, [=, this](ulong id){
        qDebug() << "Remote file deleted" << path;
        finishTransfer(path);
    });
    connect(api, &SafeApi::errorRaised, [&](ulong id, quint16 code, QString text){
        qWarning() << "Error deleting:" << text << "(" << code << ")";
        finishTransfer(path);
    });

    this->activeTransfers.insert(path, api);
    api->removeFile(id, true);
}

bool SafeDaemon::isFileAllowed(const QFileInfo &info) {
    return !info.isHidden();
}

QString SafeDaemon::makeHash(const QFileInfo &info)
{
    QFile file(info.filePath());
    if(!file.open(QFile::ReadOnly)) {
        return QString();
    }
    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(&file);
    return hash.result().toHex();
}

QString SafeDaemon::makeHash(const QString &str)
{
    QString hash(QCryptographicHash::hash(
                     str.toUtf8(), QCryptographicHash::Md5).toHex());
    return hash;
}

void SafeDaemon::updateDirHash(const QDir &dir)
{
    this->localStateDb->updateDirHash(relativeFilePath(
                                          QFileInfo(dir.absolutePath())));
}

ulong SafeDaemon::getMtime(const QFileInfo &info)
{
    return info.lastModified().toTime_t();
}

void SafeDaemon::fullIndex(const QDir &dir)
{
    qDebug() << "Doing full local index";
    QMap<QString, QPair<QString, ulong> > dir_index;
    QDirIterator iterator(dir.absolutePath(), QDirIterator::Subdirectories);
    struct s {
        ulong space = 0;
        ulong files = 0;
        ulong dirs = 0;
    } stats;

    while (iterator.hasNext()) {
        iterator.next();
        if(iterator.fileName() == "." || iterator.fileName() == "..") {
            continue;
        }
        auto info = iterator.fileInfo();
        if(info.isSymLink()) {
            continue;
        }
        QString relative(relativeFilePath(info));
        if (!info.isDir()) {
            stats.space += info.size();
            auto hash = makeHash(info);
            auto mtime = getMtime(info);
            auto dirPath = info.absolutePath();
            //index file
            stats.files++;
            if(!this->remoteStateDb->existsFile(relative)
                    && !this->localStateDb->existsFile(relative)){
                if(!remoteStateDb->existsDir(relativePath(info))) {
                    prepareTree(info, relativeFilePath(dir.path()));
                }

                emit fileAdded(info.filePath(), false);
            }
            this->localStateDb->insertFile(
                        relativePath(info),
                        relative,
                        info.fileName(),
                        mtime, hash);

            if(!dir_index.contains(dirPath)){
                // push dir
                dir_index.insert(dirPath, QPair<QString, ulong>(
                                     hash, mtime));
                continue;
            }
            dir_index[dirPath].first.append(hash);
            if(mtime > dir_index[dirPath].second) {
                dir_index[dirPath].second = mtime;
            }
        } else if (QDir(info.filePath()).count() < 3) {
            // index empty dir
            stats.dirs++;
            if(!this->remoteStateDb->existsDir(relative)
                    && !this->localStateDb->existsDir(relative) ){
                emit fileAdded(info.filePath(), true);
            }
            this->localStateDb->insertDir(relativeFilePath(info),
                                          info.dir().dirName(),
                                          getMtime(info));
        }
    }

    foreach(auto k, dir_index.keys()) {
        QString relative = relativeFilePath(k);
        if(relative.isEmpty()) {
            continue;
        }

        // index dir
        stats.dirs++;
        if(!this->remoteStateDb->existsDir(relative)
                && !this->localStateDb->existsDir(relative)){

            emit fileAdded(getFilesystemPath() + QDir::separator() + k, true);
        }
        localStateDb->insertDir(relative, QDir(k).dirName(),
                                dir_index[k].second, makeHash(dir_index[k].first));
    }
    qDebug() << "MBs:" << stats.space / (1024.0 * 1024.0)
             << "\nFiles:" << stats.files <<
                "\nDirs:" << stats.dirs;
}

void SafeDaemon::fullRemoteIndex()
{
    QEventLoop loop;
    auto api = this->apiFactory->newApi();
    uint counter = 0;

    connect(api, &SafeApi::listDirComplete, [&](ulong id, QList<SafeDir> dirs,
            QList<SafeFile> files, QJsonObject root_info){
        bool root = false;
        QString tree = root_info.value("tree").toString();
        tree.remove(0, 1);
        tree.chop(1);
        if(tree.isEmpty()){
            root = true;
            tree = QString(QDir::separator());
        }

        // index root
        if(root)
            remoteStateDb->insertDir(tree, tree, 0, root_info.value("id").toString());

        foreach(SafeFile file, files) {
            if(file.is_trash) {
                continue;
            }
            // index file
            remoteStateDb->insertFile(tree, root ? file.name : (tree + QDir::separator() + file.name),
                                      file.name, file.mtime, file.chksum, file.id);
        }

        foreach(SafeDir dir, dirs) {
            if(dir.is_trash || !dir.special_dir.isEmpty()) {
                continue;
            }
            ++counter;
            // index dir
            remoteStateDb->insertDir(root ? dir.name : (tree + QDir::separator() + dir.name),
                                     dir.name, dir.mtime, dir.id);
            api->listDir(dir.id);
        }

        --counter; // dir parsed
        if(counter < 1) {
            // no more recursion
            loop.exit();
            return;
        }
    });
    connect(api, &SafeApi::errorRaised, [&](ulong id, quint16 code, QString text){
        qWarning() << "Error remote indexing:" << text << "(" << code << ")";
        --counter;
        if(counter < 1)
            loop.exit();

    });

    ++counter;
    api->listDir(fetchDirId("/"));
    loop.exec();

    qDebug() << "Finished remote indexing";
}

void SafeDaemon::checkIndex(const QDir &dir)
{

}

QString SafeDaemon::relativeFilePath(const QFileInfo &info)
{
    QString relative = QDir(getFilesystemPath()).relativeFilePath(info.filePath());
    return relative.isEmpty() ? "/" : relative;
}

QString SafeDaemon::fetchDirId(const QString &path)
{
    QString dirId;
    QEventLoop loop;
    auto api = this->apiFactory->newApi();
    connect(api, &SafeApi::getPropsComplete, [&](ulong id, QJsonObject props){
        auto info = props.value("object").toObject();
        dirId = SafeDir(info).id;
        loop.exit();
    });
    connect(api, &SafeApi::errorRaised, [&](ulong id, quint16 code, QString text){
        qWarning() << "Error getting props:" << text << "(" << code << ")";
        loop.exit();
    });

    api->getProps(path, true);
    loop.exec();
    return dirId;
}

QString SafeDaemon::relativePath(const QFileInfo &info)
{
    QString relative;
    if(info.isDir()) {
        relative = QDir(getFilesystemPath()).relativeFilePath(info.dir().path());
    } else {
        relative = QDir(getFilesystemPath()).relativeFilePath(info.path());
    }
    return relative.isEmpty() ? "/" : relative;
}
