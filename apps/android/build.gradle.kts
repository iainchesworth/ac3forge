// Root build script: nothing lives here directly, only plugin version
// declarations shared with app/build.gradle.kts (the `apply false` pattern
// avoids applying the Android plugin at the root, where there is no
// android {} block to configure).
plugins {
    id("com.android.application") version "8.7.3" apply false
    id("org.jetbrains.kotlin.android") version "2.1.0" apply false
}
