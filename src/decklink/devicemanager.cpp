// SPDX-License-Identifier: MIT
#include "devicemanager.hpp"

#include "util/logging.hpp"

namespace mxldl::dl
{
    std::variant<std::unique_ptr<ICard>, std::string> openConfiguredCard(config::Config const& cfg, config::EnvReader const& env)
    {
        std::unique_ptr<IBackend> backend;
        if (cfg.backend == "mock")
        {
            log::warn("mock_backend_active", {{"details", "MXL_DECKLINK_BACKEND=mock is a testing facility; no hardware is used"}});
            backend = makeMockBackend(env);
        }
        else
        {
            backend = makeSdkBackend();
        }

        IBackend::CardSelector selector;
        selector.persistentId = cfg.cardId;
        selector.displayName = cfg.cardName;
        selector.index = cfg.cardIndex;

        // §3.1: name/index selection is a lab fallback, not reboot-stable.
        if (cfg.cardName)
        {
            log::warn("card_selection_by_name", {{"details", "MXL_DECKLINK_CARD_NAME selection is not reboot-stable; prefer MXL_DECKLINK_CARD_ID"}});
        }
        if (cfg.cardIndex)
        {
            log::warn("card_selection_by_index",
                {{"details", "MXL_DECKLINK_CARD_INDEX selection is not reboot-stable; prefer MXL_DECKLINK_CARD_ID"}});
        }

        auto result = backend->openCard(selector);
        if (std::holds_alternative<std::string>(result))
        {
            return std::get<std::string>(result);
        }
        auto card = std::move(std::get<std::unique_ptr<ICard>>(result));

        // Keep the backend alive for the lifetime of the card: wrap it into
        // the returned unique_ptr via a small adapter.
        struct CardWithBackend : ICard
        {
            std::unique_ptr<IBackend> backend;
            std::unique_ptr<ICard> card;

            [[nodiscard]] std::string const& displayName() const override
            {
                return card->displayName();
            }

            [[nodiscard]] std::uint32_t persistentId() const override
            {
                return card->persistentId();
            }

            [[nodiscard]] std::size_t subDeviceCount() const override
            {
                return card->subDeviceCount();
            }

            ISubDevice& subDevice(std::size_t index) override
            {
                return card->subDevice(index);
            }

            std::optional<std::string> applyProfile(config::CardProfile profile) override
            {
                return card->applyProfile(profile);
            }

            void setCallbacks(CardCallbacks callbacks) override
            {
                card->setCallbacks(std::move(callbacks));
            }
        };

        // §4.3 cross-check: every configured sub-device index must exist.
        for (auto const& ch : cfg.channels)
        {
            if (static_cast<std::size_t>(ch.subdeviceIndex) >= card->subDeviceCount())
            {
                return "channel " + std::to_string(ch.index) + ": sub-device index " + std::to_string(ch.subdeviceIndex) +
                       " does not exist on card '" + card->displayName() + "' (" + std::to_string(card->subDeviceCount()) + " sub-devices)";
            }
            auto const& sub = card->subDevice(static_cast<std::size_t>(ch.subdeviceIndex));
            if (ch.direction == config::Direction::Input && !sub.info().supportsCapture)
            {
                return "channel " + std::to_string(ch.index) + ": sub-device " + std::to_string(ch.subdeviceIndex) + " does not support capture";
            }
            if (ch.direction == config::Direction::Output && !sub.info().supportsPlayback)
            {
                return "channel " + std::to_string(ch.index) + ": sub-device " + std::to_string(ch.subdeviceIndex) + " does not support playback";
            }
            // §3.2: auto mode requires input format detection support.
            if (ch.isAutoMode() && !sub.info().supportsInputFormatDetection)
            {
                return "channel " + std::to_string(ch.index) + ": CHx_VIDEO_MODE=auto requires input format detection, which sub-device " +
                       std::to_string(ch.subdeviceIndex) + " does not support";
            }
        }

        auto wrapper = std::make_unique<CardWithBackend>();
        wrapper->backend = std::move(backend);
        wrapper->card = std::move(card);
        return std::unique_ptr<ICard>(std::move(wrapper));
    }
}
