#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <obs-frontend-api.h>

#include <QMutex>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

class QAction;
class ElidedFileLabel;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QHideEvent;
class QTimer;
class QTreeWidget;

class PlaylistProgramTracker final : public QObject {
	Q_OBJECT

public:
	PlaylistProgramTracker();
	~PlaylistProgramTracker() override;

	const QVector<QPair<QString, QString>> &public_sources() const { return public_sources_; }
	const QHash<QString, QString> &program_sources() const { return program_sources_; }

	bool is_current_program_source(const QString &uuid) const;

signals:
	void sources_changed();

private:
	static void source_event(void *data, calldata_t *calldata);
	static void channel_event(void *data, calldata_t *calldata);
	static void graph_event(void *data, calldata_t *calldata);
	static void frontend_event(enum obs_frontend_event event, void *data);
	void schedule_refresh();
	void refresh();
	void update_scene_connections(const QSet<signal_handler_t *> &handlers);
	void update_transition_connection(signal_handler_t *handler);

	QVector<QPair<QString, QString>> public_sources_; // UUID, name
	QHash<QString, QString> program_sources_;
	QSet<signal_handler_t *> scene_handlers_;
	signal_handler_t *transition_handler_ = nullptr;
	std::atomic_bool refresh_queued_ = false;
	bool collection_changing_ = false;
};

class PlaylistQueueDock final : public QWidget {
public:
	explicit PlaylistQueueDock(std::shared_ptr<PlaylistProgramTracker> program, QWidget *parent = nullptr);
	~PlaylistQueueDock() override;

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	static void playback_changed(void *data, const char *source_uuid);

	void refresh_sources();
	void refresh_snapshot();
	void refresh_progress();
	void clear_snapshot();
	void refresh_context(obs_source_t *source, const struct mps_playlist_telemetry *telemetry);
	void update_timing(const struct mps_playlist_telemetry *telemetry);
	void update_progress(int64_t time_ms, int64_t duration_ms, int64_t remaining_ms, int64_t playlist_remaining_ms);
	void schedule_playback_refresh(const char *source_uuid);
	void cache_duration(const char *path, int64_t duration_ms);
	int64_t cached_duration(const char *path) const;

	QComboBox *source_selector_ = nullptr;
	QLabel *follow_status_ = nullptr;
	ElidedFileLabel *previous_value_ = nullptr;
	ElidedFileLabel *current_value_ = nullptr;
	ElidedFileLabel *next_value_ = nullptr;
	QLabel *previous_duration_value_ = nullptr;
	QLabel *current_duration_value_ = nullptr;
	QLabel *next_duration_value_ = nullptr;
	QProgressBar *progress_bar_ = nullptr;
	QLabel *elapsed_value_ = nullptr;
	QLabel *remaining_value_ = nullptr;
	QLabel *playlist_remaining_value_ = nullptr;
	QWidget *playlist_remaining_row_ = nullptr;
	QAction *shuffle_action_ = nullptr;
	QAction *loop_action_ = nullptr;
	QTimer *progress_timer_ = nullptr;
	QAction *follow_program_action_ = nullptr;
	std::shared_ptr<PlaylistProgramTracker> program_;
	QString manual_source_uuid_;
	QString selected_source_uuid_;
	QHash<QString, int64_t> duration_cache_;
	bool has_current_ = false;
	bool context_available_ = false;
	bool follow_program_ = false;
	QMutex playback_refresh_mutex_;
	QSet<QString> pending_playback_uuids_;
	bool playback_refresh_queued_ = false;
};

class PlaylistControlDock final : public QWidget {
public:
	explicit PlaylistControlDock(std::shared_ptr<PlaylistProgramTracker> program, QWidget *parent = nullptr);
	~PlaylistControlDock() override;

private:
	static void playback_changed(void *data, const char *source_uuid);

	void refresh_sources();
	void refresh_playlist();
	void update_play_button();
	void play_selected();
	enum class PlaybackAction { Previous, PlayPause, Stop, Next };
	void control_playback(PlaybackAction action);
	void schedule_playback_refresh(const char *source_uuid);

	QComboBox *source_selector_ = nullptr;
	QLabel *follow_status_ = nullptr;
	QLabel *shuffle_info_ = nullptr;
	QAction *shuffle_action_ = nullptr;
	QAction *loop_action_ = nullptr;
	QWidget *playback_controls_ = nullptr;
	QPushButton *pause_button_ = nullptr;
	std::uint64_t playlist_revision_ = 0;
	QTreeWidget *playlist_ = nullptr;
	ElidedFileLabel *selected_value_ = nullptr;
	QPushButton *play_button_ = nullptr;
	QAction *follow_program_action_ = nullptr;
	std::shared_ptr<PlaylistProgramTracker> program_;
	QString manual_source_uuid_;
	QString selected_source_uuid_;
	QString selected_stable_id_;
	bool selection_valid_ = false;
	bool playlist_available_ = false;
	std::size_t selected_media_index_ = 0;
	std::size_t selected_folder_item_index_ = 0;
	bool follow_program_ = false;
	QMutex playback_refresh_mutex_;
	QSet<QString> pending_playback_uuids_;
	bool playback_refresh_queued_ = false;
};
