/*
 * Stoatworks Labs - About window data for Wheel.
 *
 * PROVISIONAL HAND COPY, in the shape stoatworks-backend/scripts/sync-about.py
 * generates. The project is not yet in the website's projects.json, so the
 * sync does not know it; once it is registered, re-run the sync and this file
 * is overwritten. `guide` is empty because no user guide exists, so the About
 * block has three buttons rather than four.
 *
 * `version` here is a fallback read from this repo's own manifest at sync
 * time. Anything with a build step injects the real one at build time and
 * overrides this.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "Wheel";
    inline constexpr auto slug = "wheel";
    inline constexpr auto hook = "A single-chip DLP projector, rainbow effect and all, for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "https://stoatworks-labs.com/software/wheel/";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/wheel";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}
