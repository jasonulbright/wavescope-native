// Spout output: publish the visual as a shared GPU texture that OBS,
// Resolume, TouchDesigner, etc. ingest live — the desktop analog of the web
// app's `?embed=1` browser-source trick, minus the browser. Toggled with O
// (primary window only; the sender is named "WaveScope"). Implementation is
// the vendored, pinned SpoutDX (third_party/spout — see PIN.md): it owns a
// small D3D11 device and a shared texture, and registers the sender in
// Spout's shared-memory name registry. Frames arrive here as the same
// pre-UI BGRA captures the clip recorder uses, so the cost is only paid
// while the output (or a recording) is live.

#include "../third_party/spout/SpoutDX.h"

struct SpoutOut {
    spoutDX dx;
    bool on = false;
    std::vector<uint8_t> flip;   // top-down staging for bottom-up GL frames

    bool Start() {
        if (on) return true;
        if (!dx.OpenDirectX11()) return false;
        dx.SetSenderName("WaveScope");
        on = true;
        return true;
    }

    void Stop() {
        if (!on) return;
        dx.ReleaseSender();
        dx.CloseDirectX11();
        on = false;
    }

    // BGRA in. Spout's shared texture is top-down; bottom-up sources
    // (GL readbacks) are flipped here.
    void Send(const uint8_t* bgra, int w, int h, bool bottomUp) {
        if (!on || !bgra || w <= 0 || h <= 0) return;
        const uint8_t* src = bgra;
        if (bottomUp) {
            flip.resize((size_t)w * h * 4);
            for (int y = 0; y < h; y++)
                memcpy(&flip[(size_t)y * w * 4],
                       bgra + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
            src = flip.data();
        }
        dx.SendImage(src, (unsigned)w, (unsigned)h);
    }
};
