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
        // handleButton(bool down, int button, bool isPlayer2)
        // down = true (press), button = 1 (jump/primary), isPlayer2 = false (player 1)
        m_layer->handleButton(true, 1, false);
        m_buttonDown = true;
    }

    void InputInjector::releaseButton() {
        if (!m_layer || !m_buttonDown) return;
        // down = false (release), button = 1 (jump/primary), isPlayer2 = false (player 1)
        m_layer->handleButton(false, 1, false);
        m_buttonDown = false;
    }

    void InputInjector::forceRelease() {
        if (m_buttonDown) releaseButton();
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
        if (m_layer->m_player1 && m_layer->m_player1->m_isDead)
            m_state = State::RELEASING;

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
                if (!above && m_timeBelow >= releaseDebounce)
                    m_state = State::RELEASING;
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
