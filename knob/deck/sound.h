#ifndef SOUND_H
#define SOUND_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The spike's sounds on the board speaker (PCM5100A over I2S). Calls never block: a task plays a queue.
void sound_init(void);
void sound_set_level(uint8_t level);  // 0 off · 1 quiet · 2 on
void sound_tick(void);                // a crown detent
void sound_click(void);               // a tap / an action
void sound_buzz(int times);           // the vibration hum: 1 = done, 2 = needs you
void sound_test(void);                // 1 kHz for 1 s (tests)
int sound_ready(void);                // 1 once I2S is up

#ifdef __cplusplus
}
#endif
#endif
