/********************************************************************
**  Nulloy Music Player, http://nulloy.com
**  Copyright (C) 2010-2015 Sergey Vlasov <sergey@vlasov.me>
**
**  This program can be distributed under the terms of the GNU
**  General Public License version 3.0 as published by the Free
**  Software Foundation and appearing in the file LICENSE.GPL3
**  included in the packaging of this file.  Please review the
**  following information to ensure the GNU General Public License
**  version 3.0 requirements will be met:
**
**  http://www.gnu.org/licenses/gpl-3.0.html
**
*********************************************************************/

#include "playbackEngineGstreamer.h"

#include "common.h"
#include <QtGlobal>
#include <QTimer>

static void _on_eos(GstBus *, GstMessage *, gpointer userData)
{
	NPlaybackEngineGStreamer *obj = reinterpret_cast<NPlaybackEngineGStreamer *>(userData);
	obj->_finish();
}

static void _on_error(GstBus *, GstMessage *msg, gpointer userData)
{
	gchar *debug;
	GError *err;

	gst_message_parse_error(msg, &err, &debug);
	g_free(debug);

	NPlaybackEngineGStreamer *obj = reinterpret_cast<NPlaybackEngineGStreamer *>(userData);
	obj->_emitError(err->message);
	obj->_fail();

	g_error_free(err);
}

static void _on_about_to_finish(GstElement *playbin, gpointer userData)
{
	NPlaybackEngineGStreamer *obj = reinterpret_cast<NPlaybackEngineGStreamer *>(userData);

	gchar *uri_before;
	g_object_get(playbin, "uri", &uri_before, NULL);

	obj->_crossfadingPrepare();
	obj->_emitAboutToFinish();

	gchar *uri_after = g_filename_to_uri(QFileInfo(obj->currentMedia()).absoluteFilePath().toUtf8().constData(), NULL, NULL);

	if (g_strcmp0(uri_before, uri_after) == 0) // uri hasn't changed
		obj->_crossfadingCancel();

	g_free(uri_before);
	g_free(uri_after);
}

N::PlaybackState NPlaybackEngineGStreamer::fromGstState(GstState state)
{
	switch (state) {
		case GST_STATE_PAUSED:
			return N::PlaybackPaused;
		case GST_STATE_PLAYING:
			return N::PlaybackPlaying;
		default:
			return N::PlaybackStopped;
	}
}

void NPlaybackEngineGStreamer::init()
{
	if (m_init)
		return;

	int argc;
	const char **argv;
	GError *err;
	NCore::cArgs(&argc, &argv);
	gst_init(&argc, (char ***)&argv);
	if (!gst_init_check(&argc, (char ***)&argv, &err)) {
		emit message(QMessageBox::Critical, QFileInfo(m_currentMedia).absoluteFilePath(), err->message);
		emit failed();
	}

	m_playbin = gst_element_factory_make("playbin", NULL);
	g_signal_connect(m_playbin, "about-to-finish", G_CALLBACK(_on_about_to_finish), this);

#if !defined Q_WS_WIN && !defined Q_WS_MAC
	GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(m_playbin));
	gst_bus_add_signal_watch(bus);
	g_signal_connect(bus, "message::error", G_CALLBACK(_on_error), this);
	g_signal_connect(bus, "message::eos", G_CALLBACK(_on_eos), this);
	gst_object_unref(bus);
#endif

	m_oldVolume = -1;
	m_oldPosition = -1;
	m_posponedPosition = -1;
	m_oldState = N::PlaybackStopped;
	m_currentMedia = "";
	m_durationNsec = 0;
	m_crossfading = FALSE;

	m_timer = new QTimer(this);
	connect(m_timer, SIGNAL(timeout()), this, SLOT(checkStatus()));

	m_init = TRUE;
}

NPlaybackEngineGStreamer::~NPlaybackEngineGStreamer()
{
	if (!m_init)
		return;

	stop();
	gst_object_unref(m_playbin);
}

void NPlaybackEngineGStreamer::setMedia(const QString &file)
{
#if defined Q_WS_WIN || defined Q_WS_MAC
	qreal vol = m_oldVolume;
#endif

	if (!m_crossfading)
		stop();

	if (file.isEmpty()) {
		stop();
		emit mediaChanged(m_currentMedia = "");
		return;
	}

	if (!QFile(file).exists()) {
		_fail();
		emit message(QMessageBox::Warning, file, "No such file or directory");
		return;
	}

	gchar *uri = g_filename_to_uri(QFileInfo(file).absoluteFilePath().toUtf8().constData(), NULL, NULL);
	if (uri)
		m_currentMedia = file;
	g_object_set(m_playbin, "uri", uri, NULL);

	emit mediaChanged(m_currentMedia);

#if defined Q_WS_WIN || defined Q_WS_MAC
	if (vol != -1)
		setVolume(vol);
#endif
}

void NPlaybackEngineGStreamer::setVolume(qreal volume)
{
	g_object_set(m_playbin, "volume", qBound(0.0, volume, 1.0), NULL);
}

qreal NPlaybackEngineGStreamer::volume()
{
	gdouble volume;
	g_object_get(m_playbin, "volume", &volume, NULL);
	return (qreal)volume;
}

void NPlaybackEngineGStreamer::setPosition(qreal pos)
{
	if (!hasMedia() || pos < 0)
		return;

	if (m_durationNsec > 0) {
		gst_element_seek(m_playbin, 1.0,
		                 GST_FORMAT_TIME,
		                 GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		                 GST_SEEK_TYPE_SET, pos * m_durationNsec,
		                 GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
	} else {
		m_posponedPosition = pos;
	}
}

qreal NPlaybackEngineGStreamer::position()
{
	return m_crossfading ? 0 : m_oldPosition;
}

qint64 NPlaybackEngineGStreamer::durationMsec()
{
	return m_durationNsec / 1000000;
}

void NPlaybackEngineGStreamer::play()
{
	if (!hasMedia() || m_crossfading)
		return;

	GstState gstState;
	gst_element_get_state(m_playbin, &gstState, NULL, 0);
	if (gstState != GST_STATE_PLAYING) {
		gst_element_set_state(m_playbin, GST_STATE_PLAYING);
		m_timer->start(100);
	} else {
		pause();
	}
}

void NPlaybackEngineGStreamer::pause()
{
	if (!hasMedia())
		return;

	gst_element_set_state(m_playbin, GST_STATE_PAUSED);

	m_timer->stop();
	checkStatus();
}

void NPlaybackEngineGStreamer::stop()
{
	if (!hasMedia())
		return;

	m_crossfading = FALSE;
	gst_element_set_state(m_playbin, GST_STATE_NULL);
}

bool NPlaybackEngineGStreamer::hasMedia()
{
	return !m_currentMedia.isEmpty();
}

QString NPlaybackEngineGStreamer::currentMedia()
{
	return m_currentMedia;
}

void NPlaybackEngineGStreamer::checkStatus()
{
	GstState gstState;
	gst_element_get_state(m_playbin, &gstState, NULL, 0);
	N::PlaybackState state = fromGstState(gstState);
	if (m_oldState != state)
		emit stateChanged(m_oldState = state);

	if (state == N::PlaybackPlaying || state == N::PlaybackPaused) {
		// duration may change for some reason
		// TODO use DURATION_CHANGED in gstreamer1.0
		gboolean res = gst_element_query_duration(m_playbin, GST_FORMAT_TIME, &m_durationNsec);
		if (!res)
			m_durationNsec = 0;
	}

	if (m_posponedPosition >= 0 && m_durationNsec > 0) {
		setPosition(m_posponedPosition);
		m_posponedPosition = -1;
		emit positionChanged(m_posponedPosition);
	} else {
		qreal pos;
		gint64 gstPos = 0;

		if (!hasMedia() || m_durationNsec <= 0) {
			pos = -1;
		} else {
			gboolean res = gst_element_query_position(m_playbin, GST_FORMAT_TIME, &gstPos);
			if (!res)
				gstPos = 0;
			pos = (qreal)gstPos / m_durationNsec;
		}

		if (m_oldPosition != pos) {
			if (m_oldPosition > pos)
				m_crossfading = FALSE;
			m_oldPosition = pos;
			emit positionChanged(m_crossfading ? 0 : m_oldPosition);
		}

		emit tick(m_crossfading ? 0 : gstPos / 1000000);
	}

#if defined Q_WS_WIN || defined Q_WS_MAC
	GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(m_playbin));
	GstMessage *msg = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
	if (msg) {
		switch (GST_MESSAGE_TYPE(msg)) {
		case GST_MESSAGE_EOS:
			_on_eos(bus, msg, this);
			break;
		case GST_MESSAGE_ERROR:
			_on_error(bus, msg, this);
			break;
		default:
			break;
		}
		gst_message_unref(msg);
	}
	gst_object_unref(bus);
#endif

	qreal vol = volume();
	if (qAbs(m_oldVolume - vol) > 0.0001) {
		m_oldVolume = vol;
		emit volumeChanged(vol);
	}

	if (state == N::PlaybackStopped)
		m_timer->stop();
}

void NPlaybackEngineGStreamer::_finish()
{
	stop();
	emit finished();
	emit stateChanged(m_oldState = N::PlaybackStopped);
}

void NPlaybackEngineGStreamer::_fail()
{
	if (!m_crossfading) // avoid thread deadlock
		stop();
	else
		m_crossfading = FALSE;
	emit mediaChanged(m_currentMedia = "");
	emit failed();
	emit stateChanged(m_oldState = N::PlaybackStopped);
}

void NPlaybackEngineGStreamer::_emitError(QString error)
{
	emit message(QMessageBox::Critical, QFileInfo(m_currentMedia).absoluteFilePath(), error);
}

void NPlaybackEngineGStreamer::_emitAboutToFinish()
{
	emit aboutToFinish();
}

void NPlaybackEngineGStreamer::_crossfadingPrepare()
{
	m_crossfading = TRUE;
}

void NPlaybackEngineGStreamer::_crossfadingCancel()
{
	m_crossfading = FALSE;
}
