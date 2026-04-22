#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_WASAPI
#define MA_NO_DSOUND
#define MA_NO_WINMM
#define MA_NO_COREAUDIO
#define MA_NO_SNDIO
#define MA_NO_AUDIO4
#define MA_NO_OSS
#define MA_NO_PULSEAUDIO
#define MA_NO_ALSA
#define MA_NO_JACK

#include "AudioCapture.hpp"

#include <Geode/Geode.hpp>

#include <android/log.h>
#include <cmath>
#include <jni.h>
#include <string>

#include "libs/miniaudio.h"

using namespace geode::prelude;
namespace voicecontrol {
    std::atomic<float> g_currentRMS { 0.0f };
    std::atomic<bool> g_audioAvailable { false };
    struct AudioCapture::Impl {
        ma_context context {};
        ma_device device {};

        bool contextInitialized = false;
        bool deviceInitialized = false;
        bool started = false;
        bool permanentlyDisabled = false;
        bool permissionWarned = false;
        bool deviceWarned = false;
    };

    AudioCapture::AudioCapture()
        : m_impl(std::make_unique<Impl>()) {}

    AudioCapture::~AudioCapture() {
        stop();
    }

    static JNIEnv* getJNIEnvSafe() {
        JNIEnv* env = nullptr;
        if (auto* vm = cocos2d::JniHelper::getJavaVM()) {
            if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
                if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
                    return nullptr;
                }
            }
        }

        return env;
    }

    static void clearJavaException(JNIEnv* env) {
        if (!env) return;
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }

    bool AudioCapture::checkPermission() const {
        auto* env = getJNIEnvSafe();
        if (!env) {
            log::warn("VoiceControl: failed to acquire JNI environment; assuming microphone permission is unavailable.");
            return false;
        }

        jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
        if (!activityThreadClass) {
            clearJavaException(env);
            log::warn("VoiceControl: ActivityThread class not found; assuming permission unavailable.");
            return false;
        }

        jmethodID currentApplication = env->GetStaticMethodID(
            activityThreadClass,
            "currentApplication",
            "()Landroid/app/Application;"
        );
        if (!currentApplication) {
            clearJavaException(env);
            env->DeleteLocalRef(activityThreadClass);
            log::warn("VoiceControl: ActivityThread.currentApplication unavailable; assuming permission unavailable.");
            return false;
        }

        jobject application = env->CallStaticObjectMethod(activityThreadClass, currentApplication);
        clearJavaException(env);
        env->DeleteLocalRef(activityThreadClass);

        if (!application) {
            log::warn("VoiceControl: no application context available yet.");
            return false;
        }

        jclass contextClass = env->GetObjectClass(application);
        if (!contextClass) {
            clearJavaException(env);
            env->DeleteLocalRef(application);
            log::warn("VoiceControl: failed to obtain context class.");
            return false;
        }

        jmethodID checkSelfPermission = env->GetMethodID(
            contextClass,
            "checkSelfPermission",
            "(Ljava/lang/String;)I"
        );
        if (!checkSelfPermission) {
            clearJavaException(env);
            env->DeleteLocalRef(contextClass);
            env->DeleteLocalRef(application);
            log::info("VoiceControl: checkSelfPermission unavailable (pre-Android 6). Assuming permission is granted.");
            return true;
        }

        jstring permission = env->NewStringUTF("android.permission.RECORD_AUDIO");
        if (!permission) {
            clearJavaException(env);
            env->DeleteLocalRef(contextClass);
            env->DeleteLocalRef(application);
            log::warn("VoiceControl: failed to create permission string.");
            return false;
        }

        jint result = env->CallIntMethod(application, checkSelfPermission, permission);
        clearJavaException(env);

        env->DeleteLocalRef(permission);
        env->DeleteLocalRef(contextClass);
        env->DeleteLocalRef(application);

        return result == 0;
    }

    void AudioCapture::showPermissionWarning() {
        auto msg = "Microphone permission not granted — use ADB to grant android.permission.RECORD_AUDIO";
        log::warn("VoiceControl: {}", msg);
        auto notif = Notification::create("VoiceControl", msg);
        if (notif) notif->show();
    }

    void AudioCapture::dataCallback(ma_device* pDevice, void* pOutput, const void* pInput, uint32_t frameCount) {
        (void)pOutput;
        if (!pDevice || !pInput || frameCount == 0) {
            g_currentRMS.store(0.0f, std::memory_order_relaxed);
            return;
        }

        auto* capture = static_cast<AudioCapture*>(pDevice->pUserData);
        if (!capture || !capture->m_impl) {
            g_currentRMS.store(0.0f, std::memory_order_relaxed);
            return;
        }

        const ma_uint32 channels = pDevice->capture.channels > 0 ? pDevice->capture.channels : 1;
        const size_t sampleCount = static_cast<size_t>(frameCount) * static_cast<size_t>(channels);

        double sumSquares = 0.0;
        if (pDevice->capture.format == ma_format_f32) {
            const float* samples = static_cast<const float*>(pInput);
            for (size_t i = 0; i < sampleCount; ++i) {
                const double s = static_cast<double>(samples[i]);
                sumSquares += s * s;
            }
        }
        else if (pDevice->capture.format == ma_format_s16) {
            const int16_t* samples = static_cast<const int16_t*>(pInput);
            constexpr double inv = 1.0 / 32768.0;
            for (size_t i = 0; i < sampleCount; ++i) {
                const double s = static_cast<double>(samples[i]) * inv;
                sumSquares += s * s;
            }
        }
        else {
            g_currentRMS.store(0.0f, std::memory_order_relaxed);
            return;
        }

        const double meanSquares = sumSquares / static_cast<double>(sampleCount);
        const float rms = static_cast<float>(std::sqrt(meanSquares));
        g_currentRMS.store(rms, std::memory_order_relaxed);
    }

    bool AudioCapture::init() {
        if (!m_impl || m_impl->permanentlyDisabled) {
            return false;
        }

        if (m_impl->contextInitialized || m_impl->deviceInitialized) {
            return true;
        }

        if (!checkPermission()) {
            if (!m_impl->permissionWarned) {
                m_impl->permissionWarned = true;
                showPermissionWarning();
            }
            m_impl->permanentlyDisabled = true;
            g_audioAvailable.store(false, std::memory_order_relaxed);
            return false;
        }

        ma_backend backends[] = {
            ma_backend_aaudio,
            ma_backend_opensl
        };
        ma_context_config contextConfig = ma_context_config_init();
        contextConfig.pBackendPriorities = backends;
        contextConfig.backendCount = 2;

        ma_result result = ma_context_init(backends, 2, &contextConfig, &m_impl->context);
        if (result != MA_SUCCESS) {
            log::error("VoiceControl: ma_context_init failed with result {}", static_cast<int>(result));
            m_impl->permanentlyDisabled = true;
            g_audioAvailable.store(false, std::memory_order_relaxed);
            return false;
        }

        m_impl->contextInitialized = true;
        ma_device_config deviceConfig = ma_device_config_init(ma_device_type_capture);
        deviceConfig.capture.format = ma_format_f32;
        deviceConfig.capture.channels = 1;
        deviceConfig.sampleRate = 48000;
        deviceConfig.dataCallback = &AudioCapture::dataCallback;
        deviceConfig.pUserData = this;
        result = ma_device_init(&m_impl->context, &deviceConfig, &m_impl->device);
        if (result != MA_SUCCESS) {
            log::error("VoiceControl: ma_device_init failed with result {}", static_cast<int>(result));
            if (!m_impl->deviceWarned) {
                m_impl->deviceWarned = true;
                auto notif = Notification::create(
                    "VoiceControl",
                    "No microphone device found or audio backend unavailable."
                );
                if (notif) notif->show();
            }
            ma_context_uninit(&m_impl->context);
            m_impl->contextInitialized = false;
            m_impl->permanentlyDisabled = true;
            g_audioAvailable.store(false, std::memory_order_relaxed);
            return false;
        }

        m_impl->deviceInitialized = true;
        g_currentRMS.store(0.0f, std::memory_order_relaxed);
        g_audioAvailable.store(true, std::memory_order_relaxed);

        log::info("VoiceControl: microphone capture initialized successfully.");
        return true;
    }

    bool AudioCapture::start() {
        if (!m_impl || m_impl->permanentlyDisabled) {
            return false;
        }

        if (!m_impl->deviceInitialized && !init()) {
            return false;
        }

        if (m_impl->started) {
            return true;
        }

        ma_result result = ma_device_start(&m_impl->device);
        if (result != MA_SUCCESS) {
            log::error("VoiceControl: ma_device_start failed with result {}", static_cast<int>(result));
            m_impl->permanentlyDisabled = true;

            if (!m_impl->deviceWarned) {
                m_impl->deviceWarned = true;
                auto notif = Notification::create(
                    "VoiceControl",
                    "Failed to start microphone capture. The mod will remain disabled."
                );
                if (notif) notif->show();
            }

            stop();
            return false;
        }

        m_impl->started = true;
        g_audioAvailable.store(true, std::memory_order_relaxed);
        return true;
    }

    void AudioCapture::stop() {
        if (!m_impl) {
            g_currentRMS.store(0.0f, std::memory_order_relaxed);
            g_audioAvailable.store(false, std::memory_order_relaxed);
            return;
        }

        if (m_impl->started) {
            ma_device_stop(&m_impl->device);
            m_impl->started = false;
        }

        if (m_impl->deviceInitialized) {
            ma_device_uninit(&m_impl->device);
            m_impl->deviceInitialized = false;
        }

        if (m_impl->contextInitialized) {
            ma_context_uninit(&m_impl->context);
            m_impl->contextInitialized = false;
        }

        g_currentRMS.store(0.0f, std::memory_order_relaxed);
        g_audioAvailable.store(false, std::memory_order_relaxed);
    }

    float AudioCapture::getCurrentRMS() const {
        return g_currentRMS.load(std::memory_order_relaxed);
    }

    bool AudioCapture::isAvailable() const {
        return g_audioAvailable.load(std::memory_order_relaxed);
    }
}
