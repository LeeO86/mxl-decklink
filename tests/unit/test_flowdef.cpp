// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "mxlbridge/flowdef.hpp"

using namespace mxldl;
using namespace mxldl::mxlbridge;

TEST_CASE("video flow definition JSON carries the MXL v1.0 required fields")
{
    VideoFlowParams p;
    p.id = *util::parseUuid("5fbec3b1-1b0f-417d-9059-8b94a47197ed");
    p.label = "cam 1 \"main\""; // exercises escaping
    p.description = "desc";
    p.groupHint = "studio-a-cam-1:Video";
    p.width = 1920;
    p.height = 1080;
    p.rateNumerator = 50;
    p.rateDenominator = 1;

    auto const json = buildVideoFlowDef(p);
    CHECK(json.find("\"id\":\"5fbec3b1-1b0f-417d-9059-8b94a47197ed\"") != std::string::npos);
    CHECK(json.find("\"format\":\"urn:x-nmos:format:video\"") != std::string::npos);
    CHECK(json.find("\"media_type\":\"video/v210\"") != std::string::npos);
    CHECK(json.find("\"grain_rate\":{\"numerator\":50,\"denominator\":1}") != std::string::npos);
    CHECK(json.find("\"frame_width\":1920") != std::string::npos);
    CHECK(json.find("\"frame_height\":1080") != std::string::npos);
    CHECK(json.find("\"interlace_mode\":\"progressive\"") != std::string::npos);
    CHECK(json.find("urn:x-nmos:tag:grouphint/v1.0") != std::string::npos);
    CHECK(json.find("cam 1 \\\"main\\\"") != std::string::npos);
    CHECK(json.find("\"bit_depth\":10") != std::string::npos);

    p.withAlpha = true;
    auto const jsonA = buildVideoFlowDef(p);
    CHECK(jsonA.find("\"media_type\":\"video/v210a\"") != std::string::npos);
    CHECK(jsonA.find("{\"name\":\"A\"") != std::string::npos);

    p.interlaced = true;
    auto const jsonI = buildVideoFlowDef(p);
    CHECK(jsonI.find("\"interlace_mode\":\"interlaced_tff\"") != std::string::npos);
}

TEST_CASE("audio flow definition JSON")
{
    AudioFlowParams p;
    p.id = *util::parseUuid("b3bb5be7-9fe9-4324-a5bb-4c70e1084449");
    p.label = "audio";
    p.description = "d";
    p.groupHint = "g:Audio";
    p.channelCount = 16;
    p.sourceId = util::parseUuid("2aa143ac-0ab7-4d75-bc32-5c00c13d186f");

    auto const json = buildAudioFlowDef(p);
    CHECK(json.find("\"media_type\":\"audio/float32\"") != std::string::npos);
    CHECK(json.find("\"sample_rate\":{\"numerator\":48000,\"denominator\":1}") != std::string::npos);
    CHECK(json.find("\"channel_count\":16") != std::string::npos);
    CHECK(json.find("\"bit_depth\":32") != std::string::npos);
    CHECK(json.find("\"source_id\":\"2aa143ac-0ab7-4d75-bc32-5c00c13d186f\"") != std::string::npos);
}

TEST_CASE("anc flow definition JSON")
{
    AncFlowParams p;
    p.id = *util::parseUuid("db3bd465-2772-484f-8fac-830b0471258b");
    p.label = "anc";
    p.description = "d";
    p.groupHint = "g:ANC";
    p.rateNumerator = 60000;
    p.rateDenominator = 1001;

    auto const json = buildAncFlowDef(p);
    CHECK(json.find("\"format\":\"urn:x-nmos:format:data\"") != std::string::npos);
    CHECK(json.find("\"media_type\":\"video/smpte291\"") != std::string::npos);
    CHECK(json.find("\"grain_rate\":{\"numerator\":60000,\"denominator\":1001}") != std::string::npos);
}

TEST_CASE("writer options carry the commit batch hint (§4.2)")
{
    CHECK(buildWriterOptions(256) == "{\"maxCommitBatchSizeHint\":256}");
}
