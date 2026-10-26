/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Adapted for the Qt-free native host from
// .upstream/webmscore/src/converter/internal/compat/notationmeta.cpp
// (mu::converter::NotationMeta, Score* overload and helpers).
//
// The JSON field schema and the title/composer recognition heuristics are kept
// identical to the upstream implementation. QJsonArray/QJsonObject are replaced
// by nlohmann::json arrays/objects, QString by muse::String/std::string, and the
// notation/audio track overload is intentionally absent: "tracks" is empty here.

#include "score_metadata.h"

#include <cfloat>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "global/realfn.h"
#include "global/types/string.h"

#include "engraving/dom/engravingitem.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/text.h"
#include "engraving/dom/textbase.h"
#include "engraving/dom/timesig.h"
#include "engraving/style/style.h"

namespace music {
namespace {
using Json = nlohmann::json;
using namespace mu::engraving;

std::string boolToString(bool b)
{
    return b ? "true" : "false";
}

bool shouldTryRecognizeText(const Text* text)
{
    const TextStyleType type = text->textStyleType();
    if (type == TextStyleType::DEFAULT || type == TextStyleType::FRAME) {
        return true;
    }

    return static_cast<int>(type) >= static_cast<int>(TextStyleType::USER1)
           && static_cast<int>(type) <= static_cast<int>(TextStyleType::USER12);
}

muse::String recognizeTitle(const Score* score)
{
    const MeasureBase* mb = score->first();
    if (!mb || !mb->isVBox()) {
        return muse::String();
    }

    const Text* titleText = nullptr;
    double maxFontSize = DBL_MIN;
    double minY = DBL_MAX;

    for (const EngravingItem* item : mb->el()) {
        if (!item || !item->isText()) {
            continue;
        }

        const Text* text = toText(item);
        if (!shouldTryRecognizeText(text)) {
            continue;
        }

        if (text->size() < maxFontSize) {
            continue;
        }

        if (muse::RealIsEqual(text->size(), maxFontSize) && text->y() > minY) {
            continue;
        }

        titleText = text;
        maxFontSize = text->size();
        minY = text->y();
    }

    return titleText ? titleText->plainText() : muse::String();
}

muse::String recognizeComposer(const Score* score)
{
    const MeasureBase* mb = score->first();
    if (!mb || !mb->isVBox()) {
        return muse::String();
    }

    const Text* rightmostText = nullptr;
    double rightmostTextX = mb->ldata()->bbox().center().x();

    for (const EngravingItem* item : mb->el()) {
        if (!item || !item->isText()) {
            continue;
        }

        const Text* text = toText(item);
        if (!shouldTryRecognizeText(text)) {
            continue;
        }

        if (text->x() > rightmostTextX) {
            rightmostText = text;
            rightmostTextX = text->x();
        }
    }

    return rightmostText ? rightmostText->plainText() : muse::String();
}

std::string title(const Score* score)
{
    muse::String title;
    const Text* text = score->getText(TextStyleType::TITLE);
    if (text) {
        title = text->plainText();
    }

    if (title.isEmpty()) {
        title = score->metaTag(u"workTitle");
    }

    if (title.isEmpty()) {
        title = recognizeTitle(score);
    }

    if (title.isEmpty()) {
        title = score->name();
    }

    return title.toStdString();
}

std::string subtitle(const Score* score)
{
    muse::String subtitle;
    const Text* text = score->getText(TextStyleType::SUBTITLE);
    if (text) {
        subtitle = text->plainText();
    }

    return subtitle.toStdString();
}

std::string composer(const Score* score)
{
    muse::String composer;
    const Text* text = score->getText(TextStyleType::COMPOSER);
    if (text) {
        composer = text->plainText();
    }

    if (composer.isEmpty()) {
        composer = score->metaTag(u"composer");
    }

    if (composer.isEmpty()) {
        composer = recognizeComposer(score);
    }

    return composer.toStdString();
}

std::string poet(const Score* score)
{
    muse::String poet;
    const Text* text = score->getText(TextStyleType::LYRICIST);
    if (text) {
        poet = text->plainText();
    }

    if (poet.isEmpty()) {
        poet = score->metaTag(u"lyricist");
    }

    return poet.toStdString();
}

std::string timesig(const Score* score)
{
    size_t staves = score->nstaves();
    size_t tracks = staves * VOICES;
    const Segment* timeSigSegment = score->firstSegmentMM(SegmentType::TimeSig);
    if (!timeSigSegment) {
        return std::string();
    }

    std::string timeSig;
    const EngravingItem* element = nullptr;
    for (size_t track = 0; track < tracks; ++track) {
        element = timeSigSegment->element(track);
        if (element) {
            break;
        }
    }

    if (element && element->isTimeSig()) {
        const TimeSig* ts = toTimeSig(element);
        timeSig = std::to_string(ts->numerator()) + "/" + std::to_string(ts->denominator());
    }

    return timeSig;
}

std::pair<int, std::string> tempo(const Score* score)
{
    int tempo = 0;
    muse::String tempoText;
    for (const Segment* segment = score->firstSegmentMM(SegmentType::All); segment;
         segment = segment->next1MM()) {
        auto annotations = segment->annotations();
        for (const EngravingItem* annotation : annotations) {
            if (annotation && annotation->isTempoText()) {
                const TempoText* tt = toTempoText(annotation);
                tempo = static_cast<int>(std::round(tt->tempo().toBPM().val));
                tempoText = tt->xmlText();
            }
        }
    }

    return { tempo, tempoText.toStdString() };
}

Json partsJsonArray(const Score* score)
{
    Json jsonPartsArray = Json::array();
    for (const Part* part : score->parts()) {
        Json jsonPart = Json::object();
        jsonPart["id"] = part->id().toStdString();
        muse::String name = part->longName();
        jsonPart["name"] = name.replace(u"\n", u"").toStdString();
        int midiProgram = part->midiProgram();
        jsonPart["program"] = midiProgram;
        jsonPart["instrumentId"] = part->instrumentId().toStdString();
        jsonPart["instrumentName"] = part->instrumentName().toStdString();
        jsonPart["lyricCount"] = part->lyricCount();
        jsonPart["harmonyCount"] = part->harmonyCount();
        jsonPart["hasPitchedStaff"] = boolToString(part->hasPitchedStaff());
        jsonPart["hasTabStaff"] = boolToString(part->hasTabStaff());
        jsonPart["hasDrumStaff"] = boolToString(part->hasDrumStaff());
        jsonPart["isVisible"] = boolToString(part->show());
        jsonPartsArray.push_back(jsonPart);
    }

    return jsonPartsArray;
}

Json pageFormatJson(const MStyle& style)
{
    Json format = Json::object();
    // Upstream stores round(...) as a QJson double; QJsonDocument serializes a
    // whole value without a fractional part, so int keeps the existing output.
    format["height"] = static_cast<int>(std::round(style.styleD(Sid::pageHeight) * INCH));
    format["width"] = static_cast<int>(std::round(style.styleD(Sid::pageWidth) * INCH));
    format["twosided"] = boolToString(style.styleB(Sid::pageTwosided));

    return format;
}

void findTextByType(TextStyleType textStyleType, std::vector<muse::String>& strings,
                    EngravingItem* element)
{
    if (!element || !element->isTextBase()) {
        return;
    }

    const TextBase* text = toTextBase(element);
    if (text->textStyleType() == textStyleType) {
        strings.push_back(text->plainText());
    }
}

Json typeDataJson(Score* score)
{
    Json typesData = Json::object();
    static const std::vector<std::pair<std::string, TextStyleType> > namesTypesList {
        { "titles", TextStyleType::TITLE },
        { "subtitles", TextStyleType::SUBTITLE },
        { "composers", TextStyleType::COMPOSER },
        { "poets", TextStyleType::LYRICIST }
    };

    for (const auto& nameType : namesTypesList) {
        Json typeData = Json::array();
        std::vector<muse::String> typeTextStrings;
        score->scanElements([&](EngravingItem* item) { findTextByType(nameType.second, typeTextStrings, item); });
        for (const auto& typeStr : typeTextStrings) {
            typeData.push_back(typeStr.toStdString());
        }
        typesData[nameType.first] = typeData;
    }

    return typesData;
}

Json excerptsJsonArray(const Score* score)
{
    Json jsonExcerptsArray = Json::array();

    const std::vector<Excerpt*>& excerpts = score->masterScore()->excerpts();
    for (size_t i = 0; i < excerpts.size(); ++i) {
        const Excerpt* e = excerpts[i];
        Score* part = e->excerptScore();

        Json jsonExcerpt = Json::object();
        jsonExcerpt["id"] = static_cast<int>(i);
        jsonExcerpt["title"] = e->name().toStdString();
        jsonExcerpt["parts"] = partsJsonArray(part);

        jsonExcerptsArray.push_back(jsonExcerpt);
    }

    return jsonExcerptsArray;
}
} // namespace

nlohmann::json score_metadata(mu::engraving::Score* score)
{
    if (!score) {
        return nlohmann::json::object();
    }

    Json json = Json::object();

    json["title"] = title(score);
    json["subtitle"] = subtitle(score);
    json["composer"] = composer(score);
    json["poet"] = poet(score);
    json["mscoreVersion"] = score->mscoreVersion().toStdString();
    json["fileVersion"] = score->mscVersion();
    json["pages"] = static_cast<int>(score->npages());       // no = operator for size_t
    json["measures"] = static_cast<int>(score->nmeasures()); // no = operator for size_t
    json["hasLyrics"] = boolToString(score->hasLyrics());
    json["hasHarmonies"] = boolToString(score->hasHarmonies());
    json["keysig"] = score->keysig();
    json["previousSource"] = score->metaTag(u"source").toStdString();
    json["timesig"] = timesig(score);
    json["duration"] = score->duration();
    json["lyrics"] = score->extractLyrics().toStdString();

    auto _tempo = tempo(score);
    json["tempo"] = _tempo.first;
    json["tempoText"] = _tempo.second;

    json["parts"] = partsJsonArray(score);
    json["pageFormat"] = pageFormatJson(score->style());
    json["textFramesData"] = typeDataJson(score);
    json["tracks"] = Json::array(); // the audio-track overload is not part of the Qt-free host
    json["excerpts"] = excerptsJsonArray(score);

    return json;
}

std::string score_title(const mu::engraving::Score* score)
{
    if (!score) {
        return std::string();
    }

    return title(score);
}
} // namespace music
