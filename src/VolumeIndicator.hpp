#pragma once

#include <Geode/Geode.hpp>

// Must come after Geode.hpp so cocos2d types are available.
using namespace geode::prelude;

namespace voicecontrol {

    class VolumeIndicator : public cocos2d::CCNode {
    public:
        static VolumeIndicator* create();

        bool init() override;
        void update(float dt) override;

    private:
        cocos2d::CCLayerColor* m_background = nullptr;
        cocos2d::CCLayerColor* m_fill       = nullptr;
    };

} // namespace voicecontrol
