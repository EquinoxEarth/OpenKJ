#include "directorymonitor.h"
#include "directorylisting.h"
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent>

DirectoryMonitor::DirectoryMonitor(QObject *parent, QStringList pathsToWatch) : QObject(parent)
{
    m_scanTimer.setInterval(5000);
    m_scanTimer.setSingleShot(true);
    connect(&m_scanTimer, &QTimer::timeout, this, &DirectoryMonitor::scanPaths);

    connect(&m_pathsEnumeratedWatcher, &QFutureWatcher<QStringList>::finished, this, &DirectoryMonitor::directoriesEnumerated);
    auto future = QtConcurrent::run(&DirectoryMonitor::enumeratePathsAsync, pathsToWatch);
    m_pathsEnumeratedWatcher.setFuture(future);
}

QStringList DirectoryMonitor::enumeratePathsAsync(const QStringList &paths)
{
    QStringList result;
    foreach (auto path, paths) {
        QFileInfo finfo(path);
        if (finfo.isDir() && finfo.isReadable())
        {
            result.append(path);
            for (const auto &entry : okj::listDirectoriesRecursively(path)) {
                result.append(entry.filePath());
            }
        }
    }
    return result;
}

void DirectoryMonitor::directoriesEnumerated()
{
    auto paths = m_pathsEnumeratedWatcher.future().result();
    if (!paths.isEmpty())
        m_fsWatcher.addPaths(paths);
    connect(&m_fsWatcher, &QFileSystemWatcher::directoryChanged, this, &DirectoryMonitor::directoryChanged);
}

void DirectoryMonitor::directoryChanged(const QString& dirPath)
{
    qInfo() << "Directory changed fired for dir: " << dirPath;
    m_pathsWithChangedFiles << dirPath;
    m_scanTimer.start();
}

void DirectoryMonitor::scanPaths()
{
    auto paths = m_pathsWithChangedFiles.values();
    m_pathsWithChangedFiles.clear();

    if (!paths.isEmpty())
        emit scanRequested(paths);
}
