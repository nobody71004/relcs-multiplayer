// src/net/rnet_eos.h — Epic Online Services product identifiers.
//
// One source of truth for the launcher (EOS bootstrap + Easy Anti-Cheat config),
// the integrity tooling and any future server-side EOS integration. These ids are
// public: they identify the product/sandbox/deployment, not a secret.
#pragma once

namespace rnet {

constexpr char EOS_PRODUCT_ID[]     = "fb155afe4a754a4b91929491873c844e";
constexpr char EOS_SANDBOX_ID[]     = "515e78d9d395488faf0a3f63041dbba0";
constexpr char EOS_DEPLOYMENT_ID[]  = "a287ae738e5241829bdcd1d8a0d3b0bb";
constexpr char EOS_GAME_TITLE[]     = "reLCS";
constexpr char EOS_GAME_EXE[]       = "reLCS.exe";

} // namespace rnet
