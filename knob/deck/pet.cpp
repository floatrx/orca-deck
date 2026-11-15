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

#define HATCH_S (10 * 60)          // an egg hatches after 10 min (or on the first done)
#define ADULT_S (24 * 3600)        // a baby grows up after a day
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
  bool vacation = false;
};
static Pet pet;
static bool dirty = false, loaded = false;
static uint32_t saved_at = 0, save_soon_at = 0;  // epoch s / millis
static PetReaction reaction = PET_REACT_NONE;
static uint32_t reaction_at = 0;                 // millis

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

static uint8_t adult_style()
{
  if (pet.done_n && pet.night_n * 100 >= pet.done_n * 40) return STYLE_OWL;
  if (pet.play_n * 2 > pet.done_n && pet.play_n >= 5) return STYLE_DJ;
  if (pet.done_n >= 20) return STYLE_SHIPPER;
  return STYLE_PLAIN;
}

static bool asleep(uint32_t now) { return (pet.lights_until && now < pet.lights_until) || claude_mood() == "sleep"; }

// Time passes: `dt` seconds of decay, the stages, sickness and death — applied in one go (also for the hours the
// knob was off).
static void advance(uint32_t from, uint32_t to)
{
  if (to <= from || pet.stage == PET_DEAD) return;
  float h = (to - from) / 3600.0f;
  bool sleeping = asleep(to);
  float food_before = pet.food;
  if (pet.stage != PET_EGG) {
    pet.food = fmaxf(0, pet.food - FOOD_PER_H * h);
    pet.joy = fmaxf(0, pet.joy - (JOY_PER_H + BUG_JOY_PER_H * pet.bugs) * h);
    pet.energy = sleeping ? fminf(100, pet.energy + ENERGY_ASLEEP_PER_H * h) : fmaxf(0, pet.energy - ENERGY_AWAKE_PER_H * h);
  }
  xSemaphoreTake(shared_mux, portMAX_DELAY);
  int week = shared.mac.claude_week;
  xSemaphoreGive(shared_mux);
  if (week > 90) pet.energy = fminf(pet.energy, 50);  // the weekly limit nearly gone: he is tired too
  if (pet.lights_until && to >= pet.lights_until) pet.lights_until = 0;
  // stages
  if (pet.stage == PET_EGG && to - pet.born >= HATCH_S) pet.stage = PET_BABY, pet.hatched = to, react(PET_REACT_HATCH);
  if (pet.stage == PET_BABY && to - pet.hatched >= ADULT_S) pet.stage = PET_ADULT, pet.style = adult_style(), react(PET_REACT_CHEER);
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
  if (dirty && ((save_soon_at && (int32_t)(millis() - save_soon_at) >= 0) || now - saved_at >= SAVE_EVERY_S)) save();
}

void pet_event(PetEvent e)
{
  uint32_t now = now_s();
  if (!loaded || !pet.born || pet.stage == PET_DEAD) return;
  switch (e) {
    case PET_AGENT_WORKING: react(PET_REACT_TYPING); return;
    case PET_AGENT_ATTENTION: react(PET_REACT_ALERT); return;
    case PET_AGENT_DONE:
      if (pet.stage == PET_EGG) pet.stage = PET_BABY, pet.hatched = now ? now : pet.born, react(PET_REACT_HATCH);
      else react(PET_REACT_EAT);
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
      break;
    case PET_PR_FAIL:
      if (pet.bugs < 3) pet.bugs++;
      react(PET_REACT_BUG);
      break;
    case PET_SNAKE_APPLE: pet.joy = fminf(100, pet.joy + 2); break;
  }
  touch();
}

bool pet_action(PetAction a)
{
  uint32_t now = now_s();
  if (!loaded || !pet.born) return false;
  if (pet.stage == PET_DEAD) {  // any action on the tombstone: a new egg
    if (a != PET_NEW_EGG) return false;
    new_egg(now ? now : pet.last);
    return true;
  }
  switch (a) {
    case PET_FEED:
      if (pet.stage == PET_EGG) return false;
      if (pet.food > 90) pet.joy = fmaxf(0, pet.joy - 5);  // stuffed
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
      break;
    case PET_HEAL:
      if (!pet.sick_since) return false;
      pet.sick_since = 0;
      pet.food = fmaxf(pet.food, 10);  // medicine comes with a bite
      pet.zero_since = 0;              // the hunger clock starts again when this bite is gone
      react(PET_REACT_CHEER);
      break;
    case PET_LIGHTS:
      pet.lights_until = pet.lights_until ? 0 : now + LIGHTS_S;
      break;
    case PET_VACATION:
      pet.vacation = !pet.vacation;
      break;
    case PET_NEW_EGG: return false;
  }
  touch();
  return true;
}

PetLook pet_look()
{
  PetLook l;
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

String pet_counts() { return String(pet.done_n) + " done · " + pet.night_n + " at night · " + pet.answers_n + " answers · " + pet.play_n + " games"; }

const char *pet_stage_name(PetStage s)
{
  static const char *N[] = {"egg", "baby", "adult", "dead"};
  return N[s];
}

const char *pet_style_name(PetStyle s)
{
  static const char *N[] = {"Clawd", "owl", "shipper", "DJ"};
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
  } else if (arg == "feed") pet_action(PET_FEED);
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
