#include "directorylisting.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator> // Migration oracle only; application code uses QDirListing.
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void touch(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot create fixture file");
}

void markHidden(const QString &path)
{
#ifdef Q_OS_WIN
    const auto wide = path.toStdWString();
    const auto attributes = GetFileAttributesW(wide.c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES &&
            SetFileAttributesW(wide.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN), "Cannot mark fixture hidden");
#else
    Q_UNUSED(path); // Dot-prefixed fixture names are hidden on Unix.
#endif
}

std::filesystem::path nativePath(const QString &path)
{
#ifdef Q_OS_WIN
    return path.toStdWString();
#else
    return path.toStdString();
#endif
}

bool link(const QString &target, const QString &path, bool directory = false)
{
    std::error_code error;
    if (directory)
        std::filesystem::create_directory_symlink(nativePath(target), nativePath(path), error);
    else
        std::filesystem::create_symlink(nativePath(target), nativePath(path), error);
#ifndef Q_OS_WIN
    require(!error, "Cannot create symbolic-link fixture");
#endif
    return !error; // Windows may require Developer Mode or elevated symlink privileges.
}

QStringList relativePaths(QDirListing listing, const QDir &root)
{
    QStringList result;
    for (const auto &entry : listing)
        result.append(root.relativeFilePath(entry.filePath()));
    result.sort();
    return result;
}

QStringList legacyPaths(const QDir &root, bool directories)
{
    const auto filters = directories ? QDir::AllDirs | QDir::NoDotAndDotDot : QDir::AllEntries;
    QDirIterator iterator(root.path(), filters, QDirIterator::Subdirectories);
    QStringList result;
    while (iterator.hasNext()) {
        iterator.next();
        if (directories || !iterator.fileInfo().isDir())
            result.append(root.relativeFilePath(iterator.filePath()));
    }
    result.sort();
    return result;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temporary;
        QTemporaryDir outside;
        require(temporary.isValid() && outside.isValid(), "Cannot create isolated fixture directories");
        const QDir root(temporary.path());
        require(root.mkpath("nested/more") && root.mkpath("empty") && root.mkpath(".hidden-folder"),
                "Cannot create fixture directories");
        QStringList expectedFiles{"Song.ZIP", "nested/Track.MP3", "nested/more/曲名.cdg", "notes.txt"};
        QStringList expectedDirectories{"empty", "nested", "nested/more"};
        for (const auto &name : expectedFiles)
            touch(root.filePath(name));
        touch(root.filePath(".hidden.mp3"));
        touch(root.filePath(".hidden-folder/inside.mp3"));
        markHidden(root.filePath(".hidden.mp3"));
        markHidden(root.filePath(".hidden-folder"));
        touch(QDir(outside.path()).filePath("outside.mp3"));

        if (link(root.filePath("nested/Track.MP3"), root.filePath("linked.mp3")))
            expectedFiles.append("linked.mp3");
        if (link(outside.path(), root.filePath("linked-directory"), true))
            expectedDirectories.append("linked-directory");
        link(root.filePath("missing.mp3"), root.filePath("broken.mp3"));
#ifdef Q_OS_UNIX
        const auto fifo = QFile::encodeName(root.filePath("special.mp3"));
        require(mkfifo(fifo.constData(), 0600) == 0, "Cannot create special-file fixture");
#endif
        const auto previousFiles = legacyPaths(root, false);
        const auto previousDirectories = legacyPaths(root, true);
        // Hidden-entry handling can differ between platform Qt builds. Preserve
        // the existing iterator's result rather than changing import policy.
        if (previousFiles.contains(".hidden.mp3"))
            expectedFiles.append(".hidden.mp3");
        if (previousFiles.contains(".hidden-folder/inside.mp3"))
            expectedFiles.append(".hidden-folder/inside.mp3");
        if (previousDirectories.contains(".hidden-folder"))
            expectedDirectories.append(".hidden-folder");
        expectedFiles.sort();
        expectedDirectories.sort();
        require(relativePaths(okj::listFilesRecursively(root.path()), root) == previousFiles &&
                relativePaths(okj::listDirectoriesRecursively(root.path()), root) == previousDirectories,
                "The new iterator must preserve the previous scanner and watch results");
        QStringList karaokeFiles;
        for (const auto &entry : okj::listEntriesRecursively(root.path())) {
            if (entry.isFile())
                karaokeFiles.append(root.relativeFilePath(entry.filePath()));
        }
        karaokeFiles.sort();
        require(karaokeFiles == previousFiles, "Karaoke progress traversal must preserve the same file set");
        require(relativePaths(okj::listFilesRecursively(root.path()), root) == expectedFiles,
                "File scans must preserve recursive, hidden, symlink and special-file handling");
        require(relativePaths(okj::listDirectoriesRecursively(root.path()), root) == expectedDirectories,
                "Watch scans must list directories and directory links without following them");
        require(relativePaths(okj::listFilesRecursively(root.filePath("missing")), root).isEmpty() &&
                relativePaths(okj::listDirectoriesRecursively(root.filePath("missing")), root).isEmpty(),
                "Missing or disconnected roots must return no entries");
        require(relativePaths(okj::listFilesRecursively(root.filePath("empty")), root).isEmpty(),
                "Empty directories must return no files");
        std::puts("Recursive directory listing checks passed");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
