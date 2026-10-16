#pragma once
// <switch> child selection: first child whose conditions hold.
// Static best-effort: no timeline, no scripting, no network, no events.
#include <string>

namespace pittore::svg {

// One child's conditional attributes (raw strings, empty = absent).
struct SwitchCond {
    std::string requiredFeatures;
    std::string requiredExtensions;
    std::string systemLanguage;
};

// True when every listed condition holds. Unknown feature URIs fail;
// any requiredExtension fails; systemLanguage matches BCP47 ranges against
// systemLocale (empty locale matches nothing listed).
bool switchChildOk(const SwitchCond& cond, const std::string& systemLocale);

}  // namespace pittore::svg
