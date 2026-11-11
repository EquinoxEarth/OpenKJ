#include "okjversion.h"
#include "songshop.h"
#include "updatechecker.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QPointer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtEndian>
#include <cstring>
#include <spdlog/sinks/null_sink.h>
#include <zip.h>

struct Response {
    QByteArray body;
    int status{200};
    QByteArray length;
    QNetworkReply::NetworkError error{QNetworkReply::NoError};
};

class ScriptedReply : public QNetworkReply
{
public:
    ScriptedReply(const QNetworkRequest &request, const Response &response, QObject *parent)
        : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
        if (!response.length.isEmpty())
            setRawHeader("Content-Length", response.length);
        open(ReadOnly | Unbuffered);
        QTimer::singleShot(0, this, [this, response] {
            if (isFinished())
                return;
            const auto middle = response.body.size() / 2;
            m_bytes = response.body.first(middle);
            emit readyRead();
            if (isFinished())
                return;
            m_bytes += response.body.sliced(middle);
            if (response.error != NoError)
                setError(response.error, "Scripted network error");
            setFinished(true);
            emit finished();
        });
    }

    void abort() override
    {
        if (isFinished())
            return;
        setError(OperationCanceledError, "Scripted request aborted");
        setFinished(true);
        emit finished();
    }

    qint64 bytesAvailable() const override { return m_bytes.size() + QNetworkReply::bytesAvailable(); }

protected:
    qint64 readData(char *destination, qint64 maxSize) override
    {
        const auto count = qMin<qint64>(m_bytes.size(), maxSize);
        if (count == 0)
            return isFinished() ? -1 : 0;
        std::memcpy(destination, m_bytes.constData(), count);
        m_bytes.remove(0, count);
        return count;
    }

private:
    QByteArray m_bytes;
};

class ScriptedNetwork : public QNetworkAccessManager
{
public:
    QList<Response> responses;
    QList<QNetworkRequest> requests;
    QPointer<QNetworkReply> latest;

protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        requests.append(request);
        latest = new ScriptedReply(request, responses.isEmpty() ? Response{{}, 500} : responses.takeFirst(), this);
        return latest;
    }
};

class NetworkHandlingTest : public QObject
{
    Q_OBJECT

    std::unique_ptr<Settings> settings;
    QString savedFolder;
    int savedBranch{0};
    bool savedChecks{false};

    static QString assetSuffix()
    {
#if defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
        return "macos-arm64.dmg";
#elif defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
        return "windows-x64.msi";
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
        return "linux-x86_64.flatpak";
#else
        return {};
#endif
    }

    static QJsonObject release(const QString &version, const QString &tag, bool preview = false,
                               bool draft = false, bool compatible = true)
    {
        const auto asset = QStringLiteral("OpenKJ-%1-%2").arg(version, compatible ? assetSuffix() : "other.zip");
        return {{"tag_name", tag}, {"draft", draft}, {"prerelease", preview},
                {"assets", QJsonArray{QJsonObject{{"name", asset}, {"state", "uploaded"}, {"size", 1234}}}}};
    }

    static QByteArray mp4()
    {
        // Minimal ISO BMFF metadata with an audio track and media box.
        const auto box = [](const QByteArray &type, const QByteArray &payload) {
            char length[4];
            qToBigEndian<quint32>(8 + payload.size(), length);
            return QByteArray(length, 4) + type + payload;
        };
        QByteArray header(24, '\0');
        qToBigEndian<quint32>(48000, header.data() + 12);
        qToBigEndian<quint32>(48000, header.data() + 16);
        const auto handler = QByteArray(8, '\0') + "soun" + QByteArray(12, '\0');
        const auto track = box("trak", box("mdia", box("mdhd", header) + box("hdlr", handler)));
        return box("ftyp", QByteArray("isom\0\0\0\0", 8)) + box("moov", track) + box("mdat", QByteArray(8, 'x'));
    }

    static void putFile(const QString &path, const QByteArray &body)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(body), body.size());
    }

    static void submitPurchase(SongShop &shop, ScriptedNetwork &network, bool zip = false,
                               const QByteArray &order = "{\"result\":\"SUCCESS\",\"download_links\":[\"https://cdn.example.test/song\"]}")
    {
        const auto requestCount = network.requests.size();
        network.responses.prepend({order});
        network.responses.prepend({"{\"result\":\"SUCCESS\",\"session_id\":\"mock-session\"}"});
        QSignalSpy loggedIn(&shop, &SongShop::knLoginSuccess);
        shop.knLogin("host+example@example.test", "test-password");
        QTRY_COMPARE(loggedIn.size(), 1);
        shop.knPurchase(zip ? "PY123" : "V123", "AC/DC", "曲: Live", "test-card", "01", "30", "test-cvv");
        QCOMPARE(network.requests.at(requestCount).url().scheme(), "https");
        QVERIFY(network.requests.at(requestCount).url().query(QUrl::FullyEncoded).contains("host%2Bexample"));
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        settings = std::make_unique<Settings>();
        savedFolder = settings->storeDownloadDir();
        savedBranch = settings->updatesBranch();
        savedChecks = settings->checkUpdates();
        spdlog::null_logger_mt("logger");
    }

    void cleanupTestCase()
    {
        settings->setStoreDownloadDir(savedFolder);
        settings->setUpdatesBranch(savedBranch);
        settings->setCheckUpdates(savedChecks);
    }

    void updateChecksFilterPlatformAndChannelAndCompareVersions()
    {
        if (assetSuffix().isEmpty())
            QSKIP("No release installer for this build");
        settings->setCheckUpdates(true);
        settings->setUpdatesBranch(0);
        ScriptedNetwork network;
        UpdateChecker checker(nullptr, &network);
        QSignalSpy updates(&checker, &UpdateChecker::newVersionAvailable);
        const auto prefix = QString::number(OKJ_VERSION_MAJOR + 1) + ".0.";
        const auto older = prefix + "9";
        const auto regular = prefix + "10";
        const auto preview = prefix + "11";
        const auto regularTag = "v" + regular + "-beta.2";
        const QJsonArray releases{
            release(regular, regularTag),
            release(older, "v" + older + "-beta.10"),
            release(regular, "v" + regular + "-beta.1"),
            release(preview, "v" + preview + "-preview.1", true),
            release("30000.0.0", "v30000.0.0-beta.1", false, true),
            release("40000.0.0", "v40000.0.0-beta.1", false, false, false),
            release("50000.0.0", "v50000.0.0-beta.1/other"),
            release("9999999999999999.0.0", "v9999999999999999.0.0"),
            release(OKJ_VERSION_STRING, "v" OKJ_VERSION_STRING "-beta.99")
        };
        const auto data = QJsonDocument(releases).toJson();
        network.responses.append({data});
        checker.checkForUpdates();
        checker.checkForUpdates();
        QCOMPARE(network.requests.size(), 1);
        QTRY_COMPARE(updates.size(), 1);
        QCOMPARE(updates.first().at(0).toString(), regular);
        QCOMPARE(updates.first().at(1).toUrl(), QUrl("https://github.com/EquinoxEarth/OpenKJ/releases/tag/" + regularTag));
        QVERIFY(!updates.first().at(2).toBool());
        QCOMPARE(network.requests.first().url().host(), "api.github.com");
        QCOMPARE(network.requests.first().url().scheme(), "https");
        QTRY_VERIFY(network.latest.isNull());

        settings->setUpdatesBranch(1);
        network.responses.append({data});
        checker.checkForUpdates();
        QTRY_COMPARE(updates.size(), 2);
        QCOMPARE(updates.last().at(0).toString(), preview);
        QVERIFY(updates.last().at(2).toBool());
        QTRY_VERIFY(network.latest.isNull());
    }

    void updateErrorsAndMalformedResponsesCleanUpAndAllowRetry()
    {
        if (assetSuffix().isEmpty())
            QSKIP("No release installer for this build");
        settings->setCheckUpdates(false);
        ScriptedNetwork network;
        UpdateChecker checker(nullptr, &network);
        QSignalSpy updates(&checker, &UpdateChecker::newVersionAvailable);
        checker.checkForUpdates();
        QVERIFY(network.requests.isEmpty());
        settings->setCheckUpdates(true);

        const QList<Response> rejected{
            {"[]", 503},
            {"[", 200},
            {"{\"tag_name\":\"v99.0.0\"}"},
            {"[]", 200, {}, QNetworkReply::TimeoutError},
            {QJsonDocument(QJsonArray{release(OKJ_VERSION_STRING, "v" OKJ_VERSION_STRING "-beta.99")}).toJson()}
        };
        for (const auto &response : rejected) {
            network.responses.append(response);
            checker.checkForUpdates();
            QVERIFY(network.latest);
            QTRY_VERIFY(network.latest.isNull());
            QCOMPARE(updates.size(), 0);
        }
        QCOMPARE(network.requests.size(), rejected.size());
    }

    void failedSongsNeverOverwriteOrAnnounceDownload_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("length");
        QTest::addColumn<int>("error");
        QTest::newRow("server-error") << mp4() << 503 << QByteArray() << int(QNetworkReply::NoError);
        QTest::newRow("partial-content") << mp4() << 206 << QByteArray() << int(QNetworkReply::NoError);
        QTest::newRow("timeout") << mp4().first(20) << 200 << QByteArray() << int(QNetworkReply::TimeoutError);
        QTest::newRow("length-mismatch") << mp4() << 200 << QByteArray("100") << int(QNetworkReply::NoError);
        QTest::newRow("truncated-box") << mp4().chopped(2) << 200 << QByteArray() << int(QNetworkReply::NoError);
        QTest::newRow("html") << QByteArray("<html>Unauthorized</html>") << 200 << QByteArray() << int(QNetworkReply::NoError);
        QTest::newRow("empty") << QByteArray() << 200 << QByteArray() << int(QNetworkReply::NoError);
    }

    void failedSongsNeverOverwriteOrAnnounceDownload()
    {
        QFETCH(QByteArray, body);
        QFETCH(int, status);
        QFETCH(QByteArray, length);
        QFETCH(int, error);
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        settings->setStoreDownloadDir(folder.path());
        const auto path = folder.filePath("V123 - AC_DC - 曲_ Live.mp4");
        putFile(path, "previous song");
        ScriptedNetwork network;
        network.responses.append({body, status, length, static_cast<QNetworkReply::NetworkError>(error)});
        SongShop shop(nullptr, &network);
        QSignalSpy downloaded(&shop, &SongShop::karaokeSongDownloaded);
        QSignalSpy failed(&shop, &SongShop::downloadFailed);
        submitPurchase(shop, network);
        QTRY_COMPARE(failed.size(), 1);
        QTRY_VERIFY(network.latest.isNull());
        QVERIFY(downloaded.isEmpty());
        QFile previous(path);
        QVERIFY(previous.open(QIODevice::ReadOnly));
        QCOMPARE(previous.readAll(), "previous song");
        QCOMPARE(QDir(folder.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
    }

    void songDestinationMustBeWritable()
    {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const auto blocker = folder.filePath("blocker");
        putFile(blocker, "regular file");
        settings->setStoreDownloadDir(blocker);
        ScriptedNetwork network;
        SongShop shop(nullptr, &network);
        QSignalSpy downloaded(&shop, &SongShop::karaokeSongDownloaded);
        QSignalSpy failed(&shop, &SongShop::downloadFailed);
        submitPurchase(shop, network);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(network.requests.size(), 2);
        QVERIFY(downloaded.isEmpty());

        settings->setStoreDownloadDir(folder.path());
        QVERIFY(QDir().mkdir(folder.filePath("V123 - AC_DC - 曲_ Live.mp4")));
        network.responses.append({mp4()});
        submitPurchase(shop, network);
        QTRY_COMPARE(failed.size(), 2);
        QVERIFY(downloaded.isEmpty());
        QCOMPARE(QDir(folder.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"blocker"});
    }

    void successfulDownloadsAreValidatedBeforeBeingAnnounced()
    {
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const auto destination = folder.filePath("music");
        settings->setStoreDownloadDir(destination); // No trailing slash, folder created on demand.
        ScriptedNetwork network;
        SongShop shop(nullptr, &network);
        QSignalSpy downloaded(&shop, &SongShop::karaokeSongDownloaded);
        QSignalSpy failed(&shop, &SongShop::downloadFailed);
        const auto video = mp4();
        network.responses.append({video, 200, QByteArray::number(video.size())});
        submitPurchase(shop, network);
        QTRY_COMPARE(downloaded.size(), 1);
        QFile saved(downloaded.first().first().toString());
        QCOMPARE(saved.fileName(), QDir(destination).filePath("V123 - AC_DC - 曲_ Live.mp4"));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), video);

        const auto fixture = folder.filePath("fixture.zip");
        auto *archive = zip_open(QFile::encodeName(fixture).constData(), ZIP_CREATE, nullptr);
        QVERIFY(archive);
        for (const char *entry : {"song.cdg", "song.mp3"}) {
            auto *source = zip_source_buffer(archive, "payload", 7, 0);
            QVERIFY(source);
            QVERIFY(zip_file_add(archive, entry, source, 0) >= 0);
        }
        QCOMPARE(zip_close(archive), 0);
        QFile file(fixture);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto zip = file.readAll();
        network.responses.append({zip.chopped(4)});
        submitPurchase(shop, network, true);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(downloaded.size(), 1);
        network.responses.append({zip});
        submitPurchase(shop, network, true);
        QTRY_COMPARE(downloaded.size(), 2);
        QFile savedZip(downloaded.last().first().toString());
        QVERIFY(savedZip.open(QIODevice::ReadOnly));
        QCOMPARE(savedZip.readAll(), zip);
        QCOMPARE(QDir(destination).entryList(QDir::Files | QDir::Hidden).size(), 2);
    }

    void insecureLinksAndLoginFailureAreReportedWithoutAStuckRequest()
    {
        ScriptedNetwork network;
        SongShop shop(nullptr, &network);
        QSignalSpy loginFailures(&shop, &SongShop::knLoginFailure);
        QSignalSpy downloadFailures(&shop, &SongShop::downloadFailed);
        network.responses.append({{}, 200, {}, QNetworkReply::TimeoutError});
        shop.knLogin("user", "password");
        QTRY_COMPARE(loginFailures.size(), 1);
        QTRY_VERIFY(network.latest.isNull());

        submitPurchase(shop, network, false,
                       "{\"result\":\"SUCCESS\",\"download_links\":[\"http://cdn.example.test/song\"]}");
        QTRY_COMPARE(downloadFailures.size(), 1);
        QCOMPARE(network.requests.size(), 3);
        QTRY_VERIFY(network.latest.isNull());
    }
};

QTEST_GUILESS_MAIN(NetworkHandlingTest)
#include "network_handling_test.moc"
