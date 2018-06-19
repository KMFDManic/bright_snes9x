# bright_snes9x - libretro

sandbox of www.github.com/libretro/snes9x (pre-1.56 rebase) for testing experimental features (meant for having fun only!!) - ideally not recommended for upstream core inclusion (seriously, it's going to be messy clutter!)


dropoff2:
- backport blargg ntsc filters: disabled, monochrome, rf, composite, s-video, rgb
- dsp-1 chipset: rev 1b, rev 1(a) (pilotwings light plane demo)
- interlace speed: auto (lufia 2 credits), slow, fast
- use special game hacks: enabled, disabled (emu testing)
- inline mudlord interpolation (small speedup)


dropoff1:
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

- if you actually enjoy using this win32 snes9x port, then erm. Thanks! And it's alright if you don't like/want/accept/use any of these features. They are leftovers and not intended for mainstream libretro community consumption.

- there's a rough chance of dropoff3 coming out. But there's honestly not much left to be added. Interest is naturally low. Likely almost no one will find out about this build and I can keep it a secret!
