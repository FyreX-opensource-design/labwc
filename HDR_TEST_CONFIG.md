# HDR Configuration Testing Guide

## Configuration Syntax

Add HDR settings to your `rc.xml` file:

### Global HDR Setting

```xml
<labwc_config>
  <core>
    <!-- Global HDR setting: enabled, disabled, or auto -->
    <hdr>auto</hdr>
  </core>
</labwc_config>
```

### Per-Output HDR Settings

```xml
<labwc_config>
  <!-- Per-output HDR configuration -->
  <hdr output="DP-1" enabled="yes" />
  <hdr output="HDMI-A-1" enabled="no" />
  <hdr output="eDP-1" enabled="auto" />
</labwc_config>
```

## Valid Values

- `enabled` / `yes` / `true` / `on` / `1` - Enable HDR
- `disabled` / `no` / `false` / `off` / `0` - Disable HDR  
- `auto` - Auto-detect HDR capability

## Testing

1. **Find your output names:**
   ```bash
   wlr-randr
   ```
   Look for output names like `DP-1`, `HDMI-A-1`, `eDP-1`, etc.

2. **Add HDR config to your rc.xml:**
   ```xml
   <labwc_config>
     <core>
       <hdr>auto</hdr>
     </core>
     
     <!-- Optional: per-output settings -->
     <hdr output="DP-1" enabled="yes" />
   </labwc_config>
   ```

3. **Start labwc and check logs:**
   ```bash
   labwc 2>&1 | grep -i hdr
   ```
   
   You should see log messages like:
   - `Parsed global HDR setting: auto`
   - `Parsed HDR config: output="DP-1" mode=enabled`
   - `Using per-output HDR config for DP-1: enabled`
   - `Applying HDR mode to output DP-1: enabled`

4. **Check if configuration is applied:**
   The logs will show which HDR mode is being used for each output.

## Current Status

- ✅ Configuration parsing (global and per-output)
- ✅ Debug logging to verify parsing
- ⏳ Actual wlroots HDR API implementation (placeholder)

The HDR configuration system is ready to test. The actual HDR enabling/disabling via wlroots APIs is still a placeholder and will be implemented next.




