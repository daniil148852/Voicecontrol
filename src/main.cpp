#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "AudioCapture.hpp"
#include "InputInjector.hpp"
#include "VolumeIndicator.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <functional>
#include <jni.h>

using namespace geode::prelude;

// ------------------------------------------------------------------ JVM pointer (global scope)
// Must be at global scope so JNI_OnLoad and AudioCapture.cpp can both access it.
JavaVM* g_voicecontrol_jvm = nullptr;

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_voicecontrol_jvm = vm;
    return JNI_VERSION_1_6;
}

// ------------------------------------------------------------------ mod implementation

namespace voicecontrol {

    static AudioCapture      g_audio;
    static InputInjector     g_injector;
    static VolumeIndicator*  g_indicator = nullptr;
    static std::atomic<bool> g_hardDisabled { false };

    static constexpr int kIndicatorTag = 0x5653;

    // -------------------------------------------------------------- helpers

    static bool ensureAudioRunning() {
        if (g_hardDisabled.load(std::memory_order_relaxed)) return false;
        if (g_audio.isAvailable()) return true;
        if (!g_audio.init())  { g_hardDisabled.store(true); return false; }
        if (!g_audio.start()) { g_hardDisabled.store(true); return false; }
        return true;
    }

    static void stopAudio() { g_audio.stop(); }

    static void attachIndicatorToLayer(CCNode* layer) {
        if (!layer) return;
        if (g_indicator && g_indicator->getParent()) {
            g_indicator->removeFromParentAndCleanup(true);
            g_indicator = nullptr;
        }
        if (!Mod::get()->getSettingValue<bool>("show_volume_indicator")) return;

        g_indicator = VolumeIndicator::create();
        if (!g_indicator) return;
        g_indicator->setTag(kIndicatorTag);
        layer->addChild(g_indicator, 999999);
    }

    static void removeIndicator() {
        if (g_indicator) {
            g_indicator->removeFromParentAndCleanup(true);
            g_indicator = nullptr;
        }
    }

    // -------------------------------------------------------------- Test-mic popup

    class TestMicLayer : public CCLayerColor {
    public:
        static TestMicLayer* create() {
            auto* r = new TestMicLayer();
            if (r && r->init()) { r->autorelease(); return r; }
            delete r; return nullptr;
        }

        bool init() override {
            if (!CCLayerColor::initWithColor(ccc4(18, 18, 18, 230), 290.0f, 200.0f))
                return false;

            ignoreAnchorPointForPosition(false);
            setAnchorPoint(ccp(0.5f, 0.5f));

            auto* title = CCLabelBMFont::create("Test Microphone", "bigFont.fnt");
            title->setScale(0.6f);
            title->setPosition(ccp(145.0f, 178.0f));
            addChild(title);

            m_meterBack = CCLayerColor::create(ccc4(55, 55, 55, 255), 240.0f, 14.0f);
            m_meterBack->ignoreAnchorPointForPosition(false);
            m_meterBack->setAnchorPoint(ccp(0.0f, 0.5f));
            m_meterBack->setPosition(ccp(25.0f, 128.0f));
            addChild(m_meterBack);

            m_meterFill = CCLayerColor::create(ccc4(64, 220, 96, 255), 0.0f, 14.0f);
            m_meterFill->ignoreAnchorPointForPosition(false);
            m_meterFill->setAnchorPoint(ccp(0.0f, 0.5f));
            m_meterFill->setPosition(ccp(0.0f, 7.0f));
            m_meterBack->addChild(m_meterFill);

            m_rmsLabel = CCLabelBMFont::create("RMS: 0.0000", "bigFont.fnt");
            m_rmsLabel->setScale(0.45f);
            m_rmsLabel->setPosition(ccp(145.0f, 100.0f));
            addChild(m_rmsLabel);

            m_statusLabel = CCLabelBMFont::create("SILENT", "bigFont.fnt");
            m_statusLabel->setScale(0.55f);
            m_statusLabel->setPosition(ccp(145.0f, 72.0f));
            addChild(m_statusLabel);

            schedule(schedule_selector(TestMicLayer::refresh), 0.05f);
            refresh(0.0f);
            return true;
        }

        void refresh(float) {
            const bool  ready     = ensureAudioRunning();
            const float threshold = std::max(
                0.0001f, Mod::get()->getSettingValue<float>("sensitivity")
            );
            const float rms   = g_currentRMS.load(std::memory_order_relaxed);
            const float ratio = std::clamp(rms / threshold, 0.0f, 1.0f);

            m_meterFill->setContentSize(CCSize(240.0f * ratio, 14.0f));

            std::ostringstream ss;
            ss << "RMS: " << std::fixed << std::setprecision(4) << rms;
            m_rmsLabel->setString(ss.str().c_str());

            if (!ready) {
                m_meterFill->setColor({ 128, 128, 128 });
                m_statusLabel->setString("MIC UNAVAILABLE");
                m_statusLabel->setColor({ 180, 180, 180 });
                return;
            }
            if (rms >= threshold) {
                m_meterFill->setColor({ 255, 64, 64 });
                m_statusLabel->setString("TRIGGERING");
                m_statusLabel->setColor({ 255, 64, 64 });
            } else {
                m_meterFill->setColor({ 64, 220, 96 });
                m_statusLabel->setString("SILENT");
                m_statusLabel->setColor({ 180, 180, 180 });
            }
        }

    private:
        CCLayerColor*   m_meterBack   = nullptr;
        CCLayerColor*   m_meterFill   = nullptr;
        CCLabelBMFont*  m_rmsLabel    = nullptr;
        CCLabelBMFont*  m_statusLabel = nullptr;
    };

    static void openTestPopup() {
        ensureAudioRunning();

        // 6-arg FLAlertLayer::create overload
        auto* popup = FLAlertLayer::create(
            nullptr,
            "VoiceControl",
            "",
            "Close",
            nullptr,
            320.0f
        );
        if (!popup) return;

        auto* panel = TestMicLayer::create();
        if (!panel) return;

        const auto ws = CCDirector::sharedDirector()->getWinSize();
        panel->setPosition(ccp(ws.width * 0.5f, ws.height * 0.5f));
        popup->addChild(panel, 10);
        popup->show();
    }

    // -------------------------------------------------------------- PlayLayer hook

    class $modify(VCPlayLayer, PlayLayer) {
        void onEnterTransitionDidFinish() {
            PlayLayer::onEnterTransitionDidFinish();
            if (!Mod::get()->getSettingValue<bool>("enabled")) return;
            if (ensureAudioRunning()) {
                g_injector.attach(this);
                attachIndicatorToLayer(this);
            }
        }

        void update(float dt) {
            PlayLayer::update(dt);

            if (!Mod::get()->getSettingValue<bool>("enabled")) {
                g_injector.forceRelease();
                removeIndicator();
                stopAudio();
                return;
            }

            if (!g_audio.isAvailable()) ensureAudioRunning();

            g_injector.attach(this);
            g_injector.update(dt, g_currentRMS.load(std::memory_order_relaxed));

            if (Mod::get()->getSettingValue<bool>("show_volume_indicator")) {
                if (!g_indicator || !g_indicator->getParent())
                    attachIndicatorToLayer(this);
            } else {
                removeIndicator();
            }
        }

        void onQuit() {
            g_injector.forceRelease();
            g_injector.detach();
            removeIndicator();
            stopAudio();
            PlayLayer::onQuit();
        }
    };

    // -------------------------------------------------------------- Cleanup on unload

    struct GlobalShutdown {
        ~GlobalShutdown() {
            g_injector.forceRelease();
            g_injector.detach();
            removeIndicator();
            stopAudio();
        }
    };
    static GlobalShutdown g_shutdown;

} // namespace voicecontrol
