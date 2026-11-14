#include "../src/databaseschema.h"
#include "../src/regularsimport.h"

#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

class RegularsImportTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_directory;
    QSqlDatabase m_db;

    void exec(const QString &sql)
    {
        QSqlQuery query(m_db);
        QVERIFY2(query.exec(sql), qPrintable(query.lastError().text()));
    }

    QStringList history()
    {
        QSqlQuery query(m_db);
        if (!query.exec("SELECT filepath FROM historySongs ORDER BY id")) {
            QTest::qFail(qPrintable(query.lastError().text()), __FILE__, __LINE__);
            return {};
        }
        QStringList paths;
        while (query.next())
            paths.append(query.value(0).toString());
        return paths;
    }

    QString createFile(const QString &name, const QByteArray &contents = {})
    {
        QFile file(m_directory.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) {
            QTest::qFail(qPrintable(file.errorString()), __FILE__, __LINE__);
            return {};
        }
        return file.fileName();
    }

    void addLibrarySong(const QString &path, const QString &discId, const QString &title)
    {
        QSqlQuery query(m_db);
        QVERIFY(query.prepare("INSERT INTO dbsongs (artist, title, discid, path) VALUES ('Artist', ?, ?, ?)"));
        query.addBindValue(title);
        query.addBindValue(discId);
        query.addBindValue(path);
        QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
    }

private slots:
    void init()
    {
        QVERIFY(m_directory.isValid());
        m_db = QSqlDatabase::addDatabase("QSQLITE", "regulars-import-test");
        const auto error = okj::openAndMigrateDatabase(m_db,
            m_directory.filePath(QString::fromLatin1(QTest::currentTestFunction()) + ".sqlite"));
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void cleanup()
    {
        m_db = {};
        QSqlDatabase::removeDatabase("regulars-import-test");
    }

    void failedReplaceRollsBackEntireBatch()
    {
        exec("INSERT INTO historySingers (id, name) VALUES (1, 'Singer')");
        exec("INSERT INTO historySongs (historySinger, filepath) VALUES (1, '/original.zip')");
        exec("CREATE TRIGGER reject_song BEFORE INSERT ON historySongs WHEN NEW.filepath = '/reject.zip' "
             "BEGIN SELECT RAISE(ABORT, 'injected insert failure'); END");
        const auto file = createFile("replace.json", R"([
            {"name":"New singer","songs":[{"filepath":"/other.zip"}]},
            {"name":"singer","songs":[{"filepath":"/first.zip"},{"filepath":"/reject.zip"}]}
        ])");
        auto imported = RegularsImport::readExport(file);
        QVERIFY2(imported.error.isEmpty(), qPrintable(imported.error));
        imported.singers.last().existingId = 1;
        imported.singers.last().replace = true;

        auto result = RegularsImport::importSingers(m_db, imported.singers, {}, false);
        QVERIFY(result.error.contains("injected insert failure"));
        QCOMPARE(result.imported, 0);
        QCOMPARE(history(), QStringList{"/original.zip"});
        QSqlQuery query(m_db);
        QVERIFY(query.exec("SELECT name FROM historySingers"));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), QString("Singer"));
        QVERIFY(!query.next());
        query.finish();

        exec("DROP TRIGGER reject_song");
        result = RegularsImport::importSingers(m_db, imported.singers, {}, false);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.imported, 2);
        QCOMPARE(history(), (QStringList{"/other.zip", "/first.zip", "/reject.zip"}));
    }

    void legacyQuotedTitlesAndUnresolvedMatches()
    {
        const auto exact = createFile("exact.zip");
        addLibrarySong(exact, "DISC-1", "A \"quoted\" title");
        addLibrarySong(createFile("ambiguous1.zip"), "DISC-2", "Ambiguous");
        addLibrarySong(createFile("ambiguous2.zip"), "DISC-2", "Ambiguous");
        addLibrarySong(m_directory.filePath("offline.zip"), "DISC-3", "Offline");

        QByteArray xml = R"(<regulars><singer name="One">
            <song artist="Artist" title="A &quot;quoted&quot; title" discid="DISC-1" key="-2"/>
            <song artist="Artist" title="Ambiguous" discid="DISC-2" key="0"/>
            <song artist="Artist" title="Offline" discid="DISC-3" key="0"/>
        </singer>)";
        for (int index = 0; index < 1000; ++index)
            xml += "<singer name=\"Singer " + QByteArray::number(index) + "\"/>";
        xml += "</regulars>";
        const auto imported = RegularsImport::readExport(createFile("legacy.xml", xml));
        QVERIFY2(imported.error.isEmpty(), qPrintable(imported.error));
        QCOMPARE(imported.singers.size(), 1001);
        const auto result = RegularsImport::importSingers(m_db, imported.singers, {}, true);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.imported, 1);
        QCOMPARE(result.warnings.size(), 2);
        QCOMPARE(history(), QStringList{exact});
    }
};

QTEST_GUILESS_MAIN(RegularsImportTest)
#include "regulars_import_test.moc"
