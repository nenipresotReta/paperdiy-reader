#pragma once
#include <Arduino.h>
#include "ui.h"

void networkApply(NetworkMode mode);
void networkStop();
bool networkPoll();
bool networkDeliveryChanged();
bool networkApExpired();
// On-demand radio activity suppresses manual light sleep while serving requests.
bool networkActive();
// Call before unmounting/remounting SD; aborts an incomplete transfer.
void networkStorageChanged();
