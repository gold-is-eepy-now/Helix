/* linux/input/mt.h — multitouch slots on top of linux/input.h.
 *
 * A multitouch device reports several contacts. Each contact owns a "slot"; the driver
 * selects a slot with input_mt_slot(), says whether a finger is present in it, and then
 * reports that finger's axes. input_mt_assign_slots() matches the contacts of this frame to
 * the slots used by the previous frame so a finger keeps its slot while it moves. */
#ifndef HELIX_COMPAT_LINUX_INPUT_MT_H
#define HELIX_COMPAT_LINUX_INPUT_MT_H

#include <linux/input.h>

#define INPUT_MT_POINTER     0x0001u
#define INPUT_MT_DROP_UNUSED 0x0002u
#define INPUT_MT_TRACK       0x0004u
#define INPUT_MT_MAX_SLOTS   16

struct input_mt_pos {
    s16 x, y;
};

struct input_mt_slot {
    bool present;
    int tracking_id;
    s32 x, y;                          /* last reported position of this slot */
};

struct input_mt {
    int num_slots;
    int cur_slot;
    unsigned int flags;
    int next_id;
    struct input_mt_slot slots[INPUT_MT_MAX_SLOTS];
    bool used[INPUT_MT_MAX_SLOTS];     /* touched since the last input_mt_sync_frame */
    bool was_present[INPUT_MT_MAX_SLOTS]; /* present at the previous input_sync */
    struct input_mt_pos last[INPUT_MT_MAX_SLOTS];
};

int input_mt_init_slots(struct input_dev *dev, unsigned int num_slots, unsigned int flags);
void input_mt_slot(struct input_dev *dev, int slot);
void input_mt_report_slot_state(struct input_dev *dev, unsigned int tool_type, bool active);
void input_mt_sync_frame(struct input_dev *dev);
/* Match n new contact positions to slots; slots[i] receives contact i's slot. Returns 0. */
int input_mt_assign_slots(struct input_dev *dev, int *slots, const struct input_mt_pos *pos, int num_pos,
                          int dmax);

#endif /* HELIX_COMPAT_LINUX_INPUT_MT_H */
