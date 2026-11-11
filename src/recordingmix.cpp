#include "recordingmix.h"

#include <QFile>
#include <gst/app/gstappsrc.h>

namespace {
struct MusicTap {
    GstElement *recording;
    GstAppSrc *source;
};

GstPadProbeReturn copyMusic(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    const auto *tap = static_cast<const MusicTap *>(data);
    auto *original = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!original || !GST_BUFFER_PTS_IS_VALID(original))
        return GST_PAD_PROBE_OK;

    auto *event = gst_pad_get_sticky_event(pad, GST_EVENT_SEGMENT, 0);
    auto *element = gst_pad_get_parent_element(pad);
    auto *caps = gst_pad_get_current_caps(pad);
    if (event && element && caps) {
        const GstSegment *segment;
        gst_event_parse_segment(event, &segment);
        if (segment->format == GST_FORMAT_TIME) {
            // Both pipelines use the system clock. Translate the playback
            // segment time into the recorder's running time, including seeks.
            const auto time = gst_segment_to_running_time(segment, GST_FORMAT_TIME, GST_BUFFER_PTS(original));
            const auto playbackBase = gst_element_get_base_time(element);
            const auto recordingBase = gst_element_get_base_time(tap->recording);
            if (GST_CLOCK_TIME_IS_VALID(time) && playbackBase && recordingBase &&
                playbackBase + time >= recordingBase) {
                auto *buffer = gst_buffer_copy(original);
                GST_BUFFER_PTS(buffer) = playbackBase + time - recordingBase;
                GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
                GST_BUFFER_OFFSET(buffer) = GST_BUFFER_OFFSET_NONE;
                GST_BUFFER_OFFSET_END(buffer) = GST_BUFFER_OFFSET_NONE;
                auto *sample = gst_sample_new(buffer, caps, nullptr, nullptr);
                gst_app_src_push_sample(tap->source, sample);
                gst_sample_unref(sample);
                gst_buffer_unref(buffer);
            }
        }
    }
    if (event)
        gst_event_unref(event);
    if (element)
        gst_object_unref(element);
    if (caps)
        gst_caps_unref(caps);
    return GST_PAD_PROBE_OK;
}
}

RecordingMix::RecordingMix(QObject *parent) : QObject(parent)
{
    m_busTimer.setInterval(50);
    connect(&m_busTimer, &QTimer::timeout, this, &RecordingMix::pollBus);
}

RecordingMix::~RecordingMix()
{
    // A close immediately after Stop still gives the muxer time to write its trailer.
    if (m_pipeline && m_finalizing.isValid()) {
        auto *bus = gst_element_get_bus(m_pipeline);
        if (auto *message = gst_bus_timed_pop_filtered(bus, 750 * GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR)))
            gst_message_unref(message);
        gst_object_unref(bus);
    }
    closePipeline();
}

bool RecordingMix::start(GstElement *microphone, GstPad *music, const QString &path, Format format,
                         int micLevel, int musicLevel)
{
    if (!microphone || !music) {
        if (microphone)
            gst_object_unref(microphone);
        emit error(tr("OpenKJ mix could not open the microphone or backing track."));
        return false;
    }

    const char *encoder = "audioconvert ! audio/x-raw,format=S16LE ! wavenc";
    if (format == Format::Ogg)
        encoder = "audioconvert ! vorbisenc ! oggmux";
    else if (format == Format::Mp3)
        encoder = "audioconvert ! lamemp3enc";

    const auto description = QStringLiteral(
        "audiomixer name=mix latency=120000000 ignore-inactive-pads=true ! %1 ! filesink name=output "
        "queue name=mic_queue ! audioconvert ! audioresample ! audio/x-raw,format=F32LE,rate=48000,channels=2 "
        "! volume name=mic_volume volume=0.5 ! mix. "
        "appsrc name=music is-live=true format=time block=false max-bytes=0 max-buffers=64 max-time=500000000 leaky-type=downstream "
        "! audioconvert ! audioresample ! audio/x-raw,format=F32LE,rate=48000,channels=2 "
        "! volume name=music_volume volume=0.5 ! mix.").arg(QLatin1String(encoder)).toUtf8();

    GError *parseError = nullptr;
    m_pipeline = gst_parse_launch(description.constData(), &parseError);
    if (parseError || !m_pipeline) {
        const auto message = parseError ? QString::fromUtf8(parseError->message) : tr("GStreamer pipeline unavailable.");
        g_clear_error(&parseError);
        gst_object_unref(microphone);
        closePipeline();
        emit error(tr("Could not set up OpenKJ mix: %1").arg(message));
        return false;
    }

    auto *queue = gst_bin_get_by_name(GST_BIN(m_pipeline), "mic_queue");
    auto *output = gst_bin_get_by_name(GST_BIN(m_pipeline), "output");
    m_musicSource = gst_bin_get_by_name(GST_BIN(m_pipeline), "music");
    m_micVolume = gst_bin_get_by_name(GST_BIN(m_pipeline), "mic_volume");
    m_musicVolume = gst_bin_get_by_name(GST_BIN(m_pipeline), "music_volume");
    const auto nativePath = QFile::encodeName(path);
    g_object_set(output, "location", nativePath.constData(), nullptr);
    gst_object_unref(output);
    if (!gst_bin_add(GST_BIN(m_pipeline), microphone)) {
        gst_object_unref(microphone);
        gst_object_unref(queue);
        closePipeline();
        emit error(tr("Could not add the microphone to OpenKJ mix."));
        return false;
    }
    m_microphone = microphone;
    const bool linked = gst_element_link(m_microphone, queue);
    gst_object_unref(queue);
    if (!linked) {
        closePipeline();
        emit error(tr("Could not connect the microphone to OpenKJ mix."));
        return false;
    }

    auto *clock = gst_system_clock_obtain();
    gst_pipeline_use_clock(GST_PIPELINE(m_pipeline), clock);
    gst_object_unref(clock);
    m_musicPad = GST_PAD(gst_object_ref(music));
    if (auto *caps = gst_pad_get_current_caps(music)) {
        gst_app_src_set_caps(GST_APP_SRC(m_musicSource), caps);
        gst_caps_unref(caps);
    }
    setLevels(micLevel, musicLevel);
    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        pollBus();
        if (m_pipeline) {
            closePipeline();
            emit error(tr("Could not start OpenKJ mix."));
        }
        return false;
    }
    attachMusic();
    m_busTimer.start();
    return true;
}

void RecordingMix::setLevels(int microphone, int music)
{
    if (m_micVolume)
        g_object_set(m_micVolume, "volume", qBound(0, microphone, 100) / 100.0, nullptr);
    if (m_musicVolume)
        g_object_set(m_musicVolume, "volume", qBound(0, music, 100) / 100.0, nullptr);
}

void RecordingMix::attachMusic()
{
    if (m_probe || !m_musicPad)
        return;
    auto *tap = new MusicTap{GST_ELEMENT(gst_object_ref(m_pipeline)),
                             GST_APP_SRC(gst_object_ref(m_musicSource))};
    m_probe = gst_pad_add_probe(m_musicPad, GST_PAD_PROBE_TYPE_BUFFER, copyMusic, tap, [](gpointer data) {
        auto *tap = static_cast<MusicTap *>(data);
        gst_object_unref(tap->source);
        gst_object_unref(tap->recording);
        delete tap;
    });
}

void RecordingMix::detachMusic()
{
    if (m_probe) {
        gst_pad_remove_probe(m_musicPad, m_probe);
        m_probe = 0;
    }
}

void RecordingMix::pause()
{
    if (m_pipeline && !m_finalizing.isValid()) {
        detachMusic();
        gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
    }
}

void RecordingMix::resume()
{
    if (m_pipeline && !m_finalizing.isValid()) {
        gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
        attachMusic();
    }
}

void RecordingMix::finish()
{
    if (!m_pipeline || m_finalizing.isValid())
        return;
    m_finalizing.start();
    detachMusic();
    // Resume before sending EOS so live sources can drain even when Stop
    // follows Pause. GStreamer forwards the event to both source elements.
    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    gst_element_call_async(m_pipeline, [](GstElement *pipeline, gpointer) {
        gst_element_get_state(pipeline, nullptr, nullptr, 500 * GST_MSECOND);
        gst_element_send_event(pipeline, gst_event_new_eos());
    }, nullptr, nullptr);
}

void RecordingMix::pollBus()
{
    if (!m_pipeline)
        return;
    auto *bus = gst_element_get_bus(m_pipeline);
    auto *message = gst_bus_pop_filtered(bus,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_LATENCY));
    gst_object_unref(bus);
    if (message) {
        const auto type = GST_MESSAGE_TYPE(message);
        QString failure;
        if (type == GST_MESSAGE_ERROR) {
            GError *error = nullptr;
            gst_message_parse_error(message, &error, nullptr);
            failure = tr("OpenKJ mix: %1").arg(QString::fromUtf8(error->message));
            g_clear_error(&error);
        }
        gst_message_unref(message);
        if (type == GST_MESSAGE_LATENCY)
            gst_bin_recalculate_latency(GST_BIN(m_pipeline));
        else {
            complete(failure);
            return;
        }
    }
    if (m_finalizing.isValid() && m_finalizing.elapsed() > 3000)
        complete(tr("OpenKJ mix timed out while saving the recording."));
}

void RecordingMix::complete(const QString &failure)
{
    closePipeline();
    if (!failure.isEmpty())
        emit error(failure);
    emit finished();
}

void RecordingMix::closePipeline()
{
    m_busTimer.stop();
    detachMusic();
    if (m_pipeline)
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_clear_object(&m_musicPad);
    gst_clear_object(&m_musicSource);
    gst_clear_object(&m_micVolume);
    gst_clear_object(&m_musicVolume);
    gst_clear_object(&m_pipeline);
    m_microphone = nullptr;
}
