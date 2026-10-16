// SPDX-License-Identifier: MIT
// Interlaced video in MXL (util/interlace.hpp): field split and weave, the
// index and rate math, and an interlaced flow written field by field and read
// back as woven frames.
#include <cstring>
#include <filesystem>
#include <vector>

#include <unistd.h>

#include <doctest/doctest.h>
#include <mxl/flow.h>
#include <mxl/time.h>

#include "config/videomodes.hpp"
#include "mxlbridge/domain.hpp"
#include "mxlbridge/flowdef.hpp"
#include "mxlbridge/videoreader.hpp"
#include "mxlbridge/videowriter.hpp"
#include "util/interlace.hpp"
#include "util/v210.hpp"

using namespace mxldl;
namespace fs = std::filesystem;

namespace
{
    /// A frame whose every byte of line r is the low byte of r * 7 + column.
    std::vector<std::uint8_t> makeFrame(std::size_t rowBytes, std::uint32_t height)
    {
        std::vector<std::uint8_t> frame(rowBytes * height);
        for (std::uint32_t r = 0; r < height; ++r)
        {
            for (std::size_t c = 0; c < rowBytes; ++c)
            {
                frame[r * rowBytes + c] = static_cast<std::uint8_t>(r * 7 + c);
            }
        }
        return frame;
    }

    bool rowEquals(std::uint8_t const* a, std::uint8_t const* b, std::size_t n)
    {
        return std::memcmp(a, b, n) == 0;
    }
}

TEST_CASE("field parity per grain index: tff even lines first, bff odd lines first")
{
    CHECK(util::fieldParity(0, false) == 0);
    CHECK(util::fieldParity(1, false) == 1);
    CHECK(util::fieldParity(100, false) == 0);
    CHECK(util::fieldParity(101, false) == 1);
    CHECK(util::fieldParity(0, true) == 1);
    CHECK(util::fieldParity(1, true) == 0);
    CHECK(util::fieldParity(100, true) == 1);
    CHECK(util::fieldParity(101, true) == 0);
}

TEST_CASE("split a frame into its fields and weave them back: every line back in place (tff and bff)")
{
    std::uint32_t const height = 1080;
    std::size_t const rowBytes = util::v210RowBytes(1920);
    auto const frame = makeFrame(rowBytes, height);

    for (bool const bff : {false, true})
    {
        CAPTURE(bff);
        // Grains 2k and 2k + 1 of one frame.
        std::vector<std::uint8_t> field[2];
        for (unsigned i = 0; i < 2; ++i)
        {
            field[i].assign(rowBytes * height / 2, 0);
            util::extractField(frame.data(), rowBytes, height, util::fieldParity(2 * 21 + i, bff), field[i].data(), rowBytes);
        }
        // The first field (grain 2k) holds the even lines for tff, the odd
        // lines for bff.
        unsigned const firstParity = bff ? 1 : 0;
        for (std::uint32_t r = 0; r < height / 2; ++r)
        {
            REQUIRE(rowEquals(field[0].data() + r * rowBytes, frame.data() + (2 * r + firstParity) * rowBytes, rowBytes));
            REQUIRE(rowEquals(field[1].data() + r * rowBytes, frame.data() + (2 * r + (firstParity ^ 1)) * rowBytes, rowBytes));
        }

        std::vector<std::uint8_t> woven(frame.size(), 0);
        for (unsigned i = 0; i < 2; ++i)
        {
            util::weaveField(field[i].data(), rowBytes, height, util::fieldParity(2 * 21 + i, bff), woven.data(), rowBytes);
        }
        CHECK(woven == frame);
    }
}

TEST_CASE("field split with a wider source stride (an 8-bit row or a padded card buffer) copies the grain's line length")
{
    std::uint32_t const height = 486; // NTSC
    std::size_t const srcRow = 2048;
    std::size_t const lineBytes = util::v210RowBytes(720);
    auto const frame = makeFrame(srcRow, height);
    std::vector<std::uint8_t> field(lineBytes * height / 2, 0);
    util::extractField(frame.data(), srcRow, height, 1, field.data(), lineBytes);
    for (std::uint32_t r = 0; r < height / 2; ++r)
    {
        REQUIRE(rowEquals(field.data() + r * lineBytes, frame.data() + (2 * r + 1) * srcRow, lineBytes));
    }
}

TEST_CASE("frame index k from the frame's start time: grain 2k for the first field, the field timestamps on the TAI grid")
{
    mxlRational const i50{25, 1};
    mxlRational const field50{50, 1};
    std::uint64_t const k = 44'803'311'586ULL;
    auto const frameStart = ::mxlIndexToTimestamp(&i50, k);
    // The frame arrives one period after its start, a few ms late or early.
    for (std::int64_t const lateMs : {0, 3, 15, -15})
    {
        CAPTURE(lateMs);
        auto const arrival = static_cast<std::uint64_t>(static_cast<std::int64_t>(frameStart + 40'000'000ULL) + lateMs * 1'000'000LL);
        CHECK(util::firstFieldGrainIndex(i50, arrival) == 2 * k);
    }
    // Grain 2k of the field-rate flow is the frame's start, 2k + 1 half a
    // frame later.
    CHECK(::mxlIndexToTimestamp(&field50, 2 * k) == frameStart);
    CHECK(::mxlIndexToTimestamp(&field50, 2 * k + 1) == frameStart + 20'000'000ULL);

    mxlRational const i5994{30000, 1001};
    mxlRational const field5994{60000, 1001};
    std::uint64_t const k2 = 53'709'000'000ULL;
    auto const start2 = ::mxlIndexToTimestamp(&i5994, k2);
    CHECK(util::firstFieldGrainIndex(i5994, start2 + 33'366'667ULL + 2'000'000ULL) == 2 * k2);
    CHECK(::mxlIndexToTimestamp(&field5994, 2 * k2) == start2);
}

TEST_CASE("newest complete frame of a field head")
{
    CHECK(util::frameHeadOfFieldHead(0) == 0);
    CHECK(util::frameHeadOfFieldHead(1) == 0);
    CHECK(util::frameHeadOfFieldHead(2) == 0); // frame 1 has its first field only
    CHECK(util::frameHeadOfFieldHead(3) == 1);
    CHECK(util::frameHeadOfFieldHead(2 * 1000 + 1) == 1000);
    CHECK(util::frameHeadOfFieldHead(2 * 1000) == 999);
}

TEST_CASE("ANC packets by field: SMPTE line numbers of the second field")
{
    // 1080i: field 1 lines 1-563, field 2 lines 564-1125.
    CHECK_FALSE(util::ancLineInSecondField(1080, 9));
    CHECK_FALSE(util::ancLineInSecondField(1080, 563));
    CHECK(util::ancLineInSecondField(1080, 564));
    CHECK(util::ancLineInSecondField(1080, 572));
    // 625: field 1 lines 1-312, field 2 lines 313-625.
    CHECK_FALSE(util::ancLineInSecondField(576, 23));
    CHECK(util::ancLineInSecondField(576, 336));
    // 525: field 1 lines 4-265, field 2 lines 266-525 and 1-3.
    CHECK_FALSE(util::ancLineInSecondField(486, 21));
    CHECK(util::ancLineInSecondField(486, 284));
    CHECK(util::ancLineInSecondField(486, 2));
    // Progressive heights: no second field.
    CHECK_FALSE(util::ancLineInSecondField(720, 600));
}

TEST_CASE("field order and ANC rate of the interlaced modes")
{
    CHECK(config::lookupVideoMode("NTSC")->bottomFieldFirst);
    CHECK_FALSE(config::lookupVideoMode("PAL")->bottomFieldFirst);
    CHECK_FALSE(config::lookupVideoMode("HD1080i50")->bottomFieldFirst);
    CHECK_FALSE(config::lookupVideoMode("HD1080i5994")->bottomFieldFirst);
    CHECK_FALSE(config::lookupVideoMode("HD1080p50")->bottomFieldFirst);
}

TEST_CASE("an interlaced flow written field by field reads back as woven frames (tff PAL, bff NTSC)")
{
    auto const path = fs::temp_directory_path() / ("mxldl-interlace-" + std::to_string(::getpid()));
    fs::remove_all(path);
    {
        mxlbridge::Domain domain(path.string());
        struct Case
        {
            char const* mode;
            char const* id;
            char const* interlaceMode;
        };
        for (auto const& c : {Case{"PAL", "7a0c2e11-1c1d-4a55-9a6e-0b1c2d3e4f01", "interlaced_tff"},
                 Case{"NTSC", "7a0c2e11-1c1d-4a55-9a6e-0b1c2d3e4f02", "interlaced_bff"}})
        {
            CAPTURE(c.mode);
            auto const mode = *config::lookupVideoMode(c.mode);
            mxlbridge::VideoFlowParams p;
            p.id = *util::parseUuid(c.id);
            p.label = c.mode;
            p.groupHint = "interlace:Video";
            p.width = mode.width;
            p.height = mode.height;
            p.rateNumerator = mode.rateNumerator;
            p.rateDenominator = mode.rateDenominator;
            p.interlaced = mode.interlaced;
            p.bottomFieldFirst = mode.bottomFieldFirst;
            mxlbridge::VideoWriter writer(domain, p, 1);

            std::size_t const lineBytes = util::v210RowBytes(mode.width);
            // One field per grain at twice the frame rate.
            CHECK(writer.grainPayloadSize() == lineBytes * mode.height / 2);
            CHECK(writer.grainRate().numerator == 2 * mode.rateNumerator);
            CHECK(std::string(mxlbridge::buildVideoFlowDef(p)).find(std::string("\"interlace_mode\":\"") + c.interlaceMode + "\"") != std::string::npos);

            auto const frame = makeFrame(lineBytes, mode.height);
            mxlRational const frameRate{mode.rateNumerator, mode.rateDenominator};
            auto const k = ::mxlGetCurrentIndex(&frameRate);
            auto const first = util::fieldParity(2 * k, mode.bottomFieldFirst);
            REQUIRE(writer.writeField(2 * k, frame.data(), lineBytes, mode.height, first) == MXL_STATUS_OK);
            REQUIRE(writer.writeField(2 * k + 1, frame.data(), lineBytes, mode.height, first ^ 1U) == MXL_STATUS_OK);

            mxlbridge::VideoReader reader(domain, c.id);
            CHECK(reader.interlaced());
            CHECK(reader.bottomFieldFirst() == mode.bottomFieldFirst);
            CHECK(reader.grainRate().numerator == mode.rateNumerator);
            CHECK(reader.grainRate().denominator == mode.rateDenominator);
            CHECK(reader.headIndex() == k);
            // The sync wait waits for the second field: half a frame later.
            mxlRational const fieldRate{2 * mode.rateNumerator, mode.rateDenominator};
            CHECK(reader.syncTimestamp(k) == ::mxlIndexToTimestamp(&fieldRate, 2 * k + 1));
            CHECK(reader.syncTimestamp(k) - ::mxlIndexToTimestamp(&frameRate, k) == doctest::Approx(500'000'000.0 * mode.rateDenominator / mode.rateNumerator).epsilon(1e-6));

            // The raw grains: 2k is the first field in time (even lines for
            // tff, odd lines for bff), each height / 2 lines.
            mxlGrainInfo info{};
            std::uint8_t* payload = nullptr;
            REQUIRE(::mxlFlowReaderGetGrain(reader.handle(), 2 * k, 0, &info, &payload) == MXL_STATUS_OK);
            CHECK(info.grainSize == lineBytes * mode.height / 2);
            for (std::uint32_t r = 0; r < mode.height / 2; ++r)
            {
                REQUIRE(rowEquals(payload + r * lineBytes, frame.data() + (2 * r + (mode.bottomFieldFirst ? 1 : 0)) * lineBytes, lineBytes));
            }

            // Frame k woven from both fields: every line back in place.
            mxlbridge::VideoReader::Grain grain;
            REQUIRE(reader.getGrain(k, 0, grain) == MXL_STATUS_OK);
            REQUIRE(grain.payload != nullptr);
            CHECK((grain.info.flags & MXL_GRAIN_FLAG_INVALID) == 0);
            REQUIRE(grain.info.grainSize == frame.size());
            CHECK(std::memcmp(grain.payload, frame.data(), frame.size()) == 0);

            // Frame k + 1 with only its first field written is not there yet.
            REQUIRE(writer.writeField(2 * k + 2, frame.data(), lineBytes, mode.height, first) == MXL_STATUS_OK);
            CHECK(reader.headIndex() == k);
            CHECK(reader.getGrain(k + 1, 0, grain) == MXL_ERR_OUT_OF_RANGE_TOO_EARLY);
        }
    }
    std::error_code ec;
    fs::remove_all(path, ec);
}
