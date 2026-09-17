// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * QuickOpenDialog.cpp — implementation of the quick-open dialog.
 *
 * JUCE-dependent. Uses JUCE-free WorkspaceSearchModel for search logic
 * where applicable, but file collection is JUCE-specific (recursive_directory_iterator).
 *
 * Requirement references: L-2 §1
 */

#include "QuickOpenDialog.hpp"

#include "HathorLookAndFeel.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace hathor::ui {

class QuickOpenDialogDoubleClickHandler : public juce::MouseListener
{
public:
    explicit QuickOpenDialogDoubleClickHandler(QuickOpenDialog* parent) : parent_(parent) {}
    void mouseDoubleClick(const juce::MouseEvent&) override { parent_->confirmSelection(); }
private:
    QuickOpenDialog* parent_;
};

QuickOpenDialog::QuickOpenDialog(const std::filesystem::path& workspaceRoot)
    : workspaceRoot_(workspaceRoot)
{
    setTitle("Quick Open");
    setDescription("Type to filter files. Arrow keys move, Enter opens, Escape closes.");
    filterField_ = std::make_unique<juce::TextEditor>();
    filterField_->addListener(this);
    filterField_->setFont(HathorLookAndFeel::getUiFont(16.0f));
    filterField_->setColour(juce::TextEditor::backgroundColourId,
                            juce::Colours::black.withAlpha(0.6f));
    filterField_->setColour(juce::TextEditor::textColourId, juce::Colours::white);
    filterField_->setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
    // No charset restriction: queries may contain spaces, unicode, #/@ etc.
    filterField_->setInputRestrictions(0, juce::String());
    addAndMakeVisible(filterField_.get());

    hintLabel_ = std::make_unique<juce::Label>();
    hintLabel_->setText("Type to filter files (Esc to close)", juce::dontSendNotification);
    hintLabel_->setFont(HathorLookAndFeel::getUiFont(14.0f));
    hintLabel_->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.7f));
    hintLabel_->setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(hintLabel_.get());

    listBox_ = std::make_unique<juce::ListBox>();
    listBox_->setModel(this);
    listBox_->setRowSelectedOnMouseDown(true);
    listBox_->setColour(juce::ListBox::backgroundColourId, juce::Colours::black.withAlpha(0.8f));
    listBox_->setColour(juce::ListBox::outlineColourId, juce::Colours::white.withAlpha(0.2f));
    addAndMakeVisible(listBox_.get());

    listBox_->addMouseListener(new QuickOpenDialogDoubleClickHandler(this), true);

    collectFiles(workspaceRoot_);
    filteredFiles_ = allFiles_;
    refreshFiltered();
}

QuickOpenDialog::~QuickOpenDialog()
{
    listBox_->setModel(nullptr);
}

void QuickOpenDialog::collectFiles(const std::filesystem::path& root)
{
    // Index on a worker thread so large workspaces never block the message
    // thread at construction. The generation check discards stale runs
    // (e.g. workspace re-rooted while indexing).
    const uint64_t generation = ++indexGeneration_;
    indexing_.store(true);
    juce::Component::SafePointer<QuickOpenDialog> safeSelf(this);
    std::thread([safeSelf, generation, root]() {
        std::vector<std::filesystem::path> files;
        auto ignoredDir = [](const std::filesystem::path& p) {
            const std::string name = p.filename().string();
            if (name == ".git" || name == "node_modules" || name == ".hathor"
                || name == "DerivedData" || name == ".idea"
                || name == ".vscode" || name == "CMakeFiles"
                || name == "_deps")
                return true;
            if (!name.empty() && name[0] == '.')
                return true;
            return name.rfind("build", 0) == 0;
        };
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(root, ec);
        const std::filesystem::recursive_directory_iterator end;
        for (; !ec && it != end;)
        {
            std::error_code e2;
            bool isDir = it->is_directory(e2);
            if (!e2 && isDir && ignoredDir(it->path()))
            {
                it.disable_recursion_pending();
                it.increment(e2);
                continue;
            }
            if (!e2 && it->is_regular_file(e2) && !e2)
            {
                auto ext = it->path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (ext == ".hathor" || ext == ".ck" || ext == ".txt"
                    || ext == ".md" || ext == ".json")
                    files.push_back(it->path());
            }
            it.increment(e2);
        }
        std::sort(files.begin(), files.end());
        juce::MessageManager::callAsync([safeSelf, generation,
                                         files = std::move(files)]() mutable {
            if (auto* self = safeSelf.getComponent())
                self->publishIndex(std::move(files), generation);
        });
    }).detach();
}

void QuickOpenDialog::publishIndex(std::vector<std::filesystem::path> files,
                                   uint64_t generation)
{
    if (generation != indexGeneration_.load())
        return; // stale run
    allFiles_ = std::move(files);
    indexing_.store(false);
    refreshFiltered();
}

void QuickOpenDialog::showOver(juce::Component* parent)
{
    if (!parent)
        return;

    auto bounds = parent->getBounds();
    setBounds(bounds);
    parent->addAndMakeVisible(this);
    setVisible(true);

    filterField_->setText({});
    filterField_->grabKeyboardFocus();
    selectedIndex_ = 0;
    if (listBox_)
        listBox_->selectRow(0);
}

void QuickOpenDialog::hide()
{
    setVisible(false);
    if (onCancelled)
        onCancelled();
}

void QuickOpenDialog::resized()
{
    const int hintHeight = 24;
    const int fieldHeight = 36;
    const int margin = 8;

    hintLabel_->setBounds(margin, margin, getWidth() - 2 * margin, hintHeight);
    filterField_->setBounds(margin, hintHeight + margin, getWidth() - 2 * margin, fieldHeight);
    listBox_->setBounds(margin, hintHeight + fieldHeight + 2 * margin,
                        getWidth() - 2 * margin, getHeight() - hintHeight - fieldHeight - 3 * margin);
}

void QuickOpenDialog::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.9f));
}

bool QuickOpenDialog::fuzzyMatch(std::string_view query, std::string_view path)
{
    if (query.empty())
        return true;

    // Simple subsequence fuzzy match (case-insensitive)
    auto lowerQuery = std::string(query);
    std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    auto lowerPath = std::string(path);
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    size_t qi = 0;
    for (size_t pi = 0; pi < lowerPath.size() && qi < lowerQuery.size(); ++pi)
    {
        if (lowerPath[pi] == lowerQuery[qi])
            qi++;
    }

    return qi == lowerQuery.size();
}

int QuickOpenDialog::fuzzyScore(std::string_view query, std::string_view path)
{
    if (query.empty())
        return 0;

    std::string q(query), p(path);
    std::transform(q.begin(), q.end(), q.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::transform(p.begin(), p.end(), p.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Subsequence scan with run tracking.
    int score = 0;
    size_t qi = 0;
    int run = 0;
    size_t firstAt = std::string::npos;
    const size_t fileStart = p.find_last_of("/\\") == std::string::npos
                                 ? 0
                                 : p.find_last_of("/\\") + 1;
    for (size_t pi = 0; pi < p.size() && qi < q.size(); ++pi)
    {
        if (p[pi] == q[qi])
        {
            if (firstAt == std::string::npos)
                firstAt = pi;
            ++run;
            // Consecutive runs score progressively; filename hits weigh 2x;
            // word starts (after /_-.) and path start earn bonuses.
            int w = run * run;
            if (pi >= fileStart)
                w *= 2;
            if (pi == 0 || p[pi - 1] == '/' || p[pi - 1] == '\\'
                || p[pi - 1] == '_' || p[pi - 1] == '-' || p[pi - 1] == '.')
                w += 4;
            score += w;
            ++qi;
        }
        else
        {
            run = 0;
        }
    }
    if (qi != q.size())
        return -1;
    // Early first-hit bonus; shorter paths win ties.
    score += static_cast<int>(std::max<size_t>(0, 40 - firstAt));
    score -= static_cast<int>(p.size() / 16);
    return score;
}

void QuickOpenDialog::refreshFiltered()
{
    auto query = filterField_ ? filterField_->getText() : juce::String();
    filteredFiles_.clear();

    if (query.isEmpty())
    {
        filteredFiles_ = allFiles_;
    }
    else
    {
        const std::string queryStr = query.toStdString();
        std::vector<std::pair<int, std::filesystem::path>> scored;
        for (const auto& f : allFiles_)
        {
            // Match against the workspace-relative path so "ed/tab" can
            // match editors/tabs/... not just bare filenames.
            std::error_code ec;
            const std::string rel =
                std::filesystem::relative(f, workspaceRoot_, ec).string();
            const std::string& hay = ec ? f.filename().string() : rel;
            const int s = fuzzyScore(queryStr, hay);
            if (s >= 0)
                scored.emplace_back(s, f);
        }
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (auto& s : scored)
            filteredFiles_.push_back(std::move(s.second));
    }

    if (indexing_.load() && filteredFiles_.empty() && hintLabel_)
        hintLabel_->setText("Indexing workspace…", juce::dontSendNotification);
    else if (hintLabel_)
        hintLabel_->setText("Type to filter files (Esc to close)",
                            juce::dontSendNotification);

    selectedIndex_ = 0;
    if (listBox_)
    {
        listBox_->updateContent();
        if (!filteredFiles_.empty())
            listBox_->selectRow(0);
    }
}

void QuickOpenDialog::setFilter(const juce::String& query)
{
    if (filterField_)
        filterField_->setText(query, juce::dontSendNotification);
    refreshFiltered();
}

std::filesystem::path QuickOpenDialog::selectedFile() const
{
    if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(filteredFiles_.size()))
        return {};
    return filteredFiles_[selectedIndex_];
}

void QuickOpenDialog::selectUp()
{
    if (selectedIndex_ > 0)
    {
        selectedIndex_--;
        if (listBox_)
            listBox_->selectRow(selectedIndex_);
    }
}

void QuickOpenDialog::selectDown()
{
    if (selectedIndex_ < static_cast<int>(filteredFiles_.size()) - 1)
    {
        selectedIndex_++;
        if (listBox_)
            listBox_->selectRow(selectedIndex_);
    }
}

bool QuickOpenDialog::confirmSelection()
{
    auto file = selectedFile();
    if (file.empty())
        return false;

    setVisible(false);

    if (onFileSelected)
    {
        onFileSelected(file);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// TextEditor::Listener
// ---------------------------------------------------------------------------

void QuickOpenDialog::textEditorTextChanged(juce::TextEditor& /*editor*/)
{
    refreshFiltered();
}

void QuickOpenDialog::textEditorEscapeKeyPressed(juce::TextEditor& /*editor*/)
{
    hide();
}

bool QuickOpenDialog::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::returnKey)
    {
        if (confirmSelection())
            return true;
    }
    if (key == juce::KeyPress::upKey)
    {
        selectUp();
        return true;
    }
    if (key == juce::KeyPress::downKey)
    {
        selectDown();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ListBoxModel
// ---------------------------------------------------------------------------

int QuickOpenDialog::getNumRows()
{
    return static_cast<int>(filteredFiles_.size());
}

void QuickOpenDialog::paintListBoxItem(int row, juce::Graphics& g,
                                       int width, int height, bool isSelected)
{
    juce::Rectangle<int> bounds(0, 0, width, height);

    if (isSelected)
        g.fillAll(juce::Colours::white.withAlpha(0.15f));
    else
        g.fillAll(juce::Colours::black.withAlpha(0.8f));

    auto file = filteredFiles_[row];
    juce::String displayText = file.filename().string();

    auto relative = std::filesystem::relative(file, workspaceRoot_);
    if (relative.has_parent_path())
    {
        auto parentStr = relative.parent_path().string();
        displayText = juce::String(parentStr.c_str()) + " / " + displayText;
    }

    g.setColour(juce::Colours::white);
    g.setFont(HathorLookAndFeel::getUiFont(15.0f));
    g.drawText(displayText, bounds.reduced(4, 2), juce::Justification::centredLeft, false);
}

void QuickOpenDialog::selectedRowsChanged(int lastSelectedRow)
{
    selectedIndex_ = lastSelectedRow;
}

} // namespace hathor::ui
