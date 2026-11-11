#ifndef OPENKJ_LOCALFILENAME_H
#define OPENKJ_LOCALFILENAME_H

#include <QRegularExpression>

namespace okj {

inline QString safeFilenamePart(QString part, const QString &fallback)
{
    static const QRegularExpression forbidden(QStringLiteral(R"([<>:"/\\|?*\p{Cc}])"));
    static const QRegularExpression reserved(QStringLiteral(R"(^(CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|COM[1-9¹²³]|LPT[1-9¹²³])$)"),
                                             QRegularExpression::CaseInsensitiveOption);
    part = part.trimmed();
    part.replace(forbidden, QStringLiteral("_"));

    // Bound the complete filename in bytes, including the extension and any
    // collision suffix. Never split a UTF-8 character.
    const auto utf8 = part.toUtf8();
    constexpr qsizetype maxBytes = 64;
    if (utf8.size() > maxBytes) {
        qsizetype end = maxBytes;
        while ((static_cast<unsigned char>(utf8[end]) & 0xc0) == 0x80)
            --end;
        part = QString::fromUtf8(utf8.constData(), end);
    }

    while (!part.isEmpty() && (part.endsWith(u'.') || part.back().isSpace()))
        part.chop(1);
    if (part.isEmpty())
        return fallback;
    if (part.startsWith(u'.') || reserved.match(part.section(u'.', 0, 0).trimmed()).hasMatch())
        part.prepend(u'_');
    return part;
}

} // namespace okj

#endif
