// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * WorkingSet.cpp — AI-10.2: Conversational Memory / Working Set.
 *
 * See WorkingSet.hpp for the full architecture documentation.
 */

#include "WorkingSet.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>

namespace hathor::control {

// ---------------------------------------------------------------------------
// Item type name
// ---------------------------------------------------------------------------

const char* WorkingSet::itemTypeName(ItemType t) noexcept
{
    switch (t) {
        case ItemType::Pattern:    return "pattern";
        case ItemType::Instrument: return "instrument";
        case ItemType::Session:    return "session";
        case ItemType::RenderJob:  return "render_job";
        case ItemType::Song:       return "song";
        case ItemType::Project:    return "project";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string WorkingSet::toLower(std::string_view s) noexcept
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string WorkingSet::classifyMusicalDomain(std::string_view intent) noexcept
{
    const std::string lower = toLower(intent);

    if (lower.find("acid") != std::string::npos ||
        lower.find("bass") != std::string::npos ||
        lower.find("sub") != std::string::npos)
        return "bass";

    if (lower.find("kick") != std::string::npos ||
        lower.find("bd") != std::string::npos)
        return "kick";

    if (lower.find("snare") != std::string::npos ||
        lower.find("sn") != std::string::npos)
        return "snare";

    if (lower.find("hi-hat") != std::string::npos ||
        lower.find("hihat") != std::string::npos ||
        lower.find("hh") != std::string::npos)
        return "hi-hat";

    if (lower.find("lead") != std::string::npos ||
        lower.find("saw") != std::string::npos)
        return "lead";

    if (lower.find("pad") != std::string::npos ||
        lower.find("chord") != std::string::npos)
        return "chord";

    if (lower.find("melody") != std::string::npos)
        return "melody";

    return "";
}

std::string WorkingSet::deriveAlias(const std::string& intentKeywords,
                                     const TrackedItem& item) noexcept
{
    const std::string lower = toLower(intentKeywords);
    const std::string domain = classifyMusicalDomain(lower);

    if (item.type == ItemType::Instrument) {
        if (!domain.empty())
            return "the " + domain;
        return "the instrument";
    }

    if (item.type == ItemType::Pattern) {
        if (!domain.empty())
            return "the " + domain + " pattern";
        return "the pattern";
    }

    if (item.type == ItemType::Session) {
        return "the session";
    }

    if (item.type == ItemType::RenderJob) {
        return "the render";
    }

    return "the " + std::string(itemTypeName(item.type));
}

bool WorkingSet::isPronoun(std::string_view word) noexcept
{
    const std::string lower = toLower(word);
    return lower == "it" || lower == "that" || lower == "this" ||
           lower == "them" || lower == "the last one" ||
           lower == "the last" || lower == "same";
}

// ---------------------------------------------------------------------------
// Item management
// ---------------------------------------------------------------------------

void WorkingSet::recordItem(const TrackedItem& item)
{
    std::lock_guard<std::mutex> lock(mtx_);

    const auto now = std::chrono::steady_clock::now();

    for (auto& existing : items_) {
        if (existing.id == item.id) {
            existing.name = item.name;
            existing.type = item.type;
            existing.slotName = item.slotName;
            existing.alias = item.alias.empty() ? existing.alias : item.alias;
            existing.state = item.state;
            if (item.createdAt.time_since_epoch().count() != 0)
                existing.createdAt = item.createdAt;
            existing.lastTouched = now;
            lastUpdated_ = now;
            return;
        }
    }

    TrackedItem copy = item;
    copy.createdAt = (item.createdAt.time_since_epoch().count() != 0)
                         ? item.createdAt
                         : now;
    copy.lastTouched = now;

    if (copy.alias.empty())
        copy.alias = deriveAlias(lastIntent_, copy);

    items_.push_back(std::move(copy));
    lastUpdated_ = now;
}

void WorkingSet::removeItem(std::string_view itemId)
{
    std::lock_guard<std::mutex> lock(mtx_);

    const auto it = std::find_if(items_.begin(), items_.end(),
        [&](const TrackedItem& i) { return i.id == itemId; });

    if (it != items_.end()) {
        items_.erase(it);
        lastUpdated_ = std::chrono::steady_clock::now();
    }
}

// ---------------------------------------------------------------------------
// Change tracking
// ---------------------------------------------------------------------------

void WorkingSet::recordChange(const RecordedChange& change)
{
    std::lock_guard<std::mutex> lock(mtx_);

    RecordedChange copy = change;
    if (copy.changeId == 0)
        copy.changeId = nextChangeId_++;

    changes_.push_back(std::move(copy));
    lastUpdated_ = std::chrono::steady_clock::now();
}

// ---------------------------------------------------------------------------
// Workflow integration
// ---------------------------------------------------------------------------

void WorkingSet::setLastIntent(std::string intent) noexcept
{
    std::lock_guard<std::mutex> lock(mtx_);
    lastIntent_ = std::move(intent);
}

void WorkingSet::setActiveSlot(std::string slotName) noexcept
{
    std::lock_guard<std::mutex> lock(mtx_);
    activeSlot_ = std::move(slotName);
}

void WorkingSet::updateAfterStep(const std::string& stepName,
                                  const nlohmann::json& stepResult,
                                  bool success)
{
    std::lock_guard<std::mutex> lock(mtx_);
    lastUpdated_ = std::chrono::steady_clock::now();

    if (!success) {
        // Do NOT update the working set with state from a failed operation.
        // Failed operations may have left the system in an indeterminate
        // state.  The working set retains its previous (correct) state.
        // See testing requirement #8.
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const std::string intentLower = toLower(lastIntent_);

    if (stepName == "inspect_project") {
        if (stepResult.contains("project_dir")) {
            currentProjectDir_ = stepResult.value("project_dir", std::string{});
        }
        TrackedItem item;
        item.id = "project:" + currentProjectDir_;
        item.name = stepResult.value("project_name", std::string{"project"});
        item.type = ItemType::Project;
        item.state = stepResult;
        item.createdAt = now;
        // Reuse recordItem logic (append or update)
        bool found = false;
        for (auto& existing : items_) {
            if (existing.id == item.id) {
                existing.state = item.state;
                existing.lastTouched = now;
                found = true;
                break;
            }
        }
        if (!found)
            items_.push_back(std::move(item));

    } else if (stepName == "inspect_song") {
        // Extract active slot and patterns from song info
        if (stepResult.contains("active_patterns")) {
            for (const auto& p : stepResult["active_patterns"]) {
                const std::string slot = p.value("slot", std::string{});
                if (!slot.empty() && slot != activeSlot_)
                    ; // could update active slot tracking
                (void)slot;
            }
        }
        if (stepResult.contains("tempo")) {
            // Track tempo as project state
            currentProjectDir_ = stepResult.value("project_dir",
                                                   currentProjectDir_);
        }
        TrackedItem item;
        item.id = "song:current";
        item.name = "current song";
        item.type = ItemType::Song;
        item.state = stepResult;
        item.createdAt = now;
        bool found = false;
        for (auto& existing : items_) {
            if (existing.id == item.id) {
                existing.state = item.state;
                existing.lastTouched = now;
                found = true;
                break;
            }
        }
        if (!found)
            items_.push_back(std::move(item));

    } else if (stepName == "inspect_assets") {
        // Track samples and instruments found
        if (stepResult.contains("samples")) {
            for (const auto& s : stepResult["samples"]) {
                const std::string name = s.value("name", std::string{});
                TrackedItem item;
                item.id = "sample:" + name;
                item.name = name;
                item.type = ItemType::Instrument;  // samples are instruments (sample-based)
                item.state = s;
                item.createdAt = now;
                bool found = false;
                for (auto& existing : items_) {
                    if (existing.id == item.id) {
                        existing.state = item.state;
                        existing.lastTouched = now;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    item.alias = deriveAlias(intentLower, item);
                    items_.push_back(std::move(item));
                }
            }
        }
        if (stepResult.contains("chuck_instruments")) {
            for (const auto& inst : stepResult["chuck_instruments"]) {
                const std::string name = inst.value("name", std::string{});
                TrackedItem item;
                item.id = "instrument:" + name;
                item.name = name;
                item.type = ItemType::Instrument;
                item.state = inst;
                item.createdAt = now;
                bool found = false;
                for (auto& existing : items_) {
                    if (existing.id == item.id) {
                        existing.state = item.state;
                        existing.lastTouched = now;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    item.alias = deriveAlias(intentLower, item);
                    items_.push_back(std::move(item));
                }
            }
        }

    } else if (stepName == "generate_pattern") {
        if (stepResult.contains("canonical_notation") || stepResult.contains("source")) {
            TrackedItem item;
            item.slotName = stepResult.value("slot", activeSlot_);
            if (stepResult.contains("session_id")) {
                item.id = stepResult["session_id"].get<std::string>();
                item.type = ItemType::Session;
                item.name = "ck_session";
                item.state = stepResult;
            } else if (stepResult.contains("canonical_notation")) {
                item.id = "pattern:" + item.slotName;
                item.type = ItemType::Pattern;
                item.name = item.slotName;
                item.state = stepResult;
            } else {
                item.id = "pattern:" + item.slotName;
                item.type = ItemType::Pattern;
                item.name = item.slotName;
                item.state = stepResult;
            }
            item.createdAt = now;
            item.alias = deriveAlias(intentLower, item);
            items_.push_back(std::move(item));
        }
    } else if (stepName == "compile") {
        if (stepResult.contains("session_id")) {
            TrackedItem item;
            item.id = stepResult["session_id"].get<std::string>();
            item.type = ItemType::Session;
            item.name = "ck_session";
            item.slotName = stepResult.value("slot_name",
                stepResult.value("slot", activeSlot_));
            item.state = stepResult;
            item.createdAt = now;
            bool found = false;
            for (auto& existing : items_) {
                if (existing.id == item.id && existing.type == ItemType::Session) {
                    existing.state = item.state;
                    existing.lastTouched = now;
                    found = true;
                    break;
                }
            }
            if (!found)
                items_.push_back(std::move(item));
        }
    } else if (stepName == "audition") {
        // Audition is non-destructive; we may refine the item's state
        TrackedItem item;
        item.state = stepResult;
        item.createdAt = now;

        if (stepResult.contains("session_id")) {
            item.id = stepResult["session_id"].get<std::string>();
            item.type = ItemType::Session;
            item.name = "ck_session";
            item.slotName = stepResult.value("slot", activeSlot_);
            item.alias = deriveAlias(intentLower, item);
            bool found = false;
            for (auto& existing : items_) {
                if (existing.id == item.id && existing.type == ItemType::Session) {
                    existing.state = item.state;
                    existing.lastTouched = now;
                    found = true;
                    break;
                }
            }
            if (!found)
                items_.push_back(std::move(item));
        } else if (stepResult.contains("slot")) {
            item.id = "pattern:" + stepResult.value("slot", activeSlot_);
            item.type = ItemType::Pattern;
            item.name = stepResult.value("slot", activeSlot_);
            item.slotName = stepResult.value("slot", activeSlot_);
            item.alias = deriveAlias(intentLower, item);
            bool found = false;
            for (auto& existing : items_) {
                if (existing.id == item.id && existing.type == ItemType::Pattern) {
                    existing.state = item.state;
                    existing.lastTouched = now;
                    found = true;
                    break;
                }
            }
            if (!found)
                items_.push_back(std::move(item));
        }
    } else if (stepName == "render") {
        if (stepResult.contains("job_id")) {
            TrackedItem item;
            item.id = "render_job:" + std::to_string(stepResult.value("job_id", 0));
            item.type = ItemType::RenderJob;
            item.name = stepResult.value("asset_name", std::string{"render"});
            item.state = stepResult;
            item.createdAt = now;
            item.alias = "the render";
            items_.push_back(std::move(item));
        }
    } else if (stepName == "bind_asset") {
        if (stepResult.contains("asset_name")) {
            const std::string assetName = stepResult.value("asset_name", std::string{});
            TrackedItem item;
            item.id = "instrument:" + assetName;
            item.name = assetName;
            item.type = ItemType::Instrument;
            item.state = stepResult;
            item.createdAt = now;
            item.alias = deriveAlias(intentLower, item);
            bool found = false;
            for (auto& existing : items_) {
                if (existing.id == item.id) {
                    existing.state = item.state;
                    existing.lastTouched = now;
                    found = true;
                    break;
                }
            }
            if (!found)
                items_.push_back(std::move(item));

            // Record the change (resourceId captured BEFORE item is moved
            // into the vector below, otherwise it reads a moved-from string).
            RecordedChange change;
            change.changeId = nextChangeId_++;
            change.operation = "commit_rendered_asset";
            change.resourceId = "instrument:" + assetName;
            change.slotName = activeSlot_;
            change.after = stepResult;
            change.before = nlohmann::json::object();
            change.before["existed"] = false;  // conservative default
            change.reversible = true;
            change.revertAction = "remove baked asset '" + assetName + "'";
            change.timestamp = now;
            changes_.push_back(std::move(change));
        }
    } else if (stepName == "update_song") {
        // Record the song change for potential revert
        RecordedChange change;
        change.changeId = nextChangeId_++;
        change.operation = "edit_song";
        change.resourceId = "song:" + stepResult.value("song",
            activeSlot_ + ".hathor");
        change.slotName = stepResult.value("target_slot", activeSlot_);
        change.after = stepResult;
        change.before = nlohmann::json::object();
        change.reversible = true;
        change.revertAction = "restore song '" + change.resourceId + "' to previous pattern";
        change.timestamp = now;
        changes_.push_back(std::move(change));

        // Track the song item
        TrackedItem item;
        item.id = change.resourceId;
        item.type = ItemType::Song;
        item.name = stepResult.value("song", std::string{"song"});
        item.slotName = change.slotName;
        item.state = stepResult;
        item.createdAt = now;
        bool found = false;
        for (auto& existing : items_) {
            if (existing.id == item.id) {
                existing.state = item.state;
                existing.lastTouched = now;
                found = true;
                break;
            }
        }
        if (!found)
            items_.push_back(std::move(item));
    }

    // If the step result contains a slot_name or target_slot, update active slot
    const std::string slotVal = stepResult.value("slot",
                              stepResult.value("slot_name",
                                stepResult.value("target_slot", std::string{})));
    if (!slotVal.empty())
        activeSlot_ = slotVal;
}

// ---------------------------------------------------------------------------
// Reference resolution
// ---------------------------------------------------------------------------

std::optional<WorkingSet::ItemType> WorkingSet::typeHintFromText(
    std::string_view text) noexcept
{
    const std::string s = toLower(text);
    if (s.find("instrument") != std::string::npos ||
        s.find("sample") != std::string::npos ||
        s.find("bass") != std::string::npos ||
        s.find("kick") != std::string::npos ||
        s.find("pad") != std::string::npos ||
        s.find("lead") != std::string::npos ||
        s.find("snare") != std::string::npos ||
        s.find("hi-hat") != std::string::npos)
        return ItemType::Instrument;
    if (s.find("pattern") != std::string::npos)
        return ItemType::Pattern;
    if (s.find("session") != std::string::npos)
        return ItemType::Session;
    if (s.find("render") != std::string::npos)
        return ItemType::RenderJob;
    if (s.find("song") != std::string::npos)
        return ItemType::Song;
    if (s.find("project") != std::string::npos)
        return ItemType::Project;
    return std::nullopt;
}

std::optional<WorkingSet::ItemType> WorkingSet::typeHintFromContext(
    std::string_view context) noexcept
{
    const std::string s = toLower(context);
    // Instrument / audio-parameter modifiers.
    if (s.find("dark") != std::string::npos ||
        s.find("bright") != std::string::npos ||
        s.find("warm") != std::string::npos ||
        s.find("cold") != std::string::npos ||
        s.find("loud") != std::string::npos ||
        s.find("soft") != std::string::npos ||
        s.find("gain") != std::string::npos ||
        s.find("filter") != std::string::npos ||
        s.find("distort") != std::string::npos ||
        s.find("eq") != std::string::npos ||
        s.find("tone") != std::string::npos ||
        s.find("attack") != std::string::npos ||
        s.find("decay") != std::string::npos ||
        s.find("resonant") != std::string::npos ||
        s.find("use") != std::string::npos)
        return ItemType::Instrument;
    // Pattern / rhythm modifiers.
    if (s.find("simpl") != std::string::npos ||
        s.find("complex") != std::string::npos ||
        s.find("dens") != std::string::npos ||
        s.find("spars") != std::string::npos ||
        s.find("note") != std::string::npos ||
        s.find("pattern") != std::string::npos ||
        s.find("rhythm") != std::string::npos)
        return ItemType::Pattern;
    return std::nullopt;
}

std::string WorkingSet::extractSlotReference(std::string_view text) noexcept
{
    const std::string s = toLower(text);
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (s[i] == 'd' && s[i + 1] >= '0' && s[i + 1] <= '9') {
            size_t j = i + 1;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9')
                ++j;
            return s.substr(i, j - i);
        }
    }
    return "";
}

bool WorkingSet::itemMatchesDomain(const TrackedItem& item,
                                   const std::string& domain) noexcept
{
    if (domain.empty())
        return false;
    if (!item.alias.empty() &&
        classifyMusicalDomain(item.alias) == domain)
        return true;
    if (!item.name.empty() &&
        classifyMusicalDomain(item.name) == domain)
        return true;
    if (!item.id.empty() &&
        classifyMusicalDomain(item.id) == domain)
        return true;
    return false;
}

WorkingSet::ResolveResult WorkingSet::resolveChangeReference(
    std::string_view kind) const
{
    ResolveResult result;
    std::lock_guard<std::mutex> lock(mtx_);

    if (kind == "same") {
        if (changes_.empty()) {
            result.errorMessage = "no previous change found";
            return result;
        }
        const auto& c = changes_.back();
        nlohmann::json j;
        j["type"] = "change";
        j["change_id"] = c.changeId;
        j["operation"] = c.operation;
        j["resource_id"] = c.resourceId;
        j["reversible"] = c.reversible;
        if (!c.after.is_null())
            j["after"] = c.after;
        result.resolved = std::move(j);
        result.found = true;
        return result;
    }

    // "revert" → the most recent reversible change.
    for (auto it = changes_.rbegin(); it != changes_.rend(); ++it) {
        if (it->reversible) {
            nlohmann::json j;
            j["type"] = "change";
            j["change_id"] = it->changeId;
            j["operation"] = it->operation;
            j["resource_id"] = it->resourceId;
            j["reversible"] = true;
            j["revert_action"] = it->revertAction;
            if (!it->before.is_null())
                j["before"] = it->before;
            if (!it->after.is_null())
                j["after"] = it->after;
            result.resolved = std::move(j);
            result.found = true;
            return result;
        }
    }
    result.errorMessage = "no reversible change found";
    return result;
}

WorkingSet::ResolveResult WorkingSet::resolvePronoun(
    const std::string& query,
    const std::string& intentContext,
    std::optional<ItemType> typeHint,
    const std::string& slot,
    const std::string& domain) const
{
    ResolveResult result;
    std::lock_guard<std::mutex> lock(mtx_);

    (void)intentContext; // only used below to derive an implied type

    if (items_.empty()) {
        result.errorMessage = "no tracked items in the working set";
        return result;
    }

    // If no explicit type word was present, fall back to the type implied by
    // the conversational modifier ("darker" → Instrument, "simpler" → Pattern).
    std::optional<ItemType> type = typeHint;
    if (!type.has_value()) {
        const std::string combined = toLower(intentContext) + " " + toLower(query);
        type = typeHintFromContext(combined);
    }

    std::vector<const TrackedItem*> candidates;
    for (const auto& item : items_) {
        if (type.has_value() && item.type != *type)
            continue;
        if (!slot.empty() && item.slotName != slot)
            continue;
        if (!domain.empty() && !itemMatchesDomain(item, domain))
            continue;
        candidates.push_back(&item);
    }

    if (candidates.empty()) {
        result.errorMessage = "no tracked item matches '" + query + "'";
        return result;
    }

    const auto toJson = [](const TrackedItem& item) {
        nlohmann::json j;
        j["id"] = item.id;
        j["name"] = item.name;
        j["type"] = itemTypeName(item.type);
        j["slot"] = item.slotName;
        j["alias"] = item.alias;
        if (!item.state.is_null())
            j["state"] = item.state;
        return j;
    };

    // A musical domain matching several items is genuinely ambiguous — surface
    // it instead of silently mutating the wrong object (testing requirement #5).
    if (candidates.size() > 1 && !domain.empty()) {
        result.ambiguous = true;
        result.errorMessage =
            "ambiguous reference — multiple candidates match '" + query + "'";
        for (const TrackedItem* item : candidates)
            result.candidates.push_back(toJson(*item));
        return result;
    }

    // Otherwise pick the most recently touched candidate.
    const TrackedItem* best = candidates[0];
    for (const TrackedItem* item : candidates) {
        if (item->lastTouched > best->lastTouched)
            best = item;
    }
    result.resolved = toJson(*best);
    result.found = true;
    return result;
}

WorkingSet::ResolveResult WorkingSet::resolveNamedReference(
    const std::string& query,
    const std::string& intentContext,
    std::optional<ItemType> typeHint,
    const std::string& slot,
    const std::string& domain) const
{
    ResolveResult result;
    std::lock_guard<std::mutex> lock(mtx_);

    (void)intentContext;

    std::vector<const TrackedItem*> candidates;
    for (const auto& item : items_) {
        if (typeHint.has_value() && item.type != *typeHint)
            continue;

        if (!slot.empty()) {
            // Slot-driven named reference: only items on that slot match.
            if (item.slotName == slot)
                candidates.push_back(&item);
            continue;
        }

        bool match = false;
        if (!item.alias.empty() &&
            query.find(item.alias) != std::string::npos)
            match = true;
        else if (!item.name.empty() &&
                 (query.find(item.name) != std::string::npos ||
                  query.find(toLower(item.name)) != std::string::npos))
            match = true;
        else if (!item.id.empty() &&
                 query.find(item.id) != std::string::npos)
            match = true;
        else if (!domain.empty() && itemMatchesDomain(item, domain))
            match = true;

        if (match)
            candidates.push_back(&item);
    }

    if (candidates.empty()) {
        result.errorMessage = "no tracked item matches '" + query + "'";
        return result;
    }

    const auto toJson = [](const TrackedItem& item) {
        nlohmann::json j;
        j["id"] = item.id;
        j["name"] = item.name;
        j["type"] = itemTypeName(item.type);
        j["slot"] = item.slotName;
        j["alias"] = item.alias;
        if (!item.state.is_null())
            j["state"] = item.state;
        return j;
    };

    if (candidates.size() == 1) {
        result.resolved = toJson(*candidates[0]);
        result.found = true;
        return result;
    }

    // Multiple candidates: a slot mention can narrow to a unique target.
    if (!slot.empty()) {
        std::vector<const TrackedItem*> slotCandidates;
        for (const TrackedItem* item : candidates) {
            if (item->slotName == slot)
                slotCandidates.push_back(item);
        }
        if (slotCandidates.size() == 1) {
            result.resolved = toJson(*slotCandidates[0]);
            result.found = true;
            return result;
        }
    }

    result.ambiguous = true;
    result.errorMessage =
        "ambiguous reference — multiple candidates match '" + query + "'";
    for (const TrackedItem* item : candidates)
        result.candidates.push_back(toJson(*item));
    return result;
}

WorkingSet::ResolveResult WorkingSet::resolveReference(
    std::string_view phrase,
    std::string_view intentContext) const
{
    ResolveResult result;
    const std::string raw = toLower(phrase);
    const std::string ctx = toLower(intentContext);

    // --- 1. Change references (evaluated on the FULL phrase, before any
    //        verb/article stripping that would remove the "revert" keyword).
    if (raw.find("revert") != std::string::npos ||
        raw.find("undo") != std::string::npos ||
        raw.find("last change") != std::string::npos) {
        return resolveChangeReference("revert");
    }
    if (raw.find("same") != std::string::npos) {
        return resolveChangeReference("same");
    }

    // --- 2. Object references -----------------------------------------------
    std::string q = raw;

    // Strip common leading verbs / conversational wrappers.
    const std::string prefixes[] = {
        "make ", "use ", "apply ", "edit ", "create ", "add ",
        "delete ", "remove ", "give ", "swap ", "replace "
    };
    for (const auto& p : prefixes) {
        if (q.rfind(p, 0) == 0) {
            q = toLower(q.substr(p.size()));
            break;
        }
    }

    const std::optional<ItemType> typeHint = typeHintFromText(q);
    const std::string slot = extractSlotReference(q + " " + ctx);
    const std::string domain = classifyMusicalDomain(q);

    // Pronoun reference: "it", "that", "this", optionally followed by a
    // qualifier ("it darker", "that instrument", "that bass").
    const bool pronounRef =
        q == "it" || q == "that" || q == "this" || q == "them" ||
        q.rfind("it ", 0) == 0 || q.rfind("that ", 0) == 0 ||
        q.rfind("this ", 0) == 0 ||
        q == "the last one" || q == "the last";

    if (pronounRef)
        return resolvePronoun(q, ctx, typeHint, slot, domain);

    // Named reference.
    return resolveNamedReference(q, ctx, typeHint, slot, domain);
}

// ---------------------------------------------------------------------------
// Undo / revert support
// ---------------------------------------------------------------------------

std::optional<WorkingSet::RecordedChange>
WorkingSet::getLastReversibleChange() const
{
    std::lock_guard<std::mutex> lock(mtx_);

    for (auto it = changes_.rbegin(); it != changes_.rend(); ++it) {
        if (it->reversible)
            return *it;
    }
    return std::nullopt;
}

nlohmann::json WorkingSet::getRevertInfo() const
{
    std::lock_guard<std::mutex> lock(mtx_);

    nlohmann::json j;
    j["has_revertable"] = false;
    j["last_change"] = nullptr;
    j["revert_command"] = nullptr;

    for (auto it = changes_.rbegin(); it != changes_.rend(); ++it) {
        if (it->reversible) {
            j["has_revertable"] = true;
            nlohmann::json changeJson;
            changeJson["change_id"] = it->changeId;
            changeJson["operation"] = it->operation;
            changeJson["resource_id"] = it->resourceId;
            changeJson["slot_name"] = it->slotName;
            changeJson["revert_action"] = it->revertAction;
            changeJson["timestamp"] = it->timestamp.time_since_epoch().count();
            if (!it->before.is_null())
                changeJson["before"] = it->before;
            if (!it->after.is_null())
                changeJson["after"] = it->after;
            j["last_change"] = std::move(changeJson);

            // Construct the canonical revert command
            if (it->operation == "edit_song") {
                // Revert by restoring the previous pattern via edit_song
                nlohmann::json revertCmd;
                revertCmd["cmd"] = "edit_song";
                revertCmd["song_file"] = it->resourceId;
                nlohmann::json ops = nlohmann::json::array();
                if (it->before.contains("operations")) {
                    // Reverse the operations
                    for (auto rit = it->before["operations"].rbegin();
                         rit != it->before["operations"].rend(); ++rit) {
                        nlohmann::json revOp;
                        revOp["op"] = "replace_pattern";
                        if (rit->contains("slot"))
                            revOp["slot"] = rit->value("slot", std::string{});
                        if (rit->contains("notation"))
                            revOp["notation"] = rit->value("notation", std::string{});
                        else if (it->before.contains("notation"))
                            revOp["notation"] = it->before.value("notation", std::string{});
                        revOp["confirm"] = true;
                        ops.push_back(std::move(revOp));
                    }
                }
                revertCmd["ops"] = ops;
                j["revert_command"] = std::move(revertCmd);
            } else if (it->operation == "commit_rendered_asset") {
                nlohmann::json revertCmd;
                revertCmd["cmd"] = "revert_rendered_asset";
                revertCmd["asset_name"] = it->resourceId;
                j["revert_command"] = std::move(revertCmd);
            } else {
                nlohmann::json revertCmd;
                revertCmd["cmd"] = "revert";
                revertCmd["operation"] = it->operation;
                revertCmd["resource_id"] = it->resourceId;
                j["revert_command"] = std::move(revertCmd);
            }

            break;
        }
    }

    return j;
}

// ---------------------------------------------------------------------------
// Serialization / inspection
// ---------------------------------------------------------------------------

nlohmann::json WorkingSet::toJson() const
{
    std::lock_guard<std::mutex> lock(mtx_);

    nlohmann::json j;
    j["ok"] = true;
    j["type"] = "working_set";

    // Tracked items
    nlohmann::json itemsArr = nlohmann::json::array();
    for (const auto& item : items_) {
        nlohmann::json ij;
        ij["id"] = item.id;
        ij["name"] = item.name;
        ij["type"] = itemTypeName(item.type);
        ij["slot"] = item.slotName;
        ij["alias"] = item.alias;
        if (!item.state.is_null())
            ij["state"] = item.state;
        ij["created_at"] = item.createdAt.time_since_epoch().count();
        ij["last_touched"] = item.lastTouched.time_since_epoch().count();
        itemsArr.push_back(std::move(ij));
    }
    j["items"] = std::move(itemsArr);

    // Recorded changes
    nlohmann::json changesArr = nlohmann::json::array();
    for (const auto& change : changes_) {
        nlohmann::json cj;
        cj["change_id"] = change.changeId;
        cj["operation"] = change.operation;
        cj["resource_id"] = change.resourceId;
        cj["slot_name"] = change.slotName;
        cj["reversible"] = change.reversible;
        cj["revert_action"] = change.revertAction;
        cj["timestamp"] = change.timestamp.time_since_epoch().count();
        if (!change.before.is_null())
            cj["before"] = change.before;
        if (!change.after.is_null())
            cj["after"] = change.after;
        changesArr.push_back(std::move(cj));
    }
    j["changes"] = std::move(changesArr);

    j["last_intent"] = lastIntent_;
    j["active_slot"] = activeSlot_;
    j["project_dir"] = currentProjectDir_;
    j["reconciled"] = reconciled_;
    j["last_updated"] = lastUpdated_.time_since_epoch().count();

    return j;
}

void WorkingSet::clear() noexcept
{
    std::lock_guard<std::mutex> lock(mtx_);
    items_.clear();
    changes_.clear();
    lastIntent_.clear();
    activeSlot_.clear();
    currentProjectDir_.clear();
    nextChangeId_ = 1;
    reconciled_ = true;
    lastUpdated_ = std::chrono::steady_clock::now();
}

void WorkingSet::reconcile(const nlohmann::json& projectState)
{
    std::lock_guard<std::mutex> lock(mtx_);

    // Build a set of canonical IDs that exist in the authoritative project state.
    std::vector<std::string> validIds;

    if (projectState.contains("instruments")) {
        for (const auto& inst : projectState["instruments"]) {
            const std::string name = inst.value("name", std::string{});
            if (!name.empty())
                validIds.push_back("instrument:" + name);
        }
    }

    if (projectState.contains("samples")) {
        for (const auto& s : projectState["samples"]) {
            const std::string name = s.value("name", std::string{});
            if (!name.empty())
                validIds.push_back("sample:" + name);
        }
    }

    if (projectState.contains("active_patterns")) {
        for (const auto& p : projectState["active_patterns"]) {
            const std::string slot = p.value("slot", std::string{});
            if (!slot.empty()) {
                validIds.push_back("pattern:" + slot);
                if (slot == activeSlot_)
                    reconciled_ = true;
            }
        }
    }

    // Remove items whose canonical IDs are no longer in the project state.
    // We keep changes (they are historical records) but mark stale items.
    items_.erase(
        std::remove_if(items_.begin(), items_.end(),
            [&](const TrackedItem& item) {
                // Project and Song items are always kept (they represent the
                // overall session context, not individual assets).
                if (item.type == ItemType::Project || item.type == ItemType::Song)
                    return false;

                bool valid = false;
                for (const auto& id : validIds) {
                    if (item.id == id ||
                        (item.type == ItemType::Pattern &&
                         ("pattern:" + item.slotName) == id)) {
                        valid = true;
                        break;
                    }
                }
                if (!valid) {
                    // Item is stale — log to stderr (canonical audit pattern).
                    std::fprintf(stderr,
                        "[AI-10.2 AUDIT] reconcile: removing stale item %s\n",
                        item.id.c_str());
                }
                return !valid;
            }),
        items_.end());

    lastUpdated_ = std::chrono::steady_clock::now();
}

} // namespace hathor::control
