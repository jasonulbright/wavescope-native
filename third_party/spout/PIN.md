# Spout2 (vendored, pinned)

- **What**: the Spout SDK's DirectX-only subset — `spoutDX` sender/receiver
  plus its core support classes (sender-name registry, shared memory, frame
  counting, DX11 device/texture helpers, pixel copy, utils). No OpenGL
  classes are vendored.
- **Why**: OBS (and Resolume/TouchDesigner/etc.) ingest Spout senders as
  shared GPU textures — the desktop analog of the web app's `?embed=1`.
  The share protocol (named shared-memory registry + shared-texture
  handles + access mutex + frame semaphore) is an ecosystem convention,
  not a documented standard; hand-rolling it would risk silent receiver
  incompatibility, which is exactly when DEPENDENCIES.md says to vendor.
- **Version**: 2.007.017, commit `f49e2f469f8cb25f559a6eaa61a3f5b8173fc100`,
  from github.com/leadedge/Spout2 (tag `2.007.017`).
- **License**: BSD-2-Clause (`LICENSE.txt` here, verbatim from the SDK).
- **Local changes**: none. Update by copying the same file set from a newer
  tag and changing this pin.
- **Audit surface added**: in-process D3D11 device + shared texture,
  named shared memory (`SpoutSenderNames` maps), a named mutex and
  semaphore per sender. Activated only while Spout output is toggled on;
  no network, no file parsing.
