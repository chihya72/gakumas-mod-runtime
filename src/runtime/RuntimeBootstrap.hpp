#pragma once

namespace GakumasMod::Bootstrap {
    // Starts runtime initialization from an ordinary XInput call, after the
    // Windows loader lock has been released. Safe and idempotent for every
    // proxy export to call.
    void EnsureStarted() noexcept;
}
