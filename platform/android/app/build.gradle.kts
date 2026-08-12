import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Personal, local-only opt-in for the RE-derived quarantine signer overlay
// (src/quarantine - gitignored, never committed, see that directory's own
// README.md). OFF by default so THIS file - committed, and what CI/a public
// release builds - always ships the safe (bed51-equivalent, see
// shield_quarantine_hook.hpp's signing_available()) unsigned app. Turn it on
// for your own local builds only, by adding one line to
// platform/android/local.properties - itself gitignored and per-machine
// already (see that file's header) - never edit this committed file to
// flip it:
//   ac3forge.quarantineSigner=true
// See docs/platforms/android.md's "quarantine signer dependency" section.
val localProperties = Properties().apply {
    val file = rootProject.file("local.properties")
    if (file.exists()) {
        file.inputStream().use { load(it) }
    }
}
val quarantineSignerEnabled =
    (localProperties.getProperty("ac3forge.quarantineSigner") ?: "false").toBoolean()

// Release-keystore signing, wired to environment variables rather than
// local.properties: CI (release.yml -> _build.yml's build-android job)
// decodes the ANDROID_KEYSTORE_BASE64 secret to a runner-temp file and
// exports these four before invoking assembleRelease. Absent locally and
// on ordinary CI (secrets aren't exposed there, and assembleDebug never
// reads a release signingConfig anyway) - releaseSigningAvailable gates
// both whether the config is created at all and which one `release` uses,
// so every build path degrades to the debug keystore exactly as before
// this was wired up, rather than failing when the four env vars are unset.
val releaseKeystorePath = System.getenv("ANDROID_KEYSTORE_PATH")
val releaseKeystorePassword = System.getenv("ANDROID_KEYSTORE_PASSWORD")
val releaseKeyAlias = System.getenv("ANDROID_KEY_ALIAS")
val releaseKeyPassword = System.getenv("ANDROID_KEY_PASSWORD")
val releaseSigningAvailable = !releaseKeystorePath.isNullOrBlank() &&
    !releaseKeystorePassword.isNullOrBlank() &&
    !releaseKeyAlias.isNullOrBlank() &&
    !releaseKeyPassword.isNullOrBlank()

android {
    namespace = "com.ac3forge.shield"
    // 36 is what's installed locally; 34 is the target actually exercised -
    // compiling against a newer SDK than the app targets is normal and lets
    // the app run correctly on the Shield's actual (older) system image
    // without opting into behavior changes a newer targetSdk would bring.
    compileSdk = 36

    defaultConfig {
        applicationId = "com.ac3forge.shield"
        // 26 (Oreo): the floor for AAudio, which monitor.cpp depends on
        // outright - there is no lower-API fallback path for it. Real Shield
        // TV hardware (2017 model onward) ships well above this.
        minSdk = 26
        targetSdk = 34
        versionCode = 2
        versionName = "0.3.0-beta.1"

        externalNativeBuild {
            cmake {
                // ANDROID_STL=c++_shared, not the default static libc++:
                // ac3::forge/ac3::audio are static libs linked into this one
                // shared object, and a static STL would duplicate global
                // state (locale, iostream init) if anything else in the
                // process ever pulled in libc++ too - shared avoids that
                // question entirely rather than relying on there being
                // nothing else to collide with today.
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }

        ndk {
            // The single Shield-relevant ABI. Shield TV (2017/2019/Pro) is
            // arm64 throughout; building armeabi-v7a/x86/x86_64 as well would
            // only slow every local iteration for targets that can never run
            // on the actual device this app exists for.
            abiFilters += listOf("arm64-v8a")
        }
    }

    // NDK r26.1.10909125 specifically (see docs/platforms/android.md) - the
    // version this plan targets throughout, pinned here rather than left to
    // "whichever NDK Gradle happens to resolve" so a build failure means
    // something changed, not that a different NDK silently got picked.
    ndkVersion = "26.1.10909125"

    signingConfigs {
        if (releaseSigningAvailable) {
            create("release") {
                storeFile = file(releaseKeystorePath!!)
                storePassword = releaseKeystorePassword
                keyAlias = releaseKeyAlias
                keyPassword = releaseKeyPassword
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            // The repo's own cmake_minimum_required is 3.28...4.3 (see the
            // root CMakeLists.txt), so any version in that range is fine;
            // 3.31.6 is what the SDK manager actually has available for the
            // 3.x line (there is no 3.28.x package in the SDK's repository).
            version = "3.31.6"
        }
    }

    buildTypes {
        debug {
            // AGP's default for the debug build type is CMAKE_BUILD_TYPE=Debug
            // (-O0) - fine for jni_entry.cpp's smoke tests, but nowhere near
            // real-time for live_cursor.cpp's actual DSP work
            // (AtmosEncoder::encode_frame's MDCT/bit-allocation/JOC matrix,
            // once per 32ms frame). Confirmed on-device: -O0 on this Shield's
            // Tegra X1 took ~425ms per frame, over 13x too slow to keep up -
            // bursts arrived in huge sparse gaps instead of a steady stream,
            // which is exactly why the receiver couldn't lock ("flashing").
            // RelWithDebInfo keeps this APK debuggable (isDebuggable stays
            // the debug build type's default - no separate signing/release
            // setup needed to adb install) while actually optimizing the
            // native side. See docs/platforms/android.md.
            externalNativeBuild {
                cmake {
                    arguments += listOf("-DCMAKE_BUILD_TYPE=RelWithDebInfo")
                    // See quarantineSignerEnabled's own comment above this
                    // file's android {} block - governed by a gitignored
                    // local.properties entry, never hardcoded here.
                    if (quarantineSignerEnabled) {
                        arguments += listOf("-DAC3FORGE_QUARANTINE_SIGNER=ON")
                    }
                }
            }
        }
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // Real release keystore when one is provisioned (see
            // releaseSigningAvailable above); debug-keystore signed
            // otherwise, not unsigned - an unsigned release APK would not
            // be directly installable at all. This app is sideload-only
            // (adb install / a GitHub release asset), never the Play
            // Store, so a release key only matters for update-signature
            // continuity across sideloaded installs, not a store
            // requirement. See docs/platforms/android.md.
            signingConfig = if (releaseSigningAvailable) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
            // Same reasoning as the debug build type's own comment above:
            // without an explicit CMAKE_BUILD_TYPE this native side would
            // build with whatever CMake's own default is (effectively
            // unoptimized) - the exact ~13x-too-slow-for-real-time problem
            // that comment describes, just for the variant that actually
            // ships. Release, not RelWithDebInfo: this is the shipped
            // artifact, not a local debugging build, so there is no reason
            // to carry debug symbols in it.
            externalNativeBuild {
                cmake {
                    arguments += listOf("-DCMAKE_BUILD_TYPE=Release")
                    if (quarantineSignerEnabled) {
                        arguments += listOf("-DAC3FORGE_QUARANTINE_SIGNER=ON")
                    }
                }
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.appcompat:appcompat:1.7.0")
}
