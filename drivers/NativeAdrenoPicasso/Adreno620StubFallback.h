// SPDX-License-Identifier: MIT
/* Round 10: when the CPU stub may be published late.
 *
 * Diagnosis (bootlog9, 240 s): the AGXAccelerator is registered before launchd
 * and advertises MetalPluginName, yet no process ever reads it (name_reads 0)
 * and no process ever opens the service (opens 0, foreign 0). The render
 * server therefore never gets a renderer: sheng-gpu-metal=1 suppresses the CPU
 * stub, so when the Metal plugin path never activates CoreAnimation has no
 * software fallback either (wf132's lock screen ran on that fallback, with
 * only the five setup GPU submissions). The stub advertises no
 * MetalPluginName, so Metal discovery skips it; publishing it late cannot
 * shadow the AGX plugin, and it gives the software path back.
 *
 * Pure decision, no kernel dependency, so the host tests can exercise it. */
#pragma once

namespace picasso {

/* Publish the stub once the Metal window has passed with no plugin open:
 * stub not yet published, no plugin connection at all, the stub not opted out
 * with picasso-accel-stub=0, and at least one late round elapsed (round 1 is
 * ~210 s after CP bring-up: 20 s + 40 s + 120 s + 30 s). */
inline bool stubFallbackDue(bool stubPublished, unsigned opens, bool stubOptOut, unsigned round)
{
    return !stubPublished && opens == 0 && !stubOptOut && round >= 1;
}

}  // namespace picasso
