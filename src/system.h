#pragma once
#include <stdint.h>

// Restart without holding the caller: an HTTP handler wants to send its reply
// first, and the single httpd task must not sit in delay() meanwhile.
void system_restart_in(uint32_t ms);
void system_tick(); // call often (loop()): performs a due restart
