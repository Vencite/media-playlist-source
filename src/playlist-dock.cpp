#include "playlist-dock.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

#include <QAbstractItemView>
#include <QAction>
#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QIcon>
#include <QLabel>
#include <QMetaObject>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariant>
#include <QVector>
#include <QVBoxLayout>

#include <callback/calldata.h>
#include <callback/proc.h>

#include "playlist-dock.h"
#include "playlist-telemetry.h"
#include "program-sources.hpp"

static QString filename_from_path(const QString &path)
{
	if (path.isEmpty())
		return QStringLiteral("—");

	const int slash = std::max(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
	return slash >= 0 ? path.mid(slash + 1) : path;
}

class ElidedFileLabel final : public QLabel {
public:
	explicit ElidedFileLabel(QWidget *parent = nullptr) : QLabel(parent)
	{
		setWordWrap(false);
		setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		setMinimumWidth(0);
		setTextInteractionFlags(Qt::TextSelectableByMouse);
		setFilePath(nullptr);
	}

	void setFilePath(const char *path)
	{
		full_path_ = QString::fromUtf8(path ? path : "");
		filename_ = filename_from_path(full_path_);
		setToolTip(full_path_.isEmpty() ? QString() : full_path_);
		update_text();
	}

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QLabel::resizeEvent(event);
		update_text();
	}

private:
	void update_text()
	{
		if (full_path_.isEmpty()) {
			setText(QStringLiteral("—"));
			return;
		}
		setText(fontMetrics().elidedText(filename_, Qt::ElideMiddle, qMax(1, contentsRect().width())));
	}

	QString full_path_;
	QString filename_;
};

namespace {

constexpr const char *kQueueDockId = "media-playlist-queue-dock";
constexpr const char *kControlDockId = "media-playlist-control-dock";
constexpr int kTimerIntervalMs = 250;
constexpr int kMediaIndexRole = Qt::UserRole;
constexpr int kFolderItemIndexRole = Qt::UserRole + 1;
constexpr int kStableIdRole = Qt::UserRole + 2;
constexpr int kPathRole = Qt::UserRole + 3;
constexpr const char *source_events[] = {"source_create", "source_remove", "source_destroy", "source_rename"};
constexpr const char *scene_events[] = {"item_add", "item_remove", "item_visible"};
constexpr const char *transition_events[] = {"transition_start", "transition_stop"};

QString filename_from_entry(const struct mps_playlist_item_snapshot &entry)
{
	if (entry.filename && *entry.filename)
		return QString::fromUtf8(entry.filename);
	return filename_from_path(QString::fromUtf8(entry.path ? entry.path : ""));
}

OBSSourceAutoRelease dock_source(const QString &uuid)
{
	if (uuid.isEmpty())
		return nullptr;
	OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.toUtf8().constData());
	if (!mps_is_playlist_source(source) || obs_source_removed(source))
		return nullptr;
	return source;
}

OBSSourceAutoRelease command_source(const PlaylistProgramTracker &program, bool follow, const QString &uuid)
{
	if (follow && !program.is_current_program_source(uuid))
		return nullptr;
	return dock_source(uuid);
}

QString follow_status_text(std::size_t active_source_count)
{
	if (!active_source_count)
		return QObject::tr("No active MPS");
	if (active_source_count == 1)
		return QObject::tr("Program");
	return QObject::tr("%1 active MPS").arg(active_source_count);
}

void configure_follow_context_menu(QWidget *dock, QWidget *source_header, QAction *action)
{
	dock->setContextMenuPolicy(Qt::ActionsContextMenu);
	dock->addAction(action);
	source_header->setContextMenuPolicy(Qt::ActionsContextMenu);
	source_header->addAction(action);
}

template<typename SourceRef>
void add_playback_context_actions(QWidget *dock, QWidget *header, SourceRef source_ref, QAction **shuffle,
				  QAction **loop)
{
	const auto add = [dock, header, source_ref](const QString &text, const char *key, const char *name) {
		auto *action = new QAction(text, dock);
		action->setObjectName(QString::fromUtf8(name));
		action->setCheckable(true);
		action->setEnabled(false);
		dock->addAction(action);
		header->addAction(action);
		QObject::connect(action, &QAction::triggered, dock, [source_ref, key](bool enabled) {
			auto source = source_ref();
			if (!source)
				return;
			OBSDataAutoRelease settings = obs_data_create();
			obs_data_set_bool(settings, key, enabled);
			obs_source_update(source, settings);
		});
		return action;
	};
	*shuffle = add(QObject::tr("Shuffle"), "shuffle", "mpsShuffle");
	*loop = add(QObject::tr("Loop"), "loop", "mpsLoop");
}

void update_playback_context_actions(QAction *shuffle, QAction *loop, bool available, bool shuffled, bool looping)
{
	const QSignalBlocker shuffle_blocker(shuffle), loop_blocker(loop);
	shuffle->setEnabled(available);
	loop->setEnabled(available);
	shuffle->setChecked(available && shuffled);
	loop->setChecked(available && looping);
}

QLabel *make_section_label(const QString &text, QWidget *parent = nullptr, bool bold = true)
{
	auto *label = new QLabel(text, parent);
	QFont font = label->font();
	font.setBold(bold);
	label->setFont(font);
	return label;
}

QLabel *make_secondary_label(const QString &text, QWidget *parent = nullptr)
{
	auto *label = new QLabel(text, parent);
	label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	label->setForegroundRole(QPalette::Text);
	return label;
}

QIcon palette_icon(QWidget *widget, QStyle::StandardPixmap icon, QPalette::ColorRole role)
{
	const QIcon source = widget->style()->standardIcon(icon);
	const int size = widget->style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, widget);
	QPixmap pixmap = source.pixmap(size, size);
	if (pixmap.isNull())
		return source;

	QPainter painter(&pixmap);
	painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
	painter.fillRect(pixmap.rect(), widget->palette().color(role));
	return QIcon(pixmap);
}

QLabel *make_section_icon(QStyle::StandardPixmap icon, QWidget *parent = nullptr)
{
	auto *label = new QLabel(parent);
	const int size = label->fontMetrics().height();
	label->setPixmap(palette_icon(label, icon, QPalette::Text).pixmap(size, size));
	label->setAlignment(Qt::AlignCenter);
	label->setFixedWidth(size + 4);
	return label;
}

void update_program_source_indicators(QComboBox *selector, const QHash<QString, QString> &active_sources)
{
	const QIcon active_icon = palette_icon(selector, QStyle::SP_MediaPlay, QPalette::Text);
	for (int index = 0; index < selector->count(); index++) {
		const bool active = active_sources.contains(selector->itemData(index).toString());
		selector->setItemIcon(index, active ? active_icon : QIcon());
		QFont font = selector->itemData(index, Qt::FontRole).value<QFont>();
		if (font == QFont())
			font = selector->font();
		font.setBold(active);
		selector->setItemData(index, font, Qt::FontRole);
		selector->setItemData(index, active ? QObject::tr("Active in Program scene") : QString(),
				      Qt::ToolTipRole);
	}
}

QString populate_source_selector(QComboBox *selector, QLabel *status, const PlaylistProgramTracker &program,
				 bool follow, QString &manual_uuid)
{
	const QSignalBlocker blocker(selector);
	const auto &active = program.program_sources();
	const QString program_uuid = active.size() == 1 ? active.cbegin().key() : QString();
	selector->clear();
	for (const auto &[uuid, name] : program.public_sources())
		selector->addItem(name, uuid);

	int manual_index = selector->findData(manual_uuid);
	if (manual_index < 0 && manual_uuid.isEmpty() && selector->count() > 0)
		manual_index = 0;
	manual_uuid = manual_index >= 0 ? selector->itemData(manual_index).toString() : QString();

	int selected_index = manual_index;
	if (follow && !program_uuid.isEmpty()) {
		selected_index = selector->findData(program_uuid);
		if (selected_index < 0) {
			/* Private Program copies have UUIDs but are absent from obs_enum_sources. */
			selected_index = selector->count();
			selector->addItem(active.value(program_uuid), program_uuid);
		}
	}
	selector->setCurrentIndex(selected_index);
	update_program_source_indicators(selector, active);
	status->setText(follow_status_text(static_cast<std::size_t>(active.size())));
	status->setVisible(follow);
	return follow ? program_uuid : manual_uuid;
}

QFrame *make_separator(QWidget *parent = nullptr)
{
	auto *separator = new QFrame(parent);
	separator->setFrameShape(QFrame::HLine);
	separator->setFrameShadow(QFrame::Sunken);
	return separator;
}

QString format_media_time(int64_t milliseconds)
{
	if (milliseconds < 0)
		return QStringLiteral("--:--");

	const int64_t total_seconds = milliseconds / 1000;
	const int64_t seconds = total_seconds % 60;
	const int64_t minutes = (total_seconds / 60) % 60;
	const int64_t hours = total_seconds / 3600;
	if (hours > 0)
		return QStringLiteral("%1:%2:%3")
			.arg(hours)
			.arg(minutes, 2, 10, QLatin1Char('0'))
			.arg(seconds, 2, 10, QLatin1Char('0'));
	return QStringLiteral("%1:%2").arg(minutes, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'));
}

QString format_media_duration(int64_t milliseconds)
{
	return milliseconds > 0 ? format_media_time(milliseconds) : QStringLiteral("--:--");
}

void set_selected_item_data(QTreeWidgetItem *item, std::size_t *media_index, std::size_t *folder_item_index,
			    QString *stable_id = nullptr)
{
	if (!item || !media_index || !folder_item_index || !item->data(0, kMediaIndexRole).isValid())
		return;
	*media_index = static_cast<std::size_t>(item->data(0, kMediaIndexRole).toULongLong());
	*folder_item_index = static_cast<std::size_t>(item->data(0, kFolderItemIndexRole).toULongLong());
	if (stable_id)
		*stable_id = item->data(0, kStableIdRole).toString();
}

PlaylistQueueDock *queue_dock_instance;
PlaylistControlDock *control_dock_instance;

} // namespace

PlaylistProgramTracker::PlaylistProgramTracker()
{
	auto *handler = obs_get_signal_handler();
	for (const char *event : source_events)
		signal_handler_connect(handler, event, source_event, this);
	signal_handler_connect(handler, "channel_change", channel_event, this);
	obs_frontend_add_event_callback(frontend_event, this);
	refresh();
}

PlaylistProgramTracker::~PlaylistProgramTracker()
{
	obs_frontend_remove_event_callback(frontend_event, this);
	auto *handler = obs_get_signal_handler();
	for (const char *event : source_events)
		signal_handler_disconnect(handler, event, source_event, this);
	signal_handler_disconnect(handler, "channel_change", channel_event, this);
	update_transition_connection(nullptr);
	update_scene_connections({});
}

void PlaylistProgramTracker::source_event(void *data, calldata_t *calldata)
{
	if (mps_is_playlist_source(static_cast<obs_source_t *>(calldata_ptr(calldata, "source"))))
		static_cast<PlaylistProgramTracker *>(data)->schedule_refresh();
}

void PlaylistProgramTracker::channel_event(void *data, calldata_t *calldata)
{
	if (calldata_int(calldata, "channel") == 0)
		static_cast<PlaylistProgramTracker *>(data)->schedule_refresh();
}

void PlaylistProgramTracker::graph_event(void *data, calldata_t *)
{
	static_cast<PlaylistProgramTracker *>(data)->schedule_refresh();
}

void PlaylistProgramTracker::frontend_event(enum obs_frontend_event event, void *data)
{
	auto *tracker = static_cast<PlaylistProgramTracker *>(data);
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		tracker->collection_changing_ = true;
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		tracker->collection_changing_ = false;
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_STOPPED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		break;
	default:
		return;
	}
	tracker->schedule_refresh();
}

void PlaylistProgramTracker::schedule_refresh()
{
	if (refresh_queued_.exchange(true))
		return;
	QMetaObject::invokeMethod(
		this,
		[this] {
			refresh_queued_.store(false);
			refresh();
		},
		Qt::QueuedConnection);
}

void PlaylistProgramTracker::update_scene_connections(const QSet<signal_handler_t *> &handlers)
{
	for (auto *handler : scene_handlers_ - handlers)
		for (const char *event : scene_events)
			signal_handler_disconnect(handler, event, graph_event, this);
	for (auto *handler : handlers - scene_handlers_)
		for (const char *event : scene_events)
			signal_handler_connect_ref(handler, event, graph_event, this);
	scene_handlers_ = handlers;
}

void PlaylistProgramTracker::update_transition_connection(signal_handler_t *handler)
{
	if (handler == transition_handler_)
		return;
	if (transition_handler_)
		for (const char *event : transition_events)
			signal_handler_disconnect(transition_handler_, event, graph_event, this);
	if (handler)
		for (const char *event : transition_events)
			signal_handler_connect_ref(handler, event, graph_event, this);
	transition_handler_ = handler;
}

void PlaylistProgramTracker::refresh()
{
	decltype(public_sources_) public_sources;
	decltype(program_sources_) program_sources;
	QSet<signal_handler_t *> scene_handlers;
	signal_handler_t *transition_handler = nullptr;

	/* Hold the traversed sources only while reading and connecting to their
	 * graph. connect_ref owns the signal handlers without extending source life. */
	OBSSourceAutoRelease output;
	MpsProgramSources graph;
	if (!collection_changing_) {
		obs_enum_sources(
			[](void *data, obs_source_t *source) {
				if (mps_is_playlist_source(source) && !obs_source_removed(source)) {
					const char *uuid = obs_source_get_uuid(source);
					if (uuid && *uuid)
						static_cast<decltype(public_sources) *>(data)->append(
							{QString::fromUtf8(uuid),
							 QString::fromUtf8(obs_source_get_name(source))});
				}
				return true;
			},
			&public_sources);

		/* Channel 0 includes the live transition and any temporary override.
		 * The frontend scene API exposes the editable original in Studio Mode. */
		output = obs_get_output_source(0);
		if (output && obs_source_get_type(output) == OBS_SOURCE_TYPE_TRANSITION)
			transition_handler = obs_source_get_signal_handler(output);
		update_transition_connection(transition_handler);
		graph = mps_program_sources(output);
		for (const auto &scene : graph.scenes)
			scene_handlers.insert(obs_source_get_signal_handler(scene));
		for (const auto &source : graph.playlists) {
			const char *uuid = obs_source_get_uuid(source);
			if (uuid && *uuid)
				program_sources.insert(QString::fromUtf8(uuid),
						       QString::fromUtf8(obs_source_get_name(source)));
		}
	} else {
		update_transition_connection(nullptr);
	}
	update_scene_connections(scene_handlers);

	if (public_sources == public_sources_ && program_sources == program_sources_)
		return;
	public_sources_ = std::move(public_sources);
	program_sources_ = std::move(program_sources);
	emit sources_changed();
}

bool PlaylistProgramTracker::is_current_program_source(const QString &uuid) const
{
	if (uuid.isEmpty() || collection_changing_)
		return false;
	/* A click can arrive before the queued graph refresh. Recheck at dispatch
	 * so a previous Program target cannot receive a follow-mode command. */
	OBSSourceAutoRelease output = obs_get_output_source(0);
	const auto graph = mps_program_sources(output);
	return graph.playlists.size() == 1 && uuid == QString::fromUtf8(obs_source_get_uuid(graph.playlists.front()));
}

PlaylistQueueDock::PlaylistQueueDock(std::shared_ptr<PlaylistProgramTracker> program, QWidget *parent)
	: QWidget(parent),
	  program_(std::move(program))
{
	setWindowTitle(tr("Playlist Queue"));

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);

	auto *source_header = new QWidget(this);
	auto *source_layout = new QGridLayout(source_header);
	source_layout->setContentsMargins(0, 0, 0, 0);
	source_layout->setHorizontalSpacing(8);
	source_layout->setVerticalSpacing(4);
	source_layout->addWidget(make_section_label(tr("Source"), source_header), 0, 0);
	follow_status_ = make_secondary_label(QString(), source_header);
	follow_status_->setVisible(false);
	source_layout->addWidget(follow_status_, 0, 1);
	source_selector_ = new QComboBox(source_header);
	source_selector_->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
	source_selector_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	source_layout->addWidget(source_selector_, 1, 0, 1, 2);
	source_layout->setColumnStretch(0, 1);
	layout->addWidget(source_header);
	layout->addWidget(make_separator(this));

	auto *context_layout = new QGridLayout;
	context_layout->setContentsMargins(0, 0, 0, 0);
	context_layout->setHorizontalSpacing(8);
	context_layout->setVerticalSpacing(5);
	context_layout->setColumnStretch(1, 1);
	context_layout->addWidget(make_section_icon(QStyle::SP_MediaSkipBackward, this), 0, 0);
	context_layout->addWidget(make_section_label(tr("Previous"), this), 0, 1);
	previous_duration_value_ = make_secondary_label(QStringLiteral("--:--"), this);
	context_layout->addWidget(previous_duration_value_, 0, 2);
	previous_value_ = new ElidedFileLabel(this);
	context_layout->addWidget(previous_value_, 1, 1, 1, 2);
	context_layout->addWidget(make_separator(this), 2, 0, 1, 3);

	context_layout->addWidget(make_section_icon(QStyle::SP_MediaPlay, this), 3, 0);
	context_layout->addWidget(make_section_label(tr("Now Playing"), this), 3, 1);
	current_duration_value_ = make_secondary_label(QStringLiteral("--:--"), this);
	context_layout->addWidget(current_duration_value_, 3, 2);
	current_value_ = new ElidedFileLabel(this);
	QFont current_font = current_value_->font();
	current_font.setBold(true);
	current_value_->setFont(current_font);
	context_layout->addWidget(current_value_, 4, 1, 1, 2);
	progress_bar_ = new QProgressBar(this);
	progress_bar_->setTextVisible(false);
	progress_bar_->setRange(0, 1);
	progress_bar_->setValue(0);
	context_layout->addWidget(progress_bar_, 5, 1, 1, 2);
	elapsed_value_ = make_secondary_label(QStringLiteral("--:--"), this);
	remaining_value_ = make_secondary_label(QStringLiteral("--:--"), this);
	context_layout->addWidget(elapsed_value_, 6, 1);
	context_layout->addWidget(remaining_value_, 6, 2);
	context_layout->addWidget(make_separator(this), 8, 0, 1, 3);

	context_layout->addWidget(make_section_icon(QStyle::SP_MediaSkipForward, this), 9, 0);
	context_layout->addWidget(make_section_label(tr("Up Next"), this), 9, 1);
	next_duration_value_ = make_secondary_label(QString(), this);
	context_layout->addWidget(next_duration_value_, 9, 2);
	next_value_ = new ElidedFileLabel(this);
	context_layout->addWidget(next_value_, 10, 1, 1, 2);
	layout->addLayout(context_layout);
	layout->addStretch();
	playlist_remaining_row_ = new QWidget(this);
	playlist_remaining_row_->setObjectName(QStringLiteral("mpsPlaylistRemainingRow"));
	auto *remaining_layout = new QHBoxLayout(playlist_remaining_row_);
	remaining_layout->setContentsMargins(0, 0, 0, 0);
	remaining_layout->addWidget(make_section_label(tr("Playlist remaining"), this));
	remaining_layout->addStretch();
	playlist_remaining_value_ = make_secondary_label(QStringLiteral("--:--"), this);
	remaining_layout->addWidget(playlist_remaining_value_);
	layout->addWidget(playlist_remaining_row_);

	follow_program_action_ = new QAction(tr("Follow Program MPS"), this);
	follow_program_action_->setCheckable(true);
	configure_follow_context_menu(this, source_header, follow_program_action_);
	add_playback_context_actions(
		this, source_header,
		[this] { return command_source(*program_, follow_program_, selected_source_uuid_); }, &shuffle_action_,
		&loop_action_);
	connect(follow_program_action_, &QAction::toggled, this, [this](bool enabled) {
		follow_program_ = enabled;
		refresh_sources();
	});

	progress_timer_ = new QTimer(this);
	progress_timer_->setInterval(kTimerIntervalMs);
	connect(progress_timer_, &QTimer::timeout, this, &PlaylistQueueDock::refresh_progress);
	connect(source_selector_, qOverload<int>(&QComboBox::activated), this, [this](int index) {
		const QString uuid = source_selector_->itemData(index).toString();
		if (follow_program_ && uuid == selected_source_uuid_)
			return;
		manual_source_uuid_ = uuid;
		if (follow_program_)
			follow_program_action_->setChecked(false);
		else
			refresh_sources();
	});

	connect(program_.get(), &PlaylistProgramTracker::sources_changed, this, &PlaylistQueueDock::refresh_sources);
	mps_playlist_change_add_listener(playback_changed, this);
	refresh_sources();
}

PlaylistQueueDock::~PlaylistQueueDock()
{
	if (progress_timer_)
		progress_timer_->stop();
	mps_playlist_change_remove_listener(playback_changed, this);
}

void PlaylistQueueDock::playback_changed(void *data, const char *source_uuid)
{
	static_cast<PlaylistQueueDock *>(data)->schedule_playback_refresh(source_uuid);
}

void PlaylistQueueDock::schedule_playback_refresh(const char *source_uuid)
{
	if (!source_uuid || !*source_uuid)
		return;

	bool queue_refresh = false;
	{
		QMutexLocker lock(&playback_refresh_mutex_);
		pending_playback_uuids_.insert(QString::fromUtf8(source_uuid));
		if (!playback_refresh_queued_) {
			playback_refresh_queued_ = true;
			queue_refresh = true;
		}
	}
	if (!queue_refresh)
		return;

	QMetaObject::invokeMethod(
		this,
		[this] {
			QSet<QString> changed_uuids;
			{
				QMutexLocker lock(&playback_refresh_mutex_);
				changed_uuids.swap(pending_playback_uuids_);
				playback_refresh_queued_ = false;
			}
			if (changed_uuids.contains(selected_source_uuid_))
				refresh_snapshot();
		},
		Qt::QueuedConnection);
}

void PlaylistQueueDock::refresh_sources()
{
	const QString target = populate_source_selector(source_selector_, follow_status_, *program_, follow_program_,
							manual_source_uuid_);
	if (selected_source_uuid_ != target)
		duration_cache_.clear();
	selected_source_uuid_ = target;
	refresh_snapshot();
}

void PlaylistQueueDock::clear_snapshot()
{
	update_playback_context_actions(shuffle_action_, loop_action_, false, false, false);
	playlist_remaining_row_->setVisible(true);
	previous_value_->setFilePath(nullptr);
	next_value_->setFilePath(nullptr);
	previous_duration_value_->setText(QStringLiteral("--:--"));
	next_duration_value_->clear();
	context_available_ = false;
	update_timing(nullptr);
}

void PlaylistQueueDock::cache_duration(const char *path, int64_t duration_ms)
{
	if (!path || !*path || duration_ms <= 0 || selected_source_uuid_.isEmpty())
		return;
	duration_cache_.insert(QString::fromUtf8(path), duration_ms);
}

int64_t PlaylistQueueDock::cached_duration(const char *path) const
{
	if (!path || !*path || selected_source_uuid_.isEmpty())
		return -1;
	return duration_cache_.value(QString::fromUtf8(path), -1);
}

void PlaylistQueueDock::refresh_snapshot()
{
	auto source = dock_source(selected_source_uuid_);
	if (!source) {
		clear_snapshot();
		return;
	}

	struct mps_playlist_telemetry telemetry;
	const bool found = mps_playlist_telemetry_get(source, &telemetry);
	refresh_context(source, found ? &telemetry : nullptr);
	mps_playlist_telemetry_free(&telemetry);
}

void PlaylistQueueDock::refresh_context(obs_source_t *source, const struct mps_playlist_telemetry *telemetry)
{
	struct mps_playlist_context_snapshot snapshot {};
	context_available_ = mps_playlist_context_snapshot_get(source, &snapshot);
	if (!context_available_) {
		clear_snapshot();
		return;
	}

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	const bool looping = telemetry ? telemetry->loop : obs_data_get_bool(settings, "loop");
	update_playback_context_actions(shuffle_action_, loop_action_, true, snapshot.shuffle, looping);
	playlist_remaining_row_->setVisible(!looping);
	previous_value_->setFilePath(snapshot.previous);
	next_value_->setFilePath(snapshot.next);
	int64_t standby_duration_ms = 0;
	if (snapshot.next && mps_playlist_standby_duration_get(source, snapshot.next, &standby_duration_ms))
		cache_duration(snapshot.next, standby_duration_ms);
	previous_duration_value_->setText(format_media_duration(cached_duration(snapshot.previous)));
	const int64_t next_duration = cached_duration(snapshot.next);
	next_duration_value_->setText(next_duration > 0 ? format_media_duration(next_duration) : QString());
	update_timing(telemetry);
	mps_playlist_context_snapshot_free(&snapshot);
}

void PlaylistQueueDock::refresh_progress()
{
	if (!isVisible())
		return;
	auto source = dock_source(selected_source_uuid_);
	if (!source) {
		clear_snapshot();
		return;
	}

	struct mps_playlist_telemetry telemetry;
	const bool found = mps_playlist_telemetry_get(source, &telemetry);
	if (!found || !context_available_) {
		/* A racing A/B cut can invalidate the clock without invalidating the
		 * playlist context. Re-read the context and hide unverified timing. */
		refresh_context(source, found ? &telemetry : nullptr);
	} else {
		update_playback_context_actions(shuffle_action_, loop_action_, true, telemetry.shuffle, telemetry.loop);
		playlist_remaining_row_->setVisible(!telemetry.loop);
		update_timing(&telemetry);
	}
	mps_playlist_telemetry_free(&telemetry);
}

void PlaylistQueueDock::update_timing(const struct mps_playlist_telemetry *telemetry)
{
	has_current_ = telemetry && telemetry->current_path && telemetry->state != OBS_MEDIA_STATE_NONE &&
		       telemetry->state != OBS_MEDIA_STATE_STOPPED && telemetry->state != OBS_MEDIA_STATE_ENDED;
	current_value_->setFilePath(has_current_ ? telemetry->current_path : nullptr);
	if (!has_current_) {
		update_progress(-1, -1, -1, -1);
		return;
	}
	cache_duration(telemetry->current_path, telemetry->duration_media_ms);
	update_progress(telemetry->elapsed_media_ms, telemetry->duration_media_ms, telemetry->remaining_ms,
			telemetry->playlist_remaining_known ? telemetry->playlist_remaining_ms : -1);
}

void PlaylistQueueDock::update_progress(int64_t time_ms, int64_t duration_ms, int64_t remaining_ms,
					int64_t playlist_remaining_ms)
{
	playlist_remaining_value_->setText(format_media_time(playlist_remaining_ms));
	remaining_value_->setText(remaining_ms >= 0 ? QStringLiteral("-") + format_media_time(remaining_ms)
						    : QStringLiteral("--:--"));
	elapsed_value_->setText(format_media_time(time_ms));
	current_duration_value_->setText(format_media_duration(duration_ms));
	if (!has_current_ || duration_ms <= 0 || time_ms < 0) {
		progress_bar_->setRange(0, 1);
		progress_bar_->setValue(0);
		return;
	}

	if (time_ms > duration_ms)
		time_ms = duration_ms;
	const int value = static_cast<int>((static_cast<double>(time_ms) * 1000.0) / duration_ms);
	progress_bar_->setRange(0, 1000);
	progress_bar_->setValue(value);
}

void PlaylistQueueDock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	progress_timer_->start();
	refresh_progress();
}

void PlaylistQueueDock::hideEvent(QHideEvent *event)
{
	progress_timer_->stop();
	QWidget::hideEvent(event);
}

PlaylistControlDock::PlaylistControlDock(std::shared_ptr<PlaylistProgramTracker> program, QWidget *parent)
	: QWidget(parent),
	  program_(std::move(program))
{
	setWindowTitle(tr("Playlist Control"));

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);

	auto *source_header = new QWidget(this);
	auto *source_layout = new QGridLayout(source_header);
	source_layout->setContentsMargins(0, 0, 0, 0);
	source_layout->setHorizontalSpacing(8);
	source_layout->setVerticalSpacing(4);
	source_layout->addWidget(make_section_label(tr("Source"), source_header), 0, 0);
	follow_status_ = make_secondary_label(QString(), source_header);
	follow_status_->setVisible(false);
	source_layout->addWidget(follow_status_, 0, 1);
	source_selector_ = new QComboBox(source_header);
	source_selector_->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
	source_selector_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	source_layout->addWidget(source_selector_, 1, 0, 1, 2);
	source_layout->setColumnStretch(0, 1);
	layout->addWidget(source_header);
	layout->addWidget(make_separator(this));

	playback_controls_ = new QWidget(this);
	auto *controls = new QHBoxLayout(playback_controls_);
	controls->setContentsMargins(0, 0, 0, 0);
	controls->setSpacing(6);
	const auto add_control = [this, controls](const QString &text, QStyle::StandardPixmap icon,
						  PlaybackAction action) {
		auto *button = new QPushButton(text, playback_controls_);
		button->setIcon(palette_icon(this, icon, QPalette::ButtonText));
		button->setToolTip(text);
		button->setAccessibleName(text);
		controls->addWidget(button);
		connect(button, &QPushButton::clicked, this, [this, action] { control_playback(action); });
		return button;
	};
	add_control(tr("Previous"), QStyle::SP_MediaSkipBackward, PlaybackAction::Previous);
	pause_button_ = add_control(tr("Play"), QStyle::SP_MediaPlay, PlaybackAction::PlayPause);
	add_control(tr("Stop"), QStyle::SP_MediaStop, PlaybackAction::Stop);
	add_control(tr("Next"), QStyle::SP_MediaSkipForward, PlaybackAction::Next);
	layout->addWidget(playback_controls_);
	auto *playlist_header = new QGridLayout;
	playlist_header->setContentsMargins(0, 0, 0, 0);
	playlist_header->setColumnStretch(0, 1);
	playlist_header->addWidget(make_section_label(tr("Playlist"), this), 0, 0);
	shuffle_info_ = make_secondary_label(tr("Shuffle ON"), this);
	shuffle_info_->setVisible(false);
	playlist_header->addWidget(shuffle_info_, 0, 1);
	layout->addLayout(playlist_header);
	layout->addWidget(make_separator(this));

	playlist_ = new QTreeWidget(this);
	playlist_->setColumnCount(3);
	playlist_->setHeaderLabels({QString(), tr("#"), tr("File")});
	playlist_->setHeaderHidden(false);
	playlist_->setRootIsDecorated(true);
	playlist_->setSelectionMode(QAbstractItemView::SingleSelection);
	playlist_->setTextElideMode(Qt::ElideMiddle);
	playlist_->setUniformRowHeights(true);
	playlist_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	playlist_->setIndentation(16);
	playlist_->headerItem()->setTextAlignment(0, Qt::AlignCenter);
	playlist_->headerItem()->setTextAlignment(1, Qt::AlignCenter);
	playlist_->headerItem()->setTextAlignment(2, Qt::AlignLeft | Qt::AlignVCenter);
	playlist_->header()->setMinimumHeight(playlist_->header()->fontMetrics().height() + 10);
	playlist_->header()->setStretchLastSection(false);
	playlist_->header()->setSectionResizeMode(0, QHeaderView::Fixed);
	playlist_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
	playlist_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
	playlist_->setColumnWidth(0, 24);
	playlist_->setColumnWidth(1, 44);
	layout->addWidget(playlist_, 1);

	layout->addWidget(make_section_label(tr("Selected"), this, false));
	selected_value_ = new ElidedFileLabel(this);
	layout->addWidget(selected_value_);
	play_button_ = new QPushButton(tr("Play selected file"), this);
	play_button_->setIcon(palette_icon(this, QStyle::SP_MediaPlay, QPalette::ButtonText));
	play_button_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	play_button_->setMinimumHeight(play_button_->sizeHint().height() + 6);
	play_button_->setEnabled(false);
	layout->addWidget(play_button_);

	follow_program_action_ = new QAction(tr("Follow Program MPS"), this);
	follow_program_action_->setCheckable(true);
	configure_follow_context_menu(this, source_header, follow_program_action_);
	add_playback_context_actions(
		this, source_header,
		[this] { return command_source(*program_, follow_program_, selected_source_uuid_); }, &shuffle_action_,
		&loop_action_);
	auto *show_controls = new QAction(tr("Show playback toolbar"), this);
	show_controls->setObjectName(QStringLiteral("mpsShowPlaybackControls"));
	show_controls->setCheckable(true);
	show_controls->setChecked(true);
	playback_controls_->setObjectName(QStringLiteral("mpsPlaybackControls"));
	connect(show_controls, &QAction::toggled, playback_controls_, &QWidget::setVisible);
	addAction(show_controls);
	source_header->addAction(show_controls);
	playlist_->setContextMenuPolicy(Qt::ActionsContextMenu);
	playlist_->addAction(follow_program_action_);
	playlist_->addAction(show_controls);
	playlist_->addAction(shuffle_action_);
	playlist_->addAction(loop_action_);
	connect(follow_program_action_, &QAction::toggled, this, [this](bool enabled) {
		follow_program_ = enabled;
		refresh_sources();
	});

	connect(source_selector_, qOverload<int>(&QComboBox::activated), this, [this](int index) {
		const QString uuid = source_selector_->itemData(index).toString();
		if (follow_program_ && uuid == selected_source_uuid_)
			return;
		manual_source_uuid_ = uuid;
		if (follow_program_)
			follow_program_action_->setChecked(false);
		else
			refresh_sources();
	});
	connect(playlist_, &QTreeWidget::itemSelectionChanged, this, [this] {
		QTreeWidgetItem *item = playlist_->currentItem();
		selection_valid_ = item && item->data(0, kMediaIndexRole).isValid();
		selected_stable_id_.clear();
		if (selection_valid_)
			set_selected_item_data(item, &selected_media_index_, &selected_folder_item_index_,
					       &selected_stable_id_);
		update_play_button();
	});
	play_button_->setToolTip(tr("Play the selected file. Double-click a file or press Enter to start it."));
	connect(play_button_, &QPushButton::clicked, this, &PlaylistControlDock::play_selected);
	connect(playlist_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *, int) { play_selected(); });

	connect(program_.get(), &PlaylistProgramTracker::sources_changed, this, &PlaylistControlDock::refresh_sources);
	mps_playlist_change_add_listener(playback_changed, this);
	refresh_sources();
}

PlaylistControlDock::~PlaylistControlDock()
{
	mps_playlist_change_remove_listener(playback_changed, this);
}

void PlaylistControlDock::playback_changed(void *data, const char *source_uuid)
{
	static_cast<PlaylistControlDock *>(data)->schedule_playback_refresh(source_uuid);
}

void PlaylistControlDock::schedule_playback_refresh(const char *source_uuid)
{
	if (!source_uuid || !*source_uuid)
		return;

	bool queue_refresh = false;
	{
		QMutexLocker lock(&playback_refresh_mutex_);
		pending_playback_uuids_.insert(QString::fromUtf8(source_uuid));
		if (!playback_refresh_queued_) {
			playback_refresh_queued_ = true;
			queue_refresh = true;
		}
	}
	if (!queue_refresh)
		return;

	QMetaObject::invokeMethod(
		this,
		[this] {
			QSet<QString> changed_uuids;
			{
				QMutexLocker lock(&playback_refresh_mutex_);
				changed_uuids.swap(pending_playback_uuids_);
				playback_refresh_queued_ = false;
			}
			if (changed_uuids.contains(selected_source_uuid_))
				refresh_playlist();
		},
		Qt::QueuedConnection);
}

void PlaylistControlDock::refresh_sources()
{
	const QString target = populate_source_selector(source_selector_, follow_status_, *program_, follow_program_,
							manual_source_uuid_);
	if (selected_source_uuid_ != target) {
		selection_valid_ = false;
		selected_stable_id_.clear();
	}
	selected_source_uuid_ = target;
	refresh_playlist();
}

void PlaylistControlDock::refresh_playlist()
{
	QString previous_stable_id;
	const bool restore_selection = selection_valid_ && !selected_stable_id_.isEmpty();
	const int scroll_value = playlist_->verticalScrollBar()->value();
	if (restore_selection)
		set_selected_item_data(playlist_->currentItem(), &selected_media_index_, &selected_folder_item_index_,
				       &previous_stable_id);

	const QSignalBlocker blocker(playlist_);
	playlist_->clear();
	selection_valid_ = false;
	selected_stable_id_.clear();
	shuffle_info_->setVisible(false);
	update_playback_context_actions(shuffle_action_, loop_action_, false, false, false);
	auto source = dock_source(selected_source_uuid_);
	struct mps_playlist_entries_snapshot snapshot {};
	playlist_available_ = source && mps_playlist_entries_snapshot_get(source, &snapshot);
	const bool playing = playlist_available_ && obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING;
	pause_button_->setText(playing ? tr("Pause") : tr("Play"));
	pause_button_->setIcon(
		palette_icon(this, playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay, QPalette::ButtonText));
	pause_button_->setAccessibleName(pause_button_->text());
	pause_button_->setToolTip(pause_button_->text());
	if (!playlist_available_) {
		mps_playlist_entries_snapshot_free(&snapshot);
		update_play_button();
		return;
	}

	playlist_revision_ = snapshot.playlist_revision;
	update_playback_context_actions(shuffle_action_, loop_action_, true, snapshot.shuffle, snapshot.loop);
	shuffle_info_->setText(tr("%1 entries%2")
				       .arg(snapshot.total_count)
				       .arg(snapshot.shuffle ? tr(" · shuffled playback") : QString()));
	shuffle_info_->setVisible(true);
	QTreeWidgetItem *folder_parent = nullptr;
	QTreeWidgetItem *restored_item = nullptr;
	for (std::size_t i = 0; i < snapshot.item_count; i++) {
		const struct mps_playlist_item_snapshot &entry = snapshot.items[i];
		QTreeWidgetItem *item;
		if (entry.is_folder) {
			item = new QTreeWidgetItem(playlist_);
			folder_parent = item;
			item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
			item->setExpanded(true);
		} else if (entry.is_folder_child && folder_parent) {
			item = new QTreeWidgetItem(folder_parent);
		} else {
			folder_parent = nullptr;
			item = new QTreeWidgetItem(playlist_);
		}

		item->setText(0, entry.is_current ? QStringLiteral("▶") : QString());
		const QString number =
			entry.is_folder_child
				? QStringLiteral("%1.%2").arg(entry.media_index + 1).arg(entry.folder_item_index + 1)
				: QString::number(entry.media_index + 1);
		item->setText(1, number);
		item->setText(2, filename_from_entry(entry));
		item->setToolTip(2, QString::fromUtf8(entry.path ? entry.path : ""));
		if (entry.is_folder)
			item->setIcon(2, palette_icon(this, QStyle::SP_DirIcon, QPalette::Text));
		item->setTextAlignment(0, Qt::AlignCenter);
		item->setTextAlignment(1, Qt::AlignCenter);
		item->setTextAlignment(2, Qt::AlignLeft | Qt::AlignVCenter);
		item->setData(0, kPathRole, QString::fromUtf8(entry.path ? entry.path : ""));
		if (entry.stable_id)
			item->setData(0, kStableIdRole, QString::fromUtf8(entry.stable_id));
		if (!entry.is_folder) {
			item->setData(0, kMediaIndexRole, QVariant::fromValue<qulonglong>(entry.media_index));
			item->setData(0, kFolderItemIndexRole,
				      QVariant::fromValue<qulonglong>(entry.folder_item_index));
		}
		if (entry.is_current || entry.is_folder) {
			QFont font = item->font(2);
			font.setBold(true);
			item->setFont(2, font);
		}

		if (restore_selection && !entry.is_folder && entry.stable_id &&
		    QString::fromUtf8(entry.stable_id) == previous_stable_id)
			restored_item = item;
	}

	if (restored_item) {
		playlist_->setCurrentItem(restored_item);
		selection_valid_ = true;
		set_selected_item_data(restored_item, &selected_media_index_, &selected_folder_item_index_,
				       &selected_stable_id_);
	}
	playlist_->verticalScrollBar()->setValue(scroll_value);
	mps_playlist_entries_snapshot_free(&snapshot);
	update_play_button();
}

void PlaylistControlDock::update_play_button()
{
	playback_controls_->setEnabled(playlist_available_);
	play_button_->setEnabled(selection_valid_ && playlist_available_);
	QTreeWidgetItem *item = selection_valid_ ? playlist_->currentItem() : nullptr;
	const QByteArray selected_path = item ? item->data(0, kPathRole).toString().toUtf8() : QByteArray();
	selected_value_->setFilePath(selected_path.isEmpty() ? nullptr : selected_path.constData());
}

void PlaylistControlDock::play_selected()
{
	if (!selection_valid_ || selected_stable_id_.isEmpty())
		return;
	auto source = command_source(*program_, follow_program_, selected_source_uuid_);
	struct obs_video_info video_info;
	if (!source || !obs_get_video_info(&video_info))
		return;

	struct Selection {
		OBSWeakSource source;
		QByteArray id;
		std::uint64_t revision;
		bool follow;
	};
	auto *selection =
		new Selection{OBSGetWeakRef(source), selected_stable_id_.toUtf8(), playlist_revision_, follow_program_};
	obs_queue_task(
		OBS_TASK_GRAPHICS,
		[](void *data) {
			const std::unique_ptr<Selection> selection(static_cast<Selection *>(data));
			auto source = OBSGetStrongRef(selection->source);
			if (!source || obs_source_removed(source))
				return;
			if (selection->follow) {
				OBSSourceAutoRelease output = obs_get_output_source(0);
				const auto graph = mps_program_sources(output);
				if (graph.playlists.size() != 1 || graph.playlists.front() != source)
					return;
			}
			mps_playlist_select_item(source, selection->id.constData(), selection->revision);
		},
		selection, false);
}

void PlaylistControlDock::control_playback(PlaybackAction action)
{
	auto source = command_source(*program_, follow_program_, selected_source_uuid_);
	if (!source)
		return;
	switch (action) {
	case PlaybackAction::Previous:
		obs_source_media_previous(source);
		break;
	case PlaybackAction::Next:
		obs_source_media_next(source);
		break;
	case PlaybackAction::Stop:
		obs_source_media_stop(source);
		break;
	case PlaybackAction::PlayPause: {
		const auto state = obs_source_media_get_state(source);
		if (state == OBS_MEDIA_STATE_STOPPED || state == OBS_MEDIA_STATE_ENDED)
			obs_source_media_restart(source);
		else
			obs_source_media_play_pause(source, state == OBS_MEDIA_STATE_PLAYING);
		break;
	}
	}
}

extern "C" void mps_playlist_dock_register(void)
{
	if (queue_dock_instance || control_dock_instance)
		return;

	const auto program = std::make_shared<PlaylistProgramTracker>();
	auto *queue = new PlaylistQueueDock(program);
	if (!obs_frontend_add_dock_by_id(kQueueDockId, "Playlist Queue", queue)) {
		delete queue;
		return;
	}
	queue_dock_instance = queue;

	auto *control = new PlaylistControlDock(program);
	if (!obs_frontend_add_dock_by_id(kControlDockId, "Playlist Control", control)) {
		obs_frontend_remove_dock(kQueueDockId);
		queue_dock_instance = nullptr;
		delete control;
		return;
	}
	control_dock_instance = control;
}

extern "C" void mps_playlist_dock_unregister(void)
{
	if (control_dock_instance) {
		obs_frontend_remove_dock(kControlDockId);
		control_dock_instance = nullptr;
	}
	if (queue_dock_instance) {
		obs_frontend_remove_dock(kQueueDockId);
		queue_dock_instance = nullptr;
	}
}
