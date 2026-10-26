// net/launcher/eos_bootstrap.h — Epic Online Services bootstrap for the launcher.
//
// The launcher owns the EOS platform for the client install: it initialises the
// SDK with the product/sandbox/deployment ids, logs the machine in through the
// Connect Device ID pseudo-account, and publishes the bootstrap state so the
// launcher UI can show every step (the E2E runbook checks `eos-bootstrap.log`).
#pragma once

#include <string>

namespace eosboot {

struct Status {
	bool sdkLoaded = false;       // EOS_Initialize succeeded
	bool platformReady = false;   // EOS_Platform_Create succeeded
	bool loggedIn = false;        // Connect login completed
	std::string version;          // SDK version string
	std::string productUserId;    // Connect PUID once logged in
	std::string displayName;      // the device-account display name
	std::string error;            // first fatal error, if any
	std::string steps;            // '|'-separated finished steps, in order
	std::string pendingStep;      // the step currently in flight
};

// Starts the bootstrap on a worker thread. Safe to call once; later calls are
// ignored while a bootstrap is live. The client credentials come from the dev
// portal (Product → Clients); EOS 1.19 refuses platform creation without them.
void Start(const std::string& productId, const std::string& sandboxId,
           const std::string& deploymentId, const std::string& clientId,
           const std::string& clientSecret);

// Stops the worker and shuts the platform down.
void Stop();

// Thread-safe snapshot of the current state.
Status Get();

// JSON object for the launcher UI (lcs://eosstatus).
std::string Json();

} // namespace eosboot
