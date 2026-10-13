#include "emu_profile.h"

/*
 * Haojixing V1+ / Magic Tower 50 application profile.
 *
 * This file intentionally contains application-specific entry points and
 * resource IDs.  emu_core.c must not need to know which V1+ application is
 * currently being executed.
 */

extern const u8 _binary_map0280_bin_start[], _binary_map0280_bin_end[];
extern const u8 _binary_dlg0281_bin_start[], _binary_dlg0281_bin_end[];
#define DECL_BMP(N) extern const u8 _binary_bmp##N##_bin_start[], _binary_bmp##N##_bin_end[]
DECL_BMP(00); DECL_BMP(01); DECL_BMP(02); DECL_BMP(03); DECL_BMP(04); DECL_BMP(05); DECL_BMP(06);
DECL_BMP(07); DECL_BMP(08); DECL_BMP(09); DECL_BMP(10); DECL_BMP(11); DECL_BMP(12); DECL_BMP(13);

static const EmuAppProfile profile = {
    "Haojixing V1+ Magic Tower 50",
    0x300CFC44u,
    0x300CF778u,
    0x300D0094u,
    0x30472148u
};

static u8 work_maps_file[6050];
static u8 save_slot_file[6304];
static u32 save_slot_len;
static u32 save_generation;
static u32 save_present;

static void copy_bytes(u8 *dst, const u8 *src, u32 n){ while(n--) *dst++ = *src++; }

const EmuAppProfile *emu_profile_current(void){ return &profile; }

void emu_profile_reset(void){
    copy_bytes(work_maps_file, _binary_map0280_bin_start, sizeof(work_maps_file));
    save_slot_len = 0;
    save_generation = 0;
    save_present = 0;
}

int emu_profile_open_file(u32 rid, EmuProfileFile *out){
    if(!out) return 0;
    out->data = 0; out->len = 0; out->cap = 0; out->writable = 0;
    switch(rid){
        case 0x280u:
            out->data = _binary_map0280_bin_start;
            out->len = (u32)(_binary_map0280_bin_end - _binary_map0280_bin_start);
            return 1;
        case 0x281u:
            out->data = _binary_dlg0281_bin_start;
            out->len = (u32)(_binary_dlg0281_bin_end - _binary_dlg0281_bin_start);
            return 1;
        case 0x82u:
            out->data = work_maps_file;
            out->len = sizeof(work_maps_file);
            out->cap = sizeof(work_maps_file);
            out->writable = 1;
            return 1;
        case 0x89u:
            out->data = save_slot_file;
            out->len = save_slot_len;
            out->cap = sizeof(save_slot_file);
            out->writable = 1;
            return 1;
        default:
            return 0;
    }
}

static const u8 *bmp_start(int idx, u32 *len){
    const u8 *s = 0, *e = 0;
    switch(idx){
#define CASEBMP(I,N) case I: s=_binary_bmp##N##_bin_start; e=_binary_bmp##N##_bin_end; break
        CASEBMP(0,00); CASEBMP(1,01); CASEBMP(2,02); CASEBMP(3,03);
        CASEBMP(4,04); CASEBMP(5,05); CASEBMP(6,06); CASEBMP(7,07);
        CASEBMP(8,08); CASEBMP(9,09); CASEBMP(10,10); CASEBMP(11,11);
        CASEBMP(12,12); CASEBMP(13,13);
#undef CASEBMP
        default: break;
    }
    if(len) *len = s ? (u32)(e - s) : 0;
    return s;
}

int emu_profile_bitmap(u32 rid, int index, const u8 **data, u32 *len){
    const u8 *p;
    u32 n = 0;
    if(rid != 0x176u) return 0;
    p = bmp_start(index, &n);
    if(!p) return 0;
    if(data) *data = p;
    if(len) *len = n;
    return 1;
}

void emu_profile_file_written(u32 rid, u32 new_len){
    if(rid == 0x89u){
        if(new_len > sizeof(save_slot_file)) new_len = sizeof(save_slot_file);
        save_slot_len = new_len;
        save_present = (save_slot_len == sizeof(save_slot_file));
        save_generation++;
    }
}

u32 emu_profile_save_size(void){ return save_slot_len; }
u32 emu_profile_save_generation(void){ return save_generation; }
u32 emu_profile_save_present(void){ return save_present; }
void emu_profile_mark_save_present(void){ save_present = 1; }

int emu_profile_save_export(u8 *dst, u32 cap){
    if(!dst || save_slot_len != sizeof(save_slot_file) || cap < sizeof(save_slot_file)) return 0;
    copy_bytes(dst, save_slot_file, sizeof(save_slot_file));
    return 1;
}

#include "web_save.inc"

int emu_profile_save_import(const u8 *src, u32 len){
    if(!src || len != sizeof(save_slot_file) || !web_valid_save(src)) return 0;
    copy_bytes(save_slot_file, src, sizeof(save_slot_file));
    web_restore_item_limits(save_slot_file);
    save_slot_len = sizeof(save_slot_file);
    save_present = 1;
    return 1;
}
