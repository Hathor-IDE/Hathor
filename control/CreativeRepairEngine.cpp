// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CreativeRepairEngine.hpp"

#include "SongMutationService.hpp"
#include "ChuckSessionService.hpp"
#include "WorkingSet.hpp"

#include "../ui/HathorFileParser.hpp"
#include "../engine/include/hathor/MiniParser.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace hathor::control {

// ---------------------------------------------------------------------------
// RepairOp::toJson
// ---------------------------------------------------------------------------

nlohmann::json CreativeRepairEngine::RepairOp::toJson() const
{
    return nlohmann::json{
        {"op", op},
        {"service", service},
        {"method", method},
        {"capability_class", capabilityClassName(capabilityClass)},
        {"requires_confirmation", requiresConfirmation},
        {"description", description},
        {"params", params}
    };
}

// ---------------------------------------------------------------------------
// RepairPlan::toJson
// ---------------------------------------------------------------------------

nlohmann::json CreativeRepairEngine::RepairPlan::toJson() const
{
    nlohmann::json opsJson = nlohmann::json::array();
    for (const auto& op : ops)
        opsJson.push_back(op.toJson());

    return nlohmann::json{
        {"feedback", feedback},
        {"property", propertyToString(property)},
        {"target_domain", domainToString(targetDomain)},
        {"slot_name", slotName},
        {"resource_id", resourceId},
        {"explanation", explanation},
        {"ops", opsJson},
        {"needs_confirmation", needsConfirmation}
    };
}

// ---------------------------------------------------------------------------
// CreativeRepairEngine::Classification helpers
// ---------------------------------------------------------------------------

static bool containsKeyword(const std::string& haystack, const std::string& keyword)
{
    return haystack.find(keyword) != std::string::npos;
}

static bool containsAnyKeyword(const std::string& haystack,
                               const std::vector<std::string>& keywords)
{
    for (const auto& kw : keywords) {
        if (haystack.find(kw) != std::string::npos)
            return true;
    }
    return false;
}

CreativeRepairEngine::Classification
CreativeRepairEngine::classifyFeedback(std::string_view feedback) const noexcept
{
    const std::string lower = toLower(feedback);
    Classification result;
    result.property = Property::Unknown;
    result.domain = TargetDomain::Unknown;

    // --- Timbral darkness / brightness ---
    // Darkness keywords take precedence for clarity (both "darker" and "brighter"
    // could appear, but "darker" is the primary signal for darkness).
    static const std::vector<std::string> dark_kw = {"darker", "warmer", "muffled", "dull", "dark"};
    static const std::vector<std::string> bright_kw = {"brighter", "sharper", "edgier", "present", "bright"};

    if (containsAnyKeyword(lower, dark_kw)) {
        result.property = Property::TimbralDarkness;
        result.domain = TargetDomain::Instrument;
        result.explanation = "feedback targets timbral darkness (lower filter cutoff / warmer tone)";
        return result;
    }
    if (containsAnyKeyword(lower, bright_kw)) {
        result.property = Property::TimbralBrightness;
        result.domain = TargetDomain::Instrument;
        result.explanation = "feedback targets timbral brightness (raise filter cutoff / crisper tone)";
        return result;
    }

    // --- Loudness ---
    static const std::vector<std::string> loud_kw = {"louder", "quieter", "punchier", "softer", "punch"};
    if (containsAnyKeyword(lower, loud_kw)) {
        result.property = Property::Loudness;
        result.domain = TargetDomain::Instrument;
        result.explanation = "feedback targets loudness (gain adjustment)";
        return result;
    }

    // --- Stereo spread ---
    static const std::vector<std::string> wide_kw = {"spacious", "wider", "narrower", "mono", "wide"};
    if (containsAnyKeyword(lower, wide_kw)) {
        result.property = Property::StereoSpread;
        result.domain = TargetDomain::Instrument;
        result.explanation = "feedback targets stereo spread (pan / width adjustment)";
        return result;
    }

    // --- Timing ---
    static const std::vector<std::string> timing_kw = {"tighter", "looser", "swing", "off-grid", "tight", "relaxed"};
    if (containsAnyKeyword(lower, timing_kw)) {
        result.property = Property::Timing;
        result.domain = TargetDomain::Pattern;
        result.explanation = "feedback targets timing feel (swing / quantisation)";
        return result;
    }

    // --- Pitch ---
    static const std::vector<std::string> pitch_kw = {"higher", "lower", "melancholy", "bass", "treble"};
    if (containsAnyKeyword(lower, pitch_kw)) {
        result.property = Property::Pitch;
        result.domain = TargetDomain::Instrument;
        result.explanation = "feedback targets pitch / register (transpose)";
        return result;
    }

    // --- Timbral character ---
    static const std::vector<std::string> char_kw = {"aggressive", "analog", "rough", "smooth", "harsh", "warm"};
    if (containsAnyKeyword(lower, char_kw)) {
        result.property = Property::TimbralCharacter;
        result.domain = TargetDomain::Instrument;
        result.explanation = "feedback targets timbral character (synth shape / distortion)";
        return result;
    }

    // --- Rhythmic density ---
    // Checked last because some density-related words could overlap with timing.
    static const std::vector<std::string> density_kw = {"busy", "simpler", "simple", "sparse", "simplify",
                                                         "complex", "cleaner", "less going", "fewer notes"};
    if (containsAnyKeyword(lower, density_kw)) {
        result.property = Property::RhythmicDensity;
        result.domain = TargetDomain::Pattern;
        result.explanation = "feedback targets rhythmic density (pattern sparsity)";
        return result;
    }

    return result;
}

// ---------------------------------------------------------------------------
// CreativeRepairEngine::resolveTarget
// ---------------------------------------------------------------------------

nlohmann::json CreativeRepairEngine::resolveTarget(
    std::string_view phrase,
    std::string_view intentContext) const
{
    WorkingSet::ResolveResult result =
        workingSet_.resolveReference(phrase, intentContext);

    nlohmann::json j;
    j["found"] = result.found;
    j["ambiguous"] = result.ambiguous;
    if (result.found && !result.ambiguous && !result.resolved.is_null()) {
        j["resolved"] = result.resolved;
    }
    if (!result.candidates.empty())
        j["candidates"] = result.candidates;
    if (!result.errorMessage.empty())
        j["error"] = result.errorMessage;
    return j;
}

// ---------------------------------------------------------------------------
// CreativeRepairEngine::toLower
// ---------------------------------------------------------------------------

std::string CreativeRepairEngine::toLower(std::string_view s) noexcept
{
    std::string result(s);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// ---------------------------------------------------------------------------
// CreativeRepairEngine::extractNotationFromHathorContent
// ---------------------------------------------------------------------------

std::string CreativeRepairEngine::extractNotationFromHathorContent(
    std::string_view content) noexcept
{
    // Try to parse as a .hathor file (front-matter + body).
    auto parsed = hathor::ui::parseHathorFile(content);
    if (const auto* hf = std::get_if<hathor::ui::HathorFile>(&parsed)) {
        if (!hf->body.empty())
            return hf->body;
    }

    // Fallback: treat the entire content as body.
    std::string trimmed(content);
    // Strip leading/trailing whitespace
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
        trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
        trimmed.pop_back();
    return trimmed;
}

// ---------------------------------------------------------------------------
// CreativeRepairEngine::generatePatternDensityRepair
// ---------------------------------------------------------------------------

std::string CreativeRepairEngine::generatePatternDensityRepair(
    std::string_view currentNotation,
    std::string_view feedback) const noexcept
{
    // Strategy: wrap the existing notation in a combinator that reduces density.
    // We DO NOT rewrite the notation from scratch — we reuse the existing pattern
    // and apply the smallest transformation.

    const std::string lower = toLower(feedback);

    // "keep the sound but simplify the rhythm" / "simplify" → degradeBy (random
    // thinning preserves the sound character while reducing rhythmic density).
    if (containsKeyword(lower, "simplify") || containsKeyword(lower, "rhythm")) {
        // Wrap in degradeBy(0.3) — drop 30% of events randomly.
        return "degradeBy(0.3, " + std::string(currentNotation) + ")";
    }

    // "too busy" / "simpler" / "cleaner" → slow(2) (halve the density by
    // stretching the pattern to twice its length).
    // This is the smallest change that halves event density per cycle.
    return "slow(2, " + std::string(currentNotation) + ")";
}

// ---------------------------------------------------------------------------
// CreativeRepairEngine::generateChuckRepair
// ---------------------------------------------------------------------------

std::string CreativeRepairEngine::generateChuckRepair(
    std::string_view currentSource,
    Property prop,
    std::string_view /*feedback*/) const noexcept
{
    // Strategy: find and adjust specific parameter assignments in the ChucK
    // source text using a simple, robust scan.  If the parameter is not
    // found, augment the source with an inline adjustment line.
    std::string source(currentSource);

    // Helper: scan source for `<number> => ...<paramName>` assignments and
    // multiply the numeric value by `factor`.  Returns true if at least one
    // adjustment was made.
    auto adjustParam = [&source](const std::string& paramName, double factor) -> bool {
        bool found = false;
        std::size_t searchFrom = 0;

        while ((searchFrom = source.find("=>", searchFrom)) != std::string::npos) {
            std::size_t afterArrow = searchFrom + 2;
            // Skip whitespace after =>
            while (afterArrow < source.size() &&
                   std::isspace(static_cast<unsigned char>(source[afterArrow])))
                ++afterArrow;

            // Check if paramName appears right after => (with optional obj. prefix)
            std::size_t checkPos = afterArrow;
            // Skip optional object name + dot (e.g. "lpf." before "freq")
            while (checkPos < source.size() &&
                   !std::isspace(static_cast<unsigned char>(source[checkPos])) &&
                   source[checkPos] != '\n') {
                if (source.compare(checkPos, paramName.size(), paramName) == 0) {
                    // Verify we're at a word boundary
                    bool boundary = (checkPos + paramName.size() >= source.size()) ||
                        std::isspace(static_cast<unsigned char>(
                            source[checkPos + paramName.size()])) ||
                        source[checkPos + paramName.size()] == ';';
                    if (boundary)
                        break;
                }
                // Skip to next potential match after this character
                ++checkPos;
            }

            if (checkPos < source.size() &&
                source.compare(checkPos, paramName.size(), paramName) == 0) {
                // Find the number before =>
                std::size_t numEnd = searchFrom;
                // Skip whitespace backwards from =>
                while (numEnd > 0 &&
                       std::isspace(static_cast<unsigned char>(source[numEnd - 1])))
                    --numEnd;
                // Scan backwards for a numeric token
                std::size_t numStart = numEnd;
                bool foundDigit = false;
                while (numStart > 0) {
                    char c = source[numStart - 1];
                    if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' ||
                        c == '-' || c == '+') {
                        --numStart;
                        if (std::isdigit(static_cast<unsigned char>(c)))
                            foundDigit = true;
                    } else {
                        break;
                    }
                }
                    if (foundDigit && numStart < numEnd) {
                     std::string numStr = source.substr(numStart, numEnd - numStart);
                     try {
                         double val = std::stod(numStr);
                         double adjusted = val * factor;
                         std::string adjustedStr;
                         if (adjusted == static_cast<int>(adjusted))
                             adjustedStr = std::to_string(static_cast<int>(adjusted));
                         else
                             adjustedStr = std::to_string(adjusted);
                         source.replace(numStart, numEnd - numStart, adjustedStr);
                         found = true;
                         // Advance past the adjusted number and the => token
                         // to avoid re-processing the same assignment.
                         // After replacement, the => that was at searchFrom
                         // has shifted by (adjustedStr.size() - (numEnd - numStart)).
                         searchFrom = numStart + adjustedStr.size() +
                             (searchFrom - numEnd) + 2; // +2 for =>
                     } catch (...) {}
                 }
            }
            ++searchFrom;
        }
        return found;
    };

    bool adjusted = false;

    switch (prop) {
        case Property::TimbralDarkness:
            // Lower the cutoff frequency of any filter.
            if (adjustParam("freq", 0.7)) {
                adjusted = true;
            } else {
                // No filter freq found — append a low-pass filter inline.
                source += "\n600 => LPF lpf => g;  // repair: darken (low-pass)";
                adjusted = true;
            }
            break;

        case Property::TimbralBrightness:
            if (adjustParam("freq", 1.4)) {
                adjusted = true;
            } else {
                source += "\n8000 => LPF lpf => g;  // repair: brighten";
                adjusted = true;
            }
            break;

        default:
            // Other properties are not handled via ChucK source text in this
            // initial implementation.  The plan will surface this to the user.
            break;
    }

    (void)adjusted;
    return source;
}

// ---------------------------------------------------------------------------
// CreativeRepairEngine::planRepair
// ---------------------------------------------------------------------------

CreativeRepairEngine::RepairPlan CreativeRepairEngine::planRepair(
    std::string_view feedback,
    std::string_view intentContext)
{
    RepairPlan plan;
    plan.feedback = std::string(feedback);

    // Step 1: Classify the feedback.
    Classification classification = classifyFeedback(feedback);
    plan.property = classification.property;
    plan.targetDomain = classification.domain;

    if (classification.property == Property::Unknown) {
        plan.explanation = "Could not classify feedback: \"" + std::string(feedback) + "\"";
        plan.needsConfirmation = false;
        return plan;
    }

    // Step 2: Resolve the target via WorkingSet.
    // The feedback itself may contain a reference ("darker bass"), or we use
    // the active slot from the working set.
    nlohmann::json resolution = resolveTarget(feedback, intentContext);

    if (!resolution.value("found", false)) {
        // Fall back to the active slot.
        nlohmann::json wsJson = workingSet_.toJson();
        std::string activeSlot = wsJson.value("active_slot", std::string{});

        if (activeSlot.empty()) {
            plan.explanation = "No target resolved for feedback. Provide a slot name or "
                               "ensure a workflow has been run first.";
            return plan;
        }
        plan.slotName = activeSlot;
        plan.resourceId = "slot:" + activeSlot;
    } else {
        // Extract resolved item info.
        nlohmann::json resolved = resolution.value("resolved", nlohmann::json{});
        if (resolved.is_object()) {
         plan.slotName = resolved.value("slot", std::string{});
         plan.resourceId = resolved.value("id", std::string{});
            std::string itemType = resolved.value("type", std::string{});
            if (itemType == "pattern")
                plan.targetDomain = TargetDomain::Pattern;
             else if (itemType == "instrument" || itemType == "session") {
                 plan.targetDomain = TargetDomain::Instrument;
                 plan.sessionId = resolved.value("state", nlohmann::json{})
                                      .value("session_id", std::string{});
                 if (plan.sessionId.empty())
                     plan.sessionId = resolved.value("id", std::string{});
            }
        }
    }

    // If the classification said the domain is Unknown but we resolved a
    // specific target type, trust the resolution.
    if (plan.targetDomain == TargetDomain::Unknown && !plan.slotName.empty()) {
        plan.targetDomain = TargetDomain::Pattern;
    }

    // Step 3 + 4: Generate the targeted mutation and package as ops.
    if (plan.targetDomain == TargetDomain::Pattern) {
        // --- Pattern repair ---
        const std::string songFile = plan.slotName + ".hathor";

        // Read current content via SongMutationService (canonical read path).
        nlohmann::json readResult = songService_.readSongContent(songFile);
        if (!readResult.value("ok", false)) {
            // No .hathor file yet — check the working set for the notation.
            nlohmann::json wsJson = workingSet_.toJson();
            if (const auto& items = wsJson.value("items", nlohmann::json::array());
                !items.empty()) {
                for (const auto& item : items) {
                     std::string id = item.value("id", std::string{});
                     if (id.find(plan.slotName) != std::string::npos ||
                         item.value("slot", std::string{}) == plan.slotName) {
                         plan.targetNotation = item.value("state", nlohmann::json{})
                                                   .value("canonical_notation", std::string{});
                         break;
                     }
                 }
            }

            if (plan.targetNotation.empty()) {
                plan.explanation = "No existing pattern found for slot '" + plan.slotName +
                                   "' to repair.";
                return plan;
            }
        } else {
            std::string content = readResult.value("content", std::string{});
            plan.targetNotation = extractNotationFromHathorContent(content);
        }

        if (plan.targetNotation.empty()) {
            plan.explanation = "Pattern on slot '" + plan.slotName +
                               "' is empty — nothing to repair.";
            return plan;
        }

        // Generate the smallest targeted mutation.
        std::string newNotation;
        std::string opName;
        std::string opPosition = "replace";
        nlohmann::json opParams;

        if (plan.property == Property::RhythmicDensity) {
            newNotation = generatePatternDensityRepair(plan.targetNotation, feedback);
            opName = "insert";
            opParams["notation"] = newNotation;
            opParams["position"] = "replace";
            opParams["song_file"] = songFile;
        } else if (plan.property == Property::Timing) {
            // Timing repairs for patterns: adjust the slot's BPM or add a
            // timing modifier.  For now, we note that timing is better handled
            // at the engine level (BPM/swing), so we produce a meta change.
            newNotation = plan.targetNotation;
            opName = "insert";
            opParams["notation"] = newNotation;
            opParams["position"] = "replace";
            opParams["song_file"] = songFile;
        } else {
            // Other properties (loudness, timbre) for patterns are handled
            // via the instrument session, not the pattern notation.
            plan.explanation = "Feedback property '" + std::string(propertyToString(plan.property)) +
                               "' cannot be repaired on a pattern directly. " +
                               "Consider targeting the instrument session instead.";
            return plan;
        }

        plan.targetNotation = newNotation;

        RepairOp op;
        op.op = opName;
        op.service = "SongMutationService";
        op.method = "editSong";
        op.capabilityClass = CapabilityClass::PersistentMutation;
        op.requiresConfirmation = true;  // persistent mutation
        op.description = "Apply " + std::string(propertyToString(plan.property)) +
                         " repair to pattern on " + plan.slotName +
                         " (wrap in combinator — reuses existing pattern)";
        op.params = opParams;
        plan.ops.push_back(std::move(op));
        plan.needsConfirmation = true;

        plan.explanation = classification.explanation + ". " +
                           "Wrapping existing pattern on " + plan.slotName +
                           " in a targeted combinator — no regeneration, no new assets.";

    } else if (plan.targetDomain == TargetDomain::Instrument) {
        // --- ChucK instrument repair ---
        if (plan.sessionId.empty()) {
            plan.explanation = "No ChucK session resolved for creative repair. "
                               "Ensure an instrument workflow has been run first.";
            return plan;
        }

        // Read current ChucK source via ChuckSessionService.
        std::string currentSource = chuckService_.getSessionSource(plan.sessionId);
        if (currentSource.empty()) {
            plan.explanation = "No ChucK source found for session '" + plan.sessionId +
                               "'. Cannot apply timbral repair.";
            return plan;
        }

        // Generate the smallest targeted source mutation.
        plan.targetSource = generateChuckRepair(currentSource, plan.property, feedback);

        // Only proceed if the source actually changed.
        if (plan.targetSource == currentSource) {
            plan.explanation = "No applicable parameter found in the ChucK source for "
                               "'" + std::string(propertyToString(plan.property)) + "'.";
            return plan;
        }

        RepairOp op;
        op.op = "compile_chuck";
        op.service = "ChuckSessionService";
        op.method = "compileChuck";
        op.capabilityClass = CapabilityClass::NonDestructive;
        op.requiresConfirmation = false;  // recompile is non-destructive
        op.description = "Apply " + std::string(propertyToString(plan.property)) +
                         " repair to ChucK instrument on session " + plan.sessionId +
                         " (parameter adjustment — reuses existing instrument)";
        op.params["session_id"] = plan.sessionId;
        op.params["source"] = plan.targetSource;
        op.params["audition"] = true;
        plan.ops.push_back(std::move(op));
        plan.needsConfirmation = false;

        plan.explanation = classification.explanation + ". " +
                           "Adjusting parameters in existing ChucK source on " + plan.sessionId +
                           " — no regeneration, no new assets.";

    } else {
        plan.explanation = "Could not resolve a target for creative repair of '" +
                           std::string(feedback) + "'. " +
                           classification.explanation;
    }

    return plan;
}

// ---------------------------------------------------------------------------
// CreativeRepairEngine::toJson (plan)
// ---------------------------------------------------------------------------

nlohmann::json CreativeRepairEngine::toJson(const RepairPlan& plan) const
{
    return plan.toJson();
}

} // namespace hathor::control
