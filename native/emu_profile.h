#ifndef HJX_EMU_PROFILE_H
#define HJX_EMU_PROFILE_H

#include "emu_core.h"

typedef struct {
    const char *name;
    u32 app_init;
    u32 app_paint;
    u32 app_wndproc;
    u32 ui_mode_addr;
} EmuAppProfile;

typedef struct {
    const u8 *data;
    u32 len;
    u32 cap;
    int writable;
} EmuProfileFile;

const EmuAppProfile *emu_profile_current(void);
void emu_profile_reset(void);

/* Application/package resource mapping. The emulator core only sees resource IDs. */
int emu_profile_open_file(u32 rid, EmuProfileFile *out);
int emu_profile_bitmap(u32 rid, int index, const u8 **data, u32 *len);
void emu_profile_file_written(u32 rid, u32 new_len);

/* Current application's native save-slot bridge. */
u32 emu_profile_save_size(void);
u32 emu_profile_save_generation(void);
int emu_profile_save_export(u8 *dst, u32 cap);
int emu_profile_save_import(const u8 *src, u32 len);
u32 emu_profile_save_present(void);
void emu_profile_mark_save_present(void);

#endif
