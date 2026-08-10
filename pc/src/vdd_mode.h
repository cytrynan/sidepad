// pc/src/vdd_mode.h
//
// The virtual display's target modes, shared by the daemon (wait_display /
// mode switching) and the CLI's --list-displays judgment. Native is the
// iPad Pro 12.9 panel (2732x2048); fallback is the spike-era half-ish mode
// kept for when the native mode cannot be reached (driver list, bandwidth).
// The mode list itself comes from C:\VirtualDisplayDriver\vdd_settings.xml
// (global refresh rates replicate onto every listed resolution).

#pragma once

constexpr int kVddNativeWidth = 2732;
constexpr int kVddNativeHeight = 2048;
constexpr int kVddFallbackWidth = 2048;
constexpr int kVddFallbackHeight = 1536;
constexpr int kVddRefreshHz = 120;
