#include "appstate.hpp"
#include "record.hpp"

AppState &AppState::get() {
	static AppState obj;
	return obj;
}

AppState::AppState() {
	Record r("app-state");

#define READ(a) r.read(a, #a)
	if (r.version() > 0x000710) {
		READ(play_speed);
		READ(audio_amp);
	}

	READ(video_aspect_ratio);
	READ(video_crop_ratio);
	READ(video_vertical_alignment);
	READ(video_horizontal_alignment);
	READ(video_offset);
	READ(video_effects);
	READ(video_color);
	READ(video_deinterlacing);
	READ(video_interpolator);
	READ(video_chroma_upscaler);
	READ(video_dithering);

	READ(audio_volume);
	READ(audio_volume_normalizer);
	READ(audio_tempo_scaler);
	READ(audio_muted);
	READ(audio_sync);

	READ(sub_position);
	READ(sub_display);
	READ(sub_alignment);
	READ(sub_sync);

	READ(window_stays_on_top);

	READ(open_folder_types);
	READ(open_last_folder);
	READ(open_last_file);
	READ(open_url_list);
	READ(open_url_enc);
	READ(ask_system_tray);

	READ(auto_exit);

	READ(win_pos);
	READ(win_size);

	READ(playlist_visible);
	READ(history_visible);
	READ(playinfo_visible);
#undef READ
}

void AppState::save() const {
	Record r("app-state");
#define WRITE(a) r.write(a, #a);
	WRITE(play_speed);

	WRITE(video_aspect_ratio);
	WRITE(video_crop_ratio);
	WRITE(video_vertical_alignment);
	WRITE(video_horizontal_alignment);
	WRITE(video_offset);
	WRITE(video_effects);
	WRITE(video_color);
	WRITE(video_deinterlacing);
	WRITE(video_chroma_upscaler);
	WRITE(video_interpolator);
	WRITE(video_dithering);

	WRITE(audio_volume);
	WRITE(audio_volume_normalizer);
	WRITE(audio_tempo_scaler);
	WRITE(audio_muted);
	WRITE(audio_amp);
	WRITE(audio_sync);

	WRITE(sub_position);
	WRITE(sub_display);
	WRITE(sub_alignment);
	WRITE(sub_sync);

	WRITE(window_stays_on_top);

	WRITE(open_folder_types);
	WRITE(open_last_folder);
	WRITE(open_last_file);
	WRITE(ask_system_tray);
	WRITE(open_url_list);
	WRITE(open_url_enc);

	WRITE(auto_exit);

	WRITE(win_pos);
	WRITE(win_size);

	WRITE(playlist_visible);
	WRITE(history_visible);
	WRITE(playinfo_visible);
#undef WRITE
}

