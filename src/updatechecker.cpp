#include "updatechecker.h"
#include "networkrequests.h"
#include "okjversion.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>

namespace {
const QString releasePageBase = QStringLiteral("https://github.com/EquinoxEarth/OpenKJ/releases/tag/");
const QString releaseApi = QStringLiteral("https://api.github.com/repos/EquinoxEarth/OpenKJ/releases?per_page=50");

QString platformAssetSuffix()
{
#if defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("macos-arm64.dmg");
#elif defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
    return QStringLiteral("windows-x64.msi");
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    return QStringLiteral("linux-x86_64.flatpak");
#else
    return {};
#endif
}

bool hasInstaller(const QJsonObject &release, const QString &filename)
{
    for (const auto &asset : release.value("assets").toArray()) {
        const auto object = asset.toObject();
        if (object.value("name").toString() == filename && object.value("state").toString() == "uploaded" &&
            object.value("size").toDouble() > 0)
            return true;
    }
    return false;
}

struct Release {
    QVersionNumber version;
    QString tag;
    int revision{0};
    bool preview{false};
};

Release availableUpdate(const QJsonArray &releases, bool allowPreviews, const QString &assetSuffix)
{
    static const QRegularExpression versionTag(QStringLiteral(
        R"(\A(?:(?:macos|windows|linux)-)?v((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))(?:-(?:beta|preview|prerelease)\.(0|[1-9][0-9]*))?\z)"));
    const QVersionNumber installed(OKJ_VERSION_MAJOR, OKJ_VERSION_MINOR, OKJ_VERSION_BUILD);
    Release best;
    for (const auto &entry : releases) {
        const auto release = entry.toObject();
        const auto draft = release.value("draft");
        const auto prerelease = release.value("prerelease");
        if (!draft.isBool() || draft.toBool() || !prerelease.isBool() ||
            (!allowPreviews && prerelease.toBool()))
            continue;

        const auto tag = release.value("tag_name").toString();
        const auto match = versionTag.match(tag);
        if (!match.hasMatch())
            continue;
        const auto versionString = match.captured(1);
        qsizetype suffix = 0;
        const auto version = QVersionNumber::fromString(versionString, &suffix);
        if (suffix != versionString.size() || version.segmentCount() != 3 || version <= installed)
            continue;
        bool revisionValid = true;
        const int revision = match.capturedLength(2) ? match.captured(2).toInt(&revisionValid) : 0;
        if (!revisionValid || !hasInstaller(release, QStringLiteral("OpenKJ-%1-%2").arg(versionString, assetSuffix)))
            continue;

        if (version > best.version || (version == best.version &&
            (best.preview != prerelease.toBool() ? best.preview : revision > best.revision))) {
            best = {version, tag, revision, prerelease.toBool()};
        }
    }
    return best;
}
}

UpdateChecker::UpdateChecker(QObject *parent, QNetworkAccessManager *network)
    : QObject(parent), m_network(network ? network : new QNetworkAccessManager(this))
{
}

void UpdateChecker::checkForUpdates()
{
    const auto assetSuffix = platformAssetSuffix();
    if (m_checking || !m_settings.checkUpdates() || assetSuffix.isEmpty())
        return;

    m_checking = true;
    auto request = okj::httpsRequest(QUrl(releaseApi));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "OpenKJ/" OKJ_VERSION_STRING);
    auto *reply = m_network->get(request);
    okj::readJsonReply(reply, this, 8 * 1024 * 1024, [this, assetSuffix](const QJsonDocument &response) {
        m_checking = false;
        if (!response.isArray()) {
            qWarning("Could not check the OpenKJ GitHub releases for updates.");
            return;
        }
        if (!m_settings.checkUpdates())
            return;
        const auto update = availableUpdate(response.array(), m_settings.updatesBranch() != 0, assetSuffix);
        if (!update.tag.isEmpty())
            emit newVersionAvailable(update.version.toString(), QUrl(releasePageBase + update.tag), update.preview);
    });
}
