#pragma once

// The program on the board, written in main/app.c. SDP starts it once in its own task at boot, before a new
// build is confirmed, so a build whose program crashes at start-up rolls the board back to the previous one.
void app_start(void);
