// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PetdexAttribution.hpp"

#include <cctype>

namespace hathor::ui {

namespace {

std::string trim(const std::string& s)
{
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    auto b = s.begin();
    while (b != s.end() && isSpace(static_cast<unsigned char>(*b)))
        ++b;
    auto e = s.end();
    while (e != b && isSpace(static_cast<unsigned char>(*(e - 1))))
        --e;
    return std::string(b, e);
}

} // anonymous namespace

PetdexAttribution::Info PetdexAttribution::resolve(const PetdexPet& pet)
{
    Info info;
    info.submitter = trim(pet.submittedBy);

    // D4 gate: the manifest's ONLY attribution source is `submittedBy`. A pet
    // is displayable only when that credit can actually be shown. We do NOT
    // claim a license — none is declared anywhere (manifest, pet.json, zip).
    info.canDisplay = !info.submitter.empty();

    if (info.canDisplay)
    {
        info.creditLine = "Submitted by " + info.submitter + " \xC2\xB7 " + kPlatformCredit;
        info.notice     = kNoLicenseNotice;
    }
    else
    {
        info.creditLine.clear();
        info.notice = kMissingAttributionNotice;
    }

    return info;
}

PetdexAttributionSnapshot PetdexAttribution::buildSnapshot(const PetdexPet& pet)
{
    const auto info = resolve(pet);
    PetdexAttributionSnapshot snap;
    snap.canDisplay     = info.canDisplay;
    snap.slug           = pet.slug;
    snap.displayName    = pet.displayName;
    snap.submitter      = info.submitter;
    snap.creditLine     = info.creditLine;
    snap.notice         = info.notice;
    snap.spritesheetUrl = pet.spritesheetUrl;
    return snap;
}

} // namespace hathor::ui
