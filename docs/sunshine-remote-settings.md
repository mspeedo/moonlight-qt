# Sunshine Remote Settings (experimental)

This feature adds **Sunshine Remote Settings** to the context menu of an **online paired Sunshine PC** in Moonlight's Computers view.

## Architecture

Moonlight retrieves the host address and pinned Sunshine certificate from its existing paired `NvComputer`; the web UI port is the GameStream base HTTP port plus one (normally **47990**). The new Qt `RemoteSunshineConfig` class talks to Sunshine's existing HTTPS web configuration API through `QNetworkAccessManager`, with manual redirect handling, no proxy, and strict comparison to the paired certificate. Admin credentials are requested separately and retained in memory only.

No new streaming protocol messages, third-party web renderer, WebEngine dependency, or Sunshine remote shell are required. The interface is a native Qt Quick page compatible with gamepad focus and future upstream merges.

## Features

- Authenticate against Sunshine's web UI using administrator username/password (not Moonlight pairing credentials).
- Load existing non-default options, edit values, reset overrides, or add a new option by Sunshine config key.
- Save settings, save-and-restart, or restart Sunshine without editing.
- Create host-stored named presets, switch-and-restart, and delete presets (requires the corresponding Sunshine fork).

## Important limitations

- **This is an advanced key/value settings editor**, not yet a reproduction of Sunshine's full categorized settings form. `GET /api/config` returns only configured overrides. For a setting at its default value, enter its key manually. Special complex options such as `global_prep_cmd` need careful JSON formatting; use the native Sunshine web UI for these until a typed schema/editor is built.
- No UI action is issued automatically merely because a PC has been paired.
- Saving updates the on-disk configuration; Sunshine must be restarted to apply new settings.
- A preset switch discards unsaved Moonlight-side edits, stages the preset and requests Sunshine restart. Restart can terminate the HTTPS reply and disconnect a stream; no completion is asserted.
- For a custom port-forwarding scheme, the admin port may differ from base HTTP + 1; this first version is designed for direct LAN/VPN operation.
- The host web UI must allow the client's network under `origin_web_ui_allowed` (normally `lan`). Do not open this administration endpoint to the Internet.
- Settings such as transport and encoder defaults can change upstream; avoid duplicating those defaults in Moonlight. Follow-up enhancement: expose a versioned settings schema from Sunshine, then offer typed controls and grouped panels.

## Test matrix before enabling in production

1. Windows Sunshine installed as a service; paired Moonlight from SteamOS Flatpak / Linux, Windows.
2. Correct/incorrect admin credentials and client outside allowed origin policy.
3. TLS interception or certificate replacement is rejected; no credential forwarding on redirects.
4. Edit simple setting, save, reload, restart and verify effect.
5. Create two presets, switch in both directions, validate backup and delete.
6. Switch and restart while a stream is active, recover after host reappears.
7. Original Moonlight streaming, VRR and OSD functionality unchanged.

This integration lives in a feature branch and intentionally does not modify Sunshine or Moonlight's active development branches until validation.
