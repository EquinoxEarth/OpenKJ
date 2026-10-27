#ifndef OPENKJ_DIRECTORYLISTING_H
#define OPENKJ_DIRECTORYLISTING_H

#include <QDirListing>

namespace okj {
// The karaoke scan also visits directories to report progress through trees
// containing few files. Callers select regular files with DirEntry::isFile().
inline QDirListing listEntriesRecursively(const QString &path)
{
    using F = QDirListing::IteratorFlag;
    return QDirListing(path, F::Recursive | F::ResolveSymlinks);
}

// Preserve the scanners' filtering defaults: include links to files, exclude
// broken/special entries, and do not recurse into linked directories.
inline QDirListing listFilesRecursively(const QString &path)
{
    using F = QDirListing::IteratorFlag;
    return QDirListing(path, F::Recursive | F::FilesOnly | F::ResolveSymlinks);
}

// Directory watches include links to directories, but not their descendants.
inline QDirListing listDirectoriesRecursively(const QString &path)
{
    using F = QDirListing::IteratorFlag;
    return QDirListing(path, F::Recursive | F::DirsOnly | F::ResolveSymlinks);
}
}

#endif
