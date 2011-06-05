/*  This file is part of the KDE project.

    Copyright (C) 2011 Trever Fischer <tdfischer@fedoraproject.org>

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

#ifndef Phonon_GSTREAMER_PIPELINE_H
#define Phonon_GSTREAMER_PIPELINE_H

#include "plugininstaller.h"
#include <gst/gst.h>
#include <phonon/MediaSource>
#include <phonon/MediaController>

QT_BEGIN_NAMESPACE

typedef QMultiMap<QString, QString> TagMap;

namespace Phonon
{
namespace Gstreamer
{

class MediaObject;
class PluginInstaller;

class Pipeline : public QObject
{
    Q_OBJECT

    public:
        Pipeline(QObject *parent = 0);
        virtual ~Pipeline();
        GstElement *element() const;
        GstElement *videoGraph() const;
        GstElement *audioGraph() const;
        GstElement *videoElement() const;
        GstElement *audioElement() const;
        GstStateChangeReturn setState(GstState state);
        GstState state() const;
        void writeToDot(MediaObject *media, const QString &type);
        bool queryDuration(GstFormat *format, gint64 *duration) const;
        qint64 totalDuration() const;

        Q_INVOKABLE void handleEOSMessage(GstMessage *msg);
        static gboolean cb_eos(GstBus *bus, GstMessage *msg, gpointer data);

        static gboolean cb_warning(GstBus *bus, GstMessage *msg, gpointer data);
        Q_INVOKABLE void handleWarningMessage(GstMessage *msg);

        static gboolean cb_duration(GstBus *bus, GstMessage *msg, gpointer data);
        Q_INVOKABLE void handleDurationMessage(GstMessage *msg);

        static gboolean cb_buffering(GstBus *bus, GstMessage *msg, gpointer data);
        Q_INVOKABLE void handleBufferingMessage(GstMessage *msg);

        static gboolean cb_state(GstBus *bus, GstMessage *msg, gpointer data);
        Q_INVOKABLE void handleStateMessage(GstMessage *msg);

        static gboolean cb_element(GstBus *bus, GstMessage *msg, gpointer data);
        Q_INVOKABLE void handleElementMessage(GstMessage *msg);

        static gboolean cb_error(GstBus *bus, GstMessage *msg, gpointer data);
        Q_INVOKABLE void handleErrorMessage(GstMessage *msg);

        static gboolean cb_tag(GstBus *bus, GstMessage *msg, gpointer data);
        Q_INVOKABLE void handleTagMessage(GstMessage *msg);

        static void cb_aboutToFinish(GstElement *appSrc, gpointer data);

        static void cb_endOfPads(GstElement *playbin, gpointer data);

        void setSource(const Phonon::MediaSource &source);

        static void cb_videoChanged(GstElement *playbin, gpointer data);

        GstElement *audioPipe();
        GstElement *videoPipe();
        GstElement *audioGraph();
        GstElement *videoGraph();

        bool videoIsAvailable() const;
        bool audioIsAvailable() const;

        QMultiMap<QString, QString> metaData() const;
        //FIXME: We should be able to specify merging of metadata a la gstreamer's API design
        void setMetaData(const QMultiMap<QString, QString> &newData);

        QList<MediaController::NavigationMenu> availableMenus() const;
        void updateNavigation();

        bool seekToMSec(qint64 time);
        bool isSeekable() const;

        Phonon::State phononState() const;
        static void cb_setupSource(GstElement *playbin, GParamSpec *spec, gpointer data);

    signals:
        void eos();
        void warning(const QString &message);
        void durationChanged(qint64 totalDuration);
        void buffering(int);
        void stateChanged(GstState oldState, GstState newState);
        void videoAvailabilityChanged(bool);
        void errorMessage(const QString &message, Phonon::ErrorType type);
        void metaDataChanged(QMultiMap<QString, QString>);
        void mouseOverActive(bool isActive);
        void availableMenusChanged(QList<MediaController::NavigationMenu>);
        void seekableChanged(bool isSeekable);
        void aboutToFinish();

    private:
        GstPipeline *m_pipeline;
        int m_bufferPercent;

        // Keeps track of wether or not we jump to GST_STATE_PLAYING after plugin installtion is finished.
        // Otherwise, it is possible to jump to another track, play a few seconds, pause, then finish installation
        // and spontaniously start playback without user action.
        bool m_resumeAfterInstall;
        bool m_isStream;
        QMultiMap<QString, QString> m_metaData;
        QList<MediaController::NavigationMenu> m_menus;
        Phonon::MediaSource m_lastSource;
        PluginInstaller *m_installer;
        GstElement *m_audioGraph;
        GstElement *m_videoGraph;
        GstElement *m_audioPipe;
        GstElement *m_videoPipe;

        bool m_seeking;

    private Q_SLOTS:
        void pluginInstallFailure(const QString &msg);
        void pluginInstallComplete();
        void pluginInstallStarted();

};

}
}

#endif // Phonon_GSTREAMER_PIPELINE_H
