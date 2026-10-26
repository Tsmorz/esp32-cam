#pragma once

// The two HTTP servers: control (:80 — dashboard, JSON API, OTA) and stream
// (:81 — MJPEG). Routes are listed at the bottom of http_server.cpp.
void http_start();
bool http_running(); // both servers started
