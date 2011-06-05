/*  This file is part of the KDE project.

    Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
    Copyright (C) 2011 Trever Fischer <tdfischer@fedoraproject.org>
    Copyright (C) 2011 Harald Sitter <sitter@kde.org>

    This library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2.1 or 3 of the License.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this library.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pipeline.h"
#include "mediaobject.h"
#include "backend.h"
#include "plugininstaller.h"
#include "streamreader.h"
#include "gsthelper.h"
#include <gst/pbutils/missing-plugins.h>
#include <gst/interfaces/navigation.h>
#include <gst/app/gstappsrc.h>
#define MAX_QUEUE_TIME 20 * GST_SECOND

QT_BEGIN_NAMESPACE
namespace Phonon
{
namespace Gstreamer
{

Pipeline::Pipeline(QObject *parent)
    : QObject(parent)
    , m_bufferPercent(0)
    , m_isStream(false)
    , m_installer(new PluginInstaller(this))
{
    m_pipeline = GST_PIPELINE(gst_element_factory_make("playbin2", NULL));
    gst_object_ref(m_pipeline);
    gst_object_sink(m_pipeline);
    g_signal_connect(m_pipeline, "video-changed", G_CALLBACK(cb_videoChanged), this);
    g_signal_connect(m_pipeline, "notify::source", G_CALLBACK(cb_setupSource), this);
    g_signal_connect(m_pipeline, "about-to-finish", G_CALLBACK(cb_aboutToFinish), this);

    GstBus *bus = gst_pipeline_get_bus(m_pipeline);
    gst_bus_set_sync_handler(bus, gst_bus_sync_signal_handler, NULL);
    g_signal_connect(bus, "sync-message::eos", G_CALLBACK(cb_eos), this);
    g_signal_connect(bus, "sync-message::warning", G_CALLBACK(cb_warning), this);

    //FIXME: This never gets called..?
    g_signal_connect(bus, "sync-message::duration", G_CALLBACK(cb_duration), this);

    g_signal_connect(bus, "sync-message::buffering", G_CALLBACK(cb_buffering), this);
    g_signal_connect(bus, "sync-message::state-changed", G_CALLBACK(cb_state), this);
    g_signal_connect(bus, "sync-message::element", G_CALLBACK(cb_element), this);
    g_signal_connect(bus, "sync-message::error", G_CALLBACK(cb_error), this);
    g_signal_connect(bus, "sync-message::tag", G_CALLBACK(cb_tag), this);

    // Set up audio graph
    m_audioGraph = gst_bin_new("audioGraph");
    gst_object_ref (GST_OBJECT (m_audioGraph));
    gst_object_sink (GST_OBJECT (m_audioGraph));

    // Note that these queues are only required for streaming content
    // And should ideally be created on demand as they will disable
    // pull-mode access. Also note that the max-size-time are increased to
    // reduce buffer overruns as these are not gracefully handled at the moment.
    m_audioPipe = gst_element_factory_make("queue", "audioPipe");
    g_object_set(G_OBJECT(m_audioPipe), "max-size-time",  MAX_QUEUE_TIME, (const char*)NULL);

    QByteArray tegraEnv = qgetenv("TEGRA_GST_OPENMAX");
    if (!tegraEnv.isEmpty()) {
        g_object_set(G_OBJECT(m_audioPipe), "max-size-time", 0, (const char*)NULL);
        g_object_set(G_OBJECT(m_audioPipe), "max-size-buffers", 0, (const char*)NULL);
        g_object_set(G_OBJECT(m_audioPipe), "max-size-bytes", 0, (const char*)NULL);
    }

    gst_bin_add(GST_BIN(m_audioGraph), m_audioPipe);
    GstPad *audiopad = gst_element_get_pad (m_audioPipe, "sink");
    gst_element_add_pad (m_audioGraph, gst_ghost_pad_new ("sink", audiopad));
    gst_object_unref (audiopad);

    g_object_set(m_pipeline, "audio-sink", m_audioGraph, NULL);

    // Set up video graph
    m_videoGraph = gst_bin_new("videoGraph");
    gst_object_ref (GST_OBJECT (m_videoGraph));
    gst_object_sink (GST_OBJECT (m_videoGraph));

    m_videoPipe = gst_element_factory_make("queue", "videoPipe");
    gst_bin_add(GST_BIN(m_videoGraph), m_videoPipe);
    GstPad *videopad = gst_element_get_pad(m_videoPipe, "sink");
    gst_element_add_pad(m_videoGraph, gst_ghost_pad_new("sink", videopad));
    gst_object_unref(audiopad);

    g_object_set(m_pipeline, "video-sink", m_videoGraph, NULL);

    //FIXME: Put this stuff somewhere else, or at least document why its needed.
    if (!tegraEnv.isEmpty()) {
        //TODO: Move this line into the videooutput
        //g_object_set(G_OBJECT(videoQueue), "max-size-time", 33000, (const char*)NULL);
        g_object_set(G_OBJECT(m_audioPipe), "max-size-buffers", 1, (const char*)NULL);
        g_object_set(G_OBJECT(m_audioPipe), "max-size-bytes", 0, (const char*)NULL);
    }

    connect(m_installer, SIGNAL(failure(const QString&)), this, SLOT(pluginInstallFailure(const QString&)));
    connect(m_installer, SIGNAL(started()), this, SLOT(pluginInstallStarted()));
    connect(m_installer, SIGNAL(success()), this, SLOT(pluginInstallComplete()));
}

GstElement *Pipeline::audioPipe()
{
    return m_audioPipe;
}

GstElement *Pipeline::videoPipe()
{
    return m_videoPipe;
}

GstElement *Pipeline::audioGraph()
{
    return m_audioGraph;
}

GstElement *Pipeline::videoGraph()
{
    return m_videoGraph;
}

void Pipeline::setSource(const Phonon::MediaSource &source)
{
    m_seeking = false;
    m_installer->reset();
    m_resumeAfterInstall = false;
    m_metaData.clear();

    qDebug() << source.mrl();
    QByteArray gstUri;
    switch(source.type()) {
        case MediaSource::Url:
        case MediaSource::LocalFile:
            gstUri = source.mrl().toEncoded();
            break;
        case MediaSource::Invalid:
            //TODO: Raise error
            return;
        case MediaSource::Stream:
            gstUri = "appsrc://";
            m_isStream = true;
            break;
        case MediaSource::Disc:
            switch(source.discType()) {
                case Phonon::Cd:
                    gstUri = "cdda://";
                    break;
                case Phonon::Vcd:
                    gstUri = "vcd://";
                    break;
                case Phonon::Dvd:
                    gstUri = "dvd://";
                    break;
            }
            break;
    }

    //TODO: Test this to make sure that resuming playback after plugin installation
    //when using an abstract stream source doesn't explode.
    m_lastSource = source;

    g_object_set(m_pipeline, "uri", gstUri.constData(), NULL);
}

Pipeline::~Pipeline()
{
    gst_element_set_state(GST_ELEMENT(m_pipeline), GST_STATE_NULL);
    gst_object_unref(m_pipeline);
}

GstElement *Pipeline::element() const
{
    return GST_ELEMENT(m_pipeline);
}

GstStateChangeReturn Pipeline::setState(GstState state)
{
    m_resumeAfterInstall = true;
    qDebug() << "Transitioning to state" << GstHelper::stateName(state);

    return gst_element_set_state(GST_ELEMENT(m_pipeline), state);
}

void Pipeline::writeToDot(MediaObject *media, const QString &type)
{
    GstBin *bin = GST_BIN(m_pipeline);
    if (media)
        media->backend()->logMessage(QString("Dumping %0.dot").arg(type), Backend::Debug, media);
    else {
        qDebug() << type;
    }
    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(bin, GST_DEBUG_GRAPH_SHOW_ALL, QString("phonon-%0").arg(type).toUtf8().constData());
}

bool Pipeline::queryDuration(GstFormat *format, gint64 *duration) const
{
    return gst_element_query_duration(GST_ELEMENT(m_pipeline), format, duration);
}

GstState Pipeline::state() const
{
    GstState state;
    gst_element_get_state(GST_ELEMENT(m_pipeline), &state, NULL, 1000);
    return state;
}

gboolean Pipeline::cb_eos(GstBus *bus, GstMessage *msg, gpointer data)
{
    Q_UNUSED(bus)
    MediaObject *that = static_cast<MediaObject*>(data);
    gst_mini_object_ref(GST_MINI_OBJECT_CAST(msg));
    QMetaObject::invokeMethod(that, "handleEOSMessage", Qt::QueuedConnection, Q_ARG(GstMessage*, msg));
    return true;
}

void Pipeline::handleEOSMessage(GstMessage *gstMessage)
{
    gst_mini_object_unref(GST_MINI_OBJECT_CAST(gstMessage));
    emit eos();
}

gboolean Pipeline::cb_warning(GstBus *bus, GstMessage *msg, gpointer data)
{
    Q_UNUSED(bus)
    MediaObject *that = static_cast<MediaObject*>(data);
    gst_mini_object_ref(GST_MINI_OBJECT_CAST(msg));
    QMetaObject::invokeMethod(that, "handleWarningMessage", Qt::QueuedConnection, Q_ARG(GstMessage*, msg));
    return true;
}

void Pipeline::handleWarningMessage(GstMessage *gstMessage)
{
    gchar *debug;
    GError *err;
    gst_message_parse_warning(gstMessage, &err, &debug);
    QString msgString;
    msgString.sprintf("Warning: %s\nMessage:%s", debug, err->message);
    emit warning(msgString);
    g_free (debug);
    g_error_free (err);
    gst_mini_object_unref(GST_MINI_OBJECT_CAST(gstMessage));
}

gboolean Pipeline::cb_duration(GstBus *bus, GstMessage *msg, gpointer data)
{
    Q_UNUSED(bus)
    Pipeline *that = static_cast<Pipeline*>(data);
    qDebug() << "Duration message";
    gst_mini_object_ref(GST_MINI_OBJECT_CAST(msg));
    QMetaObject::invokeMethod(that, "handleDurationMessage", Qt::QueuedConnection, Q_ARG(GstMessage*, msg));
    return true;
}

void Pipeline::handleDurationMessage(GstMessage *gstMessage)
{
    gint64 duration;
    GstFormat format;
    gst_message_parse_duration(gstMessage, &format, &duration);
    if (format == GST_FORMAT_TIME)
        emit durationChanged(duration/GST_MSECOND);
    gst_mini_object_unref(GST_MINI_OBJECT_CAST(gstMessage));
}

qint64 Pipeline::totalDuration() const
{
    GstFormat format = GST_FORMAT_TIME;
    gint64 duration = 0;
    if (queryDuration(&format, &duration)) {
        return duration/GST_MSECOND;
    }
    return -1;
}

gboolean Pipeline::cb_buffering(GstBus *bus, GstMessage *msg, gpointer data)
{
    Q_UNUSED(bus)
    MediaObject *that = static_cast<MediaObject*>(data);
    gst_mini_object_ref(GST_MINI_OBJECT_CAST(msg));
    QMetaObject::invokeMethod(that, "handleBufferingMessage", Qt::QueuedConnection, Q_ARG(GstMessage*, msg));
    return true;
}

/**
 * Handles GST_MESSAGE_BUFFERING messages
 */
void Pipeline::handleBufferingMessage(GstMessage *gstMessage)
{
    gint percent = 0;
    gst_structure_get_int (gstMessage->structure, "buffer-percent", &percent); //gst_message_parse_buffering was introduced in 0.10.11

    if (m_bufferPercent != percent) {
        emit buffering(percent);
        m_bufferPercent = percent;
    }

    gst_mini_object_unref(GST_MINI_OBJECT_CAST(gstMessage));
}

gboolean Pipeline::cb_state(GstBus *bus, GstMessage *msg, gpointer data)
{
    Q_UNUSED(bus)
    Pipeline *that = static_cast<Pipeline*>(data);
    gst_mini_object_ref(GST_MINI_OBJECT_CAST(msg));
    QMetaObject::invokeMethod(that, "handleStateMessage", Qt::QueuedConnection, Q_ARG(GstMessage*, msg));
    return true;
}

void Pipeline::handleStateMessage(GstMessage *gstMessage)
{
    GstState oldState;
    GstState newState;
    GstState pendingState;
    gst_message_parse_state_changed(gstMessage, &oldState, &newState, &pendingState);

    if (oldState == newState) {
        return;
    }

    // Apparently gstreamer sometimes enters the same state twice.
    // FIXME: Sometimes we enter the same state twice. currently not disallowed by the state machine
    if (m_seeking) {
        if (GST_STATE_TRANSITION(oldState, newState) == GST_STATE_CHANGE_PAUSED_TO_PLAYING)
            m_seeking = false;
        return;
    }

    if (gstMessage->src != GST_OBJECT(m_pipeline)) {
        gst_mini_object_unref(GST_MINI_OBJECT_CAST(gstMessage));
        return;
    }

    if (newState == GST_STATE_READY) {
        m_installer->checkInstalledPlugins();
    }

    //FIXME: This is a hack until proper state engine is implemented in the pipeline
    // Wait to update stuff until we're at the final requested state
    if (pendingState == GST_STATE_VOID_PENDING) {
        emit durationChanged(totalDuration());
        emit seekableChanged(isSeekable());
    }

    emit stateChanged(oldState, newState);
}

void Pipeline::cb_videoChanged(GstElement *playbin, gpointer data)
{
    gint videoCount;
    bool videoAvailable;
    Pipeline *that = static_cast<Pipeline*>(data);
    g_object_get(playbin, "n-video", &videoCount, NULL);
    // If there is at least one video stream, we've got video.
    videoAvailable = videoCount > 0;

    // FIXME: Only emit this if n-video goes between 0 and non zero.
    emit that->videoAvailabilityChanged(videoAvailable);
}

bool Pipeline::videoIsAvailable() const
{
    gint videoCount;
    g_object_get(m_pipeline, "n-video", &videoCount, NULL);
    return videoCount > 0;
}

bool Pipeline::audioIsAvailable() const
{
    gint audioCount;
    g_object_get(m_pipeline, "n-audio", &audioCount, NULL);
    return audioCount > 0;
}

gboolean Pipeline::cb_element(GstBus *bus, GstMessage *msg, gpointer data)
{
    Q_UNUSED(bus)
    Pipeline *that = static_cast<Pipeline*>(data);
    gst_mini_object_ref(GST_MINI_OBJECT_CAST(msg));
    QMetaObject::invokeMethod(that, "handleElementMessage", Qt::QueuedConnection, Q_ARG(GstMessage*, msg));
    return true;
}

void Pipeline::handleElementMessage(GstMessage *gstMessage)
{
    if (gst_is_missing_plugin_message(gstMessage)) {
        m_installer->addPlugin(gstMessage);
    } else {
#if GST_VERSION >= GST_VERSION_CHECK(0,10,23,0)
        switch (gst_navigation_message_get_type(gstMessage)) {
        case GST_NAVIGATION_MESSAGE_MOUSE_OVER: {
            gboolean active;
            if (!gst_navigation_message_parse_mouse_over(gstMessage, &active)) {
                break;
            }
            emit mouseOverActive(active);
            break;
        }
        case GST_NAVIGATION_MESSAGE_COMMANDS_CHANGED:
            updateNavigation();
            break;
        default:
            break;
        }
#endif // GST_VERSION
    }
    gst_mini_object_unref(GST_MINI_OBJECT_CAST(gstMessage));
}

//TODO: implement state changes
void Pipeline::pluginInstallFailure(const QString &msg)
{
    bool canPlay = audioIsAvailable() || videoIsAvailable();
    Phonon::ErrorType error = canPlay ? Phonon::NormalError : Phonon::FatalError;
    emit errorMessage(msg, error);
}

void Pipeline::pluginInstallStarted()
{
    //setState(Phonon::LoadingState);
}

void Pipeline::pluginInstallComplete()
{
    qDebug() << "Install complete." << m_resumeAfterInstall;
    if (m_resumeAfterInstall) {
        setSource(m_lastSource);
        setState(GST_STATE_PLAYING);
    }
}

gboolean Pipeline::cb_error(GstBus *bus, GstMessage *msg, gpointer data)
{
    Q_UNUSED(bus)
    Pipeline *that = static_cast<Pipeline*>(data);
    gst_mini_object_ref(GST_MINI_OBJECT_CAST(msg));
    QMetaObject::invokeMethod(that, "handleErrorMessage", Qt::QueuedConnection, Q_ARG(GstMessage*, msg));
    return true;
}

void Pipeline::handleErrorMessage(GstMessage *gstMessage)
{
    PluginInstaller::InstallStatus status = m_installer->checkInstalledPlugins();
    qDebug() << status;

    if (status == PluginInstaller::Missing) {
        Phonon::ErrorType type = (audioIsAvailable() || videoIsAvailable()) ? Phonon::NormalError : Phonon::FatalError;
        emit errorMessage(tr("One or more plugins are missing in your GStreamer installation."), type);
    } else if (status == PluginInstaller::Installed) {
        gchar *debug;
        GError *err;
        gst_message_parse_error (gstMessage, &err, &debug);
        //TODO: Log the error
        emit errorMessage(err->message, Phonon::FatalError);
        g_error_free(err);
    }
    gst_mini_object_unref(GST_MINI_OBJECT_CAST(gstMessage));
}

gboolean Pipeline::cb_tag(GstBus *bus, GstMessage *msg, gpointer data)
{
    Q_UNUSED(bus)
    Pipeline *that = static_cast<Pipeline*>(data);
    gst_mini_object_ref(GST_MINI_OBJECT_CAST(msg));
    QMetaObject::invokeMethod(that, "handleTagMessage", Qt::QueuedConnection, Q_ARG(GstMessage*, msg));
    return true;
}

/*
 * Used to iterate through the gst_tag_list and extract values
 */
void foreach_tag_function(const GstTagList *list, const gchar *tag, gpointer user_data)
{
    TagMap *newData = static_cast<TagMap *>(user_data);
    QString value;
    GType type = gst_tag_get_type(tag);
    switch (type) {
    case G_TYPE_STRING: {
            char *str = 0;
            gst_tag_list_get_string(list, tag, &str);
            value = QString::fromUtf8(str);
            g_free(str);
        }
        break;

    case G_TYPE_BOOLEAN: {
            int bval;
            gst_tag_list_get_boolean(list, tag, &bval);
            value = QString::number(bval);
        }
        break;

    case G_TYPE_INT: {
            int ival;
            gst_tag_list_get_int(list, tag, &ival);
            value = QString::number(ival);
        }
        break;

    case G_TYPE_UINT: {
            unsigned int uival;
            gst_tag_list_get_uint(list, tag, &uival);
            value = QString::number(uival);
        }
        break;

    case G_TYPE_FLOAT: {
            float fval;
            gst_tag_list_get_float(list, tag, &fval);
            value = QString::number(fval);
        }
        break;

    case G_TYPE_DOUBLE: {
            double dval;
            gst_tag_list_get_double(list, tag, &dval);
            value = QString::number(dval);
        }
        break;

    default:
        //qDebug("Unsupported tag type: %s", g_type_name(type));
        break;
    }

    QString key = QString(tag).toUpper();
    QString currVal = newData->value(key);
    if (!value.isEmpty() && !(newData->contains(key) && currVal == value))
        newData->insert(key, value);
}


void Pipeline::handleTagMessage(GstMessage *msg)
{
    GstTagList* tag_list = 0;
    gst_message_parse_tag(msg, &tag_list);
    if (tag_list) {
        TagMap newTags;
        gst_tag_list_foreach (tag_list, &foreach_tag_function, &newTags);
        gst_tag_list_free(tag_list);

        // Determine if we should no fake the album/artist tags.
        // This is a little confusing as we want to fake it on initial
        // connection where title, album and artist are all missing.
        // There are however times when we get just other information,
        // e.g. codec, and so we want to only do clever stuff if we
        // have a commonly available tag (ORGANIZATION) or we have a
        // change in title
        bool fake_it =
           (m_isStream
            && ((!newTags.contains("TITLE")
                 && newTags.contains("ORGANIZATION"))
                || (newTags.contains("TITLE")
                    && m_metaData.value("TITLE") != newTags.value("TITLE")))
            && !newTags.contains("ALBUM")
            && !newTags.contains("ARTIST"));

        TagMap oldMap = m_metaData; // Keep a copy of the old one for reference

        // Now we've checked the new data, append any new meta tags to the existing tag list
        // We cannot use TagMap::iterator as this is a multimap and when streaming data
        // could in theory be lost.
        QList<QString> keys = newTags.keys();
        for (QList<QString>::iterator i = keys.begin(); i != keys.end(); ++i) {
            QString key = *i;
            if (m_isStream) {
                // If we're streaming, we need to remove data in m_metaData
                // in order to stop it filling up indefinitely (as it's a multimap)
                m_metaData.remove(key);
            }
            QList<QString> values = newTags.values(key);
            for (QList<QString>::iterator j = values.begin(); j != values.end(); ++j) {
                QString value = *j;
                QString currVal = m_metaData.value(key);
                if (!m_metaData.contains(key) || currVal != value) {
                    m_metaData.insert(key, value);
                }
            }
        }

        // For radio streams, if we get a metadata update where the title changes, we assume everything else is invalid.
        // If we don't already have a title, we don't do anything since we're actually just appending new data into that.
        if (m_isStream && oldMap.contains("TITLE") && m_metaData.value("TITLE") != oldMap.value("TITLE")) {
            m_metaData.clear();
        }

        //m_backend->logMessage("Meta tags found", Backend::Info, this);
        if (oldMap != m_metaData) {
            // This is a bit of a hack to ensure that stream metadata is
            // returned. We get as much as we can from the Shoutcast server's
            // StreamTitle= header. If further info is decoded from the stream
            // itself later, then it will overwrite this info.
            if (m_isStream && fake_it) {
                m_metaData.remove("ALBUM");
                m_metaData.remove("ARTIST");

                // Detect whether we want to "fill in the blanks"
                QString str;
                if (m_metaData.contains("TITLE"))
                {
                    str = m_metaData.value("TITLE");
                    int splitpoint;
                    // Check to see if our title matches "%s - %s"
                    // Where neither %s are empty...
                    if ((splitpoint = str.indexOf(" - ")) > 0
                        && str.size() > (splitpoint+3)) {
                        m_metaData.insert("ARTIST", str.left(splitpoint));
                        m_metaData.replace("TITLE", str.mid(splitpoint+3));
                    }
                } else {
                    str = m_metaData.value("GENRE");
                    if (!str.isEmpty())
                        m_metaData.insert("TITLE", str);
                    else
                        m_metaData.insert("TITLE", "Streaming Data");
                }
                if (!m_metaData.contains("ARTIST")) {
                    str = m_metaData.value("LOCATION");
                    if (!str.isEmpty())
                        m_metaData.insert("ARTIST", str);
                    else
                        m_metaData.insert("ARTIST", "Streaming Data");
                }
                str = m_metaData.value("ORGANIZATION");
                if (!str.isEmpty())
                    m_metaData.insert("ALBUM", str);
                else
                    m_metaData.insert("ALBUM", "Streaming Data");
            }
            // As we manipulate the title, we need to recompare
            // oldMap and m_metaData here...
            //if (oldMap != m_metaData && !m_loading)
                emit metaDataChanged(m_metaData);
        }
    }
    gst_mini_object_unref(GST_MINI_OBJECT_CAST(msg));
}

QMultiMap<QString, QString> Pipeline::metaData() const
{
    return m_metaData;
}

//FIXME: This apparently was never implemented in mediaobject. No idea if clobbering all data is
//intended behavior...
void Pipeline::setMetaData(const QMultiMap<QString, QString> &newData)
{
    m_metaData = newData;
}

void Pipeline::updateNavigation()
{
    QList<MediaController::NavigationMenu> ret;
#if GST_VERSION >= GST_VERSION_CHECK(0,10,23,0)
    GstElement *target = gst_bin_get_by_interface(GST_BIN(m_pipeline), GST_TYPE_NAVIGATION);
    if (target) {
        GstQuery *query = gst_navigation_query_new_commands();
        gboolean res = gst_element_query(target, query);
        guint count;
        if (res && gst_navigation_query_parse_commands_length(query, &count)) {
            for(guint i = 0; i < count; ++i) {
                GstNavigationCommand cmd;
                if (!gst_navigation_query_parse_commands_nth(query, i, &cmd))
                    break;
                switch (cmd) {
                case GST_NAVIGATION_COMMAND_DVD_ROOT_MENU:
                    ret << MediaController::RootMenu;
                    break;
                case GST_NAVIGATION_COMMAND_DVD_TITLE_MENU:
                    ret << MediaController::TitleMenu;
                    break;
                case GST_NAVIGATION_COMMAND_DVD_AUDIO_MENU:
                    ret << MediaController::AudioMenu;
                    break;
                case GST_NAVIGATION_COMMAND_DVD_SUBPICTURE_MENU:
                    ret << MediaController::SubtitleMenu;
                    break;
                case GST_NAVIGATION_COMMAND_DVD_CHAPTER_MENU:
                    ret << MediaController::ChapterMenu;
                    break;
                case GST_NAVIGATION_COMMAND_DVD_ANGLE_MENU:
                    ret << MediaController::AngleMenu;
                    break;
                default:
                    break;
                }
            }
        }
    }
#endif
    if (ret != m_menus) {
        m_menus = ret;
        emit availableMenusChanged(m_menus);
    }
}

QList<MediaController::NavigationMenu> Pipeline::availableMenus() const
{
    return m_menus;
}

bool Pipeline::seekToMSec(qint64 time)
{
    m_seeking = true;
    return gst_element_seek(GST_ELEMENT(m_pipeline), 1.0, GST_FORMAT_TIME,
                     GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET,
                     time * GST_MSECOND, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
}

bool Pipeline::isSeekable() const
{
    GstQuery *query;
    gboolean result;
    gint64 start, stop;
    query = gst_query_new_seeking(GST_FORMAT_TIME);
    result = gst_element_query (GST_ELEMENT(m_pipeline), query);
    if (result) {
        gboolean seekable;
        GstFormat format;
        gst_query_parse_seeking(query, &format, &seekable, &start, &stop);
        return seekable;
    } else {
        //TODO: Log failure
    }
    return false;
}

Phonon::State Pipeline::phononState() const
{
    return Phonon::PlayingState;
    switch (state()) {
        case GST_STATE_PLAYING:
            return Phonon::PlayingState;
        case GST_STATE_READY:
            return Phonon::StoppedState;
        case GST_STATE_NULL:
            return Phonon::LoadingState;
        case GST_STATE_PAUSED:
            return Phonon::PausedState;
    }
    return Phonon::ErrorState;
}

static void cb_feedAppSrc(GstAppSrc *appSrc, guint buffsize, gpointer data)
{
    StreamReader *reader = static_cast<StreamReader*>(data);
    GstBuffer *buf = gst_buffer_new_and_alloc(buffsize);
    reader->read(reader->currentPos(), buffsize, (char*)GST_BUFFER_DATA(buf));
    gst_app_src_push_buffer(appSrc, buf);
}

static void cb_seekAppSrc(GstAppSrc *appSrc, guint64 pos, gpointer data)
{
    StreamReader *reader = static_cast<StreamReader*>(data);
    reader->setCurrentPos(pos);
}

void Pipeline::cb_setupSource(GstElement *playbin, GParamSpec *param, gpointer data)
{
    Pipeline *that = static_cast<Pipeline*>(data);
    if (that->m_isStream) {
        GstElement *phononSrc;
        g_object_get(that->m_pipeline, "source", &phononSrc, NULL);
        StreamReader *reader = new StreamReader(that->m_lastSource, that);
        if (reader->streamSize() > 0)
            g_object_set(phononSrc, "size", reader->streamSize(), NULL);
        int streamType = 0;
        if (reader->streamSeekable())
            streamType = GST_APP_STREAM_TYPE_SEEKABLE;
        else
            streamType = GST_APP_STREAM_TYPE_STREAM;
        g_object_set(phononSrc, "stream-type", streamType, NULL);
        g_object_set(phononSrc, "block", TRUE, NULL);
        g_signal_connect(phononSrc, "need-data", G_CALLBACK(cb_feedAppSrc), reader);
        g_signal_connect(phononSrc, "seek-data", G_CALLBACK(cb_seekAppSrc), reader);
    }
}

void Pipeline::cb_aboutToFinish(GstElement *appSrc, gpointer data)
{
    Pipeline *that = static_cast<Pipeline*>(data);
    emit that->aboutToFinish();
}

}
};

#include "moc_pipeline.cpp"
