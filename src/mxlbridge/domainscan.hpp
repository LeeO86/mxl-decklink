// SPDX-License-Identifier: MIT
// MXL domain and flow discovery + domain creation (SPECIFICATION.md §7.6).
//
// Everything here is deliberately filesystem-based (documented MXL v1.0.1
// on-disk layout: {domain}/{uuid}.mxl-flow/{flow_def.json,data,...},
// domain-level options.json, and the mxl-hands-on domain_def.json marker):
// no mxlInstance is created on foreign domains, so browsing can never disturb
// them.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mxldl::mxlbridge
{
    struct DomainInfo
    {
        std::string path;
        std::optional<std::string> id; // from domain_def.json
        std::optional<std::string> label;
        std::optional<std::string> description;
        bool isTmpfs = false;
        bool hasOptionsJson = false;
        std::optional<std::uint64_t> historyDurationNs; // from options.json
        std::size_t flowCount = 0;
    };

    struct FlowSummary
    {
        std::string id; // flow UUID
        std::string format; // urn:x-nmos:format:…
        std::string mediaType; // video/v210, audio/float32, video/smpte291
        std::string label;
        std::string groupHint;
        // Video geometry / rate (when present in the descriptor).
        std::optional<std::uint32_t> width;
        std::optional<std::uint32_t> height;
        std::optional<std::int64_t> rateNumerator;
        std::optional<std::int64_t> rateDenominator;
        bool interlaced = false;
        std::optional<std::uint32_t> channelCount; // audio
        // Liveness + runtime info (flock check / mxlFlowInfo header).
        bool active = false;
        std::optional<std::uint64_t> headIndex;
        std::optional<std::uint64_t> lastWriteTimeNs;
        std::optional<std::uint32_t> grainCount;
    };

    /// Scans `root` (bounded depth) for MXL domains: directories carrying a
    /// domain_def.json, an options.json, or at least one *.mxl-flow entry.
    std::vector<DomainInfo> scanDomains(std::string const& root, int maxDepth = 3);

    /// Lists the flows of one domain directory.
    std::vector<FlowSummary> listFlows(std::string const& domainPath);

    struct CreateDomainRequest
    {
        std::string path; // absolute, must be contained in the scan root
        std::string label;
        std::string description;
        std::optional<std::uint64_t> historyDurationNs; // writes options.json
    };

    struct CreateDomainResult
    {
        std::string path;
        std::string id; // generated domain UUID
        bool isTmpfs = false; // callers warn when false
    };

    /// Creates a domain directory per §7.6: mkdir -p, domain_def.json
    /// (generated id + label/description), optional options.json. Fails when
    /// the path escapes `scanRoot` or a domain already exists there.
    std::variant<CreateDomainResult, std::string> createDomain(CreateDomainRequest const& request, std::string const& scanRoot);
}
