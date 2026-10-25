#pragma once
// DSU ("cemuhook") controller client: pads served over UDP by NSO GC Driver (macOS), DS4Windows,
// BetterJoy, phone and Steam Deck apps, ... (the protocol Dolphin's DSU client reads). Every
// connected DSU slot appears as an SDL virtual gamepad, so it is assigned to a port and mapped
// like any other gamepad. Off by default; [controller] dsu_enabled / dsu_servers / dsu_layout.

namespace DsuClient {

// Attaches, updates and detaches the virtual gamepads from the latest packets. Call once per
// frame from the guest thread, before the pads are read.
void Update();

// "DSU controllers (NSO GC Driver)" submenu for the settings overlay.
void DrawSettingsMenu();

}  // namespace DsuClient
