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

// One question of a group (AskUserQuestion with several questions, or multi-select).
struct Question {
  String id, text, header;
  bool multi = false;
  std::vector<QuestionOption> options;
};

// GET /api/agent (protocol.md § AgentDetail), trimmed to what the agent page shows.
struct AgentDetail {
  String id, bucket, message;
  std::vector<String> next;
  bool has_question = false, answerable = false;
  String item_id, question;
  int revision = 0;
  std::vector<QuestionOption> options;
  std::vector<Question> questions;  // every question, when the item has a group
  bool group_answerable = false;    // the knob can answer them all (`answers`)
};

// A command is its /api/cmd body. `kind` is the body's "cmd" (for retries and the status line).
struct Command {
  String kind, body;
  uint32_t queued_at = 0;
};

enum LinkKind { LINK_NONE, LINK_BLE, LINK_USB, LINK_WIFI };

// GET /api/mac via the relay's M line: the Clock and the Mac remote. -1: not known.
struct MacStatus {
  int volume = -1, battery = -1, cpu = -1;
  bool muted = false, charging = false;
  String output;    // the Mac's sound output device
  String source, title;  // what plays: "YouTube" / "Music" / "Spotify" and the track; "" when nothing
  int claude_session = -1, claude_week = -1;     // Claude plan limits, % (-1: not known / no window)
  int session_reset_min = -1, week_reset_min = -1;
  String focus_error;  // the bridge could not run the Focus Shortcut
  String week_severity;     // normal / warning / critical
  String models, breakdown;  // "Fable 0" · "Claude Code 41|Cowork 59" (name, space, percent; | between)
  String mic;               // the Mac's microphone (Mic Mute); "" when not known
  bool mic_muted = false;
  int mic_volume = -1;      // its input level, %
  uint32_t at = 0;  // millis() of the last M line; 0: never
};

// GET /api/day via the relay's Y line: the Agent Day Timeline.
struct DaySpan {
  int16_t from, to;  // minutes of the local day
  bool waiting;      // needs you (else working)
};
struct DayRow {
  String name;
  std::vector<DaySpan> spans;
};
struct DayData {
  int now = -1, waited = 0, worked = 0;
  std::vector<DayRow> rows;
};

// GET /api/prompts via the relay's P line: the Prompt Palette.
struct PromptGroup {
  String name;
  std::vector<String> prompts;
};

// GET /api/prs via the relay's R line: PR Radar.
struct PrItem {
  String key, title, checks, review;  // "owner/repo#12" · ok|fail|run|none · approved|changes|""
  bool mine = true, rerun = false;    // mine: authored (else a review request) · rerun: has a failed run
};

// The Prompt Palette's target: a chat agent and its worktree's title.
struct DeckTarget {
  String title, agent, bucket;
};

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
  MacStatus mac;
  std::deque<String> voice;       // V lines for the relay (dictation), sent before commands
  String transcript, transcript_error;  // the relay's X line
  uint32_t transcript_version = 0;
  DayData day;
  uint32_t day_version = 0;
  std::vector<PromptGroup> prompts;
  uint32_t prompts_version = 0;
  std::vector<PrItem> prs;
  String prs_error;
  bool prs_loaded = false;
  uint32_t prs_version = 0;
};

extern SemaphoreHandle_t shared_mux;
extern String boot_note;
int battery_pin_mv();     // battery.cpp: GPIO1 (the probe)
String battery_report();
void battery_tick();      // LVGL task: a reading every 10 s
int battery_level();      // the knob's battery, %; -1 on USB or not read yet
bool battery_charging();  // on USB (the pin then reads the USB rail, not the battery)
String battery_text();    // "<sym> 92%" / "<bolt> USB": Montserrat labels only
String ui_slowest_note();  // the slowest UI pass so far this run: "180 ms in Claude"  // deck.ino: "boot #3: BROWNOUT (power dipped) after 41 s up"
extern Shared shared;

#define POLL_MS 1000
#define RETRY_FOR_MS 5000  // an undelivered command waits this long; later it would surprise
#define LINK_ALIVE_MS 3000 // a relay link is gone after this long without S or K

// link.cpp
void link_init();
void push_command(JsonDocument &body);           // any /api/cmd body
void push_simple(const char *cmd, const String &id = "", const String &pane = "");
void set_status(bool online, const String &text);
int tz_offset_min();  // the Mac's UTC offset, from the M line, kept in NVS for the Clock offline
void tz_persist();    // LVGL task only: saves a new offset to NVS
void tz_load();       // at boot, before the links start
String status_problem();
void push_voice(const String &line);  // a V line (dictation audio) for the relay  // the status line, only when something is wrong (user: «тільки якщо є помилка»)
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

// wifi_store.cpp — the Wi-Fi networks in NVS (no reflash per place)
#define WIFI_MAX 8
struct WifiNet {
  String ssid, pass;
  bool from_mac = false;  // from the Mac's wifi.json (replaced as a set), else typed on the knob
};
void wifi_store_load();     // at boot
void wifi_store_persist();  // LVGL task only: writes NVS when something changed
std::vector<WifiNet> wifi_store_list();  // + secrets.h's network as the seed
int wifi_store_count();
std::vector<String> wifi_store_urls();   // the bridge's LAN URLs, learnt first, BRIDGE_URL last
void wifi_store_set_mac(const std::vector<WifiNet> &mac, const std::vector<String> &lan);
void wifi_store_add(const String &ssid, const String &pass);
void wifi_store_forget(const String &ssid);
String wifi_store_report();  // never a password
String wifi_band_twin(const String &name);  // "home-5G" → "home" (the 2.4 GHz twin a dual-band router names so); else ""

// link_serial.cpp / link_ble.cpp / link_wifi.cpp
void serial_link_start();
void serial_send_line(const String &line);
void ble_link_start();
bool ble_send_line(const String &line);
bool ble_connected();
void ble_forget();
void ble_drop();  // test: end the connection as if the knob went out of range
void wifi_link_start();
struct WifiScanItem {
  String ssid;
  int rssi;
  bool open;
};
void wifi_request_scan();                          // the Wi-Fi app → the Wi-Fi task
uint32_t wifi_scan_take(std::vector<WifiScanItem> &out);  // the last scan; its version (0: none yet)
void wifi_request_try(const String &ssid, const String &pass);
int wifi_try_result(String &why);                  // 0 pending, 1 joined, -1 failed (why)

// haptics.cpp
void haptic_init();
void haptic_tick();
void haptic_click(int times = 1);
void haptic_buzz(int times = 1);
void haptic_sample();
void haptic_home();  // the hub's own feel: a ramp down landing on a click
String haptic_report();
uint32_t haptic_plays();  // every haptic asked for so far: the hub's "every gesture answers" check

// settings.cpp
struct Settings {
  uint8_t wifi = 1;           // 0 off · 1 fallback
  bool hide_inactive = true;  // Apex default
  uint8_t hide_completed = 2; // 0 off · 1 one hour · 2 one day (Apex default)
  uint8_t haptics = 3;        // 0 off · 1–3
  uint8_t sound = 2;          // 0 off · 1 quiet · 2 on
  uint8_t brightness = 100;   // 40–100 %
  uint8_t saver = 15;         // screensaver after this many seconds idle; 0 off
  uint8_t lang = 0;           // 0 Ukrainian · 1 English (user, 2026-11-16: «вибір мови»)
};
// The UI's words in the chosen language: L("English", "Українська").
inline const char *L(const char *en, const char *ua) { extern Settings settings; return settings.lang ? en : ua; }
extern Settings settings;
void settings_load();
void settings_apply();
void settings_set_haptics(int level);  // 0–3, saved
void settings_page_create();
void settings_page_show(bool show, bool deck_rows = false);  // deck_rows: orca-deck's own page, else the system app
bool settings_page_open();
void settings_page_turn(int step);
void settings_page_tap();
void settings_page_refresh();
void settings_page_hold();  // Connection: a hold of 2 s forgets the paired relay
void settings_tick();       // every frame: the 2 s hold

// agent_page.cpp
void agent_page_create();
void agent_page_open(const Worktree &w, const Agent &a);
void agent_page_close();
bool agent_page_is_open();
void agent_page_turn(int step);
void agent_page_tap();
void agent_page_tick();  // new detail, timers
String agent_page_debug();

// hub.cpp — the app launcher (home screen) and the input router. One app is open at a time, or the hub.
// A new app: one App, one line in hub.cpp's list. Swipe down in any app goes home (the app is never asked).
struct App {
  const char *name;
  const char *icon;                // an LV_SYMBOL_* (Montserrat 20/28 carry them)
  uint32_t color;
  void (*show)();
  void (*hide)();
  String (*subtitle)();            // the live line under the name in the hub
  void (*turn)(int step);
  void (*tap)();
  void (*hold)();
  void (*swipe)(lv_dir_t dir);     // left / right; down never arrives (it is the hub's)
};
extern const App app_deck, app_settings, app_timer, app_clock, app_mac, app_jog, app_wheel, app_focus, app_usage, app_day,
    app_prompts, app_mic, app_prs, app_wifi, app_snake, app_pet;
void hub_create();
void hub_turn(int step);           // the crown: the hub's list or the open app
void hub_home();
bool hub_is_open();
const App *hub_current();          // nullptr while the hub is on screen
bool hub_pressing();               // a finger is on the screen (or a test hold is running)
String hub_report();
void hub_test(const String &verb, const String &arg);  // "T tap|hold|swipe|open|home …"
void hub_tick();
void hub_open(const App *app);
const char *hub_shown_name(const App *app);  // the app's name on screen (Ukrainian)

// globe.cpp — the hub's background
void globe_create(lv_obj_t *parent);
void globe_tick(uint32_t now);
String globe_report();

// buddy.cpp — the screensaver (Clawd)
void buddy_create();
void buddy_tick(uint32_t now);
void buddy_input();               // any touch or crown: the idle timer starts again
bool buddy_is_shown();
void buddy_hide();
void buddy_enable(bool on);       // tests: "T saver off" keeps it away
void buddy_show_now();
String buddy_report();
String claude_mood();
String claude_limits(bool with_reset);  // "5h 34% · wk 79%" (+ " · 1d 2h" to the week's reset); "" when unknown  // the loudest agent state: attention / working / done / idle; sleep without a link


// app_timer.cpp
void timer_create();
void timer_tick();  // every frame, shown or not: it runs in the background
String timer_report();
void timer_focus(bool on);  // Focus Mode: Pomodoro on and started / stopped
uint32_t timer_left_ms();
uint32_t timer_total_ms();
bool timer_running();
bool timer_on_break();

// app_focus.cpp
void focus_create();
void focus_tick();
bool focus_is_on();  // done banners and buzzes stay quiet while on (needs-you still shows)
String focus_report();

// app_usage.cpp
void usage_create();
void usage_tick();
String usage_report();

// app_day.cpp
void day_create();
void day_tick();
String day_report();

// app_wifi.cpp — add a network on the knob: scan, a character wheel, a trial join
void wifi_app_create();
void wifi_app_tick();
String wifi_app_report();
void wifi_app_test_try(const String &ssid);  // tests: join with the stored password, save nothing

// app_prompts.cpp
void prompts_create();
void prompts_tick();
String prompts_report();
void prompts_test_target(const String &needle);  // tests: only the chat agent whose id has `needle`, else none

// app_mic.cpp
lv_obj_t *mic_glyph(lv_obj_t *parent, int size);  // a drawn microphone (the hub's badge too)
void mic_glyph_set(lv_obj_t *glyph, uint32_t color, bool slashed);
void micmute_create();
void micmute_tick();
String micmute_report();

// cc.cpp — Control Center: swipe up (not in Claude), swipe down closes
void cc_create();
void cc_open();
void cc_close(bool quiet = false);  // quiet: the hub goes home itself (its own haptic)
bool cc_is_open();
void cc_turn(int step);
void cc_tap();
void cc_hold();
void cc_tick();
String cc_report();
bool mic_is_muted();    // app_mic.cpp
void mic_toggle();
void focus_toggle();    // app_focus.cpp
void settings_adjust_brightness(int step);  // settings.cpp: ±5 % a step, ±1 % under 10, 1–100, live (not saved)
void settings_save();
String settings_report();
#define BRIGHT_MIN 1
#define BRIGHT_STEP 5

// app_prs.cpp
void prs_create();
void prs_tick();
String prs_report();

// app_clock.cpp
void clock_create();
void clock_tick();
String clock_report();

// app_mac.cpp
void mac_create();
void mac_tick();  // every frame: sends the batched volume, takes the Mac's numbers
String mac_report();

// app_jog.cpp
void jog_create();
void jog_tick();
String jog_report();

// app_wheel.cpp
void wheel_create();
void wheel_tick();
String wheel_report();
void snake_create();
void snake_tick();
String snake_report();
void snake_test_level(int n);
String usb_report();  // usbdiag.cpp

// pet.cpp — Clawd the pet (.work/plans/clawd-pet.md)
enum PetStage { PET_EGG, PET_BABY, PET_ADULT, PET_DEAD };
enum PetStyle { STYLE_PLAIN, STYLE_OWL, STYLE_SHIPPER, STYLE_DJ };
// His life in looks (user, 2026-11-16: «від яйця … до дорослого Clawd … 15-20 варіантів … розмір міняти»):
enum PetPhase { PHASE_EGG, PHASE_EGG_WARM, PHASE_EGG_WOBBLE, PHASE_EGG_CRACK, PHASE_EGG_HATCH, PHASE_NEWBORN, PHASE_BABY,
                PHASE_TODDLER, PHASE_KID, PHASE_TEEN, PHASE_ADULT, PHASE_ELDER, PHASE_LEGEND, PHASE_GRAVE };
enum PetEvent { PET_AGENT_WORKING, PET_AGENT_ATTENTION, PET_AGENT_DONE, PET_SENT, PET_PR_FAIL, PET_SNAKE_APPLE };
enum PetAction { PET_FEED, PET_PLAY, PET_SQUASH, PET_HEAL, PET_LIGHTS, PET_VACATION, PET_NEW_EGG, PET_PET, PET_WARM, PET_RESET };
enum PetReaction { PET_REACT_NONE, PET_REACT_TYPING, PET_REACT_ALERT, PET_REACT_EAT, PET_REACT_PLANE, PET_REACT_BUG,
                   PET_REACT_HATCH, PET_REACT_CHEER, PET_REACT_LOVE };
struct PetLook {
  PetStage stage = PET_ADULT;
  PetStyle style = STYLE_PLAIN;
  bool sick = false, hungry = false, sleeping = false;
  int bugs = 0;
  PetReaction reaction = PET_REACT_NONE;
  uint32_t reaction_ms = 0;  // since the reaction began
  uint8_t phase = PHASE_ADULT;  // the look along his life (pet.cpp § phases)
  float scale = 1.0f;           // his size: a newborn is under half of an adult
};
void pet_init();                 // LVGL task, at UI build: loads NVS
void pet_tick();                 // LVGL task, every frame: time passes, saves now and then
void pet_event(PetEvent e);      // LVGL task: Claude work reaches the pet
bool pet_action(PetAction a);    // LVGL task: the app's actions; false: nothing to do
PetLook pet_look();
int pet_food();
int pet_joy();
int pet_energy();
bool pet_vacation();
bool pet_born();
uint32_t pet_age_s();
String pet_counts();
const char *pet_stage_name(PetStage s);
const char *pet_style_name(PetStyle s);
const char *pet_phase_name(uint8_t phase);
String pet_report();
uint32_t pet_hatch_left_s();  // an egg: seconds to hatching; 0 otherwise
String pet_said();  // what he says right now (a few seconds), "" otherwise
void pet_test(const String &arg);
void pet_app_create();   // app_pet.cpp: the Clawd app
void pet_app_tick();
String pet_app_report();
void pet_game_result(int apples, int stars, int bugs);  // Catch (catch.cpp) is over
// catch.cpp: Catch, the pet's own game
void catch_create();
void catch_start();
bool catch_active();
void catch_tick();
void catch_turn(int step);
void catch_tap();
void catch_abort();
String catch_report();
// buddy.cpp: Clawd into any RGB565 buffer; `look` (the pet) adds his stage, accessory and reactions
void clawd_draw(lv_color_t *b, int bw, int bh, int pw, int ph, int base_y, const String &mood, uint32_t now, bool bang_ok,
                lv_color_t bg, bool floor_shadow = false, const PetLook *look = nullptr);

// mic.cpp — the PDM microphone (spike: T mic <seconds>)
void mic_test(int seconds);
bool dictation_start();          // records while the finger is held; false when the mic is busy
void dictation_stop(bool send);  // send: the relay transcribes; else it drops the audio
bool dictation_recording();
int dictation_level();           // the last block, dBFS
void dictation_bands(float *out);  // 32 bands, 0…1, low → high: the equalizer

// dictate.cpp — orca-deck's hold-to-dictate screen
void dictate_create();
void dictate_begin(const String &title, const String &agent);
void dictate_tick();
void dictate_tap();
void dictate_demo(const String &text);  // tests: the result's play-out, nothing sent
void dictate_swipe();
void dictate_close();
bool dictate_is_open();
String dictate_report();

// banner.cpp — needs-you / done of an agent while orca-deck is not on screen
void banner_create();
void banner_show(const String &worktree, const String &agent, const String &name, const String &bucket);
void banner_show_app(const App *app, const String &line);  // an app's alert: a tap opens the app
bool banner_is_shown();
void banner_hide();
bool banner_take(bool forced = false);  // a tap on the banner: opens orca-deck on its agent
void banner_tick();

// ui.cpp (orca-deck)
void ui_test_enqueue(const String &line);  // "T …" lines over USB: drive the UI from the Mac (tests)
void ui_after_send(const String &agent_id, const String &sent = "");  // `sent`: the text, to fly away
void deck_open_agent(const String &worktree, const String &agent);  // the banner's tap
std::vector<Agent> deck_all_agents();
std::vector<DeckTarget> deck_chat_targets();  // every chat agent, the one on screen in Claude first  // every agent the bridge knows, for the hub's orbits  // Apex: back to the main page, turned to the next waiting agent
void ui_close_pages();
String ui_title(const Worktree &w);
String ui_meta(const Worktree &w, const Agent *a);
uint32_t color_of(const String &bucket);
lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int y, int width = 236);
void set_text(lv_obj_t *label, const String &text);

// record.cpp — the demo recorder: frame-exact capture of the UI's real animations (T rec, knob/test/rec.py)
void rec_start(const String &arg);
void rec_event(const char *kind);      // a haptic event, logged with its frame time while recording
void ui_test_run_now(const String &line);  // ui.cpp: run one T verb at once (the recorder's script)
uint32_t deck_millis();                // millis(), but frame time while recording
// Every millis() in the deck's code reads the recorder's clock (record.cpp calls the real one as (millis)()).
#define millis() deck_millis()
