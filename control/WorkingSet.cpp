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

            // Record the change
            RecordedChange change;
            change.changeId = nextChangeId_++;
            change.operation = "commit_rendered_asset";
            change.resourceId = item.id;
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

WorkingSet::ResolveResult WorkingSet::resolvePronoun(
    std::string_view pronoun,
    const std::string& intentContext) const
{
    ResolveResult result;
    std::lock_guard<std::mutex> lock(mtx_);

    if (items_.empty()) {
        result.errorMessage = "no tracked items in the working set";
        return result;
    }

    // Find the most recently touched item, preferring musical objects
    // (instruments/patterns) over project/song metadata.
    const ItemType prefOrder[] = {
        ItemType::Instrument, ItemType::Pattern,
        ItemType::Session, ItemType::RenderJob,
        ItemType::Song, ItemType::Project
    };

    for (ItemType t : prefOrder) {
        TrackedItem* best = nullptr;
        for (auto& item : items_) {
            if (item.type == t && (!best || item.lastTouched > best->lastTouched))
                best = &item;
        }
        if (best) {
            // Use intent context to disambiguate within the type if needed.
            (void)intentContext;

            nlohmann::json j;
            j["id"] = best->id;
            j["name"] = best->name;
            j["type"] = itemTypeName(best->type);
            j["slot"] = best->slotName;
            j["alias"] = best->alias;
            if (!best->state.is_null())
                j["state"] = best->state;
            result.resolved = std::move(j);
            result.found = true;
            return result;
        }
    }

    result.errorMessage = "no suitable item found for pronoun resolution";
    return result;
}

WorkingSet::ResolveResult WorkingSet::resolveNamedReference(
    const std::string& query,
    const std::string& intentContext) const
{
    ResolveResult result;
    std::lock_guard<std::mutex> lock(mtx_);

    // "the last change" or "the last" → resolve to last reversible change
    if (query.find("last change") != std::string::npos ||
        (query.find("last") != std::string::npos && query.find("change") != std::string::npos)) {
        const auto change = getLastReversibleChange();
        if (change.has_value()) {
            nlohmann::json j;
            j["type"] = "change";
            j["change_id"] = change->changeId;
            j["operation"] = change->operation;
            j["resource_id"] = change->resourceId;
            j["reversible"] = change->reversible;
            j["revert_action"] = change->revertAction;
            if (!change->before.is_null())
                j["before"] = change->before;
            if (!change->after.is_null())
                j["after"] = change->after;
            result.resolved = std::move(j);
            result.found = true;
            return result;
        }
        result.errorMessage = "no reversible change found";
        return result;
    }

    // "same as before" → most recent change
    if (query.find("same as before") != std::string::npos ||
        query.find("same") != std::string::npos) {
        if (!changes_.empty()) {
            const auto& change = changes_.back();
            nlohmann::json j;
            j["type"] = "change";
            j["change_id"] = change.changeId;
            j["operation"] = change.operation;
            j["resource_id"] = change.resourceId;
            j["reversible"] = change.reversible;
            if (!change.after.is_null())
                j["after"] = change.after;
            result.resolved = std::move(j);
            result.found = true;
            return result;
        }
        result.errorMessage = "no previous change found";
        return result;
    }

    // Match by alias, name, or id
    std::vector<TrackedItem*> matches;
    for (auto& item : items_) {
        // Match alias (e.g. "the bass" matches alias "the bass")
        if (!item.alias.empty() &&
            query.find(item.alias) != std::string::npos) {
            matches.push_back(&item);
            continue;
        }
        // Match name (e.g. "acid_bass" or "acid bass")
        if (!item.name.empty() &&
            (query.find(item.name) != std::string::npos ||
             query.find(toLower(item.name)) != std::string::npos)) {
            matches.push_back(&item);
            continue;
        }
        // Match id (e.g. "instrument:acid_bass")
        if (query.find(item.id) != std::string::npos) {
            matches.push_back(&item);
            continue;
        }
        // Match slot name
        if (!item.slotName.empty() && query.find(item.slotName) != std::string::npos) {
            matches.push_back(&item);
            continue;
        }
    }

    // Use intent context to narrow down (e.g. "darker" hints at the most
    // recently modified instrument)
    (void)intentContext;

    if (matches.empty()) {
        result.errorMessage = "no tracked item matches '" + query + "'";
        return result;
    }

    if (matches.size() == 1) {
        TrackedItem* item = matches[0];
        nlohmann::json j;
        j["id"] = item->id;
        j["name"] = item->name;
        j["type"] = itemTypeName(item->type);
        j["slot"] = item->slotName;
        j["alias"] = item->alias;
        if (!item->state.is_null())
            j["state"] = item->state;
        result.resolved = std::move(j);
        result.found = true;
        return result;
    }

    // Multiple matches — surface ambiguity (testing requirement #5)
    result.ambiguous = true;
    for (TrackedItem* item : matches) {
        nlohmann::json j;
        j["id"] = item->id;
        j["name"] = item->name;
        j["type"] = itemTypeName(item->type);
        j["slot"] = item->slotName;
        j["alias"] = item->alias;
        result.candidates.push_back(std::move(j));
    }
    result.errorMessage = "ambiguous reference — multiple candidates match '" +
                          query + "'";
    return result;
}

WorkingSet::ResolveResult WorkingSet::resolveReference(
    std::string_view phrase,
    std::string_view intentContext) const
{
    ResolveResult result;
    std::string query = toLower(std::string(phrase));

    // Strip common leading articles and conversational wrappers
    if (query.rfind("make ", 0) == 0 || query.rfind("use ", 0) == 0 ||
        query.rfind("revert ", 0) == 0 || query.rfind("apply ", 0) == 0) {
        // "make it darker" → resolve "it"
        // "use that instrument" → resolve "that instrument"
        // "revert that" → resolve "that" as a change
        size_t start = 0;
        if (query.rfind("make ", 0) == 0) start = 5;
        else if (query.rfind("use ", 0) == 0) start = 4;
        else if (query.rfind("revert ", 0) == 0) start = 7;
        else if (query.rfind("apply ", 0) == 0) start = 6;
        query = toLower(query.substr(start));
    }

    // Check for "revert" / "undo" / "change" → resolve as a change
    if (query.find("revert") != std::string::npos ||
        query.find("undo") != std::string::npos ||
        query.find("change") != std::string::npos) {
        query = "the last change";
    }

    std::string contextStr = toLower(std::string(intentContext));

    // Check if the query is just a pronoun
    if (isPronoun(query)) {
        return resolvePronoun(query, contextStr);
    }

    // Check for explicit named references
    if (query.find("the bass") != std::string::npos ||
        query.find("bass") != std::string::npos ||
        query.find("instrument") != std::string::npos ||
        query.find("the pattern") != std::string::npos ||
        query.find("the session") != std::string::npos ||
        query.find("the render") != std::string::npos ||
        query.find("the song") != std::string::npos ||
        query.find("that") != std::string::npos ||
        query.find("this") != std::string::npos ||
        query.find("it") != std::string::npos) {
        return resolveNamedReference(query, contextStr);
    }

    // If the query contains a slot name pattern (d0-d15), try slot matching
    if (query.size() >= 2 && query[0] == 'd' &&
        query[1] >= '0' && query[1] <= '9') {
        const std::string slotName = query.substr(0, 2);
        return resolveNamedReference(slotName, contextStr);
    }

    // Fall back: try named reference resolution with the full query
    return resolveNamedReference(query, contextStr);
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
