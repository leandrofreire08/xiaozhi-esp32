#include "wifi_board.h"
#include <wifi_manager.h>                        // Fork: gate weather fetch on live connection
#include <esp_sntp.h>                            // Fork: SNTP clock (no OTA/activation server)
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "lvgl_theme.h"                        // Fork: LvglTheme::set_emoji_collection
#include "avatar/avatar_emoji_collection.h"    // Fork: generated custom avatar emojis
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_gc9a01.h>
#include "system_reset.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <esp_timer.h>
#include "i2c_device.h"
#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "power_save_timer.h"
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_manager.h"
#include "device_state.h"
#include "settings.h"
#include <cJSON.h>
#include <cmath>
#include <string>

#define TAG "Spotpear_ESP32_S3_1_28_BOX"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_16_4);


class Cst816d : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };
    Cst816d(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        uint8_t chip_id = ReadReg(0xA3);
        ESP_LOGI(TAG, "Get chip ID: 0x%02X", chip_id);
        last_chip_id_ = chip_id;
        read_buffer_ = new uint8_t[6];
    }

    ~Cst816d() {
        if (read_buffer_) {
            delete[] read_buffer_;
            read_buffer_ = nullptr;
        }
    }

    void UpdateTouchPoint() {
        if (!read_buffer_) return;
        ReadRegs(0x02, read_buffer_, 6);
        if (read_buffer_[0] == 0xFF) {
            read_buffer_[0] = 0x00;
        }
        tp_.num = read_buffer_[0] & 0x01;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
    }

    const TouchPoint_t& GetTouchPoint() const {
        return tp_;
    }

    static bool Probe(i2c_master_bus_handle_t i2c_bus, uint8_t addr, uint8_t& chip_id) {
        if (!i2c_bus) return false;
        i2c_master_dev_handle_t dev = nullptr;
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 400 * 1000,
            .scl_wait_us = 0,
            .flags = {
                .disable_ack_check = 0,
            },
        };
        esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &cfg, &dev);
        if (ret != ESP_OK || dev == nullptr) {
            return false;
        }
        uint8_t reg = 0xA3;
        uint8_t id = 0;
        ret = i2c_master_transmit_receive(dev, &reg, 1, &id, 1, 100);
        i2c_master_bus_rm_device(dev);
        if (ret == ESP_OK) {
            chip_id = id;
            return true;
        }
        return false;
    }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;
    uint8_t last_chip_id_ = 0;
};


class CustomLcdDisplay : public SpiLcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                    esp_lcd_panel_handle_t panel_handle,
                    int width,
                    int height,
                    int offset_x,
                    int offset_y,
                    bool mirror_x,
                    bool mirror_y,
                    bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    // Fork (if-my-hermes-speak): Dashboard + reactive orb. Supersedes the avatar
    // *rendering* — 3 stacked bands on the round 240 panel: clock/date (top),
    // orb pill (middle), temp·wifi·date (bottom). The orb encodes the turn state
    // by base color + motion, tinted by emotion while responding.
    // See .claude/plans/plan-request-linked-lobster.md.
    enum OrbState { ORB_IDLE, ORB_LISTENING, ORB_PROCESSING, ORB_RESPONDING, ORB_ERROR };

    virtual void SetupUI() override {
        // Parent builds all base objects (container_, emoji_box_, bars, chat).
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);

        // Retire the avatar + default bars — the dashboard owns the round screen.
        if (emoji_box_ != nullptr)  lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        if (top_bar_ != nullptr)    lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_ != nullptr) lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        SetHideSubtitle(true);  // keep bottom_bar_ chat hidden

        lv_obj_t* screen = lv_screen_active();
        auto icon_font = static_cast<LvglTheme*>(current_theme_)->icon_font()->font();

        ForceBlackBackground();

        // --- Middle band: the orb (radial-gradient pill) ---
        orb_ = lv_obj_create(screen);
        lv_obj_remove_style_all(orb_);
        lv_obj_remove_flag(orb_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(orb_, kOrbW, kOrbH);
        lv_obj_align(orb_, LV_ALIGN_CENTER, 0, kOrbY);
        lv_obj_set_style_radius(orb_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_transform_pivot_x(orb_, kOrbW / 2, 0);
        lv_obj_set_style_transform_pivot_y(orb_, kOrbH / 2, 0);
        // Radial gradient: bright core at center -> transparent at the right edge.
        // The descriptor must outlive the call (style stores a pointer) -> member.
        lv_grad_radial_init(&orb_grad_, kOrbW / 2, kOrbH / 2, kOrbW, kOrbH / 2,
                            LV_GRAD_EXTEND_PAD);

        // --- Top band: big clock + weekday ---
        time_label_ = lv_label_create(screen);
        lv_obj_set_style_text_font(time_label_, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(time_label_, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(time_label_, "--:--");
        lv_obj_align(time_label_, LV_ALIGN_TOP_MID, 0, 26);

        date_label_ = lv_label_create(screen);
        lv_obj_set_style_text_font(date_label_, &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(date_label_, lv_color_hex(0x8EA1B8), 0);
        lv_label_set_text(date_label_, "");
        lv_obj_align(date_label_, LV_ALIGN_TOP_MID, 0, 82);

        // Battery indicator — top center, above the clock. Mirrors the base-
        // computed battery glyph (from Board::GetBatteryLevel) in UpdateStatusBar.
        battery_top_ = lv_label_create(screen);
        lv_obj_set_style_text_font(battery_top_, icon_font, 0);
        lv_obj_set_style_text_color(battery_top_, lv_color_hex(0xC7D3E0), 0);
        lv_label_set_text(battery_top_, "");
        lv_obj_align(battery_top_, LV_ALIGN_TOP_MID, 0, 6);

        // --- Bottom band: temp · wifi · short date ---
        lv_obj_t* row = lv_obj_create(screen);
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, LV_HOR_RES, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -34);

        temp_label_ = lv_label_create(row);
        lv_obj_set_style_text_font(temp_label_, &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(temp_label_, lv_color_hex(0xC7D3E0), 0);
        lv_label_set_text(temp_label_, "--\xC2\xB0");   // "--°" (UTF-8 degree)

        wifi_label_ = lv_label_create(row);
        lv_obj_set_style_text_font(wifi_label_, icon_font, 0);
        lv_obj_set_style_text_color(wifi_label_, lv_color_hex(0x8EA1B8), 0);
        // The 16px icon glyph reads taller than the 16px temp digits — scale it
        // down to match. Tune kWifiScale (256 = 100%).
        lv_obj_set_style_transform_pivot_x(wifi_label_, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(wifi_label_, LV_PCT(50), 0);
        lv_obj_set_style_transform_scale(wifi_label_, kWifiScale, 0);
        lv_label_set_text(wifi_label_, "");

        SetBootSplash(true);  // boot: only the hue-cycling orb until network + clock are ready
    }

    // Assets::Apply() calls SetTheme(current_theme) after boot (the "light" theme
    // from NVS repaints the panel white right after the greeting cue). Re-force
    // black after every theme repaint so the dashboard stays black-on-black.
    virtual void SetTheme(Theme* theme) override {
        SpiLcdDisplay::SetTheme(theme);
        DisplayLockGuard lock(this);
        ForceBlackBackground();
    }

    // Orb reacts to emotion (`llm` message). While speaking -> tint the green;
    // otherwise this is the "processing" cue (no native device state for it).
    virtual void SetEmotion(const char* emotion) override {
        DisplayLockGuard lock(this);
        last_emotion_ = emotion ? emotion : "neutral";
        if (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
            ApplyResponseTint();
        } else {
            SetOrbState(ORB_PROCESSING);
        }
    }

    // Per-second heartbeat (application.cc clock tick): refresh clock/date/wifi
    // and re-sync the orb to the device state.
    virtual void UpdateStatusBar(bool update_all = false) override {
        SpiLcdDisplay::UpdateStatusBar(update_all);  // battery/mute (hidden) upkeep
        DisplayLockGuard lock(this);

        // WiFi-config screen owns the panel while provisioning; the dashboard
        // restores itself once the state leaves kDeviceStateWifiConfiguring.
        bool configuring =
            Application::GetInstance().GetDeviceState() == kDeviceStateWifiConfiguring;
        if (configuring != config_mode_) SetConfigMode(configuring);
        if (configuring) { RefreshConfigSsid(); return; }

        // Boot splash owns the panel (only the hue-cycling orb) until the clock is
        // valid — SNTP needs connectivity, so a real year means we reached the
        // network and have data to show. Then reveal the dashboard.
        if (booting_) {
            time_t bnow = time(NULL);
            struct tm* btm = localtime(&bnow);
            if (btm != nullptr && btm->tm_year >= 2025 - 1900) SetBootSplash(false);
            else return;
        }

        time_t now = time(NULL);
        struct tm* tm = localtime(&now);
        if (tm != nullptr && tm->tm_year >= 2025 - 1900 && time_label_ != nullptr) {
            char buf[16];
            strftime(buf, sizeof(buf), "%H:%M", tm);
            lv_label_set_text(time_label_, buf);
            // pt-BR names — newlib has no pt_BR locale, so map by hand.
            static const char* kWd[7]  = {"Dom","Seg","Ter","Qua","Qui","Sex","S\xC3\xA1""b"};
            static const char* kMon[12] = {"Jan","Fev","Mar","Abr","Mai","Jun",
                                           "Jul","Ago","Set","Out","Nov","Dez"};
            char dbuf[24];
            snprintf(dbuf, sizeof(dbuf), "%s %02d %s",
                     kWd[tm->tm_wday % 7], tm->tm_mday, kMon[tm->tm_mon % 12]);
            lv_label_set_text(date_label_, dbuf);
        }
        // Mirror the base-computed wifi/network icon into the bottom row.
        if (wifi_label_ != nullptr && network_label_ != nullptr) {
            lv_label_set_text(wifi_label_, lv_label_get_text(network_label_));
        }
        // Mirror the base-computed battery glyph into the top-center indicator.
        if (battery_top_ != nullptr && battery_label_ != nullptr) {
            lv_label_set_text(battery_top_, lv_label_get_text(battery_label_));
        }
        SyncOrbToDeviceState();
    }

    // Weather task pushes the temperature here (thread-safe via lock).
    void SetTemperature(float celsius, bool valid) {
        DisplayLockGuard lock(this);
        if (temp_label_ == nullptr || !valid) return;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)lroundf(celsius));
        lv_label_set_text(temp_label_, buf);
    }

private:
    // Paint the whole panel black regardless of the active theme: screen +
    // container_ opaque black, content_ (chat area) transparent so it shows
    // through. Idempotent — safe to call from SetupUI and after every SetTheme.
    void ForceBlackBackground() {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        if (container_ != nullptr) {
            lv_obj_set_style_bg_color(container_, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
        }
        if (content_ != nullptr) {
            lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
        }
    }

    static void SetHidden(lv_obj_t* o, bool hide) {
        if (o == nullptr) return;
        if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }

    // WiFi-config screen. Reuses the pulsing purple orb (ORB_IDLE) on the black
    // background (ForceBlackBackground) and swaps the clock/weather dashboard for
    // "Conecte-se a" + the AP SSID (HermesOrb-XXXX) + "192.168.4.1".
    void BuildConfigUI() {
        lv_obj_t* screen = lv_screen_active();

        cfg_title_ = lv_label_create(screen);
        lv_obj_set_style_text_font(cfg_title_, &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(cfg_title_, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(cfg_title_, "Conecte-se a");
        lv_obj_align(cfg_title_, LV_ALIGN_CENTER, 0, -82);

        cfg_ssid_ = lv_label_create(screen);
        lv_obj_set_style_text_font(cfg_ssid_, &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(cfg_ssid_, lv_color_hex(0xA24BFF), 0);  // orb accent
        lv_label_set_text(cfg_ssid_, "HermesOrb");
        lv_obj_align(cfg_ssid_, LV_ALIGN_CENTER, 0, 78);

        cfg_ip_ = lv_label_create(screen);
        lv_obj_set_style_text_font(cfg_ip_, &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(cfg_ip_, lv_color_hex(0x8EA1B8), 0);
        lv_label_set_text(cfg_ip_, "192.168.4.1");
        lv_obj_align(cfg_ip_, LV_ALIGN_CENTER, 0, 100);
    }

    // Toggle the dashboard chrome off / config text on (the orb is shared and stays).
    void SetConfigMode(bool on) {
        config_mode_ = on;
        if (on && cfg_title_ == nullptr) BuildConfigUI();
        if (on) { booting_ = false; StopBootHue(); }  // config owns the orb (steady purple)
        SetDashboardChromeHidden(on);
        SetHidden(cfg_title_, !on);
        SetHidden(cfg_ssid_, !on);
        SetHidden(cfg_ip_, !on);
        if (on) {
            RefreshConfigSsid();
            SetOrbState(ORB_IDLE);  // pulsing purple during provisioning
        }
    }

    // The AP SSID is only known once StartConfigAp() runs; refresh every tick so
    // the label self-heals if the first tick raced ahead of the AP coming up.
    void RefreshConfigSsid() {
        if (cfg_ssid_ == nullptr) return;
        std::string ssid = WifiManager::GetInstance().GetApSsid();
        if (!ssid.empty() && ssid != lv_label_get_text(cfg_ssid_)) {
            lv_label_set_text(cfg_ssid_, ssid.c_str());
        }
    }

    // --- Boot splash -------------------------------------------------------
    // At boot the dashboard would show empty placeholders (--:--, --deg, no wifi).
    // Instead show only the orb — breathing and cycling hue — until the clock is
    // valid (=> network reached + data ready), then reveal the dashboard.
    void SetDashboardChromeHidden(bool hide) {
        SetHidden(time_label_, hide);
        SetHidden(date_label_, hide);
        SetHidden(battery_top_, hide);
        SetHidden(temp_label_, hide);   // bottom row has no bg -> hiding its
        SetHidden(wifi_label_, hide);   // labels makes the row invisible
    }

    void SetBootSplash(bool on) {
        booting_ = on;
        if (on) {
            SetDashboardChromeHidden(true);
            SetOrbState(ORB_LISTENING);  // medium breathing; hue timer recolors it
            if (boot_hue_timer_ == nullptr)
                boot_hue_timer_ = lv_timer_create(BootHueCb, 40, this);
        } else {
            StopBootHue();  // UpdateStatusBar + SyncOrbToDeviceState take the orb back
            RevealDashboard();
        }
    }

    // Unhide all dashboard chrome and fade it in together (opa 0 -> cover). The
    // caller populates real text the same tick, so the fade shows live values.
    void RevealDashboard() {
        lv_obj_t* els[] = { time_label_, date_label_, battery_top_,
                            temp_label_, wifi_label_ };
        for (lv_obj_t* e : els) {
            if (e == nullptr) continue;
            lv_obj_remove_flag(e, LV_OBJ_FLAG_HIDDEN);
            lv_obj_fade_in(e, 600, 0);  // all delay 0 -> appear simultaneously
        }
    }

    void StopBootHue() {
        if (boot_hue_timer_ != nullptr) {
            lv_timer_delete(boot_hue_timer_);
            boot_hue_timer_ = nullptr;
        }
    }

    // Smooth rainbow sweep while booting (runs on the LVGL task via lv_timer).
    static void BootHueCb(lv_timer_t* t) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(t));
        if (self->orb_ == nullptr) return;
        self->boot_hue_ = (self->boot_hue_ + 3) % 360;
        lv_color_t c = lv_color_hsv_to_rgb(self->boot_hue_, 85, 100);
        uint32_t hex = ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | c.blue;
        self->ApplyOrbColor(hex);
    }

    // Orb geometry — tune here. Equal W/H + LV_RADIUS_CIRCLE = a round orb.
    // Widen kOrbW past kOrbH to go back toward the old elongated pill.
    static constexpr int kOrbW = 96;
    static constexpr int kOrbH = 96;
    static constexpr int kOrbY = 4;

    // Wifi icon scale (256 = 100%). ~0.78 makes the 16px glyph match the temp.
    static constexpr int kWifiScale = 200;

    lv_obj_t* orb_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* temp_label_ = nullptr;
    lv_obj_t* wifi_label_ = nullptr;
    lv_obj_t* battery_top_ = nullptr;
    lv_grad_dsc_t orb_grad_{};
    OrbState orb_state_ = ORB_IDLE;
    std::string last_emotion_ = "neutral";

    // WiFi-config screen widgets (lazy-built on first entry).
    lv_obj_t* cfg_title_ = nullptr;
    lv_obj_t* cfg_ssid_ = nullptr;
    lv_obj_t* cfg_ip_ = nullptr;
    bool config_mode_ = false;

    // Boot splash: only the hue-cycling orb until the network + clock are ready.
    bool booting_ = true;
    lv_timer_t* boot_hue_timer_ = nullptr;
    uint16_t boot_hue_ = 0;

    static uint32_t StateCore(OrbState s) {
        switch (s) {
            case ORB_IDLE:       return 0xA24BFF;  // purple
            case ORB_LISTENING:  return 0x38D6FF;  // cyan
            case ORB_PROCESSING: return 0x2B7FD6;  // blue
            case ORB_RESPONDING: return 0x2FD6A0;  // green
            case ORB_ERROR:      return 0xFF4D4D;  // red
        }
        return 0xA24BFF;
    }

    // Paint the orb gradient a given core color (bright center -> transparent edge).
    void ApplyOrbColor(uint32_t core) {
        if (orb_ == nullptr) return;
        lv_color_t c = lv_color_hex(core);
        orb_grad_.stops[0].color = c;
        orb_grad_.stops[0].opa   = LV_OPA_COVER;
        orb_grad_.stops[0].frac  = 0;
        orb_grad_.stops[1].color = c;
        orb_grad_.stops[1].opa   = LV_OPA_TRANSP;
        orb_grad_.stops[1].frac  = 255;
        orb_grad_.stops_count = 2;
        lv_obj_set_style_bg_color(orb_, c, 0);
        lv_obj_set_style_bg_grad(orb_, &orb_grad_, 0);
        lv_obj_set_style_bg_opa(orb_, LV_OPA_COVER, 0);
        lv_obj_invalidate(orb_);  // same grad pointer -> force a repaint on color change
    }

    // Set base color + (re)start the per-state motion animation.
    void SetOrbState(OrbState s) {
        if (orb_ == nullptr) return;
        orb_state_ = s;
        ApplyOrbColor(StateCore(s));

        lv_anim_delete(this, OrbAnimCb);  // clear prior motion
        int dur;
        switch (s) {
            case ORB_IDLE:       dur = 2200; break;
            case ORB_LISTENING:  dur = 1000; break;
            case ORB_PROCESSING: dur = 450;  break;
            case ORB_RESPONDING: dur = 320;  break;
            case ORB_ERROR:      dur = 220;  break;
            default:             dur = 1000; break;
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, this);
        lv_anim_set_exec_cb(&a, OrbAnimCb);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_duration(&a, dur);
        lv_anim_set_reverse_duration(&a, dur);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }

    // t in [0,1000] -> opacity + scale, shaped per state.
    static void OrbAnimCb(void* var, int32_t t) {
        auto* self = static_cast<CustomLcdDisplay*>(var);
        if (self->orb_ == nullptr) return;
        int32_t opa, scale;
        switch (self->orb_state_) {
            case ORB_IDLE:       opa = 60 + t * 120 / 1000; scale = 250 + t * 16 / 1000; break;
            case ORB_LISTENING:  opa = 120 + t * 110 / 1000; scale = 250 + t * 24 / 1000; break;
            case ORB_PROCESSING: opa = 150 + t * 105 / 1000; scale = 248 + t * 40 / 1000; break;
            case ORB_RESPONDING: opa = 150 + t * 105 / 1000; scale = 245 + t * 55 / 1000; break;
            case ORB_ERROR:      opa = 90 + t * 165 / 1000; scale = 256; break;
            default:             opa = 200; scale = 256; break;
        }
        lv_obj_set_style_opa(self->orb_, (lv_opa_t)opa, 0);
        lv_obj_set_style_transform_scale_x(self->orb_, scale, 0);
        lv_obj_set_style_transform_scale_y(self->orb_, scale, 0);
    }

    // Map device state -> orb base state. `processing` is sticky over listening
    // (it has no native device state; entered from SetEmotion, held until speak/idle).
    void SyncOrbToDeviceState() {
        DeviceState ds = Application::GetInstance().GetDeviceState();
        OrbState want;
        switch (ds) {
            case kDeviceStateSpeaking:   want = ORB_RESPONDING; break;
            case kDeviceStateListening:
            case kDeviceStateConnecting:
                want = (orb_state_ == ORB_PROCESSING) ? ORB_PROCESSING : ORB_LISTENING;
                break;
            case kDeviceStateFatalError: want = ORB_ERROR; break;
            case kDeviceStateIdle:       want = ORB_IDLE; break;
            default:                     return;  // starting/upgrading/etc: leave as-is
        }
        if (want == ORB_RESPONDING && orb_state_ == ORB_RESPONDING) {
            ApplyResponseTint();  // keep the emotion tint fresh while speaking
            return;
        }
        if (want != orb_state_) SetOrbState(want);
    }

    // Emotion nudges the responding-green (small category table; no sentiment inference).
    void ApplyResponseTint() {
        uint32_t c = 0x2FD6A0;  // base green
        const std::string& e = last_emotion_;
        if (e == "angry")                                   c = 0xE06B2F;  // hot override
        else if (e == "shocked" || e == "surprised")        c = 0x8AFFE0;  // bright flash
        else if (e == "sad" || e == "crying" || e == "sleepy") c = 0x1E8F6E;  // dim
        else if (e == "happy" || e == "laughing" || e == "funny" || e == "loving" ||
                 e == "delicious" || e == "confident" || e == "cool" || e == "relaxed" ||
                 e == "kissy" || e == "winking" || e == "silly" || e == "embarrassed")
            c = 0x5CF0C0;  // warm/bright
        ApplyOrbColor(c);
    }
};


class Spotpear_ESP32_S3_1_28_BOX : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button boot_button_;
    Display* display_ = nullptr;
    esp_timer_handle_t touchpad_timer_ = nullptr;
    Cst816d* cst816d_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    PowerManager* power_manager_ = nullptr;

    void InitializePowerSaveTimer() {
        rtc_gpio_init(GPIO_NUM_3);
        rtc_gpio_set_direction(GPIO_NUM_3, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(GPIO_NUM_3, 1);

        power_save_timer_ = new PowerSaveTimer(-1, 60, 290);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
            // 关闭ES8311音频编解码器
            auto codec = GetAudioCodec();
            if (codec) {
                codec->EnableInput(false);
                codec->EnableOutput(false);
            }
            rtc_gpio_set_level(GPIO_NUM_3, 0);
            // 启用保持功能，确保睡眠期间电平不变
            rtc_gpio_hold_en(GPIO_NUM_3);
            esp_lcd_panel_disp_on_off(panel_, false); //关闭显示
            esp_deep_sleep_start();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializePowerManager() {
        power_manager_ = new PowerManager(BATTERY_CHARGING_PIN, ADC_CHANNEL_0);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            // .glitch_ignore_cnt = 7,
            // .intr_priority = 0,
            // .trans_queue_depth = 0,
            // .flags = {
            //     .enable_internal_pullup = 1,
            // },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeCodecI2c_Touch() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = TP_PIN_NUM_TP_SDA,
            .scl_io_num = TP_PIN_NUM_TP_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        esp_err_t ret = i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
            i2c_bus_ = nullptr;
        }
    }


    static void touchpad_timer_callback(void* arg) {
        auto* board = static_cast<Spotpear_ESP32_S3_1_28_BOX*>(arg);
        if (!board || !board->cst816d_) return;
        static bool was_touched = false;
        static int64_t touch_start_time = 0;
        const int64_t TOUCH_THRESHOLD_MS = 500;  // 触摸时长阈值，超过500ms视为长按

        board->cst816d_->UpdateTouchPoint();
        auto touch_point = board->cst816d_->GetTouchPoint();

        // 检测触摸开始
        if (touch_point.num > 0 && !was_touched) {
            was_touched = true;
            touch_start_time = esp_timer_get_time() / 1000; // 转换为毫秒
        }
        // 检测触摸释放
        else if (touch_point.num == 0 && was_touched) {
            was_touched = false;
            int64_t touch_duration = (esp_timer_get_time() / 1000) - touch_start_time;

            // 只有短触才触发
            if (touch_duration < TOUCH_THRESHOLD_MS) {
                auto& app = Application::GetInstance();
                // During startup (before connected), pressing touch enters Wi-Fi config mode without reboot
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    board->EnterWifiConfigMode();
                    return;
                }
                app.ToggleChatState();
            }
        }
    }

    void InitializeCst816DTouchPad() {
        ESP_LOGI(TAG, "Init Cst816D");

        // RST/INT 管脚初始化
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << TP_PIN_NUM_TP_RST);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);

        gpio_config_t int_conf = {};
        int_conf.intr_type = GPIO_INTR_DISABLE;
        int_conf.mode = GPIO_MODE_INPUT;
        int_conf.pin_bit_mask = (1ULL << TP_PIN_NUM_TP_INT);
        int_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        int_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&int_conf);

        // 触摸芯片复位序列
        gpio_set_level(TP_PIN_NUM_TP_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(TP_PIN_NUM_TP_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(50));

        // 探测是否存在触摸芯片
        uint8_t chip_id = 0;
        if (!i2c_bus_) {
            ESP_LOGW(TAG, "Touch I2C bus not initialized, skip touch");
            return;
        }
        bool touch_available = Cst816d::Probe(i2c_bus_, 0x15, chip_id);
        if (!touch_available) {
            ESP_LOGW(TAG, "CST816D not found, running in non-touch mode");
            // 释放触摸I2C，避免无设备时反复报错
            i2c_del_master_bus(i2c_bus_);
            i2c_bus_ = nullptr;
            return;
        }

        cst816d_ = new Cst816d(i2c_bus_, 0x15);

        // 创建定时器，10ms 间隔
        esp_timer_create_args_t timer_args = {
            .callback = touchpad_timer_callback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "touchpad_timer",
            .skip_unhandled_events = true,
        };

        if (esp_timer_create(&timer_args, &touchpad_timer_) == ESP_OK) {
            esp_timer_start_periodic(touchpad_timer_, 10 * 1000); // 10ms = 10000us
        }
    }

    // SPI初始化
    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SPI bus");
        spi_bus_config_t buscfg = GC9A01_PANEL_BUS_SPI_CONFIG(DISPLAY_SPI_SCLK_PIN, DISPLAY_SPI_MOSI_PIN,
                                    DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    // GC9A01初始化
    void InitializeGc9a01Display() {
        ESP_LOGI(TAG, "Init GC9A01 display");
        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_handle_t io_handle = NULL;
        esp_lcd_panel_io_spi_config_t io_config = GC9A01_PANEL_IO_SPI_CONFIG(DISPLAY_SPI_CS_PIN, DISPLAY_SPI_DC_PIN, 0, NULL);
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io_handle));

        ESP_LOGI(TAG, "Install GC9A01 panel driver");
        esp_lcd_panel_handle_t panel_handle = NULL;
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_SPI_RESET_PIN;    // Set to -1 if not use
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
        panel_ = panel_handle;
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

        uint8_t data_0x62[] = { 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70 };
        esp_lcd_panel_io_tx_param(io_handle, 0x62, data_0x62, sizeof(data_0x62));

        uint8_t data_0x63[] = { 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70 };
        esp_lcd_panel_io_tx_param(io_handle, 0x63, data_0x63, sizeof(data_0x63));

        uint8_t data_0x36[] = { 0x48};
        esp_lcd_panel_io_tx_param(io_handle, 0x36, data_0x36, sizeof(data_0x36));

        // uint8_t data_0x74[] = { 0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00};
        // esp_lcd_panel_io_tx_param(io_handle, 0x74, data_0x74, sizeof(data_0x74));

        uint8_t data_0xC3[] = { 0x1F};
        esp_lcd_panel_io_tx_param(io_handle, 0xC3, data_0xC3, sizeof(data_0xC3));

        uint8_t data_0xC4[] = { 0x1F};
        esp_lcd_panel_io_tx_param(io_handle, 0xC4, data_0xC4, sizeof(data_0xC4));

        display_ = new CustomLcdDisplay(io_handle, panel_handle,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            // During startup (before connected), pressing BOOT button enters Wi-Fi config mode without reboot
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    Spotpear_ESP32_S3_1_28_BOX() : boot_button_(BOOT_BUTTON_GPIO) {
        // 先初始化触摸的I2C并探测/初始化触摸（若无触摸则跳过）
        InitializeCodecI2c_Touch();
        InitializeCst816DTouchPad();

        // 初始化音频I2C
        InitializeCodecI2c();

        // 显示相关先建立起来
        InitializeSpi();
        InitializeGc9a01Display();
        InitializeButtons();
        if (GetBacklight()) {
            GetBacklight()->RestoreBrightness();
        }

        // 显示和背光可用后再初始化省电逻辑，避免空指针
        InitializePowerSaveTimer();
        InitializePowerManager();

        // Fork (dashboard+orb): firmware-owned weather fetch (decision B).
        StartWeatherTask();
    }

    ~Spotpear_ESP32_S3_1_28_BOX() {
        if (weather_task_) {
            vTaskDelete(weather_task_);
            weather_task_ = nullptr;
        }
        if (touchpad_timer_) {
            esp_timer_stop(touchpad_timer_);
            esp_timer_delete(touchpad_timer_);
            touchpad_timer_ = nullptr;
        }
        if (cst816d_) {
            delete cst816d_;
            cst816d_ = nullptr;
        }
        if (power_save_timer_) {
            delete power_save_timer_;
            power_save_timer_ = nullptr;
        }
        if (power_manager_) {
            delete power_manager_;
            power_manager_ = nullptr;
        }
        if (display_) {
            delete display_;
            display_ = nullptr;
        }
        if (i2c_bus_) {
            i2c_del_master_bus(i2c_bus_);
            i2c_bus_ = nullptr;
        }
        if (codec_i2c_bus_) {
            i2c_del_master_bus(codec_i2c_bus_);
            codec_i2c_bus_ = nullptr;
        }
    }


    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    // Fork (if-my-hermes-speak): install the custom avatar emoji collection LAST, after
    // Assets::Apply() rebuilt the theme collection from the assets partition (which has
    // the 19 static emotions but not our animated speaking/blink). Called from
    // Application::Initialize right after assets.Apply().
    virtual void OnThemeAssetsApplied() override {
        if (display_ == nullptr) return;
        auto theme = static_cast<LvglTheme*>(display_->GetTheme());
        if (theme != nullptr) {
            theme->set_emoji_collection(std::make_shared<AvatarEmojiCollection>());
        }
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    Cst816d* GetTouchpad() {
        return cst816d_;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (!power_manager_) {
            level = 0;
            charging = false;
            discharging = true;
            return false;
        }
        
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

private:
    // --- Fork (dashboard+orb): firmware-owned weather (Open-Meteo, decision B) ---
    // A dedicated task because the Http API is blocking — never on the LVGL thread.
    // Config lives in NVS Settings("weather"): lat, lon, units (C/F), interval_min.
    TaskHandle_t weather_task_ = nullptr;

    void StartWeatherTask() {
        // 8 KB stack: TLS handshake + cJSON need headroom.
        xTaskCreate(WeatherTask, "weather", 8192, this, 3, &weather_task_);
    }

    bool time_sync_started_ = false;

    // SNTP clock. TZ from NVS Settings("weather","tz"); default UTC-3 (Brazil,
    // no DST) — matches the São Paulo weather default. POSIX TZ string.
    void StartTimeSync() {
        Settings settings("weather", false);
        std::string tz = settings.GetString("tz", "<-03>3");
        setenv("TZ", tz.c_str(), 1);
        tzset();
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        ESP_LOGI(TAG, "SNTP started (TZ=%s)", tz.c_str());
    }

    static void WeatherTask(void* arg) {
        auto* self = static_cast<Spotpear_ESP32_S3_1_28_BOX*>(arg);
        for (;;) {
            // getaddrinfo() before the tcpip stack / wifi is up hard-asserts
            // ("Invalid mbox" -> reboot loop), it does NOT return an error — so
            // gate every fetch on a live connection instead of relying on
            // FetchWeatherOnce failing gracefully.
            if (!WifiManager::GetInstance().IsConnected()) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            // No OTA/activation server here, so no server_time -> the clock would
            // stay "--:--". Start SNTP once, on the same wifi-gated path.
            if (!self->time_sync_started_) {
                self->time_sync_started_ = true;
                self->StartTimeSync();
            }
            int interval_min = 15;
            bool ok = self->FetchWeatherOnce(interval_min);
            // Retry fast until the first success, then settle to the configured
            // cadence. Last-known temp stays on screen.
            uint32_t wait_ms = ok ? (uint32_t)interval_min * 60 * 1000 : 30 * 1000;
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        }
    }

    bool FetchWeatherOnce(int& interval_min_out) {
        Settings settings("weather", false);
        std::string lat = settings.GetString("lat", "-23.55");   // default: São Paulo
        std::string lon = settings.GetString("lon", "-46.63");
        std::string units = settings.GetString("units", "C");
        interval_min_out = settings.GetInt("interval_min", 15);
        if (interval_min_out < 1) interval_min_out = 15;

        char url[256];
        snprintf(url, sizeof(url),
                 "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&current=temperature_2m",
                 lat.c_str(), lon.c_str());

        auto network = Board::GetInstance().GetNetwork();
        if (network == nullptr) return false;
        auto http = network->CreateHttp(0);
        if (!http || !http->Open("GET", url)) return false;
        if (http->GetStatusCode() != 200) { http->Close(); return false; }
        std::string body = http->ReadAll();
        http->Close();

        cJSON* root = cJSON_Parse(body.c_str());
        if (root == nullptr) return false;
        bool ok = false;
        cJSON* current = cJSON_GetObjectItem(root, "current");
        if (cJSON_IsObject(current)) {
            cJSON* t = cJSON_GetObjectItem(current, "temperature_2m");
            if (cJSON_IsNumber(t)) {
                float c = (float)t->valuedouble;
                if (units == "F" || units == "f") c = c * 9.0f / 5.0f + 32.0f;
                if (display_ != nullptr) {
                    static_cast<CustomLcdDisplay*>(display_)->SetTemperature(c, true);
                }
                ok = true;
            }
        }
        cJSON_Delete(root);
        return ok;
    }
};

DECLARE_BOARD(Spotpear_ESP32_S3_1_28_BOX);
