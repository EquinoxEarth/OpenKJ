#ifndef ZIPFILEPATH_H
#define ZIPFILEPATH_H

#include <QFile>

namespace okj {
inline QByteArray zipFilePath(const QString &path)
{
#ifdef Q_OS_WIN
    // libzip's Windows file source expects UTF-8 regardless of the code page.
    return path.toUtf8();
#else
    return QFile::encodeName(path);
#endif
}
}

#endif
