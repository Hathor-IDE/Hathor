#!/usr/bin/env node
/**
 * strudel-lsp-server.js — Strudel mini-notation Language Server Protocol (LSP) server.
 *
 * Provides language intelligence (completion, hover, diagnostics) for Strudel
 * mini-notation source (.hathor files) by wrapping the real @strudel/mini
 * parser from the Strudel ecosystem (version 1.2.6, matching kStrudelMiniNotationCompat).
 *
 * Protocol: LSP over stdio with Content-Length framing.
 *
 * This server is the "verified Strudel LSP integration" from AI-3 — it uses
 * the actual Strudel mini-notation parser, not a reimplementation. Hathor's
 * C++ engine is differential-tested against the same Strudel golden fixtures.
 *
 * Architectural boundary (AI-3/AI-4):
 *   Hathor editor  →  this LSP server  →  @strudel/mini parser
 *                      + LanguageMetadata (fallback for Hathor-specific items)
 *
 * No LLM is involved. This is purely deterministic language intelligence.
 */

// Patch @kabelsalat/web exports for ESM compatibility
import { createRequire } from 'module';
import { readFileSync, writeFileSync, existsSync, mkdirSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __dirname = dirname(fileURLToPath(import.meta.url));

// Try to patch the @kabelsalat/web package.json if needed
try {
    const kabelsalatPkgPath = join(__dirname, 'node_modules', '@kabelsalat', 'web', 'package.json');
    if (existsSync(kabelsalatPkgPath)) {
        const pkg = JSON.parse(readFileSync(kabelsalatPkgPath, 'utf8'));
        if (!pkg.exports) {
            pkg.exports = { '.': './dist/index.mjs' };
            writeFileSync(kabelsalatPkgPath, JSON.stringify(pkg, null, 2));
        }
    }
} catch (e) {
    // Ignore patch errors — may already be patched or not needed
}

import * as mini from '@strudel/mini';

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const SERVER_VERSION = '1.2.6';
const SERVER_NAME = 'strudel-lsp-server';

// Supported mini-notation functions
const FUNCTION_NAMES = [
    's', 'sound', 'fast', 'slow', 'stack', 'cat', 'fastcat', 'slowcat',
    'rev', 'every', 'sometimes', 'gain', 'pan', 'speed', 'cut',
    'begin', 'end', 'cutoff', 'room', 'delay', 'orbit',
    'silence', 'note', 'n', 'scale', 'degree', 'legato', 'stut',
    'loopAt', 'density', 'hurry', 'zoom', 'when', 'off',
    'segment', 'chop', 'striate', 'slice', 'splice', 'fit',
    'iter', 'rev', 'palindrome', 'shuffle', 'scramble',
    'segment', 'step', 'whenmod', 'linger', 'delay', 'echo',
    'clamp', 'wrap', 'scale', 'coarse', 'shift', 'vowel',
    'shape', 'jux', 'juxby', 'easiest', 'dist', 'squeeze',
    'contrast', 'map', 'fastRel', 'slowRel', 'density',
];

// Standard sample names (SuperDirt defaults)
const SAMPLE_NAMES = [
    'bd', 'sn', 'hh', 'cp', 'ht', 'mt', 'lt', 'cr', 'rd', 'oh',
    'sh', 'cb', 'sine', 'sawtooth', 'square', 'triangle', 'white',
    'pink', 'brown', 'piano', 'bass', 'pluck', 'pad', 'string',
    'flute', 'metal', 'perc', 'gui', 'hoo', 'snare', 'tom',
    'rim', 'clap', 'snap', 'click', 'claves', 'cymbal', 'hihat',
    'kick', 'kick2', 'kick_heavy', 'snare2', 'snare3',
    'closedhh', 'openhh', 'ride', 'crash', 'hat', 'kick3',
];

// Scale names
const SCALE_NAMES = [
    'major', 'minor', 'dorian', 'phrygian', 'lydian', 'mixolydian',
    'aeolian', 'ionian', 'locrian', 'major2', 'minor2', 'major6',
    'minor6', 'major7', 'minor7', 'major9', 'minor9', 'major11',
    'minor11', 'major13', 'minor13', 'octaves', 'hexMajor',
    'hexDorian', 'hexPhrygian', 'hexAeolian', 'hexMajor6',
    'hexMajor7', 'hexAdd7', 'hexAdd13', 'hexDiminished',
    'hexDim', 'hexWhole', 'hexTritone', 'hexMajorFlat5',
    'pentatonic', 'majorPenta', 'minorPenta', 'egyptian', 'palindrome',
    'indian', 'japanese', 'kumoi', 'hirajoshi', 'chinese',
    'neopolitan', 'neopolitanMajor', 'neapolitanMinor', 'persian',
    'harmonicMinor', 'harmonicMajor', 'doubleHarmonic',
    'superLocrian', 'leadingTetra', 'ultraLocrian',
    'major', 'minor', 'dorian', 'phrygian', 'lydian',
    'mixolydian', 'aeolian', 'locrian',
    'chromatic',
    'whole', 'wholeTone', 'bebop', 'blues',
    'minorMajor', 'minorMajor7',
    'majorFlat5', 'minorFlat5',
    'dorianFlat3', 'ionianSharp1', 'doremi',
    'ionian', 'dorian', 'phrygian', 'lydian', 'mixolydian',
    'aeolian', 'locrian', 'minor', 'major', 'dorian',
    // Common aliases
    'm', 'min', 'maj', 'M',
];

// Duration units (after ::)
const DURATION_UNITS = [
    ':sample', ':samp', ':sec', ':second', ':seconds',
    ':ms', ':millisecond', ':milliseconds',
    ':min', ':minute', ':minutes',
    ':hr', ':hour', ':hours',
    ':cycle', ':cycles', ':cyc',
    ':bar', ':beat',
];

// Hathor-specific sample aliases not in base Strudel
const HATHOR_SAMPLE_ALIASES = [
    'kick', 'kick2', 'kick_heavy',
    'snare2', 'snare3',
    'closedhh', 'openhh', 'ride', 'crash',
    'hat', 'kick3',
];

// ---------------------------------------------------------------------------
// Document state
// ---------------------------------------------------------------------------

const documents = new Map(); // uri → { version, text, languageId }

function getDocumentText(uri) {
    const doc = documents.get(uri);
    return doc ? doc.text : '';
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

function computeDiagnostics(uri, text) {
    const diagnostics = [];

    // Try to parse the entire document (or each line as a pattern)
    // Mini-notation is line-structured — each non-blank line is a pattern.
    const lines = text.split('\n');

    for (let lineNum = 0; lineNum < lines.length; lineNum++) {
        const line = lines[lineNum];
        const trimmed = line.trim();

        if (trimmed === '') continue;

        // Skip front-matter lines ([hathor] block)
        if (trimmed === '[hathor]') continue;
        if (/^[a-zA-Z_-]+ *=/.test(trimmed)) continue;

        try {
            mini.m(trimmed);
        } catch (e) {
            const msg = e.message || 'Parse error';
            // Find approximate column
            let col = 0;
            const match = msg.match(/at offset (\d+)/);
            if (match) {
                col = parseInt(match[1], 10);
            }

            diagnostics.push({
                range: {
                    start: { line: lineNum, character: col },
                    end: { line: lineNum, character: col + 1 },
                },
                message: msg,
                severity: 1, // Error
                source: 'strudel-lsp-server',
                code: 'PARSE_ERROR',
            });
        }
    }

    return diagnostics;
}

// ---------------------------------------------------------------------------
// Completion
// ---------------------------------------------------------------------------

/**
 * Extract the word being completed (prefix) at the given line/character position.
 * Returns { prefix, line, characterPosition }.
 */
function getCompletionPrefix(text, line, character) {
    const lines = text.split('\n');
    const currentLine = lines[line] || '';

    // Walk backwards from cursor to find the start of the current word/prefix
    let start = character;
    while (start > 0) {
        const c = currentLine[start - 1];
        // Stop at whitespace, bracket openings, operators
        if (/\s/.test(c) || c === '[' || c === ']' || c === '<' || c === '>' ||
            c === '(' || c === ')' || c === ',' || c === '*' || c === '/' ||
            c === '!' || c === '~' || c === '|' || c === '{' || c === '}' ||
            c === '"' || c === "'" || c === ':') {
            break;
        }
        start--;
    }

    const prefix = currentLine.slice(start, character);

    // Determine context: are we inside a string literal?
    let inString = false;
    let stringDelimiter = '';
    for (let i = 0; i < character; i++) {
        const c = currentLine[i];
        if ((c === '"' || c === "'") && c === stringDelimiter) {
            inString = false;
            stringDelimiter = '';
        } else if (c === '"' || c === "'") {
            inString = true;
            stringDelimiter = c;
        }
    }

    // Check if we're inside a sample string (e.g., s "bd")
    let inSampleString = false;
    if (inString) {
        // Check if the string is preceded by s( or sound(
        const beforeString = currentLine.slice(0, start).trim();
        const lastWordEnd = beforeString.search(/[a-zA-Z_]\w*\s*$/);
        if (lastWordEnd >= 0) {
            const lastWord = beforeString.slice(lastWordEnd);
            if (lastWord === 's' || lastWord === 'sound' || lastWord === 'scale' ||
                lastWord === 'degree' || lastWord === 'note' || lastWord === 'n') {
                inSampleString = true;
            }
        }
    }

    return { prefix, line: currentLine, characterPosition: character, inString, inSampleString };
}

function provideCompletion(uri, line, character) {
    const text = getDocumentText(uri);
    const { prefix, line: currentLine, inString, inSampleString } = getCompletionPrefix(text, line, character);

    const items = [];

    // L1: Basic completion — function names, sample names, scale names
    if (inString && inSampleString) {
        // Inside a sample string (s "bd" or scale "|major")
        // L3: project-aware — could filter by actual project samples
        const allNames = [...SAMPLE_NAMES, ...HATHOR_SAMPLE_ALIASES];
        const matching = allNames.filter(name => name.startsWith(prefix));

        for (const name of matching) {
            items.push({
                label: name,
                kind: 12, // EnumValue
                detail: 'sample',
                documentation: {
                    kind: 'markdown',
                    value: `\`${name}\` — sample name`
                },
            });
        }
    } else if (inString && prefix === '|') {
        // Scale name completion (scale "|minor")
        const matching = SCALE_NAMES.filter(name => name.startsWith(prefix === '|' ? '' : prefix));
        for (const name of matching) {
            items.push({
                label: name,
                kind: 12,
                detail: 'scale',
                documentation: { kind: 'markdown', value: `\`${name}\` — scale name` },
            });
        }
    } else if (inString && (prefix.startsWith(':') || currentLine.includes('::'))) {
        // Duration unit completion after ::
        for (const unit of DURATION_UNITS) {
            items.push({
                label: unit,
                kind: 12,
                detail: 'duration unit',
                documentation: { kind: 'markdown', value: `\`${unit}\` — time duration unit` },
            });
        }
    } else {
        // L1: Function/keyword completion
        // Match against function names
        const matchingFns = FUNCTION_NAMES.filter(fn => fn.startsWith(prefix));
        for (const fn of matchingFns) {
            const sig = getFunctionSignature(fn);
            items.push({
                label: fn,
                kind: 3, // Function
                detail: sig,
                documentation: {
                    kind: 'markdown',
                    value: getFunctionDoc(fn)
                },
            });
        }

        // Also match sample names as potential atoms
        const matchingSamples = SAMPLE_NAMES.filter(s => s.startsWith(prefix) && s.length > prefix.length);
        for (const s of matchingSamples) {
            items.push({
                label: s,
                kind: 12, // EnumValue
                detail: 'sample',
                documentation: { kind: 'markdown', value: `\`${s}\` — sample` },
            });
        }
    }

    // L2: If we're inside a function call, add argument hints
    if (prefix === '') {
        // Add signature help for common function contexts
        const beforeCursor = currentLine.slice(0, character);
        const funcMatch = beforeCursor.match(/(\w+)\s*\($/);
        if (funcMatch) {
            const funcName = funcMatch[1];
            const sig = getFunctionSignature(funcName);
            if (sig) {
                // Add signature help as a special completion item
                items.push({
                    label: funcName + '()',
                    kind: 3,
                    detail: sig,
                    documentation: {
                        kind: 'markdown',
                        value: getFunctionDoc(funcName)
                    },
                    insertText: funcName + '(',
                    additionalTextEdits: [{
                        range: { start: { line, character: character - funcName.length }, end: { line, character } },
                        newText: ''
                    }]
                });
            }
        }
    }

    return {
        items: items,
        isIncomplete: false,
    };
}

function getFunctionSignature(name) {
    const signatures = {
        's': 's(pattern: string)',
        'sound': 'sound(pattern: string)',
        'fast': 'fast(multiplier: number)',
        'slow': 'slow(divisor: number)',
        'stack': 'stack(...patterns: Pattern[])',
        'cat': 'cat(...patterns: Pattern[])',
        'fastcat': 'fastcat(...patterns: Pattern[])',
        'slowcat': 'slowcat(...patterns: Pattern[])',
        'rev': 'rev()',
        'every': 'every(cycle: number, func: Function)',
        'sometimes': 'sometimes(func: Function)',
        'gain': 'gain(value: number | Pattern)',
        'pan': 'pan(value: number | Pattern)',
        'speed': 'speed(value: number | Pattern)',
        'cut': 'cut(group: number | Pattern)',
        'begin': 'begin(amount: number | Pattern)',
        'end': 'end(amount: number | Pattern)',
        'cutoff': 'cutoff(frequency: number | Pattern)',
        'room': 'room(size: number | Pattern)',
        'delay': 'delay(level: number | Pattern)',
        'orbit': 'orbit(n: number | Pattern)',
        'scale': 'scale(pattern: string)',
        'degree': 'degree(pattern: string)',
        'note': 'note(pattern: string)',
        'silence': 'silence()',
        'n': 'n(pattern: string)',
    };
    return signatures[name] || null;
}

function getFunctionDoc(name) {
    const docs = {
        's': 'Play a sample pattern. Alias for sound().',
        'sound': 'Play a sample pattern by name. Primary entry point for sample-based sequences.',
        'fast': 'Speed up pattern by multiplier (e.g. 2 = twice as fast).',
        'slow': 'Slow down pattern by divisor (e.g. 2 = half speed).',
        'stack': 'Play multiple patterns simultaneously.',
        'cat': 'Concatenate patterns, playing one after another.',
        'fastcat': 'Fast concatenation of patterns (space-separated in mini-notation).',
        'slowcat': 'Slow concatenation of patterns (angle-bracket syntax in mini-notation).',
        'rev': 'Reverse the pattern within each cycle.',
        'every': 'Apply function every N cycles.',
        'sometimes': 'Apply function randomly 50% of the time.',
        'gain': 'Set volume. Values from 0 to 1.',
        'pan': 'Stereo pan. 0 = left, 0.5 = center, 1 = right.',
        'speed': 'Playback speed of sample. 1 = normal, 0.5 = half speed.',
        'cut': 'Stops a playing sample when another sample with the same cut group is triggered.',
        'begin': 'Start offset in sample (0 to 1). Skips the beginning.',
        'end': 'End offset in sample (0 to 1). Cuts off the end.',
        'cutoff': 'Per-voice low-pass filter cutoff frequency in Hz.',
        'scale': 'Set the scale (e.g. "major", "minor", "dorian").',
        'degree': 'Play notes by degree within the current scale.',
        'silence': 'Produce silence (no events).',
    };
    return docs[name] || `\`${name}\` — Strudel mini-notation function`;
}

// ---------------------------------------------------------------------------
// Hover
// ---------------------------------------------------------------------------

function provideHover(uri, line, character) {
    const text = getDocumentText(uri);
    const lines = text.split('\n');
    const currentLine = lines[line] || '';

    // Find the word at the cursor position
    let wordStart = character;
    let wordEnd = character;

    while (wordStart > 0) {
        const c = currentLine[wordStart - 1];
        if (c === ' ' || c === '\t' || c === '[' || c === ']' || c === '<' || c === '>' ||
            c === '(' || c === ')' || c === ',' || c === '*' || c === '/' ||
            c === '!' || c === '~' || c === '|' || c === '"' || c === "'" ||
            c === ':' || c === '{' || c === '}') {
            break;
        }
        wordStart--;
    }

    while (wordEnd < currentLine.length) {
        const c = currentLine[wordEnd];
        if (c === ' ' || c === '\t' || c === '[' || c === ']' || c === '<' || c === '>' ||
            c === '(' || c === ')' || c === ',' || c === '*' || c === '/' ||
            c === '!' || c === '~' || c === '|' || c === '"' || c === "'" ||
            c === ':' || c === '{' || c === '}') {
            break;
        }
        wordEnd++;
    }

    const word = currentLine.slice(wordStart, wordEnd);

    if (word.length === 0) return null;

    // Check if it's a function
    const sig = getFunctionSignature(word);
    const doc = getFunctionDoc(word);

    if (sig || doc) {
        return {
            range: {
                start: { line, character: wordStart },
                end: { line, character: wordEnd },
            },
            contents: {
                kind: 'markdown',
                value: doc ? doc : `\`${word}\``,
            },
        };
    }

    // Check if it's a sample name
    if (SAMPLE_NAMES.includes(word) || HATHOR_SAMPLE_ALIASES.includes(word)) {
        return {
            range: {
                start: { line, character: wordStart },
                end: { line, character: wordEnd },
            },
            contents: {
                kind: 'markdown',
                value: `**${word}** — sample name\n\nUsed in mini-notation patterns like \`s("${word}")\` or \`"${word}"\`.`
            },
        };
    }

    // Check if it's a scale name
    if (SCALE_NAMES.includes(word)) {
        return {
            range: {
                start: { line, character: wordStart },
                end: { line, character: wordEnd },
            },
            contents: {
                kind: 'markdown',
                value: `**${word}** — musical scale name\n\nUsed with the \`scale\` function to define harmonic context.`
            },
        };
    }

    return null;
}

// ---------------------------------------------------------------------------
// Word extraction (shared by hover, definition, references)
// ---------------------------------------------------------------------------

/**
 * Extract the word at the given line/character position.
 * Returns { word, line, wordStart, wordEnd } or null if no word found.
 */
function findWordAt(text, line, character) {
    const lines = text.split('\n');
    const currentLine = lines[line] || '';

    let wordStart = character;
    let wordEnd = character;

    while (wordStart > 0) {
        const c = currentLine[wordStart - 1];
        if (c === ' ' || c === '\t' || c === '[' || c === ']' || c === '<' || c === '>' ||
            c === '(' || c === ')' || c === ',' || c === '*' || c === '/' ||
            c === '!' || c === '~' || c === '|' || c === '"' || c === "'" ||
            c === ':' || c === '{' || c === '}') {
            break;
        }
        wordStart--;
    }

    while (wordEnd < currentLine.length) {
        const c = currentLine[wordEnd];
        if (c === ' ' || c === '\t' || c === '[' || c === ']' || c === '<' || c === '>' ||
            c === '(' || c === ')' || c === ',' || c === '*' || c === '/' ||
            c === '!' || c === '~' || c === '|' || c === '"' || c === "'" ||
            c === ':' || c === '{' || c === '}') {
            break;
        }
        wordEnd++;
    }

    const word = currentLine.slice(wordStart, wordEnd);
    if (word.length === 0) return null;

    return { word, line, wordStart, wordEnd };
}

// ---------------------------------------------------------------------------
// Definition
// ---------------------------------------------------------------------------

/**
 * Provide definition location for the word at the cursor position.
 * For Strudel functions: returns the function name location (self-definition).
 * For sample names: returns the sample alias definition if known.
 * For scale names: returns the scale name definition.
 */
function provideDefinition(uri, line, character) {
    const text = getDocumentText(uri);
    const info = findWordAt(text, line, character);
    if (!info) return [];

    const { word, wordStart, wordEnd } = info;
    const locations = [];

    // Check if it's a known function
    if (FUNCTION_NAMES.includes(word)) {
        // Find all occurrences of this function in the document
        // The "definition" is the first occurrence (declaration)
        const lines = text.split('\n');
        for (let i = 0; i < lines.length; i++) {
            const line = lines[i];
            let idx = line.indexOf(word);
            while (idx !== -1) {
                // Check it's a whole word (not a substring)
                const charBefore = idx > 0 ? line[idx - 1] : ' ';
                const charAfter = idx + word.length < line.length ? line[idx + word.length] : ' ';
                if (isWordBoundary(charBefore) && isWordBoundary(charAfter)) {
                    locations.push({
                        uri,
                        range: {
                            start: { line: i, character: idx },
                            end: { line: i, character: idx + word.length },
                        },
                    });
                }
                idx = line.indexOf(word, idx + 1);
            }
        }
    }

    // If no locations found, return the cursor position as self-definition
    if (locations.length === 0) {
        locations.push({
            uri,
            range: {
                start: { line, character: wordStart },
                end: { line, character: wordEnd },
            },
        });
    }

    return locations;
}

// ---------------------------------------------------------------------------
// References
// ---------------------------------------------------------------------------

/**
 * Provide all references to the word at the cursor position within the
 * current document.
 */
function provideReferences(uri, line, character, includeDeclaration = true) {
    const text = getDocumentText(uri);
    const info = findWordAt(text, line, character);
    if (!info) return [];

    const { word } = info;
    const locations = [];
    const lines = text.split('\n');

    for (let i = 0; i < lines.length; i++) {
        const currentLine = lines[i];
        let idx = currentLine.indexOf(word);
        while (idx !== -1) {
            const charBefore = idx > 0 ? currentLine[idx - 1] : ' ';
            const charAfter = idx + word.length < currentLine.length ? currentLine[idx + word.length] : ' ';
            if (isWordBoundary(charBefore) && isWordBoundary(charAfter)) {
                locations.push({
                    uri,
                    range: {
                        start: { line: i, character: idx },
                        end: { line: i, character: idx + word.length },
                    },
                });
            }
            idx = currentLine.indexOf(word, idx + 1);
        }
    }

    return locations;
}

/**
 * Check if a character is a word boundary (non-word character).
 */
function isWordBoundary(c) {
    return !c || ' \t\n\r'.includes(c) || '[]<>(),*/!~|:;{}()"\'='.includes(c);
}

// ---------------------------------------------------------------------------
// Document Symbol
// ---------------------------------------------------------------------------

/**
 * Provide document-level symbols for a Strudel mini-notation file.
 * Each line that starts with a known function is a top-level symbol.
 * Sample names in s("...") strings are also indexed as constants.
 */
function provideDocumentSymbol(uri) {
    const text = getDocumentText(uri);
    const lines = text.split('\n');
    const symbols = [];

    for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        const trimmed = line.trim();

        // Skip blank lines and front-matter
        if (trimmed === '') continue;
        if (trimmed === '[hathor]') continue;
        if (/^[a-zA-Z_-]+ *=/.test(trimmed)) continue;

        // Check if line starts with a function call
        const funcMatch = trimmed.match(/^(\w+)\s*\(/);
        if (funcMatch) {
            const funcName = funcMatch[1];
            if (FUNCTION_NAMES.includes(funcName)) {
                symbols.push({
                    name: funcName,
                    kind: 13, // Variable (function invocation at top level)
                    detail: getFunctionSignature(funcName),
                    range: {
                        start: { line: i, character: line.indexOf(funcName) },
                        end: { line: i, character: line.length },
                    },
                    selectionRange: {
                        start: { line: i, character: line.indexOf(funcName) },
                        end: { line: i, character: line.indexOf(funcName) + funcName.length },
                    },
                });
            }
        }

        // Check for sample patterns: s("name") or sound("name")
        const sampleMatches = trimmed.matchAll(/s\s*\(\s*"([^"]+)"\s*\)|sound\s*\(\s*"([^"]+)"\s*\)/g);
        for (const m of sampleMatches) {
            const sampleName = m[1] || m[2];
            if (sampleName) {
                symbols.push({
                    name: sampleName,
                    kind: 14, // Constant (sample name)
                    detail: 'sample',
                    range: {
                        start: { line: i, character: m.index },
                        end: { line: i, character: m.index + m[0].length },
                    },
                    selectionRange: {
                        start: { line: i, character: m.index },
                        end: { line: i, character: m.index + m[0].length },
                    },
                });
            }
        }
    }

    return symbols;
}

// ---------------------------------------------------------------------------
// Rename
// ---------------------------------------------------------------------------

/**
 * Provide rename locations for the word at the cursor position.
 * For Strudel mini-notation, we can only rename within the current document
 * since function/sample names are global identifiers.
 */
function provideRename(uri, line, character, newName) {
    const text = getDocumentText(uri);
    const info = findWordAt(text, line, character);
    if (!info) return null;

    const { word } = info;

    // Check if it's a renamable identifier (function name or sample name)
    if (!FUNCTION_NAMES.includes(word) && !SAMPLE_NAMES.includes(word) && !HATHOR_SAMPLE_ALIASES.includes(word) && !SCALE_NAMES.includes(word)) {
        return null;
    }

    // For Strudel mini-notation, renaming a global function/sample name
    // only affects this document (no cross-file rename support in this architecture).
    return {
        documentChanges: [{
            textDocument: { uri },
            edits: [{
                range: {
                    start: { line, character: info.wordStart },
                    end: { line, character: info.wordEnd },
                },
                newText: newName,
            }],
        }],
    };
}

// ---------------------------------------------------------------------------
// Prepare Rename
// ---------------------------------------------------------------------------

function providePrepareRename(uri, line, character) {
    const text = getDocumentText(uri);
    const info = findWordAt(text, line, character);
    if (!info) return null;

    const { word, wordStart, wordEnd } = info;

    // Only renamable if it's a known function, sample, scale, or alias
    const isRenamable = FUNCTION_NAMES.includes(word) ||
                        SAMPLE_NAMES.includes(word) ||
                        HATHOR_SAMPLE_ALIASES.includes(word) ||
                        SCALE_NAMES.includes(word);

    if (!isRenamable) return null;

    return {
        placeholder: word,
        range: {
            start: { line, character: wordStart },
            end: { line, character: wordEnd },
        },
    };
}

// ---------------------------------------------------------------------------
// Workspace Symbol
// ---------------------------------------------------------------------------

/**
 * Provide workspace-wide symbol search.
 * Returns all known functions, samples, and scales that match the query.
 */
function provideWorkspaceSymbol(query) {
    const symbols = [];
    const lowerQuery = (query || '').toLowerCase();

    // Function symbols
    for (const fn of FUNCTION_NAMES) {
        if (fn.toLowerCase().includes(lowerQuery)) {
            symbols.push({
                name: fn,
                kind: 12, // Function
                detail: getFunctionSignature(fn),
                location: {
                    uri: 'hathor://builtin/strudel',
                    range: {
                        start: { line: 0, character: 0 },
                        end: { line: 0, character: fn.length },
                    },
                },
            });
        }
    }

    // Sample symbols
    const allSamples = [...SAMPLE_NAMES, ...HATHOR_SAMPLE_ALIASES];
    for (const sample of allSamples) {
        if (sample.toLowerCase().includes(lowerQuery)) {
            symbols.push({
                name: sample,
                kind: 14, // Constant
                detail: 'sample',
                location: {
                    uri: 'hathor://builtin/samples',
                    range: {
                        start: { line: 0, character: 0 },
                        end: { line: 0, character: sample.length },
                    },
                },
            });
        }
    }

    // Scale symbols
    for (const scale of SCALE_NAMES) {
        if (scale.toLowerCase().includes(lowerQuery)) {
            symbols.push({
                name: scale,
                kind: 13, // Enum (scale names are treated as enums)
                detail: 'scale',
                location: {
                    uri: 'hathor://builtin/scales',
                    range: {
                        start: { line: 0, character: 0 },
                        end: { line: 0, character: scale.length },
                    },
                },
            });
        }
    }

    return symbols;
}

// ---------------------------------------------------------------------------
// Signature Help (L2)
// ---------------------------------------------------------------------------

function provideSignatureHelp(uri, line, character) {
    const text = getDocumentText(uri);
    const lines = text.split('\n');
    const currentLine = lines[line] || '';
    const beforeCursor = currentLine.slice(0, character);

    // Match function call pattern: functionName(
    const match = beforeCursor.match(/(\w+)\s*\($/);
    if (!match) return null;

    const funcName = match[1];
    const sig = getFunctionSignature(funcName);
    const doc = getFunctionDoc(funcName);

    if (!sig) return null;

    return {
        signatures: [{
            label: sig,
            documentation: {
                kind: 'markdown',
                value: doc || sig
            },
            parameters: [{
                label: sig.replace(/.*\(([^)]*)\).*/, '$1'),
            }]
        }],
        activeSignature: 0,
        activeParameter: 0,
    };
}

// ---------------------------------------------------------------------------
// LSP Protocol
// ---------------------------------------------------------------------------

let initialized = false;

function sendNotification(method, params) {
    const msg = JSON.stringify({ jsonrpc: '2.0', method, params });
    const header = `Content-Length: ${Buffer.byteLength(msg, 'utf8')}\r\n\r\n`;
    process.stdout.write(header + msg);
}

function sendResponse(id, result) {
    const msg = JSON.stringify({ jsonrpc: '2.0', id, result });
    const header = `Content-Length: ${Buffer.byteLength(msg, 'utf8')}\r\n\r\n`;
    process.stdout.write(header + msg);
}

function sendError(id, code, message) {
    const msg = JSON.stringify({
        jsonrpc: '2.0',
        id,
        error: { code, message }
    });
    const header = `Content-Length: ${Buffer.byteLength(msg, 'utf8')}\r\n\r\n`;
    process.stdout.write(header + msg);
}

function sendDiagnostics(uri, diagnostics) {
    sendNotification('textDocument/publishDiagnostics', {
        uri,
        diagnostics
    });
}

// ---------------------------------------------------------------------------
// Message parsing
// ---------------------------------------------------------------------------

let buffer = '';
let contentLength = -1;

function onData(chunk) {
    buffer += chunk.toString('utf8');

    while (true) {
        if (contentLength === -1) {
            // Look for Content-Length header
            const headerEnd = buffer.indexOf('\r\n\r\n');
            if (headerEnd === -1) return; // Need more data

            const header = buffer.slice(0, headerEnd);
            const match = header.match(/Content-Length:\s*(\d+)/i);
            if (!match) {
                // Skip unknown headers
                buffer = buffer.slice(headerEnd + 4);
                continue;
            }

            contentLength = parseInt(match[1], 10);
            buffer = buffer.slice(headerEnd + 4);
        }

        if (buffer.length < contentLength) return; // Need more data

        const message = Buffer.from(buffer.slice(0, contentLength)).toString('utf8');
        buffer = buffer.slice(contentLength);
        contentLength = -1;

        handleMessage(JSON.parse(message));
    }
}

function handleMessage(msg) {
    if (msg.jsonrpc !== '2.0') return;

    // Notification
    if (msg.id === undefined && msg.method) {
        handleNotification(msg.method, msg.params);
        return;
    }

    // Request
    if (msg.id !== undefined && msg.method) {
        try {
            const result = handleRequest(msg.method, msg.params);
            sendResponse(msg.id, result);
        } catch (e) {
            sendError(msg.id, -32603, e.message);
        }
    }
}

function handleNotification(method, params) {
    switch (method) {
        case 'initialized':
            initialized = true;
            break;
        case 'textDocument/didOpen':
            {
                const { textDocument } = params;
                documents.set(textDocument.uri, {
                    version: textDocument.version,
                    text: params.textDocument.text,
                    languageId: textDocument.languageId,
                });
                sendDiagnostics(textDocument.uri, computeDiagnostics(textDocument.uri, textDocument.text));
            }
            break;
        case 'textDocument/didChange':
            {
                const { textDocument, contentChanges } = params;
                const doc = documents.get(textDocument.uri);
                if (doc) {
                    // Apply content changes
                    for (const change of contentChanges) {
                        if (change.text !== undefined) {
                            doc.text = change.text;
                        }
                    }
                    doc.version = textDocument.version;
                    sendDiagnostics(textDocument.uri, computeDiagnostics(textDocument.uri, doc.text));
                }
            }
            break;
        case 'textDocument/willSave':
            break;
        case 'textDocument/didClose':
            {
                const { textDocument } = params;
                documents.delete(textDocument.uri);
                sendDiagnostics(textDocument.uri, []);
            }
            break;
        case 'textDocument/didSave':
            {
                const { textDocument, text } = params;
                const doc = documents.get(textDocument.uri);
                if (doc && text !== undefined) {
                    doc.text = text;
                }
                if (doc) {
                    sendDiagnostics(textDocument.uri, computeDiagnostics(textDocument.uri, doc.text));
                }
            }
            break;
        case 'shutdown':
            // Will be handled as a request
            break;
        case 'exit':
            process.exit(0);
            break;
    }
}

function handleRequest(method, params) {
    switch (method) {
        case 'initialize':
            return {
                capabilities: {
                    completionProvider: {
                        triggerCharacters: ['.', '"', "'", '(', ',', ' ', '*', '/', '!', '~', '|', '<', '>', '[', ']', ':'],
                        resolveProvider: false,
                    },
                    hoverProvider: true,
                    signatureHelpProvider: {
                        triggerCharacters: ['(', ','],
                        request: { id: 1 }
                    },
                    definitionProvider: true,
                    referencesProvider: true,
                    renameProvider: {
                        prepareProvider: true,
                    },
                    documentSymbolProvider: true,
                    workspaceSymbolProvider: true,
                    diagnosticProvider: {
                        documentChanges: true,
                        didOpen: true,
                        didClose: true,
                    },
                    textDocumentSync: {
                        openClose: true,
                        change: 1, // Incremental
                    },
                },
                serverInfo: {
                    name: SERVER_NAME,
                    version: SERVER_VERSION,
                },
            };

        case 'initialized':
            return {};

        case 'shutdown':
            return null;

        case 'textDocument/completion':
            {
                const { textDocument, position } = params;
                const text = getDocumentText(textDocument.uri);
                return provideCompletion(textDocument.uri, position.line, position.character);
            }

        case 'textDocument/hover':
            {
                const { textDocument, position } = params;
                return provideHover(textDocument.uri, position.line, position.character);
            }

        case 'textDocument/signatureHelp':
            {
                const { textDocument, position } = params;
                return provideSignatureHelp(textDocument.uri, position.line, position.character);
            }

        case 'textDocument/definition':
            {
                const { textDocument, position } = params;
                return provideDefinition(textDocument.uri, position.line, position.character);
            }

        case 'textDocument/references':
            {
                const { textDocument, position, context } = params;
                const includeDeclaration = context ? context.includeDeclaration !== false : true;
                return provideReferences(textDocument.uri, position.line, position.character, includeDeclaration);
            }

        case 'textDocument/typeDefinition':
            {
                const { textDocument, position } = params;
                return provideDefinition(textDocument.uri, position.line, position.character);
            }

        case 'textDocument/declaration':
            {
                const { textDocument, position } = params;
                return provideDefinition(textDocument.uri, position.line, position.character);
            }

        case 'textDocument/prepareRename':
            {
                const { textDocument, position } = params;
                return providePrepareRename(textDocument.uri, position.line, position.character);
            }

        case 'textDocument/rename':
            {
                const { textDocument, position, newName } = params;
                return provideRename(textDocument.uri, position.line, position.character, newName);
            }

        case 'textDocument/documentSymbol':
            {
                const { textDocument } = params;
                return provideDocumentSymbol(textDocument.uri);
            }

        case 'workspace/symbol':
            {
                const { query } = params;
                return provideWorkspaceSymbol(query || '');
            }

        case 'textDocument/publishDiagnostics':
            return null;

        default:
            return null;
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

process.stdin.setEncoding('utf8');
process.stdin.on('data', onData);

process.on('SIGTERM', () => {
    process.exit(0);
});

process.on('SIGINT', () => {
    process.exit(0);
});

// Log to stderr (not stdout, which is the LSP protocol channel)
function log(msg) {
    process.stderr.write(`[strudel-lsp-server] ${msg}\n`);
}

log(`Strudel LSP Server v${SERVER_VERSION} ready`);
