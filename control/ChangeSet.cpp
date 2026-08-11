// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChangeSet.cpp — AI-10.3 implementation.
 *
 * Requirement references: AI-10.3, AI-10, AI-7, PROGRAM.md §1421
 */

#include "ChangeSet.hpp"

#include <sstream>

namespace hathor::control {

const char* changeSetStatusName(ChangeSetStatus s) noexcept
{
    switch (s) {
        case ChangeSetStatus::Pending:   return "pending";
        case ChangeSetStatus::Accepted:  return "accepted";
        case ChangeSetStatus::Rejected:  return "rejected";
        case ChangeSetStatus::Undone:    return "undone";
        case ChangeSetStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Render a JSON scalar as "value" or "null" for human display.
std::string jsonScalar(const nlohmann::json& v)
{
    if (v.is_null())
        return "(none)";
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_float()) {
        std::ostringstream os;
        os << v.get<double>();
        return os.str();
    }
    return v.dump();
}

/// Compare a single optional-ish front-matter field for the diff summary.
/// Returns a human line if changed, else empty.
std::string diffField(const char* field,
                      const nlohmann::json& before,
                      const nlohmann::json& after,
                      const std::string& noun)
{
    const auto& b = before.value(field, nlohmann::json(nullptr));
    const auto& a = after.value(field, nlohmann::json(nullptr));

    const bool bNull = b.is_null();
    const bool aNull = a.is_null();
    const bool same  = (bNull && aNull) || (!bNull && !aNull && b == a);
    if (same)
        return {};

    std::string line;
    if (bNull) {
        line = noun + " added: " + jsonScalar(a);
    } else if (aNull) {
        line = noun + " removed (was " + jsonScalar(b) + ")";
    } else {
        line = noun + " changed from " + jsonScalar(b) + " → " + jsonScalar(a);
    }
    return line;
}

/// Escape backticks in slot names for readability.
std::string slotLabel(std::string_view slot)
{
    if (slot.empty())
        return std::string(slot);
    return "`" + std::string(slot) + "`";
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Summary generation
// ---------------------------------------------------------------------------

std::string ChangeSetManager::operationSummary(const ChangeSetOperation& op)
{
    if (op.op == "edit_song") {
        std::vector<std::string> lines;

        // BPM.
        const auto bpmLine = diffField("bpm", op.before, op.after, "BPM");
        if (!bpmLine.empty())
            lines.push_back(bpmLine);

        // Slot.
        const auto slotLine = diffField("slot", op.before, op.after, "Slot");
        if (!slotLine.empty())
            lines.push_back(slotLine);

        // Pattern (body).
        const std::string beforeBody = op.before.value("body", std::string{});
        const std::string afterBody  = op.after.value("body", std::string{});
        if (beforeBody != afterBody) {
            std::string slot = op.slotName.empty()
                ? op.before.value("slot", op.after.value("slot", std::string{}))
                : op.slotName;
            if (beforeBody.empty()) {
                lines.push_back("Pattern added on " + slotLabel(slot));
            } else if (afterBody.empty()) {
                lines.push_back("Pattern cleared on " + slotLabel(slot));
            } else {
                lines.push_back("Pattern changed on " + slotLabel(slot));
                lines.push_back("  " + beforeBody + " → " + afterBody);
            }
        }

        // Label / color / bank.
        const auto labelLine = diffField("label", op.before, op.after, "Label");
        if (!labelLine.empty())
            lines.push_back(labelLine);
        const auto colorLine = diffField("color", op.before, op.after, "Color");
        if (!colorLine.empty())
            lines.push_back(colorLine);
        const auto bankLine = diffField("bank", op.before, op.after, "Bank");
        if (!bankLine.empty())
            lines.push_back(bankLine);

        if (lines.empty())
            return "Song " + slotLabel(op.slotName) + " updated.";

        std::ostringstream os;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) os << "; ";
            os << lines[i];
        }
        return os.str();
    }

    if (op.op == "commit_rendered_asset") {
        const std::string name = op.assetName.empty()
            ? op.resourceId
            : op.assetName;
        if (op.assetExistedBefore)
            return "Existing `" + name + "` modified (re-rendered and committed).";
        return "New `" + name + ".wav` rendered and committed.";
    }

    return "Change applied to " + op.resourceId;
}

nlohmann::json ChangeSetManager::operationToJson(const ChangeSetOperation& op)
{
    nlohmann::json j;
    j["op"] = op.op;
    j["resource_id"] = op.resourceId;
    j["slot_name"] = op.slotName;
    j["summary"] = operationSummary(op);
    j["reversible"] = op.reversible;
    j["revert_action"] = op.revertAction;
    if (!op.before.is_null())
        j["before"] = op.before;
    if (!op.after.is_null())
        j["after"] = op.after;
    if (op.op == "edit_song")
        j["song_file"] = op.songFile;
    if (op.op == "commit_rendered_asset") {
        j["asset_name"] = op.assetName;
        j["asset_existed_before"] = op.assetExistedBefore;
    }
    return j;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

int ChangeSetManager::beginChangeSet(std::string intent)
{
    std::lock_guard<std::mutex> lock(mtx_);

    ChangeSet cs;
    cs.id = nextId_++;
    cs.intent = std::move(intent);
    cs.status = ChangeSetStatus::Pending;
    cs.createdAt = std::chrono::steady_clock::now();
    cs.updatedAt = cs.createdAt;
    cs.validation = nlohmann::json::object();
    cs.checkpoint = nlohmann::json::object();

    history_.push_back(std::move(cs));
    return cs.id;
}

void ChangeSetManager::addOperation(const ChangeSetOperation& op)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (history_.empty() || history_.back().status != ChangeSetStatus::Pending)
        return;
    history_.back().operations.push_back(op);
    history_.back().updatedAt = std::chrono::steady_clock::now();
}

void ChangeSetManager::setValidation(nlohmann::json validation)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (history_.empty() || history_.back().status != ChangeSetStatus::Pending)
        return;
    history_.back().validation = std::move(validation);
    history_.back().updatedAt = std::chrono::steady_clock::now();
}

void ChangeSetManager::setCheckpoint(nlohmann::json checkpoint)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (history_.empty() || history_.back().status != ChangeSetStatus::Pending)
        return;
    history_.back().checkpoint = std::move(checkpoint);
    history_.back().updatedAt = std::chrono::steady_clock::now();
}

bool ChangeSetManager::acceptCurrent()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (history_.empty() || history_.back().status != ChangeSetStatus::Pending)
        return false;
    // Accept finalises the reviewed change-set.  It performs NO mutation and
    // does NOT reapply operations — the mutations were already applied by the
    // workflow through AI-7.  We only transition status.
    history_.back().status = ChangeSetStatus::Accepted;
    history_.back().updatedAt = std::chrono::steady_clock::now();
    return true;
}

std::optional<std::vector<ChangeSetManager::RevertAction>>
ChangeSetManager::rejectCurrent()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (history_.empty() || history_.back().status != ChangeSetStatus::Pending)
        return std::nullopt;
    auto plan = buildRevertPlan(history_.back());
    if (!plan)
        return std::nullopt;
    return plan;
}

std::optional<std::vector<ChangeSetManager::RevertAction>>
ChangeSetManager::undoAccepted(int changeSetId)
{
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& cs : history_) {
        if (cs.id == changeSetId) {
            if (cs.status != ChangeSetStatus::Accepted)
                return std::nullopt;
            auto plan = buildRevertPlan(cs);
            if (!plan)
                return std::nullopt;
            return plan;
        }
    }
    return std::nullopt;
}

void ChangeSetManager::markRejected()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (history_.empty())
        return;
    history_.back().status = ChangeSetStatus::Rejected;
    history_.back().updatedAt = std::chrono::steady_clock::now();
}

void ChangeSetManager::markUndone()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (history_.empty())
        return;
    history_.back().status = ChangeSetStatus::Undone;
    history_.back().updatedAt = std::chrono::steady_clock::now();
}

void ChangeSetManager::cancelCurrent()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (history_.empty() || history_.back().status != ChangeSetStatus::Pending)
        return;
    history_.back().status = ChangeSetStatus::Cancelled;
    history_.back().updatedAt = std::chrono::steady_clock::now();
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

int ChangeSetManager::currentChangeSetId() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return history_.empty() ? 0 : history_.back().id;
}

bool ChangeSetManager::hasPending() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return !history_.empty() && history_.back().status == ChangeSetStatus::Pending;
}

std::optional<ChangeSet> ChangeSetManager::currentChangeSet() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (history_.empty())
        return std::nullopt;
    return history_.back();
}

std::optional<ChangeSet> ChangeSetManager::getChangeSet(int id) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& cs : history_) {
        if (cs.id == id)
            return cs;
    }
    return std::nullopt;
}

nlohmann::json ChangeSetManager::previewCurrent() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (history_.empty())
        return nlohmann::json{{"ok", false}, {"error", "no change-set"}};

    const auto& cs = history_.back();
    return previewLocked(cs);
}

nlohmann::json ChangeSetManager::previewLocked(const ChangeSet& cs) const
{
    nlohmann::json j = toJson(cs);
    j["ok"] = true;
    j["cmd"] = "changeset_preview";
    return j;
}

nlohmann::json ChangeSetManager::toJson(const ChangeSet& cs) const
{
    nlohmann::json j;
    j["change_set_id"] = cs.id;
    j["intent"] = cs.intent;
    j["status"] = changeSetStatusName(cs.status);
    j["created_at"] = cs.createdAt.time_since_epoch().count();
    j["updated_at"] = cs.updatedAt.time_since_epoch().count();

    nlohmann::json ops = nlohmann::json::array();
    std::vector<std::string> summaries;
    bool allReversible = true;
    for (const auto& op : cs.operations) {
        ops.push_back(operationToJson(op));
        summaries.push_back(operationSummary(op));
        if (!op.reversible)
            allReversible = false;
    }
    j["operations"] = std::move(ops);
    j["reversible"] = allReversible && !cs.operations.empty();

    std::ostringstream summary;
    for (size_t i = 0; i < summaries.size(); ++i) {
        if (i) summary << "  ·  ";
        summary << summaries[i];
    }
    j["summary"] = summary.str();

    if (!cs.validation.is_null())
        j["validation"] = cs.validation;
    if (!cs.checkpoint.is_null())
        j["checkpoint"] = cs.checkpoint;

    return j;
}

nlohmann::json ChangeSetManager::toJsonActive() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (history_.empty())
        return nlohmann::json{{"ok", false}, {"error", "no change-set"}};
    nlohmann::json j = toJson(history_.back());
    j["ok"] = true;
    return j;
}

// ---------------------------------------------------------------------------
// Revert plan
// ---------------------------------------------------------------------------

std::optional<std::vector<ChangeSetManager::RevertAction>>
ChangeSetManager::buildRevertPlan(const ChangeSet& cs) const
{
    std::vector<RevertAction> plan;

    // Revert in reverse order (last mutation undone first).
    for (auto it = cs.operations.rbegin(); it != cs.operations.rend(); ++it) {
        const auto& op = *it;

        if (!op.reversible)
            return std::nullopt;  // whole change-set reverts coherently or not at all

        if (op.op == "edit_song") {
            RevertAction ra;
            ra.kind = "restore_song";
            ra.resourceId = op.resourceId;
            ra.destructive = true;   // persistent mutation — subject to AI-1
            ra.songFile = op.songFile;
            ra.content = op.originalContent;
            plan.push_back(std::move(ra));
        } else if (op.op == "commit_rendered_asset") {
            RevertAction ra;
            ra.kind = "remove_asset";
            ra.resourceId = op.resourceId;
            ra.destructive = true;   // destructive — subject to AI-1
            ra.assetName = op.assetName;
            plan.push_back(std::move(ra));
        } else {
            // Unknown operation — cannot guarantee a coherent revert.
            return std::nullopt;
        }
    }

    return plan;
}

} // namespace hathor::control
