// Shared contract between the knob's tasks: the bridge data, the command queue and the link status.
// The LVGL task owns the widgets; the link tasks (BLE, USB, Wi-Fi) fill `shared` under `shared_mux`.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <deque>
#include <vector>
#include "lvgl.h"

LV_FONT_DECLARE(deck_14);
LV_FONT_DECLARE(deck_16);
LV_FONT_DECLARE(deck_bold_16);
LV_FONT_DECLARE(deck_bold_30);

static const uint32_t C_WORKING = 0x4CC3FF, C_ATTENTION = 0xFF7A1A, C_DONE = 0x3DDC84, C_IDLE = 0x5D5B57;
static const uint32_t C_INK = 0xF1EFE9, C_MUTED = 0x8A8780, C_DETAIL = 0xC9C6BF, C_BG = 0x050506;

struct Agent {
  String id, kind, bucket, tool, last, prompt;
  bool chat = false;
  int64_t since = 0;
};

struct Worktree {
  String id, name, repo;
  bool active = false;
  std::vector<Agent> agents;
};

struct QuestionOption {
  String id, label;
};

// GET /api/agent (protocol.md § AgentDetail), trimmed to what the agent page shows.
struct AgentDetail {
  String id, bucket, message;
  std::vector<String> next;
  bool has_question = false, answerable = false;
  String item_id, question;
  int revision = 0;
  std::vector<QuestionOption> options;
};

// A command is its /api/cmd body. `kind` is the body's "cmd" (for retries and the status line).
struct Command {
  String kind, body;
  uint32_t queued_at = 0;
};

enum LinkKind { LINK_NONE, LINK_BLE, LINK_USB, LINK_WIFI };

struct Shared {
  std::vector<Worktree> list;
  uint32_t version = 0;
  AgentDetail detail;
  uint32_t detail_version = 0;
  String want_detail;  // the agent the agent page shows; "" while it is closed
  bool online = false;
  String status = "starting…";
  LinkKind link = LINK_NONE;
  std::deque<Command> commands;
};

extern SemaphoreHandle_t shared_mux;
extern Shared shared;

#define POLL_MS 1000
#define RETRY_FOR_MS 5000  // an undelivered command waits this long; later it would surprise
#define LINK_ALIVE_MS 3000 // a relay link is gone after this long without S or K

// link.cpp
void link_init();
void push_command(JsonDocument &body);           // any /api/cmd body
void push_simple(const char *cmd, const String &id = "", const String &pane = "");
void set_status(bool online, const String &text);
bool apply_state(const String &body, const String &via);
bool apply_detail(const String &body);
void handle_line(const String &line, LinkKind via);  // S / A / D / K lines from a relay
bool pop_command(Command &out);
void drop_stale_commands();
String detail_request();  // "C {cmd:detail,id}" body when want_detail changed, else ""
const char *link_name(LinkKind kind);
uint32_t link_seen_at(LinkKind kind);
void link_seen(LinkKind kind);
bool link_alive(LinkKind kind);

// link_serial.cpp / link_ble.cpp / link_wifi.cpp
void serial_link_start();
void serial_send_line(const String &line);
void ble_link_start();
bool ble_send_line(const String &line);
bool ble_connected();
void ble_forget();
void ble_drop();  // test: end the connection as if the knob went out of range
void wifi_link_start();

// haptics.cpp
void haptic_init();
void haptic_tick();
void haptic_click(int times = 1);
void haptic_buzz(int times = 1);
void haptic_sample();

// settings.cpp
struct Settings {
  uint8_t wifi = 1;           // 0 off · 1 fallback
  bool hide_inactive = true;  // Apex default
  uint8_t hide_completed = 2; // 0 off · 1 one hour · 2 one day (Apex default)
  uint8_t haptics = 3;        // 0 off · 1–3
  uint8_t sound = 2;          // 0 off · 1 quiet · 2 on
  uint8_t brightness = 100;   // 40–100 %
};
extern Settings settings;
void settings_load();
void settings_apply();
void settings_page_create();
void settings_page_show(bool show);
bool settings_page_open();
void settings_page_turn(int step);
void settings_page_tap();
void settings_page_refresh();

// agent_page.cpp
void agent_page_create();
void agent_page_open(const Worktree &w, const Agent &a);
void agent_page_close();
bool agent_page_is_open();
void agent_page_turn(int step);
void agent_page_tap();
void agent_page_tick();  // new detail, timers
String agent_page_debug();

// ui.cpp
void ui_test_enqueue(const String &line);  // "T …" lines over USB: drive the UI from the Mac (tests)
void ui_after_send(const String &agent_id);  // Apex: back to the main page, turned to the next waiting agent
void ui_close_pages();
String ui_title(const Worktree &w);
String ui_meta(const Worktree &w, const Agent *a);
uint32_t color_of(const String &bucket);
lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int y, int width = 236);
void set_text(lv_obj_t *label, const String &text);
