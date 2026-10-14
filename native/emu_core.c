#include <stdio.h>
#include <stdlib.h>
#ifdef ANDROID
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#endif
#if defined(ANDROID) && defined(HJX_INPROCESS_MOVIE)
#include <time.h>
#endif
#include "emu_core.h"
#include "emu_profile.h"
#ifdef HJX_INPROCESS_MOVIE
#include "movie_ffmpeg_bridge.h"
#endif

#define VBASE 0x30000000u
#define FW_MEM_SIZE 0x01000000u
#define RAM2_BASE 0x20000000u
#define RAM2_SIZE 0x01000000u
#define RAM3_BASE 0x31000000u
#define RAM3_SIZE 0x03000000u
#define SRAM4_BASE 0x40000000u
#define SRAM4_SIZE 0x00100000u
#define MMIO7_BASE 0x70000000u
#define MMIO7_SIZE 0x00100000u
#define WINDOW 0x32000000u
#define SENTINEL 0x33FFE000u
#define HANDLE_BASE 0x33000000u
#define WM_PAINT 0x000Fu
#define WM_KEYDOWN 0x0100u
#define WM_TIMER 0x0113u
#define MAX_OBJECTS 1024
#define MAX_TIMERS 16
#define RES_DESC_TAG 0x003Cu
#define LCD_W 240u
#define LCD_H 320u

extern const u8 _binary_fw_bin_start[], _binary_fw_bin_end[];
extern const u8 _binary_font96_bin_start[], _binary_font96_bin_end[];
extern const u8 _binary_font100_bin_start[], _binary_font100_bin_end[];

static u8 fwmem[FW_MEM_SIZE];
static u8 ram2[RAM2_SIZE];
static u8 ram3[RAM3_SIZE];
static u8 sram4[SRAM4_SIZE];
static u8 mmio7[MMIO7_SIZE];
static u32 framebuffer[FB_W*FB_H];
static int g_error; static u32 g_error_pc;
static u32 g_bad_mem_addr,g_bad_mem_origin_pc,g_bad_mem_origin_sp,g_bad_mem_origin_lr;
static u32 g_current_cpu_pc,g_current_cpu_sp,g_current_cpu_lr;
static int invalidated, erase_pending;
static u32 next_alloc, next_handle;
static u32 firmware_payload_offset;
static u32 system_irq_enabled;
/* AK322x timer/interrupt model discovered from the V1+ firmware:
   timer channel 3: 0x20090114, raw IRQ status bit 24, controller mask bit 9, logical IRQ 24. */
typedef struct{u32 enabled,reload,counter,fires,irq_entries;}HwTimer3;
static HwTimer3 timer3;
/* AK322x RTC block recovered from the original V1+ firmware.
   The chip exposes a raw 32-bit second counter, not BCD calendar fields:
     0x200D0000  write current seconds
     0x200D0004  write alarm seconds
     0x200D0010  control/status (bit 4 = ready, bits 6/7 = busy)
     0x200D0014  read live current seconds
   The firmware converts this counter using an epoch of 1980-01-01. */
#define RTC_DEFAULT_SECONDS 0x368d6180u /* 2009-01-01 00:00:00 from 1980 epoch */
typedef struct{
 u32 fallback_seconds,host_seconds,host_valid,offset_valid;
 i64 host_offset;
 u32 alarm_seconds,control;
}HwRtc;
static HwRtc rtc;
/* AK322x audio controller recovered from the original V1+ firmware.

   The command/data aperture is 0x20050020/0x20050024.  The original
   low-level read routine (0x302397BC) selects a 0x7Fxx register through
   +0x20 and reads its 16-bit value through +0x24; the write routine
   (0x302397F4) uses the same selector and writes +0x24.

   Playback DMA submission (0x30207DCC) programs:
     0x7F14 = source address low 16 bits
     0x7F15 = source address high 16 bits
     0x7F11 = transfer length in 16-bit words (64-word aligned)

   The audio interrupt is logical IRQ5.  Firmware registers ISR 0x30190F90
   with RegisterIRQ(5, ...).  On playback completion, 0x7F01 reports event
   0x76; that ISR path calls 0x301E5044 -> 0x30207DCC to submit the next PCM
   buffer.  Reading 0x20050014 is the controller status/ACK read performed
   at the start of the ISR.

   v0.8.12 additionally recovers the active V1+ variant-5 sample clock and
   advances playback DMA from elapsed time.  The completion path therefore
   raises the same 0x76/IRQ5 event automatically when the programmed transfer
   expires; the v0.8.9 PCM capture observer remains available. */
#define AUDIO_PROBE_MAX_BYTES 16384u
#define AUDIO_IRQ_BIT 0x00000020u
typedef struct{
 u32 command,value;
 u16 regs[256];
 u32 dma_addr_lo,dma_addr_hi,dma_addr,dma_words,dma_remaining_words,dma_active;
 u32 dma_events,dma_completions,captured_bytes,signature;
 u32 irq_pending,irq_acks,irq_entries,last_event;
 u32 submit_log_addr[16],submit_log_words[16],submit_log_nz[16],submit_log_sig[16];
 u32 first_sound_bytes;
 u8 first_sound[AUDIO_PROBE_MAX_BYTES];
 u32 vf_reads,vf_large,vf_dma_hits;
 u64 vf_off[8];
 u32 vf_dest[8],vf_len[8],vf_nz[8],vf_lr[8];
 /* V1+ variant-5 playback clock.  The original firmware's 0x30208108
    accepts only stereo/16-bit and dispatches the requested sample rate to
    variant-5 clock programmer 0x30239E50.  That routine encodes one of its
    recovered rate-table entries into 0x20090028. */
 u32 sample_rate,channels,bits_per_sample,clock_valid;
 u64 dma_word_fraction; /* pending word time, including carry across DMA IRQ */
 u32 dma_clock_running,dma_gap_us;
 u8 captured[AUDIO_PROBE_MAX_BYTES];
}HwAudioDevice;
static HwAudioDevice audio_probe;
/* The host consumes only samples actually transferred by the emulated DAC.
   Single-threaded with the CPU; bounded to avoid latency growth on a slow sink. */
#define AUDIO_FIFO_BYTES 65536u
static u8 audio_fifo[AUDIO_FIFO_BYTES];
static u32 audio_fifo_read,audio_fifo_count,audio_fifo_rate,audio_fifo_epoch;
static u32 audio_output_muted;
static void memcopy(void*d0,const void*s0,u32 n);
static void audio_output_reset(void){audio_fifo_read=audio_fifo_count=audio_fifo_rate=0;audio_fifo_epoch++;}
static void audio_fifo_copy_in(const u8*src,u32 n){
 u32 pos=(audio_fifo_read+audio_fifo_count)%AUDIO_FIFO_BYTES;
 u32 first=AUDIO_FIFO_BYTES-pos;if(first>n)first=n;
 memcopy(audio_fifo+pos,src,first);
 if(n>first)memcopy(audio_fifo,src+first,n-first);
}
static void audio_fifo_copy_out(u8*dst,u32 n){
 u32 first=AUDIO_FIFO_BYTES-audio_fifo_read;if(first>n)first=n;
 memcopy(dst,audio_fifo+audio_fifo_read,first);
 if(n>first)memcopy(dst+first,audio_fifo,n-first);
}
static void audio_output_push(const u8*p,u32 n,u32 rate){
 if(audio_output_muted)return;
 if(rate!=audio_fifo_rate){audio_output_reset();audio_fifo_rate=rate;}
 if(n>AUDIO_FIFO_BYTES){p+=n-AUDIO_FIFO_BYTES;n=AUDIO_FIFO_BYTES;}
 if(n>AUDIO_FIFO_BYTES-audio_fifo_count){u32 drop=n-(AUDIO_FIFO_BYTES-audio_fifo_count);audio_fifo_read+=drop;if(audio_fifo_read>=AUDIO_FIFO_BYTES)audio_fifo_read-=AUDIO_FIFO_BYTES;audio_fifo_count-=drop;}
 audio_fifo_copy_in(p,n);
 audio_fifo_count+=n;
}
u32 emu_system_audio_read(u8*dst,u32 cap,u32*rate,u32*epoch){
 u32 n=audio_fifo_count;if(n>cap)n=cap;n&=~3u;
 if(rate)*rate=audio_fifo_rate;if(epoch)*epoch=audio_fifo_epoch;
 if(dst&&n){audio_fifo_copy_out(dst,n);audio_fifo_read+=n;if(audio_fifo_read>=AUDIO_FIFO_BYTES)audio_fifo_read-=AUDIO_FIFO_BYTES;audio_fifo_count-=n;}
 return n;
}
static u32 nand_sram[1024];
static u32 nand_cmd_count,nand_last_cmd0,nand_last_ctrl;
static FILE*nand_data_file;static char nand_data_path[512];
static char system_state_path[512];
static u32 system_state_dirty;
static u8 program_external[FW_MEM_SIZE];static u32 program_external_size;static int program_external_active;
/* V1PRU is the host copy of the firmware's @OZINGSYSTEMDATA resource, not a
   raw 512 MiB NAND image.  Keep its container offset/length separate from
   the physical NAND model so system-file bridging never fabricates OOB/FTL. */
static u32 system_data_base,system_data_len,system_data_magic_ok;
static u32 program_data_base,program_data_len,program_data_magic_ok;
/* Writable logical resources above the NAND FTL. RID133 has a fixed-size
   allocation in the firmware table; an erased allocation must still open so
   the firmware validates its marker and initializes its own settings table.
   RID138 remains absent until the original touch calibration creates it. */
static u8 sys_nv133[0x4000],sys_nv138[0x100];
static u32 sys_nv133_len,sys_nv138_len,sys_nv133_present,sys_nv138_present;
static u32 nand_last_col,nand_last_row,nand_read_commands;
static u32 debug_context_switches,debug_scheduler_selects;
static u32 debug_vflash_reads,debug_vflash_writes;
static u64 debug_vflash_last_off;static u32 debug_vflash_last_len;
typedef struct{u64 off;u32 len,wr;}DebugVfOp;static DebugVfOp debug_vf_ops[256];static u32 debug_vf_pos;
#define VFLASH_PAGE 4096u
#define VFLASH_OVERLAY_PAGES 1024u
typedef struct{u32 base,used;u8 data[VFLASH_PAGE];}VfOverlayPage;
static VfOverlayPage vflash_overlay[VFLASH_OVERLAY_PAGES];static u32 vflash_overlay_count;
/* Read-only host cache for the canonical V1PRU-backed logical flash.  The
   original VFS performs thousands of tiny repeated reads while repainting
   controls (calculator digits are a good example).  Reissuing fseek/fread for
   every 5/48/240-byte guest read is especially expensive on Win32.  Cache
   immutable 4 KiB base pages below the guest VFS; writable state still goes
   through vflash_overlay and therefore remains fully guest-visible. */
#define VFLASH_RO_CACHE_SLOTS 1024u /* 4 MiB direct-mapped cache */
typedef struct{u32 base,valid;u8 data[VFLASH_PAGE];}VfRoCachePage;
static VfRoCachePage vflash_ro_cache[VFLASH_RO_CACHE_SLOTS];
static u32 vflash_ro_cache_enabled=1u,vflash_ro_cache_hits,vflash_ro_cache_misses;
static u32 debug_storage_hle_calls,debug_ftl_bypass_calls,debug_system_file_opens,debug_system_file_reads;
static u32 debug_file_open_attempts,debug_last_file_rid,debug_file_rid_counts[256];
static u32 debug_rid_open_success[1024],debug_rid_open_fail[1024],debug_rid_first_handle[1024],debug_rid_first_lr[1024];
typedef struct{u32 sp,rid,mode,lr;}PendingOpen;static PendingOpen debug_pending_open[32];
typedef struct{u32 api,rid,mode,lr,arg2,handle;}DebugResOp;
static DebugResOp debug_res_ops[512];
static u32 debug_res_op_pos;
/* RID15 is an installed-data gate/resource on real flash.  Keep the old
   compatibility bridge switchable so update-mode experiments can observe
   what original firmware does when installed data is genuinely absent. */
static u32 debug_rid15_bridge=1u;
static u32 debug_update_status,debug_update_status_hits;
static u32 debug_touch_autocal_samples,debug_touch_autocal_xy[8];
static u32 virtual_touch_x,virtual_touch_y,virtual_touch_down;
static u32 virtual_touch_adc_x_reads,virtual_touch_adc_y_reads,virtual_touch_down_accepted,virtual_touch_up_seen;
static u32 virtual_touch_sample_x,virtual_touch_sample_y,virtual_touch_sample_active;
static u32 virtual_touch_calibration_active,virtual_touch_calibration_x,virtual_touch_calibration_y;
/* Host edges the firmware has not sampled yet.  Idle fast-forward must stop
   on the next scheduler tick after one of these arrives, instead of burning
   the rest of the host slice across later Timer3 quanta. */
static u32 touch_edge_seq,touch_edge_acked,key_edge_seq,key_edge_acked,key_contact_sampled;
static u32 trace_gpio_release,trace_touch_wake,trace_pen_clear,trace_frame;
static u32 idle_slice_quanta,idle_slice_had_edge;
/* Win32 enables this.  Idle may fast-forward only the Timer3 counts that
   wall time has earned, so the firmware auto-off timer stays on real time.
   0x668A0 = 420000 counts; native 0x30193068 reports 200 ticks/second.
   The former 12 MHz assumption made every idle sleep seven times too long.
   Tests never add credit and keep the original unlimited fast-forward. */
static u64 idle_time_credit;
static u32 idle_time_credit_on,idle_time_blocked,idle_gesture_active,idle_credit_key_edge;
static u32 cpu_run_depth,host_input_yield;
/* While a pen/key edge is still unsampled, the touch task is asleep for a
   fixed tick count.  At the former 35 ms/tick that wait caused multi-second U in
   v0.8.13.2.  Spend those ticks immediately, but only up to this cap, so a
   stuck edge cannot run the auto-off timer down.  160 ticks is 0.8 s
   of guest time; the scan itself returns well before that. */
static u64 idle_edge_burst_left=160ull*420000ull;
static u32 idle_edge_burst_touch,idle_edge_burst_key;
/* Virtual V1+ physical-key matrix.  The real Task Keypad drives one of GPIO
   14/13/3/11 and samples another through GPIO_GetLevel (0x30195190). */
static u32 virtual_key_vk,virtual_key_down;
static u32 keypad_pair_override_vk,keypad_pair_override_drive,keypad_pair_override_read;
/* Power button is not on the keypad matrix.  The system GPIO poll reads pin 7;
   a soft callback runs first, then 0x30195100 ("System Power Off") would spin. */
static u32 virtual_power_down,virtual_power_cut,virtual_power_sampled;
#ifdef EMU_TESTING
static u32 system_touch_autocal_enabled=1u;
#else
static u32 system_touch_autocal_enabled=0u;
#endif
static u32 debug_startup_milestone_counts[64],debug_startup_last_milestone,debug_startup_syscheck;
static u32 debug_lcd_writes,debug_lcd_last,debug_lcd_ring[64],debug_lcd_ring_pos;
typedef struct{u16 regs[256];u16 command;u16 x,y,x0,x1,y0,y1;u32 dma_transfers,dma_pixels;}HwLcd;
typedef struct{u32 src,w,h,x0,y0,x1,y1,sig;}DebugLcdDma;
static HwLcd lcd;
static u16 lcd_panel565[LCD_W*LCD_H];
/* Once the original GUI enables its primary 240x320 surface, AK322x display
   scanout continuously reflects that fully composed surface.  Partial LCD/DMA
   operations still occur, but popup/list layers may be composed into primary
   without a matching LCD-DMA rectangle.  Keep the panel shadow coherent with
   primary after the first full-screen primary scanout. */
static u32 lcd_primary_scanout_active;
/* Host-facing scanout bookkeeping.  The original GUI writes the primary
   framebuffer directly between LCD DMA operations.  Older builds copied the
   entire 240x320 surface after every emu_system_run() call, even when no pixel
   changed.  Track guest writes instead so scanout work is proportional to real
   display changes. */
static u32 lcd_primary_dirty;
static u32 lcd_generation;
static DebugLcdDma debug_lcd_dma[64];static u32 debug_lcd_dma_pos;
static u16 debug_lcd_full_frames[8][LCD_W*LCD_H];static u32 debug_lcd_full_count;
static u32 debug_task_select_count[32],debug_task_changes,debug_last_task;
static u64 debug_idle_fast_forwarded_steps;static u32 debug_idle_fast_forward_events;
static u32 render_fastpaths_enabled=1u;static u64 debug_render_fast_pixels;static u32 debug_render_fast_events,debug_delay1m_fast_events;
static u32 debug_desktop_obj,debug_desktop_hwnd,debug_desktop_cb,debug_desktop_msg_count;
typedef struct{u32 hwnd,msg,wparam,lparam,cb;}DebugDesktopMsg;static DebugDesktopMsg debug_desktop_msgs[64];
typedef struct{u32 sp,lr,a0,a1,a2,a3,s0,s1,s2,s3,s4,ret;}DebugWinCreate;
static DebugWinCreate debug_win_create[256];static u32 debug_win_create_count;
typedef struct{u32 sp,lr,rid,index,ret;}DebugBitmapDecode;
static DebugBitmapDecode debug_bitmap_decode[256];static u32 debug_bitmap_decode_count;
typedef struct{u32 off,sig,pixoff,dibsz,w,h,planes_bpp,comp,imgsz;}DebugBitmapHeader;
static DebugBitmapHeader debug_bitmap_headers[256];static u32 debug_bitmap_header_count;
static u32 cp15_regs[4096];
static char semihost_log[8192];
static u32 semihost_log_len;
static char semihost_line[192];
static u32 semihost_line_len;
static u32 debug_trace[256],debug_trace_pos;
static void semihost_emit_line(void){
 char out[208];u32 i=0,j=0;
 if(!semihost_line_len)return;
 out[i++]='f';out[i++]='w';out[i++]=':';out[i++]=' ';
 while(j<semihost_line_len&&i+1u<sizeof(out)){out[i++]=semihost_line[j++];}
 out[i]=0;emu_system_log_kind(EMU_LOG_FW,out);
 semihost_line_len=0;
}
static void semihost_putc(u8 c){
 if(c=='\r')return;
 if(semihost_log_len+1u<sizeof(semihost_log)){semihost_log[semihost_log_len++]=(char)c;semihost_log[semihost_log_len]=0;}
 if(c=='\n'){semihost_emit_line();return;}
 if(semihost_line_len+1u>=sizeof(semihost_line))semihost_emit_line();
 if(semihost_line_len+1u<sizeof(semihost_line))semihost_line[semihost_line_len++]=(char)c;
}
static EmuTextFallback text_fallback;

static void memzero(void*p,u32 n){u8*d=(u8*)p;while(n--)*d++=0;}
static void memcopy(void*d0,const void*s0,u32 n){u8*d=(u8*)d0;const u8*s=(const u8*)s0;while(n--)*d++=*s++;}
static u32 cstrlen32(u32 p){u32 n=0; while(n<0x10000u){u8*b; if(p>=VBASE&&p<VBASE+FW_MEM_SIZE)b=&fwmem[p-VBASE];else if(p>=RAM2_BASE&&p<RAM2_BASE+RAM2_SIZE)b=&ram2[p-RAM2_BASE];else if(p>=RAM3_BASE&&p<RAM3_BASE+RAM3_SIZE)b=&ram3[p-RAM3_BASE];else break;if(!*b)break;p++;n++;}return n;}
static u32 ror32(u32 v,u32 n){n&=31u;return n?((v>>n)|(v<<(32u-n))):v;}
static i32 s32(u32 x){return (i32)x;}
static void* memptr(u32 a,u32 n){
 if(a>=VBASE && a+n>=a && a+n<=VBASE+FW_MEM_SIZE)return fwmem+(a-VBASE);
 if(a>=RAM2_BASE && a+n>=a && a+n<=RAM2_BASE+RAM2_SIZE)return ram2+(a-RAM2_BASE);
 if(a>=RAM3_BASE && a+n>=a && a+n<=RAM3_BASE+RAM3_SIZE)return ram3+(a-RAM3_BASE);
 if(a>=SRAM4_BASE && a+n>=a && a+n<=SRAM4_BASE+SRAM4_SIZE)return sram4+(a-SRAM4_BASE);
 if(a>=MMIO7_BASE && a+n>=a && a+n<=MMIO7_BASE+MMIO7_SIZE)return mmio7+(a-MMIO7_BASE);
 if(!g_error){
  g_bad_mem_addr=a;g_bad_mem_origin_pc=g_current_cpu_pc;
  g_bad_mem_origin_sp=g_current_cpu_sp;g_bad_mem_origin_lr=g_current_cpu_lr;
 }
 g_error=2;g_error_pc=a;return 0;
}
static void* memptr_try(u32 a,u32 n){
 if(a>=VBASE && a+n>=a && a+n<=VBASE+FW_MEM_SIZE)return fwmem+(a-VBASE);
 if(a>=RAM2_BASE && a+n>=a && a+n<=RAM2_BASE+RAM2_SIZE)return ram2+(a-RAM2_BASE);
 if(a>=RAM3_BASE && a+n>=a && a+n<=RAM3_BASE+RAM3_SIZE)return ram3+(a-RAM3_BASE);
 if(a>=SRAM4_BASE && a+n>=a && a+n<=SRAM4_BASE+SRAM4_SIZE)return sram4+(a-SRAM4_BASE);
 if(a>=MMIO7_BASE && a+n>=a && a+n<=MMIO7_BASE+MMIO7_SIZE)return mmio7+(a-MMIO7_BASE);
 return 0;
}
static void nand_sram_fill_ff(void){for(u32 i=0;i<1024u;i++)nand_sram[i]=0xffffffffu;}
static void rtc_reset(void){memzero(&rtc,sizeof(rtc));rtc.fallback_seconds=RTC_DEFAULT_SECONDS;rtc.control=0x10u;}
static u32 rtc_current_seconds(void){
 i64 v=rtc.host_valid?((i64)rtc.host_seconds+rtc.host_offset):(i64)rtc.fallback_seconds;
 if(v<0)return 0u;
 if((u64)v>0xffffffffull)return 0xffffffffu;
 return (u32)v;
}
static void rtc_set_seconds(u32 seconds){
 if(seconds!=rtc_current_seconds()||(rtc.host_valid&&!rtc.offset_valid))system_state_dirty=1u;
 rtc.fallback_seconds=seconds;
 if(rtc.host_valid){rtc.host_offset=(i64)seconds-(i64)rtc.host_seconds;rtc.offset_valid=1u;}
}
static u32 audio_probe_sig(const u8*p,u32 n){u32 h=2166136261u;while(n--){h^=*p++;h*=16777619u;}return h;}
/* Recovered 0x30239E50 rate tables for the V1+ variant-5 audio clock.
   Each entry is the sample rate plus the two fields written into
   0x20090028: frac bits 18:16 and divisor bits 9:0.  0x20000034 bits 6:2
   select which parent-clock table the firmware uses: 0 -> 60 MHz,
   12 -> 84 MHz, otherwise 92 MHz. */
typedef struct{u32 rate;u16 div;u8 frac;}AudioRateCfg;
static const AudioRateCfg audio_rates_60[]={{12000,0x26,0},{11025,0x2b,6},{8000,0x38,2},{24000,0x12,2},{22050,0x15,6},{16000,0x1c,1},{48000,0x09,7},{44100,0x0a,6},{32000,0x0e,7},{96000,0x04,7},{88200,0x04,2},{64000,0x06,2}};
static const AudioRateCfg audio_rates_84[]={{12000,0x34,2},{11025,0x39,2},{8000,0x51,0},{24000,0x1a,1},{22050,0x1c,2},{16000,0x28,0},{48000,0x0d,7},{44100,0x0e,7},{32000,0x13,2},{96000,0x06,7},{88200,0x06,2},{64000,0x09,2}};
static const AudioRateCfg audio_rates_92[]={{12000,0x3a,1},{11025,0x40,0},{8000,0x56,2},{24000,0x1c,2},{22050,0x1f,1},{16000,0x2c,0},{48000,0x0e,0},{44100,0x0f,1},{32000,0x15,1},{96000,0x07,6},{88200,0x06,2},{64000,0x0a,1}};
static u32 audio_raw32(u32 a){u8*p=(u8*)memptr(a,4);return p?((u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24)):0u;}
static void audio_decode_clock(u32 reg28,u32 reg34){
 const AudioRateCfg*t;u32 n=(u32)(sizeof(audio_rates_60)/sizeof(audio_rates_60[0]));u32 sel=(reg34>>2)&0x1fu,parent_hz;
 if(sel==0u){t=audio_rates_60;parent_hz=60000000u;}else if(sel==12u){t=audio_rates_84;parent_hz=84000000u;}else{t=audio_rates_92;parent_hz=92000000u;}
 u32 frac=(reg28>>16)&7u,div=reg28&0x3ffu;
 /* Bit10 is asserted by the original clock routine after divider programming. */
 if(!(reg28&0x400u)){audio_probe.clock_valid=0u;return;}
 for(u32 i=0;i<n;i++)if((u32)t[i].frac==frac&&(u32)t[i].div==div){audio_probe.sample_rate=t[i].rate;audio_probe.channels=2u;audio_probe.bits_per_sample=16u;audio_probe.clock_valid=1u;return;}
 /* 0x30239E50's non-table fallback clears frac=0 and programs div as a
    rounded parent/(128*requested_rate)-1 value. Decode that generic mode
    from hardware fields too, so uncommon rates do not stall DMA. */
 if(frac==0u){u64 den=128ull*((u64)div+1ull);audio_probe.sample_rate=(u32)(((u64)parent_hz+den/2ull)/den);audio_probe.channels=2u;audio_probe.bits_per_sample=16u;audio_probe.clock_valid=audio_probe.sample_rate?1u:0u;return;}
 audio_probe.clock_valid=0u;
}
static void audio_raw_irq_set(int set){
 u8*p=(u8*)memptr(0x20000014u,4);if(!p)return;
 u32 v=(u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);
 if(set)v|=AUDIO_IRQ_BIT;else v&=~AUDIO_IRQ_BIT;
 p[0]=(u8)v;p[1]=(u8)(v>>8);p[2]=(u8)(v>>16);p[3]=(u8)(v>>24);
}
static void audio_raise_event(u32 event){
 audio_probe.regs[0x01]=(u16)event;audio_probe.last_event=event&0xffffu;
 audio_probe.irq_pending=1u;audio_raw_irq_set(1);
}
static void audio_probe_submit(u32 words){
 if(!words)audio_output_reset();
 u32 addr=(audio_probe.dma_addr_hi<<16)|(audio_probe.dma_addr_lo&0xffffu);
 u64 want64=(u64)words*2ull;u32 want=want64>AUDIO_PROBE_MAX_BYTES?AUDIO_PROBE_MAX_BYTES:(u32)want64;
 audio_probe.dma_words=words&0xffffu;audio_probe.dma_remaining_words=audio_probe.dma_words;audio_probe.dma_active=audio_probe.dma_words?1u:0u;
 if(!audio_probe.dma_clock_running||!words)audio_probe.dma_word_fraction=0u;
 audio_probe.dma_clock_running=audio_probe.dma_active;
 audio_probe.dma_gap_us=0;
 audio_probe.dma_addr=addr;audio_probe.dma_events++;audio_probe.captured_bytes=0;audio_probe.signature=0;
 if((audio_probe.dma_events-1u)<16u){u32 k=audio_probe.dma_events-1u;audio_probe.submit_log_addr[k]=addr;audio_probe.submit_log_words[k]=words&0xffffu;}
 if(!want)return;
 {u8*q=(u8*)memptr(addr,want);if(q){u32 nz=0;u32 i;memcopy(audio_probe.captured,q,want);audio_probe.captured_bytes=want;audio_probe.signature=audio_probe_sig(q,want);for(i=0;i<want;i++)if(q[i])nz++;if((audio_probe.dma_events-1u)<16u){audio_probe.submit_log_nz[audio_probe.dma_events-1u]=nz;audio_probe.submit_log_sig[audio_probe.dma_events-1u]=audio_probe.signature;}if(nz&&!audio_probe.first_sound_bytes){memcopy(audio_probe.first_sound,q,want);audio_probe.first_sound_bytes=want;}}}
}
static void audio_note_buffer(u32 addr,u32 words){
 u64 want64=(u64)words*2ull;u32 want=want64>AUDIO_PROBE_MAX_BYTES?AUDIO_PROBE_MAX_BYTES:(u32)want64;u8*q;u32 nz=0,i;
 if(!want)return;
 q=(u8*)memptr(addr,want);if(!q)return;
 for(i=0;i<want;i++)if(q[i])nz++;
 if(!nz||audio_probe.first_sound_bytes)return;
 memcopy(audio_probe.first_sound,q,want);audio_probe.first_sound_bytes=want;audio_probe.signature=audio_probe_sig(q,want);
}
static void audio_complete_playback(void){
 if(!audio_probe.dma_active)return;
 audio_note_buffer(audio_probe.dma_addr,audio_probe.dma_words);
 audio_probe.dma_active=0u;audio_probe.dma_remaining_words=0u;audio_probe.dma_completions++;
 audio_raise_event(0x76u);
}
static void audio_advance_us(u64 elapsed_us){
 if(!elapsed_us||!audio_probe.dma_clock_running||!audio_probe.clock_valid||!audio_probe.sample_rate)return;
 /* Keep elapsed sample time while the real firmware services IRQ5 and
    submits its next ring block. Dropping the remainder at each boundary
    loses ~10% of the stream with a 5-ms host wakeup. Never read ahead into
    a buffer the guest has not submitted. Bound catch-up after host stalls. */
 u64 words_per_sec=(u64)audio_probe.sample_rate*2ull;
 if(elapsed_us>100000ull)elapsed_us=100000ull;
 if(!audio_probe.dma_active){
  audio_probe.dma_gap_us+=(u32)elapsed_us;
  /* A short IRQ handoff belongs to the same stream; a long idle interval
     (e.g. between pronunciations) must not consume the next sound early. */
  if(audio_probe.dma_gap_us>20000u){audio_probe.dma_clock_running=0;audio_probe.dma_word_fraction=0;return;}
 }
 audio_probe.dma_word_fraction+=elapsed_us*words_per_sec;
 if(audio_probe.dma_word_fraction>words_per_sec*100000ull)audio_probe.dma_word_fraction=words_per_sec*100000ull;
 if(audio_probe.dma_active){
  u64 words=audio_probe.dma_word_fraction/1000000ull;
  u32 take=words>audio_probe.dma_remaining_words?audio_probe.dma_remaining_words:(u32)words;
  take&=~1u; /* complete stereo frames only */
  if(take){u32 addr=audio_probe.dma_addr+2u*(audio_probe.dma_words-audio_probe.dma_remaining_words);u8*p=(u8*)memptr(addr,take*2u);if(p)audio_output_push(p,take*2u,audio_probe.sample_rate);}
  audio_probe.dma_word_fraction-=(u64)take*1000000ull;
  audio_probe.dma_remaining_words-=take;
  if(!audio_probe.dma_remaining_words)audio_complete_playback();
 }
}
static void audio_probe_mmio(u32 a,u32 v){
 /* Variant-5 302398F4 pulses system reset bit 4 on playback close. */
 if(a==0x20000004u&&(v&0x10u)){
  audio_probe.dma_active=audio_probe.dma_remaining_words=audio_probe.dma_clock_running=0;audio_probe.dma_word_fraction=0;
  audio_probe.irq_pending=0;audio_raw_irq_set(0);audio_output_reset();return;
 }
 if(a==0x20050020u){audio_probe.command=v&0xffffu;return;}
 if(a!=0x20050024u)return;
 audio_probe.value=v;
 if((audio_probe.command&0xff00u)==0x7f00u)audio_probe.regs[audio_probe.command&0xffu]=(u16)v;
 switch(audio_probe.command&0xffffu){
  case 0x7f14u:audio_probe.dma_addr_lo=v&0xffffu;break;
  case 0x7f15u:audio_probe.dma_addr_hi=v&0xffffu;break;
  case 0x7f11u:audio_probe_submit(v);break;
  default:break;
 }
}
static u32 audio_probe_mmio_read(u32 a,u32 fallback){
 if(a==0x20050024u && (audio_probe.command&0xff00u)==0x7f00u)return audio_probe.regs[audio_probe.command&0xffu];
 if(a==0x20050014u){
  if(audio_probe.irq_pending){audio_probe.irq_pending=0u;audio_probe.irq_acks++;audio_raw_irq_set(0);}
  return fallback;
 }
 return fallback;
}
static int nand_open_data(void){
 if(nand_data_file)return 1;if(!nand_data_path[0])return 0;
 nand_data_file=fopen(nand_data_path,"rb");return nand_data_file?1:0;
}
static int c_nand_fill(u32 row);
static void nand_load_read(u32 col,u32 row){
 /* Data pages of the internal disk come from the configured local host directory. READ-ID and
    STATUS stay on the controller path above.  Pages outside that volume stay
    erased, which is what boot observed before a host folder was attached. */
 (void)col;nand_last_col=col;nand_last_row=row;nand_read_commands++;
 if(!c_nand_fill(row))nand_sram_fill_ff();
}
static u16 lcd_bus_decode(u32 v){u32 q=v&~0x00040000u;return (u16)(((q>>1)&0x00ffu)|((q>>2)&0xff00u));}
static void lcd_bus_write(u32 v){
 u16 x=lcd_bus_decode(v);
 debug_lcd_writes++;debug_lcd_last=v;debug_lcd_ring[debug_lcd_ring_pos++&63u]=v;
 if(!(v&0x00040000u)){lcd.command=x;return;}
 if(lcd.command==3u&&lcd.regs[3]!=x){char b[80];sprintf(b,"lcd: entry mode=%04X",x);emu_system_log(b);lcd_primary_dirty=1u;}
 lcd.regs[lcd.command&255u]=x;
 switch(lcd.command){case 0x20:lcd.x=x;break;case 0x21:lcd.y=x;break;case 0x50:lcd.x0=x;break;case 0x51:lcd.x1=x;break;case 0x52:lcd.y0=x;break;case 0x53:lcd.y1=x;break;default:break;}
}
static void lcd_mark_primary_dirty_range(u32 a,u32 n){
 const u64 b=0x30488624ull,e=b+(u64)LCD_W*(u64)LCD_H*2ull;
 u64 x=(u64)a,y=x+(u64)n;
 if(n&&x<e&&y>b)lcd_primary_dirty=1u;
}
static u8 mr8(u32 a){u8*p=(u8*)memptr(a,1);return p?*p:0;}
static u16 mr16(u32 a){u8*p=(u8*)memptr(a,2);return p?(u16)(p[0]|((u16)p[1]<<8)):0;}
static u32 mr32(u32 a){u8*p=(u8*)memptr(a,4);u32 v=p?((u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24)):0;
 /* Ordinary RAM/firmware reads cannot hit the peripheral registers below. */
 if((a&0xff000000u)!=RAM2_BASE)return v;
 /* AK322x interrupt status bit9 is the aggregate line for the high/special
    sources that the ROM dispatcher examines separately (16/17/18/19/21/24/26). */
 if(a==0x20000014u && (v&0x057f0000u))v|=0x00000200u;
 if(a==0x200d0014u)return rtc_current_seconds();
 if(a==0x200d0004u)return rtc.alarm_seconds;
 if(a==0x200d0010u)return (rtc.control|0x10u)&~0x000000c0u;
 if(a==0x20050014u||a==0x20050024u)return audio_probe_mmio_read(a,v);
 if(a==0x20010058u)v|=8u;if(a==0x2009000cu)v|=(1u<<25);
 return v;}
static void lcd_sync_primary_scanout(void){
 if(!lcd_primary_scanout_active||!lcd_primary_dirty)return;
 const u32 fb0=0x30488624u;u8*q=(u8*)memptr(fb0,LCD_W*LCD_H*2u);if(!q)return;
 /* Entry-mode AM switches the firmware work surface to 320x240.  D028 uses
    the same quarter-turn as TouchScreen's orientation 1: logical coordinates
    are (319 - physical_y, physical_x). Keep GRAM and touch hit testing aligned. */
 for(u32 y=0;y<LCD_H;y++)for(u32 x=0;x<LCD_W;x++){
  u32 i=(lcd.regs[3]&8u)?x*LCD_H+(LCD_H-1u-y):y*LCD_W+x;
  lcd_panel565[y*LCD_W+x]=(u16)(q[i*2u]|((u16)q[i*2u+1u]<<8));
 }
 lcd_primary_dirty=0u;lcd_generation++;trace_frame++;
}
static void lcd_dma_commit(void){
 u32 src=mr32(0x20010014u),shape=mr32(0x20010008u),w=shape&0xfffu,h=(shape>>12)&0xfffu;
 if(!w||!h||w>LCD_W||h>LCD_H)return;
 if(src==0x30488624u&&w==LCD_W&&h==LCD_H)lcd_primary_scanout_active=1u;
 else if(w==LCD_W&&h==LCD_H&&src!=0x304ade24u)lcd_primary_scanout_active=0u;
 /* AK322x LCD DMA targets the GRAM window programmed through 0x50..0x53.
    The V1+ firmware keeps two 240x320 RGB565 work surfaces back-to-back:
      primary 0x30488624, dirty/staging 0x304ADE24.

    On the real SoC a hardware copy/composition step feeds dirty rectangles to
    the LCD path.  We do not emulate that separate blitter yet.  CPU execution
    nevertheless leaves the primary surface fully composed, while the staging
    surface still contains stale/partial data.  Reading staging bytes directly
    caused long-press repaint rectangles to paste old desktop fragments over
    the live UI.  For a partial transfer whose programmed source is the staging
    surface, model the missing hardware copy by sourcing the matching rectangle
    from the composed primary surface. */
 u32 dx=0,dy=0,ww=(u32)lcd.x1-(u32)lcd.x0+1u,wh=(u32)lcd.y1-(u32)lcd.y0+1u;
 if(lcd.x1>=lcd.x0&&lcd.y1>=lcd.y0&&ww==w&&wh==h){dx=lcd.x0;dy=lcd.y0;}
 if(dx+w>LCD_W||dy+h>LCD_H){dx=0;dy=0;}
 {
  const u32 fb0=0x30488624u,fb1=0x304ade24u,fbbytes=LCD_W*LCD_H*2u;
  u8*q=0;u32 pitch=w,sx=0,sy=0;
  if(src==fb1 && (w!=LCD_W||h!=LCD_H)){
   q=(u8*)memptr(fb0,fbbytes);pitch=LCD_W;sx=dx;sy=dy;
  }else if(src==fb0 && (w!=LCD_W||h!=LCD_H)){
   q=(u8*)memptr(fb0,fbbytes);pitch=LCD_W;sx=dx;sy=dy;
  }else{
   q=(u8*)memptr(src,w*h*2u);pitch=w;
  }
  if(!q)return;
  u32 sig=2166136261u;
  for(u32 yy=0;yy<h;yy++)for(u32 xx=0;xx<w;xx++){
   u32 i=(sy+yy)*pitch+(sx+xx);
   if((lcd.regs[3]&8u)&&(src==fb0||src==fb1))i=(dx+xx)*LCD_H+(LCD_H-1u-(dy+yy));
   u16 px=(u16)(q[i*2u]|((u16)q[i*2u+1u]<<8));
   lcd_panel565[(dy+yy)*LCD_W+(dx+xx)]=px;
   if((((yy*w+xx)&127u)==0u)){sig^=(u8)px;sig*=16777619u;sig^=(u8)(px>>8);sig*=16777619u;}
  }
  DebugLcdDma*de=&debug_lcd_dma[debug_lcd_dma_pos++&63u];de->src=src;de->w=w;de->h=h;de->x0=dx;de->y0=dy;de->x1=dx+w-1u;de->y1=dy+h-1u;de->sig=sig;
  if(w==LCD_W&&h==LCD_H&&debug_lcd_full_count<8u){memcopy(debug_lcd_full_frames[debug_lcd_full_count],lcd_panel565,sizeof(lcd_panel565));debug_lcd_full_count++;}
 }
 if(src==0x30488624u&&w==LCD_W&&h==LCD_H)lcd_primary_dirty=0u;
 lcd_generation++;trace_frame++;lcd.dma_transfers++;lcd.dma_pixels+=w*h;
}
#ifdef AUDIO_TRACE
static u32 audio_ring_stores,audio_ring_nz_stores,audio_ring_store_pc,audio_ring_store_addr;
static u32 audio_cb_hits,audio_cb_r0,audio_cb_r1,audio_cb_r2,audio_cb_r3,audio_play_enter,audio_play_wait,audio_play_wait_r0,audio_play_post;
static u32 audio_sub_n,audio_sub_r0[8],audio_sub_r1[8],audio_sub_from[8];
static u32 audio_memcpy_ring;
static u32 audio_mix_enter,audio_mix_copy,audio_mix_dst,audio_mix_src,audio_mix_len,audio_qpush,audio_qpush_src,audio_qpush_len,audio_drv_a,audio_drv_b;
static u32 audio_mix_empty,audio_mix_at,audio_mix_r9,audio_mix_bad,audio_mix_r5,audio_mix_dest,audio_mix_give,audio_mix_a0;
static u32 audio_link_ret_n,audio_link_ret,audio_link_fill,audio_link_ind,audio_link_fn,audio_isr_cb,audio_isr_fn,audio_isr75,audio_cb_body,audio_cb_set;
static u32 audio_sig,audio_sig_drop8,audio_sig_drop5,audio_sig_f8,audio_sig_st,audio_waiter,audio_waiter_n,audio_thread,audio_thread_msg,audio_slot8_n,audio_slot8_nz;
static u32 audio_cmd_count[16],audio_cmd_lr[16],audio_cmd7_fn,audio_cmd7_obj,audio_dma_start_lr;
static void audio_ring_watch(u32 a,u32 v);
#endif
static void mw8(u32 a,u32 v){u8*p=(u8*)memptr(a,1);if(p){lcd_mark_primary_dirty_range(a,1u);
#ifdef AUDIO_TRACE
 audio_ring_watch(a,v);
#endif
 *p=(u8)v;}}
static void mw16(u32 a,u32 v){u8*p=(u8*)memptr(a,2);if(p){lcd_mark_primary_dirty_range(a,2u);
#ifdef AUDIO_TRACE
 audio_ring_watch(a,v);
#endif
 p[0]=(u8)v;p[1]=(u8)(v>>8);}}
static void mw32(u32 a,u32 v){
#ifdef AUDIO_TRACE
 audio_ring_watch(a,v);
#endif
 lcd_mark_primary_dirty_range(a,4u);
 audio_probe_mmio(a,v);
 if(a==0x20010050u)lcd_bus_write(v);
 if(a==0x20010054u&&(v&4u))lcd_dma_commit();
 /* Interrupt status bit9 is derived by hardware from the high source bits;
    don't latch a firmware read-modify-write copy of that summary bit. */
 if(a==0x20000014u)v&=~0x00000200u;
 /* AK322x clock/PLL status: firmware writes the control word and then polls
    bit 31 of 0x20088158 until the hardware reports the transition complete. */
 if(a==0x20088158u){
  /* NAND command engine.  READ-ID is a short descriptor list beginning with
     0x00048064; the controller returns the four ID bytes through +0x150.
     V1+ firmware has a dedicated geometry entry for AD DC 80 95
     (0x9580DCAD, 2 KiB pages, 64 pages/block, 4096 blocks = 512 MiB). */
  u8*cb=(u8*)memptr(0x20088100u,4);
  u32 c0=cb?((u32)cb[0]|((u32)cb[1]<<8)|((u32)cb[2]<<16)|((u32)cb[3]<<24)):0;
  nand_cmd_count++;nand_last_cmd0=c0;nand_last_ctrl=v;
  if(c0==0x00048064u){
   u8*rp=(u8*)memptr(0x20088150u,4);u32 id=0x9580dcadu;
   if(rp){rp[0]=(u8)id;rp[1]=(u8)(id>>8);rp[2]=(u8)(id>>16);rp[3]=(u8)(id>>24);}
  }else if(c0==0x00038064u){
   /* NAND STATUS: ready + previous operation passed. */
   u8*rp=(u8*)memptr(0x20088150u,4);u32 st=0x40u;
   if(rp){rp[0]=(u8)st;rp[1]=rp[2]=rp[3]=0;}
  }else if((c0&0x7ffu)==0x64u && ((c0>>11)&0xffu)==0x00u){
   /* READ0: two column bytes + three row bytes + READSTART(0x30). */
   u32 a1=mr32(0x20088104u),a2=mr32(0x20088108u),a3=mr32(0x2008810cu),a4=mr32(0x20088110u),a5=mr32(0x20088114u),c6=mr32(0x20088118u);
   if((a1&0x7ffu)==0x62u&&(a2&0x7ffu)==0x62u&&(a3&0x7ffu)==0x62u&&(a4&0x7ffu)==0x62u&&(a5&0x7ffu)==0x62u&&(c6&0x7ffu)==0x64u&&((c6>>11)&0xffu)==0x30u){
    u32 col=((a1>>11)&0xffu)|(((a2>>11)&0xffu)<<8);
    u32 row=((a3>>11)&0xffu)|(((a4>>11)&0xffu)<<8)|(((a5>>11)&0xffu)<<16);
    nand_load_read(col,row);
   }
  }
  v|=0x80000000u;
 }
 /* RTC set-time/alarm writes complete synchronously in the emulator.  The
    original firmware polls status busy bits 6/7; keep them hardware-owned and
    clear while reporting ready bit 4. */
 if(a==0x200d0000u)rtc_set_seconds(v);
 if(a==0x200d0004u)rtc.alarm_seconds=v;
 if(a==0x200d0010u){v=(v|0x00000010u)&~0x000000c0u;rtc.control=v;}
 /* Firmware programs timer 3 with 0x800668A0. 0x668A0 is exactly
    420000 counter clocks; the high bit arms/re-arms this one-shot.
    The IRQ handler re-arms it, making the RTOS system tick periodic. */
 if(a==0x20090114u){
  timer3.reload=v&0x00ffffffu;
  if(v&0x80000000u){timer3.enabled=timer3.reload!=0u;timer3.counter=timer3.reload;}
  else timer3.enabled=0;
 }
 /* Variant-5 audio sample clock. Decode only the hardware register fields
    written by the original driver, never an application-level play API. */
 if(a==0x20090028u)audio_decode_clock(v,audio_raw32(0x20000034u));
 else if(a==0x20000034u)audio_decode_clock(audio_raw32(0x20090028u),v);
 /* NAND data aperture. Firmware walks the controller SRAM one 32-bit word
    at a time by writing 0x2xxxxxxx (read) or 0x3xxxxxxx (write) to
    0x20088000; bits 21.. encode the word slot. Hardware clears bit29
    on completion and moves data through +0x14/+0x10. */
 if(a==0x20088000u && (v&0xf0000000u)==0x20000000u){
  u32 idx=(v>>21)&0x3ffu;u32 d=nand_sram[idx];
  u8*dp=(u8*)memptr(0x20088014u,4);if(dp){dp[0]=(u8)d;dp[1]=(u8)(d>>8);dp[2]=(u8)(d>>16);dp[3]=(u8)(d>>24);}
  v&=~0x20000000u;
 }else if(a==0x20088000u && (v&0xf0000000u)==0x30000000u){
  u32 idx=(v>>21)&0x3ffu;u32 d=mr32(0x20088010u);nand_sram[idx]=d;v&=~0x20000000u;
 }
 {u8*p=(u8*)memptr(a,4);if(p){p[0]=(u8)v;p[1]=(u8)(v>>8);p[2]=(u8)(v>>16);p[3]=(u8)(v>>24);}}
}

/* ARM CPU */
typedef struct{
 u32 r[16];
 u32 bank_sp[6],bank_lr[6],spsr[6];
 u32 usr_r8_12[5],fiq_r8_12[5];
 u8 N,Z,C,V,T,I,F,mode;
 u64 steps;
} CPU;
static u32 web_sleep_ms;
static CPU cpu, suspended_cpu; static int suspended_valid,suspend_request;
#ifdef AUDIO_TRACE
static void audio_ring_watch(u32 a,u32 v){if(a<0x30480584u||a>=0x30488584u)return;audio_ring_stores++;if(v){if(!audio_ring_nz_stores){audio_ring_store_pc=cpu.r[15];audio_ring_store_addr=a;}audio_ring_nz_stores++;}}
#endif
static int mode_bank(u32 mode){switch(mode&31u){case 0x11:return 1;case 0x12:return 2;case 0x13:return 3;case 0x17:return 4;case 0x1b:return 5;case 0x10:case 0x1f:default:return 0;}}
static u32 cpu_cpsr(void){return ((u32)cpu.N<<31)|((u32)cpu.Z<<30)|((u32)cpu.C<<29)|((u32)cpu.V<<28)|((u32)cpu.I<<7)|((u32)cpu.F<<6)|((u32)cpu.T<<5)|(cpu.mode?cpu.mode:0x13u);}
static void cpu_switch_mode(u32 nm){u32 om=cpu.mode?cpu.mode:0x13u;int ob=mode_bank(om),nb=mode_bank(nm);if((om&31u)==0x11u&&(nm&31u)!=0x11u){for(int i=0;i<5;i++){cpu.fiq_r8_12[i]=cpu.r[8+i];cpu.r[8+i]=cpu.usr_r8_12[i];}}else if((om&31u)!=0x11u&&(nm&31u)==0x11u){for(int i=0;i<5;i++){cpu.usr_r8_12[i]=cpu.r[8+i];cpu.r[8+i]=cpu.fiq_r8_12[i];}}if(cpu.mode){cpu.bank_sp[ob]=cpu.r[13];cpu.bank_lr[ob]=cpu.r[14];}cpu.mode=(u8)(nm&31u);cpu.r[13]=cpu.bank_sp[nb];cpu.r[14]=cpu.bank_lr[nb];}
static void cpu_set_cpsr(u32 v){u32 nm=v&31u;if(nm!=cpu.mode)cpu_switch_mode(nm);cpu.N=(u8)(v>>31);cpu.Z=(u8)((v>>30)&1u);cpu.C=(u8)((v>>29)&1u);cpu.V=(u8)((v>>28)&1u);cpu.I=(u8)((v>>7)&1u);cpu.F=(u8)((v>>6)&1u);cpu.T=(u8)((v>>5)&1u);}
static u32 cpu_spsr(void){return cpu.spsr[mode_bank(cpu.mode)];}
static void cpu_set_spsr(u32 v){cpu.spsr[mode_bank(cpu.mode)]=v;}
static u32 regv(int n){return n==15?cpu.r[15]+4u:cpu.r[n];}
static void setreg(int n,u32 v){cpu.r[n]=v;if(n==15)cpu.r[15]&=~3u;}
static int cond_ok(u32 c){switch(c){case 0:return cpu.Z;case 1:return!cpu.Z;case 2:return cpu.C;case 3:return!cpu.C;case 4:return cpu.N;case 5:return!cpu.N;case 6:return cpu.V;case 7:return!cpu.V;case 8:return cpu.C&&!cpu.Z;case 9:return!cpu.C||cpu.Z;case 10:return cpu.N==cpu.V;case 11:return cpu.N!=cpu.V;case 12:return!cpu.Z&&cpu.N==cpu.V;case 13:return cpu.Z||cpu.N!=cpu.V;case 14:return 1;default:return 0;}}
static void set_nz(u32 v){cpu.N=(u8)(v>>31);cpu.Z=(u8)(v==0);}
static u32 add_flags(u32 a,u32 b,u32 carry){u64 f=(u64)a+b+carry;u32 r=(u32)f;cpu.C=(u8)(f>>32);cpu.V=(u8)(((a>>31)==(b>>31))&&((a>>31)!=(r>>31)));set_nz(r);return r;}
static u32 sub_flags(u32 a,u32 b,u32 borrow){u64 bb=(u64)b+borrow;u32 r=a-(u32)bb;cpu.C=(u8)((u64)a>=bb);cpu.V=(u8)(((a>>31)!=(((u32)bb)>>31))&&((a>>31)!=(r>>31)));set_nz(r);return r;}
static u32 shiftv(u32 v,u32 typ,u32 amount,int byreg,u32*outc){u32 old=cpu.C;v&=0xffffffffu;if(!amount){if(byreg||typ==0){*outc=old;return v;}if(typ==1){*outc=v>>31;return 0;}if(typ==2){*outc=v>>31;return (v&0x80000000u)?0xffffffffu:0;}*outc=v&1u;return (old<<31)|(v>>1);}if(typ==0){if(amount<32){*outc=(v>>(32-amount))&1u;return v<<amount;}if(amount==32){*outc=v&1u;return 0;}*outc=0;return 0;}if(typ==1){if(amount<32){*outc=(v>>(amount-1))&1u;return v>>amount;}if(amount==32){*outc=v>>31;return 0;}*outc=0;return 0;}if(typ==2){if(amount>=32){*outc=v>>31;return(v&0x80000000u)?0xffffffffu:0;}*outc=(v>>(amount-1))&1u;return(u32)(s32(v)>>amount);}amount&=31u;if(!amount){*outc=v>>31;return v;}*outc=(v>>(amount-1))&1u;return ror32(v,amount);}
static u32 op2(u32 w,u32*outc){if((w>>25)&1u){u32 imm=w&255u,rot=((w>>8)&15u)*2u,val=ror32(imm,rot);*outc=rot?(val>>31):cpu.C;return val;}u32 v=regv(w&15u),typ=(w>>5)&3u,amt;if((w>>4)&1u){amt=regv((w>>8)&15u)&255u;return shiftv(v,typ,amt,1,outc);}amt=(w>>7)&31u;return shiftv(v,typ,amt,0,outc);}
static u32 addr_shift(u32 w){u32 c;return shiftv(regv(w&15u),(w>>5)&3u,(w>>7)&31u,0,&c);}

/* HLE objects */
enum{OBJ_NONE,OBJ_DC,OBJ_BITMAP,OBJ_FILE,OBJ_FONT,OBJ_PEN};
typedef struct{u32 kind,selected,selected_font,selected_pen,framebuffer;const u8*data;u32 len,pos,rid,cap,writable,host_system_data,host_program_data,host_nv;i32 width,height;u32 font_flags,text_color,bk_color,bk_mode,pen_color,pen_width;i32 cur_x,cur_y,origin_x,origin_y;}Obj;
typedef struct{u32 active,window,id,interval,callback;}Timer;
typedef struct{u32 active,pending,handle,parent,callback,title_ptr,result,dc,internal,entry,style,flags,modeless,closing,selection; i32 left,top,right,bottom;}ChildWindow;
static Obj objects[MAX_OBJECTS];static Timer timers[MAX_TIMERS];static ChildWindow child;
static u32 fb_dc;
static u32 web_story_pending;
/* The standalone host supplies the RTOS popup menu used by the floor flyer. */
#define WEB_FLIGHT_MENU 0x3200e800u
static u32 web_flight_active,web_flight_prepared,web_flight_selection;
static u8 web_flight_labels[2][48];
static int web_flight_create(u32 descriptors,u32 count);
static int web_flight_key(u32 key);
static int web_flight_touch(u32 x,u32 y);
static u32 web_heap_addr[64],web_heap_size[64],web_heap_used[64];

/* Firmware RTOS system-metric table used by the original WM_NCCALCSIZE path. */
#define RTOS_METRICS 0x3044C2D0u
static i32 rtos_metric(u32 index){return index<44u?(i32)mr32(RTOS_METRICS+index*4u):0;}
static int child_client_rect(u32 hwnd,i32*l,i32*t,i32*r,i32*b){
 if(!child.active||hwnd!=child.handle)return 0;
 i32 ww=child.right-child.left,hh=child.bottom-child.top;
 i32 ll=0,tt=0,rr=ww,bb=hh;
 u32 flags=mr32(hwnd+0x14),style=mr32(hwnd+0x10);
 /* Original 0x30197810, WM_NCCALCSIZE(0x83), wParam == 0. */
 if(flags&1u){
  ll+=rtos_metric(7);rr-=rtos_metric(7);
  tt+=rtos_metric(43);bb-=rtos_metric(8);
 }else{
  /* Caption-style fallback decoded from the same firmware path. */
  if((style&0x01000000u)==0u&&(style&0x00800000u)){ll+=rtos_metric(5);rr-=rtos_metric(5);tt+=rtos_metric(6);bb-=rtos_metric(6);}
  if((style&0x00C00000u)==0x00C00000u)tt+=rtos_metric(4);
 }
 if(l)*l=ll;if(t)*t=tt;if(r)*r=rr;if(b)*b=bb;return 1;
}
static int web_text_height(Obj*dc);
static int web_text_width(Obj*dc,const u8*s,u32 n);
static int text_extent_bytes(Obj*dc,const u8*q,u32 n,i32*w,i32*h){
 if(!q){if(w)*w=0;if(h)*h=0;return 0;}
 i32 tw=0;u32 i=0;
 while(i<n&&q[i]){
  if(q[i]=='\r'||q[i]=='\n'){i++;continue;}
  if(q[i]>=0x81&&i+1<n){tw+=web_text_width(dc,q+i,n-i);i+=2;}else{tw+=web_text_width(dc,q+i,n-i);i++;}
 }
 if(w)*w=tw;if(h)*h=n?web_text_height(dc):0;return 1;
}

static Obj* getobj(u32 h){
 /* Real RTOS windows embed their DC at hwnd+0x88.  The host keeps the
    drawable DC as an Obj, so make the embedded address an exact alias. */
 if(child.active&&h==child.handle+0x88u&&child.dc>=HANDLE_BASE){u32 ci=(child.dc-HANDLE_BASE)/0x100u;return ci<MAX_OBJECTS&&objects[ci].kind?&objects[ci]:0;}
 if(h<HANDLE_BASE)return 0;u32 i=(h-HANDLE_BASE)/0x100u;if(i>=MAX_OBJECTS)return 0;return objects[i].kind?&objects[i]:0;
}
static u32 new_handle(u32 kind){u32 i;for(i=0;i<MAX_OBJECTS;i++)if(!objects[i].kind)break;if(i>=MAX_OBJECTS)return 0;u32 h=HANDLE_BASE+i*0x100u;memzero(&objects[i],sizeof(Obj));objects[i].kind=kind;mw32(h,kind);return h;}
static u32 desc_id(u32 p){return mr16(p)==RES_DESC_TAG?mr32(p+2):0xffffffffu;}
static void debug_res_log(u32 api,u32 rid,u32 mode,u32 lr,u32 arg2,u32 handle){
 DebugResOp*e=&debug_res_ops[debug_res_op_pos++&511u];e->api=api;e->rid=rid;e->mode=mode;e->lr=lr;e->arg2=arg2;e->handle=handle;
}
static void hret(u32 v){cpu.r[0]=v;cpu.r[15]=cpu.r[14]&~3u;}
static u32 stackv(u32 off){return mr32(cpu.r[13]+off);}

static int bmp_info(const u8*b,u32 len,int*w,int*h,int*off,int*stride,const u8**pal){if(!b||len<54||b[0]!='B'||b[1]!='M')return 0;u32 o=(u32)b[10]|((u32)b[11]<<8)|((u32)b[12]<<16)|((u32)b[13]<<24);i32 ww=(i32)((u32)b[18]|((u32)b[19]<<8)|((u32)b[20]<<16)|((u32)b[21]<<24));i32 hh=(i32)((u32)b[22]|((u32)b[23]<<8)|((u32)b[24]<<16)|((u32)b[25]<<24));u16 bpp=(u16)(b[28]|((u16)b[29]<<8));if(bpp!=8||ww<=0||hh==0)return 0;int ah=hh<0?-hh:hh;int st=(ww+3)&~3;if(o+(u32)st*(u32)ah>len)return 0;*w=ww;*h=hh;*off=(int)o;*stride=st;*pal=b+54;return 1;}
static u32 bmp_rgb(const u8*b,u32 len,int x,int y){int w,h,o,st;const u8*pal;if(!bmp_info(b,len,&w,&h,&o,&st,&pal)||x<0||x>=w||y<0||y>=(h<0?-h:h))return 0;int yy=h>0?(h-1-y):y;u8 ix=b[o+yy*st+x];const u8*p=pal+ix*4;return((u32)p[2]<<16)|((u32)p[1]<<8)|p[0];}
static u32 cref_to_rgb(u32 c){return((c&0xffu)<<16)|(c&0xff00u)|((c>>16)&0xffu);}
static void putpx(int x,int y,u32 rgb){if((unsigned)x<FB_W&&(unsigned)y<FB_H)framebuffer[y*FB_W+x]=rgb&0xffffffu;}
static void fillrect(Obj*dc,int l,int t,int r,int b,u32 rgb){if(!dc||!dc->framebuffer)return;l+=dc->origin_x;r+=dc->origin_x;t+=dc->origin_y;b+=dc->origin_y;if(l<0)l=0;if(t<0)t=0;if(r>FB_W)r=FB_W;if(b>FB_H)b=FB_H;for(int y=t;y<b;y++)for(int x=l;x<r;x++)putpx(x,y,rgb);}

#include "web_text.inc"

static void line(Obj*dc,int x0,int y0,int x1,int y1,u32 color){if(!dc||!dc->framebuffer)return;x0+=dc->origin_x;x1+=dc->origin_x;y0+=dc->origin_y;y1+=dc->origin_y;int dx=x1>x0?x1-x0:x0-x1,sx=x0<x1?1:-1,dy=-(y1>y0?y1-y0:y0-y1),sy=y0<y1?1:-1,err=dx+dy;for(;;){putpx(x0,y0,color);if(x0==x1&&y0==y1)break;int e2=2*err;if(e2>=dy){err+=dy;x0+=sx;}if(e2<=dx){err+=dx;y0+=sy;}}}

/* RTOS HLE. Return 1 if handled. */
static void web_notice_observe(u32 addr);
static void paint_notebook_selection(void);
static int hle(u32 a){u32 h,p,rid,mode;Obj*o,*d,*s;switch(a){
 case 0x3018F28Cu: case 0x3018F3C4u:{u32 sz=cpu.r[0];for(int i=0;i<64;i++)if(web_heap_addr[i]&&!web_heap_used[i]&&web_heap_size[i]>=sz){web_heap_used[i]=1;p=web_heap_addr[i];memzero(memptr(p,sz),sz);hret(p);return 1;}if(sz<4)sz=4;p=(next_alloc+3u)&~3u;next_alloc=p+((sz+0xffu)&~0xffu);void*m=memptr(p,sz);if(m)memzero(m,sz);for(int i=0;i<64;i++)if(!web_heap_addr[i]){web_heap_addr[i]=p;web_heap_size[i]=(sz+0xffu)&~0xffu;web_heap_used[i]=1;break;}hret(p);return 1;}
 case 0x3018F4BCu:hret(1);return 1;
 case 0x302C3198u:hret(1);return 1;
 case 0x30199498u:{if(child.active&&cpu.r[0]==child.handle)hret(child.handle+0x88u);else hret(fb_dc);return 1;}
 case 0x3019B400u:case 0x3019B448u:{h=new_handle(OBJ_DC);o=getobj(h);if(o){o->text_color=0xffffffu;o->bk_color=0;o->bk_mode=1;}hret(h);return 1;}
 case 0x3019A818u:mw32(cpu.r[0]+0x58,cpu.r[1]);hret(1);return 1;
 case 0x30199BBCu:hret(cpu.r[0]?mr32(cpu.r[0]+0x20):0);return 1;
 case 0x3019A834u:hret(cpu.r[0]?mr32(cpu.r[0]+0x58):0);return 1;
 case 0x301994B0u:{u32 q=cpu.r[1];i32 l=0,t=0,rr=0,bb=0;if(!q){hret(0);return 1;}if(child_client_rect(cpu.r[0],&l,&t,&rr,&bb)){mw32(q,(u32)l);mw32(q+4,(u32)t);mw32(q+8,(u32)rr);mw32(q+12,(u32)bb);}else{mw32(q,0);mw32(q+4,0);mw32(q+8,240);mw32(q+12,320);}hret(1);return 1;}
 case 0x30199D00u:invalidated=1;hret(1);return 1;
 case 0x3019A2E0u:invalidated=1;if(cpu.r[2])erase_pending=1;hret(1);return 1;
 case 0x3019AB84u:{u32 win=cpu.r[0],id=cpu.r[1]?cpu.r[1]:1,iv=cpu.r[2],cb=cpu.r[3];for(int i=0;i<MAX_TIMERS;i++)if(!timers[i].active){timers[i].active=1;timers[i].window=win;timers[i].id=id;timers[i].interval=iv?iv:1;timers[i].callback=cb;break;}hret(id);return 1;}
 case 0x3019ABA0u:for(int i=0;i<MAX_TIMERS;i++)if(timers[i].active&&timers[i].window==cpu.r[0]&&timers[i].id==cpu.r[1])timers[i].active=0;hret(1);return 1;
 case 0x30196960u:hret(240);return 1;case 0x3019696Cu:hret(320);return 1;
 case 0x301D9D90u:{EmuProfileFile pf;rid=desc_id(cpu.r[0]);mode=cpu.r[1];h=new_handle(OBJ_FILE);o=getobj(h);if(!o){hret(0);return 1;}o->rid=rid;o->pos=0;o->writable=0;
   /* Resource 0x10 is the firmware's system-data stream.  The supplied
      V1PRU container carries that stream beginning at the detected magic,
      so expose only this one missing storage resource at the VFS boundary. */
   if(rid==0x10u&&system_data_magic_ok&&nand_open_data()){o->host_system_data=1;o->len=system_data_len;o->cap=system_data_len;(void)mode;hret(h);return 1;}
   if(!emu_profile_open_file(rid,&pf)){objects[(h-HANDLE_BASE)/0x100u].kind=0;hret(0);return 1;}o->data=pf.data;o->len=pf.len;o->cap=pf.cap;o->writable=(u32)pf.writable;(void)mode;hret(h);return 1;}
 case 0x301D9DF8u:{o=getobj(cpu.r[0]);if(!o||o->kind!=OBJ_FILE){hret(1);return 1;}i32 off=(i32)cpu.r[1];u32 wh=cpu.r[2];i64 np=wh==0?(i64)off:wh==1?(i64)o->pos+off:(i64)o->len+off;if(np<0)np=0;if((u64)np>o->len)np=o->len;o->pos=(u32)np;hret(0);return 1;}
 case 0x301D9F04u:{o=getobj(cpu.r[0]);u32 n=cpu.r[2];if(!o||o->kind!=OBJ_FILE){hret(0);return 1;}if(o->pos>o->len){hret(0);return 1;}if(n>o->len-o->pos)n=o->len-o->pos;void*q=memptr(cpu.r[1],n);if(n&&!q){hret(0);return 1;}if(q&&n){if(o->host_system_data){if(!nand_open_data()||fseek(nand_data_file,(long)(system_data_base+o->pos),SEEK_SET)!=0)n=0;else n=(u32)fread(q,1,n,nand_data_file);}else memcopy(q,o->data+o->pos,n);}o->pos+=n;hret(n);return 1;}
 case 0x301D9FF8u:{o=getobj(cpu.r[0]);u32 n=cpu.r[2];if(!o||o->kind!=OBJ_FILE||!o->writable){hret(0);return 1;}if(o->pos>o->cap){hret(0);return 1;}if(n>o->cap-o->pos)n=o->cap-o->pos;u8*dst=(u8*)o->data;void*q=memptr(cpu.r[1],n);if(n&&!q){hret(0);return 1;}if(q&&n)memcopy(dst+o->pos,q,n);o->pos+=n;if(o->pos>o->len)o->len=o->pos;emu_profile_file_written(o->rid,o->len);hret(n);return 1;}
 case 0x301D9E5Cu:{o=getobj(cpu.r[0]);if(o)o->kind=0;hret(1);return 1;}
 case 0x30012AFCu:{rid=desc_id(cpu.r[0]);int idx=(int)cpu.r[1];u32 blen=0;const u8*b=0;if(!emu_profile_bitmap(rid,idx,&b,&blen)){hret(0);return 1;}h=new_handle(OBJ_BITMAP);o=getobj(h);o->data=b;o->len=blen;o->rid=rid;int w0,h0,bo,st;const u8*pal;if(bmp_info(b,blen,&w0,&h0,&bo,&st,&pal)){o->width=w0;o->height=h0<0?-h0:h0;mw32(h+4,((u32)o->height<<16)|((u32)o->width&0xffffu));}hret(h);return 1;}
 case 0x3019B538u:{d=getobj(cpu.r[0]);u32 oh=cpu.r[1],old=0;Obj*ob=getobj(oh);if(d&&d->kind==OBJ_DC&&ob){if(ob->kind==OBJ_BITMAP){old=d->selected;d->selected=oh;}else if(ob->kind==OBJ_FONT){old=d->selected_font;d->selected_font=oh;}else if(ob->kind==OBJ_PEN){old=d->selected_pen;d->selected_pen=oh;}}hret(old);return 1;}
 case 0x3019B4A0u:case 0x3019D648u:case 0x30010CECu:o=getobj(cpu.r[0]);if(o&&cpu.r[0]!=fb_dc&&(!child.active||(cpu.r[0]!=child.dc&&cpu.r[0]!=child.handle+0x88u)))o->kind=0;hret(1);return 1;
 case 0x3019C0BCu:case 0x3019C484u:{int transparent=(a==0x3019C484u);d=getobj(cpu.r[0]);int dx=(i32)cpu.r[1],dy=(i32)cpu.r[2],ww=(i32)cpu.r[3],hh=(i32)stackv(0);s=getobj(stackv(4));int sx=(i32)stackv(8),sy=(i32)stackv(12);u32 key=transparent?stackv(16):0;if(d&&d->framebuffer&&s&&s->kind==OBJ_DC){Obj*b=getobj(s->selected);if(b&&b->kind==OBJ_BITMAP){for(int yy=0;yy<hh;yy++)for(int xx=0;xx<ww;xx++){u32 rgb=bmp_rgb(b->data,b->len,sx+xx,sy+yy);if(transparent&&(rgb==key||(key==0xffffffu&&((rgb>>16)&255)>245&&((rgb>>8)&255)>245&&(rgb&255)>245)))continue;putpx(dx+d->origin_x+xx,dy+d->origin_y+yy,rgb);}}}hret(1);return 1;}
 case 0x30010C5Cu:{h=new_handle(OBJ_FONT);o=getobj(h);if(o){o->width=(i32)cpu.r[0];o->height=(i32)cpu.r[1];o->font_flags=(cpu.r[2]&1)|((cpu.r[3]&1)<<1)|((stackv(0)&1)<<2);}hret(h);return 1;}
 case 0x3019B604u:{d=getobj(cpu.r[0]);u32 old=d?d->text_color:0;if(d)d->text_color=cpu.r[1];hret(old);return 1;}
 case 0x3019B644u:{d=getobj(cpu.r[0]);u32 old=d?d->bk_mode:1;if(d)d->bk_mode=cpu.r[1];hret(old);return 1;}
 case 0x3019B82Cu:{d=getobj(cpu.r[0]);u32 rp=cpu.r[1];u32 col=cpu.r[2];if(d&&rp){fillrect(d,(i32)mr32(rp),(i32)mr32(rp+4),(i32)mr32(rp+8),(i32)mr32(rp+12),cref_to_rgb(col));}hret(1);return 1;}
 case 0x3019ACC8u:{h=new_handle(OBJ_PEN);o=getobj(h);if(o){o->pen_width=cpu.r[1];o->pen_color=cpu.r[2];}hret(h);return 1;}
 case 0x3019AD14u:{o=getobj(cpu.r[0]);if(o)o->kind=0;hret(1);return 1;}
 case 0x3019B78Cu:{d=getobj(cpu.r[0]);if(d){d->cur_x=(i32)cpu.r[1];d->cur_y=(i32)cpu.r[2];}hret(1);return 1;}
 case 0x3019B7B4u:{d=getobj(cpu.r[0]);if(d){u32 col=0;Obj*pn=getobj(d->selected_pen);if(pn)col=cref_to_rgb(pn->pen_color);line(d,d->cur_x,d->cur_y,(i32)cpu.r[1],(i32)cpu.r[2],col);d->cur_x=(i32)cpu.r[1];d->cur_y=(i32)cpu.r[2];}hret(1);return 1;}
 case 0x3019CF58u:{d=getobj(cpu.r[0]);u32 ptr=cpu.r[3],n=stackv(0);if(n==0xffffffffu)n=cstrlen32(ptr);const u8*q=(const u8*)memptr(ptr,n);if(d&&q)draw_text_bytes(d,(i32)cpu.r[1],(i32)cpu.r[2],q,n,0,0);hret(1);return 1;}
 case 0x3019CA50u:{d=getobj(cpu.r[0]);u32 ptr=cpu.r[1],n=cpu.r[2],rp=cpu.r[3],fmt=stackv(0);if(n==0xffffffffu)n=cstrlen32(ptr);const u8*q=(const u8*)memptr(ptr,n);if(d&&q&&rp){int l=(i32)mr32(rp),t=(i32)mr32(rp+4),rr=(i32)mr32(rp+8),bb=(i32)mr32(rp+12);if(fmt&0x20u){int tw=0;for(u32 ii=0;ii<n&&q[ii];){if(q[ii]>=0x81&&ii+1<n){tw+=web_text_width(d,q+ii,n-ii);ii+=2;}else{tw+=web_text_width(d,q+ii,n-ii);ii++;}}int xx=l,yy=t;if(fmt&1u)xx=l+((rr-l)-tw)/2;else if(fmt&2u)xx=rr-tw;if(fmt&4u)yy=t+((bb-t)-web_text_height(d))/2;draw_text_bytes(d,xx,yy,q,n,0,0);}else draw_text_bytes(d,l,t,q,n,rr-l,1);}hret((u32)(n?16:0));return 1;}
 case 0x30199F30u:{/* Set/clear window style bits, original helper used by modal WM_CREATE. */u32 hwnd=cpu.r[0],mask=cpu.r[1],val=cpu.r[2];if(!hwnd){hret(0);return 1;}u32 st=mr32(hwnd+0x10);st=(st&~mask)|val;mw32(hwnd+0x10,st);if(child.active&&hwnd==child.handle)child.style=st;hret(0);return 1;}
 case 0x30197810u:if(child.active&&child.modeless&&child.callback==0x300c9294u&&cpu.r[1]==0x14u)return 0;hret(0);return 1;
 case 0x301B2524u:hret(0);return 1;
 case 0x301B4BA4u:if(cpu.r[0]==WINDOW){cpu.r[15]=emu_profile_current()->app_wndproc;return 1;}hret(0);return 1;
 case 0x301A66B4u:if(cpu.r[14]!=0x300cd6b8u)return 0;hret(web_flight_create(cpu.r[3],stackv(0))?WEB_FLIGHT_MENU:0);return 1;
 case 0x301A6EC0u:if(cpu.r[0]!=WEB_FLIGHT_MENU||!web_flight_prepared)return 0;web_flight_active=1;web_flight_selection=0;hret(1);return 1;
 case 0x301B6710u:hret(cpu.r[0]);return 1;
 case 0x301B6924u:if(cpu.r[0])mw32(cpu.r[0]+0x20,cpu.r[1]);hret(1);return 1;
 case 0x301B24E4u:if(child.active&&cpu.r[0]==child.handle){child.result=cpu.r[1];child.active=0;}hret(1);return 1;
 case 0x301B6950u:if(child.active&&cpu.r[0]==child.handle&&cpu.r[1]==0x10){if(child.modeless)child.closing=1;else child.active=0;}hret(1);return 1;
 case 0x30192D14u:case 0x30192D28u:hret(1);return 1;
 case 0x301936ECu:web_sleep_ms=cpu.r[0]*5u;if(web_sleep_ms){suspend_request=1;return 1;}hret(1);return 1;
 case 0x3018F2DCu:for(int i=0;i<64;i++)if(web_heap_addr[i]==cpu.r[0])web_heap_used[i]=0;hret(1);return 1;
 case 0x300294F0u:hret(1);return 1;
 case 0x3002969Cu:{/* Firmware setting query used by the current app profile. */hret(emu_profile_save_present());return 1;}
 case 0x30029730u:{emu_profile_mark_save_present();hret(1);return 1;}
 /* modal child creation. Keep CPU suspended at the HLE call until EndDialog. */
 case 0x30199B64u:{if(cpu.r[2]!=0x300c9294u)return 0;p=(next_alloc+3u)&~3u;next_alloc=p+0x200u;void*m=memptr(p,0x200);if(m)memzero(m,0x200);child.active=1;child.pending=1;child.handle=p;child.title_ptr=cpu.r[0];child.callback=cpu.r[2];child.style=cpu.r[1];child.parent=stackv(12);child.result=0;child.left=(i32)cpu.r[3];child.top=(i32)stackv(0);child.right=child.left+(i32)stackv(4);child.bottom=child.top+(i32)stackv(8);child.flags=0;child.dc=new_handle(OBJ_DC);d=getobj(child.dc);if(d){d->framebuffer=1;d->origin_x=child.left;d->origin_y=child.top;d->text_color=0;d->bk_color=0xffffff;d->bk_mode=1;}
   /* Synthetic RTOS window fields mirror the real 0x301997A4 constructor. */
   child.internal=p+0x80;child.entry=p+0x100;u32 packed=(((u32)(child.bottom-child.top)&0xffffu)<<16)|((u32)(child.right-child.left)&0xffffu);mw32(p+0x0c,packed);mw32(p+0x10,child.style);mw32(p+0x14,child.flags);mw32(p+0x20,child.parent);mw32(p+0x40,child.internal);mw32(p+0x60,child.callback);
   /* The firmware constructor copies at most 31 title bytes into hwnd+0x64. */
   for(u32 ti=0;ti<31u;ti++){u8 c=child.title_ptr?mr8(child.title_ptr+ti):0;mw8(p+0x64u+ti,c);if(!c)break;if(ti==30u)mw8(p+0x83u,0);}
   /* Embedded DC and its surface descriptor: DC+0x10 -> hwnd+0xAC, size at +0xB0. */
   mw32(p+0x98,p+0xacu);mw32(p+0xac,4);mw32(p+0xb0,packed);mw32(p+0xb4,5);
   mw32(child.internal+0x18,0);mw32(child.internal+0x1c,1);mw32(child.internal+0x40,0);mw32(child.internal+0x44,0);mw32(child.entry+8,child.title_ptr);child.modeless=1;child.closing=child.selection=0;hret(p);return 1;}
 case 0x301B2E80u:{p=(next_alloc+3u)&~3u;next_alloc=p+0x200u;void*m=memptr(p,0x200);if(m)memzero(m,0x200);child.active=1;child.pending=1;child.handle=p;child.title_ptr=cpu.r[0];child.callback=cpu.r[1];child.style=cpu.r[2];child.parent=mr32(cpu.r[13]);child.result=0;child.left=(i32)mr32(cpu.r[3]);child.top=(i32)mr32(cpu.r[3]+4);child.right=(i32)mr32(cpu.r[3]+8);child.bottom=(i32)mr32(cpu.r[3]+12);child.flags=(child.style&0x00C00000u)?0x29u:0x08u;child.dc=new_handle(OBJ_DC);d=getobj(child.dc);if(d){d->framebuffer=1;d->origin_x=child.left;d->origin_y=child.top;d->text_color=0;d->bk_color=0xffffff;d->bk_mode=1;}
   /* Synthetic RTOS window fields mirror the real 0x301997A4 constructor. */
   child.internal=p+0x80;child.entry=p+0x100;u32 packed=(((u32)(child.bottom-child.top)&0xffffu)<<16)|((u32)(child.right-child.left)&0xffffu);mw32(p+0x0c,packed);mw32(p+0x10,child.style);mw32(p+0x14,child.flags);mw32(p+0x20,child.parent);mw32(p+0x40,child.internal);mw32(p+0x60,child.callback);
   /* The firmware constructor copies at most 31 title bytes into hwnd+0x64. */
   for(u32 ti=0;ti<31u;ti++){u8 c=child.title_ptr?mr8(child.title_ptr+ti):0;mw8(p+0x64u+ti,c);if(!c)break;if(ti==30u)mw8(p+0x83u,0);}
   /* Embedded DC and its surface descriptor: DC+0x10 -> hwnd+0xAC, size at +0xB0. */
   mw32(p+0x98,p+0xacu);mw32(p+0xac,4);mw32(p+0xb0,packed);mw32(p+0xb4,5);
   mw32(child.internal+0x18,0);mw32(child.internal+0x1c,1);mw32(child.internal+0x40,0);mw32(child.internal+0x44,0);mw32(child.entry+8,child.title_ptr);child.modeless=child.closing=child.selection=0;suspend_request=1;return 1;}
 /* v0.7 helpers used by the original WM_NCPAINT path (0x85 -> 0x30197544). */
 case 0x3019B4C0u:{u32 dc=cpu.r[0];if(child.active&&dc==child.handle+0x88u)hret(child.handle+0xacu);else hret(dc?mr32(dc+0x10):0);return 1;}
 case 0x3019ACA4u:hret((u32)rtos_metric(cpu.r[0]));return 1;
 case 0x30196A4Cu:{/* Cached skin resource. Frame blits are HLE-noops, but the original painter requires a non-null object. */u32 rp=0x3200F000u;if(cpu.r[0]<40u){mw32(rp+4,0x00100010u);hret(rp);}else hret(0);return 1;}
 case 0x30196A84u:hret(0);return 1;
 case 0x301A269Cu:hret(cpu.r[0]);return 1;
 case 0x3019DB00u:{/* Original RTOS beveled-frame helper, decoded from 0x3019DB00. */ d=getobj(cpu.r[0]);u32 rp=cpu.r[1];u32 hi=cref_to_rgb(cpu.r[2]),fill=cref_to_rgb(cpu.r[3]),lo=cref_to_rgb(stackv(0));int mode3d=(int)stackv(4);if(d&&rp){int l=(i32)mr32(rp),t=(i32)mr32(rp+4),rr=(i32)mr32(rp+8),bb=(i32)mr32(rp+12);if(mode3d==2||mode3d==3)fillrect(d,l+1,t+1,rr-1,bb-1,fill);line(d,l,t,rr-2,t,hi);line(d,l,t,l,bb-2,hi);line(d,l+1,bb-1,rr-1,bb-1,lo);line(d,rr-1,t+1,rr-1,bb-1,lo);}hret(1);return 1;}
 case 0x301A260Cu:hret(0);return 1;case 0x301A2644u:hret(1);return 1;case 0x3019E190u:hret(1);return 1;
 case 0x301A302Cu:if(child.active&&cpu.r[0]==child.internal+0x38){hret(child.entry);}else hret(0);return 1;
 case 0x3019B5CCu:{d=getobj(cpu.r[0]);u32 old=d?d->selected_font:0;if(d)d->selected_font=cpu.r[1];hret(old);return 1;}
 case 0x30199480u:hret(cpu.r[0]?mr32(cpu.r[0]+0x10):0);return 1;case 0x3019AC2Cu:if(child.active&&child.modeless&&child.callback==0x300c9294u){u32 index=cpu.r[0];hret(index<27u?mr32(mr32(0x3019acc0u)+index*4u):0xffffffffu);}else hret(0);return 1;
 case 0x3019C898u:{d=getobj(cpu.r[0]);u32 ptr=cpu.r[1],n=cpu.r[2],out=cpu.r[3];if(n==0xffffffffu)n=cstrlen32(ptr);const u8*q=(const u8*)memptr(ptr,n?n:1);i32 tw=0,th=0;if(!d||!out||!q||!text_extent_bytes(d,q,n,&tw,&th)){hret(0);return 1;}mw32(out,(u32)tw);mw32(out+4,(u32)th);hret(1);return 1;}case 0x30199C58u:hret(child.active?child.handle:WINDOW);return 1;case 0x3019BD3Cu:hret(1);return 1;
 default:return 0;}}

/* Host-backed view of the installed 480 MiB logical flash volume.  The
   decrypted V1PRU header describes an update window [0x00a00000,0x1dc00000),
   exactly 0x1d200000 bytes.  Keep this below the original block/VFS layers. */
static int vflash_ro_fill_page(u32 base,VfRoCachePage*p){
 const u64 lo=0x00a00000ull,hi=0x1dc00000ull;
 if(!p)return 0;for(u32 i=0;i<VFLASH_PAGE;i++)p->data[i]=0xff;
 p->base=base;p->valid=1u;
 if((u64)base<hi && (u64)base+VFLASH_PAGE>lo && nand_open_data()){
  u64 a=(u64)base>lo?(u64)base:lo,b=(u64)base+VFLASH_PAGE<hi?(u64)base+VFLASH_PAGE:hi;
  if(b>a){u64 fileoff=(a-lo)+0x1cull;if(fseek(nand_data_file,(long)fileoff,SEEK_SET)==0){u32 po=(u32)(a-(u64)base),want=(u32)(b-a),got=(u32)fread(p->data+po,1,want,nand_data_file);if(got<want)for(u32 i=got;i<want;i++)p->data[po+i]=0xff;}}
 }
 return 1;
}
static int system_vflash_base_read(u64 off,void*dst,u32 n){
 u8*d=(u8*)dst;if(!d)return 0;for(u32 i=0;i<n;i++)d[i]=0xff;
 const u64 lo=0x00a00000ull,hi=0x1dc00000ull;
 if(!n||off>=(u64)0x1e000000u||off+(u64)n<=lo||off>=hi)return 1;
 if(!vflash_ro_cache_enabled){
  if(nand_open_data()){
   u64 a=off>lo?off:lo,b=off+(u64)n<hi?off+(u64)n:hi;
   if(b>a){u64 fileoff=(a-lo)+0x1cull;if(fseek(nand_data_file,(long)fileoff,SEEK_SET)==0){u32 want=(u32)(b-a),got=(u32)fread(d+(u32)(a-off),1,want,nand_data_file);if(got<want)for(u32 i=got;i<want;i++)d[(u32)(a-off)+i]=0xff;}}
  }
  return 1;
 }
 {
  u64 end=off+(u64)n,pos=off;
  while(pos<end){
   u32 base=(u32)pos&~(VFLASH_PAGE-1u),idx=(base>>12)&(VFLASH_RO_CACHE_SLOTS-1u);
   VfRoCachePage*p=&vflash_ro_cache[idx];
   if(!p->valid||p->base!=base){vflash_ro_cache_misses++;if(!vflash_ro_fill_page(base,p))return 0;}else vflash_ro_cache_hits++;
   u32 po=(u32)(pos-(u64)base),take=VFLASH_PAGE-po;if((u64)take>end-pos)take=(u32)(end-pos);
   memcopy(d+(u32)(pos-off),p->data+po,take);pos+=(u64)take;
  }
 }
 return 1;
}
static VfOverlayPage* vflash_page(u32 base,int create){
 for(u32 i=0;i<vflash_overlay_count;i++)if(vflash_overlay[i].used&&vflash_overlay[i].base==base)return &vflash_overlay[i];
 if(!create||vflash_overlay_count>=VFLASH_OVERLAY_PAGES)return 0;VfOverlayPage*p=&vflash_overlay[vflash_overlay_count++];p->base=base;p->used=1;system_vflash_base_read(base,p->data,VFLASH_PAGE);return p;
}
static int system_vflash_read(u64 off,void*dst,u32 n){
 if(!system_vflash_base_read(off,dst,n))return 0;u8*d=(u8*)dst;u64 end=off+(u64)n;
 for(u32 i=0;i<vflash_overlay_count;i++){VfOverlayPage*p=&vflash_overlay[i];u64 a=p->base,b=a+VFLASH_PAGE;if(end<=a||off>=b)continue;u64 x=off>a?off:a,y=end<b?end:b;memcopy(d+(u32)(x-off),p->data+(u32)(x-a),(u32)(y-x));}return 1;
}
static int system_vflash_write(u64 off,const void*src,u32 n){
 const u8*s=(const u8*)src;if(!s||off+(u64)n>0x1e000000ull)return 0;u32 done=0;
 while(done<n){u32 a=(u32)off+done,base=a&~(VFLASH_PAGE-1u),po=a-base,take=VFLASH_PAGE-po;if(take>n-done)take=n-done;VfOverlayPage*p=vflash_page(base,1);if(!p)return 0;
  if(!system_state_dirty)for(u32 i=0;i<take;i++)if(p->data[po+i]!=s[done+i]){system_state_dirty=1u;break;}
  memcopy(p->data+po,s+done,take);done+=take;}return 1;
}

/* Sidecar format is intentionally tiny and append-free.  It stores only pages
   dirtied by the original firmware plus the two small compatibility resources
   currently living above the missing NAND OOB/FTL metadata.  The factory
   V1PRU image is never modified. */
static int system_state_write(FILE*f){
 {
  const u8 magic[8]={'H','J','X','V','1','S','0','4'};u32 ver=4u;
  u32 rtc_saved=rtc_current_seconds(),rtc_off_valid=rtc.offset_valid?1u:0u;i64 rtc_off=rtc.host_offset;
  if(fwrite(magic,1,8,f)!=8||fwrite(&ver,4,1,f)!=1||fwrite(&vflash_overlay_count,4,1,f)!=1||
     fwrite(&sys_nv133_present,4,1,f)!=1||fwrite(&sys_nv133_len,4,1,f)!=1||
     fwrite(&sys_nv138_present,4,1,f)!=1||fwrite(&sys_nv138_len,4,1,f)!=1||
     fwrite(&rtc_saved,4,1,f)!=1||fwrite(&rtc_off,8,1,f)!=1||fwrite(&rtc_off_valid,4,1,f)!=1){return 0;}
  for(u32 i=0;i<vflash_overlay_count;i++){
   if(fwrite(&vflash_overlay[i].base,4,1,f)!=1||fwrite(vflash_overlay[i].data,1,VFLASH_PAGE,f)!=VFLASH_PAGE){return 0;}
  }
  if(sys_nv133_len&&fwrite(sys_nv133,1,sys_nv133_len,f)!=sys_nv133_len){return 0;}
  if(sys_nv138_len&&fwrite(sys_nv138,1,sys_nv138_len,f)!=sys_nv138_len){return 0;}
 }
 return fflush(f)==0;
}
#ifdef _WIN32
unsigned long __stdcall GetCurrentProcessId(void);
unsigned long __stdcall GetLastError(void);
unsigned long __stdcall GetFileAttributesA(const char*);
int __stdcall MoveFileExA(const char*,const char*,unsigned long);
#endif
static int system_state_save_file(void){
 if(!system_state_path[0])return 1;
 char temporary[560];unsigned long id=0;
#ifdef _WIN32
 id=GetCurrentProcessId();
#endif
 sprintf(temporary,"%s.tmp.%lu",system_state_path,id);
 FILE*f=fopen(temporary,"wb");if(!f)return 0;
 int ok=system_state_write(f);
#ifdef _WIN32
 if(ok&&_commit(_fileno(f))!=0)ok=0;
#endif
 if(fclose(f)!=0)ok=0;
 if(ok){
#ifdef _WIN32
  ok=MoveFileExA(temporary,system_state_path,1u|8u)!=0;
#else
  ok=rename(temporary,system_state_path)==0;
#endif
 }
 if(!ok)remove(temporary);
 if(ok)system_state_dirty=0u;
 if(ok)emu_system_log("state: saved");
 else emu_system_log("state: save failed");
 return ok;
}
static int system_state_autosave_file(void){
 if(!system_state_path[0])return 1;
#ifdef _WIN32
 /* If the user removed the file while running, recreate it even when RAM is
    unchanged. A directory at this path must still surface a save failure. */
 unsigned long attributes=GetFileAttributesA(system_state_path);
 if(!system_state_dirty&&attributes!=0xffffffffu&&!(attributes&0x10u))return 1;
#else
 if(!system_state_dirty){FILE*f=fopen(system_state_path,"rb");if(f){fclose(f);return 1;}}
#endif
 {return system_state_save_file();}
}
/* Validate a complete snapshot before touching live state. Failed loads must
   not leave half of a NAND overlay or accept an unknown format as first boot. */
static int system_state_load_file(void){
 if(!system_state_path[0])return 1;
 FILE*f=fopen(system_state_path,"rb");
 if(!f){
#ifdef _WIN32
  if(GetFileAttributesA(system_state_path)!=0xffffffffu){emu_system_log("state: load failed (path exists but unreadable)");return 0;}
  unsigned long e=GetLastError();
  if(e==2u||e==3u){emu_system_log("state: no save (cold boot)");return 1;}
  emu_system_log("state: load failed");return 0;
#else
  emu_system_log("state: no save (cold boot)");return 1;
#endif
 }
 long size;u8*data;u32 h[6],offset,rtc_saved=RTC_DEFAULT_SECONDS,rtc_valid=0;i64 rtc_off=0;
 if(fseek(f,0,SEEK_END)!=0||(size=ftell(f))<32||
    (u64)size>48ull+(u64)VFLASH_OVERLAY_PAGES*(VFLASH_PAGE+4u)+sizeof(sys_nv133)+sizeof(sys_nv138)){fclose(f);return 0;}
 data=(u8*)malloc((size_t)size);if(!data){fclose(f);return 0;}
 rewind(f);int ok=fread(data,1,(size_t)size,f)==(size_t)size;fclose(f);
 if(!ok){free(data);return 0;}
 memcopy(h,data+8,24);
 const u8 magic[7]={'H','J','X','V','1','S','0'};
 for(u32 i=0;i<7;i++)if(data[i]!=magic[i])ok=0;
 if(h[0]<2u||h[0]>4u||data[7]!='0'+h[0])ok=0;
 if(h[1]>VFLASH_OVERLAY_PAGES||h[3]>sizeof(sys_nv133)||h[5]>sizeof(sys_nv138))ok=0;
 offset=h[0]==4u?48u:32u;
 if((u64)offset+(u64)h[1]*(VFLASH_PAGE+4u)+h[3]+h[5]!=(u64)size)ok=0;
 if(!ok){free(data);return 0;}
 if(h[0]==4u){memcopy(&rtc_saved,data+32,4);memcopy(&rtc_off,data+36,8);memcopy(&rtc_valid,data+44,4);}
 for(u32 i=0;i<h[1];i++){
  u32 base;memcopy(&base,data+offset+i*(VFLASH_PAGE+4u),4);
  if(base>=0x1e000000u||(base&(VFLASH_PAGE-1u))){ok=0;break;}
  for(u32 j=0;j<i;j++){u32 other;memcopy(&other,data+offset+j*(VFLASH_PAGE+4u),4);if(base==other){ok=0;break;}}
  if(!ok)break;
 }
 if(!ok){free(data);return 0;}
 memzero(vflash_overlay,sizeof(vflash_overlay));vflash_overlay_count=h[1];
 for(u32 i=0;i<h[1];i++){
  VfOverlayPage*p=&vflash_overlay[i];memcopy(&p->base,data+offset,4);p->used=1;
  memcopy(p->data,data+offset+4,VFLASH_PAGE);offset+=VFLASH_PAGE+4u;
 }
 memzero(sys_nv133,sizeof(sys_nv133));memzero(sys_nv138,sizeof(sys_nv138));
 memcopy(sys_nv133,data+offset,h[3]);offset+=h[3];
 /* Format 2's synthetic touch calibration remains deliberately discarded. */
 if(h[0]!=2u)memcopy(sys_nv138,data+offset,h[5]);
 sys_nv133_present=h[2]?1u:0u;sys_nv133_len=h[3];
 sys_nv138_present=h[0]!=2u&&h[4]?1u:0u;sys_nv138_len=h[0]!=2u?h[5]:0u;
 if(h[0]==4u){rtc.fallback_seconds=rtc_saved;rtc.host_offset=rtc_off;rtc.offset_valid=rtc_valid?1u:0u;}
 system_state_dirty=h[0]!=4u;
 free(data);emu_system_log("state: loaded");return 1;
}
static void debug_vf_log(u64 off,u32 len,u32 wr){DebugVfOp*e=&debug_vf_ops[debug_vf_pos++&255u];e->off=off;e->len=len;e->wr=wr;}
static int system_ftl_hle(u32 a){
 /* static 0x300ff8dc/0x300ffa10 minus the 0x48 update wrapper */
 if(a==0x300ff894u){u64 off=(u64)cpu.r[1]|((u64)cpu.r[2]<<32);u32 n=cpu.r[3];void*q=memptr(cpu.r[0],n?n:1);if(!q){hret(0);return 1;}system_vflash_read(off,q,n);debug_vflash_reads++;debug_vflash_last_off=off;debug_vflash_last_len=n;debug_vf_log(off,n,0);if(audio_probe.dma_events){u32 nz=0,i,m=n>4096u?4096u:n;u8*b=(u8*)q;u32 dest=cpu.r[0];for(i=0;i<m;i++)if(b[i])nz++;if(dest<0x30488584u&&dest+n>0x30480584u)audio_probe.vf_dma_hits++;if(n>=256u){u32 k=audio_probe.vf_large<8u?audio_probe.vf_large:7u;if(audio_probe.vf_large>=8u){u32 s;for(s=0;s<7u;s++){audio_probe.vf_off[s]=audio_probe.vf_off[s+1u];audio_probe.vf_dest[s]=audio_probe.vf_dest[s+1u];audio_probe.vf_len[s]=audio_probe.vf_len[s+1u];audio_probe.vf_nz[s]=audio_probe.vf_nz[s+1u];audio_probe.vf_lr[s]=audio_probe.vf_lr[s+1u];}}audio_probe.vf_off[k]=off;audio_probe.vf_dest[k]=dest;audio_probe.vf_len[k]=n;audio_probe.vf_nz[k]=nz;{u32 mid=(cpu.r[14]==0x300fcc70u)?mr32(cpu.r[13]+20u):cpu.r[14];if(mid==0x301dc27cu)mid=mr32(cpu.r[13]+0x34u);if(mid==0x301f8894u)mid=mr32(cpu.r[13]+0x54u);if(mid==0x301d9f78u||mid==0x301d9fe0u)mid=mr32(cpu.r[13]+0x84u);if(mid==0x30107ef8u)mid=mr32(cpu.r[13]+0x94u);if(mid==0x3027a0a0u)mid=mr32(cpu.r[13]+0xacu);if(mid==0x30283e00u)mid=mr32(cpu.r[13]+0xbcu);audio_probe.vf_lr[k]=mid;}if(audio_probe.vf_large<8u)audio_probe.vf_large++;}audio_probe.vf_reads++;}hret(n);return 1;}
 if(a==0x300ff9c8u){u64 off=(u64)cpu.r[1]|((u64)cpu.r[2]<<32);u32 n=cpu.r[3];void*q=memptr(cpu.r[0],n?n:1);u32 ok=q&&system_vflash_write(off,q,n);debug_vflash_writes++;debug_vflash_last_off=off;debug_vflash_last_len=n;debug_vf_log(off,n,1);hret(ok?n:0);return 1;}
 return 0;
}

/* System-boot storage bridge.  This is intentionally narrower than the app
   HLE: it replaces only information absent from the supplied images.
   - 0x300FE090: FTL metadata/OOB initialization (not present in V1PRU)
   - RID 0x10 file stream: verified by the original firmware checker as
     @OZINGSYSTEMDATA at V1PRU+system_data_base.
   Everything above this boundary continues as original ARM firmware. */
static int system_storage_hle(u32 a){
 Obj*o;u32 h,rid,mode;
 if(a==0x300FE090u){debug_storage_hle_calls++;debug_ftl_bypass_calls++;hret(1);return 1;}
 if(a==0x301D9D90u){
  rid=desc_id(cpu.r[0]);mode=cpu.r[1];
  if(rid==0x10u){if(!system_data_magic_ok||!nand_open_data())return 0;}
  else if(rid==0x03u){if(!program_data_magic_ok)return 0;}
  else if(rid==0x0fu){/* Temporary compatibility bridge.  Disable during update-mode
                         experiments to observe the original install/update path. */
   if(!debug_rid15_bridge)return 0;
   if(!system_data_magic_ok||!nand_open_data())return 0;
  }else if(rid==133u||rid==138u){
   u32 present=rid==133u?sys_nv133_present:sys_nv138_present;
   if(mode==3u&&!present&&rid==133u){
    /* Open-failure skips SYSCFG's table setup and leaves getters reading
       uninitialized stack values (including the movie volume). Erased bytes
       enter the original invalid-marker/default/save path instead. */
    for(u32 i=0;i<sizeof(sys_nv133);i++)sys_nv133[i]=0xffu;
    sys_nv133_len=(u32)sizeof(sys_nv133);sys_nv133_present=present=1u;system_state_dirty=1u;
   }
   if(mode==3u&&!present){debug_storage_hle_calls++;hret(0);return 1;}
   if(mode!=2u&&mode!=3u)return 0;
  }else return 0;
  h=new_handle(OBJ_FILE);o=getobj(h);if(!o){hret(0);return 1;}
  o->rid=rid;o->pos=0;o->writable=0;
  if(rid==0x10u){o->host_system_data=1;o->len=system_data_len;o->cap=system_data_len;debug_system_file_opens++;}
  else if(rid==0x03u){o->host_program_data=1;o->len=program_data_len;o->cap=program_data_len;}
  else if(rid==0x0fu){o->host_system_data=1;o->len=system_data_len;o->cap=system_data_len;}
  else{
   o->host_nv=1;o->writable=(mode==2u);
   if(rid==133u){o->data=sys_nv133;o->len=sys_nv133_len;o->cap=(u32)sizeof(sys_nv133);if(mode==2u){if(sys_nv133_len||!sys_nv133_present)system_state_dirty=1u;o->len=0;sys_nv133_len=0;sys_nv133_present=1;}}
   else{o->data=sys_nv138;o->len=sys_nv138_len;o->cap=(u32)sizeof(sys_nv138);if(mode==2u){if(sys_nv138_len||!sys_nv138_present)system_state_dirty=1u;o->len=0;sys_nv138_len=0;sys_nv138_present=1;}}
  }
  debug_storage_hle_calls++;hret(h);return 1;
 }
 if(a==0x301D9DF8u){
  o=getobj(cpu.r[0]);if(!o||o->kind!=OBJ_FILE||(!o->host_system_data&&!o->host_program_data&&!o->host_nv))return 0;
  i32 off=(i32)cpu.r[1];u32 wh=cpu.r[2];i64 np=wh==0?(i64)off:wh==1?(i64)o->pos+off:(i64)o->len+off;
  if(np<0)np=0;if((u64)np>o->len)np=o->len;o->pos=(u32)np;debug_storage_hle_calls++;hret(0);return 1;
 }
 if(a==0x301D9F04u){
  o=getobj(cpu.r[0]);if(!o||o->kind!=OBJ_FILE||(!o->host_system_data&&!o->host_program_data&&!o->host_nv))return 0;
  u32 n=cpu.r[2];if(o->pos>o->len){hret(0);return 1;}if(n>o->len-o->pos)n=o->len-o->pos;void*q=memptr(cpu.r[1],n);if(n&&!q){hret(0);return 1;}
  if(q&&n){
   if(o->host_system_data){if(!nand_open_data()||fseek(nand_data_file,(long)(system_data_base+o->pos),SEEK_SET)!=0)n=0;else n=(u32)fread(q,1,n,nand_data_file);}
   else if(o->host_program_data){const u8*ps=program_external_active?program_external:_binary_fw_bin_start;memcopy(q,ps+program_data_base+o->pos,n);}
   else memcopy(q,o->data+o->pos,n);
  }
  o->pos+=n;debug_storage_hle_calls++;if(o->host_system_data)debug_system_file_reads++;hret(n);return 1;
 }
 if(a==0x301D9FF8u){
  o=getobj(cpu.r[0]);if(!o||o->kind!=OBJ_FILE||!o->host_nv||!o->writable)return 0;
  u32 n=cpu.r[2];if(o->pos>o->cap){hret(0);return 1;}if(n>o->cap-o->pos)n=o->cap-o->pos;void*q=memptr(cpu.r[1],n);if(n&&!q){hret(0);return 1;}
  if(!system_state_dirty){if(o->pos+n>o->len)system_state_dirty=1u;else for(u32 j=0;j<n;j++)if(((u8*)o->data)[o->pos+j]!=((u8*)q)[j]){system_state_dirty=1u;break;}}
  if(q&&n)memcopy((u8*)o->data+o->pos,q,n);o->pos+=n;if(o->pos>o->len)o->len=o->pos;
  if(o->rid==133u){sys_nv133_len=o->len;sys_nv133_present=1;}else if(o->rid==138u){sys_nv138_len=o->len;sys_nv138_present=1;}
  debug_storage_hle_calls++;hret(n);return 1;
 }
 if(a==0x301D9E5Cu){o=getobj(cpu.r[0]);if(!o||o->kind!=OBJ_FILE||(!o->host_system_data&&!o->host_program_data&&!o->host_nv))return 0;o->kind=0;debug_storage_hle_calls++;hret(1);return 1;}
 return 0;
}

/* Virtual touchscreen boundary used only for system bring-up.  The original
   calibration UI draws four targets, then calls the touch driver with an
   output POINT.  An ideal virtual panel can report the target itself as the
   raw sample; the firmware still performs its own calibration math and
   persists RID138 through the normal resource API. */
static int keypad_pair_for_vk(u32 vk,u32*drive_pin,u32*read_pin){
 if(keypad_pair_override_vk&&vk==keypad_pair_override_vk&&keypad_pair_override_drive&&keypad_pair_override_read){
  *drive_pin=keypad_pair_override_drive;*read_pin=keypad_pair_override_read;return 1;
 }
 switch(vk){
  case 41u:  *drive_pin=14u;*read_pin=13u;return 1; /* VK_SELECT */
  case 131u: *drive_pin=3u; *read_pin=13u;return 1; /* VK_PLAY   */
  case 27u:  *drive_pin=11u;*read_pin=3u; return 1; /* VK_ESCAPE/STOP */
  case 37u:  *drive_pin=11u;*read_pin=13u;return 1; /* LEFT  */
  case 38u:  *drive_pin=13u;*read_pin=14u;return 1; /* UP    */
  case 39u:  *drive_pin=13u;*read_pin=11u;return 1; /* RIGHT */
  case 40u:  *drive_pin=3u; *read_pin=14u;return 1; /* DOWN  */
  case 124u: *drive_pin=14u;*read_pin=3u; return 1; /* volume+ */
  case 125u: *drive_pin=14u;*read_pin=11u;return 1; /* volume- */
  default:return 0;
 }
}
void emu_debug_set_keypad_pair(u32 vk,u32 drive_pin,u32 read_pin){
 keypad_pair_override_vk=vk;keypad_pair_override_drive=drive_pin;keypad_pair_override_read=read_pin;
}
static u32 keypad_current_drive_pin(void){
 u8*p=(u8*)memptr(0x20090000u,4);u32 v=p?((u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24)):0;
 if(v&(1u<<14))return 14u;if(v&(1u<<13))return 13u;if(v&(1u<<3))return 3u;if(v&(1u<<11))return 11u;return 0xffffffffu;
}

#ifdef _WIN32
#define SD_WINAPI __stdcall
typedef struct{unsigned long attr,ct0,ct1,at0,at1,wt0,wt1,size_hi,size_lo,r0,r1;u16 name[260];u16 alt[14];} SdFind;
void* SD_WINAPI FindFirstFileW(const u16*,SdFind*);
int SD_WINAPI FindNextFileW(void*,SdFind*);
int SD_WINAPI FindClose(void*);
int SD_WINAPI MultiByteToWideChar(unsigned,unsigned long,const char*,int,unsigned short*,int);
int SD_WINAPI WideCharToMultiByte(unsigned,unsigned long,const u16*,int,char*,int,const char*,int*);
FILE* _wfopen(const u16*,const u16*);
#endif

#define SD_FILES 1024u
#define SD_DIRS 64u
/* A full firmware resource export exceeds both the old 256-entry limit
   and its 256 MiB cluster map. Keep FAT16/32 KiB clusters, using only data
   cluster numbers below FAT16's reserved 0xFFF0..0xFFFF range. */
#define SD_DIR_BYTES 65536u
#define SD_SPC 64u
#define SD_CLUSTER 32768u
#define SD_FAT_ENTRIES 65536u
#define SD_CLUSTER_LIMIT 0xFFF0u
typedef struct{char path[780];u16 path_w[260],name[256];u32 size,first;u8 n83[11];u8 is_dir,dir_id,lfn_slots;u16 parent;} HostFile;
typedef struct{
 char root[260];char label[12];
 HostFile files[SD_FILES];
 u32 file_n,clusters,fat_sectors,data_rel,bytes,reads,last_addr;
 u8 bpb[512];u8 fat[SD_FAT_ENTRIES*2u];u8 rootdir[512*32];
 u8 dir_pool[SD_DIRS][SD_DIR_BYTES];u32 dir_pool_n;
 u16 map_file[SD_FAT_ENTRIES];u32 map_off[SD_FAT_ENTRIES];
 int ready;u32 ov_id;
} HostVol;
static HostVol sd_vol,c_vol;
#define VOL_OV 160u
static u8 vol_ov[2][VOL_OV][512];
static u32 vol_ov_sec[2][VOL_OV],vol_ov_n[2];
/* Keep one host file open across sector reads.  fopen/fclose per 512-byte
   sector freezes the UI when the firmware streams an AVI. */
static FILE* host_fc_fp;
static u16 host_fc_path[260];
#ifdef ANDROID
static int host_utf8_to_wide(const char*src,u16*dst,u32 capacity){
 u32 used=0;
 while(*src){
  u32 code=(u8)*src++,extra,minimum;
  if(code<0x80u){extra=0;minimum=0;}
  else if(code>=0xc2u&&code<=0xdfu){code&=31u;extra=1;minimum=0x80u;}
  else if(code>=0xe0u&&code<=0xefu){code&=15u;extra=2;minimum=0x800u;}
  else if(code>=0xf0u&&code<=0xf4u){code&=7u;extra=3;minimum=0x10000u;}
  else return 0;
  while(extra--){u32 next=(u8)*src++;if((next&0xc0u)!=0x80u)return 0;code=(code<<6)|(next&63u);}
  if(code<minimum||code>0x10ffffu||(code>=0xd800u&&code<=0xdfffu))return 0;
  if(used+(code>0xffffu?2u:1u)>=capacity)return 0;
  if(code>0xffffu){code-=0x10000u;dst[used++]=(u16)(0xd800u+(code>>10));dst[used++]=(u16)(0xdc00u+(code&1023u));}
  else dst[used++]=(u16)code;
 }
 dst[used]=0;return 1;
}
static int host_wide_to_utf8(const u16*src,char*dst,u32 capacity){
 u32 used=0;
 while(*src){
  u32 code=*src++,count;
  if(code>=0xd800u&&code<=0xdbffu){u32 next=*src++;if(next<0xdc00u||next>0xdfffu)return 0;code=0x10000u+((code-0xd800u)<<10)+(next-0xdc00u);}
  else if(code>=0xdc00u&&code<=0xdfffu)return 0;
  count=code<0x80u?1u:code<0x800u?2u:code<0x10000u?3u:4u;
  if(used+count>=capacity)return 0;
  if(count==1u)dst[used++]=(char)code;
  else{
   dst[used++]=(char)((count==2u?0xc0u:count==3u?0xe0u:0xf0u)|(code>>(6u*(count-1u))));
   for(u32 part=count-1u;part;part--)dst[used++]=(char)(0x80u|((code>>(6u*(part-1u)))&63u));
  }
 }
 dst[used]=0;return 1;
}
#endif
static FILE* host_fopen_w(const u16*path){
#ifdef _WIN32
 static const u16 mode[]={'r','b',0};return _wfopen(path,mode);
#elif defined(ANDROID)
 char utf8[780];if(!host_wide_to_utf8(path,utf8,sizeof(utf8)))return 0;return fopen(utf8,"rb");
#else
 char p[260];u32 i;for(i=0;path[i]&&i<259u;i++){if(path[i]>127u)return 0;p[i]=(char)path[i];}p[i]=0;return fopen(p,"rb");
#endif
}
static void host_fc_close(void){if(host_fc_fp){fclose(host_fc_fp);host_fc_fp=0;}host_fc_path[0]=0;}
static FILE* host_fc_open(const u16*path){
 u32 i=0;if(!path||!path[0])return 0;
 if(host_fc_fp){
  while(path[i]||host_fc_path[i]){if(path[i]!=host_fc_path[i]){host_fc_close();break;}if(!path[i])return host_fc_fp;i++;}
  if(host_fc_fp)return host_fc_fp;
 }
 host_fc_fp=host_fopen_w(path);if(!host_fc_fp)return 0;
 i=0;while(path[i]&&i+1u<260u){host_fc_path[i]=path[i];i++;}host_fc_path[i]=0;return host_fc_fp;
}
static char host_media_path[260];
#define HOST_LOG_LINES EMU_LOG_RING_LINES
#define HOST_LOG_WIDTH 220u
static char host_log_lines[HOST_LOG_LINES][HOST_LOG_WIDTH];
static u16 host_log_kinds[HOST_LOG_LINES];
static u32 host_log_head,host_log_count,host_log_seq;
static char host_log_path[260];
static FILE* host_log_fp;
static void (*host_log_notify)(void);
static int host_log_prefix_eq(const char*msg,const char*pre){
 u32 i=0;if(!msg||!pre)return 0;
 while(pre[i]){if(msg[i]!=pre[i])return 0;i++;}
 return msg[i]==':'||msg[i]==0;
}
static u32 host_log_classify(const char*msg){
 if(!msg||!msg[0])return EMU_LOG_OTHER;
 if(host_log_prefix_eq(msg,"boot"))return EMU_LOG_BOOT;
 if(host_log_prefix_eq(msg,"host"))return EMU_LOG_HOST;
 if(host_log_prefix_eq(msg,"state"))return EMU_LOG_STATE;
 if(host_log_prefix_eq(msg,"movie"))return EMU_LOG_MOVIE;
 if(host_log_prefix_eq(msg,"media"))return EMU_LOG_MEDIA;
 if(host_log_prefix_eq(msg,"audio"))return EMU_LOG_AUDIO;
 if(host_log_prefix_eq(msg,"storage"))return EMU_LOG_STORAGE;
 if(host_log_prefix_eq(msg,"probe"))return EMU_LOG_PROBE;
 if(host_log_prefix_eq(msg,"fw"))return EMU_LOG_FW;
 if(host_log_prefix_eq(msg,"input"))return EMU_LOG_INPUT;
 return EMU_LOG_OTHER;
}
void emu_system_log_set_path(const char*path){
 u32 i=0;if(host_log_fp){fclose(host_log_fp);host_log_fp=0;}
 host_log_path[0]=0;if(!path)return;
 while(path[i]&&i+1u<sizeof(host_log_path)){host_log_path[i]=path[i];i++;}host_log_path[i]=0;
 /* Fresh file each run so on-disk order matches this session only. */
 if(host_log_path[0])host_log_fp=fopen(host_log_path,"wb");
}
void emu_system_log_set_notify(void (*fn)(void)){host_log_notify=fn;}
void emu_system_log_kind(u32 kind,const char*msg){
 u32 i=0,slot;
 if(!msg)msg="";
 if(!kind)kind=host_log_classify(msg);
 slot=host_log_head%HOST_LOG_LINES;
 while(msg[i]&&i+1u<HOST_LOG_WIDTH){host_log_lines[slot][i]=msg[i];i++;}
 host_log_lines[slot][i]=0;
 host_log_kinds[slot]=(u16)kind;
 host_log_head++;if(host_log_count<HOST_LOG_LINES)host_log_count++;
 host_log_seq++;
 if(host_log_path[0]){
  if(!host_log_fp)host_log_fp=fopen(host_log_path,"ab");
  if(host_log_fp){fwrite(host_log_lines[slot],1,i,host_log_fp);fwrite("\r\n",1,2,host_log_fp);fflush(host_log_fp);}
 }
 if(host_log_notify)host_log_notify();
}
void emu_system_log(const char*msg){emu_system_log_kind(host_log_classify(msg),msg);}
u32 emu_system_log_seq(void){return host_log_seq;}
u32 emu_system_log_ring_lines(void){return HOST_LOG_LINES;}
int emu_system_log_get(u32 seq,char*dst,u32 cap){
 u32 slot,j=0;const char*s;
 if(!dst||cap<2u||!seq||seq>host_log_seq)return 0;
 if(host_log_seq-seq>=HOST_LOG_LINES)return 0;
 slot=(seq-1u)%HOST_LOG_LINES;s=host_log_lines[slot];
 while(s[j]&&j+1u<cap){dst[j]=s[j];j++;}
 dst[j]=0;return 1;
}
u32 emu_system_log_line_kind(u32 seq){
 u32 slot;
 if(!seq||seq>host_log_seq)return 0;
 if(host_log_seq-seq>=HOST_LOG_LINES)return 0;
 slot=(seq-1u)%HOST_LOG_LINES;return host_log_kinds[slot];
}
u32 emu_system_log_dump(char*dst,u32 cap){return emu_system_log_dump_filtered(EMU_LOG_ALL,dst,cap);}
u32 emu_system_log_dump_filtered(u32 mask,char*dst,u32 cap){
 u32 n,start,i,o=0;if(!dst||cap<2u)return 0;
 if(!mask)mask=EMU_LOG_ALL;
 n=host_log_count;if(n>HOST_LOG_LINES)n=HOST_LOG_LINES;
 start=(host_log_head+HOST_LOG_LINES-n)%HOST_LOG_LINES;
 for(i=0;i<n;i++){
  u32 slot=(start+i)%HOST_LOG_LINES;const char*s;u32 j=0;
  if(!(host_log_kinds[slot]&mask))continue;
  s=host_log_lines[slot];
  while(s[j]&&o+2u<cap)dst[o++]=s[j++];
  if(o+2u<cap){dst[o++]='\r';dst[o++]='\n';}
 }
 dst[o]=0;return o;
}
#ifdef _WIN32
void* __stdcall ShellExecuteA(void*,const char*,const char*,const char*,const char*,int);
unsigned long __stdcall GetModuleFileNameA(void*,char*,unsigned long);
unsigned long __stdcall GetTempPathW(unsigned long,u16*);
unsigned int __stdcall GetTempFileNameW(const u16*,const u16*,unsigned int,u16*);
int __stdcall DeleteFileW(const u16*);
int __stdcall QueryPerformanceCounter(long long*);
int __stdcall QueryPerformanceFrequency(long long*);
int __stdcall CloseHandle(void*);
int __stdcall GetExitCodeProcess(void*,unsigned long*);
int __stdcall TerminateProcess(void*,unsigned int);
unsigned long __stdcall WaitForSingleObject(void*,unsigned long);
void __stdcall Sleep(unsigned long);
int __stdcall CreatePipe(void**,void**,void*,unsigned long);
int __stdcall SetHandleInformation(void*,unsigned long,unsigned long);
int __stdcall CreateProcessW(const u16*,u16*,void*,void*,int,unsigned long,void*,const u16*,void*,void*);
void* __stdcall CreateFileA(const char*,unsigned long,unsigned long,void*,unsigned long,unsigned long,void*);
int __stdcall PeekNamedPipe(void*,void*,unsigned long,unsigned long*,unsigned long*,unsigned long*);
int __stdcall ReadFile(void*,void*,unsigned long,unsigned long*,void*);
typedef struct{unsigned long nLength;void*lpSecurityDescriptor;int bInheritHandle;} HostSecAttr;
typedef struct{
 unsigned long cb;char*lpReserved;char*lpDesktop;char*lpTitle;
 unsigned long dwX,dwY,dwXSize,dwYSize,dwXCountChars,dwYCountChars,dwFillAttribute,dwFlags;
 unsigned short wShowWindow,cbReserved2;unsigned char*lpReserved2;
 void*hStdInput;void*hStdOutput;void*hStdError;
} HostStartupInfo;
typedef struct{void*hProcess;void*hThread;unsigned long dwProcessId,dwThreadId;} HostProcInfo;
#define HOST_HANDLE_FLAG_INHERIT 0x00000001u
#define HOST_STARTF_USESTDHANDLES 0x00000100u
#define HOST_CREATE_NO_WINDOW 0x08000000u
#define HOST_GENERIC_READ 0x80000000u
#define HOST_GENERIC_WRITE 0x40000000u
#define HOST_FILE_SHARE_READ 0x00000001u
#define HOST_FILE_SHARE_WRITE 0x00000002u
#define HOST_OPEN_EXISTING 3u
#define HOST_CREATE_ALWAYS 2u
#define HOST_STILL_ACTIVE 259u
#endif
#define HOST_MOVIE_W 240u
#define HOST_MOVIE_H 180u
#define HOST_MOVIE_BYTES (HOST_MOVIE_W*HOST_MOVIE_H*2u)
#define HOST_MOVIE_HANDLE 0x4d4f5649u /* opaque codec handle; never guest RAM */
typedef struct{
 int active,opened,paused,present,eof;
 u32 audio_only,audio_handle,audio_pending,audio_input_eof;
 void*proc;void*out;void*err_nul;void*in_nul;
 void*audio_proc;void*audio_out;
 u8 audio_data[131072];u32 audio_read,audio_count,audio_bytes;
 long long freq,next_qpc;
 u32 frames,have,wait_logs,duration_ms,frame_us,width,height,position_ms,seek_ms,state;
 u32 vx,vy,vw,vh;
 char ffmpeg[300];
#ifdef ANDROID
 char src_path[780],selected_path[780];
#else
 char src_path[260],selected_path[260];
#endif
 u16 src_w[260],selected_w[260],temporary_w[260];
 u8 frame[HOST_MOVIE_BYTES];
#ifdef HJX_INPROCESS_MOVIE
 HjxMovieDecoder *video_decoder,*audio_decoder;
 u32 padded_eof;
#endif
} HostMovie;
static HostMovie host_movie;
static int host_movie_clock(long long*stamp){
#ifdef _WIN32
 return QueryPerformanceCounter(stamp);
#elif defined(HJX_INPROCESS_MOVIE)
 struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now)!=0)return 0;
 *stamp=(long long)now.tv_sec*1000000000ll+now.tv_nsec;return 1;
#else
 (void)stamp;return 0;
#endif
}
static int host_movie_clock_frequency(long long*frequency){
#ifdef _WIN32
 return QueryPerformanceFrequency(frequency);
#elif defined(HJX_INPROCESS_MOVIE)
 *frequency=1000000000ll;return 1;
#else
 (void)frequency;return 0;
#endif
}
/* The video clock advances from its previous deadline. Using the actual
   paint time as the next base loses every late host slice and drifts behind
   the firmware's audio DMA clock during longer playback. */
static void host_movie_resume_clock(void){
 long long now;
 if(host_movie.active&&host_movie.freq&&host_movie_clock(&now))host_movie.next_qpc=now;
}
#ifdef EMU_TESTING
static u32 movie_soft_entries;
#endif
static u32 movie_le32(const u8*p){return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);}
static int host_movie_info(const u16*path){
 FILE*f;u8 b[4096];u32 n,i;
 f=host_fopen_w(path);if(!f)return 0;n=(u32)fread(b,1,sizeof(b),f);fclose(f);
 if(n<12||movie_le32(b)!=0x46464952u||movie_le32(b+8)!=0x20495641u)return 0;
 for(i=12;i+64<=n;i+=2){
  if(movie_le32(b+i)==0x68697661u&&movie_le32(b+i+4)>=56u){
   u32 us=movie_le32(b+i+8),frames=movie_le32(b+i+24);
   u32 w=movie_le32(b+i+40),h=movie_le32(b+i+44);u64 ms=(u64)us*frames/1000ull;
   if(!us||!frames||!w||!h||w>4096u||h>4096u||!ms||ms>0x7fffffffu)return 0;
   host_movie.frame_us=us;host_movie.duration_ms=(u32)ms;host_movie.width=w;host_movie.height=h;return 1;
  }
 }
 return 0;
}
static int host_movie_locate_ffmpeg(char*dst,u32 cap){
#ifdef _WIN32
 char mod[300],path[340];u32 n,i,j;
 n=GetModuleFileNameA(0,mod,sizeof(mod));if(n>=sizeof(mod))return 0;
 /* exe/tools, then ancestors/tools (also supports build-win and test DLLs). */
 for(i=n;i>0;i--)if(mod[i-1]=='\\'||mod[i-1]=='/'){
  mod[i-1]=0;
  sprintf(path,"%s\\tools\\ffmpeg.exe",mod);
  {FILE*f=fopen(path,"rb");if(f){fclose(f);for(j=0;path[j]&&j+1<cap;j++)dst[j]=path[j];dst[j]=0;return 1;}}
 }
 {FILE*f=fopen("tools\\ffmpeg.exe","rb");if(f){fclose(f);for(j=0;"tools\\ffmpeg.exe"[j]&&j+1<cap;j++)dst[j]="tools\\ffmpeg.exe"[j];dst[j]=0;return 1;}}
#else
 (void)dst;(void)cap;
#endif
 return 0;
}
static void host_movie_stop(void){
#ifdef HJX_INPROCESS_MOVIE
 hjx_movie_decoder_close(host_movie.video_decoder);host_movie.video_decoder=0;
 hjx_movie_decoder_close(host_movie.audio_decoder);host_movie.audio_decoder=0;
#endif
#ifdef _WIN32
 if(host_movie.audio_proc){unsigned long code=0;if(GetExitCodeProcess(host_movie.audio_proc,&code)&&code==HOST_STILL_ACTIVE)TerminateProcess(host_movie.audio_proc,1u);WaitForSingleObject(host_movie.audio_proc,200u);CloseHandle(host_movie.audio_proc);host_movie.audio_proc=0;}
 if(host_movie.audio_out){CloseHandle(host_movie.audio_out);host_movie.audio_out=0;}
 if(host_movie.proc){unsigned long code=0;if(GetExitCodeProcess(host_movie.proc,&code)&&code==HOST_STILL_ACTIVE)TerminateProcess(host_movie.proc,1u);WaitForSingleObject(host_movie.proc,200u);CloseHandle(host_movie.proc);host_movie.proc=0;}
 if(host_movie.out){CloseHandle(host_movie.out);host_movie.out=0;}
 if(host_movie.err_nul){CloseHandle(host_movie.err_nul);host_movie.err_nul=0;}
 if(host_movie.in_nul){CloseHandle(host_movie.in_nul);host_movie.in_nul=0;}
#endif
 host_movie.active=0;host_movie.have=0;host_movie.audio_read=host_movie.audio_count=0;audio_output_muted=0;
 audio_output_reset();
}
/* Stop/seek may restart the same source. Delete its private temporary file
   only on codec close, replacement, reset or emulator shutdown. */
static void host_movie_release_source(void){
 host_movie_stop();
#ifdef _WIN32
 if(host_movie.temporary_w[0])DeleteFileW(host_movie.temporary_w);
#elif defined(HJX_INPROCESS_MOVIE)
 if(host_movie.temporary_w[0])remove(host_movie.src_path);
#endif
 host_movie.temporary_w[0]=0;
}
static int host_movie_memory_source(u32 input){
#if defined(_WIN32) || defined(HJX_INPROCESS_MOVIE)
 u32 ctx=mr32(input+4),data=mr32(ctx),length=mr32(ctx+4),size;u8*p;FILE*f;
#ifdef _WIN32
 u16 directory[260];static const u16 prefix[]={'h','j','x',0},mode[]={'w','b',0};unsigned long n;
#else
 unsigned long n;char path[560];
#endif
 if(mr32(ctx+16)!=1u||length<12u||length>0x04000000u)return 0;
 p=(u8*)memptr(data,length);if(!p)return 0;
 if(movie_le32(p)==0x46464952u&&movie_le32(p+8)==0x20495641u){size=movie_le32(p+4);if(size<4u||size>length-8u)return 0;size+=8u;}
 else if((p[0]=='I'&&p[1]=='D'&&p[2]=='3')||(p[0]==255u&&(p[1]&0xe0u)==0xe0u))size=length;
 else if(length>=6u&&movie_le32(p)==0x4d412123u&&p[4]=='R'&&p[5]=='\n')size=length;
 else return 0;
#ifdef _WIN32
 n=GetTempPathW(260u,directory);if(!n||n>=260u)return 0;
 if(!GetTempFileNameW(directory,prefix,0,host_movie.temporary_w))return 0;
 f=_wfopen(host_movie.temporary_w,mode);
 if(!f){DeleteFileW(host_movie.temporary_w);host_movie.temporary_w[0]=0;return 0;}
 n=(unsigned long)fwrite(p,1,size,f);if(fclose(f)!=0||n!=size){DeleteFileW(host_movie.temporary_w);host_movie.temporary_w[0]=0;return 0;}
 memcopy(host_movie.src_w,host_movie.temporary_w,sizeof(host_movie.src_w));
 WideCharToMultiByte(0,0,host_movie.src_w,-1,host_movie.src_path,sizeof(host_movie.src_path),0,0);
#else
 if(!system_state_path[0])return 0;
 if(snprintf(path,sizeof(path),"%s.media.tmp",system_state_path)>=(int)sizeof(path))return 0;
 f=fopen(path,"wb");if(!f)return 0;
 n=(unsigned long)fwrite(p,1,size,f);if(fclose(f)!=0||n!=size){remove(path);return 0;}
 for(u32 index=0;;index++){host_movie.src_path[index]=path[index];host_movie.src_w[index]=host_movie.temporary_w[index]=(u8)path[index];if(!path[index])break;}
#endif
 {char b[140];sprintf(b,"movie: firmware memory media bytes=%u source=%08X",size,data);emu_system_log(b);}
 return 1;
#else
 (void)input;return 0;
#endif
}
/* Decode movie audio on a separate bounded pipe. It is returned through the
   original codec API and firmware mixer/DMA, never played directly on the host. */
static void host_movie_audio_pump(void){
#ifdef HJX_INPROCESS_MOVIE
 u32 at,room,n;int got;
 if(!host_movie.audio_decoder||host_movie.paused||host_movie.audio_input_eof)return;
 while(host_movie.audio_count<sizeof(host_movie.audio_data)){
  at=(host_movie.audio_read+host_movie.audio_count)%sizeof(host_movie.audio_data);
  room=(u32)sizeof(host_movie.audio_data)-host_movie.audio_count;
  n=(u32)sizeof(host_movie.audio_data)-at;if(n>room)n=room;if(n>4096u)n=4096u;
  got=hjx_movie_decoder_audio(host_movie.audio_decoder,host_movie.audio_data+at,n);
  if(got<=0){host_movie.audio_input_eof=1;break;}
  host_movie.audio_count+=(u32)got;
  if((u32)got<n)break;
 }
#else
#ifdef _WIN32
 unsigned long avail,got;u32 room,at,n;
 if(!host_movie.audio_out||host_movie.paused)return;
 while(host_movie.audio_count<sizeof(host_movie.audio_data)){
  if(!PeekNamedPipe(host_movie.audio_out,0,0,0,&avail,0)){host_movie.audio_input_eof=1;break;}
  if(!avail)break;
  at=(host_movie.audio_read+host_movie.audio_count)%sizeof(host_movie.audio_data);
  room=(u32)sizeof(host_movie.audio_data)-host_movie.audio_count;n=(u32)sizeof(host_movie.audio_data)-at;
  if(n>room)n=room;if(n>avail)n=avail;if(n>4096u)n=4096u;
  if(!ReadFile(host_movie.audio_out,host_movie.audio_data+at,n,&got,0)||!got)break;
  host_movie.audio_count+=got;
 }
#endif
#endif
}
static int host_movie_start(u32 seek_ms){
#ifdef HJX_INPROCESS_MOVIE
 char utf8[1024],line[400];long long now=0;
 host_movie_stop();
 if(!host_movie.opened)return 0;
#ifdef _WIN32
 if(!WideCharToMultiByte(65001u,0,host_movie.src_w,-1,utf8,sizeof(utf8),0,0))return 0;
#else
 snprintf(utf8,sizeof(utf8),"%s",host_movie.src_path);
#endif
 if(!host_movie_clock_frequency(&host_movie.freq)||!host_movie.freq)return 0;
 if(!host_movie.audio_only){
  host_movie.video_decoder=hjx_movie_decoder_open(utf8,seek_ms,0);
  if(!host_movie.video_decoder){emu_system_log("movie: in-process video open failed");return 0;}
 }
 host_movie.audio_decoder=hjx_movie_decoder_open(utf8,seek_ms,1);
 if(host_movie.audio_only&&!host_movie.audio_decoder){emu_system_log("movie: in-process audio open failed");host_movie_stop();return 0;}
 host_movie.active=1;host_movie.eof=0;host_movie.state=1;
 host_movie.audio_input_eof=host_movie.audio_decoder?0:1;host_movie.paused=0;
 host_movie.audio_bytes=0;host_movie.have=0;host_movie.frames=0;
 host_movie.padded_eof=0;
 host_movie.seek_ms=seek_ms;host_movie.position_ms=seek_ms;host_movie.wait_logs=0;
 host_movie_clock(&now);host_movie.next_qpc=now;
 sprintf(line,"movie: start in-process %.260s at=%u duration=%u frame_us=%u",host_movie.src_path,seek_ms,host_movie.duration_ms,host_movie.frame_us);emu_system_log(line);
 return 1;
#elif defined(_WIN32)
 char cmd[1024],errpath[340],line[400];u16 cmd_w[1400],exe_w[300];u32 i,n=0,wn;long long now=0;
 HostSecAttr sa;HostStartupInfo si;HostProcInfo pi;void*rd=0,*wr=0;
 host_movie_stop();
 if(!host_movie.opened)return 0;
 if(!host_movie.ffmpeg[0]&&!host_movie_locate_ffmpeg(host_movie.ffmpeg,sizeof(host_movie.ffmpeg))){emu_system_log("movie: ffmpeg not found");return 0;}
 if(!QueryPerformanceFrequency(&host_movie.freq)||!host_movie.freq)return 0;
 sa.nLength=(unsigned long)sizeof(sa);sa.lpSecurityDescriptor=0;sa.bInheritHandle=1;
 if(!CreatePipe(&rd,&wr,&sa,4096u)){emu_system_log("movie: CreatePipe fail");return 0;}
 if(!SetHandleInformation(rd,HOST_HANDLE_FLAG_INHERIT,0)){CloseHandle(rd);CloseHandle(wr);return 0;}
 host_movie.in_nul=CreateFileA("NUL",HOST_GENERIC_READ,HOST_FILE_SHARE_READ|HOST_FILE_SHARE_WRITE,&sa,HOST_OPEN_EXISTING,0,0);
 for(i=0;host_movie.ffmpeg[i];i++)if(host_movie.ffmpeg[i]=='\\'||host_movie.ffmpeg[i]=='/')n=i+1;
 for(i=0;i<n;i++)errpath[i]=host_movie.ffmpeg[i];errpath[n]=0;
 sprintf(errpath+n,"ffmpeg_movie_err.txt");
 host_movie.err_nul=CreateFileA(errpath,HOST_GENERIC_WRITE,HOST_FILE_SHARE_READ|HOST_FILE_SHARE_WRITE,&sa,HOST_CREATE_ALWAYS,0,0);
 if(host_movie.err_nul==(void*)~0ull)host_movie.err_nul=CreateFileA("NUL",HOST_GENERIC_WRITE,HOST_FILE_SHARE_READ|HOST_FILE_SHARE_WRITE,&sa,HOST_OPEN_EXISTING,0,0);
 if(host_movie.err_nul==(void*)~0ull)host_movie.err_nul=0;
 if(host_movie.in_nul==(void*)~0ull)host_movie.in_nul=0;
 if(!host_movie.in_nul||!host_movie.err_nul){CloseHandle(rd);CloseHandle(wr);host_movie_stop();emu_system_log("movie: stdio handle failure");return 0;}
 memzero(&si,sizeof(si));si.cb=(unsigned long)sizeof(si);si.dwFlags=HOST_STARTF_USESTDHANDLES;
 si.hStdOutput=wr;si.hStdError=host_movie.err_nul;si.hStdInput=host_movie.in_nul;
 sprintf(cmd,"\"%s\" -nostdin -hide_banner -loglevel error -ss %u.%03u -i \"",host_movie.ffmpeg,seek_ms/1000u,seek_ms%1000u);
 wn=(u32)MultiByteToWideChar(0,0,cmd,-1,cmd_w,1400);
 if(!wn||!MultiByteToWideChar(0,0,host_movie.ffmpeg,-1,exe_w,300)){CloseHandle(rd);CloseHandle(wr);host_movie_stop();return 0;}
 wn--;for(i=0;host_movie.src_w[i];i++)cmd_w[wn++]=host_movie.src_w[i];
 MultiByteToWideChar(0,0,host_movie.audio_only?"\" -vn -sn -dn -ac 2 -ar 44100 -f s16le -":"\" -an -sn -dn -f rawvideo -pix_fmt rgb565le -vf scale=240:180:force_original_aspect_ratio=decrease,pad=240:180:(ow-iw)/2:(oh-ih)/2:black -",-1,cmd_w+wn,(int)(1400u-wn));
 memzero(&pi,sizeof(pi));
 if(!CreateProcessW(exe_w,cmd_w,0,0,1,HOST_CREATE_NO_WINDOW,0,0,&si,&pi)){
  CloseHandle(rd);CloseHandle(wr);host_movie_stop();emu_system_log("movie: CreateProcess fail");return 0;
 }
 CloseHandle(wr);CloseHandle(pi.hThread);
 host_movie.active=1;host_movie.eof=0;host_movie.state=1;host_movie.audio_input_eof=0;host_movie.paused=0;
 if(host_movie.audio_only){host_movie.audio_proc=pi.hProcess;host_movie.audio_out=rd;}
 else{host_movie.proc=pi.hProcess;host_movie.out=rd;}
 /* A second decoder avoids multiplexing raw video and PCM in one pipe. */
 rd=wr=0;host_movie.audio_bytes=0;
 if(!host_movie.audio_only&&CreatePipe(&rd,&wr,&sa,4096u)){
  if(SetHandleInformation(rd,HOST_HANDLE_FLAG_INHERIT,0)){
   si.hStdOutput=wr;
   wn=(u32)MultiByteToWideChar(0,0,cmd,-1,cmd_w,1400);wn--;
   for(i=0;host_movie.src_w[i];i++)cmd_w[wn++]=host_movie.src_w[i];
   MultiByteToWideChar(0,0,"\" -vn -sn -dn -ac 2 -ar 44100 -f s16le -",-1,cmd_w+wn,(int)(1400u-wn));
   memzero(&pi,sizeof(pi));
   if(CreateProcessW(exe_w,cmd_w,0,0,1,HOST_CREATE_NO_WINDOW,0,0,&si,&pi)){host_movie.audio_proc=pi.hProcess;host_movie.audio_out=rd;rd=0;CloseHandle(pi.hThread);emu_system_log("movie: audio decoder running");}
  }
  if(rd)CloseHandle(rd);CloseHandle(wr);
 }
 host_movie.have=0;host_movie.frames=0;host_movie.seek_ms=seek_ms;host_movie.position_ms=seek_ms;host_movie.wait_logs=0;
 QueryPerformanceCounter(&now);host_movie.next_qpc=now;
 sprintf(line,"movie: start %.260s at=%u duration=%u frame_us=%u",host_movie.src_path,seek_ms,host_movie.duration_ms,host_movie.frame_us);emu_system_log(line);
 emu_system_log("movie: ffmpeg running");return 1;
#else
 (void)seek_ms;return 0;
#endif
}
static int host_movie_pump(void){
#if defined(_WIN32) || defined(HJX_INPROCESS_MOVIE)
 unsigned long avail=0,got=0,code=0,chunk;u32 budget=HOST_MOVIE_BYTES,x,y;long long now=0;u8*q;char line[120];
 if(host_movie.audio_only){if(host_movie.active&&!host_movie.paused)host_movie_audio_pump();return 0;}
#ifdef HJX_INPROCESS_MOVIE
 if(!host_movie.active||!host_movie.video_decoder||host_movie.paused)return 0;
#else
 if(!host_movie.active||!host_movie.out||host_movie.paused)return 0;
#endif
 host_movie_audio_pump();
#ifdef HJX_INPROCESS_MOVIE
 if(host_movie.have<HOST_MOVIE_BYTES){
  int decoded=hjx_movie_decoder_video(host_movie.video_decoder,host_movie.frame,HOST_MOVIE_BYTES);
  /* The FFmpeg CLI's default CFR sync repeats a final frame when AVI declares
     one more frame than the codec yields. Preserve that native task timing. */
  if(decoded==0&&!host_movie.padded_eof&&host_movie.frames&&host_movie.frame_us){
   u32 expected=(u32)(((u64)host_movie.duration_ms*1000u+host_movie.frame_us/2u)/host_movie.frame_us);
   u32 at=host_movie.frames+(u32)((u64)host_movie.seek_ms*1000u/host_movie.frame_us);
   if(at+1u==expected){host_movie.padded_eof=1;decoded=1;}
  }
  if(decoded<=0){
   sprintf(line,"movie: in-process end frames=%u result=%d",host_movie.frames,decoded);emu_system_log(line);
   host_movie.eof=1;host_movie.state=decoded==0?5:6;host_movie_stop();return 0;
  }
  host_movie.have=HOST_MOVIE_BYTES;
 }
#else
 /* Drain partial chunks, including with a 4 KiB pipe; never wait for a frame
    to fit in the pipe. Limit each pump to one frame's worth of work. */
 while(host_movie.have<HOST_MOVIE_BYTES&&budget){
  if(!PeekNamedPipe(host_movie.out,0,0,0,&avail,0)){
   GetExitCodeProcess(host_movie.proc,&code);sprintf(line,"movie: end code=%lu frames=%u partial=%u",code,host_movie.frames,host_movie.have);emu_system_log(line);
   host_movie.eof=1;host_movie.state=5;host_movie_stop();return 0;
  }
  if(!avail)break;
  chunk=HOST_MOVIE_BYTES-host_movie.have;if(chunk>avail)chunk=avail;if(chunk>budget)chunk=budget;
  if(!ReadFile(host_movie.out,host_movie.frame+host_movie.have,chunk,&got,0)||!got){emu_system_log("movie: pipe read failed");host_movie.eof=1;host_movie.state=6;host_movie_stop();return 0;}
  host_movie.have+=(u32)got;budget-=(u32)got;
 }
 if(host_movie.have<HOST_MOVIE_BYTES){
  /* Process exit is not EOF until stdout is drained. Peek/Read owns EOF. */
  if(host_movie.wait_logs<64u){host_movie.wait_logs++;sprintf(line,"movie: wait have=%u avail=%lu",host_movie.have,avail);emu_system_log(line);}return 1;
 }
#endif
 if(!host_movie.present)return 1;
 host_movie_clock(&now);if(now<host_movie.next_qpc)return 1;
 q=(u8*)memptr(0x30488624u,LCD_W*LCD_H*2u);if(!q)return 0;
 u32 landscape=lcd.regs[3]&8u,screen_w=landscape?LCD_H:LCD_W,screen_h=landscape?LCD_W:LCD_H;
 for(y=0;y<host_movie.vh;y++)for(x=0;x<host_movie.vw;x++){
  u32 lx=host_movie.vx+x,ly=host_movie.vy+y;if(lx>=screen_w||ly>=screen_h)continue;
  u32 src=((y*HOST_MOVIE_H/host_movie.vh)*HOST_MOVIE_W+x*HOST_MOVIE_W/host_movie.vw)*2u;
  u32 dst=ly*screen_w+lx,panel=landscape?(LCD_H-1u-lx)*LCD_W+ly:dst;
  u16 pix=(u16)(host_movie.frame[src]|((u16)host_movie.frame[src+1]<<8));
  q[dst*2]=(u8)pix;q[dst*2+1]=(u8)(pix>>8);lcd_panel565[panel]=pix;
 }
 host_movie.position_ms=host_movie.seek_ms+(u32)((u64)host_movie.frames*host_movie.frame_us/1000ull);
 host_movie.have=0;host_movie.frames++;lcd_primary_scanout_active=1u;lcd_primary_dirty=0u;lcd_generation++;trace_frame++;
 sprintf(line,"movie: frame %u pts=%u viewport=%u,%u %ux%u",host_movie.frames,host_movie.position_ms,host_movie.vx,host_movie.vy,host_movie.vw,host_movie.vh);emu_system_log(line);
 {long long period=(long long)((u64)host_movie.freq*host_movie.frame_us/1000000ull);
  if(period<1)period=1;
  host_movie.next_qpc+=period;
  /* A stalled host must not burst through seconds of stale frames. Keep at
     most two overdue frames while retaining ordinary slice jitter. */
  if(host_movie.next_qpc<now-2*period)host_movie.next_qpc=now-period;
 }
 return 1;
#else
 return 0;
#endif
}

static void sd_put16(u8*p,u32 v){p[0]=(u8)v;p[1]=(u8)(v>>8);}
static void sd_put32(u8*p,u32 v){p[0]=(u8)v;p[1]=(u8)(v>>8);p[2]=(u8)(v>>16);p[3]=(u8)(v>>24);}
static u32 sd_wlen(const u16*s){u32 n=0;while(s[n])n++;return n;}
static u16 sd_upper(u16 c){return c>='a'&&c<='z'?(u16)(c-'a'+'A'):c;}
static int sd_short_char(u16 c){c=sd_upper(c);return (c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';}
/* Return whether this is an ordinary 8.3 name. Generated aliases preserve
   the extension, so the firmware's native AVI/WAV filters still work. */
static int sd_name83(const u16*name,u8 out[11],u32 index,int force){
 u32 len=sd_wlen(name),dot=len,bl,el,i;int valid=1;
 for(i=0;i<len;i++)if(name[i]=='.')dot=i;
 bl=dot;el=dot<len?len-dot-1u:0u;
 if(!bl||bl>8u||el>3u||(dot<len&&!el))valid=0;
 for(i=0;i<len;i++)if(i!=dot&&!sd_short_char(name[i]))valid=0;
 for(i=0;i<11;i++)out[i]=' ';
 if(valid&&!force){for(i=0;i<bl;i++)out[i]=(u8)sd_upper(name[i]);}
 else{static const char hex[]="0123456789ABCDEF";out[0]='F';for(i=0;i<7;i++)out[1+i]=hex[(index>>(4*(6-i)))&15u];}
 if(el&&el<=3u){int ext_ok=1;for(i=0;i<el;i++)if(!sd_short_char(name[dot+1+i]))ext_ok=0;
  if(ext_ok)for(i=0;i<el;i++)out[8+i]=(u8)sd_upper(name[dot+1+i]);}
 return valid&&!force;
}
static int sd_same83(const u8*a,const u8*b){u32 i;for(i=0;i<11;i++)if(a[i]!=b[i])return 0;return 1;}
static u32 sd_short_text(const u8*n,u16*out){u32 i,k=0;for(i=0;i<8&&n[i]!=' ';i++)out[k++]=n[i];
 if(n[8]!=' '){out[k++]='.';for(i=8;i<11&&n[i]!=' ';i++)out[k++]=n[i];}out[k]=0;return k;}
static void sd_dir_dot(u8*d,const char*name,u32 first){
 for(int i=0;i<11;i++)d[i]=' ';
 for(int i=0;name[i]&&i<11;i++)d[i]=(u8)name[i];
 d[11]=0x10;sd_put16(d+26,first);sd_put32(d+28,0);
}
/* VFAT entries precede their short alias in descending ordinal order. */
static u32 sd_dir_file(u8*d,const HostFile*f){
 static const u8 positions[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
 u32 len=sd_wlen(f->name),ord,i;u8 sum=0;
 for(i=0;i<11;i++)sum=(u8)(((sum&1u)?0x80u:0u)+(sum>>1)+f->n83[i]);
 for(ord=f->lfn_slots;ord;ord--,d+=32){
  memzero(d,32);d[0]=(u8)(ord|(ord==f->lfn_slots?0x40u:0u));d[11]=0x0f;d[13]=sum;
  for(i=0;i<13;i++){u32 at=(ord-1u)*13u+i;sd_put16(d+positions[i],at<len?f->name[at]:at==len?0u:0xffffu);}
 }
 for(i=0;i<11;i++)d[i]=f->n83[i];d[11]=f->is_dir?0x10u:0x20u;sd_put16(d+26,f->first);sd_put32(d+28,f->is_dir?0u:f->size);
 return 1u+f->lfn_slots;
}
#if defined(_WIN32) || defined(ANDROID)
static int vol_add_host(HostVol*v,const u16*dir,const u16*name,u32 size,u8 is_dir,u16 parent){
 HostFile*f;u16 short_name[13];u32 n=sd_wlen(dir),len=sd_wlen(name),i,j,serial=v->file_n,slots=parent==0xffffu?0u:2u;int force=0;
 if(v->file_n>=SD_FILES||!len||len>255u||n+len+2u>260u)return 0;
 if(is_dir&&v->dir_pool_n>=SD_DIRS)return 0;
 f=&v->files[v->file_n];memzero(f,sizeof(*f));
 for(i=0;i<n;i++)f->path_w[i]=dir[i];
#ifdef _WIN32
 if(n&&dir[n-1]!='\\'&&dir[n-1]!='/')f->path_w[n++]='\\';
#else
 if(n&&dir[n-1]!='/')f->path_w[n++]='/';
#endif
 for(i=0;i<=len;i++){f->name[i]=name[i];f->path_w[n+i]=name[i];}
#ifdef _WIN32
 WideCharToMultiByte(0,0,f->path_w,-1,f->path,sizeof(f->path),0,0);
#else
 if(!host_wide_to_utf8(f->path_w,f->path,sizeof(f->path)))return 0;
#endif
 /* Check natural and generated names alike: an existing F0000001.AVI must
    never collide with an alias, nor may two long ASCII names be truncated. */
 for(;;){
  sd_name83(name,f->n83,serial,force);
  for(j=0;j<v->file_n;j++)if(v->files[j].parent==parent&&sd_same83(f->n83,v->files[j].n83))break;
  if(j==v->file_n)break;force=1;serial+=SD_FILES;
 }
 sd_short_text(f->n83,short_name);for(i=0;i<len&&i<12u&&name[i]==short_name[i];i++){}
 f->lfn_slots=(i==len&&short_name[i]==0)?0u:(u8)((len+12u)/13u);
 for(j=0;j<v->file_n;j++)if(v->files[j].parent==parent)slots+=1u+v->files[j].lfn_slots;
 if(slots+f->lfn_slots+2u>(parent==0xffffu?512u:SD_DIR_BYTES/32u))return 0;
 f->size=size;f->is_dir=is_dir;f->parent=parent;f->dir_id=is_dir?(u8)v->dir_pool_n++:0xffu;
 v->file_n++;return 1;
}
static void vol_scan_dir(HostVol*v,const u16*dir,u16 parent){
#ifdef _WIN32
 u16 pat[263];u32 n=sd_wlen(dir),i;SdFind fd;void*h;
 for(i=0;i<n;i++)pat[i]=dir[i];pat[n++]='\\';pat[n++]='*';pat[n]=0;
 h=FindFirstFileW(pat,&fd);if(!h||h==(void*)~0ull)return;
 do{
  /* Do not follow host junctions/symlinks into loops or outside the volume. */
  if(fd.name[0]=='.'||(fd.attr&0x400u))continue;
  if(fd.size_hi||fd.size_lo>0x7fe00000u)continue;
  if(!vol_add_host(v,dir,fd.name,(fd.attr&0x10u)?0u:fd.size_lo,(fd.attr&0x10u)?1u:0u,parent))
   emu_system_log("storage: skipped entry (host path/FAT capacity limit)");
 }while(FindNextFileW(h,&fd));
 FindClose(h);
#else
 char utf8[780],path[1560];u16 name[256];struct dirent*entry;struct stat info;DIR*directory;
 if(!host_wide_to_utf8(dir,utf8,sizeof(utf8)))return;
 directory=opendir(utf8);if(!directory)return;
 while((entry=readdir(directory))!=0){
  if(entry->d_name[0]=='.'||!host_utf8_to_wide(entry->d_name,name,256u))continue;
  if(snprintf(path,sizeof(path),"%s/%s",utf8,entry->d_name)>=(int)sizeof(path))continue;
  if(lstat(path,&info)||(!S_ISREG(info.st_mode)&&!S_ISDIR(info.st_mode)))continue;
  if(info.st_size<0||info.st_size>0x7fe00000u)continue;
  if(!vol_add_host(v,dir,name,S_ISDIR(info.st_mode)?0u:(u32)info.st_size,S_ISDIR(info.st_mode)?1u:0u,parent))
   emu_system_log("storage: skipped entry (host path/FAT capacity limit)");
 }
 closedir(directory);
#endif
}
#endif
static void vol_refresh(HostVol*v){
 host_fc_close();
 v->ready=1;v->file_n=0;v->dir_pool_n=0;v->clusters=512;memzero(v->files,sizeof(v->files));memzero(v->fat,sizeof(v->fat));memzero(v->rootdir,sizeof(v->rootdir));memzero(v->map_file,sizeof(v->map_file));memzero(v->map_off,sizeof(v->map_off));memzero(v->bpb,512);memzero(v->dir_pool,sizeof(v->dir_pool));
#if defined(_WIN32) || defined(ANDROID)
 if(v->root[0]){
  u16 root_w[260];
#ifdef _WIN32
  if(MultiByteToWideChar(0,0,v->root,-1,root_w,260)){
#else
  if(host_utf8_to_wide(v->root,root_w,260u)){
#endif
   vol_scan_dir(v,root_w,0xffffu);
   /* Breadth-first discovery: parents are always present before children. */
   for(u32 di=0;di<v->file_n;di++)if(v->files[di].is_dir)vol_scan_dir(v,v->files[di].path_w,(u16)di);
  }
 }
#endif
 /* Directory tables need space before cluster assignment finalizes sizes. */
 for(u32 i=0;i<v->file_n;i++){
  HostFile*d=&v->files[i];u32 ents,need;
  if(!d->is_dir)continue;
  ents=2u;for(u32 j=0;j<v->file_n;j++)if(v->files[j].parent==i)ents+=1u+v->files[j].lfn_slots;
  need=ents*32u;if(need>SD_DIR_BYTES)need=SD_DIR_BYTES;d->size=need;if(d->size<512u)d->size=512u;
 }
 u32 used=2;
 for(u32 i=0;i<v->file_n;i++){u32 n=(v->files[i].size+SD_CLUSTER-1u)/SD_CLUSTER;if(!n)n=1;if(used+n>SD_CLUSTER_LIMIT){v->file_n=i;break;}used+=n;}
 if(used<4088u)used=4088u;v->clusters=used-2u;
 v->fat_sectors=(v->clusters+2u+255u)/256u;if(v->fat_sectors*512u>sizeof(v->fat))v->fat_sectors=(u32)sizeof(v->fat)/512u;
 v->data_rel=1u+v->fat_sectors*2u+32u;
 u32 part=v->data_rel+v->clusters*SD_SPC;v->bytes=part*512u;
 sd_put16(v->fat,0xFFF8);sd_put16(v->fat+2,0xFFFF);
 u32 cl=2;
 for(u32 i=0;i<v->file_n;i++){
  u32 n=(v->files[i].size+SD_CLUSTER-1u)/SD_CLUSTER;if(!n)n=1;v->files[i].first=cl;
  for(u32 k=0;k<n&&cl<SD_CLUSTER_LIMIT;k++,cl++){v->map_file[cl]=(u16)(i+1u);v->map_off[cl]=k*SD_CLUSTER;sd_put16(v->fat+cl*2u,k+1u==n?0xFFFFu:cl+1u);}
 }
 for(u32 i=0;i<v->file_n;i++){
  HostFile*d=&v->files[i];u8*buf;u32 slot;
  if(!d->is_dir||d->dir_id>=SD_DIRS)continue;
  buf=v->dir_pool[d->dir_id];memzero(buf,SD_DIR_BYTES);slot=0;
  sd_dir_dot(buf+slot*32u,".",d->first);slot++;
  sd_dir_dot(buf+slot*32u,"..",d->parent==0xffffu?0u:v->files[d->parent].first);slot++;
  for(u32 j=0;j<v->file_n&&(slot+1u)*32u<=SD_DIR_BYTES;j++){
   if(v->files[j].parent!=i)continue;
   slot+=sd_dir_file(buf+slot*32u,&v->files[j]);
  }
 }
 {u32 slot=0;
  for(u32 i=0;i<v->file_n;i++){
   if(v->files[i].parent!=0xffffu)continue;
   if(slot+1u>512u)break;
   slot+=sd_dir_file(v->rootdir+slot*32u,&v->files[i]);
  }
 }
 v->bpb[0]=0xEB;v->bpb[1]=0x3C;v->bpb[2]=0x90;memcopy(v->bpb+3,"MSDOS5.0",8);
 sd_put16(v->bpb+11,512);v->bpb[13]=SD_SPC;sd_put16(v->bpb+14,1);v->bpb[16]=2;sd_put16(v->bpb+17,512);
 if(part<65536u)sd_put16(v->bpb+19,part);v->bpb[21]=0xF8;sd_put16(v->bpb+22,v->fat_sectors);sd_put16(v->bpb+24,63);sd_put16(v->bpb+26,255);sd_put32(v->bpb+28,0);
 if(part>=65536u)sd_put32(v->bpb+32,part);
 v->bpb[36]=0x80;v->bpb[38]=0x29;memcopy(v->bpb+43,v->label,11);memcopy(v->bpb+54,"FAT16   ",8);v->bpb[510]=0x55;v->bpb[511]=0xAA;
}
static int vol_fill_sector(HostVol*v,u32 sector,u8*dst){
 memzero(dst,512);
 {u32 id=v->ov_id;for(u32 i=0;i<vol_ov_n[id];i++)if(vol_ov_sec[id][i]==sector){memcopy(dst,vol_ov[id][i],512);return 1;}}
 if(!v->ready)vol_refresh(v);
 if((u64)sector*512ull>=v->bytes)return 1;
 if(sector==0u){memcopy(dst,v->bpb,512);return 1;}
 if(sector<1u+v->fat_sectors){u32 o=(sector-1u)*512u;if(o<sizeof(v->fat))memcopy(dst,v->fat+o,512);return 1;}
 if(sector<1u+v->fat_sectors*2u){u32 o=(sector-1u-v->fat_sectors)*512u;if(o<sizeof(v->fat))memcopy(dst,v->fat+o,512);return 1;}
 if(sector<v->data_rel){u32 o=(sector-(1u+v->fat_sectors*2u))*512u;if(o<sizeof(v->rootdir))memcopy(dst,v->rootdir+o,512);return 1;}
 u32 data=(sector-v->data_rel)*512u;u32 cl=data/SD_CLUSTER+2u;u32 inner=data%SD_CLUSTER;
 if(cl>=SD_CLUSTER_LIMIT||!v->map_file[cl])return 1;
 HostFile*f=&v->files[v->map_file[cl]-1u];u32 off=v->map_off[cl]+inner;if(off>=f->size)return 1;
 if(f->is_dir){
  if(f->dir_id<SD_DIRS&&off<SD_DIR_BYTES){u32 n=512u;if(off+n>f->size)n=f->size-off;if(off+n>SD_DIR_BYTES)n=SD_DIR_BYTES-off;memcopy(dst,v->dir_pool[f->dir_id]+off,n);}
  return 1;
 }
 {const char*p=f->path;u32 n=0;while(p[n])n++;
  if(n>=4u){char a=p[n-4],b=p[n-3],c=p[n-2],d=p[n-1];
   if(a=='.'&&((b=='a'||b=='A')&&(c=='v'||c=='V')&&(d=='i'||d=='I'))){
    u32 i=0;int changed=0;char line[220];
    while(p[i]&&i+1u<sizeof(host_media_path)){if(host_media_path[i]!=p[i])changed=1;host_media_path[i]=p[i];i++;}
    if(host_media_path[i])changed=1;host_media_path[i]=0;
    if(changed){
     sprintf(line,"media: path %.190s",host_media_path);emu_system_log(line);
     /* Diagnostic only. The codec open resolves the actual guest filename. */
    }
   }}}
 {FILE*fp=host_fc_open(f->path_w);if(!fp)return 1;
  if(fseek(fp,(long)off,SEEK_SET)==0){u32 n=512u;if(off+n>f->size)n=f->size-off;if(fread(dst,1,n,fp)!=n)memzero(dst,512);}
  return 1;}
}
static u32 vol_hist_addr[8],vol_hist_n;
static int vol_read(HostVol*v,u32 addr,u8*dst,u32 len){
 if(!v->root[0]||!dst||!len||(u64)addr+len>0x100000000ull)return 0;if(addr==0u&&!vol_ov_n[v->ov_id])v->ready=0;if(!v->ready)vol_refresh(v);
 v->reads++;v->last_addr=addr;if(vol_hist_n<8u)vol_hist_addr[vol_hist_n++]=addr;
 u32 done=0;while(done<len){u32 pos=addr+done;u8 sec[512];u32 inner=pos&511u;u32 n=512u-inner;if(n>len-done)n=len-done;vol_fill_sector(v,pos>>9,sec);memcopy(dst+done,sec+inner,n);done+=n;}
 return 1;
}
static int vol_write(HostVol*v,u32 addr,const u8*src,u32 len){
 if(!v->root[0]||!src||!len)return 0;if(!v->ready)vol_refresh(v);
 u32 id=v->ov_id;
 if((u64)addr+len>v->bytes)return 0;
 /* Reserve the whole write before changing any sector. A full overlay is
    a write failure, never success with the last sectors silently dropped. */
 {u32 needed=0,first=addr>>9,last=(addr+len-1u)>>9;
  for(u32 sec=first;sec<=last;sec++){
   u32 i;for(i=0;i<vol_ov_n[id];i++)if(vol_ov_sec[id][i]==sec)break;
   if(i==vol_ov_n[id]&&++needed>VOL_OV-vol_ov_n[id])return 0;
  }
 }
 while(len){
  u32 sector=addr>>9,inner=addr&511u,n=512u-inner,slot;if(n>len)n=len;
  slot=vol_ov_n[id];
  for(u32 i=0;i<vol_ov_n[id];i++)if(vol_ov_sec[id][i]==sector){slot=i;break;}
  if(slot==vol_ov_n[id]){vol_fill_sector(v,sector,vol_ov[id][slot]);vol_ov_sec[id][slot]=sector;vol_ov_n[id]++;}
  memcopy(vol_ov[id][slot]+inner,src,n);addr+=n;src+=n;len-=n;
 }
 return 1;
}
static int c_nand_fill(u32 row){
 u8 page[2048];u32 pages;
 if(!c_vol.root[0])return 0;
 if(!c_vol.ready)vol_refresh(&c_vol);
 pages=(c_vol.bytes+2047u)/2048u;if(row>=pages)return 0;
 if(!vol_read(&c_vol,row*2048u,page,2048u))return 0;
 for(u32 i=0;i<512u;i++){u8*p=page+i*4u;nand_sram[i]=(u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);}
 for(u32 i=512u;i<1024u;i++)nand_sram[i]=0xffffffffu;
 return 1;
}
static void sd_note_card(void){if(!sd_vol.ready)vol_refresh(&sd_vol);mw8(0x30473895u,1u);mw32(0x304738a4u+0x34u,sd_vol.bytes);}
/* Resolve native long names and short aliases against the mounted table;
   never concatenate a guest path or use a stale last-read AVI. */
static int host_movie_select(u32 ptr){
 HostVol*v;u32 at=2,parent=0xffffu,i,j,n;u16 part[256],short_name[13];
 host_movie.selected_path[0]=0;host_movie.selected_w[0]=0;
 if(!ptr||mr16(ptr+2)!=':')return 0;
 v=sd_upper(mr16(ptr))=='C'?&c_vol:sd_upper(mr16(ptr))=='D'?&sd_vol:0;
 if(!v||!v->root[0])return 0;if(!v->ready)vol_refresh(v);
 while(at<260u){
  while(at<260u&&(mr16(ptr+at*2)=='\\'||mr16(ptr+at*2)=='/'))at++;
  for(n=0;n<255u&&at<260u;n++,at++){
   u16 c=mr16(ptr+at*2);if(!c||c=='\\'||c=='/')break;part[n]=c;
  }
  part[n]=0;if(!n||at>=260u)return 0;
  for(i=0;i<v->file_n;i++){
   HostFile*f=&v->files[i];if(f->parent!=parent)continue;
   for(j=0;j<n&&sd_upper(part[j])==sd_upper(f->name[j]);j++){}if(j==n&&!f->name[j])break;
   sd_short_text(f->n83,short_name);
   for(j=0;j<n&&j<12u&&sd_upper(part[j])==short_name[j];j++){}if(j==n&&!short_name[j])break;
  }
  if(i==v->file_n)return 0;
  if(!mr16(ptr+at*2)){
   HostFile*f=&v->files[i];if(f->is_dir)return 0;
   memcopy(host_movie.selected_w,f->path_w,sizeof(host_movie.selected_w));
   for(j=0;f->path[j]&&j+1<sizeof(host_movie.selected_path);j++)host_movie.selected_path[j]=f->path[j];host_movie.selected_path[j]=0;return 1;
  }
  if(!v->files[i].is_dir)return 0;parent=i;
 }
 return 0;
}
static int media_is_native_audio(u32 input){
 u32 ctx=mr32(input+4),data=mr32(ctx),len=mr32(ctx+4),base=mr32(ctx+12);u8 head[12];int have=0;
 memzero(head,sizeof(head));
 if(mr32(ctx+16)==1u&&len>=12u){u8*p=(u8*)memptr(data,12);if(p){memcopy(head,p,12);have=1;}}
 else if(host_movie.selected_w[0]){FILE*f=host_fopen_w(host_movie.selected_w);if(f){if(fseek(f,(long)base,SEEK_SET)==0)have=fread(head,1,12,f)==12;fclose(f);}}
 else if(mr32(data+4)==512u&&len>=12u){system_vflash_read((u64)mr32(data+20)+base,head,12);have=1;}
 if(!have)return 0;
 if(movie_le32(head)==0x4d412123u&&head[4]=='R'&&head[5]=='\n')return 2;
 if(movie_le32(head)==0x46464952u&&movie_le32(head+8)==0x45564157u)return 1;
 if(head[0]=='I'&&head[1]=='D'&&head[2]=='3')return 2;
 /* MPEG audio sync with legal version/layer/bitrate, never RIFF AVI. */
 if(head[0]==255u&&(head[1]&0xe0u)==0xe0u&&(head[1]&6u)&&((head[1]>>3)&3u)!=1u&&(head[2]&0xf0u)!=0xf0u&&(head[2]&0xf0u)&&(head[2]&12u)!=12u)return 2;
 return 0;
}
static int system_movie_hle(u32 a){
 if(!system_irq_enabled)return 0;
#ifdef EMU_TESTING
 if(a==0x302364f4u||a==0x30237dfcu||a==0x30237fccu)movie_soft_entries++;
#endif
#ifdef MOVIE_TRACE
 static u32 trace_count;
 if(((a>=0x301e1900u&&a<0x301e3000u)||a==0x30206638u||a==0x30206c4cu) && (mr32(a)&0xffff0000u)==0xe92d0000u && trace_count++<500u){char b[180];sprintf(b,"probe: pc=%08X lr=%08X r0=%08X r1=%08X r2=%08X r3=%08X",a,cpu.r[14],cpu.r[0],cpu.r[1],cpu.r[2],cpu.r[3]);emu_system_log(b);}
#endif
 /* Keep the real player task, frame-fetch, sleep, messages and audio task.
    Only the codec API and pixel conversion/display are host-backed. */
 if(a==0x301e56e8u||a==0x301e5398u||a==0x301e62e8u){host_movie_select(mr32(cpu.r[0]));return 0;}
 if(a==0x30206638u){
  u32 i,out=cpu.r[1];char line[300];
  /* Mode 0 is shared by AMR pronunciation and video. Classify the stream,
     not the caller/mode, so AVI can never enter the ARM Xvid decoder. */
  {int kind=media_is_native_audio(cpu.r[0]);host_movie.audio_pending=0;
   if(kind==2){
    host_movie_release_source();
    memcopy(host_movie.src_path,host_movie.selected_path,sizeof(host_movie.src_path));memcopy(host_movie.src_w,host_movie.selected_w,sizeof(host_movie.src_w));
    host_movie.audio_pending=host_movie_memory_source(cpu.r[0])||host_movie.src_w[0];
   }
   if(kind){emu_system_log(host_movie.audio_pending?"audio: compressed host codec pending":"audio: native firmware decoder");return 0;}
  }
  host_movie_release_source();host_movie.audio_only=host_movie.audio_handle=0;host_movie.opened=0;host_movie.present=0;host_movie.paused=0;host_movie.eof=0;host_movie.state=0;
  for(i=0;host_movie.selected_path[i]&&i+1<sizeof(host_movie.src_path);i++)host_movie.src_path[i]=host_movie.selected_path[i];host_movie.src_path[i]=0;
  memcopy(host_movie.src_w,host_movie.selected_w,sizeof(host_movie.src_w));
  host_movie_memory_source(cpu.r[0]);
  if(!host_movie_info(host_movie.src_w)){host_movie_release_source();emu_system_log("movie: open rejected (missing/unsupported AVI)");if(out)mw8(out,1);hret(0);return 1;}
  host_movie.opened=1;host_movie.frames=0;host_movie.position_ms=host_movie.seek_ms=0;
  if(out){for(i=0;i<16;i++)mw8(out+i,0);mw32(out+4,4096u);mw32(out+8,HOST_MOVIE_BYTES);}
  sprintf(line,"movie: open %.260s duration=%u",host_movie.src_path,host_movie.duration_ms);emu_system_log(line);
  hret(HOST_MOVIE_HANDLE);return 1;
 }
 /* Native open supplies format/duration and owns its allocations. Only the
    MPEG decoder calls use the host, replacing the missing DSP service. */
 if(a==0x30206854u&&host_movie.audio_pending){
  host_movie_stop();host_movie.audio_pending=0;host_movie.audio_only=1;host_movie.audio_handle=cpu.r[0];host_movie.opened=1;host_movie.eof=host_movie.paused=0;
  host_movie.duration_ms=mr32(cpu.r[0]+0x58+4);host_movie.seek_ms=host_movie.position_ms=host_movie.audio_bytes=0;
 }
 if(cpu.r[0]==HOST_MOVIE_HANDLE||(host_movie.audio_handle&&cpu.r[0]==host_movie.audio_handle)){
  u32 out=cpu.r[1],i;
  if(a==0x30206854u){ /* 44-byte movie information */
   if(host_movie.audio_only){for(i=0;i<44;i++)mw8(out+i,mr8(cpu.r[0]+0x58+i));mw32(out+0x1c,44100);mw16(out+0x20,2);mw16(out+0x22,16);hret(1);return 1;}
   for(i=0;i<44;i++)mw8(out+i,0);
   mw8(out,2);mw8(out+1,1);mw8(out+2,1);mw32(out+4,host_movie.duration_ms);
   mw16(out+0x10,host_movie.width);mw16(out+0x12,host_movie.height);
   mw32(out+0x1c,44100u);mw16(out+0x20,2);mw16(out+0x22,16);hret(1);return 1;
  }
  if(a==0x302065d0u){u32 native=host_movie.audio_only;emu_system_log("media: close");host_movie_release_source();host_movie.opened=host_movie.present=0;host_movie.audio_handle=host_movie.audio_only=0;if(native)return 0;hret(1);return 1;}
  if(a==0x302069c0u){if(host_movie.audio_only){host_movie.paused=0;audio_output_muted=0;}if(!host_movie.active&&!host_movie.eof&&!host_movie_start(host_movie.seek_ms)){host_movie.eof=1;host_movie.state=6;hret(0xffffffffu);}else hret(0);return 1;}
  if(a==0x30206bb8u){hret(host_movie.state);return 1;}
  if(a==0x30206a60u){emu_system_log("movie: stop");host_movie_stop();host_movie.eof=1;host_movie.state=5;host_movie.present=0;hret(1);return 1;}
  if(a==0x30206a8cu){host_movie.paused=1;audio_output_muted=1;audio_output_reset();hret(1);return 1;}
  if(a==0x30206bc8u){ /* RGB output descriptor: size, width/height, buffer */
   mw16(out+4,HOST_MOVIE_W);mw16(out+6,HOST_MOVIE_H);
   /* 0F88 is replaced; no YUV data goes to the original converter. */
   hret(host_movie.eof?0xffffffffu:host_movie.position_ms);return 1;
  }
  if(a==0x30206c18u){
   /* Preserve the original audio task/device handshake until video EOF.
      Underflow or a video without audio supplies silence, not premature EOF
      (which disables native STOP/seek through IsMoviePlaying). */
   u32 n=mr32(out+8),dst=mr32(out+16);u8*p;
   if(host_movie.eof){hret(0);return 1;}
   if(n>4096u)n=4096u;n&=~3u;p=n?(u8*)memptr(dst,n):0;
   if(!p){hret(0xffffffffu);return 1;}
   host_movie_audio_pump();memzero(p,n);
   if(host_movie.audio_only&&host_movie.audio_input_eof&&!host_movie.audio_count){host_movie.eof=1;host_movie.state=5;hret(0);return 1;}
   {u32 take=host_movie.audio_count;if(take>n)take=n;take&=~3u;
    for(i=0;i<take;i++)p[i]=host_movie.audio_data[(host_movie.audio_read+i)%sizeof(host_movie.audio_data)];
    host_movie.audio_read=(host_movie.audio_read+take)%sizeof(host_movie.audio_data);host_movie.audio_count-=take;host_movie.audio_bytes+=take;
    if(host_movie.audio_only)host_movie.position_ms=host_movie.seek_ms+(u32)((u64)host_movie.audio_bytes*1000u/176400u);
   }
   hret(n);return 1;
  }
  if(a==0x30206c4cu){u32 pos=out;if(pos>=host_movie.duration_ms)pos=host_movie.duration_ms-1u;emu_system_log("movie: seek");hret(host_movie_start(pos)?pos:0xffffffffu);return 1;}
  if(a==0x30206ab8u||a==0x30206b38u||a==0x30206894u||a==0x30206cc0u||a==0x30206ce4u){hret(1);return 1;}
 }
 if(a==0x301e04f8u&&host_movie.opened&&!host_movie.audio_only){hret(host_movie.position_ms);return 1;}
 if(a==0x301e042cu&&host_movie.opened&&!host_movie.audio_only){hret(0);return 1;}
 if(a==0x301e0f88u&&host_movie.opened&&!host_movie.audio_only){
  u32 owner=mr32(cpu.r[0]+0x44u),x=mr32(owner+0x20),y=mr32(owner+0x24),w=mr32(owner+0x28),h=mr32(owner+0x2c);
  u32 screen_w=(lcd.regs[3]&8u)?LCD_H:LCD_W,screen_h=(lcd.regs[3]&8u)?LCD_W:LCD_H;
  if(x<screen_w&&y<screen_h&&w&&h){if(w>screen_w-x)w=screen_w-x;if(h>screen_h-y)h=screen_h-y;host_movie.vx=x;host_movie.vy=y;host_movie.vw=w;host_movie.vh=h;host_movie.present=1;}
  hret(0);return 1;
 }
 if(a==0x301e1da0u&&host_movie.opened){int was_paused=host_movie.paused;host_movie.paused=cpu.r[5]?1:0;audio_output_muted=host_movie.paused;if(host_movie.paused)audio_output_reset();else if(was_paused)host_movie_resume_clock();emu_system_log(host_movie.paused?"movie: pause":"movie: resume");return 0;}
 return 0;
}
static u32 disk_io_n,disk_io_cmd[8],disk_io_fn[8],hle_n,hle_addr[8];
static u32 trace_pc[24],trace_n,trace_arm;
static void trace_note(u32 pc){
 u32 i,w;if(!trace_arm||trace_n>=24u)return;
 if(!((pc>=0x30014000u&&pc<0x30017000u)||(pc>=0x301da000u&&pc<0x301e0000u)||(pc>=0x300fd000u&&pc<0x30101000u)))return;
 w=mr32(pc);if((w&0x0FFF0000u)!=0x092D0000u)return;
 for(i=0;i<trace_n;i++)if(trace_pc[i]==pc)return;
 trace_pc[trace_n++]=pc;
}
static HostVol* vol_by_disk(u32 disk){
 int saved=g_error;u32 saved_pc=g_error_pc;u32 letter=mr8(disk+0x2cu);g_error=saved;g_error_pc=saved_pc;
 if(letter==0x43u&&c_vol.root[0])return &c_vol;
 if(letter==0x44u&&sd_vol.root[0])return &sd_vol;
 return 0;
}
static int vol_hle_rw(u32 a,int wr){
 u32 disk,req,buf,add,len,sector,addr;HostVol*v;u8*p;
 if(a!=0x301dc220u&&a!=0x301dc284u)return 0;
 if((a==0x301dc284u)!=wr)return 0;
 disk=cpu.r[0]-4u;v=vol_by_disk(disk);if(!v)return 0;
 req=cpu.r[1];buf=mr32(req);add=mr32(req+4u);len=mr32(req+0xcu);sector=mr32(disk+0x10u);addr=(sector<<9)+add;
 {int saved=g_error;u32 saved_pc=g_error_pc;p=len?(u8*)memptr(buf,len):0;if(!p){g_error=saved;g_error_pc=saved_pc;hret(0u);return 1;}}
 /* The disk stream ABI returns bytes transferred. A boolean success moved
    the native VFAT reader forward by one byte after an LFN entry, corrupting
    the following short entry and every multi-entry long filename. */
 hret((wr?vol_write(v,addr,p,len):vol_read(v,addr,p,len))?len:0u);
 return 1;
}
static int system_sd_hle(u32 a){
 if(a==0x3015d070u){if(sd_vol.root[0]){sd_note_card();hret(1u);}else hret(0u);return 1;}
 if(vol_hle_rw(a,0)||vol_hle_rw(a,1))return 1;
 if((a==0x3015d7acu||a==0x3015d6ecu)&&sd_vol.root[0]){if(hle_n<8u)hle_addr[hle_n++]=cpu.r[0];u8*p=(u8*)memptr(cpu.r[1],cpu.r[2]?cpu.r[2]:1u);hret(p&&vol_read(&sd_vol,cpu.r[0],p,cpu.r[2])?1u:0u);return 1;}
 if(a==0x301da138u && disk_io_n<8u){u32 base=mr32(cpu.r[0]+0x30u);disk_io_cmd[disk_io_n]=cpu.r[1];disk_io_fn[disk_io_n]=base?mr32(base+0x1cu):0;disk_io_n++;}
 return 0;
}
static void vol_set_root(HostVol*v,const char*path,const char*label){
 u32 i=0;v->root[0]=0;v->ready=0;vol_ov_n[v->ov_id]=0;memzero(v->label,12);memcopy(v->label,label,11);
 if(!path)return;while(path[i]&&i+1u<sizeof(v->root)){v->root[i]=path[i];i++;}v->root[i]=0;
}
void emu_system_set_sdcard(const char*path){sd_vol.ov_id=1;vol_set_root(&sd_vol,path,"SDCARD     ");}
void emu_system_set_cdrive(const char*path){c_vol.ov_id=0;vol_set_root(&c_vol,path,"CDRIVE     ");}
int emu_debug_sd_read(u32 addr,u8*dst,u32 len){return vol_read(&sd_vol,addr,dst,len);}
int emu_debug_c_read(u32 addr,u8*dst,u32 len){return vol_read(&c_vol,addr,dst,len);}
int emu_debug_c_write(u32 addr,const u8*src,u32 len){return vol_write(&c_vol,addr,src,len);}
u32 emu_debug_sd_reads(void){return sd_vol.reads;}
u32 emu_debug_sd_hist(u32 i,u32*addr){if(i>=hle_n)return 0;if(addr)*addr=hle_addr[i];return hle_n;}
u32 emu_debug_hit(u32 i,u32*pc,u32*r0,u32*w,u32*n){(void)i;(void)pc;(void)r0;(void)w;(void)n;return 0;}
void emu_debug_hit_boot(u8*dst){if(dst)memzero(dst,16);}
u32 emu_debug_disk_snap(u32 i,u32*ptr,u32*fn,u8*dst){(void)i;(void)ptr;(void)fn;(void)dst;return 0;}
u32 emu_debug_disk_io(u32 i,u32*cmd,u32*fn){if(i>=disk_io_n)return 0;if(cmd)*cmd=disk_io_cmd[i];if(fn)*fn=disk_io_fn[i];return disk_io_n;}
u32 emu_debug_trace(u32 i,u32*pc){if(i>=trace_n)return 0;if(pc)*pc=trace_pc[i];return trace_n;}
void emu_debug_trace_arm(void){trace_n=0;trace_arm=1;}
u32 emu_debug_nand_reads(void){return nand_read_commands;}
u32 emu_debug_vf_recent(u32 i,u32*lo,u32*hi,u32*len,u32*wr){u32 n=debug_vf_pos<16u?debug_vf_pos:16u;if(i>=n)return 0;{DebugVfOp*e=&debug_vf_ops[(debug_vf_pos-n+i)&255u];if(lo)*lo=(u32)e->off;if(hi)*hi=(u32)(e->off>>32);if(len)*len=e->len;if(wr)*wr=e->wr;}return n;}

static int system_device_hle(u32 a){
 if(a==0x30194d4cu && cpu.r[14]==0x30093884u){
  u32 sp=cpu.r[13],dst=cpu.r[0];
  if(dst>=sp+36u && dst<=sp+60u && ((dst-(sp+36u))&7u)==0u){
   u32 index=(dst-(sp+36u))/8u;
   virtual_touch_calibration_x=mr32(sp+4u+index*8u);
   virtual_touch_calibration_y=mr32(sp+8u+index*8u);
   virtual_touch_calibration_active=1u;
  }
 }
 if(a==0x301949f0u && virtual_touch_sample_active){
  virtual_touch_sample_active=0u;
  if(virtual_touch_down && !virtual_touch_down_accepted){virtual_touch_down_accepted=1u;touch_edge_acked=touch_edge_seq;host_input_yield=1u;}
 }
 /* Low-level virtual touch hardware.  The original Task TouchScreen polls
    GPIO channel 12 for pen state, then samples ADC channels 0x90/0xd0 five
    times and performs the firmware's own calibration/filter/message logic.
    Keep the emulation boundary here rather than synthesizing GUI messages. */
 if(a==0x30195190u && cpu.r[0]==27u && sd_vol.root[0]){hret(0u);return 1;}
 /* Power key: active-high on GPIO 7 (see firmware helper at 0x301954c8). */
 if(a==0x30195190u && cpu.r[0]==7u){
  if(virtual_power_down&&!virtual_power_sampled)emu_system_log("input: power-key sampled");
  virtual_power_sampled=1u;hret(virtual_power_down?1u:0u);return 1;
 }
 if(a==0x30195190u && cpu.r[0]==12u){
  trace_touch_wake++;
  /* Ack only when this poll actually observes pen-up.  Acking on the first
     poll cleared the edge while the task still had its debounce sleep left,
     and that sleep then ran at 35 ms/tick (U 1647 ms). */
  if(!virtual_touch_down){if(!virtual_touch_up_seen){trace_pen_clear++;if(host_input_yield<2u)host_input_yield=2u;}virtual_touch_up_seen=1u;touch_edge_acked=touch_edge_seq;}
  hret(virtual_touch_down?0u:1u);return 1;
 }
 /* Physical keys stay below the GUI layer: Task Keypad still performs the
    original scan/debounce/message translation.  We only emulate the contact
    closure between the driven and sampled GPIO pins. */
 if(a==0x30195190u && debug_last_task==0x3047f100u && (cpu.r[0]==14u||cpu.r[0]==13u||cpu.r[0]==3u||cpu.r[0]==11u)){
  u32 dp=0,rp=0,level=0;key_edge_acked=key_edge_seq;
  if(keypad_pair_for_vk(virtual_key_vk,&dp,&rp)&&cpu.r[0]==rp&&keypad_current_drive_pin()==dp){key_contact_sampled=1u;level=virtual_key_down?1u:0u;}
  hret(level);return 1;
 }
 if(a==0x301e7894u && (cpu.r[0]==0x90u||cpu.r[0]==0xd0u)){
  u32 sample_x=virtual_touch_x,sample_y=virtual_touch_y;
  if(cpu.r[14]==0x3019496cu){
   virtual_touch_sample_x=sample_x;virtual_touch_sample_y=sample_y;
   if(virtual_touch_calibration_active){
    i32 offset_x=(i32)sample_x-(i32)virtual_touch_calibration_x;
    i32 offset_y=(i32)sample_y-(i32)virtual_touch_calibration_y;
    if(offset_x>=-12 && offset_x<=12 && offset_y>=-12 && offset_y<=12){
     virtual_touch_sample_x=virtual_touch_calibration_x;virtual_touch_sample_y=virtual_touch_calibration_y;
    }
   }
   virtual_touch_sample_active=1u;
  }
  if(virtual_touch_sample_active){sample_x=virtual_touch_sample_x;sample_y=virtual_touch_sample_y;}
  if(virtual_touch_down){
   if(cpu.r[0]==0x90u)virtual_touch_adc_x_reads++;else virtual_touch_adc_y_reads++;
  }
  hret(cpu.r[0]==0x90u?sample_x:sample_y);return 1;
 }
 if(system_touch_autocal_enabled && a==0x30194d4cu && cpu.r[14]==0x30093884u){
  u32 sp=cpu.r[13],dst=cpu.r[0];
  if(dst>=sp+36u && dst<=sp+60u && ((dst-(sp+36u))&7u)==0u){
   u32 i=(dst-(sp+36u))/8u;
   u32 x=mr32(sp+4u+i*8u),y=mr32(sp+8u+i*8u);
   mw32(dst,x);mw32(dst+4u,y);
   if(i<4u){debug_touch_autocal_xy[i*2u]=x;debug_touch_autocal_xy[i*2u+1u]=y;}
   debug_touch_autocal_samples++;hret(0);return 1;
  }
 }
 return 0;
}

/* Dispatch only pages containing intercepted entry points. Most ARM
   instructions are ordinary code, so avoid five unrelated HLE calls each
   time. Keep the narrow handlers authoritative for individual addresses. */
static int system_hle_dispatch(u32 pc){
 if(pc==0x30093884u)virtual_touch_calibration_active=0u;
 switch(pc>>12){
 case 0x30194u:case 0x30195u:case 0x301e7u:
  return system_irq_enabled?system_device_hle(pc):0;
 case 0x301e0u:case 0x301e1u:case 0x301e5u:case 0x301e6u:case 0x30206u:
  return system_movie_hle(pc);
#ifdef EMU_TESTING
 case 0x30236u:case 0x30237u:return system_movie_hle(pc);
#endif
#ifdef MOVIE_TRACE
 case 0x301e2u:case 0x301e3u:return system_movie_hle(pc);
#endif
 case 0x3015du:case 0x301dau:case 0x301dcu:return system_sd_hle(pc);
 case 0x300ffu:return system_irq_enabled?system_ftl_hle(pc):0;
 case 0x300feu:case 0x301d9u:return system_irq_enabled?system_storage_hle(pc):0;
 default:return 0;
 }
}

#ifdef EMU_PC_PROFILE
static u32 pc_profile[FW_MEM_SIZE/4u],pc_profile_on,pc_profile_tick;
void emu_debug_pc_profile(u32*dst,u32 enable){
 if(dst)memcopy(dst,pc_profile,sizeof(pc_profile));
 memzero(pc_profile,sizeof(pc_profile));pc_profile_tick=0;pc_profile_on=enable;
}
#endif
#ifdef EMU_TOWER_PROFILE
static u32 tower_hwnd,tower_key,tower_key_count;
void emu_debug_tower_state(u32*out){out[0]=tower_hwnd;out[1]=tower_hwnd?mr32(tower_hwnd+0x58u):0;out[2]=tower_key;out[3]=tower_key_count;}
#endif
static int cpu_step(void){u32 pc=cpu.r[15];g_current_cpu_pc=pc;g_current_cpu_sp=cpu.r[13];g_current_cpu_lr=cpu.r[14];
#ifdef EMU_TOWER_PROFILE
 if(pc==0x300d0094u){tower_hwnd=cpu.r[0];if(cpu.r[1]==0x100u){tower_key=cpu.r[2];tower_key_count++;}}
#endif
#if defined(EMU_KEY_PROFILE) && !defined(EMU_TESTING)
 /* Optional release-speed observer of messages produced by the real keypad. */
 if(system_irq_enabled&&pc==0x301b61b8u&&debug_desktop_msg_count<64u){u32 msg=mr32(cpu.r[13]+0x1cu);if(msg==0x100u||msg==0x101u){DebugDesktopMsg*dm=&debug_desktop_msgs[debug_desktop_msg_count++];dm->hwnd=mr32(cpu.r[13]+0x18u);dm->msg=msg;dm->wparam=mr32(cpu.r[13]+0x20u);dm->lparam=mr32(cpu.r[13]+0x24u);dm->cb=0;}}
#endif
#ifdef EMU_PC_PROFILE
 if(pc_profile_on&&++pc_profile_tick==127u){pc_profile_tick=0;if(pc>=VBASE&&pc<VBASE+FW_MEM_SIZE)pc_profile[(pc-VBASE)>>2]++;}
#endif
 /* System_Start lives at runtime 0x3008fc90 (the analysis ELF includes the
    0x48-byte update wrapper).  Probe the instruction immediately after each
    major BL so a long boot tells us exactly which init stage returned. */
 /* System_Start milestone probes (also in release so the log window shows boot progress). */
 if(system_irq_enabled){
  static const u32 ms[41]={
   0x3008fc90u,0x3008fc9cu,0x3008fca0u,0x3008fca8u,0x3008fcacu,0x3008fcb0u,
   0x3008fcb4u,0x3008fcc0u,0x3008fcc4u,0x3008fcd0u,0x3008fce0u,0x3008fce4u,
   0x3008fcf0u,0x3008fd04u,0x3008fd10u,0x3008fd28u,0x3008fd30u,0x3008fd38u,
   0x3008fd40u,0x3008fd48u,0x3008fd50u,0x3008fd54u,0x3008fd58u,0x3008fd5cu,
   0x3008fd64u,0x3008fd68u,0x3008fd74u,0x3008fd8cu,0x3008fd94u,0x3008fd98u,
   0x3008fda4u,0x3008fdacu,0x3008fdb4u,0x3008fdd0u,0x3008fdf0u,0x3008fe00u,
   0x3008fe0cu,0x3008fe1cu,0x3008fe2cu,0x3008fe30u,0x3008fe38u};
  for(u32 mi=0;mi<41u;mi++)if(pc==ms[mi]){
   debug_startup_milestone_counts[mi]++;
   if(mi>debug_startup_last_milestone){
    char b[80];debug_startup_last_milestone=mi;
    if(pc==0x3008fcf0u){debug_startup_syscheck=cpu.r[0];sprintf(b,"boot: milestone %u System_StartCheck r0=%08X",mi,cpu.r[0]);}
    else sprintf(b,"boot: milestone %u pc=%08X",mi,pc);
    emu_system_log(b);
   }else if(pc==0x3008fcf0u)debug_startup_syscheck=cpu.r[0];
  }
 }
#ifdef EMU_TESTING
#ifdef MOVIE_TRACE
 if(pc==0x301e039cu){char b[240];u32 p=cpu.r[0];sprintf(b,"probe: input=%08X hwnd=%08X kind=%u data=%08X callback=%08X volume=%u",p,mr32(p),mr32(p+4),mr32(p+0x18),mr32(p+0x14),mr32(p+0x24));emu_system_log(b);}
 if(pc==0x302c7c14u&&cpu.r[14]>=0x301df000u&&cpu.r[14]<0x301e7000u){char b[240],fmt[120];u32 i;for(i=0;i<119u;i++){fmt[i]=(char)mr8(cpu.r[0]+i);if(!fmt[i])break;}fmt[i]=0;sprintf(b,"probe: fmt %.119s r1=%08X r2=%08X lr=%08X",fmt,cpu.r[1],cpu.r[2],cpu.r[14]);emu_system_log(b);}
#endif
 if(system_irq_enabled){
  if(pc==0x302ac4a4u)debug_context_switches++;
  if(pc==0x301903ecu)debug_scheduler_selects++;
  if(pc==0x30190428u){
   u32 t=cpu.r[0];
   if(t>=0x3047ef50u){u32 d=t-0x3047ef50u;if(d%0x90u==0u&&d/0x90u<32u)debug_task_select_count[d/0x90u]++;}
   if(debug_last_task&&t!=debug_last_task)debug_task_changes++;
   debug_last_task=t;
  }
 }
 debug_trace[debug_trace_pos++&255u]=pc;if(cpu.T){g_error=3;g_error_pc=pc;return 0;}
 if(system_irq_enabled&&pc==0x30100500u){debug_update_status=cpu.r[0];debug_update_status_hits++;}
 if(system_irq_enabled&&pc==0x301b610cu){debug_desktop_obj=cpu.r[0];}
 if(system_irq_enabled&&pc==0x301b6180u){debug_desktop_hwnd=cpu.r[0];if(debug_desktop_obj)debug_desktop_cb=mr32(debug_desktop_obj+0x238u);}
 if(system_irq_enabled&&pc==0x301b61b8u&&debug_desktop_msg_count<64u){DebugDesktopMsg*dm=&debug_desktop_msgs[debug_desktop_msg_count++];dm->hwnd=mr32(cpu.r[13]+0x18u);dm->msg=mr32(cpu.r[13]+0x1cu);dm->wparam=mr32(cpu.r[13]+0x20u);dm->lparam=mr32(cpu.r[13]+0x24u);dm->cb=debug_desktop_obj?mr32(debug_desktop_obj+0x238u):0;}
 /* CreateWindow wrapper (static 0x30199bac, runtime -0x48).  Record every
    real firmware window/control creation so we can reconstruct the UI tree. */
 if(system_irq_enabled&&pc==0x30199b64u&&debug_win_create_count<256u){DebugWinCreate*e=&debug_win_create[debug_win_create_count++];e->sp=cpu.r[13];e->lr=cpu.r[14];e->a0=cpu.r[0];e->a1=cpu.r[1];e->a2=cpu.r[2];e->a3=cpu.r[3];e->s0=mr32(cpu.r[13]);e->s1=mr32(cpu.r[13]+4u);e->s2=mr32(cpu.r[13]+8u);e->s3=mr32(cpu.r[13]+12u);e->s4=mr32(cpu.r[13]+16u);e->ret=0xffffffffu;}
 if(system_irq_enabled&&pc==0x30199bb4u){u32 esp=cpu.r[13]+40u;for(u32 wi=debug_win_create_count;wi>0;wi--){DebugWinCreate*e=&debug_win_create[wi-1u];if(e->sp==esp&&e->ret==0xffffffffu){e->ret=cpu.r[0];break;}}}
 /* Bitmap/resource decoder wrapper (static 0x30012b0c). */
 if(system_irq_enabled&&pc==0x30012ac4u&&debug_bitmap_decode_count<256u){DebugBitmapDecode*e=&debug_bitmap_decode[debug_bitmap_decode_count++];e->sp=cpu.r[13];e->lr=cpu.r[14];e->rid=desc_id(cpu.r[0]);e->index=cpu.r[1];e->ret=0xffffffffu;}
 if(system_irq_enabled&&pc==0x30012af8u){u32 esp=cpu.r[13]+16u;for(u32 bi=debug_bitmap_decode_count;bi>0;bi--){DebugBitmapDecode*e=&debug_bitmap_decode[bi-1u];if(e->sp==esp&&e->ret==0xffffffffu){e->ret=cpu.r[0];break;}}}
 /* Immediately after the decoder has read BITMAPFILEHEADER (14) and its
    44-byte info header.  Preserve the raw interpreted fields before any
    allocation/conversion so a VFS/seek error is obvious. */
 if(system_irq_enabled&&pc==0x3001270cu&&debug_bitmap_header_count<256u){u32 dh=mr32(cpu.r[13]+0x10u),fh=mr32(cpu.r[13]+0x14u);DebugBitmapHeader*e=&debug_bitmap_headers[debug_bitmap_header_count++];e->off=cpu.r[11];e->sig=mr16(fh);e->pixoff=mr32(fh+10u);e->dibsz=mr32(dh);e->w=mr32(dh+4u);e->h=mr32(dh+8u);e->planes_bpp=mr32(dh+12u);e->comp=mr32(dh+16u);e->imgsz=mr32(dh+20u);}
 if(system_irq_enabled&&pc==0x301D9D90u){u32 dr=desc_id(cpu.r[0]);debug_file_open_attempts++;debug_last_file_rid=dr;if(dr<256u)debug_file_rid_counts[dr]++;debug_res_log(pc,dr,cpu.r[1],cpu.r[14],cpu.r[2],0);for(u32 pi=0;pi<32u;pi++)if(!debug_pending_open[pi].sp){debug_pending_open[pi].sp=cpu.r[13];debug_pending_open[pi].rid=dr;debug_pending_open[pi].mode=cpu.r[1];debug_pending_open[pi].lr=cpu.r[14];break;}}
 if(system_irq_enabled&&pc==0x301D9DF4u){u32 osp=cpu.r[13]+16u;for(u32 pi=0;pi<32u;pi++)if(debug_pending_open[pi].sp==osp){u32 rr=debug_pending_open[pi].rid;if(rr<1024u){if(cpu.r[0]){debug_rid_open_success[rr]++;if(!debug_rid_first_handle[rr]){debug_rid_first_handle[rr]=cpu.r[0];debug_rid_first_lr[rr]=debug_pending_open[pi].lr;}}else debug_rid_open_fail[rr]++;}debug_pending_open[pi].sp=0;break;}}
 if(system_irq_enabled&&(pc==0x301D9DF8u||pc==0x301D9F04u||pc==0x301D9FF8u||pc==0x301D9E5Cu)){
  Obj*ro=getobj(cpu.r[0]);u32 rr=ro?ro->rid:0xffffffffu;debug_res_log(pc,rr,(pc==0x301D9DF8u)?cpu.r[2]:0,cpu.r[14],(pc==0x301D9F04u||pc==0x301D9FF8u)?cpu.r[2]:cpu.r[1],cpu.r[0]);
 }
#else
 /* Release build keeps only the task identity needed by the physical-key GPIO
    matrix.  The large trace/milestone/window/resource probes are test-only;
    running them on every interpreted ARM instruction made UI input needlessly
    sluggish. */
 if(system_irq_enabled&&pc==0x30190428u)debug_last_task=cpu.r[0];
 if(cpu.T){g_error=3;g_error_pc=pc;return 0;}
#endif
#ifdef AUDIO_TRACE
 if(pc==0x30207dccu&&audio_sub_n<8u){audio_sub_r0[audio_sub_n]=cpu.r[0];audio_sub_r1[audio_sub_n]=cpu.r[1];audio_sub_from[audio_sub_n]=mr32(cpu.r[13]+4u);audio_sub_n++;}
 if(pc==0x300116c0u){if(!audio_cb_hits){audio_cb_r0=cpu.r[0];audio_cb_r1=cpu.r[1];audio_cb_r2=cpu.r[2];audio_cb_r3=cpu.r[3];}audio_cb_hits++;}
 if(pc==0x30011908u)audio_play_enter++;
 if(pc==0x30011a30u){if(!audio_play_wait)audio_play_wait_r0=cpu.r[0];audio_play_wait++;}
 if(pc==0x30011ab4u)audio_play_post++;
 if(pc==0x30236144u)audio_mix_enter++;
 if(pc==0x30236178u){if(!cpu.r[0])audio_mix_empty++;}
 if(pc==0x302361b4u){audio_mix_at++;audio_mix_r9=cpu.r[9];}
 if(pc==0x302361e0u){if(!audio_mix_r5){audio_mix_r5=cpu.r[5];audio_mix_dest=mr32(cpu.r[13]+4u);} }
 if(pc==0x302361f4u)audio_mix_give++;
 if(pc==0x302362a0u)audio_mix_a0++;
 if(pc==0x302361d4u)audio_mix_bad++;
 if(pc==0x3023625cu){if(!audio_mix_copy){audio_mix_dst=cpu.r[0];audio_mix_src=cpu.r[1];audio_mix_len=cpu.r[2];}audio_mix_copy++;}
 if(pc==0x30264204u){if(!audio_qpush){audio_qpush_src=cpu.r[1];audio_qpush_len=cpu.r[2];}audio_qpush++;}
 if(pc==0x30237afcu)audio_drv_a++;
 if(pc==0x3023807cu)audio_drv_b++;
 if(pc==0x302380c0u){audio_link_ret_n++;audio_link_ret=cpu.r[0];}
 if(pc==0x302380ccu)audio_link_fill++;
 if(pc==0x30238100u){audio_link_ind++;audio_link_fn=cpu.r[12];}
 if(pc==0x30191058u){audio_isr_cb++;audio_isr_fn=cpu.r[2];}
 if(pc==0x301910dcu)audio_isr75++;
 if(pc==0x301dd0d4u)audio_cb_body++;
 if(pc==0x301dd118u)audio_cb_set++;
 if(pc==0x30190af4u){if(!audio_sig_f8)audio_sig_f8=cpu.r[0];}
 if(pc==0x30190afcu)audio_sig_drop8++;
 if(pc==0x30190b08u)audio_sig_st=cpu.r[0];
 if(pc==0x30190b10u)audio_sig_drop5++;
 if(pc==0x30190b18u)audio_sig++;
 if(pc==0x30190b2cu){if(cpu.r[4])audio_waiter_n++;if(!audio_waiter)audio_waiter=cpu.r[4];}
 if(pc==0x301b501cu)audio_thread++;
 if(pc==0x301b4ef0u)audio_thread_msg++;
 if(pc==0x30191828u){audio_slot8_n++;if(cpu.r[2])audio_slot8_nz=cpu.r[2];}
 if(pc==0x30191134u){u32 c=cpu.r[0]&15u;audio_cmd_count[c]++;audio_cmd_lr[c]=cpu.r[14];}
 if(pc==0x301ddb3cu){audio_cmd7_fn=cpu.r[2];audio_cmd7_obj=cpu.r[0];}
 if(pc==0x30221484u)audio_dma_start_lr=cpu.r[14];
#endif
 if(system_hle_dispatch(pc)){cpu.steps++;return 1;}if(!system_irq_enabled&&hle(pc)){cpu.steps++;return 1;}u32 w=mr32(pc);if(g_error)return 0;cpu.r[15]=pc+4;cpu.steps++;u32 cond=w>>28;if(!cond_ok(cond))return 1;
 if((w&0x0ffffff0u)==0x012fff10u||(w&0x0ffffff0u)==0x012fff30u){u32 rm=w&15u,t=regv(rm);if(w&0x20u)cpu.r[14]=pc+4;cpu.T=t&1u;cpu.r[15]=t&~1u;return 1;}
 if(((w>>25)&7u)==5u){i32 imm=(i32)(w&0xffffffu);if(imm&0x800000)imm-=1<<24;if((w>>24)&1u)cpu.r[14]=pc+4;cpu.r[15]=pc+8+(u32)(imm<<2);return 1;}
 if(((w>>24)&15u)==15u){
  u32 imm=w&0x00ffffffu;
  if(imm==0x00123456u){
   /* ARM semihosting used by the firmware debug/printf runtime. */
   if(cpu.r[0]==3u)semihost_putc(mr8(cpu.r[1]));
   else if(cpu.r[0]==4u){u32 q=cpu.r[1];for(;;){u8 c=mr8(q++);if(!c)break;semihost_putc(c);}}
   cpu.r[0]=0;return !g_error;
  }
  {u32 oldpsr=cpu_cpsr();cpu_switch_mode(0x13u);cpu_set_spsr(oldpsr);cpu.I=1;cpu.T=0;cpu.r[14]=pc+4u;cpu.r[15]=VBASE+8u;return 1;}
 }
 if((w&0x0fbf0fffu)==0x010f0000u){u32 psr=((w>>22)&1u)?cpu_spsr():cpu_cpsr();setreg((w>>12)&15u,psr);return 1;}
 if((w&0x0fb0fff0u)==0x0120f000u||(w&0x0fb0f000u)==0x0320f000u){u32 v;if((w>>25)&1u)v=ror32(w&255u,((w>>8)&15u)*2u);else v=regv(w&15u);u32 fields=(w>>16)&15u,mask=0;if(fields&1u)mask|=0x000000ffu;if(fields&2u)mask|=0x0000ff00u;if(fields&4u)mask|=0x00ff0000u;if(fields&8u)mask|=0xff000000u;if((w>>22)&1u){u32 oldpsr=cpu_spsr();cpu_set_spsr((oldpsr&~mask)|(v&mask));}else{u32 oldpsr=cpu_cpsr();cpu_set_cpsr((oldpsr&~mask)|(v&mask));}return 1;}
 if((w&0x0f000010u)==0x0e000010u&&((w>>8)&15u)==15u){u32 L=(w>>20)&1u,op1=(w>>21)&7u,crn=(w>>16)&15u,rd=(w>>12)&15u,op2=(w>>5)&7u,crm=w&15u,key=(op1<<9)|(crn<<5)|(op2<<2)|(crm&3u);key&=4095u;if(L){u32 v=cp15_regs[key];if(crn==0&&crm==0)v=0x41069265u;setreg(rd,v);}else cp15_regs[key]=regv(rd);return 1;}
 if((w&0x0fc000f0u)==0x00000090u){u32 A=(w>>21)&1u,S=(w>>20)&1u,rd=(w>>16)&15u,rn=(w>>12)&15u,rs=(w>>8)&15u,rm=w&15u;u32 v=regv(rm)*regv(rs);if(A)v+=regv(rn);setreg(rd,v);if(S)set_nz(v);return 1;}
 if((w&0x0f8000f0u)==0x00800090u){u32 sg=(w>>22)&1u,acc=(w>>21)&1u,S=(w>>20)&1u,hi=(w>>16)&15u,lo=(w>>12)&15u,rs=(w>>8)&15u,rm=w&15u;u64 v;if(sg)v=(u64)((i64)s32(regv(rm))*(i64)s32(regv(rs)));else v=(u64)regv(rm)*(u64)regv(rs);if(acc)v+=((u64)regv(hi)<<32)|regv(lo);setreg(lo,(u32)v);setreg(hi,(u32)(v>>32));if(S){cpu.N=(u8)(v>>63);cpu.Z=(u8)(v==0);}return 1;}
 if((w&0x0e000090u)==0x00000090u&&((w>>5)&3u)!=0){u32 P=(w>>24)&1u,U=(w>>23)&1u,I=(w>>22)&1u,W=(w>>21)&1u,L=(w>>20)&1u,rn=(w>>16)&15u,rd=(w>>12)&15u,typ=(w>>5)&3u,base=regv(rn),off=I?((((w>>8)&15u)<<4)|(w&15u)):regv(w&15u),eff=P?(U?base+off:base-off):base;if(L){u32 v;if(typ==1)v=mr16(eff);else if(typ==2){v=mr8(eff);if(v&0x80)v|=0xffffff00u;}else if(typ==3){v=mr16(eff);if(v&0x8000)v|=0xffff0000u;}else{g_error=5;g_error_pc=pc;return 0;}setreg(rd,v);}else{if(typ!=1){g_error=5;g_error_pc=pc;return 0;}mw16(eff,regv(rd));}if(W||!P)setreg(rn,U?base+off:base-off);return 1;}
 if(((w>>26)&3u)==1u){u32 I=(w>>25)&1u,P=(w>>24)&1u,U=(w>>23)&1u,B=(w>>22)&1u,W=(w>>21)&1u,L=(w>>20)&1u,rn=(w>>16)&15u,rd=(w>>12)&15u,base=regv(rn),off=I?addr_shift(w):(w&0xfffu),eff=P?(U?base+off:base-off):base;if(L){u32 v=B?mr8(eff):mr32(eff&~3u);if(!B&&(eff&3u))v=ror32(v,(eff&3u)*8u);setreg(rd,v);}else{u32 v=regv(rd);if(B)mw8(eff,v);else mw32(eff&~3u,v);}if(W||!P)setreg(rn,U?base+off:base-off);return !g_error;}
 if(((w>>25)&7u)==4u){u32 P=(w>>24)&1u,U=(w>>23)&1u,S=(w>>22)&1u,W=(w>>21)&1u,L=(w>>20)&1u,rn=(w>>16)&15u,regs=w&0xffffu;int n=0;for(int i=0;i<16;i++)if(regs&(1u<<i))n++;u32 base=regv(rn),aa=U?base+(P?4:0):base-4u*n+(P?0:4);for(int i=0;i<16;i++)if(regs&(1u<<i)){if(L)setreg(i,mr32(aa));else mw32(aa,regv(i));aa+=4;}if(W)setreg(rn,U?base+4u*n:base-4u*n);if(L&&S&&(regs&0x8000u))cpu_set_cpsr(cpu_spsr());return !g_error;}
 if(((w>>26)&3u)==0u){u32 op=(w>>21)&15u,S=(w>>20)&1u,rn=(w>>16)&15u,rd=(w>>12)&15u,a0=regv(rn),shc,bb=op2(w,&shc),r=0;int write=1,logic=0;switch(op){case 0:r=a0&bb;logic=1;break;case 1:r=a0^bb;logic=1;break;case 2:r=a0-bb;break;case 3:r=bb-a0;break;case 4:r=a0+bb;break;case 5:r=a0+bb+cpu.C;break;case 6:r=a0-bb-(1-cpu.C);break;case 7:r=bb-a0-(1-cpu.C);break;case 8:r=a0&bb;write=0;logic=1;break;case 9:r=a0^bb;write=0;logic=1;break;case 10:r=a0-bb;write=0;break;case 11:r=a0+bb;write=0;break;case 12:r=a0|bb;logic=1;break;case 13:r=bb;logic=1;break;case 14:r=a0&~bb;logic=1;break;case 15:r=~bb;logic=1;break;}if(S||!write){if(op==2||op==6||op==10)sub_flags(a0,bb,op==6?(1-cpu.C):0);else if(op==3||op==7)sub_flags(bb,a0,op==7?(1-cpu.C):0);else if(op==4||op==5||op==11)add_flags(a0,bb,op==5?cpu.C:0);else{set_nz(r);cpu.C=(u8)shc;}}if(write){setreg(rd,r);if(S&&rd==15u)cpu_set_cpsr(cpu_spsr());}return 1;}
 g_error=7;g_error_pc=pc;return 0;}

static int hw_audio_irq_pending(void){
 return (mr32(0x20000014u)&AUDIO_IRQ_BIT)!=0u && (mr32(0x20000018u)&AUDIO_IRQ_BIT)==0u && mr32(0x30488594u+5u*4u)!=0x30191988u;
}
static int hw_timer3_irq_pending(void){
 return (mr32(0x20000014u)&0x01000000u)!=0u && (mr32(0x20000018u)&0x00000200u)==0u && mr32(0x30488594u+24u*4u)!=0x30191988u;
}
static void cpu_raise_irq(void){
 int ai=hw_audio_irq_pending(),ti=hw_timer3_irq_pending();
 u32 old=cpu_cpsr(),next=cpu.r[15];cpu_switch_mode(0x12u);cpu_set_spsr(old);cpu.I=1;cpu.T=0;cpu.r[14]=next+4u;cpu.r[15]=VBASE+0x18u;
 if(ai)audio_probe.irq_entries++;
 if(ti)timer3.irq_entries++;
}
static void hw_timer3_expire(void){
 u8*p=(u8*)memptr(0x20000014u,4);
 if(p){u32 v=(u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);v|=0x01000000u;p[0]=(u8)v;p[1]=(u8)(v>>8);p[2]=(u8)(v>>16);p[3]=(u8)(v>>24);}
 timer3.enabled=0;timer3.counter=0u;timer3.fires++;
}
static void hw_timer3_step(void){
 if(!timer3.enabled||timer3.counter==0u)return;
 timer3.counter--;
 if(timer3.counter==0u)hw_timer3_expire();
}
static int hw_irq_pending(void){
 /* IRQ5 is the AK322x audio controller.  IRQ24 is Timer3 and is routed
    through the controller's high-source summary mask bit9. */
 return hw_audio_irq_pending()||hw_timer3_irq_pending();
}
/* Original firmware bitmap decoder hot loop (runtime 0x300128A4) converts
   one 8-bit indexed scanline to RGB565.  Applications repaint controls through
   this generic system primitive, so interpreting the 15-instruction inner loop
   pixel-by-pixel dominates host latency.  Batch only complete loop iterations,
   preserve the exact guest instruction count, CMP flags and Timer3 progress,
   and never skip across a Timer3 expiry. */
static int render_plain_ram(u32 a,u32 n);
static int render_disjoint(u32 a,u32 n,u32 b,u32 m);
static u32 render_fast_palette8(u32 budget_left){
 u32 cur,width,remain,n,step_budget,timer_budget;
 u8*src,*pal,*dst;
 if(!render_fastpaths_enabled||cpu.r[15]!=0x300128a4u||budget_left<15u)return 0u;
 cur=cpu.r[0];width=cpu.r[4];if(cur>=width)return 0u;remain=width-cur;
 step_budget=budget_left/15u;n=remain<step_budget?remain:step_budget;
 if(timer3.enabled&&timer3.counter){
  if(timer3.counter<=15u)return 0u;
  timer_budget=(timer3.counter-1u)/15u;if(n>timer_budget)n=timer_budget;
 }
 if(!n||n>0x7fffffffu)return 0u;
 if(!render_plain_ram(cpu.r[9]+cur,n)||!render_plain_ram(cpu.r[7],1024u)||!render_plain_ram(cpu.r[12]+cur*2u,n*2u)||
    !render_disjoint(cpu.r[9]+cur,n,cpu.r[12]+cur*2u,n*2u)||!render_disjoint(cpu.r[7],1024u,cpu.r[12]+cur*2u,n*2u))return 0u;
 src=(u8*)memptr_try(cpu.r[9]+cur,n);pal=(u8*)memptr_try(cpu.r[7],1024u);dst=(u8*)memptr_try(cpu.r[12]+cur*2u,n*2u);
 if(!src||!pal||!dst)return 0u;
 for(u32 j=0;j<n;j++){
  u32 ix=src[j],r=pal[ix*4u],g=pal[ix*4u+1u],b=pal[ix*4u+2u];
  u32 c=(r>>3)|((g<<3)&cpu.r[14])|((b<<8)&0xf800u);
  dst[j*2u]=(u8)c;dst[j*2u+1u]=(u8)(c>>8);
  cpu.r[1]=c;
 }
 cpu.r[2]=cpu.r[12]+(cur+n-1u)*2u;cpu.r[3]=0xf800u;
 lcd_mark_primary_dirty_range(cpu.r[12]+cpu.r[0]*2u,n*2u);
 cur+=n;cpu.r[0]=cur;sub_flags(cur,width,0u);cpu.r[15]=(cur==width)?0x300128e0u:0x300128a4u;
 {u32 guest=15u*n;cpu.steps+=(u64)guest;if(timer3.enabled&&timer3.counter)timer3.counter-=guest;debug_render_fast_pixels+=(u64)n;debug_render_fast_events++;return guest;}
}

/* Hot aligned core of the original firmware memcpy at 0x302A21AC.
   Each iteration copies exactly 32 bytes in six ARM instructions.  Batch the
   same forward copies while preserving pointer/remainder registers, CMP/SUB
   flags, guest steps and Timer3 progress. */
static u32 render_fast_memcpy32(u32 budget_left){
 u32 start_r2,n,total,step_budget,timer_budget,pre;
 u8*src,*dst;
 if(!render_fastpaths_enabled||cpu.r[15]!=0x302a21acu||budget_left<6u||!cpu.C||((cpu.r[0]|cpu.r[1])&3u))return 0u;
 start_r2=cpu.r[2];total=start_r2/32u+1u;step_budget=budget_left/6u;n=total<step_budget?total:step_budget;
 if(timer3.enabled&&timer3.counter){if(timer3.counter<=6u)return 0u;timer_budget=(timer3.counter-1u)/6u;if(n>timer_budget)n=timer_budget;}
 if(!n||n>0x07ffffffu)return 0u;
 if(!render_plain_ram(cpu.r[1],32u*n)||!render_plain_ram(cpu.r[0],32u*n)||
    (cpu.r[0]!=cpu.r[1]&&!render_disjoint(cpu.r[0],32u*n,cpu.r[1],32u*n)))return 0u;
 src=(u8*)memptr_try(cpu.r[1],32u*n);dst=(u8*)memptr_try(cpu.r[0],32u*n);if(!src||!dst)return 0u;
 /* The final LDM remains observable when a slice ends inside memcpy. */
 cpu.r[3]=mr32(cpu.r[1]+32u*n-16u);cpu.r[4]=mr32(cpu.r[1]+32u*n-12u);
 cpu.r[12]=mr32(cpu.r[1]+32u*n-8u);cpu.r[14]=mr32(cpu.r[1]+32u*n-4u);
#ifdef AUDIO_TRACE
 if(cpu.r[0]+32u*n>0x30480584u&&cpu.r[0]<0x30488584u)audio_memcpy_ring++;
#endif
 lcd_mark_primary_dirty_range(cpu.r[0],32u*n);memcopy(dst,src,32u*n);cpu.r[0]+=32u*n;cpu.r[1]+=32u*n;pre=start_r2-32u*(n-1u);cpu.r[2]=pre-32u;sub_flags(pre,32u,0u);cpu.r[15]=(n==total)?0x302a21c4u:0x302a21acu;
 {u32 guest=6u*n;cpu.steps+=(u64)guest;if(timer3.enabled&&timer3.counter)timer3.counter-=guest;return guest;}
}
/* Original firmware's tiny ~1000-iteration hardware settle delay.  Hardware
   modeled here is already synchronous, so collapse only when no Timer3 expiry
   can occur inside the exact 3002-instruction routine. */
static u32 render_fast_delay1000(u32 budget_left){
 if(!render_fastpaths_enabled||cpu.r[15]!=0x30191a20u||budget_left<3002u)return 0u;
 if(timer3.enabled&&timer3.counter<=3002u)return 0u;
 cpu.r[0]=1000u;sub_flags(1000u,1000u,0u);cpu.r[15]=cpu.r[14];cpu.steps+=3002u;if(timer3.enabled&&timer3.counter)timer3.counter-=3002u;return 3002u;
}


/* Original SD/storage settle loop.  The verified body is:
     3015D0EC  ADD r0, r0, #1
     3015D0F0  CMP r0, r1          ; r1 = 0xFFFFF
     3015D0F4  BCC 3015D0EC
   It has no memory or MMIO side effects.  An earlier fast path was attached
   to 0x3015D134, which is a later BEQ in this same function, so the real
   ~1,048,575-iteration wait was still interpreted and only some clicks paid
   for it.  Batch the real loop.  Guest steps, CMP flags and Timer3
   boundaries stay exact. */
static u32 render_fast_delay1m(u32 budget_left){
 u32 cur,limit,remain,n,step_budget,timer_budget;
 if(!render_fastpaths_enabled||cpu.r[15]!=0x3015d0ecu||budget_left<3u)return 0u;
 cur=cpu.r[0];limit=cpu.r[1];if(cur>=limit)return 0u;remain=limit-cur;
 step_budget=budget_left/3u;n=remain<step_budget?remain:step_budget;
 if(timer3.enabled&&timer3.counter){if(timer3.counter<=3u)return 0u;timer_budget=(timer3.counter-1u)/3u;if(n>timer_budget)n=timer_budget;}
 if(!n)return 0u;cur+=n;cpu.r[0]=cur;sub_flags(cur,limit,0u);cpu.r[15]=(cur==limit)?0x3015d0f8u:0x3015d0ecu;debug_delay1m_fast_events++;
 {u32 guest=3u*n;cpu.steps+=(u64)guest;if(timer3.enabled&&timer3.counter)timer3.counter-=guest;return guest;}
}

/* These are the firmware's generic RGB565 surface primitives, not application
   drawing commands. Preserve even scratch stack writes and callee-clobbered
   registers; never batch across a budget/Timer3 boundary or touch MMIO. */
static int render_plain_ram(u32 a,u32 n){
 return (a>=VBASE&&(u64)a+n<=VBASE+FW_MEM_SIZE)||
        (a>=RAM3_BASE&&(u64)a+n<=RAM3_BASE+RAM3_SIZE);
}
static int render_disjoint(u32 a,u32 n,u32 b,u32 m){return (u64)a+n<=b||(u64)b+m<=a;}
static u32 render_fast_clip(u32 budget_left){
 u32 rect=cpu.r[0],x=cpu.r[1],y=cpu.r[2],sp=cpu.r[13],a=rect,b=0,cost=5,result=0;
 if(!render_fastpaths_enabled||cpu.r[15]!=0x3019b02cu||(sp&3u)||!render_plain_ram(sp-16,16))return 0;
 if(rect){
  if((rect&3u)||!render_plain_ram(rect,16)||!render_disjoint(rect,16,sp-16,16))return 0;
  a=x;b=mr32(rect);cost=10;
  if((i32)a>=(i32)b){b=mr32(rect+8);cost=14;
   if((i32)a<(i32)b){a=y;b=mr32(rect+4);cost=18;
    if((i32)a>=(i32)b){b=mr32(rect+12);cost=22;result=(i32)a<(i32)b;}
   }
  }
 }
 if(budget_left<cost||(timer3.enabled&&timer3.counter<=cost))return 0;
 mw32(sp-16,rect);mw32(sp-12,x);mw32(sp-8,y);mw32(sp-4,cpu.r[14]);
 sub_flags(a,b,0);cpu.r[0]=result;cpu.r[1]=rect;cpu.r[2]=x;cpu.r[3]=y;cpu.r[15]=cpu.r[14]&~3u;
 cpu.steps+=cost;if(timer3.enabled&&timer3.counter)timer3.counter-=cost;return cost;
}
static int render_surface_pixel(u32 surface,u32 x,u32 y,u32 sp,u32*pixel){
 if((surface&3u)||!render_plain_ram(surface,16)||!render_disjoint(surface,16,sp-16,24))return 0;
 *pixel=mr32(surface+12)+y*((3u+((mr32(surface+4)&0xffffu)<<1))&~3u)+(x<<1);
 return render_plain_ram(*pixel,2)&&render_disjoint(*pixel,2,sp-16,24);
}
/* One exact iteration of the native color-key surface blit. Sequential reads
   and writes retain overlapping-buffer behavior; short timer/budget tails
   fall back to the interpreter. 43 instructions if transparent, 79 if drawn. */
static u32 render_fast_colorkey(u32 budget_left){
 u32 sp=cpu.r[13],src,dst=0,rgb,v,x=cpu.r[4]+cpu.r[9],cost,column=cpu.r[4],dx=0;
 if(!render_fastpaths_enabled||cpu.r[15]!=0x3019efecu||(sp&3u)||!render_plain_ram(sp-16,24))return 0;
 if(!render_surface_pixel(cpu.r[10],x,cpu.r[7],sp,&src))return 0;
 v=mr16(src);rgb=((v&31u)<<3)|(((v&0x7e0u)>>3)<<8)|(((v&0xf800u)>>8)<<16);
 cost=rgb==cpu.r[8]?43u:79u;
 if(budget_left<cost||(timer3.enabled&&timer3.counter<=cost))return 0;
 if(rgb!=cpu.r[8]){
  dx=column+mr32(sp+4);if(!render_surface_pixel(mr32(sp),dx,cpu.r[6],sp,&dst))return 0;
  mw32(sp-12,column);mw32(sp-8,cpu.r[5]);mw32(sp-4,0x3019f01cu);mw32(sp-16,0x3019d518u);
  cpu.r[2]=(rgb>>8)&0xf800u;cpu.r[1]=((rgb>>5)&0x7e0u)|((rgb&255u)>>3)|cpu.r[2];
  mw16(dst,cpu.r[1]);cpu.r[0]=1;cpu.r[3]=dx;
 }else{
  mw32(sp-12,rgb);mw32(sp-8,column);mw32(sp-4,0x3019effcu);mw32(sp-16,0x3019d6f4u);
  cpu.r[0]=cpu.r[3]=rgb;cpu.r[1]=(v&0x7e0u)>>3;cpu.r[2]=cpu.r[7];
 }
 cpu.r[14]=0x3019d4f8u;cpu.r[4]++;sub_flags(cpu.r[4],cpu.r[11],0);
 cpu.r[15]=(i32)cpu.r[4]<(i32)cpu.r[11]?0x3019efecu:0x3019f028u;
 cpu.steps+=cost;if(timer3.enabled&&timer3.counter)timer3.counter-=cost;return cost;
}
static u32 render_fast_surface(u32 budget_left){
 u32 pc=cpu.r[15],cost,sp=cpu.r[13],lr=cpu.r[14],surface=cpu.r[0];
 u32 x=cpu.r[1],y=cpu.r[2],color=cpu.r[3],stride,offset,pixel;
 if(!render_fastpaths_enabled)return 0;
 switch(pc){
 case 0x3019d4d0u:cost=4;break;
 case 0x3019d4e0u:cost=13;break;
 case 0x3019d504u:cost=30;break;
 case 0x3019d6dcu:cost=34;break;
 default:return 0;
 }
 if(budget_left<cost||(timer3.enabled&&timer3.counter<=cost))return 0;
 if(pc==0x3019d4d0u){cpu.r[1]=3;cpu.r[0]=(3u+(surface<<1))&~3u;}
 else{
  if((sp&3u)||(surface&3u)||!render_plain_ram(surface,16)||!render_plain_ram(sp-16u,16)||
     !render_disjoint(surface,16,sp-16u,16))return 0;
  stride=(3u+((mr32(surface+4)&0xffffu)<<1))&~3u;offset=y*stride+(x<<1);
  pixel=mr32(surface+12)+offset;
  if(pc!=0x3019d4e0u&&(!render_plain_ram(pixel,2)||!render_disjoint(pixel,2,sp-16u,16)))return 0;
  if(pc==0x3019d4e0u){mw32(sp-4,lr);cpu.r[0]=offset;cpu.r[1]=3;cpu.r[3]=x;}
  else if(pc==0x3019d504u){
   mw32(sp-12,cpu.r[4]);mw32(sp-8,cpu.r[5]);mw32(sp-4,lr);mw32(sp-16,0x3019d518u);
   cpu.r[2]=(color>>8)&0xf800u;cpu.r[1]=((color>>5)&0x7e0u)|((color&255u)>>3)|cpu.r[2];
   mw16(pixel,cpu.r[1]);cpu.r[0]=1;cpu.r[3]=x;
  }else{
   u32 v=mr16(pixel),rgb=((v&31u)<<3)|(((v&0x7e0u)>>3)<<8)|(((v&0xf800u)>>8)<<16);
   mw32(sp-12,rgb);mw32(sp-8,cpu.r[4]);mw32(sp-4,lr);mw32(sp-16,0x3019d6f4u);
   cpu.r[0]=cpu.r[3]=rgb;cpu.r[1]=(v&0x7e0u)>>3;
  }
  cpu.r[14]=0x3019d4f8u;
 }
 cpu.r[15]=lr&~3u;cpu.steps+=cost;if(timer3.enabled&&timer3.counter)timer3.counter-=cost;
 return cost;
}

/* Native monochrome glyph loop: exactly one clipped foreground/background
   pixel, including original stack residue and instruction accounting. */
static u32 render_fast_glyph(u32 budget_left){
 u32 sp=cpu.r[13],ctx=cpu.r[4],rect=cpu.r[9],point=cpu.r[7],x,y,cost=5,inside=0,draw=0;
 u32 color=0,surface=0,pixel=0,fg=0,ret=0;
 if(!render_fastpaths_enabled||cpu.r[15]!=0x302c2db0u||(sp&3u)||!render_plain_ram(sp-16,28)||
    (point&3u)||!render_plain_ram(point,4))return 0;
 x=mr32(point)+cpu.r[8];y=mr32(sp+4);
 if(rect){
  if((rect&3u)||!render_plain_ram(rect,16)||!render_disjoint(rect,16,sp-16,28))return 0;
  cost=10;
  if((i32)x>=(i32)mr32(rect)){cost=14;
   if((i32)x<(i32)mr32(rect+8)){cost=18;
    if((i32)y>=(i32)mr32(rect+4)){cost=22;inside=(i32)y<(i32)mr32(rect+12);}
   }
  }
 }
 cost+=14;
 if(inside){
  if((ctx&3u)||!render_plain_ram(ctx,36)||!render_disjoint(ctx,36,sp-16,28))return 0;
  fg=(cpu.r[5]&cpu.r[6])!=0;
  draw=fg||!(mr32(ctx+32)&0x01000000u);
  cost+=fg?37u:draw?39u:5u;
  if(draw){
   color=mr32(ctx+(fg?20:24));surface=mr32(ctx+16);
   if(!render_surface_pixel(surface,x,y,sp,&pixel)||!render_disjoint(surface,16,sp-16,28)||!render_disjoint(pixel,2,sp-16,28))return 0;
  }
 }
 if(budget_left<cost||(timer3.enabled&&timer3.counter<=cost))return 0;
 mw32(sp,x);
 if(draw){
  ret=fg?0x302c2de8u:0x302c2e08u;
  mw32(sp-16,0x3019d518u);mw32(sp-12,ctx);mw32(sp-8,cpu.r[5]);mw32(sp-4,ret);
  cpu.r[2]=(color>>8)&0xf800u;cpu.r[1]=((color>>5)&0x7e0u)|((color&255u)>>3)|cpu.r[2];
  mw16(pixel,cpu.r[1]);cpu.r[3]=x;cpu.r[14]=0x3019d4f8u;
 }else{
  mw32(sp-16,rect);mw32(sp-12,x);mw32(sp-8,y);mw32(sp-4,0x302c2dc8u);
  cpu.r[1]=rect;cpu.r[2]=x;cpu.r[3]=y;cpu.r[14]=0x302c2dc8u;
 }
 cpu.r[5]>>=1;cpu.r[8]++;cpu.r[0]=mr32(sp+8)&255u;sub_flags(cpu.r[0],cpu.r[8],0);
 cpu.r[15]=(i32)cpu.r[0]>(i32)cpu.r[8]?0x302c2db0u:0x302c2e20u;
 cpu.steps+=cost;if(timer3.enabled&&timer3.counter)timer3.counter-=cost;return cost;
}

static int sched_input_edge_pending(void){
 return touch_edge_seq!=touch_edge_acked||key_edge_seq!=key_edge_acked;
}
/* Idle itself is free.  Returning to Windows after every Timer3 quantum
   while a pen edge was still waiting made the click time depend on host
   scheduling between slices, so the same key could be fast or slow.
   The edge is already visible before the slice.  Stay in the guest until
   the firmware samples it, then return so the host can record latency.
   Audio DMA still yields once per quantum because its completion is advanced
   from host time between slices. */
static int sched_idle_should_yield(void){
 if(!idle_slice_quanta)return 0;
 if(idle_slice_had_edge&&!sched_input_edge_pending())return 1;
 if(audio_probe.dma_active)return 1;
 return 0;
}
static int cpu_run(u32 max,u32 stop){
 u32 i=0;cpu_run_depth++;idle_slice_quanta=0;idle_time_blocked=0;idle_slice_had_edge=sched_input_edge_pending()?1u:0u;
 /* A press and the matching release are different edges.  Refill on each
    one.  v0.8.13.4 spent the whole allowance on the press, then the release
    fell back to 35 ms/tick and U went to about 2 s. */
 if(touch_edge_seq!=idle_edge_burst_touch||key_edge_seq!=idle_edge_burst_key){
  idle_edge_burst_touch=touch_edge_seq;idle_edge_burst_key=key_edge_seq;idle_edge_burst_left=160ull*420000ull;
 }
 while(i<max){web_notice_observe(cpu.r[15]);
  /* Press accepted: hand the slice back now so the host can publish the
     release before this click's remaining work keeps the pen down.
     A fast series otherwise waits in line behind that extra pen-down time. */
  if(cpu_run_depth==1u&&idle_gesture_active&&host_input_yield==1u){host_input_yield=0u;g_error=8;g_error_pc=cpu.r[15];cpu_run_depth--;return 0;}
  if(cpu_run_depth==1u&&host_movie.active&&host_input_yield>=2u){host_input_yield=0u;g_error=8;g_error_pc=cpu.r[15];cpu_run_depth--;return 0;}
  /* While host movie is feeding frames, return often so Win32 stays alive even
     if Task_Movie_Video never sets host_input_yield. */
  if(cpu_run_depth==1u&&host_movie.active&&i&&(i%50000u)==0u){g_error=8;g_error_pc=cpu.r[15];cpu_run_depth--;return 0;}
  if(cpu.r[15]==stop){cpu_run_depth--;return 1;}
  /* Original System Power Off ends in an infinite loop after cutting power.
     Surface that to the host as a soft power-off instead of hanging the guest. */
  if(cpu_run_depth==1u&&cpu.r[15]==0x30195100u){virtual_power_cut=1u;g_error=8;g_error_pc=cpu.r[15];cpu_run_depth--;return 0;}
  if(system_irq_enabled&&!cpu.I&&cpu.mode!=0x12u&&hw_irq_pending())cpu_raise_irq();
  if((cpu.r[15]&0xfffff000u)==0x3019d000u){u32 fast=render_fast_surface(max-i);if(fast){i+=fast;continue;}}
  if(cpu.r[15]==0x3019b02cu){u32 fast=render_fast_clip(max-i);if(fast){i+=fast;continue;}}
  if(cpu.r[15]==0x3019efecu){u32 fast=render_fast_colorkey(max-i);if(fast){i+=fast;continue;}}
  if(cpu.r[15]==0x302c2db0u){u32 fast=render_fast_glyph(max-i);if(fast){i+=fast;continue;}}
  {u32 fast=render_fast_palette8(max-i);if(fast){i+=fast;continue;}}
  {u32 fast=render_fast_memcpy32(max-i);if(fast){i+=fast;continue;}}
  {u32 fast=render_fast_delay1000(max-i);if(fast){i+=fast;continue;}}
  {u32 fast=render_fast_delay1m(max-i);if(fast){i+=fast;continue;}}
  /* V1+ RTOS Idle Task is literally one ARM instruction at runtime
       0x3018FEFC: EAFFFFFE  (B .)
     It has no architectural side effects besides consuming instruction time
     while Timer3 counts toward the next scheduler tick.  Interpreting that
     same self-branch hundreds of thousands of times makes event-driven apps
     feel artificially sluggish on the host.  Advance the *exact* number of
     idle guest instructions and Timer3 clocks in one operation.  Guest step
     count, PC, timer expiry and IRQ timing are unchanged. */
  if(system_irq_enabled&&cpu.r[15]==0x3018fefcu&&timer3.enabled&&timer3.counter&&
     !hw_audio_irq_pending()){
   /* Release observed: let the touch task finish its up message, then return
      at Idle so the next press can start. */
   if(cpu_run_depth==1u&&idle_gesture_active&&host_input_yield>=2u){host_input_yield=0u;g_error=8;g_error_pc=cpu.r[15];cpu_run_depth--;return 0;}
   if(sched_idle_should_yield()){g_error=8;g_error_pc=cpu.r[15];cpu_run_depth--;return 0;}
   /* Horizon is the soonest of the remaining host budget and Timer3.
      Audio IRQ already blocks this path via hw_audio_irq_pending().
      After the firmware samples a pending pen/key edge, or while audio DMA
      is active, the slice returns instead of skipping another quantum. */
   u32 left=max-i,skip=timer3.counter<left?timer3.counter:left;
   /* A host gesture stays active from press until the firmware has seen the
      release.  Edge acks happen earlier, and pacing the ticks after that ack
      is what left U around 900 ms.  While the gesture is open, idle ticks are
      not limited to wall-clock credit. */
   if(idle_time_credit_on && !idle_gesture_active && sched_input_edge_pending() && idle_edge_burst_left){
    if((u64)skip>idle_edge_burst_left)skip=(u32)idle_edge_burst_left;
    idle_edge_burst_left-=skip;
   }else if(idle_time_credit_on && !idle_gesture_active){
    if(!idle_time_credit){idle_time_blocked=1u;g_error=8;g_error_pc=cpu.r[15];cpu_run_depth--;return 0;}
    if((u64)skip>idle_time_credit)skip=(u32)idle_time_credit;
    idle_time_credit-=skip;
   }
   cpu.steps+=(u64)skip;i+=skip;timer3.counter-=skip;debug_idle_fast_forwarded_steps+=(u64)skip;debug_idle_fast_forward_events++;
   if(timer3.counter==0u){hw_timer3_expire();idle_slice_quanta++;}
   continue;
  }
  if(!cpu_step()){cpu_run_depth--;return 0;}
  hw_timer3_step();i++;
  if(suspend_request){cpu_run_depth--;return 2;}
 }
 g_error=8;g_error_pc=cpu.r[15];cpu_run_depth--;return 0;
}
static int call_arm(u32 addr,u32 r0,u32 r1,u32 r2,u32 r3,u32 max){cpu.r[15]=addr;cpu.r[14]=SENTINEL;cpu.r[0]=r0;cpu.r[1]=r1;cpu.r[2]=r2;cpu.r[3]=r3;int z=cpu_run(max,SENTINEL);if(z==2){suspended_cpu=cpu;suspended_valid=1;suspend_request=0;return 2;}return z==1?1:0;}
static int resume_suspended(void){if(!suspended_valid)return 1;cpu=suspended_cpu;cpu.r[0]=child.result;cpu.r[15]=cpu.r[14]&~3u;suspended_valid=0;int z=cpu_run(1200000,SENTINEL);if(z==2){suspended_cpu=cpu;suspended_valid=1;suspend_request=0;return 2;}return z==1?1:0;}
static void clearfb(void){for(u32 i=0;i<FB_W*FB_H;i++)framebuffer[i]=0;}

/* Original RTOS non-client painter reached from WM_NCPAINT (0x85). */
static void paint_nonclient(void){
 if(!child.active)return;
 /* Caption skin blits are not backed by the standalone host. The original
    modal painter fills only the client below the speaker's name, so make
    its non-client caption opaque before the firmware draws that name.
    Use the RTOS client boundary and the dialog's existing background colour. */
 if(!child.modeless&&(mr32(child.handle+0x10u)&0x00c00000u)==0x00c00000u){
  i32 top=0;
  if(child_client_rect(child.handle,0,&top,0,0)&&top>0){
   Obj*d=getobj(child.dc);
   if(d)fillrect(d,0,0,child.right-child.left,top,cref_to_rgb(d->bk_color));
  }
 }
 CPU saved=cpu;int sv=suspended_valid;CPU ssave=suspended_cpu;suspended_valid=0;
 call_arm(0x30197544u,child.handle,0,0,0,300000);
 cpu=saved;suspended_valid=sv;suspended_cpu=ssave;g_error=0;paint_notebook_selection();
}
static void deliver_child(void){if(!child.active||!child.pending)return;child.pending=0;CPU saved=cpu;int sv=suspended_valid;CPU ssave=suspended_cpu;suspended_valid=0;call_arm(child.callback,child.handle,1,0,0,300000);call_arm(child.callback,child.handle,WM_PAINT,0,0,500000);paint_nonclient();cpu=saved;suspended_valid=sv;suspended_cpu=ssave;g_error=0;}
#include "web_windows.inc"

int emu_set_system_data_image(const char*path){
 if(nand_data_file){fclose(nand_data_file);nand_data_file=0;}nand_data_path[0]=0;system_data_base=system_data_len=system_data_magic_ok=0;
 memzero(vflash_ro_cache,sizeof(vflash_ro_cache));vflash_ro_cache_hits=vflash_ro_cache_misses=0u;
 if(!path)return 1;u32 i=0;while(path[i]&&i+1u<sizeof(nand_data_path)){nand_data_path[i]=path[i];i++;}nand_data_path[i]=0;
 if(!nand_open_data())return 0;
 /* The decrypted V1PRU wrapper is 0x1c bytes on this firmware.  Detect the
    marker instead of hard-coding it so alternate dumps can place the stream
    at file offset zero or another small wrapper offset. */
 {u8 head[256];size_t got;long end;rewind(nand_data_file);got=fread(head,1,sizeof(head),nand_data_file);
  for(u32 off=0;off+16u<=got;off++){static const u8 magic[16]={'@','O','Z','I','N','G','S','Y','S','T','E','M','D','A','T','A'};u32 j=0;while(j<16u&&head[off+j]==magic[j])j++;if(j==16u){system_data_base=off;system_data_magic_ok=1;break;}}
  if(fseek(nand_data_file,0,SEEK_END)==0&&(end=ftell(nand_data_file))>=0&&(u32)end>=system_data_base)system_data_len=(u32)end-system_data_base;
  rewind(nand_data_file);}
 return system_data_magic_ok?1:0;
}
void emu_set_text_fallback(EmuTextFallback cb){text_fallback=cb;(void)text_fallback;}
int emu_set_program_image(const char*path){
 program_external_active=0;program_external_size=0;
 if(!path||!path[0])return 1;
 FILE*f=fopen(path,"rb");if(!f)return 0;
 if(fseek(f,0,SEEK_END)!=0){fclose(f);return 0;}long z=ftell(f);if(z<=0||(u64)z>sizeof(program_external)){fclose(f);return 0;}
 rewind(f);if(fread(program_external,1,(u32)z,f)!=(u32)z){fclose(f);return 0;}fclose(f);
 program_external_size=(u32)z;program_external_active=1;return 1;
}
static int load_firmware_image(void){
 const u8*src=program_external_active?program_external:_binary_fw_bin_start;
 u32 fwsz=program_external_active?program_external_size:(u32)(_binary_fw_bin_end-_binary_fw_bin_start);
 firmware_payload_offset=0;program_data_base=program_data_len=program_data_magic_ok=0;
 /* V1PU has the same 0x1c-byte update-container wrapper as V1PRU.  Prefer
    the second OZUPDSYS marker, which is followed by the native update stream. */
 for(u32 off=0;off+8u<=fwsz&&off<256u;off++){
  static const u8 pm[8]={'O','Z','U','P','D','S','Y','S'};u32 j=0;while(j<8u&&src[off+j]==pm[j])j++;
  if(j==8u){program_data_base=off;program_data_magic_ok=1;if(off>=0x1cu)break;}
 }
 if(program_data_magic_ok)program_data_len=fwsz-program_data_base;
 if(fwsz>0x4cu && src[0x0c]=='O' && src[0x0d]=='Z' && src[0x0e]=='U' && src[0x0f]=='P' &&
    (src[0x4b]&0xf0u)==0xe0u) firmware_payload_offset=0x48u;
 else if(fwsz>0x2cu && (src[0x2b]&0xf0u)==0xe0u) firmware_payload_offset=0x28u;
 else return 0;
 memzero(fwmem,FW_MEM_SIZE);
 {u32 n=fwsz-firmware_payload_offset;if(n>FW_MEM_SIZE)n=FW_MEM_SIZE;memcopy(fwmem,src+firmware_payload_offset,n);}
 return 1;
}
int emu_init(void){web_story_pending=0;web_flight_active=web_flight_prepared=0;web_notice_active=web_notice_return=0;web_battle_drawn=0;parent_shop_valid=parent_notebook_valid=0;web_sleep_ms=0;memzero(web_heap_addr,sizeof(web_heap_addr));memzero(web_heap_size,sizeof(web_heap_size));memzero(web_heap_used,sizeof(web_heap_used));const EmuAppProfile*profile=emu_profile_current();if(!profile)return 0;g_error=0;g_error_pc=0;system_irq_enabled=0;memzero(&timer3,sizeof(timer3));rtc_reset();memzero(&audio_probe,sizeof(audio_probe));audio_output_reset();memzero(fwmem,FW_MEM_SIZE);memzero(ram2,RAM2_SIZE);memzero(ram3,RAM3_SIZE);memzero(objects,sizeof(objects));memzero(timers,sizeof(timers));memzero(&child,sizeof(child));memzero(&cpu,sizeof(cpu));memzero(&suspended_cpu,sizeof(suspended_cpu));suspended_valid=suspend_request=0;invalidated=erase_pending=0;next_alloc=0x32010000u;next_handle=HANDLE_BASE;emu_profile_reset();
 if(!load_firmware_image())return 0;
 /* imported save survives init only through host reimport */clearfb();
 fb_dc=new_handle(OBJ_DC);Obj*d=getobj(fb_dc);d->framebuffer=1;d->text_color=0xffffff;d->bk_color=0;d->bk_mode=1;mw32(WINDOW+0x20,WINDOW);mw32(SENTINEL,0xEAFFFFFEu);cpu.mode=0x13u;cpu.r[13]=0x33FFF000u;cpu.r[14]=SENTINEL;cpu.bank_sp[3]=cpu.r[13];cpu.bank_lr[3]=cpu.r[14];
 if(!call_arm(profile->app_init,WINDOW,0,0,0,600000))return 0;deliver_child();emu_paint();return g_error?0:1;}
void emu_paint(void){if(paint_web_flight())return;if(paint_web_combat())return;if(erase_pending){clearfb();erase_pending=0;}invalidated=0;if(child.active){CPU saved=cpu;int sv=suspended_valid;CPU ss=suspended_cpu;suspended_valid=0;paint_shop_background();call_arm(child.callback,child.handle,WM_PAINT,0,0,500000);paint_nonclient();cpu=saved;suspended_valid=sv;suspended_cpu=ss;g_error=0;}else call_arm(emu_profile_current()->app_paint,WINDOW,0,0,0,700000);paint_web_notice();}
int emu_key(u32 key){if(web_sleep_ms||web_combat_active())return 1;web_notice_active=0;if(web_flight_active)return web_flight_key(key);if(child.active)return child_key(key);u32 old=emu_mode();int ok=call_arm(emu_profile_current()->app_wndproc,WINDOW,WM_KEYDOWN,(key&0xffffu)<<16,0,900000);if(!ok&&g_error)return 0;if(child.active&&child.pending)deliver_child();if(emu_mode()==old&&!child.active)emu_paint();return 1;}
int emu_timer(u32 id){if(web_sleep_ms||child.active||web_flight_active)return 1;u32 old=emu_mode();int ok=call_arm(emu_profile_current()->app_wndproc,WINDOW,WM_TIMER,id,0,900000);if(!ok&&g_error)return 0;if(child.active&&child.pending)deliver_child();if(emu_mode()!=old&&!child.active)emu_paint();return 1;}
u32 emu_mode(void){const EmuAppProfile*p=emu_profile_current();return p?mr32(p->ui_mode_addr):0;}int emu_dialog_active(void){return child.active||web_flight_active?1:0;}u32 emu_dialog_title_ptr(void){return child.active?child.title_ptr:0;}
static u32 statep(void){return mr32(WINDOW+0x58);}u32 emu_selection(void){u32 s=statep();return s?mr32(s):0;}u32 emu_floor(void){u32 s=statep();return s?mr8(s+5):0;}u32 emu_x(void){u32 s=statep();return s?mr32(s+0x76):0;}u32 emu_y(void){u32 s=statep();return s?mr32(s+0x7a):0;}u32 emu_hp(void){u32 s=statep();return s?mr32(s+0x46):0;}u32 emu_atk(void){u32 s=statep();return s?mr32(s+0x4a):0;}u32 emu_def(void){u32 s=statep();return s?mr32(s+0x4e):0;}const u32* emu_framebuffer(void){return framebuffer;}
int emu_timer_count(void){int n=0;for(int i=0;i<MAX_TIMERS;i++)if(timers[i].active)n++;return n;}int emu_timer_info(int idx,u32*id,u32*iv){for(int i=0;i<MAX_TIMERS;i++)if(timers[i].active){if(idx--==0){if(id)*id=timers[i].id;if(iv)*iv=timers[i].interval;return 1;}}return 0;}
u32 emu_save_size(void){return emu_profile_save_size();}u32 emu_save_generation(void){return emu_profile_save_generation();}int emu_save_export(u8*dst,u32 cap){return emu_profile_save_export(dst,cap);}int emu_save_import(const u8*src,u32 len){int ok=emu_profile_save_import(src,len);if(ok)for(int i=0;i<MAX_OBJECTS;i++)if(objects[i].kind==OBJ_FILE&&objects[i].rid==0x89u)objects[i].len=len;return ok;}int emu_last_error(void){return g_error;}u32 emu_last_error_pc(void){return g_error_pc;}
int emu_copy_cstring(u32 ptr,u8*dst,u32 cap){if(!dst||!cap)return 0;u32 n=0;while(n+1<cap){u8 c=mr8(ptr+n);if(g_error){g_error=0;break;}dst[n++]=c;if(!c)return (int)n;}dst[n]=0;return (int)n;}
static int emu_system_reset_internal(void){
 virtual_touch_sample_x=virtual_touch_sample_y=virtual_touch_sample_active=0u;
 virtual_touch_calibration_active=virtual_touch_calibration_x=virtual_touch_calibration_y=0u;
 key_contact_sampled=0u;virtual_key_vk=virtual_key_down=0u;virtual_power_down=virtual_power_cut=virtual_power_sampled=0u;
 host_movie_release_source();memzero(&host_movie,sizeof(host_movie));host_media_path[0]=0;host_fc_close();
#ifdef EMU_TESTING
 movie_soft_entries=0; /* Per boot; native audio shares the media-init entry. */
#endif
 g_error=0;g_error_pc=0;
 /* System mode must be self-contained.  Older bring-up tests called emu_init()
    first, which happened to initialize the host-side handle allocator.  The
    native Windows frontend enters here directly, so leaving next_handle at
    its C static default (0) made RID 0x10/0x03 bridges fail to allocate an
    OBJ_FILE and sent the real firmware into its update screen forever. */
 memzero(objects,sizeof(objects));memzero(timers,sizeof(timers));memzero(&child,sizeof(child));
 next_handle=HANDLE_BASE;next_alloc=0x32010000u;invalidated=erase_pending=0;
 memzero(ram2,RAM2_SIZE);memzero(ram3,RAM3_SIZE);memzero(sram4,SRAM4_SIZE);memzero(mmio7,MMIO7_SIZE);memzero(cp15_regs,sizeof(cp15_regs));memzero(semihost_log,sizeof(semihost_log));semihost_log_len=0;semihost_line_len=0;memzero(debug_trace,sizeof(debug_trace));debug_trace_pos=0;
 if(!load_firmware_image())return 0;
 memzero(sys_nv133,sizeof(sys_nv133));memzero(sys_nv138,sizeof(sys_nv138));sys_nv133_len=sys_nv138_len=sys_nv133_present=sys_nv138_present=0;system_state_dirty=1u;
 memzero(&cpu,sizeof(cpu));suspend_request=0;suspended_valid=0;
 idle_credit_key_edge=0;
#ifdef EMU_TOWER_PROFILE
 tower_hwnd=tower_key=tower_key_count=0;
#endif
 cpu.mode=0x13u;cpu.I=1;cpu.F=1;system_irq_enabled=1;memzero(&timer3,sizeof(timer3));rtc_reset();memzero(&audio_probe,sizeof(audio_probe));audio_output_reset();nand_sram_fill_ff();nand_cmd_count=nand_last_cmd0=nand_last_ctrl=0;nand_last_col=nand_last_row=nand_read_commands=0;debug_context_switches=debug_scheduler_selects=0;debug_vflash_reads=debug_vflash_writes=0;debug_vflash_last_off=0;debug_vflash_last_len=0;debug_vf_pos=0;memzero(debug_vf_ops,sizeof(debug_vf_ops));vflash_overlay_count=0;memzero(vflash_overlay,sizeof(vflash_overlay));debug_storage_hle_calls=debug_ftl_bypass_calls=debug_system_file_opens=debug_system_file_reads=0;debug_file_open_attempts=debug_last_file_rid=0;memzero(debug_file_rid_counts,sizeof(debug_file_rid_counts));memzero(debug_rid_open_success,sizeof(debug_rid_open_success));memzero(debug_rid_open_fail,sizeof(debug_rid_open_fail));memzero(debug_rid_first_handle,sizeof(debug_rid_first_handle));memzero(debug_rid_first_lr,sizeof(debug_rid_first_lr));memzero(debug_pending_open,sizeof(debug_pending_open));debug_res_op_pos=0;memzero(debug_res_ops,sizeof(debug_res_ops));memzero(debug_startup_milestone_counts,sizeof(debug_startup_milestone_counts));debug_startup_last_milestone=debug_startup_syscheck=0;debug_touch_autocal_samples=0;memzero(debug_touch_autocal_xy,sizeof(debug_touch_autocal_xy));virtual_touch_x=virtual_touch_y=virtual_touch_down=0;virtual_touch_adc_x_reads=virtual_touch_adc_y_reads=virtual_touch_down_accepted=virtual_touch_up_seen=0;virtual_key_vk=virtual_key_down=0;touch_edge_seq=touch_edge_acked=key_edge_seq=key_edge_acked=0;trace_gpio_release=trace_touch_wake=trace_pen_clear=trace_frame=0;idle_slice_quanta=idle_slice_had_edge=0;idle_time_credit=0;idle_time_credit_on=0;idle_time_blocked=0;idle_gesture_active=0;host_input_yield=0;cpu_run_depth=0;idle_edge_burst_left=160ull*420000ull;idle_edge_burst_touch=idle_edge_burst_key=0;debug_lcd_writes=debug_lcd_last=debug_lcd_ring_pos=0;memzero(debug_lcd_ring,sizeof(debug_lcd_ring));debug_lcd_dma_pos=0;memzero(debug_lcd_dma,sizeof(debug_lcd_dma));debug_lcd_full_count=0;memzero(debug_lcd_full_frames,sizeof(debug_lcd_full_frames));memzero(&lcd,sizeof(lcd));memzero(lcd_panel565,sizeof(lcd_panel565));lcd_primary_scanout_active=0u;lcd_primary_dirty=0u;lcd_generation=0u;debug_task_changes=debug_last_task=0;memzero(debug_task_select_count,sizeof(debug_task_select_count));debug_idle_fast_forwarded_steps=0;debug_idle_fast_forward_events=0;debug_render_fast_pixels=0;debug_render_fast_events=0;debug_delay1m_fast_events=0;debug_desktop_obj=debug_desktop_hwnd=debug_desktop_cb=debug_desktop_msg_count=0;memzero(debug_desktop_msgs,sizeof(debug_desktop_msgs));debug_win_create_count=0;memzero(debug_win_create,sizeof(debug_win_create));debug_bitmap_decode_count=0;memzero(debug_bitmap_decode,sizeof(debug_bitmap_decode));debug_bitmap_header_count=0;memzero(debug_bitmap_headers,sizeof(debug_bitmap_headers));cpu.r[15]=VBASE;cpu.r[13]=0x31fff000u;cpu.r[14]=0;cpu.bank_sp[3]=cpu.r[13];
 return 1;
 }

int emu_system_init_ex(const char*v1pu_path,const char*v1pru_path,const char*state_path){
 {char b[300];sprintf(b,"boot: init V1PU=%s",v1pu_path?v1pu_path:"(null)");emu_system_log(b);}
 if(!emu_set_program_image(v1pu_path)){emu_system_log("boot: V1PU load failed");return 0;}
 if(!emu_set_system_data_image(v1pru_path)){emu_system_log("boot: V1PRU load failed");return 0;}
 system_state_path[0]=0;if(state_path){u32 i=0;while(state_path[i]&&i+1u<sizeof(system_state_path)){system_state_path[i]=state_path[i];i++;}system_state_path[i]=0;if(state_path[i]){emu_system_log("boot: state path too long");return 0;}}
 if(!emu_system_reset_internal()){emu_system_log("boot: reset failed");return 0;}
 if(!system_state_load_file()){emu_system_log("boot: state load failed");return 0;}
 emu_system_log("boot: cpu reset, firmware starting");return 1;
}
int emu_system_init(const char*v1pru_path,const char*state_path){return emu_system_init_ex(0,v1pru_path,state_path);}
int emu_system_run(u32 instruction_budget){g_error=0;g_error_pc=0;if(!instruction_budget){lcd_sync_primary_scanout();if(host_movie.active)host_movie_pump();return 1;}{int z=cpu_run(instruction_budget,0xffffffffu);if(!z&&g_error==2){char b[180];sprintf(b,"fault: invalid memory addr=%08X origin_pc=%08X sp=%08X lr=%08X",g_bad_mem_addr,g_bad_mem_origin_pc,g_bad_mem_origin_sp,g_bad_mem_origin_lr);emu_system_log(b);}if(!z&&g_error==8){g_error=0;g_error_pc=0;z=1;}lcd_sync_primary_scanout();if(host_movie.active)host_movie_pump();return z;}}
u32 emu_system_timer_counts_per_second(void){return EMU_SYSTEM_TIMER_COUNTS_PER_SEC;}
void emu_system_add_idle_credit(u32 counts){
 idle_time_credit_on=1u;
 /* The host polls input before adding elapsed time. A stalled slice's
    backlog must not age a fresh contact into native long-press repeats.
    Preserve ordinary small slices, including the scanner's pending ticks. */
 if(idle_credit_key_edge){idle_credit_key_edge=0;if(counts>EMU_SYSTEM_TIMER_COUNTS_PER_SEC/20u)counts=0;}
 idle_time_credit+=(u64)counts;
 if(idle_time_credit>2ull*EMU_SYSTEM_TIMER_COUNTS_PER_SEC)idle_time_credit=2ull*EMU_SYSTEM_TIMER_COUNTS_PER_SEC;
}
void emu_system_set_gesture_active(int on){idle_gesture_active=on?1u:0u;}
int emu_system_idle_time_blocked(void){return idle_time_blocked?1:0;}
void emu_system_touch(u32 x,u32 y,int down){
 u32 nd=down?1u:0u;if(x>=LCD_W)x=LCD_W-1u;if(y>=LCD_H)y=LCD_H-1u;virtual_touch_x=x;virtual_touch_y=y;
 if(nd!=virtual_touch_down){
  char b[80];touch_edge_seq++;
  sprintf(b,"input: touch %s x=%u y=%u",nd?"down":"up",x,y);emu_system_log(b);
 }
 if(nd&&!virtual_touch_down){virtual_touch_adc_x_reads=virtual_touch_adc_y_reads=0u;virtual_touch_down_accepted=0u;virtual_touch_up_seen=0u;}
 else if(!nd&&virtual_touch_down){virtual_touch_up_seen=0u;trace_gpio_release++;}
 virtual_touch_down=nd;
}
u32 emu_system_touch_down_accepted(void){return virtual_touch_down_accepted;}
u32 emu_system_touch_up_seen(void){return virtual_touch_up_seen;}
void emu_system_input_trace(u32*gpio_release,u32*touch_wake,u32*pen_clear,u32*frame){
 if(gpio_release)*gpio_release=trace_gpio_release;if(touch_wake)*touch_wake=trace_touch_wake;if(pen_clear)*pen_clear=trace_pen_clear;if(frame)*frame=trace_frame;
}
void emu_system_key(u32 vk,int down){
 u32 nd=down?1u:0u;
 if(nd!=virtual_key_down||vk!=virtual_key_vk){
  char b[64];key_edge_seq++;key_contact_sampled=0u;
  if(idle_time_credit>EMU_SYSTEM_TIMER_COUNTS_PER_SEC/20u)idle_time_credit=0;idle_credit_key_edge=1u;
  sprintf(b,"input: key vk=%u %s",vk,nd?"down":"up");emu_system_log(b);
 }
 virtual_key_vk=vk;virtual_key_down=nd;
}
void emu_system_power_key(int down){
 u32 nd=down?1u:0u;
 if(nd!=virtual_power_down){
  virtual_power_sampled=0u;
  char b[48];key_edge_seq++;
  if(idle_time_credit>EMU_SYSTEM_TIMER_COUNTS_PER_SEC/20u)idle_time_credit=0;idle_credit_key_edge=1u;
  sprintf(b,"input: power-key %s",nd?"down":"up");emu_system_log(b);
 }
 virtual_power_down=nd;
}
u32 emu_system_power_key_down(void){return virtual_power_down;}
u32 emu_system_power_key_sampled(void){return virtual_power_sampled;}
u32 emu_system_power_cut_pending(void){return virtual_power_cut;}
void emu_system_clear_power_cut(void){virtual_power_cut=0;}
u32 emu_system_key_sampled(void){return key_contact_sampled;}
void emu_system_clear_touch_calibration(void){if(sys_nv138_present||sys_nv138_len)system_state_dirty=1u;sys_nv138_present=0u;sys_nv138_len=0u;memzero(sys_nv138,sizeof(sys_nv138));}
void emu_system_rtc_host_time(u32 seconds_since_1980){
 if(!rtc.host_valid){rtc.host_valid=1u;if(!rtc.offset_valid){rtc.host_offset=0;rtc.offset_valid=1u;}}
 rtc.host_seconds=seconds_since_1980;
}
void emu_system_set_rtc_seconds(u32 seconds_since_1980){rtc_set_seconds(seconds_since_1980);}
u32 emu_system_rtc_seconds(void){return rtc_current_seconds();}
void emu_system_audio_advance_us(u64 elapsed_us){audio_advance_us(elapsed_us);}
u32 emu_system_audio_dma_events(void){return audio_probe.dma_events;}
u32 emu_system_audio_dma_completions(void){return audio_probe.dma_completions;}
u32 emu_system_audio_sample_rate(void){return audio_probe.sample_rate;}
u32 emu_system_audio_last_event(void){return audio_probe.last_event;}
int emu_system_audio_copy(u8*dst,u32 cap){u32 n=audio_probe.captured_bytes;if(!dst)return 0;if(n>cap)n=cap;if(n)memcopy(dst,audio_probe.captured,n);return (int)n;}
int emu_system_audio_submit(u32 index,u32*addr,u32*words,u32*nonzero,u32*sig){if(index>=audio_probe.dma_events||index>=16u)return 0;if(addr)*addr=audio_probe.submit_log_addr[index];if(words)*words=audio_probe.submit_log_words[index];if(nonzero)*nonzero=audio_probe.submit_log_nz[index];if(sig)*sig=audio_probe.submit_log_sig[index];return 1;}
int emu_system_audio_copy_sound(u8*dst,u32 cap){u32 n=audio_probe.first_sound_bytes;if(!dst)return 0;if(n>cap)n=cap;if(n)memcopy(dst,audio_probe.first_sound,n);return (int)n;}
u32 emu_system_audio_irq_entries(void){return audio_probe.irq_entries;}
void emu_system_audio_irq_gate(u32*raw,u32*mask,u32*vector){if(raw)*raw=mr32(0x20000014u);if(mask)*mask=mr32(0x20000018u);if(vector)*vector=mr32(0x30488594u+5u*4u);}
u32 emu_system_audio_vf_reads(void){return audio_probe.vf_reads;}
int emu_system_audio_vf(u32 index,u32*dest,u32*len,u32*nz,u32*lr,u32*off_lo,u32*off_hi){if(index>=audio_probe.vf_large||index>=8u)return 0;if(dest)*dest=audio_probe.vf_dest[index];if(len)*len=audio_probe.vf_len[index];if(nz)*nz=audio_probe.vf_nz[index];if(lr)*lr=audio_probe.vf_lr[index];if(off_lo)*off_lo=(u32)audio_probe.vf_off[index];if(off_hi)*off_hi=(u32)(audio_probe.vf_off[index]>>32);return 1;}
u32 emu_system_audio_vf_dma_hits(void){return audio_probe.vf_dma_hits;}
int emu_system_mem_copy(u32 addr,u8*dst,u32 n){u8*p;if(!dst||!n)return 0;p=(u8*)memptr(addr,n);if(!p)return 0;memcopy(dst,p,n);return (int)n;}
static u32 pcm_hot_addr[8],pcm_hot_nz[8];
static void pcm_hot_add(u32 addr,u32 nz){u32 i,slot;if(nz<32u)return;slot=8u;for(i=0;i<8u;i++)if(nz>pcm_hot_nz[i]){slot=i;break;}if(slot>=8u)return;for(i=7u;i>slot;i--){pcm_hot_addr[i]=pcm_hot_addr[i-1u];pcm_hot_nz[i]=pcm_hot_nz[i-1u];}pcm_hot_addr[slot]=addr;pcm_hot_nz[slot]=nz;}
static void pcm_hot_range(const u8*p,u32 base,u32 n){u32 off=0;while(off+4096u<=n){u32 nz=0,i;const u8*s=p+off;for(i=0;i<4096u;i++)if(s[i])nz++;pcm_hot_add(base+off,nz);off+=4096u;}}
void emu_system_pcm_scan(void){u32 i;for(i=0;i<8u;i++){pcm_hot_addr[i]=0;pcm_hot_nz[i]=0;}pcm_hot_range(fwmem+0x480000u,0x30480000u,FW_MEM_SIZE-0x480000u);pcm_hot_range(ram3,RAM3_BASE,RAM3_SIZE);pcm_hot_range(sram4,SRAM4_BASE,SRAM4_SIZE);}
int emu_system_pcm_hot(u32 index,u32*addr,u32*nz){if(index>=8u||!pcm_hot_nz[index])return 0;if(addr)*addr=pcm_hot_addr[index];if(nz)*nz=pcm_hot_nz[index];return 1;}
#ifdef AUDIO_TRACE
u32 emu_audio_trace_cb(u32*r0,u32*r1,u32*r2,u32*r3){if(r0)*r0=audio_cb_r0;if(r1)*r1=audio_cb_r1;if(r2)*r2=audio_cb_r2;if(r3)*r3=audio_cb_r3;return audio_cb_hits;}
u32 emu_audio_trace_play(u32*wait,u32*wait_r0,u32*post){if(wait)*wait=audio_play_wait;if(wait_r0)*wait_r0=audio_play_wait_r0;if(post)*post=audio_play_post;return audio_play_enter;}
u32 emu_audio_trace_ring(u32*stores,u32*nz,u32*pc,u32*addr,u32*memcpy_n){if(stores)*stores=audio_ring_stores;if(nz)*nz=audio_ring_nz_stores;if(pc)*pc=audio_ring_store_pc;if(addr)*addr=audio_ring_store_addr;if(memcpy_n)*memcpy_n=audio_memcpy_ring;return audio_ring_nz_stores;}
u32 emu_audio_trace_sub(u32 index,u32*r0,u32*r1,u32*from){if(index>=audio_sub_n)return 0;if(r0)*r0=audio_sub_r0[index];if(r1)*r1=audio_sub_r1[index];if(from)*from=audio_sub_from[index];return 1;}
u32 emu_audio_trace_mix(u32*copy,u32*dst,u32*src,u32*len,u32*qpush,u32*qsrc,u32*qlen,u32*drv_a,u32*drv_b){if(copy)*copy=audio_mix_copy;if(dst)*dst=audio_mix_dst;if(src)*src=audio_mix_src;if(len)*len=audio_mix_len;if(qpush)*qpush=audio_qpush;if(qsrc)*qsrc=audio_qpush_src;if(qlen)*qlen=audio_qpush_len;if(drv_a)*drv_a=audio_drv_a;if(drv_b)*drv_b=audio_drv_b;return audio_mix_enter;}
u32 emu_audio_trace_mix_exit(u32*empty,u32*at,u32*r9,u32*bad){if(empty)*empty=audio_mix_empty;if(at)*at=audio_mix_at;if(r9)*r9=audio_mix_r9;if(bad)*bad=audio_mix_bad;return audio_mix_give;}
u32 emu_audio_trace_mix_r5(u32*r5,u32*dest,u32*give,u32*a0){if(r5)*r5=audio_mix_r5;if(dest)*dest=audio_mix_dest;if(give)*give=audio_mix_give;if(a0)*a0=audio_mix_a0;return audio_mix_give;}
u32 emu_audio_trace_link(u32*ret_n,u32*ret,u32*fill,u32*ind,u32*fn,u32*isr,u32*isr_fn,u32*ev75,u32*body,u32*set){if(ret_n)*ret_n=audio_link_ret_n;if(ret)*ret=audio_link_ret;if(fill)*fill=audio_link_fill;if(ind)*ind=audio_link_ind;if(fn)*fn=audio_link_fn;if(isr)*isr=audio_isr_cb;if(isr_fn)*isr_fn=audio_isr_fn;if(ev75)*ev75=audio_isr75;if(body)*body=audio_cb_body;if(set)*set=audio_cb_set;return audio_link_fill;}
u32 emu_audio_trace_sig(u32*sig,u32*drop8,u32*drop5,u32*f8,u32*st){if(sig)*sig=audio_sig;if(drop8)*drop8=audio_sig_drop8;if(drop5)*drop5=audio_sig_drop5;if(f8)*f8=audio_sig_f8;if(st)*st=audio_sig_st;return audio_sig;}
u32 emu_audio_trace_waiter(u32*waiter,u32*n,u32*thread,u32*msg){if(waiter)*waiter=audio_waiter;if(n)*n=audio_waiter_n;if(thread)*thread=audio_thread;if(msg)*msg=audio_thread_msg;return audio_slot8_nz;}
u32 emu_audio_trace_slot8(u32*n,u32*nz){if(n)*n=audio_slot8_n;if(nz)*nz=audio_slot8_nz;return audio_slot8_n;}
u32 emu_audio_trace_cmd(u32 cmd,u32*n,u32*lr){if(cmd>=16u)return 0;if(n)*n=audio_cmd_count[cmd];if(lr)*lr=audio_cmd_lr[cmd];return audio_cmd_count[cmd];}
u32 emu_audio_trace_cmd7(u32*fn,u32*obj){if(fn)*fn=audio_cmd7_fn;if(obj)*obj=audio_cmd7_obj;return audio_dma_start_lr;}
#endif
const u16* emu_system_lcd565(void){return lcd_panel565;}
u32 emu_system_lcd_generation(void){return lcd_generation;}
u32 emu_system_lcd_width(void){return LCD_W;}
u32 emu_system_lcd_height(void){return LCD_H;}
int emu_system_save_state(void){return system_state_save_file();}
int emu_system_autosave_state(void){return system_state_autosave_file();}
void emu_system_movie_resume_clock(void){host_movie_resume_clock();}
int emu_system_load_state(void){return system_state_load_file();}
u64 emu_system_steps(void){return cpu.steps;}
u32 emu_system_pc(void){return cpu.r[15];}
#ifdef EMU_TESTING
u32 emu_debug_movie_stats(u32*dst){if(dst){dst[0]=host_movie.frames;dst[1]=host_movie.position_ms;dst[2]=host_movie.duration_ms;dst[3]=host_movie.active;dst[4]=host_movie.paused;dst[5]=host_movie.opened;dst[6]=host_movie.state;dst[7]=movie_soft_entries;dst[8]=host_movie.vx;dst[9]=host_movie.vy;dst[10]=host_movie.vw;dst[11]=host_movie.vh;}return 12;}
#endif
void emu_system_shutdown(void){host_movie_release_source();host_movie.opened=host_movie.present=0;host_fc_close();if(host_log_fp){fclose(host_log_fp);host_log_fp=0;}}
u32 emu_system_movie_frames(void){return host_movie.frames;}

#if defined(EMU_TESTING) || defined(EMU_RELEASE_PROBE)
u32 emu_debug_state(void){return statep();}

u32 emu_debug_dialog_handle(void){return child.active?child.handle:0;}
void emu_debug_write8(u32 a,u32 v){mw8(a,v);}
void emu_debug_write32(u32 a,u32 v){mw32(a,v);}
u32 emu_debug_read32(u32 a){return mr32(a);}
int emu_debug_call(u32 addr,u32 r0,u32 r1,u32 r2,u32 r3){int z=call_arm(addr,r0,r1,r2,r3,1200000);if(child.active&&child.pending)deliver_child();return z;}
int emu_debug_system_boot(u32 max_steps){if(!emu_system_reset_internal())return 0;return max_steps?emu_system_run(max_steps):1;}
int emu_debug_system_run(u32 max_steps){return emu_system_run(max_steps);}
void emu_debug_touch_set(u32 x,u32 y,u32 down){emu_system_touch(x,y,down);}
void emu_debug_set_touch_autocal(u32 enabled){system_touch_autocal_enabled=enabled?1u:0u;}
u32 emu_debug_touch_down(void){return virtual_touch_down;}
u32 emu_debug_pc(void){return cpu.r[15];}
u64 emu_debug_steps(void){return cpu.steps;}
u32 emu_debug_firmware_payload_offset(void){return firmware_payload_offset;}
u32 emu_debug_reg(int index){return (index>=0&&index<16)?cpu.r[index]:0;}
void emu_debug_set_reg(int index,u32 value){if(index>=0&&index<16)cpu.r[index]=value;}
u32 emu_debug_cpsr(void){return cpu_cpsr();}
void emu_debug_set_cpsr(u32 value){cpu_set_cpsr(value);}
const char* emu_debug_semihost_log(void){return semihost_log;}
int emu_debug_trace_copy(u32*dst,int cap){int n=cap<256?cap:256;if(n<0)n=0;for(int i=0;i<n;i++)dst[i]=debug_trace[(debug_trace_pos-(u32)n+(u32)i)&255u];return n;}
u32 emu_debug_timer3_reload(void){return timer3.reload;}
u32 emu_debug_timer3_counter(void){return timer3.counter;}
u32 emu_debug_timer3_fires(void){return timer3.fires;}
u32 emu_debug_irq_entries(void){return timer3.irq_entries;}
u32 emu_debug_audio_dma_events(void){return audio_probe.dma_events;}
u32 emu_debug_audio_dma_addr(void){return audio_probe.dma_addr;}
u32 emu_debug_audio_dma_words(void){return audio_probe.dma_words;}
u32 emu_debug_audio_dma_bytes(void){return audio_probe.captured_bytes;}
u32 emu_debug_audio_dma_signature(void){return audio_probe.signature;}
u32 emu_debug_audio_command(void){return audio_probe.command;}
u32 emu_debug_audio_reg(u32 command){return ((command&0xff00u)==0x7f00u)?audio_probe.regs[command&0xffu]:0u;}
u32 emu_debug_audio_dma_active(void){return audio_probe.dma_active;}
u32 emu_debug_audio_dma_remaining_words(void){return audio_probe.dma_remaining_words;}
u32 emu_debug_audio_dma_completions(void){return audio_probe.dma_completions;}
u32 emu_debug_audio_irq_pending(void){return audio_probe.irq_pending;}
u32 emu_debug_audio_irq_acks(void){return audio_probe.irq_acks;}
u32 emu_debug_audio_irq_entries(void){return audio_probe.irq_entries;}
u32 emu_debug_audio_last_event(void){return audio_probe.last_event;}
u32 emu_debug_audio_sample_rate(void){return audio_probe.sample_rate;}
u32 emu_debug_audio_channels(void){return audio_probe.channels;}
u32 emu_debug_audio_bits_per_sample(void){return audio_probe.bits_per_sample;}
u32 emu_debug_audio_clock_valid(void){return audio_probe.clock_valid;}
void emu_debug_audio_complete(void){audio_complete_playback();}
void emu_debug_audio_advance_us(u64 elapsed_us){audio_advance_us(elapsed_us);}
int emu_debug_audio_copy(u8*dst,u32 cap){u32 n=audio_probe.captured_bytes;if(!dst)return 0;if(n>cap)n=cap;if(n)memcopy(dst,audio_probe.captured,n);return (int)n;}
u32 emu_debug_context_switches(void){return debug_context_switches;}
u32 emu_debug_vflash_reads(void){return debug_vflash_reads;}
u32 emu_debug_vflash_writes(void){return debug_vflash_writes;}
u64 emu_debug_vflash_last_off(void){return debug_vflash_last_off;}
u32 emu_debug_vflash_last_len(void){return debug_vflash_last_len;}
u32 emu_debug_vflash_overlay_pages(void){return vflash_overlay_count;}
void emu_debug_set_vflash_cache(u32 enabled){vflash_ro_cache_enabled=enabled?1u:0u;}
u32 emu_debug_vflash_cache_hits(void){return vflash_ro_cache_hits;}
u32 emu_debug_vflash_cache_misses(void){return vflash_ro_cache_misses;}
u32 emu_debug_vflash_op_count(void){return debug_vf_pos<256u?debug_vf_pos:256u;}
int emu_debug_vflash_op(u32 back,u64*off,u32*len,u32*wr){u32 have=emu_debug_vflash_op_count();if(back>=have)return 0;DebugVfOp*e=&debug_vf_ops[(debug_vf_pos-1u-back)&255u];if(off)*off=e->off;if(len)*len=e->len;if(wr)*wr=e->wr;return 1;}
u32 emu_debug_scheduler_selects(void){return debug_scheduler_selects;}
u32 emu_debug_task_select_count(int index){return (index>=0&&index<32)?debug_task_select_count[index]:0;}
u32 emu_debug_task_changes(void){return debug_task_changes;}
u64 emu_debug_idle_fast_forwarded_steps(void){return debug_idle_fast_forwarded_steps;}
u32 emu_debug_idle_fast_forward_events(void){return debug_idle_fast_forward_events;}
void emu_debug_set_render_fastpaths(u32 enabled){render_fastpaths_enabled=enabled?1u:0u;}
u64 emu_debug_render_fast_pixels(void){return debug_render_fast_pixels;}
u32 emu_debug_render_fast_events(void){return debug_render_fast_events;}
u32 emu_debug_delay1m_fast_events(void){return debug_delay1m_fast_events;}
u32 emu_debug_nand_cmd_count(void){return nand_cmd_count;}
u32 emu_debug_nand_last_cmd0(void){return nand_last_cmd0;}
u32 emu_debug_nand_last_ctrl(void){return nand_last_ctrl;}
u32 emu_debug_nand_last_col(void){return nand_last_col;}
u32 emu_debug_nand_last_row(void){return nand_last_row;}
u32 emu_debug_nand_read_commands(void){return nand_read_commands;}
u32 emu_debug_system_data_base(void){return system_data_base;}
u32 emu_debug_system_data_len(void){return system_data_len;}
u32 emu_debug_system_data_magic_ok(void){return system_data_magic_ok;}
u32 emu_debug_program_data_base(void){return program_data_base;}
u32 emu_debug_program_data_len(void){return program_data_len;}
u32 emu_debug_program_data_magic_ok(void){return program_data_magic_ok;}
u32 emu_debug_storage_hle_calls(void){return debug_storage_hle_calls;}
u32 emu_debug_ftl_bypass_calls(void){return debug_ftl_bypass_calls;}
u32 emu_debug_system_file_opens(void){return debug_system_file_opens;}
u32 emu_debug_system_file_reads(void){return debug_system_file_reads;}
u32 emu_debug_file_open_attempts(void){return debug_file_open_attempts;}
u32 emu_debug_last_file_rid(void){return debug_last_file_rid;}
void emu_debug_set_rid15_bridge(u32 enabled){debug_rid15_bridge=enabled?1u:0u;}
u32 emu_debug_res_op_count(void){return debug_res_op_pos<512u?debug_res_op_pos:512u;}
int emu_debug_res_op(u32 back,u32*out6){
 u32 have=debug_res_op_pos<512u?debug_res_op_pos:512u;if(!out6||back>=have)return 0;DebugResOp*e=&debug_res_ops[(debug_res_op_pos-1u-back)&511u];
 out6[0]=e->api;out6[1]=e->rid;out6[2]=e->mode;out6[3]=e->lr;out6[4]=e->arg2;out6[5]=e->handle;return 1;
}

u32 emu_debug_file_rid_count(u32 rid){return rid<256u?debug_file_rid_counts[rid]:0;}
u32 emu_debug_rid_open_success(u32 rid){return rid<1024u?debug_rid_open_success[rid]:0;}
u32 emu_debug_rid_open_fail(u32 rid){return rid<1024u?debug_rid_open_fail[rid]:0;}
u32 emu_debug_rid_first_handle(u32 rid){return rid<1024u?debug_rid_first_handle[rid]:0;}
u32 emu_debug_rid_first_lr(u32 rid){return rid<1024u?debug_rid_first_lr[rid]:0;}
u32 emu_debug_update_status(void){return debug_update_status;}
u32 emu_debug_update_status_hits(void){return debug_update_status_hits;}
u32 emu_debug_startup_last_milestone(void){return debug_startup_last_milestone;}
u32 emu_debug_startup_milestone_count(u32 i){return i<64u?debug_startup_milestone_counts[i]:0;}
u32 emu_debug_startup_syscheck(void){return debug_startup_syscheck;}
u32 emu_debug_touch_autocal_samples(void){return debug_touch_autocal_samples;}
u32 emu_debug_touch_autocal_coord(u32 i){return i<8u?debug_touch_autocal_xy[i]:0;}
u32 emu_debug_lcd_writes(void){return debug_lcd_writes;}
u32 emu_debug_lcd_last(void){return debug_lcd_last;}
int emu_debug_lcd_ring_copy(u32*dst,int cap){int n=cap<64?cap:64;if(n<0)n=0;u32 have=debug_lcd_ring_pos<64u?debug_lcd_ring_pos:64u;if((u32)n>have)n=(int)have;for(int i=0;i<n;i++)dst[i]=debug_lcd_ring[(debug_lcd_ring_pos-(u32)n+(u32)i)&63u];return n;}
u32 emu_debug_lcd_dma_transfers(void){return lcd.dma_transfers;}
u32 emu_debug_lcd_dma_pixels(void){return lcd.dma_pixels;}
u16 emu_debug_lcd_pixel565(u32 x,u32 y){return (x<LCD_W&&y<LCD_H)?lcd_panel565[y*LCD_W+x]:0;}
u32 emu_debug_lcd_dma_event_count(void){return debug_lcd_dma_pos<64u?debug_lcd_dma_pos:64u;}
int emu_debug_lcd_dma_event(u32 back,u32*out8){u32 have=emu_debug_lcd_dma_event_count();if(!out8||back>=have)return 0;DebugLcdDma*e=&debug_lcd_dma[(debug_lcd_dma_pos-1u-back)&63u];out8[0]=e->src;out8[1]=e->w;out8[2]=e->h;out8[3]=e->x0;out8[4]=e->y0;out8[5]=e->x1;out8[6]=e->y1;out8[7]=e->sig;return 1;}
u32 emu_debug_lcd_full_frame_count(void){return debug_lcd_full_count;}
u16 emu_debug_lcd_full_frame_pixel(u32 frame,u32 x,u32 y){return (frame<debug_lcd_full_count&&x<LCD_W&&y<LCD_H)?debug_lcd_full_frames[frame][y*LCD_W+x]:0;}
u32 emu_debug_desktop_obj(void){return debug_desktop_obj;}u32 emu_debug_desktop_hwnd(void){return debug_desktop_hwnd;}u32 emu_debug_desktop_cb(void){return debug_desktop_cb;}u32 emu_debug_desktop_msg_count(void){return debug_desktop_msg_count;}
int emu_debug_desktop_msg(u32 i,u32*out5){if(!out5||i>=debug_desktop_msg_count||i>=64u)return 0;DebugDesktopMsg*e=&debug_desktop_msgs[i];out5[0]=e->hwnd;out5[1]=e->msg;out5[2]=e->wparam;out5[3]=e->lparam;out5[4]=e->cb;return 1;}
u32 emu_debug_win_create_count(void){return debug_win_create_count<256u?debug_win_create_count:256u;}
int emu_debug_win_create(u32 i,u32*out12){if(!out12||i>=emu_debug_win_create_count())return 0;DebugWinCreate*e=&debug_win_create[i];out12[0]=e->sp;out12[1]=e->lr;out12[2]=e->a0;out12[3]=e->a1;out12[4]=e->a2;out12[5]=e->a3;out12[6]=e->s0;out12[7]=e->s1;out12[8]=e->s2;out12[9]=e->s3;out12[10]=e->s4;out12[11]=e->ret;return 1;}
u32 emu_debug_bitmap_decode_count(void){return debug_bitmap_decode_count<256u?debug_bitmap_decode_count:256u;}
int emu_debug_bitmap_decode(u32 i,u32*out5){if(!out5||i>=emu_debug_bitmap_decode_count())return 0;DebugBitmapDecode*e=&debug_bitmap_decode[i];out5[0]=e->lr;out5[1]=e->rid;out5[2]=e->index;out5[3]=e->ret;out5[4]=e->sp;return 1;}
u32 emu_debug_bitmap_header_count(void){return debug_bitmap_header_count<256u?debug_bitmap_header_count:256u;}
int emu_debug_bitmap_header(u32 i,u32*out9){if(!out9||i>=emu_debug_bitmap_header_count())return 0;DebugBitmapHeader*e=&debug_bitmap_headers[i];out9[0]=e->off;out9[1]=e->sig;out9[2]=e->pixoff;out9[3]=e->dibsz;out9[4]=e->w;out9[5]=e->h;out9[6]=e->planes_bpp;out9[7]=e->comp;out9[8]=e->imgsz;return 1;}
#endif

/* Web host: RTOS ticks are 5 ms. Resume the original ARM continuation. */
u32 web_busy(void){return web_sleep_ms;}
int web_tick(u32 ms){
 if(!web_sleep_ms){
  /* Map loads outside the movement handler need its original event pass.
     A zero key is also the firmware's own post-combat event message. */
  u32 state=statep();
  if(!web_story_pending||emu_mode()!=0||child.active||web_flight_active||
     suspended_valid||web_combat_active()||!state||mr32(state+0x1du)!=1u)return 1;
  web_story_pending=0;
  int z=call_arm(emu_profile_current()->app_wndproc,WINDOW,WM_KEYDOWN,0,0,900000);
  deliver_child();emu_paint();return z?2:0;
 }
 if(ms<web_sleep_ms){web_sleep_ms-=ms;return 1;}
 web_sleep_ms=0;int z=resume_suspended();deliver_child();emu_paint();return z!=0;
}
int web_checkpoint(void){
 if(emu_mode()!=0||child.active||web_flight_active||web_sleep_ms||!statep()||mr32(statep()+0x1du)!=1u)return 0;
 if(!call_arm(0x300c4dd8u,WINDOW,0,0,0,900000))return 0;
 if(!call_arm(0x300c4e84u,WINDOW,0,0,0,900000))return 0;
 return emu_save_size()==6304u&&!g_error;
}
int web_touch(u32 x,u32 y){
 if(web_sleep_ms||web_combat_active())return 1;
 web_notice_active=0;
 if(web_flight_active)return web_flight_touch(x,y);
 /* NPC offers have distinct accept/cancel hit areas in their ARM callback.
    A blanket Enter here turns even a tap on "later" into a transaction. */
 if(child.active)return child.modeless||child.callback==0x300cebbcu||child.callback==0x300c8d98u?child_touch(x,y):emu_key(41);
 int ok=call_arm(emu_profile_current()->app_wndproc,WINDOW,0x201u,0,(y<<16)|x,900000);
 if(ok&&!child.active&&!web_sleep_ms)ok=call_arm(emu_profile_current()->app_wndproc,WINDOW,0x202u,0,(y<<16)|x,900000);
 deliver_child();emu_paint();return ok!=0;
}
