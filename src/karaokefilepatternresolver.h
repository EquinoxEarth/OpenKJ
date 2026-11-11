#ifndef KARAOKEFILEPATTERNRESOLVER_H
#define KARAOKEFILEPATTERNRESOLVER_H

#include "src/models/tablemodelkaraokesourcedirs.h"
#include "custompattern.h"
#include <QSqlDatabase>

class KaraokeFilePatternResolver
{

public:

    struct KaraokeFilePattern
    {
        SourceDir::NamingPattern pattern;
        CustomPattern customPattern;
    };

    KaraokeFilePatternResolver() = default;
    explicit KaraokeFilePatternResolver(const QSqlDatabase &db);

    const KaraokeFilePattern& getPattern(const QString &filename);

    static const KaraokeFilePattern& getDefaultPattern();

private:

    QSqlDatabase m_db;
    QMap<QString, KaraokeFilePattern> m_path_pattern_map;
    bool m_initialized {false};

    void InitializeData();

};

#endif // KARAOKEFILEPATTERNRESOLVER_H
