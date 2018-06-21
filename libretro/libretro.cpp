#include "libretro.h"

#include "snes9x.h"
#include "memmap.h"
#include "srtc.h"
#include "apu/apu.h"
#include "apu/bapu/snes/snes.hpp"
#include "gfx.h"
#include "snapshot.h"
#include "controls.h"
#include "cheats.h"
#include "logger.h"
#include "display.h"
#include "conffile.h"
#include <stdio.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include "../apu/bapu/dsp/SPC_DSP.h"

#define RETRO_DEVICE_JOYPAD_MULTITAP ((2 << 8) | RETRO_DEVICE_JOYPAD)
#define RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE ((1 << 8) | RETRO_DEVICE_LIGHTGUN)
#define RETRO_DEVICE_LIGHTGUN_JUSTIFIER ((2 << 8) | RETRO_DEVICE_LIGHTGUN)
#define RETRO_DEVICE_LIGHTGUN_JUSTIFIERS ((3 << 8) | RETRO_DEVICE_LIGHTGUN)
#define RETRO_DEVICE_LIGHTGUN_MACSRIFLE ((4 << 8) | RETRO_DEVICE_LIGHTGUN)

#define RETRO_MEMORY_SNES_BSX_RAM ((1 << 8) | RETRO_MEMORY_SAVE_RAM)
#define RETRO_MEMORY_SNES_BSX_PRAM ((2 << 8) | RETRO_MEMORY_SAVE_RAM)
#define RETRO_MEMORY_SNES_SUFAMI_TURBO_A_RAM ((3 << 8) | RETRO_MEMORY_SAVE_RAM)
#define RETRO_MEMORY_SNES_SUFAMI_TURBO_B_RAM ((4 << 8) | RETRO_MEMORY_SAVE_RAM)
#define RETRO_MEMORY_SNES_GAME_BOY_RAM ((5 << 8) | RETRO_MEMORY_SAVE_RAM)
#define RETRO_MEMORY_SNES_GAME_BOY_RTC ((6 << 8) | RETRO_MEMORY_RTC)

#define RETRO_GAME_TYPE_BSX             0x101 | 0x1000
#define RETRO_GAME_TYPE_BSX_SLOTTED     0x102 | 0x1000
#define RETRO_GAME_TYPE_SUFAMI_TURBO    0x103 | 0x1000
#define RETRO_GAME_TYPE_SUPER_GAME_BOY  0x104 | 0x1000
#define RETRO_GAME_TYPE_MULTI_CART      0x105 | 0x1000


#define SNES_4_3 4.0f / 3.0f

char g_rom_dir[1024];
char g_basename[1024];
char retro_system_directory[4096];
char retro_save_directory[4096];

int hires_blend = 0;
bool overclock_cycles = false;
bool reduce_sprite_flicker = false;
bool randomize_memory = false;
int one_c, slow_one_c, two_c;
int16 macsrifle_adjust_x, macsrifle_adjust_y;

retro_log_printf_t log_cb = NULL;
static retro_video_refresh_t video_cb = NULL;
static retro_audio_sample_t audio_cb = NULL;
static retro_audio_sample_batch_t audio_batch_cb = NULL;
static retro_input_poll_t poll_cb = NULL;
static retro_input_state_t input_state_cb = NULL;

static bool lufia2_credits_hack = false;
static uint16 *gfx_blend;

static int audio_interp_max = 32768;
static int audio_interp_mode = 2;
static uint8 audio_interp_custom[0x10000];

static int blargg_filter = 0;
int dsp1_chipset = 1;
static bool interlace_speed = 0;
static int runahead_speed = 0;
static bool ignore_bios = false;
static bool special_hires_game = false;

#include "render.cpp"
#include "mudlord.cpp"

#define MAX_SNES_WIDTH_OUT SNES_NTSC_OUT_WIDTH(256)


static void extract_basename(char *buf, const char *path, size_t size)
{
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';

   char *ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char *base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
      buf[0] = '\0';
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}


static retro_environment_t environ_cb;
static unsigned crop_overscan_mode = 0;
static unsigned aspect_ratio_mode = 0;
static bool rom_loaded = false;

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   static const struct retro_subsystem_memory_info multi_a_memory[] = {
      { "srm", RETRO_MEMORY_SNES_SUFAMI_TURBO_A_RAM },
   };

   static const struct retro_subsystem_memory_info multi_b_memory[] = {
      { "srm", RETRO_MEMORY_SNES_SUFAMI_TURBO_B_RAM },
   };

   static const struct retro_subsystem_rom_info multicart_roms[] = {
      { "Cart A", "smc|sfc|swc|fig|bs", false, false, false, multi_a_memory, 1 },
      { "Add-On B", "smc|sfc|swc|fig|bs", false, false, false, multi_b_memory, 1 },
   };

   static const struct retro_subsystem_info subsystems[] = {
      { "Multi-Cart Link", "multicart_addon", multicart_roms, 2, RETRO_GAME_TYPE_MULTI_CART },
      {}
   };

   cb(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO,  (void*)subsystems);


   struct retro_variable variables[] = {
      // These variable names and possible values constitute an ABI with ZMZ (ZSNES Libretro player).
      // Changing "Show layer 1" is fine, but don't change "layer_1"/etc or the possible values ("Yes|No").
      // Adding more variables and rearranging them is safe.
      { "snes9x_up_down_allowed", "Allow opposing directions; disabled|enabled" },
      { "snes9x_blargg", "Blargg NTSC filter; disabled|monochrome|rf|composite|s-video|rgb" },
      { "snes9x_hires_blend", "Hires blending; disabled|half|full|special" },
      { "snes9x_overclock_superfx", "SuperFX overclocking; 100%|150%|200%|250%|300%|350%|400%|450%|500%|50%" },
      { "snes9x_overclock_cycles", "Reduce slowdown (Unsafe); disabled|compatible|max" },
      { "snes9x_reduce_sprite_flicker", "Reduce flickering (Unsafe); disabled|enabled" },
      { "snes9x_randomize_memory", "Randomize memory (Unsafe); disabled|enabled" },
      { "snes9x_audio_interpolation", "Audio interpolation; gaussian|cubic|sinc|4-tap (custom)|8-tap (custom)|none|linear" },
      { "snes9x_layer_1", "Show layer 1; enabled|disabled" },
      { "snes9x_layer_2", "Show layer 2; enabled|disabled" },
      { "snes9x_layer_3", "Show layer 3; enabled|disabled" },
      { "snes9x_layer_4", "Show layer 4; enabled|disabled" },
      { "snes9x_layer_5", "Show sprite layer; enabled|disabled" },
      { "snes9x_gfx_clip", "Enable graphic clip windows; enabled|disabled" },
      { "snes9x_gfx_transp", "Enable transparency effects; enabled|disabled" },
      { "snes9x_gfx_hires", "Enable hires mode; enabled|disabled" },
      { "snes9x_sndchan_1", "Enable sound channel 1; enabled|disabled" },
      { "snes9x_sndchan_2", "Enable sound channel 2; enabled|disabled" },
      { "snes9x_sndchan_3", "Enable sound channel 3; enabled|disabled" },
      { "snes9x_sndchan_4", "Enable sound channel 4; enabled|disabled" },
      { "snes9x_sndchan_5", "Enable sound channel 5; enabled|disabled" },
      { "snes9x_sndchan_6", "Enable sound channel 6; enabled|disabled" },
      { "snes9x_sndchan_7", "Enable sound channel 7; enabled|disabled" },
      { "snes9x_sndchan_8", "Enable sound channel 8; enabled|disabled" },
      { "snes9x_dsp1_chipset", "DSP-1 chipset; revision 1b|revision 1(a)" },
      { "snes9x_vram_allow", "Allow invalid VRAM access (Unsafe); disabled|enabled" },
      { "snes9x_special_hacks", "Use special game hacks; enabled|disabled" },
      { "snes9x_interlace", "Interlace speed; auto|slow|fast" },
      { "snes9x_speakers", "Audio output mode; 16-bit stereo|mute|8-bit mono|8-bit stereo|16-bit mono" },
      { "snes9x_overscan", "Crop overscan; auto|enabled|disabled" },
      { "snes9x_aspect", "Preferred aspect ratio; auto|ntsc|pal|4:3" },
      { "snes9x_region", "Console region; auto|japan, usa|europe" },
      { "snes9x_runahead", "Internal runahead; disabled|1|2|3|4|5|6|7|8|9|10" },
      { "snes9x_ignore_bios", "Ignore using BIOS; disabled|enabled" },
      { "snes9x_macsrifle_adjust_x", "M.A.C.S. Rifle - adjust aim x; 0|1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24|25|-25|-24|-23|-22|-21|-20|-19|-18|-17|-16|-15|-14|-13|-12|-11|-10|-9|-8|-7|-6|-5|-4|-3|-2|-1" },
      { "snes9x_macsrifle_adjust_y", "M.A.C.S. Rifle - adjust aim y; 0|1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24|25|-25|-24|-23|-22|-21|-20|-19|-18|-17|-16|-15|-14|-13|-12|-11|-10|-9|-8|-7|-6|-5|-4|-3|-2|-1" },
      {},
   };

   environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);

   static const struct retro_controller_description port_1[] = {
      { "None", RETRO_DEVICE_NONE },
      { "SNES Joypad", RETRO_DEVICE_JOYPAD },
      { "SNES Mouse", RETRO_DEVICE_MOUSE },
      { "Multitap", RETRO_DEVICE_JOYPAD_MULTITAP },
   };

   static const struct retro_controller_description port_2[] = {
      { "None", RETRO_DEVICE_NONE },
      { "SNES Joypad", RETRO_DEVICE_JOYPAD },
      { "SNES Mouse", RETRO_DEVICE_MOUSE },
      { "Multitap", RETRO_DEVICE_JOYPAD_MULTITAP },
      { "SuperScope", RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE },
      { "Justifier", RETRO_DEVICE_LIGHTGUN_JUSTIFIER },
      { "M.A.C.S. Rifle", RETRO_DEVICE_LIGHTGUN_MACSRIFLE },
   };

   static const struct retro_controller_info ports[] = {
      { port_1, 4 },
      { port_2, 7 },
      {},
   };

   environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

void update_geometry(void)
{
   struct retro_system_av_info av_info;
   retro_get_system_av_info(&av_info);
   environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
}

static void update_variables(void)
{
   bool geometry_update = false;
   char key[256];
   struct retro_variable var;

   var.key = "snes9x_hires_blend";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
	 {
      if (strcmp(var.value, "disabled") == 0)
				hires_blend = 0;
      else if (strcmp(var.value, "half") == 0)
				hires_blend = 1;
      else if (strcmp(var.value, "full") == 0)
				hires_blend = 2;
      else if (strcmp(var.value, "special") == 0)
				hires_blend = 3;
	 }
   else
      hires_blend = false;

   var.key = "snes9x_overclock_superfx";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      int newval;
      if(sscanf(var.value,"%d%%",&newval))
         Settings.SuperFXClockMultiplier = newval;
   }

   var.key = "snes9x_up_down_allowed";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      Settings.UpAndDown = !strcmp(var.value, "disabled") ? false : true;
   }
   else
      Settings.UpAndDown = false;

   var.key = "snes9x_overclock_cycles";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "compatible") == 0)
      {
         overclock_cycles = true;
         one_c = 4;
         slow_one_c = 5;
         two_c = 6;
      }
      else if (strcmp(var.value, "max") == 0)
      {
         overclock_cycles = true;
         one_c = 3;
         slow_one_c = 3;
         two_c = 3;
      }
      else
         overclock_cycles = false;
   }

   var.key = "snes9x_reduce_sprite_flicker";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         reduce_sprite_flicker = true;
      else
         reduce_sprite_flicker = false;
   }

   var.key = "snes9x_randomize_memory";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         randomize_memory = true;
      else
         randomize_memory = false;
   }

   var.key = "snes9x_audio_interpolation";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int oldval = audio_interp_mode;

      if (strcmp(var.value, "none") == 0)
         audio_interp_mode = 0;
      else if (strcmp(var.value, "linear") == 0)
         audio_interp_mode = 1;
      else if (strcmp(var.value, "gaussian") == 0)
         audio_interp_mode = 2;
      else if (strcmp(var.value, "cubic") == 0)
         audio_interp_mode = 3;
      else if (strcmp(var.value, "sinc") == 0)
         audio_interp_mode = 4;
      else if (strcmp(var.value, "4-tap (custom)") == 0)
      {
         char name[PATH_MAX + 1];
         sprintf(name,"%s/snes_custom_4tap.bin",retro_system_directory);
         FILE *fp = fopen(name,"rb");
         if(fp)
         {
            if(fread(audio_interp_custom,1,0x8000,fp))
            {
               audio_interp_mode = 5;
               audio_interp_max = 32768;
            }
         }
         fclose(fp);
      }
      else if (strcmp(var.value, "8-tap (custom)") == 0)
      {
         char name[PATH_MAX + 1];
         sprintf(name,"%s/snes_custom_8tap.bin",retro_system_directory);
         FILE *fp = fopen(name,"rb");
         if(fp)
         {
            if(fread(audio_interp_custom,1,0x10000,fp))
            {
               audio_interp_mode = 6;
               audio_interp_max = 32768;
            }
            fclose(fp);
         }
      }

			if (oldval != audio_interp_mode)
         audio_interp_max = 32768;
   }

	 int disabled_channels=0;
   strcpy(key, "snes9x_sndchan_x");
   var.key=key;
   for (int i=0;i<8;i++)
   {
      key[strlen("snes9x_sndchan_")]='1'+i;
      var.value=NULL;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && !strcmp("disabled", var.value))
         disabled_channels|=1<<i;
   }
   S9xSetSoundControl(disabled_channels^0xFF);

   int disabled_layers=0;
   strcpy(key, "snes9x_layer_x");
   for (int i=0;i<5;i++)
   {
      key[strlen("snes9x_layer_")]='1'+i;
      var.value=NULL;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && !strcmp("disabled", var.value))
         disabled_layers|=1<<i;
   }
   Settings.BG_Forced=disabled_layers;

   //for some reason, Transparency seems to control both the fixed color and the windowing registers?
   var.key="snes9x_gfx_clip";
   var.value=NULL;
   Settings.DisableGraphicWindows=(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && !strcmp("disabled", var.value));

   var.key="snes9x_gfx_transp";
   var.value=NULL;
   Settings.Transparency=!(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && !strcmp("disabled", var.value));

   var.key="snes9x_gfx_hires";
   var.value=NULL;
   Settings.SupportHiRes=!(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && !strcmp("disabled", var.value));

   var.key = "snes9x_overscan";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      unsigned newval = 0;
      if (strcmp(var.value, "enabled") == 0)
         newval = 1;
      else if (strcmp(var.value, "disabled") == 0)
         newval = 2;

      if (newval != crop_overscan_mode)
      {
         crop_overscan_mode = newval;
         geometry_update = true;
      }
   }

   var.key = "snes9x_aspect";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      unsigned newval = 0;
      if (strcmp(var.value, "ntsc") == 0)
         newval = 1;
      else if (strcmp(var.value, "pal") == 0)
         newval = 2;
      else if (strcmp(var.value, "4:3") == 0)
         newval = 3;

      if (newval != aspect_ratio_mode)
      {
         aspect_ratio_mode = newval;
         geometry_update = true;
      }
   }

   var.key="snes9x_macsrifle_adjust_x";
   var.value=NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      macsrifle_adjust_x = atoi(var.value);

   var.key="snes9x_macsrifle_adjust_y";
   var.value=NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      macsrifle_adjust_y = atoi(var.value);

   var.key = "snes9x_region";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "auto") == 0)
			{
				 Settings.ForceNTSC = false;
				 Settings.ForcePAL = false;
			}
      else if (strcmp(var.value, "japan, usa") == 0)
			{
				 Settings.ForceNTSC = true;
				 Settings.ForcePAL = false;
			}
      else if (strcmp(var.value, "europe") == 0)
			{
				 Settings.ForceNTSC = false;
				 Settings.ForcePAL = true;
			}
   }

   var.key = "snes9x_speakers";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "16-bit stereo") == 0)
			{
				 Settings.SixteenBitSound = true;
				 Settings.Stereo = true;
				 Settings.Mute = false;
			}
      else if (strcmp(var.value, "16-bit mono") == 0)
			{
				 Settings.SixteenBitSound = true;
				 Settings.Stereo = false;
				 Settings.Mute = false;
			}
      if (strcmp(var.value, "8-bit stereo") == 0)
			{
				 Settings.SixteenBitSound = false;
				 Settings.Stereo = true;
				 Settings.Mute = false;
			}
      else if (strcmp(var.value, "8-bit mono") == 0)
			{
				 Settings.SixteenBitSound = false;
				 Settings.Stereo = false;
				 Settings.Mute = false;
			}
      else if (strcmp(var.value, "mute") == 0)
			{
				 Settings.SixteenBitSound = false;
				 Settings.Stereo = false;
				 Settings.Mute = true;
			}
   }

   var.key = "snes9x_vram_allow";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
				 Settings.BlockInvalidVRAMAccess = true;
      else if (strcmp(var.value, "enabled") == 0)
				 Settings.BlockInvalidVRAMAccess = false;
   }

   var.key="snes9x_blargg";
   var.value=NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	 {
      if (strcmp(var.value, "disabled") == 0)
         blargg_filter = 0;
      else if (strcmp(var.value, "monochrome") == 0)
         blargg_filter = 1;
      else if (strcmp(var.value, "rf") == 0)
         blargg_filter = 2;
      else if (strcmp(var.value, "composite") == 0)
         blargg_filter = 3;
      else if (strcmp(var.value, "s-video") == 0)
         blargg_filter = 4;
      else if (strcmp(var.value, "rgb") == 0)
         blargg_filter = 5;
	 }

   var.key="snes9x_special_hacks";
   var.value=NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	 {
      if (strcmp(var.value, "enabled") == 0)
         Settings.DisableGameSpecificHacks = false;
      else if (strcmp(var.value, "disabled") == 0)
         Settings.DisableGameSpecificHacks = true;
	 }

   var.key="snes9x_dsp1_chipset";
   var.value=NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	 {
      if (strcmp(var.value, "revision 1b") == 0)
         dsp1_chipset = 1;
      else if (strcmp(var.value, "revision 1(a)") == 0)
         dsp1_chipset = 0;
	 }

   var.key = "snes9x_interlace";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "auto") == 0)
				 interlace_speed = 0;
      else if (strcmp(var.value, "slow") == 0)
				 interlace_speed = 1;
      else if (strcmp(var.value, "fast") == 0)
				 interlace_speed = 2;
   }

   var.key = "snes9x_runahead";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
				 runahead_speed = 0;
      else if (strcmp(var.value, "1") == 0)
				 runahead_speed = 1;
      else if (strcmp(var.value, "2") == 0)
				 runahead_speed = 2;
      else if (strcmp(var.value, "3") == 0)
				 runahead_speed = 3;
      else if (strcmp(var.value, "4") == 0)
				 runahead_speed = 4;
      else if (strcmp(var.value, "5") == 0)
				 runahead_speed = 5;
      else if (strcmp(var.value, "6") == 0)
				 runahead_speed = 6;
      else if (strcmp(var.value, "7") == 0)
				 runahead_speed = 7;
      else if (strcmp(var.value, "8") == 0)
				 runahead_speed = 8;
      else if (strcmp(var.value, "9") == 0)
				 runahead_speed = 9;
      else if (strcmp(var.value, "10") == 0)
				 runahead_speed = 10;
   }

   var.key="snes9x_ignore_bios";
   var.value=NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	 {
      if (strcmp(var.value, "disabled") == 0)
         ignore_bios = false;
      else if (strcmp(var.value, "enabled") == 0)
         ignore_bios = true;
	 }

   if (geometry_update)
      update_geometry();
}

static void S9xAudioCallback(void*)
{
   const int BUFFER_SIZE = 256;
   // This is called every time 128 to 132 samples are generated, which happens about 8 times per frame.  A buffer size of 256 samples is enough here.
   static int16_t audio_buf[BUFFER_SIZE];

   S9xFinalizeSamples();
   size_t avail = S9xGetSampleCount();
   while (avail >= BUFFER_SIZE)
   {
      //this loop will never be entered, but handle oversized sample counts just in case
      S9xMixSamples((uint8*)audio_buf, BUFFER_SIZE);
      audio_batch_cb(audio_buf, BUFFER_SIZE >> 1);

      avail -= BUFFER_SIZE;
   }
   if (avail > 0)
   {
      S9xMixSamples((uint8*)audio_buf, avail);
      audio_batch_cb(audio_buf, avail >> 1);
   }
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info,0,sizeof(retro_system_info));

   info->library_name = "Snes9x Bright";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version = VERSION GIT_VERSION;
   info->valid_extensions = "smc|sfc|swc|fig|bs";
   info->need_fullpath = false;
   info->block_extract = false;
}

float get_aspect_ratio(unsigned width, unsigned height)
{
   if (aspect_ratio_mode == 3) // 4:3
   {
     return SNES_4_3;
   }

   float sample_frequency_ntsc = 135000000.0 / 11.0;
   float sample_frequency_pal = 14750000.0;

   float sample_freq = retro_get_region() == RETRO_REGION_NTSC ? sample_frequency_ntsc : sample_frequency_pal;
   float dot_rate = SNES::cpu.frequency / 4.0;

   if (aspect_ratio_mode == 1) // ntsc
   {
      sample_freq = sample_frequency_ntsc;
      dot_rate = NTSC_MASTER_CLOCK / 4.0;
   }
   else if (aspect_ratio_mode == 2) // pal
   {
      sample_freq = sample_frequency_pal;
      dot_rate = PAL_MASTER_CLOCK / 4.0;
   }

   float par = sample_freq / 2.0 / dot_rate;
   return (float)width * par / (float)height;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info,0,sizeof(retro_system_av_info));
   unsigned width = SNES_WIDTH;
   unsigned height = PPU.ScreenHeight;
   if (crop_overscan_mode == 1) // enabled
      height = SNES_HEIGHT;
   else if (crop_overscan_mode == 2) // disabled
      height = SNES_HEIGHT_EXTENDED;

   info->geometry.base_width = width;
   info->geometry.base_height = height;
	 info->geometry.max_width = MAX_SNES_WIDTH_OUT;
   info->geometry.max_height = MAX_SNES_HEIGHT;
   info->geometry.aspect_ratio = get_aspect_ratio(width, height);
   info->timing.sample_rate = 32040;
   info->timing.fps = retro_get_region() == RETRO_REGION_NTSC ? 21477272.0 / 357366.0 : 21281370.0 / 425568.0;
}

unsigned retro_api_version()
{
   return RETRO_API_VERSION;
}

void retro_reset()
{
   S9xSoftReset();
}

static unsigned snes_devices[2];
void retro_set_controller_port_device(unsigned port, unsigned device)
{
   if (port < 2)
   {
      int offset = snes_devices[0] == RETRO_DEVICE_JOYPAD_MULTITAP ? 4 : 1;
      switch (device)
      {
         case RETRO_DEVICE_NONE:
            S9xSetController(port, CTL_NONE, port, 0, 0, 0);
            snes_devices[port] = RETRO_DEVICE_NONE;
            break;
         case RETRO_DEVICE_JOYPAD:
            S9xSetController(port, CTL_JOYPAD, port * offset, 0, 0, 0);
            snes_devices[port] = RETRO_DEVICE_JOYPAD;
            break;
         case RETRO_DEVICE_JOYPAD_MULTITAP:
            S9xSetController(port, CTL_MP5, port * offset, port * offset + 1, port * offset + 2, port * offset + 3);
            snes_devices[port] = RETRO_DEVICE_JOYPAD_MULTITAP;
            break;
         case RETRO_DEVICE_MOUSE:
            S9xSetController(port, CTL_MOUSE, port, 0, 0, 0);
            snes_devices[port] = RETRO_DEVICE_MOUSE;
            break;
         case RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE:
            S9xSetController(port, CTL_SUPERSCOPE, 0, 0, 0, 0);
            snes_devices[port] = RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE;
            break;
         case RETRO_DEVICE_LIGHTGUN_JUSTIFIER:
            S9xSetController(port, CTL_JUSTIFIER, 0, 0, 0, 0);
            snes_devices[port] = RETRO_DEVICE_LIGHTGUN_JUSTIFIER;
            break;
         case RETRO_DEVICE_LIGHTGUN_MACSRIFLE:
            S9xSetController(port, CTL_MACSRIFLE, 0, 0, 0, 0);
            snes_devices[port] = RETRO_DEVICE_LIGHTGUN_MACSRIFLE;
            break;
         default:
            if (log_cb)
               log_cb(RETRO_LOG_ERROR, "[libretro]: Invalid device.\n");
      }
      if (!port)
         retro_set_controller_port_device(1, snes_devices[1]);
   }
   else if(device != RETRO_DEVICE_NONE)
      log_cb(RETRO_LOG_INFO, "[libretro]: Nonexistent Port (%d).\n", port);
}

void retro_cheat_reset()
{
   S9xDeleteCheats();
   S9xApplyCheats();
}

void retro_cheat_set(unsigned index, bool enabled, const char *codeline)
{
   uint32 address;
   uint8 val;

   bool8 sram;
   uint8 bytes[3];//used only by GoldFinger, ignored for now
   char codeCopy[256];
   char* code;

   if (codeline == (char *)'\0') return;

   strcpy(codeCopy,codeline);
   code=strtok(codeCopy,"+,.; ");
   while (code != NULL) {
      //Convert GH RAW to PAR
      if (strlen(code)==9 && code[6]==':')
      {
         code[6]=code[7];
         code[7]=code[8];
         code[8]='\0';
      }

      if (S9xGameGenieToRaw(code, address, val)==NULL ||
          S9xProActionReplayToRaw(code, address, val)==NULL)
      {
         Cheat.c[Cheat.num_cheats].address = address;
         Cheat.c[Cheat.num_cheats].byte = val;
         Cheat.c[Cheat.num_cheats].enabled = enabled;
         Cheat.c[Cheat.num_cheats].saved = FALSE; // it'll be saved next time cheats run anyways
         Cheat.num_cheats++;
      }
      else if (S9xGoldFingerToRaw(code, address, sram, val, bytes)==NULL)
      {
         if (!sram)
         {
            for (int i=0;i<val;i++){
               Cheat.c[Cheat.num_cheats].address = address;
               Cheat.c[Cheat.num_cheats].byte = bytes[i];
               Cheat.c[Cheat.num_cheats].enabled = enabled;
               Cheat.c[Cheat.num_cheats].saved = FALSE; // it'll be saved next time cheats run anyways
               Cheat.num_cheats++;
            }
         }
      }
      else
      {
         printf("CHEAT: Failed to recognize %s\n",code);
      }
      code=strtok(NULL,"+,.; "); // bad code, ignore
   }
   Settings.ApplyCheats=true;
   S9xApplyCheats();
}

#define MAX_MAPS 32
static struct retro_memory_descriptor memorydesc[MAX_MAPS];
static unsigned memorydesc_c;

static bool merge_mapping()
{
   if (memorydesc_c==1) return false;//can't merge the only one
   struct retro_memory_descriptor * a=&memorydesc[MAX_MAPS - (memorydesc_c-1)];
   struct retro_memory_descriptor * b=&memorydesc[MAX_MAPS - memorydesc_c];
//printf("test %x/%x\n",a->start,b->start);
   if (a->flags != b->flags) return false;
   if (a->disconnect != b->disconnect) return false;
   if (a->len != b->len) return false;
   if (a->addrspace || b->addrspace) return false;//we don't use these
   if (((char*)a->ptr)+a->offset==((char*)b->ptr)+b->offset && a->select==b->select)
   {
//printf("merge/mirror\n");
      a->select&=~(a->start^b->start);
      memorydesc_c--;
      return true;
   }
   uint32 len=a->len;
   if (!len) len=(0x1000000 - a->select);
   if (len && ((len-1) & (len | a->disconnect))==0 && ((char*)a->ptr)+a->offset+len == ((char*)b->ptr)+b->offset)
   {
//printf("merge/consec\n");
      a->select &=~ len;
      a->disconnect &=~ len;
      memorydesc_c--;
      return true;
   }
//printf("nomerge\n");
   return false;
}

void S9xAppendMapping(struct retro_memory_descriptor *desc)
{
   //do it backwards - snes9x defines the last one to win, while we define the first one to win
   //printf("add %x\n",desc->start);
   memcpy(&memorydesc[MAX_MAPS - (++memorydesc_c)], desc, sizeof(struct retro_memory_descriptor));
   while (merge_mapping()) {}
}

static void init_descriptors(void)
{
   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "X" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Y" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "X" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Y" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Select" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },

      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "A" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "X" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Y" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Select" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },

      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "A" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "X" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Y" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Select" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },

      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "A" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "X" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Y" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Select" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },

      { 0, 0, 0, 0, "" },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

static bool ChronoTriggerFrameHack;

static bool valid_normal_bank (uint8 bankbyte)
{
   switch (bankbyte)
   {
      case 32: case 33: case 48: case 49:
         return (true);
   }

   return (false);
}

static int is_bsx (uint8 *p)
{
   if ((p[26] == 0x33 || p[26] == 0xFF) && (!p[21] || (p[21] & 131) == 128) && valid_normal_bank(p[24]))
   {
      unsigned char	m = p[22];

      if (!m && !p[23])
         return (2);

      if ((m == 0xFF && p[23] == 0xFF) || (!(m & 0xF) && ((m >> 4) - 1 < 12)))
         return (1);
   }

   return (0);
}

static bool8 is_SufamiTurbo_Cart(const uint8 *data, size_t size)
{
   if (size >= 0x80000 && size <= 0x100000 &&
       strncmp((const char*)data, "BANDAI SFC-ADX", 14) == 0 && strncmp((const char*)(data + 0x10), "SFC-ADX BACKUP", 14) != 0)
      return (TRUE);

   return (FALSE);
}

static void Remove_Header(uint8_t *&romptr, size_t &romsize, bool multicart_sufami)
{
   if (romptr==0 || romsize==0) return;

   uint32 calc_size = (romsize / 0x2000) * 0x2000;
   if ((romsize - calc_size == 512 && !Settings.ForceNoHeader) || Settings.ForceHeader)
   {
      romptr += 512;
      romsize -= 512;

      if(log_cb) log_cb(RETRO_LOG_INFO,"[libretro]: ROM header removed\n");
   }

   if (multicart_sufami)
   {
      if (strncmp((const char*)(romptr+0x100000), "BANDAI SFC-ADX", 14) == 0 &&
          strncmp((const char*)(romptr+0x000000), "BANDAI SFC-ADX", 14) == 0)
      {
         romptr += 0x100000;
         romsize -= 0x100000;

         if(log_cb) log_cb(RETRO_LOG_INFO,"[libretro]: Sufami Turbo Multi-ROM bios removed\n");
      }
   }
}

static bool LoadBIOS(uint8 *biosrom, const char *biosname, size_t biossize)
{
   FILE	*fp;
   char	name[PATH_MAX + 1];
   bool8 r = false;

   strcpy(name, S9xGetDirectory(ROMFILENAME_DIR));
   strcat(name, SLASH_STR);
   strcat(name, biosname);

   fp = fopen(name, "rb");
   if (!fp)
   {
      strcpy(name, S9xGetDirectory(BIOS_DIR));
      strcat(name, SLASH_STR);
      strcat(name, biosname);

      fp = fopen(name, "rb");
   }

   if (fp)
   {
      size_t size;

      fseek(fp,0,SEEK_END);
      size = ftell(fp);

      if (size == biossize+0x200)
        fseek(fp,0x200,SEEK_SET);
      else
        fseek(fp,0,SEEK_SET);

      size = fread((void *) biosrom, 1, biossize, fp);
      fclose(fp);

      if(size == biossize) r = true;
   }


   if(log_cb)
   {
      if(!r)
        log_cb(RETRO_LOG_INFO, "[libretro]: BIOS not found [%s]\n", biosname);

      else
        log_cb(RETRO_LOG_INFO, "[libretro]: BIOS loaded [%s]\n", biosname);
   }
   return (r);
}

void retro_load_init_reset()
{
   struct retro_memory_map map={ memorydesc+MAX_MAPS-memorydesc_c, memorydesc_c };
   if (rom_loaded) environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);

   int pixel_format = RGB555;
   if(environ_cb) {
      pixel_format = RGB565;
      enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
      if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
         pixel_format = RGB555;
   }
   S9xGraphicsDeinit();
   S9xSetRenderPixelFormat(pixel_format);
   S9xGraphicsInit();

   /* Check if Chrono Trigger is loaded, if so, we need to set a variable to
    * true to get rid of an annoying mid-frame resolution switch to 256x239
    * which can cause an undesirable flicker/breakup of the screen for a
    * split second - this happens whenever the game switches from normal
    * mode to battle mode and vice versa. */
   ChronoTriggerFrameHack = false;
   if (Memory.match_nc("CHRONO TRIGGER") || /* Chrono Trigger */
       Memory.match_id("ACT") ||
       Memory.match_id("AC9J")       /* Chrono Trigger (Sample) */
      )
   {
      ChronoTriggerFrameHack = true;
   }

   lufia2_credits_hack = false;
   if (Memory.match_id("ANIE") ||
		   Memory.match_id("ANIP") ||
			 Memory.match_id("ANID") ||
			 Memory.match_id("ANIH") ||
       Memory.match_id("ANIS") ||
       Memory.match_id("ANIJ")
      )
   {
      lufia2_credits_hack = true;
   }

	 special_hires_game = false;
   if (Memory.match_id("AFJE") ||		// Kirby's Dream Land 3
			 Memory.match_id("AFJJ") ||		// Hoshi no Kirby 3
       Memory.match_nn("JURASSIC PARK") ||
       Memory.match_nn("SFC SAILORMOON S")		// Bishoujo Senshi Sailor Moon S - Kondo wa Puzzle de Oshioki yo!
      )
   {
      special_hires_game = true;
   }

	 // re-apply settings
	 update_variables();
   update_geometry();
   
   if(randomize_memory)
   {
      srand(time(NULL));
      for(int lcv=0; lcv<0x20000; lcv++)
         Memory.RAM[lcv] = rand()%256;
   }

   audio_interp_max = 32768;
}

bool retro_load_game(const struct retro_game_info *game)
{
   init_descriptors();
   memorydesc_c = 0;
   rom_loaded = false;

   update_variables();

   if(game->data == NULL && game->size == 0 && game->path != NULL)
      rom_loaded = Memory.LoadROM(game->path);
   else
   {
      uint8_t *romptr = (uint8_t *) game->data;
      size_t romsize = game->size;

      Remove_Header(romptr, romsize, false);

      if (game->path != NULL)
      {
         extract_basename(g_basename, game->path, sizeof(g_basename));
         extract_directory(g_rom_dir, game->path, sizeof(g_rom_dir));
      }

      if (!ignore_bios && is_SufamiTurbo_Cart(romptr,romsize)) {
          uint8 *biosrom = new uint8[0x40000];
          uint8 *biosptr = biosrom;

         if (LoadBIOS(biosptr,"STBIOS.bin",0x40000)) {
            if (log_cb) log_cb(RETRO_LOG_INFO, "[libretro]: Loading Sufami Turbo game\n");

            rom_loaded = Memory.LoadMultiCartMem((const uint8_t*)romptr, romsize, 0, 0, biosptr, 0x40000);
         }

				 if (!rom_loaded)
					 rom_loaded = Memory.LoadROMMem((const uint8_t*)romptr, romsize);

				 if(biosrom) delete[] biosrom;
      }

      else
      if (!ignore_bios && ((is_bsx((uint8 *) romptr+0x7fc0)==1) | (is_bsx((uint8 *) romptr+0xffc0)==1))) {
          uint8 *biosrom = new uint8[0x100000];
          uint8 *biosptr = biosrom;

         if (LoadBIOS(biosptr,"BS-X.bin",0x100000)) {
            if (log_cb) log_cb(RETRO_LOG_INFO, "[libretro]: Loading Satellaview game\n");

            rom_loaded = Memory.LoadMultiCartMem(biosptr, 0x100000, (const uint8_t*)romptr, romsize, 0, 0);
         }

				 if (!rom_loaded)
					 rom_loaded = Memory.LoadROMMem((const uint8_t*)romptr, romsize);

         if(biosrom) delete[] biosrom;
      }

      else
         rom_loaded = Memory.LoadROMMem((const uint8_t*)romptr, romsize);
   }

   if (rom_loaded)
      retro_load_init_reset();
   
   else if (log_cb)
      log_cb(RETRO_LOG_ERROR, "[libretro]: Rom loading failed...\n");

   return rom_loaded;
}

void retro_unload_game(void)
{}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
   uint8_t *romptr[3];
   size_t romsize[3];

   for(size_t lcv=0; lcv<num_info; lcv++)
   {
      romptr[lcv] = (uint8_t *) info[lcv].data;
      romsize[lcv] = info[lcv].size;

      Remove_Header(romptr[lcv], romsize[lcv], true);
   }

   init_descriptors();
   memorydesc_c = 0;
   rom_loaded = false;

   update_variables();

   switch (game_type) {
      case RETRO_GAME_TYPE_BSX:
         if(num_info == 1) {
            rom_loaded = Memory.LoadROMMem((const uint8_t*)romptr[0],romsize[0]);
         } else if(num_info == 2) {
            memcpy(Memory.BIOSROM,(const uint8_t*)romptr[0],info[0].size);
            rom_loaded = Memory.LoadROMMem((const uint8_t*)romptr[1],info[1].size);
         }

         if (!rom_loaded && log_cb)
            log_cb(RETRO_LOG_ERROR, "[libretro]: BSX ROM loading failed...\n");

         break;

      case RETRO_GAME_TYPE_BSX_SLOTTED:
      case RETRO_GAME_TYPE_MULTI_CART:
         if(num_info == 2) {
            if (!ignore_bios && is_SufamiTurbo_Cart((const uint8_t*)romptr[0], romsize[0])) {
                uint8 *biosrom = new uint8[0x40000];
                uint8 *biosptr = biosrom;

               if (LoadBIOS(biosptr,"STBIOS.bin",0x40000)) {
                  if (log_cb) log_cb(RETRO_LOG_INFO, "[libretro]: Loading Sufami Turbo link game\n");

                  rom_loaded = Memory.LoadMultiCartMem((const uint8_t*)romptr[0], romsize[0],
                               (const uint8_t*)romptr[1], romsize[1], biosptr, 0x40000);
               }

							 if (!rom_loaded)
                  rom_loaded = Memory.LoadMultiCartMem((const uint8_t*)romptr[0], romsize[0],
                               (const uint8_t*)romptr[1], romsize[1], NULL, 0);

               if (biosrom) delete[] biosrom;
            }

            else {
               if (log_cb) log_cb(RETRO_LOG_INFO, "[libretro]: Loading Multi-Cart link game\n");

               rom_loaded = Memory.LoadMultiCartMem((const uint8_t*)romptr[0], romsize[0],
                            (const uint8_t*)romptr[1], romsize[1], NULL, 0);
            }
         }

         if (!rom_loaded && log_cb)
            log_cb(RETRO_LOG_ERROR, "[libretro]: Multirom loading failed...\n");
        
         break;

      case RETRO_GAME_TYPE_SUFAMI_TURBO:
         if(num_info == 2) {
            uint8 *biosrom = new uint8[0x100000];

            if ((rom_loaded = LoadBIOS(biosrom,"STBIOS.bin",0x100000)))
               rom_loaded = Memory.LoadMultiCartMem((const uint8_t*)romptr[0], romsize[0],
                            (const uint8_t*)romptr[1], romsize[1], biosrom, 0x40000);

            if (biosrom) delete[] biosrom;
         }

         if (!rom_loaded && log_cb)
            log_cb(RETRO_LOG_ERROR, "[libretro]: Sufami Turbo ROM loading failed...\n");
        
         break;

      default:
         rom_loaded = false;
         log_cb(RETRO_LOG_ERROR, "[libretro]: Multi-cart ROM loading failed...\n");
         break;
   }

   if (rom_loaded) retro_load_init_reset();

   return rom_loaded;
}

static void map_buttons();

static void check_system_specs(void)
{
   /* TODO - might have to variably set performance level based on SuperFX/SA-1/etc */
   unsigned level = 12;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

void retro_init(void)
{
   struct retro_log_callback log;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   const char *dir = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
      snprintf(retro_system_directory, sizeof(retro_system_directory), "%s", dir);
   else
      snprintf(retro_system_directory, sizeof(retro_system_directory), "%s", ".");

   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
      snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", dir);
   else
      snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", ".");

   // State that SNES9X supports achievements.
   bool achievements = true;
   environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);

   memset(&Settings, 0, sizeof(Settings));
   Settings.SuperFXClockMultiplier = 100;
   Settings.MouseMaster = TRUE;
   Settings.SuperScopeMaster = TRUE;
   Settings.JustifierMaster = TRUE;
   Settings.MultiPlayer5Master = TRUE;
   Settings.MacsRifleMaster = TRUE;
   Settings.FrameTimePAL = 20000;
   Settings.FrameTimeNTSC = 16667;
   Settings.SixteenBitSound = TRUE;
   Settings.Stereo = TRUE;
   Settings.SoundPlaybackRate = 32040;
   Settings.SoundInputRate = 32040;
   Settings.SupportHiRes = TRUE;
   Settings.Transparency = TRUE;
   Settings.AutoDisplayMessages = TRUE;
   Settings.InitialInfoStringTimeout = 120;
   Settings.HDMATimingHack = 100;
   Settings.BlockInvalidVRAMAccessMaster = TRUE;
   Settings.CartAName[0] = 0;
   Settings.CartBName[0] = 0;
   Settings.AutoSaveDelay = 1;
   Settings.DontSaveOopsSnapshot = TRUE;

   CPU.Flags = 0;

   if (!Memory.Init() || !S9xInitAPU())
   {
      Memory.Deinit();
      S9xDeinitAPU();

      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "[libretro]: Failed to init Memory or APU.\n");
      exit(1);
   }

   //very slow devices will still pop

   //this needs to be applied to all snes9x cores

   //increasing the buffer size does not cause extra lag(tested with 1000ms buffer)
   //bool8 S9xInitSound (int buffer_ms, int lag_ms)

   S9xInitSound(32, 0); //give it a 1.9 frame long buffer, or 1026 samples.  The audio buffer is flushed every 128-132 samples anyway, so it won't get anywhere near there.

   S9xSetSoundMute(FALSE);
   S9xSetSamplesAvailableCallback(S9xAudioCallback, NULL);

   GFX.Pitch = MAX_SNES_WIDTH_OUT * sizeof(uint16);
   GFX.Screen = (uint16*) calloc(1, GFX.Pitch * MAX_SNES_HEIGHT);
   gfx_blend = (uint16*) calloc(1, GFX.Pitch * MAX_SNES_HEIGHT);
   S9xGraphicsInit();

   S9xInitInputDevices();
   for (int i = 0; i < 2; i++)
   {
      S9xSetController(i, CTL_JOYPAD, i, 0, 0, 0);
      snes_devices[i] = RETRO_DEVICE_JOYPAD;
   }

   S9xUnmapAllControls();
   map_buttons();
   check_system_specs();
}

#define MAP_BUTTON(id, name) S9xMapButton((id), S9xGetCommandT((name)), false)
#define MAKE_BUTTON(pad, btn) (((pad)<<4)|(btn))

#define PAD_1 1
#define PAD_2 2
#define PAD_3 3
#define PAD_4 4
#define PAD_5 5

#define BTN_B RETRO_DEVICE_ID_JOYPAD_B
#define BTN_Y RETRO_DEVICE_ID_JOYPAD_Y
#define BTN_SELECT RETRO_DEVICE_ID_JOYPAD_SELECT
#define BTN_START RETRO_DEVICE_ID_JOYPAD_START
#define BTN_UP RETRO_DEVICE_ID_JOYPAD_UP
#define BTN_DOWN RETRO_DEVICE_ID_JOYPAD_DOWN
#define BTN_LEFT RETRO_DEVICE_ID_JOYPAD_LEFT
#define BTN_RIGHT RETRO_DEVICE_ID_JOYPAD_RIGHT
#define BTN_A RETRO_DEVICE_ID_JOYPAD_A
#define BTN_X RETRO_DEVICE_ID_JOYPAD_X
#define BTN_L RETRO_DEVICE_ID_JOYPAD_L
#define BTN_R RETRO_DEVICE_ID_JOYPAD_R
#define BTN_FIRST BTN_B
#define BTN_LAST BTN_R

#define MOUSE_X RETRO_DEVICE_ID_MOUSE_X
#define MOUSE_Y RETRO_DEVICE_ID_MOUSE_Y
#define MOUSE_LEFT RETRO_DEVICE_ID_MOUSE_LEFT
#define MOUSE_RIGHT RETRO_DEVICE_ID_MOUSE_RIGHT
#define MOUSE_FIRST MOUSE_X
#define MOUSE_LAST MOUSE_RIGHT

#define SCOPE_X RETRO_DEVICE_ID_SUPER_SCOPE_X
#define SCOPE_Y RETRO_DEVICE_ID_SUPER_SCOPE_Y
#define SCOPE_TRIGGER RETRO_DEVICE_ID_LIGHTGUN_TRIGGER
#define SCOPE_CURSOR RETRO_DEVICE_ID_LIGHTGUN_CURSOR
#define SCOPE_TURBO RETRO_DEVICE_ID_LIGHTGUN_TURBO
#define SCOPE_PAUSE RETRO_DEVICE_ID_LIGHTGUN_PAUSE
#define SCOPE_FIRST SCOPE_X
#define SCOPE_LAST SCOPE_PAUSE

#define JUSTIFIER_X RETRO_DEVICE_ID_JUSTIFIER_X
#define JUSTIFIER_Y RETRO_DEVICE_ID_JUSTIFIER_Y
#define JUSTIFIER_TRIGGER RETRO_DEVICE_ID_LIGHTGUN_TRIGGER
#define JUSTIFIER_OFFSCREEN RETRO_DEVICE_ID_LIGHTGUN_TURBO
#define JUSTIFIER_START RETRO_DEVICE_ID_LIGHTGUN_PAUSE
#define JUSTIFIER_FIRST JUSTIFIER_X
#define JUSTIFIER_LAST JUSTIFIER_START

#define MACSRIFLE_X RETRO_DEVICE_ID_MACSRIFLE_X
#define MACSRIFLE_Y RETRO_DEVICE_ID_MACSRIFLE_Y
#define MACSRIFLE_TRIGGER RETRO_DEVICE_ID_LIGHTGUN_TRIGGER
#define MACSRIFLE_FIRST MACSRIFLE_X
#define MACSRIFLE_LAST MACSRIFLE_TRIGGER

#define BTN_POINTER (BTN_LAST + 1)
#define BTN_POINTER2 (BTN_POINTER + 1)


static void map_buttons()
{
   MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_A), "Joypad1 A");
   MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_B), "Joypad1 B");
   MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_X), "Joypad1 X");
   MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_Y), "Joypad1 Y");
   MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_SELECT), "{Joypad1 Select,Mouse1 L}");
   MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_START), "{Joypad1 Start,Mouse1 R}");
   MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_L), "Joypad1 L");
   MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_R), "Joypad1 R");
   MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_LEFT), "Joypad1 Left");
   MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_RIGHT), "Joypad1 Right");
   MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_UP), "Joypad1 Up");
   MAP_BUTTON(MAKE_BUTTON(PAD_1, BTN_DOWN), "Joypad1 Down");
   S9xMapPointer((BTN_POINTER), S9xGetCommandT("Pointer Mouse1+Superscope+Justifier1+MacsRifle"), false);
   S9xMapPointer((BTN_POINTER2), S9xGetCommandT("Pointer Mouse2"), false);

   MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_A), "Joypad2 A");
   MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_B), "Joypad2 B");
   MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_X), "Joypad2 X");
   MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_Y), "Joypad2 Y");
   MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_SELECT), "{Joypad2 Select,Mouse2 L,Superscope Fire,Justifier1 Trigger,MacsRifle Trigger}");
   MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_START), "{Joypad2 Start,Mouse2 R,Superscope Cursor,Justifier1 Start}");
   MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_L), "Joypad2 L");
   MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_R), "Joypad2 R");
   MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_LEFT), "Joypad2 Left");
   MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_RIGHT), "Joypad2 Right");
   MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_UP), "{Joypad2 Up,Superscope ToggleTurbo,Justifier1 AimOffscreen}");
   MAP_BUTTON(MAKE_BUTTON(PAD_2, BTN_DOWN), "{Joypad2 Down,Superscope Pause}");

   MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_A), "Joypad3 A");
   MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_B), "Joypad3 B");
   MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_X), "Joypad3 X");
   MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_Y), "Joypad3 Y");
   MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_SELECT), "Joypad3 Select");
   MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_START), "Joypad3 Start");
   MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_L), "Joypad3 L");
   MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_R), "Joypad3 R");
   MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_LEFT), "Joypad3 Left");
   MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_RIGHT), "Joypad3 Right");
   MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_UP), "Joypad3 Up");
   MAP_BUTTON(MAKE_BUTTON(PAD_3, BTN_DOWN), "Joypad3 Down");

   MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_A), "Joypad4 A");
   MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_B), "Joypad4 B");
   MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_X), "Joypad4 X");
   MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_Y), "Joypad4 Y");
   MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_SELECT), "Joypad4 Select");
   MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_START), "Joypad4 Start");
   MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_L), "Joypad4 L");
   MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_R), "Joypad4 R");
   MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_LEFT), "Joypad4 Left");
   MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_RIGHT), "Joypad4 Right");
   MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_UP), "Joypad4 Up");
   MAP_BUTTON(MAKE_BUTTON(PAD_4, BTN_DOWN), "Joypad4 Down");

   MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_A), "Joypad5 A");
   MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_B), "Joypad5 B");
   MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_X), "Joypad5 X");
   MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_Y), "Joypad5 Y");
   MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_SELECT), "Joypad5 Select");
   MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_START), "Joypad5 Start");
   MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_L), "Joypad5 L");
   MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_R), "Joypad5 R");
   MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_LEFT), "Joypad5 Left");
   MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_RIGHT), "Joypad5 Right");
   MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_UP), "Joypad5 Up");
   MAP_BUTTON(MAKE_BUTTON(PAD_5, BTN_DOWN), "Joypad5 Down");
}

// libretro uses relative values for analogue devices.
// S9x seems to use absolute values, but do convert these into relative values in the core. (Why?!)
// Hack around it. :)

static int16_t snes_mouse_state[2][2] = {{0}, {0}};
static int16_t snes_scope_state[2] = {0};
static int16_t snes_justifier_state[2][2] = {{0}, {0}};
static int16_t snes_macsrifle_state[2] = {0};

static void report_buttons()
{
   int _x, _y;
   int offset = snes_devices[0] == RETRO_DEVICE_JOYPAD_MULTITAP ? 4 : 1;
   for (int port = 0; port <= 1; port++)
   {
      switch (snes_devices[port])
      {
         case RETRO_DEVICE_NONE:
            break;
            
         case RETRO_DEVICE_JOYPAD:
            for (int i = BTN_FIRST; i <= BTN_LAST; i++)
               S9xReportButton(MAKE_BUTTON(port * offset + 1, i), input_state_cb(port * offset, RETRO_DEVICE_JOYPAD, 0, i));
            break;

         case RETRO_DEVICE_JOYPAD_MULTITAP:
            for (int j = 0; j < 4; j++)
               for (int i = BTN_FIRST; i <= BTN_LAST; i++)
                  S9xReportButton(MAKE_BUTTON(port * offset + j + 1, i), input_state_cb(port * offset + j, RETRO_DEVICE_JOYPAD, 0, i));
            break;

         case RETRO_DEVICE_MOUSE:
            _x = input_state_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
            _y = input_state_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
            snes_mouse_state[port][0] += _x;
            snes_mouse_state[port][1] += _y;
            S9xReportPointer(BTN_POINTER + port, snes_mouse_state[port][0], snes_mouse_state[port][1]);
            for (int i = MOUSE_LEFT; i <= MOUSE_LAST; i++)
               S9xReportButton(MAKE_BUTTON(port + 1, i), input_state_cb(port, RETRO_DEVICE_MOUSE, 0, i));
            break;

         case RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE:
            snes_scope_state[0] += input_state_cb(port, RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE, 0, RETRO_DEVICE_ID_LIGHTGUN_X);
            snes_scope_state[1] += input_state_cb(port, RETRO_DEVICE_LIGHTGUN_SUPER_SCOPE, 0, RETRO_DEVICE_ID_LIGHTGUN_Y);
            if (snes_scope_state[0] < 0) snes_scope_state[0] = 0;
            else if (snes_scope_state[0] > (SNES_WIDTH-1)) snes_scope_state[0] = SNES_WIDTH-1;
            if (snes_scope_state[1] < 0) snes_scope_state[1] = 0;
            else if (snes_scope_state[1] > (SNES_HEIGHT-1)) snes_scope_state[1] = SNES_HEIGHT-1;
            S9xReportPointer(BTN_POINTER, snes_scope_state[0], snes_scope_state[1]);
            for (int i = SCOPE_TRIGGER; i <= SCOPE_LAST; i++)
               S9xReportButton(MAKE_BUTTON(2, i), input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, i));
            break;

         case RETRO_DEVICE_LIGHTGUN_JUSTIFIER:
         case RETRO_DEVICE_LIGHTGUN_JUSTIFIERS:
            snes_justifier_state[port][0] += input_state_cb(port, RETRO_DEVICE_LIGHTGUN_JUSTIFIER, 0, RETRO_DEVICE_ID_LIGHTGUN_X);
            snes_justifier_state[port][1] += input_state_cb(port, RETRO_DEVICE_LIGHTGUN_JUSTIFIER, 0, RETRO_DEVICE_ID_LIGHTGUN_Y);
            if (snes_justifier_state[port][0] < 0) snes_justifier_state[port][0] = 0;
            else if (snes_justifier_state[port][0] > (SNES_WIDTH-1)) snes_justifier_state[port][0] = SNES_WIDTH-1;
            if (snes_justifier_state[port][1] < 0) snes_justifier_state[port][1] = 0;
            else if (snes_justifier_state[port][1] > (SNES_HEIGHT-1)) snes_justifier_state[port][1] = SNES_HEIGHT-1;
            S9xReportPointer(BTN_POINTER, snes_justifier_state[port][0], snes_justifier_state[port][1]);
            for (int i = JUSTIFIER_TRIGGER; i <= JUSTIFIER_LAST; i++)
               S9xReportButton(MAKE_BUTTON(2, i), input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, i));
            break;

         case RETRO_DEVICE_LIGHTGUN_MACSRIFLE:
            snes_macsrifle_state[0] += input_state_cb(port, RETRO_DEVICE_LIGHTGUN_MACSRIFLE, 0, RETRO_DEVICE_ID_LIGHTGUN_X);
            snes_macsrifle_state[1] += input_state_cb(port, RETRO_DEVICE_LIGHTGUN_MACSRIFLE, 0, RETRO_DEVICE_ID_LIGHTGUN_Y);
            if (snes_macsrifle_state[0] < 0) snes_macsrifle_state[0] = 0;
            else if (snes_macsrifle_state[0] > (SNES_WIDTH-1)) snes_macsrifle_state[0] = SNES_WIDTH-1;
            if (snes_macsrifle_state[1] < 0) snes_macsrifle_state[1] = 0;
            else if (snes_macsrifle_state[1] > (SNES_HEIGHT-1)) snes_macsrifle_state[1] = SNES_HEIGHT-1;
            S9xReportPointer(BTN_POINTER, snes_macsrifle_state[0]+macsrifle_adjust_x, snes_macsrifle_state[1]+macsrifle_adjust_y);
            for (int i = MACSRIFLE_TRIGGER; i <= MACSRIFLE_LAST; i++)
               S9xReportButton(MAKE_BUTTON(2, i), input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, i));
            break;

         default:
            if (log_cb)
               log_cb(RETRO_LOG_ERROR, "[libretro]: Unknown device...\n");
      }
   }
}

static uint8 savebuf[0x200000];
static size_t savesize = 0;

void retro_run()
{
   //sanity check to prevent infinite loop inside emulator
   if (!rom_loaded) return;

   static uint16 height = PPU.ScreenHeight;
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables();
   if (height != PPU.ScreenHeight)
   {
      update_geometry();
      height = PPU.ScreenHeight;
   }

   int result = -1;
   bool okay = environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &result);
   if (okay)
   {
      bool audioEnabled = 0 != (result & 2);
      bool videoEnabled = 0 != (result & 1);
      bool hardDisableAudio = 0 != (result & 8);
      IPPU.RenderThisFrame = videoEnabled;
      S9xSetSoundMute(!audioEnabled || hardDisableAudio);
      Settings.HardDisableAudio = hardDisableAudio;

	    if(runahead_speed && (audioEnabled==0 || videoEnabled==0 || hardDisableAudio!=0)) return;
   }
   else
   {
      IPPU.RenderThisFrame = true;
      S9xSetSoundMute(false);
      Settings.HardDisableAudio = false;
   }

	 if(runahead_speed==0)
	    savesize=0;

	 if(savesize)
	 {
		 // 2nd frame
		 Settings.FastSavestates = true;
		 S9xUnfreezeGameMem((const uint8_t*)savebuf,savesize);
		 Settings.FastSavestates = false;

		 poll_cb();
		 report_buttons();

  	 IPPU.RenderThisFrame = false;
		 S9xSetSoundMute(true);
	   
	   S9xMainLoop();
	   S9xAudioCallback(NULL);
		 S9xFreezeGameMem((uint8_t*)savebuf,savesize);

		 for(int lcv=1; lcv<runahead_speed; lcv++)
		 {
		   S9xMainLoop();
		   S9xAudioCallback(NULL);
		 }
	 }
	 else
	 {
		 // 1st frame
		 savesize = S9xFreezeSize();

		 poll_cb();
     report_buttons();

		 S9xFreezeGameMem((uint8_t*)savebuf,savesize);
	 }


 	 IPPU.RenderThisFrame = true;
   S9xSetSoundMute(false);

	 S9xMainLoop();
	 S9xAudioCallback(NULL);
}

void retro_deinit()
{
   S9xDeinitAPU();
   Memory.Deinit();
   S9xGraphicsDeinit();
   S9xUnmapAllControls();

   free(GFX.Screen);
   free(gfx_blend);
}


unsigned retro_get_region()
{
   return Settings.PAL ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

void* retro_get_memory_data(unsigned type)
{
   void* data;

   switch(type) {
      case RETRO_MEMORY_SNES_SUFAMI_TURBO_A_RAM:
      case RETRO_MEMORY_SAVE_RAM:
         data = Memory.SRAM;
         break;
      case RETRO_MEMORY_SNES_SUFAMI_TURBO_B_RAM:
         data = Multi.sramB;
         break;
      case RETRO_MEMORY_RTC:
         data = RTCData.reg;
         break;
      case RETRO_MEMORY_SYSTEM_RAM:
         data = Memory.RAM;
         break;
      case RETRO_MEMORY_VIDEO_RAM:
         data = Memory.VRAM;
         break;
      //case RETRO_MEMORY_ROM:
      //   data = Memory.ROM;
      //   break;
      default:
         data = NULL;
         break;
   }

   return data;
}

size_t retro_get_memory_size(unsigned type)
{
   size_t size;

   switch(type) {
      case RETRO_MEMORY_SNES_SUFAMI_TURBO_A_RAM:
      case RETRO_MEMORY_SAVE_RAM:
         size = (unsigned) (Memory.SRAMSize ? (1 << (Memory.SRAMSize + 3)) * 128 : 0);
         if (size > 0x20000)
            size = 0x20000;
         break;
      case RETRO_MEMORY_SNES_SUFAMI_TURBO_B_RAM:
         size = (unsigned) (Multi.cartType==4 && Multi.sramSizeB ? (1 << (Multi.sramSizeB + 3)) * 128 : 0);
         break;
      case RETRO_MEMORY_RTC:
         size = (Settings.SRTC || Settings.SPC7110RTC)?20:0;
         break;
      case RETRO_MEMORY_SYSTEM_RAM:
         size = 128 * 1024;
         break;
      case RETRO_MEMORY_VIDEO_RAM:
         size = 64 * 1024;
         break;
      //case RETRO_MEMORY_ROM:
      //   size = Memory.CalculatedSize;
      //   break;
      default:
         size = 0;
         break;
   }

   return size;
}

size_t retro_serialize_size()
{
   return rom_loaded ? S9xFreezeSize() : 0;
}

bool retro_serialize(void *data, size_t size)
{
   int result = -1;
   bool okay = false;
   okay = environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &result);
   if (okay)
   {
      Settings.FastSavestates = 0 != (result & 4);
   }

	 if(runahead_speed && Settings.FastSavestates)
		  return true;

	 if (S9xFreezeGameMem((uint8_t*)data,size) == FALSE)
      return false;

   return true;
}

bool retro_unserialize(const void* data, size_t size)
{
   int result = -1;
   bool okay = false;
   okay = environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &result);
   if (okay)
   {
      Settings.FastSavestates = 0 != (result & 4);
			if (Settings.FastSavestates == 0) update_variables();
   }

	 if(runahead_speed && Settings.FastSavestates)
		  return true;

   if (S9xUnfreezeGameMem((const uint8_t*)data,size) != SUCCESS)
      return false;

	// reset runahead
  savesize = 0;

	return true;
}

bool8 S9xDeinitUpdate(int width, int height)
{
   // Apply Chrono Trigger Framehack
   if (!Settings.DisableGameSpecificHacks && ChronoTriggerFrameHack && (height > SNES_HEIGHT))
      height = SNES_HEIGHT;

   if (crop_overscan_mode == 1) // enabled
   {
      if (height >= SNES_HEIGHT << 1)
      {
         height = SNES_HEIGHT << 1;
      }
      else
      {
         height = SNES_HEIGHT;
      }
   }
   else if (crop_overscan_mode == 2) // disabled
   {
      if (height > SNES_HEIGHT_EXTENDED)
      {
         if (height < SNES_HEIGHT_EXTENDED << 1)
            memset(GFX.Screen + (GFX.Pitch >> 1) * height,0,GFX.Pitch * ((SNES_HEIGHT_EXTENDED << 1) - height));
         height = SNES_HEIGHT_EXTENDED << 1;
      }
      else
      {
         if (height < SNES_HEIGHT_EXTENDED)
            memset(GFX.Screen + (GFX.Pitch >> 1) * height,0,GFX.Pitch * (SNES_HEIGHT_EXTENDED - height));
         height = SNES_HEIGHT_EXTENDED;
      }
   }

	 if(blargg_filter)
	 {
		  if(blargg_filter==1)
				 RenderBlarggNTSCMonochrome(GFX.Screen, GFX.Pitch, gfx_blend, GFX.Pitch, width, height);
		  else if(blargg_filter==2)
				 RenderBlarggNTSCRf(GFX.Screen, GFX.Pitch, gfx_blend, GFX.Pitch, width, height);
		  else if(blargg_filter==3)
				 RenderBlarggNTSCComposite(GFX.Screen, GFX.Pitch, gfx_blend, GFX.Pitch, width, height);
		  else if(blargg_filter==4)
				 RenderBlarggNTSCSvideo(GFX.Screen, GFX.Pitch, gfx_blend, GFX.Pitch, width, height);
		  else if(blargg_filter==5)
				 RenderBlarggNTSCRgb(GFX.Screen, GFX.Pitch, gfx_blend, GFX.Pitch, width, height);

	    video_cb(gfx_blend, SNES_NTSC_OUT_WIDTH(256), height, GFX.Pitch);
	 }
	 else if(width==MAX_SNES_WIDTH && hires_blend)
   {
		  if (hires_blend==3 && !special_hires_game)
				video_cb(GFX.Screen, width, height, GFX.Pitch);
			else
			{
		     RenderMergeHires(GFX.Screen, GFX.Pitch, gfx_blend, GFX.Pitch, width, height, hires_blend);

			   if (hires_blend==3)
				    video_cb(gfx_blend, width/2, height, GFX.Pitch);
			   else
				    video_cb(gfx_blend, width, height, GFX.Pitch);
			}
   }
   else
   { 
      video_cb(GFX.Screen, width, height, GFX.Pitch);
   }

   return TRUE;
}

bool8 S9xContinueUpdate(int width, int height)
{
	 if(interlace_speed==1) return TRUE;

   // scrolling credits interlace -- show whole frame
   if(!Settings.DisableGameSpecificHacks && interlace_speed==0 && lufia2_credits_hack && PPU.BGMode==6)
      return TRUE;

   return S9xDeinitUpdate(width, height);
}

// Dummy functions that should probably be implemented correctly later.
void S9xParsePortConfig(ConfigFile&, int) {}
void S9xSyncSpeed() {}
const char* S9xStringInput(const char* in) { return in; }

#ifdef _WIN32
#define SLASH '\\'
#else
#define SLASH '/'
#endif

const char* S9xGetFilename(const char* in, s9x_getdirtype type)
{
   static char newpath[2048];

   newpath[0] = '\0';

   switch (type)
   {
      case ROMFILENAME_DIR:
         sprintf(newpath, "%s%c%s%s", g_rom_dir, SLASH, g_basename, in);
         return newpath;
      default:
         break;
   }

   return in;
}

const char* S9xGetDirectory(s9x_getdirtype type)
{
   switch (type)
   {
      case ROMFILENAME_DIR:
         return g_rom_dir;
      case BIOS_DIR:
         return retro_system_directory;
      default:
         break;
   }

   return "";
}
void S9xInitInputDevices() {}
const char* S9xChooseFilename(unsigned char) { return ""; }
void S9xHandlePortCommand(s9xcommand_t, short, short) {}
bool S9xPollButton(unsigned int, bool*) { return false; }
void S9xToggleSoundChannel(int) {}
const char* S9xGetFilenameInc(const char* in, s9x_getdirtype) { return ""; }
const char* S9xBasename(const char* in) { return in; }
bool8 S9xInitUpdate() { return TRUE; }
void S9xExtraUsage() {}
bool8 S9xOpenSoundDevice() { return TRUE; }
bool S9xPollAxis(unsigned int, short*) { return FALSE; }
void S9xSetPalette() {}
void S9xParseArg(char**, int&, int) {}
void S9xExit() {}
bool S9xPollPointer(unsigned int, short*, short*) { return false; }

void S9xMessage(int type, int, const char* s)
{
   if (!log_cb) return;

   switch (type)
   {
      case S9X_DEBUG:
         log_cb(RETRO_LOG_DEBUG, "%s\n", s);
         break;
      case S9X_WARNING:
         log_cb(RETRO_LOG_WARN, "%s\n", s);
         break;
      case S9X_INFO:
         log_cb(RETRO_LOG_INFO, "%s\n", s);
         break;
      case S9X_ERROR:
         log_cb(RETRO_LOG_ERROR, "%s\n", s);
         break;
      default:
         log_cb(RETRO_LOG_DEBUG, "%s\n", s);
         break;
   }
}

bool8 S9xOpenSnapshotFile(const char* filepath, bool8 read_only, STREAM *file)
{
   if(read_only)
   {
      if((*file = OPEN_STREAM(filepath, "rb")) != 0)
      {
         return (TRUE);
      }
   }
   else
   {
      if((*file = OPEN_STREAM(filepath, "wb")) != 0)
      {
         return (TRUE);
      }
   }
   return (FALSE);
}

void S9xCloseSnapshotFile(STREAM file)
{
   CLOSE_STREAM(file);
}

void S9xAutoSaveSRAM()
{
   return;
}

#ifndef __WIN32__
// S9x weirdness.
void _splitpath (const char *path, char *drive, char *dir, char *fname, char *ext)
{
   *drive = 0;

   const char   *slash = strrchr(path, SLASH_CHAR),
                *dot   = strrchr(path, '.');

   if (dot && slash && dot < slash)
      dot = NULL;

   if (!slash)
   {
      *dir = 0;

      strcpy(fname, path);

      if (dot)
      {
         fname[dot - path] = 0;
         strcpy(ext, dot + 1);
      }
      else
         *ext = 0;
   }
   else
   {
      strcpy(dir, path);
      dir[slash - path] = 0;

      strcpy(fname, slash + 1);

      if (dot)
      {
         fname[dot - slash - 1] = 0;
         strcpy(ext, dot + 1);
      }
      else
         *ext = 0;
   }
}

void _makepath (char *path, const char *, const char *dir, const char *fname, const char *ext)
{
   if (dir && *dir)
   {
      strcpy(path, dir);
      strcat(path, SLASH_STR);
   }
   else
      *path = 0;

   strcat(path, fname);

   if (ext && *ext)
   {
      strcat(path, ".");
      strcat(path, ext);
   }
}
#endif // __WIN32__
