// The Shield Atmos demo - a standalone Gradle build living beside, not
// inside, ac3forge's CMake build. It reaches into the repo root only through
// app/src/main/cpp/CMakeLists.txt's add_subdirectory() (see that file's
// header comment) - nothing here assumes a Gradle-based CMake project
// structure for the rest of the repo.

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "shield-atmos-demo"
include(":app")
