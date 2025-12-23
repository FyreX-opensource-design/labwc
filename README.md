# labFyre

<h3 align="center">[<a
href="https://labwc.github.io/">Website</a>] [<a
href="https://github.com/labwc/labwc-scope#readme">Scope</a>] [<a
href="https://web.libera.chat/gamja/?channels=#labwc">IRC&nbsp;Channel</a>] [<a
href="NEWS.md">Release&nbsp;Notes</a>]</h3>

- [1. Project Description](#1-project-description)
  - [1.1 What Is This?](#11-what-is-this)
  - [1.2 Why](#12-why)
  - [1.3 Why The Openbox Theme Specification?](#13-why-the-openbox-theme-specification)
  - [1.4 High Modularity](#14-high moduarity)
  - [1.5 Screenshot](#16-screenshot)
- [2. Build and Installation](#2-build-and-installation)
- [3. Configuration](#3-configuration)
- [4. Theming](#4-theming)
- [5. Usage](#5-usage)
- [6. Integration](#6-integration)
- [7. Translations](#7-translations)

## 1. Project Description

### 1.1 What Is This?

Quoted from upstream: Labwc stands for Lab Wayland Compositor, where lab can mean any of the
following:

- lightweight and *box-inspired
- sense of experimentation and treading new ground
- inspired by BunsenLabs and ArchLabs
- your favorite pet

LabFyre and do all of this and more, it's a soft fork of labwc with support for
shell script control

LabwFyre is a [wlroots]-based window-stacking and tiling compositor for [Wayland], built from [labwc] 
and has support for [Openbox] themes.

It is light-weight and independent with a focus that can range from simply stacking windows well
to a tiling layout and keybinds for each device and application.
It still holds support for a no-bling/frills approach, yet animations are still in the works.
It relies on clients for panels, screenshots, wallpapers and so on to create a full desktop environment.

LabFyre has no reliance on any particular Desktop Environment, Desktop Shell or
session. Nor does it depend on any UI toolkits such as Qt or GTK.

### 1.2 Why?

Firstly, upsteam devs believed that there is a need for a simple Wayland window-stacking
compositor which strikes a balance between minimalism and bloat approximately
at the level where Window Managers like Openbox reside in the X11 domain.  Most
of the core developers were accustomed to low resource Desktop Environments such
as Mate/XFCE or standalone Window Managers such as Openbox under X11.  Labwc
aimed to make a similar setup possible under Wayland, with small and independent
components rather than a large, integrated software eco-system.

Secondly, the Wayland community has achieved an amazing amount so far, and the upstream devs
want to help solve the unsolved problems to make Wayland viable for more
people. Labwc only understood [wayland-protocols] &amp; [wlr-protocols], and it could not be
controlled with dbus, sway/i3/custom-IPC or other technology.

This was quite limiting for some scripts we wanted to make, specifically sticky keys 
and universial copy/paste, as well as the fact that some other features were limited
such as virtual outputs being purely tied to actions within rc.xml.
We extend this to be triggerable via shell commands, so it makes scripting a lot easier.

### 1.3 Why The Openbox Theme Specification?

This is from the upstream Labwc, and a lot of themes have support for it
GTK and QT themes can still be used to some extent, but should be Server side over
client.

### 1.4 high modularity

All the triggers and extensions are triggerable via flags sent to the binary.
This allows you to use any language to make plugins.

Bash, Zsh, Xonsh, python, go, java, zig... so long as it can trigger system commands
you can write a plugin for it.

See [scope] for full details on implemented features.

### 1.5 Screenshot

The obligatory screenshot (origionally from mainline:

<a href="https://labwc.github.io/img/scrot1.png">
  <img src="https://labwc.github.io/img/scrot1-small.png">
</a><br />
<a href="https://labwc.github.io/obligatory-screenshot.html">
  <small>Screenshot description</small>
</a>

## 2. Build and Installation

To build, simply run:

    meson setup build/
    meson compile -C build/

Run-time dependencies include:

- wlroots, wayland, libinput, xkbcommon
- libxml2, cairo, pango, glib-2.0
- libpng
- librsvg >=2.46 (optional)
- libsfdo (optional)
- xwayland, xcb (optional)

Build dependencies include:

- meson, ninja, gcc/clang
- wayland-protocols

Disable xwayland with `meson -Dxwayland=disabled build/`

For OS/distribution specific details see [wiki].

If the right version of `wlroots` is not found on the system, the build setup
will automatically download the wlroots repo. If this fallback is not desired
please use:

    meson setup --wrap-mode=nodownload build/

To enforce the supplied wlroots.wrap file, run:

    meson setup --force-fallback-for=wlroots build/

If installing after using the wlroots.wrap file, use the following to
prevent installing the wlroots headers:

    meson install --skip-subprojects -C build/

## 3. Configuration

User config files are located at `${XDG_CONFIG_HOME:-$HOME/.config/labwc/}`
and system configs can be stored at `/etc/xdg/labwc/`
with the following seven files being used: [rc.xml], [menu.xml], [autostart], [reconfigure],
[shutdown], [environment] and [themerc-override].

Run `labwc --reconfigure` to reload configuration and theme.

For a step-by-step initial configuration guide, see [getting-started].

## 4. Theming

Themes are located at `~/.local/share/themes/\<theme-name\>/labwc/` or
equivalent `XDG_DATA_{DIRS,HOME}` location in accordance with freedesktop XDG
directory specification.

For full theme options, see [labwc-theme(5)] or the [themerc] example file.

For themes, search the internet for "openbox themes" and place them in
`~/.local/share/themes/`. Some good starting points include:

- https://github.com/addy-dclxvi/openbox-theme-collections
- https://github.com/the-zero885/Lubuntu-Arc-Round-Openbox-Theme
- https://github.com/BunsenLabs/bunsen-themes

## 5. Usage

    ./build/labwc [-s <command>]

> **_NOTE:_** If you are running on **NVIDIA**, you will need the
> `nvidia-drm.modeset=1` kernel parameter.

If you have not created an rc.xml config file, default bindings will be:

| combination              | action
| ------------------------ | ------
| `alt`-`tab`              | activate next window
| `alt`-`shift`-`tab`      | activate previous window
| `super`-`return/enter`         | lab-sensible-terminal
| `alt`-`F4`               | close window
| `super`-`a`              | toggle maximize
| `super`-`mouse-left`     | move window
| `super`-`mouse-right`    | resize window
| `super`-`arrow`          | resize window to fill half the output
| `alt`-`space`            | show the window menu
| `XF86_AudioLowerVolume`  | amixer sset Master 5%-
| `XF86_AudioRaiseVolume`  | amixer sset Master 5%+
| `XF86_AudioMute`         | amixer sset Master toggle
| `XF86_MonBrightnessUp`   | brightnessctl set +10%
| `XF86_MonBrightnessDown` | brightnessctl set 10%-

A root-menu can be opened by clicking on the desktop.

## 6. Integration

Suggested apps to use with Labwc:

- Screen shooter: [grim] [slurp]
- Screen recorder: [wf-recorder]
- Background image: [swaybg] [swww]
- Panel: [waybar], [lavalauncher], [sfwbar], [xfce4-panel]
- Launchers: [bemenu], [fuzzel], [wofi]
- Output managers: [wlopm], [kanshi], [shikane], [wlr-randr]
- Screen locker: [swaylock], [gtklock]
- Gamma adjustment: [gammastep]
- Idle screen inhibitor: [swayidle], [sway-audio-idle-inhibit]
- Compositor Control: [wlrctl]

See [integration] for further details.

## 7. Translations

The default window bar menu can be translated on the [weblate platform](https://translate.lxqt-project.org/projects/labwc/labwc/).

<a href="https://translate.lxqt-project.org/engage/labwc/?utm_source=widget">
<img src="https://translate.lxqt-project.org/widgets/labwc/-/labwc/multi-blue.svg" alt="Translation status" />
</a>

[Wayland]: https://wayland.freedesktop.org/
[Openbox]: https://openbox.org/help/Contents
[wlroots]: https://gitlab.freedesktop.org/wlroots/wlroots
[sway]: https://github.com/swaywm
[wayland-protocols]: https://gitlab.freedesktop.org/wayland/wayland-protocols
[wlr-protocols]: https://gitlab.freedesktop.org/wlroots/wlr-protocols
[scope]: https://github.com/labwc/labwc-scope#readme
[wiki]: https://github.com/labwc/labwc/wiki
[getting-started]: https://labwc.github.io/getting-started.html
[integration]: https://labwc.github.io/integration.html
[metacity]: https://github.com/GNOME/metacity

[rc.xml]: docs/rc.xml.all
[menu.xml]: docs/menu.xml
[autostart]: docs/autostart
[shutdown]: docs/shutdown
[environment]: docs/environment
[themerc-override]: docs/themerc
[themerc]: docs/themerc
[labwc-theme(5)]: https://labwc.github.io/labwc-theme.5.html

[gamescope]: https://github.com/Plagman/gamescope
[grim]: https://github.com/emersion/grim
[wf-recorder]: https://github.com/ammen99/wf-recorder
[swaybg]: https://github.com/swaywm/swaybg
[waybar]: https://github.com/Alexays/Waybar
[lavalauncher]: https://sr.ht/~leon_plickat/LavaLauncher
[sfwbar]: https://github.com/LBCrion/sfwbar
[xfce4-panel]: https://gitlab.xfce.org/xfce/xfce4-panel
[bemenu]: https://github.com/Cloudef/bemenu
[fuzzel]: https://codeberg.org/dnkl/fuzzel
[wofi]: https://hg.sr.ht/~scoopta/wofi
[wlopm]: https://git.sr.ht/~leon_plickat/wlopm
[kanshi]: https://sr.ht/~emersion/kanshi/
[wlr-randr]: https://sr.ht/~emersion/wlr-randr/
[swaylock]: https://github.com/swaywm/swaylock
[gammastep]: https://gitlab.com/chinstrap/gammastep
[sway-audio-idle-inhibit]: https://github.com/ErikReider/SwayAudioIdleInhibit
