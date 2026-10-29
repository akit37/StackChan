/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "workers.h"
#include <src/misc/lv_area.h>
#include <src/misc/lv_text.h>
#include <stackchan/stackchan.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <wifi_manager.h>
#include <ssid_manager.h>
#include <assets/lang_config.h>
#include <smooth_ui_toolkit.hpp>
#include <memory>

using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace setup_workers;
using namespace stackchan;

static std::string _tag = "Setup-Connectivity";

WifiSetupWorker::WifiSetupWorker()
{
    _state       = State::ConfigAp;
    _last_state  = State::None;
    _is_first_in = true;
    _sta_connected = false;

    // Start Wi-Fi Config AP mode
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized()) {
        WifiManagerConfig config;
        config.ssid_prefix = "StackChan";
        config.language    = Lang::CODE;
        wifi.Initialize(config);
    }

    // Set callback to detect when iPhone/device joins the SoftAP
    wifi.SetEventCallback([this](WifiEvent event, const std::string& data) {
        if (event == WifiEvent::ConfigApStationConnected) {
            _sta_connected = true;
        } else if (event == WifiEvent::ConfigApStationDisconnected) {
            _sta_connected = false;
        }
    });

    if (!wifi.IsConfigMode()) {
        wifi.StartConfigAp();
    }

    // Create default avatar in background
    auto avatar = std::make_unique<avatar::DefaultAvatar>();
    avatar->init(lv_screen_active(), &lv_font_montserrat_24);
    avatar->leftEye().setVisible(false);
    avatar->rightEye().setVisible(false);
    avatar->mouth().setVisible(false);
    GetStackChan().attachAvatar(std::move(avatar));
}

WifiSetupWorker::~WifiSetupWorker()
{
    auto& wifi = WifiManager::GetInstance();
    if (wifi.IsConfigMode()) {
        wifi.StopConfigAp();
    }
    GetStackChan().resetAvatar();
}

void WifiSetupWorker::update()
{
    cleanup_ui();
    update_state();
}

void WifiSetupWorker::update_state()
{
    switch (_state) {
        case State::ConfigAp: {
            if (_is_first_in) {
                _is_first_in = false;

                auto& data = _state_config_ap_data;

                data.panel = std::make_unique<Container>(lv_screen_active());
                data.panel->setBgColor(lv_color_hex(0xEDF4FF));
                data.panel->align(LV_ALIGN_CENTER, 0, 0);
                data.panel->setBorderWidth(0);
                data.panel->setSize(320, 240);
                data.panel->setRadius(0);

                data.title = std::make_unique<Label>(lv_screen_active());
                data.title->setTextFont(&lv_font_montserrat_20);
                data.title->setTextColor(lv_color_hex(0x26206A));
                data.title->align(LV_ALIGN_TOP_MID, 0, 6);
                data.title->setText("Wi-Fi Setup");

                // 現在設定されているWi-Fi（接続先）があれば分かりやすく表示
                std::string current_saved_ssid = "";
                const auto& saved_list = SsidManager::GetInstance().GetSsidList();
                if (!saved_list.empty()) {
                    current_saved_ssid = saved_list[0].ssid;
                }

                if (!current_saved_ssid.empty()) {
                    data.current_wifi_label = std::make_unique<Label>(lv_screen_active());
                    data.current_wifi_label->setTextFont(&lv_font_montserrat_14);
                    data.current_wifi_label->setTextColor(lv_color_hex(0x4A6FA5));
                    data.current_wifi_label->align(LV_ALIGN_TOP_MID, 0, 24);
                    data.current_wifi_label->setText(fmt::format("Current: [{}]", current_saved_ssid));
                }

                data.subtitle = std::make_unique<Label>(lv_screen_active());
                data.subtitle->setTextFont(&lv_font_montserrat_14);
                data.subtitle->setTextColor(lv_color_hex(0x7E7B9C));
                data.subtitle->align(LV_ALIGN_TOP_MID, 0, current_saved_ssid.empty() ? 26 : 42);
                data.subtitle->setText("Scan with iPhone camera");

                std::string ap_ssid = WifiManager::GetInstance().GetApSsid();
                if (ap_ssid.empty()) {
                    ap_ssid = "StackChan-Setup";
                }
                std::string qrcode_payload = fmt::format("WIFI:S:{};T:nopass;;", ap_ssid);

                data.qrcode = std::make_unique<Qrcode>(lv_screen_active());
                data.qrcode->setSize(96);
                data.qrcode->setDarkColor(lv_color_hex(0x221C5B));
                data.qrcode->setLightColor(lv_color_hex(0xFFFFFF));
                data.qrcode->update(qrcode_payload);
                data.qrcode->align(LV_ALIGN_CENTER, 0, 2);

                data.ssid_label = std::make_unique<Label>(lv_screen_active());
                data.ssid_label->setTextFont(&lv_font_montserrat_14);
                data.ssid_label->setTextColor(lv_color_hex(0x525064));
                data.ssid_label->align(LV_ALIGN_CENTER, 0, 62);
                data.ssid_label->setText(fmt::format("AP: {}", ap_ssid));

                data.btn_back = std::make_unique<Button>(lv_screen_active());
                apply_button_common_style(*data.btn_back);
                data.btn_back->setSize(90, 28);
                data.btn_back->align(LV_ALIGN_CENTER, 0, 94);
                data.btn_back->setBgColor(lv_color_hex(0xD4D9E0));
                data.btn_back->label().setText("Cancel");
                data.btn_back->label().setTextColor(lv_color_hex(0x525064));
                data.btn_back->label().setTextFont(&lv_font_montserrat_14);
                data.btn_back->onClick().connect([this]() { _state_config_ap_data.back_clicked = true; });
            }

            if (_state_config_ap_data.back_clicked) {
                _is_done = true;
            }

            // If a client (iPhone) connected to the SoftAP, transition to loading state
            if (_sta_connected) {
                switch_state(State::DeviceConnected);
                break;
            }

            // Check if Wi-Fi setup succeeded
            // (When AP mode completes or Wi-Fi connected or SSID has been saved to NVS)
            auto& wifi     = WifiManager::GetInstance();
            auto& ssid_mgr = SsidManager::GetInstance();
            if (!ssid_mgr.GetSsidList().empty() && (!wifi.IsConfigMode() || wifi.IsConnected())) {
                switch_state(State::Done);
            }

            break;
        }
        case State::DeviceConnected: {
            if (_is_first_in) {
                _is_first_in = false;

                auto& data = _state_device_connected_data;

                data.title = std::make_unique<Label>(lv_screen_active());
                data.title->setTextFont(&lv_font_montserrat_20);
                data.title->setTextColor(lv_color_hex(0x26206A));
                data.title->align(LV_ALIGN_TOP_MID, 0, 16);
                data.title->setText("Phone Connected!");

                data.subtitle = std::make_unique<Label>(lv_screen_active());
                data.subtitle->setTextFont(&lv_font_montserrat_14);
                data.subtitle->setTextColor(lv_color_hex(0x7E7B9C));
                data.subtitle->align(LV_ALIGN_TOP_MID, 0, 44);
                data.subtitle->setText("Opening setup page...");

                data.spinner = std::make_unique<Spinner>(lv_screen_active());
                data.spinner->setSize(64, 64);
                data.spinner->align(LV_ALIGN_CENTER, 0, 8);
                data.spinner->setAnimParams(1000, 60);

                data.tip = std::make_unique<Label>(lv_screen_active());
                data.tip->setTextFont(&lv_font_montserrat_14);
                data.tip->setTextColor(lv_color_hex(0x26206A));
                data.tip->align(LV_ALIGN_BOTTOM_MID, 0, -20);
                data.tip->setText("Please select Wi-Fi on phone");
            }

            // If client disconnected without configuring, go back to QR code
            if (!_sta_connected) {
                switch_state(State::ConfigAp);
                break;
            }

            // Check if Wi-Fi setup succeeded
            auto& wifi     = WifiManager::GetInstance();
            auto& ssid_mgr = SsidManager::GetInstance();
            if (!ssid_mgr.GetSsidList().empty() && (!wifi.IsConfigMode() || wifi.IsConnected())) {
                switch_state(State::Done);
            }

            break;
        }
        case State::Done: {
            if (_is_first_in) {
                _is_first_in = false;

                auto& avatar = GetStackChan().avatar();
                avatar.leftEye().setVisible(true);
                avatar.rightEye().setVisible(true);
                avatar.mouth().setVisible(true);
                avatar.setEmotion(avatar::Emotion::Happy);
                avatar.setSpeech("Connected!\nRebooting...");

                GetStackChan().addModifier(std::make_unique<SpeakingModifier>(1500, 180, false));

                _state_done_data.reboot_count = 3;
                _last_tick                   = GetHAL().millis();
            }

            if (GetHAL().millis() - _last_tick > 1000) {
                _last_tick = GetHAL().millis();
                if (_state_done_data.reboot_count > 0) {
                    _state_done_data.reboot_count--;
                    auto& avatar = GetStackChan().avatar();
                    avatar.setSpeech(fmt::format("Done! Reboot in {}s", _state_done_data.reboot_count));
                } else {
                    mclog::tagInfo(_tag, "Wi-Fi configured, rebooting...");
                    GetHAL().delay(100);
                    GetHAL().reboot();
                }
            }

            break;
        }
        default:
            break;
    }
}

void WifiSetupWorker::cleanup_ui()
{
    if (_last_state == State::None) {
        return;
    }

    switch (_last_state) {
        case State::ConfigAp: {
            _state_config_ap_data.reset();
            break;
        }
        case State::DeviceConnected: {
            _state_device_connected_data.reset();
            break;
        }
        case State::Done: {
            break;
        }
        default:
            break;
    }

    _last_state = State::None;
}

void WifiSetupWorker::switch_state(State newState)
{
    _last_state  = _state;
    _state       = newState;
    _is_first_in = true;
}
