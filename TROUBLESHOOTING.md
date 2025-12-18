# Troubleshooting PKGBUILD Issues

## Dependency Resolution Problems

If `makepkg -si` fails with dependency resolution errors, try the following:

### 1. Update Package Databases
```bash
sudo pacman -Syy
```

### 2. Install base-devel Group
```bash
sudo pacman -S --needed base-devel
```

### 3. Check for Missing Dependencies

The package requires:
- `wlroots0.19` - Available in extra repository
- `libsfdo` - Available in extra repository (optional)
- Standard packages: wayland, libinput, xkbcommon, etc.

If `wlroots0.19` is not found, you may need to:
```bash
sudo pacman -S wlroots0.19
```

### 4. Build Without Installing Dependencies (Not Recommended)
```bash
makepkg -si --nodeps
```
**Warning:** This may result in a broken build.

### 5. Manual Dependency Installation

If specific dependencies fail, install them manually:
```bash
sudo pacman -S wlroots0.19 wayland libinput xkbcommon libxml2 glib2 cairo pango pixman libpng libdrm
sudo pacman -S git meson ninja wayland-protocols scdoc
```

### 6. Check AUR for Missing Packages

Some dependencies might only be in AUR:
```bash
yay -S missing-package-name
# or
paru -S missing-package-name
```

## Common Issues

### Issue: "wlroots-0.19" not found
**Solution:** The package name is `wlroots0.19` (no hyphen). This has been fixed in the PKGBUILD.

### Issue: Version constraints causing problems
**Solution:** Version constraints like `>=1.14` have been removed. The package manager will install the latest compatible version.

### Issue: libsfdo not found
**Solution:** `libsfdo` is in the extra repository. If it's not found, update your package databases:
```bash
sudo pacman -Syy
sudo pacman -S libsfdo
```

## Getting Help

If you continue to have issues:
1. Check the full error message from `makepkg -si`
2. Verify all repositories are enabled in `/etc/pacman.conf`
3. Check the Arch Linux forums or AUR comments for similar issues

