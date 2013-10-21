#include "playengine.hpp"
#include "playengine_p.hpp"
#include "videorendereritem.hpp"
#include "globalqmlobject.hpp"

struct PlayEngine::Data {
	Data(PlayEngine *engine): p(engine) {}
	PlayEngine *p = nullptr;

	ImagePlayback image;

	MediaInfoObject mediaInfo;
	AvInfoObject videoInfo, audioInfo;

	bool hasImage = false;
	bool subStreamsVisible = true;

	Thread thread{p};
	AudioController *audio = nullptr;
	QByteArray fileName;
	QTimer imageTicker, avTicker;
	bool quit = false, looping = false, init = false;
	int start = 0;
	bool muted = false;
	int volume = 100;
	double amp = 1.0;
	double speed = 1.0;
	MPContext *mpctx = nullptr;
	VideoOutput *video = nullptr;
	GetMrlInt getStartTimeFunc, getCacheFunc;
	PlaylistModel playlist;
	QByteArray hwAccCodecs;
	QMutex mutex;
	QMap<QString, QString> subtitleNames;
	QList<QTemporaryFile*> subtitleFiles;

	int duration = 0, title = 0, audioSync = 0, begin = 0;
	StreamList subStreams, audioStreams, videoStreams;
	VideoRendererItem *renderer = nullptr;
	DvdInfo dvd;
	ChapterList chapters;

	QByteArray vfs;

	static int mpCommandFilter(MPContext *mpctx, mp_cmd *cmd) {
		auto e = static_cast<PlayEngine*>(mpctx->priv); auto d = e->d;
		if (cmd->id < 0) {
			QMutexLocker locker(&d->mutex);
			switch (cmd->id) {
			case MpSetProperty:
				mp_property_do(cmd->name, M_PROPERTY_SET, &cmd->args[0].v, mpctx);
				break;
			case MpSetAudioLevel:
				d->audio->setLevel(cmd->args[0].v.d);
				break;
			case MpSetAudioMuted:
				d->audio->setMuted(cmd->args[0].v.i);
				break;
			case MpResetAudioChain:
				reinit_audio_chain(mpctx);
				break;
			case MpResetDeint:
				d->video->setDeintOptions(d->deint_swdec, d->deint_hwdec);
				break;
			case MpSetDeintEnabled:
				d->video->setDeintEnabled(d->deint != DeintMode::None);
				break;
			default:
				break;
			}
			cmd->id = MP_CMD_IGNORE;
			return true;
		}
		return false;
	}
	static int mpEventFilter(MPContext *mpctx) {
		enum MpEvent : uint {
			None = 1u << MP_EVENT_NONE,
			Tick = 1u << MP_EVENT_TICK,
			PropertyChanged = 1u << MP_EVENT_PROPERTY,
			TracksChanged = 1u << MP_EVENT_TRACKS_CHANGED,
			StartFile  = 1u << MP_EVENT_START_FILE,
			EndFile = 1u << MP_EVENT_END_FILE
		};
		auto e = static_cast<PlayEngine*>(mpctx->priv);
		int &events = *reinterpret_cast<int*>(mpctx->command_ctx);
		if (events & TracksChanged) {
			std::array<StreamList, STREAM_TYPE_COUNT> streams;
			QString name[STREAM_TYPE_COUNT];
			name[STREAM_AUDIO] = tr("Audio %1");
			name[STREAM_VIDEO] = tr("Video %1");
			name[STREAM_SUB] = tr("Subtitle %1");
			for (int i=0; i<mpctx->num_tracks; ++i) {
				const auto track = mpctx->tracks[i];
				auto &list = streams[track->type];
				if (!list.contains(track->user_tid)) {
					auto &stream = list[track->user_tid];
					stream.m_title = QString::fromLocal8Bit(track->title);
					stream.m_lang = QString::fromLocal8Bit(track->lang);
					stream.m_id = track->user_tid;
					stream.m_name = name[track->type].arg(track->user_tid);
				}
			}
			postData(e, UpdateTrack, streams);
		}
		events = 0;
		return true;
	}
	int getStartTime(const Mrl &mrl) { return getStartTimeFunc ? getStartTimeFunc(mrl) : 0; }
	int getCache(const Mrl &mrl) { return getCacheFunc ? getCacheFunc(mrl) : 0; }
	QByteArray &setFileName(const Mrl &mrl) {
		fileName = "\"";
		fileName += mrl.toString().toLocal8Bit();
		fileName += "\"";
		return fileName;
	}
	template<typename T = int>
	bool enqueue(int id, const char *name = "", const T &v = 0) {
		const bool ret = mpctx && mpctx->input;
		if (ret) {
			mp_cmd_t *cmd = (mp_cmd_t*)talloc_ptrtype(NULL, cmd);
			cmd->id = id;
			cmd->name = (char*)name;
			if (std::is_same<bool, T>::value)
				getCmdArg<int>(cmd) = v;
			else
				getCmdArg<T>(cmd) = v;
			mp_input_queue_cmd(mpctx->input, cmd);
		}
		return ret;
	}
	void updateAudioLevel() { enqueue(MpSetAudioLevel, "", (double)(volume)*0.01*amp); }
	template<typename T> void setmp(const char *name, T value) { enqueue(MpSetProperty, name, value); }
	void tellmp(const QByteArray &cmd) {
		if (mpctx && mpctx->input)
			mp_input_queue_cmd(mpctx->input, mp_input_parse_cmd(mpctx->input, bstr0(cmd.data()), ""));
	}
	void tellmp(const QByteArray &cmd, std::initializer_list<QByteArray> list) {
		int size = cmd.size();
		for (auto &one : list) size += one.size();
		QByteArray str; str.reserve(size + list.size()*2 + 2); str = cmd;
		for (auto &one : list) { str += ' '; str += one; }
		tellmp(str);
	}
	template<typename T>
	void tellmp(const QByteArray &cmd, const T &arg) {
		tellmp(cmd, {qbytearray_from<T>(arg)});
	}
	template<typename T, typename S>
	void tellmp(const QByteArray &cmd, const T &a1, const S &a2) {
		tellmp(cmd, {qbytearray_from(a1), qbytearray_from(a2)});
	}
	template<typename T, typename S, typename R>
	void tellmp(const QByteArray &cmd, const T &a1, const S &a2, const R &a3) {
		tellmp(cmd, {qbytearray_from(a1), qbytearray_from(a2), qbytearray_from(a3)});
	}
//	template<template <typename> class T> void tellmp(const QByteArray &cmd, const T<QString> &args) {
//		QString c = cmd; for (auto arg : args) {c += _L(' ') % arg;} tellmp(c);
//	}

	int currentTrackId(int type) const {
		return (mpctx && mpctx->current_track[type]) ? mpctx->current_track[type]->user_tid : -1;
	}

	void clear() {
		dvd.clear();
		audioStreams.clear();
		videoStreams.clear();
		subStreams.clear();
		subtitleFiles.clear();
		qDeleteAll(subtitleFiles);
		subtitleFiles.clear();
		emit p->audioStreamsChanged(audioStreams);
		emit p->videoStreamsChanged(videoStreams);
		emit p->subtitleStreamsChanged(subStreams);
		title = 0;
	}

	void play(int time) {
		start = time;
		tellmp("loadfile", setFileName(playlist.loadedMrl()), 0);
	}

	bool load(int row, int start) {
		if (!playlist.isValidRow(row))
			return false;
		if (playlist.loaded() == row) {
			if (start >= 0 && !looping)
				play(start);
		} else {
			playlist.setLoaded(row);
			postData(p, MrlChanged, playlist.loadedMrl());
			if (start < 0)
				p->stop();
			else
				play(start);
		}
		return true;
	}

	VideoFormat videoFormat;
	DeintOption deint_swdec, deint_hwdec;
	DeintMode deint = DeintMode::Auto;
	QByteArray ao = "";
	AudioDriver audioDriver = AudioDriver::Auto;

	int position = 0;

	void updateMediaName() {
		QString name;
		auto mrl = p->mrl();
		if (mrl.isLocalFile())
			name = tr("File") % _L(": ") % QFileInfo(mrl.toLocalFile()).fileName();
		else if (mrl.isDvd()) {
			const auto dvd = p->dvd();
			if (dvd.volume.isEmpty())
				name = _L("DVD: ") % dvd.volume;
			else
				name = _L("DVD: ") % mrl.toLocalFile();
		} else
			name = _L("URL: ") % mrl.toString();
		mediaInfo.setName(name);
	}
};

PlayEngine::PlayEngine()
: d(new Data(this)) {
	d->audio = new AudioController(this);
	d->video = new VideoOutput(this);
	d->imageTicker.setInterval(20);
	d->avTicker.setInterval(200);
	d->updateMediaName();
	mp_register_player_command_filter(Data::mpCommandFilter);
	mp_register_player_event_filter(Data::mpEventFilter);
	connect(&d->imageTicker, &QTimer::timeout, [this] () {
		bool begin = false, duration = false, pos = false;
		if (d->hasImage) {
			pos = _Change(d->position, d->image.pos());
			begin = _Change(d->duration, d->image.duration());
			duration = _Change(d->begin, 0);
		}
		if (pos)
			emit tick(d->position);
		if (begin)
			emit beginChanged(d->begin);
		if (duration)
			emit durationChanged(d->duration);
		if (begin || duration)
			emit endChanged(end());
		if (pos || begin || duration)
			emit relativePositionChanged();
	});
	connect(&d->avTicker, &QTimer::timeout, [this] () {
		d->position = get_current_time(d->mpctx)*1000.0 + 0.5;
		emit tick(d->position);
		emit relativePositionChanged();
	});
	connect(d->video, &VideoOutput::formatChanged, [this] (const VideoFormat &format) {
		postData(this, VideoFormatChanged, format);
	});
	connect(&d->playlist, &PlaylistModel::playRequested, [this] (int row) {
		d->load(row, d->getStartTime(d->playlist[row]));
	});
}

PlayEngine::~PlayEngine() {
	delete d->audio;
	delete d->video;
//	finalizeGL();
	delete d;
}

int PlayEngine::begin() const { return d->begin; }
int PlayEngine::end() const { return d->begin + d->duration; }

void PlayEngine::setImageDuration(int duration) {
	d->image.setDuration(duration);
}

int PlayEngine::duration() const {
	return d->duration;
}

const DvdInfo &PlayEngine::dvd() const {return d->dvd;}
int PlayEngine::currentDvdTitle() const {return d->title;}
ChapterList PlayEngine::chapters() const {return d->chapters;}

StreamList PlayEngine::subtitleStreams() const {return d->subStreams;}

VideoRendererItem *PlayEngine::videoRenderer() const {return d->renderer;}

StreamList PlayEngine::videoStreams() const {return d->videoStreams;}

int PlayEngine::audioSync() const {return d->audioSync;}
StreamList PlayEngine::audioStreams() const {return d->audioStreams;}

void PlayEngine::run() {
	d->thread.start();
}

QThread *PlayEngine::thread() const {
	return &d->thread;
}

void PlayEngine::waitUntilTerminated() {
	d->thread.wait();
}

void PlayEngine::waitUntilInitilaized() {
	while (!d->init)
		QThread::msleep(1);
//	d->ticker.start();
	d->videoInfo.setVideo(this);
	d->audioInfo.setAudio(this);
}

double PlayEngine::speed() const {
	return d->speed;
}

void PlayEngine::setSpeed(double speed) {
	if (_ChangeZ(d->speed, speed)) {
		d->setmp("speed", speed);
		emit speedChanged(d->speed);
	}
}

void PlayEngine::seek(int pos) {
	if (d->hasImage)
		d->image.seek(pos, false);
	else
		d->tellmp("seek", (double)pos/1000.0, 2);
}

void PlayEngine::relativeSeek(int pos) {
	if (d->hasImage)
		d->image.seek(pos, true);
	else
		d->tellmp("seek", (double)pos/1000.0, 0);
	emit sought();
}

void PlayEngine::setClippingMethod(ClippingMethod method) {
	d->audio->setClippingMethod(method);
}

typedef QPair<AudioDriver, const char*> AudioDriverName;
const std::array<AudioDriverName, AudioDriverInfo::size()-1> audioDriverNames = {{
	{AudioDriver::ALSA, "alsa"},
	{AudioDriver::PulseAudio, "pulse"},
	{AudioDriver::CoreAudio, "coreaudio"},
	{AudioDriver::PortAudio, "portaudio"},
	{AudioDriver::JACK, "jack"},
	{AudioDriver::OpenAL, "openal"}
}};

void PlayEngine::setAudioDriver(AudioDriver driver) {
	if (_Change(d->audioDriver, driver)) {
		auto it = _FindIf(audioDriverNames, [driver] (const AudioDriverName &one) { return one.first == driver; });
		d->ao = it != audioDriverNames.end() ? it->second : "";
	}
}

QQuickItem *PlayEngine::screen() const {
	return d->renderer;
}

AudioDriver PlayEngine::preferredAudioDriver() const {
	return d->audioDriver;
}

AudioDriver PlayEngine::audioDriver() const {
	if (!d->mpctx->ao)
		return preferredAudioDriver();
	auto name = d->mpctx->ao->driver->info->short_name;
	auto it = _FindIf(audioDriverNames, [name] (const AudioDriverName &one) { return !qstrcmp(name, one.second);});
	return it != audioDriverNames.end() ? it->first : AudioDriver::Auto;
}

void PlayEngine::setMinimumCache(int playback, int seeking) {
	d->mpctx->opts->stream_cache_min_percent = playback;
	d->mpctx->opts->stream_cache_pause = playback*0.5;
	d->mpctx->opts->stream_cache_seek_min_percent = seeking;
}

void PlayEngine::setGetCacheFunction(const GetMrlInt &func) {
	d->getCacheFunc = func;
}

void PlayEngine::setGetStartTimeFunction(const GetMrlInt &func) {
	d->getStartTimeFunc = func;
}

double PlayEngine::volumeNormalizer() const {
	auto gain = d->audio->gain(); return gain < 0 ? 1.0 : gain;
}

bool PlayEngine::isHwAccActivated() const {
	return d->video->hwAcc() != nullptr;
}

void PlayEngine::setHwAccCodecs(const QList<int> &codecs) {
	d->hwAccCodecs.clear();
	for (auto id : codecs) {
		if (const char *name = HwAcc::codecName(id)) {
			d->hwAccCodecs.append(name);
			d->hwAccCodecs.append(',');
		}
	}
	d->hwAccCodecs.chop(1);
}

bool PlayEngine::isSubtitleStreamsVisible() const {return d->subStreamsVisible;}

void PlayEngine::setSubtitleStreamsVisible(bool visible) {
	d->subStreamsVisible = visible;
	const auto id = currentSubtitleStream();
	d->setmp("sub-visibility", (d->subStreamsVisible && id >= 0));
}

void PlayEngine::setCurrentSubtitleStream(int id) {
	d->setmp("sub-visibility", (d->subStreamsVisible && id >= 0));
	d->setmp("sub", id);
}

int PlayEngine::currentSubtitleStream() const {
	return d->currentTrackId(STREAM_SUB);
}

void PlayEngine::addSubtitleStream(const QString &fileName, const QString &enc) {
	QFileInfo info(fileName);
	QFile in(fileName);
	QTemporaryFile *out = new QTemporaryFile(QDir::tempPath() % "/XXXXXX_" % info.fileName());
	if (in.open(QFile::ReadOnly) && out->open()) {
		QTextStream sin, sout;
		sin.setDevice(&in);
		sin.setCodec(enc.toLocal8Bit());
		sout.setDevice(out);
		sout.setCodec("UTF-8");
		QString line;
		while (!(line = sin.readLine()).isNull())
			sout << line << endl;
		sout.flush();
	} else {
		delete out;
		out = nullptr;
	}
	if (out) {
		out->close();
		d->subtitleFiles.append(out);
		d->subtitleNames[out->fileName()] = info.fileName();
		d->tellmp("sub_add", out->fileName());
	}
}

void PlayEngine::removeSubtitleStream(int id) {
	auto it = d->subStreams.find(id);
	if (it != d->subStreams.end()) {
		auto fileName = it->fileName();
		if (!fileName.isEmpty()) {
			d->subtitleNames.remove(fileName);
			for (int i=0; i<d->subtitleFiles.size(); ++i) {
				if (d->subtitleFiles[i]->fileName() == fileName) {
					delete d->subtitleFiles.takeAt(i);
					break;
				}
			}
		}
		d->tellmp("sub_remove", id);
	}
}

double PlayEngine::avgfps() const {
	return d->renderer->avgfps();
}

double PlayEngine::avgsync() const {
	double sync = 0.0;
	if (d->mpctx) {
		if (d->mpctx->sh_audio && d->mpctx->sh_video)
			sync = (d->mpctx->last_av_difference)*1000.0;
		if (d->renderer)
			sync -= d->renderer->delay();
	}
	return sync;
}

template<typename T>
static bool _CheckSwap(T &the, T &one) { if (the != one) { the.swap(one); return true; } return false; }

void PlayEngine::customEvent(QEvent *event) {
	switch ((int)event->type()) {
	case UpdateChapterList: {
		auto chapters = getData<ChapterList>(event);
		if (_CheckSwap(d->chapters, chapters))
			emit chaptersChanged(d->chapters);
		break;
	} case UpdateTrack: {
		auto streams = getData<std::array<StreamList, STREAM_TYPE_COUNT>>(event);
		if (_CheckSwap(d->videoStreams, streams[STREAM_VIDEO]))
			emit videoStreamsChanged(d->videoStreams);
		if (_CheckSwap(d->audioStreams, streams[STREAM_AUDIO]))
			emit audioStreamsChanged(d->audioStreams);
		if (!streams[STREAM_SUB].isEmpty()) {
			streams[STREAM_SUB][-1].m_name = tr("No Subtitle");
			for (auto &one : streams[STREAM_SUB]) {
				auto it = d->subtitleNames.constFind(one.m_title);
				if (it != d->subtitleNames.constEnd()) {
					one.m_fileName.swap(one.m_title);
					one.m_title = *it;
				}
			}
		}
		if (_CheckSwap(d->subStreams, streams[STREAM_SUB]))
			emit subtitleStreamsChanged(d->subStreams);
		break;
	} case StreamOpen:
		d->updateMediaName();
		d->audioInfo.setAudio(this);
		d->start = 0;
		d->position = 0;
		emit tick(d->position);
		emit seekableChanged(isSeekable());
		emit started(d->playlist.loadedMrl());
		emit mediaChanged();
		emit audioChanged();
		break;
	case StateChange: {
		const auto state = getData<PlayEngine::State>(event);
		const bool wasRunning = isRunning();
		if (_Change(m_state, state)) {
			emit stateChanged(m_state);
			if (m_state == Playing) {
				if (d->hasImage)
					d->imageTicker.start();
				else if (d->mpctx && d->mpctx->demuxer)
					d->avTicker.start();
			} else {
				if (d->hasImage)
					d->imageTicker.stop();
				else
					d->avTicker.stop();
			}
			if (wasRunning != isRunning())
				emit runningChanged();
		}
		break;
	} case MrlStopped: {
		Mrl mrl; int terminated = 0, duration = 0;
		getAllData(event, mrl, terminated, duration);
		emit stopped(mrl, terminated, duration);
		break;
	} case MrlFinished: {
		const auto mrl = getData<Mrl>(event);
		emit finished(mrl);
		break;
	} case PlaylistFinished:
		emit d->playlist.finished();
		break;
	case TimeRangeChange:
		getAllData(event, d->begin, d->duration);
		emit durationChanged(d->duration);
		emit beginChanged(d->begin);
		emit endChanged(end());
		break;
	case MrlChanged: {
		const auto mrl = getData<Mrl>(event);
		d->updateMediaName();
		emit mrlChanged(mrl);
		emit mediaChanged();
	} case VideoFormatChanged: {
		const auto format = getData<VideoFormat>(event);
		if (_Change(d->videoFormat, format)) {
			d->videoInfo.setVideo(this);
			emit videoFormatChanged(d->videoFormat);
			emit videoChanged();
		}
	} default:
		break;
	}
}

MediaInfoObject *PlayEngine::mediaInfo() const {
	return &d->mediaInfo;
}

AvInfoObject *PlayEngine::audioInfo() const {
	return &d->audioInfo;
}

AvInfoObject *PlayEngine::videoInfo() const {
	return &d->videoInfo;
}

void PlayEngine::setState(PlayEngine::State state) {
	postData(this, StateChange, state);
}

void PlayEngine::setCurrentChapter(int id) {
	d->setmp("chapter", id);
}

void PlayEngine::setCurrentDvdTitle(int id) {
	auto mrl = d->playlist.loadedMrl();
	if (mrl.isDvd()) {
		const QString path = "dvd://" % QString::number(id) % mrl.toString().mid(6);
		d->fileName = path.toLocal8Bit();
		d->tellmp("loadfile", path, 0);
	}
}

bool PlayEngine::parse(const Id &id) {
	if (id.name.isEmpty())
		return false;
	if (id.name.startsWith(_L("DVD_"))) {
		auto dvd = id.name.midRef(4);
		if (_Same(dvd, "TITLES")) {
//			m_dvd.titles[id.value.toInt()];
		} else if(dvd.startsWith(_L("TITLE_"))) {
			auto title = _MidRef(dvd, 6);
			int idx = id.name.indexOf(_L("_"), title.position());
			if (idx != -1) {
				bool ok = false;
				int tid = id.name.mid(title.position(), idx-title.position()).toInt(&ok);
				if (ok) {
					auto var = id.name.midRef(idx+1);
					auto &title = d->dvd.titles[tid];
					title.m_id = tid;
					title.number = tid;
					title.m_name = tr("Title %1").arg(tid);
					if (_Same(var, "CHAPTERS"))
						title.chapters = id.value.toInt();
					else if (_Same(var, "ANGLES"))
						title.angles = id.value.toInt();
					else if (_Same(var, "LENGTH"))
						title.length = id.value.toDouble()*1000+0.5;
					else
						return false;
				} else
					return false;
			} else
				return false;
		} else if (_Same(dvd, "VOLUME_ID")) {
			d->dvd.volume = id.value;
		} else if (_Same(dvd, "CURRENT_TITLE")) {
			d->title = id.value.toInt();
		} else
			return false;
		return true;
	} else
		return false;
	return true;
}

MPContext *PlayEngine::context() const {
	return d->mpctx;
}



bool PlayEngine::isInitialized() const {
	return d->init;
}

int PlayEngine::playImage(const Mrl &mrl, int &terminated, int &duration) {
	QImage image;
	if (!image.load(mrl.toLocalFile()))
		return MPERROR_OPEN_FILE;
	auto mpctx = d->mpctx;
	auto error = prepare_playback(mpctx);
	if (error != MPERROR_NONE)
		return error;
	setState(mpctx->paused ? PlayEngine::Paused : PlayEngine::Playing);
	d->video->output(image);
	postData(this, StreamOpen);
	postData(this, UpdateChapterList, ChapterList());
	d->image.restart();
	while (!mpctx->stop_play) {
		mp_cmd_t *cmd = nullptr;
		while ((cmd = mp_input_get_cmd(mpctx->input, 0, 1)) != NULL) {
			cmd = mp_input_get_cmd(mpctx->input, 0, 0);
			run_command(mpctx, cmd);
			mp_cmd_free(cmd);
			if (mpctx->stop_play)
				break;
		}
		if (!d->image.run(isPaused()))
			break;
		QThread::msleep(50);
	}
	terminated = duration = 0;
	return error;
}

int PlayEngine::playAudioVideo(const Mrl &/*mrl*/, int &terminated, int &duration) {
	d->video->output(QImage());
	auto mpctx = d->mpctx;
	if (d->hwAccCodecs.isEmpty()) {
		d->mpctx->opts->hwdec_api = HWDEC_NONE;
		d->mpctx->opts->hwdec_codecs = nullptr;
	} else {
#ifdef Q_OS_LINUX
		d->mpctx->opts->hwdec_api = HWDEC_VAAPI;
#elif defined(Q_OS_MAC)
		d->mpctx->opts->hwdec_api = HWDEC_VDA;
#endif
		d->mpctx->opts->hwdec_codecs = d->hwAccCodecs.data();
	}
	d->mpctx->opts->stream_cache_size = d->getCache(Mrl(QString::fromLocal8Bit(d->mpctx->playlist->current->filename)));
	d->mpctx->opts->audio_driver_list->name = d->ao.data();
	d->mpctx->opts->play_start.pos = d->start*1e-3;
	d->mpctx->opts->play_start.type = REL_TIME_ABSOLUTE;
	d->setmp("audio-delay", d->audioSync*0.001);
	d->video->setDeintOptions(d->deint_swdec, d->deint_hwdec);
	d->video->setDeintEnabled(d->deint != DeintMode::None);
	auto error = prepare_playback(mpctx);
	d->updateAudioLevel();
	if (error != MPERROR_NONE)
		return error;
	postData(this, StreamOpen);
	ChapterList chapters(get_chapter_count(mpctx));
	for (int i=0; i<chapters.size(); ++i) {
		const QString time = _MSecToString(chapter_start_time(mpctx, i)*1000, _L("hh:mm:ss.zzz"));
		if (char *name = chapter_name(mpctx, i)) {
			chapters[i].m_name = QString::fromLocal8Bit(name) % '(' % time % ')';
			talloc_free(name);
		} else
			chapters[i].m_name = '(' % QString::number(i+1) % ") " % time;
		chapters[i].m_id = i;
	}
	uint title = 0, titles = 0;
	if (mpctx->demuxer && mpctx->demuxer->stream) {
		auto stream = mpctx->demuxer->stream;
		stream->control(stream, STREAM_CTRL_GET_CURRENT_TITLE, &title);
		stream->control(stream, STREAM_CTRL_GET_NUM_TITLES, &titles);
	}
	postData(this, UpdateChapterList, chapters);
	int begin = 0; duration = 0;
	auto checkTimeRange = [this, &begin, &duration] () {
		if (_Change(begin, int(get_start_time(d->mpctx)*1000))
				| _Change(duration, int(get_time_length(d->mpctx)*1000)))
			postData(this, TimeRangeChange, begin, duration);
	};
	d->tellmp("vf set", d->vfs);
	auto state = this->state(), newState = Loading;
	while (!mpctx->stop_play) {
		if (!duration)
			checkTimeRange();
		run_playloop(mpctx);
		if (mpctx->stop_play)
			break;
		newState = Playing;
		if (mpctx->paused_for_cache && !mpctx->opts->pause)
			newState = Loading;
		else if (mpctx->paused)
			newState = Paused;
		if (_Change(state, newState))
			setState(state);
	}
	terminated = time();
	duration = this->duration();
	return error;
}

void PlayEngine::setVolume(int volume) {
	if (_Change(d->volume, qBound(0, volume, 100))) {
		d->updateAudioLevel();
		emit volumeChanged(d->volume);
	}
}

bool PlayEngine::isMuted() const {
	return d->muted;
}

int PlayEngine::volume() const {
	return d->volume;
}

double PlayEngine::amp() const {
	return d->amp;
}

void PlayEngine::setAmp(double amp) {
	if (_ChangeZ(d->amp, qBound(0.0, amp, 10.0))) {
		d->updateAudioLevel();
		emit preampChanged(d->amp);
	}
}

void PlayEngine::setMuted(bool muted) {
	if (_Change(d->muted, muted)) {
		d->enqueue(MpSetAudioMuted, "", (int)muted);
		d->setmp("mute", (int)d->muted);
		emit mutedChanged(d->muted);
	}
}

void PlayEngine::exec() {
	QStringList args;
	args << "cmplayer-mpv";
	auto mpvOptions = qgetenv("CMPLAYER_MPV_OPTIONS").trimmed();
	if (!mpvOptions.isEmpty())
		args += QString::fromLocal8Bit(mpvOptions).split(' ', QString::SkipEmptyParts);
	args << "--no-config" << "--idle" << "--no-fs"
		<< ("--af=dummy:address=" % QString::number((quint64)(quintptr)(void*)(d->audio)))
		<< ("--vo=null:address=" % QString::number((quint64)(quintptr)(void*)(d->video)))
		<< "--softvol=yes" << "--softvol-max=1000.0" << "--fixed-vo" << "--no-autosub" << "--osd-level=0" << "--quiet" << "--identify"
		<< "--no-consolecontrols" << "--no-mouse-movements" << "--subcp=utf8" << "--ao=null,";
	QVector<QByteArray> args_byte(args.size());
	QVector<char*> args_raw(args.size());
	for (int i=0; i<args.size(); ++i) {
		args_byte[i] = args[i].toLocal8Bit();
		args_raw[i] = args_byte[i].data();
	}
	qDebug() << "Initialize mpv with" << args;
	auto mpctx = d->mpctx = create_player(args_raw.size(), args_raw.data());
	d->mpctx->priv = this;
	auto tmp_ao = d->mpctx->opts->audio_driver_list->name;
	d->init = true;
	d->quit = false;
	initialize_vaapi();
	initialize_vdpau();
	while (!d->quit) {
		d->hasImage = false;
		idle_player(mpctx);
		if (mpctx->stop_play == PT_QUIT)
			break;
		if (d->quit)
			break;
		Q_ASSERT(mpctx->playlist->current);
		d->clear();
		Mrl mrl = d->playlist.loadedMrl();
		d->looping = true;
		setState(PlayEngine::Loading);
		d->hasImage = mrl.isImage();
		int terminated = 0, duration = 0;
		int error = MPERROR_NONE;
		if (d->hasImage)
			error = playImage(mrl, terminated, duration);
		else
			error = playAudioVideo(mrl, terminated, duration);
		clean_up_playback(mpctx);
		if (error != MPERROR_NONE)
			setState(PlayEngine::Error);
		d->looping = false;
		qDebug() << "terminate playback";
		if (mpctx->stop_play == PT_QUIT) {
			if (error == MPERROR_NONE) {
				setState(PlayEngine::Stopped);
				postData(this, MrlStopped, d->playlist.loadedMrl(), terminated, duration);
			}
			break;
		}
		playlist_entry *entry = nullptr;
		if (error == MPERROR_NONE) {
			switch (mpctx->stop_play) {
			case KEEP_PLAYING:
			case AT_END_OF_FILE: {// finished
				setState(PlayEngine::Finished);
				postData(this, MrlFinished, mrl);
				playlist_clear(mpctx->playlist);
				if (d->playlist.hasNext()) {
					const auto prev = d->playlist.loadedMrl();
					d->playlist.setLoaded(d->playlist.next());
					const auto mrl = d->playlist.loadedMrl();
					if (prev != mrl)
						postData(this, MrlChanged, mrl);
					d->start = d->getStartTime(mrl);
					playlist_add(mpctx->playlist, playlist_entry_new(mrl.toString().toLocal8Bit()));
					entry = mpctx->playlist->first;
				} else
					postData(this, PlaylistFinished);
				break;
			} case PT_CURRENT_ENTRY: // stopped by loadfile
				entry = mpctx->playlist->current;
			default: // just stopped
				setState(PlayEngine::Stopped);
				postData(this, MrlStopped, mrl, terminated, duration);
				break;
			}
		}
		mpctx->playlist->current = entry;
		mpctx->playlist->current_was_replaced = false;
		mpctx->stop_play = KEEP_PLAYING;
		if (!mpctx->playlist->current && !mpctx->opts->player_idle_mode)
			break;
	}
	qDebug() << "terminate loop";
	mpctx->opts->hwdec_codecs = nullptr;
	mpctx->opts->hwdec_api = HWDEC_NONE;
	mpctx->opts->audio_driver_list->name = tmp_ao;
	destroy_player(mpctx);
	finalize_vaapi();
	finalize_vdpau();
	d->mpctx = nullptr;
	d->init = false;
	qDebug() << "terminate engine";
}


void PlayEngine::setVideoFilters(const QString &vfs) {
	if (_Change(d->vfs, vfs.toLocal8Bit()) && d->looping)
		d->tellmp("vf set", d->vfs);
}

void PlayEngine::quit() {
	d->tellmp("quit 1");
}

void PlayEngine::reload() {
	d->play(time());
}

void PlayEngine::load(const Mrl &mrl, bool play) {
	load(mrl, play ? d->getStartTime(mrl) : -1);
}

void PlayEngine::load(const Mrl &mrl, int start) {
	auto row = d->playlist.rowOf(mrl);
	if (row < 0)
		row = d->playlist.append(mrl);
	d->load(row, start);
}

int PlayEngine::time() const {
	return d->position;
}

bool PlayEngine::isSeekable() const {
	return d->mpctx && d->mpctx->stream && d->mpctx->stream->seek && (!d->mpctx->demuxer || d->mpctx->demuxer->seekable);
}

bool PlayEngine::hasVideo() const {
	return d->mpctx && d->mpctx->sh_video;
}

bool PlayEngine::atEnd() const {
	return d->mpctx->stop_play == AT_END_OF_FILE;
}

int PlayEngine::currentChapter() const {
	if (d->looping)
		return get_current_chapter(d->mpctx);
	return -2;
}

void PlayEngine::pause() {
	if (d->hasImage)
		setState(PlayEngine::Paused);
	else
		d->setmp("pause", 1);
}

void PlayEngine::unpause() {
	if (d->hasImage)
		setState(PlayEngine::Playing);
	else
		d->setmp("pause", 0);
}

void PlayEngine::play() {
	switch (m_state) {
	case PlayEngine::Stopped:
	case PlayEngine::Finished:
	case PlayEngine::Error:
		d->play(d->getStartTime(d->playlist.loadedMrl()));
		break;
	case PlayEngine::Loading:
		// do nothing. just wait
		break;
	default:
		unpause();
		break;
	}
}

void PlayEngine::setPlaylist(const Playlist &playlist) {
	d->playlist.setPlaylist(playlist);
}

Mrl PlayEngine::mrl() const {
	return d->playlist.loadedMrl();
}

int PlayEngine::currentAudioStream() const {
	return d->currentTrackId(STREAM_AUDIO);
}

void PlayEngine::setCurrentVideoStream(int id) {
	d->setmp("video", id);
}

int PlayEngine::currentVideoStream() const {
	return hasVideo() ? d->currentTrackId(STREAM_VIDEO) : -1;
}

void PlayEngine::setCurrentAudioStream(int id) {
	d->setmp("audio", id);
}

void PlayEngine::setAudioSync(int sync) {
	if (_Change(d->audioSync, sync))
		d->setmp("audio-delay", (float)(sync*0.001));
}

const PlaylistModel &PlayEngine::playlist() const {
	return d->playlist;
}

PlaylistModel &PlayEngine::playlist() {
	return d->playlist;
}

double PlayEngine::fps() const {
	return hasVideo() ? d->mpctx->sh_video->fps : 25;
}

void PlayEngine::setVideoRenderer(VideoRendererItem *renderer) {
	if (_Change(d->renderer, renderer))
		d->video->setRenderer(d->renderer);
}

double PlayEngine::bps(double fps) const {
	return d->videoFormat.bps(fps);
}

VideoFormat PlayEngine::videoFormat() const {
	return d->videoFormat;
}

void PlayEngine::registerObjects() {
	static auto utilProvider = [](QQmlEngine *, QJSEngine *) -> QObject* {return new UtilObject;};
	static auto settingsProvider = [](QQmlEngine *, QJSEngine *) -> QObject* {return new SettingsObject;};

	qRegisterMetaType<PlayEngine::State>("State");
	qRegisterMetaType<Mrl>("Mrl");
	qRegisterMetaType<VideoFormat>("VideoFormat");
	qRegisterMetaType<QVector<int>>("QVector<int>");
	qRegisterMetaType<StreamList>("StreamList");
	qmlRegisterType<AvInfoObject>();
	qmlRegisterType<AvIoFormat>();
	qmlRegisterType<MediaInfoObject>();
	qmlRegisterType<PlayEngine>("CMPlayerCore", 1, 0, "Engine");
	qmlRegisterSingletonType<UtilObject>("CMPlayerCore", 1, 0, "Util", utilProvider);
	qmlRegisterSingletonType<SettingsObject>("CMPlayerCore", 1, 0, "Settings", settingsProvider);
}

void PlayEngine::setVolumeNormalizerActivated(bool on) {
	if (d->audio->setNormalizerActivated(on))
		emit volumeNormalizerActivatedChanged(on);
}

void PlayEngine::setTempoScalerActivated(bool on) {
	if (d->audio->setTempoScalerActivated(on)) {
		if (d->looping)
			d->enqueue(MpResetAudioChain);
		emit tempoScaledChanged(on);
	}
}

bool PlayEngine::isVolumeNormalizerActivated() const {
	return d->audio->isNormalizerActivated();
}

bool PlayEngine::isTempoScaled() const {
	return d->audio->isTempoScalerActivated();
}

void PlayEngine::stop() {
	d->tellmp("stop");
}

void PlayEngine::setVolumeNormalizerOption(double length, double target, double silence, double min, double max) {
	d->audio->setNormalizerOption(length, target, silence, min, max);
}

void PlayEngine::setDeintOptions(const DeintOption &swdec, const DeintOption &hwdec) {
	if (d->deint_swdec == swdec && d->deint_hwdec == hwdec)
		return;
	QMutexLocker locker(&d->mutex);
	if (d->deint_swdec == swdec && d->deint_hwdec == hwdec)
		return;
	d->deint_swdec = swdec;
	d->deint_hwdec = hwdec;
	d->enqueue(MpResetDeint);
}

void PlayEngine::setDeintMode(DeintMode mode) {
	if (d->deint == mode)
		return;
	QMutexLocker locker(&d->mutex);
	if (d->deint == mode)
		return;
	d->deint = mode;
	d->enqueue(MpSetDeintEnabled);
}

DeintMode PlayEngine::deintMode() const {
	return d->deint;
}

QString PlayEngine::stateText() const {
	switch (m_state) {
	case Playing:
		return tr("Playing");
	case Stopped:
		return tr("Stopped");
	case Finished:
		return tr("Finished");
	case Loading:
		return tr("Loading");
	case Error:
		return tr("Error");
	default:
		return tr("Paused");
	}
}
