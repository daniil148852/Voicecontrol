#include <Geode/Geode.hpp>

#include "AudioCapture.hpp"
#include "InputInjector.hpp"
#include "VolumeIndicator.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <functional>

using namespace geode::prelude;
namespace voicecontrol {
    static AudioCapture g_audio;
    static InputInjector g_injector;
    static VolumeIndicator* g_indicator = nullptr;
    static std::atomic<bool> g_hardDisabled { false };

    static constexpr int kIndicatorTag = 0x5653;
    static constexpr float kSliderMinSensitivity = 0.001f;
    static constexpr float kSliderMaxSensitivity = 0.5f;
    static void showUserNotification(const std::string& message) {
        log::warn("VoiceControl: {}", message);
        auto notif = Notification::create("VoiceControl", message.c_str());
        if (notif) notif->show();
    }

    static bool ensureAudioRunning() {
        if (g_hardDisabled.load(std::memory_order_relaxed)) {
            return false;
        }

        if (g_audio.isAvailable()) {
            return true;
        }

        if (!g_audio.init()) {
            g_hardDisabled.store(true, std::memory_order_relaxed);
            return false;
        }

        if (!g_audio.start()) {
            g_hardDisabled.store(true, std::memory_order_relaxed);
            return false;
        }

        return true;
    }

    static void stopAudio() {
        g_audio.stop();
    }

    static void attachIndicatorToLayer(CCNode* layer) {
        if (!layer) {
            return;
        }

        if (g_indicator && g_indicator->getParent()) {
            g_indicator->removeFromParentAndCleanup(true);
            g_indicator = nullptr;
        }

        if (!Mod::get()->getSettingValue<bool>("show_volume_indicator")) {
            return;
        }

        g_indicator = VolumeIndicator::create();
        if (!g_indicator) {
            return;
        }

        g_indicator->setTag(kIndicatorTag);
        layer->addChild(g_indicator, 999999);
    }

    static void removeIndicator() {
        if (g_indicator) {
            g_indicator->removeFromParentAndCleanup(true);
            g_indicator = nullptr;
        }
    }

    class SensitivitySlider : public CCLayer {
    public:
        static SensitivitySlider* create(float initialValue, std::function<void(float)> onChanged) {
            auto ret = new SensitivitySlider();
            if (ret && ret->init(initialValue, std::move(onChanged))) {
                ret->autorelease();
                return ret;
            }
            delete ret;
            return nullptr;
        }

        bool init(float initialValue, std::function<void(float)> onChanged) {
            if (!CCLayer::init()) {
                return false;
            }

            m_onChanged = std::move(onChanged);
            setContentSize(CCSize(260.0f, 42.0f));
            setTouchEnabled(true);
            m_track = CCLayerColor::create(ccc4(75, 75, 75, 220), 260.0f, 10.0f);
            m_track->ignoreAnchorPointForPosition(false);
            m_track->setAnchorPoint(ccp(0.0f, 0.5f));
            m_track->setPosition(ccp(0.0f, 21.0f));
            addChild(m_track);
            m_fill = CCLayerColor::create(ccc4(90, 180, 255, 220), 0.0f, 10.0f);
            m_fill->ignoreAnchorPointForPosition(false);
            m_fill->setAnchorPoint(ccp(0.0f, 0.5f));
            m_fill->setPosition(ccp(0.0f, 21.0f));
            addChild(m_fill);
            m_knob = CCLayerColor::create(ccc4(240, 240, 240, 255), 14.0f, 24.0f);
            m_knob->ignoreAnchorPointForPosition(false);
            m_knob->setAnchorPoint(ccp(0.5f, 0.5f));
            addChild(m_knob);

            m_valueLabel = CCLabelBMFont::create("", "bigFont.fnt");
            m_valueLabel->setScale(0.42f);
            m_valueLabel->setAnchorPoint(ccp(0.0f, 0.5f));
            m_valueLabel->setPosition(ccp(0.0f, 38.0f));
            addChild(m_valueLabel);

            setActualValue(initialValue, false);
            return true;
        }

        void registerWithTouchDispatcher() override {
            CCTouchDispatcher::sharedDispatcher()->addTargetedDelegate(this, 0, true);
        }

        bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
            const CCPoint p = convertToNodeSpace(touch->getLocation());
            if (p.x < 0.0f || p.x > 260.0f || p.y < 0.0f || p.y > 42.0f) {
                return false;
            }

            m_dragging = true;
            updateFromX(p.x);
            return true;
        }

        void ccTouchMoved(CCTouch* touch, CCEvent*) override {
            if (!m_dragging) {
                return;
            }

            const CCPoint p = convertToNodeSpace(touch->getLocation());
            updateFromX(p.x);
        }

        void ccTouchEnded(CCTouch*, CCEvent*) override {
            m_dragging = false;
        }

        void ccTouchCancelled(CCTouch*, CCEvent*) override {
            m_dragging = false;
        }

        void setActualValue(float value, bool notify = true) {
            const float normalized = std::clamp(
                (value - kSliderMinSensitivity) / (kSliderMaxSensitivity - kSliderMinSensitivity),
                0.0f,
                1.0f
            );
            m_value = normalized;
            updateVisuals(notify);
        }

        float getActualValue() const {
            return kSliderMinSensitivity + m_value * (kSliderMaxSensitivity - kSliderMinSensitivity);
        }

    private:
        void updateFromX(float x) {
            m_value = std::clamp(x / 260.0f, 0.0f, 1.0f);
            updateVisuals(true);
        }

        void updateVisuals(bool notify) {
            const float actualValue = getActualValue();
            m_fill->setContentSize(CCSize(260.0f * m_value, 10.0f));
            m_knob->setPosition(ccp(260.0f * m_value, 21.0f));

            std::ostringstream ss;
            ss << "Sensitivity: " << std::fixed << std::setprecision(4) << actualValue;
            m_valueLabel->setString(ss.str().c_str());

            if (notify && m_onChanged) {
                m_onChanged(actualValue);
            }
        }

        bool m_dragging = false;
        float m_value = 0.0f;

        std::function<void(float)> m_onChanged;
        CCLayerColor* m_track = nullptr;
        CCLayerColor* m_fill = nullptr;
        CCLayerColor* m_knob = nullptr;
        CCLabelBMFont* m_valueLabel = nullptr;

        static constexpr float kSliderMinSensitivity = 0.001f;
        static constexpr float kSliderMaxSensitivity = 0.5f;
    };
    class TestMicPanel : public CCLayerColor {
    public:
        static TestMicPanel* create() {
            auto ret = new TestMicPanel();
            if (ret && ret->init()) {
                ret->autorelease();
                return ret;
            }
            delete ret;
            return nullptr;
        }

        bool init() override {
            if (!CCLayerColor::initWithColor(ccc4(18, 18, 18, 230), 290.0f, 190.0f)) {
                return false;
            }

            setContentSize(CCSize(290.0f, 190.0f));
            ignoreAnchorPointForPosition(false);
            setAnchorPoint(ccp(0.5f, 0.5f));
            auto title = CCLabelBMFont::create("Test Microphone", "bigFont.fnt");
            title->setScale(0.6f);
            title->setPosition(ccp(145.0f, 168.0f));
            addChild(title);

            m_meterBack = CCLayerColor::create(ccc4(55, 55, 55, 255), 240.0f, 14.0f);
            m_meterBack->ignoreAnchorPointForPosition(false);
            m_meterBack->setAnchorPoint(ccp(0.0f, 0.5f));
            m_meterBack->setPosition(ccp(25.0f, 118.0f));
            addChild(m_meterBack);

            m_meterFill = CCLayerColor::create(ccc4(64, 220, 96, 255), 0.0f, 14.0f);
            m_meterFill->ignoreAnchorPointForPosition(false);
            m_meterFill->setAnchorPoint(ccp(0.0f, 0.5f));
            m_meterFill->setPosition(ccp(0.0f, 7.0f));
            m_meterBack->addChild(m_meterFill);
            m_rmsLabel = CCLabelBMFont::create("RMS: 0.0000", "bigFont.fnt");
            m_rmsLabel->setScale(0.45f);
            m_rmsLabel->setPosition(ccp(145.0f, 92.0f));
            addChild(m_rmsLabel);

            m_statusLabel = CCLabelBMFont::create("SILENT", "bigFont.fnt");
            m_statusLabel->setScale(0.55f);
            m_statusLabel->setPosition(ccp(145.0f, 66.0f));
            addChild(m_statusLabel);
            m_noteLabel = CCLabelBMFont::create("", "bigFont.fnt");
            m_noteLabel->setScale(0.34f);
            m_noteLabel->setPosition(ccp(145.0f, 44.0f));
            addChild(m_noteLabel);

            auto currentSensitivity = Mod::get()->getSettingValue<float>("sensitivity");
            m_slider = SensitivitySlider::create(currentSensitivity, [](float value) {
                auto mod = Mod::get();
                if (!mod) return;

                mod->setSettingValue("sensitivity", value);
            });
            m_slider->setPosition(ccp(15.0f, 5.0f));
            addChild(m_slider);

            schedule(schedule_selector(TestMicPanel::refresh), 0.05f);
            refresh(0.0f);
            return true;
        }

        void refresh(float) {
            const bool audioReady = ensureAudioRunning();
            const float threshold = std::max(0.0001f, Mod::get()->getSettingValue<float>("sensitivity"));
            const float rms = g_currentRMS.load(std::memory_order_relaxed);

            std::ostringstream ss;
            ss << "RMS: " << std::fixed << std::setprecision(4) << rms;
            m_rmsLabel->setString(ss.str().c_str());

            const float ratio = std::clamp(rms / threshold, 0.0f, 1.0f);
            const float meterWidth = 240.0f * ratio;
            m_meterFill->setContentSize(CCSize(meterWidth, 14.0f));

            if (!audioReady) {
                m_meterFill->setColor({ 128, 128, 128 });
                m_statusLabel->setString("MIC OFF");
                m_statusLabel->setColor({ 180, 180, 180 });
                m_noteLabel->setString("Grant mic permission via ADB.");
                m_noteLabel->setColor({ 180, 180, 180 });
                return;
            }

            if (rms >= threshold) {
                m_meterFill->setColor({ 255, 64, 64 });
                m_statusLabel->setString("TRIGGERING");
                m_statusLabel->setColor({ 255, 64, 64 });
                m_noteLabel->setString("");
            }
            else if (rms >= threshold * 0.8f) {
                m_meterFill->setColor({ 255, 220, 64 });
                m_statusLabel->setString("SILENT");
                m_statusLabel->setColor({ 180, 180, 180 });
                m_noteLabel->setString("Close to trigger.");
                m_noteLabel->setColor({ 220, 220, 120 });
            }
            else {
                m_meterFill->setColor({ 64, 220, 96 });
                m_statusLabel->setString("SILENT");
                m_statusLabel->setColor({ 180, 180, 180 });
                m_noteLabel->setString("");
            }
        }

    private:
        CCLayerColor* m_meterBack = nullptr;
        CCLayerColor* m_meterFill = nullptr;
        CCLabelBMFont* m_rmsLabel = nullptr;
        CCLabelBMFont* m_statusLabel = nullptr;
        CCLabelBMFont* m_noteLabel = nullptr;
        SensitivitySlider* m_slider = nullptr;
    };

    static void openTestPopup() {
        ensureAudioRunning();

        auto popup = FLAlertLayer::create(
            nullptr,
            "VoiceControl",
            "",
            "Close",
            nullptr,
            320.0f,
            false,
            220.0f
        );

        if (!popup) {
            return;
        }

        auto panel = TestMicPanel::create();
        if (!panel) {
            return;
        }

        panel->setPosition(ccp(160.0f, 110.0f));
        popup->addChild(panel, 10);
        popup->show();
    }

    class $modify(VoiceControlSettingsLayer, SettingsLayer) {
        bool init() {
            if (!SettingsLayer::init()) {
                return false;
            }

            auto winSize = CCDirector::sharedDirector()->getWinSize();

            auto menu = CCMenu::create();
            menu->setPosition(CCPointZero);
            this->addChild(menu, 9999);

            auto buttonNode = CCNode::create();
            buttonNode->setContentSize(CCSize(180.0f, 32.0f));

            auto buttonBg = CCLayerColor::create(ccc4(50, 50, 50, 220), 180.0f, 32.0f);
            buttonBg->ignoreAnchorPointForPosition(false);
            buttonBg->setAnchorPoint(ccp(0.0f, 0.0f));
            buttonBg->setPosition(CCPointZero);
            buttonNode->addChild(buttonBg);

            auto buttonLabel = CCLabelBMFont::create("Test Microphone", "bigFont.fnt");
            buttonLabel->setScale(0.48f);
            buttonLabel->setPosition(ccp(90.0f, 16.0f));
            buttonNode->addChild(buttonLabel);
            auto button = CCMenuItemSpriteExtra::create(
                buttonNode,
                this,
                menu_selector(VoiceControlSettingsLayer::onTestMicrophone)
            );
            button->setPosition(ccp(winSize.width - 110.0f, 36.0f));
            menu->addChild(button);

            return true;
        }

        void onTestMicrophone(CCObject*) {
            openTestPopup();
        }
    };

    class $modify(VoiceControlPlayLayer, PlayLayer) {
        void onEnterTransitionDidFinish() {
            PlayLayer::onEnterTransitionDidFinish();
            if (!Mod::get()->getSettingValue<bool>("enabled")) {
                return;
            }

            if (ensureAudioRunning()) {
                g_injector.attach(this);
                attachIndicatorToLayer(this);
            }
        }

        void update(float dt) {
            if (!Mod::get()->getSettingValue<bool>("enabled")) {
                g_injector.forceRelease();
                removeIndicator();
                stopAudio();
                PlayLayer::update(dt);
                return;
            }

            if (!g_audio.isAvailable()) {
                ensureAudioRunning();
            }

            const float rms = g_currentRMS.load(std::memory_order_relaxed);
            g_injector.attach(this);
            g_injector.update(dt, rms);
            if (Mod::get()->getSettingValue<bool>("show_volume_indicator")) {
                if (!g_indicator || !g_indicator->getParent()) {
                    attachIndicatorToLayer(this);
                }
            }
            else {
                removeIndicator();
            }

            PlayLayer::update(dt);
        }

        void onQuit() {
            g_injector.forceRelease();
            g_injector.detach();
            removeIndicator();
            stopAudio();

            PlayLayer::onQuit();
        }
    };

    struct GlobalShutdown {
        ~GlobalShutdown() {
            g_injector.forceRelease();
            g_injector.detach();
            removeIndicator();
            stopAudio();
        }
    };

    static GlobalShutdown g_shutdown;
}
