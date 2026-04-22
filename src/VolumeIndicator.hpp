#pragma once

#include <Geode/Geode.hpp>

namespace voicecontrol {
    class VolumeIndicator : public CCNode {
    public:
        static VolumeIndicator* create();
        bool init() override;
        void update(float dt) override;

    private:
        CCLayerColor* m_background = nullptr;
        CCLayerColor* m_fill = nullptr;
    };
}
