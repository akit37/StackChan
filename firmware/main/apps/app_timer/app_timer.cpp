/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_timer.h"
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <board.h>
#include <audio/audio_codec.h>
#include <hal/board/config.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <assets/assets.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdio>

static const std::string _tag = "AppTimer";

AppTimer::AppTimer()
{
    setAppInfo().name = "TIMER";
    static auto icon  = assets::get_image("icon_timer.bin");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = 0x2ECC71;
    setAppInfo().userData       = (void*)&theme_color;
}

void AppTimer::onCreate()
{
    mclog::tagInfo(_tag, "onCreate");
}

void AppTimer::onOpen()
{
    mclog::tagInfo(_tag, "onOpen");

    _state                   = State::Setting;
    _selected_minutes        = 5;
    _total_seconds           = 0;
    _remaining_seconds       = 0;
    _alert_30_percent_played = false;
    _alert_10_percent_played = false;
    _servo_action            = ServoAction::None;

    // FreeRTOS イベントキューの初期化 (サイズ10)
    if (!_event_queue) {
        _event_queue = xQueueCreate(10, sizeof(TimerMsg));
    }

    // サウンド再生キュー & バックグラウンド再生タスクの初期化
    if (!_sound_queue) {
        _sound_queue = xQueueCreate(5, sizeof(BellPattern));
    }
    if (!_sound_task_handle) {
        xTaskCreatePinnedToCore(&AppTimer::sound_task_entry, "timer_snd", 4096, this, 3, &_sound_task_handle, 1);
    }

    // esp_timer の初期化
    esp_timer_create_args_t timer_args = {
        .callback        = &AppTimer::timer_callback,
        .arg             = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "presentation_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&timer_args, &_timer_handle);

    LvglLockGuard lock;

    create_setting_ui();
    create_timer_ui();

    // 頭部タッチ (なでなで) シグナルのバインド
    _head_pet_connection = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture gesture) {
        if (_state == State::Finished && _servo_action == ServoAction::None) {
            if (gesture == HeadPetGesture::SwipeForward || gesture == HeadPetGesture::SwipeBackward ||
                gesture == HeadPetGesture::Press) {
                trigger_bow_salute();
            }
        }
    });

    // 初期状態は設定画面を表示
    lv_obj_clear_flag(_panel_setting, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_panel_timer, LV_OBJ_FLAG_HIDDEN);

    // 下部ホームインジケーター（アプリ終了）
    view::create_home_indicator([this]() { close(); }, 0x2ECC71, 0x1A5235);
}

void AppTimer::onRunning()
{
    // 1. タイマーイベントキューの処理 (ノンブロッキング)
    if (_event_queue) {
        TimerMsg msg;
        while (xQueueReceive(_event_queue, &msg, 0) == pdTRUE) {
            handle_timer_msg(msg);
        }
    }

    // 2. 残り10%中の定期的なキョロキョロ
    if (_state == State::Running && _alert_10_percent_played && _servo_action == ServoAction::None) {
        uint32_t now = GetHAL().millis();
        if (now >= _next_look_around_ms && _next_look_around_ms != 0) {
            trigger_look_around();
        }
    }

    // 3. サーボアニメーションの更新
    update_servo_animation();

    // 4. スタックちゃんの顔（まばたき）アニメーション更新
    update_avatar_face();

    // 5. StackChan モーション更新
    GetStackChan().update();

    // 6. LVGL UI / ホームインジケーターの更新
    {
        LvglLockGuard lock;
        view::update_home_indicator();
    }
}

void AppTimer::onClose()
{
    mclog::tagInfo(_tag, "onClose");

    // 頭部タッチシグナルのバインド解除
    if (_head_pet_connection != -1) {
        GetHAL().onHeadPetGesture.disconnect(_head_pet_connection);
        _head_pet_connection = -1;
    }

    stop_timer();

    if (_timer_handle) {
        esp_timer_delete(_timer_handle);
        _timer_handle = nullptr;
    }

    if (_event_queue) {
        vQueueDelete(_event_queue);
        _event_queue = nullptr;
    }

    if (_sound_task_handle) {
        vTaskDelete(_sound_task_handle);
        _sound_task_handle = nullptr;
    }

    if (_sound_queue) {
        vQueueDelete(_sound_queue);
        _sound_queue = nullptr;
    }

    // サーボをホームポジションに復帰
    GetStackChan().motion().goHome();

    LvglLockGuard lock;
    view::destroy_home_indicator();
    destroy_ui();
}

void AppTimer::timer_callback(void* arg)
{
    auto* self = static_cast<AppTimer*>(arg);
    if (!self || self->_state != State::Running) {
        return;
    }

    self->_remaining_seconds--;

    TimerMsg msg;
    msg.remainingSeconds = self->_remaining_seconds;
    msg.totalSeconds     = self->_total_seconds;

    if (self->_remaining_seconds <= 0) {
        msg.type = TimerMsg::Type::Finished;
    } else {
        msg.type = TimerMsg::Type::Tick;
    }

    if (self->_event_queue) {
        xQueueSend(self->_event_queue, &msg, 0);
    }
}

void AppTimer::start_timer()
{
    _state                   = State::Running;
    _total_seconds           = _selected_minutes * 60;
    _remaining_seconds       = _total_seconds;
    _alert_30_percent_played = false;
    _alert_10_percent_played = false;
    _next_look_around_ms     = 0;
    _servo_action            = ServoAction::None;

    // まばたきのタイマー初期化
    _next_blink_ms = GetHAL().millis() + 2500;
    _is_blinking   = false;

    LvglLockGuard lock;
    lv_obj_add_flag(_panel_setting, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_panel_timer, LV_OBJ_FLAG_HIDDEN);

    if (_label_status) {
        lv_label_set_text(_label_status, "RUNNING");
    }
    if (_btn_pause_label) {
        lv_label_set_text(_btn_pause_label, "PAUSE");
    }

    // 目と口を標準サイズに初期化
    if (_eye_left && _eye_right) {
        lv_obj_set_size(_eye_left, 16, 16);
        lv_obj_set_size(_eye_right, 16, 16);
    }
    if (_mouth) {
        lv_obj_set_size(_mouth, 22, 5);
    }

    update_arc_and_display(_remaining_seconds, _total_seconds);

    // 1秒 (1,000,000マイクロ秒) 周期タイマー開始
    if (_timer_handle) {
        esp_timer_start_periodic(_timer_handle, 1000000);
    }
}

void AppTimer::pause_timer()
{
    if (_state != State::Running) return;
    _state = State::Paused;

    if (_timer_handle) {
        esp_timer_stop(_timer_handle);
    }

    LvglLockGuard lock;
    if (_label_status) {
        lv_label_set_text(_label_status, "PAUSED");
    }
    if (_btn_pause_label) {
        lv_label_set_text(_btn_pause_label, "RESUME");
    }
}

void AppTimer::resume_timer()
{
    if (_state != State::Paused) return;
    _state = State::Running;

    if (_timer_handle) {
        esp_timer_start_periodic(_timer_handle, 1000000);
    }

    LvglLockGuard lock;
    if (_label_status) {
        lv_label_set_text(_label_status, "RUNNING");
    }
    if (_btn_pause_label) {
        lv_label_set_text(_btn_pause_label, "PAUSE");
    }
}

void AppTimer::stop_timer()
{
    if (_timer_handle) {
        esp_timer_stop(_timer_handle);
    }
    _state = State::Setting;

    LvglLockGuard lock;
    if (_panel_timer && _panel_setting) {
        lv_obj_add_flag(_panel_timer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_panel_setting, LV_OBJ_FLAG_HIDDEN);
    }
    GetStackChan().motion().goHome();
}

void AppTimer::handle_timer_msg(const TimerMsg& msg)
{
    LvglLockGuard lock;

    if (msg.type == TimerMsg::Type::Finished) {
        _state = State::Finished;
        if (_timer_handle) {
            esp_timer_stop(_timer_handle);
        }
        update_arc_and_display(0, msg.totalSeconds);
        if (_label_status) {
            lv_label_set_text(_label_status, "TIME UP!");
        }
        if (_btn_pause_label) {
            lv_label_set_text(_btn_pause_label, "RESET");
        }

        // 時間切れチャイムが鳴ったら、いったんまっすぐ向いてストップ
        _servo_action = ServoAction::None;
        GetStackChan().motion().goHome(350);

        if (_eye_left && _eye_right) {
            lv_obj_set_size(_eye_left, 16, 16);
            lv_obj_set_size(_eye_right, 16, 16);
        }

        trigger_alert_finished();
        return;
    }

    update_arc_and_display(msg.remainingSeconds, msg.totalSeconds);

    float ratio = (float)msg.remainingSeconds / (float)msg.totalSeconds;

    // 残り 30% 到達判定 (黄に色変化 & 1鈴 + 1回首振り)
    if (ratio <= 0.30f && !_alert_30_percent_played) {
        _alert_30_percent_played = true;
        trigger_alert_30();
    }

    // 残り 10% 到達判定 (赤に色変化 & 2鈴 + キョロキョロ)
    if (ratio <= 0.10f && !_alert_10_percent_played) {
        _alert_10_percent_played = true;
        trigger_alert_10();
    }
}

void AppTimer::update_arc_and_display(int remaining, int total)
{
    if (!_arc_timer || !_label_time_display) return;

    remaining = std::max(0, remaining);
    total     = std::max(1, total);

    // 1. 円の角度計算 (満タンで360度、0で0度)
    int angle = (remaining * 360) / total;
    lv_arc_set_angles(_arc_timer, 0, angle);

    // 2. 色の決定: 最初は緑、残り30%で黄、残り10%で赤
    float ratio = (float)remaining / (float)total;
    lv_color_t color;
    if (ratio > 0.30f) {
        color = lv_color_hex(0x2ECC71); // 緑
    } else if (ratio > 0.10f) {
        color = lv_color_hex(0xF1C40F); // 黄
    } else {
        color = lv_color_hex(0xE74C3C); // 赤
    }
    lv_obj_set_style_arc_color(_arc_timer, color, LV_PART_INDICATOR);

    // 3. 残り時間 MM:SS 表示
    int minutes = remaining / 60;
    int seconds = remaining % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", minutes, seconds);
    lv_label_set_text(_label_time_display, buf);
}

void AppTimer::play_bell(BellPattern pattern)
{
    if (_sound_queue) {
        xQueueSend(_sound_queue, &pattern, 0);
    }
}

void AppTimer::sound_task_entry(void* arg)
{
    auto* self = static_cast<AppTimer*>(arg);
    BellPattern pattern;
    while (true) {
        if (xQueueReceive(self->_sound_queue, &pattern, portMAX_DELAY) == pdTRUE) {
            self->execute_bell_sound(pattern);
        }
    }
}

void AppTimer::execute_bell_sound(BellPattern pattern)
{
    auto& board      = Board::GetInstance();
    auto audio_codec = board.GetAudioCodec();
    if (!audio_codec) {
        return;
    }

    if (!audio_codec->output_enabled()) {
        audio_codec->EnableOutput(true);
    }

    auto play_bell_pulse = [&](float freq, float duration_sec, float decay_rate) {
        const int sample_rate = AUDIO_OUTPUT_SAMPLE_RATE;
        const int total_samples = static_cast<int>(sample_rate * duration_sec);
        std::vector<int16_t> buf(total_samples);

        for (int i = 0; i < total_samples; ++i) {
            float t = static_cast<float>(i) / sample_rate;
            float attack = std::min(1.0f, t * 400.0f);
            float decay = std::exp(-t * decay_rate);
            float env = attack * decay;
            // ベル音: 基音 + 2.76倍音（鐘の非調和音）
            float s = (std::sin(2.0f * (float)M_PI * freq * t) * 0.75f +
                       std::sin(2.0f * (float)M_PI * (freq * 2.76f) * t) * 0.25f) * env;
            buf[i] = static_cast<int16_t>(s * 22000.0f);
        }
        audio_codec->OutputData(buf);
    };

    if (pattern == BellPattern::Single) {
        // 1鈴: 1046Hz (C6)
        play_bell_pulse(1046.5f, 0.7f, 4.0f);
    } else if (pattern == BellPattern::Double) {
        // 2鈴: 1046Hz -> 100ms -> 1046Hz
        play_bell_pulse(1046.5f, 0.4f, 5.0f);
        vTaskDelay(pdMS_TO_TICKS(120));
        play_bell_pulse(1046.5f, 0.6f, 4.0f);
    } else if (pattern == BellPattern::Finished) {
        // 終了: 和音風 3鈴チャイム (G5 -> B5 -> D6)
        play_bell_pulse(784.0f, 0.35f, 5.0f);
        vTaskDelay(pdMS_TO_TICKS(100));
        play_bell_pulse(987.8f, 0.35f, 5.0f);
        vTaskDelay(pdMS_TO_TICKS(100));
        play_bell_pulse(1174.7f, 0.9f, 3.0f);
    }

    vTaskDelay(pdMS_TO_TICKS(150));
    audio_codec->EnableOutput(false);
}

void AppTimer::trigger_alert_30()
{
    mclog::tagInfo(_tag, "alert 30% (1 bell & 1 head turn)");
    play_bell(BellPattern::Single);

    if (_servo_action == ServoAction::None) {
        _servo_action          = ServoAction::Alert30Turn;
        _servo_action_start_ms = GetHAL().millis();
        // 軽やかに片側へ首を振って戻る (1回首振り)
        GetStackChan().motion().lookAtNormalized(0.25f, 0.0f, 650);
    }
}

void AppTimer::trigger_alert_10()
{
    mclog::tagInfo(_tag, "alert 10% (2 bells & look around)");
    play_bell(BellPattern::Double);

    trigger_look_around();
}

void AppTimer::trigger_look_around()
{
    if (_servo_action != ServoAction::None) return;
    mclog::tagInfo(_tag, "look around motion (gentle)");
    _servo_action          = ServoAction::LookAroundLeft;
    _servo_action_start_ms = GetHAL().millis();
    // 角度控えめ・速度ゆっくりで見渡す
    GetStackChan().motion().lookAtNormalized(-0.30f, 0.0f, 750);
}

void AppTimer::trigger_alert_finished()
{
    mclog::tagInfo(_tag, "alert finished (time up bell, wait for salute)");
    play_bell(BellPattern::Finished);
    // 自動でのお辞儀は行わず、正面を向いて待機 (お辞儀のタイミングは手動)
}

void AppTimer::trigger_bow_salute()
{
    if (_servo_action != ServoAction::None) return;
    mclog::tagInfo(_tag, "bow salute triggered! turning 180 deg and bow");

    _servo_action          = ServoAction::BowSaluteSmileTurn;
    _servo_action_start_ms = GetHAL().millis();

    LvglLockGuard lock;
    // にっこり笑顔 (目を瞑る)
    if (_eye_left && _eye_right) {
        lv_obj_set_size(_eye_left, 16, 2);
        lv_obj_set_size(_eye_right, 16, 2);
    }
    if (_label_status) {
        lv_label_set_text(_label_status, "ARIGATO!");
    }

    // 180度反対（Yaw目一杯反転、少し見上げる）をゆっくり向く
    GetStackChan().motion().lookAtNormalized(1.0f, 0.1f, 900);
}

void AppTimer::update_servo_animation()
{
    if (_servo_action == ServoAction::None) return;

    uint32_t elapsed = GetHAL().millis() - _servo_action_start_ms;

    switch (_servo_action) {
        // --- 残り30%の色変化時の1回首振り ---
        case ServoAction::Alert30Turn:
            if (elapsed >= 650) {
                _servo_action          = ServoAction::Alert30Home;
                _servo_action_start_ms = GetHAL().millis();
                GetStackChan().motion().goHome(600);
            }
            break;
        case ServoAction::Alert30Home:
            if (elapsed >= 600) {
                _servo_action = ServoAction::None;
            }
            break;

        // --- 残り10%のキョロキョロ (速度を抑えてゆったり見渡す) ---
        case ServoAction::LookAroundLeft:
            if (elapsed >= 800) {
                _servo_action          = ServoAction::LookAroundRight;
                _servo_action_start_ms = GetHAL().millis();
                GetStackChan().motion().lookAtNormalized(0.30f, 0.0f, 800);
            }
            break;
        case ServoAction::LookAroundRight:
            if (elapsed >= 850) {
                _servo_action          = ServoAction::LookAroundHome;
                _servo_action_start_ms = GetHAL().millis();
                GetStackChan().motion().goHome(750);
            }
            break;
        case ServoAction::LookAroundHome:
            if (elapsed >= 800) {
                _servo_action = ServoAction::None;
                // 残り10%中であれば、時間切れまで時々キョロキョロするように次回時刻を設定 (4〜7秒後)
                if (_state == State::Running && _alert_10_percent_played) {
                    _next_look_around_ms = GetHAL().millis() + 4000 + (esp_random() % 3000);
                }
            }
            break;

        // --- 頭を撫でられた時のお辞儀シーケンス (丁寧で落ち着いた動作) ---
        case ServoAction::BowSaluteSmileTurn:
            if (elapsed >= 1000) {
                // 反対を向いた状態で深々とお辞儀
                _servo_action          = ServoAction::BowSaluteBowDown;
                _servo_action_start_ms = GetHAL().millis();
                GetStackChan().motion().lookAtNormalized(1.0f, -0.80f, 750);
            }
            break;
        case ServoAction::BowSaluteBowDown:
            if (elapsed >= 850) {
                // お辞儀をキープ
                _servo_action          = ServoAction::BowSaluteBowHold;
                _servo_action_start_ms = GetHAL().millis();
            }
            break;
        case ServoAction::BowSaluteBowHold:
            if (elapsed >= 2000) {
                // お辞儀から少し頭を持ち上げる（余韻・感謝の会釈）
                _servo_action          = ServoAction::BowSaluteLiftHead;
                _servo_action_start_ms = GetHAL().millis();
                GetStackChan().motion().lookAtNormalized(1.0f, -0.20f, 650);
            }
            break;
        case ServoAction::BowSaluteLiftHead:
            if (elapsed >= 800) {
                // 正面にゆっくり復帰
                _servo_action          = ServoAction::BowSaluteBowUp;
                _servo_action_start_ms = GetHAL().millis();
                GetStackChan().motion().goHome(900);
            }
            break;
        case ServoAction::BowSaluteBowUp:
            if (elapsed >= 1000) {
                _servo_action = ServoAction::None;
                LvglLockGuard lock;
                // パチッと目を開く
                if (_eye_left && _eye_right) {
                    lv_obj_set_size(_eye_left, 16, 16);
                    lv_obj_set_size(_eye_right, 16, 16);
                }
            }
            break;

        default:
            _servo_action = ServoAction::None;
            break;
    }
}

void AppTimer::update_avatar_face()
{
    // お辞儀中は目を瞑ったままキープ
    if (_servo_action == ServoAction::BowSaluteSmileTurn ||
        _servo_action == ServoAction::BowSaluteBowDown ||
        _servo_action == ServoAction::BowSaluteBowHold) {
        return;
    }

    if (_state != State::Running && _state != State::Paused) return;
    if (!_eye_left || !_eye_right) return;

    uint32_t now = GetHAL().millis();

    if (!_is_blinking) {
        if (now >= _next_blink_ms) {
            _is_blinking    = true;
            _blink_start_ms = now;
            LvglLockGuard lock;
            if (_eye_left && _eye_right) {
                lv_obj_set_size(_eye_left, 16, 2);
                lv_obj_set_size(_eye_right, 16, 2);
            }
        }
    } else {
        if (now - _blink_start_ms >= 140) {
            _is_blinking   = false;
            _next_blink_ms = now + 3000 + (esp_random() % 3000); // 3〜6秒ごとにまばたき
            LvglLockGuard lock;
            if (_eye_left && _eye_right) {
                lv_obj_set_size(_eye_left, 16, 16);
                lv_obj_set_size(_eye_right, 16, 16);
            }
        }
    }
}

void AppTimer::create_setting_ui()
{
    _panel_setting = lv_obj_create(lv_screen_active());
    lv_obj_set_size(_panel_setting, 320, 240);
    lv_obj_set_align(_panel_setting, LV_ALIGN_CENTER);
    lv_obj_set_style_bg_color(_panel_setting, lv_color_hex(0x181824), 0);
    lv_obj_set_style_border_width(_panel_setting, 0, 0);
    lv_obj_set_style_radius(_panel_setting, 0, 0);
    lv_obj_clear_flag(_panel_setting, LV_OBJ_FLAG_SCROLLABLE);

    // タイトル
    lv_obj_t* title = lv_label_create(_panel_setting);
    lv_label_set_text(title, "PRESENTATION TIMER");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x8E8EA0), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    // ホームに戻るボタン (左上)
    lv_obj_t* btn_back = lv_button_create(_panel_setting);
    lv_obj_set_size(btn_back, 44, 34);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x282A36), 0);
    lv_obj_set_style_radius(btn_back, 8, 0);

    lv_obj_t* lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_back);

    lv_obj_add_event_cb(
        btn_back,
        [](lv_event_t* e) {
            auto* self = static_cast<AppTimer*>(lv_event_get_user_data(e));
            self->close();
        },
        LV_EVENT_CLICKED, this);

    // 時間表示
    _label_time_setting = lv_label_create(_panel_setting);
    lv_obj_set_style_text_font(_label_time_setting, &MontserratSemiBold26, 0);
    lv_obj_set_style_text_color(_label_time_setting, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(_label_time_setting, LV_ALIGN_CENTER, 0, -32);
    update_setting_label();

    auto setup_btn = [this](int x, int delta, const char* label_str) {
        lv_obj_t* btn = lv_button_create(_panel_setting);
        lv_obj_set_size(btn, 62, 38);
        lv_obj_align(btn, LV_ALIGN_CENTER, x, 22);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x282A36), 0);
        lv_obj_set_style_radius(btn, 8, 0);

        lv_obj_t* l = lv_label_create(btn);
        lv_label_set_text(l, label_str);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_center(l);

        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                auto* self = static_cast<AppTimer*>(lv_event_get_user_data(e));
                auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
                int d = (int)(intptr_t)lv_obj_get_user_data(target);
                self->_selected_minutes = std::clamp(self->_selected_minutes + d, 1, 99);
                self->update_setting_label();
            },
            LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(btn, (void*)(intptr_t)delta);
    };

    // 4つの調整ボタン
    setup_btn(-105, -10, "-10");
    setup_btn(-35,  -1,  "-1");
    setup_btn(35,   1,   "+1");
    setup_btn(105,  10,  "+10");

    // START ボタン
    lv_obj_t* btn_start = lv_button_create(_panel_setting);
    lv_obj_set_size(btn_start, 140, 42);
    lv_obj_align(btn_start, LV_ALIGN_CENTER, 0, 75);
    lv_obj_set_style_bg_color(btn_start, lv_color_hex(0x2ECC71), 0);
    lv_obj_set_style_radius(btn_start, 21, 0);

    lv_obj_t* lbl_start = lv_label_create(btn_start);
    lv_label_set_text(lbl_start, "START");
    lv_obj_set_style_text_font(lbl_start, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl_start);

    lv_obj_add_event_cb(
        btn_start,
        [](lv_event_t* e) {
            auto* self = static_cast<AppTimer*>(lv_event_get_user_data(e));
            self->start_timer();
        },
        LV_EVENT_CLICKED, this);
}

void AppTimer::update_setting_label()
{
    if (!_label_time_setting) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d min", _selected_minutes);
    lv_label_set_text(_label_time_setting, buf);
}

void AppTimer::create_timer_ui()
{
    _panel_timer = lv_obj_create(lv_screen_active());
    lv_obj_set_size(_panel_timer, 320, 240);
    lv_obj_set_align(_panel_timer, LV_ALIGN_CENTER);
    lv_obj_set_style_bg_color(_panel_timer, lv_color_hex(0x101018), 0);
    lv_obj_set_style_border_width(_panel_timer, 0, 0);
    lv_obj_set_style_radius(_panel_timer, 0, 0);
    lv_obj_clear_flag(_panel_timer, LV_OBJ_FLAG_SCROLLABLE);

    // 1. CANCEL ボタン (左上)
    lv_obj_t* btn_cancel = lv_button_create(_panel_timer);
    lv_obj_set_size(btn_cancel, 68, 30);
    lv_obj_align(btn_cancel, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x282A36), 0);
    lv_obj_set_style_radius(btn_cancel, 6, 0);

    lv_obj_t* lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "CANCEL");
    lv_obj_set_style_text_font(lbl_cancel, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_cancel);

    lv_obj_add_event_cb(
        btn_cancel,
        [](lv_event_t* e) {
            auto* self = static_cast<AppTimer*>(lv_event_get_user_data(e));
            self->stop_timer();
        },
        LV_EVENT_CLICKED, this);

    // 2. 残り時間表示バッジ (右上: 数字は控えめに小さく表示)
    lv_obj_t* badge_time = lv_obj_create(_panel_timer);
    lv_obj_set_size(badge_time, 78, 30);
    lv_obj_align(badge_time, LV_ALIGN_TOP_RIGHT, -10, 8);
    lv_obj_set_style_bg_color(badge_time, lv_color_hex(0x1E202C), 0);
    lv_obj_set_style_border_width(badge_time, 0, 0);
    lv_obj_set_style_radius(badge_time, 6, 0);
    lv_obj_clear_flag(badge_time, LV_OBJ_FLAG_SCROLLABLE);

    _label_time_display = lv_label_create(badge_time);
    lv_obj_set_style_text_font(_label_time_display, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_label_time_display, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(_label_time_display);
    lv_label_set_text(_label_time_display, "00:00");

    // 3. 円弧ゲージ (中央: 直径176px)
    _arc_timer = lv_arc_create(_panel_timer);
    lv_obj_set_size(_arc_timer, 176, 176);
    lv_obj_align(_arc_timer, LV_ALIGN_CENTER, 0, -8);
    lv_arc_set_rotation(_arc_timer, 270); // 12時方向 (上) から時計回り
    lv_arc_set_bg_angles(_arc_timer, 0, 360);
    lv_arc_set_angles(_arc_timer, 0, 360);

    // ノブ（つまみ）を非表示
    lv_obj_remove_style(_arc_timer, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(_arc_timer, LV_OBJ_FLAG_CLICKABLE);

    // 背景レールスタイル
    lv_obj_set_style_arc_width(_arc_timer, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(_arc_timer, lv_color_hex(0x222230), LV_PART_MAIN);

    // インジケータースタイル
    lv_obj_set_style_arc_width(_arc_timer, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(_arc_timer, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(_arc_timer, lv_color_hex(0x2ECC71), LV_PART_INDICATOR);

    // 4. スタックちゃんのかわいい顔パーツ (円ゲージの中央)
    _face_container = lv_obj_create(_panel_timer);
    lv_obj_set_size(_face_container, 140, 100);
    lv_obj_align(_face_container, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_bg_opa(_face_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_face_container, 0, 0);
    lv_obj_clear_flag(_face_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_face_container, LV_OBJ_FLAG_CLICKABLE);

    // 画面タップでも頭部タッチと同様にお辞儀を発動可能にする
    lv_obj_add_event_cb(
        _face_container,
        [](lv_event_t* e) {
            auto* self = static_cast<AppTimer*>(lv_event_get_user_data(e));
            if (self->_state == State::Finished && self->_servo_action == ServoAction::None) {
                self->trigger_bow_salute();
            }
        },
        LV_EVENT_CLICKED, this);

    // 左目
    _eye_left = lv_obj_create(_face_container);
    lv_obj_set_size(_eye_left, 16, 16);
    lv_obj_set_style_radius(_eye_left, 8, 0);
    lv_obj_set_style_bg_color(_eye_left, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(_eye_left, 0, 0);
    lv_obj_align(_eye_left, LV_ALIGN_CENTER, -28, -6);
    lv_obj_clear_flag(_eye_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_eye_left, LV_OBJ_FLAG_CLICKABLE);

    // 右目
    _eye_right = lv_obj_create(_face_container);
    lv_obj_set_size(_eye_right, 16, 16);
    lv_obj_set_style_radius(_eye_right, 8, 0);
    lv_obj_set_style_bg_color(_eye_right, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(_eye_right, 0, 0);
    lv_obj_align(_eye_right, LV_ALIGN_CENTER, 28, -6);
    lv_obj_clear_flag(_eye_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_eye_right, LV_OBJ_FLAG_CLICKABLE);

    // 口
    _mouth = lv_obj_create(_face_container);
    lv_obj_set_size(_mouth, 22, 5);
    lv_obj_set_style_radius(_mouth, 2, 0);
    lv_obj_set_style_bg_color(_mouth, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(_mouth, 0, 0);
    lv_obj_align(_mouth, LV_ALIGN_CENTER, 0, 16);
    lv_obj_clear_flag(_mouth, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_mouth, LV_OBJ_FLAG_CLICKABLE);

    // 5. ステータスラベル (PAUSED / TIME UP 通知用)
    _label_status = lv_label_create(_panel_timer);
    lv_obj_set_style_text_font(_label_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_label_status, lv_color_hex(0x8E8EA0), 0);
    lv_obj_align(_label_status, LV_ALIGN_CENTER, 0, 36);
    lv_label_set_text(_label_status, "");

    // 6. PAUSE / RESUME / RESET ボタン (左下余白)
    lv_obj_t* btn_pause = lv_button_create(_panel_timer);
    lv_obj_set_size(btn_pause, 68, 30);
    lv_obj_align(btn_pause, LV_ALIGN_BOTTOM_LEFT, 10, -8);
    lv_obj_set_style_bg_color(btn_pause, lv_color_hex(0x282A36), 0);
    lv_obj_set_style_radius(btn_pause, 6, 0);

    _btn_pause_label = lv_label_create(btn_pause);
    lv_label_set_text(_btn_pause_label, "PAUSE");
    lv_obj_set_style_text_font(_btn_pause_label, &lv_font_montserrat_14, 0);
    lv_obj_center(_btn_pause_label);

    lv_obj_add_event_cb(
        btn_pause,
        [](lv_event_t* e) {
            auto* self = static_cast<AppTimer*>(lv_event_get_user_data(e));
            if (self->_state == State::Running) {
                self->pause_timer();
            } else if (self->_state == State::Paused) {
                self->resume_timer();
            } else if (self->_state == State::Finished) {
                self->stop_timer();
            }
        },
        LV_EVENT_CLICKED, this);
}

void AppTimer::destroy_ui()
{
    if (_panel_setting) {
        lv_obj_delete(_panel_setting);
        _panel_setting      = nullptr;
        _label_time_setting = nullptr;
    }
    if (_panel_timer) {
        lv_obj_delete(_panel_timer);
        _panel_timer        = nullptr;
        _arc_timer          = nullptr;
        _label_time_display = nullptr;
        _label_status       = nullptr;
        _btn_pause_label    = nullptr;
        _face_container     = nullptr;
        _eye_left           = nullptr;
        _eye_right          = nullptr;
        _mouth              = nullptr;
    }
}
