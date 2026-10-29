/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <memory>
#include <cstdint>

/**
 * @brief プレゼンタイマーアプリ (Presentation Timer)
 */
class AppTimer : public mooncake::AppAbility {
public:
    AppTimer();
    ~AppTimer() override = default;

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum class State {
        Setting,
        Running,
        Paused,
        Finished
    };

    struct TimerMsg {
        enum Type {
            Tick,
            Finished
        } type;
        int remainingSeconds;
        int totalSeconds;
    };

    enum class ServoAction {
        None,
        // 残り30%の色変化時の1回首振り
        Alert30Turn,
        Alert30Home,
        // 残り10%のキョロキョロ
        LookAroundLeft,
        LookAroundRight,
        LookAroundHome,
        // 頭を撫でられた時のお辞儀 (笑顔で180度反対向いて目を瞑ってお辞儀)
        BowSaluteSmileTurn,
        BowSaluteBowDown,
        BowSaluteBowHold,
        BowSaluteLiftHead,
        BowSaluteBowUp
    };

    // 状態変数
    State _state = State::Setting;
    int _selected_minutes = 5;       // 01〜99 分 (初期値5分)
    int _total_seconds = 0;
    int _remaining_seconds = 0;
    bool _alert_30_percent_played = false;
    bool _alert_10_percent_played = false;
    uint32_t _next_look_around_ms = 0; // 残り10%中の定期キョロキョロ用
    int _head_pet_connection = -1;     // 頭部タッチシグナル接続ID

    enum class BellPattern {
        Single,    // 残り30% (1鈴)
        Double,    // 残り10% (2鈴)
        Finished   // 終了 (3鈴)
    };

    // FreeRTOSキュー & esp_timer
    QueueHandle_t _event_queue = nullptr;
    QueueHandle_t _sound_queue = nullptr;
    TaskHandle_t _sound_task_handle = nullptr;
    esp_timer_handle_t _timer_handle = nullptr;

    // サーボアニメーション管理 (ノンブロッキング)
    ServoAction _servo_action = ServoAction::None;
    uint32_t _servo_action_start_ms = 0;

    // まばたき管理
    uint32_t _next_blink_ms = 0;
    bool _is_blinking = false;
    uint32_t _blink_start_ms = 0;

    // LVGL UIオブジェクト
    // --- 設定画面 ---
    lv_obj_t* _panel_setting = nullptr;
    lv_obj_t* _label_time_setting = nullptr;

    // --- タイマー画面 ---
    lv_obj_t* _panel_timer = nullptr;
    lv_obj_t* _arc_timer = nullptr;
    lv_obj_t* _label_time_display = nullptr;
    lv_obj_t* _label_status = nullptr;
    lv_obj_t* _btn_pause_label = nullptr;

    // --- スタックちゃんの顔パーツ ---
    lv_obj_t* _face_container = nullptr;
    lv_obj_t* _eye_left = nullptr;
    lv_obj_t* _eye_right = nullptr;
    lv_obj_t* _mouth = nullptr;

    // 内部メソッド
    void create_setting_ui();
    void create_timer_ui();
    void destroy_ui();
    void update_setting_label();

    void start_timer();
    void pause_timer();
    void resume_timer();
    void stop_timer();

    void handle_timer_msg(const TimerMsg& msg);
    void update_arc_and_display(int remaining, int total);
    void update_avatar_face();
    void trigger_alert_30();
    void trigger_alert_10();
    void trigger_look_around();
    void trigger_alert_finished();
    void trigger_bow_salute();

    void update_servo_animation();
    void play_bell(BellPattern pattern);
    void execute_bell_sound(BellPattern pattern);

    static void sound_task_entry(void* arg);
    static void timer_callback(void* arg);
};
