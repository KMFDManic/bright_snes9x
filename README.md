# bright_snes9x - libretro

sandbox of www.github.com/libretro/snes9x (pre-1.56 rebase) for testing experimental features (meant for having fun only!!) -      ideally not recommended for upstream core inclusion (seriously, it's going to be messy clutter!)


dropoff 6 preview:
- add byuu's hitachi dsp chipset code (cx4)


dropoff 5:
- add byuu's nec dsp chipset code (dsp1-4, st010-011)
- add special chipset emulation: hardware (LLE), software (HLE)
- fixed default gaussian volume
- fixed internal runahead soft reset


dropoff 4:
- improved direct bsx game loading (no bios)
- backport st010 accuracy fixes
- backport interlace fixes (Air Strike Patrol, F1 ROC II)
- clear interlace flicker on load state
- correctly apply settings on load state
- internal runahead interlace fixes (many)
- show interlace frames: even, odd, both


dropoff 3:
- internal runahead feature (good speedup)
- hires blending: disabled, half, full, special
- fix Lufia 2 interlace game id detection
- ignore bios option (bs-x, sufami turbo)
- try loading game even if bios missing
- backport Little Magic smp fix


dropoff 2:
- backport blargg ntsc filters: disabled, monochrome, rf, composite, s-video, rgb
- dsp-1 chipset: rev 1b, rev 1(a) (pilotwings light plane demo)
- interlace speed: auto (lufia 2 credits), slow, fast
- use special game hacks: enabled, disabled (emu testing)
- inline mudlord interpolation (small speedup)


dropoff 1:
- apply settings on boot and loadstate (sound channels, layers, misc)
- speakers mode: 16-bit stereo/mono, 8-bit stereo/mono, mute
- console region: auto, japan-usa (ntsc), europe (pal)
- allow invalid vram access: disabled, enabled (bad hacks, 60hz pal games, debugging)
- interpolation: 4-tap (4096 gaussian + no brr overflow), 8-tap (4096 sinc). default gaussian has brr overflow hardware error
- backport game fixes (Top Gear 3000, Dragon Ball Z: Super Butouden 2, Final Fantasy 6)
- change core name to Snes9x Bright


install:
- download Retroarch from www.libretro.com
- copy bright_snes9x to main Retroarch -- cores + system folders
- load core, load content, run
- now change options and play! modify settings as needed


notes:
- 4096 point interpolation will sound very similar to mudlord's 256 ones. Because Brad Miller is that good at using small tables to make high quality output. But for imaginary phantom audio benefits, 4/8-tap custom is the way to go!

- allow invalid vram access. This is useful for playing Europe in ntsc mode. Like Marko's Magic Football with copier protection disabled. Possibly Lucky Luke. World Masters Golf. And smoothing out some others. Or (old) translation hacks that don't work on real hardware. Plus newer hacks can abuse this feature and give lots of illegal speedup! And notably, this option breaks Hook gameplay.

- internal runahead feature. runs faster. Kirby's Dream Land 3 ## 0: 188 ## 1: 93 (frontend) ==> 104 (secondary) ==> 121 (internal). Also fixes lightgun cursor.

- hires blending special activates custom blending only for these games. extra compatible with lores scalers. Bishoujo Senshi Sailor Moon S - Kondo wa Puzzle de Oshioki yo, Kirby's Dream Land 3 / Hoshi no Kirby 3, Jurassic Park

- memory randomization. Only Super Off Road seems to use it so far.

- hardware chipset emulation. Name your BIOS files dsp1.bin, dsp1b.bin, dsp2.bin, dsp3.bin, dsp4.bin, st0010.bin, st0011.bin, cx4.bin and place in Retroarch system folder.

- there's a rough chance of dropoff 7 coming out. But there's honestly not much left to be added (sgb? st018?). Interest is naturally low. Likely almost no one will find out about this build and I can keep it a secret!

- if you actually enjoy using this win32 snes9x port, then erm. Thanks! And it's alright if you don't like/want/accept/use any of these features. They are leftovers and not intended for mainstream libretro / snes9x community consumption.
