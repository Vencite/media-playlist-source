#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <obs-frontend-api.h>

#include <QMutex>
#include <QSet>
#include <QString>
#include <QWidget>

class ElidedFileLabel;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QHideEvent;
class QTimer;
class QTreeWidget;

class PlaylistQueueDock final : public QWidget {
public:
	explicit PlaylistQueueDock(QWidget *parent = nullptr);
	~PlaylistQueueDock() override;

	void schedule_refresh();

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	static void playback_changed(void *data, const char *source_uuid);
	static void source_event(void *data, calldata_t *calldata);
	static void frontend_event(enum obs_frontend_event event, void *data);

	void refresh_sources();
	void refresh_snapshot();
	void refresh_progress();
	void clear_snapshot();
	void update_progress(int64_t time_ms, int64_t duration_ms);
	void schedule_playback_refresh(const char *source_uuid);

	QComboBox *source_selector_ = nullptr;
	ElidedFileLabel *previous_value_ = nullptr;
	ElidedFileLabel *current_value_ = nullptr;
	ElidedFileLabel *next_value_ = nullptr;
	QProgressBar *progress_bar_ = nullptr;
	QLabel *elapsed_value_ = nullptr;
	QLabel *remaining_value_ = nullptr;
	QTimer *progress_timer_ = nullptr;
	QString selected_source_uuid_;
	bool has_current_ = false;
	std::atomic_bool refresh_queued_ = false;
	QMutex playback_refresh_mutex_;
	QSet<QString> pending_playback_uuids_;
	bool playback_refresh_queued_ = false;
};

class PlaylistControlDock final : public QWidget {
public:
	explicit PlaylistControlDock(QWidget *parent = nullptr);
	~PlaylistControlDock() override;

	void schedule_refresh();

private:
	static void playback_changed(void *data, const char *source_uuid);
	static void source_event(void *data, calldata_t *calldata);
	static void frontend_event(enum obs_frontend_event event, void *data);

	void refresh_sources();
	void refresh_playlist();
	void update_play_button();
	void play_selected();
	void schedule_playback_refresh(const char *source_uuid);

	QComboBox *source_selector_ = nullptr;
	QLabel *shuffle_info_ = nullptr;
	QTreeWidget *playlist_ = nullptr;
	QPushButton *play_button_ = nullptr;
	QString selected_source_uuid_;
	QString selected_stable_id_;
	bool selection_valid_ = false;
	std::size_t selected_media_index_ = 0;
	std::size_t selected_folder_item_index_ = 0;
	std::atomic_bool refresh_queued_ = false;
	QMutex playback_refresh_mutex_;
	QSet<QString> pending_playback_uuids_;
	bool playback_refresh_queued_ = false;
};
