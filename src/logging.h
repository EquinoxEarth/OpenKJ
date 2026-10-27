#ifndef OPENKJ_LOGGING_H
#define OPENKJ_LOGGING_H

#include <QByteArray>
#include <QString>
#include <spdlog/fmt/ostr.h>

// Modern fmt requires explicit formatters instead of automatically falling
// back to operator<<. Keep Qt text in UTF-8, including embedded null bytes.
template <>
struct fmt::formatter<QString> : fmt::formatter<fmt::string_view> {
    template <typename FormatContext>
    auto format(const QString &value, FormatContext &context) const
    {
        const auto utf8 = value.toUtf8();
        return fmt::formatter<fmt::string_view>::format(
            fmt::string_view(utf8.constData(), static_cast<size_t>(utf8.size())), context);
    }
};

#endif
