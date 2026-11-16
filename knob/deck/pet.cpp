// Clawd the pet (.work/plans/clawd-pet.md; user, 2026-11-15: a tamagotchi made of the saver's Clawd that lives on
// the user's work with Claude). Three gauges decay with real time — food, joy, energy — and Claude work feeds them:
// a done is a meal, a send or an answer from the knob is joy, a failed PR check lets a bug in, a full weekly limit
// tires him. Classic rules (user's pick): food at 0 for 12 h makes him sick, 24 h more untreated and he dies; then an
// egg. Time runs while the knob is off (caught up from the wall clock the Mac sets), unless vacation is on.
// The state lives in NVS namespace "pet"; it is written only from the LVGL task (pet_tick / pet_action / pet_event
// are all called there).
#include "deck.h"
#include <Preferences.h>
#include <time.h>

#define HATCH_S (24 * 3600)        // an egg hatches in a day; every done while it waits warms it a little
#define EGG_WARM_S (15 * 60)    // a done warms the egg by 15 min (an hour hatched it in hours on a busy night)
#define ADULT_S (7 * 86400)        // a week from hatching to grown-up (newborn → baby → toddler → kid → teen)
#define ELDER_S (30 * 86400)
#define LEGEND_S (100 * 86400)
#define SICK_AFTER_S (12 * 3600)   // food at 0 this long: sick
#define DEAD_AFTER_S (24 * 3600)   // sick this long, untreated: dead
#define SAVE_EVERY_S (10 * 60)
#define LIGHTS_S (8 * 3600)        // Lights: he sleeps this long, or until woken

static const float FOOD_PER_H = 4.0f, JOY_PER_H = 3.3f, BUG_JOY_PER_H = 2.0f, ENERGY_AWAKE_PER_H = 5.0f,
                   ENERGY_ASLEEP_PER_H = 15.0f;

struct Pet {
  uint32_t born = 0, hatched = 0, last = 0, sick_since = 0, zero_since = 0, lights_until = 0, died_age = 0;
  uint8_t stage = PET_EGG, style = STYLE_PLAIN, bugs = 0;
  float food = 70, joy = 70, energy = 80;
  uint16_t done_n = 0, night_n = 0, play_n = 0, answers_n = 0;
  uint16_t streak = 0, care_day = 0;  // days in a row you looked after him; the last such day (local days since 1970)
  bool vacation = false;
};
static Pet pet;
static bool dirty = false, loaded = false;
static uint32_t saved_at = 0, save_soon_at = 0;  // epoch s / millis
static PetReaction reaction = PET_REACT_NONE;
static uint32_t reaction_at = 0;                 // millis
// What he says (user, 2026-11-16: the knob is his pet, in Ukrainian): a short line shown in a bubble for a few
// seconds — on the Clawd app and the screensaver.
static uint8_t phase_now(uint32_t now);  // § phases, below
static String said;
static uint32_t said_at = 0;
#define SAY_MS 4500

static void say(const char *const *lines, int n)
{
  said = lines[random(0, n)];
  said_at = millis();
}
// SAY(L("en", "ua"), …): built on each call, so the line follows the chosen language
#define SAY(...)                                         \
  do {                                                   \
    const char *const LINES[] = {__VA_ARGS__};          \
    say(LINES, sizeof(LINES) / sizeof(LINES[0]));        \
  } while (0)

String pet_said() { return said.length() && millis() - said_at < SAY_MS ? said : String(""); }

static uint32_t now_s()
{
  time_t t = time(nullptr);
  return t < 1700000000 ? 0 : (uint32_t)t;  // 0: the Mac has not set the clock yet
}

static void react(PetReaction r)
{
  reaction = r;
  reaction_at = millis();
}

static void touch() { dirty = true, save_soon_at = millis() + 5000; }  // events come in bursts: one write

static void save()
{
  Preferences p;
  p.begin("pet", false);
  p.putUInt("born", pet.born), p.putUInt("hatched", pet.hatched), p.putUInt("last", pet.last);
  p.putUInt("sick", pet.sick_since), p.putUInt("zero", pet.zero_since), p.putUInt("lights", pet.lights_until);
  p.putUInt("diedAge", pet.died_age);
  p.putUChar("stage", pet.stage), p.putUChar("style", pet.style), p.putUChar("bugs", pet.bugs);
  p.putUChar("food", (uint8_t)lroundf(pet.food)), p.putUChar("joy", (uint8_t)lroundf(pet.joy));
  p.putUChar("energy", (uint8_t)lroundf(pet.energy));
  p.putUShort("done", pet.done_n), p.putUShort("night", pet.night_n), p.putUShort("play", pet.play_n);
  p.putUShort("answers", pet.answers_n), p.putBool("vacation", pet.vacation);
  p.putUShort("streak", pet.streak), p.putUShort("careDay", pet.care_day);
  p.end();
  dirty = false;
  saved_at = now_s();
  save_soon_at = 0;
}

static void new_egg(uint32_t now)
{
  pet = Pet();
  pet.born = now;
  pet.last = now;
  react(PET_REACT_NONE);
  touch();
}

void pet_init()
{
  Preferences p;
  p.begin("pet", true);
  bool known = p.isKey("born");
  if (known) {
    pet.born = p.getUInt("born"), pet.hatched = p.getUInt("hatched"), pet.last = p.getUInt("last");
    pet.sick_since = p.getUInt("sick"), pet.zero_since = p.getUInt("zero"), pet.lights_until = p.getUInt("lights");
    pet.died_age = p.getUInt("diedAge");
    pet.stage = p.getUChar("stage", PET_EGG), pet.style = p.getUChar("style", STYLE_PLAIN), pet.bugs = p.getUChar("bugs");
    pet.food = p.getUChar("food", 70), pet.joy = p.getUChar("joy", 70), pet.energy = p.getUChar("energy", 80);
    pet.done_n = p.getUShort("done"), pet.night_n = p.getUShort("night"), pet.play_n = p.getUShort("play");
    pet.answers_n = p.getUShort("answers"), pet.vacation = p.getBool("vacation");
    pet.streak = p.getUShort("streak"), pet.care_day = p.getUShort("careDay");
  }
  p.end();
  loaded = true;
  // A first boot has no clock yet: the egg is laid when the Mac sets it (pet_tick).
}

static int local_hour(uint32_t now)
{
  time_t t = now + tz_offset_min() * 60;
  struct tm tm;
  gmtime_r(&t, &tm);
  return tm.tm_hour;
}

// A day you did something with him (fed, stroked, played, warmed) extends the streak; a missed day starts it over.
static void cared(uint32_t now)
{
  if (!now) return;
  uint16_t day = (uint16_t)((now + tz_offset_min() * 60) / 86400);
  if (day == pet.care_day) return;
  pet.streak = pet.care_day && day == pet.care_day + 1 ? pet.streak + 1 : 1;
  pet.care_day = day;
  if (pet.streak > 1) {
    static String line;
    line = L("Streak: ", "Серія: ") + String(pet.streak) + (settings.lang ? " days together!" : pet.streak < 5 ? " дні разом!" : " днів разом!");
    said = line, said_at = millis();
  }
}

static uint8_t adult_style()
{
  if (pet.done_n && pet.night_n * 100 >= pet.done_n * 40) return STYLE_OWL;
  if (pet.play_n * 2 > pet.done_n && pet.play_n >= 5) return STYLE_DJ;
  if (pet.done_n >= 20) return STYLE_SHIPPER;
  return STYLE_PLAIN;
}

// He sleeps when we do: 00:00–10:00 local (user, 2026-11-16), when the lights are off, and when every agent is done.
// "Wake" keeps him up while you play with him; 15 min of nothing and he is asleep again.
static uint32_t awake_until = 0;
#define NIGHT_END_H 10
#define WAKE_S (15 * 60)  // woken up: he dozes off again after 15 min without anything from you (user, 2026-11-16)
static void keep_awake(uint32_t now) { if (awake_until && now < awake_until) awake_until = now + WAKE_S; }
static bool night(uint32_t now) { return now && local_hour(now) < NIGHT_END_H; }
static bool asleep(uint32_t now)
{
  if (awake_until && now < awake_until) return false;
  return (pet.lights_until && now < pet.lights_until) || night(now) || claude_mood() == "sleep";
}

// Time passes: `dt` seconds of decay, the stages, sickness and death — applied in one go (also for the hours the
// knob was off).
static void advance(uint32_t from, uint32_t to)
{
  if (to <= from || pet.stage == PET_DEAD) return;
  float h = (to - from) / 3600.0f;
  bool sleeping = asleep(to);
  float food_before = pet.food;
  if (pet.stage != PET_EGG) {
    float slow = night(to) ? 0.5f : 1.0f;  // asleep at night: hunger and boredom come at half the pace
    pet.food = fmaxf(0, pet.food - FOOD_PER_H * h * slow);
    pet.joy = fmaxf(0, pet.joy - (JOY_PER_H + BUG_JOY_PER_H * pet.bugs) * h * slow);
    pet.energy = sleeping ? fminf(100, pet.energy + ENERGY_ASLEEP_PER_H * h) : fmaxf(0, pet.energy - ENERGY_AWAKE_PER_H * h);
  }
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  int week = shared.mac.claude_week;
  xSemaphoreGive(shared_mux);
  if (week > 90) pet.energy = fminf(pet.energy, 50);  // the weekly limit nearly gone: he is tired too
  if (pet.lights_until && to >= pet.lights_until) pet.lights_until = 0;
  // stages
  if (pet.stage == PET_EGG && to - pet.born >= HATCH_S) {
    pet.stage = PET_BABY, pet.hatched = to, react(PET_REACT_HATCH);
    SAY(L("Hi! I'm Clawd", "Привіт! Я Clawd"), L("I hatched!", "Я вилупився!"));
  }
  if (pet.stage == PET_BABY && to - pet.hatched >= ADULT_S) {
    pet.stage = PET_ADULT, pet.style = adult_style(), react(PET_REACT_CHEER);
    SAY(L("I grew up!", "Я виріс!"), L("Look at me now!", "Дивись, який я тепер!"));
  }
  // classic rules: hungry → sick → dead
  if (pet.food <= 0) {
    // the moment the food ran out, inside this step (a long step — hours off — must not start the clock early)
    if (!pet.zero_since) pet.zero_since = from + (uint32_t)(food_before / FOOD_PER_H * 3600);
    if (!pet.sick_since && to - pet.zero_since >= SICK_AFTER_S) pet.sick_since = pet.zero_since + SICK_AFTER_S;
  } else {
    pet.zero_since = 0;
  }
  if (pet.sick_since && to - pet.sick_since >= DEAD_AFTER_S) {
    pet.died_age = pet.sick_since + DEAD_AFTER_S - pet.born;
    pet.stage = PET_DEAD;
    haptic_buzz(2);
  }
}

// He calls you, tamagotchi-style: hungry, sick, bugs or bored → a buzz and a banner (a tap opens the Clawd app).
// At most every 2 h, 10:00–24:00 (he sleeps from midnight), never in Focus, never asleep.
static void call_you(uint32_t now)
{
  static uint32_t last_call = 0;  // millis; 0: not since boot
  if (pet.stage == PET_EGG || pet.stage == PET_DEAD || pet.vacation) return;
  if (last_call && millis() - last_call < 2 * 3600 * 1000UL) return;
  int h = local_hour(now);
  if (h < NIGHT_END_H || focus_is_on() || asleep(now)) return;  // awake hours only: 10:00–24:00
  const char *why = pet.sick_since ? L("I feel bad, heal me", "мені погано, полікуй") : pet.food < 20 ? L("I'm hungry", "їсти хочу")
                  : pet.bugs     ? L("bugs here!", "тут баги!")
                  : pet.joy < 20 ? L("bored, let's play?", "нудно, пограємо?")
                                                                        : nullptr;
  if (!why) return;
  if (!last_call && millis() < 60000) return;  // not right at boot
  last_call = millis();
  banner_show_app(&app_pet, String("Clawd: ") + why);
  haptic_buzz(1);
}

void pet_tick()
{
  if (!loaded) return;
  uint32_t now = now_s();
  if (!now) return;  // no wall clock yet: nothing ages
  if (!pet.born) {   // the very first egg, laid once the clock is known
    new_egg(now);
    return;
  }
  if (now < pet.last) pet.last = now;  // the clock went back: never a negative step
  if (now - pet.last >= 60) {          // once a minute (and the whole gap after a power-off)
    if (pet.vacation) {
      if (pet.lights_until) pet.lights_until += now - pet.last;  // a frozen night stays as long
      pet.born += now - pet.last;  // vacation freezes his age too
      if (pet.hatched) pet.hatched += now - pet.last;
      if (pet.zero_since) pet.zero_since += now - pet.last;
      if (pet.sick_since) pet.sick_since += now - pet.last;
    } else {
      advance(pet.last, now);
    }
    pet.last = now;
    dirty = true;
  }
  call_you(now);
  // § milestones: every new phase of his life is an event — a banner, a festive buzz, a word from him
  static int8_t seen_phase = -1;
  uint8_t ph = phase_now(now);
  if (seen_phase >= 0 && ph != seen_phase) {
    static String line;
    line = String("Clawd: ") + pet_phase_name(ph) + "!";
    banner_show_app(&app_pet, line);
    Serial.printf("pet milestone: %d -> %d (%s)\n", seen_phase, ph, pet_phase_name(ph));
    haptic_click(3);
    if (ph == PHASE_EGG_WARM) SAY(L("Warming up in here…", "Я тут гріюсь…"));
    else if (ph == PHASE_EGG_CRACK) SAY(L("Crack! Coming out!", "Хрусь! Я лізу!"));
    else if (ph == PHASE_TEEN) SAY(L("Look, a skateboard!", "Дивись, у мене скейт!"));
    else if (ph == PHASE_ELDER) SAY(L("I'm a grown-up now", "Я вже поважний"));
    else if (ph == PHASE_LEGEND) SAY(L("I'm a legend!", "Я легенда!"));
  }
  seen_phase = ph;
  if (pet.stage == PET_EGG && pet_hatch_left_s() < 3600) {  // less than an hour to go: a heartbeat now and then
    static uint32_t beat_at = 0;
    if (!beat_at || millis() - beat_at > 5 * 60000UL) beat_at = millis(), haptic_click(2);
  }
  if (dirty && ((save_soon_at && (int32_t)(millis() - save_soon_at) >= 0) || now - saved_at >= SAVE_EVERY_S)) save();
}

void pet_event(PetEvent e)
{
  uint32_t now = now_s();
  if (!loaded || !pet.born || pet.stage == PET_DEAD) return;
  switch (e) {
    case PET_AGENT_WORKING: {  // a task given to Claude: he types along, and work is fun (user, 2026-11-16)
      static uint32_t last_task = 0;
      react(PET_REACT_TYPING);
      SAY(L("Let's work!", "Працюємо!"), L("Typing…", "Я друкую…"), L("Ooh, a new task!", "О, нова задача!"), L("Claude thinks, I help", "Клод думає, я допомагаю"));
      if (pet.stage == PET_EGG || (last_task && millis() - last_task < 60000)) return;
      last_task = millis();
      pet.joy = fminf(100, pet.joy + 5);
      break;
    }
    case PET_AGENT_ATTENTION:
      react(PET_REACT_ALERT);
      SAY(L("You're needed!", "Тебе кличуть!"), L("An agent waits for you", "Агент чекає на тебе"), L("Hey! Look here", "Ей! Подивись сюди"));
      return;
    case PET_AGENT_DONE:
      if (pet.stage == PET_EGG) {  // work warms the egg: it hatches an hour sooner
        pet.born -= EGG_WARM_S;
        SAY(L("Warm… knock-knock", "Тепло… тук-тук"), L("I'll hatch soon", "Я скоро вилуплюсь"), L("Almost…", "Ще трошки…"));
      } else {
        react(PET_REACT_EAT);
        SAY(L("Yum! Another task?", "Смачно! Ще задачу?"), L("Done! Nom-nom", "Готово! Ням-ням"), L("Task eaten", "Задачу з’їдено"), L("One more done!", "Ще одна готова!"));
      }
      pet.food = fminf(100, pet.food + 15);
      pet.done_n++;
      if (now) {
        int h = local_hour(now);
        if (h >= 22 || h < 5) pet.night_n++;
      }
      break;
    case PET_SENT:
      pet.joy = fminf(100, pet.joy + 10);
      pet.answers_n++;
      react(PET_REACT_PLANE);
      SAY(L("Delivering!", "Лечу передати!"), L("Sent!", "Відправив!"), L("Claude got it", "Клод отримав"));
      break;
    case PET_PR_FAIL:
      if (pet.bugs < 3) pet.bugs++;
      react(PET_REACT_BUG);
      SAY(L("A bug! Squash it!", "Баг! Розчави його!"), L("Oops, tests failed", "Ой, тести впали"), L("A bug crawls here", "Тут повзає баг"));
      break;
    case PET_SNAKE_APPLE: pet.joy = fminf(100, pet.joy + 2); break;
  }
  touch();
}

bool pet_action(PetAction a)
{
  uint32_t now = now_s();
  if (!loaded || !pet.born) return false;
  if (a == PET_RESET) {  // Settings → start over: a fresh egg, whatever he was (user, 2026-11-16)
    new_egg(now ? now : pet.last);
    SAY(L("Hi! I'm an egg again", "Привіт! Я ще яйце"));
    return true;
  }
  if (a == PET_VACATION) {  // allowed at any stage, even an egg
    pet.vacation = !pet.vacation;
    touch();
    return true;
  }
  keep_awake(now);
  if (a != PET_VACATION && a != PET_LIGHTS) cared(now);
  if (pet.stage == PET_DEAD) {  // any action on the tombstone: a new egg
    if (a != PET_NEW_EGG) return false;
    new_egg(now ? now : pet.last);
    return true;
  }
  if (pet.stage == PET_EGG && a != PET_WARM) return false;  // an egg is only kept warm (user, 2026-11-16)
  switch (a) {
    case PET_WARM: {  // a tap on the egg warms it: hatching 2 min sooner, up to 30 taps an hour
      static uint32_t window_at = 0;
      static int warms = 0;
      if (pet.stage != PET_EGG) return false;
      if (!window_at || millis() - window_at > 3600000UL) window_at = millis(), warms = 0;
      if (warms >= 30) {
        SAY(L("I'm warm enough", "Мені вже тепло"), L("Enough, I've got this", "Досить, я сам"));
        return false;
      }
      warms++;
      pet.born -= 120;
      react(PET_REACT_LOVE);
      SAY(L("Warm…", "Тепло…"), L("Knock-knock", "Тук-тук"), L("Almost there!", "Ще трошки!"), L("I'm in here", "Я тут, всередині"));
      break;
    }
    case PET_FEED:
      if (pet.food > 90) {  // stuffed
        pet.joy = fmaxf(0, pet.joy - 5);
        SAY(L("I'm full…", "Я вже ситий…"), L("No room left", "Більше не влізе"));
      } else {
        SAY(L("Nom!", "Ням!"), L("Thanks!", "Дякую!"), L("Delicious", "Смакота"));
      }
      pet.food = fminf(100, pet.food + 20);
      react(PET_REACT_EAT);
      break;
    case PET_PLAY:
      if (pet.stage == PET_EGG) return false;
      pet.play_n++;
      pet.joy = fminf(100, pet.joy + 5);
      break;
    case PET_SQUASH:
      if (!pet.bugs) return false;
      pet.bugs--;
      pet.joy = fminf(100, pet.joy + 3);
      react(PET_REACT_CHEER);
      SAY(L("Crunch! One bug less", "Хрусь! Минус баг"), L("All clean!", "Чисто!"));
      break;
    case PET_HEAL:
      if (!pet.sick_since) return false;
      pet.sick_since = 0;
      pet.food = fmaxf(pet.food, 10);  // medicine comes with a bite
      pet.zero_since = 0;              // the hunger clock starts again when this bite is gone
      react(PET_REACT_CHEER);
      SAY(L("I feel better!", "Мені краще!"), L("Thanks, doc", "Дякую, лікарю"));
      break;
    case PET_LIGHTS:
      if (asleep(now) && !pet.lights_until) {  // asleep by the clock or the agents: wake him for a while
        awake_until = now + WAKE_S;
        SAY(L("Oh, I was asleep…", "Ой, я спав…"), L("Is it morning?", "Що, вже ранок?"));
      } else {
        awake_until = 0;
        pet.lights_until = pet.lights_until ? 0 : now + LIGHTS_S;
      }
      break;
    case PET_VACATION:
      pet.vacation = !pet.vacation;
      break;
    case PET_PET: {  // stroked: he loves it, but joy from it comes once in 30 s (no farming with a held finger)
      static uint32_t last_pet = 0;
      if (pet.stage == PET_EGG) return false;
      if (!last_pet || millis() - last_pet > 30000) pet.joy = fminf(100, pet.joy + 6), last_pet = millis();
      react(PET_REACT_LOVE);
      SAY(L("Purr-r-r", "Мур-р-р"), L("More! More!", "Ще! Ще!"), L("Hee-hee", "Хі-хі"));
      break;
    }
    case PET_NEW_EGG: return false;
  }
  touch();
  return true;
}

void pet_game_result(int apples, int stars, int bugs)
{
  keep_awake(now_s());
  cared(now_s());
  if (!loaded || !pet.born || pet.stage == PET_DEAD || pet.stage == PET_EGG) return;
  pet.food = fminf(100, pet.food + apples * 2);
  pet.joy = fmaxf(0, fminf(100, pet.joy + 4 + apples + stars * 4 - bugs * 2));
  pet.energy = fmaxf(0, pet.energy - 6);  // running about tires him
  pet.play_n++;
  react(PET_REACT_CHEER);
  if (apples + stars > 8) SAY(L("What a game! Again!", "Оце гра! Ще раз!"), L("I'm the champ!", "Я чемпіон!"));
  else SAY(L("That was fun!", "Було весело!"), L("Play again?", "Ще пограємо?"));
  touch();
}

// § phases: the look along his life, from his age. Egg: by how far it is to hatching; then by the time since.
static uint8_t phase_now(uint32_t now)
{
  if (pet.stage == PET_DEAD) return PHASE_GRAVE;
  if (!now) now = pet.last;
  if (pet.stage == PET_EGG) {
    float p = pet.born && now > pet.born ? (now - pet.born) / (float)HATCH_S : 0;
    return p < 0.33f ? PHASE_EGG : p < 0.66f ? PHASE_EGG_WARM : p < 0.85f ? PHASE_EGG_WOBBLE : p < 0.95f ? PHASE_EGG_CRACK : PHASE_EGG_HATCH;
  }
  uint32_t s = pet.hatched && now > pet.hatched ? now - pet.hatched : 0;
  if (pet.stage == PET_BABY)
    return s < 6 * 3600 ? PHASE_NEWBORN : s < 86400 ? PHASE_BABY : s < 2 * 86400 ? PHASE_TODDLER : s < 4 * 86400 ? PHASE_KID : PHASE_TEEN;
  return s < ELDER_S ? PHASE_ADULT : s < LEGEND_S ? PHASE_ELDER : PHASE_LEGEND;
}

static float phase_scale(uint8_t ph)
{
  // a newborn is as small as he can be (user, 2026-11-16: «максимально маленьким»), then he grows
  static const float S[] = {1, 1, 1, 1, 1, 0.25f, 0.35f, 0.5f, 0.65f, 0.82f, 1, 1, 1, 1};
  return S[ph];
}

const char *pet_phase_name(uint8_t ph)
{
  static const char *UA[] = {"яйце", "тепле яйце", "яйце хитається", "яйце тріснуло", "вилуплюється", "новонароджений",
                             "малюк", "карапуз", "дитина", "підліток", "дорослий", "старійшина", "легенда", "спочив"};
  static const char *EN[] = {"egg", "warm egg", "egg wobbles", "egg cracked", "hatching", "newborn", "baby", "toddler",
                             "kid", "teen", "adult", "elder", "legend", "at rest"};
  return settings.lang ? EN[ph] : UA[ph];
}

PetLook pet_look()
{
  PetLook l;
  l.phase = phase_now(now_s());
  l.scale = phase_scale(l.phase);
  if (pet.stage == PET_BABY && pet.hatched) {  // he grows every hour, not in jumps (user, 2026-11-16: «з часом він має рости»)
    static const float AT_H[] = {0, 6, 24, 48, 96, 168}, SIZE[] = {0.25f, 0.35f, 0.5f, 0.65f, 0.82f, 1.0f};
    uint32_t now = now_s() ? now_s() : pet.last;
    float h = now > pet.hatched ? (now - pet.hatched) / 3600.0f : 0;
    for (int i = 0; i < 5; i++)
      if (h < AT_H[i + 1]) {
        l.scale = SIZE[i] + (SIZE[i + 1] - SIZE[i]) * (h - AT_H[i]) / (AT_H[i + 1] - AT_H[i]);
        break;
      }
  }
  l.stage = (PetStage)pet.stage;
  l.style = (PetStyle)pet.style;
  l.sick = pet.sick_since != 0;
  l.hungry = pet.food < 25;
  l.sleeping = pet.born && asleep(now_s() ? now_s() : pet.last);
  l.bugs = pet.bugs;
  l.reaction = millis() - reaction_at < 2500 ? reaction : PET_REACT_NONE;
  l.reaction_ms = millis() - reaction_at;
  return l;
}

uint32_t pet_hatch_left_s()
{
  uint32_t now = now_s() ? now_s() : pet.last;
  if (pet.stage != PET_EGG || !pet.born) return 0;
  uint32_t age = now > pet.born ? now - pet.born : 0;
  return age >= HATCH_S ? 0 : HATCH_S - age;
}

int pet_food() { return (int)lroundf(pet.food); }
int pet_joy() { return (int)lroundf(pet.joy); }
int pet_energy() { return (int)lroundf(pet.energy); }
bool pet_vacation() { return pet.vacation; }
bool pet_born() { return pet.born != 0; }

uint32_t pet_age_s()
{
  if (pet.stage == PET_DEAD) return pet.died_age;
  uint32_t now = now_s();
  return pet.born && now > pet.born ? now - pet.born : 0;
}

String pet_counts()
{
  return String(pet.done_n) + L(" tasks · ", " задач · ") + pet.night_n + L(" at night\n", " вночі\n") + pet.answers_n +
         L(" answers · ", " відповідей · ") + pet.play_n + L(" games\ncare streak: ", " ігор\nсерія догляду: ") + pet.streak + L(" d", " дн");
}

const char *pet_stage_name(PetStage s)
{
  static const char *UA[] = {"яйце", "малюк", "дорослий", "спочив"}, *EN[] = {"egg", "baby", "adult", "at rest"};
  const char *const *N = settings.lang ? EN : UA;
  return N[s];
}

const char *pet_style_name(PetStyle s)
{
  static const char *UA[] = {"Clawd", "сова", "шипер", "діджей"}, *EN[] = {"Clawd", "owl", "shipper", "DJ"};
  const char *const *N = settings.lang ? EN : UA;
  return N[s];
}

String pet_report()
{
  return String("pet ") + pet_stage_name((PetStage)pet.stage) + " style=" + pet_style_name((PetStyle)pet.style) +
         " food=" + pet_food() + " joy=" + pet_joy() + " energy=" + pet_energy() + " bugs=" + pet.bugs +
         " sick=" + (pet.sick_since ? 1 : 0) + " age=" + pet_age_s() / 60 + "m vac=" + pet.vacation + " " + pet_counts() +
         (dirty ? " (unsaved)" : "");
}

// Tests: "pet" reports; "pet age <hours>" runs that much time now; "pet feed|play|squash|heal|lights|vac|egg";
// "pet done|sent|bug" as if the event came; "pet kill" (sick for a day already).
void pet_test(const String &arg)
{
  uint32_t now = now_s();
  if (arg.startsWith("age ")) {
    uint32_t dt = (uint32_t)(arg.substring(4).toFloat() * 3600);
    if (now && pet.born) {
      pet.born -= dt;
      if (pet.hatched) pet.hatched -= dt;
      if (pet.zero_since) pet.zero_since -= dt;
      if (pet.sick_since) pet.sick_since -= dt;
      advance(pet.last - dt, pet.last);
      touch();
    }
  } else if (arg.startsWith("style ")) pet.stage = PET_ADULT, pet.style = constrain(arg.substring(6).toInt(), 0, 3), touch();
  else if (arg.startsWith("phase ")) {  // any look at once: its age is set to the middle of that phase
    int ph = constrain(arg.substring(6).toInt(), 0, (int)PHASE_GRAVE);
    uint32_t t = now ? now : pet.last;
    static const float EGG_P[] = {0.15f, 0.5f, 0.75f, 0.9f, 0.97f};
    static const uint32_t AGE[] = {3 * 3600, 15 * 3600, 36 * 3600, 3 * 86400, 5 * 86400, 15 * 86400, 60 * 86400, 150 * 86400};
    if (ph <= PHASE_EGG_HATCH) pet.stage = PET_EGG, pet.born = t - (uint32_t)(EGG_P[ph] * HATCH_S);
    else if (ph == PHASE_GRAVE) pet.stage = PET_DEAD, pet.died_age = 9 * 86400;
    else {
      pet.stage = ph <= PHASE_TEEN ? PET_BABY : PET_ADULT;
      pet.hatched = t - AGE[ph - PHASE_NEWBORN];
      pet.born = pet.hatched - HATCH_S;
    }
    touch();
  } else if (arg.startsWith("stage ")) {  // the stage's clock starts now, or the next tick moves him on
    pet.stage = constrain(arg.substring(6).toInt(), 0, 3);
    if (pet.stage == PET_EGG) pet.born = now ? now : pet.last;
    if (pet.stage == PET_BABY) pet.hatched = now ? now : pet.last;
    touch();
  }
  else if (arg == "sick") pet.sick_since = now ? now : pet.last, touch();
  else if (arg == "hungry") pet.food = 10, touch();
  else if (arg == "feed") pet_action(PET_FEED);
  else if (arg == "play") pet_action(PET_PLAY);
  else if (arg == "squash") pet_action(PET_SQUASH);
  else if (arg == "heal") pet_action(PET_HEAL);
  else if (arg == "lights") pet_action(PET_LIGHTS);
  else if (arg == "vac") pet_action(PET_VACATION);
  else if (arg == "egg") new_egg(now ? now : pet.last);
  else if (arg == "done") pet_event(PET_AGENT_DONE);
  else if (arg == "sent") pet_event(PET_SENT);
  else if (arg == "bug") pet_event(PET_PR_FAIL);
  else if (arg == "kill") {
    pet.food = 0;
    pet.sick_since = (now ? now : pet.last) - DEAD_AFTER_S + 5;
    pet.last -= 60;  // the next tick runs the clock (and he dies)
    touch();
  }
  Serial.println(pet_report());
}
