#include "InputInjector.hpp"
#include "AudioCapture.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>

using namespace geode::prelude;

namespace voicecontrol {

    void InputInjector::attach(PlayLayer* layer) {
        if (m_layer != layer) forceRelease();
        m_layer     = layer;
        m_timeAbove = 0.0f;
        m_timeBelow = 0.0f;
        m_state     = State::IDLE;
        m_buttonDown = false;
    }

    void InputInjector::detach() {
        forceRelease();
        m_layer = nullptr;
    }

    void InputInjector::pressButton() {
        if (!m_layer || m_buttonDown) return;
        
        auto* player = m_layer->m_player1;
        if (!player) return;

        // ================================================================
        // NUCLEAR OPTION: Direct physics manipulation
        // ================================================================
        
        log::info("VoiceControl: NUCLEAR PRESS - triggering jump");
        
        // Set player internal state flags
        player->m_isHolding = true;
        player->m_hasJustHeld = true;
        
        // Get current game mode
        bool isShip     = player->m_isShip;
        bool isBall     = player->m_isBall;
        bool isUfo      = player->m_isUfo;
        bool isWave     = player->m_isWave;
        bool isRobot    = player->m_isRobot;
        bool isSpider   = player->m_isSpider;
        bool isSwing    = player->m_isSwing;
        bool isCube     = !isShip && !isBall && !isUfo && !isWave && !isRobot && !isSpider && !isSwing;
        
        // CUBE MODE
        if (isCube) {
            if (player->m_isOnGround) {
                log::info("VoiceControl: Cube jump from ground");
                player->m_yVelocity = 11.180f;  // Standard jump velocity
                player->m_isOnGround = false;
                player->m_hasJustJumped = true;
                
                // Play jump sound
                if (auto* audioEngine = FMODAudioEngine::sharedEngine()) {
                    audioEngine->playEffect("playSound_01.ogg");
                }
            } else {
                log::info("VoiceControl: Cube in air - cannot jump");
            }
        }
        
        // SHIP MODE
        else if (isShip) {
            log::info("VoiceControl: Ship thrust");
            player->m_yVelocity = 5.77f;  // Ship upward thrust
            player->m_isHolding2 = true;
        }
        
        // BALL MODE
        else if (isBall) {
            log::info("VoiceControl: Ball gravity toggle");
            player->m_yVelocity = player->m_yVelocity * -1.0f;
            player->flipGravity(true, false);
        }
        
        // UFO MODE
        else if (isUfo) {
            log::info("VoiceControl: UFO boost");
            float boostPower = player->m_isUpsideDown ? -10.5f : 10.5f;
            player->m_yVelocity = boostPower;
            
            // Play UFO sound
            if (auto* audioEngine = FMODAudioEngine::sharedEngine()) {
                audioEngine->playEffect("playSound_01.ogg");
            }
        }
        
        // WAVE MODE
        else if (isWave) {
            log::info("VoiceControl: Wave up");
            player->m_waveTrail->m_isSolid = true;
        }
        
        // ROBOT MODE
        else if (isRobot) {
            if (player->m_isOnGround) {
                log::info("VoiceControl: Robot jump");
                player->m_yVelocity = 11.180f;
                player->m_isOnGround = false;
                player->m_hasJustJumped = true;
            }
        }
        
        // SPIDER MODE
        else if (isSpider) {
            log::info("VoiceControl: Spider teleport");
            player->toggleGravity();
        }
        
        // SWING MODE
        else if (isSwing) {
            log::info("VoiceControl: Swing direction change");
            player->m_isHolding2 = true;
        }
        
        m_buttonDown = true;
    }

    void InputInjector::releaseButton() {
        if (!m_layer || !m_buttonDown) return;
        
        auto* player = m_layer->m_player1;
        if (!player) return;

        log::info("VoiceControl: NUCLEAR RELEASE");
        
        // Clear holding state
        player->m_isHolding = false;
        player->m_hasJustHeld = false;
        player->m_isHolding2 = false;
        
        // Ship/Wave specific release logic
        if (player->m_isShip) {
            log::info("VoiceControl: Ship release - gravity takes over");
            // Ship will start falling
        }
        
        if (player->m_isWave) {
            log::info("VoiceControl: Wave release - neutral");
            if (player->m_waveTrail) {
                player->m_waveTrail->m_isSolid = false;
            }
        }
        
        m_buttonDown = false;
    }

    void InputInjector::forceRelease() {
        if (m_buttonDown) {
            releaseButton();
        }
        m_state     = State::IDLE;
        m_timeAbove = 0.0f;
        m_timeBelow = 0.0f;
    }

    void InputInjector::update(float dt, float rms) {
        if (!m_layer) return;

        auto* mod = Mod::get();
        if (!mod
            || !mod->getSettingValue<bool>("enabled")
            || !g_audioAvailable.load(std::memory_order_relaxed))
        {
            forceRelease();
            return;
        }

        const float threshold       = std::max(0.0001f, mod->getSettingValue<float>("sensitivity"));
        const float pressDebounce   = static_cast<float>(mod->getSettingValue<int>("press_debounce_ms"))   / 1000.0f;
        const float releaseDebounce = static_cast<float>(mod->getSettingValue<int>("release_debounce_ms")) / 1000.0f;

        const bool above = rms > threshold;

        if (above) { m_timeAbove += dt; m_timeBelow  = 0.0f; }
        else       { m_timeBelow += dt; m_timeAbove  = 0.0f; }

        // Dead player: force release immediately.
        if (m_layer->m_player1 && m_layer->m_player1->m_isDead) {
            m_state = State::RELEASING;
        }

        switch (m_state) {
            case State::IDLE:
                if (above && m_timeAbove >= pressDebounce) {
                    pressButton();
                    m_state = State::PRESSED;
                }
                break;

            case State::PRESSED:
                if (above) {
                    m_state = State::HELD;
                } else if (m_timeBelow >= releaseDebounce) {
                    m_state = State::RELEASING;
                }
                break;

            case State::HELD:
                if (!above && m_timeBelow >= releaseDebounce) {
                    m_state = State::RELEASING;
                }
                break;

            case State::RELEASING:
                releaseButton();
                m_state     = State::IDLE;
                m_timeAbove = 0.0f;
                m_timeBelow = 0.0f;
                break;
        }
    }

} // namespace voicecontrol
