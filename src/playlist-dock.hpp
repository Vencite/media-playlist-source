#pragma once

#include <atomic>

#include <obs-frontend-api.h>

#include <QString>
#include <QWidget>

class QComboBox;
class QLabel;

class PlaylistDock final : public QWidget {
public:
	explicit PlaylistDock(QWidget *parent = nullptr);
	~PlaylistDock() override;

	void schedule_refresh();

private:
	static void playback_changed(void *data, const char *source_uuid);
	static void source_event(void *data, calldata_t *calldata);
	static void frontend_event(enum obs_frontend_event event, void *data);
	static bool enum_source(void *data, obs_source_t *source);

	void refresh_sources();
	void refresh_snapshot();
	void clear_snapshot();

	QComboBox *source_selector_;
	QLabel *previous_value_;
	QLabel *current_value_;
	QLabel *next_value_;
	QString selected_source_uuid_;
	std::atomic_bool refresh_queued_ = false;
};
