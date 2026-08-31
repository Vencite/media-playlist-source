#include "playlist-dock.hpp"

#include <cstring>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QVector>
#include <QVBoxLayout>

#include "playlist-dock.h"

namespace {

constexpr const char *kSourceId = "media_playlist_source_codeyan";
constexpr const char *kDockId = "media-playlist-source-dock";

struct SourceInfo {
	QString uuid;
	QString name;
};

QString display_path(const char *path)
{
	if (!path || !*path)
		return QStringLiteral("—");

	const QString value = QString::fromUtf8(path);
	const int slash = qMax(value.lastIndexOf(QLatin1Char('/')), value.lastIndexOf(QLatin1Char('\\')));
	return slash >= 0 ? value.mid(slash + 1) : value;
}

void add_value(QVBoxLayout *layout, const QString &title, QLabel **value)
{
	layout->addWidget(new QLabel(title));
	*value = new QLabel(QStringLiteral("—"));
	(*value)->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(*value);
}

} // namespace

PlaylistDock::PlaylistDock(QWidget *parent) : QWidget(parent)
{
	setWindowTitle(tr("Media Playlist"));

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(new QLabel(tr("MEDIA PLAYLIST")));

	auto *source_row = new QHBoxLayout;
	source_row->addWidget(new QLabel(tr("Source:")));
	source_selector_ = new QComboBox;
	source_selector_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	source_row->addWidget(source_selector_, 1);
	layout->addLayout(source_row);

	add_value(layout, tr("PREVIOUS"), &previous_value_);
	add_value(layout, tr("NOW PLAYING"), &current_value_);
	add_value(layout, tr("UP NEXT"), &next_value_);
	layout->addStretch();

	connect(source_selector_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
		if (index >= 0)
			selected_source_uuid_ = source_selector_->itemData(index).toString();
		else
			selected_source_uuid_.clear();
		refresh_snapshot();
	});

	signal_handler_t *handler = obs_get_signal_handler();
	signal_handler_connect(handler, "source_create", source_event, this);
	signal_handler_connect(handler, "source_remove", source_event, this);
	signal_handler_connect(handler, "source_destroy", source_event, this);
	signal_handler_connect(handler, "source_rename", source_event, this);
	obs_frontend_add_event_callback(frontend_event, this);
	mps_playlist_change_add_listener(playback_changed, this);
	refresh_sources();
}

PlaylistDock::~PlaylistDock()
{
	mps_playlist_change_remove_listener(playback_changed, this);
	obs_frontend_remove_event_callback(frontend_event, this);

	signal_handler_t *handler = obs_get_signal_handler();
	signal_handler_disconnect(handler, "source_create", source_event, this);
	signal_handler_disconnect(handler, "source_remove", source_event, this);
	signal_handler_disconnect(handler, "source_destroy", source_event, this);
	signal_handler_disconnect(handler, "source_rename", source_event, this);
}

void PlaylistDock::playback_changed(void *data, const char *source_uuid)
{
	(void)source_uuid;
	static_cast<PlaylistDock *>(data)->schedule_refresh();
}

void PlaylistDock::source_event(void *data, calldata_t *calldata)
{
	(void)calldata;
	static_cast<PlaylistDock *>(data)->schedule_refresh();
}

void PlaylistDock::frontend_event(enum obs_frontend_event event, void *data)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		static_cast<PlaylistDock *>(data)->schedule_refresh();
}

bool PlaylistDock::enum_source(void *data, obs_source_t *source)
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

void PlaylistDock::schedule_refresh()
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

void PlaylistDock::refresh_sources()
{
	QVector<SourceInfo> sources;
	obs_enum_sources(enum_source, &sources);

	const QString previous_selection = selected_source_uuid_;
	int selected_index = -1;
	source_selector_->blockSignals(true);
	source_selector_->clear();
	for (const SourceInfo &source : sources) {
		source_selector_->addItem(source.name, source.uuid);
		if (source.uuid == previous_selection)
			selected_index = source_selector_->count() - 1;
	}
	if (selected_index < 0 && previous_selection.isEmpty() && source_selector_->count() > 0)
		selected_index = 0;
	source_selector_->setCurrentIndex(selected_index);
	selected_source_uuid_ = selected_index >= 0 ? source_selector_->itemData(selected_index).toString() : QString();
	source_selector_->blockSignals(false);
	refresh_snapshot();
}

void PlaylistDock::clear_snapshot()
{
	previous_value_->setText(QStringLiteral("—"));
	current_value_->setText(QStringLiteral("—"));
	next_value_->setText(QStringLiteral("—"));
}

void PlaylistDock::refresh_snapshot()
{
	clear_snapshot();
	if (selected_source_uuid_.isEmpty())
		return;

	const QByteArray uuid = selected_source_uuid_.toUtf8();
	obs_source_t *source = obs_get_source_by_uuid(uuid.constData());
	if (!source)
		return;

	struct mps_playlist_snapshot snapshot = {0};
	if (mps_playlist_snapshot_get(source, &snapshot)) {
		previous_value_->setText(display_path(snapshot.previous));
		current_value_->setText(display_path(snapshot.current));
		next_value_->setText(display_path(snapshot.next));
	}
	mps_playlist_snapshot_free(&snapshot);
	obs_source_release(source);
}

namespace {

PlaylistDock *dock_instance;

} // namespace

extern "C" void mps_playlist_dock_register(void)
{
	if (dock_instance)
		return;

	dock_instance = new PlaylistDock;
	if (!obs_frontend_add_dock_by_id(kDockId, "Media Playlist", dock_instance)) {
		delete dock_instance;
		dock_instance = nullptr;
	}
}

extern "C" void mps_playlist_dock_unregister(void)
{
	if (!dock_instance)
		return;

	obs_frontend_remove_dock(kDockId);
	dock_instance = nullptr;
}
