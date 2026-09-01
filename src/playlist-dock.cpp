#include "playlist-dock.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <QAbstractItemView>
#include <QAction>
#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QMetaObject>
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

constexpr const char *kSourceId = "media_playlist_source_codeyan";
constexpr const char *kQueueDockId = "media-playlist-queue-dock";
constexpr const char *kControlDockId = "media-playlist-control-dock";
constexpr int kTimerIntervalMs = 250;
constexpr int kMediaIndexRole = Qt::UserRole;
constexpr int kFolderItemIndexRole = Qt::UserRole + 1;
constexpr int kStableIdRole = Qt::UserRole + 2;
constexpr int kPathRole = Qt::UserRole + 3;
constexpr std::size_t kMaxProgramSceneDepth = 64;

struct SourceInfo {
	QString uuid;
	QString name;
};

QString filename_from_entry(const struct mps_playlist_item_snapshot &entry)
{
	if (entry.filename && *entry.filename)
		return QString::fromUtf8(entry.filename);
	return filename_from_path(QString::fromUtf8(entry.path ? entry.path : ""));
}

bool enum_mps_source(void *data, obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	if (!id || std::strcmp(id, kSourceId) != 0)
		return true;

	const char *uuid = obs_source_get_uuid(source);
	if (!uuid || !*uuid)
		return true;

	auto *sources = static_cast<QVector<SourceInfo> *>(data);
	const char *name = obs_source_get_name(source);
	sources->append({QString::fromUtf8(uuid), QString::fromUtf8(name ? name : "")});
	return true;
}

QVector<SourceInfo> enumerate_mps_sources()
{
	QVector<SourceInfo> sources;
	obs_enum_sources(enum_mps_source, &sources);
	return sources;
}

struct ProgramSourceTraversal {
	QSet<QString> source_uuids;
	QSet<QString> scene_uuids;
	QSet<const obs_source_t *> visited_scenes;
};

struct ProgramSceneEnumeration {
	ProgramSourceTraversal *traversal;
	std::size_t depth;
};

void enumerate_program_scene(obs_scene_t *scene, ProgramSourceTraversal *traversal, std::size_t depth);

bool enum_program_scene_item(obs_scene_t *, obs_sceneitem_t *item, void *data)
{
	auto *enumeration = static_cast<ProgramSceneEnumeration *>(data);
	if (!obs_sceneitem_visible(item))
		return true;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;
	const char *id = obs_source_get_unversioned_id(source);
	if (id && std::strcmp(id, kSourceId) == 0) {
		const char *uuid = obs_source_get_uuid(source);
		if (uuid && *uuid)
			enumeration->traversal->source_uuids.insert(QString::fromUtf8(uuid));
	}

	obs_scene_t *nested_scene = obs_sceneitem_is_group(item) ? obs_sceneitem_group_get_scene(item)
								 : obs_scene_from_source(source);
	if (nested_scene)
		enumerate_program_scene(nested_scene, enumeration->traversal, enumeration->depth + 1);
	return true;
}

void enumerate_program_scene(obs_scene_t *scene, ProgramSourceTraversal *traversal, std::size_t depth)
{
	if (!scene || !traversal || depth > kMaxProgramSceneDepth)
		return;
	obs_source_t *scene_source = obs_scene_get_source(scene);
	if (!scene_source || traversal->visited_scenes.contains(scene_source))
		return;
	traversal->visited_scenes.insert(scene_source);
	const char *scene_uuid = obs_source_get_uuid(scene_source);
	if (scene_uuid && *scene_uuid)
		traversal->scene_uuids.insert(QString::fromUtf8(scene_uuid));
	ProgramSceneEnumeration enumeration = {traversal, depth};
	obs_scene_enum_items(scene, enum_program_scene_item, &enumeration);
}

ProgramSourceTraversal enumerate_program_sources()
{
	ProgramSourceTraversal traversal;
	obs_source_t *program_source = obs_frontend_get_current_scene();
	if (!program_source)
		return traversal;

	obs_scene_t *program_scene = obs_scene_from_source(program_source);
	if (program_scene)
		enumerate_program_scene(program_scene, &traversal, 0);
	obs_source_release(program_source);
	return traversal;
}

void set_program_scene_event_connections(QSet<QString> &connected, const QSet<QString> &desired,
					 signal_callback_t callback, void *data)
{
	static const char *event_names[] = {"item_add", "item_remove", "item_visible"};
	for (const QString &uuid : connected - desired) {
		const QByteArray utf8 = uuid.toUtf8();
		obs_source_t *source = obs_get_source_by_uuid(utf8.constData());
		if (!source)
			continue;
		signal_handler_t *handler = obs_source_get_signal_handler(source);
		for (const char *event_name : event_names)
			signal_handler_disconnect(handler, event_name, callback, data);
		obs_source_release(source);
	}
	for (const QString &uuid : desired - connected) {
		const QByteArray utf8 = uuid.toUtf8();
		obs_source_t *source = obs_get_source_by_uuid(utf8.constData());
		if (!source)
			continue;
		signal_handler_t *handler = obs_source_get_signal_handler(source);
		for (const char *event_name : event_names)
			signal_handler_connect(handler, event_name, callback, data);
		obs_source_release(source);
	}
	connected = desired;
}

void connect_obs_events(signal_callback_t callback, void *data)
{
	signal_handler_t *handler = obs_get_signal_handler();
	if (!handler)
		return;
	signal_handler_connect(handler, "source_create", callback, data);
	signal_handler_connect(handler, "source_remove", callback, data);
	signal_handler_connect(handler, "source_destroy", callback, data);
	signal_handler_connect(handler, "source_rename", callback, data);
}

void disconnect_obs_events(signal_callback_t callback, void *data)
{
	signal_handler_t *handler = obs_get_signal_handler();
	if (!handler)
		return;
	signal_handler_disconnect(handler, "source_create", callback, data);
	signal_handler_disconnect(handler, "source_remove", callback, data);
	signal_handler_disconnect(handler, "source_destroy", callback, data);
	signal_handler_disconnect(handler, "source_rename", callback, data);
}

bool needs_source_refresh(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		return true;
	default:
		return false;
	}
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
	label->setForegroundRole(QPalette::PlaceholderText);
	return label;
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

PlaylistQueueDock::PlaylistQueueDock(QWidget *parent) : QWidget(parent)
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
	context_layout->setColumnStretch(0, 1);
	context_layout->addWidget(make_section_label(tr("Previous"), this, false), 0, 0);
	previous_duration_value_ = make_secondary_label(QStringLiteral("--:--"), this);
	context_layout->addWidget(previous_duration_value_, 0, 1);
	previous_value_ = new ElidedFileLabel(this);
	context_layout->addWidget(previous_value_, 1, 0, 1, 2);
	context_layout->addWidget(make_separator(this), 2, 0, 1, 2);

	context_layout->addWidget(make_section_label(tr("Now Playing"), this), 3, 0);
	current_duration_value_ = make_secondary_label(QStringLiteral("--:--"), this);
	context_layout->addWidget(current_duration_value_, 3, 1);
	current_value_ = new ElidedFileLabel(this);
	QFont current_font = current_value_->font();
	current_font.setBold(true);
	current_value_->setFont(current_font);
	context_layout->addWidget(current_value_, 4, 0, 1, 2);
	progress_bar_ = new QProgressBar(this);
	progress_bar_->setTextVisible(false);
	progress_bar_->setRange(0, 1);
	progress_bar_->setValue(0);
	context_layout->addWidget(progress_bar_, 5, 0, 1, 2);
	elapsed_value_ = new QLabel(QStringLiteral("--:--"), this);
	elapsed_value_->setForegroundRole(QPalette::PlaceholderText);
	remaining_value_ = make_secondary_label(QStringLiteral("--:--"), this);
	context_layout->addWidget(elapsed_value_, 6, 0);
	context_layout->addWidget(remaining_value_, 6, 1);
	context_layout->addWidget(make_separator(this), 7, 0, 1, 2);

	context_layout->addWidget(make_section_label(tr("Up Next"), this, false), 8, 0);
	next_duration_value_ = make_secondary_label(QStringLiteral("--:--"), this);
	context_layout->addWidget(next_duration_value_, 8, 1);
	next_value_ = new ElidedFileLabel(this);
	context_layout->addWidget(next_value_, 9, 0, 1, 2);
	layout->addLayout(context_layout);
	layout->addStretch();

	follow_program_action_ = new QAction(tr("Follow Program MPS"), this);
	follow_program_action_->setCheckable(true);
	configure_follow_context_menu(this, source_header, follow_program_action_);
	connect(follow_program_action_, &QAction::toggled, this, [this](bool enabled) {
		follow_program_ = enabled;
		follow_status_->setVisible(enabled);
		if (enabled) {
			if (apply_program_follow())
				refresh_snapshot();
		} else {
			set_program_scene_event_connections(program_scene_uuids_, {}, program_scene_event, this);
		}
	});

	progress_timer_ = new QTimer(this);
	progress_timer_->setInterval(kTimerIntervalMs);
	connect(progress_timer_, &QTimer::timeout, this, &PlaylistQueueDock::refresh_progress);
	connect(source_selector_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
		selected_source_uuid_ = index >= 0 ? source_selector_->itemData(index).toString() : QString();
		refresh_snapshot();
	});

	connect_obs_events(source_event, this);
	obs_frontend_add_event_callback(frontend_event, this);
	mps_playlist_change_add_listener(playback_changed, this);
	refresh_sources();
}

PlaylistQueueDock::~PlaylistQueueDock()
{
	if (progress_timer_)
		progress_timer_->stop();
	set_program_scene_event_connections(program_scene_uuids_, {}, program_scene_event, this);
	mps_playlist_change_remove_listener(playback_changed, this);
	obs_frontend_remove_event_callback(frontend_event, this);
	disconnect_obs_events(source_event, this);
}

void PlaylistQueueDock::playback_changed(void *data, const char *source_uuid)
{
	static_cast<PlaylistQueueDock *>(data)->schedule_playback_refresh(source_uuid);
}

void PlaylistQueueDock::source_event(void *data, calldata_t *calldata)
{
	(void)calldata;
	static_cast<PlaylistQueueDock *>(data)->schedule_refresh();
}

void PlaylistQueueDock::program_scene_event(void *data, calldata_t *calldata)
{
	(void)calldata;
	static_cast<PlaylistQueueDock *>(data)->schedule_program_refresh();
}

void PlaylistQueueDock::frontend_event(enum obs_frontend_event event, void *data)
{
	auto *dock = static_cast<PlaylistQueueDock *>(data);
	if (needs_source_refresh(event))
		dock->schedule_refresh();
	else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED)
		dock->schedule_program_refresh();
}

void PlaylistQueueDock::schedule_refresh()
{
	if (refresh_queued_.exchange(true))
		return;
	QMetaObject::invokeMethod(
		this,
		[this] {
			refresh_queued_.store(false);
			refresh_sources();
		},
		Qt::QueuedConnection);
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

void PlaylistQueueDock::schedule_program_refresh()
{
	if (!follow_program_ || program_refresh_queued_.exchange(true))
		return;
	QMetaObject::invokeMethod(
		this,
		[this] {
			program_refresh_queued_.store(false);
			if (apply_program_follow())
				refresh_snapshot();
		},
		Qt::QueuedConnection);
}

bool PlaylistQueueDock::apply_program_follow()
{
	if (!follow_program_)
		return false;
	const ProgramSourceTraversal program = enumerate_program_sources();
	set_program_scene_event_connections(program_scene_uuids_, program.scene_uuids, program_scene_event, this);
	const QSet<QString> &active_sources = program.source_uuids;
	follow_status_->setText(follow_status_text(static_cast<std::size_t>(active_sources.size())));
	if (active_sources.size() != 1)
		return false;

	const QString source_uuid = *active_sources.cbegin();
	if (source_uuid == selected_source_uuid_)
		return false;
	const int index = source_selector_->findData(source_uuid);
	if (index < 0)
		return false;
	const QSignalBlocker blocker(source_selector_);
	source_selector_->setCurrentIndex(index);
	selected_source_uuid_ = source_uuid;
	return true;
}

void PlaylistQueueDock::refresh_sources()
{
	const QVector<SourceInfo> sources = enumerate_mps_sources();
	const QString previous_selection = selected_source_uuid_;
	int selected_index = -1;
	{
		const QSignalBlocker blocker(source_selector_);
		source_selector_->clear();
		for (const SourceInfo &source : sources) {
			source_selector_->addItem(source.name, source.uuid);
			if (source.uuid == previous_selection)
				selected_index = source_selector_->count() - 1;
		}
		if (selected_index < 0 && previous_selection.isEmpty() && source_selector_->count() > 0)
			selected_index = 0;
		source_selector_->setCurrentIndex(selected_index);
		if (selected_index >= 0)
			selected_source_uuid_ = source_selector_->itemData(selected_index).toString();
		else
			selected_source_uuid_.clear();
	}
	if (follow_program_)
		apply_program_follow();
	refresh_snapshot();
}

void PlaylistQueueDock::clear_snapshot()
{
	previous_value_->setFilePath(nullptr);
	current_value_->setFilePath(nullptr);
	next_value_->setFilePath(nullptr);
	previous_duration_value_->setText(QStringLiteral("--:--"));
	current_duration_value_->setText(QStringLiteral("--:--"));
	next_duration_value_->setText(QStringLiteral("--:--"));
	current_path_.clear();
	has_current_ = false;
	update_progress(-1, 0);
}

void PlaylistQueueDock::cache_duration(const char *path, int64_t duration_ms)
{
	if (!path || !*path || duration_ms <= 0 || selected_source_uuid_.isEmpty())
		return;
	duration_cache_.insert(selected_source_uuid_ + QLatin1Char('\n') + QString::fromUtf8(path), duration_ms);
}

int64_t PlaylistQueueDock::cached_duration(const char *path) const
{
	if (!path || !*path || selected_source_uuid_.isEmpty())
		return -1;
	return duration_cache_.value(selected_source_uuid_ + QLatin1Char('\n') + QString::fromUtf8(path), -1);
}

void PlaylistQueueDock::refresh_snapshot()
{
	if (selected_source_uuid_.isEmpty()) {
		clear_snapshot();
		return;
	}

	const QByteArray uuid = selected_source_uuid_.toUtf8();
	obs_source_t *source = obs_get_source_by_uuid(uuid.constData());
	if (!source) {
		clear_snapshot();
		return;
	}

	struct mps_playlist_context_snapshot snapshot = {0};
	const bool found = mps_playlist_context_snapshot_get(source, &snapshot);
	int64_t time_ms = 0;
	int64_t duration_ms = 0;
	int64_t standby_duration_ms = 0;
	const bool timing_found = found && mps_playlist_timing_get(source, &time_ms, &duration_ms);
	const bool standby_duration_found =
		found && snapshot.next &&
		mps_playlist_standby_duration_get(source, snapshot.next, &standby_duration_ms);
	obs_source_release(source);
	if (!found) {
		mps_playlist_context_snapshot_free(&snapshot);
		clear_snapshot();
		return;
	}

	previous_value_->setFilePath(snapshot.previous);
	current_value_->setFilePath(snapshot.current);
	next_value_->setFilePath(snapshot.next);
	if (timing_found)
		cache_duration(snapshot.current, duration_ms);
	if (standby_duration_found)
		cache_duration(snapshot.next, standby_duration_ms);
	previous_duration_value_->setText(format_media_duration(cached_duration(snapshot.previous)));
	next_duration_value_->setText(format_media_duration(cached_duration(snapshot.next)));
	current_path_ = QString::fromUtf8(snapshot.current ? snapshot.current : "");
	has_current_ = snapshot.current != nullptr;
	update_progress(timing_found ? time_ms : -1, timing_found ? duration_ms : 0);
	mps_playlist_context_snapshot_free(&snapshot);
}

void PlaylistQueueDock::refresh_progress()
{
	if (!isVisible() || selected_source_uuid_.isEmpty() || !has_current_)
		return;

	const QByteArray uuid = selected_source_uuid_.toUtf8();
	obs_source_t *source = obs_get_source_by_uuid(uuid.constData());
	if (!source) {
		clear_snapshot();
		return;
	}

	int64_t time_ms = 0;
	int64_t duration_ms = 0;
	const bool found = mps_playlist_timing_get(source, &time_ms, &duration_ms);
	obs_source_release(source);
	if (found) {
		const QByteArray current_path = current_path_.toUtf8();
		cache_duration(current_path.constData(), duration_ms);
		update_progress(time_ms, duration_ms);
	} else {
		clear_snapshot();
	}
}

void PlaylistQueueDock::update_progress(int64_t time_ms, int64_t duration_ms)
{
	if (!has_current_ || duration_ms <= 0) {
		progress_bar_->setRange(0, 1);
		progress_bar_->setValue(0);
		current_duration_value_->setText(QStringLiteral("--:--"));
		elapsed_value_->setText(QStringLiteral("--:--"));
		remaining_value_->setText(QStringLiteral("--:--"));
		return;
	}

	if (time_ms < 0)
		time_ms = 0;
	else if (time_ms > duration_ms)
		time_ms = duration_ms;
	const int value = static_cast<int>((static_cast<double>(time_ms) * 1000.0) / duration_ms);
	progress_bar_->setRange(0, 1000);
	progress_bar_->setValue(value);
	current_duration_value_->setText(format_media_duration(duration_ms));
	elapsed_value_->setText(format_media_time(time_ms));
	remaining_value_->setText(QStringLiteral("-") + format_media_time(duration_ms - time_ms));
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

PlaylistControlDock::PlaylistControlDock(QWidget *parent) : QWidget(parent)
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
	play_button_ = new QPushButton(tr("Play Selected"), this);
	play_button_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
	play_button_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	play_button_->setMinimumHeight(play_button_->sizeHint().height() + 6);
	play_button_->setEnabled(false);
	layout->addWidget(play_button_);

	follow_program_action_ = new QAction(tr("Follow Program MPS"), this);
	follow_program_action_->setCheckable(true);
	configure_follow_context_menu(this, source_header, follow_program_action_);
	connect(follow_program_action_, &QAction::toggled, this, [this](bool enabled) {
		follow_program_ = enabled;
		follow_status_->setVisible(enabled);
		if (enabled) {
			if (apply_program_follow())
				refresh_playlist();
		} else {
			set_program_scene_event_connections(program_scene_uuids_, {}, program_scene_event, this);
		}
	});

	connect(source_selector_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
		selected_source_uuid_ = index >= 0 ? source_selector_->itemData(index).toString() : QString();
		selection_valid_ = false;
		selected_stable_id_.clear();
		refresh_playlist();
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
	connect(play_button_, &QPushButton::clicked, this, &PlaylistControlDock::play_selected);

	connect_obs_events(source_event, this);
	obs_frontend_add_event_callback(frontend_event, this);
	mps_playlist_change_add_listener(playback_changed, this);
	refresh_sources();
}

PlaylistControlDock::~PlaylistControlDock()
{
	set_program_scene_event_connections(program_scene_uuids_, {}, program_scene_event, this);
	mps_playlist_change_remove_listener(playback_changed, this);
	obs_frontend_remove_event_callback(frontend_event, this);
	disconnect_obs_events(source_event, this);
}

void PlaylistControlDock::playback_changed(void *data, const char *source_uuid)
{
	static_cast<PlaylistControlDock *>(data)->schedule_playback_refresh(source_uuid);
}

void PlaylistControlDock::source_event(void *data, calldata_t *calldata)
{
	(void)calldata;
	static_cast<PlaylistControlDock *>(data)->schedule_refresh();
}

void PlaylistControlDock::program_scene_event(void *data, calldata_t *calldata)
{
	(void)calldata;
	static_cast<PlaylistControlDock *>(data)->schedule_program_refresh();
}

void PlaylistControlDock::frontend_event(enum obs_frontend_event event, void *data)
{
	auto *dock = static_cast<PlaylistControlDock *>(data);
	if (needs_source_refresh(event))
		dock->schedule_refresh();
	else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED)
		dock->schedule_program_refresh();
}

void PlaylistControlDock::schedule_refresh()
{
	if (refresh_queued_.exchange(true))
		return;
	QMetaObject::invokeMethod(
		this,
		[this] {
			refresh_queued_.store(false);
			refresh_sources();
		},
		Qt::QueuedConnection);
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

void PlaylistControlDock::schedule_program_refresh()
{
	if (!follow_program_ || program_refresh_queued_.exchange(true))
		return;
	QMetaObject::invokeMethod(
		this,
		[this] {
			program_refresh_queued_.store(false);
			if (apply_program_follow())
				refresh_playlist();
		},
		Qt::QueuedConnection);
}

bool PlaylistControlDock::apply_program_follow()
{
	if (!follow_program_)
		return false;
	const ProgramSourceTraversal program = enumerate_program_sources();
	set_program_scene_event_connections(program_scene_uuids_, program.scene_uuids, program_scene_event, this);
	const QSet<QString> &active_sources = program.source_uuids;
	follow_status_->setText(follow_status_text(static_cast<std::size_t>(active_sources.size())));
	if (active_sources.size() != 1)
		return false;

	const QString source_uuid = *active_sources.cbegin();
	if (source_uuid == selected_source_uuid_)
		return false;
	const int index = source_selector_->findData(source_uuid);
	if (index < 0)
		return false;
	const QSignalBlocker blocker(source_selector_);
	source_selector_->setCurrentIndex(index);
	selected_source_uuid_ = source_uuid;
	selection_valid_ = false;
	selected_stable_id_.clear();
	return true;
}

void PlaylistControlDock::refresh_sources()
{
	const QVector<SourceInfo> sources = enumerate_mps_sources();
	const QString previous_selection = selected_source_uuid_;
	int selected_index = -1;
	{
		const QSignalBlocker blocker(source_selector_);
		source_selector_->clear();
		for (const SourceInfo &source : sources) {
			source_selector_->addItem(source.name, source.uuid);
			if (source.uuid == previous_selection)
				selected_index = source_selector_->count() - 1;
		}
		if (selected_index < 0 && previous_selection.isEmpty() && source_selector_->count() > 0)
			selected_index = 0;
		source_selector_->setCurrentIndex(selected_index);
		if (selected_index >= 0)
			selected_source_uuid_ = source_selector_->itemData(selected_index).toString();
		else
			selected_source_uuid_.clear();
	}
	if (follow_program_)
		apply_program_follow();
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
	if (selected_source_uuid_.isEmpty()) {
		update_play_button();
		return;
	}

	const QByteArray uuid = selected_source_uuid_.toUtf8();
	obs_source_t *source = obs_get_source_by_uuid(uuid.constData());
	if (!source) {
		update_play_button();
		return;
	}

	struct mps_playlist_entries_snapshot snapshot = {0};
	const bool found = mps_playlist_entries_snapshot_get(source, &snapshot);
	obs_source_release(source);
	if (!found) {
		mps_playlist_entries_snapshot_free(&snapshot);
		update_play_button();
		return;
	}

	shuffle_info_->setVisible(snapshot.shuffle);
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
		item->setTextAlignment(0, Qt::AlignCenter);
		item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
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
	play_button_->setEnabled(selection_valid_ && !selected_source_uuid_.isEmpty());
	QTreeWidgetItem *item = selection_valid_ ? playlist_->currentItem() : nullptr;
	const QByteArray selected_path = item ? item->data(0, kPathRole).toString().toUtf8() : QByteArray();
	selected_value_->setFilePath(selected_path.isEmpty() ? nullptr : selected_path.constData());
}

void PlaylistControlDock::play_selected()
{
	if (!selection_valid_ || selected_source_uuid_.isEmpty())
		return;

	const QByteArray uuid = selected_source_uuid_.toUtf8();
	obs_source_t *source = obs_get_source_by_uuid(uuid.constData());
	if (!source)
		return;

	bool called = false;
	const char *id = obs_source_get_unversioned_id(source);
	proc_handler_t *handler = id && std::strcmp(id, kSourceId) == 0 ? obs_source_get_proc_handler(source) : nullptr;
	if (handler) {
		calldata_t calldata;
		calldata_init(&calldata);
		calldata_set_int(&calldata, "media_index", static_cast<long long>(selected_media_index_));
		calldata_set_int(&calldata, "folder_item_index", static_cast<long long>(selected_folder_item_index_));
		called = proc_handler_call(handler, "select_index", &calldata);
		calldata_free(&calldata);
	}
	obs_source_release(source);
	if (called)
		schedule_playback_refresh(uuid.constData());
}

extern "C" void mps_playlist_dock_register(void)
{
	if (queue_dock_instance || control_dock_instance)
		return;

	auto *queue = new PlaylistQueueDock;
	if (!obs_frontend_add_dock_by_id(kQueueDockId, "Playlist Queue", queue)) {
		delete queue;
		return;
	}
	queue_dock_instance = queue;

	auto *control = new PlaylistControlDock;
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
