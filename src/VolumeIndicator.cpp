#include "VolumeIndicator.hpp"
#include "AudioCapture.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>

using namespace geode::prelude;

namespace voicecontrol {

    VolumeIndicator* VolumeIndicator::create() {
        auto* ret = new VolumeIndicator();
        if (ret && ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }

    bool VolumeIndicator::init() {
        if (!CCNode::init()) return false;

        const auto winSize = CCDirector::sharedDirector()->getWinSize();
        setContentSize(winSize);

        const float barMaxW = winSize.width - 24.0f;

        m_background = CCLayerColor::create(ccc4(0, 0, 0, 120), barMaxW, 8.0f);
        m_background->ignoreAnchorPointForPosition(false);
        m_background->setAnchorPoint(ccp(0.5f, 0.5f));
        m_background->setPosition(ccp(winSize.width * 0.5f, 18.0f));
        addChild(m_background, 0);

        m_fill = CCLayerColor::create(ccc4(0, 220, 0, 220), 0.0f, 8.0f);
        m_fill->ignoreAnchorPointForPosition(false);
        m_fill->setAnchorPoint(ccp(0.0f, 0.5f));
        m_fill->setPosition(ccp(0.0f, 4.0f));
        m_background->addChild(m_fill, 1);

        scheduleUpdate();
        return true;
    }

    void VolumeIndicator::update(float /*dt*/) {
        const auto  winSize   = CCDirector::sharedDirector()->getWinSize();
        const float barMaxW   = std::max(1.0f, winSize.width - 24.0f);
        const float threshold = std::max(
            0.0001f, Mod::get()->getSettingValue<float>("sensitivity")
        );
        const float rms   = g_currentRMS.load(std::memory_order_relaxed);
        const float ratio = std::clamp(rms / threshold, 0.0f, 1.0f);

        m_background->setContentSize(CCSize(barMaxW, 8.0f));
        m_background->setPosition(ccp(winSize.width * 0.5f, 18.0f));
        m_fill->setContentSize(CCSize(barMaxW * ratio, 8.0f));

        ccColor3B col;
        if      (rms >= threshold)          col = { 255, 64,  64  };
        else if (rms >= threshold * 0.8f)   col = { 255, 220, 64  };
        else                                col = { 64,  220, 96  };

        m_fill->setColor(col);
    }

} // namespace voicecontrol
