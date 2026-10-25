#pragma once
#include "Controller.h"

// SoftAP + HTTP API. Handlers only enqueue commands or read snapshots.
// Returns false if the access point could not start.
bool webApiBegin(Controller& ctl);

