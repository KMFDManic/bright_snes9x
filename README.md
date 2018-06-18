# bright_snes9x - libretro

sandbox of libretro/snes9x for testing experimental features (for fun only!!) - not recommended for upstream core inclusion

changelog:
- re-apply settings on loadstate and post-init (sound channels, show layers, misc)
- speakers mode: 16-bit stereo/mono, 8-bit stereo/mono, mute
- console region: auto, japan-usa (ntsc), europe (pal)
- block vram access: enabled, disabled (bad hacks, 60hz pal games, debugging)
- interpolation: 4-tap (4096 gaussian + no brr overflow), 8-tap (4096 sinc). default gaussian has brr overflow hardware error
- backport game fixes (Top Gear 3000, Dragon Ball Z: Super Butouden 2, Umihara Kawase)
