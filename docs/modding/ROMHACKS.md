# Romhacks

Lighthouse can run Banjo-Kazooie romhacks. You point it at the hack's ROM once, it converts the
hack into a mod overlay, and from then on you switch it on and off from the Mod Menu.

## What you need

- A romhack `.z64` file. Leave it wherever you downloaded it, you will browse to it.
- Your base game already set up from a **US 1.0** ROM. Romhacks are built against US 1.0, so a base
  built from any other region or revision is refused.

## Installing a hack

1. Launch the game.
2. Open the port menu. Escape on the desktop, or click the LEFT stick (L3) on a controller. In the
   headset this menu appears on the panel in front of you. Note the right stick click is not this,
   that cycles the VR view mode.
3. Go to **Mod Menu**.
4. Click **Generate Romhack from ROM**.
5. Pick the hack's `.z64` in the file browser.
6. The game converts the hack and then closes itself. That is expected, not a crash: the overlay is
   picked up when the game next starts.
7. Reopen the game. The hack is running.

The converted overlay is written to `mods/~romhacks/`. You never put the `.z64` there yourself.

## Switching hacks on and off

- Enable and disable overlays in **Mod Menu**, then restart. Mods are not reloaded while the game
  is running.
- Running more than one overlay, drag them to set the order. Priority runs top to bottom, so an
  entry overrides everything listed below it.
- To go back to the normal game, disable the hack and restart.

## What comes across, and what does not

- Art, textures and level changes convert fine.
- Custom **code** does not. Lighthouse extracts assets, not the hack's own game code, so a hack that
  adds new moves or new systems will have those parts missing or broken. When a hack ships code you
  get a "Custom Code Romhack Detected" prompt during conversion and can choose to continue anyway.

## Saves

Each hack keeps its own save file, under `saves/~romhacks/`. Your main game's save cannot be touched
by a hack, and the hack's progress does not follow you back to the base game.

## The mods folder

| Folder | What goes in it |
|---|---|
| `mods/` | Ordinary asset mods and texture packs as `.o2r` files. No conversion needed, drop them in. |
| `mods/~romhacks/` | Converted hack overlays. Written by **Generate Romhack from ROM**. |
| `mods/~shared/` | Mods that apply whichever hack is loaded. |
| `mods/~lang/` | Language packs. See `LANGUAGE PACKS.md`. |

A loose folder in `mods/` is treated as an unpacked asset overlay, which is handy while making a mod.

## Troubleshooting

- **"Base is not US v1.0"**: the base game was built from the wrong ROM version. Re-extract it from a
  US 1.0 ROM and try again.
- **The hack does not show up**: check it is listed and enabled in Mod Menu, then restart. Nothing
  loads mid-session.
- **Moves or features missing**: that is the custom code limit above, not a fault in the port.
- **Two mods fighting**: conflicting overlays are quarantined and reported in a popup. Reorder them
  or disable one.

## In VR

The stereo rendering works on the game's projection, not on its assets, so a hack should render in
VR the same as the base game. This has not been tested yet. If you run a hack in the headset, the
parts worth checking are the sky, the HUD counters and the pause menu.
