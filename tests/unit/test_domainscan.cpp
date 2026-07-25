// SPDX-License-Identifier: MIT
#include <filesystem>
#include <fstream>

#include <unistd.h>

#include <doctest/doctest.h>

#include "mxlbridge/domainscan.hpp"

using namespace mxldl::mxlbridge;
namespace fs = std::filesystem;

namespace
{
    struct TempRoot
    {
        fs::path path;

        TempRoot()
        {
            static int counter = 0;
            path = fs::temp_directory_path() / ("mxldl-scan-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
            fs::create_directories(path);
        }

        ~TempRoot()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    void write(fs::path const& p, std::string const& content)
    {
        fs::create_directories(p.parent_path());
        std::ofstream out(p);
        out << content;
    }
}

TEST_CASE("domain scan recognizes the three marker types (§7.6)")
{
    TempRoot root;
    // 1) mxl-hands-on convention: domain_def.json
    write(root.path / "a" / "domain_def.json", R"({"id":"71ef9b5c-98c1-4f98-9def-1d61ee9a4fdb","label":"studio-a","description":"desc-a"})");
    // 2) MXL options.json
    write(root.path / "nested" / "b" / "options.json", R"({"urn:x-mxl:option:history_duration/v1.0": 100000000})");
    // 3) bare flow directory
    write(root.path / "c" / "5fbec3b1-1b0f-417d-9059-8b94a47197ed.mxl-flow" / "flow_def.json", "{}");
    // Not a domain: plain directory
    fs::create_directories(root.path / "not-a-domain");

    auto const domains = scanDomains(root.path.string());
    REQUIRE(domains.size() == 3);

    auto find = [&](std::string const& tail) -> DomainInfo const* {
        for (auto const& d : domains)
        {
            if (d.path.size() >= tail.size() && d.path.compare(d.path.size() - tail.size(), tail.size(), tail) == 0)
            {
                return &d;
            }
        }
        return nullptr;
    };

    auto const* a = find("/a");
    REQUIRE(a != nullptr);
    CHECK(a->label == "studio-a");
    CHECK(a->id == "71ef9b5c-98c1-4f98-9def-1d61ee9a4fdb");
    CHECK(a->flowCount == 0);

    auto const* b = find("/b");
    REQUIRE(b != nullptr);
    CHECK(b->historyDurationNs == 100'000'000ULL);

    auto const* c = find("/c");
    REQUIRE(c != nullptr);
    CHECK(c->flowCount == 1);
}

TEST_CASE("flow listing parses descriptors and reports inactive flows")
{
    TempRoot root;
    auto const flowDir = root.path / "dom" / "5fbec3b1-1b0f-417d-9059-8b94a47197ed.mxl-flow";
    write(flowDir / "flow_def.json", R"({
        "id":"5fbec3b1-1b0f-417d-9059-8b94a47197ed",
        "format":"urn:x-nmos:format:video",
        "media_type":"video/v210",
        "label":"cam 1",
        "tags":{"urn:x-nmos:tag:grouphint/v1.0":["studio:Video"]},
        "grain_rate":{"numerator":50,"denominator":1},
        "frame_width":1920,"frame_height":1080,"interlace_mode":"progressive"})");
    write(flowDir / "data", ""); // no lock holder → inactive; header unreadable → no runtime info

    auto const audioDir = root.path / "dom" / "b3bb5be7-9fe9-4324-a5bb-4c70e1084449.mxl-flow";
    write(audioDir / "flow_def.json", R"({
        "id":"b3bb5be7-9fe9-4324-a5bb-4c70e1084449",
        "format":"urn:x-nmos:format:audio",
        "media_type":"audio/float32",
        "sample_rate":{"numerator":48000},
        "channel_count":16})");

    auto const flows = listFlows((root.path / "dom").string());
    REQUIRE(flows.size() == 2);

    auto const& video = flows[0].mediaType == "video/v210" ? flows[0] : flows[1];
    auto const& audio = flows[0].mediaType == "video/v210" ? flows[1] : flows[0];
    CHECK(video.id == "5fbec3b1-1b0f-417d-9059-8b94a47197ed");
    CHECK(video.label == "cam 1");
    CHECK(video.groupHint == "studio:Video");
    CHECK(video.width == 1920);
    CHECK(video.height == 1080);
    CHECK(video.rateNumerator == 50);
    CHECK_FALSE(video.interlaced);
    CHECK_FALSE(video.active);
    CHECK_FALSE(video.headIndex.has_value());

    CHECK(audio.mediaType == "audio/float32");
    CHECK(audio.channelCount == 16);
    CHECK(audio.rateNumerator == 48000);
}

TEST_CASE("domain creation writes markers and enforces containment (§7.6)")
{
    TempRoot root;
    CreateDomainRequest req;
    req.path = (root.path / "new-domain").string();
    req.label = "new";
    req.description = "created by test";
    req.historyDurationNs = 150'000'000ULL;

    auto const result = createDomain(req, root.path.string());
    REQUIRE(std::holds_alternative<CreateDomainResult>(result));
    auto const& created = std::get<CreateDomainResult>(result);
    CHECK(fs::exists(fs::path(created.path) / "domain_def.json"));
    CHECK(fs::exists(fs::path(created.path) / "options.json"));

    // Round trip through the scanner.
    auto const domains = scanDomains(root.path.string());
    REQUIRE(domains.size() == 1);
    CHECK(domains[0].label == "new");
    CHECK(domains[0].id == created.id);
    CHECK(domains[0].historyDurationNs == 150'000'000ULL);

    // Escaping the scan root is rejected.
    CreateDomainRequest escape;
    escape.path = (root.path / ".." / "escape").string();
    escape.label = "x";
    auto const rejected = createDomain(escape, root.path.string());
    REQUIRE(std::holds_alternative<std::string>(rejected));

    // Creating over an existing domain is rejected.
    auto const dup = createDomain(req, root.path.string());
    REQUIRE(std::holds_alternative<std::string>(dup));
}

TEST_CASE("pathIsUnderRoot rejects traversal and non-segment prefixes")
{
    TempRoot root;
    auto const inside = (root.path / "child").string();
    fs::create_directories(inside);

    CHECK(pathIsUnderRoot(inside, root.path.string()));
    CHECK(pathIsUnderRoot(root.path.string(), root.path.string()));
    CHECK(pathIsUnderRoot((root.path / "child" / ".." / "child").string(), root.path.string()));

    // `..` that leaves the root.
    CHECK_FALSE(pathIsUnderRoot((root.path / ".." / "outside").string(), root.path.string()));

    // Non-segment prefix: /tmp/foo must not match /tmp/foobar.
    auto const sibling = root.path.string() + "-evil";
    fs::create_directories(sibling);
    CHECK_FALSE(pathIsUnderRoot(sibling, root.path.string()));
    std::error_code ec;
    fs::remove_all(sibling, ec);
}
