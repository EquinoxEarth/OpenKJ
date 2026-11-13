#include "audionormalization.h"
#include "localmediauri.h"

#include <QDeadlineTimer>
#include <algorithm>
#include <cmath>
#include <gst/gst.h>

std::optional<double> okj::analyzeTrackGain(const QString &path, const std::atomic_bool &cancelled)
{
    if (cancelled.load())
        return {};

    GError *error = nullptr;
    auto *analysisSink = gst_parse_bin_from_description(
        "audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=2 ! "
        "rganalysis forced=true ! fakesink sync=false", TRUE, &error);
    if (error || !analysisSink) {
        g_clear_error(&error);
        if (analysisSink)
            gst_object_unref(analysisSink);
        return {};
    }
    auto *pipeline = gst_element_factory_make("playbin", nullptr);
    if (!pipeline) {
        gst_object_unref(analysisSink);
        return {};
    }
    gst_object_ref_sink(analysisSink);
    const auto uri = localMediaUri(path);
    // Select audio only so a video or subtitle stream cannot stall analysis.
    constexpr guint playAudio = 1u << 1; // GST_PLAY_FLAG_AUDIO
    g_object_set(pipeline, "uri", uri.constData(), "flags", playAudio,
                 "audio-sink", analysisSink, nullptr);
    gst_object_unref(analysisSink);
    auto *bus = gst_element_get_bus(pipeline);
    std::optional<double> gainDb;
    bool complete = false;
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE) {
        QDeadlineTimer deadline(10000);
        while (!cancelled.load() && !deadline.hasExpired()) {
            auto *message = gst_bus_timed_pop_filtered(bus, 50 * GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_TAG | GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
            if (!message)
                continue;
            const auto type = GST_MESSAGE_TYPE(message);
            if (type == GST_MESSAGE_TAG) {
                GstTagList *tags = nullptr;
                gst_message_parse_tag(message, &tags);
                double value;
                if (gst_tag_list_get_double(tags, GST_TAG_TRACK_GAIN, &value) && std::isfinite(value))
                    gainDb = value;
                gst_tag_list_unref(tags);
            }
            gst_message_unref(message);
            if (type == GST_MESSAGE_EOS)
                complete = true;
            if (complete || type == GST_MESSAGE_ERROR)
                break;
        }
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);

    if (!complete || !gainDb || cancelled.load())
        return {};
    // Quiet tracks retain their original level. Gain reduction adds no clipping.
    return std::pow(10.0, std::min(*gainDb, 0.0) / 20.0);
}
