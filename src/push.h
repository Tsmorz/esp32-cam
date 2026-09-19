#pragma once
#include "detection_types.h"
#include <freertos/FreeRTOS.h>

// Detections pushed over UDP, for a robot that shouldn't have to poll HTTP.
// A client subscribes by sending "sub <API_TOKEN>" to PUSH_PORT and refreshing
// it at least every 10 s ("bye <API_TOKEN>" unsubscribes); anything else is
// ignored, and with no API_TOKEN set nothing can subscribe (see
// push_message.h). From then on every processed frame
// arrives as one compact JSON datagram at the sender's address — the same
// fields as /detections without corners and camera model (see
// detections_json.h), including the capture time and the IMU orientation then.
static constexpr int PUSH_PORT = 5005;

void push_start(BaseType_t core, UBaseType_t prio); // subscription listener
void push_frame(const DetectionFrame &f); // send to subscribers (detector task)
int push_subscribers();
