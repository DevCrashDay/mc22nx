#pragma once
// Switch only: the NRO sets its own clocks (overclock kit rates, stock maximums without a kit; switch_clocks_logic.hpp),
// keeps them while it runs and puts the old ones back at the end.

void switch_clocks_start();    // after sdmc:/mc2/env.txt is read (PS2_CLOCKS)
void switch_clocks_tick();     // often; acts at most once a second (re-applies after dock/undock or a system reset)
void switch_clocks_restore();  // at exit and from the crash handler; idempotent, no allocation
