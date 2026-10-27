#include "logging.h"

#include <spdlog/async.h>
#include <spdlog/sinks/ostream_sink.h>
#include <cstdio>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
}

int main()
{
    try {
        std::ostringstream output;
        std::string error;
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
        auto pool = std::make_shared<spdlog::details::thread_pool>(128, 1);
        auto logger = std::make_shared<spdlog::async_logger>(
            "logging-test", sink, pool, spdlog::async_overflow_policy::block);
        logger->set_pattern("%v");
        logger->set_error_handler([&](const std::string &message) { error = message; });

        auto singer = QStringLiteral("Zoë 李 {karaoke}");
        const auto originalSinger = singer.toUtf8().toStdString();
        const auto path = QStringLiteral("/music/Björk/夜 {}.zip");
        logger->info("{} | {}", singer, path);
        singer = QStringLiteral("changed after enqueue");
        logger->info("temporary {}", QStringLiteral("Élodie"));
        logger->info("aligned [{:>8}] num={:03} flag={:5}", QStringLiteral("Jo"), 7, true);
        logger->info(std::string("Qt message has {braces} and 100%"));
        const auto binaryText = QStringLiteral("A") + QChar(0) + QStringLiteral("B");
        logger->info("embedded [{}]", binaryText);
        logger->flush();
        pool.reset(); // Joins the worker after it drains messages and the flush request.

        const auto text = output.str();
        require(error.empty(), "Logging must not report formatting errors");
        require(text.find(originalSinger + " | " + path.toUtf8().toStdString()) != std::string::npos,
                "Async logs must retain Unicode singers, paths, and literal braces");
        require(text.find(QStringLiteral("temporary Élodie").toUtf8().toStdString()) != std::string::npos,
                "Temporary Qt strings must survive async logging");
        require(text.find("aligned [      Jo] num=007 flag=true") != std::string::npos,
                "Qt strings must retain alignment alongside numeric and boolean formatting");
        require(text.find("Qt message has {braces} and 100%") != std::string::npos,
                "Qt message forwarding must treat runtime message text literally");
        require(text.find("embedded [" + binaryText.toUtf8().toStdString() + "]") != std::string::npos,
                "Qt string formatting must not truncate embedded nulls");
        std::puts("Async Qt logging checks passed");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
