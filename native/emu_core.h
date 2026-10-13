#ifndef HJX_EMU_CORE_H
#define HJX_EMU_CORE_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed int i32;
typedef unsigned long long u64;
typedef signed long long i64;

#define FB_W 240
#define FB_H 320
#define EMU_SYSTEM_LCD_W 240
#define EMU_SYSTEM_LCD_H 320

typedef int (*EmuTextFallback)(u32*,int,int,const u8*,u32,int,int,u32,u32,u32);
void emu_set_text_fallback(EmuTextFallback cb);
int emu_set_system_data_image(const char*path);
int emu_set_program_image(const char*path);

/* Legacy application/profile mode (kept for v0.7 regression tests). */
int emu_init(void);
int emu_key(u32 firmware_key);
int emu_timer(u32 timer_id);
void emu_paint(void);
u32 emu_mode(void);
int emu_dialog_active(void);
u32 emu_selection(void);
u32 emu_floor(void);
u32 emu_x(void);
u32 emu_y(void);
u32 emu_hp(void);
u32 emu_atk(void);
u32 emu_def(void);
const u32* emu_framebuffer(void);
int emu_timer_count(void);
int emu_timer_info(int index,u32*id,u32*interval_ms);
u32 emu_save_size(void);
u32 emu_save_generation(void);
int emu_save_export(u8*dst,u32 cap);
int emu_save_import(const u8*src,u32 len);
int emu_last_error(void);
u32 emu_last_error_pc(void);
u32 emu_dialog_title_ptr(void);
int emu_copy_cstring(u32 ptr,u8*dst,u32 cap);

/* Full V1+ system mode.  This runs the original firmware from reset.
   V1PRU is mounted read-only as the factory data image; firmware writes go
   into an optional sidecar state file, never back into V1PRU itself. */
int emu_system_init(const char*v1pru_path,const char*state_path);
int emu_system_init_ex(const char*v1pu_path,const char*v1pru_path,const char*state_path);
/* Host folder presented as the original SD card (drive D:).  Empty path disables it. */
void emu_system_set_sdcard(const char*path);
void emu_system_set_cdrive(const char*path);
int emu_debug_sd_read(u32 addr,u8*dst,u32 len);
int emu_debug_c_read(u32 addr,u8*dst,u32 len);
int emu_debug_c_write(u32 addr,const u8*src,u32 len);
u32 emu_debug_disk_io(u32 i,u32*cmd,u32*fn);
u32 emu_debug_nand_reads(void);
u32 emu_debug_vf_recent(u32 i,u32*lo,u32*hi,u32*len,u32*wr);
u32 emu_debug_trace(u32 i,u32*pc);
void emu_debug_trace_arm(void);
u32 emu_debug_sd_reads(void);
u32 emu_debug_sd_hist(u32 i,u32*addr);
u32 emu_debug_hit(u32 i,u32*pc,u32*r0,u32*w,u32*n);
void emu_debug_hit_boot(u8*dst);
u32 emu_debug_disk_snap(u32 i,u32*ptr,u32*fn,u8*dst);
int emu_system_run(u32 instruction_budget);
/* Native 0x30193068 returns 200 ticks/sec; Timer3 reload is 420000.
   Keep host pacing tied to that contract (5 ms/tick), not the 12 MHz crystal. */
#define EMU_SYSTEM_TIMER_COUNTS_PER_SEC 84000000u
u32 emu_system_timer_counts_per_second(void);
/* Idle fast-forward spends at most these Timer3 counts.  420000
   counts are one 5 ms RTOS tick.  Without this, guest idle time races ahead
   of the wall clock and the firmware auto-off timer expires early. */
void emu_system_add_idle_credit(u32 counts);
/* While a press is still in the host queue, do not stretch the touch task's
   tick sleep across wall-clock time.  Auto-off pacing stays on when no
   gesture is open. */
void emu_system_set_gesture_active(int on);
int emu_system_idle_time_blocked(void);
void emu_system_touch(u32 x,u32 y,int down);
u32 emu_system_touch_down_accepted(void);
u32 emu_system_touch_up_seen(void);
/* Release-path counters.  gpio_release is a host pen-up, touch_wake is the
   original Task TouchScreen polling GPIO, pen_clear is that poll observing
   up, frame is an LCD generation change. */
void emu_system_input_trace(u32*gpio_release,u32*touch_wake,u32*pen_clear,u32*frame);
void emu_system_key(u32 vk,int down);
/* Current contact level was read at this key's matching GPIO matrix pair. */
u32 emu_system_key_sampled(void);
/* Dedicated power button (GPIO 7), separate from the keypad matrix. */
void emu_system_power_key(int down);
u32 emu_system_power_key_down(void);
/* Current power contact level has been read by the firmware GPIO poll. */
u32 emu_system_power_key_sampled(void);
/* Firmware reached System Power Off (would spin forever on hardware cut). */
u32 emu_system_power_cut_pending(void);
void emu_system_clear_power_cut(void);
void emu_system_clear_touch_calibration(void);
/* Host supplies local wall-clock seconds since 1980.  Firmware still accesses
   only the emulated AK322x RTC MMIO registers. */
void emu_system_rtc_host_time(u32 seconds_since_1980);
void emu_system_set_rtc_seconds(u32 seconds_since_1980);
u32 emu_system_rtc_seconds(void);
/* Advance host-monotonic audio time. Firmware still configures sample rate and
   submits DMA exclusively through the emulated AK322x MMIO path. */
void emu_system_audio_advance_us(u64 elapsed_us);
u32 emu_system_audio_dma_events(void);
u32 emu_system_audio_dma_completions(void);
u32 emu_system_audio_sample_rate(void);
/* Drain stereo s16le frames transferred by the emulated audio DMA. */
u32 emu_system_audio_read(u8*dst,u32 cap,u32*rate,u32*epoch);
u32 emu_system_audio_last_event(void);
int emu_system_audio_copy(u8*dst,u32 cap);
int emu_system_audio_submit(u32 index,u32*addr,u32*words,u32*nonzero,u32*sig);
int emu_system_audio_copy_sound(u8*dst,u32 cap);
u32 emu_system_audio_irq_entries(void);
void emu_system_audio_irq_gate(u32*raw,u32*mask,u32*vector);
u32 emu_system_audio_vf_reads(void);
int emu_system_audio_vf(u32 index,u32*dest,u32*len,u32*nz,u32*lr,u32*off_lo,u32*off_hi);
u32 emu_system_audio_vf_dma_hits(void);
int emu_system_mem_copy(u32 addr,u8*dst,u32 n);
void emu_system_pcm_scan(void);
int emu_system_pcm_hot(u32 index,u32*addr,u32*nz);
#ifdef AUDIO_TRACE
u32 emu_audio_trace_cb(u32*r0,u32*r1,u32*r2,u32*r3);
u32 emu_audio_trace_play(u32*wait,u32*wait_r0,u32*post);
u32 emu_audio_trace_ring(u32*stores,u32*nz,u32*pc,u32*addr,u32*memcpy_n);
u32 emu_audio_trace_sub(u32 index,u32*r0,u32*r1,u32*from);
u32 emu_audio_trace_mix(u32*copy,u32*dst,u32*src,u32*len,u32*qpush,u32*qsrc,u32*qlen,u32*drv_a,u32*drv_b);
u32 emu_audio_trace_mix_exit(u32*empty,u32*at,u32*r9,u32*bad);
u32 emu_audio_trace_mix_r5(u32*r5,u32*dest,u32*give,u32*a0);
u32 emu_audio_trace_link(u32*ret_n,u32*ret,u32*fill,u32*ind,u32*fn,u32*isr,u32*isr_fn,u32*ev75,u32*body,u32*set);
u32 emu_audio_trace_sig(u32*sig,u32*drop8,u32*drop5,u32*f8,u32*st);
u32 emu_audio_trace_waiter(u32*waiter,u32*n,u32*thread,u32*msg);
u32 emu_audio_trace_slot8(u32*n,u32*nz);
u32 emu_audio_trace_cmd(u32 cmd,u32*n,u32*lr);
u32 emu_audio_trace_cmd7(u32*fn,u32*obj);
#endif
const u16* emu_system_lcd565(void);
/* Monotonic host hint: changes only when the emulated LCD panel is updated. */
u32 emu_system_lcd_generation(void);
u32 emu_system_lcd_width(void);
u32 emu_system_lcd_height(void);
int emu_system_save_state(void);
/* Periodic save can skip disk I/O when persistent flash/NV/RTC is unchanged.
   Explicit Save, Restart and Close still write through unconditionally. */
int emu_system_autosave_state(void);
/* Rebase the host video deadline after toolbar pause; guest pause uses the
   same clock reset through the original movie player path. */
void emu_system_movie_resume_clock(void);
int emu_system_load_state(void);
u64 emu_system_steps(void);
u32 emu_system_pc(void);
/* Dev log: ring buffer + optional file. L / F8 in the Win32 frontend shows it. */
enum {
 EMU_LOG_BOOT=1u<<0,
 EMU_LOG_HOST=1u<<1,
 EMU_LOG_STATE=1u<<2,
 EMU_LOG_MOVIE=1u<<3,
 EMU_LOG_MEDIA=1u<<4,
 EMU_LOG_AUDIO=1u<<5,
 EMU_LOG_STORAGE=1u<<6,
 EMU_LOG_PROBE=1u<<7,
 EMU_LOG_OTHER=1u<<8,
 EMU_LOG_FW=1u<<9,
 EMU_LOG_INPUT=1u<<10,
 EMU_LOG_SYSTEM=EMU_LOG_BOOT|EMU_LOG_STATE|EMU_LOG_OTHER,
 EMU_LOG_ALL=0x7ffu
};
#define EMU_LOG_RING_LINES 512u
void emu_system_log_set_path(const char*path);
void emu_system_log_set_notify(void (*fn)(void));
void emu_system_log(const char*msg);
void emu_system_log_kind(u32 kind,const char*msg);
u32 emu_system_log_seq(void);
u32 emu_system_log_ring_lines(void);
/* Copy the line written when seq became this value; 0 if aged out of the ring. */
int emu_system_log_get(u32 seq,char*dst,u32 cap);
u32 emu_system_log_line_kind(u32 seq);
u32 emu_system_log_dump(char*dst,u32 cap);
u32 emu_system_log_dump_filtered(u32 mask,char*dst,u32 cap);
u32 emu_system_movie_frames(void);
/* Release host movie process/pipes when closing the native frontend. */
void emu_system_shutdown(void);

#if defined(EMU_TESTING) || defined(EMU_RELEASE_PROBE)
u32 emu_debug_movie_stats(u32*dst);
u32 emu_debug_state(void);
u32 emu_debug_dialog_handle(void);
void emu_debug_write8(u32 a,u32 v);
void emu_debug_write32(u32 a,u32 v);
u32 emu_debug_read32(u32 a);
int emu_debug_call(u32 addr,u32 r0,u32 r1,u32 r2,u32 r3);
int emu_debug_system_boot(u32 max_steps);
int emu_debug_system_run(u32 max_steps);
void emu_debug_touch_set(u32 x,u32 y,u32 down);
void emu_debug_set_touch_autocal(u32 enabled);
void emu_debug_set_keypad_pair(u32 vk,u32 drive_pin,u32 read_pin);
u32 emu_debug_touch_down(void);
u32 emu_debug_pc(void);
u64 emu_debug_steps(void);
u32 emu_debug_firmware_payload_offset(void);
u32 emu_debug_reg(int index);
void emu_debug_set_reg(int index,u32 value);
u32 emu_debug_cpsr(void);
void emu_debug_set_cpsr(u32 value);
const char* emu_debug_semihost_log(void);
int emu_debug_trace_copy(u32*dst,int cap);
u32 emu_debug_timer3_reload(void);
u32 emu_debug_timer3_counter(void);
u32 emu_debug_timer3_fires(void);
u32 emu_debug_irq_entries(void);
u32 emu_debug_audio_dma_events(void);
u32 emu_debug_audio_dma_addr(void);
u32 emu_debug_audio_dma_words(void);
u32 emu_debug_audio_dma_bytes(void);
u32 emu_debug_audio_dma_signature(void);
u32 emu_debug_audio_command(void);
u32 emu_debug_audio_reg(u32 command);
u32 emu_debug_audio_dma_active(void);
u32 emu_debug_audio_dma_remaining_words(void);
u32 emu_debug_audio_dma_completions(void);
u32 emu_debug_audio_irq_pending(void);
u32 emu_debug_audio_irq_acks(void);
u32 emu_debug_audio_irq_entries(void);
u32 emu_debug_audio_last_event(void);
u32 emu_debug_audio_sample_rate(void);
u32 emu_debug_audio_channels(void);
u32 emu_debug_audio_bits_per_sample(void);
u32 emu_debug_audio_clock_valid(void);
void emu_debug_audio_complete(void);
void emu_debug_audio_advance_us(u64 elapsed_us);
int emu_debug_audio_copy(u8*dst,u32 cap);
u32 emu_debug_context_switches(void);
u32 emu_debug_vflash_reads(void);
u32 emu_debug_vflash_writes(void);
u64 emu_debug_vflash_last_off(void);
u32 emu_debug_vflash_last_len(void);
u32 emu_debug_vflash_overlay_pages(void);
void emu_debug_set_vflash_cache(u32 enabled);
u32 emu_debug_vflash_cache_hits(void);
u32 emu_debug_vflash_cache_misses(void);
u32 emu_debug_vflash_op_count(void);
int emu_debug_vflash_op(u32 back,u64*off,u32*len,u32*wr);
u32 emu_debug_scheduler_selects(void);
u32 emu_debug_task_select_count(int index);
u32 emu_debug_task_changes(void);
u64 emu_debug_idle_fast_forwarded_steps(void);
u32 emu_debug_idle_fast_forward_events(void);
void emu_debug_set_render_fastpaths(u32 enabled);
u64 emu_debug_render_fast_pixels(void);
u32 emu_debug_render_fast_events(void);
u32 emu_debug_delay1m_fast_events(void);
u32 emu_debug_nand_cmd_count(void);
u32 emu_debug_nand_last_cmd0(void);
u32 emu_debug_nand_last_ctrl(void);
u32 emu_debug_nand_last_col(void);
u32 emu_debug_nand_last_row(void);
u32 emu_debug_nand_read_commands(void);
u32 emu_debug_system_data_base(void);
u32 emu_debug_system_data_len(void);
u32 emu_debug_system_data_magic_ok(void);
u32 emu_debug_program_data_base(void);
u32 emu_debug_program_data_len(void);
u32 emu_debug_program_data_magic_ok(void);
u32 emu_debug_storage_hle_calls(void);
u32 emu_debug_ftl_bypass_calls(void);
u32 emu_debug_system_file_opens(void);
u32 emu_debug_system_file_reads(void);
u32 emu_debug_file_open_attempts(void);
u32 emu_debug_last_file_rid(void);
void emu_debug_set_rid15_bridge(u32 enabled);
u32 emu_debug_res_op_count(void);
int emu_debug_res_op(u32 back,u32*out6);
u32 emu_debug_file_rid_count(u32 rid);
u32 emu_debug_rid_open_success(u32 rid);
u32 emu_debug_rid_open_fail(u32 rid);
u32 emu_debug_rid_first_handle(u32 rid);
u32 emu_debug_rid_first_lr(u32 rid);
u32 emu_debug_update_status(void);
u32 emu_debug_update_status_hits(void);
u32 emu_debug_startup_last_milestone(void);
u32 emu_debug_startup_milestone_count(u32 i);
u32 emu_debug_startup_syscheck(void);
u32 emu_debug_touch_autocal_samples(void);
u32 emu_debug_touch_autocal_coord(u32 i);
u32 emu_debug_lcd_writes(void);
u32 emu_debug_lcd_last(void);
int emu_debug_lcd_ring_copy(u32*dst,int cap);
u32 emu_debug_lcd_dma_transfers(void);
u32 emu_debug_lcd_dma_pixels(void);
u16 emu_debug_lcd_pixel565(u32 x,u32 y);
u32 emu_debug_lcd_dma_event_count(void);
int emu_debug_lcd_dma_event(u32 back,u32*out8);
u32 emu_debug_lcd_full_frame_count(void);
u16 emu_debug_lcd_full_frame_pixel(u32 frame,u32 x,u32 y);
u32 emu_debug_desktop_obj(void);u32 emu_debug_desktop_hwnd(void);u32 emu_debug_desktop_cb(void);u32 emu_debug_desktop_msg_count(void);int emu_debug_desktop_msg(u32 i,u32*out5);
u32 emu_debug_win_create_count(void);int emu_debug_win_create(u32 i,u32*out12);
u32 emu_debug_bitmap_decode_count(void);int emu_debug_bitmap_decode(u32 i,u32*out5);
u32 emu_debug_bitmap_header_count(void);int emu_debug_bitmap_header(u32 i,u32*out9);
#endif /* EMU_TESTING */

#endif /* HJX_EMU_CORE_H */
