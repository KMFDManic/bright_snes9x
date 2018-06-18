# bright_snes9x - libretro

sandbox of github.com/libretro/snes9x (pre-1.56 rebase) for testing experimental features (just for fun only!!) - not recommended for upstream core inclusion (seriously)


changelog:
- re-apply settings on loadstate and post-init (sound channels, show layers, misc)
- speakers mode: 16-bit stereo/mono, 8-bit stereo/mono, mute
- console region: auto, japan-usa (ntsc), europe (pal)
- block vram access: enabled, disabled (bad hacks, 60hz pal games, debugging)
- interpolation: 4-tap (4096 gaussian + no brr overflow), 8-tap (4096 sinc). default gaussian has brr overflow hardware error
- backport game fixes (Top Gear 3000, Dragon Ball Z: Super Butouden 2, Umihara Kawase)
- change core name to Snes9x Bright


install:
- download Retroarch from www.libretro.com
- copy bright_snes9x to main Retroarch -- cores + system folders
- load core, load content
- change options and play! modify settings as needed
